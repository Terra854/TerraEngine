# RelWithDebInfo Install Validation (T152)

Validation date: 2026-07-14  
Canonical product tree: `out/build/RelWithDebInfo/Build`

## Command and result

```powershell
cmake --build --preset RelWithDebInfo --target install -j 4
```

Result: **PASS**, exit code 0. The install rebuilt the self-contained managed
TerraEngine and TerraEngine.Editor publications with zero warnings and zero
errors, compiled the external shader files, and refreshed the installed native
executables, libraries, tools, headers, documentation, templates, managed
runtime, and required dependencies.

## Required product inventory

| Installed path | Bytes | Status |
|---|---:|---|
| `Engine/TerraEngine.dll` | 19,514,368 | Present |
| `Launcher/TerraEngine-Launcher.exe` | 465,920 | Present |
| `Editor/TerraEngine-Editor.exe` | 20,066,304 | Present |
| `Editor/TerraEngine-Export.exe` | 3,611,136 | Present |
| `Editor/TerraEnginePhysicsColliderCompiler.exe` | 798,208 | Present |
| `Engine/TerraEnginePhysicsGameplayStress.exe` | 116,736 | Present |
| `Engine/TerraEnginePhysicsConstraintCharacterStress.exe` | 90,624 | Present |
| `Docs/PhysicsColliderAssets.md` | 4,399 | Present |
| `Docs/PhysicsGameplay.md` | 5,085 | Present |
| `Docs/PhysicsConstraintsAndCharacters.md` | 9,227 | Present |
| `Docs/Testing.md` | 49,554 | Present |
| `ThirdPartyNotices/JoltPhysics-LICENSE.txt` | 1,059 | Present |
| `include/TerraEngine/Physics/PhysicsSystem.h` | 4,048 | Present |
| `include/TerraEngine/Physics/Backend/IPhysicsBackend.h` | 14,307 | Present |

The installed tree contains 494 files totaling 392,876,124 bytes.

## Provider-boundary audit

The installed tree contains only the Jolt dependency description and license
notices named for Jolt:

- `Docs/Dependencies/JoltPhysics.md`
- `Docs/Licenses/JoltPhysics-LICENSE.txt`
- `ThirdPartyNotices/JoltPhysics-LICENSE.txt`

No `External/JoltPhysics` tree, Jolt public headers, provider source, samples,
tests, or tools were installed. The public physics headers expose the
TerraEngine-owned `IPhysicsBackend` contract. Runtime binaries use the pinned
provider statically and do not require a globally installed physics package.
