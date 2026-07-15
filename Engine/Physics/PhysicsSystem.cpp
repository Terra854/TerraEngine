// PhysicsSystem.cpp - seven declared workloads over the shared Jolt 5.6 world.

#include "PhysicsSystem.h"

#include "Backend/JoltPhysicsBackend.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/Assets/PhysicsAssetChangeJournal.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Assets/PhysicsMaterial.h"
#include "Physics/PhysicsAuthoringGeneration.h"
#include "Physics/PhysicsPreparationPlan.h"
#include "Physics/PhysicsTransactions.h"
#include "Physics/TransformPublicationPacket.h"
#include "Resource/ResourceManager.h"
#include "Scene/TransactionalMutation.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Physics {
	namespace {
		using Clock = std::chrono::steady_clock;

		double MillisecondsSince(Clock::time_point start) {
			return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		}

		bool SamePose(const PhysicsPose& left, const PhysicsPose& right) {
			constexpr float epsilon = 1.0e-6F;
			// Unit quaternions q and -q encode the same orientation. Jolt may choose
			// either representation when extracting a body, so compare the absolute
			// dot product instead of component signs before suppressing a duplicate
			// authored transform publication.
			const float rotationDot = left.rotation.x * right.rotation.x
				+ left.rotation.y * right.rotation.y
				+ left.rotation.z * right.rotation.z
				+ left.rotation.w * right.rotation.w;
			return std::fabs(left.position.x - right.position.x) <= epsilon
				&& std::fabs(left.position.y - right.position.y) <= epsilon
				&& std::fabs(left.position.z - right.position.z) <= epsilon
				&& std::fabs(rotationDot) >= 1.0F - epsilon;
		}

		glm::vec3 ToGlmPosition(const PhysicsVector3& value) {
			return {value.x, value.y, value.z};
		}

		glm::quat ToGlmRotation(const PhysicsQuaternion& value) {
			return glm::normalize(glm::quat(value.w, value.x, value.y, value.z));
		}

		PhysicsVector3 NormalizeOr(PhysicsVector3 value, PhysicsVector3 fallback) {
			const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
			if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8F) return fallback;
			const float inverseLength = 1.0F / std::sqrt(lengthSquared);
			return {value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
		}

		PhysicsVector3 PerpendicularTo(PhysicsVector3 axis) {
			axis = NormalizeOr(axis, {0.0F, 1.0F, 0.0F});
			const PhysicsVector3 reference = std::fabs(axis.x) < 0.9F
				? PhysicsVector3{1.0F, 0.0F, 0.0F}
				: PhysicsVector3{0.0F, 1.0F, 0.0F};
			return NormalizeOr({
				axis.y * reference.z - axis.z * reference.y,
				axis.z * reference.x - axis.x * reference.z,
				axis.x * reference.y - axis.y * reference.x}, {0.0F, 0.0F, 1.0F});
		}

		std::string_view ToDiagnosticsPath(PhysicsPreparationExecutionPath path) {
			switch(path) {
			case PhysicsPreparationExecutionPath::Fast: return "fast";
			case PhysicsPreparationExecutionPath::Inline: return "inline";
			case PhysicsPreparationExecutionPath::WorkerRanges: return "worker_ranges";
			case PhysicsPreparationExecutionPath::Rejected: return "rejected";
			}
			return "rejected";
		}
	}

	struct PhysicsSystem::Impl {
		struct FrameWorkloadActivation {
			bool snapshot = false;
			bool prepare = false;
			bool commandCommit = false;
			bool simulate = false;
			bool extract = false;
			bool publish = false;
			bool publicationBarrier = false;
			std::size_t authoringItems = 0U;
			std::size_t providerItems = 0U;
			std::size_t commandItems = 0U;
			std::size_t queryItems = 0U;
		};
		struct RuntimeBody {
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			PhysicsBodyHandle body;
			PhysicsShapeHandle shape;
			PhysicsMotionType motionType = PhysicsMotionType::Static;
			PhysicsPose parentWorldPose;
			PhysicsVector3 parentWorldScale{1.0F, 1.0F, 1.0F};
			bool hasParent = false;
			std::uint64_t objectTransformRevision = 0U;
			std::uint64_t hierarchyGeneration = 0U;
			TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
			std::uint64_t parentObjectGeneration = 0U;
			std::uint64_t parentTransformRevision = 0U;
			bool hasPublishedPose = false;
			PhysicsPose lastPublishedPose;
		};

		struct PreparedBody {
			PhysicsBodyDescriptor body;
			std::vector<PhysicsColliderDescriptor> colliders;
			std::optional<Backend::PhysicsShapeRecipe> shapeRecipe;
			std::optional<PhysicsMaterialRuntime> material;
			std::string diagnostic;
		};

		struct RuntimeConstraint {
			TerraEngine::UUID ownerObjectId{};
			PhysicsGeneration ownerObjectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			TerraEngine::UUID recordId{};
			PhysicsGeneration recordGeneration = 0U;
			PhysicsGeneration resetGeneration = 0U;
			PhysicsConstraintHandle handle;
			Backend::PhysicsConstraintCreateRequest request;
		};

		struct RuntimeCharacter {
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			PhysicsCharacterHandle handle;
		};

		struct PendingTransformPublication {
			TransformPublicationPacket packet;
			std::string groupId;
		};

		Impl()
			: backend(std::make_unique<Backend::JoltPhysicsBackend>(ThreadPool::instance())) {
		}

		FrameWorkloadActivation InspectFrameWorkloadActivationLocked(float deltaTime) const {
			FrameWorkloadActivation activation;
			const bool worldActive = world.Handle().IsValid();
			const PhysicsCommandQueueDiagnostics commands = world.CommandQueueDiagnostics();
			const PhysicsQueryDiagnostics queries = world.QueryDiagnostics();
			activation.commandItems = commands.queuedDepth;
			activation.queryItems = queries.queuedDepth;
			activation.providerItems = world.RegistryStatistics().bodyCount;
			activation.authoringItems = authoring ? authoring->Data().bodies.size() : activation.providerItems;

			bool authoringChanged = pendingSettings.has_value() || !authoring || !authoringSceneRevisionValid;
			if (worldActive && !authoringChanged)
			{
				authoringChanged = ObjectManager::instance().GetSpatialRevision() != authoringEquivalentSceneSpatialRevision
					|| !PhysicsAssetChangeJournal::Instance().SnapshotSince(observedPhysicsAssetGeneration).Empty();
			}

			const PhysicsWorldTimingState timing = world.Timing();
			const PhysicsSettings settings = worldActive ? world.Settings() : configuredSettings;
			const bool explicitStep = timing.pendingExplicitStepCount > 0U;
			const bool accumulating = worldActive && !world.IsPaused() && deltaTime > 0.0F;
			const bool predictedStep = explicitStep || (accumulating
				&& timing.accumulator + deltaTime >= settings.fixedTimeStep);
			const bool preparationWork = worldActive
				&& (authoringChanged || accumulating || explicitStep || activation.commandItems > 0U);

			// Snapshot remains protected because it observes live scene generations.
			// Every later phase is activated only when its prerequisite can produce
			// data, so paused unchanged worlds lower to omitted descriptors.
			activation.snapshot = worldActive && preparationWork;
			activation.prepare = preparationWork;
			activation.commandCommit = preparationWork;
			activation.simulate = worldActive && predictedStep;
			activation.extract = activation.simulate;
			activation.publish = activation.extract;
			activation.publicationBarrier = activation.publish;
			return activation;
		}

		bool ApplyPendingSettingsLocked() {
			if(!pendingSettings) return true;
			PhysicsSettings requested = std::move(*pendingSettings);
			pendingSettings.reset();
			if(!requested.Validate().accepted) return false;
			configuredSettings = requested;
			++settingsGeneration;
			if(!world.Handle().IsValid()) return true;

			// Layer/filter and solver changes require a fresh provider world. The
			// restart occurs entirely inside PhysicsSnapshot's protected boundary;
			// the same frame then captures authoring and stages every runtime record
			// against the new world generation before publication.
			if(!StopWorldLocked()) return false;
			return StartWorldLocked(configuredSettings);
		}

		bool StartWorldLocked(const PhysicsSettings& requestedSettings) {
			if(world.Handle().IsValid()) return true;
			if(!backendInitialized || !world.Create(requestedSettings)) return false;
			// A recreated world owns a new generation. Drop immutable editor data from
			// the former generation before any consumer can select it by accident.
			PublishedPhysicsDebugPackets().Clear();
			PublishPhysicsDiagnosticsSnapshot({});
			Backend::PhysicsBackendWorldConfiguration configuration;
			configuration.world = world.Handle();
			configuration.settings = requestedSettings;
			const Backend::PhysicsBackendResult result = backend->CreateWorld(configuration);
			if(result.Succeeded()) {
				diagnosticCounters = {};
				diagnosticTimings = {};
				diagnosticQueues = {};
				publicationDiagnostics = {};
				stateStorePublicationCount = 0U;
				stateStoreRejectedBatchCount = 0U;
				lastStatePagesReused = 0U;
				lastStatePagesCopied = 0U;
				lastAggregateDiagnosticsTick = 0U;
				return true;
			}
			world.MarkFailed();
			world.BeginStopping();
			world.BeginDraining();
			world.CompleteTeardown();
			return false;
		}

		bool StopWorldLocked() {
			const PhysicsWorldHandle handle = world.Handle();
			if(!handle.IsValid()) return true;
			for(const PendingTransformPublication& publication : pendingTransformPublications) {
				Scene::SceneMutationQueue::Instance().CancelPublicationGroup(publication.groupId);
			}
			world.BeginStopping();
			world.BeginDraining();
			const Backend::PhysicsBackendResult destroyed = backend->DestroyWorld(handle);
			runtimeBodies.clear();
			bodyObjects.clear();
			runtimeConstraints.clear();
			runtimeCharacters.clear();
			preparedBodies.clear();
			preparedRemovals.clear();
			preparedConstraints.clear();
			preparedConstraintRemovals.clear();
			preparedCharacters.clear();
			preparedCharacterRemovals.clear();
			plannedSteps = {};
			preparedQueries = {};
			executedQueries.clear();
			completedSteps.clear();
			pendingFlushSteps.clear();
			pendingTransformPublications.clear();
			authoring.reset();
			authoringGeneration.reset();
			preparationPlan.reset();
			observedPhysicsAssetGeneration = 0U;
			authoringEquivalentSceneSpatialRevision = 0U;
			authoringSceneRevisionValid = false;
			lastPreparedAuthoringSnapshotGeneration = 0U;
			extraction = {};
			preparedBodyStateBatch.reset();
			preparedTransformSources.clear();
			cookedAssetWriteTimes.clear();
			cookedAssetsNeedingReload.clear();
			materialAssetWriteTimes.clear();
			materialAssetsNeedingReload.clear();
			materialAssets.clear();
			latestBodies.reset();
			latestIncrementalBodies.reset();
			latestCommands.reset();
			latestEvents.reset();
			latestCharacters.reset();
			latestDiagnostics.reset();
			PublishedPhysicsDebugPackets().Clear();
			PublishPhysicsDiagnosticsSnapshot({});
			const bool completed = world.CompleteTeardown();
			return destroyed.Succeeded() && completed;
		}

		void SnapshotLocked() {
			authoringSnapshotReused = false;
			if(!ApplyPendingSettingsLocked()) {
				++diagnosticCounters.rejectedCount;
				return;
			}
			if(!world.Handle().IsValid()) return;
			const Clock::time_point start = Clock::now();
			ObjectManager& objects = ObjectManager::instance();
			const std::uint64_t sceneSpatialRevision = objects.GetSpatialRevision();
			const PhysicsAssetChangeSnapshot assetChanges =
				PhysicsAssetChangeJournal::Instance().SnapshotSince(observedPhysicsAssetGeneration);
			if(authoring && authoringSceneRevisionValid
				&& authoringEquivalentSceneSpatialRevision == sceneSpatialRevision
				&& assetChanges.Empty()) {
				// A prior protected flush proved that the only intervening scene writes
				// were this system's dynamic-body publications. Those transforms are
				// physics-authoritative, so the immutable authoring snapshot remains valid.
				// The prior frame already consumed its dirty list during preparation;
				// requiring that stored list to be empty would rebuild the same bodies after
				// every physics-authored Transform revision.
				authoringSnapshotReused = true;
				diagnosticTimings.prepareMs = MillisecondsSince(start);
				return;
			}

			const PhysicsAuthoringSnapshot* previous = authoring ? &*authoring : nullptr;
			std::optional<PhysicsAuthoringSnapshot> captured;
			if(authoring && authoringSceneRevisionValid
				&& authoringEquivalentSceneSpatialRevision == sceneSpatialRevision) {
				// An asset-only change reuses the immutable component snapshot. The exact
				// dependency ranges identify which retained descriptors need rebuilding.
				PhysicsAuthoringData retained = authoring->Data();
				++retained.snapshotGeneration;
				if(retained.snapshotGeneration == 0U) retained.snapshotGeneration = 1U;
				captured = PhysicsAuthoringSnapshot::Create(std::move(retained), previous);
			} else {
				captured = PhysicsAuthoringSnapshot::Capture(
					objects, world.Handle(), world.Settings(), world.Timing().tick, previous);
			}
			if(captured) {
				const auto generation = PhysicsAuthoringSnapshot::BuildGeneration(
					previous, *captured, assetChanges);
				if(!generation) {
					++diagnosticCounters.rejectedCount;
					diagnosticTimings.prepareMs = MillisecondsSince(start);
					return;
				}
				authoring = std::move(*captured);
				authoringGeneration = std::move(*generation);
				observedPhysicsAssetGeneration = assetChanges.toGenerationInclusive;
				authoringEquivalentSceneSpatialRevision = sceneSpatialRevision;
				authoringSceneRevisionValid = true;
			}
			else ++diagnosticCounters.rejectedCount;
			diagnosticTimings.prepareMs = MillisecondsSince(start);
		}

		bool CookedAssetChangedLocked(const TerraEngine::UUID& assetId) {
			const std::filesystem::path path = ResourceManager::Instance().GetRegisteredAssetPath(
				assetId, AssetType::CookedCollider);
			if(path.empty()) return true;
			std::error_code error;
			const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
			if(error) {
				// Packaged TEPAK entries have no host file timestamp. Treat the first
				// observation as dirty, then keep the immutable mounted bytes stable.
				return cookedAssetWriteTimes.emplace(assetId, std::filesystem::file_time_type::min()).second;
			}
			const auto [iterator, inserted] = cookedAssetWriteTimes.emplace(assetId, writeTime);
			if(inserted || iterator->second != writeTime) {
				iterator->second = writeTime;
				cookedAssetsNeedingReload.insert(assetId);
				return true;
			}
			return false;
		}

		std::shared_ptr<const PhysicsCookedShapeAssetState> ResolveCookedAssetLocked(
			const TerraEngine::UUID& assetId,
			std::string& diagnostic) {
			auto current = world.AcquireCookedShape(assetId);
			if(current && !cookedAssetsNeedingReload.contains(assetId)) return current;
			const std::filesystem::path path = ResourceManager::Instance().GetRegisteredAssetPath(
				assetId, AssetType::CookedCollider);
			if(path.empty()) {
				diagnostic = "Cooked collider GUID is not registered with the resource manager.";
				if(current) world.MarkCookedShapeRecookFailed(assetId, diagnostic);
				return current;
			}

			const std::vector<std::uint8_t> rawBytes = ResourceManager::Instance().ReadFileBytes(path);
			// One load attempt consumes this change notification. A subsequent file
			// timestamp change will enqueue another atomic replacement attempt.
			cookedAssetsNeedingReload.erase(assetId);
			std::vector<std::byte> bytes(rawBytes.size());
			if(!rawBytes.empty()) std::memcpy(bytes.data(), rawBytes.data(), rawBytes.size());
			CookedColliderDiagnostics diagnostics;
			const std::optional<CookedColliderAsset> asset = CookedColliderAsset::Deserialize(bytes, diagnostics);
			if(!asset || asset->Envelope().assetId != assetId) {
				diagnostic = !diagnostics.entries.empty()
					? diagnostics.entries.front().message
					: "Cooked collider envelope GUID does not match the scene reference.";
				if(current) world.MarkCookedShapeRecookFailed(assetId, diagnostic);
				return current;
			}
			const CookedColliderCompatibilityResult compatibility = asset->CheckCompatibility(
				CookedColliderAsset::HostPlatform(), CookedColliderProviderFamily::Jolt,
				kJoltCookProviderVersion, kJoltCookRevision);
			if(!compatibility.compatible) {
				diagnostic = compatibility.message;
				if(current) world.MarkCookedShapeRecookFailed(assetId, diagnostic);
				return current;
			}

			const CookedColliderEnvelope& envelope = asset->Envelope();
			if(current && current->data->recipeHash == envelope.recipeHash
				&& current->data->payloadChecksum == envelope.payloadChecksum) {
				if(envelope.status == CookedColliderStatus::StaleLastKnownGood && envelope.lastKnownGood) {
					world.MarkCookedShapeRecookFailed(assetId, envelope.lastKnownGood->recookFailure);
					return world.AcquireCookedShape(assetId);
				}
				return current;
			}

			PhysicsCookedShapePublication publication;
			publication.assetId = assetId;
			publication.assetRevision = current ? current->assetRevision + 1U : 1U;
			if(publication.assetRevision == 0U) publication.assetRevision = 1U;
			publication.data.recipeHash = envelope.recipeHash;
			publication.data.formatFamily = envelope.payloadFormatFamily;
			publication.data.provider = std::string(ToString(envelope.providerFamily));
			publication.data.providerVersion = envelope.providerVersion;
			publication.data.providerRevision = envelope.cookRevision;
			publication.data.formatVersion = envelope.payloadFormatVersion;
			publication.data.payloadChecksum = envelope.payloadChecksum;
			publication.data.payload.assign(asset->Payload().begin(), asset->Payload().end());
			if(!world.PublishCookedShape(std::move(publication))) {
				diagnostic = "Cooked collider cache rejected a conflicting recipe hash or stale publication.";
				if(current) world.MarkCookedShapeRecookFailed(assetId, diagnostic);
				return current;
			}
			if(envelope.status == CookedColliderStatus::StaleLastKnownGood && envelope.lastKnownGood) {
				world.MarkCookedShapeRecookFailed(assetId, envelope.lastKnownGood->recookFailure);
			}
			return world.AcquireCookedShape(assetId);
		}

		bool MaterialAssetChangedLocked(const TerraEngine::UUID& assetId) {
			const std::filesystem::path path = ResourceManager::Instance().GetRegisteredAssetPath(
				assetId, AssetType::PhysicsMaterial);
			if(path.empty()) return true;
			std::error_code error;
			const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
			if(error) {
				return materialAssetWriteTimes.emplace(assetId, std::filesystem::file_time_type::min()).second;
			}
			const auto [iterator, inserted] = materialAssetWriteTimes.emplace(assetId, writeTime);
			if(inserted || iterator->second != writeTime) {
				iterator->second = writeTime;
				materialAssetsNeedingReload.insert(assetId);
				return true;
			}
			return false;
		}

		std::optional<PhysicsMaterialRuntime> ResolveMaterialLocked(
			const TerraEngine::UUID& assetId,
			std::string& diagnostic) {
			if(assetId == TerraEngine::EMPTY_UUID) return PhysicsMaterialRuntime{};
			const auto current = materialAssets.find(assetId);
			if(current != materialAssets.end() && !materialAssetsNeedingReload.contains(assetId)) {
				return current->second;
			}
			const std::filesystem::path path = ResourceManager::Instance().GetRegisteredAssetPath(
				assetId, AssetType::PhysicsMaterial);
			if(path.empty()) {
				diagnostic = "Physics material GUID is not registered with the resource manager.";
				return current != materialAssets.end()
					? std::optional<PhysicsMaterialRuntime>(current->second)
					: std::nullopt;
			}

			const std::vector<std::uint8_t> rawBytes = ResourceManager::Instance().ReadFileBytes(path);
			materialAssetsNeedingReload.erase(assetId);
			const nlohmann::json document = nlohmann::json::parse(
				rawBytes.begin(), rawBytes.end(), nullptr, false);
			PhysicsMaterial material;
			PhysicsMaterialDiagnostics diagnostics;
			if(document.is_discarded() || !PhysicsMaterial::LoadJson(document, material, diagnostics)
				|| material.assetId != assetId) {
				diagnostic = !diagnostics.entries.empty()
					? diagnostics.entries.front().message
					: "Physics material JSON is invalid or its GUID does not match the scene reference.";
				return current != materialAssets.end()
					? std::optional<PhysicsMaterialRuntime>(current->second)
					: std::nullopt;
			}
			PhysicsMaterialRuntime runtime = material.BuildRuntime();
			materialAssets.insert_or_assign(assetId, runtime);
			return runtime;
		}

		std::optional<PhysicsMaterialRuntime> ResolveBodyMaterialLocked(
			const std::vector<PhysicsColliderDescriptor>& colliders,
			std::string& diagnostic) {
			const TerraEngine::UUID assetId = colliders.front().materialId;
			const bool mixedMaterials = std::any_of(colliders.begin(), colliders.end(),
				[&](const PhysicsColliderDescriptor& collider) { return collider.materialId != assetId; });
			if(mixedMaterials) {
				// Jolt exposes friction and restitution at body granularity in this
				// backend. Reject instead of silently assigning the wrong subshape data.
				diagnostic = "One rigid body cannot combine collider records with different physics materials.";
				return std::nullopt;
			}
			return ResolveMaterialLocked(assetId, diagnostic);
		}

		std::optional<Backend::PhysicsShapeRecipe> MakeColliderRecipeLocked(
			const PhysicsColliderDescriptor& collider,
			std::string& diagnostic) {
			Backend::PhysicsShapeRecipe recipe;
			recipe.kind = collider.kind;
			recipe.dimensions = collider.dimensions;
			recipe.localCenter = collider.localPose.position;
			recipe.localRotation = collider.localPose.rotation;
			recipe.stableRecipeHash = collider.stableRecipeHash;
			recipe.subShapeKey = collider.subShapeKey;
			recipe.sensor = collider.trigger;
			if(collider.cookedColliderAssetId == TerraEngine::EMPTY_UUID) return recipe;

			const auto state = ResolveCookedAssetLocked(collider.cookedColliderAssetId, diagnostic);
			if(!state || !state->data) return std::nullopt;
			recipe.sourceAssetId = collider.cookedColliderAssetId;
			recipe.stableRecipeHash ^= state->data->recipeHash
				+ 0x9e3779b97f4a7c15ULL
				+ (recipe.stableRecipeHash << 6U)
				+ (recipe.stableRecipeHash >> 2U);
			if(recipe.stableRecipeHash == 0U) recipe.stableRecipeHash = 1U;
			recipe.cookedFormatFamily = state->data->formatFamily;
			recipe.cookedProvider = state->data->provider;
			recipe.cookedProviderVersion = state->data->providerVersion;
			recipe.cookedProviderRevision = state->data->providerRevision;
			recipe.cookedFormatVersion = state->data->formatVersion;
			recipe.cookedPayloadChecksum = state->data->payloadChecksum;
			recipe.cookedPayload = std::shared_ptr<const std::vector<std::byte>>(
				state->data, &state->data->payload);
			return recipe;
		}

		std::optional<Backend::PhysicsShapeRecipe> MakeShapeRecipeLocked(
			const std::vector<PhysicsColliderDescriptor>& colliders,
			std::string& diagnostic) {
			if(colliders.size() == 1U) return MakeColliderRecipeLocked(colliders.front(), diagnostic);
			Backend::PhysicsShapeRecipe recipe;
			recipe.kind = PhysicsShapeKind::Compound;
			recipe.dimensions = {1.0F, 1.0F, 1.0F};
			recipe.stableRecipeHash = 14695981039346656037ULL;
			recipe.children.reserve(colliders.size());
			for(const PhysicsColliderDescriptor& collider : colliders) {
				auto child = MakeColliderRecipeLocked(collider, diagnostic);
				if(!child) return std::nullopt;
				recipe.stableRecipeHash ^= child->stableRecipeHash
					+ 0x9e3779b97f4a7c15ULL
					+ (recipe.stableRecipeHash << 6U)
					+ (recipe.stableRecipeHash >> 2U);
				recipe.children.push_back(std::move(*child));
			}
			if(recipe.stableRecipeHash == 0U) recipe.stableRecipeHash = 1U;
			return recipe;
		}

		void PrepareLocked(float deltaTime) {
			const Clock::time_point start = Clock::now();
			lastPreparationRejected = false;
			preparationPlanExecutedThisFrame = false;
			preparedBodies.clear();
			preparedRemovals.clear();
			preparedConstraints.clear();
			preparedConstraintRemovals.clear();
			preparedCharacters.clear();
			preparedCharacterRemovals.clear();
			plannedSteps = world.PlanFixedSteps(deltaTime);
			preparedQueries = {};
			if(!plannedSteps.requests.empty()) {
				preparedQueries = world.PrepareQueries(plannedSteps.requests.back().tick);
			}
			if(!authoring) {
				diagnosticTimings.prepareMs += MillisecondsSince(start);
				return;
			}

			std::unordered_set<TerraEngine::UUID> bodiesToPrepare;
			const bool isNewAuthoringGeneration = authoringGeneration
				&& lastPreparedAuthoringSnapshotGeneration != authoringGeneration->Data().snapshotGeneration;
			if(isNewAuthoringGeneration) {
				PhysicsPreparationPlanOptions options;
				options.parallelThreshold = configuredSettings.parallelPreparationThreshold;
				options.rangeSize = configuredSettings.providerExtractionBatchSize;
				options.availableWorkerCount = std::max<std::size_t>(1U, ThreadPool::instance().getThreadCount());
				preparationPlan = PhysicsPreparationPlan::Build(
					*authoringGeneration,
					plannedSteps.requests.empty() ? world.Timing().tick + 1U : plannedSteps.requests.back().tick,
					std::move(options));
				if(!preparationPlan || !ExecutePhysicsPreparationPlan(
					*preparationPlan, ThreadPool::instance(), preparationScratch,
					[](const PhysicsPreparationWorkItem& item) {
						// Worker validation is deliberately value-only. Provider shapes and
						// live scene state remain owned by the following protected commit.
						return item.kind == PhysicsPreparationWorkKind::RemoveBody
							|| item.operation.descriptorIndex != kInvalidPhysicsDescriptorIndex;
					})) {
					preparationPlan.reset();
					lastPreparationRejected = true;
					++diagnosticCounters.rejectedCount;
					diagnosticTimings.prepareMs += MillisecondsSince(start);
					return;
				}
				preparationPlanExecutedThisFrame = true;

				const PhysicsPreparationPlanData& plan = preparationPlan->Data();
				bodiesToPrepare.reserve(plan.creates.size() + plan.updates.size()
					+ plan.dependencyRebuilds.size());
				const auto appendObjects = [&bodiesToPrepare](
					const std::vector<PhysicsPreparationOperation>& operations) {
					for(const PhysicsPreparationOperation& operation : operations) {
						bodiesToPrepare.insert(operation.objectId);
					}
				};
				appendObjects(plan.creates);
				appendObjects(plan.updates);
				appendObjects(plan.dependencyRebuilds);
				for(const PhysicsPreparationOperation& removal : plan.removals) {
					preparedRemovals.push_back(removal.objectId);
				}

				// Asset generations are explicit. Primitive recipes never poll the file
				// system; only journaled cooked/material dependencies request reload.
				for(const PhysicsAuthoringAssetObservation& observation :
					authoringGeneration->Data().assetObservations) {
					if(observation.assetKind == PhysicsAssetKind::CookedCollider) {
						cookedAssetsNeedingReload.insert(observation.assetId);
					} else {
						materialAssetsNeedingReload.insert(observation.assetId);
					}
				}
			}
			std::unordered_map<TerraEngine::UUID, std::vector<PhysicsColliderDescriptor>> collidersByObject;
			collidersByObject.reserve(bodiesToPrepare.size());
			for(const PhysicsColliderDescriptor& collider : authoring->Data().colliders) {
				if(collider.enabled && bodiesToPrepare.contains(collider.objectId)) {
					collidersByObject[collider.objectId].push_back(collider);
				}
			}
			for(const PhysicsBodyDescriptor& body : authoring->Data().bodies) {
				if(!bodiesToPrepare.contains(body.objectId)) continue;
				const auto colliders = collidersByObject.find(body.objectId);
				if(colliders != collidersByObject.end() && !colliders->second.empty()) {
					PreparedBody prepared;
					prepared.body = body;
					prepared.colliders = std::move(colliders->second);
					prepared.shapeRecipe = MakeShapeRecipeLocked(prepared.colliders, prepared.diagnostic);
					prepared.material = ResolveBodyMaterialLocked(prepared.colliders, prepared.diagnostic);
					preparedBodies.push_back(std::move(prepared));
				}
			}
			std::unordered_set<TerraEngine::UUID> rebuildingBodies;
			for(const PreparedBody& prepared : preparedBodies) rebuildingBodies.insert(prepared.body.objectId);
			for(const TerraEngine::UUID& objectId : preparedRemovals) rebuildingBodies.insert(objectId);

			std::unordered_set<TerraEngine::UUID> presentConstraints;
			std::unordered_set<TerraEngine::UUID> rebuildingConstraints;
			for(const PhysicsConstraintDescriptor& constraint : authoring->Data().constraints) {
				presentConstraints.insert(constraint.recordId);
				const auto live = runtimeConstraints.find(constraint.recordId);
				const bool endpointRebuilt = rebuildingBodies.contains(constraint.firstObjectId)
					|| rebuildingBodies.contains(constraint.secondObjectId);
				if(live == runtimeConstraints.end()
					|| live->second.ownerObjectGeneration != constraint.ownerObjectGeneration
					|| live->second.componentGeneration != constraint.componentGeneration
					|| live->second.recordGeneration != constraint.recordGeneration
					|| live->second.resetGeneration != constraint.resetGeneration
					|| endpointRebuilt) {
					preparedConstraints.push_back(constraint);
					rebuildingConstraints.insert(constraint.recordId);
				}
			}
			// Gear constraints retain provider references to their two authored hinge
			// records. Rebuild a gear whenever either hinge is replaced, even when the
			// gear's own serialized generation is unchanged.
			for(const PhysicsConstraintDescriptor& constraint : authoring->Data().constraints) {
				if(constraint.kind != PhysicsConstraintKind::Gear
					|| rebuildingConstraints.contains(constraint.recordId)
					|| (!rebuildingConstraints.contains(constraint.firstGearConstraintId)
						&& !rebuildingConstraints.contains(constraint.secondGearConstraintId))) continue;
				preparedConstraints.push_back(constraint);
				rebuildingConstraints.insert(constraint.recordId);
			}
			for(const auto& [recordId, live] : runtimeConstraints) {
				(void)live;
				if(!presentConstraints.contains(recordId)) preparedConstraintRemovals.push_back(recordId);
			}
			// Gear records depend on their authored hinge records, so commit all other
			// kinds first while preserving authored order inside each group.
			std::stable_sort(preparedConstraints.begin(), preparedConstraints.end(),
				[](const PhysicsConstraintDescriptor& left, const PhysicsConstraintDescriptor& right) {
					return left.kind != PhysicsConstraintKind::Gear && right.kind == PhysicsConstraintKind::Gear;
				});

			std::unordered_set<TerraEngine::UUID> presentCharacters;
			for(const PhysicsCharacterDescriptor& character : authoring->Data().characters) {
				presentCharacters.insert(character.objectId);
				const auto live = runtimeCharacters.find(character.objectId);
				if(live == runtimeCharacters.end()
					|| live->second.objectGeneration != character.objectGeneration
					|| live->second.componentGeneration != character.componentGeneration) {
					preparedCharacters.push_back(character);
				}
			}
			for(const auto& [objectId, live] : runtimeCharacters) {
				(void)live;
				if(!presentCharacters.contains(objectId)) preparedCharacterRemovals.push_back(objectId);
			}
			// Dirty records describe work for one immutable snapshot generation. Keep
			// the snapshot available to worker jobs, but do not replay its rebuild set
			// when a later frame safely reuses the same authoring packet.
			lastPreparedAuthoringSnapshotGeneration = authoring->Data().snapshotGeneration;
			diagnosticTimings.prepareMs += MillisecondsSince(start);
		}

		void AppendCommandResult(
			PhysicsCommandId commandId,
			PhysicsResultStatus status,
			PhysicsTick tick,
			std::string message) {
			commandResults.push_back({commandId, status, tick, std::move(message)});
			if(status != PhysicsResultStatus::Success && status != PhysicsResultStatus::Accepted) {
				++diagnosticCounters.rejectedCount;
			}
		}

		static Backend::PhysicsBackendMotorMode ConvertMotorMode(PhysicsConstraintMotorMode mode) {
			switch(mode) {
			case PhysicsConstraintMotorMode::Velocity: return Backend::PhysicsBackendMotorMode::Velocity;
			case PhysicsConstraintMotorMode::Position: return Backend::PhysicsBackendMotorMode::Position;
			case PhysicsConstraintMotorMode::Disabled: return Backend::PhysicsBackendMotorMode::Off;
			}
			return Backend::PhysicsBackendMotorMode::Off;
		}

		static Backend::PhysicsConstraintAxisSettings ConvertAxisSettings(
			const PhysicsConstraintLimits& limits,
			const PhysicsConstraintSpring& spring,
			const PhysicsConstraintMotor& motor) {
			Backend::PhysicsConstraintAxisSettings converted;
			converted.limitEnabled = limits.enabled;
			converted.minimum = limits.minimum;
			converted.maximum = limits.maximum;
			converted.springEnabled = spring.enabled;
			converted.springUsesFrequency = false;
			converted.springFrequency = spring.stiffness;
			converted.springDamping = spring.damping;
			converted.motorMode = ConvertMotorMode(motor.mode);
			converted.motorTargetPosition = motor.targetPosition;
			converted.motorTargetVelocity = motor.targetVelocity;
			converted.motorMaximumForce = motor.maximumForce;
			return converted;
		}

		PhysicsBodyHandle ResolveConstraintEndpoint(
			PhysicsConstraintEndpointKind kind,
			const TerraEngine::UUID& objectId) const {
			if(kind == PhysicsConstraintEndpointKind::WorldAnchor) return {};
			const auto body = runtimeBodies.find(objectId);
			return body != runtimeBodies.end() ? body->second.body : PhysicsBodyHandle{};
		}

		void CommitLocked() {
			const Clock::time_point start = Clock::now();
			commandResults.clear();
			struct StagedBody {
				TerraEngine::UUID objectId{};
				RuntimeBody runtime;
			};
			struct StagedConstraint {
				TerraEngine::UUID recordId{};
				RuntimeConstraint runtime;
				PhysicsConstraintRuntimeRegistration registration;
			};
			struct StagedCharacter {
				TerraEngine::UUID objectId{};
				RuntimeCharacter runtime;
			};

			Backend::PhysicsBackendStagingToken staging;
			std::vector<StagedBody> stagedBodies;
			std::vector<StagedConstraint> stagedConstraints;
			std::vector<StagedCharacter> stagedCharacters;
			std::vector<PhysicsConstraintHandle> registeredConstraintHandles;
			std::unordered_map<TerraEngine::UUID, PhysicsBodyHandle> stagedBodyHandles;
			std::unordered_map<TerraEngine::UUID, PhysicsConstraintHandle> stagedConstraintHandles;
			bool transactionRejected = false;
			std::string transactionDiagnostic;

			const auto rejectTransaction = [&](std::string diagnostic) {
				if(transactionRejected) return;
				transactionRejected = true;
				transactionDiagnostic = std::move(diagnostic);
			};

			const auto rollbackTransaction = [&]() {
				for(auto handle = registeredConstraintHandles.rbegin();
					handle != registeredConstraintHandles.rend(); ++handle) {
					world.RemoveConstraint(*handle);
				}
				if(staging.IsValid()) {
					const Backend::PhysicsBackendResult rolledBack = backend->RollbackStaging(staging);
					if(!rolledBack.Succeeded()) {
						// A provider commit can consume its staging token before reporting a
						// fatal health failure. Remove every new record in dependency order
						// so the last known-good live world remains the only visible state.
						for(auto constraint = stagedConstraints.rbegin();
							constraint != stagedConstraints.rend(); ++constraint) {
							backend->DestroyConstraint(constraint->runtime.handle);
						}
						for(auto character = stagedCharacters.rbegin();
							character != stagedCharacters.rend(); ++character) {
							backend->DestroyCharacter(character->runtime.handle);
						}
						for(auto body = stagedBodies.rbegin(); body != stagedBodies.rend(); ++body) {
							backend->DestroyBody(body->runtime.body);
							backend->DestroyShape(body->runtime.shape);
						}
					}
				}
				for(const StagedConstraint& constraint : stagedConstraints) {
					world.Release(constraint.runtime.handle);
				}
				for(const StagedCharacter& character : stagedCharacters) {
					world.Release(character.runtime.handle);
				}
				for(const StagedBody& body : stagedBodies) {
					world.Release(body.runtime.body);
					world.Release(body.runtime.shape);
				}
			};

			for(const PreparedBody& prepared : preparedBodies) {
				if(transactionRejected) break;
				if(prepared.colliders.empty()) {
					rejectTransaction("A prepared rigid body has no collider records.");
					break;
				}
				const std::uint8_t bodyLayer = prepared.colliders.front().collisionLayer;
				const bool mixedLayers = std::any_of(prepared.colliders.begin(), prepared.colliders.end(),
					[bodyLayer](const PhysicsColliderDescriptor& collider) { return collider.collisionLayer != bodyLayer; });
				if(mixedLayers) {
					rejectTransaction("One rigid body cannot combine collider records from different collision layers.");
					break;
				}
				if(!prepared.shapeRecipe || !prepared.material) {
					rejectTransaction(prepared.diagnostic.empty()
						? "Collider assets could not be resolved." : prepared.diagnostic);
					break;
				}

				const PhysicsShapeHandle newShape = world.AllocateShape();
				const PhysicsBodyHandle newBody = world.AllocateBody();
				if(!newShape.IsValid() || !newBody.IsValid()) {
					if(newShape.IsValid()) world.Release(newShape);
					if(newBody.IsValid()) world.Release(newBody);
					rejectTransaction("Physics handle capacity was exhausted while preparing a body rebuild.");
					break;
				}

				Backend::PhysicsBackendResult result = backend->CreateShape(
					world.Handle(), newShape, *prepared.shapeRecipe, staging);
				if(result.Succeeded()) {
					Backend::PhysicsBodyCreateRequest request;
					request.handle = newBody;
					request.objectId = prepared.body.objectId;
					request.objectGeneration = prepared.body.objectGeneration;
					request.componentGeneration = prepared.body.componentGeneration;
					request.shape = newShape;
					request.motionType = prepared.body.motionType;
					request.motionQuality = PhysicsSettings::ResolveMotionQuality(
						configuredSettings.continuousCollisionEnabled, prepared.body.motionQuality);
					request.pose = prepared.body.worldPose;
					request.linearVelocity = prepared.body.initialLinearVelocity;
					request.angularVelocity = prepared.body.initialAngularVelocity;
					request.mass = prepared.body.mass;
					// Jolt has one friction coefficient per body. The dynamic value is the
					// correct runtime coefficient; static friction remains authored data for
					// future solver policies and editor inspection.
					request.friction = prepared.material->dynamicFriction;
					request.restitution = prepared.material->restitution;
					request.linearDamping = prepared.body.linearDamping;
					request.angularDamping = prepared.body.angularDamping;
					request.maximumLinearVelocity = prepared.body.maximumLinearVelocity;
					request.maximumAngularVelocity = prepared.body.maximumAngularVelocity;
					request.allowedDegreesOfFreedom = prepared.body.allowedDegreesOfFreedom;
					request.collisionLayer = bodyLayer;
					request.collisionMask = prepared.body.collisionMask;
					request.frictionCombine = prepared.material->frictionCombineMode;
					request.restitutionCombine = prepared.material->restitutionCombineMode;
					request.useGravity = prepared.body.useGravity;
					request.allowSleeping = prepared.body.allowSleeping;
					request.startAwake = prepared.body.startAwake;
					request.enabled = prepared.body.enabled;
					request.sensor = std::all_of(prepared.colliders.begin(), prepared.colliders.end(),
						[](const PhysicsColliderDescriptor& collider) { return collider.trigger; });
					result = backend->CreateBody(request, staging);
				}
				if(!result.Succeeded()) {
					// Track both handles before rolling back so provider and engine handle
					// ownership remain paired even when body creation rejects after shape staging.
					RuntimeBody failedRuntime{
						prepared.body.objectId, prepared.body.objectGeneration,
						prepared.body.componentGeneration, newBody, newShape,
						prepared.body.motionType, prepared.body.parentWorldPose,
						prepared.body.parentWorldScale, prepared.body.hasParent};
					failedRuntime.objectTransformRevision = prepared.body.objectTransformRevision;
					failedRuntime.hierarchyGeneration = authoring->Data().hierarchyGeneration;
					failedRuntime.parentObjectId = prepared.body.parentObjectId;
					failedRuntime.parentObjectGeneration = prepared.body.parentObjectGeneration;
					failedRuntime.parentTransformRevision = prepared.body.parentTransformRevision;
					stagedBodies.push_back({prepared.body.objectId, std::move(failedRuntime)});
					rejectTransaction(result.message);
					break;
				}

				RuntimeBody runtime{
					prepared.body.objectId,
					prepared.body.objectGeneration,
					prepared.body.componentGeneration,
					newBody,
					newShape,
					prepared.body.motionType,
					prepared.body.parentWorldPose,
					prepared.body.parentWorldScale,
					prepared.body.hasParent};
				runtime.objectTransformRevision = prepared.body.objectTransformRevision;
				runtime.hierarchyGeneration = authoring->Data().hierarchyGeneration;
				runtime.parentObjectId = prepared.body.parentObjectId;
				runtime.parentObjectGeneration = prepared.body.parentObjectGeneration;
				runtime.parentTransformRevision = prepared.body.parentTransformRevision;
				stagedBodies.push_back({prepared.body.objectId, runtime});
				stagedBodyHandles[prepared.body.objectId] = newBody;
			}

			for(const PhysicsConstraintDescriptor& prepared : preparedConstraints) {
				if(transactionRejected) break;
				const auto resolveEndpoint = [&](PhysicsConstraintEndpointKind kind,
					const TerraEngine::UUID& objectId) {
					if(kind == PhysicsConstraintEndpointKind::WorldAnchor) return PhysicsBodyHandle{};
					const auto staged = stagedBodyHandles.find(objectId);
					return staged != stagedBodyHandles.end()
						? staged->second : ResolveConstraintEndpoint(kind, objectId);
				};
				const PhysicsBodyHandle bodyA = resolveEndpoint(
					prepared.firstEndpointKind, prepared.firstObjectId);
				const PhysicsBodyHandle bodyB = resolveEndpoint(
					prepared.secondEndpointKind, prepared.secondObjectId);
				if((prepared.firstEndpointKind == PhysicsConstraintEndpointKind::ConnectedObject && !bodyA.IsValid())
					|| (prepared.secondEndpointKind == PhysicsConstraintEndpointKind::ConnectedObject && !bodyB.IsValid())) {
					rejectTransaction("Constraint endpoint resolution rejected an absent or stale body UUID.");
					break;
				}

				const PhysicsConstraintHandle handle = world.AllocateConstraint();
				if(!handle.IsValid()) {
					rejectTransaction("Physics constraint handle capacity was exhausted.");
					break;
				}
				Backend::PhysicsConstraintCreateRequest request;
				request.handle = handle;
				request.ownerObjectId = prepared.ownerObjectId;
				request.ownerObjectGeneration = prepared.ownerObjectGeneration;
				request.componentGeneration = prepared.componentGeneration;
				request.recordId = prepared.recordId;
				request.recordGeneration = prepared.recordGeneration;
				request.kind = prepared.kind;
				request.bodyA = bodyA;
				request.bodyB = bodyB;
				request.localFrameA = prepared.firstFrame;
				request.localFrameB = prepared.secondFrame;
				request.axisA = NormalizeOr(prepared.firstAxis, {0.0F, 1.0F, 0.0F});
				request.axisB = NormalizeOr(prepared.secondAxis, {0.0F, 1.0F, 0.0F});
				request.normalAxisA = PerpendicularTo(request.axisA);
				request.normalAxisB = PerpendicularTo(request.axisB);
				if(prepared.kind == PhysicsConstraintKind::SixDegreesOfFreedom) {
					for(std::size_t axis = 0U; axis < request.axes.size(); ++axis) {
						request.axes[axis] = ConvertAxisSettings(
							prepared.axisLimits[axis], prepared.axisSprings[axis], prepared.axisMotors[axis]);
					}
				} else {
					const std::size_t axis = prepared.kind == PhysicsConstraintKind::Distance
						|| prepared.kind == PhysicsConstraintKind::Slider ? 0U : 3U;
					request.axes[axis] = ConvertAxisSettings(prepared.limits, prepared.spring, prepared.motor);
				}
				if(prepared.kind == PhysicsConstraintKind::Gear) {
					const auto resolveGear = [&](const TerraEngine::UUID& recordId) {
						const auto staged = stagedConstraintHandles.find(recordId);
						if(staged != stagedConstraintHandles.end()) return staged->second;
						const auto live = runtimeConstraints.find(recordId);
						return live != runtimeConstraints.end() ? live->second.handle : PhysicsConstraintHandle{};
					};
					request.gearConstraintA = resolveGear(prepared.firstGearConstraintId);
					request.gearConstraintB = resolveGear(prepared.secondGearConstraintId);
					if(!request.gearConstraintA.IsValid() || !request.gearConstraintB.IsValid()) {
						world.Release(handle);
						rejectTransaction("Gear constraint references must resolve to two live or staged authored hinge records.");
						break;
					}
				}
				request.gearRatio = prepared.gearRatio;
				request.breakForce = prepared.breakForce;
				request.breakTorque = prepared.breakTorque;
				request.collideConnected = prepared.collideConnected;
				request.enabled = prepared.enabled;
				request.resetGeneration = prepared.resetGeneration;

				Backend::PhysicsBackendResult result = backend->CreateConstraint(request, staging);
				if(!result.Succeeded()) {
					world.Release(handle);
					rejectTransaction(result.message);
					break;
				}
				RuntimeConstraint runtime{
					prepared.ownerObjectId, prepared.ownerObjectGeneration, prepared.componentGeneration,
					prepared.recordId, prepared.recordGeneration, prepared.resetGeneration, handle, request};
				PhysicsConstraintRuntimeRegistration registration{handle, prepared.ownerObjectId,
					prepared.ownerObjectGeneration, prepared.componentGeneration, prepared.recordId,
					prepared.recordGeneration, prepared.breakForce, prepared.breakTorque,
					prepared.resetGeneration};
				stagedConstraints.push_back({prepared.recordId, runtime, registration});
				stagedConstraintHandles[prepared.recordId] = handle;
			}

			for(const PhysicsCharacterDescriptor& prepared : preparedCharacters) {
				if(transactionRejected) break;
				const PhysicsCharacterHandle handle = world.AllocateCharacter();
				if(!handle.IsValid()) {
					rejectTransaction("Physics character handle capacity was exhausted.");
					break;
				}
				Backend::PhysicsCharacterCreateRequest request;
				request.handle = handle;
				request.objectId = prepared.objectId;
				request.objectGeneration = prepared.objectGeneration;
				request.componentGeneration = prepared.componentGeneration;
				request.pose = prepared.worldPose;
				request.shapeKind = prepared.shapeKind == PhysicsCharacterShapeKind::Capsule
					? PhysicsShapeKind::Capsule : PhysicsShapeKind::Cylinder;
				request.radius = prepared.radius;
				request.height = prepared.height;
				request.localCenter = prepared.localCenter;
				request.upDirection = NormalizeOr(prepared.upDirection, {0.0F, 1.0F, 0.0F});
				request.stepHeight = prepared.stepHeight;
				request.supportVolumeRadius = prepared.supportVolumeRadius;
				request.supportVolumeHeight = prepared.supportVolumeHeight;
				request.maximumSlopeAngleRadians = prepared.maximumSlopeAngleDegrees * 0.01745329251994329577F;
				request.contactPadding = prepared.contactPadding;
				request.mass = prepared.mass;
				request.maximumStrength = prepared.maximumStrength;
				request.collisionLayer = prepared.collisionLayer;
				request.collisionMask = prepared.collisionMask;
				request.groundAdhesion = prepared.groundAdhesion;
				request.gravityScale = prepared.gravityScale;
				request.jumpSpeed = prepared.jumpSpeed;
				request.allowStairWalking = prepared.allowStairWalking;
				request.useGravity = prepared.useGravity;
				request.enabled = prepared.enabled;
				Backend::PhysicsBackendResult result = backend->CreateCharacter(request, staging);
				if(!result.Succeeded()) {
					world.Release(handle);
					rejectTransaction(result.message);
					break;
				}
				stagedCharacters.push_back({prepared.objectId, RuntimeCharacter{
					prepared.objectId, prepared.objectGeneration, prepared.componentGeneration, handle}});
			}

			// Register engine-owned constraint state before provider publication. The
			// PhysicsSystem lock prevents observers from seeing this provisional state,
			// and rollback removes every registration if any later operation rejects.
			for(const StagedConstraint& constraint : stagedConstraints) {
				if(transactionRejected) break;
				if(!world.RegisterConstraint(constraint.registration)) {
					rejectTransaction("The world rejected a staged constraint runtime registration.");
					break;
				}
				registeredConstraintHandles.push_back(constraint.runtime.handle);
			}

			if(!transactionRejected && staging.IsValid()) {
				const Backend::PhysicsBackendResult committed = backend->CommitStaging(staging);
				if(!committed.Succeeded()) rejectTransaction(committed.message);
			}
			if(transactionRejected) {
				rollbackTransaction();
				AppendCommandResult(0U, PhysicsResultStatus::ProviderFailure, world.Timing().tick,
					transactionDiagnostic.empty() ? "The physics mutation batch was rejected atomically."
						: transactionDiagnostic);
			} else {
				std::unordered_set<TerraEngine::UUID> retiringConstraints(
					preparedConstraintRemovals.begin(), preparedConstraintRemovals.end());
				for(const PhysicsConstraintDescriptor& prepared : preparedConstraints) {
					retiringConstraints.insert(prepared.recordId);
				}
				std::vector<TerraEngine::UUID> orderedConstraintRetirements(
					retiringConstraints.begin(), retiringConstraints.end());
				std::stable_sort(orderedConstraintRetirements.begin(), orderedConstraintRetirements.end(),
					[this](const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
						const auto leftRecord = runtimeConstraints.find(left);
						const auto rightRecord = runtimeConstraints.find(right);
						const bool leftGear = leftRecord != runtimeConstraints.end()
							&& leftRecord->second.request.kind == PhysicsConstraintKind::Gear;
						const bool rightGear = rightRecord != runtimeConstraints.end()
							&& rightRecord->second.request.kind == PhysicsConstraintKind::Gear;
						return leftGear && !rightGear;
					});
				// Gears retire before referenced hinges, and all constraints retire
				// before endpoint bodies. These operations cannot reject after the live
				// handles and dependencies were preflighted above.
				for(const TerraEngine::UUID& recordId : orderedConstraintRetirements) {
					const auto old = runtimeConstraints.find(recordId);
					if(old == runtimeConstraints.end()) continue;
					backend->DestroyConstraint(old->second.handle);
					world.RemoveConstraint(old->second.handle);
					world.Release(old->second.handle);
					runtimeConstraints.erase(old);
				}

				std::unordered_set<TerraEngine::UUID> retiringCharacters(
					preparedCharacterRemovals.begin(), preparedCharacterRemovals.end());
				for(const PhysicsCharacterDescriptor& prepared : preparedCharacters) {
					retiringCharacters.insert(prepared.objectId);
				}
				for(const TerraEngine::UUID& objectId : retiringCharacters) {
					const auto old = runtimeCharacters.find(objectId);
					if(old == runtimeCharacters.end()) continue;
					backend->DestroyCharacter(old->second.handle);
					world.Release(old->second.handle);
					runtimeCharacters.erase(old);
				}

				std::unordered_set<TerraEngine::UUID> retiringBodies(
					preparedRemovals.begin(), preparedRemovals.end());
				for(const PreparedBody& prepared : preparedBodies) {
					retiringBodies.insert(prepared.body.objectId);
				}
				for(const TerraEngine::UUID& objectId : retiringBodies) {
					const auto old = runtimeBodies.find(objectId);
					if(old == runtimeBodies.end()) continue;
					backend->DestroyBody(old->second.body);
					backend->DestroyShape(old->second.shape);
					bodyObjects.erase(old->second.body.slotIndex);
					world.Release(old->second.body);
					world.Release(old->second.shape);
					runtimeBodies.erase(old);
				}

				for(StagedBody& body : stagedBodies) {
					bodyObjects[body.runtime.body.slotIndex] = body.objectId;
					runtimeBodies[body.objectId] = std::move(body.runtime);
				}
				for(StagedConstraint& constraint : stagedConstraints) {
					runtimeConstraints[constraint.recordId] = std::move(constraint.runtime);
				}
				for(StagedCharacter& character : stagedCharacters) {
					runtimeCharacters[character.objectId] = std::move(character.runtime);
				}
			}

			const PhysicsTick inclusiveTick = !plannedSteps.requests.empty()
				? plannedSteps.requests.back().tick
				: world.Timing().tick;
			PhysicsCommandBatch batch = world.MergeCommands(inclusiveTick);
			diagnosticCounters.commandCount += batch.commands.size();
			for(const PhysicsCommand& command : batch.commands) {
				Backend::PhysicsBackendResult result;
				if(command.type == PhysicsCommandType::ResetConstraint) {
					const auto runtime = runtimeConstraints.find(command.recordId);
					const auto* reset = std::get_if<PhysicsConstraintResetPayload>(&command.payload);
					if(runtime == runtimeConstraints.end() || reset == nullptr
						|| runtime->second.handle != command.constraint
						|| runtime->second.ownerObjectGeneration != command.objectGeneration
						|| runtime->second.componentGeneration != command.componentGeneration
						|| runtime->second.recordGeneration != command.recordGeneration
						|| reset->resetGeneration <= runtime->second.resetGeneration) {
						result = {PhysicsResultStatus::StaleTarget, Backend::PhysicsBackendErrorCode::StaleHandle,
							"The constraint reset targets a stale identity or non-increasing reset generation."};
					} else {
						result = backend->ApplyCommand(command);
						if(result.Succeeded()) {
							if(world.ResetConstraint(command.constraint, reset->resetGeneration)) {
								runtime->second.resetGeneration = reset->resetGeneration;
								runtime->second.request.resetGeneration = reset->resetGeneration;
								runtime->second.request.enabled = true;
							} else {
								runtime->second.request.enabled = false;
								backend->UpdateConstraint(runtime->second.request);
								result = {PhysicsResultStatus::Conflict, Backend::PhysicsBackendErrorCode::InvalidState,
									"Constraint reset publication rejected after provider application; provider state was disabled again."};
							}
						}
					}
				} else {
					result = backend->ApplyCommand(command);
				}
				AppendCommandResult(command.commandId,
					result.Succeeded() ? PhysicsResultStatus::Success : PhysicsResultStatus::Rejected,
					inclusiveTick, result.message);
			}
			diagnosticCounters.staleCount += batch.staleWorldGenerationCount;
			diagnosticTimings.commitMs = MillisecondsSince(start);
		}

		void SimulateLocked() {
			const Clock::time_point start = Clock::now();
			completedSteps.clear();
			for(const PhysicsFixedStepRequest& request : plannedSteps.requests) {
				const Backend::PhysicsBackendResult result = backend->Step({
					request.world, request.tick, request.deltaTime, 1U});
				if(!result.Succeeded()) {
					world.MarkFailed();
					AppendCommandResult(0U, PhysicsResultStatus::ProviderFailure, request.tick, result.message);
					break;
				}
				completedSteps.push_back(request);
			}
			diagnosticTimings.simulateMs = MillisecondsSince(start);
		}

		void ExtractLocked() {
			if(completedSteps.empty()) return;
			extraction = {};
			preparedBodyStateBatch.reset();
			preparedTransformSources.clear();
			Backend::PhysicsBackendExtractionRequest request;
			request.world = world.Handle();
			request.tick = completedSteps.back().tick;
			// Event draining is controlled by the extraction field mask as well as the
			// drainEvents flag. Always request that immutable stream so narrowing body
			// reads cannot silently disable callbacks and automation contact history.
			request.fields = PhysicsBodyStateFieldMask::BodyState
				| PhysicsBodyStateFieldMask::Events;
			// Constraint impulses and character grounding cost provider work only when
			// those runtime families exist, while still preserving their publication
			// contract in mixed scenes.
			if(!runtimeConstraints.empty()) request.fields |= PhysicsBodyStateFieldMask::Constraints;
			if(!runtimeCharacters.empty()) request.fields |= PhysicsBodyStateFieldMask::Characters;
			request.batchSize = configuredSettings.providerExtractionBatchSize;
			request.parallelThreshold = configuredSettings.parallelExtractionThreshold;
			request.fullState = world.AcquireCompleteBodyState() == nullptr;
			request.selection = request.fullState
				? Backend::PhysicsBackendExtractionSelection::FullState
				: Backend::PhysicsBackendExtractionSelection::PublicationRelevant;
			request.allowParallelReads = true;
			request.drainEvents = true;
			const Backend::PhysicsBackendResult result = backend->Extract(request, extraction);
			if(!result.Succeeded()) {
				AppendCommandResult(0U, PhysicsResultStatus::ProviderFailure, request.tick, result.message);
				extraction.bodies.clear();
			} else {
				preparedBodyStateBatch = PhysicsWorld::ConvertExtractedBodyStates(extraction);
				if(!preparedBodyStateBatch) {
					AppendCommandResult(0U, PhysicsResultStatus::StaleTarget,
						request.tick, "Provider extraction could not be converted into generation-safe engine state.");
				}
			}

			if(preparedBodyStateBatch) {
				preparedTransformSources.reserve(preparedBodyStateBatch->updates.size());
				for(const IncrementalBodyStateUpdate& update : preparedBodyStateBatch->updates) {
					if(update.removed || !HasPhysicsBodyStateFields(update.fields, PhysicsBodyStateFieldMask::Pose)) continue;
					const auto runtime = runtimeBodies.find(update.state.objectId);
					if(runtime == runtimeBodies.end() || runtime->second.body != update.state.handle
						|| runtime->second.motionType != PhysicsMotionType::Dynamic) continue;
					const RuntimeBody& body = runtime->second;
					PhysicsPose localPose = update.state.worldPose;
					if(body.hasParent) {
						glm::vec3 position = ToGlmPosition(update.state.worldPose.position);
						glm::quat rotation = ToGlmRotation(update.state.worldPose.rotation);
						const glm::vec3 parentPosition = ToGlmPosition(body.parentWorldPose.position);
						const glm::quat parentRotation = ToGlmRotation(body.parentWorldPose.rotation);
						const glm::vec3 parentScale = ToGlmPosition(body.parentWorldScale);
						position = glm::inverse(parentRotation) * (position - parentPosition);
						position = {position.x / parentScale.x, position.y / parentScale.y, position.z / parentScale.z};
						rotation = glm::normalize(glm::inverse(parentRotation) * rotation);
						localPose = {{position.x, position.y, position.z},
							{rotation.x, rotation.y, rotation.z, rotation.w}};
					}
					preparedTransformSources.push_back({
						.objectId = body.objectId,
						.body = body.body,
						.objectLifetimeGeneration = body.objectGeneration,
						.bodyGeneration = body.body.slotGeneration,
						.componentGeneration = body.componentGeneration,
						.expectedTransformRevision = body.objectTransformRevision,
						.expectedHierarchyGeneration = body.hierarchyGeneration,
						.parentObjectId = body.parentObjectId,
						.expectedParentLifetimeGeneration = body.parentObjectGeneration,
						.expectedParentTransformRevision = body.parentTransformRevision,
						.localPose = localPose,
						.worldPoseReceipt = update.state.worldPose,
						.poseChanged = !body.hasPublishedPose
							|| !SamePose(body.lastPublishedPose, update.state.worldPose)});
				}
			}
			ExecutePreparedQueriesLocked();
		}

		Backend::PhysicsBackendQueryRequest MakeBackendQueryRequest(const PhysicsQueryRequest& request) const {
			Backend::PhysicsBackendQueryRequest converted;
			converted.world = request.world;
			converted.kind = request.kind;
			converted.hitMode = request.hitMode;
			converted.pose = request.pose;
			converted.direction = request.direction;
			converted.shapeExtent = request.shapeExtent;
			converted.distance = request.distance;
			converted.filter = request.filter;
			converted.maximumHits = request.maximumHits;
			return converted;
		}

		void ExecutePreparedQueriesLocked() {
			const Clock::time_point start = Clock::now();
			executedQueries.clear();
			if(preparedQueries.queries.empty()) {
				diagnosticTimings.queryMs = 0.0;
				return;
			}
			executedQueries.resize(preparedQueries.queries.size());
			ThreadPool& pool = ThreadPool::instance();
			const std::size_t workerCount = pool.getThreadCount();
			if(workerCount <= 1U || preparedQueries.queries.size() < 64U) {
				for(std::size_t index = 0U; index < preparedQueries.queries.size(); ++index) {
					executedQueries[index] = backend->Query(
						MakeBackendQueryRequest(preparedQueries.queries[index].request));
				}
				diagnosticTimings.queryMs = MillisecondsSince(start);
				return;
			}

			// Each task writes a disjoint result range and all backend calls take a
			// shared read lock after simulation has joined. No task touches scene data.
			const std::size_t taskCount = std::min(workerCount - 1U, preparedQueries.queries.size());
			const std::size_t chunkSize = (preparedQueries.queries.size() + taskCount - 1U) / taskCount;
			std::vector<std::future<void>> futures;
			futures.reserve(taskCount);
			for(std::size_t begin = 0U; begin < preparedQueries.queries.size(); begin += chunkSize) {
				const std::size_t end = std::min(preparedQueries.queries.size(), begin + chunkSize);
				auto task = pool.createTask([this, begin, end]() {
					for(std::size_t index = begin; index < end; ++index) {
						executedQueries[index] = backend->Query(
							MakeBackendQueryRequest(preparedQueries.queries[index].request));
					}
				});
				futures.push_back(task->getFuture());
				pool.enqueue(task);
			}
			for(std::future<void>& future : futures) future.get();
			diagnosticTimings.queryMs = MillisecondsSince(start);
		}

		bool IsCurrentParticipant(const PhysicsEventParticipant& participant) const {
			if(world.Status(participant.body) != PhysicsHandleStatus::Alive) return false;
			const auto indexed = bodyObjects.find(participant.body.slotIndex);
			if(indexed == bodyObjects.end()) return false;
			const auto runtime = runtimeBodies.find(indexed->second);
			return runtime != runtimeBodies.end()
				&& runtime->second.body == participant.body
				&& runtime->second.objectId == participant.objectId
				&& runtime->second.objectGeneration == participant.objectGeneration
				&& runtime->second.componentGeneration == participant.componentGeneration;
		}

		std::vector<PhysicsQueryHit> ConvertQueryHits(
			const Backend::PhysicsBackendQueryResult& provider,
			std::uint64_t& staleCount) const {
			std::vector<PhysicsQueryHit> hits;
			hits.reserve(provider.hits.size());
			for(const Backend::PhysicsBackendQueryHit& source : provider.hits) {
				if(world.Status(source.body) != PhysicsHandleStatus::Alive) {
					++staleCount;
					continue;
				}
				const auto indexed = bodyObjects.find(source.body.slotIndex);
				if(indexed == bodyObjects.end()) {
					++staleCount;
					continue;
				}
				const auto runtime = runtimeBodies.find(indexed->second);
				if(runtime == runtimeBodies.end() || runtime->second.body != source.body
					|| runtime->second.objectId != source.objectId
					|| runtime->second.objectGeneration != source.objectGeneration
					|| runtime->second.componentGeneration != source.componentGeneration) {
					++staleCount;
					continue;
				}
				PhysicsQueryHit hit;
				hit.body = source.body;
				hit.objectId = source.objectId;
				hit.objectGeneration = source.objectGeneration;
				hit.componentGeneration = source.componentGeneration;
				hit.materialId = source.materialId;
				hit.surfaceTag = source.surfaceTag;
				hit.position = source.position;
				hit.normal = source.normal;
				hit.distance = source.distance;
				hit.fraction = source.fraction;
				hit.subShapeKey = source.subShapeKey;
				hit.collisionLayer = source.collisionLayer;
				hit.motionType = source.motionType;
				hit.sensor = source.sensor;
				hits.push_back(std::move(hit));
			}
			return hits;
		}

		void PublishTransformTransactionsLocked(const IncrementalBodyStatePublication& publication) {
			if(preparedTransformSources.empty() || !authoring || !publication.complete) {
				++publicationDiagnostics.emptyPacketsSkipped;
				publicationDiagnostics.currentEntries = 0U;
				publicationDiagnostics.currentChunks = 0U;
				return;
			}
			const PhysicsAuthoringData& authored = authoring->Data();
			const std::uint64_t groupId = ++nextTransformPublicationGroupId;
			const std::uint64_t packetGeneration = ++nextTransformPacketGeneration;
			TransformPublicationPacket packet = PrepareTransformPublicationPacket(
				{publication.complete->world, publication.complete->tick,
					publication.complete->storeGeneration, authored.snapshotGeneration},
				authored.sceneGeneration,
				authored.hierarchyGeneration,
				groupId == 0U ? ++nextTransformPublicationGroupId : groupId,
				packetGeneration == 0U ? ++nextTransformPacketGeneration : packetGeneration,
				preparedTransformSources,
				configuredSettings.transformPublicationChunkSize,
				TerraEngine::Scheduling::SameFramePhysicsPublicationConsumerActive()
					? TransformPublicationVisibilityBoundary::AfterPhysics
					: TransformPublicationVisibilityBoundary::FrameEnd);
			if(packet.entries.empty()) {
				++publicationDiagnostics.emptyPacketsSkipped;
				publicationDiagnostics.currentEntries = 0U;
				publicationDiagnostics.currentChunks = 0U;
				return;
			}
			++publicationDiagnostics.preparedPackets;
			publicationDiagnostics.currentEntries = packet.entries.size();
			publicationDiagnostics.currentChunks = packet.chunks.size();
			publicationDiagnostics.targetBoundary = packet.targetBoundary == TransformPublicationVisibilityBoundary::AfterPhysics
				? "after_physics" : "frame_end";

			PhysicsTransactionRuntimeState runtime;
			runtime.world = world.Handle();
			runtime.bodyHandles.reserve(runtimeBodies.size());
			runtime.bodyGenerations.reserve(runtimeBodies.size());
			for(const auto& [objectId, body] : runtimeBodies) {
				runtime.bodyHandles.emplace(objectId, body.body);
				runtime.bodyGenerations.emplace(objectId, body.body.slotGeneration);
			}
			const TransformPublicationValidationResult validation =
				ValidatePhysicsTransformPublication(ObjectManager::instance(), runtime, packet);
			if(!validation.accepted) {
				++diagnosticCounters.rejectedCount;
				++publicationDiagnostics.rejectedPackets;
				publicationDiagnostics.lastFailure = validation.message;
				return;
			}

			Scene::TransactionPublicationGroup group = BuildTransformPublicationGroup(packet);
			pendingTransformPublications.push_back({packet, group.groupId});
			Scene::SceneMutationQueue::Instance().EnqueuePublicationGroup(std::move(group));
		}

		void PublishLocked() {
			const Clock::time_point start = Clock::now();
			if(completedSteps.empty()) return;
			if(preparedBodyStateBatch) {
				const auto publication = world.PublishPreparedBodyStates(*preparedBodyStateBatch);
				if(publication) {
					latestIncrementalBodies = publication->complete;
					// The flat compatibility view is materialized only when a legacy
					// caller asks for it, so unchanged pages remain shared in the hot path.
					latestBodies.reset();
					lastStatePagesReused = publication->pagesReused;
					lastStatePagesCopied = publication->pagesCopied;
					++stateStorePublicationCount;
					PublishTransformTransactionsLocked(*publication);
				} else {
					++stateStoreRejectedBatchCount;
					++diagnosticCounters.rejectedCount;
				}
			}
			latestCommands = std::make_shared<const PhysicsCommandResultSnapshot>(PhysicsCommandResultSnapshot{
				world.Handle(), extraction.tick,
				std::make_shared<const std::vector<PhysicsCommandResult>>(commandResults)});

			for(std::size_t index = 0U; index < preparedQueries.queries.size(); ++index) {
				const Backend::PhysicsBackendQueryResult& provider = executedQueries[index];
				std::uint64_t stale = 0U;
				std::vector<PhysicsQueryHit> hits = ConvertQueryHits(provider, stale);
				diagnosticCounters.staleCount += stale;
				diagnosticCounters.queryHitCount += hits.size();
				diagnosticCounters.queryTruncatedCount += provider.truncated ? 1U : 0U;
				world.CompleteQuery(preparedQueries.queries[index], extraction.tick,
					provider.result.Succeeded() ? PhysicsResultStatus::Success : PhysicsResultStatus::ProviderFailure,
					hits, provider.truncated, provider.result.message);
			}
			diagnosticCounters.queryCount += preparedQueries.queries.size();

			std::vector<PhysicsConstraintImpulseSample> constraintSamples;
			constraintSamples.reserve(extraction.constraints.size());
			for(const Backend::PhysicsBackendConstraintState& provider : extraction.constraints) {
				const auto live = runtimeConstraints.find(provider.recordId);
				if(live == runtimeConstraints.end() || live->second.handle != provider.handle
					|| live->second.ownerObjectId != provider.ownerObjectId
					|| live->second.ownerObjectGeneration != provider.ownerObjectGeneration
					|| live->second.componentGeneration != provider.componentGeneration
					|| live->second.recordGeneration != provider.recordGeneration) {
					++diagnosticCounters.staleCount;
					continue;
				}
				constraintSamples.push_back({provider.handle, provider.ownerObjectId,
					provider.ownerObjectGeneration, provider.componentGeneration, provider.recordId,
					provider.recordGeneration, live->second.resetGeneration,
					provider.linearImpulseMagnitude, provider.angularImpulseMagnitude});
			}

			std::vector<PhysicsEvent> breakEvents;
			if(!constraintSamples.empty()) {
				world.EvaluateConstraintImpulses(extraction.tick, completedSteps.back().deltaTime,
					constraintSamples, breakEvents);
				for(const PhysicsEvent& event : breakEvents) {
					const auto live = runtimeConstraints.find(event.constraintRecordId);
					if(live == runtimeConstraints.end()) continue;
					live->second.request.enabled = false;
					const Backend::PhysicsBackendResult disabled = backend->UpdateConstraint(live->second.request);
					if(!disabled.Succeeded()) {
						world.MarkFailed();
						AppendCommandResult(0U, PhysicsResultStatus::ProviderFailure, extraction.tick,
							"A broken constraint could not be disabled at the protected publication boundary.");
					}
				}
			}

			std::vector<PhysicsCharacterState> characterStates;
			characterStates.reserve(extraction.characters.size());
			for(const Backend::PhysicsBackendCharacterState& provider : extraction.characters) {
				const auto live = runtimeCharacters.find(provider.objectId);
				if(live == runtimeCharacters.end() || live->second.handle != provider.handle
					|| live->second.objectGeneration != provider.objectGeneration
					|| live->second.componentGeneration != provider.componentGeneration) {
					++diagnosticCounters.staleCount;
					continue;
				}
				PhysicsCharacterGroundState groundState = PhysicsCharacterGroundState::InAir;
				switch(provider.groundState) {
				case Backend::PhysicsCharacterGroundState::OnGround: groundState = PhysicsCharacterGroundState::OnGround; break;
				case Backend::PhysicsCharacterGroundState::OnSteepGround: groundState = PhysicsCharacterGroundState::OnSteepGround; break;
				case Backend::PhysicsCharacterGroundState::NotSupported: groundState = PhysicsCharacterGroundState::NotSupported; break;
				case Backend::PhysicsCharacterGroundState::InAir: groundState = PhysicsCharacterGroundState::InAir; break;
				}
				PhysicsBodyHandle support = provider.supportBody;
				if(support.IsValid() && world.Status(support) != PhysicsHandleStatus::Alive) support = {};
				characterStates.push_back({provider.handle, provider.objectId, provider.objectGeneration,
					provider.componentGeneration, world.Handle(), extraction.tick, provider.pose,
					provider.linearVelocity, groundState, provider.groundPosition, provider.groundNormal,
					provider.supportVelocity, support, provider.contactCount, provider.supported});
			}
			if(!characterStates.empty()) world.PublishCharacterStates(extraction.tick, characterStates);
			latestCharacters = world.AcquireCharacterStatePacket(extraction.tick, extraction.tick);

			std::vector<PhysicsEvent> currentEvents;
			currentEvents.reserve(extraction.events.size() + breakEvents.size());
			std::uint64_t staleEvents = 0U;
			for(const PhysicsEvent& event : extraction.events) {
				if(!IsCurrentParticipant(event.first)
					|| (event.hasSecondParticipant && !IsCurrentParticipant(event.second))) {
					++staleEvents;
					continue;
				}
				currentEvents.push_back(event);
			}
			currentEvents.insert(currentEvents.end(), breakEvents.begin(), breakEvents.end());
			diagnosticCounters.staleCount += staleEvents;
			const Clock::time_point eventStart = Clock::now();
			latestEvents = world.PublishEvents(
				extraction.tick, currentEvents, extraction.droppedEventCount + staleEvents);
			if(latestEvents != nullptr) {
				diagnosticCounters.eventCount += latestEvents->events->size();
				diagnosticCounters.eventFilteredCount += latestEvents->filteredCount;
				diagnosticCounters.eventDroppedCount += latestEvents->droppedCount;
				diagnosticQueues.eventDepth = latestEvents->events->size();
			}
			diagnosticTimings.eventPublishMs = MillisecondsSince(eventStart);
			diagnosticCounters.bodyCount = runtimeBodies.size();
			diagnosticCounters.shapeCount = runtimeBodies.size();
			diagnosticCounters.contactCount = extraction.contactCount;

			if(world.Settings().debugVisualizationEnabled) {
				// Debug data is copied at the protected publish boundary. Every vector is
				// immutable after this point, so Scene View and profiler redraws never
				// retain a provider object or read a world that is being stepped.
				const std::vector<PhysicsBodyState> debugStates = latestIncrementalBodies
					? latestIncrementalBodies->Flatten()
					: std::vector<PhysicsBodyState>{};
				const std::size_t maximumPrimitives = world.Settings().maxDebugPrimitives;
				std::size_t primitiveCount = 0U;
				auto hasCapacity = [&]() { return primitiveCount < maximumPrimitives; };
				auto lines = std::make_shared<std::vector<PhysicsDebugLine>>();
				auto triangles = std::make_shared<std::vector<PhysicsDebugTriangle>>();
				auto contacts = std::make_shared<std::vector<PhysicsDebugContact>>();
				auto constraints = std::make_shared<std::vector<PhysicsDebugConstraint>>();
				auto characters = std::make_shared<std::vector<PhysicsDebugCharacter>>();
				auto queries = std::make_shared<std::vector<PhysicsDebugQuery>>();

				// Preserve at least one useful item from each available category before
				// body wireframes consume the remainder of a deliberately small budget.
				for(const PhysicsEvent& event : currentEvents) {
					if(!hasCapacity()) break;
					if(event.type != PhysicsEventType::Contact
						&& event.type != PhysicsEventType::CollisionEnter
						&& event.type != PhysicsEventType::CollisionStay) continue;
					contacts->push_back({event.point, event.normal, event.impulse});
					++primitiveCount;
				}
				for(const auto& [recordId, runtime] : runtimeConstraints) {
					(void)recordId;
					if(!hasCapacity()) break;
					constraints->push_back({runtime.request.localFrameA.position,
						runtime.request.localFrameB.position,
						!runtime.request.enabled});
					++primitiveCount;
				}
				for(const PhysicsCharacterState& character : characterStates) {
					if(!hasCapacity()) break;
					float radius = 0.5F;
					float height = 2.0F;
					if(authoring) {
						const auto& authoredCharacters = authoring->Data().characters;
						const auto authored = std::find_if(authoredCharacters.begin(), authoredCharacters.end(),
							[&](const PhysicsCharacterDescriptor& value) { return value.objectId == character.objectId; });
						if(authored != authoredCharacters.end()) {
							radius = authored->radius;
							height = authored->height;
						}
					}
					characters->push_back({character.pose.position, character.groundNormal, radius, height});
					++primitiveCount;
				}
				for(const PhysicsPreparedQuery& prepared : preparedQueries.queries) {
					if(!hasCapacity()) break;
					queries->push_back({prepared.request.pose.position, prepared.request.direction, prepared.request.distance});
					++primitiveCount;
				}

				// Draw authored collider bounds from the immutable authoring/body
				// snapshots. The simple wire bounds are provider-neutral and remain useful
				// for every shape family, including cooked meshes whose triangles stay in
				// the asset cache rather than crossing the diagnostics boundary.
				if(authoring) {
					for(const PhysicsColliderDescriptor& collider : authoring->Data().colliders) {
						if(!hasCapacity() || !collider.enabled) break;
						const auto body = std::find_if(debugStates.begin(), debugStates.end(),
							[&](const PhysicsBodyState& value) { return value.objectId == collider.objectId; });
						if(body == debugStates.end()) continue;
						const PhysicsVector3 center{
							body->worldPose.position.x + collider.localPose.position.x,
							body->worldPose.position.y + collider.localPose.position.y,
							body->worldPose.position.z + collider.localPose.position.z};
						const PhysicsVector3 extent{
							std::max(0.02F, std::fabs(collider.dimensions.x) * 0.5F),
							std::max(0.02F, std::fabs(collider.dimensions.y) * 0.5F),
							std::max(0.02F, std::fabs(collider.dimensions.z) * 0.5F)};
						const std::array<PhysicsVector3, 8U> corners{{
							{center.x - extent.x, center.y - extent.y, center.z - extent.z},
							{center.x + extent.x, center.y - extent.y, center.z - extent.z},
							{center.x + extent.x, center.y + extent.y, center.z - extent.z},
							{center.x - extent.x, center.y + extent.y, center.z - extent.z},
							{center.x - extent.x, center.y - extent.y, center.z + extent.z},
							{center.x + extent.x, center.y - extent.y, center.z + extent.z},
							{center.x + extent.x, center.y + extent.y, center.z + extent.z},
							{center.x - extent.x, center.y + extent.y, center.z + extent.z}}};
						constexpr std::array<std::array<std::size_t, 2U>, 12U> edges{{
							{{0U, 1U}}, {{1U, 2U}}, {{2U, 3U}}, {{3U, 0U}},
							{{4U, 5U}}, {{5U, 6U}}, {{6U, 7U}}, {{7U, 4U}},
							{{0U, 4U}}, {{1U, 5U}}, {{2U, 6U}}, {{3U, 7U}}}};
						const std::uint32_t color = collider.trigger ? 0xFFFFA040U : 0xFF40C0FFU;
						for(const auto& edge : edges) {
							if(!hasCapacity()) break;
							lines->push_back({corners[edge[0]], corners[edge[1]], color});
							++primitiveCount;
						}
						if(hasCapacity()) {
							triangles->push_back({corners[4U], corners[5U], corners[6U], color & 0x60FFFFFFU});
							++primitiveCount;
						}
					}
				}

				constexpr float axisLength = 0.35F;
				for(const PhysicsBodyState& state : debugStates) {
					if(hasCapacity()) {
						lines->push_back({state.worldPose.position,
							{state.worldPose.position.x + axisLength, state.worldPose.position.y, state.worldPose.position.z},
							0xFF4040FFU});
						++primitiveCount;
					}
					if(hasCapacity()) {
						lines->push_back({state.worldPose.position,
							{state.worldPose.position.x, state.worldPose.position.y + axisLength, state.worldPose.position.z},
							0xFF40FF40U});
						++primitiveCount;
					}
					if(hasCapacity()) {
						lines->push_back({state.worldPose.position,
							{state.worldPose.position.x, state.worldPose.position.y, state.worldPose.position.z + axisLength},
							0xFFFF4040U});
						++primitiveCount;
					}
					if(hasCapacity()) {
						const PhysicsVector3& center = state.worldPose.position;
						triangles->push_back({
							{center.x - 0.15F, center.y, center.z - 0.15F},
							{center.x + 0.15F, center.y, center.z - 0.15F},
							{center.x, center.y, center.z + 0.15F}, 0x6040A0FFU});
						++primitiveCount;
					}
					if(!hasCapacity()) break;
				}

				auto packet = std::make_shared<PhysicsDebugPacket>();
				packet->world = world.Handle();
				packet->producedTick = extraction.tick;
				packet->expiryTick = extraction.tick + world.Settings().retainedDebugTicks;
				packet->lines = std::move(lines);
				packet->triangles = std::move(triangles);
				packet->contacts = std::move(contacts);
				packet->constraints = std::move(constraints);
				packet->characters = std::move(characters);
				packet->queries = std::move(queries);
				PublishedPhysicsDebugPackets().Publish(std::move(packet));
			}
			diagnosticTimings.publishMs = MillisecondsSince(start);
		}

		void PublicationBarrierLocked() {
			// The scheduler's protected After Physics flush runs immediately after
			// this workload. Retain requests and pose receipts until that boundary
			// reports whether each generation-validated transaction committed.
			pendingFlushSteps.insert(
				pendingFlushSteps.end(), completedSteps.begin(), completedSteps.end());
			const PhysicsWorldRegistryStatistics registry = world.RegistryStatistics();
			const PhysicsCommandQueueDiagnostics queue = world.CommandQueueDiagnostics();
			const PhysicsQueryDiagnostics queryDiagnostics = world.QueryDiagnostics();
			diagnosticCounters.bodyCount = registry.bodyCount;
			diagnosticCounters.shapeCount = registry.shapeCount;
			diagnosticCounters.constraintCount = registry.constraintCount;
			diagnosticCounters.characterCount = registry.characterCount;
			diagnosticQueues.commandDepth = queue.queuedDepth;
			diagnosticCounters.commandCancelledCount = queue.cancelledCount;
			diagnosticQueues.queryDepth = queryDiagnostics.queuedDepth + queryDiagnostics.preparedDepth;
			const PhysicsCookedShapeCacheStatistics cacheStatistics = world.CookedShapeCacheStatistics();
			PhysicsDiagnosticsCache cache;
			cache.sharedShapeCount = cacheStatistics.sharedShapeCount;
			cache.assetCount = cacheStatistics.assetCount;
			cache.payloadBytes = cacheStatistics.payloadBytes;
			cache.hitCount = cacheStatistics.hitCount;
			cache.missCount = cacheStatistics.missCount;
			cache.publicationCount = cacheStatistics.publicationCount;
			cache.retirementCount = cacheStatistics.retirementCount;

			PhysicsDiagnosticsMemory memory;
			memory.snapshotBytes = (latestIncrementalBodies ? latestIncrementalBodies->retainedBytes : 0U)
				+ sizeof(PhysicsCommandResult) * (latestCommands && latestCommands->results ? latestCommands->results->size() : 0U)
				+ sizeof(PhysicsCharacterState) * (latestCharacters && latestCharacters->states ? latestCharacters->states->size() : 0U)
				+ sizeof(PhysicsEvent) * (latestEvents && latestEvents->events ? latestEvents->events->size() : 0U);
			if(const auto* jolt = dynamic_cast<const Backend::JoltPhysicsBackend*>(backend.get())) {
				const Backend::PhysicsAllocationDiagnostics allocations = jolt->AllocationDiagnostics();
				memory.outstandingProviderAllocations = allocations.outstandingAllocations;
				memory.highWaterProviderAllocations = allocations.highWaterOutstandingAllocations;
			}
			diagnosticSnapshotHighWaterBytes = std::max(diagnosticSnapshotHighWaterBytes, memory.snapshotBytes);
			memory.highWaterBytes = diagnosticSnapshotHighWaterBytes;

			PhysicsAuthoringDiagnostics authoringDiagnostics;
			authoringDiagnostics.snapshotReused = authoringSnapshotReused;
			if(authoringGeneration) {
				const PhysicsAuthoringGenerationData& data = authoringGeneration->Data();
				authoringDiagnostics.authoringGeneration = data.generation;
				authoringDiagnostics.snapshotGeneration = data.snapshotGeneration;
				authoringDiagnostics.assetJournalGeneration = data.assetJournalGeneration;
				authoringDiagnostics.changedObjects = authoringSnapshotReused ? 0U : data.changes.size();
				authoringDiagnostics.assetObservations = authoringSnapshotReused ? 0U : data.assetObservations.size();
				authoringDiagnostics.fullAssetRefresh = !authoringSnapshotReused && data.fullAssetRefresh;
				if(!authoringSnapshotReused) for(const PhysicsAuthoringChange& change : data.changes) {
					const auto count = [&change](PhysicsAuthoringChangeReason reason) {
						return HasAnyReason(change.reasons, reason) ? 1ULL : 0ULL;
					};
					authoringDiagnostics.added += count(PhysicsAuthoringChangeReason::Added);
					authoringDiagnostics.removed += count(PhysicsAuthoringChangeReason::Removed);
					authoringDiagnostics.transforms += count(PhysicsAuthoringChangeReason::Transform);
					authoringDiagnostics.bodyComponents += count(PhysicsAuthoringChangeReason::BodyComponent);
					authoringDiagnostics.colliderComponents += count(PhysicsAuthoringChangeReason::ColliderComponent);
					authoringDiagnostics.hierarchyDependencies += count(PhysicsAuthoringChangeReason::HierarchyDependency);
					authoringDiagnostics.colliderAssets += count(PhysicsAuthoringChangeReason::ColliderAsset);
					authoringDiagnostics.materialAssets += count(PhysicsAuthoringChangeReason::MaterialAsset);
					authoringDiagnostics.settings += count(PhysicsAuthoringChangeReason::Settings);
					authoringDiagnostics.lifecycle += count(PhysicsAuthoringChangeReason::Lifecycle);
				}
			}

			PhysicsPreparationDiagnostics preparationDiagnostics;
			preparationDiagnostics.scratchActiveItems = preparationScratch.ActiveItems();
			preparationDiagnostics.scratchHighWatermark = preparationScratch.HighWatermark();
			preparationDiagnostics.scratchAllocations = preparationScratch.AllocationCount();
			preparationDiagnostics.scratchReuses = preparationScratch.ReuseCount();
			preparationDiagnostics.rejected = lastPreparationRejected;
			if(preparationPlan && preparationPlanExecutedThisFrame) {
				const PhysicsPreparationPlanData& data = preparationPlan->Data();
				preparationDiagnostics.executionPath = ToDiagnosticsPath(data.executionPath);
				preparationDiagnostics.estimatedWorkItems = data.estimatedWorkItems;
				preparationDiagnostics.rangeCount = data.ranges.size();
				preparationDiagnostics.creates = data.creates.size();
				preparationDiagnostics.removals = data.removals.size();
				preparationDiagnostics.updates = data.updates.size();
				preparationDiagnostics.dependencyRebuilds = data.dependencyRebuilds.size();
				preparationDiagnostics.failure = data.rejectionReason;
			}

			PhysicsExtractionDiagnostics extractionDiagnostics;
			extractionDiagnostics.providerOrderGeneration = extraction.providerOrderGeneration;
			extractionDiagnostics.totalBodies = extraction.totalBodyCount;
			extractionDiagnostics.activeBodies = extraction.activeBodyCount;
			extractionDiagnostics.examinedBodies = extraction.examinedBodyCount;
			extractionDiagnostics.extractedBodies = extraction.extractedBodyCount;
			extractionDiagnostics.convertedBodies = preparedBodyStateBatch ? preparedBodyStateBatch->updates.size() : 0U;
			extractionDiagnostics.removedBodies = extraction.removedBodies.size();
			extractionDiagnostics.rangeCount = extraction.extractionRangeCount;
			extractionDiagnostics.fieldMask = static_cast<std::uint32_t>(extraction.extractedFields);
			extractionDiagnostics.fullState = extraction.fullState;
			extractionDiagnostics.parallelReads = extraction.usedParallelReads;
			extractionDiagnostics.serialFallback = extraction.usedSerialFallback;
			extractionDiagnostics.eventDrain = extraction.eventDrainPerformed;
			if(const auto changed = world.AcquireChangedBodyState()) {
				extractionDiagnostics.changedBodies = changed->changes ? changed->changes->size() : 0U;
			}

			PhysicsStateStoreDiagnostics stateStoreDiagnostics;
			stateStoreDiagnostics.publicationCount = stateStorePublicationCount;
			stateStoreDiagnostics.rejectedBatchCount = stateStoreRejectedBatchCount;
			stateStoreDiagnostics.pagesReused = lastStatePagesReused;
			stateStoreDiagnostics.pagesCopied = lastStatePagesCopied;
			if(latestIncrementalBodies) {
				stateStoreDiagnostics.storeGeneration = latestIncrementalBodies->storeGeneration;
				stateStoreDiagnostics.providerOrderGeneration = latestIncrementalBodies->providerOrderGeneration;
				stateStoreDiagnostics.pageCount = latestIncrementalBodies->pages.size();
				stateStoreDiagnostics.bodyCount = latestIncrementalBodies->bodyCount;
				stateStoreDiagnostics.retainedBytes = latestIncrementalBodies->retainedBytes;
			}
			publicationDiagnostics.pendingGroups = pendingTransformPublications.size();

			const bool currentFailure = world.State() == PhysicsWorldState::Failed
				|| lastPreparationRejected || !publicationDiagnostics.lastFailure.empty();
			PhysicsDiagnosticsRate rate;
			rate.aggregateIntervalFrames = configuredSettings.aggregateDiagnosticsIntervalFrames;
			rate.aggregateRefreshed = ShouldRefreshPhysicsAggregateDiagnostics(
				world.Timing().completedTick, rate.aggregateIntervalFrames, lastAggregateDiagnosticsTick, currentFailure);
			if(rate.aggregateRefreshed) lastAggregateDiagnosticsTick = world.Timing().completedTick;
			rate.lastAggregateTick = lastAggregateDiagnosticsTick;
			auto scheduler = std::make_shared<std::vector<PhysicsSchedulerDeclaration>>();
			const TerraEngine::Scheduling::SameFramePhysicsPublicationState publicationState =
				TerraEngine::Scheduling::SameFramePhysicsPublicationStateSnapshot();
			scheduler->push_back({"PhysicsSnapshot", "Physics", "SceneGraph,PhysicsSettings", "PhysicsAuthoringSnapshot", "protected snapshot", false,
				true, "active physics frame", "", authoringDiagnostics.changedObjects});
			scheduler->push_back({"PhysicsPrepare", "Physics", "PhysicsAuthoringSnapshot,PhysicsQueryQueue", "PhysicsPreparedWork,PhysicsPreparedQueries", "none", true,
				true, "snapshot or fixed-step input available", "PhysicsSnapshot", preparationDiagnostics.estimatedWorkItems});
			scheduler->push_back({"PhysicsCommandCommit", "Physics", "PhysicsPreparedWork", "PhysicsWorld", "hard boundary", false,
				true, "prepared provider mutation or command work available", "PhysicsPrepare", diagnosticCounters.commandCount});
			scheduler->push_back({"PhysicsSimulate", "Physics", "PhysicsWorld", "PhysicsWorld", "provider ownership", true,
				!completedSteps.empty(), completedSteps.empty() ? "no fixed step planned" : "fixed step planned", "PhysicsCommandCommit", completedSteps.size()});
			scheduler->push_back({"PhysicsExtract", "Physics", "PhysicsWorld,PhysicsPreparedQueries", "PhysicsPublishedSnapshots,PhysicsBackendQueryResults,PhysicsRawEvents", "immutable provider snapshot then protected event drain", true,
				!completedSteps.empty(), completedSteps.empty() ? "no completed provider step" : "provider step completed", "PhysicsSimulate", extractionDiagnostics.extractedBodies});
			scheduler->push_back({"PhysicsPublish", "Physics", "PhysicsPublishedSnapshots,PhysicsEventSubscriptions", "PhysicsQueryResults,PhysicsEventPackets,PhysicsTransformTransactions", "hard boundary", false,
				!completedSteps.empty(), completedSteps.empty() ? "no extracted packet" : "extracted packet ready", "PhysicsExtract", publicationDiagnostics.currentEntries});
			scheduler->push_back({"PhysicsPublicationBarrier", "Physics", "PhysicsTransformTransactions", "SceneGraph",
				publicationState.active ? "After Physics" : "Frame End", false,
				!completedSteps.empty(), publicationState.reason, "PhysicsPublish", pendingTransformPublications.size()});
			PhysicsDiagnosticsSnapshot diagnostics;
			diagnostics.world = world.Handle();
			diagnostics.tick = world.Timing().completedTick;
			diagnostics.state = world.State();
			diagnostics.counters = diagnosticCounters;
			diagnostics.timings = diagnosticTimings;
			diagnostics.memory = memory;
			diagnostics.cache = cache;
			diagnostics.queues = diagnosticQueues;
			diagnostics.authoring = std::move(authoringDiagnostics);
			diagnostics.preparation = std::move(preparationDiagnostics);
			diagnostics.extraction = extractionDiagnostics;
			diagnostics.stateStore = stateStoreDiagnostics;
			diagnostics.publication = publicationDiagnostics;
			diagnostics.rate = rate;
			diagnostics.scheduler = std::move(scheduler);
			latestDiagnostics = std::make_shared<const PhysicsDiagnosticsSnapshot>(std::move(diagnostics));
			PublishPhysicsDiagnosticsSnapshot(latestDiagnostics);
			completedSteps.clear();
		}

		void CompleteAfterPhysicsFlushLocked(
			const std::vector<Scene::TransactionResult>& transactionResults,
			const std::vector<Scene::TransactionPublicationGroupResult>& publicationGroupResults) {
			const bool hasPhysicsPublications = !pendingTransformPublications.empty();
			bool onlyPhysicsPublications = hasPhysicsPublications
				&& transactionResults.empty()
				&& publicationGroupResults.size() == pendingTransformPublications.size();
			if(onlyPhysicsPublications) {
				for(const Scene::TransactionPublicationGroupResult& result : publicationGroupResults) {
					const bool belongsToPhysics = std::any_of(
						pendingTransformPublications.begin(), pendingTransformPublications.end(),
						[&result](const PendingTransformPublication& publication) {
							return publication.groupId == result.groupId;
						});
					if(!belongsToPhysics) {
						onlyPhysicsPublications = false;
						break;
					}
				}
			}
			std::unordered_map<std::string, const Scene::TransactionPublicationGroupResult*> resultsByGroup;
			resultsByGroup.reserve(publicationGroupResults.size());
			for(const Scene::TransactionPublicationGroupResult& result : publicationGroupResults) {
				resultsByGroup.emplace(result.groupId, &result);
			}

			for(const PendingTransformPublication& publication : pendingTransformPublications) {
				const auto result = resultsByGroup.find(publication.groupId);
				if(result != resultsByGroup.end()) {
					const TransformPublicationReceipt receipt =
						MakeTransformPublicationReceipt(publication.packet, *result->second);
					if(receipt.Accepted()) {
						++publicationDiagnostics.acceptedPackets;
						publicationDiagnostics.acknowledgedEntries += receipt.acknowledgedEntries.size();
						publicationDiagnostics.lastFailure.clear();
						for(const TransformPublicationEntryReceipt& acknowledged : receipt.acknowledgedEntries) {
							auto runtime = runtimeBodies.find(acknowledged.objectId);
							if(runtime == runtimeBodies.end() || runtime->second.body != acknowledged.body) continue;
							runtime->second.lastPublishedPose = acknowledged.worldPose;
							runtime->second.hasPublishedPose = true;
							const auto object = ObjectManager::instance().getObject(acknowledged.objectId);
							if(object) {
								runtime->second.objectTransformRevision =
									ObjectManager::instance().GetObjectTransformRevision(*object);
							}
						}
						continue;
					}
				}
				{
					// Leave lastPublishedPose unchanged so the next extraction retries a
					// rejected write even if the provider body has already gone to sleep.
					++diagnosticCounters.rejectedCount;
					++publicationDiagnostics.rejectedPackets;
					publicationDiagnostics.lastFailure = "Transform publication did not commit at its declared boundary.";
				}
			}
			pendingTransformPublications.clear();
			if(onlyPhysicsPublications) {
				// Mixed flushes can contain a genuine external authoring edit, so only
				// acknowledge the revision when every mutation came from physics itself.
				authoringEquivalentSceneSpatialRevision = ObjectManager::instance().GetSpatialRevision();
				authoringSceneRevisionValid = true;
			}

			for(const PhysicsFixedStepRequest& request : pendingFlushSteps) {
				if(world.MarkStepCompleted(request) && request.explicitPausedStep) {
					++explicitStepPublicationCount;
				}
			}
			pendingFlushSteps.clear();
		}

		std::unique_ptr<Backend::IPhysicsBackend> backend;
		PhysicsWorld world;
		bool backendInitialized = false;
		bool editorLifecycleControlled = false;
		std::optional<PhysicsAuthoringSnapshot> authoring;
		std::optional<PhysicsAuthoringGeneration> authoringGeneration;
		std::optional<PhysicsPreparationPlan> preparationPlan;
		PhysicsPreparationScratch preparationScratch;
		PhysicsAssetGeneration observedPhysicsAssetGeneration = 0U;
		std::uint64_t authoringEquivalentSceneSpatialRevision = 0U;
		bool authoringSceneRevisionValid = false;
		PhysicsGeneration lastPreparedAuthoringSnapshotGeneration = 0U;
		bool authoringSnapshotReused = false;
		bool lastPreparationRejected = false;
		bool preparationPlanExecutedThisFrame = false;
		std::vector<PreparedBody> preparedBodies;
		std::vector<TerraEngine::UUID> preparedRemovals;
		std::vector<PhysicsConstraintDescriptor> preparedConstraints;
		std::vector<TerraEngine::UUID> preparedConstraintRemovals;
		std::vector<PhysicsCharacterDescriptor> preparedCharacters;
		std::vector<TerraEngine::UUID> preparedCharacterRemovals;
		PhysicsFixedStepPlan plannedSteps;
		PhysicsPreparedQueryBatch preparedQueries;
		std::vector<Backend::PhysicsBackendQueryResult> executedQueries;
		std::vector<PhysicsFixedStepRequest> completedSteps;
		std::vector<PhysicsFixedStepRequest> pendingFlushSteps;
		std::vector<PendingTransformPublication> pendingTransformPublications;
		Backend::PhysicsBackendExtraction extraction;
		std::optional<IncrementalBodyStateUpdateBatch> preparedBodyStateBatch;
		std::vector<TransformPublicationSourceRecord> preparedTransformSources;
		std::uint64_t nextTransformPublicationGroupId = 0U;
		std::uint64_t nextTransformPacketGeneration = 0U;
		std::unordered_map<TerraEngine::UUID, RuntimeBody> runtimeBodies;
		std::unordered_map<TerraEngine::UUID, RuntimeConstraint> runtimeConstraints;
		std::unordered_map<TerraEngine::UUID, RuntimeCharacter> runtimeCharacters;
		// Slot lookup keeps extraction O(n) for large scenes while the full typed
		// handle comparison above still rejects stale slot generations.
		std::unordered_map<PhysicsSlotIndex, TerraEngine::UUID> bodyObjects;
		std::unordered_map<TerraEngine::UUID, std::filesystem::file_time_type> cookedAssetWriteTimes;
		std::unordered_set<TerraEngine::UUID> cookedAssetsNeedingReload;
		std::unordered_map<TerraEngine::UUID, std::filesystem::file_time_type> materialAssetWriteTimes;
		std::unordered_set<TerraEngine::UUID> materialAssetsNeedingReload;
		std::unordered_map<TerraEngine::UUID, PhysicsMaterialRuntime> materialAssets;
		std::vector<PhysicsCommandResult> commandResults;
		std::shared_ptr<const PhysicsBodyStateSnapshot> latestBodies;
		std::shared_ptr<const IncrementalBodyStateSnapshot> latestIncrementalBodies;
		std::shared_ptr<const PhysicsCommandResultSnapshot> latestCommands;
		std::shared_ptr<const PhysicsEventPacket> latestEvents;
		std::shared_ptr<const PhysicsCharacterStatePacket> latestCharacters;
		std::shared_ptr<const PhysicsDiagnosticsSnapshot> latestDiagnostics;
		PhysicsSettings configuredSettings = PhysicsSettings::Defaults();
		std::optional<PhysicsSettings> pendingSettings;
		std::uint64_t settingsGeneration = 1U;
		std::uint64_t diagnosticSnapshotHighWaterBytes = 0U;
		PhysicsDiagnosticsCounters diagnosticCounters;
		PhysicsDiagnosticsTimings diagnosticTimings;
		PhysicsDiagnosticsQueues diagnosticQueues;
		PhysicsPublicationDiagnostics publicationDiagnostics;
		std::uint64_t stateStorePublicationCount = 0U;
		std::uint64_t stateStoreRejectedBatchCount = 0U;
		std::uint64_t lastStatePagesReused = 0U;
		std::uint64_t lastStatePagesCopied = 0U;
		PhysicsTick lastAggregateDiagnosticsTick = 0U;
		std::uint64_t explicitStepPublicationCount = 0U;
		mutable std::mutex mutex;
	};

	PhysicsSystem& PhysicsSystem::Instance() {
		static PhysicsSystem instance;
		return instance;
	}

	PhysicsSystem::PhysicsSystem()
		: impl(new Impl()) {
	}

	PhysicsSystem::~PhysicsSystem() {
		delete impl;
	}

	void PhysicsSystem::Initialize() {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(impl->backendInitialized) return;
		// Export startup can queue mounted Physics Settings before the provider is
		// initialized. Apply that typed request now so the very first world uses the
		// packaged layer/filter and solver contract rather than one frame of defaults.
		if(!impl->ApplyPendingSettingsLocked()) {
			throw std::runtime_error("Initial Physics Settings are invalid.");
		}
		const Backend::PhysicsBackendResult result = impl->backend->Initialize();
		if(!result.Succeeded()) throw std::runtime_error("Physics backend initialization failed: " + result.message);
		impl->backendInitialized = true;
		if(!impl->editorLifecycleControlled && !impl->StartWorldLocked(impl->configuredSettings)) {
			throw std::runtime_error("Physics world initialization failed.");
		}
	}

	void PhysicsSystem::Update(float deltaTime) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->SnapshotLocked();
		impl->PrepareLocked(deltaTime);
		impl->CommitLocked();
		impl->SimulateLocked();
		impl->ExtractLocked();
		impl->PublishLocked();
		impl->PublicationBarrierLocked();
	}

	void PhysicsSystem::Shutdown() {
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->StopWorldLocked();
		if(impl->backendInitialized) impl->backend->Shutdown();
		impl->backendInitialized = false;
	}

	std::string PhysicsSystem::SystemName() {
		return "PhysicsSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration PhysicsSystem::SchedulerDeclaration() {
		TerraEngine::Scheduling::SystemDeclaration declaration;
		declaration.name = "PhysicsSystem";
		declaration.phase = "Physics";
		declaration.after = {"ScriptingSystem"};
		declaration.before = {"UISystem"};
		declaration.reads = {"SceneGraph", "ObjectManager", "PhysicsSettingsUpdateQueue", "PhysicsAuthoringSnapshot", "PhysicsQueryQueue", "PhysicsEventSubscriptions"};
		declaration.writes = {"PhysicsSettings", "PhysicsPreparedWork", "PhysicsWorld", "PhysicsQueryResults", "PhysicsEventPackets", "PhysicsPublishedSnapshots", "PhysicsTransformTransactions", "SceneMutationQueue"};
		declaration.mainThread = true;
		declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Immutable authoring preparation, provider stepping through the shared job adapter, and extraction into typed packets.";
		declaration.protectedCommit = "World generation changes, provider staging commit, command ordering, snapshot swap, and transform transaction publication.";
		declaration.protectedReason = declaration.protectedCommit;
		declaration.profilingLabel = "Physics";
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> PhysicsSystem::DeclareFrameWorkloads(float deltaTime) {
		using namespace TerraEngine::Scheduling;
		static const std::array<FrameWorkloadDeclaration, 7> stableDescriptors = [] {
			std::array<FrameWorkloadDeclaration, 7> descriptors;
			auto configure = [](FrameWorkloadDeclaration& descriptor, std::string name,
				std::vector<std::string> reads, std::vector<std::string> writes,
				bool workerEligible, WorkerSystemPhaseKind kind) {
				descriptor.workloadName = std::move(name);
				descriptor.ownerSystem = "PhysicsSystem";
				descriptor.phase = "Physics";
				descriptor.stableDescriptorId = "PhysicsSystem/Physics/" + descriptor.workloadName;
				descriptor.activationPredicateKey = "physics-frame-work-available";
				descriptor.reads = std::move(reads);
				descriptor.writes = std::move(writes);
				descriptor.workerEligible = workerEligible;
				descriptor.mainThreadRequired = !workerEligible;
				descriptor.mutationPolicy = workerEligible
					? FrameWorkloadMutationPolicy::None
					: FrameWorkloadMutationPolicy::DeferredOnly;
				descriptor.phaseKind = kind;
				descriptor.boundaryClassification = workerEligible
					? HardBoundaryClassification::WorkerSafe
					: HardBoundaryClassification::Protected;
				descriptor.hardBoundary = !workerEligible;
				descriptor.safeSubwork = workerEligible
					? "Consumes immutable preparation values or provider-owned snapshots and emits typed packets without mutating ObjectManager."
					: std::string{};
				descriptor.protectedReason = workerEligible
					? std::string{}
					: "Live-world ownership, provider ordering, and publication commitment remain protected.";
				descriptor.profilingLabel = descriptor.workloadName;
			};

			configure(descriptors[0], "PhysicsSnapshot",
				{"SceneGraph", "ObjectManager", "PhysicsSettingsUpdateQueue"},
				{"PhysicsSettings", "PhysicsWorld", "PhysicsAuthoringSnapshot"}, false, WorkerSystemPhaseKind::Snapshot);
			configure(descriptors[1], "PhysicsPrepare",
				{"PhysicsAuthoringSnapshot", "PhysicsPublishedSnapshots", "PhysicsQueryQueue"},
				{"PhysicsPreparedWork", "PhysicsPreparedQueries"}, true, WorkerSystemPhaseKind::WorkerPrepare);
			configure(descriptors[2], "PhysicsCommandCommit",
				{"PhysicsPreparedWork", "PhysicsWorld"}, {"PhysicsWorld"}, false, WorkerSystemPhaseKind::ProtectedCommit);
			configure(descriptors[3], "PhysicsSimulate",
				{"PhysicsWorld"}, {"PhysicsWorld"}, true, WorkerSystemPhaseKind::WorkerPrepare);
			configure(descriptors[4], "PhysicsExtract",
				{"PhysicsWorld", "PhysicsPreparedQueries"},
				{"PhysicsPublishedSnapshots", "PhysicsBackendQueryResults", "PhysicsRawEvents"}, true, WorkerSystemPhaseKind::WorkerPrepare);
			configure(descriptors[5], "PhysicsPublish",
				{"PhysicsPublishedSnapshots", "PhysicsBackendQueryResults", "PhysicsRawEvents", "PhysicsEventSubscriptions"},
				{"PhysicsQueryResults", "PhysicsEventPackets", "PhysicsTransformTransactions", "SceneMutationQueue"}, false, WorkerSystemPhaseKind::ProtectedCommit);
			configure(descriptors[6], "PhysicsPublicationBarrier",
				{"PhysicsTransformTransactions"}, {"PhysicsPublishedSnapshots"}, false, WorkerSystemPhaseKind::Barrier);
			for (std::size_t index = 1U; index < descriptors.size(); ++index)
			{
				descriptors[index].explicitDependencies = {descriptors[index - 1U].stableDescriptorId};
			}
			return descriptors;
		}();

		std::lock_guard<std::mutex> lock(impl->mutex);
		const Impl::FrameWorkloadActivation activation = impl->InspectFrameWorkloadActivationLocked(deltaTime);
		std::vector<FrameWorkloadDeclaration> workloads(stableDescriptors.begin(), stableDescriptors.end());
		const std::array<bool, 7> active = {
			activation.snapshot,
			activation.prepare,
			activation.commandCommit,
			activation.simulate,
			activation.extract,
			activation.publish,
			activation.publicationBarrier};
		const std::array<std::size_t, 7> estimates = {
			activation.authoringItems,
			activation.authoringItems + activation.queryItems,
			activation.authoringItems + activation.commandItems,
			activation.simulate ? 1U : 0U,
			activation.providerItems + activation.queryItems,
			activation.providerItems + activation.queryItems,
			activation.publicationBarrier ? 1U : 0U};
		for (std::size_t index = 0U; index < workloads.size(); ++index)
		{
			workloads[index].active = active[index];
			workloads[index].ready = impl->world.Handle().IsValid();
			workloads[index].readinessReason = workloads[index].ready ? std::string{} : "physics world is not active";
			workloads[index].estimatedWorkItemCount = estimates[index];
		}
		workloads[1].inlineWorkItemThreshold = impl->configuredSettings.parallelPreparationThreshold;
		workloads[3].inlineWorkItemThreshold = 1U;
		workloads[4].inlineWorkItemThreshold = impl->configuredSettings.parallelExtractionThreshold;
		workloads[6].sameFrameVisibilityRequired = activation.publicationBarrier
			&& activation.providerItems > 0U
			&& TerraEngine::Scheduling::SameFramePhysicsPublicationConsumerActive();
		workloads[0].work = [this](JobGraphContext&) { std::lock_guard<std::mutex> workloadLock(impl->mutex); impl->SnapshotLocked(); };
		workloads[1].work = [this, deltaTime](JobGraphContext&) { std::lock_guard<std::mutex> workloadLock(impl->mutex); impl->PrepareLocked(deltaTime); };
		workloads[2].work = [this](JobGraphContext&) { std::lock_guard<std::mutex> workloadLock(impl->mutex); impl->CommitLocked(); };
		workloads[3].work = [this](JobGraphContext&) { std::lock_guard<std::mutex> workloadLock(impl->mutex); impl->SimulateLocked(); };
		workloads[4].work = [this](JobGraphContext&) { std::lock_guard<std::mutex> workloadLock(impl->mutex); impl->ExtractLocked(); };
		workloads[5].work = [this](JobGraphContext&) { std::lock_guard<std::mutex> workloadLock(impl->mutex); impl->PublishLocked(); };
		workloads[6].work = [this](JobGraphContext&) { std::lock_guard<std::mutex> workloadLock(impl->mutex); impl->PublicationBarrierLocked(); };
		return workloads;
	}

	void PhysicsSystem::SetEditorLifecycleControlled(bool controlled) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->editorLifecycleControlled = controlled;
	}

	bool PhysicsSystem::StartWorld() {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!impl->ApplyPendingSettingsLocked()) return false;
		return impl->StartWorldLocked(impl->configuredSettings);
	}

	bool PhysicsSystem::StartWorld(const PhysicsSettings& settings) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		const PhysicsSettingsValidationResult validation = settings.Validate();
		if(!validation.accepted) return false;
		if(impl->world.Handle().IsValid()) {
			if(impl->pendingSettings) return false;
			impl->pendingSettings = settings;
			return impl->ApplyPendingSettingsLocked();
		}
		impl->configuredSettings = settings;
		++impl->settingsGeneration;
		return impl->StartWorldLocked(impl->configuredSettings);
	}

	bool PhysicsSystem::StopWorld() {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->StopWorldLocked();
	}

	bool PhysicsSystem::QueueSettingsUpdate(PhysicsSettings settings) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!settings.Validate().accepted || impl->pendingSettings.has_value()) return false;
		impl->pendingSettings = std::move(settings);
		return true;
	}

	PhysicsSettings PhysicsSystem::ConfiguredSettings() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->configuredSettings;
	}

	std::uint64_t PhysicsSystem::SettingsGeneration() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->settingsGeneration;
	}

	bool PhysicsSystem::SetPaused(bool paused) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.SetPaused(paused);
	}

	bool PhysicsSystem::RequestExplicitStep() {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.RequestExplicitPausedStep();
	}

	bool PhysicsSystem::IsPaused() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.IsPaused();
	}

	float PhysicsSystem::FixedTimeStep() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.Settings().fixedTimeStep;
	}

	PhysicsWorldHandle PhysicsSystem::WorldHandle() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.Handle();
	}

	PhysicsCommandReceipt PhysicsSystem::SubmitCommand(PhysicsCommand command) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.EnqueueCommand(std::move(command));
	}

	PhysicsQueryReceipt PhysicsSystem::SubmitQuery(PhysicsQueryRequest request) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.SubmitQuery(std::move(request));
	}

	PhysicsQueryBatchSubmission PhysicsSystem::SubmitQueryBatch(
		std::span<const PhysicsQueryRequest> requests) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.SubmitQueryBatch(requests);
	}

	bool PhysicsSystem::CancelQuery(PhysicsQueryHandle handle) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.CancelQuery(handle);
	}

	PhysicsQueryResult PhysicsSystem::ReadQuery(PhysicsQueryHandle handle) const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.ReadQuery(handle);
	}

	PhysicsQueryResult PhysicsSystem::QueryImmediate(PhysicsQueryRequest request) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		const PhysicsWorldHandle activeWorld = impl->world.Handle();
		const PhysicsTick currentTick = impl->world.Timing().tick;
		PhysicsQueryManager immediate(impl->world.Settings());
		const PhysicsQueryReceipt receipt = immediate.Submit(request, activeWorld, currentTick);
		if(!receipt.accepted) {
			PhysicsQueryResult rejected;
			rejected.world = activeWorld;
			rejected.tick = currentTick;
			rejected.status = PhysicsResultStatus::Rejected;
			rejected.rejectionReason = receipt.rejectionReason;
			rejected.diagnostic = receipt.diagnostic;
			rejected.hits = std::make_shared<const std::vector<PhysicsQueryHit>>();
			return rejected;
		}
		const PhysicsTick executionTick = std::max<PhysicsTick>(1U, currentTick);
		PhysicsPreparedQueryBatch prepared = immediate.Prepare(activeWorld, executionTick);
		if(prepared.queries.empty()) return immediate.Read(receipt.handle, activeWorld, currentTick);
		const Backend::PhysicsBackendQueryResult provider = impl->backend->Query(
			impl->MakeBackendQueryRequest(prepared.queries.front().request));
		std::uint64_t stale = 0U;
		std::vector<PhysicsQueryHit> hits = impl->ConvertQueryHits(provider, stale);
		impl->diagnosticCounters.staleCount += stale;
		immediate.Complete(prepared.queries.front(), activeWorld, executionTick,
			provider.result.Succeeded() ? PhysicsResultStatus::Success : PhysicsResultStatus::ProviderFailure,
			hits, provider.truncated, provider.result.message);
		PhysicsQueryResult result = immediate.Read(receipt.handle, activeWorld, executionTick);
		// Immediate results are immutable values, not retained handles. Clearing the
		// temporary manager handle prevents callers from attempting a later read.
		result.handle = {};
		return result;
	}

	PhysicsEventSubscription PhysicsSystem::SubscribeEvents(PhysicsEventSubscriptionPolicy policy) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.SubscribeEvents(std::move(policy));
	}

	bool PhysicsSystem::UnsubscribeEvents(std::uint64_t id, PhysicsGeneration generation) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.UnsubscribeEvents(id, generation);
	}

	std::shared_ptr<const PhysicsBodyStateSnapshot> PhysicsSystem::LatestBodySnapshot() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!impl->latestBodies && impl->latestIncrementalBodies) {
			impl->latestBodies = std::make_shared<const PhysicsBodyStateSnapshot>(PhysicsBodyStateSnapshot{
				impl->latestIncrementalBodies->world,
				impl->latestIncrementalBodies->tick,
				std::make_shared<const std::vector<PhysicsBodyState>>(
					impl->latestIncrementalBodies->Flatten())});
		}
		return impl->latestBodies;
	}

	std::shared_ptr<const IncrementalBodyStateSnapshot> PhysicsSystem::LatestIncrementalBodySnapshot() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->latestIncrementalBodies;
	}

	std::shared_ptr<const PhysicsCommandResultSnapshot> PhysicsSystem::LatestCommandResults() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->latestCommands;
	}

	std::shared_ptr<const PhysicsEventPacket> PhysicsSystem::LatestEventPacket() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->latestEvents;
	}

	std::shared_ptr<const PhysicsEventPacket> PhysicsSystem::ReadRetainedEventPacket(PhysicsTick requestedTick) const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		// PhysicsWorld validates the active generation and retention window while
		// holding its own lock. Returning its shared immutable packet keeps the
		// automation reader outside all mutable provider and collector state.
		return impl->world.AcquireDiagnosticEventPacket(requestedTick, impl->world.Timing().tick);
	}

	std::shared_ptr<const PhysicsCharacterStatePacket> PhysicsSystem::LatestCharacterStatePacket() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->latestCharacters;
	}

	std::shared_ptr<const PhysicsDiagnosticsSnapshot> PhysicsSystem::LatestDiagnostics() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->latestDiagnostics;
	}

	PhysicsSystemJobDrainDiagnostics PhysicsSystem::JobDrainDiagnostics() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		const auto* jolt = dynamic_cast<const Backend::JoltPhysicsBackend*>(impl->backend.get());
		if(jolt == nullptr) return {};
		const Backend::PhysicsBackendJobDrainDiagnostics diagnostics = jolt->JobDiagnostics();
		return {diagnostics.queued, diagnostics.activePumps};
	}

	std::uint64_t PhysicsSystem::ExplicitStepPublicationCount() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->explicitStepPublicationCount;
	}

	void PhysicsSystem::OnAfterPhysicsFlush(
		const std::vector<TerraEngine::Scene::TransactionResult>& results,
		const std::vector<TerraEngine::Scene::TransactionPublicationGroupResult>& publicationGroups) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->CompleteAfterPhysicsFlushLocked(results, publicationGroups);
	}
}
