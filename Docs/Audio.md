# Audio runtime

TerraEngine's audio foundation imports WAV and Ogg Vorbis clips, stores them in
a checked provider-neutral asset, and schedules immutable frame packets through
the selected backend. The only production backend currently supplied is
`terra.miniaudio`. The boundary is intentionally public so another developer can
add FMOD, Wwise, or a different provider without changing components, scripting,
mixing, spatialization, or cooked project data.

## Creator workflow

1. Copy a `.wav` or `.ogg` file into the project's `Assets` directory.
2. Wait for `AudioClipImporter` to publish the cooked clip. A failed reimport
   retains the last complete generation instead of replacing it with bad data.
3. Add an `Audio Source` component and assign the clip using the audio-filtered
   Inspector asset field.
4. Enable **Play On Start** for immediate playback, or call the native, managed,
   or automation control API at runtime.
5. For spatial sound, enable spatial playback and add an eligible `Audio
   Listener`. Non-spatial music remains playable when no listener exists.
6. Use **Audio Mixer** for master/group gain and mute state. Project values are
   persisted in `ProjectSettings/AudioSettings.json`.
7. Use **Audio Diagnostics** when a clip is virtualized, rejected, or the output
   device is unavailable. Game View's **Mute Audio** controls master mute; it
   does not pause or alter logical playback time.

The repository includes pinned Kenney music and sound-effect fixtures under
`Tests/Fixtures/Audio/ThirdParty/Kenney`. They are validation media, licensed by
their adjacent notices, and are not included in an exported game unless the
project explicitly imports them as its own assets.

## Cooked and exported format

Import produces a `.teaudio` file. It is a versioned little-endian `TEAF`
envelope containing the asset UUID, encoding, channels, sample rate, frame
count, bounded offsets, checksums, and the retained encoded WAV or Ogg payload.
The initial foundation preserves the source encoding rather than transcoding it:

- WAV input becomes a checked `TEAF` envelope containing WAV bytes.
- Ogg Vorbis input becomes a checked `TEAF` envelope containing Ogg bytes.

Export discovers the cooked `.teaudio` dependency, validates its identity and
checksum, and writes it into the project's TEPAK archive. Exported runtimes load
that same provider-neutral asset through normal content mounting; there are no
provider banks and no dependency on the original source file. Corrupt staged
audio fails export instead of becoming a runtime-only error.

## Playback, spatialization, and mixing

Logical playback time belongs to the engine, not the output device. Pause holds
the cursor, resume continues it, stop resets it, mute leaves it advancing, and a
virtualized voice can seek to its current cursor when it becomes audible again.
Commands are admitted as bounded typed batches and become visible at the next
declared audio snapshot/commit boundary. If any command in a batch is invalid,
the entire batch is rejected without partial state changes.

The engine selects an eligible listener deterministically and calculates the
tested attenuation, orientation, stereo pan, spatial blend, priority, and voice
virtualization before calling the backend. Mix groups are flat: each source
targets one group, and effective gain combines source, group, and master state.
The provider receives final provider-neutral clip and voice commands; it does
not apply a second distance curve or own TerraEngine's authoring semantics.

## Adding another provider

An external provider implements
`TerraEngine::Audio::Backend::IAudioBackend` using only the installed headers:

- `TerraEngine/Audio/AudioTypes.h`
- `TerraEngine/Audio/Backend/IAudioBackend.h`
- `TerraEngine/Audio/Backend/AudioBackendRegistry.h`

Register a descriptor and factory before audio initialization, use a unique
provider ID and supported contract version, preflight a complete frame packet
before mutating provider state, keep observation channels bounded, and make
shutdown idempotent. The registry freezes during initialization so late or
duplicate registration is rejected deterministically. Provider SDK types must
remain private to that module; do not expose them through engine components,
serialized scenes, scripting, or the public headers above.

Validate the public header boundary against the Windows build-tree import
library (the Windows install intentionally ships runtime DLLs, not a link-time
`.lib`):

```powershell
& out/build/RelWithDebInfo/Build/Tools/Audio/VerifyInstalledAudioProviderContract.ps1 `
  -IncludeRoot out/build/RelWithDebInfo/Build/include `
  -LibraryPath out/build/RelWithDebInfo/TerraEngine.lib `
  -RuntimeDirectory out/build/RelWithDebInfo/Build/Engine
