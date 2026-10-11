// ShadowPagePreparationTests.cpp — complete light-qualified authored page cuts.
#include "TestHarness.h"
#include "Renderer/ShadowWorkPlan.h"
#include "Renderer/RHI/Vulkan/VulkanShadowPagePreparation.h"
#include "File Formats/TerraEngineMeshFile.h"

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::Vulkan;
		std::shared_ptr<Mesh> PageMesh()
		{
			// Two independent cooked pages each cover two owner meshlets. The
			// record joins reflect the real cooker group/cluster/page contract.
			auto mesh = std::make_shared<Mesh>(); mesh->hasVirtualGeometry = true; mesh->publicationGeneration = 3U;
			mesh->meshletRecords.resize(4U); mesh->meshletBounds.resize(4U);
			for (std::uint32_t index = 0U; index < 4U; ++index) {
				MeshletRecord record{}; record.vertexOffset = index * 3U; record.vertexCount = 3U;
				record.primitiveIndexOffset = index * 3U; record.primitiveIndexCount = 3U;
				mesh->meshletRecords[index] = record;
				VirtualGeometryCullableGroupRecord group{};
				group.groupIndex = index; group.pageId = 10U + index / 2U;
				group.vertexOffset = record.vertexOffset; group.vertexCount = record.vertexCount;
				group.primitiveOffset = record.primitiveIndexOffset; group.primitiveCount = 1U;
				mesh->virtualGeometryCullableGroups.push_back(group);
			}
			for (std::uint32_t index = 0U; index < 2U; ++index) {
				VirtualGeometryClusterRecord cluster{}; cluster.clusterId = index + 20U;
				cluster.pageId = index + 10U; cluster.groupOffset = index * 2U; cluster.groupCount = 2U;
				mesh->virtualGeometryClusters.push_back(cluster);
				VirtualGeometryClusterPageLinkRecord link{};
				link.clusterId = cluster.clusterId; link.pageId = cluster.pageId;
				link.firstGroup = cluster.groupOffset; link.groupCount = cluster.groupCount;
				mesh->virtualGeometryClusterPageLinks.push_back(link);
				VirtualGeometryPageRecord page{}; page.pageId = cluster.pageId;
				page.groupOffset = cluster.groupOffset; page.groupCount = cluster.groupCount;
				page.lodCount = 1U; page.fallbackPageId = page.pageId;
				page.clusterPageLinkOffset = index; page.clusterPageLinkCount = 1U;
				mesh->virtualGeometryPages.push_back(page);
			}
			return mesh;
		}
		ShadowPageCutRequest Request(const std::shared_ptr<Mesh>& mesh)
		{
			ShadowPageCutRequest request; request.meshId[0] = 1U; request.mesh = mesh;
			request.cpuPublicationGeneration = 3U; request.ownerMeshletCount = 4U; return request;
		}
	}
	void RegisterShadowPagePreparationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.ShadowPage.NativeBindingByteExtentSurvivesSemanticNormalization", [](TestContext& context) {
			// Start from a complete immutable owner stamp, then apply the same
			// page binding mapping that native command preparation consumes.
			Renderer::ShadowCasterStamp owner; owner.objectId[0] = 1; owner.meshId[0] = 2; owner.assetId = owner.meshId;
			owner.lifetimeGeneration = 7; owner.meshRevision = 3; owner.assetRevision = 3;
			owner.meshletCount = 4; owner.evidenceMask = Renderer::kCompleteShadowCasterEvidence;
			PreparedShadowPageBinding binding; binding.segment = {10, 10, 20, 0, 2, 0};
			binding.descriptorIndex = 0; binding.payloadBufferIndex = 5;
			binding.cpuPublicationGeneration = 3; binding.payloadAllocationGeneration = 8;
			binding.payloadByteSize = 1024; // Actual full shared native buffer extent supplied by its owner.
			auto mapped = owner;
			context.Expect(ApplyShadowAuthoredPageBinding(binding, mapped), "A matching published and allocated page segment must map.");
			context.Expect(mapped.payloadByteOffset == 0 && mapped.payloadByteSize == 1024 && mapped.payloadSource == 0x100,
				"Semantic proof must retain the actual full native buffer extent and authored-local source.");
			context.Expect(mapped.meshRevision == 3 && mapped.pageContentRevision == 3 && mapped.payloadContentRevision == 3
				&& mapped.residencyGeneration == 8 && mapped.firstMeshlet == 0 && mapped.meshletCount == 2 && mapped.firstCluster == 20,
				"The byte proof must retain actual publication, allocation and selected segment identities.");
			std::vector<Renderer::ShadowCasterStampInput> inputs{{mapped, {}}};
			const auto membership = Renderer::NormalizeShadowCasterMembership(inputs);
			context.Expect(membership.reusable && membership.rows.size() == 1 && membership.rows.front().payloadByteSize == 1024,
				"Actual semantic normalization must preserve the truthful nonzero byte proof.");
			binding.segment.fallbackPageId = UINT32_MAX;
			auto noFallback = owner; context.Expect(ApplyShadowAuthoredPageBinding(binding, noFallback) && noFallback.fallbackPageId == UINT32_MAX,
				"A valid authored page with no fallback preserves its existing sentinel semantics.");
			binding.segment.fallbackPageId = 10;
			auto repeated = owner; context.Expect(ApplyShadowAuthoredPageBinding(binding, repeated) && Renderer::ExactShadowCasterStampEqual(mapped, repeated),
				"An identical native allocation and segment must preserve exact reusable proof.");
		});
		registry.Add("Renderer.Efficiency.ShadowPage.InvalidNativeBindingPreservesWholeOwnerFallback", [](TestContext& context) {
			Renderer::ShadowCasterStamp owner; owner.objectId[0] = 1; owner.meshId[0] = 2; owner.assetId = owner.meshId;
			owner.lifetimeGeneration = 7; owner.meshRevision = 3; owner.assetRevision = 3; owner.meshletCount = 4;
			owner.evidenceMask = Renderer::kCompleteShadowCasterEvidence;
			PreparedShadowPageBinding valid; valid.segment = {10, 10, 20, 0, 2, 0};
			valid.descriptorIndex = 0; valid.payloadBufferIndex = 5;
			valid.cpuPublicationGeneration = 3; valid.payloadAllocationGeneration = 8; valid.payloadByteSize = 1024;
			for(unsigned failure = 0; failure < 12; ++failure) {
				auto invalid = valid; auto preserved = owner;
				switch(failure) {
				case 0: invalid.payloadByteSize = 0; break;
				case 1: invalid.payloadByteOffset = UINT64_MAX; invalid.payloadByteSize = 16; break;
				case 2: invalid.payloadBufferIndex = UINT32_MAX; break;
				case 3: invalid.descriptorIndex = UINT32_MAX; break;
				case 4: invalid.cpuPublicationGeneration = 4; break;
				case 5: invalid.payloadAllocationGeneration = 0; break;
				case 6: invalid.segment.ownerMeshletOffset = 3; break;
				case 7: invalid.segment.ownerMeshletCount = 0; break;
				case 8: invalid.segment.selectedLod = 1; break;
				case 9: invalid.source = static_cast<ShadowPagePayloadSource>(255); break;
				case 10: preserved.assetRevision = 4; break;
				case 11: preserved.meshId = EMPTY_UUID; break;
				}
				const auto unchanged = preserved;
				context.Expect(!ApplyShadowAuthoredPageBinding(invalid, preserved) && preserved == unchanged,
					"Invalid native page proof must fail without replacing any complete owner fallback fields.");
			}
		});
		registry.Add("Renderer.Efficiency.ShadowPage.CompleteCoverage", [](TestContext& context) {
			const auto cut = BuildShadowAuthoredPageCut(Request(PageMesh()));
			context.Expect(cut.complete && cut.segments.size() == 2U, "Both authored page segments must cover the complete owner span.");
			if (cut.segments.size() == 2U) context.Expect(cut.segments[1].ownerMeshletOffset == 2U && cut.segments[1].pageId == 11U, "Cut must preserve actual page/range identity.");
		});
		registry.Add("Renderer.Efficiency.ShadowPage.GapFallsBackAtomically", [](TestContext& context) {
			auto mesh = PageMesh(); mesh->virtualGeometryClusters.pop_back();
			const auto cut = BuildShadowAuthoredPageCut(Request(mesh));
			context.Expect(!cut.complete && cut.segments.empty(), "Missing page coverage must reject every partial segment.");
		});
		registry.Add("Renderer.Efficiency.ShadowPage.OverlapFallsBackAtomically", [](TestContext& context) {
			auto mesh = PageMesh(); mesh->virtualGeometryClusters.push_back(mesh->virtualGeometryClusters.front());
			const auto cut = BuildShadowAuthoredPageCut(Request(mesh));
			context.Expect(!cut.complete && cut.segments.empty(), "Overlapping or duplicate coverage must reject the entire cut.");
		});
		registry.Add("Renderer.Efficiency.ShadowPage.InvalidRangeAndPageJoin", [](TestContext& context) {
			auto mesh = PageMesh(); auto request = Request(mesh); request.ownerMeshletOffset = 3U;
			context.Expect(!BuildShadowAuthoredPageCut(request).complete, "Out-of-range owner selection must fall back.");
			request = Request(mesh); mesh->virtualGeometryClusterPageLinks[1].pageId = 77U;
			context.Expect(!BuildShadowAuthoredPageCut(request).complete, "A page must prove its actual cluster join.");
		});
		registry.Add("Renderer.Efficiency.ShadowPage.UnsupportedInputsFallback", [](TestContext& context) {
			auto request = Request(PageMesh()); request.deformationUnsupported = true;
			context.Expect(!BuildShadowAuthoredPageCut(request).complete, "Unsupported deformation cannot use authored bind-pose page proof.");
			request.deformationUnsupported = false; request.materialUnsupported = true;
			context.Expect(!BuildShadowAuthoredPageCut(request).complete, "Unsupported material must preserve complete owner fallback.");
		});
		registry.Add("Renderer.Efficiency.ShadowPage.ActualPublicationRequired", [](TestContext& context) {
			auto mesh = PageMesh(); auto request = Request(mesh); request.cpuPublicationGeneration = 9U;
			context.Expect(!BuildShadowAuthoredPageCut(request).complete, "A caller-provided counter cannot replace the retained payload publication.");
			request.cpuPublicationGeneration = 3U; mesh->publicationGeneration = 0U;
			context.Expect(!BuildShadowAuthoredPageCut(request).complete, "An unpublished mesh cannot authorize page payload proof.");
		});
		registry.Add("Renderer.Efficiency.ShadowPage.OffCameraCasterAndStableIdentity", [](TestContext& context) {
			// This light-selected mesh has no camera packet/table. Its accepted
			// authored coverage alone must produce a stable independent cut.
			auto request = Request(PageMesh()); const auto first = BuildShadowAuthoredPageCut(request);
			const auto second = BuildShadowAuthoredPageCut(request);
			context.Expect(first.complete && second.complete && first.segments == second.segments,
				"Off-camera light casters must have the same complete stable authored page cut.");
		});
	}
}
