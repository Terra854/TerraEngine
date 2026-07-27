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

<!-- SPECKIT START -->
## Active Spec Kit Plan

- Current feature plan: `specs/043-complete-animation-system/plan.md`
- Technology context: C++20 native engine/editor and HLSL shader code, C#/.NET 10 managed gameplay bindings, bundled fastgltf, Assimp, meshoptimizer, Jolt Physics v5.6.0 commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`, Vulkan/VMA/volk, `ThreadPool`, the scheduler/job graph, `SceneMutationQueue`, `TransactionalMutation`, animation asset cooking and GPU evaluation, editor automation, TEPAK export/runtime mounting, CTest, performance/stress/soak validation, and installed-editor loopback testing.

## GPT-5 Delegation Policy for Spec Kit

Apply the **Model and execution-mode recommendation** subsection during `/speckit.plan` and `/speckit.tasks` when the active parent model belongs to the GPT-5 family.

Apply all remaining subsections only when:

- the active workflow is `/speckit.implement`;
- the active parent model belongs to the GPT-5 family;
- the `terra` and `luna` custom agents are available.

For other models or Spec Kit commands, ignore the inapplicable parts of this section and follow the normal workflow without assuming Terra or Luna exist.

### Model and execution-mode recommendation

During `/speckit.plan`, record a provisional warning when the proposed architecture appears unusually ambiguous, tightly coupled, high-risk, or unsuitable for parallel implementation.

During `/speckit.tasks`, after `plan.md` and the task graph are complete, produce or confirm the final execution-mode recommendation for the later `/speckit.implement` run.

The recommendation must consider:

- task count and dependency structure;
- critical-path length and available parallelism;
- coupling among affected systems;
- clarity of file and subsystem boundaries;
- likely overlap in files, interfaces, generated artifacts, or test fixtures;
- the amount of repetitive, mechanical, or read-heavy work;
- architectural ambiguity and implementation risk;
- expected integration and validation cost;
- prior failed implementation attempts, when applicable.

Recommend one of:

- **Mixed GPT-5.6 delegation** — use a Sol orchestrator with Terra and Luna workers when the task list contains several bounded, independent, or safely parallelizable assignments.
- **Sol-only** — use one coherent Sol session when the work is small, tightly coupled, difficult to partition, or dominated by one complex implementation or debugging problem.
- **Higher Sol reasoning effort** — recommend `xhigh` when the work involves substantial ambiguity, architecture, concurrency, security, transactional correctness, difficult integration, or repeated failed attempts.
- **Ultra** — recommend Ultra only when the task is exceptionally large or difficult, maximum quality is preferred over cost, and broad proactive delegation is likely to provide meaningful benefit.

The final recommendation must state:

1. The recommended execution mode.
2. Why it fits the completed plan and task graph.
3. The main cost, accuracy, and coordination tradeoffs.

Record the final recommendation in `tasks.md` under an `Execution Recommendation` section so it is available when `/speckit.implement` begins.

The recommendation is advisory. During `/speckit.implement`, follow the active configuration unless the user explicitly wants a different mode.

If the active execution mode appears materially unsuitable when `/speckit.implement` begins, report the concern before making substantial changes.

### Orchestrator ownership

The parent GPT-5 session is the implementation orchestrator. It owns:

- interpreting `spec.md`, `plan.md`, `tasks.md`, and related artifacts;
- selecting ready tasks and resolving dependencies;
- deciding what to delegate;
- resolving ambiguous, conflicting, cross-cutting, or high-impact decisions;
- coordinating overlapping systems and integrating worker changes;
- updating task status;
- performing final integrated compilation and validation;
- determining whether the implementation satisfies the specification.

The orchestrator exclusively performs:

- the full project or solution build;
- the complete CTest and regression suites;
- integration and installed-editor loopback validation;
- final acceptance checks;
- final task completion decisions.

Worker-local validation is supporting evidence only. It does not replace the orchestrator's integrated validation.

### Delegation workflow

Before delegating, the orchestrator must:

1. Read the current feature's `spec.md`, `plan.md`, and `tasks.md`.
2. Identify ready tasks and their dependencies.
3. Define each assignment's scope, expected files, constraints, and acceptance criteria.
4. Define the smallest useful local build and test scope.
5. Avoid concurrent assignments that may edit overlapping files, symbols, interfaces, generated artifacts, or test fixtures.
6. Keep materially ambiguous tasks in the parent session.

Each worker assignment must include:

- applicable Spec Kit task IDs;
- the exact deliverable and definition of done;
- relevant requirements and plan constraints;
- expected files or subsystem boundaries;
- prohibited or unrelated areas;
- dependencies on other tasks;
- permitted local compilation and test scope;
- validation reserved for the orchestrator.

Workers may read Spec Kit artifacts, but must not modify `spec.md`, `plan.md`, or `tasks.md`. Required planning changes must be reported to the orchestrator.

### Delegation budget and agent reuse

Delegation must remain selective. Do not create a new worker for every Spec Kit task.

For each implementation phase:

