// AnimationAssetOperations.h - transactional animation asset operations.

#pragma once

#include "AnimationAutomationTransactions.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

class AssetDatabase;

namespace TerraEngine::Editor::Automation
{
	using AnimationCookedAssetEdit = std::function<AutomationDispatchResult(
		std::span<const std::uint8_t> currentBytes,
		std::vector<std::uint8_t>& candidateBytes)>;

	// Resolve one registered cooked asset, prepare a complete candidate in memory,
	// and atomically replace its source before asking AssetDatabase to publish the
	// build artifact. A failed compile/verification restores the original bytes.
	[[nodiscard]] AutomationDispatchResult CommitCookedAnimationAssetEdit(
		AssetDatabase& assetDatabase,
		const TerraEngine::UUID& assetId,
		std::string_view requiredImporter,
		std::size_t maximumBytes,
		bool dryRun,
		const AnimationCookedAssetEdit& edit);

	[[nodiscard]] AutomationDispatchResult ExecuteAnimationAssetOperation(
		std::string_view operation,
		const nlohmann::json& arguments,
		AnimationAutomationTransactions& transactions,
		std::uint64_t generation);
}
