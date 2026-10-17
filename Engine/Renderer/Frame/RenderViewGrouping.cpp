// Retained immutable compatibility membership and view-local dynamic instance materialization.
#include "RenderFrameState.h"
#include "Profiling/ProfilerScope.h"
#include <bit>
#include <chrono>
void RenderSystem::PrepareFrameGroupedSubmissions(
	const TerraEngine::Renderer::Frame::RetainedRenderTopologyState& retainedRenderTopology,
	TerraEngine::Renderer::Frame::GroupedSubmissionMembershipCacheState& groupedSubmissionMembershipCache,
	std::uint64_t snapshotRendererGeneration, bool renderTopologyRebuilt, RenderViewBuildState& target,
	const std::vector<RenderItemRecord>& commands, RenderOutputKind outputKind, RenderViewType viewType,
	std::uint64_t viewGeneration, std::uint64_t materializedCompatibilitySignature, bool viewEvidenceReused)
{
		// This view-local sidecar is built after visibility and editor tint are
		// final, while the original commands remain complete fallback coverage.
		target.groupedInstanceRecords.clear();
		target.groupedSubmissions.clear();
		target.drawResourcePreparationCommandIndices.clear();
		target.renderOptimization = {};
		target.renderOptimization.frameGeneration = activeRenderFrameId;
		target.renderOptimization.topologyGeneration =
			retainedRenderTopology.topology.generation;
		target.renderOptimization.viewGeneration = viewGeneration;
		target.renderOptimization.rendererGeneration = snapshotRendererGeneration;
		target.renderOptimization.visibleInstanceCount = static_cast<std::uint32_t>(
			std::min<std::size_t>(commands.size(),
				std::numeric_limits<std::uint32_t>::max()));
		target.renderOptimization.topologyInvalidations = renderTopologyRebuilt ? 1u : 0u;
		target.renderOptimization.topologyReuseHits = renderTopologyRebuilt ? 0u : 1u;
		// This is the cache result for this exact view, not topology reuse. Keeping
		// the counters independent prevents the profiler from reporting a stable
		// packet topology as though it had also reused frustum/VG evidence.
		target.renderOptimization.viewEvidenceReuseHits =
			viewEvidenceReused ? 1u : 0u;
		target.renderOptimization.viewEvidenceInvalidations =
			viewEvidenceReused ? 0u : 1u;
		// Measure immutable compatibility discovery separately from dynamic record
		// materialization. Keeping these phases distinct makes profiler evidence
		// actionable when a large crowd regresses in only one part of preparation.
		const auto topologyBuildBegin = std::chrono::steady_clock::now();
		// Stable mesh/material/pipeline membership lives in retainedRenderTopology.
		// This per-view map hashes only values that still come from the one shared
		// push packet. Packet descriptors, page IDs, page byte ranges, meshlet
		// offsets, and animation records are intentionally absent: RenderInstanceRecord
		// carries them to the task/mesh stages for each crowd member. Keeping those
		// per-instance selectors in the key would recreate one draw per page and
		// defeat device-authored page-local grouping.
		const auto compatible = [&](std::uint32_t left, std::uint32_t right) {
			const RenderItemRecord& a = commands[left]; const RenderItemRecord& b = commands[right];
			return a.meshId == b.meshId && a.materialId == b.materialId
				&& a.materialIndex == b.materialIndex
				&& a.textureIndex == b.textureIndex
				&& a.geometryRoute == b.geometryRoute
				&& a.virtualMeshletCount == b.virtualMeshletCount
				&& a.selectedVirtualLod == b.selectedVirtualLod
				&& a.virtualGeometryFlags == b.virtualGeometryFlags
				&& a.virtualGeometryPayloadSource == b.virtualGeometryPayloadSource
				&& a.taskShaderCullingEnabled == b.taskShaderCullingEnabled
				&& a.animationBinding.source == b.animationBinding.source
				&& a.animationBinding.RequiresConservativeCoarseVisibility()
					== b.animationBinding.RequiresConservativeCoarseVisibility()
				&& a.editorTint == b.editorTint;
		};
		const auto dynamicCompatibilityHash = [](const RenderItemRecord& command,
			std::uint32_t topologyTemplateIndex) {
			std::size_t hash = std::hash<std::uint32_t>{}(topologyTemplateIndex);
			const auto combine = [&hash](std::uint64_t value) {
				hash ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL
					+ (hash << 6u) + (hash >> 2u);
			};
			combine(command.virtualMeshletCount);
			combine(command.materialIndex); combine(command.textureIndex);
			combine(static_cast<std::uint32_t>(command.geometryRoute));
			combine(command.selectedVirtualLod); combine(command.virtualGeometryFlags);
			combine(static_cast<std::uint32_t>(command.virtualGeometryPayloadSource));
			combine(command.taskShaderCullingEnabled ? 1u : 0u);
			combine(static_cast<std::uint32_t>(command.animationBinding.source));
			combine(command.animationBinding.RequiresConservativeCoarseVisibility()
				? 1u : 0u);
			// Scene prefab context uses one shared tint for surrounding objects. The
			// tint remains a push-constant value, so equal-tint instances may share a
			// dispatch while focused and context groups remain visually independent.
			combine(std::bit_cast<std::uint32_t>(command.editorTint.x));
			combine(std::bit_cast<std::uint32_t>(command.editorTint.y));
			combine(std::bit_cast<std::uint32_t>(command.editorTint.z));
			combine(std::bit_cast<std::uint32_t>(command.editorTint.w));
			// A view can become visible before its GPU animation packet is sealed.
			// Group membership rejects that pre-ready command, so the retained cache
			// must rebuild when the exact packet identity becomes complete. Without
			// this bit, a startup miss permanently preserved an empty crowd grouping.
			combine(command.HasCompleteGpuAnimation() ? 1u : 0u);
			return hash;
		};
		// Materialization folds the dynamic animation authority lanes into the
		// visibility evidence while commands are already hot. Untinted views can use
		// that proof directly; context-tinted editor views take the complete scan
		// because focus membership can change independently of visibility evidence.
		std::uint64_t compatibilitySignature = 1469598103934665603ULL;
		const auto combineCompatibilitySignature = [&compatibilitySignature](
			std::uint64_t value) {
			compatibilitySignature ^= value + 0x9e3779b97f4a7c15ULL
				+ (compatibilitySignature << 6U) + (compatibilitySignature >> 2U);
		};
		combineCompatibilitySignature(target.renderOptimization.topologyGeneration);
		combineCompatibilitySignature(static_cast<std::uint32_t>(outputKind));
		combineCompatibilitySignature(static_cast<std::uint32_t>(viewType));
		if(materializedCompatibilitySignature != 0U
			&& target.editorContextTint == glm::vec4(0.0f))
		{
			combineCompatibilitySignature(materializedCompatibilitySignature);
		}
		else
		{
			for(std::uint32_t index = 0u; index < commands.size(); ++index) {
				const RenderItemRecord& command = commands[index];
				// Visible commands carry a one-based canonical snapshot index. Use that
				// index to reach the retained immutable membership instead of rebuilding
				// a mesh/material/pipeline key for every object in every rendered view.
				const std::uint32_t identityIndex = command.conventionalVisibilityIdentityIndex;
				const std::size_t topologyRecordIndex = identityIndex == 0u
					? retainedRenderTopology.topology.records.size()
					: static_cast<std::size_t>(identityIndex - 1u);
				if(topologyRecordIndex >= retainedRenderTopology.topology.records.size()) {
					combineCompatibilitySignature(index);
					continue;
				}
				const PersistentRenderTopologyRecord& topologyRecord =
					retainedRenderTopology.topology.records[topologyRecordIndex];
				combineCompatibilitySignature(dynamicCompatibilityHash(
					command, topologyRecord.groupTemplateIndex));
				combineCompatibilitySignature(
					static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(command.editorTint.x))
						| (static_cast<std::uint64_t>(
							std::bit_cast<std::uint32_t>(command.editorTint.y)) << 32U));
				combineCompatibilitySignature(
					static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(command.editorTint.z))
						| (static_cast<std::uint64_t>(
							std::bit_cast<std::uint32_t>(command.editorTint.w)) << 32U));
			}
		}
		std::uint64_t viewCacheKey = static_cast<std::uint64_t>(outputKind);
		viewCacheKey ^= static_cast<std::uint64_t>(viewType) << 8U;
		// View generation advances with frame publication, while compatible member
		// ordering depends only on this output/view lane and the signature above.
		// Keeping it out of the key turns this into a bounded persistent cache instead
		// of allocating a dead entry for every frame of a long editor session.
		GroupedSubmissionMembershipCacheRecord& membershipCache =
			groupedSubmissionMembershipCache.recordsByView[viewCacheKey];
		const bool membershipCacheHit = membershipCache.valid
			&& membershipCache.compatibilitySignature == compatibilitySignature;
		if(!membershipCacheHit) {
			std::vector<GroupedCompatibilityBucket> rebuiltBuckets;
			rebuiltBuckets.reserve(commands.size());
			std::unordered_map<std::size_t, std::vector<std::size_t>> bucketsByDynamicHash;
			bucketsByDynamicHash.reserve(commands.size());
			for(std::uint32_t index = 0u; index < commands.size(); ++index) {
				const RenderItemRecord& command = commands[index];
				const std::uint32_t identityIndex =
					command.conventionalVisibilityIdentityIndex;
				const std::size_t topologyRecordIndex = identityIndex == 0u
					? retainedRenderTopology.topology.records.size()
					: static_cast<std::size_t>(identityIndex - 1u);
				if(topologyRecordIndex >= retainedRenderTopology.topology.records.size()) {
					continue;
				}
				const PersistentRenderTopologyRecord& topologyRecord =
					retainedRenderTopology.topology.records[topologyRecordIndex];
				if(!(command.UsesGroupableMeshTaskRoute()
					&& (command.animationBinding.source == TerraEngine::Renderer::AnimationRenderPoseSource::BindPose
						|| command.HasCompleteGpuAnimation())
					&& topologyRecord.groupTemplateIndex
						!= PersistentRenderTopologyRecord::kInvalidGroupTemplateIndex)) {
					continue;
				}
				const std::size_t hash = dynamicCompatibilityHash(
					command, topologyRecord.groupTemplateIndex);
				auto& collisionCandidates = bucketsByDynamicHash[hash];
				auto existing = std::find_if(collisionCandidates.begin(),
					collisionCandidates.end(), [&](std::size_t bucketIndex) {
						return rebuiltBuckets[bucketIndex].topologyTemplateIndex
							== topologyRecord.groupTemplateIndex
							&& compatible(rebuiltBuckets[bucketIndex].representative, index);
					});
				if(existing == collisionCandidates.end()) {
					const std::size_t bucketIndex = rebuiltBuckets.size();
					GroupedCompatibilityBucket bucket{};
					bucket.topologyTemplateIndex = topologyRecord.groupTemplateIndex;
					bucket.representative = index;
					bucket.members.push_back(index);
					rebuiltBuckets.push_back(std::move(bucket));
					collisionCandidates.push_back(bucketIndex);
				}
				else {
					rebuiltBuckets[*existing].members.push_back(index);
				}
			}
			membershipCache.valid = true;
			membershipCache.compatibilitySignature = compatibilitySignature;
			membershipCache.buckets = std::move(rebuiltBuckets);
			membershipCache.submissionTemplates.clear();
			membershipCache.drawResourcePreparationCommandIndices.clear();
			membershipCache.submissionTemplates.reserve(
				membershipCache.buckets.size());
			membershipCache.drawResourcePreparationCommandIndices.reserve(
				commands.size());
			std::vector<std::uint8_t> groupedCommandCoverage(commands.size(), 0u);
			std::uint32_t firstInstance = 0u;
			for(const GroupedCompatibilityBucket& bucket : membershipCache.buckets) {
				if(bucket.members.size() <= 1u) {
					continue;
				}
				const RenderItemRecord& representative =
					commands[bucket.representative];
				membershipCache.drawResourcePreparationCommandIndices.push_back(
					bucket.representative);
				GroupedRenderSubmission submission{};
				submission.representativeCommandIndex = bucket.representative;
				submission.group.firstInstance = firstInstance;
				submission.group.instanceCount = static_cast<std::uint32_t>(
					bucket.members.size());
				submission.group.key.meshId = representative.meshId;
				submission.group.key.materialId = representative.materialId;
				submission.group.key.geometryRoute = static_cast<std::uint32_t>(
					representative.geometryRoute);
				submission.group.key.renderPassVariant = static_cast<std::uint32_t>(
					outputKind);
				submission.group.key.viewVariant = static_cast<std::uint32_t>(viewType);
				firstInstance += submission.group.instanceCount;
				membershipCache.submissionTemplates.push_back(std::move(submission));
				for(const std::uint32_t commandIndex : bucket.members) {
					if(commandIndex < groupedCommandCoverage.size()) {
						groupedCommandCoverage[commandIndex] = 1u;
					}
				}
			}
			for(std::uint32_t commandIndex = 0u;
				commandIndex < groupedCommandCoverage.size(); ++commandIndex)
			{
				if(groupedCommandCoverage[commandIndex] == 0u) {
					membershipCache.drawResourcePreparationCommandIndices.push_back(
						commandIndex);
				}
			}
		}
		membershipCache.lastTouchedFrameId = activeRenderFrameId;
		const std::vector<GroupedCompatibilityBucket>& buckets = membershipCache.buckets;
		target.groupedInstanceRecords.reserve(commands.size());
		target.groupedSubmissions = membershipCache.submissionTemplates;
		target.drawResourcePreparationCommandIndices =
			membershipCache.drawResourcePreparationCommandIndices;
		// Only publication generations vary for a retained group descriptor. Patch
		// those scalars in the view-local copy while the mesh/material/pass identity
		// and executable ranges remain cache-owned and immutable.
		for(GroupedRenderSubmission& submission : target.groupedSubmissions) {
			submission.group.topologyGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.viewGeneration = viewGeneration;
			submission.group.key.meshGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.key.materialGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.key.pipelineGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.key.residencyGeneration =
				target.renderOptimization.topologyGeneration;
			submission.group.key.rendererGeneration =
				target.renderOptimization.rendererGeneration;
			submission.group.executable = submission.group.key.IsComplete();
		}
		target.renderOptimization.topologyBuildNanoseconds =
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - topologyBuildBegin).count());
		const auto dynamicMaterializationBegin = std::chrono::steady_clock::now();
		for(const GroupedCompatibilityBucket& bucket : buckets) {
			if(bucket.members.size() > 1u) {
				for(const std::uint32_t commandIndex : bucket.members) {
					const RenderItemRecord& command = commands[commandIndex];
					RenderInstanceRecord record{}; record.modelMatrix = command.modelMatrix;
					record.editorTint = command.editorTint;
					// The identity key resolves through this frame's immutable table;
					// keeping it in the stream means every grouped member, rather than
					// only the representative push constants, can prove its owner.
					record.objectIdentity = glm::uvec4(
						command.conventionalVisibilityIdentityIndex,
						static_cast<std::uint32_t>(command.geometryRoute),
						command.virtualGeometryFlags,
						RenderInstanceRecord::PackCompatibilityPolicy(
							command.taskShaderCullingEnabled,
							static_cast<std::uint32_t>(
								command.animationBinding.source),
							command.animationBinding.
								RequiresConservativeCoarseVisibility()));
					record.materialAndTexture = glm::uvec4(command.materialIndex,
						command.textureIndex, commandIndex,
						command.conventionalVisibilityIdentityIndex);
					record.animationSelectors = glm::uvec4(
						command.animation.firstGpuWorkPacket,
						// Dense grouped rendering resolves the current immutable plan at the
						// backend boundary and consumes only the stable packet index. Keeping
						// this redundant generation zero prevents every crowd record from
						// becoming dirty each frame. Non-dense paths patch both selector words.
						0u,
						static_cast<std::uint32_t>(
							command.animation.rendererGeneration),
						static_cast<std::uint32_t>(
							command.animation.programGeneration));
					record.geometrySelectors = glm::uvec4(command.selectedVirtualLod, command.virtualGeometryPacketDescriptorIndex,
						command.virtualMeshletOffset, command.virtualMeshletCount);
					record.virtualGeometrySelectors = glm::uvec4(
						command.selectedVirtualClusterOffset, command.selectedVirtualClusterCount,
						command.selectedVirtualPageNeedOffset, command.selectedVirtualPageNeedCount);
					record.virtualGeometryPayloadSelectors = glm::uvec4(
						static_cast<std::uint32_t>(command.virtualGeometryPayloadSource),
						static_cast<std::uint32_t>(command.virtualGeometryEmittedSource),
						static_cast<std::uint32_t>(command.virtualGeometryPagePayloadByteOffset),
						static_cast<std::uint32_t>(command.virtualGeometryPagePayloadByteSize));
					target.groupedInstanceRecords.push_back(record);
				}
			}
		}
		target.renderOptimization.groupedInstanceCount = static_cast<std::uint32_t>(target.groupedInstanceRecords.size());
		target.renderOptimization.groupedSubmissionCount = static_cast<std::uint32_t>(target.groupedSubmissions.size());
		target.renderOptimization.fallbackInstanceCount = target.renderOptimization.visibleInstanceCount - target.renderOptimization.groupedInstanceCount;
		target.renderOptimization.fallbackSubmissionCount = target.renderOptimization.fallbackInstanceCount;
		target.renderOptimization.avoidedSubmissionCount = target.renderOptimization.groupedInstanceCount - target.renderOptimization.groupedSubmissionCount;
		target.renderOptimization.instanceStreamBytes = static_cast<std::uint32_t>(std::min<std::size_t>(
			target.groupedInstanceRecords.size() * sizeof(RenderInstanceRecord), std::numeric_limits<std::uint32_t>::max()));
		target.renderOptimization.dynamicMaterializationNanoseconds =
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - dynamicMaterializationBegin).count());
		// Validate the exact executable ranges through the production Vulkan work
		// cache contract before the backend is allowed to suppress fallback draws.
		// Group validation reads only instance counts and executable group ranges.
		// Keep one small contiguous group array, but validate the already-owned instance
		// stream directly instead of copying all instances and fallback draw commands.
		std::vector<SharedRenderGroup> dynamicGroups;
		dynamicGroups.reserve(target.groupedSubmissions.size());
		for (const GroupedRenderSubmission& submission
			: target.groupedSubmissions)
		{
			dynamicGroups.push_back(submission.group);
		}
		const TerraEngine::Renderer::Vulkan::GroupedInstanceStreamPlan groupedPlan =
			TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(
				target.groupedInstanceRecords,
				dynamicGroups,
				activeRenderFrameId,
				snapshotRendererGeneration,
				retainedRenderTopology.topology.valid,
				std::numeric_limits<std::uint32_t>::max() / sizeof(RenderInstanceRecord),
				snapshotRendererGeneration);
		if (!groupedPlan.valid && !target.groupedSubmissions.empty())
		{
			// Atomic fallback keeps the complete per-model command list authoritative.
			target.groupedInstanceRecords.clear();
			target.groupedSubmissions.clear();
			target.renderOptimization.groupedInstanceCount = 0u;
			target.renderOptimization.groupedSubmissionCount = 0u;
			target.renderOptimization.fallbackInstanceCount =
				target.renderOptimization.visibleInstanceCount;
			target.renderOptimization.fallbackSubmissionCount =
				target.renderOptimization.visibleInstanceCount;
			target.renderOptimization.avoidedSubmissionCount = 0u;
			target.renderOptimization.instanceStreamBytes = 0u;
			switch (groupedPlan.rejectionReason)
			{
			case RenderGroupRejectionReason::IncompleteKey:
				++target.renderOptimization.rejectedIncompleteKeyCount;
				break;
			case RenderGroupRejectionReason::InstanceRange:
				++target.renderOptimization.rejectedOutOfRangeCount;
				break;
			case RenderGroupRejectionReason::StaleGeneration:
				++target.renderOptimization.rejectedStaleGenerationCount;
				break;
			case RenderGroupRejectionReason::InstanceStreamAllocation:
				++target.renderOptimization.fallbackAllocationFailureCount;
				break;
			default:
				++target.renderOptimization.rejectedUnsupportedVariantCount;
				break;
			}
		}
		target.renderOptimization.valid = 1u;
	}
