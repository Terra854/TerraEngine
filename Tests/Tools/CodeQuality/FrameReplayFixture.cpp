// Installed correctness replay, compiled twice against the complete headers
// matching the original and candidate DLLs. See frame-replay-workloads.json.
// The parent runner owns DLL/export/header-closure pins and watchdogs. This
// executable never changes timing controls or claims a performance result.
// Required CLI: --install-root --project-root --asset-manifest --manifest
// --scenario --workers (1, 2, or 8) --output (fresh) --run-nonce,
// --project-closure, --expected-engine-path and --expected-engine-sha256
// (trusted parent pins, checked before any engine API is called).
#include <Windows.h>
#include <bcrypt.h>

#include "FrameReplayAccess.h"
#include "FrameReplayProjection.h"
#include "Animation/AnimationSystem.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Core/MainEngine.h"
#include "Components/MeshRenderComponent.h"
#include "Components/Transform.h"
#include "Components/AnimatorComponent.h"
#include "Components/TerrainComponent.h"
#include "Scripting/ScriptingSystem.h"
#include "Physics/PhysicsSystem.h"
#include "UI/UISystem.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "Renderer/RHI/RHIContext.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Window/WindowSystem.h"

#include <nlohmann/json.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
	using Json = nlohmann::json;
	using Path = std::filesystem::path;
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::Renderer;
	using namespace TerraEngine::Scheduling;
	using TerraEngine::TestSupport::FrameReplay::PublishedValue;
	struct MissingPrerequisite final : std::runtime_error { using std::runtime_error::runtime_error; };
	void Require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}
	Json ReadJson(const Path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		Require(stream.good(), "Cannot read replay input JSON.");
		return Json::parse(stream);
	}
	// IEEE words preserve signed zero and every exact published float bit; no
	// tolerance, locale, decimal formatting, or NaN-to-null conversion is applied.
	std::uint32_t Bits(float value) { return std::bit_cast<std::uint32_t>(value); }
	std::uint64_t Bits(double value) { return std::bit_cast<std::uint64_t>(value); }
	Json VectorBits(const glm::vec3& value) { return {Bits(value.x), Bits(value.y), Bits(value.z)}; }
	Json VectorBits(const glm::vec4& value) { return {Bits(value.x), Bits(value.y), Bits(value.z), Bits(value.w)}; }
	Json Words(const glm::uvec4& value) { return {value.x, value.y, value.z, value.w}; }
	Json MatrixBits(const glm::mat4& value)
	{
		Json result = Json::array();
		for(glm::length_t column = 0; column < 4; ++column)
			for(glm::length_t row = 0; row < 4; ++row) result.push_back(Bits(value[column][row]));
		return result;
	}
	Json Transform(const AnimationTransform& value)
	{
		return {{"translationBits", {Bits(value.translation.x), Bits(value.translation.y), Bits(value.translation.z)}},
			{"rotationBits", {Bits(value.rotation.x), Bits(value.rotation.y), Bits(value.rotation.z), Bits(value.rotation.w)}},
			{"scaleBits", {Bits(value.scale.x), Bits(value.scale.y), Bits(value.scale.z)}}};
	}
	Json WorldPose(const TerraEngine::Scene::WorldPose& value)
	{
		return {{"cell", {value.position.cell.x, value.position.cell.y, value.position.cell.z}},
			{"offsetBits", {Bits(value.position.offsetMetres.x), Bits(value.position.offsetMetres.y), Bits(value.position.offsetMetres.z)}},
			{"rotationBits", {Bits(value.rotation.x), Bits(value.rotation.y), Bits(value.rotation.z), Bits(value.rotation.w)}},
			{"scaleBits", VectorBits(value.scale)}, {"transformRevision", value.transformRevision},
			{"coordinateFrameRevision", value.coordinateFrameRevision}};
	}
	Json Key(RenderSlotGenerationKey value) { return {{"ordinal", value.ordinal}, {"lifetimeGeneration", value.lifetimeGeneration}}; }
	Json Handle(AnimatorInstanceHandle value) { return {{"slotIndex", value.slotIndex}, {"generation", value.generation}}; }
	Json Asset(const AnimationAssetHandle& value)
	{
		return {{"assetId", TerraEngine::ToHexString(value.assetId.value)}, {"generation", value.generation}};
	}
	Json Command(const AnimationCommandReceipt& value)
	{
		return {{"target", Handle(value.target)}, {"producerId", value.producerId}, {"producerSequence", value.producerSequence},
			{"correlationId", value.correlationId}, {"idempotencyKey", value.idempotencyKey}, {"cancellationToken", value.cancellationToken},
			{"status", static_cast<unsigned>(value.status)}, {"targetTick", value.targetTick}, {"observableTick", value.observableTick},
			{"publicationGeneration", value.publicationGeneration}, {"duplicate", value.duplicate}};
	}
	Json AnimationGeneration(const RenderItemRecord::PublishedAnimationGeneration& value)
	{
		return {{"stableInstanceId", value.stableInstanceId}, {"poseGeneration", value.poseGeneration},
			{"morphGeneration", value.morphGeneration}, {"deformationContentIdLow", value.deformationContentIdLow},
			{"deformationContentIdHigh", value.deformationContentIdHigh}, {"paletteGeneration", value.paletteGeneration},
			{"deformablePayloadGeneration", value.deformablePayloadGeneration}, {"programGeneration", value.programGeneration},
			{"animationPlanGeneration", value.animationPlanGeneration}, {"rendererGeneration", value.rendererGeneration},
			{"firstGpuWorkPacket", value.firstGpuWorkPacket}, {"gpuWorkPacketCount", value.gpuWorkPacketCount},
			{"gpuStageMask", value.gpuStageMask}, {"source", value.source}, {"complete", value.complete}};
	}
	Json RenderCommand(const RenderItemRecord& value)
	{
		return {{"objectId", TerraEngine::ToHexString(value.objectId)}, {"meshId", TerraEngine::ToHexString(value.meshId)},
			{"materialId", TerraEngine::ToHexString(value.materialId)}, {"worldPose", WorldPose(value.canonicalWorldPose)},
			{"modelMatrixBits", MatrixBits(value.modelMatrix)}, {"editorTintBits", VectorBits(value.editorTint)},
			{"materialIndex", value.materialIndex}, {"meshDataIndex", value.meshDataIndex}, {"textureIndex", value.textureIndex},
			{"visibilityIdentityIndex", value.conventionalVisibilityIdentityIndex}, {"geometryRoute", static_cast<unsigned>(value.geometryRoute)},
			{"objectLifetimeGeneration", value.objectLifetimeGeneration}, {"animation", AnimationGeneration(value.animation)},
			{"virtualMeshletOffset", value.virtualMeshletOffset}, {"virtualMeshletCount", value.virtualMeshletCount},
			{"selectedVirtualLod", value.selectedVirtualLod}, {"virtualGeometryFlags", value.virtualGeometryFlags},
			{"selectedVirtualClusterOffset", value.selectedVirtualClusterOffset}, {"selectedVirtualClusterCount", value.selectedVirtualClusterCount},
			{"selectedVirtualPageNeedOffset", value.selectedVirtualPageNeedOffset}, {"selectedVirtualPageNeedCount", value.selectedVirtualPageNeedCount},
			{"virtualGeometryPageId", value.virtualGeometryPageId}, {"virtualGeometryFallbackPageId", value.virtualGeometryFallbackPageId},
			{"taskShaderCullingEnabled", value.taskShaderCullingEnabled}, {"sealedCommand", PublishedValue(value)}};
	}
	Json Instance(const RenderInstanceRecord& value)
	{
		return {{"modelMatrixBits", MatrixBits(value.modelMatrix)}, {"editorTintBits", VectorBits(value.editorTint)},
			{"objectIdentity", Words(value.objectIdentity)}, {"materialAndTexture", Words(value.materialAndTexture)},
			{"animationSelectors", Words(value.animationSelectors)}, {"geometrySelectors", Words(value.geometrySelectors)},
			{"virtualGeometrySelectors", Words(value.virtualGeometrySelectors)},
			{"virtualGeometryPayloadSelectors", Words(value.virtualGeometryPayloadSelectors)}};
	}

	Json RenderRoot(const std::shared_ptr<const RenderFramePublication>& root)
	{
		if(!root) return {{"present", false}, {"root", nullptr}};
		const auto& receipt = root->receipt;
		Json value = {{"frameId", root->frameId}, {"rendererSessionGeneration", root->rendererSessionGeneration},
			{"backendResourceGeneration", root->backendResourceGeneration}, {"sceneGeneration", root->sceneGeneration},
			{"animationPresentationGeneration", root->animationPresentationGeneration}, {"expectedPreviousGeneration", root->expectedPreviousGeneration},
			{"generation", root->generation}, {"selectedOrdinals", root->selectedOrdinals},
			{"receipt", {{"outcome", static_cast<unsigned>(receipt.outcome)}, {"publicationGeneration", receipt.publicationGeneration},
				{"expectedPreviousGeneration", receipt.expectedPreviousGeneration}, {"observedPreviousGeneration", receipt.observedPreviousGeneration},
				{"reusedPersistentPages", receipt.reusedPersistentPages}, {"changedDynamicPages", receipt.changedDynamicPages},
				{"viewCount", receipt.viewCount}, {"rangeCount", receipt.rangeCount}, {"diagnostic", receipt.DiagnosticText()}}}};
		value["persistent"] = nullptr;
		if(root->persistent)
		{
			Json pages = Json::array();
			for(const auto& page : root->persistent->pages)
			{
				if(!page) { pages.push_back(nullptr); continue; }
				Json slots = Json::array();
				for(const auto& slot : page->slots) slots.push_back({{"key", Key(slot.key)}, {"objectId", TerraEngine::ToHexString(slot.objectId)},
					{"meshId", TerraEngine::ToHexString(slot.meshId)}, {"materialId", TerraEngine::ToHexString(slot.materialId)},
					{"meshGeneration", slot.meshGeneration}, {"materialGeneration", slot.materialGeneration}, {"resourceGeneration", slot.resourceGeneration},
					{"packetTemplateIdentity", slot.packetTemplateIdentity}, {"routeTemplate", slot.routeTemplate}, {"occupied", slot.occupied}});
				pages.push_back({{"generation", page->generation}, {"firstOrdinal", page->firstOrdinal},
					{"producerValidatedSlotCount", page->producerValidatedSlotCount}, {"slots", std::move(slots)}});
			}
			value["persistent"] = {{"generation", root->persistent->generation},
				{"rendererSessionGeneration", root->persistent->rendererSessionGeneration}, {"pages", std::move(pages)}};
		}
		value["dynamic"] = nullptr;
		if(root->dynamic)
		{
			Json pages = Json::array();
			for(const auto& page : root->dynamic->pages)
			{
				if(!page) { pages.push_back(nullptr); continue; }
				Json lanes = Json::array();
				for(const auto& lane : page->lanes) lanes.push_back({{"key", Key(lane.key)}, {"instance", Instance(lane.instance)},
					{"worldPose", WorldPose(lane.canonicalWorldPose)}, {"hasCanonicalWorldPose", lane.hasCanonicalWorldPose},
					{"previousModelMatrixBits", MatrixBits(lane.previousModelMatrix)}, {"animation", AnimationGeneration(lane.animation)},
					{"transformGeneration", lane.transformGeneration}, {"animationPresentationGeneration", lane.animationPresentationGeneration},
					{"materialOverrideGeneration", lane.materialOverrideGeneration}, {"boundsGeneration", lane.boundsGeneration},
					{"lifecycleGeneration", lane.lifecycleGeneration}, {"resourceGeneration", lane.resourceGeneration}, {"occupied", lane.occupied},
					{"animationBinding", PublishedValue(lane.animationBinding)}, {"shadowCasterInputs", PublishedValue(lane.shadowCasterInputs)}});
				pages.push_back({{"generation", page->generation}, {"firstOrdinal", page->firstOrdinal}, {"changedLanes", page->changedLanes},
					{"producerValidatedLaneCount", page->producerValidatedLaneCount},
					{"producerValidatedPersistentGeneration", page->producerValidatedPersistentGeneration}, {"lanes", std::move(lanes)}});
			}
			value["dynamic"] = {{"generation", root->dynamic->generation},
				{"rendererSessionGeneration", root->dynamic->rendererSessionGeneration}, {"pages", std::move(pages)}};
		}
		value["views"] = Json::array();
		for(std::size_t index = 0; index < root->views.size(); ++index)
		{
			const auto& view = root->views[index];
			Json ordinals = Json::array();
			for(const auto& key : view.VisibleOrdinals()) ordinals.push_back(Key(key));
			Json commands = Json::array();
			const auto resolver = MakeRenderViewCommandResolver(*root, index);
			for(std::size_t selection = 0; selection < resolver.Count(); ++selection)
			{
				RenderItemRecord command{};
				Require(resolver.Resolve(selection, command), "Actual sealed view command cannot be resolved.");
				commands.push_back(RenderCommand(command));
			}
			value["views"].push_back({{"outputKind", static_cast<unsigned>(view.outputKind)}, {"viewType", static_cast<unsigned>(view.viewType)},
				{"outputId", view.outputId}, {"generation", view.generation}, {"lightIdentity", view.lightIdentity}, {"visibleOrdinals", std::move(ordinals)},
				{"selectedLods", view.SelectedLods()}, {"routeSelectors", view.RouteSelectors()}, {"packetTemplateIdentities", view.PacketTemplateIdentities()},
				{"tintSelectors", view.TintSelectors()}, {"editorTints", PublishedValue(view.EditorTints())}, {"resolvedCommands", std::move(commands)}});
		}
		value["submissionRanges"] = Json::array();
		for(const auto& range : root->submissionRanges) value["submissionRanges"].push_back({{"route", static_cast<unsigned>(range.route)},
			{"viewIndex", range.viewIndex}, {"firstSelection", range.firstSelection}, {"selectionCount", range.selectionCount},
			{"packetTemplateIdentity", range.packetTemplateIdentity}, {"singleton", range.singleton}});
		value["resourcePreparation"] = {{"generation", root->resourcePreparation.generation}, {"records", Json::array()}};
		for(const auto& row : root->resourcePreparation.records) value["resourcePreparation"]["records"].push_back({
			{"resourceId", TerraEngine::ToHexString(row.resourceId)}, {"resourceGeneration", row.resourceGeneration},
			{"preparationIndex", row.preparationIndex}, {"resident", row.resident}, {"fallbackAllowed", row.fallbackAllowed}});
		// These are the sealed values consumed by the real backend, not rebuilt
		// approximations from diagnostics. Shared storage is traversed as const
		// values; physical addresses, reference counts and allocation state never
		// enter the parity projection.
		value["referenceFrames"] = PublishedValue(root->referenceFrames);
		value["presentationInputs"] = PublishedValue(root->presentationInputs);
		value["backendWork"] = PublishedValue(root->backendWork);
		return {{"present", true}, {"root", std::move(value)}};
	}

	Json Parameter(const AnimationParameterValue& value)
	{
		return std::visit([](const auto& item) -> Json {
			using T = std::decay_t<decltype(item)>;
			if constexpr(std::is_same_v<T, std::monostate>) return nullptr;
			else if constexpr(std::is_same_v<T, float>) return {{"bits", Bits(item)}};
			else if constexpr(std::is_same_v<T, AnimationFloat3>) return {{"bits", {Bits(item.x), Bits(item.y), Bits(item.z)}}};
			else if constexpr(std::is_same_v<T, AnimationQuaternion>) return {{"bits", {Bits(item.x), Bits(item.y), Bits(item.z), Bits(item.w)}}};
			else if constexpr(std::is_same_v<T, AnimationTransform>) return Transform(item);
			else return item;
		}, value);
	}
	Json Event(const AnimationEventRecord& value)
	{
		const auto& id = value.identity;
		return {{"identity", {{"animator", Handle(id.animator)}, {"stateInstanceId", id.stateInstanceId},
			{"clipAssetId", TerraEngine::ToHexString(id.clipAssetId.value)}, {"eventId", id.eventId}, {"sourceTick", id.sourceTick}, {"loopCount", id.loopCount}}},
			{"payloadType", static_cast<unsigned>(value.payloadType)}, {"payload", value.payload}, {"sourceStateId", value.sourceStateId},
			{"layerIndex", value.layerIndex}, {"direction", static_cast<int>(value.direction)}, {"correlationId", value.correlationId},
			{"producerOrder", value.producerOrder}, {"historical", value.historical}};
	}
	Json RootMotion(const AnimationRootMotionDelta& value)
	{
		return {{"animator", Handle(value.animator)}, {"sourceBeginTick", value.sourceBeginTick}, {"sourceEndTick", value.sourceEndTick},
			{"localTranslationBits", {Bits(value.localTranslation.x), Bits(value.localTranslation.y), Bits(value.localTranslation.z)}},
			{"worldTranslationBits", {Bits(value.worldTranslation.x), Bits(value.worldTranslation.y), Bits(value.worldTranslation.z)}},
			{"localYawBits", Bits(value.localYawRadians)}, {"worldYawBits", Bits(value.worldYawRadians)},
			{"accumulatedCycles", value.accumulatedCycles}, {"sourceStateId", value.sourceStateId}, {"layerIndex", value.layerIndex},
			{"generation", value.generation}, {"policy", static_cast<unsigned>(value.policy)},
			{"sourceGeneration", value.sourceGeneration}, {"evaluationDurationBits", Bits(value.evaluationDurationSeconds)}};
	}
	Json Snapshot(const std::shared_ptr<const AnimationSnapshot>& root)
	{
		if(!root) return nullptr;
		const auto& playback = root->playback;
		Json value = {{"animator", Handle(root->animator)}, {"publicationGeneration", root->publicationGeneration}, {"tick", root->tick},
			{"playback", {{"currentStateId", playback.currentStateId}, {"previousStateId", playback.previousStateId},
				{"transitionProgressBits", Bits(playback.transitionProgress)}, {"normalizedTimeBits", Bits(playback.normalizedTime)},
				{"loopCount", playback.loopCount}, {"effectiveSpeedBits", Bits(playback.effectiveSpeed)},
				{"latestStatus", static_cast<unsigned>(playback.latestStatus)}, {"activeLayers", Json::array()}, {"parameters", Json::array()}}},
			{"pose", nullptr}};
		for(const auto& layer : playback.activeLayers) value["playback"]["activeLayers"].push_back({{"layerIndex", layer.layerIndex},
			{"stateId", layer.stateId}, {"weightBits", Bits(layer.weight)}, {"stateTick", layer.stateTick}});
		for(const auto& parameter : playback.parameters) value["playback"]["parameters"].push_back({{"parameterId", parameter.parameterId},
			{"type", static_cast<unsigned>(parameter.type)}, {"value", Parameter(parameter.value)}});
		if(root->pose)
		{
			const auto& pose = *root->pose;
			const auto& generation = pose.Generation();
			const auto& layout = pose.Layout();
			Json result = {{"sealed", pose.IsSealed()}, {"layout", {{"jointCount", layout.jointCount}, {"morphCount", layout.morphCount},
				{"curveCount", layout.curveCount}, {"eventCapacity", layout.eventCapacity}, {"scratchBytes", layout.scratchBytes}}},
				{"generation", {{"animator", Handle(generation.animator)},
				{"poseGeneration", generation.poseGeneration}, {"skeleton", Asset(generation.skeleton)}, {"programGeneration", generation.programGeneration}, {"tick", generation.tick}}},
				{"localTransforms", Json::array()}, {"modelTransforms", Json::array()}, {"morphValues", Json::array()},
				{"curveValues", Json::array()}, {"events", Json::array()}, {"rootMotion", nullptr}};
			for(const auto& transform : pose.LocalTransforms()) result["localTransforms"].push_back(Transform(transform));
			for(const auto& transform : pose.ModelTransforms()) result["modelTransforms"].push_back(Transform(transform));
			for(const auto& channel : pose.MorphValues()) result["morphValues"].push_back({{"stableId", channel.stableId}, {"valueBits", Bits(channel.value)}});
			for(const auto& channel : pose.CurveValues()) result["curveValues"].push_back({{"stableId", channel.stableId}, {"valueBits", Bits(channel.value)}});
			for(const auto& event : pose.Events()) result["events"].push_back(Event(event));
			if(pose.RootMotionDelta()) result["rootMotion"] = RootMotion(*pose.RootMotionDelta());
			value["pose"] = std::move(result);
		}
		return value;
	}
	Json SkeletonProof(const std::shared_ptr<const SkeletonAsset>& skeleton)
	{
		// Static cooked skeleton tables are pinned as actual project inputs.
		// Retain the loaded owner identity, runtime generation and content proof
		// here rather than duplicating immutable source labels/bind tables per row.
		if(!skeleton) return nullptr;
		Json joints = Json::array();
		for(const auto& joint : skeleton->joints) joints.push_back(joint.stableId);
		return {{"assetId", TerraEngine::ToHexString(skeleton->assetId.value)}, {"runtimeGeneration", skeleton->runtimeGeneration},
			{"compatibilityFingerprint", skeleton->compatibilityFingerprint}, {"referencePoseHash", skeleton->referencePoseHash},
			{"rootJointIndex", skeleton->rootJointIndex}, {"formatVersion", skeleton->formatVersion}, {"jointStableIds", std::move(joints)}};
	}
	Json AnimationRoot(const std::shared_ptr<const AnimationAuthorityFrame>& root)
	{
		if(!root) return {{"present", false}, {"root", nullptr}};
		const auto& receipt = root->receipt;
		Json value = {{"frameId", root->frameId}, {"sessionGeneration", root->sessionGeneration},
			{"lifecycleGeneration", root->lifecycleGeneration}, {"authorityGeneration", root->authorityGeneration},
			{"receipt", {{"outcome", static_cast<unsigned>(receipt.outcome)}, {"authorityGeneration", receipt.authorityGeneration},
				{"expectedPreviousGeneration", receipt.expectedPreviousGeneration}, {"observedPreviousGeneration", receipt.observedPreviousGeneration},
				{"reusedPages", receipt.reusedPages}, {"changedPages", receipt.changedPages}, {"evaluatedRows", receipt.evaluatedRows},
				{"fallbackRows", receipt.fallbackRows}, {"eventCount", receipt.eventCount}, {"gpuCandidateCount", receipt.gpuCandidateCount}}},
			{"pages", Json::array()}, {"sidecars", nullptr}};
		if(root->pages) for(const auto& page : *root->pages)
		{
			if(!page) { value["pages"].push_back(nullptr); continue; }
			Json rows = Json::array();
			for(const auto& row : page->rows) rows.push_back({{"key", {{"slot", row.key.slot}, {"lifetimeGeneration", row.key.lifetimeGeneration}}},
				{"ownerObjectId", TerraEngine::ToHexString(row.ownerObjectId)}, {"objectGeneration", row.objectGeneration},
				{"componentGeneration", row.componentGeneration}, {"sourceGeneration", row.sourceGeneration}, {"poseGeneration", row.poseGeneration},
				{"authorityPublicationGeneration", row.authorityPublicationGeneration}, {"tick", row.tick}, {"stateId", row.stateId},
				{"renderBindingGeneration", row.renderBindingGeneration}, {"controller", Asset(row.controller)}, {"clip", Asset(row.clip)},
				{"skeleton", Asset(row.skeleton)}, {"skeletonAssetProof", SkeletonProof(row.skeletonAsset)}, {"poseMemoryReservationPresent", row.poseMemoryReservation != nullptr},
				{"jointStableIds", row.jointStableIds}, {"skeletonCompatibilityFingerprint", row.skeletonCompatibilityFingerprint},
				{"visualPoseContentIdLow", row.visualPoseContentIdLow}, {"visualPoseContentIdHigh", row.visualPoseContentIdHigh},
				{"sourceObjectWorldTransform", Transform(row.sourceObjectWorldTransform)}, {"sourceObjectWorldTransformCaptured", row.sourceObjectWorldTransformCaptured},
				{"requestedExecutionPolicy", static_cast<unsigned>(row.requestedExecutionPolicy)}, {"effectiveExecutionMode", static_cast<unsigned>(row.effectiveExecutionMode)},
				{"rootMotionPolicy", static_cast<unsigned>(row.rootMotionPolicy)}, {"updatePolicy", static_cast<unsigned>(row.updatePolicy)},
				{"eventPolicy", static_cast<unsigned>(row.eventPolicy)}, {"cullingPolicy", static_cast<unsigned>(row.cullingPolicy)},
				{"status", static_cast<unsigned>(row.status)}, {"snapshot", Snapshot(row.snapshot)}, {"active", row.active},
				{"safeFallback", row.safeFallback}, {"streamedClipPayloadsActive", row.streamedClipPayloadsActive}});
			value["pages"].push_back({{"firstSlot", page->firstSlot}, {"capacity", page->capacity}, {"changedMasks", page->changedMasks},
				{"workerValidatedRowCount", page->workerValidatedRowCount}, {"rows", std::move(rows)}});
		}
		if(root->sidecars)
		{
			const auto& sidecars = *root->sidecars;
			Json side = {{"events", Json::array()}, {"rootMotion", Json::array()},
				{"summary", {{"evaluatedRows", sidecars.summary.evaluatedRows}, {"fallbackRows", sidecars.summary.fallbackRows},
					{"rejectedRows", sidecars.summary.rejectedRows}, {"reserved", sidecars.summary.reserved}}},
				{"workerValidatedAuthorityGeneration", sidecars.workerValidatedAuthorityGeneration}};
			for(const auto& event : sidecars.events) side["events"].push_back(Event(event));
			for(const auto& motion : sidecars.rootMotion) side["rootMotion"].push_back(RootMotion(motion));
			const auto add = [&](const char* name, const std::vector<AnimationSpecialistRecord>& records) {
				side[name] = Json::array();
				for(const auto& row : records) side[name].push_back({{"key", {{"slot", row.key.slot},
					{"lifetimeGeneration", row.key.lifetimeGeneration}, {"sessionGeneration", row.key.sessionGeneration}}},
					{"authorityGeneration", row.authorityGeneration}, {"correlationId", row.correlationId},
					{"status", static_cast<unsigned>(row.status)}, {"kind", row.kind}, {"flags", row.flags}});
			};
			add("callbacks", sidecars.callbacks); add("physics", sidecars.physics); add("attachments", sidecars.attachments);
			add("procedural", sidecars.procedural); add("rollback", sidecars.rollback); add("completions", sidecars.completions);
			add("gpuCandidates", sidecars.gpuCandidates); add("diagnostics", sidecars.diagnostics);
			value["sidecars"] = std::move(side);
		}
		return {{"present", true}, {"root", std::move(value)}};
	}

	Json Declaration(const FrameWorkloadDeclaration& row)
	{
		return {{"workloadName", row.workloadName}, {"ownerSystem", row.ownerSystem}, {"phase", row.phase},
			{"stableDescriptorId", row.stableDescriptorId}, {"stableDescriptorGeneration", row.stableDescriptorGeneration},
			{"activationPredicateKey", row.activationPredicateKey}, {"active", row.active}, {"ready", row.ready},
			{"readinessReason", row.readinessReason}, {"reads", row.reads}, {"writes", row.writes},
			{"explicitDependencies", row.explicitDependencies}, {"executionOrder", row.executionOrder},
			{"estimatedWorkItemCount", row.estimatedWorkItemCount}, {"inlineWorkItemThreshold", row.inlineWorkItemThreshold},
			{"partitionPolicy", static_cast<unsigned>(row.partitionPolicy)}, {"partitionWorkItemCount", row.partitionWorkItemCount},
			{"maximumPartitionCount", row.maximumPartitionCount}, {"sameFrameVisibilityRequired", row.sameFrameVisibilityRequired},
			{"workerEligible", row.workerEligible}, {"mainThreadRequired", row.mainThreadRequired},
			{"mutationPolicy", static_cast<unsigned>(row.mutationPolicy)}, {"phaseKind", static_cast<unsigned>(row.phaseKind)},
			{"boundaryClassification", static_cast<unsigned>(row.boundaryClassification)}, {"hardBoundary", row.hardBoundary},
			{"safeSubwork", row.safeSubwork}, {"protectedCommit", row.protectedCommit}, {"protectedReason", row.protectedReason},
			{"fallbackReason", row.fallbackReason}, {"profilingLabel", row.profilingLabel}};
	}
	Json Plan(const FrameExecutionPlan& plan, const ScheduleSummary& summary)
	{
		Json value = {{"valid", plan.valid}, {"completed", plan.completed}, {"executionMode", static_cast<unsigned>(plan.executionMode)},
			{"parallelExecutionUsed", plan.parallelExecutionUsed}, {"declarations", Json::array()}, {"dependencyEdges", Json::array()},
			{"results", Json::array()}, {"phaseNames", summary.phases}, {"systemNames", summary.systems},
			{"declaredFlushNames", summary.flushPoints}, {"afterPhysicsFlushActive", summary.afterPhysicsFlushActive},
			{"afterPhysicsConsumer", summary.afterPhysicsConsumer}, {"afterPhysicsReason", summary.afterPhysicsReason},
			{"declaredMutationFlushCount", plan.migrationSummary.mutationFlushCount}};
		value["scheduleValid"] = summary.valid;
		value["scheduleDiagnostics"] = Json::array();
		for(const auto& diagnostic : summary.diagnostics) value["scheduleDiagnostics"].push_back({
			{"severity", static_cast<unsigned>(diagnostic.severity)}, {"category", static_cast<unsigned>(diagnostic.category)},
			{"systemName", diagnostic.systemName}, {"phaseName", diagnostic.phaseName}, {"resourceName", diagnostic.resourceName},
			{"message", diagnostic.message}, {"recommendedAction", diagnostic.recommendedAction}});
		for(const auto& declaration : plan.workloads) value["declarations"].push_back(Declaration(declaration));
		for(const auto& edge : plan.dependencyEdges) value["dependencyEdges"].push_back({edge.prerequisiteIndex, edge.dependentIndex});
		for(const auto& row : plan.results) value["results"].push_back({{"stableDescriptorId", row.declaration.stableDescriptorId},
			{"executionPolicy", static_cast<unsigned>(row.executionPolicy)}, {"completed", row.completed}, {"fallbackUsed", row.fallbackUsed},
			{"inlineSelected", row.inlineSelected}, {"omitted", row.omitted}, {"partitionCount", row.partitionCount}});
		value["firstFailure"] = nullptr;
		if(plan.firstFailure) value["firstFailure"] = {{"code", static_cast<unsigned>(plan.firstFailure->code)},
			{"workloadName", plan.firstFailure->workloadName}, {"resourceName", plan.firstFailure->resourceName}, {"message", plan.firstFailure->message}};
		return value;
	}
	Json Scratch(const RenderFrameScratchValidationState& state)
	{
		// Lifetime/episode identities are deliberately diagnostic-only. Equality
		// compares pressure and reclamation facts without normalizing new domains.
		return {{"active", state.active}, {"frameId", state.latest.frameId},
			{"usedBytes", state.latest.usedBytes}, {"peakBytes", state.latest.peakBytes},
			{"capacityBytes", state.latest.capacityBytes}, {"reuseAllocationCount", state.latest.reuseAllocationCount},
			{"fallbackAllocationCount", state.latest.fallbackAllocationCount}, {"resetCount", state.latest.resetCount},
			{"outcome", static_cast<unsigned>(state.latest.outcome)},
			{"fallbackAllocations", state.reclamation.totalFallbackAllocationCount},
			{"fallbackDeallocations", state.reclamation.totalFallbackDeallocationCount},
			{"outstandingFallbackAllocations", state.reclamation.outstandingFallbackAllocationCount},
			{"outstandingFallbackBytes", state.reclamation.outstandingFallbackBytes}};
	}
	Json Graph(const JobGraphSummary& summary)
	{
		Json value = {{"valid", summary.valid}, {"completed", summary.completed}, {"cancelled", summary.cancelled},
			{"fatalExecution", summary.fatalExecution}, {"secondaryWorkerFailureCount", summary.secondaryWorkerFailureCount},
			{"settledWorkerCount", summary.settledWorkerCount}, {"terminalStates", Json::array()},
			{"workerJobCount", summary.workerJobCount}, {"protectedJobCount", summary.protectedJobCount},
			{"inlineJobCount", summary.inlineJobCount}, {"rejectedJobCount", summary.rejectedJobCount},
			{"fallbackJobCount", summary.fallbackJobCount}, {"blockedDependencyCount", summary.blockedDependencyCount}};
		for(const auto state : summary.terminalStates) value["terminalStates"].push_back(static_cast<unsigned>(state));
		const auto add = [&](const char* name, const std::vector<JobGraphResultEntry>& entries) {
			value[name] = Json::array();
			for(const auto& entry : entries) value[name].push_back({{"name", entry.name}, {"kind", static_cast<unsigned>(entry.kind)},
				{"state", static_cast<unsigned>(entry.state)}, {"executionPolicy", static_cast<unsigned>(entry.executionPolicy)},
				{"partitionIndex", entry.partitionIndex}, {"partitionCount", entry.partitionCount}, {"itemBegin", entry.itemBegin},
				{"itemEnd", entry.itemEnd}, {"usefulWorkItemCount", entry.usefulWorkItemCount}, {"dependencyWave", entry.dependencyWave}});
		};
		add("jobs", summary.jobs); add("taskGroups", summary.taskGroups); add("barriers", summary.barriers);
		return value;
	}
	Json FilePin(const Path& path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		Require(input.good(), "Cannot pin replay input file.");
		const auto length = input.tellg();
		Require(length >= 0, "Cannot size replay input file.");
		input.seekg(0);
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Cannot open SHA256 provider.");
		struct AlgorithmOwner { BCRYPT_ALG_HANDLE value; ~AlgorithmOwner() { BCryptCloseAlgorithmProvider(value, 0); } } owner{algorithm};
		BCRYPT_HASH_HANDLE hash = nullptr;
		Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "Cannot create SHA256 context.");
		struct HashOwner { BCRYPT_HASH_HANDLE value; ~HashOwner() { BCryptDestroyHash(value); } } hashOwner{hash};
		std::array<char, 65536> buffer{};
		while(input)
		{
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const auto count = input.gcount();
			if(count != 0) Require(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) >= 0, "Cannot hash replay input.");
		}
		Require(input.eof(), "Replay input pin read failed.");
		std::array<unsigned char, 32> digest{};
		Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "Cannot finish replay input pin.");
		constexpr char alphabet[] = "0123456789abcdef";
		std::string text;
		for(const auto byte : digest) { text.push_back(alphabet[byte >> 4]); text.push_back(alphabet[byte & 15]); }
		return {{"path", std::filesystem::absolute(path).lexically_normal().generic_string()}, {"sha256", text},
			{"bytes", static_cast<std::uint64_t>(length)}};
	}
	Path ModulePath(HMODULE module)
	{
		std::array<wchar_t, 32768> path{};
		const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
		Require(count != 0 && count < path.size(), "Cannot identify loaded replay module.");
		return Path(path.data());
	}
	void WriteFresh(const Path& path, const Json& value)
	{
		// CREATE_NEW prevents replacing a prior or concurrently created receipt.
		const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		Require(file != INVALID_HANDLE_VALUE, "Replay output must be a fresh file in an existing directory.");
		struct Owner { HANDLE value; ~Owner() { CloseHandle(value); } } owner{file};
		const auto bytes = value.dump(2) + "\n";
		std::size_t offset = 0;
		while(offset < bytes.size())
		{
			DWORD written = 0;
			const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536));
			Require(WriteFile(file, bytes.data() + offset, count, &written, nullptr) && written != 0, "Cannot write replay receipt.");
			offset += written;
		}
		Require(FlushFileBuffers(file) != FALSE, "Cannot flush replay receipt.");
	}
	Json EmptyLane(const char* name)
	{
		return {{"laneId", name}, {"status", "Incomplete"},
			{"authoritative", {{"frames", Json::array()}, {"boundaries", Json::array()}, {"mutationObservations", Json::array()},
				{"lifetimeObservations", Json::array()}, {"scenarioAssertions", Json::array()}}},
			{"diagnostic", {{"timings", Json::array()}, {"runtimeIdentifiers", Json::array()}}}};
	}
	void Assertion(Json& lane, const std::string& id, Json expected, Json observed)
	{
		const bool passed = expected == observed;
		lane["authoritative"]["scenarioAssertions"].push_back({{"id", id}, {"expected", std::move(expected)},
			{"observed", std::move(observed)}, {"passed", passed}});
		if(!passed) lane["status"] = "Failed";
	}
	struct BoundaryObserver
	{
		std::mutex mutex;
		Json events = Json::array();
		std::size_t frame = 0;
		bool armed = false;
		bool injected = false;
		bool faultRanOnWorker = false;
		DWORD coordinatorThread = GetCurrentThreadId();
		std::string action;
		AnimatorInstanceHandle animator;
		Json cancellationCacheFacts = Json::object();
		void Append(const FrameWorkloadDeclaration& declaration, const FrameWorkloadPartition& partition,
			const char* checkpoint, bool failed = false)
		{
			std::lock_guard lock(mutex);
			events.push_back({{"frameOrdinal", frame}, {"stableDescriptorId", declaration.stableDescriptorId},
				{"executionOrder", declaration.executionOrder}, {"partitionIndex", partition.partitionIndex},
				{"partitionCount", partition.partitionCount}, {"itemBegin", partition.itemBegin}, {"itemEnd", partition.itemEnd},
				{"checkpoint", checkpoint}, {"failed", failed}, {"onCoordinator", GetCurrentThreadId() == coordinatorThread},
				{"observedSequence", events.size() + 1U}});
		}
		void Probe(const FrameWorkloadDeclaration& declaration, bool before)
		{
			// This callback is invoked only at an existing protected exchange. It
			// changes the real producer's generation through its public ingress;
			// it never modifies JobGraph cancellation state or an authority root.
			if(!armed || injected || declaration.workloadName != "PublishAnimationAuthority") return;
			if((before && action == "BeforeAuthorityExchange") || (!before && action == "AfterAuthorityExchange"))
			{
				Require(animator.IsValid(), "Cancellation requires an actual live animator.");
				AnimationEvaluationExternalGenerations generations;
				generations.consumerGeneration = 68050;
				cancellationCacheFacts["entriesBeforeIngress"] = AnimationSystem::Instance().ReadProductionIntegrationSnapshot().cacheEntryCount;
				const auto status = AnimationSystem::Instance().UpdateEvaluationGenerations(animator, generations);
				Require(status == AnimationStatus::Ok, "Production generation cancellation ingress rejected.");
				cancellationCacheFacts["entriesAfterIngress"] = AnimationSystem::Instance().ReadProductionIntegrationSnapshot().cacheEntryCount;
				injected = true;
			}
		}
		void AppendFallback(const std::string& owner, const char* checkpoint, bool failed = false)
		{
			std::lock_guard lock(mutex);
			// This is the real native Update invoked by the scheduler's own
			// compatibility workload. Its exact descriptor is joined after settle.
			events.push_back({{"frameOrdinal", frame}, {"fallbackSystem", owner}, {"checkpoint", checkpoint},
				{"failed", failed}, {"onCoordinator", GetCurrentThreadId() == coordinatorThread}, {"observedSequence", events.size() + 1U}});
		}
		Json Finish(const FrameExecutionPlan& plan, Json& lane)
		{
			std::lock_guard lock(mutex);
			Json issues = Json::array();
			for(auto& event : events) if(event.contains("fallbackSystem"))
			{
				const auto owner = event.at("fallbackSystem").get<std::string>();
				const FrameWorkloadResult* matched = nullptr;
				for(const auto& result : plan.results) if(result.declaration.ownerSystem == owner)
				{
					Require(!matched, "Native compatibility Update has ambiguous actual plan descriptors."); matched = &result;
				}
				Require(matched && matched->partitionCount == 1 && !matched->declaration.rangeWork,
					"Native compatibility Update has no unique single-partition actual descriptor.");
				event["stableDescriptorId"] = matched->declaration.stableDescriptorId;
				event["executionOrder"] = matched->declaration.executionOrder;
				event["partitionIndex"] = 0; event["partitionCount"] = matched->partitionCount;
				event["itemBegin"] = 0; event["itemEnd"] = matched->declaration.estimatedWorkItemCount;
			}
			std::map<std::string, std::vector<const Json*>> byDescriptor;
			std::map<std::pair<std::string, std::size_t>, std::vector<const Json*>> invocations;
			for(auto& event : events)
			{
				const auto id = event.at("stableDescriptorId").get<std::string>();
				const auto declaration = std::find_if(plan.workloads.begin(), plan.workloads.end(),
					[&](const auto& value) { return value.stableDescriptorId == id; });
				if(declaration == plan.workloads.end()) issues.push_back("unknown-descriptor:" + id);
				else event["executionOrder"] = declaration->executionOrder;
				byDescriptor[id].push_back(&event);
				invocations[{id, event.at("partitionIndex").get<std::size_t>()}].push_back(&event);
			}
			for(const auto& [key, pair] : invocations)
			{
				if(pair.size() != 2 || pair[0]->at("checkpoint") != "Enter" || pair[1]->at("checkpoint") != "Exit"
					|| pair[0]->at("observedSequence").get<std::size_t>() >= pair[1]->at("observedSequence").get<std::size_t>())
					issues.push_back("unpaired-or-reversed:" + key.first + "#" + std::to_string(key.second));
				else for(const auto* field : {"partitionCount", "itemBegin", "itemEnd"})
					if(pair[0]->at(field) != pair[1]->at(field)) issues.push_back("partition-metadata:" + key.first);
			}
			std::size_t completedInvocations = 0, checkedDependencies = 0, omittedDependencies = 0;
			for(const auto& result : plan.results)
			{
				const auto& id = result.declaration.stableDescriptorId;
				if(result.omitted || result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted)
					if(!result.omitted || result.executionPolicy != FrameWorkloadExecutionPolicy::Omitted
						|| !result.completed || result.partitionCount != 0 || byDescriptor.contains(id))
						issues.push_back("contradictory-native-omission:" + id);
				if(result.completed && !result.omitted)
				{
					completedInvocations += result.partitionCount;
					for(std::size_t partition = 0; partition < result.partitionCount; ++partition)
						if(!invocations.contains({id, partition})) issues.push_back("missing-completed-partition:" + id + "#" + std::to_string(partition));
				}
				if(const auto observed = byDescriptor.find(id); observed != byDescriptor.end())
					for(const auto* event : observed->second)
						if(event->at("partitionCount") != result.partitionCount
							|| event->at("partitionIndex").get<std::size_t>() >= result.partitionCount)
							issues.push_back("invalid-partition-count:" + id);
			}
			for(const auto& edge : plan.dependencyEdges)
			{
				if(edge.prerequisiteIndex >= plan.workloads.size() || edge.dependentIndex >= plan.workloads.size())
				{ issues.push_back("invalid-dependency-index"); continue; }
				const auto& beforeId = plan.workloads[edge.prerequisiteIndex].stableDescriptorId;
				const auto& afterId = plan.workloads[edge.dependentIndex].stableDescriptorId;
				const auto after = byDescriptor.find(afterId);
				if(after == byDescriptor.end()) continue;
				const auto prerequisite = std::find_if(plan.results.begin(), plan.results.end(), [&](const auto& result) {
					return result.declaration.stableDescriptorId == beforeId;
				});
				// The native executor retains dependency metadata for intentionally
				// omitted zero-partition barriers. Such a barrier has no callback to
				// precede its dependent; every non-omitted edge still needs real exits.
				if(prerequisite != plan.results.end() && prerequisite->omitted
					&& prerequisite->executionPolicy == FrameWorkloadExecutionPolicy::Omitted
					&& prerequisite->completed && prerequisite->partitionCount == 0 && !byDescriptor.contains(beforeId))
				{ ++omittedDependencies; continue; }
				++checkedDependencies;
				const auto before = byDescriptor.find(beforeId);
				if(before == byDescriptor.end()) { issues.push_back("missing-prerequisite:" + beforeId + "->" + afterId); continue; }
				std::size_t lastExit = 0, firstEnter = std::numeric_limits<std::size_t>::max();
				for(const auto* event : before->second) if(event->at("checkpoint") == "Exit")
					lastExit = std::max(lastExit, event->at("observedSequence").get<std::size_t>());
				for(const auto* event : after->second) if(event->at("checkpoint") == "Enter")
					firstEnter = std::min(firstEnter, event->at("observedSequence").get<std::size_t>());
				if(lastExit == 0 || lastExit >= firstEnter) issues.push_back("dependency-not-settled:" + beforeId + "->" + afterId);
			}
			if(!lane["authoritative"].contains("causality")) lane["authoritative"]["causality"] = Json::array();
			lane["authoritative"]["causality"].push_back({{"frameOrdinal", frame}, {"passed", issues.empty()}, {"issues", issues},
				{"observedInvocationCount", invocations.size()}, {"expectedCompletedInvocationCount", completedInvocations},
				{"checkedDependencyCount", checkedDependencies}, {"omittedDependencyCount", omittedDependencies}});
			if(!issues.empty()) lane["status"] = "Failed";
			lane["diagnostic"]["runtimeIdentifiers"].push_back({{"kind", "ActualBoundaryObservationOrder"}, {"frameOrdinal", frame}, {"events", events}});
			Json ordered = events;
			// Independent worker partitions have no specified wall-clock order.
			// Keep their complete actual chronological order diagnostic, while the
			// authoritative fingerprint follows descriptor/partition/checkpoint.
			std::stable_sort(ordered.begin(), ordered.end(), [](const Json& left, const Json& right) {
				return std::tuple(left.at("frameOrdinal").get<std::size_t>(), left.at("executionOrder").get<std::size_t>(),
					left.at("stableDescriptorId").get<std::string>(), left.at("partitionIndex").get<std::size_t>(),
					left.at("checkpoint") == "Enter" ? 0 : 1)
					< std::tuple(right.at("frameOrdinal").get<std::size_t>(), right.at("executionOrder").get<std::size_t>(),
						right.at("stableDescriptorId").get<std::string>(), right.at("partitionIndex").get<std::size_t>(),
						right.at("checkpoint") == "Enter" ? 0 : 1);
			});
			for(auto& row : ordered) row.erase("observedSequence");
			events = Json::array();
			return ordered;
		}
	};
	class ObservedSystem final : public SystemsBase
	{
	public:
		ObservedSystem(SystemsBase& actual, BoundaryObserver& observer) : actual(actual), observer(observer) {}
		// Engine owns initialization and shutdown. The decorator forwards only
		// frame execution; it never acquires or duplicates a production lifetime.
		void Initialize() override {}
		void Shutdown() override {}
		void Update(float delta) override {
			Require(emptyDeclaration, "Compatibility Update ran without an actual empty native declaration.");
			observer.AppendFallback(actual.SystemName(), "Enter");
			try { actual.Update(delta); }
			catch(...) { observer.AppendFallback(actual.SystemName(), "Exit", true); throw; }
			observer.AppendFallback(actual.SystemName(), "Exit");
		}
		std::string SystemName() override { return actual.SystemName(); }
		SystemDeclaration SchedulerDeclaration() override { return actual.SchedulerDeclaration(); }
		std::vector<FrameWorkloadDeclaration> DeclareFrameWorkloads(float delta) override
		{
			auto declarations = actual.DeclareFrameWorkloads(delta);
			emptyDeclaration = declarations.empty();
			for(auto& declaration : declarations)
			{
				// Metadata and ranges remain the exact actual producer values. A
				// small copied descriptor supplies observer identity, never work.
				auto identity = declaration;
				identity.work = {}; identity.rangeWork = {};
				if(declaration.work)
				{
					auto original = std::move(declaration.work);
					declaration.work = [this, original = std::move(original), identity](JobGraphContext& context) {
						const FrameWorkloadPartition partition{0, 1, 0, identity.estimatedWorkItemCount};
						observer.Append(identity, partition, "Enter");
						try
						{
							observer.Probe(identity, true);
							original(context);
							observer.Probe(identity, false);
							if(observer.armed && observer.action == "ThrowAtGeometryRoutePrepare"
								&& identity.workloadName == "GeometryRoutePrepare")
							{
								observer.injected = true;
								observer.faultRanOnWorker = GetCurrentThreadId() != observer.coordinatorThread;
								throw std::runtime_error("frame-replay-real-workload-fault");
							}
						}
						catch(...) { observer.Append(identity, partition, "Exit", true); throw; }
						observer.Append(identity, partition, "Exit");
					};
				}
				if(declaration.rangeWork)
				{
					auto original = std::move(declaration.rangeWork);
					declaration.rangeWork = [this, original = std::move(original), identity](const FrameWorkloadPartition& partition, JobGraphContext& context) {
						observer.Append(identity, partition, "Enter");
						try { original(partition, context); }
						catch(...) { observer.Append(identity, partition, "Exit", true); throw; }
						observer.Append(identity, partition, "Exit");
					};
				}
			}
			return declarations;
		}
	private:
		SystemsBase& actual;
		BoundaryObserver& observer;
		bool emptyDeclaration = false;
	};

	struct Options
	{
		Path installRoot, projectRoot, assetManifest, projectClosure, manifest, output, expectedEnginePath;
		std::string scenario, runNonce, expectedEngineSha256;
		std::size_t workers = 0;
	};
	struct EngineLifetime
	{
		Engine& engine;
		bool entered = false;
		void Close()
		{
			if(!entered) return;
			engine.Shutdown();
			entered = false;
		}
		~EngineLifetime()
		{
			// Both matching Engine versions provide RAII shutdown. This explicit
			// guard also gives the fixture a deterministic close boundary for its
			// held-root observations, including exceptions during Start.
			if(entered) try { engine.Shutdown(); } catch(...) {}
		}
	};
	Options Parse(int argc, char** argv)
	{
		std::map<std::string, std::string> values;
		for(int index = 1; index < argc; index += 2)
		{
			Require(index + 1 < argc, "Every replay option requires one value.");
			Require(values.emplace(argv[index], argv[index + 1]).second, "Duplicate replay option.");
		}
		const std::vector<std::string> keys{"--install-root", "--project-root", "--asset-manifest", "--project-closure", "--manifest", "--scenario", "--workers", "--output", "--run-nonce", "--expected-engine-sha256", "--expected-engine-path"};
		Require(values.size() == keys.size(), "Replay requires exactly the documented CLI options.");
		for(const auto& key : keys) Require(values.contains(key) && !values.at(key).empty(), "Missing replay CLI option.");
		Options result;
		result.installRoot = Path(values.at("--install-root")); result.projectRoot = Path(values.at("--project-root"));
		result.assetManifest = Path(values.at("--asset-manifest")); result.projectClosure = Path(values.at("--project-closure"));
		result.manifest = Path(values.at("--manifest")); result.output = Path(values.at("--output"));
		result.scenario = values.at("--scenario"); result.runNonce = values.at("--run-nonce");
		result.expectedEngineSha256 = values.at("--expected-engine-sha256");
		result.expectedEnginePath = Path(values.at("--expected-engine-path"));
		Require(result.expectedEnginePath.is_absolute(), "Expected engine path must be an absolute trusted parent pin.");
		Require(result.expectedEngineSha256.size() == 64 && std::all_of(result.expectedEngineSha256.begin(), result.expectedEngineSha256.end(),
			[](char character) { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); }),
			"Expected engine SHA256 must be a canonical lowercase trusted parent pin.");
		Require(values.at("--workers") == "1" || values.at("--workers") == "2" || values.at("--workers") == "8", "Replay worker count must be 1, 2, or 8.");
		result.workers = static_cast<std::size_t>(std::stoul(values.at("--workers")));
		Require(!std::filesystem::exists(result.output), "Replay output already exists.");
		return result;
	}
	std::vector<Path> AssetManifestPaths(const Options& options)
	{
		// Sort only the input-file inventory, never a published engine array.
		// Importer-emitted grid manifests carry genuine virtual geometry bindings;
		// this avoids assuming the small Hero mesh has page-backed geometry.
		std::vector<Path> generated;
		const auto directory = options.projectRoot / "Build/Assets/.terra-animation-manifests";
		for(const auto& entry : std::filesystem::directory_iterator(directory))
			if(entry.is_regular_file() && entry.path().extension() == ".json") generated.push_back(entry.path());
		std::sort(generated.begin(), generated.end());
		std::vector<Path> result{options.assetManifest};
		result.insert(result.end(), generated.begin(), generated.end());
		return result;
	}
	void VerifyProjectClosure(const Options& options, const Json& scenario)
	{
		// This standard-only check runs before Engine construction. A portable
		// sealed closure can verify isolated reference/candidate project copies;
		// its relative file names and bytes, rather than an old absolute root,
		// identify the actual authored input. The parent still owns import proof.
		const auto closure = ReadJson(options.projectClosure);
		Require(closure.at("schemaVersion") == "frame-replay-project-closure-v1", "Unsupported replay project closure.");
		Require(closure.at("profile") == scenario.at("inputProfile"), "Project closure profile differs from frozen workload.");
		Require(closure.at("files").is_array() && !closure.at("files").empty(), "Project closure has no actual file inventory.");
		const auto root = std::filesystem::canonical(options.projectRoot);
		std::set<std::string> expected;
		for(const auto& row : closure.at("files"))
		{
			const auto relative = Path(row.at("path").get<std::string>());
			Require(!relative.empty() && !relative.is_absolute(), "Project closure file must be project-relative.");
			for(const auto& part : relative) Require(part != "..", "Project closure file escapes project.");
			const auto actualRelative = std::filesystem::relative(std::filesystem::canonical(root / relative), root);
			for(const auto& part : actualRelative) Require(part != "..", "Project closure symlink escapes project.");
			Require(expected.insert(relative.lexically_normal().generic_string()).second, "Project closure repeats a file.");
			const auto actual = FilePin(root / relative);
			Require(actual.at("sha256") == row.at("sha256") && actual.at("bytes") == row.at("bytes"), "Project input bytes differ from sealed closure.");
		}
		Require(expected.contains("Assets/Scenes/Scene.tescene"), "Project closure omits the actual authored scene.");
		std::set<std::string> actual;
		for(const auto& entry : std::filesystem::recursive_directory_iterator(root)) if(entry.is_regular_file())
			actual.insert(entry.path().lexically_relative(root).generic_string());
		Require(actual == expected, "Project has unsealed or missing input files.");
	}
	void RegisterAssets(const Options& options)
	{
		const std::map<std::string, AssetType> types{{"Mesh", AssetType::Mesh}, {"DeformableMesh", AssetType::Mesh}, {"Material", AssetType::Material},
			{"Skeleton", AssetType::Skeleton}, {"AnimationClip", AssetType::AnimationClip}};
		const auto cooked = options.projectRoot / "Build/Assets";
		for(const auto& path : AssetManifestPaths(options))
		{
			// Keep the document alive while iterating its referenced child array.
			// C++20 does not extend a temporary root through json::at().
			const auto assetDocument = ReadJson(path);
			for(const auto& row : assetDocument.at("subAssets"))
			{
				const auto kind = row.at("kind").get<std::string>();
				if(!types.contains(kind)) continue;
				const auto id = TerraEngine::ParseUUID(row.at("guid").get<std::string>());
				Require(id.has_value(), "Cooked asset has an invalid authored GUID.");
				const auto relative = Path(row.at("compiledPath").get<std::string>());
				Require(!relative.is_absolute(), "Cooked resource must remain within the qualified project.");
				for(const auto& part : relative) Require(part != "..", "Cooked resource escapes qualified project.");
				ResourceManager::Instance().RegisterAssetPath(*id, cooked / relative, types.at(kind));
			}
		}
	}
	void Start(Engine& engine, const Options& options, Json& lane)
	{
		const auto start = std::chrono::steady_clock::now();
		const auto deadline = start + std::chrono::milliseconds(60000);
		struct ReadinessTiming {
			Json& lane; std::chrono::steady_clock::time_point start;
			~ReadinessTiming() { lane["diagnostic"]["timings"].push_back({{"kind", "StartReadiness"}, {"elapsedNanoseconds",
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()}}); }
		} readinessTiming{lane, start};
		ResourceManager::Instance().SetEngineRoot(options.installRoot / "Editor");
		ResourceManager::Instance().SetAssetRoot(options.projectRoot / "Build/Assets");
		// Engine.Initialize preserves an already initialized pool in both matched
		// runtimes. Establish the requested workers first rather than replacing an
		// executor after systems and asynchronous resource owners have entered it.
		Require(ThreadPool::instance().getThreadCount() == 0, "Replay must start with a quiescent uninitialized singleton executor.");
		ThreadPool::instance().initialize(options.workers);
		Require(ThreadPool::instance().getThreadCount() == options.workers, "Requested replay pool could not be initialized.");
		engine.Initialize();
		Require(ThreadPool::instance().getThreadCount() == options.workers, "Actual replay pool does not match requested workers.");
		RegisterAssets(options);
		auto& scenes = SceneManager::Instance();
		const auto scene = scenes.LoadScene(options.projectRoot / "Assets/Scenes/Scene.tescene");
		Require(scenes.SetActiveScene(scene), "Qualified replay scene could not be activated.");
		// Registration alone does not prove resident native payloads. Complete each
		// unique authored dependency before admitting the next so setup has a
		// deterministic publication sequence. Measured callbacks still use the
		// requested 1/2/8-worker pool; only this pre-bootstrap readiness is ordered.
		struct Dependency { TerraEngine::UUID id; AssetType type; std::shared_future<TerraEngine::UUID> completion; };
		std::vector<Dependency> dependencies;
		auto& resources = ResourceManager::Instance();
		lane["authoritative"]["resourceReadiness"].push_back({{"deadlineMilliseconds", 60000}, {"resources", Json::array()}});
		auto& proof = lane["authoritative"]["resourceReadiness"].back()["resources"];
		const auto admit = [&](const TerraEngine::UUID& id, AssetType type) {
			if(std::chrono::steady_clock::now() >= deadline) throw MissingPrerequisite("Start readiness deadline expired before admission.");
			if(id == TerraEngine::EMPTY_UUID) return;
			if(std::any_of(dependencies.begin(), dependencies.end(), [&](const Dependency& value) { return value.id == id && value.type == type; })) return;
			const auto path = resources.GetRegisteredAssetPath(id, type);
			if(path.empty()) throw MissingPrerequisite("Authored dependency has no registered native path: id="
				+ TerraEngine::ToHexString(id) + ", type=" + std::to_string(static_cast<unsigned>(type)));
			const auto relative = std::filesystem::relative(path, options.projectRoot);
			for(const auto& part : relative) if(part == "..") throw MissingPrerequisite("Authored native dependency escapes sealed project.");
			auto pin = FilePin(path); pin.erase("path");
			proof.push_back({{"assetId", TerraEngine::ToHexString(id)}, {"assetType", static_cast<unsigned>(type)},
				{"path", relative.generic_string()}, {"file", std::move(pin)}, {"status", "Pending"}});
			std::shared_future<TerraEngine::UUID> future;
			if(type == AssetType::Mesh) future = resources.LoadMeshAsync(path);
			else if(type == AssetType::Material) future = resources.LoadMaterialAsync(path);
			else future = resources.LoadAnimationAssetAsync(path, type);
			dependencies.push_back({id, type, std::move(future)});
			// Retain and check this actual asynchronous receipt before the next
			// admission. Waiting later in vector order cannot order publications
			// that were already admitted concurrently. Keep the original deadline.
			auto& admitted = dependencies.back();
			if(!admitted.completion.valid() || admitted.completion.wait_until(deadline) != std::future_status::ready)
				throw MissingPrerequisite("Authored native dependency exceeded fixed Start readiness deadline.");
			if(admitted.completion.get() != admitted.id)
				throw MissingPrerequisite("Authored native dependency returned a different canonical ID.");
		};
		try {
			ObjectManager::instance().forEachObjectWithComponents<MeshRenderComponent>([&](Object, const MeshRenderComponent& mesh) {
				admit(mesh.meshId, AssetType::Mesh); admit(mesh.materialId, AssetType::Material);
			});
			ObjectManager::instance().forEachObjectWithComponents<AnimatorComponent>([&](Object, const AnimatorComponent& animator) {
				admit(animator.skeletonOverrideAssetId, AssetType::Skeleton); admit(animator.defaultClipAssetId, AssetType::AnimationClip);
			});
			for(std::size_t index = 0; index < dependencies.size(); ++index) {
				auto& dependency = dependencies[index]; auto& row = proof[index];
				if(!dependency.completion.valid() || dependency.completion.wait_until(deadline) != std::future_status::ready)
					throw MissingPrerequisite("Authored native dependency exceeded fixed Start readiness deadline.");
				const auto actual = dependency.completion.get(); row["nativeId"] = TerraEngine::ToHexString(actual);
				if(actual != dependency.id) throw MissingPrerequisite("Authored native dependency returned a different canonical ID.");
				const auto publication = resources.TryCopyRendererEfficiencyResourcePublication(dependency.id, dependency.type);
				if(!publication || !publication->present || publication->assetId != dependency.id || publication->resourceGeneration == 0)
					throw MissingPrerequisite("Authored native dependency has no exact immutable publication.");
				if(dependency.type == AssetType::Mesh && !resources.AcquireMeshSnapshot(dependency.id))
					throw MissingPrerequisite("Authored mesh immutable owner is absent after completion.");
				if(dependency.type == AssetType::Material && !resources.AcquireMaterialSnapshot(dependency.id))
					throw MissingPrerequisite("Authored material immutable owner is absent after completion.");
				if(std::chrono::steady_clock::now() >= deadline) throw MissingPrerequisite("Start readiness deadline expired during publication checks.");
				row["publicationGeneration"] = publication->resourceGeneration; row["status"] = "Published";
			}
		} catch(const std::exception& error) {
			lane["authoritative"]["resourceReadiness"].back()["failure"] = error.what();
			throw MissingPrerequisite(error.what());
		}
		AnimationSystem::Instance().SetAuthoredComponentPlaybackEnabled(true);
		AnimationSystem::Instance().SetAuthoredComponentPlaybackAdvancing(true);
	}
	AnimatorInstanceHandle FirstAnimator(const std::shared_ptr<const AnimationAuthorityFrame>& frame)
	{
		if(frame && frame->pages) for(const auto& page : *frame->pages) if(page)
			for(const auto& row : page->rows) if(row.active) return {row.key.slot, row.key.lifetimeGeneration};
		return {};
	}
	TerraEngine::UUID FirstObject(const std::shared_ptr<const RenderFramePublication>& frame)
	{
		if(frame && frame->persistent) for(const auto& page : frame->persistent->pages) if(page)
			for(const auto& row : page->slots) if(row.occupied) return row.objectId;
		return TerraEngine::EMPTY_UUID;
	}
	Json ProductionAdmission()
	{
		const auto integration = AnimationSystem::Instance().ReadProductionIntegrationSnapshot();
		Json value = {{"planStatus", static_cast<unsigned>(integration.planStatus)},
			{"rejectionReason", static_cast<unsigned>(integration.rejectionReason)}, {"rejectionItemIndex", integration.rejectionItemIndex},
			{"executionPolicy", static_cast<unsigned>(integration.executionPolicy)}, {"availableWorkerCount", integration.availableWorkerCount},
			{"selectedWorkerCount", integration.selectedWorkerCount}, {"itemCount", integration.itemCount}, {"rangeCount", integration.rangeCount},
			{"scratchBytes", integration.scratchBytes}, {"cacheEntryCount", integration.cacheEntryCount},
			{"reusedItemCount", integration.reusedItemCount}, {"fallbackItemCount", integration.fallbackItemCount},
			{"deterministicPublicationValidated", integration.deterministicPublicationValidated},
			{"cancellationRequested", integration.cancellationRequested}, {"counters", nullptr}};
		if(const auto diagnostics = AcquireAnimationDiagnosticsSnapshot())
		{
			const auto& counters = diagnostics->counters;
			value["counters"] = {{"activeAnimators", counters.activeAnimators},
				{"compactEligibleAnimators", counters.compactEligibleAnimators}, {"compactProcessedAnimators", counters.compactProcessedAnimators},
				{"completeCapturedAnimators", counters.completeCapturedAnimators}, {"completeProcessedAnimators", counters.completeProcessedAnimators}};
		}
		return value;
	}
	struct CapturedInputsObserver
	{
		std::mutex mutex;
		Json latest = nullptr;
		void Observe(const RenderFrameScratchValidationEvent& event)
		{
			if(event.checkpoint != RenderFrameScratchValidationCheckpoint::Captured) return;
			Json value = {{"frameId", event.episode.frameId}, {"cameras", Json::array()}, {"lights", Json::array()}};
			if(event.capturedInputs)
			{
				for(const auto& camera : event.capturedInputs->cameras) value["cameras"].push_back({
					{"objectId", TerraEngine::ToHexString(camera.objectId)}, {"verticalFovBits", Bits(camera.camera.verticalFovDegrees)},
					{"aspectRatioBits", Bits(camera.camera.aspectRatio)}, {"nearPlaneBits", Bits(camera.camera.nearPlane)},
					{"farPlaneBits", Bits(camera.camera.farPlane)}, {"upBits", VectorBits(camera.camera.up)}});
				for(const auto& light : event.capturedInputs->lights)
				{
					std::uint64_t digest = 1469598103934665603ULL;
					AppendScratchLightValidationDigest(digest, light.objectId, light.light);
					value["lights"].push_back({{"objectId", TerraEngine::ToHexString(light.objectId)}, {"typedValueDigest", digest}});
				}
			}
			std::lock_guard lock(mutex); latest = std::move(value);
		}
		Json Copy() { std::lock_guard lock(mutex); return latest; }
	};
	Json Renderables()
	{
		Json result = Json::array();
		for(const auto& row : RenderSystem::Instance().GetLatestRenderablesSnapshot()) result.push_back({
			{"objectId", TerraEngine::ToHexString(row.objectId)}, {"meshId", TerraEngine::ToHexString(row.meshId)},
			{"materialId", TerraEngine::ToHexString(row.materialId)}, {"worldPose", WorldPose(row.canonicalWorldPose)},
			{"transformGeneration", row.transformGeneration}, {"visibilityIdentityIndex", row.conventionalVisibilityIdentityIndex},
			{"taskShaderCullingEnabled", row.taskShaderCullingEnabled}, {"usingFallback", row.usingFallback}});
		return result;
	}
	Json SceneMeshInputs()
	{
		// This is a protected scene INPUT observation, deliberately distinct from
		// capturedRenderables. The baseline no-output branch returns before it
		// constructs renderer renderable records, so those outputs stay empty.
		Json result = Json::array();
		ObjectManager::instance().forEachObjectWithComponents<MeshRenderComponent>([&](Object object, const MeshRenderComponent& mesh) {
			const auto* transform = ObjectManager::instance().GetComponent<::Transform>(object);
			const auto* terrain = ObjectManager::instance().GetComponent<TerrainComponent>(object);
			result.push_back({{"objectId", TerraEngine::ToHexString(ObjectManager::instance().getUUID(object))},
				{"meshId", TerraEngine::ToHexString(mesh.meshId)}, {"materialId", TerraEngine::ToHexString(mesh.materialId)},
				{"transformPresent", transform != nullptr}, {"transformActive", transform && IsRuntimeComponentActive(*transform)},
				{"terrainPresent", terrain != nullptr}, {"terrainActive", terrain && IsRuntimeComponentActive(*terrain)},
				{"objectActive", ObjectManager::instance().isObjectActiveInHierarchy(object)},
				{"spatialObjectId", TerraEngine::ToHexString(mesh.spatialObjectId)}, {"taskShaderCullingEnabled", mesh.taskShaderCullingEnabled}});
		});
		return result;
	}
	enum class ExpectedFrameFailure { None, InjectedGeometryWorker };
	Json ObserveMultiOutput(const Json& capturedInputs, const std::shared_ptr<const RenderFramePublication>& root, const Json& control);
	void CaptureFrame(Json& lane, std::size_t ordinal, const FrameExecutionPlan& plan, const ScheduleSummary& summary,
		const AnimationCommandReceipt& command, CapturedInputsObserver& captured,
		ExpectedFrameFailure expectedFailure = ExpectedFrameFailure::None, const JobGraphSummary* graph = nullptr)
	{
		auto scheduler = Plan(plan, summary);
		scheduler["jobGraph"] = graph ? Graph(*graph) : Json(nullptr);
		const auto diagnostics = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
		const auto game = RenderSystem::Instance().GetLatestRenderStats(RenderViewType::Game);
		const auto scene = RenderSystem::Instance().GetLatestRenderStats(RenderViewType::Scene);
		const auto scratch = RenderSystem::Instance().GetFrameScratchValidationState();
		lane["authoritative"]["frames"].push_back({{"ordinal", ordinal},
			{"renderer", RenderRoot(LoadReplayRenderPublication(RenderSystem::Instance()))},
			{"capturedRenderables", Renderables()}, {"capturedInputs", captured.Copy()}, {"sceneMeshInputs", SceneMeshInputs()},
			{"outputObservation", {{"diagnosticsFrameId", diagnostics.frameId}, {"diagnosticsValid", diagnostics.valid},
				{"diagnosticsPublicationGeneration", diagnostics.publicationGeneration}, {"viewSubmitWorkCount", diagnostics.viewSubmitWork.size()},
				{"gameStatsValid", game.valid}, {"sceneStatsValid", scene.valid},
				{"scratchCompleted", !scratch.active && scratch.latest.outcome == RenderFrameScratchOutcome::Completed}}},
			{"animation", AnimationRoot(AnimationSystem::Instance().LoadPublishedAuthorityFrame())},
			{"animationAdmission", ProductionAdmission()}, {"scheduler", std::move(scheduler)},
			{"commandReceipts", Json::array({Command(command)})}, {"scratch", Scratch(RenderSystem::Instance().GetFrameScratchValidationState())}});
		if(lane["authoritative"].contains("sceneViewControl"))
			lane["authoritative"]["frames"].back()["multiOutputObservation"] = ObserveMultiOutput(captured.Copy(),
				LoadReplayRenderPublication(RenderSystem::Instance()), lane["authoritative"]["sceneViewControl"]);
		lane["diagnostic"]["timings"].push_back({{"frameOrdinal", ordinal},
			{"workerPreparationTimeNs", plan.migrationSummary.workerPreparationTimeNs},
			{"protectedCommitTimeNs", plan.migrationSummary.protectedCommitTimeNs}});
		if(const auto root = LoadReplayRenderPublication(RenderSystem::Instance()))
			for(std::size_t view = 0; view < root->presentationInputs.size(); ++view)
			{
				const auto& input = root->presentationInputs[view];
				const auto& timing = input.renderOptimization;
				lane["diagnostic"]["timings"].push_back({{"kind", "PublishedRenderOptimizationTiming"}, {"frameOrdinal", ordinal},
					{"viewOrdinal", view}, {"frameId", root->frameId}, {"outputId", input.output.outputId},
					{"topologyBuildNanoseconds", timing.topologyBuildNanoseconds}, {"dynamicMaterializationNanoseconds", timing.dynamicMaterializationNanoseconds},
					{"instanceStreamUploadNanoseconds", timing.instanceStreamUploadNanoseconds}, {"groupedCommandRecordingNanoseconds", timing.groupedCommandRecordingNanoseconds}});
				const auto addLatency = [&](const char* carrier, const std::vector<VirtualGeometryCounterReadbackRecord>& counters) {
					for(std::size_t index = 0; index < counters.size(); ++index) lane["diagnostic"]["timings"].push_back({
						{"kind", "PublishedVirtualGeometryCompletionTiming"}, {"frameOrdinal", ordinal}, {"viewOrdinal", view},
						{"outputId", input.output.outputId}, {"carrier", carrier}, {"recordIndex", index},
						{"counterIdentityAndValues", PublishedValue(counters[index])}, {"latencyFrames", counters[index].latencyFrames}});
				};
				addLatency("virtualGeometryCounterReadbacks", input.virtualGeometryCounterReadbacks);
				if(input.retainedVirtualGeometryGpuEvidence) addLatency("retainedVirtualGeometryGpuEvidence.counterReadbacks", input.retainedVirtualGeometryGpuEvidence->counterReadbacks);
			}
		// The injected worker case must preserve its real failed plan. Only its
		// precise first-frame failure is expected; unrelated errors still reject.
		const bool exactInjectedFailure = expectedFailure == ExpectedFrameFailure::InjectedGeometryWorker
			&& lane.at("laneId") == "ProducerBoundaries" && ordinal == 0 && !plan.valid && !plan.completed
			&& plan.firstFailure && plan.firstFailure->code == ArchitectureDiagnosticCode::ExecutionFailed
			&& plan.firstFailure->workloadName == "GeometryRoutePrepare"
			&& plan.firstFailure->resourceName == "RenderSystem/Render/GeometryRoutePrepare#0"
			&& plan.firstFailure->message == "Job threw during worker execution: frame-replay-real-workload-fault.";
		Require(summary.valid && (expectedFailure == ExpectedFrameFailure::None ? plan.valid : exactInjectedFailure)
			&& !plan.workloads.empty() && !plan.results.empty(),
			"Measured execution has an invalid schedule or empty native workload/result plan.");
		for(const char* target : {"PublishAnimationAuthority", "RenderPublicationCapture", "GeometryRoutePrepare", "VirtualGeometryPrepare", "BackendCommit", "RenderPublication"})
			Require(std::any_of(plan.workloads.begin(), plan.workloads.end(), [&](const auto& work) { return work.workloadName == target; }),
				"Measured execution omitted a required native producer descriptor.");
	}
	void ConfigureMultiOutput(Json& lane)
	{
		// Two authored runtime cameras still produce one MainRuntimeView. Request
		// a genuine second Scene output through the maintained public API, using
		// the actual second authored camera and its exact canonical world pose.
		const auto id = TerraEngine::ParseUUID("068c0000000000000000000000000002");
		const auto object = ObjectManager::instance().getObject(*id);
		if(!object) throw MissingPrerequisite("Multi-output second authored camera is absent.");
		const auto* camera = ObjectManager::instance().GetComponent<CameraComponent>(*object);
		const auto pose = ObjectManager::instance().GetResolvedWorldPose(*object);
		if(!camera || !camera->active || !pose) throw MissingPrerequisite("Multi-output camera or canonical pose is unavailable.");
		bool active = false;
		ObjectManager::instance().forEachObjectWithComponents<::Transform, CameraComponent>([&](Object candidate, const ::Transform&, const CameraComponent&) {
			if(candidate == *object) active = true;
		});
		if(!active) throw MissingPrerequisite("Multi-output authored camera is not in actual active camera membership.");
		const auto finite = [](double value) { return std::isfinite(value); };
		Require(finite(camera->verticalFovDegrees) && finite(camera->aspectRatio) && finite(camera->nearPlane) && finite(camera->farPlane)
			&& finite(pose->rotation.x) && finite(pose->rotation.y) && finite(pose->rotation.z) && finite(pose->rotation.w)
			&& finite(pose->position.offsetMetres.x) && finite(pose->position.offsetMetres.y) && finite(pose->position.offsetMetres.z)
			&& finite(pose->scale.x) && finite(pose->scale.y) && finite(pose->scale.z)
			&& finite(camera->up.x) && finite(camera->up.y) && finite(camera->up.z) && glm::length(pose->rotation) > 0.0001F,
			"Multi-output camera has nonfinite authored values.");
		const auto rotation = glm::normalize(pose->rotation);
		const auto forward = rotation * glm::vec3(0, 0, -1);
		auto up = rotation * camera->up;
		if(glm::length(up) < 0.0001F) up = glm::vec3(0, 1, 0);
		const auto view = glm::lookAt(glm::vec3(0), forward, glm::normalize(up));
		const auto nearPlane = std::max(0.0001F, camera->nearPlane);
		auto projection = glm::perspectiveRH_ZO(glm::radians(std::max(0.001F, camera->verticalFovDegrees)),
			camera->aspectRatio > 0 ? camera->aspectRatio : 1.0F, nearPlane, std::max(nearPlane + 0.001F, camera->farPlane));
		projection[1][1] *= -1;
		const auto window = WindowSystem::Instance().GetLatestWorkerSnapshot();
		Require(window.workerReadable && window.width > 0 && window.height > 0, "Actual protected window extent is unavailable.");
		const glm::uvec2 extent(static_cast<unsigned>(window.width), static_cast<unsigned>(window.height));
		auto& renderer = RenderSystem::Instance();
		renderer.SetSceneViewTargetSize(extent);
		renderer.SetSceneViewCamera(view, projection, pose->position, false);
		lane["authoritative"]["sceneViewControl"] = {{"authoredCameraId", TerraEngine::ToHexString(*id)}, {"canonicalPose", WorldPose(*pose)},
			{"viewMatrixBits", MatrixBits(view)}, {"projectionMatrixBits", MatrixBits(projection)}, {"targetExtent", {extent.x, extent.y}},
			{"extentSource", "WindowSystem.GetLatestWorkerSnapshot"}, {"runtimePrimaryOverride", false}};
	}
	Json ObserveMultiOutput(const Json& capturedInputs, const std::shared_ptr<const RenderFramePublication>& root, const Json& control)
	{
		// Native runtime submission iterates the captured camera array once and
		// emits MainRuntimeView/output2 for each camera. That output ID names a
		// destination, not a unique camera. Preserve every ordered publication.
		Json proof = {{"association", "CapturedCameraOrderToNativeRuntimeSubmissionOrder"},
			{"expectedAuthoredCameraIds", {"068c0000000000000000000000000001", "068c0000000000000000000000000002"}},
			{"scenePresentationIndex", 0}, {"cameraObservations", Json::array()}, {"rangeViewIndices", Json::array()}};
		const auto& cameras = capturedInputs.at("cameras");
		bool valid = root && cameras.size() == 2 && root->views.size() == cameras.size() + 1
			&& root->presentationInputs.size() == root->views.size();
		std::set<std::string> ids;
		if(root) {
			for(const auto& range : root->submissionRanges) {
				proof["rangeViewIndices"].push_back(range.viewIndex);
				valid = valid && range.viewIndex < root->views.size();
			}
			for(std::size_t index = 0; index < root->views.size(); ++index) {
				const auto& view = root->views[index];
				if(index >= root->presentationInputs.size()) { valid = false; continue; }
				const auto& input = root->presentationInputs[index];
				const auto kind = index == 0 ? RenderOutputKind::SceneView : RenderOutputKind::MainRuntimeView;
				const auto type = index == 0 ? RenderViewType::Scene : RenderViewType::Game;
				const std::uint32_t output = index == 0 ? 1 : 2;
				valid = valid && view.outputKind == kind && view.viewType == type && view.outputId == output
					&& input.output.kind == kind && input.output.diagnosticsView == type && input.output.outputId == output;
			}
			if(!root->presentationInputs.empty()) valid = valid
				&& MatrixBits(root->presentationInputs[0].viewMatrix) == control.at("viewMatrixBits")
				&& MatrixBits(root->presentationInputs[0].projectionMatrix) == control.at("projectionMatrixBits");
		}
		for(std::size_t ordinal = 0; ordinal < cameras.size(); ++ordinal) {
			const auto& captured = cameras[ordinal];
			const auto textId = captured.at("objectId").get<std::string>();
			ids.insert(textId);
			const auto id = TerraEngine::ParseUUID(textId);
			const auto object = id ? ObjectManager::instance().getObject(*id) : std::nullopt;
			const auto* camera = object ? ObjectManager::instance().GetComponent<CameraComponent>(*object) : nullptr;
			const auto pose = object ? ObjectManager::instance().GetResolvedWorldPose(*object) : std::nullopt;
			bool active = false;
			ObjectManager::instance().forEachObjectWithComponents<::Transform, CameraComponent>([&](Object candidate, const ::Transform&, const CameraComponent&) {
				if(object && candidate == *object) active = true;
			});
			const bool cameraEnabled = camera && camera->active;
			active = active && cameraEnabled;
			Json row = {{"cameraOrdinal", ordinal}, {"presentationIndex", ordinal + 1}, {"capturedCamera", captured},
				{"activeTransformCamera", active}, {"cameraEnabled", cameraEnabled}, {"protectedCamera", nullptr}, {"canonicalPose", nullptr},
				{"derivedViewMatrix", nullptr}, {"derivedProjectionMatrix", nullptr}};
			if(camera && pose && active) {
				row["protectedCamera"] = {{"objectId", textId}, {"verticalFovBits", Bits(camera->verticalFovDegrees)},
					{"aspectRatioBits", Bits(camera->aspectRatio)}, {"nearPlaneBits", Bits(camera->nearPlane)},
					{"farPlaneBits", Bits(camera->farPlane)}, {"upBits", VectorBits(camera->up)}};
				row["canonicalPose"] = WorldPose(*pose);
				const auto rotation = glm::normalize(pose->rotation);
				const auto forward = rotation * glm::vec3(0, 0, -1);
				auto up = rotation * camera->up;
				if(glm::length(up) < 0.0001f) up = glm::vec3(0, 1, 0);
				const auto view = glm::lookAt(glm::vec3(0), forward, glm::normalize(up));
				const float nearPlane = std::max(0.0001f, camera->nearPlane);
				auto projection = glm::perspectiveRH_ZO(glm::radians(std::max(0.001f, camera->verticalFovDegrees)),
					camera->aspectRatio > 0 ? camera->aspectRatio : 1.0f, nearPlane, std::max(nearPlane + 0.001f, camera->farPlane));
				projection[1][1] *= -1;
				row["derivedViewMatrix"] = PublishedValue(view); row["derivedProjectionMatrix"] = PublishedValue(projection);
				valid = valid && row.at("protectedCamera") == captured && root && ordinal + 1 < root->presentationInputs.size();
				if(root && ordinal + 1 < root->presentationInputs.size()) {
					const auto& input = root->presentationInputs[ordinal + 1];
					valid = valid && row.at("derivedViewMatrix") == PublishedValue(input.viewMatrix)
						&& row.at("derivedProjectionMatrix") == PublishedValue(input.projectionMatrix);
				}
			} else valid = false;
			proof["cameraObservations"].push_back(std::move(row));
		}
		valid = valid && ids == std::set<std::string>{"068c0000000000000000000000000001", "068c0000000000000000000000000002"};
		proof["passed"] = valid;
		return proof;
	}
	void QualifyProfile(Json& lane, const Json& scenario, const char* checkpoint = "AfterMeasuredFrames", std::size_t frameOrdinal = 2)
	{
		std::size_t direct = 0, virtualGeometry = 0;
		const auto root = LoadReplayRenderPublication(RenderSystem::Instance());
		if(root) for(std::size_t view = 0; view < root->views.size(); ++view)
		{
			const auto resolver = MakeRenderViewCommandResolver(*root, view);
			for(std::size_t index = 0; index < resolver.Count(); ++index)
			{
				RenderItemRecord command{};
				if(!resolver.Resolve(index, command)) continue;
				if(command.UsesVirtualGeometryRoute()) ++virtualGeometry;
				else ++direct;
			}
		}
		const auto diagnostics = AcquireAnimationDiagnosticsSnapshot();
		const auto complete = diagnostics ? diagnostics->counters.completeProcessedAnimators : 0;
		const auto compact = diagnostics ? diagnostics->counters.compactProcessedAnimators : 0;
		const auto profile = scenario.at("inputProfile").get<std::string>();
		const bool routes = scenario.at("id") == "no-output" || (profile == "DirectComplete"
			? direct != 0 && virtualGeometry == 0 : profile == "VirtualCompact" ? virtualGeometry != 0 : direct != 0 || virtualGeometry != 0);
		const bool noOutput = scenario.at("id") == "no-output";
		const bool animation = noOutput ? FirstAnimator(AnimationSystem::Instance().LoadPublishedAuthorityFrame()).IsValid()
			: profile == "DirectComplete" ? complete != 0 && compact == 0
			: profile == "VirtualCompact" ? compact != 0 : complete != 0 && compact != 0;
		lane["authoritative"]["profileObservation"] = {{"requestedProfile", profile}, {"directCommands", direct},
			{"virtualCommands", virtualGeometry}, {"completeProcessedAnimators", complete}, {"compactProcessedAnimators", compact},
			{"checkpoint", checkpoint}, {"frameOrdinal", frameOrdinal}, {"nativePublicationFrameId", root ? Json(root->frameId) : Json(nullptr)},
			{"nativePublicationGeneration", root ? Json(root->generation) : Json(nullptr)},
			{"nativePublicationReceiptGeneration", root ? Json(root->receipt.publicationGeneration) : Json(nullptr)}};
		const bool priorFailure = lane.at("status") == "Failed";
		Assertion(lane, "profile-observed", true, routes && animation);
		if(!(routes && animation) && !priorFailure)
		{
			lane["status"] = "Incomplete";
			lane["unavailableCapability"] = "Actual qualified scene did not demonstrate the requested geometry/animation admission profile.";
		}
	}
	void RunLane(Json& lane, const Options& options, const Json& manifest, const Json& scenario)
	{
		Require(manifest.at("readinessPolicy") == "AuthoredNativeDependenciesExactPublicationBeforeBootstrap"
			&& manifest.at("readinessDeadlineMilliseconds") == 60000, "Unsupported fixed native readiness policy.");
		using namespace TerraEngine::Scene;
		const bool producer = lane.at("laneId") == "ProducerBoundaries";
		Engine engine;
		EngineLifetime lifetime{engine, true};
		Start(engine, options, lane);
		auto& renderer = RenderSystem::Instance();
		auto& animation = AnimationSystem::Instance();
		const auto delta = scenario.at("deltaSeconds").get<float>();
		const auto bootstrap = manifest.at("bootstrapFrames").get<std::size_t>();
		auto captured = std::make_shared<CapturedInputsObserver>();
		auto callbacks = std::make_shared<RenderFrameScratchValidationCallbacks>();
		callbacks->onEvent = [captured](const auto& event) { captured->Observe(event); };
		renderer.SetFrameScratchValidationCallbacks(callbacks);
		struct CallbackOwner
		{
			RenderSystem& renderer;
			~CallbackOwner() { try { renderer.SetFrameScratchValidationCallbacks({}); } catch(...) {} }
		} callbackOwner{renderer};
		for(std::size_t index = 0; index < bootstrap; ++index)
		{
			const bool updated = engine.Update(delta);
			Json failure = nullptr;
			if(const auto& observed = engine.GetFrameFailure()) failure = {{"frame", observed->frame},
				{"workload", observed->workload}, {"phase", observed->phase}, {"reason", observed->reason}};
			Json recovery = nullptr;
			if(const auto* context = renderer.GetContext())
			{
				const auto observed = context->GetLatestRendererRecoverySnapshot();
				recovery = {{"state", static_cast<unsigned>(observed.state)}, {"renderSessionGeneration", observed.renderSessionGeneration},
					{"frameId", observed.frameId}, {"message", observed.message}, {"firstFailureStage", static_cast<unsigned>(observed.firstFailureStage)},
					{"firstFailureResult", observed.firstFailureResult}, {"lastError", observed.lastError}};
			}
			lane["diagnostic"]["runtimeIdentifiers"].push_back({{"kind", "BootstrapFrame"}, {"ordinal", index},
				{"engineUpdateReturned", updated}, {"engineFailure", failure}, {"rendererRegistered", engine.GetSystem<RenderSystem>() != nullptr},
				{"rendererContextPresent", renderer.GetContext() != nullptr}, {"rendererRecovery", std::move(recovery)},
				{"schedulerSummaryValid", engine.GetSchedulerSummary().valid},
				{"plan", Plan(engine.GetParallelFrameExecutionPlan(), engine.GetSchedulerSummary())}, {"graph", Graph(engine.GetJobGraphSummary())},
				{"sceneMeshInputs", SceneMeshInputs()}, {"capturedInputs", captured->Copy()}, {"capturedRenderables", Renderables()},
				{"runtimeLayoutRejectedRecordCount", renderer.GetLatestRenderGraphDiagnostics().runtimeLayoutRejectedRecordCount},
				{"runtimeLayoutRejectionReasons", renderer.GetLatestRenderGraphDiagnostics().runtimeLayoutRejectionReasons},
				{"renderPublicationPresent", LoadReplayRenderPublication(renderer) != nullptr}});
			Require(updated, "Engine stopped during fixed replay bootstrap.");
		}
		// Clear intentionally removes the publication. Retain only its authored
		// scalar identity for typed ingress, never an old root/cache for cold work.
		const auto bootstrapObject = FirstObject(LoadReplayRenderPublication(renderer));
		if(scenario.at("warmupFrames") == 0) renderer.Clear();
		for(std::size_t index = 0; index < scenario.at("warmupFrames").get<std::size_t>(); ++index)
		{
			if(options.scenario == "multi-output") ConfigureMultiOutput(lane);
			Require(engine.Update(delta), "Engine stopped during fixed replay warmup.");
		}
		lane["status"] = "Passed";
		lane["actualWorkers"] = ThreadPool::instance().getThreadCount();
		Assertion(lane, "actual-workers", options.workers, ThreadPool::instance().getThreadCount());
		BoundaryObserver observer;
		observer.action = scenario.at("action").get<std::string>();
		observer.animator = FirstAnimator(animation.LoadPublishedAuthorityFrame());
		ObservedSystem animationObserver(animation, observer), rendererObserver(renderer, observer);
		ObservedSystem presentationObserver(AnimationPresentationSystem::Instance(), observer);
		const auto requireSystem = [&]<typename T>() -> T& {
			auto* actual = engine.GetSystem<T>();
			Require(actual != nullptr, "Producer dependency closure lacks an initialized native Engine system.");
			return *actual;
		};
		Require(&requireSystem.template operator()<AnimationSystem>() == &animation
			&& &requireSystem.template operator()<AnimationPresentationSystem>() == &AnimationPresentationSystem::Instance()
			&& &requireSystem.template operator()<RenderSystem>() == &renderer,
			"Observed producer singleton is not the initialized Engine-owned system.");
		ObservedSystem scriptingObserver(requireSystem.template operator()<ScriptingSystem>(), observer);
		ObservedSystem physicsObserver(requireSystem.template operator()<TerraEngine::Physics::PhysicsSystem>(), observer);
		ObservedSystem uiObserver(requireSystem.template operator()<UISystem>(), observer);
		ObservedSystem streamingObserver(requireSystem.template operator()<TerraEngine::Resource::VirtualGeometryStreamingSystem>(), observer);
		SystemScheduler scheduler;
		// The subsystem lane uses the production scheduler's unchanged defaults
		// and actual producer declarations. Its protected queue callback is scoped
		// to this lane; Engine.Update remains the sole full-engine flush authority.
		if(producer) {
			// This is the complete native dependency closure in original relative
			// registration order. No declaration, dependency or callback is replaced.
			for(auto* system : std::array<ObservedSystem*, 7>{&scriptingObserver, &animationObserver, &physicsObserver,
				&presentationObserver, &uiObserver, &streamingObserver, &rendererObserver})
				scheduler.RegisterSystem(*system, system->SchedulerDeclaration());
		}
		const auto& builtSchedule = producer ? scheduler.BuildSchedule() : engine.GetSchedulerSummary();
		const auto scheduleFacts = Plan(FrameExecutionPlan{}, builtSchedule);
		lane["authoritative"]["scheduleConstruction"] = {{"valid", builtSchedule.valid},
			{"systems", builtSchedule.systems}, {"diagnostics", scheduleFacts.at("scheduleDiagnostics")}};
		Require(builtSchedule.valid, "Native schedule construction failed before mutation ingress.");
		auto& queue = SceneMutationQueue::Instance();
		auto objectId = bootstrapObject;
		if(objectId == TerraEngine::EMPTY_UUID)
		{
			const auto renderables = renderer.GetLatestRenderablesSnapshot();
			if(!renderables.empty()) objectId = renderables.front().objectId;
			else if(options.scenario == "no-output")
			{
				const auto inputs = SceneMeshInputs();
				if(!inputs.empty()) objectId = *TerraEngine::ParseUUID(inputs.front().at("objectId").get<std::string>());
			}
		}
		const auto uniqueName = std::string("frame-replay-") + options.scenario;
		const auto transactionId = uniqueName + "-visibility";
		if(objectId == TerraEngine::EMPTY_UUID) throw MissingPrerequisite("Fixed bootstrap did not publish an actual authored render object for mutation visibility.");
		TransactionalMutationBuilder builder(transactionId, TransactionOrigin::Automation, "FrameReplayFixture");
		builder.ObserveObject(ObjectManager::instance(), objectId).SetName(objectId, uniqueName);
		auto transaction = builder.Build();
		auto task = ThreadPool::instance().createTask([transaction = std::move(transaction)]() mutable {
			SceneMutationQueue::Instance().EnqueueTransaction(std::move(transaction));
		});
		auto ingress = task->getFuture(); ThreadPool::instance().enqueue(task); ingress.get();
		const bool invisibleBefore = !ObjectManager::instance().FindObjectByName(uniqueName).has_value();
		lane["authoritative"]["mutationObservations"].push_back({{"checkpoint", "WorkerIngressBeforeFrame"},
			{"transactionId", transactionId}, {"objectId", TerraEngine::ToHexString(objectId)}, {"pendingCount", queue.Size()},
			{"newNameVisible", !invisibleBefore}});
		Assertion(lane, "mutation-not-visible-before-frame", true, invisibleBefore && !queue.Empty());
		const auto heldRender = LoadReplayRenderPublication(renderer);
		const auto heldAnimation = animation.LoadPublishedAuthorityFrame();
		const auto heldRenderValues = RenderRoot(heldRender), heldAnimationValues = AnimationRoot(heldAnimation);
		const auto beforeAuthority = heldAnimation;
		bool commandPublished = false, allCompleted = true, workerFailureObserved = false, continuationRetired = false;
		bool rejectionRetained = false, beforeCancellationRetained = false, beforeCommandRejected = false, afterCancellationCommitted = false, afterCommandCompleted = false;
		bool coldTopologyFresh = false, warmTopologyReused = false;
		const auto frameCount = scenario.at("measuredFrames").get<std::size_t>();
		for(std::size_t index = 0; index < frameCount; ++index)
		{
			// The real renderer clears transient override cameras at frame end,
			// so the same authored second-view request must be renewed each frame.
			if(options.scenario == "multi-output") ConfigureMultiOutput(lane);
			observer.frame = index; observer.armed = producer && index == 0;
			if(observer.armed && observer.action == "RejectedAllocation")
				SetAnimationAuthorityFailureInjectionForTesting(AnimationAuthorityPublicationOutcome::RejectedAllocation);
			AnimationCommand request;
			request.target = FirstAnimator(animation.LoadPublishedAuthorityFrame()); request.kind = AnimationCommandKind::SetSpeed;
			request.playbackSpeed = 1.0F; request.producerId = 68050; request.producerSequence = index + 1;
			request.correlationId = index + 1; request.idempotencyKey = index + 1;
			(void)animation.SubmitCommand(request);
			const auto started = std::chrono::steady_clock::now();
			if(producer)
			{
				const auto plan = scheduler.ExecuteFrameWorkloads(delta, [&](const MutationFlushPoint& point) {
					FrameWorkloadDeclaration identity;
					identity.stableDescriptorId = "SystemScheduler/MutationFlush/" + point.name;
					observer.Append(identity, {0, 1, 0, 0}, "Enter");
					MutationFlushRuntimeState state;
					const auto pending = queue.Size();
					const bool sameFramePhysicsConsumer = SameFramePhysicsPublicationConsumerActive();
					const bool skip = point.name == "Before Physics" || (point.name == "After Physics" && !sameFramePhysicsConsumer);
					SceneMutationBatchResult result;
					if(!skip && !queue.Empty()) result = SceneManager::Instance().ApplySceneMutations("frame-replay-subsystem-flush");
					const bool physicsBoundary = (point.name == "After Physics" && sameFramePhysicsConsumer)
						|| (point.name == "Frame End" && !sameFramePhysicsConsumer);
					const auto physicsTickBefore = TerraEngine::Physics::PhysicsSystem::Instance().CurrentFixedTick();
					if(physicsBoundary) {
						// The included native provider must acknowledge its real protected
						// Transform receipts before managed consumers receive its events.
						// This scoped completion is not the full Engine flush callback.
						TerraEngine::Physics::PhysicsSystem::Instance().OnAfterPhysicsFlush(result.transactionResults, result.publicationGroupResults);
						ScriptingSystem::Instance().DeliverPhysicsEventsAfterFlush();
					}
					const auto physicsTickAfter = TerraEngine::Physics::PhysicsSystem::Instance().CurrentFixedTick();
					state.flushAttempted = !skip; state.pendingAfterFlush = queue.Size();
					lane["authoritative"]["mutationObservations"].push_back({{"checkpoint", "SubsystemProtectedFlush"},
						{"frameOrdinal", index}, {"name", point.name}, {"attempted", !skip}, {"pendingBefore", pending},
						{"pendingAfter", state.pendingAfterFlush}, {"applied", result.applied}, {"acceptedCount", result.acceptedMutationCount},
						{"rejectedCount", result.rejectedMutationCount}});
					lane["authoritative"]["mutationObservations"].push_back({{"checkpoint", "SubsystemNativeProviderAcknowledgment"},
						{"frameOrdinal", index}, {"name", point.name}, {"sameFramePhysicsConsumer", sameFramePhysicsConsumer},
						{"physicsAcknowledgmentInvoked", physicsBoundary}, {"managedEventDeliveryInvoked", physicsBoundary},
						{"physicsTickBefore", physicsTickBefore}, {"physicsTickAfter", physicsTickAfter},
						{"transactionReceiptCount", result.transactionResults.size()}, {"publicationGroupReceiptCount", result.publicationGroupResults.size()}});
					observer.Append(identity, {0, 1, 0, 0}, "Exit");
					return state;
				});
				const auto receipt = animation.ReadCommandReceipt(request.producerId, request.producerSequence, request.correlationId);
				const auto expectedFailure = options.scenario == "worker-failure" && index == 0
					&& observer.injected && observer.faultRanOnWorker ? ExpectedFrameFailure::InjectedGeometryWorker : ExpectedFrameFailure::None;
				CaptureFrame(lane, index, plan, scheduler.LatestSummary(), receipt, *captured, expectedFailure);
				// Direct ExecuteFrameWorkloads returns this execution's plan; the
				// scheduler's compatibility latest-plan cache is not updated here.
				const auto boundaries = observer.Finish(plan, lane);
				for(const auto& boundary : boundaries) lane["authoritative"]["boundaries"].push_back(boundary);
				if(observer.action == "ThrowAtGeometryRoutePrepare" && index == 0)
				{
					workerFailureObserved = !plan.completed && plan.firstFailure.has_value();
					// Qualify the actual admission at the fault boundary. The held
					// renderer root can predate the failed frame; its identity is explicit.
					// Clear below intentionally retires GPU resources, so later recovery
					// admission is recorded without promising GPU recovery latency.
					QualifyProfile(lane, scenario, "FaultFrameBeforeRendererClear", index);
					renderer.Clear(); continuationRetired = !renderer.GetFrameScratchValidationState().active;
				}
				else allCompleted = allCompleted && plan.valid && plan.completed;
			}
			else
			{
				const bool completed = engine.Update(delta);
				allCompleted = allCompleted && completed;
				CaptureFrame(lane, index, engine.GetParallelFrameExecutionPlan(), engine.GetSchedulerSummary(),
					animation.ReadCommandReceipt(request.producerId, request.producerSequence, request.correlationId), *captured,
					ExpectedFrameFailure::None, &engine.GetJobGraphSummary());
			}
			ClearAnimationAuthorityFailureInjectionForTesting();
			const auto receipt = animation.ReadCommandReceipt(request.producerId, request.producerSequence, request.correlationId);
			commandPublished = commandPublished || receipt.status == AnimationStatus::Ok;
			if(index == 0)
			{
				const auto afterAuthority = animation.LoadPublishedAuthorityFrame();
				rejectionRetained = afterAuthority == beforeAuthority && heldAnimationValues == AnimationRoot(afterAuthority);
				beforeCancellationRetained = rejectionRetained;
				beforeCommandRejected = receipt.status == AnimationStatus::Rejected && receipt.publicationGeneration == 0;
				afterCancellationCommitted = afterAuthority && beforeAuthority && afterAuthority->authorityGeneration > beforeAuthority->authorityGeneration;
				afterCommandCompleted = receipt.status == AnimationStatus::Ok && afterAuthority && receipt.publicationGeneration == afterAuthority->authorityGeneration;
				if(producer && (options.scenario == "cancel-before-exchange" || options.scenario == "cancel-after-exchange")) {
					observer.cancellationCacheFacts["entriesAfterCallbacks"] = animation.ReadProductionIntegrationSnapshot().cacheEntryCount;
					lane["authoritative"]["cancellationObservation"] = {{"firstCommandReceipt", Command(receipt)},
						{"cacheCounts", observer.cancellationCacheFacts}, {"cacheMembershipClaim", "CountOnlyNoEntryIdentityProof"}};
				}
				const auto afterRender = LoadReplayRenderPublication(renderer);
				coldTopologyFresh = !heldRender && afterRender && afterRender->persistent && !afterRender->persistent->pages.empty()
					&& afterRender->receipt.reusedPersistentPages == 0;
				warmTopologyReused = heldRender && afterRender && heldRender->persistent && heldRender->persistent == afterRender->persistent;
			}
			const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
			lane["diagnostic"]["timings"].push_back({{"frameOrdinal", index}, {"wallNanoseconds", duration}});
		}
		Assertion(lane, "observed-frame-count", frameCount, lane["authoritative"]["frames"].size());
		Assertion(lane, producer ? "production-boundaries-observed" : "full-engine-execution", true,
			producer ? !lane["authoritative"]["boundaries"].empty() && allCompleted : allCompleted);
		const bool noOutput = scenario.at("id") == "no-output";
		Assertion(lane, "renderer-root-observed", true, noOutput ? !LoadReplayRenderPublication(renderer) : LoadReplayRenderPublication(renderer) != nullptr);
		Assertion(lane, "animation-root-observed", true, animation.LoadPublishedAuthorityFrame() != nullptr);
		Assertion(lane, "authored-render-identity", true, noOutput ? !SceneMeshInputs().empty() : FirstObject(LoadReplayRenderPublication(renderer)) != TerraEngine::EMPTY_UUID);
		Assertion(lane, "authored-animation-identity", true, FirstAnimator(animation.LoadPublishedAuthorityFrame()).IsValid());
		Assertion(lane, "command-published", true, commandPublished);
		const auto renamed = ObjectManager::instance().FindObjectByName(uniqueName);
		const bool visibleAfter = renamed && ObjectManager::instance().getUUID(*renamed) == objectId;
		const auto mutationReceipt = queue.TakeCompletedAutomationTransactionReceipt(transactionId);
		lane["authoritative"]["mutationObservations"].push_back({{"checkpoint", "AfterFrames"}, {"transactionId", transactionId},
			{"objectId", TerraEngine::ToHexString(objectId)}, {"newNameVisible", visibleAfter}, {"receiptPresent", mutationReceipt.has_value()},
			{"status", mutationReceipt ? static_cast<unsigned>(mutationReceipt->status) : 0U},
			{"appliedWriteCount", mutationReceipt ? mutationReceipt->appliedWriteCount : 0U},
			{"rejectedWriteCount", mutationReceipt ? mutationReceipt->rejectedWriteCount : 0U}});
		Assertion(lane, "mutation-visible-after-frame", true, visibleAfter);
		Assertion(lane, "mutation-receipt-committed", true, mutationReceipt && mutationReceipt->status == TransactionStatus::Committed && mutationReceipt->appliedWriteCount != 0);
		if(producer && options.scenario == "worker-failure")
			lane["authoritative"]["postRetirementProfileObservation"] = {{"checkpoint", "AfterMeasuredRecoveryFrames"},
				{"frameOrdinal", frameCount - 1}, {"qualifying", false}, {"animationAdmission", ProductionAdmission()}};
		else QualifyProfile(lane, scenario, "AfterMeasuredFrames", frameCount - 1);
		const auto id = scenario.at("id").get<std::string>();
		if(id == "cold-direct-complete" || id == "cold-virtual-compact")
			Assertion(lane, "cold-topology-not-reused", true, coldTopologyFresh);
		if(id == "warm-direct-complete" || id == "warm-virtual-compact")
			Assertion(lane, "warm-topology-reused", true, warmTopologyReused);
		if(id == "multi-output" || id == "no-output")
		{
			const auto root = LoadReplayRenderPublication(renderer);
			const auto count = root ? root->views.size() : 0;
			const auto actualInputs = captured->Copy();
			bool noOutputProof = !root && !actualInputs.is_null() && actualInputs.at("cameras").empty();
			bool multiOutputProof = count >= 2;
			for(const auto& frame : lane["authoritative"]["frames"])
			{
				if(id == "multi-output")
				{
					if(frame.at("renderer").at("root").is_null()) { multiOutputProof = false; continue; }
					multiOutputProof = multiOutputProof && frame.at("multiOutputObservation").at("passed") == true;
					continue;
				}
				const auto& observed = frame.at("outputObservation");
				noOutputProof = noOutputProof && frame.at("renderer").at("present") == false
					&& frame.at("renderer").at("root").is_null() && frame.at("capturedRenderables").empty()
					&& !frame.at("sceneMeshInputs").empty() && !frame.at("capturedInputs").is_null() && frame.at("capturedInputs").at("cameras").empty()
					&& observed.at("diagnosticsFrameId").get<std::uint64_t>() != 0 && observed.at("diagnosticsValid") == true
					&& observed.at("diagnosticsPublicationGeneration") == 0 && observed.at("viewSubmitWorkCount") == 0
					&& observed.at("gameStatsValid") == false && observed.at("sceneStatsValid") == false && observed.at("scratchCompleted") == true;
			}
			Assertion(lane, id == "multi-output" ? "output-count-at-least-two" : "output-count-zero", true,
				id == "multi-output" ? multiOutputProof : noOutputProof);
		}
		if(producer && id == "rejection-rollback")
		{
			Assertion(lane, "rejected-candidate-retains-previous-root", true, rejectionRetained);
			AnimationCommand valid; valid.target = FirstAnimator(animation.LoadPublishedAuthorityFrame()); valid.kind = AnimationCommandKind::Pause;
			valid.producerId = 68051; valid.producerSequence = 1; valid.correlationId = 68051;
			auto invalid = valid; invalid.target = {}; invalid.producerSequence = 2;
			const std::array requests{valid, invalid};
			const auto beforePending = animation.PendingCommandCount(), beforeCaptured = animation.CapturedCommandCount();
			const auto beforeRoot = animation.LoadPublishedAuthorityFrame();
			const auto beforeValues = AnimationRoot(beforeRoot);
			const auto batch = animation.SubmitCommandBatch(requests);
			const auto failedReceipt = batch.commands.size() == requests.size() ? batch.commands.back() : AnimationCommandReceipt{};
			const auto failedLookup = animation.ReadCommandReceipt(invalid.producerId, invalid.producerSequence, invalid.correlationId);
			const auto firstReceipt = animation.ReadCommandReceipt(valid.producerId, valid.producerSequence, valid.correlationId);
			const auto afterPending = animation.PendingCommandCount(), afterCaptured = animation.CapturedCommandCount();
			const auto afterRoot = animation.LoadPublishedAuthorityFrame();
			const bool valuesUnchanged = beforeValues == AnimationRoot(afterRoot);
			Json receipts = Json::array();
			for(const auto& receipt : batch.commands) receipts.push_back(Command(receipt));
			lane["authoritative"]["rejectedCommandBatchObservation"] = {{"batchStatus", static_cast<unsigned>(batch.status)},
				{"batchReceipts", std::move(receipts)}, {"firstReceipt", Command(firstReceipt)}, {"failedReceipt", Command(failedReceipt)}, {"failedLookup", Command(failedLookup)},
				{"pendingBefore", beforePending}, {"pendingAfter", afterPending}, {"capturedBefore", beforeCaptured}, {"capturedAfter", afterCaptured},
				{"rootRetained", beforeRoot == afterRoot}, {"rootValuesRetained", valuesUnchanged},
				{"authorityGenerationBefore", beforeRoot ? beforeRoot->authorityGeneration : 0}, {"authorityGenerationAfter", afterRoot ? afterRoot->authorityGeneration : 0}};
			Assertion(lane, "rejected-command-batch-atomic", true, batch.status == AnimationStatus::InvalidArgument
				&& batch.commands.size() == requests.size() && failedReceipt.status == AnimationStatus::InvalidArgument
				&& firstReceipt.status == AnimationStatus::NotReady && failedLookup.status == AnimationStatus::NotReady
				&& beforePending == afterPending && beforeCaptured == afterCaptured && beforeRoot == afterRoot && valuesUnchanged);
		}
		if(producer && (id == "cancel-before-exchange" || id == "cancel-after-exchange"))
		{
			Assertion(lane, "cancellation-ingress-observed", true, observer.injected);
			if(id == "cancel-before-exchange") {
				Assertion(lane, "before-exchange-retains-previous-root", true, beforeCancellationRetained);
				// Generation ingress changes the publication epoch, not JobGraph's
				// context. The real finalizer rejects the unready authority token.
				Assertion(lane, "before-exchange-command-rejected", true, beforeCommandRejected);
			}
			else
			{
				Assertion(lane, "after-exchange-root-committed", true, afterCancellationCommitted);
				Assertion(lane, "after-exchange-command-completed", true, afterCommandCompleted);
			}
		}
		if(producer && id == "worker-failure")
		{
			Assertion(lane, "worker-fault-injected", true, observer.injected && observer.faultRanOnWorker);
			Assertion(lane, "worker-fault-reported", true, workerFailureObserved);
			Assertion(lane, "failed-continuation-retired", true, continuationRetired);
		}
		if(id == "held-reader")
		{
			Assertion(lane, "held-render-root-immutable", true, heldRender && heldRenderValues == RenderRoot(heldRender));
			Assertion(lane, "held-animation-root-immutable", true, heldAnimation && heldAnimationValues == AnimationRoot(heldAnimation));
		}
		if(id == "scene-clear")
		{
			// Follow the maintained public scene-owner invalidation/clear order at
			// a settled protected boundary. UnloadAll only clears document authority;
			// it does not itself clear ObjectManager or the renderer publication.
			(void)animation.NotifyLifecycleInvalidation(AnimationLifecycleBoundary::Scene);
			SceneManager::Instance().UnloadAll();
			ObjectManager::instance().clear();
			renderer.Clear();
			Assertion(lane, "scene-clear-root-retired", true, !LoadReplayRenderPublication(renderer) && !animation.LoadPublishedAuthorityFrame());
			Assertion(lane, "scene-clear-held-roots-immutable", true, heldRender && heldAnimation
				&& heldRenderValues == RenderRoot(heldRender) && heldAnimationValues == AnimationRoot(heldAnimation));
		}
		if(id == "shutdown" || id == "restart")
		{
			const auto oldRender = LoadReplayRenderPublication(renderer);
			const auto oldAnimation = animation.LoadPublishedAuthorityFrame();
			const auto oldRenderValues = RenderRoot(oldRender), oldAnimationValues = AnimationRoot(oldAnimation);
			lifetime.Close();
			if(id == "shutdown")
			{
				Assertion(lane, "shutdown-render-root-retired", true, !LoadReplayRenderPublication(renderer));
				Assertion(lane, "shutdown-animation-root-retired", true, !animation.LoadPublishedAuthorityFrame());
				Assertion(lane, "shutdown-held-roots-immutable", true, heldRender && heldAnimation
					&& heldRenderValues == RenderRoot(heldRender) && heldAnimationValues == AnimationRoot(heldAnimation));
			}
			else
			{
				const bool renderRetired = !LoadReplayRenderPublication(renderer);
				const bool animationRetired = !animation.LoadPublishedAuthorityFrame();
				const bool contextReleased = renderer.GetContext() == nullptr;
				lane["authoritative"]["restartShutdownObservation"] = {{"rendererRootRetired", renderRetired},
					{"animationRootRetired", animationRetired}, {"contextReleased", contextReleased}};
				Assertion(lane, "restart-shutdown-roots-retired", true, renderRetired && animationRetired && contextReleased);
				lifetime.entered = true;
				Start(engine, options, lane);
				for(std::size_t index = 0; index < bootstrap; ++index) Require(engine.Update(delta), "Restart bootstrap failed.");
				const auto freshRender = LoadReplayRenderPublication(renderer);
				const auto freshAnimation = animation.LoadPublishedAuthorityFrame();
				Assertion(lane, "restart-roots-published", true, freshRender && freshAnimation);
				// Backend recovery counters belong to each native context instance and
				// can repeat after reinitialization. The strongly retained old root
				// instead proves that the new publication is a distinct allocation.
				const bool freshContextPresent = renderer.GetContext() != nullptr;
				const bool distinctPublication = oldRender && freshRender && oldRender != freshRender;
				Assertion(lane, "restart-new-render-publication", true, renderRetired && contextReleased && freshContextPresent && distinctPublication);
				Assertion(lane, "restart-new-animation-session", true, oldAnimation && freshAnimation && oldAnimation->sessionGeneration != freshAnimation->sessionGeneration);
				const auto identity = [](const auto& root) -> Json {
					if(!root) return nullptr;
					return {{"rendererSessionGeneration", root->rendererSessionGeneration}, {"backendResourceGeneration", root->backendResourceGeneration},
						{"generation", root->generation}, {"frameId", root->frameId}};
				};
				lane["authoritative"]["restartPublicationIdentity"] = {{"rendererGenerationDomain", "BackendRecoveryInstanceMayRepeatAcrossReinitialization"},
					{"old", identity(oldRender)}, {"fresh", identity(freshRender)}, {"freshContextPresent", freshContextPresent},
					{"distinctRetainedPublication", distinctPublication}};
				lane["authoritative"]["restartPublications"] = {{"renderer", RenderRoot(freshRender)}, {"animation", AnimationRoot(freshAnimation)}};
				const bool retainedValues = heldRenderValues == RenderRoot(heldRender) && heldAnimationValues == AnimationRoot(heldAnimation)
					&& oldRenderValues == RenderRoot(oldRender) && oldAnimationValues == AnimationRoot(oldAnimation);
				lane["authoritative"]["restartRetainedRoots"] = {{"heldRenderImmutable", heldRenderValues == RenderRoot(heldRender)},
					{"heldAnimationImmutable", heldAnimationValues == AnimationRoot(heldAnimation)},
					{"preShutdownRenderImmutable", oldRenderValues == RenderRoot(oldRender)}, {"preShutdownAnimationImmutable", oldAnimationValues == AnimationRoot(oldAnimation)}};
				Assertion(lane, "restart-held-roots-immutable", true, heldRender && heldAnimation && oldRender && oldAnimation && retainedValues);
			}
		}
		lane["authoritative"]["lifetimeObservations"].push_back({{"checkpoint", "AfterScenario"}, {"action", scenario.at("action")},
			{"heldRenderPresent", heldRender != nullptr}, {"heldAnimationPresent", heldAnimation != nullptr},
			{"heldRenderImmutable", heldRenderValues == RenderRoot(heldRender)}, {"heldAnimationImmutable", heldAnimationValues == AnimationRoot(heldAnimation)},
			{"scratch", Scratch(renderer.GetFrameScratchValidationState())}});
		lifetime.Close();
	}
}

