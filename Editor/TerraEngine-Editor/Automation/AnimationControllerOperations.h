// AnimationControllerOperations.h - controller authoring transaction adapter.

#pragma once

#include "AnimationAutomationTransactions.h"

class AssetDatabase;

namespace TerraEngine::Editor::Automation
{
	[[nodiscard]] AutomationDispatchResult ExecuteAnimationControllerOperation(
		std::string_view operation,
		const nlohmann::json& arguments,
		AnimationAutomationTransactions& transactions,
		std::uint64_t generation,
		AssetDatabase* assetDatabase = nullptr);
}
