# Navigation workload terrain seed

`Navigation059.teterrain` and its private `.data` bundle were copied from the
successful `NavigationAgent059Test` authored project. The seed is a genuine
129x129, one-metre flat TERRAROOT1 asset, not generated navigation geometry.

- Asset GUID: `d5e5eea9b22f2cbb4661dfe7e04607e5`
- CURRENT root: `29c949fa9456640c74577efe9ea6280b`
- Source: `out/build/RelWithDebInfo/Build/Editor/NavigationAgent059Test/Assets`

The workload generator verifies the descriptor, metadata, CURRENT pointer, and
root manifest before copying the seed into a generated project.

The validation executable's optional cache-fixture mode builds a separate small
7x7 provider tile through `BuildRecastGroundTiles` and writes its pristine
Detour bytes into a checked `.tenav` container. That fixture is a standalone
codec/provider round-trip diagnostic; it is not a cache for this 129x129
workload and does not prove runtime tile consumption.
