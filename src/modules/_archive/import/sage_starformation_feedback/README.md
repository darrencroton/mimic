# SAGE Star Formation & Feedback Module

The `sage_starformation_feedback` module implements disk star formation following the Kennicutt-Schmidt law with a critical surface density threshold (Kauffmann 1996), plus supernova-driven feedback. Cold gas above the critical mass forms stars with efficiency ε_SF over the dynamical timescale. Feedback reheats cold gas to the hot phase and ejects hot gas from the halo using an energy-driven wind model. Metals are produced via instantaneous recycling with mass-dependent distribution (Krumholz & Dekel 2011). Disk scale radius is calculated from halo spin following Mo, Mao & White (1998).

## Parameters

This module requires the following parameters in the input YAML file:

- **SFprescription** (int): Star formation law (0=Kennicutt-Schmidt with density threshold)
- **SfrEfficiency** (double): Star formation efficiency (epsilon_SF in SFR = epsilon_SF * M_cold / t_dyn)
- **EnergySNcode** (double, code_units): Supernova energy per event in code units
- **EtaSNcode** (double, code_units): Supernova rate per unit stellar mass formed
- **SupernovaRecipeOn** (int): Enable supernova feedback (0=off, 1=on)
- **FeedbackReheatingEpsilon** (double): Reheating efficiency - M_reheat = epsilon * M_stars
- **FeedbackEjectionEfficiency** (double): Ejection efficiency in energy-driven wind formula
- **RecycleFraction** (double): Fraction of stellar mass immediately recycled to ISM
- **Yield** (double): Metal yield per unit stellar mass formed
- **FracZleaveDisk** (double): Fraction of newly produced metals that leave the disk in winds
- **DiskInstabilityOn** (int): Enable disk instability physics (0=off, 1=on)

**Configuration Example**:
```
EnabledModules  sage_calculate_infall,sage_cooling,sage_starformation_feedback
SageStarformationFeedback_SfrEfficiency  0.02
SageStarformationFeedback_RecycleFraction  0.43
SageStarformationFeedback_FeedbackReheatingEpsilon  3.0
SageStarformationFeedback_FeedbackEjectionEfficiency  0.3
SageStarformationFeedback_Yield  0.03
```

**Additional Comments**:

**References**: Kennicutt (1998), Kauffmann et al. (1996), Mo, Mao & White (1998), Krumholz & Dekel (2011), Croton et al. (2006, 2016)
