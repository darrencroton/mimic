/**
 * @file    snapshot_collectives.c
 * @brief   Whole-population operations for process_snapshot modules across tasks
 *
 * Implements the snapshot collectives declared in snapshot_collectives.h: the
 * callback gate every collective shares, the serial identity used under
 * NTask <= 1 and in non-MPI builds, and the MPI implementations.
 *
 * The MPI rank is a sample-sort. Each task validates and sorts its keys, the
 * tasks agree whether any task failed, then NTask - 1 splitters are chosen
 * from regular samples (at most SAMPLES_PER_TASK per task, allgathered and
 * sorted identically on every task). Each key goes to the task owning its
 * bucket (MPI_Alltoallv); each owner sorts its bucket and ranks every key as
 * the bucket's global offset (an exclusive scan of bucket sizes) plus its
 * position, and the ranks travel back along the same counts and are
 * unpermuted into the caller's order. Because the ranks are positions in one
 * total order of unique keys, the splitters affect only the load balance,
 * never the result, which therefore depends only on the multiset of keys.
 *
 * Every failure a task can detect locally is agreed with an MPI_Allreduce
 * before any later collective, so all tasks take the same sequence of calls
 * and return the same status. MPI errors themselves abort the job through
 * MPI_COMM_WORLD's default MPI_ERRORS_ARE_FATAL handler, as for the driver's
 * other MPI calls, so their return codes are not checked here.
 */

#include "snapshot_collectives.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef MPI
#include <mpi.h>
#endif

#include "error.h"
#include "memory.h"
#include "module_registry.h"
#include "task_layout.h"

/* ==========================================================================
 * Callback gate
 * ========================================================================== */

/** @brief Phrase naming a running-callback kind in a refusal */
static const char *callback_kind_phrase(enum RunningCallbackKind kind) {
  switch (kind) {
  case RUNNING_CALLBACK_INIT:
    return "init()";
  case RUNNING_CALLBACK_FULL_HALO:
    return "a process_full_halo callback";
  case RUNNING_CALLBACK_PER_EVENT:
    return "a process_per_event callback";
  case RUNNING_CALLBACK_BY_GALAXY:
    return "a process_by_galaxy callback";
  case RUNNING_CALLBACK_CLEANUP:
    return "cleanup()";
  case RUNNING_CALLBACK_SNAPSHOT:
    return "a process_snapshot callback";
  case RUNNING_CALLBACK_NONE:
    return "outside any module callback";
  }
  return "an unknown callback kind";
}

/**
 * @brief   Whether a collective may run in the current callback
 *
 * Allowed during snapshot dispatch and outside any callback; refused, with an
 * ERROR_LOG naming the function, the module and the callback kind, in every
 * other kind (and in any kind this file does not know, failing closed). The
 * kind is the same on every task, so a refusal is too.
 *
 * @param   function  Collective being called, for the message
 * @return  1 if allowed, 0 if refused
 */
static int collective_allowed(const char *function) {
  const char *module = NULL;
  const enum RunningCallbackKind kind = module_registry_running_callback(&module);
  if (kind == RUNNING_CALLBACK_SNAPSHOT || kind == RUNNING_CALLBACK_NONE) {
    return 1;
  }
  ERROR_LOG("%s refused for module '%s': called from %s, which is not synchronised across "
            "tasks; snapshot collectives run only in a process_snapshot callback",
            function, module != NULL ? module : "(unknown)", callback_kind_phrase(kind));
  return 0;
}

/* ==========================================================================
 * Key order and local validation
 * ========================================================================== */

/** A key with its index in the array it was sorted from */
struct RankEntry {
  struct SnapshotRankKey key;
  int64_t index;
};

/** @brief qsort order of keys: value descending, then id ascending */
static int compare_keys(const void *pa, const void *pb) {
  const struct SnapshotRankKey *a = pa;
  const struct SnapshotRankKey *b = pb;
  if (a->value > b->value) {
    return -1;
  }
  if (a->value < b->value) {
    return 1;
  }
  return (a->id > b->id) - (a->id < b->id);
}

