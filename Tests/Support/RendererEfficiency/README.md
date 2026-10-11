# Renderer efficiency validation support

This directory owns renderer acceptance fixtures, camera and fault controls,
readback evidence containers, editor validation sessions, fixture bindings,
publication checks, install identity checks, and evidence export. `Renderer/`
contains native fixture helpers; `Editor/` contains the editor-side harness.

The engine retains rendering algorithms, renderer options and counters, GPU
readback operations, and the typed frame/publication interfaces that the harness
uses. The editor retains its general automation service and device lifecycle.
Their implementation files call this support at the existing scheduler and
protected mutation boundaries; moving the harness does not change those boundaries.

These support sources are compiled into the editor and editor test executable so
installed-editor validation can exercise the real integration. Fixture fault
controls remain enabled by `TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION` when
`BUILD_TESTING` is enabled. This source move does not make the complete validation
controller optional or remove the backend readback hooks.

From a Visual Studio developer shell, configure, compile and install with:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^(Engine|Editor)\.RendererEfficiency$' --output-on-failure
```

See `Docs/Testing.md` for bundled PowerShell configuration and the CTest workflow.
The native acceptance pipeline is a separate, explicitly invoked operation;
building this support and running the filtered unit tests do not launch it.
