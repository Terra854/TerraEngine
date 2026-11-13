// EditorClosePolicy.h -- Shared decision used by import-close continuation.
#pragma once

namespace TerraEngine::Editor
{
	enum class EditorCloseDecision { SaveConfirmation, ExecutePendingAction };

	// Inputs are captured on the protected UI thread after import cancellation.
	// The returned decision controls the caller's existing save/execute branch.
	[[nodiscard]] EditorCloseDecision EvaluateImportClosePolicy(
		bool sceneDirty, bool terrainDirty, bool prefabEditing, bool prefabDirty) noexcept;
}
