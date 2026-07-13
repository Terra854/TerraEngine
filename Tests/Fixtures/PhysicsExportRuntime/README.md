# Physics Export Runtime Fixture

This source-controlled fixture defines the editor Play Mode versus exported-runtime
physics contract. It intentionally contains a `.tecollider` recipe and no cooked
`.tepcoll` payload: the installed baseline must cook it into its disposable work
directory before export. A shipped package must contain only the cooked envelope,
Physics Settings, material data, and NativeAOT binding; it must not retain this
source recipe.

Expected invariant names are `bodies`, `stepping`, `filtering`, `queries`,
`events`, `constraints`, `characters`, `transitions`, and `shutdown`. The
headless runner compares those names after cook, export, launch, and orderly
shutdown. The corruption seeds are deliberately non-runnable inputs for
validator tests: missing settings, a placeholder managed binding, and a corrupt
cooked-collider envelope.

The fixture requires the repository-owned Jolt Physics v5.6.0 provider at
commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`; it does not require a system
physics SDK or any globally installed runtime dependency.
