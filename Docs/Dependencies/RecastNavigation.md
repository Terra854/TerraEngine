# Recast Navigation dependency

This page records the approved Recast Navigation dependency boundary for
TerraEngine's native ground-navigation runtime. The dependency is engine-owned
and provider-private; gameplay, scene, scripting, editor, and export contracts
must not expose Recast or Detour types.

## Approved source and rationale

- **Project:** Recast Navigation
- **Version:** `1.6.0`
- **Repository:** `https://github.com/recastnavigation/recastnavigation.git`
- **Exact source commit:** `6dc1667f580357e8a2154c28b7867bea7e8ad3a7`
- **Approved repository location:** pristine submodule at `External/recastnavigation`
- **License/notice source:** `External/recastnavigation/License.txt`

Recast supplies ground-mesh generation from captured scene geometry and Detour
supplies ground navigation queries over the generated mesh. Both are runtime
inputs because offline baking is optional and runtime generation remains
mandatory. The engine owns source capture, immutable publication, streaming,
route validation, and lifecycle behavior around the provider.

## Core-only static build boundary

TerraEngine statically compiles only the provider's core source groups through
the engine-owned `cmake/TerraEngineRecast.cmake` wrapper:

- `Recast/Source`
- `Detour/Source`

The integration must not compile or link RecastDemo, DetourCrowd,
DetourTileCache, DebugUtils, examples, upstream tests, SDL, Catch2, or any
provider-owned worker pool. No new managed navigation package is part of this
approval. Provider headers remain private implementation inputs and are not
installed as TerraEngine public headers.

The adapter and provider must use the engine's C++20 baseline and matching
64-bit Detour polygon references. These ABI-sensitive choices must remain
consistent between the static libraries and `Engine/Navigation/Backend`.
TerraEngine owns the CMake target, include visibility, warning/ABI settings,
and install rules; no upstream source file may be patched.

## Redistribution and notice obligations

`License.txt` contains the upstream Zlib-style notice and must remain
unchanged in the pristine submodule. Any source or binary redistribution that
contains this dependency must preserve that notice in the installed or
packaged third-party notices. The implementation must install the notice while
keeping provider headers, demos, tests, and unrelated provider tools out of
the public runtime tree.

## Upgrade and source-integrity policy

The approved pin and scope are exact. Changing the repository, tag, commit,
source contents, patches, static-library boundary, excluded modules, or public
surface requires returning to planning for renewed explicit approval. Do not
silently substitute a system package, downloaded library, or another Recast or
Detour revision.

## Reproducible verification

The following read-only checks verify the source identity and notice location:

```powershell
$src = "External/recastnavigation"
git -C $src rev-parse HEAD
Get-Content "$src/License.txt"
```

The first command must print
`6dc1667f580357e8a2154c28b7867bea7e8ad3a7`. Compile, focused tests, install,
and installed-tree notice verification are implementation gates; this document
does not claim those qualification results until the build gate records them.
