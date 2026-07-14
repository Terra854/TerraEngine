# US7 Validation

Validated on 2026-07-14 using only the `RelWithDebInfo` preset.

## Focused compile and tests

```powershell
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Launcher TerraEngine-Editor TerraEngine-Export --parallel 4
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "^(Engine\.PhysicsUS7|Editor\.PhysicsUS7)$" --output-on-failure
```

The affected targets compiled and both focused CTest registrations passed. The engine suite was rerun after the external NativeAOT runtime-layout repair and passed again.

## Install and live parity

```powershell
cmake --build --preset RelWithDebInfo --target install --parallel 4
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/ExportRuntimeBaseline/RunBaseline.ps1 `
	-BuildDir out/build/RelWithDebInfo `
	-WorkRoot out/validation/t153-final-export-parity-5 `
	-RunId t153-final-export-parity-5
```

Both commands passed. The live run used the installed collider compiler, installed Vulkan editor, shared Play Mode and `PhysicsSystem`, NativeAOT export, published launcher, and mounted TEPAK.

The final independently produced editor and export records matched exactly: 4 bodies, 90 fixed steps at 20,000 microseconds, gravity Y of -15,500 millimetres per second squared, broad-phase group 7, 1 filtered query hit, contact-event presence, 1 constraint, 1 character, 2 world transitions, ready managed/cooked content, orderly shutdown, and zero queued or active physics jobs after drain. Export validation reported zero errors and verified the pinned Jolt 5.6.0 revision and cooked-collider GUID.

The six durable export evidence files and both parity records are archived under `evidence/export-parity/`. Complete disposable logs remain under `out/validation/t153-final-export-parity-5/logs`.
