# TerraEngine Contributor Guide

Welcome to TerraEngine! Please follow these repo-wide guidelines whenever you add, modify, or review code. They ensure consistent build behavior, documentation quality, and runtime performance across the project.

## Build & Dependency Expectations
1. **CMake install required** – You must invoke `cmake --build <build-dir> --target install` (or equivalent via `cmake --install`) before finishing a task if any code changes were made (including moving and renaming files). Update docs/scripts accordingly and verify the install tree is produced before marking a task complete.
2. **Self-contained dependencies** – Do not rely on globally installed system libraries or package managers at runtime/build time. Vendor, subtree, or otherwise bundle all third-party code inside `External/` (or another committed directory) and make the CMake configuration point exclusively to those copies.
3. **Shader separation** – All GPU shader source must live in dedicated shader files (e.g., `*.vert`, `*.frag`, `*.comp`) under `Engine/Shaders/` (create subfolders there if needed). Never embed shader strings directly in C++ or config files.

## Code Quality & Documentation
1. **Explain with comments** – Any code you touch must contain clear, beginner-friendly comments explaining intent, inputs, outputs, and tricky logic. Prioritize maintainability over brevity.
2. **Task-based multithreading** – Architect new features around task/job systems that can run across multiple threads. Avoid single-threaded bottlenecks; where sequential execution is unavoidable, justify it in comments.
3. **C++ style** – Target C++20 as the minimum language baseline. Prefer RAII and smart pointers, keep headers minimal, and avoid macros when a typed construct suffices.
4. **Component architecture** – Every engine component must follow the CRTP pattern via `Component<Derived>` in `Engine/Core/Object/Component.h`. Provide a `static constexpr std::string_view kComponentName` and register properties through the optional `Derived::reflect` helper when reflection metadata is required.
5. **Systems inheritance** – All engine systems must inherit from `SystemsBase` to ensure consistent lifecycle handling and extensibility across modules.
6. **Indent requirements** – Indentation must use a single tab level.

## Repository Structure
1. Keep engine sources under `Engine/`, third-party code in `External/`, and **all shaders in `Engine/Shaders/`** (use module-specific subdirectories beneath that folder).
2. Place build tooling/scripts under `Tools/` with usage comments explaining prerequisites and commands.

## Testing & Verification
1. Provide reproducible instructions (README or per-feature docs) describing how to configure, build, install, and run relevant tests/demos from a clean checkout.
2. When adding features affecting rendering or threading, include stress or regression tests demonstrating correctness under multithreaded loads.
3. After all implementation tasks are complete, run a test compile of the codebase before finishing so the changed code is proven to compile successfully.

## Pull Request Expectations
1. Reference this guide in PR descriptions to confirm compliance (self-contained deps, shaders separated, comments present, install step executed).
2. Call out any deviations (e.g., temporary system dependency) and supply a plan/timeline for remediation.

Thank you for helping keep TerraEngine portable, performant, and approachable!
