/**
 * @file    rate_assembly.c
 * @brief   The rate phase: startup plan, component gather, law adapter and write-back
 *
 * rate_assembly.h states the contract. The plan holds the configured rate
 * modules by name, their transfers in global transfer order and their
 * observers, the row layout of a component's entries and the integration
 * settings. A segment's tables are rebuilt from the workspace at every
 * substep, so a discrete phase may add or retire rows between segments.
 *
 * Indexing. With P entries per row and T configured transfers, entry p of a
 * component's row r has index r * P + p, and transfer t of row r is flux
 * r * T + t, so failures map back to a row, a property and a transfer by
 * division alone.
 */

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "fof_workspace.h"
#include "memory.h"
#include "rate_assembly.h"
#include "rate_integrator.h"
#include "types.h"
#include "generated/reservoir_table.h"

/** Alignment of every array carved from one allocation (struct Halo and double included). */
#define CARVE_ALIGN ((size_t)_Alignof(max_align_t))

/** One configured transfer, in global transfer order. */
struct PlanTransfer {
  const struct Module *module;
  const struct RateTransfer *transfer;
  int master; /* plan index of a slaved source's master, else -1 */
};

/** One configured observer, ordered by (property, observed transfer's global index). */
struct PlanObserver {
  int property; /* reservoir table index */
  int transfer; /* plan transfer index */
};

struct RateAssemblyPlan {
  const char *phase_name;

  int num_modules;
  const struct Module **modules; /* [num_modules], by name */
  int *slot_base;                /* [num_modules]: first slot_transfer index of a module */
  int *slot_transfer;            /* [num_transfers]: module slot -> plan transfer */

  int num_transfers;
  struct PlanTransfer *transfers; /* [num_transfers], by global index */

  int num_observers;
  struct PlanObserver *observers; /* [num_observers] */

  int entries_per_row;
  int *entry_slot;    /* [MIMIC_RESERVOIR_COUNT]: row slot of a reservoir or tracer, else -1 */
  int *slot_property; /* [entries_per_row]: reservoir table index of each row slot */
  int *slot_observer; /* [entries_per_row]: plan observer of each row slot, else -1 */

  bool whole_group;
  bool needs_central; /* a transfer goes to the central or a module reads it */
  int max_coefficients;
  struct RateIntegratorSettings settings;
};

/* ---------------------------------------------------------------------------
 * Names for diagnostics
 * ------------------------------------------------------------------------- */

static const char *transfer_kind_name(enum RateTransferKind kind) {
  switch (kind) {
  case RATE_TRANSFER_INTERNAL:
    return "internal";
  case RATE_TRANSFER_SINK:
    return "sink";
  case RATE_TRANSFER_SOURCE:
    return "source";
  case RATE_TRANSFER_SLAVED_SOURCE:
    return "slaved source";
  }
  return "unknown kind";
}

static const char *reservoir_kind_name(enum ReservoirKind kind) {
  switch (kind) {
  case RESERVOIR_KIND_RESERVOIR:
    return "reservoir";
  case RESERVOIR_KIND_TRACER:
    return "tracer";
  case RESERVOIR_KIND_OBSERVER:
    return "observer";
  }
  return "unknown kind";
}

/** @brief The reservoir table entry at @p index, or NULL when the index is outside the table */
static const struct ReservoirEntry *table_entry(int index) {
  const int count = MIMIC_RESERVOIR_COUNT;
  if (index < 0 || index >= count) {
    return NULL;
  }
  return &mimic_reservoir_table()[index];
}

/** @brief The name of total @p total (or "(none)") */
static const char *total_name(int total) {
  const int count = MIMIC_RESERVOIR_TOTAL_COUNT;
  if (total < 0 || total >= count) {
    return "(none)";
  }
  return mimic_reservoir_total_names()[total];
}

/** @brief "external", a property name with its kind, or an out-of-table index, for refusals */
static const char *describe_end(int index, char *buffer, size_t size) {
  if (index == RATE_TRANSFER_EXTERNAL) {
    snprintf(buffer, size, "external");
    return buffer;
  }
  const struct ReservoirEntry *entry = table_entry(index);
  if (entry == NULL) {
    snprintf(buffer, size, "reservoir table index %d (outside the table)", index);
  } else {
    snprintf(buffer, size, "'%s' (a %s)", entry->name, reservoir_kind_name(entry->kind));
  }
  return buffer;
}

/* ---------------------------------------------------------------------------
 * Startup checks
 * ------------------------------------------------------------------------- */

/** @brief Whether @p index names a table entry of kind @p kind */
static bool end_is(int index, enum ReservoirKind kind) {
  const struct ReservoirEntry *entry = table_entry(index);
  return entry != NULL && entry->kind == kind;
}

/**
 * @brief   The refusal of a transfer's ends against the reservoir table, or NULL when they hold
 *
 * Writes the reason into @p buffer. An internal transfer runs between two
 * reservoirs of one total; a sink leaves a reservoir for outside and has no
 * recipient record; a source or slaved source enters a reservoir or a tracer
 * from outside.
 */
