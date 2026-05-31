# Mimic SHAM Model

This package contains a Mimic-native pseudo-SHAM model. It tracks peak halo
proxies through the normal Mimic halo inheritance path and assigns a stellar
mass from a deterministic stellar-to-halo mass relation.

## Scientific Status

This is not a true subhalo abundance matching implementation and should not be
used for serious science in its current form.

A true SHAM ranks galaxies and halos or subhalos across a complete catalogue,
usually at a snapshot or volume level, then assigns galaxy properties by a
global monotonic matching between cumulative abundances. That global ranking is
the core abundance-matching step. Mimic currently processes one FoF workspace at
a time, so this model cannot rank all halos/subhalos across the required global
catalogue path before assigning stellar masses.

The current implementation is therefore a calibrated, local proxy: it tracks
`Mpeak` and `Vpeak` along each processed branch and applies an analytic stellar
mass relation with deterministic scatter. It is useful for exercising the model
package architecture, output schema, and plotting path, but it is only a
pseudo-SHAM until Mimic has a volume/snapshot-wide ranking stage.

Run with:

```bash
make MODEL=sham
./mimic input/runs/sham_millennium.yaml
```

The model currently provides:

- `sham_assign_stellar_mass`: peak-proxy tracking, stellar-mass assignment,
  deterministic scatter, and optional orphan ageing.
- A standalone `model_properties.yaml` containing every galaxy property required
  by this model set.
- Self-contained plotting diagnostics in `plots/figures/`.
