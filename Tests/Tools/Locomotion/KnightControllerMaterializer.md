# Knight navigation-demo controller

Validation-only helper for the CC0 Quaternius knight imported during the live-editor test. Its skeleton and clip IDs are specific to that import, not portable character defaults. It creates a new controller file and refuses to overwrite an existing path.

Prerequisites: configured and built RelWithDebInfo checkout, Windows Visual Studio compiler and SDK, and the imported knight package in the existing navigation demo. Normal configure/build/test instructions remain in `Docs/Testing.md`.

Compile from the repository root in PowerShell:

```powershell
& Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1 -CMakeArguments @('-E','env','cl','/nologo','/std:c++20','/EHsc','/MD','/IEngine','Tests/Tools/Locomotion/KnightControllerMaterializer.cpp','out/build/RelWithDebInfo/TerraEngine.lib','/Fe:out/evidence/locomotion/060/knight-live/KnightControllerMaterializer.exe','/Fo:out/evidence/locomotion/060/knight-live/KnightControllerMaterializer.obj')
$env:PATH=(Resolve-Path out/build/RelWithDebInfo/Build/Editor).Path+';'+$env:PATH
& out/evidence/locomotion/060/knight-live/KnightControllerMaterializer.exe '<new output path>.teanimctrl'
cmake --install out/build/RelWithDebInfo --config RelWithDebInfo
```

The output is serialized and deserialized with the production codec before writing. The editor's `animation.controller.validate` additionally checks the imported controller. States 101/102/103 represent Idle/Walking/Run; triggers 701/702/703 select their destinations, with six 0.2-second transitions. The helper does not drive motion or prove visual playback correctness.

The current live-demo script and evidence reside under the installed navigation-demo project and `out/evidence/locomotion/060/knight-live`. Transition playback and rendered capsule alignment remain pending; see that evidence directory's `RESULTS.md` for the stopped test and its policy blocker.
