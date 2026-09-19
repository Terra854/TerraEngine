// NavigationAgentSystem.cpp - bounded capture, worker preparation, publication.

#include "NavigationAgentSystem.h"

#include <chrono>
#include "Components/NavigationAgentComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Navigation/NavigationSystem.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Navigation {
	namespace {
		constexpr std::size_t kNavigationAgentWorkerChunkCount = 8U;
		constexpr std::array<const char*, kNavigationAgentWorkerChunkCount> kPreparedChunkResources = {
			TerraEngine::Scheduling::kResourceNavigationAgentPrepared0,
			TerraEngine::Scheduling::kResourceNavigationAgentPrepared1,
			TerraEngine::Scheduling::kResourceNavigationAgentPrepared2,
			TerraEngine::Scheduling::kResourceNavigationAgentPrepared3,
			TerraEngine::Scheduling::kResourceNavigationAgentPrepared4,
			TerraEngine::Scheduling::kResourceNavigationAgentPrepared5,
			TerraEngine::Scheduling::kResourceNavigationAgentPrepared6,
			TerraEngine::Scheduling::kResourceNavigationAgentPrepared7};

		[[nodiscard]] bool IsRouteEvidenceCurrent(
			const NavigationObservationEvidence& observation) {
			const std::shared_ptr<const NavigationWorldRoot> world =
				NavigationSystem::Instance().PublishedWorld();
			if(!world || !observation.HasWorldEvidence()) return false;
			const NavigationObservationEvidence current{0U, 0U, 0U,
				world->terrainPublicationGeneration, world->rootGeneration,
				world->sourceIndexRevision};
			return ValidateNavigationRouteObservation(observation, current).usable;
		}

		// Agents retain a completed route while it remains their active intent. A
		// route can be refreshed after an unrelated source publication, so its
		// authoritative route record is more precise than an old copied root tuple.
		[[nodiscard]] std::optional<NavigationObservationEvidence> ReadRetainedRouteObservation(
			const NavigationAgentRuntimeRecord& runtime) {
			if(!runtime.route.IsValid()) return std::nullopt;
			return NavigationSystem::Instance().ReadCurrentRouteObservation(runtime.route);
		}

		[[nodiscard]] bool IsTerminal(const NavigationRequestRecord& record) {
			return record.lifecycle == NavigationRequestLifecycle::Terminal;
		}

		[[nodiscard]] bool SameSettings(const NavigationAgentAuthoredSettings& left,
			const NavigationAgentAuthoredSettings& right) noexcept {
			return left.traversal == right.traversal
				&& left.arrivalToleranceMetres == right.arrivalToleranceMetres
				&& left.targetDisplacementThresholdMetres == right.targetDisplacementThresholdMetres
				&& left.minimumReplanIntervalSeconds == right.minimumReplanIntervalSeconds;
		}
	}

	NavigationAgentSystem& NavigationAgentSystem::Instance() {
		static NavigationAgentSystem system;
		return system;
	}

	void NavigationAgentSystem::Initialize() {
		std::lock_guard lock(mutex_);
		if(initialized_.load(std::memory_order_acquire)) return;
		store_ = std::make_unique<NavigationAgentStore>(kNavigationAgentMaximumRuntimeRecords);
		queue_ = std::make_unique<NavigationAgentCommandQueue>(kNavigationAgentMaximumCommandCount);
		bindings_.clear();
		bindingIndexes_.clear();
		bindingIndexes_.rehash(0U);
		captured_.clear();
		prepared_.clear();
		std::atomic_store_explicit(&publishedEntityBindings_,
			std::make_shared<const std::vector<NavigationAgentPublishedEntityBinding>>(),
			std::memory_order_release);
		std::atomic_store_explicit(&publishedDiagnostics_,
			std::make_shared<const NavigationAgentDiagnosticsSnapshot>(), std::memory_order_release);
		publishedObjectBindingOverflowCount_.store(0U, std::memory_order_release);
		bindingSnapshotGeneration_ = 1U;
		publishedBindingSnapshotGeneration_ = 0U;
		publishedObjectMembershipRevision_ = 0U;
		hasPublishedEntityBindingSnapshot_ = false;
		elapsedSeconds_ = 0.0;
		lastBoundaryGeneration_ = 0U;
		publicationGeneration_ = 0U;
		diagnosticsRevision_ = 0U;
		retryCount_ = 0U;
		rejectedCommandCount_ = 0U;
		lastProtectedPublishMicroseconds_ = 0U;
		// A recreated agent runtime must not retain a declaration hint from a prior
		// engine lifetime.
		NavigationSystem::Instance().SetPendingAgentRouteAdmissionDemand(0U);
		initialized_.store(true, std::memory_order_release);
	}

	void NavigationAgentSystem::Update(float deltaTime) {
		if(!initialized_.load(std::memory_order_acquire)) return;
		elapsedSeconds_ += std::max(0.0F, deltaTime);
		// Direct-update fixtures do not register NavigationAgentCommandSystem. Keep
		// their documented compatibility path identical to the scheduler's early
		// command boundary, while the second call from CaptureProtected is a no-op.
		CaptureCommandAdmission();
		CaptureProtected();
		PrepareCapturedWork();
		PublishPreparedWork();
	}

	void NavigationAgentSystem::Shutdown() {
		RevokeAllAgents(NavigationReason::SceneReplaced);
		std::lock_guard lock(mutex_);
		if(queue_) queue_->Clear();
		NavigationSystem::Instance().SetPendingAgentRouteAdmissionDemand(0U);
		queue_.reset();
		store_.reset();
		captured_.clear();
		prepared_.clear();
		bindingIndexes_.clear();
		// Shutdown owns the final map lifetime, so release empty buckets rather than
		// reporting a zero-agent runtime that still retains prior lookup storage.
		bindingIndexes_.rehash(0U);
		std::atomic_store_explicit(&publishedEntityBindings_,
			std::make_shared<const std::vector<NavigationAgentPublishedEntityBinding>>(),
			std::memory_order_release);
		std::atomic_store_explicit(&publishedDiagnostics_,
			std::make_shared<const NavigationAgentDiagnosticsSnapshot>(), std::memory_order_release);
		publishedObjectBindingOverflowCount_.store(0U, std::memory_order_release);
		hasPublishedEntityBindingSnapshot_ = false;
		contractTestWorkAvailable_.store(false, std::memory_order_release);
		initialized_.store(false, std::memory_order_release);
	}

	std::string NavigationAgentSystem::SystemName() {
		return "NavigationAgentSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration NavigationAgentSystem::SchedulerDeclaration() {
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName();
		declaration.phase = "Navigation";
		declaration.after = {"NavigationSystem"};
		declaration.before = {"ScriptingSystem"};
		declaration.reads = {kResourceNavigationAgentCommands, kResourceNavigationAgentCapture,
			kResourceNavigationAgentPrepared, kResourceNavigationResults,
			kResourceNavigationRouteValidity};
		declaration.writes = {kResourceNavigationAgentState, kResourceNavigationAgentGuidance};
		declaration.mainThread = false;
		declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Prepare copied agent progress and guidance candidates without ECS or provider access.";
		declaration.protectedCommit = "Capture attachments and publish generation-qualified guidance at protected boundaries.";
		declaration.protectedReason = "Agent attachment, intent, route, and world evidence must match atomically.";
		declaration.profilingLabel = "NavigationAgents";
		declaration.omitWhenNoFrameWorkloads = true;
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration>
	NavigationAgentSystem::DeclareFrameWorkloads(float deltaTime) {
		using namespace TerraEngine::Scheduling;
		// Capture must run even before a runtime record exists: an authored component
		// can be attached since the previous frame and only this protected scan binds it.
		if(!initialized_.load(std::memory_order_acquire)) return {};
		elapsedSeconds_ += std::max(0.0F, deltaTime);
		auto capture = MakeProtectedCommitPhase("NavigationAgentCapture", SystemName(),
			{kResourceNavigationAgentCommands, kResourceNavigationAgentState, kResourceNavigationResults,
				kResourceNavigationRouteValidity},
			{kResourceNavigationAgentCapture, kResourceNavigationAgentState},
			"Object and component capture is restricted to the Navigation protected boundary.",
			"NavigationAgentCapture");
		capture.phase = "Navigation";
		capture.stableDescriptorId = "NavigationAgentSystem/Navigation/Capture";
		capture.work = [this](JobGraphContext&) { CaptureProtected(); };

		std::vector<FrameWorkloadDeclaration> workloads;
		workloads.push_back(std::move(capture));
		for(std::size_t chunk = 0U; chunk < kNavigationAgentWorkerChunkCount; ++chunk) {
			auto prepare = MakeWorkerPreparePhase("NavigationAgentPrepare", SystemName(),
				{kResourceNavigationAgentCapture}, {kPreparedChunkResources[chunk]}, "NavigationAgentPrepare");
			prepare.phase = "Navigation";
			prepare.stableDescriptorId = chunk == 0U
				? "NavigationAgentSystem/Navigation/Prepare"
				: "NavigationAgentSystem/Navigation/Prepare/" + std::to_string(chunk);
			prepare.explicitDependencies = {"NavigationAgentSystem/Navigation/Capture"};
			const std::size_t capturedCount = captured_.size();
			prepare.estimatedWorkItemCount = std::max<std::size_t>(1U,
				((capturedCount * (chunk + 1U)) / kNavigationAgentWorkerChunkCount)
					- ((capturedCount * chunk) / kNavigationAgentWorkerChunkCount));
			prepare.work = [this, chunk](JobGraphContext&) {
				const std::size_t count = captured_.size();
				const std::size_t begin = (count * chunk) / kNavigationAgentWorkerChunkCount;
				const std::size_t end = (count * (chunk + 1U)) / kNavigationAgentWorkerChunkCount;
				PrepareCapturedRange(begin, end);
			};
			workloads.push_back(std::move(prepare));
		}
		std::vector<std::string> publishReads{kResourceNavigationAgentPrepared, kResourceNavigationAgentCapture,
			kResourceNavigationResults, kResourceNavigationRouteValidity};
		for(const char* resource : kPreparedChunkResources) publishReads.emplace_back(resource);
		auto publish = MakeProtectedCommitPhase("NavigationAgentPublish", SystemName(),
			std::move(publishReads),
			{kResourceNavigationAgentState, kResourceNavigationAgentGuidance},
			"Guidance becomes visible only after attachment, route, and world revalidation.",
			"NavigationAgentPublish");
		publish.phase = "Navigation";
		// Candidate publication is the protected visibility edge for gameplay.
		publish.hardBoundary = true;
		publish.stableDescriptorId = "NavigationAgentSystem/Navigation/Publish";
		for(std::size_t chunk = 0U; chunk < kNavigationAgentWorkerChunkCount; ++chunk)
			publish.explicitDependencies.push_back(chunk == 0U
				? "NavigationAgentSystem/Navigation/Prepare"
				: "NavigationAgentSystem/Navigation/Prepare/" + std::to_string(chunk));
		publish.work = [this](JobGraphContext&) { PublishPreparedWork(); };
		workloads.push_back(std::move(publish));
		return workloads;
	}

	void NavigationAgentSystem::RebuildBindingIndexLocked() {
		// Erasing from a vector shifts later slots. Rebuild from that deterministic
		// source after a structural mutation instead of trying to patch every shifted
		// index individually, which keeps the sidecar simple and fail-closed.
		bindingIndexes_.clear();
		bindingIndexes_.reserve(bindings_.size());
		for(std::size_t index = 0U; index < bindings_.size(); ++index) {
			const Binding& binding = bindings_[index];
			if(binding.objectId == EMPTY_UUID) continue;
			bindingIndexes_.insert_or_assign(binding.objectId, index);
		}
	}

	NavigationAgentSystem::Binding* NavigationAgentSystem::FindBindingForObject(UUID objectId) {
		const auto found = bindingIndexes_.find(objectId);
		if(found == bindingIndexes_.end() || found->second >= bindings_.size()) return nullptr;
		Binding& binding = bindings_[found->second];
		return binding.objectId == objectId ? &binding : nullptr;
	}

	const NavigationAgentSystem::Binding* NavigationAgentSystem::FindBindingForObject(UUID objectId) const {
		const auto found = bindingIndexes_.find(objectId);
		if(found == bindingIndexes_.end() || found->second >= bindings_.size()) return nullptr;
		const Binding& binding = bindings_[found->second];
		return binding.objectId == objectId ? &binding : nullptr;
	}

	NavigationAgentSystem::Binding* NavigationAgentSystem::FindBinding(const NavigationAgentIdentity& identity) {
		Binding* binding = FindBindingForObject(identity.object.objectId);
		// UUID lookup is only an acceleration. A reused object UUID must never allow
		// a stale lifetime, attachment generation, world, or runtime slot to mutate a
		// replacement record.
		return binding != nullptr && binding->identity == identity ? binding : nullptr;
	}

	const NavigationAgentSystem::Binding* NavigationAgentSystem::FindBinding(const NavigationAgentIdentity& identity) const {
		const Binding* binding = FindBindingForObject(identity.object.objectId);
		return binding != nullptr && binding->identity == identity ? binding : nullptr;
	}

	bool NavigationAgentSystem::IsCurrentAttachment(ObjectManager& objects,
		const NavigationAgentRuntimeRecord& record) const {
		const auto object = objects.getObject(record.identity.object.objectId);
		if(!object || objects.getObjectLifetimeGeneration(*object) != record.identity.object.lifetimeGeneration)
			return false;
		const auto* component = objects.getRegistry().try_get<NavigationAgentComponent>(object->EntityId());
		return component != nullptr && component->active && component->ValidateAuthoredValues()
			&& objects.isObjectActiveInHierarchy(*object)
			&& objects.getObjectComponentGeneration(*object) == record.sourceComponentGeneration;
	}

	void NavigationAgentSystem::ReleaseRuntimeRecord(const NavigationAgentRuntimeRecord& record) {
		if(!record.request.IsValid()) return;
		NavigationSystem& navigation = NavigationSystem::Instance();
		(void)navigation.CancelRequest(record.request);
		(void)navigation.ReleaseRequestResult(record.request);
	}

	void NavigationAgentSystem::ReconcileBindings(ObjectManager& objects, const NavigationWorldHandle& world) {
		if(!store_ || !world.IsValid()) return;
		bool bindingsChanged = false;
		for(auto iterator = bindings_.begin(); iterator != bindings_.end();) {
			const auto record = store_->Read(iterator->identity);
			if(!record.record || record.record->identity.world != world || !IsCurrentAttachment(objects, *record.record)) {
				if(record.record) ReleaseRuntimeRecord(*record.record);
				(void)store_->Release(iterator->identity);
				iterator = bindings_.erase(iterator);
				bindingsChanged = true;
			} else ++iterator;
		}
		if(bindingsChanged) RebuildBindingIndexLocked();
		for(const entt::entity entity : objects.getRegistry().view<NavigationAgentComponent>()) {
			const auto object = objects.getObject(entity);
			if(!object) continue;
			const auto* component = objects.getRegistry().try_get<NavigationAgentComponent>(entity);
			if(!component || !component->active || !component->ValidateAuthoredValues()
				|| !objects.isObjectActiveInHierarchy(*object)) continue;
			const UUID objectId = objects.getUUID(*object);
			const NavigationGeneration componentGeneration = objects.getObjectComponentGeneration(*object);
			const NavigationGeneration componentValueRevision = objects.GetComponentValueMutationRevision(
				NavigationAgentComponent::kComponentName);
			Binding* binding = FindBindingForObject(objectId);
			if(binding != nullptr && binding->componentGeneration == componentGeneration
				&& binding->identity.world == world) {
				const auto old = store_->Read(binding->identity);
				const NavigationAgentAuthoredSettings settings = component->ToAuthoredSettings();
				if(old.record && !SameSettings(old.record->settings, settings)) {
					ReleaseRuntimeRecord(*old.record);
					const NavigationGeneration nextPublication = std::max<NavigationGeneration>(1U, publicationGeneration_ + 1U);
					(void)store_->Update(binding->identity, [&settings, nextPublication](NavigationAgentRuntimeRecord& runtime) {
							runtime.settings = settings;
							++runtime.configurationRevision;
							if(runtime.configurationRevision == 0U) runtime.configurationRevision = 1U;
							runtime.request = {};
							runtime.route = {};
							runtime.guidance = {};
							runtime.publicationRevision = nextPublication;
							if(runtime.intentSequence != 0U && runtime.state != NavigationAgentState::Stopped)
								runtime.state = NavigationAgentState::Requesting;
						});
					binding->componentValueRevision = componentValueRevision;
					binding->routePoints.clear();
					binding->routeObservation = {};
					binding->routeGeneration = 0U;
					binding->waypointIndex = 0U;
				}
				continue;
			}
			NavigationGeneration nextConfigurationRevision = 1U;
			if(binding != nullptr) {
				const auto old = store_->Read(binding->identity);
				if(old.record) {
					nextConfigurationRevision = old.record->configurationRevision + 1U;
					if(nextConfigurationRevision == 0U) nextConfigurationRevision = 1U;
					ReleaseRuntimeRecord(*old.record);
				}
				(void)store_->Release(binding->identity);
			}
			NavigationAgentIdentity identity{};
			identity.world = world;
			identity.object = {objectId, objects.getObjectLifetimeGeneration(*object)};
			identity.attachmentGeneration = componentGeneration;
			NavigationAgentRuntimeRecord record{};
			record.settings = component->ToAuthoredSettings();
			record.configurationRevision = nextConfigurationRevision;
			record.sourceComponentGeneration = componentGeneration;
			const auto created = store_->Create(identity, std::move(record));
			if(created.Succeeded()) {
				const Binding replacement{objectId, created.identity, componentGeneration, componentValueRevision};
				if(binding != nullptr) {
					// Same UUID replacement preserves deterministic vector order and avoids
					// shifting unrelated agents' sidecar slots.
					*binding = replacement;
				} else {
					bindings_.push_back(replacement);
					bindingIndexes_.insert_or_assign(objectId, bindings_.size() - 1U);
				}
				bindingsChanged = true;
			} else if(binding != nullptr) {
				// The old store record was intentionally released before attempting a
				// replacement. Remove its dead vector entry if allocation fails.
				const std::size_t index = static_cast<std::size_t>(binding - bindings_.data());
				bindings_.erase(bindings_.begin() + static_cast<std::ptrdiff_t>(index));
				RebuildBindingIndexLocked();
				bindingsChanged = true;
			}
		}
		if(bindingsChanged) {
			++bindingSnapshotGeneration_;
			if(bindingSnapshotGeneration_ == 0U) bindingSnapshotGeneration_ = 1U;
		}
	}

	void NavigationAgentSystem::RebuildPublishedEntityBindings(ObjectManager& objects) {
		const std::uint64_t membershipRevision = objects.GetRuntimeActiveMembershipRevision();
		if(hasPublishedEntityBindingSnapshot_
			&& publishedBindingSnapshotGeneration_ == bindingSnapshotGeneration_
			&& publishedObjectMembershipRevision_ == membershipRevision) return;

		// Agent owners are always published first. A large unrelated Transform view
		// must never make a live agent unaddressable merely because its entity sorts
		// after the bounded target prefix.
		constexpr std::size_t maximumBindings = kNavigationAgentMaximumRuntimeRecords * 4U;
		std::vector<NavigationAgentPublishedEntityBinding> next;
		next.reserve(maximumBindings);
		std::unordered_set<std::uint32_t> publishedEntities;
		publishedEntities.reserve(maximumBindings);
		for(const Binding& agentBinding : bindings_) {
			const auto object = objects.getObject(agentBinding.objectId);
			if(!object) continue;
			const std::uint32_t entity = static_cast<std::uint32_t>(object->EntityId());
			if(!publishedEntities.insert(entity).second) continue;
			next.push_back({entity, {agentBinding.objectId,
				objects.getObjectLifetimeGeneration(*object)}, agentBinding.identity});
		}

		std::uint32_t omittedTargetCount = 0U;
		for(const entt::entity entity : objects.getRegistry().view<Transform>()) {
			const auto object = objects.getObject(entity);
			if(!object) continue;
			const std::uint32_t entityValue = static_cast<std::uint32_t>(entity);
			if(publishedEntities.contains(entityValue)) continue;
			if(next.size() >= maximumBindings) {
				// Count every omitted eligible target so the ABI can distinguish a
				// bounded snapshot from an actually stale entity.
				if(omittedTargetCount != std::numeric_limits<std::uint32_t>::max()) ++omittedTargetCount;
				continue;
			}
			NavigationAgentPublishedEntityBinding binding{};
			binding.entity = entityValue;
			binding.object = {objects.getUUID(*object), objects.getObjectLifetimeGeneration(*object)};
			next.push_back(binding);
		}
		std::sort(next.begin(), next.end());
		std::atomic_store_explicit(&publishedEntityBindings_,
			std::make_shared<const std::vector<NavigationAgentPublishedEntityBinding>>(std::move(next)),
			std::memory_order_release);
		publishedObjectBindingOverflowCount_.store(omittedTargetCount, std::memory_order_release);
		publishedBindingSnapshotGeneration_ = bindingSnapshotGeneration_;
		publishedObjectMembershipRevision_ = membershipRevision;
		hasPublishedEntityBindingSnapshot_ = true;
	}

	NavigationAgentIdentity NavigationAgentSystem::FindIdentity(ObjectManager& objects, UUID objectId) {
		const auto world = NavigationSystem::Instance().PublishedWorld();
		if(!world || !world->world.IsValid()) return {};
		std::lock_guard lock(mutex_);
		ReconcileBindings(objects, world->world);
		RebuildPublishedEntityBindings(objects);
		const Binding* binding = FindBindingForObject(objectId);
		return binding == nullptr ? NavigationAgentIdentity{} : binding->identity;
	}

	std::optional<NavigationAgentIdentity> NavigationAgentSystem::ResolvePublishedAgent(
		std::uint32_t entity) const noexcept {
		const auto snapshot = std::atomic_load_explicit(&publishedEntityBindings_, std::memory_order_acquire);
		if(!snapshot) return std::nullopt;
		const auto found = std::lower_bound(snapshot->begin(), snapshot->end(), entity,
			[](const NavigationAgentPublishedEntityBinding& binding, std::uint32_t value) {
				return binding.entity < value;
			});
		return found != snapshot->end() && found->entity == entity && found->agent.IsValid()
			? std::optional<NavigationAgentIdentity>{found->agent} : std::nullopt;
	}

	std::optional<NavigationAgentIdentity> NavigationAgentSystem::ResolvePublishedAgent(
		const UUID& objectId) const noexcept {
		const auto snapshot = std::atomic_load_explicit(&publishedEntityBindings_, std::memory_order_acquire);
		if(!snapshot) return std::nullopt;

		// Entity order supports the hot entity lookup above. UUID resolution is an
		// infrequent editor/MCP operation, so scanning this bounded snapshot keeps
		// the published representation simple and avoids a second mutable index.
		const auto found = std::find_if(snapshot->begin(), snapshot->end(), [&objectId](const auto& binding) {
			return binding.object.objectId == objectId && binding.agent.IsValid();
		});
		return found != snapshot->end() ? std::optional<NavigationAgentIdentity>{found->agent} : std::nullopt;
	}

	std::optional<NavigationObjectIdentity> NavigationAgentSystem::ResolvePublishedObject(
		std::uint32_t entity) const noexcept {
		const auto snapshot = std::atomic_load_explicit(&publishedEntityBindings_, std::memory_order_acquire);
		if(!snapshot) return std::nullopt;
		const auto found = std::lower_bound(snapshot->begin(), snapshot->end(), entity,
			[](const NavigationAgentPublishedEntityBinding& binding, std::uint32_t value) {
				return binding.entity < value;
			});
		return found != snapshot->end() && found->entity == entity && found->object.IsValid()
			? std::optional<NavigationObjectIdentity>{found->object} : std::nullopt;
	}

	std::uint32_t NavigationAgentSystem::PublishedObjectBindingOverflowCount() const noexcept {
		return publishedObjectBindingOverflowCount_.load(std::memory_order_acquire);
	}

	bool NavigationAgentSystem::HasLiveIdentity(NavigationAgentIdentity identity) const noexcept {
		std::lock_guard lock(mutex_);
		return store_ != nullptr && store_->Read(identity).record.has_value();
	}

	NavigationAgentReceipt NavigationAgentSystem::QueueLocationDestination(NavigationAgentIdentity identity,
		NavigationLocation destination, NavigationGeneration configuration, NavigationGeneration intent,
		std::uint64_t producer, std::uint64_t correlation) {
		NavigationAgentCommand command{};
		command.identity = identity;
		command.kind = NavigationAgentCommandKind::SetLocationDestination;
		command.destination.kind = NavigationDestinationKind::Location;
		command.destination.location = std::move(destination);
		command.expectedConfigurationRevision = configuration;
		command.expectedIntentSequence = intent;
		command.producerOrderingToken = producer;
		command.correlationId = correlation;
		std::lock_guard lock(mutex_);
		const NavigationAgentReceipt receipt = queue_ ? queue_->Enqueue(std::move(command)) : NavigationAgentReceipt{};
		if(receipt.state == NavigationAgentReceiptState::Rejected) ++rejectedCommandCount_;
		RefreshPendingRouteAdmissionDemandLocked();
		return receipt;
	}

	NavigationAgentReceipt NavigationAgentSystem::QueueObjectDestination(NavigationAgentIdentity identity,
		NavigationObjectIdentity destination, NavigationGeneration configuration, NavigationGeneration intent,
		std::uint64_t producer, std::uint64_t correlation) {
		NavigationAgentCommand command{};
		command.identity = identity;
		command.kind = NavigationAgentCommandKind::SetObjectDestination;
		command.destination.kind = NavigationDestinationKind::Object;
		command.destination.object = destination;
		command.expectedConfigurationRevision = configuration;
		command.expectedIntentSequence = intent;
		command.producerOrderingToken = producer;
		command.correlationId = correlation;
		std::lock_guard lock(mutex_);
		const NavigationAgentReceipt receipt = queue_ ? queue_->Enqueue(std::move(command)) : NavigationAgentReceipt{};
		if(receipt.state == NavigationAgentReceiptState::Rejected) ++rejectedCommandCount_;
		RefreshPendingRouteAdmissionDemandLocked();
		return receipt;
	}

	NavigationAgentReceipt NavigationAgentSystem::QueueStop(NavigationAgentIdentity identity,
		NavigationGeneration configuration, NavigationGeneration intent,
		std::uint64_t producer, std::uint64_t correlation) {
		NavigationAgentCommand command{};
		command.identity = identity;
		command.kind = NavigationAgentCommandKind::Stop;
		command.expectedConfigurationRevision = configuration;
		command.expectedIntentSequence = intent;
		command.producerOrderingToken = producer;
		command.correlationId = correlation;
		std::lock_guard lock(mutex_);
		const NavigationAgentReceipt receipt = queue_ ? queue_->Enqueue(std::move(command)) : NavigationAgentReceipt{};
		if(receipt.state == NavigationAgentReceiptState::Rejected) ++rejectedCommandCount_;
		RefreshPendingRouteAdmissionDemandLocked();
		return receipt;
	}

	std::optional<NavigationAgentReceipt> NavigationAgentSystem::ReadReceipt(
		NavigationAgentIdentity identity, std::uint64_t correlation) const {
		std::lock_guard lock(mutex_);
		return queue_ ? queue_->ReadReceipt(identity, correlation) : std::nullopt;
	}

	NavigationAgentStatusSnapshot NavigationAgentSystem::ReadStatus(NavigationAgentIdentity identity) const {
		std::lock_guard lock(mutex_);
		const auto record = store_ ? store_->Read(identity) : NavigationAgentStoreReadResult{};
		return record.record ? NavigationAgentStatusSnapshot{record.record->state, record.record->outcome,
			record.record->reason, record.record->configurationRevision, record.record->intentSequence,
			record.record->publicationRevision}
			: NavigationAgentStatusSnapshot{NavigationAgentState::Failed, NavigationOutcome::Stale,
				NavigationReason::SceneReplaced, 0U, 0U, 0U};
	}

	NavigationAgentGuidanceSnapshot NavigationAgentSystem::ReadGuidance(NavigationAgentIdentity identity) const {
		std::lock_guard lock(mutex_);
		const auto record = store_ ? store_->Read(identity) : NavigationAgentStoreReadResult{};
		if(!record.record) {
			// An identity can outlive an attachment in a managed wrapper. Return an
			// explicit stale terminal snapshot rather than a misleading idle default.
			NavigationAgentGuidanceSnapshot stale{};
			stale.identity = identity;
			stale.state = NavigationAgentState::Failed;
			stale.outcome = NavigationOutcome::Stale;
			stale.reason = NavigationReason::SceneReplaced;
			return stale;
		}

		const NavigationAgentRuntimeRecord& runtime = *record.record;
		NavigationAgentGuidanceSnapshot snapshot = runtime.guidance;
		// Guidance and status must describe one protected publication. Fill these
		// fields from the runtime record even when movement advice is unavailable.
		snapshot.identity = runtime.identity;
		snapshot.configurationRevision = runtime.configurationRevision;
		snapshot.intentSequence = runtime.intentSequence;
		snapshot.publicationGeneration = runtime.publicationRevision;
		snapshot.state = runtime.state;
		snapshot.outcome = runtime.outcome;
		snapshot.reason = runtime.reason;
		snapshot.destination = runtime.destination;

		const auto world = NavigationSystem::Instance().PublishedWorld();
		const std::optional<NavigationObservationEvidence> retainedObservation =
			ReadRetainedRouteObservation(runtime);
		if(retainedObservation) snapshot.observation = *retainedObservation;
		const bool usable = snapshot.IsUsable()
			&& (retainedObservation || (!runtime.route.IsValid() && IsRouteEvidenceCurrent(snapshot.observation)))
			&& world && snapshot.nextWaypoint.world == world->world;
		if(usable) return snapshot;

		// A neutral result remains diagnostic but cannot be consumed as movement.
		snapshot.routeGeneration = 0U;
		snapshot.observation = {};
		snapshot.nextWaypoint = {};
		snapshot.suggestedDirection = glm::dvec3{0.0};
		snapshot.remainingDistanceMetres = 0.0;
		snapshot.usable = false;
		return snapshot;
	}

	std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> NavigationAgentSystem::PublishedDiagnostics() const noexcept {
		return std::atomic_load_explicit(&publishedDiagnostics_, std::memory_order_acquire);
	}

	std::optional<NavigationAgentSelectedDiagnostic> NavigationAgentSystem::ReadPublishedSelectedDiagnostic(
		NavigationAgentIdentity identity, NavigationGeneration diagnosticsRevision) const {
		const std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> diagnostics = PublishedDiagnostics();
		if(!diagnostics || diagnostics->revision != diagnosticsRevision) return std::nullopt;
		const auto summary = std::lower_bound(diagnostics->agents.begin(), diagnostics->agents.end(), identity.runtimeSlot,
			[](const NavigationAgentDiagnosticRecord& record, NavigationSlotIndex slot) {
				return record.identity.runtimeSlot < slot;
			});
		if(summary == diagnostics->agents.end() || summary->identity != identity) return std::nullopt;

		NavigationAgentSelectedDiagnostic selected{};
		selected.agent = *summary;
		// The route vector is copied only for the current editor selection. The
		// immutable summary above establishes state/reason/revisions first; this
		// lock merely obtains the matching bounded route owned by that publication.
		std::lock_guard lock(mutex_);
		// A new normal/boundary publication invalidates the summary just selected.
		// Returning no record makes the editor retry against one coherent revision.
		const auto latest = std::atomic_load_explicit(&publishedDiagnostics_, std::memory_order_acquire);
		if(!latest || latest->revision != diagnosticsRevision || !store_) return std::nullopt;
		const auto runtime = store_->Read(identity);
		if(!runtime.record
			|| runtime.record->configurationRevision != selected.agent.guidance.configurationRevision
			|| runtime.record->intentSequence != selected.agent.guidance.intentSequence
			|| runtime.record->publicationRevision != selected.agent.guidance.publicationGeneration)
			return std::nullopt;
		const Binding* binding = FindBinding(identity);
		const std::optional<NavigationObservationEvidence> retainedObservation =
			ReadRetainedRouteObservation(*runtime.record);
		if(binding && summary->guidance.IsUsable()
			&& binding->routeGeneration == summary->guidance.routeGeneration
			&& !binding->routePoints.empty()
			&& (retainedObservation || (!runtime.record->route.IsValid()
				&& IsRouteEvidenceCurrent(binding->routeObservation)))) {
			selected.routePoints = binding->routePoints;
			if(retainedObservation) selected.agent.guidance.observation = *retainedObservation;
			selected.routeCurrent = true;
		}
		if(!selected.routeCurrent && selected.agent.guidance.usable) {
			// A diagnostic may remain useful after its route was invalidated, but it
			// must never leave a caller with movement-ready advice from old evidence.
			selected.agent.guidance.routeGeneration = 0U;
			selected.agent.guidance.observation = {};
			selected.agent.guidance.nextWaypoint = {};
			selected.agent.guidance.suggestedDirection = glm::dvec3{0.0};
			selected.agent.guidance.remainingDistanceMetres = 0.0;
			selected.agent.guidance.usable = false;
		}
		return selected;
	}

	void NavigationAgentSystem::PublishDiagnosticsLocked() {
		auto next = std::make_shared<NavigationAgentDiagnosticsSnapshot>();
		++diagnosticsRevision_;
		if(diagnosticsRevision_ == 0U) diagnosticsRevision_ = 1U;
		next->revision = diagnosticsRevision_;
		next->retryCount = retryCount_;
		next->rejectedCommandCount = rejectedCommandCount_;
		next->lastProtectedPublishMicroseconds = lastProtectedPublishMicroseconds_;
		if(store_) {
			const std::vector<NavigationAgentRuntimeRecord> records = store_->Snapshot();
			next->activeAgents = records.size();
			next->agents.reserve(records.size());
			for(const NavigationAgentRuntimeRecord& runtime : records) {
				NavigationAgentGuidanceSnapshot guidance = runtime.guidance;
				// Neutral guidance still carries state and destination diagnostics. This
				// mirrors ReadGuidance without exposing a mutable runtime record.
				guidance.identity = runtime.identity;
				guidance.configurationRevision = runtime.configurationRevision;
				guidance.intentSequence = runtime.intentSequence;
				guidance.publicationGeneration = runtime.publicationRevision;
				guidance.state = runtime.state;
				guidance.outcome = runtime.outcome;
				guidance.reason = runtime.reason;
				guidance.destination = runtime.destination;
				next->agents.push_back({runtime.identity, std::move(guidance)});
				switch(runtime.state) {
				case NavigationAgentState::Requesting: ++next->requestingAgents; break;
				case NavigationAgentState::Following: ++next->followingAgents; break;
				case NavigationAgentState::Arrived: ++next->arrivedAgents; break;
				case NavigationAgentState::Stopped: ++next->stoppedAgents; break;
				case NavigationAgentState::Failed: ++next->failedAgents; break;
				case NavigationAgentState::Idle: break;
				}
			}
			std::sort(next->agents.begin(), next->agents.end(), [](const auto& left, const auto& right) {
				return left.identity.runtimeSlot < right.identity.runtimeSlot;
			});
			next->currentOwnedMemoryBytes += store_->MemoryBytes();
		}
		if(queue_) {
			next->pendingCommands = queue_->PendingCount();
			next->commandCapacity = queue_->Capacity();
			next->retainedReceipts = queue_->ReceiptCount();
			next->currentOwnedMemoryBytes += queue_->MemoryBytes();
			next->coalescedCommandCount = queue_->CoalescedCount();
		}
		// The aggregate is honest about all system-owned allocation, including
		// routes and worker packets that do not live in the bounded record store.
		next->currentOwnedMemoryBytes += bindings_.capacity() * sizeof(Binding);
		next->currentOwnedMemoryBytes += bindingIndexes_.size()
			* (sizeof(UUID) + sizeof(std::size_t))
			+ bindingIndexes_.bucket_count() * sizeof(void*);
		for(const Binding& binding : bindings_)
			next->currentOwnedMemoryBytes += binding.routePoints.capacity() * sizeof(NavigationLocation);
		next->currentOwnedMemoryBytes += captured_.capacity() * sizeof(NavigationAgentCapture);
		for(const NavigationAgentCapture& capture : captured_)
			next->currentOwnedMemoryBytes += capture.routePoints.capacity() * sizeof(NavigationLocation);
		next->currentOwnedMemoryBytes += prepared_.capacity() * sizeof(NavigationAgentCandidate);
		if(const auto entityBindings = std::atomic_load_explicit(&publishedEntityBindings_, std::memory_order_acquire))
			next->currentOwnedMemoryBytes += entityBindings->capacity() * sizeof(NavigationAgentPublishedEntityBinding);
		next->currentOwnedMemoryBytes += sizeof(NavigationAgentDiagnosticsSnapshot)
			+ next->agents.capacity() * sizeof(NavigationAgentDiagnosticRecord);
		std::atomic_store_explicit(&publishedDiagnostics_,
			std::shared_ptr<const NavigationAgentDiagnosticsSnapshot>(std::move(next)),
			std::memory_order_release);
	}

	void NavigationAgentSystem::RefreshPendingRouteAdmissionDemandLocked() const {
		// The queue owns command coalescing and Stop semantics. Recompute after every
		// protected drain instead of decrementing a separate counter that could drift
		// across supersession, owner revocation, or receipt rejection.
		NavigationSystem::Instance().SetPendingAgentRouteAdmissionDemand(
			queue_ ? queue_->PendingRouteDemandCount() : 0U);
	}

	void NavigationAgentSystem::DrainQueuedCommandsLocked(ObjectManager& objects, const NavigationWorldHandle& world) {
		const std::vector<NavigationAgentCommand> commands = queue_->Drain();
		for(const NavigationAgentCommand& command : commands) {
			NavigationAgentReceipt receipt{command.identity, NavigationAgentReceiptState::Rejected,
				NavigationOutcome::InvalidRequest, NavigationReason::None, command.correlationId, 0U};
			const auto existing = store_->Read(command.identity);
			if(!existing.record || !IsCurrentAttachment(objects, *existing.record)
				|| (command.expectedConfigurationRevision != 0U
					&& command.expectedConfigurationRevision != existing.record->configurationRevision)
				|| (command.expectedIntentSequence != 0U
					&& command.expectedIntentSequence != existing.record->intentSequence)) {
				receipt.outcome = NavigationOutcome::Stale;
				receipt.reason = NavigationReason::SceneReplaced;
				queue_->CompleteReceipt(receipt);
				continue;
			}
			if(command.kind == NavigationAgentCommandKind::SetObjectDestination) {
				const auto target = objects.getObject(command.destination.object.objectId);
				if(!target || objects.getObjectLifetimeGeneration(*target) != command.destination.object.lifetimeGeneration
					|| !objects.isObjectActiveInHierarchy(*target)) {
					receipt.outcome = NavigationOutcome::InvalidEndpoint;
					receipt.reason = NavigationReason::InvalidDestination;
					queue_->CompleteReceipt(receipt);
					continue;
				}
			}
			if(command.kind == NavigationAgentCommandKind::SetLocationDestination
				&& (command.destination.location.world != command.identity.world
					|| command.destination.location.world != world)) {
				receipt.outcome = NavigationOutcome::InvalidEndpoint;
				receipt.reason = NavigationReason::InvalidDestination;
				queue_->CompleteReceipt(receipt);
				continue;
			}
			ReleaseRuntimeRecord(*existing.record);
			if(Binding* binding = FindBinding(command.identity)) {
				binding->routePoints.clear();
				binding->routeObservation = {};
				binding->routeGeneration = 0U;
				binding->waypointIndex = 0U;
			}
			const bool applied = store_->Update(command.identity, [&command](NavigationAgentRuntimeRecord& runtime) {
				++runtime.intentSequence;
				runtime.request = {};
				runtime.route = {};
				runtime.guidance = {};
				runtime.outcome = NavigationOutcome::Queued;
				runtime.reason = NavigationReason::None;
				if(command.kind == NavigationAgentCommandKind::Stop) {
					runtime.destination = {};
					runtime.state = NavigationAgentState::Stopped;
				} else {
					runtime.destination = command.destination;
					runtime.state = NavigationAgentState::Requesting;
				}
			});
			if(applied) {
				if(command.kind != NavigationAgentCommandKind::Stop) {
					const auto current = store_->Read(command.identity);
					if(current.record) {
						NavigationLocation destination = current.record->destination.location;
						if(command.kind == NavigationAgentCommandKind::SetObjectDestination) {
							const auto target = objects.getObject(current.record->destination.object.objectId);
							const auto pose = target ? objects.GetCanonicalWorldPose(*target) : std::nullopt;
							if(!target || !pose
								|| objects.getObjectLifetimeGeneration(*target)
									!= current.record->destination.object.lifetimeGeneration) {
								(void)store_->Update(command.identity, [](NavigationAgentRuntimeRecord& runtime) {
									runtime.state = NavigationAgentState::Failed;
									runtime.outcome = NavigationOutcome::InvalidEndpoint;
									runtime.reason = NavigationReason::InvalidDestination;
								});
							} else destination = {world, pose->position};
						}
						const auto refreshed = store_->Read(command.identity);
						if(refreshed.record && refreshed.record->state != NavigationAgentState::Failed) {
							const auto routeReceipt = NavigationSystem::Instance().RequestAgentRoute(
								objects, refreshed.record->identity.object, destination,
								refreshed.record->settings.traversal);
							(void)store_->Update(command.identity, [&routeReceipt](NavigationAgentRuntimeRecord& runtime) {
								if(routeReceipt.Succeeded()) {
									runtime.request = routeReceipt.request;
									runtime.state = NavigationAgentState::Requesting;
								} else {
									runtime.outcome = routeReceipt.outcome;
									runtime.reason = routeReceipt.reason;
									const bool retryable = routeReceipt.outcome == NavigationOutcome::UnavailableCoverage
										|| routeReceipt.outcome == NavigationOutcome::RejectedCapacity
										|| routeReceipt.outcome == NavigationOutcome::Stale;
									runtime.state = retryable ? NavigationAgentState::Requesting
										: NavigationAgentState::Failed;
								}
							});
						}
					}
					if(Binding* binding = FindBinding(command.identity)) {
						// A freshly accepted command requests its first route immediately, but
						// later automatic retries still observe the authored backoff interval.
						const auto current = store_->Read(command.identity);
						binding->nextRequestTimeSeconds = elapsedSeconds_
							+ (current.record ? current.record->settings.minimumReplanIntervalSeconds : 0.0);
					}
				}
				receipt.state = NavigationAgentReceiptState::Applied;
				receipt.outcome = NavigationOutcome::Queued;
				receipt.admittedSequence = existing.record->intentSequence + 1U;
			}
			queue_->CompleteReceipt(receipt);
		}
		// Agent mutex remains held by every caller, so no producer can append between
		// this drain and the recomputed declaration hint.
		RefreshPendingRouteAdmissionDemandLocked();
	}

	void NavigationAgentSystem::CaptureCommandAdmission() {
		if(!initialized_.load(std::memory_order_acquire)) return;
		std::lock_guard lock(mutex_);
		if(!store_ || !queue_) return;
		if(queue_->PendingCount() == 0U) {
			// The early system runs each Navigation phase for Stop/removal liveness, but
			// an empty queue must not repeat the later ECS reconciliation scan.
			RefreshPendingRouteAdmissionDemandLocked();
			return;
		}
		ObjectManager& objects = ObjectManager::instance();
		const auto world = NavigationSystem::Instance().PublishedWorld();
		if(!world || !world->world.IsValid()) return;
		ReconcileBindings(objects, world->world);
		RebuildPublishedEntityBindings(objects);
		DrainQueuedCommandsLocked(objects, world->world);
	}

	void NavigationAgentSystem::CaptureProtected() {
		if(!initialized_.load(std::memory_order_acquire)) return;
		std::lock_guard lock(mutex_);
		if(!store_ || !queue_) return;
		ObjectManager& objects = ObjectManager::instance();
		const auto world = NavigationSystem::Instance().PublishedWorld();
		if(!world || !world->world.IsValid()) return;
		ReconcileBindings(objects, world->world);
		RebuildPublishedEntityBindings(objects);
		// A normal scheduler frame drains this queue in NavigationAgentCommandSystem
		// before Navigation captures routes. Keeping this fallback makes a direct
		// agent update and a deliberately omitted command workload safe and compatible.
		DrainQueuedCommandsLocked(objects, world->world);
		captured_.clear();
		for(const NavigationAgentRuntimeRecord& runtime : store_->Snapshot()) {
			auto capture = CaptureNavigationAgent(objects, runtime);
			if(!capture) continue;
			Binding* binding = FindBinding(runtime.identity);
			if(binding) {
				binding->capturedOwnerTransformRevision = capture->owner.transformRevision;
				binding->capturedTargetTransformRevision = capture->target
					? capture->target->transformRevision : 0U;
			}
			const std::optional<NavigationObservationEvidence> retainedObservation =
				ReadRetainedRouteObservation(runtime);
			if(binding && retainedObservation) binding->routeObservation = *retainedObservation;
			if(binding && !binding->routePoints.empty()
				&& (retainedObservation || (!runtime.route.IsValid()
					&& IsRouteEvidenceCurrent(binding->routeObservation)))) {
				capture->routePoints = binding->routePoints;
				capture->waypointIndex = binding->waypointIndex;
				capture->routeUsable = true;
				capture->observation = retainedObservation ? *retainedObservation : binding->routeObservation;
				capture->routeGeneration = binding->routeGeneration;
			}
			if(runtime.request.IsValid()) {
				const auto request = NavigationSystem::Instance().ReadRequest(runtime.request);
				if(request.record) capture->observation = request.record->observation;
				if(request.record && request.record->outcome == NavigationOutcome::Complete) {
					const NavigationRouteHandle route = NavigationSystem::Instance().PublishedRouteForRequest(runtime.request);
					const auto data = NavigationSystem::Instance().ReadRoute(route);
					const auto validation = NavigationSystem::Instance().ValidateRoute(route);
					const std::optional<NavigationObservationEvidence> observation =
						NavigationSystem::Instance().ReadCurrentRouteObservation(route);
					if(data.IsValid() && validation.usable && observation) {
						capture->routePoints = data.points;
						capture->routeUsable = true;
						capture->routeGeneration = route.slotGeneration;
						capture->observation = *observation;
					}
				}
			}
			capture->publicationGeneration = std::max<NavigationGeneration>(1U, publicationGeneration_ + 1U);
			captured_.push_back(std::move(*capture));
		}
		prepared_.assign(captured_.size(), {});
	}

	void NavigationAgentSystem::PrepareCapturedRange(std::size_t begin, std::size_t end) {
		for(std::size_t index = begin; index < end; ++index)
			prepared_[index] = PrepareNavigationAgent(captured_[index]);
	}

	void NavigationAgentSystem::PrepareCapturedWork() {
		PrepareCapturedRange(0U, captured_.size());
	}

	void NavigationAgentSystem::PublishPreparedWork() {
		const auto publicationStarted = std::chrono::steady_clock::now();
		if(!initialized_.load(std::memory_order_acquire)) return;
		std::lock_guard lock(mutex_);
		if(!store_) return;
		ObjectManager& objects = ObjectManager::instance();
		// A protected edit may have landed after capture. Refresh configuration
		// revisions before matching candidates so an old worker packet cannot win.
		const auto world = NavigationSystem::Instance().PublishedWorld();
		if(!world || !world->world.IsValid()) return;
		ReconcileBindings(objects, world->world);
		for(const NavigationAgentCandidate& candidate : prepared_) {
			const auto current = store_->Read(candidate.runtime.identity);
			if(!current.record || current.record->configurationRevision != candidate.runtime.configurationRevision
				|| current.record->intentSequence != candidate.runtime.intentSequence
				|| !IsCurrentAttachment(objects, *current.record)) continue;
			// Repeat protected pose/target capture before publishing a worker result.
			// A worker candidate cannot retain authority if either endpoint changed.
			const auto recaptured = CaptureNavigationAgent(objects, *current.record);
			if(!recaptured || (current.record->destination.kind == NavigationDestinationKind::Object
				&& !recaptured->target)) {
				ReleaseRuntimeRecord(*current.record);
				if(Binding* staleBinding = FindBinding(current.record->identity)) {
					staleBinding->routePoints.clear();
					staleBinding->routeObservation = {};
					staleBinding->routeGeneration = 0U;
					staleBinding->waypointIndex = 0U;
				}
				const NavigationGeneration nextPublication = std::max<NavigationGeneration>(1U, publicationGeneration_ + 1U);
				(void)store_->Update(current.record->identity, [nextPublication](NavigationAgentRuntimeRecord& runtime) {
					runtime.request = {};
					runtime.route = {};
					runtime.guidance = {};
					runtime.state = NavigationAgentState::Failed;
					runtime.outcome = NavigationOutcome::InvalidEndpoint;
					runtime.reason = NavigationReason::InvalidDestination;
					runtime.publicationRevision = nextPublication;
				});
				continue;
			}
			if(Binding* capturedBinding = FindBinding(current.record->identity);
				capturedBinding && (capturedBinding->capturedOwnerTransformRevision != recaptured->owner.transformRevision
					|| capturedBinding->capturedTargetTransformRevision
						!= (recaptured->target ? recaptured->target->transformRevision : 0U))) {
				// The worker used an older endpoint pose. Suppress it for this frame;
				// next protected capture creates a fresh immutable packet.
				const NavigationGeneration nextPublication = std::max<NavigationGeneration>(1U, publicationGeneration_ + 1U);
				(void)store_->Update(current.record->identity, [nextPublication](NavigationAgentRuntimeRecord& runtime) {
					runtime.guidance = {};
					runtime.publicationRevision = nextPublication;
				});
				continue;
			}
			NavigationAgentRuntimeRecord published = candidate.runtime;
			Binding* binding = FindBinding(published.identity);
			if(!binding) continue;
			binding->waypointIndex = candidate.nextWaypointIndex;
			if(candidate.requestReplan && published.request.IsValid() && published.route.IsValid()) {
				// A moving target or a newly invalid route supersedes an otherwise
				// current completed pair. Pending requests have no route handle and are
				// intentionally left alone, preventing a request loop every frame.
				ReleaseRuntimeRecord(published);
				published.request = {};
				published.route = {};
				binding->routePoints.clear();
				binding->routeObservation = {};
				binding->routeGeneration = 0U;
				binding->waypointIndex = 0U;
			}
			if(published.request.IsValid()) {
				const auto request = NavigationSystem::Instance().ReadRequest(published.request);
				if(!request.record) {
					// A released/expired request cannot leave a detached route handle in
					// the agent record. The next worker packet will request fresh coverage.
					binding->routePoints.clear();
					binding->routeObservation = {};
					binding->routeGeneration = 0U;
					published.request = {};
					published.route = {};
				}
				if(request.record && IsTerminal(*request.record)) {
					// Preserve every terminal provider result for diagnostics, including
					// retryable coverage/capacity failures that remain Requesting.
					published.outcome = request.record->outcome;
					published.reason = request.record->reason;
					bool retainCompletedRoute = false;
					if(request.record->outcome == NavigationOutcome::Complete) {
						const NavigationRouteHandle route = NavigationSystem::Instance().PublishedRouteForRequest(published.request);
						const auto routeData = NavigationSystem::Instance().ReadRoute(route);
						const std::optional<NavigationObservationEvidence> observation =
							NavigationSystem::Instance().ReadCurrentRouteObservation(route);
						if(routeData.IsValid() && observation) {
							binding->routePoints = routeData.points;
							binding->routeObservation = *observation;
							binding->routeGeneration = route.slotGeneration;
							binding->waypointIndex = candidate.nextWaypointIndex;
							published.route = route;
							retainCompletedRoute = true;
						}
					}
					if(!retainCompletedRoute) {
						// A stale route is deliberately released before a replacement is
						// admitted. Retaining only a generation-qualified Complete route
						// bounds ownership to one request/route pair per active agent.
						binding->routePoints.clear();
						binding->routeObservation = {};
						binding->routeGeneration = 0U;
						const bool retryableProviderOutcome = request.record->outcome == NavigationOutcome::UnavailableCoverage
							|| request.record->outcome == NavigationOutcome::RejectedCapacity
							|| request.record->outcome == NavigationOutcome::Stale;
						// A transient provider result invalidates movement immediately, but does
						// not revoke the agent's authored destination or intent.  Publishing
						// Requesting makes the neutral guidance state explicit and lets the
						// bounded replan path submit only after its normal backoff interval.
						published.state = retryableProviderOutcome
							? NavigationAgentState::Requesting
							: NavigationAgentState::Failed;
						ReleaseRuntimeRecord(published);
						published.request = {};
						published.route = {};
					}
				}
			}
			if(candidate.requestReplan && !published.request.IsValid() && elapsedSeconds_ >= binding->nextRequestTimeSeconds) {
				const NavigationLocation destination = published.destination.kind == NavigationDestinationKind::Object
					? (candidate.runtime.destination.object.IsValid() ? candidate.runtime.destination.location : NavigationLocation{})
					: published.destination.location;
				NavigationLocation requestDestination = destination;
				if(published.destination.kind == NavigationDestinationKind::Object) {
					const auto target = objects.getObject(published.destination.object.objectId);
					const auto pose = target ? objects.GetCanonicalWorldPose(*target) : std::nullopt;
					if(!target || !pose || objects.getObjectLifetimeGeneration(*target) != published.destination.object.lifetimeGeneration) {
						published.state = NavigationAgentState::Failed;
						published.outcome = NavigationOutcome::InvalidEndpoint;
						published.reason = NavigationReason::InvalidDestination;
					} else requestDestination = {published.identity.world, pose->position};
				}
				if(published.state != NavigationAgentState::Failed) {
					const bool retryingTransientFailure = published.state == NavigationAgentState::Requesting
						&& (published.outcome == NavigationOutcome::UnavailableCoverage
							|| published.outcome == NavigationOutcome::RejectedCapacity
							|| published.outcome == NavigationOutcome::Stale);
					if(retryingTransientFailure) ++retryCount_;
					const auto receipt = NavigationSystem::Instance().RequestAgentRoute(objects,
						published.identity.object, requestDestination, published.settings.traversal);
					if(receipt.Succeeded()) published.request = receipt.request;
					else {
						published.outcome = receipt.outcome;
						published.reason = receipt.reason;
						const bool retryable = receipt.outcome == NavigationOutcome::UnavailableCoverage
							|| receipt.outcome == NavigationOutcome::RejectedCapacity
							|| receipt.outcome == NavigationOutcome::Stale;
						published.state = retryable ? NavigationAgentState::Requesting : NavigationAgentState::Failed;
					}
					if(receipt.Succeeded()) published.state = NavigationAgentState::Requesting;
					binding->nextRequestTimeSeconds = elapsedSeconds_ + published.settings.minimumReplanIntervalSeconds;
				}
			}
			const std::optional<NavigationObservationEvidence> retainedObservation =
				ReadRetainedRouteObservation(published);
			if(retainedObservation) published.guidance.observation = *retainedObservation;
			if(published.state == NavigationAgentState::Stopped
				|| (!retainedObservation && (published.route.IsValid()
					|| !IsRouteEvidenceCurrent(published.guidance.observation))))
				published.guidance = {};
			if(!published.guidance.IsUsable()) published.guidance = {};
			if(published.state == NavigationAgentState::Arrived && published.request.IsValid()) {
				// Arrival has no future movement consumer. Retire the retained provider
				// handles immediately instead of keeping completed routes indefinitely.
				ReleaseRuntimeRecord(published);
				published.request = {};
				published.route = {};
				binding->routePoints.clear();
				binding->routeObservation = {};
				binding->routeGeneration = 0U;
			}
			published.publicationRevision = std::max<NavigationGeneration>(1U, publicationGeneration_ + 1U);
			(void)store_->Update(published.identity, [&published](NavigationAgentRuntimeRecord& runtime) {
				runtime = published;
			});
		}
		++publicationGeneration_;
		if(publicationGeneration_ == 0U) publicationGeneration_ = 1U;
		lastProtectedPublishMicroseconds_ = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - publicationStarted).count());
		// This is the normal protected publication point. It copies summaries, not
		// route polylines, so diagnostics scale linearly with agent count.
		PublishDiagnosticsLocked();
	}

	void NavigationAgentSystem::ObserveProtectedBoundary(NavigationProtectedBoundary boundary,
		std::uint64_t publicationGeneration) {
		std::lock_guard lock(mutex_);
		lastBoundary_ = boundary;
		lastBoundaryGeneration_ = std::max(lastBoundaryGeneration_, publicationGeneration);
		if(!store_) return;
		bool diagnosticsChanged = false;
		const auto world = NavigationSystem::Instance().PublishedWorld();
		if(world && world->world.IsValid()) ReconcileBindings(ObjectManager::instance(), world->world);
		for(const NavigationAgentRuntimeRecord& record : store_->Snapshot()) {
			const auto recaptured = CaptureNavigationAgent(ObjectManager::instance(), record);
			Binding* binding = FindBinding(record.identity);
			const std::optional<NavigationObservationEvidence> retainedObservation =
				ReadRetainedRouteObservation(record);
			if(binding && retainedObservation) binding->routeObservation = *retainedObservation;
			const bool ownerMissing = !recaptured;
			const bool targetMissing = !ownerMissing && record.destination.kind == NavigationDestinationKind::Object
				&& (!recaptured || !recaptured->target);
			const bool endpointChanged = binding && recaptured
				&& (binding->capturedOwnerTransformRevision != recaptured->owner.transformRevision
					|| binding->capturedTargetTransformRevision
						!= (recaptured->target ? recaptured->target->transformRevision : 0U));
			const bool routeEvidenceStale = record.route.IsValid()
				? !retainedObservation.has_value()
				: !IsRouteEvidenceCurrent(record.guidance.observation);
			if(ownerMissing || targetMissing || endpointChanged || routeEvidenceStale) {
				diagnosticsChanged = true;
				if(ownerMissing || targetMissing) {
					// A missing owner pose cannot consume or later release a retained route.
					// Retire both provider handles at this protected edge before publishing
					// the terminal record, exactly as a missing object target does.
					ReleaseRuntimeRecord(record);
					if(binding) {
						binding->routePoints.clear();
						binding->routeObservation = {};
						binding->routeGeneration = 0U;
						binding->waypointIndex = 0U;
					}
				}
				const NavigationGeneration nextPublication = std::max<NavigationGeneration>(1U, publicationGeneration_ + 1U);
				(void)store_->Update(record.identity, [ownerMissing, targetMissing, nextPublication](NavigationAgentRuntimeRecord& runtime) {
					if(ownerMissing || targetMissing) {
						runtime.request = {};
						runtime.route = {};
						runtime.state = NavigationAgentState::Failed;
						runtime.outcome = NavigationOutcome::InvalidEndpoint;
						runtime.reason = ownerMissing
							? NavigationReason::InvalidStart : NavigationReason::InvalidDestination;
					}
					runtime.guidance = {};
					runtime.publicationRevision = nextPublication;
				});
			}
		}
		// Boundary invalidation is uncommon relative to normal publication. Refresh
		// summaries only when it actually changed a visible agent record.
		if(diagnosticsChanged) PublishDiagnosticsLocked();
	}

	void NavigationAgentSystem::RevokeAllAgents(NavigationReason) {
		std::vector<NavigationAgentRuntimeRecord> records;
		{
			std::lock_guard lock(mutex_);
			if(store_) records = store_->Snapshot();
			bindings_.clear();
			bindingIndexes_.clear();
			// Revoke publishes an empty diagnostic snapshot. Release buckets here so
			// that snapshot remains an honest zero-agent ownership boundary.
			bindingIndexes_.rehash(0U);
			std::atomic_store_explicit(&publishedEntityBindings_,
				std::make_shared<const std::vector<NavigationAgentPublishedEntityBinding>>(),
				std::memory_order_release);
			std::atomic_store_explicit(&publishedDiagnostics_,
				std::make_shared<const NavigationAgentDiagnosticsSnapshot>(), std::memory_order_release);
			publishedObjectBindingOverflowCount_.store(0U, std::memory_order_release);
			hasPublishedEntityBindingSnapshot_ = false;
			// Do not reclaim worker packet vectors here. A lifecycle call can arrive
			// while a declared prepare chunk still reads its disjoint range; publication
			// rejects released identities and the next protected capture reuses storage.
		}
		for(const auto& record : records) ReleaseRuntimeRecord(record);
		std::lock_guard lock(mutex_);
		if(store_) for(const auto& record : records) (void)store_->Release(record.identity);
		if(queue_) queue_->Clear();
		RefreshPendingRouteAdmissionDemandLocked();
	}

	void NavigationAgentSystem::SetContractTestWorkAvailable(bool available) noexcept {
		contractTestWorkAvailable_.store(available, std::memory_order_release);
	}
}
