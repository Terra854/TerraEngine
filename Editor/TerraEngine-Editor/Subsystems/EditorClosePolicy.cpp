// EditorClosePolicy.cpp -- Preserve every owner's authored changes during import close.
#include "EditorClosePolicy.h"

namespace TerraEngine::Editor
{
	EditorCloseDecision EvaluateImportClosePolicy(
		bool sceneDirty, bool terrainDirty, bool prefabEditing, bool prefabDirty) noexcept
	{
		// Terrain authoring owns durable state independently of scene/prefab edits.
		// Every dirty owner must choose Save before the caller executes close/exit.
		return sceneDirty || terrainDirty || (prefabEditing && prefabDirty)
			? EditorCloseDecision::SaveConfirmation : EditorCloseDecision::ExecutePendingAction;
	}
}
