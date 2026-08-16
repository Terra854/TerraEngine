#include "VirtualGeometryWorldHierarchy.h"

// World-hierarchy records are immutable frame inputs. Construction remains
// header-only so worker preparation can copy them without accessing live scene
// or renderer state.

// The frame identity is intentionally retained by the immutable snapshot
// owner. A zero frame ID is never a valid reusable hierarchy result; callers
// must refresh it for each view/origin generation before selection.
