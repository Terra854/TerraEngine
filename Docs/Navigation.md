# TerraEngine Navigation

TerraEngine navigation is an engine-owned, ground-only runtime service. Public
gameplay and managed APIs use provider-neutral records and commands; Recast and
Detour remain private to the native navigation backend.

## Approved dependency and scope

The approved provider is Recast Navigation `1.6.0` at exact source commit
`6dc1667f580357e8a2154c28b7867bea7e8ad3a7`, checked out as the pristine
`External/recastnavigation` submodule. The engine-owned
`cmake/TerraEngineRecast.cmake` integration statically compiles only
`Recast/Source` and `Detour/Source`.

The approved boundary excludes RecastDemo, DetourCrowd, DetourTileCache,
DebugUtils, examples, upstream tests, SDL, Catch2, provider-owned worker pools,
and a new managed navigation package. Provider headers are private build
inputs and are not part of the TerraEngine public API or installed SDK.
Runtime generation is mandatory; optional caches only accelerate generation and
must never become required inputs.

The engine adapter and static provider libraries must share the C++20 baseline
and 64-bit Detour polygon-reference configuration. TerraEngine owns the CMake
target and integration boundary, and no upstream Recast source files may be
modified.

## Notice and redistribution

The upstream notice is preserved at
`External/recastnavigation/License.txt`. It must accompany source or binary
redistributions that contain Recast or Detour, including the installed
third-party notice bundle. The install surface must contain the notice without
publishing provider headers, demos, tests, or unrelated tools. See
[Recast Navigation dependency](Dependencies/RecastNavigation.md) for the full
pin, scope, and verification record.

## Verification status

Initial dependency verification completed on 2026-09-06 with the Windows x64
Visual Studio developer environment, MSVC 19.51.36252.0, Ninja, and the
`RelWithDebInfo` preset. The host's normal shell did not expose the compiler,
Ninja, or Windows SDK tools, so this work must run from a configured developer
prompt rather than treating those ambient tools as engine dependencies.

```powershell
git -C External/recastnavigation rev-parse HEAD
git -C External/recastnavigation status --porcelain
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-Engine
cmake --build --preset RelWithDebInfo --target install
```

The revision command returned
`6dc1667f580357e8a2154c28b7867bea7e8ad3a7`, and the porcelain status was
empty before and after the build. Configuration, the native
`TerraEngine-Engine` build, and the `install` target all returned success. The
first native build compiled and linked `TerraEngineRecastCore.lib` and
`TerraEngineDetourCore.lib` before linking `TerraEngine.dll`.

The installed product tree is
`out/build/RelWithDebInfo/Build`. Its audited navigation artifacts are:

- `ThirdPartyNotices/RecastNavigation-Zlib.txt`, byte-identical to the pristine
  upstream `External/recastnavigation/License.txt` and containing its copyright,
  warranty, permission, and notice-preservation clauses.
- `Docs/Navigation.md` and `Docs/Dependencies/RecastNavigation.md`.

The installed-tree scan found no Recast or Detour provider headers, standalone
libraries, source files, or provider directories. Focused navigation tests are
not part of this dependency-only checkpoint; they are introduced with the runtime
foundation test tasks and remain required before that phase is complete.

## Approval boundary

Any dependency revision, source patch, module-scope expansion, provider-owned
runtime service, or public provider-header exposure requires renewed explicit
approval and a return to planning. The approved dependency supplies ground
generation and queries only; it does not provide flight navigation, gameplay
decisions, locomotion, or engine publication guarantees.

## Clean-checkout installed acceptance

The acceptance matrix is source-free: after configuring, building, and
installing from a clean checkout, invoke the runner from the installed tree and
use an evidence directory outside the install tree. Do not point it at binaries
or fixtures in the build tree.

```powershell
git submodule update --init --recursive
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor `
    TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export
cmake --build --preset RelWithDebInfo --target install

$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$navigationTool = Join-Path $installRoot 'Tests/Tools/Navigation/RunInstalledNavigationAcceptance.ps1'
$evidenceRoot = [IO.Path]::GetFullPath('out/evidence/navigation')
```

Runtime generation is mandatory. Navigation caches are optional accelerators:
an absent or invalid cache must cause deterministic regeneration, not a passing
or failing result based on cache presence alone. The installed manifest and
runtime binaries remain mandatory inputs.

Run the semantic matrix as one three-lane comparison so the installed editor,
rendered export, and headless export all share one checked provenance set. The
command uses only installed files and retains raw lane evidence beneath the
supplied output directory:

```powershell
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $evidenceRoot 'semantic') `
    -Lanes @('editor', 'export-rendered', 'export-headless') -Matrices semantic
```

