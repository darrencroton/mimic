# Mimic: A Physics-Agnostic Galaxy Evolution Framework

```
    __  ___  ____  __  ___  ____  ______
   /  |/  / /  _/ /  |/  / /  _/ / ____/
  / /|_/ /  / /  / /|_/ /  / /  / /
 / /  / / _/ /  / /  / / _/ /  / /___
/_/  /_/ /___/ /_/  /_/ /___/  \____/
```

## The Problem

Cosmological N-body simulations tell us where dark matter halos form and how they merge, but they say nothing about the galaxies inside them. Semi-analytic models (SAMs) bridge that gap: they apply physically motivated prescriptions for gas cooling, star formation, feedback, and mergers to halo merger trees, producing mock galaxy catalogues in minutes rather than the months a full hydrodynamic simulation would take.

The catch is that traditional SAMs are monolithic. The physics is hard-wired into the source code, so testing a different feedback prescription, dropping a process to measure its effect, or comparing two model variants means editing C code, recompiling, and hoping nothing else silently changed. The science question — *what does this physics choice do to the galaxy population?* — gets buried under software archaeology.

**Mimic** separates the two concerns. A physics-agnostic core owns tree processing, memory, I/O, validation, and output provenance. The astrophysics lives in self-contained **model packages** — collections of runtime modules that you select, order, and parameterise in a YAML file. The input side is modular in the same way: **simulation packages** wrap each merger-tree catalogue with its cosmology and units. Changing the physics pipeline means editing configuration, not code; entirely different galaxy formation models run on the same infrastructure, and the same model runs unchanged across different simulations.

## What You Can Do With It

- **Generate mock galaxy catalogues** from halo merger trees: stellar masses, gas reservoirs, star formation rates, black holes, metals, and positions, written as self-documenting HDF5 or compact binary.
- **Experiment with the physics.** Disable a feedback channel, swap an AGN mode, reorder a pipeline stage, or run pure halo tracking with no galaxy physics at all — all from the run file, with the active pipeline recorded in the output for reproducibility.
- **Swap the simulation under the model.** Simulation packages are as interchangeable as models: run identical physics on different merger-tree catalogues — a small box for development, a larger one for production, different resolutions or cosmologies to test the robustness of your conclusions.
- **Build your own model.** Properties and modules are declared in YAML metadata and generated into type-safe C, so a new physics module is a small, testable unit rather than a patch across a monolith. Every model package is self-contained: its physics, properties, parameters, tests, and plots live together under `models/<model>/`.
- **Start from shipped, validated physics.** Mimic comes with ready-to-run model packages — the current default, `sage16`, is a modular port of the published SAGE model, reproduced to near-bit-parity against the original code ([parity report](docs/SAGE16-PARITY-REPORT.md)) — proof that the modular architecture can carry a full production model without altering its science.
- **Inspect results immediately** with a plotting suite covering the standard diagnostics: mass functions, scaling relations, gas fractions, star formation histories, and more.

## Who It's For

- **Researchers** who need mock catalogues for survey design, clustering studies, or comparison with observations.
- **Galaxy formation modellers** who want to isolate and test individual physical prescriptions, or develop new models on proven infrastructure.
- **Developers** who want a disciplined, metadata-driven SAM framework with a three-tier test suite (unit, integration, scientific) underneath.

## Quick Start

From a fresh clone to your first galaxy catalogue and plots:

```bash
git clone https://github.com/darrencroton/mimic.git
cd mimic
./scripts/first_run.sh    # creates directories, downloads mini-Millennium tree data, sets up Python env

make                       # builds the default model + simulation packages (currently sage16 + mini-Millennium)
./mimic models/sage16/input/sage16_mini-millennium.yaml
```

This evolves galaxies through the mini-Millennium simulation ([Springel et al. 2005](http://arxiv.org/abs/astro-ph/0504097)) with the default model's full physics pipeline and writes the catalogue to `output/sage16-mini-millennium/`. Then visualise it:

```bash
source mimic_venv/bin/activate
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml
deactivate
```

You'll find mass functions, scaling relations, star formation histories, and more under `output/sage16-mini-millennium/plots/`. Mimic also writes a ready-to-run analysis script (`example_Mvir_Len_plot.py`) into the output directory so you can start exploring the catalogue in Python straight away.

Any other model + simulation pairing runs the same way — build with `make MODEL=<name> SIMULATION=<name>` and point `./mimic` at the matching run file.

**Prerequisites**: a C compiler (gcc or clang), GNU Make, and Python 3.9+. HDF5 libraries are recommended (build with `make USE-HDF5=no` without them); MPI is optional for parallel runs.

## Where to Go Next

The documentation follows your journey:

| You're asking | Read |
| --- | --- |
| *How do I use Mimic for my science?* | **[User Guide](docs/USER-GUIDE.md)** — workflows for running simulations, configuring physics, reading outputs, and plotting, plus troubleshooting |
| *How do I extend or modify Mimic?* | **[Developer Guide](docs/DEVELOPER-GUIDE.md)** — architecture, writing physics modules, the property system, adding simulations, and testing |
| *Why is it designed this way?* | **[Vision](docs/VISION.md)** — the architectural principles and design boundaries |

Useful entry points along the way:

- **`models/<model>/README.md`** — every model package documents its own scientific scope, pipeline, and references; see [models/](models/) for what's shipped
- **[plot/mimic-plot/README.md](plot/mimic-plot/README.md)** — the plotting manual: available plots, options, and adding new figures
- **[tests/README.md](tests/README.md)** — running the test suite

## Citation

Cite the references for the model package you use in your research — each package's README lists them. For the default `sage16` package, that is:

- Croton et al. 2016, [Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results](http://arxiv.org/abs/1601.04709), ApJS, 222, 22

## Contributing

Contributions are welcome — new physics modules, new model packages, new simulation support, or framework improvements. Start with the [Developer Guide](docs/DEVELOPER-GUIDE.md) for coding standards and the development workflow, and the [Vision](docs/VISION.md) document for the architectural principles new code should follow. All changes should come with tests — see [tests/README.md](tests/README.md).

## License and Contact

Mimic is open source; see [LICENSE.txt](LICENSE.txt) for details.

**Darren Croton** · dcroton@swin.edu.au · [darrencroton.github.io](https://darrencroton.github.io)
