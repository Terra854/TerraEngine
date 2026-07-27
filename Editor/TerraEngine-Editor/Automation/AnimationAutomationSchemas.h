// AnimationAutomationSchemas.h - versioned bounded MCP contracts for animation.
#pragma once
#include "EditorAutomationTypes.h"
#include <optional>
#include <string_view>
#include <vector>
namespace TerraEngine::Editor::Automation {
	inline constexpr std::uint32_t kAnimationAutomationSchemaVersion = 1U;
	inline constexpr std::size_t kAnimationAutomationMaximumItems = 256U;
	[[nodiscard]] nlohmann::json AnimationAutomationInputSchema(std::string_view operation);
	[[nodiscard]] nlohmann::json AnimationAutomationResultSchema(bool jobResult = false);
	[[nodiscard]] std::vector<std::string> AnimationAutomationOperationNames();
	[[nodiscard]] std::optional<AutomationCapabilityDescriptor> DescribeAnimationAutomationOperation(std::string_view operation);
	[[nodiscard]] AutomationDispatchResult ValidateAnimationAutomationRequest(std::string_view operation, const nlohmann::json& arguments);
}
