#include "LightingDirtyFixture.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace TerraEngine::Tests::RendererEfficiency
{
	namespace
	{
		using Json = nlohmann::json;
		constexpr std::uint32_t PoolSize = 1025;
		constexpr float BaselineIntensity = 0.25f;

		// IDs are stable across runs and separate from the foundation/P1 namespaces.
		std::string LightId(std::uint32_t ordinal)
		{
			std::ostringstream text;
			text << "6520000000000000" << std::hex << std::setfill('0') << std::setw(16) << ordinal;
			return text.str();
		}
		Json Pose()
		{
			const Json offset = {0.0, 3.0, 0.0};
			return {{"WorldPose", {{"position", {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}},
				{"offsetMetres", offset}}}, {"rotation", {1, 0, 0, 0}}, {"scale", {1, 1, 1}}}},
				{"ParentRelativePose", {{"translationMetres", offset}, {"rotation", {1, 0, 0, 0}},
				{"scale", {1, 1, 1}}, {"transformSpaceId", 0}}}};
		}
		Json Light(std::uint32_t ordinal, bool active)
		{
			return {{"uuid", LightId(ordinal)}, {"name", "Lighting Record " + std::to_string(ordinal)},
				{"active", active}, {"isStatic", false}, {"tag", "Untagged"}, {"layer", "Default"},
				{"children", Json::array()}, {"components", {{"Transform", Pose()},
				{"LightComponent", {{"type", 1}, {"unit", 0}, {"color", {1, 1, 1}},
				{"intensity", BaselineIntensity}, {"range", 30.0}, {"castsShadows", false},
				{"authoredPriority", static_cast<float>(ordinal == 1001 ? 2048 : PoolSize + 1 - ordinal)},
				{"stabilityPreference", 0.0}}}}}};
		}
		// Remove inherited illumination recursively, while retaining foundation
		// geometry. Holding authored animation avoids unrelated per-frame changes.
		void PrepareFoundation(Json& objects)
		{
			for (auto& object : objects)
			{
				if (object.contains("components"))
				{
					auto& components = object["components"];
					components.erase("LightComponent");
					if (components.contains("AnimatorComponent"))
						components["AnimatorComponent"]["playAutomatically"] = false;
				}
				if (object.contains("children")) PrepareFoundation(object["children"]);
			}
			objects.erase(std::remove_if(objects.begin(), objects.end(), [](const Json& object)
				{ return object.value("uuid", "") == "65000000000000000000000000000002"; }), objects.end());
		}
		Json ActiveRequest(std::uint32_t ordinal, bool active)
		{
			return {{"operation", "SetActive"}, {"objectId", LightId(ordinal)}, {"active", active}};
		}
		Json IntensityRequest(std::uint32_t ordinal, float intensity)
		{
			return {{"operation", "UpdateComponentProperty"}, {"objectId", LightId(ordinal)},
				{"componentName", "LightComponent"}, {"propertyName", "intensity"}, {"value", intensity}};
		}
		Json Order(std::uint32_t count, bool reverse = false, std::uint32_t omitted = 0)
		{
			Json ids = Json::array();
			for (std::uint32_t i = 1; i <= count; ++i) if (i != omitted) ids.push_back(LightId(i));
			if (reverse) std::reverse(ids.begin(), ids.end());
			return ids;
		}
	}

	LightingDirtyFixture BuildLightingDirtyFixture(const Json& foundationScene,
		const Json& definition, std::string_view manifestHash, std::uint64_t seed)
	{
		// Reject drift rather than silently inventing a workload or shortening its
		// observation window. These are recipes, never measured acceptance results.
		const std::string id = definition.at("id").get<std::string>();
		const bool dirty = id == "lighting-dirty";
		if ((!dirty && id != "lighting-thresholds") || definition.at("stage") != "P2"
			|| definition.at("seed").get<std::uint64_t>() != seed
			|| seed != (dirty ? 65002u : 65003u)
			|| manifestHash.size() != 64 || !std::all_of(manifestHash.begin(), manifestHash.end(),
				[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })
			|| definition.at("cameraIds") != Json::array({"scene-fixed"}))
			throw std::invalid_argument("Lighting fixture identity/manifest contract is invalid.");
		const auto& parameters = definition.at("parameters");
		if (parameters.at("uploadCapacity") != 1024 || parameters.at("retainedLightBudget") != (dirty ? 1000 : 1024)
			|| parameters.at("lightOrder") != "stable-id-ascending" || parameters.at("selectionTies") != false
			|| (dirty ? parameters.at("lightCount") != 1000 : parameters.at("lightCounts") != Json::array({0, 16, 17, 1024, 1025})))
			throw std::invalid_argument("Lighting fixture budget/count contract is invalid.");
		const std::vector<std::string> actions = dirty ? std::vector<std::string>{"stable-all-slots", "edit-retained-light-500",
			"edit-outside-retained-subset", "add-light", "remove-light", "reorder-lights", "shrink-light-count", "zero-lights"}
			: std::vector<std::string>{"zero", "fallback-16", "clustered-17", "capacity-1024", "over-cap-1025"};
		if (definition.at("steps").size() != actions.size()) throw std::invalid_argument("Lighting fixture step count drift.");
		LightingDirtyFixture result;
		result.scene = foundationScene;
		PrepareFoundation(result.scene.at("objects"));
		const std::uint32_t initialCount = dirty ? 1000 : 0;
		for (std::uint32_t i = 1; i <= PoolSize; ++i) result.scene["objects"].push_back(Light(i, i <= initialCount));
		result.steps = {{"schemaVersion", 1}, {"fixtureId", id}, {"fixtureHash", manifestHash}, {"seed", seed},
			{"stage", "P2"}, {"selectedStage", "P2"}, {"manifestStage", "P2"},
			{"fullManifestStepCount", actions.size()}, {"selectedManifestStepIds", Json::array()},
			{"excludedManifestStepIds", Json::array()}, {"resetBaselineBeforeStep", true},
			{"assets", Json::array()}, {"targets", {{"lightingPool", Order(PoolSize)}}}, {"steps", Json::array()},
			{"lightingPoolSize", PoolSize}, {"membershipMutation", "SetActive"},
			{"membershipMutationReason", "Add/remove changes real collected candidate membership; serialized pool lifetimes remain unchanged."}};
		const std::array<std::uint32_t, 5> thresholdCounts{0, 16, 17, 1024, 1025};
		for (std::size_t ordinal = 0; ordinal < actions.size(); ++ordinal)
		{
			const auto& frozen = definition["steps"][ordinal];
			std::ostringstream suffix; suffix << std::setfill('0') << std::setw(2) << ordinal;
			const std::string stepId = id + "-" + suffix.str();
			if (frozen.at("id") != stepId || frozen.at("action") != actions[ordinal] || frozen.at("ordinal") != ordinal
				|| frozen.at("warmupFrames") != 60 || frozen.at("measuredFrames") != 300)
				throw std::invalid_argument("Lighting frozen step identity/window drift.");
			std::uint32_t baselineCount = dirty ? 1000 : thresholdCounts[ordinal];
			std::uint32_t count = baselineCount, retainedBudget = dirty ? 1000 : 1024, omitted = 0;
			Json mutations = Json::array();
			if (dirty)
			{
				if (ordinal == 1) mutations.push_back(IntensityRequest(500, 0.5f));
				if (ordinal == 2) { retainedBudget = 999; mutations.push_back(IntensityRequest(1000, 0.5f)); }
				if (ordinal == 3) { count = 1001; mutations.push_back(ActiveRequest(1001, true)); }
				if (ordinal == 4) { count = 999; omitted = 500; mutations.push_back(ActiveRequest(500, false)); }
				if (ordinal == 6 || ordinal == 7)
				{
					count = ordinal == 6 ? 16 : 0;
					for (std::uint32_t i = count + 1; i <= baselineCount; ++i) mutations.push_back(ActiveRequest(i, false));
				}
			}
			Json baseline = Json::array();
			for (std::uint32_t i = 1; i <= PoolSize; ++i) baseline.push_back(ActiveRequest(i, i <= baselineCount));
			baseline.push_back(IntensityRequest(500, BaselineIntensity));
			baseline.push_back(IntensityRequest(1000, BaselineIntensity));
			const bool changed = dirty && ordinal != 0;
			Json step = frozen;
			step["nativePayload"] = {{"baselineTransactionRequests", baseline}, {"transactionRequests", mutations},
				{"baselineCandidateOrder", Order(baselineCount)},
				{"candidateOrder", Order(omitted ? baselineCount : count, dirty && ordinal == 5, omitted)},
				{"lightingBudgets", {{"maxUploadedLights", 1024}, {"maxDynamicLights", retainedBudget}, {"maxShadowLights", 0}}},
				{"expectedRecipe", {{"sourceCount", count}, {"analysisCount", std::min(count, 1024u)},
				{"retainedCount", std::min(count, retainedBudget)}, {"analysisMustChange", changed},
				{"finalMustChange", changed && ordinal != 2}, {"outsideRetainedOrdinal", ordinal == 2 && dirty ? Json(1000) : Json(nullptr)}}}};
			step["requiredCapabilities"] = {"TypedLightingBudgetControl", "TypedLightingCandidateOrder",
				"OwnedSceneTransactionReceipt", "CompletionQualifiedLightingBufferEvidence", "ActualPhysicalSlotCoverage"};
			step["requiredBundle"] = {{"views", Json::array({{{"viewType", "Scene"}, {"viewId", 1},
				{"lightingPurposes", {"FinalLights", "Analysis"}}}})}, {"requiredPhysicalSlotCount", 8}};
			step["observation"] = {{"requireSettingsGeneration", true}, {"requireMutationGeneration", true},
				{"requireActualCandidateIdentity", true}, {"requireActualSlotIdentity", true}};
			step["support"] = {{"status", "RequiresRuntimeCapability"},
				{"reason", "Recipe only; native completed writes, selection identities and physical slot visits must be observed."}};
			result.steps["selectedManifestStepIds"].push_back(stepId);
			result.steps["steps"].push_back(std::move(step));
		}
		result.files.emplace_back("renderer-efficiency-steps.json", result.steps);
		return result;
	}
}
