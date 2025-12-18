# Plan: .NET 10 CoreCLR Integration

## Objective
Integrate .NET 10 (CoreCLR) into TerraEngine to serve two primary purposes:
1.  **Runtime Scripting:** Allow users to write game logic in C#.
2.  **Editor Evolution:** Enable the migration of the Editor from C++ to C#, leveraging the robust .NET ecosystem.

## Core Mandates
*   **Embedding (Not Hosting):** The C++ Engine (`TerraEngine-Engine`) remains the owner of the process and lifecycle. It will load and initialize the .NET Runtime.
*   **Decoupling:** The Scripting Runtime must be a distinct module (`Engine/Scripting`), accessible by both the Runtime (Game) and Editor systems.
*   **.NET 10 Readiness:** Design for the latest .NET standards (using `nethost` and `hostfxr` APIs which are forward-compatible).

## 1. Architecture Overview

The integration will be split into three distinct layers to ensure that CoreCLR hosting is a shared utility used by both Scripting and the Editor.

### 1.1 Module Location
*   **C++ Hosting Layer:** `Engine/DotNet/` (Shared Utility)
    *   `DotNetHost.h/cpp`: Manages `hostfxr` lifetime, runtime initialization, and provides a generic `LoadAssembly` and `GetFunctionPointer` API.
*   **C++ Systems Layer:**
    *   `Engine/Scripting/`: Gameplay-specific scripting system. Uses `DotNetHost` to load game scripts.
    *   `Editor/`: Editor system. Uses `DotNetHost` to load editor-specific managed assemblies.
*   **Managed Layer (C#):** `Managed/` (New root-level directory)
    *   `TerraEngine.Core/`: The foundational C# bindings and interop for the Engine.
    *   `TerraEngine.Runtime/`: User-land gameplay scripting API.
    *   `TerraEngine.Editor/`: Managed implementation of the Editor.
*   **Dependencies:** `External/dotnet/`
    *   Includes: `nethost.h`, `coreclr_delegates.h`, `hostfxr.h`
    *   Libs: `libnethost.lib` (static link).

### 1.2 Data Flow
1.  **Initialization:** `MainEngine` (or a dedicated bootstrap) initializes `DotNetHost`.
2.  **Service Provision:** `ScriptingSystem` and `EditorSystem` request `DotNetHost` to load their respective assemblies.
3.  **Interop:**
    *   **C++ -> C#:** `DotNetHost` returns function pointers to static C# methods.
    *   **C# -> C++:** Standard P/Invoke to `extern "C"` functions exported by the Engine.

## 2. Implementation Plan

### Phase 1: The Shared Host
**Goal:** Create a standalone C++ module to embed CoreCLR.

1.  **Engine/DotNet Implementation:**
    *   Create `DotNetHost` class.
    *   Encapsulate `hostfxr` and `nethost` logic.
    *   Expose `bool Initialize()`, `bool LoadAssembly(path)`, and `void* GetManagedFunction(...)`.
2.  **Build System (CMake):**
    *   Update root `CMakeLists.txt` to include `Engine/DotNet`.
    *   Configure CMake to manage C# projects (using `enable_language(CSharp)` or custom templates).
    *   **Note:** CMake will generate the `.sln` and `.csproj` files in the build directory; do not create them manually in the source tree.

### Phase 2: Foundation (Managed Libraries)
**Goal:** Setup the C# source and build logic within the CMake ecosystem.

1.  **Managed Structure:**
    *   Define `Managed/TerraEngine.Core` and other projects in `CMakeLists.txt`.
    *   Direct build output to `Build/Binaries/Managed/`.
2.  **Shared Bindings:**
    *   Ensure `TerraEngine.Core` contains logic used by both Runtime and Editor.

## 3. Directory Structure Proposal

```text
TerraEngine/
├── Engine/
│   ├── DotNet/                    # CoreCLR Embedding (Shared)
│   │   ├── DotNetHost.h/cpp       # hostfxr / nethost wrapper
│   │   └── DotNetInterop.h        # Interop helper macros/types
│   ├── Scripting/                 # Gameplay Scripting System
│   │   ├── ScriptingSystem.h/cpp
│   │   └── ScriptGlue.cpp         # Script-specific C-API
├── Managed/                       # Managed Source Code
│   ├── TerraEngine.Core/          # Core Engine Bindings (Source Only)
│   ├── TerraEngine.Runtime/       # Game Scripting (Source Only)
│   └── TerraEngine.Editor/        # Managed Editor Logic (Source Only)
├── External/
│   └── dotnet/                    # Vendor: nethost, hostfxr headers
```

## 4. Next Steps for Developer

1.  **Download .NET 10 (or 9) SDK.**
2.  **Acquire `nethost`**: Locate `nethost.h` and `libnethost.lib` from the SDK installation.
3.  **Create `Scripting/` folder** and initialize the C# solution.
4.  **Implement `ScriptEngine`** following the `nethost` sample pattern.