- begin with no more than three concurrent workers;
- group related tasks that share a subsystem and stable context into one worker   assignment;
- prefer reusing an existing worker for follow-up work in the same subsystem;
- do not spawn a replacement worker merely because a task boundary was crossed;
- spawn additional workers only when there is substantial independent work that can proceed concurrently without overlapping ownership;
- do not exceed four active workers without first explaining why the additional parallelism outweighs coordination cost.

Before spawning a worker, the orchestrator must determine that the assignment contains enough substantial work to justify repository orientation and handoff overhead.

Prefer parent execution when:

- the assignment is small;
- it modifies a central integration file;
- it depends immediately on another active assignment;
- it requires the same context already held by the orchestrator;
- the orchestrator would need to repeat most of the worker's investigation;
- no meaningful wall-clock parallelism would result.

A worker may handle multiple sequential task IDs within the same subsystem when their requirements, files, and validation scope are closely related.

### Agent lifecycle

Keep a worker assigned to the same subsystem across related tasks when possible.
Use follow-up instructions in the existing worker context instead of spawning a new worker.

Conclude a worker only when:

- its subsystem assignment is complete;
- its remaining tasks are blocked;
- continued work would cross into another worker's ownership;
- the worker has become unreliable or its context is no longer useful.

### Use `luna` for

- targeted repository search and evidence gathering;
- locating files, symbols, call paths, dependencies, and conventions;
- extracting requirements from Spec Kit artifacts;
- inspecting logs and build or test output;
- mechanical, repetitive, and low-risk edits;
- documentation and straightforward structured transformations;
- checklist-based inspection;
- lightweight, narrowly scoped validation.

Suitable Luna validation includes:

- formatters and linters;
- static checks;
- a narrowly named unit test;
- a filtered test case;
- a small validation script;
- the smallest practical compilation target for a simple change.

Luna must escalate unexpected complexity, architectural decisions, plan conflicts, broad scope, or failures outside its assignment.

### Use `terra` for

- substantial but bounded tasks from `tasks.md`;
- implementation within a clearly defined subsystem;
- debugging through code inspection and task-local execution;
- synthesizing Luna findings;
- reviewing changes against the specification and plan;
- identifying correctness, ownership, lifetime, concurrency, compatibility, and regression risks;
- proposing solutions for underspecified implementation details, subject to orchestrator approval.

Terra may compile the directly affected target, run task-specific tests, and fix failures caused by its own changes.

Terra must not redefine requirements, override plan decisions, or expand into unrelated tasks.

### Worker validation

Workers may perform only the task-local validation needed for their assignments.

Workers may:

- compile the directly affected target or smallest practical build scope;
- run relevant unit tests or filtered CTest cases;
- run formatters, linters, static analysis, or local validation scripts;
- diagnose and fix failures caused by their own changes;
- repeat local validation until the work is ready for integration;
- report exact commands and outcomes.

Workers must not, unless explicitly authorized for a specific assignment:

- run the full project or solution build;
- run the complete CTest or regression suite;
- run installed-editor loopback validation;
- perform final integration or acceptance testing;
- declare the overall feature complete;
- mark Spec Kit tasks complete.

Use the narrowest validation scope that provides meaningful confidence. Do not duplicate full-project validation across workers.

### Worker rules

All workers must:

- stay within assigned task IDs and boundaries;
- avoid unrelated cleanup, refactoring, or feature additions;
- inspect existing architecture and conventions before editing;
- avoid overlapping concurrent edits;
- distinguish evidence from inference;
- report contradictions among the implementation, specification, plan, and task list;
- leave unresolved architectural and requirement decisions to the orchestrator;
- report exact local validation commands and results;
- clearly distinguish local validation from integrated validation.

Workers must not:

- modify Spec Kit planning artifacts;
- mark tasks complete;
- commit, merge, revert, or discard another worker's changes unless directed;
- broaden build or test scope without clear need or authorization;
- perform destructive, irreversible, externally visible, or high-impact actions without explicit authorization.

### Worker handoff

Each worker must return:

1. Spec Kit task IDs addressed
2. Result and changes made
3. Files and symbols inspected or modified
4. Requirements and plan constraints applied
5. Local compilation, tests, and checks, including exact commands and outcomes
6. Failures fixed or still present
7. Assumptions, unresolved issues, and integration risks
8. Recommended integrated validation

### Integration and validation

After collecting worker results, the orchestrator must:

1. Inspect and reconcile all changes.
2. Resolve overlapping edits and conflicting assumptions.
3. Verify conformance with `spec.md`, `plan.md`, and `tasks.md`.
4. Review worker-local validation and identify missing coverage.
5. Compile the fully integrated implementation.
6. Run the applicable full CTest, regression, integration, and installed-editor loopback validation.
7. Diagnose cross-cutting failures directly or delegate bounded investigation and repair.
8. Rerun the necessary integrated validation after fixes.
9. Repeat until acceptance criteria pass or a blocker is documented.
10. Update task status only after successful integrated validation.

Use subagents only when delegation is likely to improve quality, preserve context, enable safe parallelism, or reduce cost. Do not delegate trivial work when coordination overhead would exceed the benefit.
<!-- SPECKIT END -->
