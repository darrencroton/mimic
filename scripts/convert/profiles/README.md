# Converter mapping profiles

Declarative column-mapping profiles for the generalised converter. A profile says which source columns fill the adapter's required roles, and which additional numeric fields to carry through. It says nothing about *how* to read a source — that is the adapter's business — and nothing about physics.

Profiles are parsed and validated by [`scripts/convert/column_schema.py`](../column_schema.py). Every file here loads through that module unchanged; the tests in [`scripts/convert/tests/test_column_schema.py`](../tests/test_column_schema.py) load all of them on every run, so a profile that drifts out of the grammar fails the converter suite rather than a later conversion.

## What ships here

| Profile | Source format | Purpose |
|---|---|---|
| `consistent_trees_ascii.yaml` | `consistent_trees_ascii` | Default: the existing ASCII selection, no extras |
| `consistent_trees_ascii_extras_example.yaml` | `consistent_trees_ascii` | Worked example: scalar, renamed-integer and assembled-vector extras |
| `consistent_trees_hdf5.yaml` | `consistent_trees_hdf5` | Default: the fields the reference C reader consumes, no extras |
| `consistent_trees_hdf5_extras_example.yaml` | `consistent_trees_hdf5` | Worked example: extras drawn from the wider `Forests/` field set |
| `lhalo_binary.yaml` | `lhalo_binary` | Default: the shipped 104-byte record, no extras |
| `lhalo_binary_extras_example.yaml` | `lhalo_binary` | Worked example: whole-scalar, renamed and vector-component extras |

The `_extras_example` profiles are documentation that runs. They are not defaults and no simulation package points at them.

## Grammar

A profile has exactly these top-level keys, and `binary_layout` only for `lhalo_binary`:

```yaml
schema_version: 1              # the only accepted version
source_format: lhalo_binary    # consistent_trees_ascii | consistent_trees_hdf5 | lhalo_binary
required_columns: {...}        # every role the adapter fixes, each with a nonempty alias list
extra_fields: [...]            # [] when nothing extra is selected; the key is still required
binary_layout: {...}           # lhalo_binary only; rejected on the other two formats
```

An unknown key fails. A missing key fails — nothing is defaulted. A duplicate YAML key fails rather than last-wins.

### `required_columns`

Maps the adapter's fixed canonical role names to nonempty alias lists. The role set is closed: a missing role and an unknown role both fail. Exactly one alias must resolve in each source file; zero matches and two matches are both fatal. That is why the snapshot role lists both spellings — a file carries `snap_num` or `snap_idx`, never both.

Alias matching is **suffix-stripped and case-insensitive for Consistent-Trees ASCII** (truncated at the first `(`, matching `src/io/vertical/ctrees/parse_ctrees.h`) and **exact for HDF5 dataset names and binary record field names** — those are stored object names, not printed header tokens, and a case-insensitive match there would invent an equivalence the source does not have.

Structural metadata — L-Halo tree headers, forests-HDF5 `ForestInfo`, the per-file group layout — is adapter-owned. It cannot be remapped or omitted, and has no role here.

### `extra_fields`

Each entry has exactly `name`, `sources`, `type`, `units`, `h_convention` and `description`.

- `name` is the emitted output name: ASCII `[A-Za-z][A-Za-z0-9_]*`, at most 63 bytes. Duplicates fail, and so does any collision with a reserved topology, identity or core payload name.
- `sources` is one entry for a scalar `type`, three for a `vec3_*` type. Each is `{field: <source name>}` for a stored scalar, or `{field: <source name>, component: 0|1|2}` for one element of a stored vector. Repeating the same `(field, component)` pair inside one output field fails.
- `type` is one of `int`, `long long`, `float`, `double`, `vec3_int`, `vec3_float` — the property generator's numeric types, so anything the converter can emit is something a simulation package can declare. Integers never pass through floating point.
- `h_convention` is `carried`, `free` or `none`.
- `units` and `description` are nonempty strings.

Reusing a source field that also fills a required role is allowed, and the extra carries that field's **pre-convention** value. `consistent_trees_ascii_extras_example.yaml` shows the case that matters: the payload `Spin` is the producer's normalised J/Mvir, while the `AngularMomentum` extra carries the raw catalog J.

**One caveat worth stating plainly.** An extra's `units` is any nonempty string, by contract. A unit outside the registry in [`scripts/generate_properties.py`](../../generate_properties.py) — `Msun/h Mpc/h km/s` is one such today — converts and validates fine, but a consuming Mimic simulation package cannot declare it until that unit is added there. The converter does not silently substitute a known unit in its place.

### `binary_layout`

`byte_order` (`little` or `big`), `itemsize`, and `offsets` mapping every source field name to its byte offset. It is validated against the ordered `simulations/<package>/halo_properties.yaml`, never guessed from host packing:

- every declared source property must have an offset, and every offset must name a declared property;
- no field may extend past `itemsize`, and no two fields may overlap;
- padding between fields is allowed — a record may carry bytes this converter never reads — but a double-claimed or out-of-record byte range is not.

A binary profile's aliases must match a property's **`name:`** spelling in that `halo_properties.yaml`, never its `source:` spelling. The two often differ — `simulations/mini-millennium/halo_properties.yaml` declares `M_Crit200` with `source: Mvir` — and only `name:` is what the layout is keyed by, so `M_Crit200: [Mvir]` is a mapping no source can resolve. It is rejected when the schema is built, not left to fail at conversion time.

Byte order is a property of the file, not of the machine reading it. Numpy's native `=` is not accepted, so reading a big-endian source on a little-endian host cannot silently succeed. The shipped L-Halo layout is the observed little-endian 104-byte record; a big-endian source needs its own profile with the same offsets and `byte_order: big`.

Selecting extras never changes the on-disk stride, and an unselected field is still covered by `offsets` — the full ordered layout is preserved either way.

## Schema identity

A validated profile is frozen into a canonical schema whose SHA-256 digest is the `column_mapping_sha256` a horizontal-HDF5 v3 file records. The digest covers the adapter, the complete resolved mapping, the source layout, the declared payload types and units, and every extra's type, sources, units and h convention.

It deliberately does **not** cover presentation: comments, YAML key order, alias order within a role and extra-definition order are all normalised away, so reformatting a profile does not invalidate a dataset produced from it. Anything that changes how a value is read, typed, named or labelled does move the digest — including two types that happen to share a width, such as `int` and `float`.

## Related documentation

- [`scripts/convert/README.md`](../README.md): the converter itself
- [`docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md`](../../../docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md): the draft v3 output contract these profiles feed
- [`docs/dev/HORIZONTAL-HDF5-FORMAT.md`](../../../docs/dev/HORIZONTAL-HDF5-FORMAT.md): the frozen v2 contract, unchanged by this work
- [`docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`](../../../docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md): contracts C1–C3, which this grammar implements