static const char *transfer_end_refusal(const struct RateTransfer *t, char *buffer, size_t size) {
  char from[160];
  char to[160];
  describe_end(t->from, from, sizeof(from));
  describe_end(t->to, to, sizeof(to));

  if (t->to_record != RATE_TRANSFER_TO_SELF && t->to_record != RATE_TRANSFER_TO_CENTRAL) {
    snprintf(buffer, size, "has an unknown recipient record value %d", (int)t->to_record);
    return buffer;
  }
  switch (t->kind) {
  case RATE_TRANSFER_INTERNAL:
    if (!end_is(t->from, RESERVOIR_KIND_RESERVOIR) || !end_is(t->to, RESERVOIR_KIND_RESERVOIR)) {
      snprintf(buffer, size,
               "runs from %s to %s; the reservoir table must hold both ends of an internal "
               "transfer as reservoirs",
               from, to);
      return buffer;
    }
    if (table_entry(t->from)->total != table_entry(t->to)->total) {
      snprintf(buffer, size,
               "runs from %s in total '%s' to %s in total '%s'; an internal transfer stays "
               "within one total",
               from, total_name(table_entry(t->from)->total), to,
               total_name(table_entry(t->to)->total));
      return buffer;
    }
    return NULL;
  case RATE_TRANSFER_SINK:
    if (!end_is(t->from, RESERVOIR_KIND_RESERVOIR) || t->to != RATE_TRANSFER_EXTERNAL ||
        t->to_record != RATE_TRANSFER_TO_SELF) {
      snprintf(buffer, size,
               "runs from %s to %s; a sink must leave a reservoir for external, with no "
               "recipient record",
               from, to);
      return buffer;
    }
    return NULL;
  case RATE_TRANSFER_SOURCE:
  case RATE_TRANSFER_SLAVED_SOURCE:
    if (t->from != RATE_TRANSFER_EXTERNAL ||
        !(end_is(t->to, RESERVOIR_KIND_RESERVOIR) || end_is(t->to, RESERVOIR_KIND_TRACER))) {
      snprintf(buffer, size,
               "runs from %s to %s; a source must enter a reservoir or a tracer from external",
               from, to);
      return buffer;
    }
    if ((t->kind == RATE_TRANSFER_SLAVED_SOURCE) != (t->slaved_master >= 0)) {
      snprintf(buffer, size, "is a %s with master global index %d", transfer_kind_name(t->kind),
               t->slaved_master);
      return buffer;
    }
    return NULL;
  }
  snprintf(buffer, size, "has an unknown kind value %d", (int)t->kind);
  return buffer;
}

/** @brief Log the first line of every refusal and return -1 */
static int refuse_header(const char *phase_name) {
  ERROR_LOG("Configuration error in phase '%s':", phase_name);
  return -1;
}

/** @brief Check the settings' ranges (the run-file parser guarantees them for a run file) */
static int check_settings(const char *phase_name, const struct MimicRateIntegration *in,
                          struct RateIntegratorSettings *out) {
  rate_integrator_default_settings(out);
  if (in->scheme != RATE_SCHEME_EXPLICIT && in->scheme != RATE_SCHEME_IMPLICIT) {
    refuse_header(phase_name);
    ERROR_LOG("  modules.rate_integration.scheme has value %d; it must be explicit or implicit",
              in->scheme);
    return -1;
  }
  if (!(in->relative_tolerance > 0.0 && in->relative_tolerance < 1.0) ||
      !(in->absolute_tolerance > 0.0 && in->absolute_tolerance < 1.0)) {
    refuse_header(phase_name);
    ERROR_LOG("  modules.rate_integration tolerances (relative %g, absolute %g) must lie in (0, 1)",
              in->relative_tolerance, in->absolute_tolerance);
    return -1;
  }
  if (in->max_steps_per_segment < 1 || in->max_rejections_per_step < 1) {
    refuse_header(phase_name);
    ERROR_LOG("  modules.rate_integration budgets (max_steps_per_segment %" PRId64
              ", max_rejections_per_step %d) must be at least 1",
              in->max_steps_per_segment, in->max_rejections_per_step);
    return -1;
  }
  out->scheme = (enum RateScheme)in->scheme;
  out->relative_tolerance = in->relative_tolerance;
  out->absolute_tolerance = in->absolute_tolerance;
  out->max_steps_per_segment = in->max_steps_per_segment;
  out->max_rejections_per_step = in->max_rejections_per_step;
  return 0;
}

static int compare_plan_transfers(const void *a, const void *b) {
  const int ga = ((const struct PlanTransfer *)a)->transfer->global_index;
  const int gb = ((const struct PlanTransfer *)b)->transfer->global_index;
  return (ga > gb) - (ga < gb);
}

/** Context of compare_plan_observers(); qsort has no user argument */
static const struct PlanTransfer *observer_sort_transfers = NULL;

static int compare_plan_observers(const void *a, const void *b) {
  const struct PlanObserver *oa = a;
  const struct PlanObserver *ob = b;
  if (oa->property != ob->property) {
    return (oa->property > ob->property) - (oa->property < ob->property);
  }
  const int ga = observer_sort_transfers[oa->transfer].transfer->global_index;
  const int gb = observer_sort_transfers[ob->transfer].transfer->global_index;
  return (ga > gb) - (ga < gb);
}

/** @brief Round @p bytes up to the carve alignment */
static size_t align_up(size_t bytes) {
  return (bytes + CARVE_ALIGN - 1) / CARVE_ALIGN * CARVE_ALIGN;
}

/** @brief Reserve @p count elements of @p size at the aligned @p *offset; returns their offset */
static size_t carve(size_t *offset, size_t count, size_t size) {
  const size_t at = align_up(*offset);
  *offset = at + count * size;
  return at;
}

/** @brief Check each transfer's ends and coefficient slot; fills plan->transfers unsorted */
static int collect_transfers(struct RateAssemblyPlan *plan) {
  char reason[512];
  int t = 0;
  for (int m = 0; m < plan->num_modules; m++) {
    const struct Module *mod = plan->modules[m];
    plan->slot_base[m] = t;
    for (int k = 0; k < mod->num_rate_transfers; k++) {
      const struct RateTransfer *tr = &mod->rate_transfers[k];
      const char *refusal = transfer_end_refusal(tr, reason, sizeof(reason));
      if (refusal == NULL && tr->coefficient_index != k) {
        snprintf(reason, sizeof(reason), "has coefficient index %d at position %d of its table",
                 tr->coefficient_index, k);
        refusal = reason;
      }
      if (refusal == NULL && tr->global_index < 0) {
        snprintf(reason, sizeof(reason), "has the negative global index %d", tr->global_index);
        refusal = reason;
      }
      if (refusal != NULL) {
        refuse_header(plan->phase_name);
        ERROR_LOG("  Module '%s' transfer '%s' (%s) %s", mod->name, tr->name,
                  transfer_kind_name(tr->kind), refusal);
        ERROR_LOG("  The module's generated metadata does not match the reservoir table: "
                  "regenerate with 'make generate'");
        return -1;
      }
      plan->transfers[t++] = (struct PlanTransfer){.module = mod, .transfer = tr, .master = -1};
    }
    if (mod->num_rate_transfers > plan->max_coefficients) {
      plan->max_coefficients = mod->num_rate_transfers;
    }
  }
  return 0;
}

