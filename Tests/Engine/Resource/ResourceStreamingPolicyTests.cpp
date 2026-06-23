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
			policy.preloadDistance = -1.0;
			policy.visibleRetentionSeconds = -2.0;
			policy.neededRetentionSeconds = -3.0;
			policy.protectedNeedTypes.clear();

			policy = Resource::SanitizeStreamingPolicy(policy);

			context.Expect(policy.preloadDistance >= 0.0, "Preload distance must sanitize to a non-negative value.");
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
	}
}
