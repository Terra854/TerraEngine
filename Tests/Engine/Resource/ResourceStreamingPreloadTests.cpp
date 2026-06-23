#include "TestHarness.h"

#include "Resource/ResourceStreaming.h"

namespace TerraEngine::Tests {
namespace {
	TerraEngine::UUID MakeId(std::uint8_t value)
	{
		TerraEngine::UUID id{};
		id.fill(0);
		id[15] = value;
		return id;
	}
}

	void RegisterResourceStreamingPreloadTests(TestRegistry& registry)
	{
		registry.Add("Resource.Streaming.Preload.DisabledRulesDoNotFailValidation", [](TestContext& context) {
			Resource::PreloadRule rule;
			rule.enabled = false;
			rule.criteriaType = Resource::PreloadCriteriaType::Region;

			context.Expect(Resource::ValidatePreloadRule(rule), "Disabled preload rules should be ignored by validation and evaluation.");
		});

		registry.Add("Resource.Streaming.Preload.NonDistanceCriteriaRequireValues", [](TestContext& context) {
			Resource::PreloadRule rule;
			rule.ruleId = "tag-rule";
			rule.criteriaType = Resource::PreloadCriteriaType::AssetTag;
			rule.assetScope = { MakeId(1u) };

			std::string reason;
			context.Expect(!Resource::ValidatePreloadRule(rule, &reason), "Tag preload rules need a tag value.");
			rule.criteriaValue = "boss-arena";
			context.Expect(Resource::ValidatePreloadRule(rule, &reason), "Tag preload rules with a scope and tag should validate.");
		});

		registry.Add("Resource.Streaming.Preload.PolicyKeepsRuntimeExplicitProtected", [](TestContext& context) {
			const Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();

			context.Expect(Resource::IsProtectedNeed(Resource::StreamingNeedType::RuntimeExplicit, policy), "Explicit runtime preloads should protect assets from cleanup while active.");
			context.Expect(Resource::ProtectionForNeed(Resource::StreamingNeedType::DistancePreload) == Resource::StreamingProtectionReason::PreloadRule, "Distance preloads should explain themselves as preload protection.");
		});

		registry.Add("Resource.Streaming.Preload.RulesExpandIntoDeterministicNeeds", [](TestContext& context) {
			Resource::PreloadRule distanceRule;
			distanceRule.ruleId = "distance-near-player";
			distanceRule.criteriaType = Resource::PreloadCriteriaType::Distance;
			distanceRule.criteriaValue = "30";
			distanceRule.assetScope = { MakeId(2u), MakeId(1u) };
			distanceRule.priority = 25;
			distanceRule.graceSeconds = 3.0;
			distanceRule.diagnosticName = "near player";

			const std::vector<Resource::StreamingNeed> needs = Resource::EvaluatePreloadRules({ distanceRule }, 10.0);

			context.Expect(needs.size() == 2u, "A valid preload rule should emit one need per scoped asset.");
			context.Expect(needs[0].priority == 25, "Rule priority should flow into emitted needs.");
			context.Expect(needs[0].needType == Resource::StreamingNeedType::DistancePreload, "Distance criteria should emit distance-preload needs.");
			context.Expect(needs[0].expiresAtSeconds == 13.0, "Rule grace seconds should become a deterministic expiry time.");
			context.Expect(needs[0].reason == "near player", "Diagnostic names should be preserved as human-readable reasons.");
		});
	}
}