/** @brief Sort the transfers, refuse a shared global index, and map module slots to them */
static int order_transfers(struct RateAssemblyPlan *plan) {
  if (plan->num_transfers > 1) {
    qsort(plan->transfers, (size_t)plan->num_transfers, sizeof(struct PlanTransfer),
          compare_plan_transfers);
  }
  for (int t = 0; t < plan->num_transfers; t++) {
    const struct PlanTransfer *pt = &plan->transfers[t];
    if (t > 0 && plan->transfers[t - 1].transfer->global_index == pt->transfer->global_index) {
      const struct PlanTransfer *prev = &plan->transfers[t - 1];
      refuse_header(plan->phase_name);
      ERROR_LOG("  Transfers '%s.%s' and '%s.%s' share global transfer index %d",
                prev->module->name, prev->transfer->name, pt->module->name, pt->transfer->name,
                pt->transfer->global_index);
      return -1;
    }
    for (int m = 0; m < plan->num_modules; m++) {
      if (plan->modules[m] == pt->module) {
        plan->slot_transfer[plan->slot_base[m] + pt->transfer->coefficient_index] = t;
        break;
      }
    }
  }
  return 0;
}

/** @brief Resolve every slaved source's master among the configured transfers */
static int resolve_masters(struct RateAssemblyPlan *plan) {
  for (int t = 0; t < plan->num_transfers; t++) {
    struct PlanTransfer *pt = &plan->transfers[t];
    if (pt->transfer->kind != RATE_TRANSFER_SLAVED_SOURCE) {
      continue;
    }
    const char *declared = pt->transfer->slaved_to != NULL ? pt->transfer->slaved_to : "(unnamed)";
    for (int u = 0; u < plan->num_transfers; u++) {
      if (plan->transfers[u].transfer->global_index == pt->transfer->slaved_master) {
        pt->master = u;
        break;
      }
    }
    if (pt->master < 0) {
      refuse_header(plan->phase_name);
      ERROR_LOG("  Module '%s' transfer '%s' is a slaved source of '%s' (global index %d), whose "
                "module is not configured in this rate phase",
                pt->module->name, pt->transfer->name, declared, pt->transfer->slaved_master);
      ERROR_LOG("  Fix: add the master's module to phase '%s' as process_rates", plan->phase_name);
      return -1;
    }
    const enum RateTransferKind master_kind = plan->transfers[pt->master].transfer->kind;
    if (master_kind != RATE_TRANSFER_INTERNAL && master_kind != RATE_TRANSFER_SINK) {
      refuse_header(plan->phase_name);
      ERROR_LOG("  Module '%s' transfer '%s' is a slaved source of '%s', a %s; a master must be "
                "an internal transfer or a sink",
                pt->module->name, pt->transfer->name, declared, transfer_kind_name(master_kind));
      return -1;
    }
  }
  return 0;
}

/** @brief Check and order the observers */
static int collect_observers(struct RateAssemblyPlan *plan) {
  int o = 0;
  for (int m = 0; m < plan->num_modules; m++) {
    const struct Module *mod = plan->modules[m];
    for (int k = 0; k < mod->num_rate_observers; k++) {
      const struct RateObserver *ob = &mod->rate_observers[k];
      if (!end_is(ob->property, RESERVOIR_KIND_OBSERVER) || ob->transfer < 0 ||
          ob->transfer >= mod->num_rate_transfers) {
        char property[160];
        refuse_header(plan->phase_name);
        ERROR_LOG("  Module '%s' declares an observer of %s on transfer slot %d; an observer "
                  "names a property the reservoir table holds as an observer and one of its "
                  "module's %d transfers",
                  mod->name, describe_end(ob->property, property, sizeof(property)), ob->transfer,
                  mod->num_rate_transfers);
        return -1;
      }
      plan->observers[o++] =
          (struct PlanObserver){.property = ob->property,
                                .transfer = plan->slot_transfer[plan->slot_base[m] + ob->transfer]};
    }
  }
  if (plan->num_observers > 1) {
    observer_sort_transfers = plan->transfers;
    qsort(plan->observers, (size_t)plan->num_observers, sizeof(struct PlanObserver),
          compare_plan_observers);
    observer_sort_transfers = NULL;
  }
  return 0;
}

