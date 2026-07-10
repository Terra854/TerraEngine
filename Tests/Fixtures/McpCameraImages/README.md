# MCP Camera Image Fixtures

These notes describe the authored camera states used by MCP camera image tests.
The current automated coverage builds the objects directly in editor automation
tests so the suite stays headless and deterministic.

- Active Game View camera: an enabled object with an active `CameraComponent`
  represents the runtime camera source for `game_view.capture`.
- Enabled non-game camera: an authored object with `Transform` and
  `CameraComponent` can be addressed by UUID, entity id, or display name through
  `camera.capture_object`.
- Disabled camera: an inactive object or inactive `CameraComponent` remains
  readable when `includeDisabled` is true. Capture must not enable, activate,
  select, reparent, or otherwise mutate the object.
- Custom camera: an ad-hoc pose and projection supplied in JSON produces an
  immutable descriptor. Validation rejects zero-sized targets, invalid clipping
  planes, and zero-length direction vectors before any renderer boundary work.

Native backend pixel readback is intentionally separate from these authored
state fixtures. Headless automation tests use the deterministic PNG fallback,
while installed-editor smoke tests exercise live Vulkan pixels and require the
response metadata to report backend readback instead of fallback content.
