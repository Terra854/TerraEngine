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

	void RegisterResourceStreamingPolicyTests(TestRegistry& registry)
	{
		registry.Add("Resource.Streaming.Policy.SanitizesNegativeValues", [](TestContext& context) {
			Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();
			policy.streamingDistance = -1.0;
			policy.visibleRetentionSeconds = -2.0;
			policy.neededRetentionSeconds = -3.0;
			policy.protectedNeedTypes.clear();

			policy = Resource::SanitizeStreamingPolicy(policy);

			context.Expect(policy.streamingDistance >= 0.0, "Streaming distance must sanitize to a non-negative value.");
			context.Expect(policy.visibleRetentionSeconds >= 0.0, "Visible retention must sanitize to a non-negative value.");
			context.Expect(policy.neededRetentionSeconds >= 0.0, "Needed retention must sanitize to a non-negative value.");
			context.Expect(!policy.protectedNeedTypes.empty(), "Protected need types should never sanitize to an empty set.");
		});

		registry.Add("Resource.Streaming.Policy.StateTransitionsAreExplicit", [](TestContext& context) {
			context.Expect(Resource::StateAfterRegistration(true) == Resource::StreamablePayloadState::Deferred, "Deferred scene payload policy should register assets without loading heavy payloads.");
			context.Expect(Resource::StateAfterRegistration(false) == Resource::StreamablePayloadState::Registered, "Disabled deferral should preserve legacy registration behavior.");
			context.Expect(Resource::StateAfterLoadResult(false, false) == Resource::StreamablePayloadState::Failed, "Failed loads must stay distinguishable from deferred assets.");
			context.Expect(Resource::StateAfterLoadResult(true, true) == Resource::StreamablePayloadState::UploadingVram, "Render-facing loads should mark the asset as waiting for GPU upload.");
			context.Expect(Resource::StateAfterLoadResult(true, false) == Resource::StreamablePayloadState::RamResident, "CPU-only loads should become RAM resident.");
		});

		registry.Add("Resource.Streaming.Policy.MergesNeedsDeterministically", [](TestContext& context) {
			std::vector<Resource::StreamingNeed> needs;
			needs.push_back({ "late", MakeId(2u), Resource::StreamingNeedType::RulePreload, "", {}, 0.0, 1, "low priority" });
			needs.push_back({ "visible", MakeId(1u), Resource::StreamingNeedType::Visible, "scene", {}, 0.0, 100, "visible" });
			needs.push_back({ "visible", MakeId(1u), Resource::StreamingNeedType::Visible, "scene", {}, 0.0, 100, "visible duplicate" });
			needs.push_back({ "expired", MakeId(3u), Resource::StreamingNeedType::RuntimeExplicit, "", {}, 5.0, 200, "expired" });

			const std::vector<Resource::StreamingNeed> merged = Resource::MergeStreamingNeeds(std::move(needs), 10.0);

			context.ExpectEqual(std::size_t{2u}, merged.size(), "Duplicate and expired needs should be removed.");
			context.Expect(merged.front().assetId == MakeId(1u), "Higher-priority visible needs should sort first.");
			context.Expect(merged.back().assetId == MakeId(2u), "Lower-priority preload needs should remain after visible needs.");
		});

		registry.Add("Resource.Streaming.Policy.ValidatesPreloadRules", [](TestContext& context) {
			Resource::PreloadRule rule;
			rule.ruleId = "distance";
			rule.criteriaType = Resource::PreloadCriteriaType::Distance;
			rule.criteriaValue = "25.5";
			rule.assetScope = { MakeId(9u) };

			std::string reason;
			context.Expect(Resource::ValidatePreloadRule(rule, &reason), "A positive distance rule with an asset scope should validate.");
			rule.criteriaValue = "-1";
			context.Expect(!Resource::ValidatePreloadRule(rule, &reason), "Negative distance rules should be rejected.");
			context.Expect(!reason.empty(), "Invalid rules should explain what failed.");
		});

		registry.Add("Resource.Streaming.Policy.WorkerPlanningProducesCandidatesBeforeProtectedRegistration", [](TestContext& context) {
			std::vector<Resource::PreloadRule> rules;
			Resource::PreloadRule visibleRule;
			visibleRule.ruleId = "near-visible";
			visibleRule.criteriaType = Resource::PreloadCriteriaType::Distance;
			visibleRule.criteriaValue = "16";
			visibleRule.assetScope = { MakeId(7u), MakeId(8u) };
			visibleRule.priority = 100;
			visibleRule.diagnosticName = "worker candidate";
			rules.push_back(visibleRule);

			Resource::PreloadRule invalidRule = visibleRule;
			invalidRule.ruleId = "invalid-empty";
			invalidRule.assetScope = {};
			rules.push_back(invalidRule);

			const std::vector<Resource::StreamingNeed> candidates = Resource::EvaluatePreloadRules(rules, 12.0);
			TerraEngine::Scheduling::FrameWorkloadDeclaration declaration = Resource::BuildResourcePreparationWorkloadDeclaration(candidates.size());
			const TerraEngine::Scheduling::FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ declaration }, "resource-worker-planning");

			context.ExpectEqual(static_cast<std::size_t>(2), candidates.size(), "Worker planning should return only valid asset candidates.");
			context.Expect(plan.valid, "Resource candidate planning declaration should validate for worker preparation.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.workerSafeCount, "Resource candidate planning should be visible as worker-safe work.");
			context.Expect(declaration.protectedCommit.find("ResourceManager registration") != std::string::npos, "Resource registration should remain documented as a protected handoff.");
			for (const Resource::StreamingNeed& candidate : candidates)
			{
				context.Expect(candidate.needType == Resource::StreamingNeedType::DistancePreload, "Distance preload rules should produce deterministic worker candidates.");
				context.Expect(Resource::ProtectionForNeed(candidate.needType) == Resource::StreamingProtectionReason::PreloadRule, "Registration/loading handoff should retain the protected preload reason.");
			}
		});

		registry.Add("Resource.Streaming.Policy.DefaultAssetPoliciesMatchRuntimeCost", [](TestContext& context) {
			const Resource::StreamingAssetPolicy shaderPolicy = Resource::MakeDefaultStreamingAssetPolicy(Resource::StreamableAssetType::ShaderCollection);
			const Resource::StreamingAssetPolicy materialPolicy = Resource::MakeDefaultStreamingAssetPolicy(Resource::StreamableAssetType::Material);
			const Resource::StreamingAssetPolicy texturePolicy = Resource::MakeDefaultStreamingAssetPolicy(Resource::StreamableAssetType::Texture);

			context.Expect(shaderPolicy.priorityClass == Resource::StreamingPriorityClass::Critical, "Shader collections should default to critical priority because pipeline rebuilds depend on them.");
			context.Expect(shaderPolicy.residencyTier == Resource::StreamingResidencyTier::Persistent, "Shader collections should default to persistent residency until pipeline lifetime is more granular.");
			context.Expect(materialPolicy.priorityClass == Resource::StreamingPriorityClass::Important, "Materials should load before ordinary background resources because renderables depend on them.");
			context.Expect(materialPolicy.residencyTier == Resource::StreamingResidencyTier::Warm, "Materials should remain warm longer than fully streamed payloads.");
			context.Expect(texturePolicy.residencyTier == Resource::StreamingResidencyTier::Streamed, "Textures should remain streamable by default because they usually dominate memory.");
		});

			registry.Add("Resource.Streaming.Policy.TextureMipResidencyUsesSourceMipIndexRange", [](TestContext& context) {
				Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();
				context.Expect(policy.textureMipStreamingEnabled, "New projects should enable texture mip streaming by default.");
				context.ExpectEqual(300.0, policy.streamingDistance, "Default texture streaming distance should keep full-detail mips longer.");
				context.ExpectEqual(std::uint32_t{0u}, policy.textureMinStreamMip, "Default near texture residency should keep mip 0.");
				context.ExpectEqual(std::uint32_t{8u}, policy.textureMaxStreamMip, "Default far texture residency should avoid extremely tiny mips.");
				policy.textureMipStreamingEnabled = false;
				Resource::TextureMipResidency full = Resource::ResolveTextureMipResidency(policy, 8u);

				context.ExpectEqual(std::uint32_t{0u}, full.sourceMipOffset, "Mip streaming disabled should keep the original base mip.");
				context.ExpectEqual(std::uint32_t{8u}, full.residentMipCount, "Mip streaming disabled should keep the complete source chain.");
				context.Expect(!full.partial, "Full-res residency should not be marked as partial.");

				policy.textureMipStreamingEnabled = true;
				policy.textureMinStreamMip = 2u;
				policy.textureMaxStreamMip = 3u;
				const Resource::TextureMipResidency partial = Resource::ResolveTextureMipResidency(policy, 8u);

				context.ExpectEqual(std::uint32_t{2u}, partial.sourceMipOffset, "Partial residency should start at the configured minimum stream mip index.");
				context.ExpectEqual(std::uint32_t{6u}, partial.residentMipCount, "Resident mip count should include the selected source mip and every lower-detail tail mip.");
				context.ExpectEqual(std::uint32_t{8u}, partial.sourceMipCount, "Diagnostics should retain the authored mip count.");
				context.Expect(partial.partial, "Capped texture residency should be marked as partial.");
			});

			registry.Add("Resource.Streaming.Policy.VirtualGeometryPageBudgetsDefaultToExplicitOptIn", [](TestContext& context) {
				Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();
				policy.virtualGeometryPageMemoryBudgetBytes = 64ull * 1024ull * 1024ull;
				policy.virtualGeometryPageUploadBudgetBytes = 8ull * 1024ull * 1024ull;
				policy.virtualGeometryPageRetirementBudgetBytes = 16ull * 1024ull * 1024ull;

				const Resource::StreamingPolicy sanitized = Resource::SanitizeStreamingPolicy(policy);

				context.ExpectEqual(64ull * 1024ull * 1024ull, sanitized.virtualGeometryPageMemoryBudgetBytes, "VG page memory budgets should remain explicit streaming policy data.");
				context.ExpectEqual(8ull * 1024ull * 1024ull, sanitized.virtualGeometryPageUploadBudgetBytes, "VG page upload budgets should survive policy sanitization.");
				context.ExpectEqual(16ull * 1024ull * 1024ull, sanitized.virtualGeometryPageRetirementBudgetBytes, "VG page retirement budgets should survive policy sanitization.");
			});
		}
	}