/** @brief Refuse a dependency cycle among the totals, naming them */
static int check_total_cycle(const struct RateAssemblyPlan *plan) {
  const int n = MIMIC_RESERVOIR_TOTAL_COUNT;
  if (n == 0) {
    return 0;
  }
  int *depends = mymalloc_cat((size_t)n * (size_t)n * sizeof(int), MEM_UTILITY);
  int *cycle = mymalloc_cat((size_t)(n + 1) * sizeof(int), MEM_UTILITY);
  memset(depends, 0, (size_t)n * (size_t)n * sizeof(int));

  /* A tracer's total depends on its parent's total. */
  const int count = MIMIC_RESERVOIR_COUNT;
  for (int e = 0; e < count; e++) {
    const struct ReservoirEntry *entry = table_entry(e);
    const struct ReservoirEntry *parent = table_entry(entry->parent);
    if (entry->kind == RESERVOIR_KIND_TRACER && parent != NULL && entry->total >= 0 &&
        parent->total >= 0) {
      depends[entry->total * n + parent->total] = 1;
    }
  }
  /* A slaved source's recipient total depends on its master's donor total. */
  for (int t = 0; t < plan->num_transfers; t++) {
    const struct PlanTransfer *pt = &plan->transfers[t];
    if (pt->master < 0) {
      continue;
    }
    const int recipient = table_entry(pt->transfer->to)->total;
    const int donor = table_entry(plan->transfers[pt->master].transfer->from)->total;
    depends[recipient * n + donor] = 1;
  }

  const int length = rate_assembly_find_total_cycle(n, depends, cycle);
  int result = 0;
  if (length > 0) {
    char names[1024];
    size_t used = 0;
    names[0] = '\0';
    for (int k = 0; k < length && used < sizeof(names); k++) {
      const int written = snprintf(names + used, sizeof(names) - used, "%s'%s'",
                                   k == 0 ? "" : " -> ", total_name(cycle[k]));
      if (written < 0) {
        break;
      }
      used += (size_t)written;
    }
    refuse_header(plan->phase_name);
    ERROR_LOG("  The totals of the configured rate modules form a dependency cycle: %s (each "
              "total depends on the next: a slaved source's recipient total on its master's "
              "donor total, a tracer's total on its parent's)",
              names);
    result = -1;
  }
  myfree(cycle);
  myfree(depends);
  return result;
}

/** @brief Lay out one row's entries: reservoirs and tracers, and observers, in table order */
static void lay_out_row(struct RateAssemblyPlan *plan) {
  const int count = MIMIC_RESERVOIR_COUNT;
  int p = 0;
  for (int e = 0; e < count; e++) {
    plan->entry_slot[e] = -1;
    if (table_entry(e)->kind != RESERVOIR_KIND_OBSERVER) {
      plan->entry_slot[e] = p;
      plan->slot_property[p] = e;
      plan->slot_observer[p] = -1;
      p++;
      continue;
    }
    for (int o = 0; o < plan->num_observers; o++) {
      if (plan->observers[o].property == e) {
        plan->slot_property[p] = e;
        plan->slot_observer[p] = o;
        p++;
      }
    }
  }
  plan->entries_per_row = p;
}

static void insert_module_by_name(const struct Module **modules, int count,
                                  const struct Module *mod) {
  int at = count;
  while (at > 0 && strcmp(modules[at - 1]->name, mod->name) > 0) {
    modules[at] = modules[at - 1];
    at--;
  }
  modules[at] = mod;
}

int rate_assembly_plan_build(const char *phase_name, const struct PhaseModuleConfig *entries,
                             int num_entries, const struct MimicRateIntegration *settings,
                             struct RateAssemblyPlan **plan_out) {
  *plan_out = NULL;
  struct RateIntegratorSettings checked;
  if (check_settings(phase_name, settings, &checked) != 0) {
    return -1;
  }

  int num_transfers = 0;
  int num_observers = 0;
  for (int i = 0; i < num_entries; i++) {
    if (entries[i].resolved == NULL) {
      FATAL_ERROR("Rate phase '%s': module '%s' was not resolved before its plan was built",
                  phase_name, entries[i].module_name);
    }
    num_transfers += entries[i].resolved->num_rate_transfers;
    num_observers += entries[i].resolved->num_rate_observers;
  }
  const int num_reservoirs = MIMIC_RESERVOIR_COUNT;
  const int max_slots = num_reservoirs + num_observers;

  size_t bytes = 0;
  const size_t at_plan = carve(&bytes, 1, sizeof(struct RateAssemblyPlan));
  const size_t at_modules = carve(&bytes, (size_t)num_entries, sizeof(const struct Module *));
  const size_t at_slot_base = carve(&bytes, (size_t)num_entries, sizeof(int));
  const size_t at_slot_transfer = carve(&bytes, (size_t)num_transfers, sizeof(int));
  const size_t at_transfers = carve(&bytes, (size_t)num_transfers, sizeof(struct PlanTransfer));
  const size_t at_observers = carve(&bytes, (size_t)num_observers, sizeof(struct PlanObserver));
  const size_t at_entry_slot = carve(&bytes, (size_t)num_reservoirs, sizeof(int));
  const size_t at_slot_property = carve(&bytes, (size_t)max_slots, sizeof(int));
  const size_t at_slot_observer = carve(&bytes, (size_t)max_slots, sizeof(int));

  char *base = mymalloc_cat(bytes, MEM_UTILITY);
  memset(base, 0, bytes);
  struct RateAssemblyPlan *plan = (struct RateAssemblyPlan *)(void *)(base + at_plan);
  plan->phase_name = phase_name;
  plan->modules = (const struct Module **)(void *)(base + at_modules);
  plan->slot_base = (int *)(void *)(base + at_slot_base);
  plan->slot_transfer = (int *)(void *)(base + at_slot_transfer);
  plan->transfers = (struct PlanTransfer *)(void *)(base + at_transfers);
  plan->observers = (struct PlanObserver *)(void *)(base + at_observers);
  plan->entry_slot = (int *)(void *)(base + at_entry_slot);
  plan->slot_property = (int *)(void *)(base + at_slot_property);
  plan->slot_observer = (int *)(void *)(base + at_slot_observer);
  plan->num_transfers = num_transfers;
  plan->num_observers = num_observers;
  plan->settings = checked;

  for (int i = 0; i < num_entries; i++) {
    insert_module_by_name(plan->modules, plan->num_modules++, entries[i].resolved);
  }

  if (collect_transfers(plan) != 0 || order_transfers(plan) != 0 || resolve_masters(plan) != 0 ||
      collect_observers(plan) != 0 || check_total_cycle(plan) != 0) {
    rate_assembly_plan_free(plan);
    return -1;
  }
  lay_out_row(plan);

  bool to_central = false;
  bool reads_central = false;
  for (int t = 0; t < plan->num_transfers; t++) {
    to_central |= plan->transfers[t].transfer->to_record == RATE_TRANSFER_TO_CENTRAL;
  }
  for (int m = 0; m < plan->num_modules; m++) {
    reads_central |= plan->modules[m]->reads_central;
  }
  plan->needs_central = to_central || reads_central;
  plan->whole_group = plan->needs_central;

  VERBOSE_LOG("Rate phase '%s': %d module(s), %d transfer(s), %d observer(s); %s; %s scheme",
              phase_name, plan->num_modules, plan->num_transfers, plan->num_observers,
              plan->whole_group ? "each FoF group is one component"
                                : "each galaxy is its own component",
              plan->settings.scheme == RATE_SCHEME_IMPLICIT ? "implicit" : "explicit");
  *plan_out = plan;
  return 0;
}

