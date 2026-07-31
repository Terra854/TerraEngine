#include "VirtualGeometryWorldHierarchy.h"

// World-hierarchy records are immutable frame inputs. Construction remains
// header-only so worker preparation can copy them without accessing live scene
// or renderer state.
