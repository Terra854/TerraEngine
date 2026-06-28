# TerraEngine Contributor Guide

Welcome to TerraEngine! Please follow these repo-wide guidelines whenever you add, modify, or review code. They ensure consistent build behavior, documentation quality, and runtime performance across the project.

## Build & Dependency Expectations
1. **CMake install required** – You must invoke `cmake --build <build-dir> --target install` (or equivalent via `cmake --install`) before finishing a task if any code changes were made (including moving and renaming files). Update docs/scripts accordingly and verify the install tree is produced before marking a task complete.
2. **Self-contained dependencies** – Do not rely on globally installed system libraries or package managers at runtime/build time. Vendor, subtree, or otherwise bundle all third-party code inside `External/` (or another committed directory) and make the CMake configuration point exclusively to those copies.
3. **Shader separation** – All GPU shader source must live in dedicated shader files (e.g., `*.vert`, `*.frag`, `*.comp`) under `Engine/Shaders/` or `Editor/Shaders/` (create subfolders there if needed). Never embed shader strings directly in C++ or config files.

## Code Quality & Documentation
1. **Explain with comments** – Any code you touch must contain clear, beginner-friendly comments explaining intent, inputs, outputs, and tricky logic. Prioritize maintainability over brevity.
2. **Task-based multithreading** – Architect new features around task/job systems that can run across multiple threads. Avoid single-threaded bottlenecks; where sequential execution is unavoidable, justify it in comments.
3. **C++ style** – Target C++20 as the minimum language baseline. Prefer RAII and smart pointers, keep headers minimal, and avoid macros when a typed construct suffices.
4. **Component architecture** – Every engine component must follow the CRTP pattern via `Component<Derived>` in `Engine/Core/Object/Component.h`. Provide a `static constexpr std::string_view kComponentName` and register properties through the optional `Derived::reflect` helper when reflection metadata is required.
5. **Systems inheritance** – All engine systems must inherit from `SystemsBase` to ensure consistent lifecycle handling and extensibility across modules.
6. **Indent requirements** – Indentation must use a single tab level.

## Runtime Architecture Expectations
1. **Scheduler declarations required** – Every new or modified engine/editor system must declare its scheduler phase, read resources, write resources, worker-safe preparation work, and protected boundary work. Diagnostics should make those declarations visible when the system affects frame execution.
2. **Transactional mutation path** – Scene, object, hierarchy, and component writes that originate from worker jobs, managed C# callbacks, MCP/editor automation, future physics/animation/networking/AI systems, or other cross-system producers must use `SceneMutationQueue`, transactional mutation APIs, or a typed deferred mutation path. Do not mutate live `ObjectManager` or component storage from worker-safe code.
3. **Visibility boundaries** – Queued transactional/deferred mutations become visible only after a declared protected flush boundary. Add a flush after a phase only when later same-frame systems must observe those writes; otherwise prefer the frame-end flush to preserve throughput.
4. **Rejected mutations are atomic** – Rejected or conflicting mutation batches must leave no partial live-state changes. Add regression tests for conflict, rollback, and visibility timing whenever a feature adds new mutation operations or new system phases.
5. **Hot paths stay typed** – Native high-frequency systems should use typed transaction batches, snapshots, packets, or scheduler-owned prepared data. JSON transaction payloads are for managed scripting, editor automation, MCP, compatibility, and tests unless explicitly justified.

## Repository Structure
1. Keep engine sources under `Engine/`, third-party code in `External/`, and **all engine shaders in `Engine/Shaders/` with editor only shaders in `Editor/Shaders/`** (use module-specific subdirectories beneath that folder).
2. Place build tooling/scripts under `Tools/` with usage comments explaining prerequisites and commands.

## Testing & Verification
1. Provide reproducible instructions (README or per-feature docs) describing how to configure, build, install, and run relevant tests/demos from a clean checkout.
2. When adding features affecting rendering or threading, include stress or regression tests demonstrating correctness under multithreaded loads.
3. When adding systems or mutation behavior, include tests proving scheduler read/write declarations, flush-boundary visibility, and rejected-batch rollback behavior.
4. After all implementation tasks are complete, run a test compile of the codebase before finishing so the changed code is proven to compile successfully.
5. Use `Docs/Testing.md` for the current CTest workflow; keep engine unit tests under `Tests/Engine` and editor unit tests under `Tests/Editor` unless a later feature explicitly expands the scope.

## Commit Message Expectations
1. Follow the detailed, area-first style used by the earlier project history through commit `3aa8ae41466ea3c6ffed54cfeb1d881a1aad2d8c`: `<Area>: <short imperative summary>.`
2. Use the subject prefix to name the subsystem or kind of work being changed rather than a generic change category. Prefer existing title-case areas such as `Renderer`, `Editor`, `Resource`, `Asset Pipeline`, `Scene`, and `Build`; add a similarly clear area only when the change does not fit an existing one.
3. After the one-line subject, include a blank line and a longer commit body. Use concise bullet points to explain the concrete behavioral, architectural, build, asset, or documentation changes made by the commit.
4. Keep the subject focused on the main effect of the change, and keep body bullets specific enough that a reader can understand what changed without opening the diff.

## Pull Request Expectations
1. Reference this guide in PR descriptions to confirm compliance (self-contained deps, shaders separated, comments present, install step executed).
2. Call out any deviations (e.g., temporary system dependency) and supply a plan/timeline for remediation.

Thank you for helping keep TerraEngine portable, performant, and approachable!
