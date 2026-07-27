# Animation export runtime fixture

This fixture represents a cooked-only startup scene. `cooked-assets.json` is an executable cook recipe for all seven animation classes: tests build the real v1 envelopes and compare their validated content-hash fingerprints. `parity-manifest.json` records the archive-only startup scene and exact state, event, root-motion, motion-matching, ragdoll, deformation, and bounds evidence expected from editor/export parity runs.