void rate_assembly_plan_free(struct RateAssemblyPlan *plan) {
  if (plan != NULL) {
    myfree(plan); /* the plan struct is the first object of its one allocation */
  }
}

/* ---------------------------------------------------------------------------
 * Dependency cycles
 * ------------------------------------------------------------------------- */

/** @brief Depth-first search from @p node; returns the cycle length written, or 0 */
static int find_cycle_from(int node, int n, const int *depends, int *state, int *path, int depth,
                           int *cycle) {
  state[node] = 1;
  path[depth] = node;
  for (int next = 0; next < n; next++) {
    if (depends[node * n + next] == 0) {
      continue;
    }
    if (state[next] == 1) {
      int start = depth;
      while (path[start] != next) {
        start--;
      }
      int length = 0;
      for (int k = start; k <= depth; k++) {
        cycle[length++] = path[k];
      }
      cycle[length++] = next;
      return length;
    }
    if (state[next] == 0) {
      const int length = find_cycle_from(next, n, depends, state, path, depth + 1, cycle);
      if (length > 0) {
        return length;
      }
    }
  }
  state[node] = 2;
  return 0;
}

int rate_assembly_find_total_cycle(int num_totals, const int *depends, int *cycle) {
  if (num_totals <= 0) {
    return 0;
  }
  int *state = mymalloc_cat((size_t)num_totals * 2 * sizeof(int), MEM_UTILITY);
  int *path = state + num_totals;
  memset(state, 0, (size_t)num_totals * sizeof(int));
  int length = 0;
  for (int node = 0; node < num_totals && length == 0; node++) {
    if (state[node] == 0) {
      length = find_cycle_from(node, num_totals, depends, state, path, 0, cycle);
    }
  }
  myfree(state);
  return length;
}

/* ---------------------------------------------------------------------------
 * One segment
 * ------------------------------------------------------------------------- */

/** One component's tables, carved from the scratch allocation. */
struct Segment {
  int num_rows;
  int central; /* the central's component row, or -1 */
  int *rows;   /* [num_rows]: workspace row of each component row */
  double *dT;  /* [num_rows]: each row's dT, 0 for a sentinel row */
  double *delta;
  struct Halo *halos;          /* [num_rows] scratch rows */
  struct GalaxyData *galaxies; /* [num_rows] scratch galaxies */
  struct RateEntry *entries;   /* [num_rows * P] */
  double *state;               /* [num_rows * P] */
  struct RateFlux *fluxes;     /* [num_rows * T] */
  double *coefficients;        /* [max_coefficients] one law's slots */
  void *integrator;            /* the integrator's workspace */
  size_t integrator_bytes;
};

/** Byte offsets of a segment's tables within the scratch allocation. */
struct SegmentLayout {
  size_t rows, dT, delta, halos, galaxies, entries, state, fluxes, coefficients, integrator;
  size_t total;
};

static void lay_out_segment(const struct RateAssemblyPlan *plan, int num_rows,
                            size_t integrator_bytes, struct SegmentLayout *l) {
  const size_t r = (size_t)num_rows;
  const size_t entries = r * (size_t)plan->entries_per_row;
  const size_t fluxes = r * (size_t)plan->num_transfers;
  size_t offset = 0;
  l->rows = carve(&offset, r, sizeof(int));
  l->dT = carve(&offset, r, sizeof(double));
  l->delta = carve(&offset, r, sizeof(double));
  l->halos = carve(&offset, r, sizeof(struct Halo));
  l->galaxies = carve(&offset, r, sizeof(struct GalaxyData));
  l->entries = carve(&offset, entries, sizeof(struct RateEntry));
  l->state = carve(&offset, entries, sizeof(double));
  l->fluxes = carve(&offset, fluxes, sizeof(struct RateFlux));
  l->coefficients = carve(&offset, (size_t)plan->max_coefficients, sizeof(double));
  l->integrator = carve(&offset, integrator_bytes, 1);
  l->total = align_up(offset);
}

static void bind_segment(const struct SegmentLayout *l, void *bytes, size_t integrator_bytes,
                         struct Segment *seg) {
  char *base = bytes;
  seg->rows = (int *)(void *)(base + l->rows);
  seg->dT = (double *)(void *)(base + l->dT);
  seg->delta = (double *)(void *)(base + l->delta);
  seg->halos = (struct Halo *)(void *)(base + l->halos);
  seg->galaxies = (struct GalaxyData *)(void *)(base + l->galaxies);
  seg->entries = (struct RateEntry *)(void *)(base + l->entries);
  seg->state = (double *)(void *)(base + l->state);
  seg->fluxes = (struct RateFlux *)(void *)(base + l->fluxes);
  seg->coefficients = (double *)(void *)(base + l->coefficients);
  seg->integrator = base + l->integrator;
  seg->integrator_bytes = integrator_bytes;
}