int main(int argc, char** argv)
{
	Json receipt;
	Path output;
	try
	{
		const auto options = Parse(argc, argv); output = options.output;
		const auto manifest = ReadJson(options.manifest);
		Require(manifest.at("schemaVersion") == "frame-replay-workloads-v1", "Unsupported replay workload manifest.");
		const auto found = std::find_if(manifest.at("scenarios").begin(), manifest.at("scenarios").end(),
			[&](const Json& row) { return row.at("id") == options.scenario; });
		Require(found != manifest.at("scenarios").end(), "Unknown replay scenario.");
		const auto scenario = *found;
		const auto engineModule = GetModuleHandleW(L"TerraEngine.dll");
		Require(engineModule != nullptr, "Replay engine DLL is not loaded.");
		const auto expectedRelative = std::filesystem::relative(std::filesystem::canonical(options.expectedEnginePath), std::filesystem::canonical(options.installRoot));
		Require(!expectedRelative.empty() && !expectedRelative.is_absolute(), "Expected engine DLL must be within the trusted install root.");
		for(const auto& part : expectedRelative) Require(part != "..", "Expected engine DLL escapes trusted install root.");
		Require(std::filesystem::equivalent(ModulePath(engineModule), options.expectedEnginePath), "Replay loaded an unexpected engine DLL.");
		// This guard executes before any Engine construction, singleton access,
		// virtual call, or private inline member access. A matching-header tool
		// beside the wrong DLL must fail using only WinAPI/standard/CNG code.
		const auto enginePin = FilePin(ModulePath(engineModule));
		Require(enginePin.at("sha256") == options.expectedEngineSha256, "Loaded engine DLL does not match the trusted parent ABI pin.");
		VerifyProjectClosure(options, scenario);
		Json assetManifestPins = Json::array();
		for(const auto& path : AssetManifestPaths(options)) assetManifestPins.push_back(FilePin(path));
		receipt = {{"schemaVersion", "frame-stage-replay-v1"}, {"protocolFixture", false}, {"runNonce", options.runNonce},
			{"scenarioId", options.scenario}, {"requestedWorkers", options.workers}, {"actualWorkers", nullptr},
			{"status", "Incomplete"}, {"unsupportedObservations", Json::array()},
			{"controls", {{"manifest", FilePin(options.manifest)}, {"projectRoot", std::filesystem::absolute(options.projectRoot).lexically_normal().generic_string()},
				{"assetManifest", FilePin(options.assetManifest)}, {"projectClosure", FilePin(options.projectClosure)}, {"scenario", scenario},
				{"resolvedAssetManifests", std::move(assetManifestPins)},
				{"loadedEngine", enginePin}, {"expectedEngineSha256", options.expectedEngineSha256}, {"engineHashVerified", true},
				{"expectedEnginePath", options.expectedEnginePath.generic_string()},
				{"hostExecutable", FilePin(ModulePath(nullptr))}}},
			{"coverageJoin", {{"requiredLanes", {"FullEngine", "ProducerBoundaries"}}, {"observedLanes", Json::array()}}},
			{"lanes", Json::array({EmptyLane("FullEngine"), EmptyLane("ProducerBoundaries")})}};
		for(auto& lane : receipt["lanes"])
		{
			try { RunLane(lane, options, manifest, scenario); }
			catch(const MissingPrerequisite& error)
			{
				ClearAnimationAuthorityFailureInjectionForTesting();
				lane["status"] = "Incomplete";
				lane["unavailableCapability"] = error.what();
			}
			catch(const std::exception& error)
			{
				ClearAnimationAuthorityFailureInjectionForTesting();
				lane["status"] = "Failed";
				lane["failure"] = error.what();
			}
			if(lane.contains("actualWorkers")) receipt["actualWorkers"] = lane.at("actualWorkers");
			if(lane.contains("unavailableCapability")) receipt["unsupportedObservations"].push_back({
				{"laneId", lane.at("laneId")}, {"reason", lane.at("unavailableCapability")}});
			if(lane.at("status") == "Passed") receipt["coverageJoin"]["observedLanes"].push_back(lane.at("laneId"));
		}
		bool failed = false, incomplete = false;
		for(const auto& lane : receipt["lanes"]) { failed = failed || lane.at("status") == "Failed"; incomplete = incomplete || lane.at("status") == "Incomplete"; }
		receipt["status"] = failed ? "Failed" : incomplete ? "Incomplete" : "Passed";
		WriteFresh(output, receipt);
		return receipt.at("status") == "Passed" ? 0 : 1;
	}
	catch(const std::exception& error)
	{
		std::cerr << "Frame replay: " << error.what() << '\n';
		if(!output.empty() && !receipt.is_null())
		{
			receipt["status"] = "Failed"; receipt["failure"] = error.what();
			try { WriteFresh(output, receipt); } catch(...) {}
		}
		return 1;
	}
}
