# Physics Export Runtime Fixture

This source-controlled fixture defines the editor Play Mode versus exported-runtime
physics contract. It intentionally contains a `.tecollider` recipe and no cooked `.tepcoll`
payload: the installed baseline must cook it into its disposable work
directory before export. A shipped package must contain only the cooked envelope,
Physics Settings, material data, and NativeAOT binding; it must not retain this
source recipe.

Expected invariant names are `bodies`, `stepping`, `filtering`, `queries`,
`events`, `constraints`, `characters`, `transitions`, and `shutdown`. The
headless runner compares those names after cook, export, launch, and orderly
shutdown. The corruption seeds are deliberately non-runnable inputs for
validator tests: missing settings, a placeholder managed binding, and a corrupt
cooked-collider envelope.

`project.json` owns invariant vocabulary version 1. `Scenes/Startup.tescene`
repeats that vocabulary with the source-collider path, headless capability, and
the required zero-active-job shutdown result so a runner can reject stale or
partially staged fixture inputs before launching either mode.

The fixture requires the repository-owned Jolt Physics v5.6.1 provider at
commit `2e28006e4a367624bb512bf2c37ca99a93d9d1ea`; it does not require a system
physics SDK or any globally installed runtime dependency.