/** @brief Grow the scratch to at least @p bytes (contents kept, so the caller rebinds) */
static void reserve_scratch(struct RateAssemblyScratch *scratch, size_t bytes) {
  if (bytes <= scratch->capacity) {
    return;
  }
  size_t capacity = scratch->capacity + scratch->capacity / 2;
  if (capacity < bytes) {
    capacity = bytes;
  }
  capacity = align_up(capacity); /* the allocator rounds sizes up; keep its count exact */
  scratch->bytes = myrealloc_cat(scratch->bytes, capacity, MEM_UTILITY);
  scratch->capacity = capacity;
}

void rate_assembly_scratch_release(struct RateAssemblyScratch *scratch) {
  if (scratch->bytes != NULL) {
    /* A test-tier memory reset between init and cleanup must not abort the
     * process on a stale pointer; ./mimic never resets the memory system. */
    if (is_tracked_memory_block(scratch->bytes)) {
      myfree(scratch->bytes);
    } else {
      WARNING_LOG("Rate scratch held a pointer the memory system no longer tracks (memory "
                  "system reset without module_system_cleanup()); dropping it");
    }
  }
  scratch->bytes = NULL;
  scratch->capacity = 0;
}

static bool row_is_active(const struct Halo *halo) {
  return halo->galaxy != NULL && halo->Type != 3;
}

/** @brief A row's dT for the law's clock and its segment length; fatal on an invalid dT */
static void row_clock(const struct RateAssemblyPlan *plan, const struct ModuleContext *ctx,
                      const struct FoFWorkspace *ws, int w, double *dT, double *delta) {
  const struct Halo *h = &ws->halos[w];
  const double dt = (double)h->dT;
  if (h->SnapNum < 0 && dt <= 0.0) {
    *dT = 0.0; /* the first-snapshot sentinel: no elapsed time */
    *delta = 0.0;
    return;
  }
  if (!(dt > 0.0) || !isfinite(dt)) {
    FATAL_ERROR("Rate phase '%s': workspace row %d (UniqueGalaxyID %lld, Type %d, SnapNum %d) "
                "has dT = %.17g at snapshot %d; a row's dT must be positive and finite, or carry "
                "the first-snapshot sentinel (SnapNum < 0 and dT <= 0)",
                plan->phase_name, w, h->UniqueGalaxyID, h->Type, h->SnapNum, dt,
                ctx->snapshot_number);
  }
  *dT = dt;
  *delta = dt / (double)ctx->num_substeps;
}

/** @brief Fill the component's entries; observers in their slots, linked to their row's flux */
static void build_entries(const struct RateAssemblyPlan *plan, struct Segment *seg) {
  const int P = plan->entries_per_row;
  const int T = plan->num_transfers;
  for (int r = 0; r < seg->num_rows; r++) {
    for (int p = 0; p < P; p++) {
      const struct ReservoirEntry *entry = table_entry(plan->slot_property[p]);
      struct RateEntry *out = &seg->entries[r * P + p];
      out->row = r;
      out->parent = -1;
      out->flux = -1;
      if (entry->kind == RESERVOIR_KIND_OBSERVER) {
        out->kind = RATE_ENTRY_OBSERVER;
        out->total = 0; /* not read */
        out->flux = r * T + plan->observers[plan->slot_observer[p]].transfer;
      } else {
        out->kind = entry->kind == RESERVOIR_KIND_TRACER ? RATE_ENTRY_TRACER : RATE_ENTRY_RESERVOIR;
        out->total = entry->total;
        if (entry->kind == RESERVOIR_KIND_TRACER) {
          out->parent = r * P + plan->entry_slot[entry->parent];
        }
      }
    }
  }
}

/** @brief Fill the component's fluxes, row by row in global transfer order */
static void build_fluxes(const struct RateAssemblyPlan *plan, struct Segment *seg) {
  const int P = plan->entries_per_row;
  const int T = plan->num_transfers;
  for (int r = 0; r < seg->num_rows; r++) {
    for (int t = 0; t < T; t++) {
      const struct PlanTransfer *pt = &plan->transfers[t];
      const struct RateTransfer *tr = pt->transfer;
      const int recipient = (tr->to_record == RATE_TRANSFER_TO_CENTRAL) ? seg->central : r;
      struct RateFlux *f = &seg->fluxes[r * T + t];
      f->global_index = tr->global_index;
      f->master = -1;
      f->from = RATE_END_EXTERNAL;
      f->to = RATE_END_EXTERNAL;
      switch (tr->kind) {
      case RATE_TRANSFER_INTERNAL:
        f->kind = RATE_FLUX_INTERNAL;
        f->from = r * P + plan->entry_slot[tr->from];
        f->to = recipient * P + plan->entry_slot[tr->to];
        break;
      case RATE_TRANSFER_SINK:
        f->kind = RATE_FLUX_SINK;
        f->from = r * P + plan->entry_slot[tr->from];
        break;
      case RATE_TRANSFER_SOURCE:
        f->kind = RATE_FLUX_SOURCE;
        f->to = recipient * P + plan->entry_slot[tr->to];
        break;
      case RATE_TRANSFER_SLAVED_SOURCE:
        f->kind = RATE_FLUX_SLAVED;
        f->to = recipient * P + plan->entry_slot[tr->to];
        f->master = r * T + pt->master;
        break;
      }
    }
  }
}

/** @brief Address of the double member at reservoir table index @p e of @p galaxy */
static double *galaxy_field(struct GalaxyData *galaxy, int e) {
  return (double *)(void *)((char *)galaxy + table_entry(e)->offset);
}

/** @brief Value of the double member at reservoir table index @p e of @p galaxy */
static double galaxy_value(const struct GalaxyData *galaxy, int e) {
  return *(const double *)(const void *)((const char *)galaxy + table_entry(e)->offset);
}

/** @brief Whether row slot @p p is the first observer slot of its property */
static bool first_observer_slot(const struct RateAssemblyPlan *plan, int p) {
  return p == 0 || plan->slot_property[p - 1] != plan->slot_property[p];
}

