# Export Runtime Baseline Sample

This project is the minimal known-good export sample for TerraEngine. It is
kept deliberately small so failures point to one of the exported runtime
contracts instead of to complex demo content.

## Coverage

- Startup scene
- Mesh
- Material
- Texture
- UI
- Script
- Explicit project render settings
- Boot summary reporting

## Expected Result

The exported runtime should launch into `Scenes/Startup.tescene`, emit a boot
summary, and display a simple scene with one visible object, one UI signal, and
one script-controlled runtime signal.

The `*.temf`, `*.tetf`, and `*.tmat` files in this folder are source-control
placeholders that name the required compiled assets. A runnable manual export
should regenerate those files through the mesh, texture, and material compiler
paths before packaging.

Expected baseline signals:

- Project metadata: `Export Runtime Baseline` version `1.0.0`
- Startup scene: `Scenes/Startup.tescene`
- Render settings source: `ProjectSettings/RenderSettings.json`
- UI text: `Export Runtime Baseline Ready`
- Script log: `ExportRuntimeBaseline.ScriptStarted`
