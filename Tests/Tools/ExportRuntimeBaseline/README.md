# Export Runtime Baseline Tools

Tools in this directory support manual and CI validation of
`Tests/Fixtures/ExportRuntimeBaseline`.

## Intended Commands

- Export the sample project.
- Launch the exported runtime in validation mode.
- Report whether render settings, material, texture, scene, UI, script,
  startup, or packaging validation failed.

The implementation should keep scripts self-contained and point to installed
TerraEngine binaries and assets rather than relying on global tools.