```

This compiles, links, and runs a separate consumer, and fails if miniaudio's
private headers leak into the SDK. From a clean installed tree the same script
can still perform the header-leakage check, but it will report that a Windows
import library is required for the compile/link portion. FMOD and Wwise are not
bundled because this foundation prioritizes a small self-contained open
dependency and a verifiable provider-neutral contract. A future provider can
still integrate either one.
Advanced acoustics, including ideas derived from research such as Microsoft's
Project Triton, are deliberately postponed until the basic source, listener,
mixing, and provider lifecycle is stable.

## Reproducible validation

Verify the committed downloads and deterministic malformed inputs:

```powershell
Tools/Audio/AcquireAudioFixtures.ps1 -VerifyCommitted
Tools/Audio/GenerateAudioFixtures.ps1 -VerifyCommitted
```

Build, install, and run the device-independent suite:

```powershell
cmake --preset RelWithDebInfo
cmake --build out/build/RelWithDebInfo --target TerraEngine-Engine TerraEngine-Editor TerraEngine-Export TerraEngineAudioClipCompiler TerraEngineAudioStressRunner TerraEngine-EngineTests TerraEngine-EditorTests
cmake --build out/build/RelWithDebInfo --target install
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "^(Engine|Editor)\\.Audio" --output-on-failure
```

The installed stress runner hashes every prepared voice. The committed baseline
requires at least 128 sources and 100 identical replays:

```powershell
$audioTools = 'out/build/RelWithDebInfo/Build/Tools/Audio'
& "$audioTools/RunAudioStress.ps1" -RequestedSources 128 -RepeatCount 100 -Output "$audioTools/Evidence/stress.json"
& "$audioTools/CompareAudioEvidence.ps1" -Evidence "$audioTools/Evidence/stress.json"
& "$audioTools/RunAudioSoak.ps1" -DurationMinutes 30 -RequestedSources 128 -RepeatCount 100 -Output "$audioTools/Evidence/soak.json"
```

The stress and soak wrappers create `Build/Tools/Audio/Evidence` when needed.
Stress evidence is compared with
`Tests/Fixtures/Audio/Performance/baseline.json`; both workloads are
device-independent and automatable on Windows. Audible editor scenarios remain
manual evidence and must not be reported as passed without an available device.

These checks validate import, state transitions, packet generation, export,
public SDK isolation, determinism, and long-running preparation without needing
speakers. They do not prove audible output. When a playback device is available,
run every scenario in `specs/051-audio-runtime-foundation/quickstart.md` from the
installed editor and record the audible lane separately.

The audio runtime foundation Windows acceptance run completed 75/75 CTests, the installed
provider consumer, committed-fixture verification, the 128-source/100-replay
stress gate, and a real 30-minute soak. The soak completed 130,382 iterations
with the expected hash, a 64/64 voice cap, 0% retained-resource growth, 0.0523 ms
maximum median, and 0.0861 ms maximum p95. Audible WAV, Ogg, stereo pan, and
attenuation checks were also heard on the Windows default output device. On
Ubuntu 24.04, strict offline managed restore, native/managed builds, all 12 audio
CTest groups, the install target, and the same stress hash passed using the
repository-locked `linux-x64` runtime and NativeAOT packs. WSL exposed no Linux
playback device, so audible Linux output remains the only unexecuted Linux audio
lane; see `Docs/LinuxSupport.md`.

## Troubleshooting

- **No sound with valid logical voices:** check the diagnostics provider/device
  status and OS output device. Initialization failure intentionally leaves the
  engine safely silent rather than failing the frame.
- **Clip will not import:** confirm the file signature matches WAV or Ogg Vorbis;
  extension-only renames are rejected. Review the compiler diagnostic and keep
  the last-known-good cooked generation.
- **Spatial clip is virtualized:** add/enable a listener, inspect source range and
  priority, and check the configured/provider voice limit.
- **Controls appear delayed:** receipts publish after the declared AudioCommit
  boundary; inspect the correlation ID and rejection diagnostic.
- **External provider will not register:** register before engine initialization,
  use the current contract version, and avoid a duplicate provider ID.
- **Muted/headless validation:** run the dedicated `Engine.AudioOfflineRender`
	CTest registration documented in `Docs/Testing.md`. It uses the real miniaudio
	decoder and mixer to produce PCM in memory with `noDevice=true`, verifies a
	known waveform and repeatability, and never opens an operating-system playback
	device. Also run the fake-provider tests and installed stress tools. Report the
	audible quickstart as deferred rather than claiming it passed.
