# Renderer Deformation

This module owns renderer-facing geometry deformation after animation has
published a pose, palette, and morph state. It deliberately does not evaluate
animation graphs or mutate scene state.

The renderer builds immutable work plans from the union of page demands across
all views. Backends consume those plans before the first geometry pass and
publish matching current geometry, history, bounds, and generation metadata.
