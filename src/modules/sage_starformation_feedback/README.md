# SAGE Star Formation & Feedback Module

The `sage_starformation_feedback` module implements disk star formation following the Kennicutt-Schmidt law with a critical surface density threshold (Kauffmann 1996), plus supernova-driven feedback. Cold gas above the critical mass forms stars with efficiency ε_SF over the dynamical timescale. Feedback reheats cold gas to the hot phase and ejects hot gas from the halo using an energy-driven wind model. Metals are produced via instantaneous recycling with mass-dependent distribution (Krumholz & Dekel 2011). Disk scale radius is calculated from halo spin following Mo, Mao & White (1998).

**Configuration Example**:
```
EnabledModules  sage_infall,sage_cooling,sage_starformation_feedback
SageStarformationFeedback_SfrEfficiency  0.02
SageStarformationFeedback_RecycleFraction  0.43
SageStarformationFeedback_FeedbackReheatingEpsilon  3.0
SageStarformationFeedback_FeedbackEjectionEfficiency  0.3
SageStarformationFeedback_Yield  0.03
```

**Additional Comments**:

**References**: Kennicutt (1998), Kauffmann et al. (1996), Mo, Mao & White (1998), Krumholz & Dekel (2011), Croton et al. (2006, 2016)
