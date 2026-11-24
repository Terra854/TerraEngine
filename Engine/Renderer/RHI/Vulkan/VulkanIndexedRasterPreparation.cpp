// VulkanIndexedRasterPreparation.cpp - Whole-view indexed resource qualification.
//
// Scheduler ownership: immutable Mesh topology is prepared by ThreadPool jobs.
// The protected Vulkan view boundary reads sealed commands and accepted resource
// owners, then writes only optional index uploads, descriptor caches and exact
// indexed pipeline variants. Effective settings/recording remain with the caller.
#include "VulkanResourceSource.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <future>

struct VulkanContext::Impl::IndexedRasterPreparationState
{
	struct Result
	{
		std::shared_ptr<const TerraEngine::Renderer::Vulkan::IndexedCornerGeometry> topology;
		std::string reason;
	};
	struct Job
	{
		std::shared_ptr<const Mesh> owner;
		std::future<Result> future;
		Result result;
	};
	struct Configuration
	{
		std::shared_ptr<const ShaderCollection> defaultOwner;
		VulkanRasterPipelineConfiguration raster;
	};
	// Completed entries disappear after successful GPU attachment. The actual
	// VulkanMesh then owns the topology. Outstanding immutable jobs are bounded
	// even when callers switch views or a CPU resource is repeatedly recooked.
	static constexpr std::size_t kMaximumJobs = 32;
	static constexpr std::size_t kMaximumUploadsPerCall = 8;
	std::unordered_map<TerraEngine::UUID, Job> jobs;
	std::vector<Configuration> configurations;
};

namespace
{
	using IndexedGeometry = TerraEngine::Renderer::Vulkan::IndexedCornerGeometry;
	struct IndexedMeshRequest
	{
		TerraEngine::UUID id{};
		std::shared_ptr<const Mesh> owner;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
		std::vector<std::uint32_t> materialFlags;
	};
	struct IndexedMaterialRequest
	{
		TerraEngine::UUID id{};
		std::shared_ptr<const Material> owner;
	};

	bool IndexedCpuPoseMatches(const TerraEngine::Renderer::RenderFramePublication& publication,
		const RenderItemRecord& command, const Mesh& mesh)
	{
		using namespace TerraEngine::Renderer;
		const auto& binding = command.animationBinding;
		if (!binding.HasCpuDeformation())
			return binding.source == AnimationRenderPoseSource::BindPose;
		if (!publication.backendWork || !publication.backendWork->valid
			|| publication.backendWork->frameId != publication.frameId
			|| publication.backendWork->animationWork.frameId != publication.frameId
			|| !publication.backendWork->animationWork.valid) return false;
		const auto& plan = publication.backendWork->animationWork.cpuDeformationPlan;
		if (!plan || !plan->IsComplete() || plan->frameId != publication.frameId
			|| (publication.backendWork->animationWork.rendererGeneration != 0
				&& plan->rendererGeneration != publication.backendWork->animationWork.rendererGeneration)
			// Both roots are authored from the same protected RHI recovery session
			// by CaptureFrameInputs/BuildFrameCpuDeformationPlan and frame sealing.
			|| plan->rendererGeneration != publication.rendererSessionGeneration
			|| binding.cpuDeformationRecordIndex >= plan->records.size()
			|| !mesh.hasDeformablePayload || mesh.deformablePayloadGeneration != binding.deformablePayloadGeneration
			|| mesh.deformableMetadata.skeletonCompatibilityFingerprint != binding.skeletonCompatibilityFingerprint) return false;
		const auto& record = plan->records[binding.cpuDeformationRecordIndex];
		return record.IsComplete() && record.meshId == command.meshId
			&& record.stableInstanceId == command.animation.stableInstanceId
			&& record.poseGeneration == binding.posePublicationGeneration
			&& record.deformablePayloadGeneration == binding.deformablePayloadGeneration
			&& record.rendererGeneration == plan->rendererGeneration
			&& record.skeletonCompatibilityFingerprint == binding.skeletonCompatibilityFingerprint
			&& record.data == binding.cpuDeformation;
	}

	bool IndexedRangesMatch(const IndexedMeshRequest& request, const IndexedGeometry& topology)
	{
		if (topology.sourceOwner != request.owner || topology.cpuPublicationGeneration != request.owner->publicationGeneration) return false;
		for (const auto [offset, count] : request.ranges)
			if (!TerraEngine::Renderer::Vulkan::ResolveIndexedCornerRange(topology, offset, count)) return false;
		return true;
	}
}

