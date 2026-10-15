# Audio control acceptance

Prerequisites: an x64 Visual Studio developer shell, the repository's pinned
dependencies and bundled .NET SDK, and a Windows desktop with a working renderer
and audio provider for the installed gameplay lane. Missing required capability
fails acceptance. Native source-setting and queue tests remain device independent.

From a clean checkout, configure, compile and install before running tests:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Compile/install failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --no-tests=error --output-on-failure
$install = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$work = Join-Path $PWD ('out/validation/audio-controls-' + [Guid]::NewGuid().ToString('N'))
& "$install/Tests/Tools/Audio/RunInstalledAudioControlAcceptance.ps1" -InstallRoot $install -WorkRoot $work -Lane All -Repetitions 100
```

Run the installed copy of the runner. Its manifest verifies the native and
managed binaries, bundled tools and authored fixture inputs. WorkRoot must be
fresh; all attempts retain their evidence. `-Lane Managed` runs the independent
real C# gameplay lane while developing the bridge; final acceptance requires
`-Lane All`. No runner creates an export or grants editor approval on your behalf.

The native selections are `Audio.ControlContinuity` (minimum 12 cases),
`Audio.PlaybackBinding` (10), `Audio.SourceSettings` (10), and
`Editor.AudioControlPersistence` (4). Continuity observes actual 1, 2 and 8 task
producers completing 20,000 keyed requests each. Settings evidence records 120
later live captures. The renderer child compiles production publication code
directly and checks five allocation-safety families at 100 repetitions, including
real scratch allocation failures and zero-allocation reporting paths. Termination
and watchdog exits fail the lane.

Gameplay acceptance observes actual managed callbacks and positive-delta playback
frames: autoplay, a fresh facade's pause/resume/stop, pending Play/Pause and stale
attachment rejection. A native imitation of a C# request cannot satisfy this lane.
Queue retry retention, engine-owned playback IDs and durable source-setting
precedence are described in `Docs/Audio.md`.
