#include "Audio/AudioSystem.h"
#include "Audio/Backend/MiniaudioBackend.h"
#include "Audio/Assets/AudioClipAsset.h"
#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"
#include "Scene/TransactionCommitOutcome.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <array>
#include <limits>
#include <tuple>
#include <exception>
#include <type_traits>
#include <iterator>

namespace TerraEngine::Audio {
	namespace {
		AudioVoiceToken VoiceTokenFor(const AudioSourceIdentity& identity) noexcept {
			std::uint64_t hash = 1469598103934665603ULL;
			const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
			for(const auto byte : identity.objectId) mix(byte);
			mix(identity.componentGeneration);
			mix(identity.playbackGeneration);
			return hash == 0U ? 1U : hash;
		}

		std::string ClipKey(const TerraEngine::UUID& id, AudioGeneration generation) {
			return TerraEngine::ToHexString(id) + ':' + std::to_string(generation);
		}

		// Copy mix policy into owned source values before immutable construction;
		// this helper never dereferences live component storage.
		void ApplyMixValues(AudioAuthoringSnapshotData& data, const AudioMixSnapshot& snapshot) noexcept {
			data.masterVolume = snapshot.master.volume;
			data.masterMuted = snapshot.master.muted;
			for(auto& source : data.sources) {
				auto group = std::find_if(snapshot.groups.begin(), snapshot.groups.end(), [&](const auto& item) {
					return item.id == source.mixGroupId;
				});
				if(group == snapshot.groups.end() && !snapshot.groups.empty()) group = snapshot.groups.begin();
				source.mixVolume = group == snapshot.groups.end() ? 1.0F : group->volume;
				source.mixMuted = group != snapshot.groups.end() && group->muted;
			}
		}
	}
	AudioSystem& AudioSystem::Instance() { static AudioSystem system; return system; }
	void AudioSystem::Initialize() {
		std::scoped_lock lock(mutex); if(initialized) return;
		// Reset both admission owners together; reservations belong to the queue's
		// epoch, while the central playback allocator deliberately survives restart.
		controls.Reset(); playReservations.clear();
		Backend::RegisterMiniaudioBackend(); auto& registry = Backend::GlobalAudioBackendRegistry(); registry.Freeze(); AudioResult createResult; backend = registry.Create(providerId, createResult);
		const auto found = registry.Find(providerId); if(found) descriptor = *found;
		AudioResult result = createResult; if(!backend || !(result = backend->Initialize({})).Succeeded()) { PublishFailure("provider.initialize", result.message.empty() ? "Audio is safely silent because the configured provider could not initialize." : result.message); backend.reset(); }
		deviceAvailable = backend != nullptr;
		if(deviceAvailable)
			TERRAENGINE_LOG(LogLevel::Info, "Audio provider initialized: " << providerId);
		initialized = true;
	}
	void AudioSystem::Update(float deltaTime) {
		std::scoped_lock lock(mutex);
		if(!initialized) return;
		CapturePhase(deltaTime);
		SnapshotPhase();
		PreparePhase();
		CommitPhase();
	}
	void AudioSystem::CapturePhase(float deltaTime) {
		++frame;
		auto data = authoring.Data();
		auto candidatePlayback = playback;
		std::vector<AudioAuthoringSourceRecord> liveSources;
		std::vector<AudioAuthoringListenerRecord> liveListeners;
		ObjectManager& objects = ObjectManager::instance();
		if(!manualAuthoringSnapshot) {
			objects.forEachObjectWithComponents<AudioSourceComponent, Transform>([&](Object object, AudioSourceComponent& source, Transform& transform) {
				const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
				if(!pose) return;
				AudioAuthoringSourceRecord record;
				record.identity = {objects.getUUID(object), objects.getObjectComponentGeneration(object), 0U};
				record.attachment = {record.identity.objectId, objects.GetSceneGeneration(),
					objects.getObjectLifetimeGeneration(object), record.identity.componentGeneration};
				record.objectEntity = object.entityValue;
				// A scene replacement can reuse UUID/component versions. Retire the old
				// seed against its complete attachment before considering auto-start.
				const auto priorSource = std::find_if(data.sources.begin(), data.sources.end(), [&](const auto& prior) {
					return prior.identity.objectId == record.identity.objectId;
				});
				if(priorSource == data.sources.end() || priorSource->attachment != record.attachment)
					candidatePlayback.erase(std::remove_if(candidatePlayback.begin(), candidatePlayback.end(), [&](const auto& seed) {
						return seed.identity.objectId == record.identity.objectId;
					}), candidatePlayback.end());
				record.clipAssetId = source.clipAssetId;
				// Keep cells and double offsets together until worker listener selection.
				// Separately converting cell offsets changes the physical separation.
				record.worldPosition = pose->position;
				record.volume = source.volume; record.pitch = source.pitch;
				record.mixGroupId = source.mixGroupId;
				record.maximumDistance = source.maxDistance; record.spatialBlend = source.spatialBlend; record.priority = source.priority;
				record.enabled = source.active; record.loop = source.loop; record.spatial = source.spatialBlend > 0.0F;
				// A provider-initialization failure leaves audio safely silent. Avoid
				// initializing decoder/resource infrastructure in that state: the source
				// still publishes as a pending logical voice, and there is no provider
				// that could consume retained clip bytes during this frame.
				if(backend) {
					if(const auto clip = ResourceManager::Instance().GetAudioClip(source.clipAssetId)) {
						record.clipGeneration = clip->Generation(); record.clipFrames = clip->FrameCount();
						record.clipChecksum = clip->Metadata().payloadChecksum; record.clipSampleRate = clip->SampleRate(); record.clipChannels = clip->Channels();
						record.clipEncoding = clip->Encoding() == AudioEncoding::OggVorbis ? AudioClipEncoding::OggVorbis : AudioClipEncoding::Wav;
						record.clipPayload = clip->RetainedPayload();
					}
				}
				// playOnStart is a runtime lifecycle event, not an editor-preview action.
				// The editor explicitly opens this gate on Play Mode entry; exported games
				// leave it enabled from startup.
				if(runtimePlaybackEnabled && source.playOnStart && std::none_of(candidatePlayback.begin(), candidatePlayback.end(), [&](const auto& seed) { return seed.identity.objectId == record.identity.objectId && seed.identity.componentGeneration == record.identity.componentGeneration; })) {
					auto identity = record.identity;
					identity.playbackGeneration = AllocatePlaybackGeneration();
					if(identity.playbackGeneration != 0U)
						candidatePlayback.push_back({identity, AudioPlaybackState::Playing, 0U, record.clipAssetId, record.clipGeneration});
					else PublishFailure("control.generation_exhausted", "Audio playback identities are exhausted; auto-start was refused.");
				}
				liveSources.push_back(std::move(record));
			});
			objects.forEachObjectWithComponents<AudioListenerComponent, Transform>([&](Object object, AudioListenerComponent& listener, Transform& transform) {
				const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
				if(!pose) return;
				const glm::vec3 forward = pose->rotation * glm::vec3(0.0F, 0.0F, -1.0F);
				liveListeners.push_back({objects.getUUID(object), objects.getObjectComponentGeneration(object),
					{}, {forward.x, forward.y, forward.z}, listener.outputMode, listener.active, listener.eligible, pose->position});
			});
		}
		if(!manualAuthoringSnapshot) {
			data.coordinateMode = AudioCoordinateMode::CanonicalWorld;
			data.sources = std::move(liveSources); data.listeners = std::move(liveListeners);
		}
		data.frame = frame; data.deltaSeconds = deltaTime; data.masterVolume = mix.Snapshot().master.volume; data.masterMuted = mix.Snapshot().master.muted;
		// A disabled runtime is also paused for provider/timeline purposes. This is
		// a second safety boundary for explicit controls racing a Play Mode exit.
		data.gamePaused = !runtimePlaybackEnabled || runtimePlaybackPaused;
		authoring = AudioAuthoringSnapshot::Create(std::move(data));
		playback.swap(candidatePlayback);
	}
	void AudioSystem::Shutdown() { std::scoped_lock lock(mutex); controls.Stop(); if(backend) { (void)backend->Shutdown(); backend.reset(); } activeVoices.clear(); registeredClips.clear(); playback.clear(); playReservations.clear(); published.reset(); controlReceipts.clear(); failures.clear(); authoring = AudioAuthoringSnapshot::Create({}); manualAuthoringSnapshot = false; frame = 0U; packetSequence = 0U; deviceAvailable = false; initialized = false; }
	std::string AudioSystem::SystemName() { return "AudioSystem"; }
	TerraEngine::Scheduling::SystemDeclaration AudioSystem::SchedulerDeclaration() {
		TerraEngine::Scheduling::SystemDeclaration declaration;
		declaration.name = SystemName(); declaration.phase = "Audio"; declaration.before = {"UISystem", "RenderSystem"};
		declaration.reads = {"ObjectManager", "RuntimeLayout", "AudioAuthoring", "Canonical world poses", "Audio listeners", "Audio clip generations", "Audio pending Play reservations"};
		declaration.writes = {"AudioPreparedFrame", "AudioProvider", "AudioDiagnostics", "Audio source component values", "Scene transaction publication", "Audio control ownership", "Audio playback bindings"};
		declaration.mainThread = true; declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::Protected;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Listener selection, checked canonical localization, attenuation, cursor advancement, and voice ranking over immutable records.";
		declaration.protectedCommit = "Capture source/listener values, commit typed source settings and prepared companion state, acknowledge controls, then publish provider and playback snapshots.";
		declaration.protectedReason = "Scene capture and audio device calls cannot execute in worker-safe preparation.";
		declaration.profilingLabel = "Audio"; declaration.omitWhenNoFrameWorkloads = true; declaration.usesDefaultMetadata = false;
		return declaration;
	}
	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> AudioSystem::DeclareFrameWorkloads(float deltaTime) {
		auto snapshot = TerraEngine::Scheduling::MakeSnapshotPhase("AudioSnapshot", SystemName(), {"Audio components", "Transforms", "Canonical world poses", "Audio listeners", "Audio clip generations", "Audio control queue"}, "AudioSnapshot");
		snapshot.phase = "Audio";
		snapshot.stableDescriptorId = "AudioSystem/Audio/AudioSnapshot";
		snapshot.writes = {"Audio frame snapshot", "Audio receipts", "Audio playback bindings", "Audio pending Play reservations", "Audio source component values", "Scene transaction publication", "Audio control ownership"};
		snapshot.estimatedWorkItemCount = authoring.Data().sources.size();
		// Live ObjectManager capture is a protected operation. The resulting value
		// snapshot, rather than component storage, crosses into worker preparation.
		snapshot.workerEligible = false;
		snapshot.mainThreadRequired = true;
		snapshot.protectedReason = "Live audio components and transforms must be captured on the protected scene boundary.";
		snapshot.work = [this, deltaTime](TerraEngine::Scheduling::JobGraphContext&) {
			std::scoped_lock lock(mutex);
			if(!initialized) return;
			CapturePhase(deltaTime);
			SnapshotPhase();
		};

		auto prepare = TerraEngine::Scheduling::MakeWorkerPreparePhase("AudioPrepare", SystemName(), {"Audio frame snapshot", "Playback snapshot"}, {"Audio prepared frame"}, "AudioPrepare");
		prepare.phase = "Audio";
		prepare.stableDescriptorId = "AudioSystem/Audio/AudioPrepare";
		prepare.explicitDependencies = {snapshot.stableDescriptorId};
		prepare.estimatedWorkItemCount = authoring.Data().sources.size();
		prepare.partitionPolicy = TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::DeterministicRanges;
		prepare.work = [this](TerraEngine::Scheduling::JobGraphContext&) {
			// The lock protects publication ownership only. PrepareAudioFrame reads
			// immutable value records and never touches live scene or provider state.
			std::scoped_lock lock(mutex);
			if(initialized) PreparePhase();
		};

		auto commit = TerraEngine::Scheduling::MakeProtectedCommitPhase("AudioCommit", SystemName(), {"Audio prepared frame", "Provider observations"}, {"Audio provider", "Playback snapshot", "Audio diagnostics"}, "Provider/device calls require the protected audio boundary.", "AudioCommit");
		commit.phase = "Audio";
		commit.stableDescriptorId = "AudioSystem/Audio/AudioCommit";
		commit.explicitDependencies = {prepare.stableDescriptorId};
		commit.estimatedWorkItemCount = prepared.voices.size();
		commit.work = [this](TerraEngine::Scheduling::JobGraphContext&) {
			std::scoped_lock lock(mutex);
			if(initialized) CommitPhase();
		};
		return {std::move(snapshot), std::move(prepare), std::move(commit)};
	}
	AudioGeneration AudioSystem::AllocatePlaybackGeneration() noexcept {
		const auto allocated = nextPlaybackGeneration;
		if(allocated != 0U)
			nextPlaybackGeneration = allocated == std::numeric_limits<AudioGeneration>::max() ? 0U : allocated + 1U;
		return allocated;
	}
	bool AudioSystem::ResolveAdmission(AudioControlBatch& batch, AudioControlReceipt& receipt) {
		// Queue admission already holds its lock after the system lock. Reserve
		// every allocation before appending any external predecessor reservation.
		playReservations.reserve(playReservations.size() + batch.commands.size());
		std::vector<PlayReservation> staged;
		staged.reserve(batch.commands.size());
		if(batch.requestedFrame == 0U) batch.requestedFrame = frame + 1U;
		receipt.requestedFrame = batch.requestedFrame;
		auto reject = [&](AudioControlRejection reason, const char* diagnostic) {
			receipt.rejection = reason;
			receipt.diagnostic = diagnostic;
			return false;
		};
		for(auto& command : batch.commands) {
			if(command.kind > AudioControlKind::SetMasterMuted
				|| (command.targetMode != AudioControlTargetMode::ExactInstance
					&& command.targetMode != AudioControlTargetMode::BindCurrentAtAdmission))
				return reject(AudioControlRejection::InvalidValue, "Audio command kind or target mode is unsupported.");
			if(command.kind > AudioControlKind::SetSourcePitch) continue;
			const auto source = std::find_if(authoring.Data().sources.begin(), authoring.Data().sources.end(), [&](const auto& item) {
				return item.identity.objectId == command.source.objectId;
			});
			if(source == authoring.Data().sources.end()) return reject(AudioControlRejection::StaleTarget, "Audio source has not been captured or has retired.");
			// Compatibility is confined to explicitly owned snapshots; production
			// commands never turn a missing generation into a wildcard lookup.
			if(manualAuthoringSnapshot && command.source.sceneGeneration == 0U)
				command.source.sceneGeneration = source->attachment.sceneGeneration;
			const AudioSourceAttachmentKey attachment{command.source.objectId, command.source.sceneGeneration,
				command.source.objectGeneration, command.source.componentGeneration};
			if(!attachment.IsValid() || attachment != source->attachment)
				return reject(AudioControlRejection::StaleTarget, "Audio source attachment is stale.");
			if(command.kind == AudioControlKind::Play) {
				const auto generation = AllocatePlaybackGeneration();
				if(generation == 0U) return reject(AudioControlRejection::GlobalCapacity, "Audio playback identity capacity is exhausted.");
				command.source.playbackGeneration = generation;
				staged.push_back({attachment, batch.requestedFrame, batch.batchSequence, command.commandIndex, generation});
			} else if(command.kind <= AudioControlKind::Stop) {
				if(command.targetMode == AudioControlTargetMode::BindCurrentAtAdmission) {
					const PlayReservation* predecessor = nullptr;
					const auto order = std::tuple{batch.requestedFrame, batch.batchSequence, command.commandIndex};
					const auto consider = [&](const PlayReservation& candidate) {
						const auto candidateOrder = std::tuple{candidate.requestedFrame, candidate.sequence, candidate.commandIndex};
						if(candidate.attachment == attachment && candidateOrder < order
							&& (predecessor == nullptr || std::tuple{predecessor->requestedFrame, predecessor->sequence, predecessor->commandIndex} < candidateOrder))
							predecessor = &candidate;
					};
					for(const auto& candidate : playReservations) consider(candidate);
					for(const auto& candidate : staged) consider(candidate);
					command.source.playbackGeneration = predecessor == nullptr ? 0U : predecessor->playbackGeneration;
					if(predecessor == nullptr) {
						const auto current = std::find_if(playback.begin(), playback.end(), [&](const auto& seed) {
							return seed.identity.objectId == attachment.objectId && seed.identity.componentGeneration == attachment.componentGeneration
								&& seed.state != AudioPlaybackState::Stopped && seed.state != AudioPlaybackState::Completed && seed.state != AudioPlaybackState::Failed;
						});
						if(current != playback.end()) command.source.playbackGeneration = current->identity.playbackGeneration;
					}
				}
				if(command.source.playbackGeneration == 0U)
					return reject(AudioControlRejection::StaleTarget, "Audio source has no controllable playback instance.");
			}
			if(command.kind <= AudioControlKind::Stop)
				receipt.resolvedPlaybackGeneration = command.source.playbackGeneration;
		}
		// All target checks, allocations and diagnostic construction precede this
		// trivially copied append. The queue performs only no-fail publication next.
		for(const auto& reservation : staged) playReservations.push_back(reservation);
		return true;
	}
	AudioControlReceipt AudioSystem::SubmitControlBatch(AudioControlBatch batch) {
		std::scoped_lock lock(mutex);
		return controls.Submit(std::move(batch), [](void* owner, AudioControlBatch& staged, AudioControlReceipt& receipt) {
			return static_cast<AudioSystem*>(owner)->ResolveAdmission(staged, receipt);
		}, this);
	}
	std::optional<AudioSourceBinding> AudioSystem::ReadSourceBinding(std::uint32_t entity) const {
		std::scoped_lock lock(mutex);
		for(const auto& source : authoring.Data().sources)
			if(source.objectEntity == entity && source.attachment.IsValid()) return AudioSourceBinding{entity, source.attachment};
		return std::nullopt;
	}
	std::optional<AudioSourceBinding> AudioSystem::ReadSourceBinding(const TerraEngine::UUID& objectId) const {
		std::scoped_lock lock(mutex);
		for(const auto& source : authoring.Data().sources)
			if(source.attachment.objectId == objectId && source.attachment.IsValid()) return AudioSourceBinding{source.objectEntity, source.attachment};
		return std::nullopt;
	}
	std::optional<AudioSourcePlaybackObservation> AudioSystem::ReadSourcePlayback(const AudioSourceAttachmentKey& attachment) const {
		std::scoped_lock lock(mutex);
		if(!published || std::none_of(authoring.Data().sources.begin(), authoring.Data().sources.end(), [&](const auto& source) {
			return source.attachment == attachment;
		})) return std::nullopt;
		// Snapshot and commit are distinct scheduler workloads. A newly captured
		// attachment must never borrow an older publication with reused UUIDs.
		if(std::none_of(published->sourceBindings.begin(), published->sourceBindings.end(), [&](const auto& binding) {
			return binding.attachment == attachment;
		})) return std::nullopt;
		for(const auto& voice : published->voices) {
			if(voice.identity.objectId != attachment.objectId || voice.identity.componentGeneration != attachment.componentGeneration) continue;
			const auto logical = std::find_if(published->logicalPlayback.begin(), published->logicalPlayback.end(), [&](const auto& seed) { return seed.identity == voice.identity; });
			if(logical != published->logicalPlayback.end()) return AudioSourcePlaybackObservation{published->frame, voice, logical->state};
		}
		return std::nullopt;
	}
	std::optional<AudioControlReceipt> AudioSystem::ReadControlReceipt(AudioBatchSequence batchSequence) const {
		std::scoped_lock lock(mutex);
		const auto found = std::find_if(controlReceipts.rbegin(), controlReceipts.rend(), [&](const auto& receipt) {
			return receipt.batchSequence == batchSequence;
		});
		return found == controlReceipts.rend() ? std::nullopt : std::optional<AudioControlReceipt>(*found);
	}
	std::shared_ptr<const AudioPlaybackSnapshot> AudioSystem::LatestPlaybackSnapshot() const { std::scoped_lock lock(mutex); return published; }
	AudioMixSnapshot AudioSystem::LatestMixSnapshot() const { return mix.Snapshot(); }
	bool AudioSystem::SetMasterMix(float volume, bool muted, std::string& diagnostic) {
		if(!mix.SetMasterVolume(volume, diagnostic)) return false;
		return mix.SetMasterMuted(muted);
	}
	bool AudioSystem::ReplaceMixSnapshot(AudioMixSnapshot snapshot, std::string& diagnostic) { return mix.Replace(std::move(snapshot), diagnostic); }
	bool AudioSystem::SetMixGroupVolume(const TerraEngine::UUID& id, float volume, std::string& diagnostic) { return mix.SetGroupVolume(id, volume, diagnostic); }
	bool AudioSystem::SetMixGroupMuted(const TerraEngine::UUID& id, bool muted, std::string& diagnostic) { return mix.SetGroupMuted(id, muted, diagnostic); }
	bool AudioSystem::RenameMixGroup(const TerraEngine::UUID& id, std::string name, std::string& diagnostic) { return mix.RenameGroup(id, std::move(name), diagnostic); }
	void AudioSystem::SetAuthoringSnapshot(AudioAuthoringSnapshot snapshot) {
		std::scoped_lock lock(mutex);
		auto data = snapshot.Data();
		for(auto& source : data.sources)
			if(!source.attachment.IsValid()) source.attachment = {source.identity.objectId, 1U, 1U, source.identity.componentGeneration};
		auto candidatePlayback = playback;
		candidatePlayback.erase(std::remove_if(candidatePlayback.begin(), candidatePlayback.end(), [&](const auto& seed) {
			const auto oldSource = std::find_if(authoring.Data().sources.begin(), authoring.Data().sources.end(), [&](const auto& source) { return source.identity.objectId == seed.identity.objectId; });
			return oldSource == authoring.Data().sources.end() || std::none_of(data.sources.begin(), data.sources.end(), [&](const auto& source) { return source.attachment == oldSource->attachment; });
		}), candidatePlayback.end());
		authoring = AudioAuthoringSnapshot::Create(std::move(data));
		playback.swap(candidatePlayback);
		manualAuthoringSnapshot = true;
	}
	void AudioSystem::SetConfiguredProvider(std::string value) { std::scoped_lock lock(mutex); if(!initialized) providerId = std::move(value); }
	void AudioSystem::SetRuntimePlaybackEnabled(bool enabled) {
		std::scoped_lock lock(mutex);
		if(enabled != runtimePlaybackEnabled && !manualAuthoringSnapshot) {
			// Play entry swaps the live scene before script creation. Retire edit
			// bindings now so a first script facade waits for runtime capture instead
			// of permanently binding an earlier scene's coincident entity number.
			auto data = authoring.Data();
			data.sources.clear(); data.listeners.clear();
			auto emptyCapture = AudioAuthoringSnapshot::Create(std::move(data));
			authoring = std::move(emptyCapture);
			published.reset();
			playback.clear(); playReservations.clear(); controls.Reset();
		}
		runtimePlaybackEnabled = enabled;
		runtimePlaybackPaused = false;
		// Leaving Play Mode retires every logical instance. A later Play entry then
		// creates a fresh playOnStart generation at frame zero.
		if(!enabled) playback.clear();
	}
	void AudioSystem::SetRuntimePlaybackPaused(bool paused) {
		std::scoped_lock lock(mutex);
		runtimePlaybackPaused = runtimePlaybackEnabled && paused;
	}
	void AudioSystem::SnapshotPhase() {
		auto data = authoring.Data();
		// Deleting an object removes it from the immutable capture. Prune its
		// logical seed here so later commit can release the orphan provider voice.
		playback.erase(std::remove_if(playback.begin(), playback.end(), [&](const AudioPlaybackSeed& seed) {
			return std::none_of(data.sources.begin(), data.sources.end(), [&](const auto& source) {
				return source.identity.objectId == seed.identity.objectId
					&& source.identity.componentGeneration == seed.identity.componentGeneration;
			});
		}), playback.end());
		const auto drained = controls.DrainEligible(frame);
		// Draining transfers ownership rather than releasing pending capacity. On
		// any exceptional exit every unacknowledged owner becomes terminal; retries
		// cannot leave an abandoned in-flight request consuming capacity forever.
		struct SettlementGuard {
			AudioControlQueue& queue;
			const std::vector<AudioControlBatch>& batches;
			std::vector<PlayReservation>& reservations;
			~SettlementGuard() {
				for(const auto& pending : batches) {
					(void)queue.Complete(pending.completionToken, AudioControlStatus::Rejected,
						AudioControlRejection::InternalFailure);
					reservations.erase(std::remove_if(reservations.begin(), reservations.end(), [&](const auto& record) {
						return record.sequence == pending.batchSequence;
					}), reservations.end());
				}
			}
		} settlement{controls, drained, playReservations};
		// Reserve the bounded receipt ring before any live publication. Moving its
		// owned strings and erasing the oldest element cannot allocate after commit.
		static_assert(std::is_nothrow_move_constructible_v<AudioControlReceipt>
			&& std::is_nothrow_move_assignable_v<AudioControlReceipt>);
		static_assert(std::is_nothrow_move_assignable_v<AudioAuthoringSnapshotData>
			&& std::is_nothrow_move_assignable_v<AudioAuthoringSnapshot>
			&& std::is_nothrow_move_assignable_v<std::vector<AudioPlaybackSeed>>);
		controlReceipts.reserve(257U);
		for(const auto& batch : drained) {
			auto candidateData = data;
			auto candidatePlayback = playback;
			auto candidateMix = mix.Snapshot();
			bool mixChanged = false;
			AudioControlReceipt receipt{batch.batchSequence, batch.requestedFrame, AudioControlStatus::Applied,
				AudioControlRejection::None, batch.correlationId, {}};
			const auto settleReceipt = [&]() noexcept {
				receipt.completionToken = batch.completionToken;
				for(const auto& command : batch.commands)
					if(command.kind <= AudioControlKind::Stop) receipt.resolvedPlaybackGeneration = command.source.playbackGeneration;
				const auto status = receipt.status;
				const auto rejection = receipt.rejection;
				controlReceipts.push_back(std::move(receipt));
				if(controlReceipts.size() > 256U) controlReceipts.erase(controlReceipts.begin());
				(void)controls.Complete(batch.completionToken, status, rejection);
			};
			for(const auto& command : batch.commands) {
				const AudioSourceIdentity identity{command.source.objectId, command.source.componentGeneration, command.source.playbackGeneration};
				auto source = std::find_if(candidateData.sources.begin(), candidateData.sources.end(), [&](const auto& item) {
					return item.identity.objectId == identity.objectId && item.identity.componentGeneration == identity.componentGeneration;
				});
				auto seed = std::find_if(candidatePlayback.begin(), candidatePlayback.end(), [&](const auto& item) {
					return item.identity == identity;
				});
				const bool sourceCommand = command.kind <= AudioControlKind::SetSourcePitch;
				const AudioSourceAttachmentKey attachment{command.source.objectId, command.source.sceneGeneration,
					command.source.objectGeneration, command.source.componentGeneration};
				if(sourceCommand && (identity.objectId == TerraEngine::EMPTY_UUID || identity.componentGeneration == 0U
					|| source == candidateData.sources.end() || source->attachment != attachment)) {
					receipt = {batch.batchSequence, batch.requestedFrame, AudioControlStatus::Rejected,
						AudioControlRejection::StaleTarget, batch.correlationId, "Audio source target is missing or stale."};
					break;
				}
				if((command.kind == AudioControlKind::SetSourceVolume || command.kind == AudioControlKind::SetSourcePitch
					|| command.kind == AudioControlKind::SetGroupVolume || command.kind == AudioControlKind::SetMasterVolume)
					&& !std::isfinite(command.scalarValue)) {
					receipt = {batch.batchSequence, batch.requestedFrame, AudioControlStatus::Rejected,
						AudioControlRejection::InvalidValue, batch.correlationId, "Audio control values must be finite."};
					break;
				}
				switch(command.kind) {
				case AudioControlKind::Play:
					if(identity.playbackGeneration == 0U) {
						receipt = {batch.batchSequence, batch.requestedFrame, AudioControlStatus::Rejected,
							AudioControlRejection::InvalidTarget, batch.correlationId, "Play requires a non-zero playback generation."};
						break;
					}
					candidatePlayback.erase(std::remove_if(candidatePlayback.begin(), candidatePlayback.end(), [&](const auto& item) {
						return item.identity.objectId == identity.objectId && item.identity.componentGeneration == identity.componentGeneration;
					}), candidatePlayback.end());
					candidatePlayback.push_back({identity, AudioPlaybackState::Playing, 0U, source->clipAssetId, source->clipGeneration});
					break;
				case AudioControlKind::Pause:
				case AudioControlKind::Resume:
				case AudioControlKind::Stop:
					if(seed == candidatePlayback.end() || seed->state == AudioPlaybackState::Stopped
						|| seed->state == AudioPlaybackState::Completed || seed->state == AudioPlaybackState::Failed) {
						receipt = {batch.batchSequence, batch.requestedFrame, AudioControlStatus::Rejected,
							AudioControlRejection::StaleTarget, batch.correlationId, "Playback generation is no longer active."};
						break;
					}
					seed->state = command.kind == AudioControlKind::Pause ? AudioPlaybackState::Paused
						: command.kind == AudioControlKind::Resume ? AudioPlaybackState::Playing : AudioPlaybackState::Stopped;
					if(command.kind == AudioControlKind::Stop) seed->logicalFrame = 0U;
					break;
				case AudioControlKind::SetSourceVolume: source->volume = std::clamp(command.scalarValue, 0.0F, 1.0F); break;
				case AudioControlKind::SetSourcePitch: source->pitch = NormalizeAudioPitch(command.scalarValue); break;
				case AudioControlKind::SetGroupVolume:
				case AudioControlKind::SetGroupMuted: {
					const auto group = std::find_if(candidateMix.groups.begin(), candidateMix.groups.end(), [&](const auto& item) { return item.id == command.mixGroupId; });
					if(group == candidateMix.groups.end()) {
						receipt = {batch.batchSequence, batch.requestedFrame, AudioControlStatus::Rejected,
							AudioControlRejection::InvalidTarget, batch.correlationId, "Audio mix group does not exist."};
						break;
					}
					if(command.kind == AudioControlKind::SetGroupVolume) group->volume = AudioMixConfiguration::NormalizeVolume(command.scalarValue);
					else group->muted = command.booleanValue;
					mixChanged = true;
					break;
				}
				case AudioControlKind::SetMasterVolume: candidateMix.master.volume = AudioMixConfiguration::NormalizeVolume(command.scalarValue); mixChanged = true; break;
				case AudioControlKind::SetMasterMuted: candidateMix.master.muted = command.booleanValue; mixChanged = true; break;
				}
				if(receipt.status == AudioControlStatus::Rejected) break;
			}
			std::string diagnostic;
			if(receipt.status != AudioControlStatus::Rejected && mixChanged && !AudioMixConfiguration::Validate(candidateMix, diagnostic))
				receipt = {batch.batchSequence, batch.requestedFrame, AudioControlStatus::Rejected,
					AudioControlRejection::InvalidValue, batch.correlationId, diagnostic};
			if(receipt.status != AudioControlStatus::Rejected) {
				std::vector<TerraEngine::Scene::AudioSourceSettingsUpdate> settings;
				if(!manualAuthoringSnapshot) {
					settings.reserve(batch.commands.size());
					for(const auto& command : batch.commands) {
						if(command.kind != AudioControlKind::SetSourceVolume && command.kind != AudioControlKind::SetSourcePitch) continue;
						const AudioSourceAttachmentKey attachment{command.source.objectId, command.source.sceneGeneration,
							command.source.objectGeneration, command.source.componentGeneration};
						auto update = std::find_if(settings.begin(), settings.end(), [&](const auto& item) { return item.attachment == attachment; });
						if(update == settings.end()) { settings.push_back({attachment}); update = std::prev(settings.end()); }
						// Fold ordered commands into one typed update for each attachment.
						if(command.kind == AudioControlKind::SetSourceVolume) update->volume = NormalizeAudioUnit(command.scalarValue);
						else update->pitch = NormalizeAudioPitch(command.scalarValue);
					}
				}
				const auto expectedMixGeneration = candidateMix.generation;
				auto mixPublication = (mixChanged || !settings.empty())
					? mix.PrepareReplace(std::move(candidateMix), expectedMixGeneration, diagnostic)
					: std::optional<AudioMixConfiguration::PreparedPublication>{};
				if((mixChanged || !settings.empty()) && !mixPublication)
					receipt = {batch.batchSequence, batch.requestedFrame, AudioControlStatus::Rejected,
						AudioControlRejection::InvalidValue, batch.correlationId, diagnostic};
				if(receipt.status != AudioControlStatus::Rejected) {
					ApplyMixValues(candidateData, mixPublication ? mixPublication->Candidate() : candidateMix);
					// Complete all immutable storage before live writes. The accepted
					// source, playback and mix candidates are then adopted by no-fail moves.
					auto candidateAuthoring = AudioAuthoringSnapshot::Create(candidateData);
					const auto adoptCompanions = [&]() noexcept {
						if(mixChanged) mixPublication->Commit();
						data = std::move(candidateData);
						playback = std::move(candidatePlayback);
						authoring = std::move(candidateAuthoring);
					};
					if(!settings.empty()) {
						TerraEngine::Scene::TransactionalMutationBuilder builder("audio-control-" + std::to_string(batch.batchSequence),
							TerraEngine::Scene::TransactionOrigin::System, "AudioSnapshot source settings");
						builder.UpdateAudioSourceSettingsBatch(std::move(settings));
						const auto transaction = builder.Build();
						TerraEngine::Scene::TransactionCommitOutcome outcome;
						try {
							const auto result = TerraEngine::Scene::CommitTransaction(ObjectManager::instance(), transaction, outcome);
							if(result.status != TerraEngine::Scene::TransactionStatus::Committed)
								receipt = {batch.batchSequence, batch.requestedFrame, AudioControlStatus::Rejected,
									AudioControlRejection::StaleTarget, batch.correlationId, result.message};
						} catch(const TerraEngine::Scene::TransactionRestorationFailure&) {
							throw;
						} catch(...) {
							if(!outcome.committed) throw;
							// The preallocated outcome survives even allocation failure while
							// constructing the observer exception. Adopt and acknowledge the
							// committed logical batch before propagating any such failure.
							adoptCompanions();
							settleReceipt();
							throw;
						}
					}
					if(receipt.status != AudioControlStatus::Rejected) adoptCompanions();
				}
				// A source-only guard discards its unchanged mix candidate after
				// adoption, preserving the unrelated mix generation and values.
			}
			settleReceipt();
		}
		const auto mixSnapshot = mix.Snapshot();
		ApplyMixValues(data, mixSnapshot);
		authoring = AudioAuthoringSnapshot::Create(std::move(data));
	}
	void AudioSystem::PreparePhase() { prepared = PrepareAudioFrame(authoring, playback); }
	void AudioSystem::CommitPhase() {
		bool preservePlaybackPublication = false;
		if(backend) {
			AudioBackendFramePacket packet; packet.sequence = packetSequence + 1U; packet.timelineFrame = frame;
			auto candidateActiveVoices = activeVoices;
			auto candidateRegisteredClips = registeredClips;
			packet.masterGain = authoring.Data().masterVolume; packet.masterMuted = authoring.Data().masterMuted; packet.gamePaused = authoring.Data().gamePaused;
			// A DeviceLost observation makes the next complete frame a reconciliation
			// request. The provider retains no engine-private state: its normal packet
			// already describes every audible voice at the authoritative logical cursor.
			packet.requestRecovery = !deviceAvailable;
			std::unordered_set<std::string> clipsUsedThisFrame;
			std::unordered_set<AudioVoiceToken> voicesUsedThisFrame;
			for(const auto& voice : prepared.voices) {
				const auto seed = std::find_if(playback.begin(), playback.end(), [&](const auto& item) { return item.identity == voice.identity; });
				if(seed == playback.end() || (seed->state != AudioPlaybackState::Playing && seed->state != AudioPlaybackState::Paused))
					continue;
				// Virtual voices keep their engine-owned logical cursor but consume no
				// provider voice. An active voice that becomes muted, out of range, or
				// voice-limited is removed by the stale-voice pass below and will seek
				// to its current cursor if it later becomes audible again.
				if(voice.audibility != AudioAudibility::Audible)
					continue;
				// The prepared voice carries its playback generation, whereas the
				// authoring record identifies the durable source component only.
				const auto source = std::find_if(authoring.Data().sources.begin(), authoring.Data().sources.end(), [&](const auto& item) {
					return item.identity.objectId == voice.identity.objectId
						&& item.identity.componentGeneration == voice.identity.componentGeneration;
				});
				if(source == authoring.Data().sources.end() || !source->clipPayload || source->clipAssetId == TerraEngine::EMPTY_UUID
					|| source->clipGeneration == 0U)
					continue;
				const AudioVoiceToken token = VoiceTokenFor(voice.identity);
				voicesUsedThisFrame.insert(token);
				const std::string clipKey = ClipKey(voice.clipAssetId, voice.clipGeneration);
				clipsUsedThisFrame.insert(clipKey);
				if(!candidateRegisteredClips.contains(clipKey)) {
					packet.registrations.push_back({source->clipAssetId, source->clipGeneration, source->clipEncoding,
						source->clipSampleRate, source->clipChannels, source->clipFrames, source->clipChecksum, source->clipPayload});
					candidateRegisteredClips.emplace(clipKey, AudioClipRelease{source->clipAssetId, source->clipGeneration});
				}
				const auto previous = candidateActiveVoices.find(token);
				const bool clipChanged = previous != candidateActiveVoices.end()
					&& (previous->second.clipAssetId != voice.clipAssetId || previous->second.clipGeneration != voice.clipGeneration);
				if(clipChanged)
					packet.voices.push_back({token, previous->second.identity, previous->second.clipAssetId,
						previous->second.clipGeneration, 0U, 0.0F, 1.0F, false, true, false, false, {}});
				packet.voices.push_back({token, voice.identity, voice.clipAssetId, voice.clipGeneration,
					voice.logicalFrame, voice.gain, voice.pitch, previous == candidateActiveVoices.end() || clipChanged,
					false, voice.paused, voice.loop, voice.spatial});
				candidateActiveVoices[token] = {voice.identity, voice.clipAssetId, voice.clipGeneration};
			}
			for(auto voice = candidateActiveVoices.begin(); voice != candidateActiveVoices.end();) {
				if(voicesUsedThisFrame.contains(voice->first)) { ++voice; continue; }
				packet.voices.push_back({voice->first, voice->second.identity, voice->second.clipAssetId,
					voice->second.clipGeneration, 0U, 0.0F, 1.0F, false, true, false, false, {}});
				voice = candidateActiveVoices.erase(voice);
			}
			for(auto clip = candidateRegisteredClips.begin(); clip != candidateRegisteredClips.end();) {
				if(clipsUsedThisFrame.contains(clip->first)) { ++clip; continue; }
				packet.releases.push_back(clip->second);
				clip = candidateRegisteredClips.erase(clip);
			}
			const std::size_t previousActiveVoiceCount = activeVoices.size();
			const auto result = backend->SubmitFrame(packet);
			if(!result.Succeeded()) {
				// The old immutable playback publication remains authoritative until a
				// later complete packet succeeds; a failed provider call cannot expose
				// a partially accepted logical frame to diagnostics readers.
				PublishFailure("provider.submit", result.message);
				preservePlaybackPublication = true;
			} else {
				packetSequence = packet.sequence;
				activeVoices = std::move(candidateActiveVoices);
				registeredClips = std::move(candidateRegisteredClips);
				if(activeVoices.size() != previousActiveVoiceCount || !packet.registrations.empty() || !packet.releases.empty()) {
					TERRAENGINE_LOG(LogLevel::Debug, "Audio provider committed frame " << frame
						<< " with activeVoices=" << activeVoices.size()
						<< " registrations=" << packet.registrations.size()
						<< " releases=" << packet.releases.size());
				}
				std::array<AudioBackendObservation, 64U> observations{}; std::size_t requiredCount = 0U;
				const AudioResult observationResult = backend->PollObservations(observations, requiredCount);
				const std::size_t observed = std::min(observations.size(), requiredCount);
				for(std::size_t index = 0U; index < observed; ++index) {
					const auto& observation = observations[index];
					if(observation.kind == AudioBackendObservationKind::DeviceLost) { deviceAvailable = false; PublishFailure("provider.device_lost", observation.message); }
					else if(observation.kind == AudioBackendObservationKind::DeviceRecovered) deviceAvailable = true;
					else if(observation.kind == AudioBackendObservationKind::Overflow) PublishFailure("provider.observation_overflow", observation.message);
					else if(observation.kind == AudioBackendObservationKind::ProviderFailure || observation.kind == AudioBackendObservationKind::ClipRejected) PublishFailure("provider.observation_failure", observation.message);
				}
				if(!observationResult.Succeeded()) PublishFailure("provider.poll", observationResult.message);
			}
		}
		// A provider rejects an entire packet. Do not claim the newly prepared
		// playback became visible in that case; readers retain the last complete
		// immutable publication while diagnostics still expose the failure episode.
		if(!preservePlaybackPublication) {
			AudioPlaybackSnapshot candidate{frame, prepared.voices, playback};
			candidate.sourceBindings.reserve(authoring.Data().sources.size());
			for(const auto& source : authoring.Data().sources)
				candidate.sourceBindings.push_back({source.objectEntity, source.attachment});
			published = std::make_shared<const AudioPlaybackSnapshot>(std::move(candidate));
		}
		auto diagnostics = std::make_shared<AudioDiagnosticsSnapshot>();
		diagnostics->provider = descriptor;
		diagnostics->deviceAvailable = deviceAvailable;
		diagnostics->listenerCandidates = prepared.listenerCandidateCount;
		diagnostics->failures = failures;
		diagnostics->controlReceipts = controlReceipts;
		diagnostics->scheduler = {
			{"AudioSnapshot", "protected snapshot", "components/canonical world poses/listeners/clip generations/controls/pending Play reservations", "frame snapshot/receipts/source component values/scene transaction publication/source bindings/terminal ownership", "typed source commits and controls visible to prepare; frame-end mutations retain their declared flush", false, authoring.Data().sources.size()},
			{"AudioPrepare", "worker prepare", "immutable canonical-or-shared-local frame/prior playback", "listener-relative prepared plan", "no live mutation", true, authoring.Data().sources.size()},
			{"AudioCommit", "protected commit", "prepared plan/observations", "provider/snapshots", "visible after commit", false, prepared.voices.size()}
		};
		for(const auto& voice : prepared.voices) {
			if(voice.audibility == AudioAudibility::Audible) ++diagnostics->audibleVoices;
			else ++diagnostics->virtualVoices;
		}
		PublishAudioDiagnosticsSnapshot(std::move(diagnostics));
	}
	void AudioSystem::PublishFailure(std::string category, std::string message) {
		const auto found = std::find_if(failures.begin(), failures.end(), [&](const auto& episode) {
			return episode.category == category;
		});
		if(found == failures.end()) {
			TERRAENGINE_LOG(LogLevel::Error, "Audio failure [" << category << "]: " << message);
			failures.push_back({std::move(category), {}, frame, frame, 1U, std::move(message)});
		} else {
			found->lastFrame = frame;
			++found->count;
		}
	}
}
