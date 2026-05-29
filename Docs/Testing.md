# TerraEngine Testing

This document is the reproducible CTest workflow for the current unit test
layout. Engine tests live under `Engine/Tests`; editor tests live under
`Editor/Tests`. Launcher tests are intentionally out of scope for the current
test feature.

## Configure

```powershell
cmake --preset Debug
```

## Build Unit Tests

```powershell
cmake --build out/build/Debug --target TerraEngine-EngineTests
cmake --build out/build/Debug --target TerraEngine-EditorTests
```

## Run CTest

```powershell
ctest --test-dir out/build/Debug --output-on-failure
```

## Install After Code Changes

Run the install target before finishing any task that changed code so the
install tree stays verified with the compiled outputs.

```powershell
cmake --build out/build/Debug --target install
```