/** @brief Write a state's reservoir, tracer and observer values into one row's galaxy */
static void store_row(const struct RateAssemblyPlan *plan, const double *row_state,
                      struct GalaxyData *galaxy) {
  const int P = plan->entries_per_row;
  for (int p = 0; p < P; p++) {
    double *field = galaxy_field(galaxy, plan->slot_property[p]);
    if (plan->slot_observer[p] < 0) {
      *field = row_state[p];
    } else if (first_observer_slot(plan, p)) {
      *field = row_state[p]; /* the first observer entry started at the property's value */
    } else {
      *field += row_state[p];
    }
  }
}

/** @brief Read one row's start state from its galaxy (observers: the first entry carries it) */
static void load_row(const struct RateAssemblyPlan *plan, const struct GalaxyData *galaxy,
                     double *row_state) {
  const int P = plan->entries_per_row;
  for (int p = 0; p < P; p++) {
    const double value = galaxy_value(galaxy, plan->slot_property[p]);
    row_state[p] = (plan->slot_observer[p] < 0 || first_observer_slot(plan, p)) ? value : 0.0;
  }
}

/** What the law adapter needs, and what it records about a law that failed. */
struct LawAdapter {
  const struct RateAssemblyPlan *plan;
  const struct ModuleContext *ctx;
  struct Segment *seg;
  RateLawInvoker invoke;
  int failed_module; /* plan module index of a law that returned non-zero, else -1 */
  int failed_row;
  int failed_code;
};

/** @brief struct RateLaw.evaluate: load the trial state, then call every law of every row */
static int evaluate_laws(void *data, double tau, const double *state, double *coefficients) {
  struct LawAdapter *a = data;
  const struct RateAssemblyPlan *plan = a->plan;
  const struct ModuleContext *ctx = a->ctx;
  struct Segment *seg = a->seg;
  const int P = plan->entries_per_row;
  const int T = plan->num_transfers;

  for (int r = 0; r < seg->num_rows; r++) {
    store_row(plan, &state[r * P], &seg->galaxies[r]);
  }

  struct RateContext rctx = {.snapshot_number = ctx->snapshot_number,
                             .redshift = ctx->redshift,
                             .time = 0.0,
                             .central = NULL,
                             .params = ctx->params};
  for (int r = 0; r < seg->num_rows; r++) {
    rctx.time = ctx->time + seg->dT[r] - ((double)ctx->substep_number + tau) * seg->delta[r];
    for (int m = 0; m < plan->num_modules; m++) {
      const struct Module *mod = plan->modules[m];
      const int n = mod->num_rate_transfers;
      if (n > 0) {
        memset(seg->coefficients, 0, (size_t)n * sizeof(double));
      }
      rctx.central = mod->reads_central ? &seg->halos[seg->central] : NULL;
      const int rc = a->invoke(mod, &rctx, &seg->halos[r], seg->coefficients);
      if (rc != 0) {
        a->failed_module = m;
        a->failed_row = r;
        a->failed_code = rc;
        return rc;
      }
      for (int k = 0; k < n; k++) {
        coefficients[r * T + plan->slot_transfer[plan->slot_base[m] + k]] = seg->coefficients[k];
      }
    }
  }
  return 0;
}

/** @brief End the run with a one-screen account of a failed segment */
static void report_failure(const struct RateAssemblyPlan *plan, const struct ModuleContext *ctx,
                           const struct FoFWorkspace *ws, const struct Segment *seg,
                           const struct LawAdapter *law, const struct RateSegmentOutcome *outcome) {
  const struct RateFailure *f = &outcome->failure;
  const int P = plan->entries_per_row;
  const int T = plan->num_transfers;
  const char *module = "(none)";
  const char *transfer = "(none)";
  const char *kind = "";
  int row = f->row;
  char transfer_buffer[128];

  if (f->flux >= 0 && T > 0) {
    const struct PlanTransfer *pt = &plan->transfers[f->flux % T];
    module = pt->module->name;
    transfer = pt->transfer->name;
    kind = transfer_kind_name(pt->transfer->kind);
    if (row < 0) {
      row = f->flux / T;
    }
  } else if (law->failed_module >= 0) {
    module = plan->modules[law->failed_module]->name;
    snprintf(transfer_buffer, sizeof(transfer_buffer), "(none named: the law returned %d)",
             law->failed_code);
    transfer = transfer_buffer;
    row = law->failed_row;
  }
  const char *entry = "(none)";
  if (f->entry >= 0 && P > 0) {
    entry = table_entry(plan->slot_property[f->entry % P])->name;
    if (row < 0) {
      row = f->entry / P;
    }
  }
  const int w = (row >= 0 && row < seg->num_rows) ? seg->rows[row] : -1;
  const long long id = w >= 0 ? ws->halos[w].UniqueGalaxyID : -1;
  const int type = w >= 0 ? ws->halos[w].Type : -1;
  const char *cause = rate_cause_name(f->cause);

  ERROR_LOG("Rate segment failed in phase '%s' at snapshot %d, substep %d of %d (a component "
            "of %d row(s), %s scheme):",
            plan->phase_name, ctx->snapshot_number, ctx->substep_number + 1, ctx->num_substeps,
            seg->num_rows, plan->settings.scheme == RATE_SCHEME_IMPLICIT ? "implicit" : "explicit");
  ERROR_LOG("  cause: %s%s%s%s", cause, (f->detail != NULL && f->detail[0] != '\0') ? " (" : "",
            f->detail != NULL ? f->detail : "",
            (f->detail != NULL && f->detail[0] != '\0') ? ")" : "");
  ERROR_LOG("  module '%s', transfer '%s'%s%s", module, transfer, kind[0] != '\0' ? ", a " : "",
            kind);
  ERROR_LOG("  row %d of the component (workspace row %d, UniqueGalaxyID %lld, Type %d), "
            "entry '%s'",
            row, w, id, type, entry);
  if (f->total >= 0) {
    ERROR_LOG("  total '%s'", total_name(f->total));
  }
  ERROR_LOG("  tau %.17g, step %.17g; %" PRId64 " accepted and %" PRId64
            " rejected step(s), %" PRId64 " law evaluation(s)",
            f->tau, f->step, outcome->stats.accepted_steps, outcome->stats.rejected_steps,
            outcome->stats.law_evaluations);
  FATAL_ERROR("Rate segment failed: cause %s, module '%s', transfer '%s', workspace row %d "
              "(UniqueGalaxyID %lld), tau %.17g",
              cause, module, transfer, w, id, f->tau);
}

