#include "VirtualGeometryRepresentations.h"

// Aggregate representation records are value-only contracts. Their behavior
// is intentionally implemented inline so cooker and runtime tools share the
// same validation rules without creating a second stateful subsystem.
