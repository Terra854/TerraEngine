# Full RelWithDebInfo Regression (T151)

Validation date: 2026-07-14  
Configuration: `RelWithDebInfo`, Windows x64, Visual Studio 2026 toolchain

## Complete CTest result

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
```

Result: **47/47 registrations passed, 0 failed, in 33.48 seconds**.
This includes every physics dependency, foundation, user-story, performance,
stress, and editor soak registration as well as the complete renderer and
export regression set. In particular, `Engine.PhysicsStress` passed in 3.95
seconds and `Editor.PhysicsSoak` passed in 0.93 seconds.

## Regressions found and closed

The first complete run identified pre-existing or newly exposed contract-test
drift rather than physics simulation failures:

- A Windows repository-root search could loop forever because a drive root is
  its own parent. The traversal now terminates and the maintained `Object.h`
  removal contract is checked directly.
- Export test packages now stage the required minimal startup scene; a new
  negative test preserves fail-closed behavior when it is absent.
- Headless export documentation and CI contract tests now use the current
  RelWithDebInfo names, Spec 042 quickstart, and evidence artifact.
- Project Settings expects the intentional fourth Physics category.
- The Vulkan fence-order source check now verifies semantic token order without
  depending on line-ending style.

The nine registrations affected by those corrections passed 9/9 in a focused
rerun before the complete 47-test run. The object-facade and Vulkan source
checks also passed 3/3, and the broader export/documentation/settings group
passed 28/28.

## Sanitizer boundary

The complete regression is correctness evidence, not sanitizer evidence. The
main Windows project has no full-engine ASan or TSan lane compatible with this
run's RelWithDebInfo-only requirement. The capability audit and explicit
exclusions are recorded in `sanitizer-validation.md` without claiming a
sanitizer pass.