/** @brief qsort order of entries: their keys, then their source index (deterministic) */
static int compare_entries(const void *pa, const void *pb) {
  const struct RankEntry *a = pa;
  const struct RankEntry *b = pb;
  const int by_key = compare_keys(&a->key, &b->key);
  return by_key != 0 ? by_key : (a->index > b->index) - (a->index < b->index);
}

/** @brief qsort order of int64 ids, ascending */
static int compare_ids(const void *pa, const void *pb) {
  const int64_t a = *(const int64_t *)pa;
  const int64_t b = *(const int64_t *)pb;
  return (a > b) - (a < b);
}

/**
 * @brief   Find a repeated id among @p count keys
 *
 * @param   keys       Keys to check (may be NULL when @p count is 0)
 * @param   count      Number of keys (>= 0)
 * @param   duplicate  Receives the smallest repeated id when one exists
 * @return  1 if an id repeats, 0 otherwise
 */
static int find_duplicate_id(const struct SnapshotRankKey *keys, int64_t count,
                             int64_t *duplicate) {
  if (count < 2) {
    return 0;
  }
  int64_t *ids = mymalloc_cat((size_t)count * sizeof(*ids), MEM_UTILITY);
  for (int64_t i = 0; i < count; i++) {
    ids[i] = keys[i].id;
  }
  qsort(ids, (size_t)count, sizeof(*ids), compare_ids);
  int found = 0;
  for (int64_t i = 1; i < count && !found; i++) {
    if (ids[i] == ids[i - 1]) {
      *duplicate = ids[i];
      found = 1;
    }
  }
  myfree(ids);
  return found;
}

/**
 * @brief   Check the caller's arguments and keys for module_snapshot_rank()
 *
 * Logs the first problem found: a negative count, a NULL array with a positive
 * count, a count beyond @p max_count, a NaN value or a repeated id.
 *
 * @return  0 when the keys can be ranked, -1 otherwise
 */
static int validate_rank_keys(const struct SnapshotRankKey *keys, int64_t count,
                              const int64_t *ranks, int64_t max_count) {
  if (count < 0 || (count > 0 && (keys == NULL || ranks == NULL))) {
    ERROR_LOG("module_snapshot_rank: invalid arguments (count=%" PRId64 ", keys=%p, ranks=%p)",
              count, (const void *)keys, (const void *)ranks);
    return -1;
  }
  if (count > max_count) {
    ERROR_LOG("module_snapshot_rank: %" PRId64 " keys exceed the %" PRId64 " one task can rank",
              count, max_count);
    return -1;
  }
  for (int64_t i = 0; i < count; i++) {
    if (isnan(keys[i].value)) {
      ERROR_LOG("module_snapshot_rank: key %" PRId64 " (id %" PRId64 ") has a NaN value", i,
                keys[i].id);
      return -1;
    }
  }
  int64_t duplicate = 0;
  if (find_duplicate_id(keys, count, &duplicate)) {
    ERROR_LOG("module_snapshot_rank: id %" PRId64 " appears more than once in this task's keys",
              duplicate);
    return -1;
  }
  return 0;
}

/**
 * @brief   Copy @p count keys into entries remembering their index, sorted by key
 *
 * @return  Tracked MEM_UTILITY array of @p count entries (caller frees)
 */
static struct RankEntry *sorted_entries(const struct SnapshotRankKey *keys, int64_t count) {
  struct RankEntry *entries = mymalloc_cat((size_t)count * sizeof(*entries), MEM_UTILITY);
  for (int64_t i = 0; i < count; i++) {
    entries[i].key = keys[i];
    entries[i].index = i;
  }
  qsort(entries, (size_t)count, sizeof(*entries), compare_entries);
  return entries;
}

/* ==========================================================================
 * MPI implementations
 * ========================================================================== */

#ifdef MPI

/** Regular samples each task contributes to the splitter choice */
#define SAMPLES_PER_TASK 256

/**
 * @brief   Agree across tasks whether any task failed
 *
 * @param   function      Collective being called, for the message
 * @param   local_failed  Non-zero when this task failed (and already logged why)
 * @return  1 on every task when any task failed, 0 on every task otherwise
 */