For the installed-only audit, validate all lane artifacts in one dry run from
the repository root (or from a separate validation working directory):

```powershell
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $evidenceRoot 'installed-audit') `
    -Lanes @('editor', 'export-rendered', 'export-headless') -Matrices @('semantic') -DryRun
```

The full installed matrix uses all three lanes. Performance, idle-overload,
and usability are bounded acceptance matrices; the soak command below is the
required 30-minute run and the lifecycle command runs 100 cycles:

```powershell
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $evidenceRoot 'performance') `
    -Lanes export-rendered -Matrices performance -DurationMinutes 5 -Qualification
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $evidenceRoot 'idle-overload') `
    -Lanes export-rendered -Matrices idle-overload -DurationMinutes 5 -IdlePairs 5 -Qualification
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $evidenceRoot 'soak-30m') `
    -Lanes @('export-rendered', 'export-headless') -Matrices soak -DurationMinutes 30 -Qualification
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $evidenceRoot 'lifecycle-100') `
    -Lanes editor -Matrices lifecycle -Cycles 100 -Qualification
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $evidenceRoot 'usability') `
    -Lanes editor -Matrices usability -UsabilityInvalidCases 5 -Qualification
```

`-DurationSecondsOverride` is a smoke-test aid and cannot replace the
30-minute soak gate. Every run writes `navigation-acceptance-result.json` and
per-lane `stdout.txt`, `stderr.txt`, and `result.json` files under its contained
work directory. A missing installed artifact, failed lane, missing raw result,
or unavailable measurement is a nonzero/failed acceptance outcome; do not
reinterpret it as zero work. The installed audit must retain the complete
`out/evidence/navigation` tree and record the exact install-tree identity used.

## Ordinary object gameplay workflow

Navigation is useful to a game object through an engine-owned agent binding. Add
an enabled `NavigationAgentComponent` beside the object's canonical `Transform`
using the normal scene transaction path. At the protected navigation boundary,
the engine resolves the object's stable identity, world, profile, and current
pose into a generation-qualified runtime agent. Native gameplay or the supplied
C# facade then sets a destination, polls the command receipt, reads the request,
and copies the immutable route points.

The caller owns movement. Before each step, gameplay/physics validates the next
route segment against the current pose and route generation, then submits the
pose or movement intent through the existing typed transform/physics path. A
stale or invalidated route must stop movement and trigger a bounded replan from
the newly published pose. Navigation never writes `Transform`, velocity,
physics, or animation and does not teleport an object to a route endpoint.

Minimal native flow (status/error handling abbreviated):

```cpp
auto agent = navigation.ResolveObjectAgent(objectId); // scene-owned generation handle
auto command = navigation.QueueSetDestination(agent, destination, routePolicy, expectedIntentGeneration);
auto applied = navigation.ReadCommand(command.command);
auto request = navigation.ReadRequest(applied.record->resultingRequest);
auto state = navigation.ReadAgent(agent);
auto route = navigation.ReadRoute(state.record->activeRoute);
gameplayController.MoveAlong(route.points); // caller validates and owns movement
```

Equivalent C# gameplay starts from the component already attached to the object:

```csharp
NavigationAgent? agent = navigationAgentComponent.RuntimeAgent;
if (agent == null)
    return; // the component has not crossed its publication boundary yet

NavigationCommandResult command = agent.SetDestination(targetWorldPosition);

// In later updates, keep polling the same resolved agent until the command is
// applied and its asynchronous request publishes a route.
command = agent.RefreshDestinationCommand();
NavigationRouteResult route = agent.ReadRoute();
if (route.Complete)
    gameplayController.MoveAlong(route.Points);
```

`SetDestination` is asynchronous. Gameplay refreshes the command and request,
then reads the route points. Keep the resolved `NavigationAgent` instance while
that request is in flight; receiving a route does not itself move the object.

Managed gameplay follows the same sequence with the engine-supplied
`NavigationAgent` facade. No Recast/Detour reference or developer-provided
navigation library is required. Terrain support is mandatory in the engine,
but terrain presence is scene-dependent: the acceptance workflow must prove
both an object routing over eligible terrain and an object routing in a
terrain-free mesh/collider scene.
