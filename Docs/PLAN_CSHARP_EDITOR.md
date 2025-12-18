# Plan: C# Editor Integration (Hexa.NET.ImGui)

## Objective
Migrate the `TerraEngine` Editor UI from C++ to C# using `Hexa.NET.ImGui` (a wrapper around `cimgui`). This enables rapid UI development and access to the rich .NET ecosystem while maintaining the high-performance C++ backend for rendering and window management.

## Core Mandates
*   **Dependency Management:** All dependencies (including `Hexa.NET.ImGui`, `cimgui`, and any future .NET libraries) MUST be vendored in the `External/` directory. 
*   **No External NuGet:** Do not rely on external NuGet feeds for core engine or editor components to ensure build reproducibility and offline capability.
*   **Source-Based Integration:** Prefer project references to vendored source code over pre-compiled binaries where possible.

## 1. Architecture

### 1.1 Core Concept
*   **Host (C++)**: The `TerraEngine-Editor` executable remains the entry point. It manages:
    *   Window creation (GLFW).
    *   Vulkan Context & Swapchain.
    *   ImGui Backend Initialization (`ImGui_ImplGlfw`, `ImGui_ImplVulkan`).
    *   Frame lifecycle (`NewFrame`, `Render`, `Present`).
*   **Guest (C#)**: The `TerraEngine.Editor` assembly runs inside the host via `DotNetHost`. It is responsible for:
    *   Issuing ImGui commands (`ImGui.Begin`, `ImGui.Button`, etc.) during the update phase.
    *   Managing Editor state (selection, inspectors, windows).

### 1.2 ImGui Shared Context
To allow C# to issue commands to the same ImGui context initialized by C++:
1.  **Shared Binary**: We will compile `cimgui` as a shared library (`cimgui.dll` / `libcimgui.so`).
2.  **Context Sharing**:
    *   C++ initializes ImGui.
    *   C++ retrieves the `ImGuiContext*`.
    *   C++ passes this pointer to C# during initialization.
    *   C# calls `ImGui.SetCurrentContext(ptr)`.
3.  **Synchronization**: C++ handles `NewFrame()` before calling C# and `Render()` after C# returns.

## 2. Implementation Steps

### Phase 1: Build System & Dependencies
**Goal**: Build `cimgui` as a shared library that exposes the C-API required by `Hexa.NET.ImGui`.

1.  **CMake Update**:
    *   Add `External/cimgui` as a subdirectory.
    *   Define a `cimgui` target as a `SHARED` library.
    *   Ensure `IMGUI_STATIC` is *not* defined for this target (to allow export).
    *   Update `TerraEngine-Editor` to copy `cimgui.dll` to the output directory.
2.  **Nuget/References**:
    *   `TerraEngine.Editor` will reference `Hexa.NET.ImGui` (likely via project reference or direct source inclusion if vendored in `External`).
    *   *Note*: Since `External/Hexa.NET.ImGui` is present, we can include it as a source project or build it.

### Phase 2: Managed Editor Project
**Goal**: Create the C# project structure.

1.  **Project File**: Create `Managed/TerraEngine.Editor/TerraEngine.Editor.csproj`.
    *   Target Framework: `net10.0` (or `net9.0`).
    *   Allow `Unsafe` blocks.
2.  **Entry Point**: Create `Managed/TerraEngine.Editor/Source/EditorApplication.cs`.
    *   `public static void Initialize(IntPtr context)`
    *   `public static void Update(float deltaTime)`
    *   `public static void Shutdown()`

### Phase 3: C++ Integration
**Goal**: Hook the C# methods into the C++ Editor loop.

1.  **EditorSystem.cpp**:
    *   Use `DotNetHost::LoadAssembly` to load `TerraEngine.Editor.dll`.
    *   Get function pointers for `Initialize`, `Update`, `Shutdown`.
2.  **Lifecycle**:
    *   **Init**: Call `ManagedInitialize(ImGui::GetCurrentContext())`.
    *   **Update**: Inside the main loop, between `ImGui::NewFrame()` and `ImGui::Render()`, call `ManagedUpdate(dt)`.
3.  **Input**: Ensure C++ forwards input events to ImGui (already handled by `ImGui_ImplGlfw`).

## 3. Work Items

### 3.1 CMake Configuration (`CMakeLists.txt`)
*   Define `cimgui` target.
*   Add `TerraEngine.Editor` custom build target (calling `dotnet build`).
*   Ensure DLLs (`cimgui.dll`, `TerraEngine.Editor.dll`) are installed to `Build/Editor`.

### 3.2 C# Project Setup (`Managed/TerraEngine.Editor`)
*   Setup `.csproj` to reference `TerraEngine.Core`.
*   Setup `Hexa.NET.ImGui` usage.

### 3.3 C++ Editor Modification (`Editor/`)
*   Modify `EditorMain.cpp` or `EditorSystem.cpp` to bootstrap the .NET runtime.

## 4. Technical Constraints
*   **P/Invoke Resolution**: `Hexa.NET.ImGui` must be able to find `cimgui.dll`. We may need to use `NativeLibrary.SetDllImportResolver` in C# to point to the correct path if it's not in the immediate search path, but placing it alongside the executable is safest.
*   **Version Mismatch**: `cimgui` version must match the headers used by `TerraEngine` C++ (which comes from `External/imgui` inside `External/cimgui`).