static int any_task_failed(const char *function, int local_failed) {
  int flag = local_failed != 0;
  int any = 0;
  MPI_Allreduce(&flag, &any, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);
  if (any && !flag) {
    ERROR_LOG("%s failed on another task; every task returns -1", function);
  }
  return any;
}

/**
 * @brief   Agree a reduction's arguments across tasks before reducing
 *
 * One MPI_Allreduce (MPI_MAX) of {invalid, n, -n}: the first entry is the
 * logical or of the tasks' argument failures, the others give the largest and
 * smallest `n`, which must agree for the reduction to be well formed. Every
 * task receives the same values and so takes the same decision.
 *
 * @param   agreed_n  On success, the `n` every task passed; the caller skips its
 *                    reduction when it is 0, identically on every task
 * @return  0 when every task may reduce, -1 on every task otherwise
 */
static int agree_reduction_arguments(const char *function, int n, int locally_valid,
                                     int *agreed_n) {
  const int checked_n = n >= 0 ? n : 0;
  int local[3] = {locally_valid ? 0 : 1, checked_n, -checked_n};
  int global[3] = {0, 0, 0};
  MPI_Allreduce(local, global, 3, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
  if (global[0] != 0) {
    if (locally_valid) {
      ERROR_LOG("%s failed on another task; every task returns -1", function);
    }
    return -1;
  }
  if (global[1] != -global[2]) {
    ERROR_LOG("%s: n differs across tasks (smallest %d, largest %d); every task must pass the "
              "same n",
              function, -global[2], global[1]);
    return -1;
  }
  *agreed_n = global[1];
  return 0;
}

/**
 * @brief   First position in @p sorted whose key does not precede @p key
 *
 * @param   sorted  Keys in compare_keys() order
 * @param   count   Number of keys
 * @param   key     Key to place
 * @return  Position in [0, count]
 */
static int64_t lower_bound_key(const struct SnapshotRankKey *sorted, int64_t count,
                               const struct SnapshotRankKey *key) {
  int64_t lo = 0;
  int64_t hi = count;
  while (lo < hi) {
    const int64_t mid = lo + (hi - lo) / 2;
    if (compare_keys(&sorted[mid], key) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/**
 * @brief   The sample-sort rank across NTask > 1 tasks (see the file comment)
 *
 * @param   keys          The caller's keys
 * @param   count         Number of keys
 * @param   ranks         Output, written only on success
 * @param   local_failed  Non-zero when this task's validation failed
 * @return  0 on success, -1 on every task when any task failed
 */
static int rank_across_tasks(const struct SnapshotRankKey *keys, int64_t count, int64_t *ranks,
                             int local_failed) {
  static const char function[] = "module_snapshot_rank";
  const int ntask = effective_task_count();
  const MPI_Comm comm = MPI_COMM_WORLD;
  int status = -1;

  struct SnapshotRankKey *sorted = NULL;  /* this task's keys, sorted */
  int64_t *order = NULL;                  /* sorted position -> caller index */
  struct SnapshotRankKey *sample = NULL;  /* this task's regular samples */
  int *sample_counts = NULL;              /* samples each task contributes */
  int *sample_displs = NULL;              /* allgatherv displacements */
  struct SnapshotRankKey *samples = NULL; /* every task's samples, sorted */
  int *send_counts = NULL;                /* keys this task sends to each bucket */
  int *send_displs = NULL;                /* their offsets in `sorted` */
  int *recv_counts = NULL;                /* keys this bucket receives from each task */
  int *recv_displs = NULL;                /* their offsets in `bucket` */
  struct SnapshotRankKey *bucket = NULL;  /* the keys this task owns */
  int64_t *bucket_ranks = NULL;           /* global rank of each bucket key */
  int64_t *sorted_ranks = NULL;           /* global rank of each sorted local key */
  int nsample = 0;                        /* samples this task contributes */
  int total_samples = 0;                  /* samples over every task */
  int64_t bucket_count = 0;               /* keys this task owns */
  int64_t bucket_offset = 0;              /* global rank of this bucket's first key */

  MPI_Datatype key_type;
  MPI_Type_contiguous((int)sizeof(struct SnapshotRankKey), MPI_BYTE, &key_type);
  MPI_Type_commit(&key_type);

  /* 1. Sort locally and agree the validation outcome before any exchange. */
  if (!local_failed) {
    struct RankEntry *entries = sorted_entries(keys, count);
    sorted = mymalloc_cat((size_t)count * sizeof(*sorted), MEM_UTILITY);
    order = mymalloc_cat((size_t)count * sizeof(*order), MEM_UTILITY);
    for (int64_t i = 0; i < count; i++) {
      sorted[i] = entries[i].key;
      order[i] = entries[i].index;
    }
    myfree(entries);
  }
  if (any_task_failed(function, local_failed)) {
    goto cleanup;
  }

  /* 2. Regular samples from every task, sorted identically on every task. */
  nsample = (int)(count < SAMPLES_PER_TASK ? count : SAMPLES_PER_TASK);
  sample = mymalloc_cat((size_t)nsample * sizeof(*sample), MEM_UTILITY);
  for (int i = 0; i < nsample; i++) {
    sample[i] = sorted[(int64_t)i * count / nsample];
  }
  sample_counts = mymalloc_cat((size_t)ntask * sizeof(*sample_counts), MEM_UTILITY);
  sample_displs = mymalloc_cat((size_t)ntask * sizeof(*sample_displs), MEM_UTILITY);
  MPI_Allgather(&nsample, 1, MPI_INT, sample_counts, 1, MPI_INT, comm);
  for (int t = 0; t < ntask; t++) {
    sample_displs[t] = total_samples;
    total_samples += sample_counts[t];
  }
  samples = mymalloc_cat((size_t)total_samples * sizeof(*samples), MEM_UTILITY);
  MPI_Allgatherv(sample, nsample, key_type, samples, sample_counts, sample_displs, key_type, comm);
  if (total_samples == 0) {
    status = 0; /* every task holds zero keys: nothing to rank, on every task */
    goto cleanup;
  }
  qsort(samples, (size_t)total_samples, sizeof(*samples), compare_keys);

  /* 3. Bucket b holds the keys from splitter b - 1 (inclusive) to splitter b
   *    (exclusive), splitter j being samples[(j + 1) * total / ntask]. Equal
   *    splitters make empty buckets; a bucket may hold every key. */
  send_counts = mymalloc_cat((size_t)ntask * sizeof(*send_counts), MEM_UTILITY);
  send_displs = mymalloc_cat((size_t)ntask * sizeof(*send_displs), MEM_UTILITY);
  for (int b = 0, start = 0; b < ntask; b++) {
    int64_t end = count;
    if (b < ntask - 1) {
      const int64_t splitter = (int64_t)(b + 1) * total_samples / ntask;
      end = lower_bound_key(sorted, count, &samples[splitter]);
    }
    send_displs[b] = start;
    send_counts[b] = (int)(end - start);
    start = (int)end;
  }

  recv_counts = mymalloc_cat((size_t)ntask * sizeof(*recv_counts), MEM_UTILITY);
  recv_displs = mymalloc_cat((size_t)ntask * sizeof(*recv_displs), MEM_UTILITY);
  MPI_Alltoall(send_counts, 1, MPI_INT, recv_counts, 1, MPI_INT, comm);
  for (int t = 0; t < ntask; t++) {
    bucket_count += recv_counts[t];
  }
  const int bucket_too_large = bucket_count > INT_MAX;
  if (bucket_too_large) {
    ERROR_LOG("%s: this task's bucket would receive %" PRId64 " keys, more than %d", function,
              bucket_count, INT_MAX);
  }
  if (any_task_failed(function, bucket_too_large)) {
    goto cleanup;
  }
  for (int t = 0, offset = 0; t < ntask; t++) {
    recv_displs[t] = offset;
    offset += recv_counts[t];
  }

  /* 4. Exchange keys to their bucket owners. */
  bucket = mymalloc_cat((size_t)bucket_count * sizeof(*bucket), MEM_UTILITY);
  MPI_Alltoallv(sorted, send_counts, send_displs, key_type, bucket, recv_counts, recv_displs,
                key_type, comm);

  /* 5. Rank the bucket: global offset of the bucket plus position within it. */
  MPI_Exscan(&bucket_count, &bucket_offset, 1, MPI_INT64_T, MPI_SUM, comm);
  if (current_task_id() == 0) {
    bucket_offset = 0; /* MPI_Exscan leaves task 0's result undefined */
  }
  int64_t duplicate = 0;
  const int bucket_duplicate = find_duplicate_id(bucket, bucket_count, &duplicate);
  if (bucket_duplicate) {
    ERROR_LOG("%s: id %" PRId64 " appears on more than one task", function, duplicate);
  }
  if (any_task_failed(function, bucket_duplicate)) {
    goto cleanup;
  }
  struct RankEntry *bucket_entries = sorted_entries(bucket, bucket_count);
  bucket_ranks = mymalloc_cat((size_t)bucket_count * sizeof(*bucket_ranks), MEM_UTILITY);
  for (int64_t pos = 0; pos < bucket_count; pos++) {
    bucket_ranks[bucket_entries[pos].index] = bucket_offset + pos;
  }
  myfree(bucket_entries);

  /* 6. Ranks travel back along the same counts, arriving in this task's
   *    sorted order, and are unpermuted into the caller's order. */
  sorted_ranks = mymalloc_cat((size_t)count * sizeof(*sorted_ranks), MEM_UTILITY);
  MPI_Alltoallv(bucket_ranks, recv_counts, recv_displs, MPI_INT64_T, sorted_ranks, send_counts,
                send_displs, MPI_INT64_T, comm);
  for (int64_t i = 0; i < count; i++) {
    ranks[order[i]] = sorted_ranks[i];
  }
  status = 0;

cleanup:
  myfree(sorted_ranks);
  myfree(bucket_ranks);
  myfree(bucket);
  myfree(recv_displs);
  myfree(recv_counts);
  myfree(send_displs);
  myfree(send_counts);
  myfree(samples);
  myfree(sample_displs);
  myfree(sample_counts);
  myfree(sample);
  myfree(order);
  myfree(sorted);
  MPI_Type_free(&key_type);
  return status;
}

#endif /* MPI */

/* ==========================================================================
 * Public collectives
 * ========================================================================== */

int module_snapshot_rank(const struct SnapshotContext *ctx, const struct SnapshotRankKey *keys,
                         int64_t count, int64_t *ranks) {
  (void)ctx;
  if (!collective_allowed("module_snapshot_rank")) {
    return -1;
  }

#ifdef MPI
  if (run_is_distributed()) {
    const int local_failed = validate_rank_keys(keys, count, ranks, INT_MAX) != 0;
    return rank_across_tasks(keys, count, ranks, local_failed);
  }
#endif

  if (validate_rank_keys(keys, count, ranks, INT64_MAX) != 0) {
    return -1;
  }
  if (count == 0) {
    return 0;
  }
  struct RankEntry *entries = sorted_entries(keys, count);
  for (int64_t pos = 0; pos < count; pos++) {
    ranks[entries[pos].index] = pos;
  }
  myfree(entries);
  return 0;
}

/**
 * @brief   Shared argument check of the reductions
 *
 * @return  1 when @p n and the arrays are usable, 0 after logging why not
 */
static int reduction_arguments_valid(const char *function, int n, const void *first,
                                     const void *second) {
  if (n < 0 || (n > 0 && (first == NULL || second == NULL))) {
    ERROR_LOG("%s: invalid arguments (n=%d)", function, n);
    return 0;
  }
  return 1;
}

/**
 * @brief   Shared prologue of the reductions: gate, argument check, serial identity, agreement
 *
 * Serial runs and non-MPI builds reduce over one task, so the caller's values already are the
 * result. Under distribution the tasks first agree on the arguments (see
 * agree_reduction_arguments()), and every task then takes the same decision.
 *
 * @param   function  Collective being called, for the messages
 * @param   n         Element count the caller passed
 * @param   first     First array the caller passed (may be NULL when n == 0)
 * @param   second    Second array the caller passed (may be NULL when n == 0)
 * @param   agreed_n  Set to the element count to reduce: 0 when there is nothing to reduce
 *                    (serial run, or n == 0 on every task), identically on every task
 * @return  0 to carry on (reducing when *agreed_n > 0), -1 when the call is refused or invalid
 */
static int reduction_prepare(const char *function, int n, const void *first, const void *second,
                             int *agreed_n) {
  *agreed_n = 0;
  if (!collective_allowed(function)) {
    return -1;
  }
  const int valid = reduction_arguments_valid(function, n, first, second);
  if (!run_is_distributed()) {
    return valid ? 0 : -1; /* the reduction over one task is its own values */
  }
#ifdef MPI
  if (agree_reduction_arguments(function, n, valid, agreed_n) != 0) {
    return -1;
  }
#endif
  return 0;
}

#ifdef MPI
typedef MPI_Datatype ReductionType;
typedef MPI_Op ReductionOp;
#define REDUCTION_I64 MPI_INT64_T
#define REDUCTION_F64 MPI_DOUBLE
#define REDUCTION_SUM MPI_SUM
#else
/* Placeholders keeping reduce_in_place()'s signature the same without MPI, where they are unused.
 */
typedef int ReductionType;
typedef int ReductionOp;
#define REDUCTION_I64 0
#define REDUCTION_F64 0
#define REDUCTION_SUM 0
#endif

/**
 * @brief   Reduce one array in place across tasks: the whole body of the scalar sums
 *
 * @param   function  Collective being called, for the messages
 * @param   values    Array reduced in place; may be NULL when n == 0
 * @param   n         Element count this task passed; the tasks agree on it before reducing
 * @param   type      Element datatype
 * @param   op        Reduction operation
 * @return  0 on success, -1 on every task when the call is refused, invalid or inconsistent
 */
static int reduce_in_place(const char *function, void *values, int n, ReductionType type,
                           ReductionOp op) {
  int agreed_n = 0;
  if (reduction_prepare(function, n, values, values, &agreed_n) != 0) {
    return -1;
  }
#ifdef MPI
  if (agreed_n > 0) {
    MPI_Allreduce(MPI_IN_PLACE, values, agreed_n, type, op, MPI_COMM_WORLD);
  }
#else
  (void)type;
  (void)op;
#endif
  return 0;
}

int module_snapshot_sum_i64(const struct SnapshotContext *ctx, int64_t *values, int n) {
  (void)ctx;
  return reduce_in_place("module_snapshot_sum_i64", values, n, REDUCTION_I64, REDUCTION_SUM);
}

int module_snapshot_sum_f64(const struct SnapshotContext *ctx, double *values, int n) {
  (void)ctx;
  return reduce_in_place("module_snapshot_sum_f64", values, n, REDUCTION_F64, REDUCTION_SUM);
}

int module_snapshot_min_max_f64(const struct SnapshotContext *ctx, double *minima, double *maxima,
                                int n) {
  (void)ctx;
  int agreed_n = 0;
  if (reduction_prepare("module_snapshot_min_max_f64", n, minima, maxima, &agreed_n) != 0) {
    return -1;
  }
#ifdef MPI
  if (agreed_n > 0) { /* the arrays may be NULL when n == 0 */
    MPI_Allreduce(MPI_IN_PLACE, minima, agreed_n, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, maxima, agreed_n, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  }
#endif
  return 0;
}

int module_snapshot_any(const struct SnapshotContext *ctx, int flag) {
  (void)ctx;
  if (!collective_allowed("module_snapshot_any")) {
    return -1;
  }
  int any = flag != 0;
#ifdef MPI
  if (run_is_distributed()) {
    const int local = any;
    MPI_Allreduce(&local, &any, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);
  }
#endif
  return any ? 1 : 0;
}

int module_snapshot_is_root_task(void) { return !run_is_distributed() || current_task_id() == 0; }
