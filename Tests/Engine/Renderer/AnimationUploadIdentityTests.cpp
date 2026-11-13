#include "TestHarness.h"
#include <stdexcept>
#include <string_view>
#include <chrono>
#include <atomic>
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"
#include "Tests/Support/ReviewRemediation/ExistingRendererFixtureSession.h"
#include "Tests/Support/ReviewRemediation/AnimationUploadProductionProbe.h"
#include "Utilities/PlatformUtils.h"
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		// Retain each invocation separately. Exclusive directory creation plus a
		// monotonic time/counter suffix prevents concurrent/repeated evidence writes
		// from replacing an earlier native receipt; collisions retry without deletion.
		std::filesystem::path FreshEvidenceDirectory(const std::filesystem::path& parent, std::string_view label) {
			static std::atomic_uint64_t next{0};
			std::filesystem::create_directories(parent);
			for (unsigned attempt = 0; attempt < 100; ++attempt) {
				const auto path = parent / (std::string(label) + "-"
					+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
					+ std::to_string(next.fetch_add(1u, std::memory_order_relaxed)));
				if (path.parent_path() != parent) throw std::runtime_error("Evidence label must remain within its owned parent.");
				if (std::filesystem::create_directory(path)) return path;
			}
			throw std::runtime_error("Could not reserve a fresh evidence directory.");
		}

		TerraEngine::UUID UploadMesh(std::uint8_t value) { TerraEngine::UUID id{}; id[15] = value; return id; }
		Renderer::CpuAnimationDeformationPlan UploadFixture()
		{
			Renderer::CpuAnimationDeformationPlan plan; plan.frameId = plan.rendererGeneration = 1;
			for (unsigned index = 0; index < 3; ++index)
			{
				auto data = std::make_shared<Renderer::CpuAnimationDeformationData>();
				const bool contentA = index < 2;
				data->jointCount = 1; data->contentIdLow = contentA ? 11 : 22; data->contentIdHigh = contentA ? 33 : 44;
				data->paletteMatrices.resize(contentA ? 1 : 2);
				for (auto& palette : data->paletteMatrices) palette.row0[0] = contentA ? 7.0f : 19.0f;
				data->morphWeights = contentA ? std::vector<float>{0.25f} : std::vector<float>{0.5f, 0.75f};
				Renderer::CpuAnimationDeformationRecord record;
				record.stableInstanceId = index + 1; record.meshId = UploadMesh(1);
				record.poseGeneration = record.deformablePayloadGeneration = record.rendererGeneration = record.skeletonCompatibilityFingerprint = 1;
				record.data = std::move(data); plan.records.push_back(std::move(record));
			}
			return plan;
		}
	}
	void RegisterAnimationUploadIdentityTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Review.AnimationUpload.ActualUploadAndWorkOffsetsAgreeForAAB", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			const auto input = UploadFixture();
			const auto upload = BuildCpuAnimationUploadLayout(input);
			const auto work = BuildCpuAnimationDeformationOffsets(input);
			context.Expect(upload.valid && work.valid && upload.slices.size() == 2, "Both actual production preparation policies must execute.");
			// These byte arrays flatten the ACTUAL upload layout's accepted slices;
			// the test does not implement content deduplication or offset planning.
			std::vector<std::byte> palettes(upload.paletteBytes), morphs(upload.morphBytes);
			for (const auto& slice : upload.slices)
			{
				const auto& data = *input.records[slice.sourceRecordIndex].data;
				std::memcpy(palettes.data() + slice.paletteOffsetBytes, data.paletteMatrices.data(), data.paletteMatrices.size() * sizeof(Animation::GpuAnimationPaletteRecord));
				std::memcpy(morphs.data() + slice.morphOffsetBytes, data.morphWeights.data(), data.morphWeights.size() * sizeof(float));
			}
			for (std::size_t index = 0; index < input.records.size(); ++index)
			{
				const auto& record = input.records[index];
				const auto& slice = upload.slices.at(upload.recordSliceIndices.at(index));
				const auto& offsets = work.byInstanceMesh.at({record.stableInstanceId, record.meshId});
				context.Expect(offsets.paletteOffsetBytes == slice.paletteOffsetBytes && offsets.morphOffsetBytes == slice.morphOffsetBytes,
					"Deformation records must reference the same actual deduplicated slices as upload bridges.");
				const auto paletteBytes = record.data->paletteMatrices.size() * sizeof(Animation::GpuAnimationPaletteRecord);
				const auto morphBytes = record.data->morphWeights.size() * sizeof(float);
				context.Expect(offsets.paletteOffsetBytes <= palettes.size() && paletteBytes <= palettes.size() - offsets.paletteOffsetBytes
					&& offsets.morphOffsetBytes <= morphs.size() && morphBytes <= morphs.size() - offsets.morphOffsetBytes,
					"Every work interval must remain within actual uploaded bytes.");
				context.Expect(std::memcmp(palettes.data() + offsets.paletteOffsetBytes, record.data->paletteMatrices.data(), paletteBytes) == 0
					&& std::memcmp(morphs.data() + offsets.morphOffsetBytes, record.data->morphWeights.data(), morphBytes) == 0,
					"Actual addressed palette/morph values must belong to this instance's content.");
			}
		});
		registry.Add("Renderer.Review.AnimationUpload.OneAnimatorSupportsDistinctMeshPayloads", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			auto input = UploadFixture();
			// The first and third records share the animator, but have different
			// mesh-local palettes. The second still exercises content deduplication.
			input.records[2].stableInstanceId = input.records[0].stableInstanceId;
			input.records[2].meshId = UploadMesh(2);
			const auto work = BuildCpuAnimationDeformationOffsets(input);
			context.Expect(work.valid && work.byInstanceMesh.size() == 3, "One animator must admit multiple mesh owners.");
			const auto first = work.byInstanceMesh.at({1, UploadMesh(1)});
			const auto second = work.byInstanceMesh.at({2, UploadMesh(1)});
			const auto third = work.byInstanceMesh.at({1, UploadMesh(2)});
			context.Expect(first.paletteOffsetBytes == second.paletteOffsetBytes && first.morphOffsetBytes == second.morphOffsetBytes,
				"Identical content still shares actual upload bytes across instances.");
			context.Expect(first.paletteOffsetBytes != third.paletteOffsetBytes && first.morphOffsetBytes != third.morphOffsetBytes,
				"Different mesh-local content must address distinct actual slices.");
			input.records[2].meshId = UploadMesh(1);
			const auto rejected = BuildCpuAnimationDeformationOffsets(input);
			context.Expect(!rejected.valid && rejected.byInstanceMesh.empty(), "Conflicting payloads for one instance/mesh pair reject without partial offsets.");
		});
		registry.Add("Renderer.Review.AnimationUpload.MeshPageZeroOwnersAreDistinct", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			const std::vector<PersistentPhysicalPageUploadInput> inputs{{UploadMesh(1), 0, 1, 1, 256}, {UploadMesh(2), 0, 1, 1, 256}};
			PersistentPhysicalPageAllocationState allocator;
			auto plan = BuildPersistentPhysicalPageUploadPlan(inputs, 4096, 4096, 1, allocator);
			context.Expect(plan.valid && plan.uploads.size() == 2, "Both real mesh-page owners must be planned.");
			context.Expect(plan.uploads[0].allocationId != plan.uploads[1].allocationId,
				"Equal page numbers in distinct meshes must not share physical allocation IDs.");
			auto reuse = inputs; reuse[0].existingAllocationId = plan.uploads[0].allocationId; reuse[1].existingAllocationId = plan.uploads[1].allocationId;
			context.Expect(CommitPersistentPhysicalPageUploadPlan(plan, allocator), "The owning pool must atomically commit its accepted candidate.");
			const auto retained = BuildPersistentPhysicalPageUploadPlan(reuse, 4096, 4096, 2, allocator);
			context.Expect(retained.valid && retained.reusedPages == 2, "Later planning must retain both independent existing owners.");
			VirtualGeometryPageUploadIntent first{}; first.meshId = UploadMesh(1); first.pageId = 0; first.contentGeneration = 1;
			// Complete typed intent evidence lets this assertion reach retirement
			// identity validation instead of stopping at an unrelated missing hash.
			first.byteSize = 256; first.sourceHash = 1; first.existingAllocationId = retained.uploads[0].allocationId;
			const auto intent = BuildVirtualGeometryUploadIntentPlan({first}, {retained.uploads[1].allocationId});
			context.Expect(intent.valid && intent.descriptorRefreshAllocations.size() == 1 && intent.retirementAllocations.size() == 1
				&& intent.descriptorRefreshAllocations[0] != intent.retirementAllocations[0], "Retiring one mesh must not retire the other's descriptor owner.");
		});
		registry.Add("Renderer.Review.AnimationUpload.HighGenerationDoesNotAliasNewOwners", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			const std::vector<PersistentPhysicalPageUploadInput> input{{UploadMesh(3), 0, 1, 1, 256}};
			PersistentPhysicalPageAllocationState allocator;
			auto low = BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 1, allocator);
			context.Expect(low.valid, "The ordinary generation control must execute.");
			const auto firstId = low.uploads.at(0).allocationId;
			context.Expect(CommitPersistentPhysicalPageUploadPlan(low, allocator), "Publish the first exact owner.");
			const auto generation = (std::uint64_t{1} << 32u) + 1;
			const auto high = BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, generation, allocator);
			context.Expect(high.valid && high.uploads.at(0).allocationId != firstId
				&& high.candidateAllocator.owners.at(high.uploads.at(0).allocationId).allocationGeneration == generation,
				"High generations must retain all 64 bits without aliasing any prior allocation.");
		});
		registry.Add("Renderer.Review.AnimationUpload.DuplicatePagesRejectAtomicallyAcrossPriorityGroups", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			PersistentPhysicalPageAllocationState allocator;
			PersistentPhysicalPageUploadInput first{UploadMesh(4), 0, 1, 1, 256};
			auto initial = BuildPersistentPhysicalPageUploadPlan({first}, 4096, 4096, 1, allocator);
			context.Expect(CommitPersistentPhysicalPageUploadPlan(initial, allocator), "Establish one genuine live allocation before duplicate rejection.");
			const auto owners = allocator.owners;
			const auto revision = allocator.revision, next = allocator.nextAllocationId;
			for (unsigned variant = 0; variant != 4; ++variant) {
				auto duplicate = first;
				PersistentPhysicalPageUploadInput separator{UploadMesh(5), 0, 1, 1, 256};
				if (variant == 1) ++duplicate.contentGeneration;
				if (variant == 2) duplicate.pinnedFallback = separator.pinnedFallback = true;
				if (variant == 3) first.existingAllocationId = initial.uploads.at(0).allocationId;
				// Another page separates duplicate identities after priority sorting;
				// one reused plus one new record must also reject as one transaction.
				auto refused = BuildPersistentPhysicalPageUploadPlan({first, separator, duplicate}, 4096, 4096, 2, allocator);
				context.Expect(!refused.valid && !CommitPersistentPhysicalPageUploadPlan(refused, allocator), "Repeated mesh/page identity must reject regardless of generation, priority or reuse.");
				context.Expect(allocator.owners == owners && allocator.revision == revision && allocator.nextAllocationId == next, "Rejected planning cannot publish a prefix or consume live IDs.");
			}
			context.Expect(BuildPersistentPhysicalPageUploadPlan({first, {UploadMesh(5), 0, 1, 1, 256}}, 4096, 4096, 2, allocator).valid, "Distinct mesh/page owners remain a valid control.");
		});
		registry.Add("Renderer.Review.AnimationUpload.AllocatorRejectsStaleAndPartialCandidates", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			PersistentPhysicalPageAllocationState allocator;
			std::vector<PersistentPhysicalPageUploadInput> input{{UploadMesh(4), 0, 1, 1, 256}};
			auto first = BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 1, allocator);
			auto stale = BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 2, allocator);
			context.Expect(allocator.owners.empty() && allocator.nextAllocationId == 1, "Worker preparation cannot publish live IDs.");
			context.Expect(CommitPersistentPhysicalPageUploadPlan(first, allocator), "First parallel candidate must commit.");
			const auto owners = allocator.owners; const auto revision = allocator.revision, next = allocator.nextAllocationId;
			context.Expect(!CommitPersistentPhysicalPageUploadPlan(stale, allocator), "Stale parallel candidate must reject normally.");
			input.push_back({UploadMesh(5), 0, 1, 1, 256});
			auto refused = BuildPersistentPhysicalPageUploadPlan(input, 256, 256, 3, allocator);
			context.Expect(!refused.valid && !CommitPersistentPhysicalPageUploadPlan(refused, allocator), "Budget rejection after staged work cannot commit a prefix.");
			input.resize(1); input[0].existingAllocationId = owners.begin()->first; input[0].meshId = UploadMesh(6);
			context.Expect(!BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 4, allocator).valid, "A recorded ID cannot be borrowed by another mesh.");
			input[0].existingAllocationId = 999;
			context.Expect(!BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 4, allocator).valid, "Unknown external IDs have no ownership evidence.");
			context.Expect(allocator.owners == owners && allocator.revision == revision && allocator.nextAllocationId == next,
				"Every rejection must preserve the exact owner map, revision and counter.");
			context.Expect(!BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 4).valid, "Legacy stateless API must fail closed.");
		});
		registry.Add("Renderer.Review.AnimationUpload.RetirementAndExhaustionNeverReuseIds", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			PersistentPhysicalPageAllocationState allocator;
			std::vector<PersistentPhysicalPageUploadInput> input{{UploadMesh(7), 0, 1, 1, 256, 0, 10}};
			auto issued = BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 1, allocator);
			const std::array<std::uint64_t, 1> retired{issued.uploads.at(0).allocationId};
			context.Expect(CommitPersistentPhysicalPageUploadPlan(issued, allocator), "Publish the pending-fence owner.");
			input[0].existingAllocationId = retired[0]; input[0].completionFence = 5;
			auto reuse = BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 2, allocator);
			const auto previousRevision = allocator.revision;
			context.Expect(CommitPersistentPhysicalPageUploadPlan(reuse, allocator)
				&& allocator.revision == previousRevision + 1 && allocator.owners.at(retired[0]).lastUseFence == 10,
				"Reused fence evidence advances revision without overwriting a newer submitted last use.");
			input[0].existingAllocationId = 0;
			context.Expect(!BuildPersistentPhysicalPageUploadPlan({}, 4096, 4096, 2, allocator, retired, 9).valid, "Incomplete fences cannot retire ownership.");
			auto retirement = BuildPersistentPhysicalPageUploadPlan({}, 4096, 4096, 2, allocator, retired, 10);
			context.Expect(CommitPersistentPhysicalPageUploadPlan(retirement, allocator) && allocator.owners.empty(), "Completed retirement removes bounded owner records.");
			auto replacement = BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 3, allocator);
			context.Expect(replacement.valid && replacement.uploads.at(0).allocationId > retired[0], "Retirement cannot rewind an issued lifetime ID.");
			PersistentPhysicalPageAllocationState nearLimit; nearLimit.nextAllocationId = UINT64_MAX;
			input.push_back({UploadMesh(8), 0, 1, 1, 256});
			auto exhausted = BuildPersistentPhysicalPageUploadPlan(input, 4096, 4096, 4, nearLimit);
			context.Expect(!exhausted.valid && !CommitPersistentPhysicalPageUploadPlan(exhausted, nearLimit)
				&& nearLimit.nextAllocationId == UINT64_MAX && nearLimit.owners.empty(), "Exhaustion after one staged ID rejects atomically.");
		});
		if (Review::VulkanReviewFixture::IsNativeReviewEnabled())
			registry.Add("Native.ReviewSimulation.AnimationUpload.ProductionAabPublishAndWorkRecords", [](TestContext& context) {
				using namespace Review;
				const std::string name="Native.ReviewSimulation.AnimationUpload.ProductionAabPublishAndWorkRecords";
				if (!VulkanReviewFixture::IsWatchedChild(name))
				{
					const auto child=VulkanReviewFixture::RunWatchedSelf(name,std::chrono::seconds(120));
					RecordNativeReviewChild(name,child);
					context.Expect(child.started && !child.timedOut && child.exitCode==0,"Actual production A/A/B child must settle: "+child.output); return;
				}
				ExistingRendererFixtureSession session(name);
				// Resolve the installed coherent baker output beside this executable.
				// The actual public loader, not synthetic page bytes, owns the source.
				const auto cohort=std::filesystem::path(PlatformUtils::GetExecutablePath()).parent_path();
				const auto base=cohort/"TerraEngineProductionShapeFixture";
				std::uint64_t fingerprint=0; std::uint32_t joints=0; std::string key,ignored;
				std::ifstream identity(base.string()+".identity");
				while (identity>>key)
				{
					if(key=="skeletonCompatibilityFingerprint") identity>>fingerprint;
					else if(key=="sourceSkeletonJointCount") identity>>joints;
					else if(key=="skeleton" || key=="clip") identity>>ignored;
					else throw TestFailure("Unknown installed coherent fixture identity field.");
				}
				context.Expect(fingerprint && joints==1 && std::filesystem::is_regular_file(base.string()+".manifest"),"Installed coherent mesh identity/manifest is required.");
				auto& resources=ResourceManager::Instance();
				const auto mesh=resources.LoadDeformableMesh(base.string()+".temf",joints,fingerprint);
				const auto resident=resources.AcquireMeshSnapshot(mesh);
				context.Expect(mesh!=EMPTY_UUID && resident && resident->hasDeformableDemandedPages
					&& !resident->deformableDemandedPageEntries.empty(),"Actual loaded demanded-page owner must exist.");
				auto cpu=std::make_shared<Renderer::CpuAnimationDeformationPlan>(UploadFixture());
				std::vector<Renderer::DeformationPageDemand> demands;
				const auto& page=resident->deformableDemandedPageEntries.front();
				const auto& sections=resident->deformableDemandedPageSections.front();
				std::uint32_t vertices=0,meshlets=0;
				for(const auto& section:sections)
				{
					if(section.kind==TemfDeformablePageSectionKind::PackedVertices) vertices=section.elementCount;
					if(section.kind==TemfDeformablePageSectionKind::Meshlets) meshlets=section.elementCount;
				}
				context.Expect(vertices && meshlets,"Actual production page must contain vertices and meshlets.");
				for(std::size_t index=0; index<cpu->records.size(); ++index)
				{
					auto& record=cpu->records[index]; record.meshId=mesh;
					record.deformablePayloadGeneration=resident->deformablePayloadGeneration; record.skeletonCompatibilityFingerprint=fingerprint;
					auto data=std::make_shared<Renderer::CpuAnimationDeformationData>(*record.data);
					for(std::size_t matrix=0; matrix<data->paletteMatrices.size(); ++matrix)
					{
						auto& palette=data->paletteMatrices[matrix]; const float shift=matrix?9.0F:(index<2?1.0F:4.0F);
						palette.row0={1,0,0,shift}; palette.row1={0,1,0,shift+1}; palette.row2={0,0,1,shift+2};
					}
					record.data=data;
					Renderer::DeformationPageDemand demand;
					demand.key.page={mesh,resident->deformablePayloadGeneration,page.pageId,page.contentGeneration,page.contentGeneration,
						Renderer::kAnimatedGeometryAbiVersion,1,1};
					demand.key.deformation={record.stableInstanceId,1,record.poseGeneration,fingerprint,1,resident->deformablePayloadGeneration,
						cpu->rendererGeneration,1,data->contentIdLow,data->contentIdHigh};
					demand.key.inputLayoutVersion=demand.key.animationLodVersion=Renderer::kAnimatedGeometryAbiVersion;
					demand.ownerStableInstanceId=demand.sourceStableInstanceId=record.stableInstanceId; demand.sourceRecordIndex=static_cast<std::uint32_t>(index);
					demand.vertexCount=vertices; demand.meshletCount=meshlets; demand.clusterCount=1; demand.requestedViewMask=1;
					demands.push_back(demand);
				}
				Renderer::RenderBackendWorkPublication publication;
				publication.frameId=cpu->frameId; publication.valid=true;
				publication.animationWork.frameId=cpu->frameId; publication.animationWork.rendererGeneration=static_cast<std::uint32_t>(cpu->rendererGeneration);
				publication.animationWork.cpuDeformationPlan=cpu;
				publication.deformationWork=std::make_shared<Renderer::GpuDeformationPlan>(Renderer::BuildGpuDeformationPlan(cpu->frameId,cpu->rendererGeneration,demands));
				context.Expect(cpu->IsComplete() && publication.deformationWork->IsComplete(),"Actual public planning must seal the coherent three-consumer plan.");
				const auto observed=ExerciseAnimationUploadProduction(session.Native(),publication,session.Artifacts());
				context.Expect(observed.published && observed.recorded && observed.completed && observed.workCount==2
					&& observed.uploadAndWorkValuesMatch && observed.deformedValuesMatch,"Actual production A/A/B publication/work/GPU values must agree.");
				session.CloseRenderer(); context.Expect(session.ValidationErrors()==0,"Actual production upload/deformation episode must remain validation clean.");
			});
		if (Review::VulkanReviewFixture::IsNativeReviewEnabled())
			registry.Add("Native.ReviewSimulation.AnimationUpload.PublishedAabByteRanges", [](TestContext& context) {
				using Review::VulkanReviewFixture;
				const std::string name = "Native.ReviewSimulation.AnimationUpload.PublishedAabByteRanges";
				if (!VulkanReviewFixture::IsWatchedChild(name))
				{
					const auto child = VulkanReviewFixture::RunWatchedSelf(name);
					context.Expect(child.started && !child.timedOut && child.exitCode == 0, "Actual native A/A/B byte-range qualification must complete: " + child.output); return;
				}
				auto options = VulkanReviewFixture::NativeDeviceOptions();
				VulkanReviewFixture fixture(name, options);
				auto input = UploadFixture();
				// Real fenced byte addressing must also work when one animator owns
				// different mesh-local palettes; A/A still exercises content sharing.
				input.records[2].stableInstanceId = input.records[0].stableInstanceId;
				input.records[2].meshId = UploadMesh(2);
				const auto upload = Renderer::Vulkan::BuildCpuAnimationUploadLayout(input);
				const auto work = Renderer::Vulkan::BuildCpuAnimationDeformationOffsets(input);
				context.Expect(upload.valid && work.valid, "The same two production-bound planning policies must admit A/A/B.");
				std::vector<std::byte> palettes(upload.paletteBytes), morphs(upload.morphBytes);
				for (const auto& slice : upload.slices)
				{
					const auto& data = *input.records[slice.sourceRecordIndex].data;
					std::memcpy(palettes.data() + slice.paletteOffsetBytes, data.paletteMatrices.data(), data.paletteMatrices.size() * sizeof(Animation::GpuAnimationPaletteRecord));
					std::memcpy(morphs.data() + slice.morphOffsetBytes, data.morphWeights.data(), data.morphWeights.size() * sizeof(float));
				}
				const auto paletteUpload = fixture.CreateBuffer(palettes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, palettes);
				const auto morphUpload = fixture.CreateBuffer(morphs.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, morphs);
				const auto paletteDevice = fixture.CreateBuffer(palettes.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
				const auto morphDevice = fixture.CreateBuffer(morphs.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
				const auto proof = fixture.Submit([&](VkCommandBuffer command, const auto& native) {
					const VkBufferCopy paletteCopy{0, 0, palettes.size()}, morphCopy{0, 0, morphs.size()};
					native.functions->vkCmdCopyBuffer(command, paletteUpload.handle, paletteDevice.handle, 1, &paletteCopy);
					native.functions->vkCmdCopyBuffer(command, morphUpload.handle, morphDevice.handle, 1, &morphCopy);
				});
				context.Expect(proof.completed, "Actual upload copies must complete before observing work-addressed bytes.");
				const auto paletteBytes = fixture.Read(paletteDevice), morphBytes = fixture.Read(morphDevice);
				nlohmann::json observations = nlohmann::json::array(); bool correct = true;
				for (const auto& record : input.records)
				{
					const auto offset = work.byInstanceMesh.at({record.stableInstanceId, record.meshId});
					const auto paletteSize = record.data->paletteMatrices.size() * sizeof(Animation::GpuAnimationPaletteRecord), morphSize = record.data->morphWeights.size() * sizeof(float);
					const bool bounded = offset.paletteOffsetBytes <= paletteBytes.size() && paletteSize <= paletteBytes.size() - offset.paletteOffsetBytes
						&& offset.morphOffsetBytes <= morphBytes.size() && morphSize <= morphBytes.size() - offset.morphOffsetBytes;
					const bool values = bounded && std::memcmp(paletteBytes.data() + offset.paletteOffsetBytes, record.data->paletteMatrices.data(), paletteSize) == 0
						&& std::memcmp(morphBytes.data() + offset.morphOffsetBytes, record.data->morphWeights.data(), morphSize) == 0;
					correct &= bounded && values;
					observations.push_back({{"instance", record.stableInstanceId}, {"paletteOffset", offset.paletteOffsetBytes}, {"morphOffset", offset.morphOffsetBytes}, {"bounded", bounded}, {"valuesMatch", values}});
				}
				const auto output = FreshEvidenceDirectory(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/review-remediation/animation-upload-identities", "shared-content-ranges");
				std::ofstream(output / "ranges.json") << nlohmann::json{{"scope", "ProductionPlansAndActualFencedUploadBytes_NotDeformationDispatch"}, {"records", observations}}.dump(2);
				{ std::ofstream raw(output / "palettes.bin", std::ios::binary); raw.write(reinterpret_cast<const char*>(paletteBytes.data()), paletteBytes.size()); }
				{ std::ofstream raw(output / "morphs.bin", std::ios::binary); raw.write(reinterpret_cast<const char*>(morphBytes.data()), morphBytes.size()); }
				try { fixture.Finish(); }
				catch (...) { fixture.WriteEvidence(output / "native.json"); throw; }
				fixture.WriteEvidence(output / "native.json");
				// Invalid ranges are rejected BEFORE any deformation dispatch. Native
				// upload bytes are observed, but this case never claims shader output.
				context.Expect(correct, "Every A/A/B work record must address its own accepted uploaded palette and morph bytes.");
			});
	}
}