/**
 * @brief   Integrate one component across the substep and write it back
 *
 * @param   single_row  The component's one workspace row, or -1 for every active row
 */
static void integrate_component(const struct RateAssemblyPlan *plan,
                                const struct ModuleContext *ctx, struct FoFWorkspace *ws,
                                struct RateAssemblyScratch *scratch, RateLawInvoker invoke,
                                int single_row, int num_rows) {
  const int P = plan->entries_per_row;
  const int T = plan->num_transfers;
  if ((int64_t)num_rows * (int64_t)(P > T ? P : T) > (int64_t)INT_MAX) {
    FATAL_ERROR("Rate phase '%s': a component of %d rows has more entries or fluxes than an int "
                "indexes",
                plan->phase_name, num_rows);
  }

  /* The tables first: their layout does not depend on the integrator's share. */
  struct SegmentLayout layout;
  lay_out_segment(plan, num_rows, 0, &layout);
  reserve_scratch(scratch, layout.total);
  struct Segment seg = {.num_rows = num_rows, .central = -1};
  bind_segment(&layout, scratch->bytes, 0, &seg);

  int r = 0;
  for (int64_t w = 0; w < ws->count; w++) {
    if ((single_row >= 0 && w != single_row) || !row_is_active(&ws->halos[w])) {
      continue;
    }
    seg.rows[r] = (int)w;
    row_clock(plan, ctx, ws, (int)w, &seg.dT[r], &seg.delta[r]);
    if (w == ctx->central_index) {
      seg.central = r;
    }
    r++;
  }
  if (plan->needs_central && seg.central < 0) {
    FATAL_ERROR("Rate phase '%s': the FoF central (workspace row %d) is not an active row (it "
                "has no galaxy or is Type 3), but a configured transfer goes to the central or "
                "a configured module reads it",
                plan->phase_name, ctx->central_index);
  }
  build_entries(plan, &seg);
  build_fluxes(plan, &seg);

  /* Then the integrator's share, sized for this component; growing keeps the tables. */
  struct RateComponent component = {.entries = seg.entries,
                                    .num_entries = num_rows * P,
                                    .fluxes = seg.fluxes,
                                    .num_fluxes = num_rows * T,
                                    .row_delta = seg.delta,
                                    .num_rows = num_rows,
                                    .num_totals = MIMIC_RESERVOIR_TOTAL_COUNT};
  const size_t integrator_bytes =
      rate_integrator_workspace_bytes(&component, plan->settings.scheme);
  lay_out_segment(plan, num_rows, integrator_bytes, &layout);
  reserve_scratch(scratch, layout.total);
  bind_segment(&layout, scratch->bytes, integrator_bytes, &seg);
  component.entries = seg.entries;
  component.fluxes = seg.fluxes;
  component.row_delta = seg.delta;

  for (r = 0; r < num_rows; r++) {
    const struct Halo *source = &ws->halos[seg.rows[r]];
    seg.galaxies[r] = *source->galaxy;
    seg.halos[r] = *source;
    seg.halos[r].galaxy = &seg.galaxies[r];
    load_row(plan, source->galaxy, &seg.state[r * P]);
  }

  struct LawAdapter adapter = {.plan = plan,
                               .ctx = ctx,
                               .seg = &seg,
                               .invoke = invoke,
                               .failed_module = -1,
                               .failed_row = -1,
                               .failed_code = 0};
  const struct RateLaw law = {.evaluate = evaluate_laws, .data = &adapter};
  struct RateSegmentOutcome outcome;
  const enum RateCause cause =
      rate_integrate_segment(&component, &law, &plan->settings, seg.integrator,
                             seg.integrator_bytes, seg.state, NULL, NULL, &outcome);
  if (cause != RATE_CAUSE_NONE) {
    report_failure(plan, ctx, ws, &seg, &adapter, &outcome);
  }

  for (r = 0; r < num_rows; r++) {
    store_row(plan, &seg.state[r * P], ws->halos[seg.rows[r]].galaxy);
  }
}

void rate_assembly_run(const struct RateAssemblyPlan *plan, const struct ModuleContext *ctx,
                       struct FoFWorkspace *ws, struct RateAssemblyScratch *scratch,
                       RateLawInvoker invoke) {
  if (plan == NULL || ctx == NULL || ws == NULL || scratch == NULL || invoke == NULL) {
    FATAL_ERROR("rate_assembly_run called without a plan, context, workspace, scratch or invoker");
  }
  if (ctx->num_substeps <= 0) {
    FATAL_ERROR("Rate phase '%s': the module context has %d substeps", plan->phase_name,
                ctx->num_substeps);
  }

  if (plan->whole_group) {
    int active = 0;
    for (int64_t w = 0; w < ws->count; w++) {
      active += row_is_active(&ws->halos[w]);
    }
    if (active > 0) {
      integrate_component(plan, ctx, ws, scratch, invoke, -1, active);
    }
    return;
  }
  for (int64_t w = 0; w < ws->count; w++) {
    if (row_is_active(&ws->halos[w])) {
      integrate_component(plan, ctx, ws, scratch, invoke, (int)w, 1);
    }
  }
}
