// PhysicsSystem.cpp - seven declared workloads over the shared Jolt 5.6 world.

#include "PhysicsSystem.h"

#include "Animation/Ragdoll/RagdollActivation.h"
#include "Animation/Runtime/RootMotion.h"

#include "Backend/JoltPhysicsBackend.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/Assets/PhysicsAssetChangeJournal.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Assets/PhysicsMaterial.h"
#include "Physics/PhysicsAuthoringGeneration.h"
#include "Physics/PhysicsPreparationPlan.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
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
#include <map>
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

		std::uint64_t NanosecondsFromMilliseconds(double milliseconds) {
			if(!std::isfinite(milliseconds) || milliseconds <= 0.0) return 0U;
			return static_cast<std::uint64_t>(milliseconds * 1'000'000.0);
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
			std::size_t ragdollItems = 0U;
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
			PhysicsFrameMembership frameMembership;
		};

		struct RuntimeTerrainCollision {
			TerraEngine::UUID terrainAssetId = TerraEngine::EMPTY_UUID;
			std::int64_t areaX = 0;
			std::int64_t areaZ = 0;
			PhysicsShapeHandle shape;
			PhysicsBodyHandle body;
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

		// Ragdoll provider objects deliberately live outside the scene rigid-body
		// maps. Animation owns the visual skeleton and consumes copied poses, so a
		// simulated body must never masquerade as an authored ObjectManager entity.
		struct RuntimeRagdollBody {
			std::uint64_t bodyId = 0U;
			std::uint64_t stableJointId = 0U;
			PhysicsBodyHandle body;
			PhysicsShapeHandle shape;
			Animation::AnimationTransform lastWorldPose{};
			Animation::AnimationFloat3 lastLinearVelocity{};
			Animation::AnimationFloat3 lastAngularVelocity{};
		};

		struct RuntimeRagdollConstraint {
			std::uint64_t constraintId = 0U;
			PhysicsConstraintHandle handle;
			PhysicsConstraintRuntimeRegistration registration;
			// Retaining the provider-neutral create packet lets a later typed motor
			// update replace only drive settings without reconstructing authoring data.
			Backend::PhysicsConstraintCreateRequest request;
		};

		struct RuntimeRagdoll {
			Animation::RagdollGenerationStamp generation{};
			std::shared_ptr<const Animation::RagdollProfileAsset> profile;
			std::vector<RuntimeRagdollBody> bodies;
			std::vector<RuntimeRagdollConstraint> constraints;
			bool active = false;
		};

		struct PendingTransformPublication {
			TransformPublicationPacket packet;
			std::string groupId;
			Backend::PhysicsBackendExtractionToken extractionToken;
			Backend::IPhysicsBackend* extractionBackend = nullptr;
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
			activation.ragdollItems = ragdollPhysicsBridge.PendingCount();
			activation.providerItems = world.RegistryStatistics().bodyCount;
			activation.authoringItems = authoring ? authoring->Data().bodies.size() : activation.providerItems;

			bool authoringChanged = pendingSettings.has_value() || !authoring || !authoringSceneRevisionValid;
			if (worldActive && !authoringChanged)
			{
				const auto packet = ObjectManager::instance().LoadPublishedSceneChangePacketShared();
				authoringChanged = packet == nullptr
					|| !packet->Validate(ObjectManager::instance().GetSceneGeneration()).valid
					|| packet->PublicationRevision() != lastAuthoringScenePublicationRevision + 1U
					|| !PhysicsAssetChangeJournal::Instance().SnapshotSince(observedPhysicsAssetGeneration).Empty();
			}

			const PhysicsWorldTimingState timing = world.Timing();
			const PhysicsSettings settings = worldActive ? world.Settings() : configuredSettings;
			const bool explicitStep = timing.pendingExplicitStepCount > 0U;
			const bool accumulating = worldActive && !world.IsPaused() && deltaTime > 0.0F;
			const bool predictedStep = explicitStep || (accumulating
				&& timing.accumulator + deltaTime >= settings.fixedTimeStep);
			const bool preparationWork = worldActive
				&& (authoringChanged || accumulating || explicitStep || activation.commandItems > 0U
					|| activation.ragdollItems > 0U);

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
			if(!backendInitialized) {
				TERRAENGINE_LOG(LogLevel::Error,
					"Physics world creation failed because the backend is not initialized.");
				return false;
			}
			if(!world.Create(requestedSettings)) {
				TERRAENGINE_LOG(LogLevel::Error,
					"Physics world creation failed because the engine-facing world rejected its settings or lifecycle state.");
				return false;
			}
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
				// Terrain collision is derived from authored height data rather than a
				// scene collider component. Re-stage its latest immutable candidate for
				// this new world generation before the first simulation frame.
				TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().RequestRepublish();
				return true;
			}
			// Preserve the provider's actionable failure at the protected creation
			// boundary. Play Mode reports only a boolean to automation, so without
			// this log a backend allocation or configuration error is indistinguishable
			// from an uninitialized engine system.
			TERRAENGINE_LOG(LogLevel::Error,
				"Physics world creation failed: " << result.message);
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
				if(publication.extractionToken.IsValid()) {
					(void)backend->AbortExtraction(publication.extractionToken);
				}
			}
			world.BeginStopping();
			world.BeginDraining();
			const Backend::PhysicsBackendResult destroyed = backend->DestroyWorld(handle);
			// Provider handles cannot cross a world-generation boundary. The terrain
			// collision system retains the desired recipe and republishes fresh handles
			// after StartWorldLocked creates the replacement world.
			terrainCollisions.clear();
			runtimeBodies.clear();
			bodyObjects.clear();
			runtimeConstraints.clear();
			runtimeCharacters.clear();
			runtimeRagdolls.clear();
			preparedBodies.clear();
			for(auto& [id, frame] : frameBackends) {
				(void)id;
				frame.preparedBodies.clear();
			}
			preparedRemovals.clear();
			preparedConstraints.clear();
			preparedConstraintRemovals.clear();
			preparedCharacters.clear();
			preparedCharacterRemovals.clear();
			plannedSteps = {};
			preparedQueries = {};
			backendQueryRequests.clear();
			executedQueries.clear();
			completedSteps.clear();
			pendingFlushSteps.clear();
			pendingTransformPublications.clear();
			authoring.reset();
			authoringGeneration.reset();
			preparationPlan.reset();
			observedPhysicsAssetGeneration = 0U;
			lastAuthoringSceneGeneration = 0U;
			lastAuthoringScenePublicationRevision = 0U;
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
			// Any retained ragdoll pose belongs to the old world generation. Clear it
			// before the provider teardown makes a replacement world observable.
			ragdollPhysicsBridge.Clear();
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
			const std::shared_ptr<const Scene::SceneChangePacket> scenePacket =
				objects.LoadPublishedSceneChangePacketShared();
			const PhysicsAssetChangeSnapshot assetChanges =
				PhysicsAssetChangeJournal::Instance().SnapshotSince(observedPhysicsAssetGeneration);
			const bool packetValid = scenePacket != nullptr
				&& scenePacket->Validate(objects.GetSceneGeneration()).valid;
			const bool packetContinuous = authoring && packetValid
				&& scenePacket->SceneGeneration() == lastAuthoringSceneGeneration
				&& scenePacket->PublicationRevision() == lastAuthoringScenePublicationRevision + 1U;
			const auto packetAffectsAuthoring = [this, &scenePacket]() {
				if(scenePacket == nullptr || !authoring) return true;
				std::unordered_set<TerraEngine::UUID> changed;
				auto append = [this, &changed](const auto& records, bool transforms) {
					for(const Scene::SceneObjectRevisionSnapshot& record : records) {
						// A successful physics transform receipt already copied this exact
						// revision into RuntimeBody. It is output feedback, not new scene
						// authoring, so recapturing it would create a self-invalidating loop.
						const auto runtime = runtimeBodies.find(record.objectId);
						if(transforms && runtime != runtimeBodies.end()
							&& runtime->second.hasPublishedPose
							&& runtime->second.objectTransformRevision == record.transformRevision) continue;
						changed.insert(record.objectId);
					}
				};
				append(scenePacket->Added(), false); append(scenePacket->Removed(), false);
				append(scenePacket->ComponentChanged(), false); append(scenePacket->TransformChanged(), true);
				append(scenePacket->HierarchyChanged(), false);
				if(changed.empty()) return false;
				for(const PhysicsBodyDescriptor& body : authoring->Data().bodies) {
					if(changed.contains(body.objectId) || (body.hasParent && changed.contains(body.parentObjectId))) return true;
				}
				for(const PhysicsConstraintDescriptor& constraint : authoring->Data().constraints) {
					if(changed.contains(constraint.ownerObjectId) || changed.contains(constraint.firstObjectId)
						|| changed.contains(constraint.secondObjectId)) return true;
				}
				return std::any_of(authoring->Data().characters.begin(), authoring->Data().characters.end(),
					[&changed](const PhysicsCharacterDescriptor& character) { return changed.contains(character.objectId); });
			};
			const bool sceneAffectsPhysics = packetAffectsAuthoring();
			if(authoring && packetContinuous && !sceneAffectsPhysics && assetChanges.Empty()) {
				// An exact packet proves this frame changed no physics owner, ancestor,
				// endpoint, or character. Reuse the immutable generation verbatim: no
				// descriptor capture and no later reconciliation work are required.
				authoringSnapshotReused = true;
				lastAuthoringScenePublicationRevision = scenePacket->PublicationRevision();
				diagnosticTimings.prepareMs = MillisecondsSince(start);
				return;
			}

			const PhysicsAuthoringSnapshot* previous = authoring ? &*authoring : nullptr;
			std::optional<PhysicsAuthoringSnapshot> captured;
			if(authoring && packetContinuous && !sceneAffectsPhysics) {
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
				// Apply only memberships committed at the protected migration boundary.
				// A body never chooses a destination from its float pose; canonical pose
				// is localized against the explicitly stamped frame descriptor instead.
				PhysicsAuthoringData stamped = captured->Data();
				bool membershipsValid = true;
				for(PhysicsBodyDescriptor& body : stamped.bodies) {
					const auto override = frameMembershipOverrides.find(body.objectId);
					if(override == frameMembershipOverrides.end()) continue;
					std::optional<PhysicsFrameDescriptor> frame = frameRegistry.Find(override->second.frame);
					if(!frame && override->second.frame.frameId == kPrimaryPhysicsFrameId
						&& override->second.world == world.Handle()) {
						frame = PhysicsFrameDescriptor{override->second.frame, world.Handle(), {}, 8'192.0};
					}
					if(!frame || frame->world != override->second.world) {
						membershipsValid = false;
						break;
					}
					const PhysicsFramePoseConversion localized =
						LocalizePhysicsPose(body.canonicalWorldPose, *frame);
					if(!localized.Succeeded()) {
						membershipsValid = false;
						break;
					}
					body.frameMembership = override->second;
					body.worldPose = localized.localPose.pose;
				}
				if(membershipsValid) {
					for(const PhysicsConstraintDescriptor& constraint : stamped.constraints) {
						const auto first = std::find_if(stamped.bodies.begin(), stamped.bodies.end(),
							[&constraint](const PhysicsBodyDescriptor& body) { return body.objectId == constraint.firstObjectId; });
						const auto second = std::find_if(stamped.bodies.begin(), stamped.bodies.end(),
							[&constraint](const PhysicsBodyDescriptor& body) { return body.objectId == constraint.secondObjectId; });
						if(first != stamped.bodies.end() && second != stamped.bodies.end()
							&& first->frameMembership.frame != second->frameMembership.frame) {
							membershipsValid = false;
							break;
						}
					}
				}
				if(!membershipsValid) {
					++diagnosticCounters.rejectedCount;
					diagnosticTimings.prepareMs = MillisecondsSince(start);
					return;
				}
				captured = PhysicsAuthoringSnapshot::Create(std::move(stamped), previous);
				if(!captured) {
					++diagnosticCounters.rejectedCount;
					diagnosticTimings.prepareMs = MillisecondsSince(start);
					return;
				}
				const auto builtGeneration = PhysicsAuthoringSnapshot::BuildGeneration(
					previous, *captured, assetChanges);
				if(!builtGeneration) {
					++diagnosticCounters.rejectedCount;
					diagnosticTimings.prepareMs = MillisecondsSince(start);
					return;
				}
				PhysicsAuthoringGenerationData generationData = builtGeneration->Data();
				generationData.sceneGeneration = packetValid ? scenePacket->SceneGeneration() : 0U;
				generationData.scenePublicationRevision = packetValid ? scenePacket->PublicationRevision() : 0U;
				generationData.previousScenePublicationRevision = lastAuthoringScenePublicationRevision;
				if(!authoring) generationData.fallbackReason = PhysicsAuthoringFallbackReason::InitialWorld;
				else if(scenePacket == nullptr) generationData.fallbackReason = PhysicsAuthoringFallbackReason::ScenePacketUnavailable;
				else if(!packetValid) generationData.fallbackReason = PhysicsAuthoringFallbackReason::ScenePacketInvalid;
				else if(scenePacket->SceneGeneration() != lastAuthoringSceneGeneration) {
					// A new scene can reuse a packet revision. Recording this separately
					// makes the fail-closed full capture actionable in diagnostics.
					generationData.fallbackReason = PhysicsAuthoringFallbackReason::SceneGenerationChanged;
				}
				else if(!packetContinuous) generationData.fallbackReason = PhysicsAuthoringFallbackReason::ScenePacketDiscontinuity;
				else if(assetChanges.requiresFullRefresh) generationData.fallbackReason = PhysicsAuthoringFallbackReason::AssetJournalRefresh;
				const auto generation = PhysicsAuthoringGeneration::Create(std::move(generationData));
				if(!generation) {
					++diagnosticCounters.rejectedCount;
					diagnosticTimings.prepareMs = MillisecondsSince(start);
					return;
				}
				authoring = std::move(*captured);
				authoringGeneration = std::move(*generation);
				observedPhysicsAssetGeneration = assetChanges.toGenerationInclusive;
				authoringEquivalentSceneSpatialRevision = objects.GetSpatialRevision();
				lastAuthoringSceneGeneration = packetValid ? scenePacket->SceneGeneration() : 0U;
				lastAuthoringScenePublicationRevision = packetValid ? scenePacket->PublicationRevision() : 0U;
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
			for(auto& [id, frame] : frameBackends) {
				(void)id;
				frame.preparedBodies.clear();
			}
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
			std::vector<TerraEngine::UUID> bodyPreparationOrder;
			std::unordered_map<TerraEngine::UUID, PhysicsAuthoringChangeReason> bodyPreparationReasons;
			const bool isNewAuthoringGeneration = authoringGeneration
				&& lastPreparedAuthoringSnapshotGeneration != authoringGeneration->Data().snapshotGeneration;
			if(isNewAuthoringGeneration) {
				PhysicsPreparationPlanOptions options;
				options.parallelThreshold = configuredSettings.parallelPreparationThreshold;
				options.rangeSize = configuredSettings.providerExtractionBatchSize;
				options.availableWorkerCount = std::max<std::size_t>(1U, ThreadPool::instance().getThreadCount());
				// The immutable generation already expanded body/endpoint ownership into
				// typed closure IDs. Convert only those records into worker-plan inputs;
				// unrelated constraints and characters remain absent from this tick.
				for(const TerraEngine::UUID& recordId : authoringGeneration->Data().constraintRecordIds) {
					const PhysicsConstraintDescriptor* constraint = authoring->FindConstraintDescriptor(recordId);
					if(constraint == nullptr) continue; // Removed records remain removal-only work.
					options.constraintOperations.push_back({constraint->ownerObjectId,
						constraint->ownerObjectGeneration, kInvalidPhysicsDescriptorIndex,
						PhysicsAuthoringChangeReason::HierarchyDependency});
				}
				for(const TerraEngine::UUID& objectId : authoringGeneration->Data().characterObjectIds) {
					const PhysicsCharacterDescriptor* character = authoring->FindCharacterDescriptor(objectId);
					if(character == nullptr) continue; // Removal work does not need a live descriptor.
					options.characterOperations.push_back({character->objectId, character->objectGeneration,
						kInvalidPhysicsDescriptorIndex, PhysicsAuthoringChangeReason::HierarchyDependency});
				}
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
							|| item.kind == PhysicsPreparationWorkKind::Constraint
							|| item.kind == PhysicsPreparationWorkKind::Character
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
				bodyPreparationOrder.reserve(plan.creates.size() + plan.updates.size()
					+ plan.dependencyRebuilds.size());
				const auto appendObjects = [&bodiesToPrepare, &bodyPreparationOrder, &bodyPreparationReasons](
					const std::vector<PhysicsPreparationOperation>& operations) {
					for(const PhysicsPreparationOperation& operation : operations) {
						if(bodiesToPrepare.insert(operation.objectId).second) {
							// Preserve the deterministic plan order while using the set only
							// for membership checks in the dependent reconciliation below.
							bodyPreparationOrder.push_back(operation.objectId);
						}
						bodyPreparationReasons[operation.objectId] = operation.reasons;
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
			if(!isNewAuthoringGeneration) {
				// The fixed step, commands, queries, and extraction still proceed, but
				// an unchanged immutable authoring generation owns no body/collider/
				// constraint/character reconciliation work. Returning here is what
				// keeps sleeping large worlds proportional to real changes.
				diagnosticTimings.prepareMs += MillisecondsSince(start);
				return;
			}
			for(const TerraEngine::UUID& objectId : bodyPreparationOrder) {
				const PhysicsBodyDescriptorIndexRecord* descriptor = authoring->FindBodyDescriptorIndex(objectId);
				if(descriptor == nullptr) continue; // A removal has no current body descriptor.
				const PhysicsBodyDescriptor& body = authoring->Data().bodies[descriptor->bodyVectorIndex];
				if(body.frameMembership.world != world.Handle()) {
					const auto frame = frameBackends.find(body.frameMembership.frame.frameId.value);
					if(frame == frameBackends.end() || frame->second.frame != body.frameMembership.frame
						|| descriptor->colliderCount == 0U) {
						++diagnosticCounters.rejectedCount;
						continue;
					}
					PreparedBody prepared;
					prepared.body = body;
					prepared.colliders.reserve(descriptor->colliderCount);
					for(std::uint32_t index = descriptor->colliderBegin;
						index < descriptor->colliderBegin + descriptor->colliderCount; ++index) {
						const PhysicsColliderDescriptor& collider = authoring->Data().colliders[index];
						if(collider.enabled) prepared.colliders.push_back(collider);
					}
					if(prepared.colliders.empty()) continue;
					prepared.shapeRecipe = MakeShapeRecipeLocked(prepared.colliders, prepared.diagnostic);
					prepared.material = ResolveBodyMaterialLocked(prepared.colliders, prepared.diagnostic);
					frame->second.preparedBodies.push_back(std::move(prepared));
					continue;
				}
				const auto reasons = bodyPreparationReasons.find(body.objectId);
				const auto runtime = runtimeBodies.find(body.objectId);
				if(reasons != bodyPreparationReasons.end()
					&& reasons->second == PhysicsAuthoringChangeReason::Transform
					&& runtime != runtimeBodies.end()
					&& runtime->second.objectGeneration == body.objectGeneration
					&& runtime->second.componentGeneration == body.componentGeneration
					&& runtime->second.hasPublishedPose
					&& SamePose(runtime->second.lastPublishedPose, body.worldPose)) {
					// A global spatial revision can force a fresh authoring capture even
					// when this body's only changed Transform came from physics itself.
					// Recreating the provider body here would restore authored initial
					// velocity every tick, turning gravity into a slow constant drift.
					// Exact pose and generation agreement proves there is no editor or
					// script-authored change to send back to the provider.
					continue;
				}
				if(descriptor->colliderCount > 0U) {
					PreparedBody prepared;
					prepared.body = body;
					prepared.colliders.reserve(descriptor->colliderCount);
					for(std::uint32_t index = descriptor->colliderBegin;
						index < descriptor->colliderBegin + descriptor->colliderCount; ++index) {
						const PhysicsColliderDescriptor& collider = authoring->Data().colliders[index];
						if(collider.enabled) prepared.colliders.push_back(collider);
					}
					if(prepared.colliders.empty()) continue;
					prepared.shapeRecipe = MakeShapeRecipeLocked(prepared.colliders, prepared.diagnostic);
					prepared.material = ResolveBodyMaterialLocked(prepared.colliders, prepared.diagnostic);
					preparedBodies.push_back(std::move(prepared));
				}
			}
			std::unordered_set<TerraEngine::UUID> rebuildingBodies;
			for(const PreparedBody& prepared : preparedBodies) rebuildingBodies.insert(prepared.body.objectId);
			for(const TerraEngine::UUID& objectId : preparedRemovals) rebuildingBodies.insert(objectId);

			std::unordered_set<TerraEngine::UUID> rebuildingConstraints;
			for(const TerraEngine::UUID& recordId : authoringGeneration->Data().constraintRecordIds) {
				const PhysicsConstraintDescriptor* constraint = authoring->FindConstraintDescriptor(recordId);
				if(constraint == nullptr) {
					if(runtimeConstraints.contains(recordId)) preparedConstraintRemovals.push_back(recordId);
					continue;
				}
				const auto live = runtimeConstraints.find(constraint->recordId);
				const bool endpointRebuilt = rebuildingBodies.contains(constraint->firstObjectId)
					|| rebuildingBodies.contains(constraint->secondObjectId);
				if(live == runtimeConstraints.end()
					|| live->second.ownerObjectGeneration != constraint->ownerObjectGeneration
					|| live->second.componentGeneration != constraint->componentGeneration
					|| live->second.recordGeneration != constraint->recordGeneration
					|| live->second.resetGeneration != constraint->resetGeneration
					|| endpointRebuilt) {
					preparedConstraints.push_back(*constraint);
					rebuildingConstraints.insert(constraint->recordId);
				}
			}
			// The immutable closure already includes transitive gear dependents, so no
			// all-constraint scan is necessary after endpoint work is selected.
			// Gear records depend on their authored hinge records, so commit all other
			// kinds first while preserving authored order inside each group.
			std::stable_sort(preparedConstraints.begin(), preparedConstraints.end(),
				[](const PhysicsConstraintDescriptor& left, const PhysicsConstraintDescriptor& right) {
					return left.kind != PhysicsConstraintKind::Gear && right.kind == PhysicsConstraintKind::Gear;
				});

			for(const TerraEngine::UUID& objectId : authoringGeneration->Data().characterObjectIds) {
				const PhysicsCharacterDescriptor* character = authoring->FindCharacterDescriptor(objectId);
				if(character == nullptr) {
					if(runtimeCharacters.contains(objectId)) preparedCharacterRemovals.push_back(objectId);
					continue;
				}
				const auto live = runtimeCharacters.find(character->objectId);
				if(live == runtimeCharacters.end()
					|| live->second.objectGeneration != character->objectGeneration
					|| live->second.componentGeneration != character->componentGeneration) {
					preparedCharacters.push_back(*character);
				}
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

		static TerraEngine::UUID MakeRagdollRuntimeUuid(
			std::uint64_t ragdollId,
			std::uint64_t recordId,
			std::uint64_t family) noexcept {
			// Provider records require UUID ownership metadata even though ragdoll
			// bodies are not scene objects. A deterministic namespace keeps rebuilds
			// stable and lets extraction verify exact ownership without random state.
			TerraEngine::UUID value{};
			std::uint64_t high = ragdollId ^ (family * 0x9e3779b97f4a7c15ULL);
			std::uint64_t low = recordId ^ (ragdollId + 0xd6e8feb86659fd93ULL);
			if(high == 0U && low == 0U) low = 1U;
			std::memcpy(value.data(), &high, sizeof(high));
			std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
			return value;
		}

		static PhysicsVector3 ToPhysicsVector(const Animation::AnimationFloat3& value) noexcept {
			return {value.x, value.y, value.z};
		}

		static PhysicsQuaternion ToPhysicsRotation(const Animation::AnimationQuaternion& value) noexcept {
			const float lengthSquared = value.x * value.x + value.y * value.y
				+ value.z * value.z + value.w * value.w;
			if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8F) return {};
			const float inverseLength = 1.0F / std::sqrt(lengthSquared);
			return {value.x * inverseLength, value.y * inverseLength,
				value.z * inverseLength, value.w * inverseLength};
		}

		static PhysicsPose ToPhysicsPose(const Animation::AnimationTransform& value) noexcept {
			return {ToPhysicsVector(value.translation), ToPhysicsRotation(value.rotation)};
		}

		static Animation::AnimationTransform ToAnimationTransform(const PhysicsPose& value) noexcept {
			return {{value.position.x, value.position.y, value.position.z},
				{value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w},
				{1.0F, 1.0F, 1.0F}};
		}

		static std::uint64_t MixRagdollRecipeHash(
			std::uint64_t hash,
			std::uint64_t value) noexcept {
			hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
			return hash == 0U ? 1U : hash;
		}

		std::optional<Backend::PhysicsShapeRecipe> MakeRagdollShapeRecipeLocked(
			const Animation::RagdollBodyRecord& body,
			std::string& diagnostic) {
			if(body.colliders.empty()) {
				diagnostic = "A ragdoll body has no collider recipe.";
				return std::nullopt;
			}

			Backend::PhysicsShapeRecipe compound;
			compound.kind = PhysicsShapeKind::Compound;
			compound.dimensions = {1.0F, 1.0F, 1.0F};
			compound.stableRecipeHash = MixRagdollRecipeHash(14695981039346656037ULL, body.bodyId);
			compound.children.reserve(body.colliders.size());
			for(std::size_t index = 0U; index < body.colliders.size(); ++index) {
				const Animation::RagdollColliderRecord& collider = body.colliders[index];
				Backend::PhysicsShapeRecipe child;
				child.localCenter = ToPhysicsVector(collider.localCenter);
				child.localRotation = ToPhysicsRotation(collider.localOrientation);
				child.subShapeKey = static_cast<std::uint32_t>(index + 1U);
				child.sensor = collider.isSensor;
				child.stableRecipeHash = MixRagdollRecipeHash(body.bodyId, collider.colliderId);
				switch(collider.shape) {
				case Animation::RagdollColliderShape::Sphere:
					child.kind = PhysicsShapeKind::Sphere;
					child.dimensions = {collider.dimensions.x, collider.dimensions.x, collider.dimensions.x};
					break;
				case Animation::RagdollColliderShape::Capsule:
					child.kind = PhysicsShapeKind::Capsule;
					child.dimensions = {collider.dimensions.x,
						2.0F * (collider.dimensions.y + collider.dimensions.x), collider.dimensions.x};
					break;
				case Animation::RagdollColliderShape::Box:
					child.kind = PhysicsShapeKind::Box;
					child.dimensions = {2.0F * collider.dimensions.x,
						2.0F * collider.dimensions.y, 2.0F * collider.dimensions.z};
					break;
				case Animation::RagdollColliderShape::ConvexHull:
					child.kind = PhysicsShapeKind::ConvexHull;
					child.dimensions = {1.0F, 1.0F, 1.0F};
					if(collider.convexHullSource == Animation::RagdollConvexHullSource::InlineVertices) {
						child.points.reserve(collider.convexHullVertices.size());
						for(const Animation::AnimationFloat3& point : collider.convexHullVertices) {
							child.points.push_back(ToPhysicsVector(point));
						}
					} else if(collider.convexHullSource == Animation::RagdollConvexHullSource::ImmutableCookedAsset) {
						const TerraEngine::UUID assetId = collider.immutableShape.assetId.value;
						const auto state = ResolveCookedAssetLocked(assetId, diagnostic);
						if(!state || !state->data
							|| state->publicationGeneration != collider.immutableShape.expectedGeneration) {
							diagnostic = diagnostic.empty()
								? "A ragdoll collider references an absent or stale immutable cooked shape."
								: diagnostic;
							return std::nullopt;
						}
						child.sourceAssetId = assetId;
						child.cookedFormatFamily = state->data->formatFamily;
						child.cookedProvider = state->data->provider;
						child.cookedProviderVersion = state->data->providerVersion;
						child.cookedProviderRevision = state->data->providerRevision;
						child.cookedFormatVersion = state->data->formatVersion;
						child.cookedPayloadChecksum = state->data->payloadChecksum;
						child.cookedPayload = std::shared_ptr<const std::vector<std::byte>>(
							state->data, &state->data->payload);
						child.stableRecipeHash = MixRagdollRecipeHash(
							child.stableRecipeHash, state->data->recipeHash);
					} else {
						diagnostic = "A convex ragdoll collider has no immutable point or cooked-shape source.";
						return std::nullopt;
					}
					break;
				}
				compound.stableRecipeHash = MixRagdollRecipeHash(
					compound.stableRecipeHash, child.stableRecipeHash);
				compound.children.push_back(std::move(child));
			}
			if(compound.children.size() == 1U) return std::move(compound.children.front());
			return compound;
		}

		static PhysicsConstraintKind ConvertRagdollConstraintKind(
			Animation::RagdollConstraintKind kind) noexcept {
			switch(kind) {
			case Animation::RagdollConstraintKind::Fixed: return PhysicsConstraintKind::Fixed;
			case Animation::RagdollConstraintKind::Hinge: return PhysicsConstraintKind::Hinge;
			case Animation::RagdollConstraintKind::Cone: return PhysicsConstraintKind::Cone;
			case Animation::RagdollConstraintKind::SwingTwist: return PhysicsConstraintKind::SwingTwist;
			case Animation::RagdollConstraintKind::SixDof: return PhysicsConstraintKind::SixDegreesOfFreedom;
			}
			return PhysicsConstraintKind::Fixed;
		}

		static Backend::PhysicsConstraintAxisSettings ConvertRagdollAxis(
			const Animation::RagdollAxisLimit& limit,
			const Animation::RagdollAxisMotor& motor) noexcept {
			Backend::PhysicsConstraintAxisSettings converted;
			converted.limitEnabled = limit.mode != Animation::RagdollLimitMode::Free;
			converted.minimum = limit.mode == Animation::RagdollLimitMode::Locked ? 0.0F : limit.minimum;
			converted.maximum = limit.mode == Animation::RagdollLimitMode::Locked ? 0.0F : limit.maximum;
			converted.springEnabled = limit.springFrequencyHertz > 0.0F;
			converted.springUsesFrequency = true;
			converted.springFrequency = limit.springFrequencyHertz;
			converted.springDamping = limit.springDampingRatio;
			switch(motor.mode) {
			case Animation::RagdollMotorMode::Disabled:
				converted.motorMode = Backend::PhysicsBackendMotorMode::Off;
				break;
			case Animation::RagdollMotorMode::Position:
				converted.motorMode = Backend::PhysicsBackendMotorMode::Position;
				break;
			case Animation::RagdollMotorMode::Velocity:
				converted.motorMode = Backend::PhysicsBackendMotorMode::Velocity;
				break;
			}
			converted.motorTargetPosition = motor.targetPosition;
			converted.motorTargetVelocity = motor.targetVelocity;
			converted.motorMaximumForce = motor.maximumForce;
			return converted;
		}

		void ReleaseRagdollHandlesLocked(RuntimeRagdoll& ragdoll) noexcept {
			for(auto constraint = ragdoll.constraints.rbegin(); constraint != ragdoll.constraints.rend(); ++constraint) {
				world.RemoveConstraint(constraint->handle);
				world.Release(constraint->handle);
			}
			for(auto body = ragdoll.bodies.rbegin(); body != ragdoll.bodies.rend(); ++body) {
				world.Release(body->body);
				world.Release(body->shape);
			}
		}

		void DestroyCommittedRagdollLocked(RuntimeRagdoll& ragdoll) noexcept {
			// Constraints always retire before endpoint bodies; provider deletion then
			// cannot reject because of a surviving dependency. Handles are released only
			// after the provider no longer references their slots.
			for(auto constraint = ragdoll.constraints.rbegin(); constraint != ragdoll.constraints.rend(); ++constraint) {
				world.RemoveConstraint(constraint->handle);
				const Backend::PhysicsBackendResult result = backend->DestroyConstraint(constraint->handle);
				if(!result.Succeeded()) ++diagnosticCounters.rejectedCount;
				world.Release(constraint->handle);
			}
			for(auto body = ragdoll.bodies.rbegin(); body != ragdoll.bodies.rend(); ++body) {
				const Backend::PhysicsBackendResult bodyResult = backend->DestroyBody(body->body);
				const Backend::PhysicsBackendResult shapeResult = backend->DestroyShape(body->shape);
				if(!bodyResult.Succeeded() || !shapeResult.Succeeded()) ++diagnosticCounters.rejectedCount;
				world.Release(body->body);
				world.Release(body->shape);
			}
		}

		bool StageRagdollLocked(
			std::uint64_t ragdollId,
			const Animation::RagdollGenerationStamp& generation,
			std::shared_ptr<const Animation::RagdollProfileAsset> profile,
			const Animation::RagdollActivationPlan& activation,
			bool active,
			Backend::PhysicsBackendStagingToken& staging,
			RuntimeRagdoll& output,
			std::string& diagnostic) {
			if(!profile || profile->runtimeGeneration != generation.profileGeneration
				|| generation.worldGeneration != world.Handle().worldGeneration
				|| activation.generation != generation
				|| Animation::ValidateRagdollProfileAssetStructure(*profile) != Animation::AnimationStatus::Ok
				|| profile->bodies.size() != activation.bodies.size()
				|| profile->bodies.size() > Animation::kRagdollSnapshotMaximumBodies) {
				diagnostic = "The ragdoll profile, activation pose, or active physics-world generation is incompatible.";
				return false;
			}

			RuntimeRagdoll staged;
			staged.generation = generation;
			staged.profile = std::move(profile);
			staged.active = active;
			staged.bodies.reserve(staged.profile->bodies.size());
			std::unordered_map<std::uint64_t, PhysicsBodyHandle> bodyHandles;
			bodyHandles.reserve(staged.profile->bodies.size());
			for(std::size_t index = 0U; index < staged.profile->bodies.size(); ++index) {
				const Animation::RagdollBodyRecord& profileBody = staged.profile->bodies[index];
				const Animation::RagdollActivationBody& activationBody = activation.bodies[index];
				if(profileBody.bodyId != activationBody.bodyId
					|| profileBody.stableJointId != activationBody.stableJointId
					|| profileBody.collisionLayer >= configuredSettings.layers.size()
					|| !configuredSettings.layers[profileBody.collisionLayer].enabled) {
					diagnostic = "A ragdoll body mapping or collision layer is invalid for the active physics settings.";
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}

				auto shapeRecipe = MakeRagdollShapeRecipeLocked(profileBody, diagnostic);
				if(!shapeRecipe) {
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}
				const PhysicsShapeHandle shape = world.AllocateShape();
				const PhysicsBodyHandle body = world.AllocateBody();
				if(!shape.IsValid() || !body.IsValid()) {
					if(shape.IsValid()) world.Release(shape);
					if(body.IsValid()) world.Release(body);
					diagnostic = "Physics handle capacity was exhausted while staging a ragdoll body.";
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}
				staged.bodies.push_back({profileBody.bodyId, profileBody.stableJointId,
					body, shape, activationBody.worldPose,
					activationBody.linearVelocity, activationBody.angularVelocity});

				Backend::PhysicsBackendResult result = backend->CreateShape(
					world.Handle(), shape, *shapeRecipe, staging);
				if(!result.Succeeded()) {
					diagnostic = result.message;
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}
				Backend::PhysicsBodyCreateRequest request;
				request.handle = body;
				request.objectId = MakeRagdollRuntimeUuid(ragdollId, profileBody.bodyId, 1U);
				request.objectGeneration = static_cast<PhysicsGeneration>(generation.publicationGeneration);
				request.componentGeneration = static_cast<PhysicsGeneration>(generation.profileGeneration);
				request.shape = shape;
				request.motionType = profileBody.motionType == Animation::RagdollMotionType::Dynamic
					? PhysicsMotionType::Dynamic : PhysicsMotionType::Kinematic;
				request.motionQuality = PhysicsMotionQuality::LinearCast;
				request.pose = ToPhysicsPose(activationBody.worldPose);
				request.linearVelocity = ToPhysicsVector(activationBody.linearVelocity);
				request.angularVelocity = ToPhysicsVector(activationBody.angularVelocity);
				request.mass = profileBody.massKilograms;
				request.friction = profileBody.colliders.front().friction;
				request.restitution = profileBody.colliders.front().restitution;
				request.linearDamping = profileBody.linearDamping;
				request.angularDamping = profileBody.angularDamping;
				request.maximumLinearVelocity = profileBody.maximumLinearVelocity;
				request.maximumAngularVelocity = profileBody.maximumAngularVelocity;
				request.gravityFactor = profileBody.gravityFactor;
				request.collisionLayer = static_cast<std::uint8_t>(profileBody.collisionLayer);
				request.collisionMask = profileBody.collisionMask;
				request.useGravity = profileBody.gravityFactor > 0.0F;
				request.sensor = std::all_of(profileBody.colliders.begin(), profileBody.colliders.end(),
					[](const Animation::RagdollColliderRecord& collider) { return collider.isSensor; });
				request.allowSleeping = profileBody.allowSleep;
				request.startAwake = active && profileBody.startAwake;
				request.enabled = active;
				result = backend->CreateBody(request, staging);
				if(!result.Succeeded()) {
					diagnostic = result.message;
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}
				bodyHandles.emplace(profileBody.bodyId, body);
			}

			staged.constraints.reserve(staged.profile->constraints.size());
			for(const Animation::RagdollConstraintRecord& profileConstraint : staged.profile->constraints) {
				const auto parent = bodyHandles.find(profileConstraint.parentBodyId);
				const auto child = bodyHandles.find(profileConstraint.childBodyId);
				if(parent == bodyHandles.end() || child == bodyHandles.end()) {
					diagnostic = "A ragdoll constraint references an absent staged endpoint body.";
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}
				const PhysicsConstraintHandle handle = world.AllocateConstraint();
				if(!handle.IsValid()) {
					diagnostic = "Physics constraint handle capacity was exhausted while staging a ragdoll.";
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}
				Backend::PhysicsConstraintCreateRequest request;
				request.handle = handle;
				request.ownerObjectId = MakeRagdollRuntimeUuid(ragdollId, profileConstraint.constraintId, 2U);
				request.ownerObjectGeneration = static_cast<PhysicsGeneration>(generation.publicationGeneration);
				request.componentGeneration = static_cast<PhysicsGeneration>(generation.profileGeneration);
				request.recordId = MakeRagdollRuntimeUuid(ragdollId, profileConstraint.constraintId, 3U);
				request.recordGeneration = static_cast<PhysicsGeneration>(generation.profileGeneration);
				request.kind = ConvertRagdollConstraintKind(profileConstraint.kind);
				request.bodyA = parent->second;
				request.bodyB = child->second;
				request.localFrameA = {ToPhysicsVector(profileConstraint.parentFrame.localPosition),
					ToPhysicsRotation(profileConstraint.parentFrame.localOrientation)};
				request.localFrameB = {ToPhysicsVector(profileConstraint.childFrame.localPosition),
					ToPhysicsRotation(profileConstraint.childFrame.localOrientation)};
				request.axisA = NormalizeOr(ToPhysicsVector(profileConstraint.parentFrame.primaryAxis), {1.0F, 0.0F, 0.0F});
				request.axisB = NormalizeOr(ToPhysicsVector(profileConstraint.childFrame.primaryAxis), {1.0F, 0.0F, 0.0F});
				request.normalAxisA = NormalizeOr(ToPhysicsVector(profileConstraint.parentFrame.secondaryAxis), {0.0F, 1.0F, 0.0F});
				request.normalAxisB = NormalizeOr(ToPhysicsVector(profileConstraint.childFrame.secondaryAxis), {0.0F, 1.0F, 0.0F});
				for(std::size_t axis = 0U; axis < request.axes.size(); ++axis) {
					request.axes[axis] = ConvertRagdollAxis(
						profileConstraint.limits[axis], profileConstraint.motors[axis]);
				}
				request.breakForce = profileConstraint.breakForce > 0.0F
					? profileConstraint.breakForce : std::numeric_limits<float>::infinity();
				request.breakTorque = profileConstraint.breakTorque > 0.0F
					? profileConstraint.breakTorque : std::numeric_limits<float>::infinity();
				request.collideConnected = !profileConstraint.disableCollisionBetweenBodies;
				request.enabled = active;
				request.resetGeneration = static_cast<PhysicsGeneration>(generation.publicationGeneration);
				const Backend::PhysicsBackendResult result = backend->CreateConstraint(request, staging);
				if(!result.Succeeded()) {
					world.Release(handle);
					diagnostic = result.message;
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}
				PhysicsConstraintRuntimeRegistration registration{handle, request.ownerObjectId,
					request.ownerObjectGeneration, request.componentGeneration, request.recordId,
					request.recordGeneration, request.breakForce, request.breakTorque,
					request.resetGeneration};
				staged.constraints.push_back({profileConstraint.constraintId, handle, registration, request});
				if(!world.RegisterConstraint(registration)) {
					diagnostic = "The physics world rejected a staged ragdoll constraint registration.";
					ReleaseRagdollHandlesLocked(staged);
					return false;
				}
			}
			output = std::move(staged);
			return true;
		}

		void CommitRagdollRequestsLocked() {
			std::vector<Animation::RagdollPhysicsRequest> requests;
			if(ragdollPhysicsBridge.Drain(requests) != Animation::AnimationStatus::Ok) {
				++diagnosticCounters.rejectedCount;
				return;
			}
			if(requests.empty()) return;

			const PhysicsTick physicsTick = !plannedSteps.requests.empty()
				? plannedSteps.requests.back().tick : world.Timing().tick;
			const Animation::AnimationFixedTick committedStep =
				static_cast<Animation::AnimationFixedTick>(std::max<PhysicsTick>(1U, physicsTick));
			Backend::PhysicsBackendStagingToken staging;
			std::unordered_map<std::uint64_t, RuntimeRagdoll> candidate = runtimeRagdolls;
			std::vector<RuntimeRagdoll> stagedAllocations;
			std::vector<RuntimeRagdoll> retireAfterCommit;
			std::vector<std::uint64_t> touchedPoseIds;
			struct RagdollMotorUpdate {
				Backend::PhysicsConstraintCreateRequest previous;
				Backend::PhysicsConstraintCreateRequest next;
			};
			std::vector<RagdollMotorUpdate> motorUpdates;
			std::size_t appliedMotorUpdateCount = 0U;
			std::string diagnostic;

			const auto rollbackStaging = [&]() {
				if(staging.IsValid()) {
					const Backend::PhysicsBackendResult rolledBack = backend->RollbackStaging(staging);
					if(!rolledBack.Succeeded()) {
						// A failed provider commit may have consumed the token. Explicitly
						// retire each staged dependency in reverse order before releasing slots.
						for(auto ragdoll = stagedAllocations.rbegin(); ragdoll != stagedAllocations.rend(); ++ragdoll) {
							for(auto constraint = ragdoll->constraints.rbegin(); constraint != ragdoll->constraints.rend(); ++constraint) {
								backend->DestroyConstraint(constraint->handle);
							}
							for(auto body = ragdoll->bodies.rbegin(); body != ragdoll->bodies.rend(); ++body) {
								backend->DestroyBody(body->body);
								backend->DestroyShape(body->shape);
							}
						}
					}
				}
				for(RuntimeRagdoll& ragdoll : stagedAllocations) ReleaseRagdollHandlesLocked(ragdoll);
			};

			const auto rollbackMotorUpdates = [&]() {
				while(appliedMotorUpdateCount > 0U) {
					--appliedMotorUpdateCount;
					const Backend::PhysicsBackendResult restored = backend->UpdateConstraint(
						motorUpdates[appliedMotorUpdateCount].previous);
					if(!restored.Succeeded()) ++diagnosticCounters.rejectedCount;
				}
			};

			const auto rejectBatch = [&](std::string reason) {
				rollbackMotorUpdates();
				rollbackStaging();
				const Animation::AnimationStatus rejected = ragdollPhysicsBridge.RejectBatchAtProtectedBoundary(
					requests, committedStep, std::move(reason));
				if(rejected != Animation::AnimationStatus::Rejected) {
					ragdollPhysicsBridge.RestoreUndeliveredDrain(std::move(requests));
				}
				diagnosticCounters.rejectedCount += requests.size();
			};

			for(const Animation::RagdollPhysicsRequest& request : requests) {
				if(request.generation.worldGeneration != world.Handle().worldGeneration) {
					diagnostic = "A ragdoll request targets a stale physics-world generation.";
					break;
				}
				auto current = candidate.find(request.ragdollId);
				if(request.kind == Animation::RagdollPhysicsRequestKind::Create) {
					if(current != candidate.end() || !request.profile) {
						diagnostic = "Ragdoll creation requires an absent identity and a resolved immutable profile.";
						break;
					}
				} else {
					if(current == candidate.end()) {
						diagnostic = "A ragdoll lifecycle request targets an absent runtime identity.";
						break;
					}
					if(request.expectedCurrentGeneration.IsValid()
						&& current->second.generation != request.expectedCurrentGeneration) {
						diagnostic = "A ragdoll lifecycle request targets a stale current generation.";
						break;
					}
				}

				if(request.kind == Animation::RagdollPhysicsRequestKind::UpdateMotors) {
					if(!current->second.active || !current->second.profile
						|| request.motorDrives.empty()) {
						diagnostic = "A ragdoll motor update requires one active compatible runtime.";
						break;
					}
					for(const Animation::RagdollMotorDrive& drive : request.motorDrives) {
						auto runtimeConstraint = std::find_if(
							current->second.constraints.begin(), current->second.constraints.end(),
							[&drive](const RuntimeRagdollConstraint& value) {
								return value.constraintId == drive.constraintId;
							});
						const auto authoredConstraint = std::find_if(
							current->second.profile->constraints.begin(),
							current->second.profile->constraints.end(),
							[&drive](const Animation::RagdollConstraintRecord& value) {
								return value.constraintId == drive.constraintId;
							});
						if(runtimeConstraint == current->second.constraints.end()
							|| authoredConstraint == current->second.profile->constraints.end()) {
							diagnostic = "A ragdoll motor update references an absent authored constraint.";
							break;
						}
						Backend::PhysicsConstraintCreateRequest next = runtimeConstraint->request;
						for(std::size_t axis = 0U; axis < next.axes.size(); ++axis) {
							next.axes[axis] = ConvertRagdollAxis(
								authoredConstraint->limits[axis], authoredConstraint->motors[axis]);
							next.axes[axis].motorMaximumForce *= drive.strengthScale;
						}
						motorUpdates.push_back({runtimeConstraint->request, next});
						runtimeConstraint->request = std::move(next);
					}
					if(!diagnostic.empty()) break;
					continue;
				}

				touchedPoseIds.push_back(request.ragdollId);
				if(request.kind == Animation::RagdollPhysicsRequestKind::Teardown) {
					retireAfterCommit.push_back(current->second);
					candidate.erase(current);
					continue;
				}

				const std::shared_ptr<const Animation::RagdollProfileAsset> profile = request.profile
					? request.profile : current->second.profile;
				Animation::RagdollActivationPlan activation;
				if(request.kind == Animation::RagdollPhysicsRequestKind::Deactivate) {
					activation.generation = request.generation;
					activation.bodies.reserve(current->second.bodies.size());
					for(const RuntimeRagdollBody& body : current->second.bodies) {
						activation.bodies.push_back({body.bodyId, body.stableJointId, body.lastWorldPose,
							body.lastLinearVelocity, body.lastAngularVelocity});
					}
				} else {
					const Animation::AnimationStatus prepared = Animation::PrepareRagdollActivation(
						request.activationPose, *profile, request.generation, activation);
					if(prepared != Animation::AnimationStatus::Ok) {
						diagnostic = "The complete animation pose could not be mapped to the requested ragdoll generation.";
						break;
					}
				}

				const bool targetActive = request.kind == Animation::RagdollPhysicsRequestKind::Activate
					|| (request.kind == Animation::RagdollPhysicsRequestKind::Rebuild
						&& current != candidate.end() && current->second.active);
				RuntimeRagdoll staged;
				if(!StageRagdollLocked(request.ragdollId, request.generation, profile,
					activation, targetActive, staging, staged, diagnostic)) {
					break;
				}
				if(current != candidate.end()) retireAfterCommit.push_back(current->second);
				candidate.insert_or_assign(request.ragdollId, staged);
				stagedAllocations.push_back(std::move(staged));
			}

			if(!diagnostic.empty()) {
				rejectBatch(std::move(diagnostic));
				return;
			}
			std::sort(touchedPoseIds.begin(), touchedPoseIds.end());
			touchedPoseIds.erase(std::unique(touchedPoseIds.begin(), touchedPoseIds.end()), touchedPoseIds.end());
			std::shared_ptr<const Animation::RagdollPhysicsResultSnapshot> preparedResults;
			if(ragdollPhysicsBridge.PrepareResultSnapshot(requests, committedStep,
				Animation::AnimationStatus::Ok, "The ragdoll lifecycle batch committed atomically.",
				preparedResults) != Animation::AnimationStatus::Ok) {
				rollbackStaging();
				ragdollPhysicsBridge.RestoreUndeliveredDrain(std::move(requests));
				++diagnosticCounters.rejectedCount;
				return;
			}
			// Motor replacement is allocation-free after provider validation. Apply all
			// drives only after every request and result record has been prepared; if a
			// provider unexpectedly rejects one, restore the already-applied prefix.
			for(; appliedMotorUpdateCount < motorUpdates.size(); ++appliedMotorUpdateCount) {
				const Backend::PhysicsBackendResult updated = backend->UpdateConstraint(
					motorUpdates[appliedMotorUpdateCount].next);
				if(!updated.Succeeded()) {
					diagnostic = updated.message.empty()
						? "The provider rejected a ragdoll motor drive update." : updated.message;
					break;
				}
			}
			if(!diagnostic.empty()) {
				rejectBatch(std::move(diagnostic));
				return;
			}
			if(staging.IsValid()) {
				const Backend::PhysicsBackendResult committed = backend->CommitStaging(staging);
				if(!committed.Succeeded()) {
					rejectBatch(committed.message.empty()
						? "The provider rejected the complete staged ragdoll batch." : committed.message);
					return;
				}
			}

			for(RuntimeRagdoll& retired : retireAfterCommit) DestroyCommittedRagdollLocked(retired);
			runtimeRagdolls = std::move(candidate);
			ragdollPhysicsBridge.PublishPreparedResultsAtProtectedBoundary(
				std::move(preparedResults), touchedPoseIds);
			diagnosticCounters.commandCount += requests.size();
		}

		void CommitSecondaryFrameBodiesLocked() {
			for(auto& [id, frame] : frameBackends) {
				(void)id;
				if(frame.preparedBodies.empty()) continue;
				PhysicsWorld* frameWorld = frameRegistry.FindWorldForProtectedAccess(frame.frame);
				if(frameWorld == nullptr) continue;
				Backend::PhysicsBackendAdmissionReservation reservation;
				reservation.world = frameWorld->Handle();
				reservation.shapeCount = static_cast<std::uint32_t>(frame.preparedBodies.size());
				reservation.bodyCount = static_cast<std::uint32_t>(frame.preparedBodies.size());
				Backend::PhysicsBackendPreparedAdmission admission;
				if(!frame.backend->ReserveAdmission(reservation, admission).Succeeded()) {
					++diagnosticCounters.rejectedCount;
					continue;
				}
				std::vector<RuntimeBody> staged;
				std::vector<Backend::PhysicsBodyCreateRequest> requests;
				bool rejected = false;
				for(const PreparedBody& prepared : frame.preparedBodies) {
					if(!prepared.shapeRecipe || !prepared.material || prepared.colliders.empty()) { rejected = true; break; }
					const PhysicsShapeHandle shape = frameWorld->AllocateShape();
					const PhysicsBodyHandle body = frameWorld->AllocateBody();
					if(!shape.IsValid() || !body.IsValid()
						|| !frame.backend->CreateShape(frameWorld->Handle(), shape, *prepared.shapeRecipe, admission.staging).Succeeded()) {
						if(shape.IsValid()) frameWorld->Release(shape);
						if(body.IsValid()) frameWorld->Release(body);
						rejected = true;
						break;
					}
					Backend::PhysicsBodyCreateRequest request;
					request.handle = body; request.objectId = prepared.body.objectId;
					request.objectGeneration = prepared.body.objectGeneration;
					request.componentGeneration = prepared.body.componentGeneration;
					request.shape = shape; request.motionType = prepared.body.motionType;
					request.motionQuality = prepared.body.motionQuality; request.pose = prepared.body.worldPose;
					request.linearVelocity = prepared.body.initialLinearVelocity;
					request.angularVelocity = prepared.body.initialAngularVelocity;
					request.mass = prepared.body.mass; request.friction = prepared.material->dynamicFriction;
					request.restitution = prepared.material->restitution; request.collisionLayer = prepared.colliders.front().collisionLayer;
					request.collisionMask = prepared.body.collisionMask; request.useGravity = prepared.body.useGravity;
					request.allowSleeping = prepared.body.allowSleeping; request.startAwake = prepared.body.startAwake;
					request.enabled = prepared.body.enabled;
					requests.push_back(request);
					RuntimeBody runtime{prepared.body.objectId, prepared.body.objectGeneration,
						prepared.body.componentGeneration, body, shape, prepared.body.motionType,
						prepared.body.parentWorldPose, prepared.body.parentWorldScale, prepared.body.hasParent};
					runtime.objectTransformRevision = prepared.body.objectTransformRevision;
					runtime.hierarchyGeneration = authoring->Data().hierarchyGeneration;
					runtime.parentObjectId = prepared.body.parentObjectId;
					runtime.parentObjectGeneration = prepared.body.parentObjectGeneration;
					runtime.parentTransformRevision = prepared.body.parentTransformRevision;
					runtime.frameMembership = prepared.body.frameMembership;
					staged.push_back(std::move(runtime));
				}
				if(!rejected && !frame.backend->CreateBodies(requests, admission.staging).Succeeded()) rejected = true;
				if(!rejected && !frame.backend->CommitStaging(admission.staging).Succeeded()) rejected = true;
				if(rejected) {
					(void)frame.backend->RollbackStaging(admission.staging);
					for(const RuntimeBody& runtime : staged) { frameWorld->Release(runtime.body); frameWorld->Release(runtime.shape); }
					++diagnosticCounters.rejectedCount;
					continue;
				}
				for(RuntimeBody& runtime : staged) {
					const auto old = frame.runtimeBodies.find(runtime.objectId);
					if(old != frame.runtimeBodies.end()) {
						(void)frame.backend->DestroyBody(old->second.body);
						(void)frame.backend->DestroyShape(old->second.shape);
						frame.bodyObjects.erase(old->second.body.slotIndex);
						frameWorld->Release(old->second.body); frameWorld->Release(old->second.shape);
					}
					frame.bodyObjects[runtime.body.slotIndex] = runtime.objectId;
					frame.runtimeBodies[runtime.objectId] = std::move(runtime);
				}
			}
		}

		void CommitLocked() {
			const Clock::time_point start = Clock::now();
			CommitSecondaryFrameBodiesLocked();
			CommitRagdollRequestsLocked();
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
			// The complete typed closure is known before provider staging. Reserve all
			// engine-owned containers once so a 10,000-body admission does not grow
			// maps and rollback vectors record by record inside the protected phase.
			stagedBodies.reserve(preparedBodies.size());
			stagedConstraints.reserve(preparedConstraints.size());
			stagedCharacters.reserve(preparedCharacters.size());
			registeredConstraintHandles.reserve(preparedConstraints.size());
			stagedBodyHandles.reserve(preparedBodies.size());
			stagedConstraintHandles.reserve(preparedConstraints.size());

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

			const std::size_t admissionItemCount = preparedBodies.size()
				+ preparedConstraints.size() + preparedCharacters.size();
			if(admissionItemCount != 0U) {
				Backend::PhysicsBackendAdmissionReservation reservation;
				reservation.world = world.Handle();
				reservation.shapeCount = static_cast<std::uint32_t>(preparedBodies.size());
				reservation.bodyCount = static_cast<std::uint32_t>(preparedBodies.size());
				reservation.constraintCount = static_cast<std::uint32_t>(preparedConstraints.size());
				reservation.characterCount = static_cast<std::uint32_t>(preparedCharacters.size());
				Backend::PhysicsBackendPreparedAdmission preparedAdmission;
				const Backend::PhysicsBackendResult reserved = backend->ReserveAdmission(
					reservation, preparedAdmission);
				if(!reserved.Succeeded()) rejectTransaction(reserved.message);
				else staging = preparedAdmission.staging;
			}
			// Shapes must be staged before their bodies, but the body records themselves
			// are independent until constraints bind them. Keep that immutable packet so
			// providers can admit a large unconstrained body wave under one boundary.
			std::vector<Backend::PhysicsBodyCreateRequest> bodyAdmissionPacket;
			bodyAdmissionPacket.reserve(preparedBodies.size());

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
					bodyAdmissionPacket.push_back(std::move(request));
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
				failedRuntime.frameMembership = prepared.body.frameMembership;
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
				runtime.frameMembership = prepared.body.frameMembership;
				stagedBodies.push_back({prepared.body.objectId, runtime});
				stagedBodyHandles[prepared.body.objectId] = newBody;
			}

			if(!transactionRejected && !bodyAdmissionPacket.empty()) {
				const Backend::PhysicsBackendResult result = backend->CreateBodies(bodyAdmissionPacket, staging);
				if(!result.Succeeded()) {
					// The provider retains staged ownership until rollback, including every
					// member before the rejected request, so no partial runtime can publish.
					rejectTransaction(result.message);
				}
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

		void PublishRagdollExtractionLocked() {
			if(runtimeRagdolls.empty() || extraction.tick == 0U) return;
			// Build one bounded lookup for this immutable extraction. A ragdoll may
			// contain many bodies, so repeatedly scanning all extracted state would turn
			// a crowd into quadratic protected-boundary work.
			std::unordered_map<PhysicsSlotIndex, const Backend::PhysicsBackendBodyState*> extractionBySlot;
			extractionBySlot.reserve(extraction.bodies.size());
			for(const Backend::PhysicsBackendBodyState& state : extraction.bodies) {
				const auto [entry, inserted] = extractionBySlot.emplace(state.handle.slotIndex, &state);
				// A duplicate provider slot is invalid evidence. Store a null marker so
				// every affected ragdoll rejects atomically instead of selecting one record.
				if(!inserted) entry->second = nullptr;
			}
			std::vector<std::uint64_t> ragdollIds;
			ragdollIds.reserve(runtimeRagdolls.size());
			for(const auto& [ragdollId, ragdoll] : runtimeRagdolls) {
				if(ragdoll.active && ragdoll.profile) ragdollIds.push_back(ragdollId);
			}
			std::sort(ragdollIds.begin(), ragdollIds.end());
			for(const std::uint64_t ragdollId : ragdollIds) {
				auto runtime = runtimeRagdolls.find(ragdollId);
				if(runtime == runtimeRagdolls.end()) continue;
				RuntimeRagdoll& ragdoll = runtime->second;
				if(!ragdoll.active || !ragdoll.profile) continue;
				Animation::PhysicsPoseSnapshot snapshot;
				snapshot.generation = ragdoll.generation;
				snapshot.generation.worldGeneration = world.Handle().worldGeneration;
				snapshot.generation.fixedStep = static_cast<Animation::AnimationFixedTick>(extraction.tick);
				snapshot.bodies.reserve(ragdoll.bodies.size());
				std::vector<Animation::PhysicsPoseExpectedBodyGeneration> expectedBodies;
				expectedBodies.reserve(ragdoll.bodies.size());
				std::vector<RuntimeRagdollBody> updatedBodies = ragdoll.bodies;
				bool complete = true;
				for(std::size_t index = 0U; index < ragdoll.bodies.size(); ++index) {
					const RuntimeRagdollBody& runtimeBody = ragdoll.bodies[index];
					const auto indexed = extractionBySlot.find(runtimeBody.body.slotIndex);
					const Backend::PhysicsBackendBodyState* extracted = indexed == extractionBySlot.end()
						? nullptr : indexed->second;
					const TerraEngine::UUID expectedObjectId = MakeRagdollRuntimeUuid(
						ragdollId, runtimeBody.bodyId, 1U);
					if(extracted == nullptr || extracted->handle != runtimeBody.body
						|| extracted->objectId != expectedObjectId
						|| extracted->objectGeneration != ragdoll.generation.publicationGeneration
						|| extracted->componentGeneration != ragdoll.generation.profileGeneration
						|| world.Status(runtimeBody.body) != PhysicsHandleStatus::Alive
						|| !HasPhysicsBodyStateFields(extracted->fields, PhysicsBodyStateFieldMask::BodyState)) {
						complete = false;
						break;
					}
					Animation::PhysicsPoseBodySnapshot body;
					body.bodyId = runtimeBody.bodyId;
					body.stableJointId = runtimeBody.stableJointId;
					body.modelPose = ToAnimationTransform(extracted->pose);
					body.linearVelocity = {extracted->linearVelocity.x,
						extracted->linearVelocity.y, extracted->linearVelocity.z};
					body.angularVelocity = {extracted->angularVelocity.x,
						extracted->angularVelocity.y, extracted->angularVelocity.z};
					body.sleeping = !extracted->awake;
					body.bodyGeneration = runtimeBody.body.slotGeneration;
					snapshot.bodies.push_back(body);
					expectedBodies.push_back({body.bodyId, body.bodyGeneration});
					updatedBodies[index].lastWorldPose = body.modelPose;
					updatedBodies[index].lastLinearVelocity = body.linearVelocity;
					updatedBodies[index].lastAngularVelocity = body.angularVelocity;
				}
				for(const RuntimeRagdollConstraint& constraint : ragdoll.constraints) {
					if(world.Status(constraint.handle) != PhysicsHandleStatus::Alive
						|| constraint.handle.worldGeneration != snapshot.generation.worldGeneration) {
						complete = false;
						break;
					}
				}
				if(!complete || snapshot.bodies.size() != ragdoll.bodies.size()) {
					++diagnosticCounters.staleCount;
					continue;
				}
				snapshot.completeCurrentStep = true;
				const Animation::RagdollGenerationStamp publishedGeneration = snapshot.generation;
				if(ragdollPhysicsBridge.PublishCurrentStepPose(
					ragdollId, std::move(snapshot), publishedGeneration, expectedBodies)
					== Animation::AnimationStatus::Ok) {
					ragdoll.generation.fixedStep = static_cast<Animation::AnimationFixedTick>(extraction.tick);
					ragdoll.bodies = std::move(updatedBodies);
				} else {
					++diagnosticCounters.staleCount;
				}
			}
		}

		void ExtractSecondaryFramesLocked() {
			if(!authoring) return;
			for(auto& [id, frame] : frameBackends) {
				(void)id;
				PhysicsWorld* frameWorld = frameRegistry.FindWorldForProtectedAccess(frame.frame);
				if(frameWorld == nullptr || frame.runtimeBodies.empty()) continue;
				const PhysicsTick tick = frameWorld->Timing().completedTick;
				if(tick == 0U) continue;
				Backend::PhysicsBackendExtraction extraction;
				Backend::PhysicsBackendExtractionRequest request;
				request.world = frameWorld->Handle(); request.tick = tick;
				request.selection = Backend::PhysicsBackendExtractionSelection::FullState;
				request.fullState = true; request.fields = PhysicsBodyStateFieldMask::BodyState;
				request.drainEvents = true;
				if(!frame.backend->Extract(request, extraction).Succeeded()) {
					++diagnosticCounters.rejectedCount;
					continue;
				}
				PhysicsBodyStateValidationScratch scratch;
				const auto updates = PhysicsWorld::ConvertExtractedBodyStates(extraction, scratch);
				if(!updates) { ++diagnosticCounters.staleCount; continue; }
				const auto descriptor = frameRegistry.Find(frame.frame);
				if(!descriptor) { ++diagnosticCounters.staleCount; continue; }
				std::vector<TransformPublicationSourceRecord> sources;
				for(const IncrementalBodyStateUpdate& update : updates->updates) {
					if(update.removed) continue;
					const auto runtime = frame.runtimeBodies.find(update.state.objectId);
					if(runtime == frame.runtimeBodies.end() || runtime->second.body != update.state.handle
						|| runtime->second.motionType != PhysicsMotionType::Dynamic) continue;
					const RuntimeBody& body = runtime->second;
					sources.push_back({.objectId = body.objectId, .body = body.body, .frame = frame.frame,
						.objectLifetimeGeneration = body.objectGeneration, .bodyGeneration = body.body.slotGeneration,
						.componentGeneration = body.componentGeneration, .expectedTransformRevision = body.objectTransformRevision,
						.expectedHierarchyGeneration = body.hierarchyGeneration, .parentObjectId = body.parentObjectId,
						.expectedParentLifetimeGeneration = body.parentObjectGeneration,
						.expectedParentTransformRevision = body.parentTransformRevision,
						.canonicalFrameOrigin = descriptor->canonicalOrigin, .localPose = update.state.worldPose,
						.worldPoseReceipt = update.state.worldPose, .poseChanged = true});
				}
				if(sources.empty()) continue;
				TransformPublicationPacket packet = PrepareTransformPublicationPacket(
					{frameWorld->Handle(), frame.frame, tick, 1U, authoring->Data().snapshotGeneration},
					authoring->Data().sceneGeneration, authoring->Data().hierarchyGeneration,
					++nextTransformPublicationGroupId, ++nextTransformPacketGeneration, sources,
					configuredSettings.transformPublicationChunkSize,
					TerraEngine::Scheduling::SameFramePhysicsPublicationConsumerActive()
						? TransformPublicationVisibilityBoundary::AfterPhysics : TransformPublicationVisibilityBoundary::FrameEnd);
				if(!ValidateTransformPublicationPacket(packet).accepted) { ++diagnosticCounters.rejectedCount; continue; }
				Scene::TransactionPublicationGroup group = BuildTransformPublicationGroup(packet);
				pendingTransformPublications.push_back({packet, group.groupId, {}, frame.backend.get()});
				Scene::SceneMutationQueue::Instance().EnqueuePublicationGroup(std::move(group));
			}
		}

		void ExtractLocked() {
			ExtractSecondaryFramesLocked();
			if(completedSteps.empty()) return;
			const Clock::time_point extractionStart = Clock::now();
			extraction = {};
			extractionToken = {};
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
			const bool ragdollConstraints = std::any_of(runtimeRagdolls.begin(), runtimeRagdolls.end(),
				[](const auto& entry) { return !entry.second.constraints.empty(); });
			if(!runtimeConstraints.empty() || ragdollConstraints) request.fields |= PhysicsBodyStateFieldMask::Constraints;
			if(!runtimeCharacters.empty()) request.fields |= PhysicsBodyStateFieldMask::Characters;
			request.batchSize = configuredSettings.providerExtractionBatchSize;
			request.parallelThreshold = configuredSettings.parallelExtractionThreshold;
			// Ragdoll presentation consumes every mapped body even when sleeping or
			// offscreen. Those bodies are appended as explicit handles, so ragdolls do
			// not turn an otherwise incremental frame into a full provider-world read.
			request.fullState = world.AcquireCompleteBodyState() == nullptr;
			request.selection = request.fullState
				? Backend::PhysicsBackendExtractionSelection::FullState
				: Backend::PhysicsBackendExtractionSelection::PublicationRelevant;
			if(!request.fullState) {
				for(const auto& [ragdollId, ragdoll] : runtimeRagdolls) {
					(void)ragdollId;
					if(!ragdoll.active) continue;
					for(const RuntimeRagdollBody& body : ragdoll.bodies) request.explicitHandles.push_back(body.body);
				}
				std::sort(request.explicitHandles.begin(), request.explicitHandles.end());
				request.explicitHandles.erase(std::unique(request.explicitHandles.begin(), request.explicitHandles.end()),
					request.explicitHandles.end());
			}
			request.allowParallelReads = true;
			request.drainEvents = true;
			Backend::PhysicsBackendResult result;
			if(!request.fullState) {
				// A normal tick asks the provider for a retryable lease. The profile is
				// value-only; Jolt owns its active/dirty membership until publication
				// acknowledges it at the protected boundary below.
				Backend::PhysicsBackendFixedStepWorkProfile profile;
				profile.world = request.world;
				profile.tick = request.tick;
				profile.fields = request.fields;
				profile.rangeSize = request.batchSize;
				profile.explicitRequiredHandles = request.explicitHandles;
				// Ragdoll and event readers are explicit immutable consumers of this
				// lease, making provider selection and acknowledgement diagnostics clear.
				profile.consumers = Backend::PhysicsBackendConsumerMask::GameplayEvents
					| Backend::PhysicsBackendConsumerMask::DiagnosticEvents;
				if(!request.explicitHandles.empty()) {
					profile.consumers = profile.consumers | Backend::PhysicsBackendConsumerMask::RagdollPose;
				}
				profile.availableWorkers = static_cast<std::uint32_t>(
					std::max<std::size_t>(1U, ThreadPool::instance().getThreadCount()));
				result = backend->Extract(profile, extraction, extractionToken);
			} else {
				// Initial and recovery publication retain the established complete-state
				// compatibility path until their full-world consumer has an incremental page.
				result = backend->Extract(request, extraction);
			}
			if(!result.Succeeded()) {
				AppendCommandResult(0U, PhysicsResultStatus::ProviderFailure, request.tick, result.message);
				extraction.bodies.clear();
			} else {
				preparedBodyStateBatch = PhysicsWorld::ConvertExtractedBodyStates(
					extraction, bodyStateValidationScratch);
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
						.frame = body.frameMembership.frame,
						.objectLifetimeGeneration = body.objectGeneration,
						.bodyGeneration = body.body.slotGeneration,
						.componentGeneration = body.componentGeneration,
						.expectedTransformRevision = body.objectTransformRevision,
						.expectedHierarchyGeneration = body.hierarchyGeneration,
						.parentObjectId = body.parentObjectId,
						.expectedParentLifetimeGeneration = body.parentObjectGeneration,
						.expectedParentTransformRevision = body.parentTransformRevision,
						// Primary-frame runtime bodies are localized about canonical zero;
						// secondary packets receive their descriptor origin during their
						// matching frame extraction path.
						.canonicalFrameOrigin = {},
						.localPose = localPose,
						.worldPoseReceipt = update.state.worldPose,
						.poseChanged = !body.hasPublishedPose
							|| !SamePose(body.lastPublishedPose, update.state.worldPose)});
				}
			}
			if(result.Succeeded()) PublishRagdollExtractionLocked();
			ExecutePreparedQueriesLocked();
			diagnosticTimings.extractionMs = MillisecondsSince(extractionStart);
		}

		Backend::PhysicsBackendQueryRequest MakeBackendQueryRequest(const PhysicsQueryRequest& request) const {
			Backend::PhysicsBackendQueryRequest converted;
			converted.world = request.world;
			converted.frame = request.frame;
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
			backendQueryRequests.clear();
			executedQueries.clear();
			if(preparedQueries.queries.empty()) {
				diagnosticTimings.queryMs = 0.0;
				return;
			}
			backendQueryRequests.reserve(preparedQueries.queries.size());
			for(const PhysicsPreparedQuery& query : preparedQueries.queries) {
				backendQueryRequests.push_back(MakeBackendQueryRequest(query.request));
			}
			executedQueries.resize(backendQueryRequests.size());
			ThreadPool& pool = ThreadPool::instance();
			const std::size_t workerCount = pool.getThreadCount();
			if(workerCount <= 1U || backendQueryRequests.size() < 64U) {
				const Backend::PhysicsBackendResult batch = backend->QueryBatch(
					backendQueryRequests, executedQueries);
				if(!batch.Succeeded()) {
					for(Backend::PhysicsBackendQueryResult& result : executedQueries) {
						result = {batch, {}, false};
					}
				}
				diagnosticTimings.queryMs = MillisecondsSince(start);
				return;
			}

			// Each task writes a disjoint result range and all backend calls take a
			// shared read lock after simulation has joined. No task touches scene data.
			const std::size_t taskCount = std::min(workerCount - 1U, backendQueryRequests.size());
			const std::size_t chunkSize = (backendQueryRequests.size() + taskCount - 1U) / taskCount;
			std::vector<std::future<void>> futures;
			futures.reserve(taskCount);
			for(std::size_t begin = 0U; begin < backendQueryRequests.size(); begin += chunkSize) {
				const std::size_t end = std::min(backendQueryRequests.size(), begin + chunkSize);
				auto task = pool.createTask([this, begin, end]() {
					const std::span<const Backend::PhysicsBackendQueryRequest> requests(
						backendQueryRequests.data() + begin, end - begin);
					const std::span<Backend::PhysicsBackendQueryResult> results(
						executedQueries.data() + begin, end - begin);
					const Backend::PhysicsBackendResult batch = backend->QueryBatch(requests, results);
					if(!batch.Succeeded()) {
						for(Backend::PhysicsBackendQueryResult& result : results) {
							result = {batch, {}, false};
						}
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
			std::uint64_t& staleCount,
			PhysicsFrameHandle frame = {}) const {
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
				hit.frame = frame;
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
				{publication.complete->world, {kPrimaryPhysicsFrameId, publication.complete->world.worldGeneration}, publication.complete->tick,
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
			pendingTransformPublications.push_back({packet, group.groupId, extractionToken});
			Scene::SceneMutationQueue::Instance().EnqueuePublicationGroup(std::move(group));
		}

		void PublishLocked() {
			const Clock::time_point start = Clock::now();
			if(completedSteps.empty()) return;
			bool statePublicationAccepted = !preparedBodyStateBatch.has_value();
			if(preparedBodyStateBatch) {
				const Clock::time_point validationStart = Clock::now();
				const auto publication = world.PublishPreparedBodyStates(*preparedBodyStateBatch);
				diagnosticTimings.stateValidationMs = MillisecondsSince(validationStart);
				if(publication) {
					statePublicationAccepted = true;
					latestIncrementalBodies = publication->complete;
					// The flat compatibility view is materialized only when a legacy
					// caller asks for it, so unchanged pages remain shared in the hot path.
					latestBodies.reset();
					lastStatePagesReused = publication->pagesReused;
					lastStatePagesCopied = publication->pagesCopied;
					++stateStorePublicationCount;
					const Clock::time_point transactionStart = Clock::now();
					PublishTransformTransactionsLocked(*publication);
					diagnosticTimings.transactionConstructionMs = MillisecondsSince(transactionStart);
				} else {
					++stateStoreRejectedBatchCount;
					++diagnosticCounters.rejectedCount;
				}
			}
			const Clock::time_point snapshotStart = Clock::now();
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
			if(latestCommands) {
				// Root-motion receipts become final only after Physics has published
				// both the command outcome and the adjusted authoritative target pose.
				TerraEngine::Animation::AnimationRootMotionResultPublisher::Instance().ObservePhysicsResults(
					*latestCommands, latestIncrementalBodies, latestCharacters);
			}
			diagnosticTimings.snapshotConstructionMs = MillisecondsSince(snapshotStart);

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
			if(extractionToken.IsValid()) {
				const bool deferredToTransactionReceipt = std::any_of(
					pendingTransformPublications.begin(), pendingTransformPublications.end(),
					[this](const PendingTransformPublication& publication) {
						return publication.extractionToken.world == extractionToken.world
							&& publication.extractionToken.tick == extractionToken.tick
							&& publication.extractionToken.value == extractionToken.value;
					});
				const Backend::PhysicsBackendResult leaseResult = deferredToTransactionReceipt
					? Backend::PhysicsBackendResult{}
					: (statePublicationAccepted
						? backend->AcknowledgeExtraction(extractionToken)
						: backend->AbortExtraction(extractionToken));
				if(!leaseResult.Succeeded()) {
					AppendCommandResult(0U, PhysicsResultStatus::ProviderFailure, extraction.tick,
						"The provider rejected the protected extraction lease transition.");
				}
				extractionToken = {};
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
				authoringDiagnostics.sceneGeneration = data.sceneGeneration;
				authoringDiagnostics.scenePublicationRevision = data.scenePublicationRevision;
				authoringDiagnostics.constraintClosureCount = data.constraintRecordIds.size();
				authoringDiagnostics.characterClosureCount = data.characterObjectIds.size();
				authoringDiagnostics.removalClosureCount = data.removedObjectIds.size();
				authoringDiagnostics.fallbackReason = static_cast<std::uint8_t>(data.fallbackReason);
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
			scheduler->push_back({"PhysicsCommandCommit", "Physics", "PhysicsPreparedWork,RagdollPhysicsQueue", "PhysicsWorld,RagdollPhysicsSnapshots,RagdollPhysicsResults", "hard boundary: typed ragdoll drain", false,
				true, "prepared provider mutation, command, or ragdoll request work available", "PhysicsPrepare", diagnosticCounters.commandCount + ragdollPhysicsBridge.PendingCount()});
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
			const auto stage = [](PhysicsStageKind kind, double milliseconds, std::uint64_t workItems) {
				PhysicsStageMeasurement measurement;
				measurement.stage = kind;
				measurement.availability = PhysicsMetricAvailability::Available;
				measurement.unavailableReason = PhysicsMetricUnavailableReason::None;
				measurement.durationNanoseconds = NanosecondsFromMilliseconds(milliseconds);
				measurement.workItems = workItems;
				return measurement;
			};
			diagnostics.stages = {
				stage(PhysicsStageKind::AuthoringPreparation, diagnosticTimings.prepareMs, preparationDiagnostics.estimatedWorkItems),
				stage(PhysicsStageKind::ProviderCommit, diagnosticTimings.commitMs, diagnosticCounters.commandCount),
				stage(PhysicsStageKind::Simulation, diagnosticTimings.simulateMs, completedSteps.size()),
				stage(PhysicsStageKind::Extraction, diagnosticTimings.extractionMs, extractionDiagnostics.examinedBodies),
				stage(PhysicsStageKind::EventPublication, diagnosticTimings.eventPublishMs, extraction.events.size()),
				stage(PhysicsStageKind::StateValidation, diagnosticTimings.stateValidationMs, extractionDiagnostics.convertedBodies),
				stage(PhysicsStageKind::SnapshotConstruction, diagnosticTimings.snapshotConstructionMs, extractionDiagnostics.changedBodies),
				stage(PhysicsStageKind::TransactionConstruction, diagnosticTimings.transactionConstructionMs, publicationDiagnostics.currentEntries)};
			const Backend::PhysicsBackendSimulationQuality appliedQuality = backend->AppliedSimulationQuality();
			diagnostics.quality.settingsSchemaVersion = configuredSettings.schemaVersion;
			diagnostics.quality.settingsGeneration = settingsGeneration;
			diagnostics.quality.worldGeneration = appliedQuality.world.worldGeneration;
			diagnostics.quality.velocityIterations = appliedQuality.velocityIterations;
			diagnostics.quality.positionIterations = appliedQuality.positionIterations;
			diagnostics.quality.applied = appliedQuality.applied;
			const Backend::PhysicsBackendJobTelemetry jobTelemetry = backend->JobTelemetry();
			diagnostics.jobs.submitted = jobTelemetry.submitted;
			diagnostics.jobs.completed = jobTelemetry.completed;
			diagnostics.jobs.rejected = jobTelemetry.rejected;
			diagnostics.jobs.batchCount = jobTelemetry.batchCount;
			diagnostics.jobs.lockAcquisitionCount = jobTelemetry.lockAcquisitionCount;
			diagnostics.jobs.pumpSubmissionCount = jobTelemetry.pumpSubmissionCount;
			diagnostics.jobs.barrierWaitCount = jobTelemetry.barrierWaitCount;
			diagnostics.jobs.barrierWaitMicroseconds = jobTelemetry.barrierWaitMicroseconds;
			diagnostics.jobs.maximumBarrierWaitMicroseconds = jobTelemetry.maximumBarrierWaitMicroseconds;
			diagnostics.jobs.jobExecutionMicroseconds = jobTelemetry.jobExecutionMicroseconds;
			diagnostics.jobs.queued = jobTelemetry.queued;
			diagnostics.jobs.activePumps = jobTelemetry.activePumps;
			diagnostics.jobs.maximumActivePumps = jobTelemetry.maximumActivePumps;
			diagnostics.jobs.workerLimit = jobTelemetry.workerLimit;
			diagnostics.jobs.observedWorkerThreads = jobTelemetry.observedWorkerThreads;
			diagnostics.jobs.idle = jobTelemetry.idle;
			diagnostics.authoring = std::move(authoringDiagnostics);
			diagnostics.preparation = std::move(preparationDiagnostics);
			diagnostics.extraction = extractionDiagnostics;
			diagnostics.stateStore = stateStoreDiagnostics;
			diagnostics.publication = publicationDiagnostics;
			diagnostics.rate = rate;
			auto frameDiagnostics = std::make_shared<std::vector<PhysicsFrameDiagnostics>>();
			for(const PhysicsFrameDescriptor& descriptor : frameRegistry.Descriptors()) {
				PhysicsFrameDiagnostics frame;
				frame.descriptor = descriptor;
				const auto backendRecord = frameBackends.find(descriptor.frame.frameId.value);
				if(backendRecord != frameBackends.end()
					&& backendRecord->second.frame == descriptor.frame) {
					frame.migrationAcceptedCount = backendRecord->second.migrationAcceptedCount;
					frame.migrationRolledBackCount = backendRecord->second.migrationRolledBackCount;
				}
				if(PhysicsWorld* frameWorld = frameRegistry.FindWorldForProtectedAccess(descriptor.frame)) {
					const PhysicsWorldRegistryStatistics statistics = frameWorld->RegistryStatistics();
					const PhysicsQueryDiagnostics queries = frameWorld->QueryDiagnostics();
					frame.bodyCount = statistics.bodyCount;
					frame.queryDepth = queries.queuedDepth;
					frame.eventDepth = frameWorld->AcquireEventPacket(
						frameWorld->Timing().completedTick, frameWorld->Timing().completedTick) ? 1U : 0U;
				}
				frameDiagnostics->push_back(std::move(frame));
			}
			diagnostics.frames = std::move(frameDiagnostics);
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
				bool publicationAccepted = false;
				if(result != resultsByGroup.end()) {
					const TransformPublicationReceipt receipt =
						MakeTransformPublicationReceipt(publication.packet, *result->second);
					if(receipt.Accepted()) {
						publicationAccepted = true;
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
					}
				}
				if(!publicationAccepted) {
					// Leave lastPublishedPose unchanged so the next extraction retries a
					// rejected write even if the provider body has already gone to sleep.
					++diagnosticCounters.rejectedCount;
					++publicationDiagnostics.rejectedPackets;
					publicationDiagnostics.lastFailure = "Transform publication did not commit at its declared boundary.";
				}
				if(publication.extractionToken.IsValid()) {
					Backend::IPhysicsBackend* extractionBackend = publication.extractionBackend != nullptr
						? publication.extractionBackend : backend.get();
					const Backend::PhysicsBackendResult leaseResult = publicationAccepted
						? extractionBackend->AcknowledgeExtraction(publication.extractionToken)
						: extractionBackend->AbortExtraction(publication.extractionToken);
					if(!leaseResult.Succeeded()) {
						++diagnosticCounters.rejectedCount;
						publicationDiagnostics.lastFailure = "The provider rejected the extraction lease receipt transition.";
					}
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

		void AdvanceSecondaryFramesLocked(float deltaTime) {
			// Each island has its own provider adapter, so stepping one frame cannot
			// share broad-phase, query, or callback state with a distant frame.
			for(auto& [id, record] : frameBackends) {
				(void)id;
				PhysicsWorld* frameWorld = frameRegistry.FindWorldForProtectedAccess(record.frame);
				if(frameWorld == nullptr) continue;
				const PhysicsFixedStepPlan plan = frameWorld->PlanFixedSteps(deltaTime);
				for(const PhysicsFixedStepRequest& request : plan.requests) {
					const Backend::PhysicsBackendResult stepped = record.backend->Step({
						.world = request.world, .tick = request.tick, .deltaTime = request.deltaTime});
					if(stepped.Succeeded()) (void)frameWorld->MarkStepCompleted(request);
					else (void)frameWorld->MarkFailed();
				}
			}
		}

		std::unique_ptr<Backend::IPhysicsBackend> backend;
		PhysicsWorld world;
		// Secondary islands own distinct Jolt adapter instances. Jolt's private
		// state is therefore never shared across unrelated canonical origins.
		struct FrameBackendRecord {
			PhysicsFrameHandle frame;
			std::unique_ptr<Backend::IPhysicsBackend> backend;
			std::vector<PreparedBody> preparedBodies;
			std::unordered_map<TerraEngine::UUID, RuntimeBody> runtimeBodies;
			std::unordered_map<PhysicsSlotIndex, TerraEngine::UUID> bodyObjects;
			std::uint64_t migrationAcceptedCount = 0U;
			std::uint64_t migrationRolledBackCount = 0U;
		};
		PhysicsFrameRegistry frameRegistry;
		std::map<std::uint64_t, FrameBackendRecord> frameBackends;
		std::unordered_map<TerraEngine::UUID, PhysicsFrameMembership> frameMembershipOverrides;
		bool backendInitialized = false;
		bool editorLifecycleControlled = false;
		std::optional<PhysicsAuthoringSnapshot> authoring;
		std::optional<PhysicsAuthoringGeneration> authoringGeneration;
		std::optional<PhysicsPreparationPlan> preparationPlan;
		PhysicsPreparationScratch preparationScratch;
		PhysicsAssetGeneration observedPhysicsAssetGeneration = 0U;
		std::uint64_t authoringEquivalentSceneSpatialRevision = 0U;
		std::uint64_t lastAuthoringSceneGeneration = 0U;
		std::uint64_t lastAuthoringScenePublicationRevision = 0U;
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
		// Prepared provider packets are retained across ticks so their capacity can
		// be reused instead of rebuilding a temporary request vector per query task.
		std::vector<Backend::PhysicsBackendQueryRequest> backendQueryRequests;
		std::vector<Backend::PhysicsBackendQueryResult> executedQueries;
		std::vector<PhysicsFixedStepRequest> completedSteps;
		std::vector<PhysicsFixedStepRequest> pendingFlushSteps;
		std::vector<PendingTransformPublication> pendingTransformPublications;
		Backend::PhysicsBackendExtraction extraction;
		Backend::PhysicsBackendExtractionToken extractionToken;
		PhysicsBodyStateValidationScratch bodyStateValidationScratch;
		std::optional<IncrementalBodyStateUpdateBatch> preparedBodyStateBatch;
		std::vector<TransformPublicationSourceRecord> preparedTransformSources;
		std::uint64_t nextTransformPublicationGroupId = 0U;
		std::uint64_t nextTransformPacketGeneration = 0U;
		std::unordered_map<TerraEngine::UUID, RuntimeBody> runtimeBodies;
		std::unordered_map<TerraEngine::UUID, RuntimeConstraint> runtimeConstraints;
		std::unordered_map<TerraEngine::UUID, RuntimeCharacter> runtimeCharacters;
		std::unordered_map<std::uint64_t, RuntimeRagdoll> runtimeRagdolls;
		// Slot lookup keeps extraction O(n) for large scenes while the full typed
		// handle comparison above still rejects stale slot generations.
		std::unordered_map<PhysicsSlotIndex, TerraEngine::UUID> bodyObjects;
		std::unordered_map<TerraEngine::UUID, std::filesystem::file_time_type> cookedAssetWriteTimes;
		std::unordered_set<TerraEngine::UUID> cookedAssetsNeedingReload;
		std::unordered_map<TerraEngine::UUID, std::filesystem::file_time_type> materialAssetWriteTimes;
		std::unordered_set<TerraEngine::UUID> materialAssetsNeedingReload;
		std::unordered_map<TerraEngine::UUID, PhysicsMaterialRuntime> materialAssets;
		std::vector<PhysicsCommandResult> commandResults;
		// The bridge is the only cross-system ingress. It owns copied requests and
		// immutable pose/result publications; PhysicsSystem owns the protected drain.
		Animation::RagdollPhysicsBridge ragdollPhysicsBridge;
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
		std::vector<RuntimeTerrainCollision> terrainCollisions;
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
		impl->AdvanceSecondaryFramesLocked(deltaTime);
		impl->ExtractLocked();
		impl->PublishLocked();
		impl->PublicationBarrierLocked();
	}

	void PhysicsSystem::Shutdown() {
		std::lock_guard<std::mutex> lock(impl->mutex);
		for(auto& [id, record] : impl->frameBackends) {
			(void)id;
			const auto descriptor = impl->frameRegistry.Find(record.frame);
			if(descriptor) (void)record.backend->DestroyWorld(descriptor->world);
			(void)record.backend->Shutdown();
			if(descriptor) (void)impl->frameRegistry.Destroy(record.frame);
		}
		impl->frameBackends.clear();
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
		declaration.reads = {"SceneGraph", "ObjectManager", "PhysicsSettingsUpdateQueue", "PhysicsAuthoringSnapshot", "PhysicsQueryQueue", "PhysicsEventSubscriptions", "RagdollPhysicsQueue", TerraEngine::Scheduling::kResourceTerrainPublishedCollision, TerraEngine::Scheduling::kResourcePhysicsFrameRegistry, TerraEngine::Scheduling::kResourcePhysicsFrameMigrations};
		declaration.writes = {"PhysicsSettings", "PhysicsPreparedWork", "PhysicsWorld", "PhysicsQueryResults", "PhysicsEventPackets", "PhysicsPublishedSnapshots", "RagdollPhysicsSnapshots", "RagdollPhysicsResults", "PhysicsTransformTransactions", "SceneMutationQueue", TerraEngine::Scheduling::kResourcePhysicsFrameRegistry, TerraEngine::Scheduling::kResourcePhysicsFrameMigrations};
		declaration.mainThread = true;
		declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Immutable authoring preparation, provider stepping through the shared job adapter, and extraction into typed packets.";
		declaration.protectedCommit = "World generation changes, provider staging commit, typed ragdoll request drain, immutable ragdoll snapshot/result swap, command ordering, and transform transaction publication.";
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
				{"PhysicsPreparedWork", "PhysicsWorld", "RagdollPhysicsQueue"},
				{"PhysicsWorld", "RagdollPhysicsSnapshots", "RagdollPhysicsResults"}, false, WorkerSystemPhaseKind::ProtectedCommit);
			configure(descriptors[3], "PhysicsSimulate",
				{"PhysicsWorld", kResourceTerrainPublishedCollision}, {"PhysicsWorld"}, true, WorkerSystemPhaseKind::WorkerPrepare);
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
			activation.authoringItems + activation.commandItems + activation.ragdollItems,
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
		workloads[3].work = [this, deltaTime](JobGraphContext&) { std::lock_guard<std::mutex> workloadLock(impl->mutex); impl->SimulateLocked(); impl->AdvanceSecondaryFramesLocked(deltaTime); };
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

	std::optional<PhysicsFrameDescriptor> PhysicsSystem::CreateFrame(
		const PhysicsFrameCreateRequest& request) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!impl->backendInitialized) return std::nullopt;
		// Frame world and provider creation are one protected decision: retire the
		// engine record if the matching private Jolt world cannot be initialized.
		const std::optional<PhysicsFrameDescriptor> descriptor = impl->frameRegistry.Create(request);
		if(!descriptor) return std::nullopt;
		auto backend = std::make_unique<Backend::JoltPhysicsBackend>(ThreadPool::instance());
		if(!backend->Initialize().Succeeded()) {
			(void)impl->frameRegistry.Destroy(descriptor->frame);
			return std::nullopt;
		}
		Backend::PhysicsBackendWorldConfiguration configuration;
		configuration.world = descriptor->world;
		configuration.frame = descriptor->frame;
		configuration.settings = request.settings;
		if(!backend->CreateWorld(configuration).Succeeded()) {
			(void)backend->Shutdown();
			(void)impl->frameRegistry.Destroy(descriptor->frame);
			return std::nullopt;
		}
		impl->frameBackends.emplace(descriptor->frame.frameId.value,
			Impl::FrameBackendRecord{descriptor->frame, std::move(backend)});
		return descriptor;
	}

	bool PhysicsSystem::DestroyFrame(PhysicsFrameHandle frame) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		const auto found = impl->frameBackends.find(frame.frameId.value);
		if(!frame.IsValid() || found == impl->frameBackends.end() || found->second.frame != frame) {
			return false;
		}
		const std::optional<PhysicsFrameDescriptor> descriptor = impl->frameRegistry.Find(frame);
		if(!descriptor) return false;
		const Backend::PhysicsBackendResult destroyed = found->second.backend->DestroyWorld(descriptor->world);
		if(!destroyed.Succeeded()) return false;
		(void)found->second.backend->Shutdown();
		impl->frameBackends.erase(found);
		return impl->frameRegistry.Destroy(frame);
	}

	std::vector<PhysicsFrameDescriptor> PhysicsSystem::FrameDescriptors() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->frameRegistry.Descriptors();
	}

	PhysicsFrameMigrationReceipt PhysicsSystem::CommitFrameMigration(
		const PhysicsFrameMigrationRequest& request,
		const PhysicsFrameMigrationOperations& operations) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		std::optional<PhysicsFrameDescriptor> destination =
			impl->frameRegistry.Find(request.destinationFrame);
		if(!destination && request.destinationFrame.frameId == kPrimaryPhysicsFrameId
			&& request.destinationFrame.frameGeneration == impl->world.Handle().worldGeneration) {
			destination = PhysicsFrameDescriptor{request.destinationFrame, impl->world.Handle(), {}, 8'192.0};
		}
		if(!destination) {
			return {false, PhysicsFrameMigrationState::PreflightFailed, 0U,
				"The destination physics frame is absent or stale."};
		}
		for(const PhysicsFrameMigrationMember& member : request.members) {
			const bool primarySource = member.sourceMembership.frame.frameId == kPrimaryPhysicsFrameId
				&& member.sourceMembership.world == impl->world.Handle();
			if(!primarySource && !impl->frameRegistry.Find(member.sourceMembership.frame)) {
				return {false, PhysicsFrameMigrationState::PreflightFailed, 0U,
					"A source physics frame is absent or stale."};
			}
		}
		PhysicsFrameMigrationReceipt receipt = CommitPhysicsFrameMigration(request, operations);
		if(receipt.accepted) {
			for(const PhysicsFrameMigrationMember& member : request.members) {
				impl->frameMembershipOverrides[member.objectId] = {
					request.destinationFrame, destination->world,
					request.expectedDestinationResidencyGeneration};
			}
		}
		const auto backend = impl->frameBackends.find(request.destinationFrame.frameId.value);
		if(backend != impl->frameBackends.end()) {
			if(receipt.accepted) ++backend->second.migrationAcceptedCount;
			else if(receipt.state == PhysicsFrameMigrationState::RolledBack) ++backend->second.migrationRolledBackCount;
		}
		return receipt;
	}

	TerrainCollisionReplacementReceipt PhysicsSystem::CommitTerrainCollisionReplacement(
		TerrainCollisionReplacementPacket packet) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		TerrainCollisionReplacementReceipt rejected;
		if(!impl->backend || !impl->world.Handle().IsValid() || packet.recipe.terrainAssetId == TerraEngine::EMPTY_UUID) {
			rejected.retainedLastKnownGood = true;
			rejected.message = "Terrain collision replacement requires an active primary physics world and terrain identity.";
			return rejected;
		}
		const auto prior = std::find_if(impl->terrainCollisions.begin(), impl->terrainCollisions.end(), [&](const Impl::RuntimeTerrainCollision& value) {
			return value.terrainAssetId == packet.recipe.terrainAssetId && value.areaX == packet.recipe.areaX && value.areaZ == packet.recipe.areaZ;
		});
		packet.expectedWorld = impl->world.Handle();
		if(prior != impl->terrainCollisions.end()) { packet.priorShape = prior->shape; packet.priorBody = prior->body; }
		packet.replacementShape = impl->world.AllocateShape(); packet.replacementBody.handle = impl->world.AllocateBody();
		packet.replacementBody.shape = packet.replacementShape;
		packet.replacementBody.objectId = packet.replacementBody.objectId == TerraEngine::EMPTY_UUID ? packet.recipe.terrainAssetId : packet.replacementBody.objectId;
		packet.replacementBody.objectGeneration = packet.replacementBody.objectGeneration == 0U ? std::max<PhysicsGeneration>(1U, static_cast<PhysicsGeneration>(packet.recipe.authoredGeneration)) : packet.replacementBody.objectGeneration;
		packet.replacementBody.componentGeneration = packet.replacementBody.componentGeneration == 0U ? 1U : packet.replacementBody.componentGeneration;
		packet.replacementBody.motionType = PhysicsMotionType::Static;
		if(!packet.replacementShape.IsValid() || !packet.replacementBody.handle.IsValid()) {
			if(packet.replacementShape.IsValid()) (void)impl->world.Release(packet.replacementShape);
			if(packet.replacementBody.handle.IsValid()) (void)impl->world.Release(packet.replacementBody.handle);
			rejected.retainedLastKnownGood = true;
			rejected.message = "Terrain collision replacement could not allocate fresh typed handles.";
			return rejected;
		}
		TerrainCollisionReplacementReceipt receipt = TerraEngine::Physics::CommitTerrainCollisionReplacement(*impl->backend, packet);
		if(!receipt.committed) {
			(void)impl->world.Release(packet.replacementShape); (void)impl->world.Release(packet.replacementBody.handle);
			return receipt;
		}
		if(prior != impl->terrainCollisions.end()) {
			// The helper already retired the provider body. Releasing only its typed
			// body handle makes stale packets fail while retaining the old shape for
			// deferred provider-safe retirement and last-known-good diagnostics.
			(void)impl->world.Release(prior->body);
			prior->shape = packet.replacementShape; prior->body = packet.replacementBody.handle;
		} else {
			impl->terrainCollisions.push_back({packet.recipe.terrainAssetId, packet.recipe.areaX, packet.recipe.areaZ, packet.replacementShape, packet.replacementBody.handle});
		}
		return receipt;
	}

	bool PhysicsSystem::RemoveTerrainCollisions(TerraEngine::UUID terrainAssetId) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(terrainAssetId == TerraEngine::EMPTY_UUID) return false;
		bool removed = false;
		for(auto collision = impl->terrainCollisions.begin(); collision != impl->terrainCollisions.end();) {
			if(collision->terrainAssetId != terrainAssetId) {
				++collision;
				continue;
			}
			// Provider destruction happens before typed-handle retirement. If no world
			// is active, StopWorld already destroyed provider bodies and cleared this
			// collection, so this branch only owns live protected state.
			if(impl->backend && collision->body.IsValid())
				(void)impl->backend->DestroyBody(collision->body);
			if(impl->backend && collision->shape.IsValid())
				(void)impl->backend->DestroyShape(collision->shape);
			if(collision->body.IsValid()) (void)impl->world.Release(collision->body);
			if(collision->shape.IsValid()) (void)impl->world.Release(collision->shape);
			collision = impl->terrainCollisions.erase(collision);
			removed = true;
		}
		return removed;
	}

	Animation::AnimationFixedTick PhysicsSystem::CurrentFixedTick() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		// Timing::tick names the most recently issued physics step. Before the first
		// step it is zero, so reserve generation one for the imminent first
		// extraction rather than leaking an invalid zero stamp to animation.
		return static_cast<Animation::AnimationFixedTick>(
			std::max<PhysicsTick>(1U, impl->world.Timing().tick));
	}

	PhysicsCommandReceipt PhysicsSystem::SubmitCommand(PhysicsCommand command) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.EnqueueCommand(std::move(command));
	}

	Animation::AnimationStatus PhysicsSystem::SubmitRagdollPhysicsRequest(
		Animation::RagdollPhysicsRequest request) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!impl->world.Handle().IsValid()) return Animation::AnimationStatus::NotReady;
		return impl->ragdollPhysicsBridge.Enqueue(std::move(request));
	}

	Animation::AnimationStatus PhysicsSystem::SubmitRagdollPhysicsRequestBatch(
		std::span<const Animation::RagdollPhysicsRequest> requests) {
		// The bridge owns copied request values. No provider object or live world
		// storage crosses this thread-safe producer boundary.
		return impl->ragdollPhysicsBridge.EnqueueBatch(requests);
	}

	std::shared_ptr<const Animation::PhysicsPoseSnapshot> PhysicsSystem::LatestRagdollPhysicsPose(
		std::uint64_t ragdollId,
		const Animation::RagdollGenerationStamp& expectedGeneration) const {
		return impl->ragdollPhysicsBridge.AcquireCurrentStepPose(ragdollId, expectedGeneration);
	}

	std::shared_ptr<const Animation::PhysicsPoseSnapshot> PhysicsSystem::LatestRagdollPhysicsPoseAtOrAfter(
		std::uint64_t ragdollId,
		const Animation::RagdollGenerationStamp& expectedIdentity) const {
		return impl->ragdollPhysicsBridge.AcquireLatestCurrentStepPose(
			ragdollId, expectedIdentity);
	}

	std::shared_ptr<const Animation::RagdollPhysicsResultSnapshot> PhysicsSystem::LatestRagdollPhysicsResults() const {
		return impl->ragdollPhysicsBridge.LatestResults();
	}

	bool PhysicsSystem::CancelCommand(PhysicsCommandId commandId) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->world.CancelCommand(commandId);
	}

	PhysicsQueryReceipt PhysicsSystem::SubmitQuery(PhysicsQueryRequest request) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(request.frame.IsValid()) {
			PhysicsWorld* frameWorld = impl->frameRegistry.FindWorldForProtectedAccess(request.frame);
			if(frameWorld == nullptr || request.world != frameWorld->Handle()) {
				return {false, {}, PhysicsQueryRejectionReason::StaleWorld,
					"The submitted query does not match a live physics frame generation."};
			}
			return frameWorld->SubmitQuery(std::move(request));
		}
		return impl->world.SubmitQuery(std::move(request));
	}

	PhysicsQueryBatchSubmission PhysicsSystem::SubmitQueryBatch(
		std::span<const PhysicsQueryRequest> requests) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		if(!requests.empty() && requests.front().frame.IsValid()) {
			const PhysicsFrameHandle frame = requests.front().frame;
			PhysicsWorld* frameWorld = impl->frameRegistry.FindWorldForProtectedAccess(frame);
			if(frameWorld == nullptr || !std::all_of(requests.begin(), requests.end(),
				[frame, frameWorld](const PhysicsQueryRequest& request) {
					return request.frame == frame && request.world == frameWorld->Handle();
				})) {
				return {false, PhysicsQueryRejectionReason::StaleWorld, {}};
			}
			return frameWorld->SubmitQueryBatch(requests);
		}
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
		PhysicsWorld* targetWorld = &impl->world;
		Backend::IPhysicsBackend* targetBackend = impl->backend.get();
		if(request.frame.IsValid()) {
			const std::optional<PhysicsFrameDescriptor> frame = impl->frameRegistry.Find(request.frame);
			const auto backend = impl->frameBackends.find(request.frame.frameId.value);
			if(!frame || backend == impl->frameBackends.end() || backend->second.frame != request.frame) {
				PhysicsQueryResult rejected;
				rejected.world = request.world;
				rejected.frame = request.frame;
				rejected.status = PhysicsResultStatus::StaleTarget;
				rejected.rejectionReason = PhysicsQueryRejectionReason::StaleWorld;
				rejected.diagnostic = "The query targets an absent or stale physics frame.";
				rejected.hits = std::make_shared<const std::vector<PhysicsQueryHit>>();
				return rejected;
			}
			targetWorld = impl->frameRegistry.FindWorldForProtectedAccess(request.frame);
			targetBackend = backend->second.backend.get();
		}
		const PhysicsWorldHandle activeWorld = targetWorld != nullptr ? targetWorld->Handle() : PhysicsWorldHandle{};
		const PhysicsTick currentTick = targetWorld != nullptr ? targetWorld->Timing().tick : 0U;
		PhysicsQueryManager immediate(targetWorld != nullptr ? targetWorld->Settings() : PhysicsSettings::Defaults());
		const PhysicsQueryReceipt receipt = immediate.Submit(request, activeWorld, currentTick);
		if(!receipt.accepted) {
			PhysicsQueryResult rejected;
			rejected.world = activeWorld;
			rejected.frame = request.frame;
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
		const Backend::PhysicsBackendQueryResult provider = targetBackend->Query(
			impl->MakeBackendQueryRequest(prepared.queries.front().request));
		std::uint64_t stale = 0U;
		std::vector<PhysicsQueryHit> hits = impl->ConvertQueryHits(provider, stale, request.frame);
		impl->diagnosticCounters.staleCount += stale;
		immediate.Complete(prepared.queries.front(), activeWorld, executionTick,
			provider.result.Succeeded() ? PhysicsResultStatus::Success : PhysicsResultStatus::ProviderFailure,
			hits, provider.truncated, provider.result.message);
		PhysicsQueryResult result = immediate.Read(receipt.handle, activeWorld, executionTick);
		result.frame = request.frame;
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