bool VulkanContext::Impl::PrepareIndexedRasterView(
	const TerraEngine::Renderer::RenderFramePublication& publication, std::uint32_t viewIndex,
	VkRenderPass pass, VkSampleCountFlagBits samples, VulkanRasterPipelineConfiguration& configuration,
	std::string& reason, bool& preparationPending)
{
	using namespace TerraEngine::Renderer;
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	preparationPending = false;
	const auto reject = [&reason](const char* detail) { reason = detail; return false; };
	if (deviceLost || !initialized || publication.frameId == 0
		|| publication.rendererSessionGeneration != renderSessionGeneration
		|| publication.backendResourceGeneration != backendResourceGeneration
		|| viewIndex >= publication.views.size() || viewIndex >= publication.presentationInputs.size()
		|| !publication.persistent || !publication.dynamic)
		return reject("Indexed view has a stale or unavailable sealed publication.");
	const auto& selection = publication.views[viewIndex];
	const auto& presentation = publication.presentationInputs[viewIndex];
	if (!presentation.Matches(selection)) return reject("Indexed view presentation does not match its sealed selection.");
	const auto compatibility = indexedRasterPasses.find(pass);
	if (compatibility == indexedRasterPasses.end() || compatibility->second.samples != samples
		|| (samples != VK_SAMPLE_COUNT_1_BIT && samples != VK_SAMPLE_COUNT_4_BIT)
		|| compatibility->second.usesVrsAttachment
		|| compatibility->second.identityFormat != VK_FORMAT_R32G32B32A32_UINT)
		return reject("Indexed view requires an actual registered single/four-sample identity pass with VRS off.");
	if(compatibility->second.screenSpaceMask && (samples != VK_SAMPLE_COUNT_1_BIT
		|| compatibility->second.screenSpaceRaster.expired() || !compatibility->second.screenSpaceFragment
		|| !compatibility->second.screenSpaceFragmentGeneration))
		return reject("Indexed screen-space preparation requires the exact retained single-sample MRT registration.");
	const auto resolver = MakeRenderViewCommandResolver(publication, viewIndex);
	if (resolver.Count() == 0 && !selection.VisibleOrdinals().empty())
		return reject("Indexed view command resolver is incomplete.");
	try
	{
		auto& resources = ResourceManager::Instance();
		const auto defaultId = resources.GetDefaultShaderCollectionID();
		const auto defaultTaskId = resources.GetDefaultTaskShaderCollectionID();
		const auto defaultOwner = resources.AcquireShaderCollectionSnapshot(defaultId);
		const auto taskOwner = resources.AcquireShaderCollectionSnapshot(defaultTaskId);
		if (!defaultOwner) return reject("Indexed view has no accepted Default surface publication.");
		const auto sameDefaultFragment = [](const auto& left, const auto& right) {
			if (!left || !right) return false;
			const auto fragment = [](const auto& owner) {
				return std::find_if(owner->stages.begin(), owner->stages.end(), [](const auto& stage) { return stage.stage == ShaderStage::Fragment; });
			};
			const auto a = fragment(left), b = fragment(right);
			return a != left->stages.end() && b != right->stages.end() && a->shaderId == b->shaderId
				&& a->publicationGeneration == b->publicationGeneration && a->shader == b->shader;
		};
		std::vector<IndexedMeshRequest> meshes;
		std::vector<IndexedMaterialRequest> materials;
		std::unordered_map<TerraEngine::UUID, std::size_t> meshIndices, materialIndices;
		bool usesDefaultTaskSurface = false;
		meshes.reserve(std::min<std::size_t>(resolver.Count(), 128));
		materials.reserve(std::min<std::size_t>(resolver.Count(), 128));
		// Reject unsupported visible packets before optional GPU cache writes. The
		// resolver copies values from the sealed root; workers never resolve ECS.
		for (std::size_t index = 0; index < resolver.Count(); ++index)
		{
			RenderItemRecord command{};
			if (!resolver.Resolve(index, command)) return reject("Indexed view contains an unresolved sealed command.");
			if (command.geometryRoute != GeometryRoute::DirectMeshlet
				|| command.HasCompleteGpuAnimation() || command.animation.source == 2U || command.animation.source == 3U
				|| command.animationBinding.source == AnimationRenderPoseSource::GpuVisual
				|| (command.virtualGeometryFlags & 0xc0000000U) != 0
				|| ((command.virtualGeometryFlags >> 8U) & 255U) != 0
				|| command.virtualGeometryPayloadSource != VirtualGeometryPagePayloadSource::Unknown
				|| command.virtualGeometryEmittedSource != VirtualGeometryEmittedPageSource::None
				|| command.virtualGeometryPagePayloadByteSize != 0
				|| command.selectedVirtualPageNeedCount != 0
				|| command.virtualGeometryPayloadBufferIndex != INVALID_BINDLESS_BUFFER_INDEX
				|| command.virtualGeometryPayloadDescriptorIndex != INVALID_BINDLESS_BUFFER_INDEX
				|| command.virtualGeometryPacketDescriptorIndex != INVALID_BINDLESS_BUFFER_INDEX)
				return reject("Indexed view contains an unsupported GPU, grouped, page-local or geometry-debug packet.");
			if (command.meshId == TerraEngine::EMPTY_UUID || command.materialId == TerraEngine::EMPTY_UUID)
				return reject("Indexed view requires explicit ordinary mesh and material resource identities.");
			const auto materialOwner = resources.AcquireMaterialSnapshot(command.materialId);
			if (!materialOwner || materialOwner->publicationGeneration == 0
				|| materialOwner->header.textureCount > materialOwner->textures.size())
				return reject("Indexed view material publication is unavailable or incomplete.");
			const auto collectionId = materialOwner->header.shaderCollectionId;
			if ((collectionId != TerraEngine::EMPTY_UUID && collectionId != defaultId && collectionId != defaultTaskId)
				|| materialOwner->header.alphaMode > static_cast<std::uint32_t>(AlphaMode::Mask)
				|| (materialOwner->header.flags & TerrainLayerBlend) != 0)
				return reject("Indexed view supports ordinary opaque/cutout Default surface materials only.");
			if (collectionId == defaultTaskId && defaultTaskId != defaultId)
			{
				if (!sameDefaultFragment(defaultOwner, taskOwner))
					return reject("Indexed Default task variant does not retain the same exact surface fragment.");
				usesDefaultTaskSurface = true;
			}
			if (!materialIndices.contains(command.materialId))
			{
				materialIndices.emplace(command.materialId, materials.size());
				materials.push_back({command.materialId, materialOwner});
			}
			else if (materials[materialIndices.at(command.materialId)].owner != materialOwner)
				return reject("Indexed material publication changed during view qualification.");
			const auto meshOwner = resources.AcquireMeshSnapshot(command.meshId);
			if (!meshOwner || meshOwner->publicationGeneration == 0 || !IndexedCpuPoseMatches(publication, command, *meshOwner))
				return reject("Indexed mesh or exact sealed CPU deformation publication is unavailable.");
			if ((command.virtualGeometryFlags & 0x80U) != 0 && !command.animationBinding.HasCpuDeformation())
				return reject("Indexed owner-palette packet has no qualified CPU deformation owner.");
			const std::uint32_t offset = command.virtualMeshletOffset;
			if (offset >= meshOwner->meshletRecords.size()) return reject("Indexed selected meshlet offset is outside its owner.");
			const std::size_t remaining = meshOwner->meshletRecords.size() - offset;
			const auto count = command.virtualMeshletCount == 0 ? remaining : command.virtualMeshletCount;
			if (count == 0 || count > remaining || count > std::numeric_limits<std::uint32_t>::max())
				return reject("Indexed selected meshlet range is outside its owner.");
			if (!meshIndices.contains(command.meshId))
			{
				meshIndices.emplace(command.meshId, meshes.size());
				meshes.push_back({command.meshId, meshOwner, {}, {}});
			}
			auto& mesh = meshes[meshIndices.at(command.meshId)];
			if (mesh.owner != meshOwner) return reject("Indexed mesh publication changed during view qualification.");
			const std::pair range{offset, static_cast<std::uint32_t>(count)};
			if (std::find(mesh.ranges.begin(), mesh.ranges.end(), range) == mesh.ranges.end()) mesh.ranges.push_back(range);
			if (std::find(mesh.materialFlags.begin(), mesh.materialFlags.end(), materialOwner->header.flags) == mesh.materialFlags.end())
				mesh.materialFlags.push_back(materialOwner->header.flags);
		}
		if (!indexedRasterPreparation) indexedRasterPreparation = std::make_shared<IndexedRasterPreparationState>();
		auto& state = *indexedRasterPreparation;
		const auto ready = [](const auto& job) {
			return !job.future.valid() || job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		};
		// Never erase an unfinished job to make room for another. Its immutable
		// owner remains bounded until settlement, even across repeated recooks.
		for (auto entry = state.jobs.begin(); entry != state.jobs.end();)
		{
			if (!meshIndices.contains(entry->first) && ready(entry->second)) entry = state.jobs.erase(entry);
			else ++entry;
		}
		std::size_t uploads = 0;
		for (const auto& request : meshes)
		{
			const auto gpu = meshCache.find(request.id);
			if (gpu != meshCache.end() && gpu->second.sourceOwner == request.owner
				&& gpu->second.cpuPublicationGeneration == request.owner->publicationGeneration
				&& gpu->second.indexedCornerBuffer.buffer != VK_NULL_HANDLE && gpu->second.indexedAllocationGeneration != 0
				&& gpu->second.allocationGeneration != 0 && gpu->second.indexedTopology
				&& gpu->second.indexedCornerBuffer.size >= static_cast<VkDeviceSize>(gpu->second.indexedTopology->indices.size()) * sizeof(std::uint32_t)
				&& IndexedRangesMatch(request, *gpu->second.indexedTopology))
			{
				const auto obsolete = state.jobs.find(request.id);
				if (obsolete != state.jobs.end() && ready(obsolete->second)) state.jobs.erase(obsolete);
				continue;
			}
			auto entry = state.jobs.find(request.id);
			if (entry != state.jobs.end() && entry->second.owner != request.owner)
			{
				if (!ready(entry->second)) { preparationPending = true; continue; }
				state.jobs.erase(entry); entry = state.jobs.end();
			}
			if (entry == state.jobs.end())
			{
				if (state.jobs.size() >= IndexedRasterPreparationState::kMaximumJobs) { preparationPending = true; continue; }
				auto task = ThreadPool::instance().createTask([owner = request.owner]() {
					IndexedRasterPreparationState::Result result;
					auto topology = std::make_shared<IndexedGeometry>();
					if (TerraEngine::Renderer::Vulkan::TryBuildIndexedCornerGeometry(owner, *topology, result.reason)) result.topology = std::move(topology);
					return result;
				});
				IndexedRasterPreparationState::Job job{request.owner, task->getFuture(), {}};
				entry = state.jobs.emplace(request.id, std::move(job)).first;
				try { ThreadPool::instance().enqueue(task, request.owner->meshletRecords.size()); }
				catch (...) { state.jobs.erase(entry); throw; }
			}
			if (!ready(entry->second)) { preparationPending = true; continue; }
			if (entry->second.future.valid())
			{
				try { entry->second.result = entry->second.future.get(); }
				catch (const std::exception& error) { entry->second.result.reason = std::string("Immutable topology job failed: ") + error.what(); }
			}
			const auto topology = entry->second.result.topology;
			if (!topology)
			{
				reason = "Indexed immutable topology rejected: " + entry->second.result.reason;
				preparationPending = false; return false;
			}
			if (!IndexedRangesMatch(request, *topology)) { preparationPending = false; return reject("Indexed topology does not contain every exact selected owner range."); }
			if (uploads >= IndexedRasterPreparationState::kMaximumUploadsPerCall) { preparationPending = true; continue; }
			if (!EnsureIndexedCornerMesh(request.id, topology, reason)) { preparationPending = false; return false; }
			++uploads;
			state.jobs.erase(entry);
		}
		if (preparationPending) return reject("Indexed immutable topology preparation is pending at the protected boundary.");

		const std::array formats{compatibility->second.colorFormat, compatibility->second.identityFormat, compatibility->second.depthFormat};
		auto cached = std::find_if(state.configurations.begin(), state.configurations.end(), [&](const auto& entry) {
			if (entry.defaultOwner != defaultOwner || entry.raster.qualifiedPass != pass || entry.raster.samples != samples
				|| entry.raster.attachmentFormats != formats || !entry.raster.preparedCollection
				|| entry.raster.screenSpaceMask != compatibility->second.screenSpaceMask
				|| entry.raster.screenSpaceFragmentGeneration != compatibility->second.screenSpaceFragmentGeneration
				|| entry.raster.screenSpaceFragment != compatibility->second.screenSpaceFragment
				|| entry.raster.screenSpaceRaster.lock() != compatibility->second.screenSpaceRaster.lock()) return false;
			for (const auto& stage : entry.raster.preparedCollection->stages)
				if (resources.AcquireShaderSnapshot(stage.shaderId) != stage.shader) return false;
			return true;
		});
		VulkanRasterPipelineConfiguration candidate;
		if (cached != state.configurations.end()) candidate = cached->raster;
		else
		{
			auto prepared = PrepareDefaultIndexedPipelineConfiguration(pass, samples, reason);
			if (!prepared) return false;
			candidate = std::move(*prepared);
			if (state.configurations.size() >= 8) state.configurations.erase(state.configurations.begin());
			state.configurations.push_back({defaultOwner, candidate});
		}
		std::unordered_map<std::uint32_t, PipelineKey> keys;
		for (const auto& request : materials)
		{
			const auto flags = request.owner->header.flags;
			if (!keys.contains(flags))
			{
				EnsurePipeline(defaultId, pass, flags, true, candidate);
				const auto key = BuildPipelineKey(defaultId, flags, true, candidate);
				if (!key) return reject("Indexed pipeline has no exact accepted stage publication key.");
				const auto passCache = pipelineCache.find(pass);
				if (passCache == pipelineCache.end()) return reject("Indexed native pipeline creation failed.");
				const auto pipeline = passCache->second.find(*key);
				if (pipeline == passCache->second.end() || pipeline->second.pipeline == VK_NULL_HANDLE || pipeline->second.layout == VK_NULL_HANDLE
					|| pipeline->second.rasterRoute != VulkanRasterRoute::IndexedCorners || pipeline->second.rasterSamples != samples
					|| pipeline->second.hasTaskStage || !pipeline->second.usesConventionalVisibilityIdentity || !pipeline->second.supportsDefaultPushConstants
					|| pipeline->second.screenSpaceMask != candidate.screenSpaceMask
					|| pipeline->second.screenSpaceFragmentGeneration != candidate.screenSpaceFragmentGeneration
					|| pipeline->second.screenSpaceFragment != candidate.screenSpaceFragment)
					return reject("Indexed view has no exact usable vertex/fragment native pipeline variant.");
				keys.emplace(flags, *key);
			}
			(void)EnsureMaterialDescriptorSets(defaultId, request.id);
			const auto material = materialSourceOwners.find(request.id);
			const auto dependencies = materialTextureSourceOwners.find(request.id);
			if (!materialIndexMap.contains(request.id) || material == materialSourceOwners.end() || material->second != request.owner
				|| dependencies == materialTextureSourceOwners.end()) return reject("Indexed material upload has no exact current GPU owner.");
			for (const auto& [id, owner] : dependencies->second)
			{
				const auto texture = textureCache.find(id);
				if (!owner || texture == textureCache.end() || texture->second.sourceOwner != owner || resources.AcquireTextureSnapshot(id) != owner
					|| texture->second.residentMipOffset > resources.ResolveTextureMipResidencyForTexture(id, texture->second.sourceMipCount).sourceMipOffset)
					return reject("Indexed material texture upload is missing its qualified source or requested mip residency.");
			}
		}
		for (const auto& request : meshes)
		{
			for (const auto flags : request.materialFlags)
			{
				const auto& pipeline = pipelineCache.at(pass).at(keys.at(flags));
				if (EnsureMeshDescriptorSet(request.id, pipeline) == VK_NULL_HANDLE)
					return reject("Indexed mesh descriptors do not match the exact prepared pipeline layout.");
			}
			if (resources.AcquireMeshSnapshot(request.id) != request.owner)
				return reject("Indexed source mesh was retired before complete view admission.");
		}
		for (const auto& request : materials)
			if (resources.AcquireMaterialSnapshot(request.id) != request.owner)
				return reject("Indexed source material was retired before complete view admission.");
		if (usesDefaultTaskSurface && resources.AcquireShaderCollectionSnapshot(defaultTaskId) != taskOwner)
			return reject("Indexed Default task surface was retired before complete view admission.");
		if (resources.AcquireShaderCollectionSnapshot(defaultId) != defaultOwner)
			return reject("Indexed Default surface was retired before complete view admission.");
		for (const auto& stage : candidate.preparedCollection->stages)
			if (resources.AcquireShaderSnapshot(stage.shaderId) != stage.shader)
				return reject("Indexed shader stage was retired before complete view admission.");
		if (deviceLost || !initialized || publication.rendererSessionGeneration != renderSessionGeneration
			|| publication.backendResourceGeneration != backendResourceGeneration)
			return reject("Indexed renderer lifetime changed before complete view admission.");
		// Only a fully qualified view replaces the caller's configuration. Optional
		// cache uploads from a failed/pending candidate do not activate settings.
		configuration = std::move(candidate);
		reason.clear();
		return true;
	}
	catch (const std::exception& error)
	{
		preparationPending = false;
		reason = std::string("Indexed view preparation failed: ") + error.what();
		return false;
	}
}
