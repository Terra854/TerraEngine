#include "Audio/AudioSystem.h"
#include "Audio/Backend/MiniaudioBackend.h"
#include "Audio/Assets/AudioClipAsset.h"
#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <array>

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
	}
	AudioSystem& AudioSystem::Instance() { static AudioSystem system; return system; }
	void AudioSystem::Initialize() {
		std::scoped_lock lock(mutex); if(initialized) return; controls.Reset(); Backend::RegisterMiniaudioBackend(); auto& registry = Backend::GlobalAudioBackendRegistry(); registry.Freeze(); AudioResult createResult; backend = registry.Create(providerId, createResult);
		const auto found = registry.Find(providerId); if(found) descriptor = *found;
		AudioResult result = createResult; if(!backend || !(result = backend->Initialize({})).Succeeded()) { PublishFailure("provider.initialize", result.message.empty() ? "Audio is safely silent because the configured provider could not initialize." : result.message); backend.reset(); }
		deviceAvailable = backend != nullptr;
		initialized = true;
	}
	void AudioSystem::Update(float deltaTime) {
		std::scoped_lock lock(mutex); if(!initialized) return; ++frame;
		auto data = authoring.Data();
		std::vector<AudioAuthoringSourceRecord> liveSources;
		std::vector<AudioAuthoringListenerRecord> liveListeners;
		ObjectManager& objects = ObjectManager::instance();
		objects.forEachObjectWithComponents<AudioSourceComponent, Transform>([&](Object object, AudioSourceComponent& source, Transform& transform) {
			AudioAuthoringSourceRecord record;
			record.identity = {objects.getUUID(object), objects.getObjectComponentGeneration(object), 1U};
			record.clipAssetId = source.clipAssetId;
			record.position = {transform.position.x, transform.position.y, transform.position.z};
			record.volume = source.volume; record.pitch = source.pitch; record.minimumDistance = source.minDistance;
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
			if(source.playOnStart && std::none_of(playback.begin(), playback.end(), [&](const auto& seed) { return seed.identity.objectId == record.identity.objectId && seed.identity.componentGeneration == record.identity.componentGeneration; }))
				playback.push_back({record.identity, AudioPlaybackState::Playing, 0U, record.clipAssetId, record.clipGeneration});
			liveSources.push_back(std::move(record));
		});
		objects.forEachObjectWithComponents<AudioListenerComponent, Transform>([&](Object object, AudioListenerComponent& listener, Transform& transform) {
			const glm::vec3 forward = transform.rotation * glm::vec3(0.0F, 0.0F, -1.0F);
			liveListeners.push_back({objects.getUUID(object), objects.getObjectComponentGeneration(object),
				{transform.position.x, transform.position.y, transform.position.z}, {forward.x, forward.y, forward.z}, listener.active, listener.eligible});
		});
		if(!manualAuthoringSnapshot) { data.sources = std::move(liveSources); data.listeners = std::move(liveListeners); }
		data.frame = frame; data.deltaSeconds = deltaTime; data.masterVolume = mix.Snapshot().master.volume; data.masterMuted = mix.Snapshot().master.muted;
		authoring = AudioAuthoringSnapshot::Create(std::move(data)); SnapshotPhase(); PreparePhase(); CommitPhase();
	}
	void AudioSystem::Shutdown() { std::scoped_lock lock(mutex); controls.Stop(); if(backend) { (void)backend->Shutdown(); backend.reset(); } activeVoices.clear(); registeredClips.clear(); playback.clear(); controlReceipts.clear(); failures.clear(); authoring = AudioAuthoringSnapshot::Create({}); manualAuthoringSnapshot = false; frame = 0U; packetSequence = 0U; deviceAvailable = false; initialized = false; }
	std::string AudioSystem::SystemName() { return "AudioSystem"; }
	TerraEngine::Scheduling::SystemDeclaration AudioSystem::SchedulerDeclaration() {
		TerraEngine::Scheduling::SystemDeclaration declaration;
		declaration.name = SystemName(); declaration.phase = "Audio"; declaration.before = {"UISystem", "RenderSystem"};
		declaration.reads = {"ObjectManager", "RuntimeLayout", "AudioAuthoring"};
		declaration.writes = {"AudioPreparedFrame", "AudioProvider", "AudioDiagnostics"};
		declaration.mainThread = true; declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::Protected;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Partitionable attenuation, listener selection, cursor advancement, and voice ranking over immutable records.";
		declaration.protectedCommit = "Capture source/listener values, drain typed controls, submit one provider packet, and publish immutable snapshots.";
		declaration.protectedReason = "Scene capture and audio device calls cannot execute in worker-safe preparation.";
		declaration.profilingLabel = "Audio"; declaration.omitWhenNoFrameWorkloads = true; declaration.usesDefaultMetadata = false;
		return declaration;
	}
	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> AudioSystem::DeclareFrameWorkloads(float) {
		auto snapshot = TerraEngine::Scheduling::MakeSnapshotPhase("AudioSnapshot", SystemName(), {"Audio components", "Transforms", "Audio control queue"}, "AudioSnapshot"); snapshot.writes = {"Audio frame snapshot", "Audio receipts"}; snapshot.estimatedWorkItemCount = authoring.Data().sources.size();
		auto prepare = TerraEngine::Scheduling::MakeWorkerPreparePhase("AudioPrepare", SystemName(), {"Audio frame snapshot", "Playback snapshot"}, {"Audio prepared frame"}, "AudioPrepare"); prepare.estimatedWorkItemCount = authoring.Data().sources.size(); prepare.partitionPolicy = TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::DeterministicRanges;
		auto commit = TerraEngine::Scheduling::MakeProtectedCommitPhase("AudioCommit", SystemName(), {"Audio prepared frame", "Provider observations"}, {"Audio provider", "Playback snapshot", "Audio diagnostics"}, "Provider/device calls require the protected audio boundary.", "AudioCommit"); commit.estimatedWorkItemCount = prepared.voices.size(); return {std::move(snapshot), std::move(prepare), std::move(commit)};
	}
	AudioControlReceipt AudioSystem::SubmitControlBatch(AudioControlBatch batch) { return controls.Submit(std::move(batch)); }
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
	void AudioSystem::SetAuthoringSnapshot(AudioAuthoringSnapshot snapshot) { std::scoped_lock lock(mutex); authoring = std::move(snapshot); manualAuthoringSnapshot = true; }
	void AudioSystem::SetConfiguredProvider(std::string value) { std::scoped_lock lock(mutex); if(!initialized) providerId = std::move(value); }
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
		for(const auto& batch : controls.DrainEligible(frame)) {
			auto candidateData = data;
			auto candidatePlayback = playback;
			auto candidateMix = mix.Snapshot();
			bool mixChanged = false;
			AudioControlReceipt receipt{batch.batchSequence, batch.requestedFrame, AudioControlStatus::Applied,
				AudioControlRejection::None, batch.correlationId, {}};
			for(const auto& command : batch.commands) {
				const AudioSourceIdentity identity{command.source.objectId, command.source.componentGeneration, command.source.playbackGeneration};
				auto source = std::find_if(candidateData.sources.begin(), candidateData.sources.end(), [&](const auto& item) {
					return item.identity.objectId == identity.objectId && item.identity.componentGeneration == identity.componentGeneration;
				});
				auto seed = std::find_if(candidatePlayback.begin(), candidatePlayback.end(), [&](const auto& item) {
					return item.identity == identity;
				});
				const bool sourceCommand = command.kind <= AudioControlKind::SetSourcePitch;
				if(sourceCommand && (identity.objectId == TerraEngine::EMPTY_UUID || identity.componentGeneration == 0U
					|| source == candidateData.sources.end())) {
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
					if(seed == candidatePlayback.end()) {
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
				data = std::move(candidateData);
				playback = std::move(candidatePlayback);
				if(mixChanged) (void)mix.Replace(std::move(candidateMix), diagnostic);
			}
			controlReceipts.push_back(std::move(receipt));
			if(controlReceipts.size() > 256U) controlReceipts.erase(controlReceipts.begin());
		}
		const auto mixSnapshot = mix.Snapshot();
		data.masterVolume = mixSnapshot.master.volume;
		data.masterMuted = mixSnapshot.master.muted;
		for(auto& source : data.sources) {
			auto group = std::find_if(mixSnapshot.groups.begin(), mixSnapshot.groups.end(), [&](const auto& item) {
				return item.id == source.mixGroupId;
			});
			if(group == mixSnapshot.groups.end() && !mixSnapshot.groups.empty()) group = mixSnapshot.groups.begin();
			source.mixVolume = group == mixSnapshot.groups.end() ? 1.0F : group->volume;
			source.mixMuted = group != mixSnapshot.groups.end() && group->muted;
		}
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
			published = std::make_shared<const AudioPlaybackSnapshot>(AudioPlaybackSnapshot{frame, prepared.voices});
		}
		auto diagnostics = std::make_shared<AudioDiagnosticsSnapshot>();
		diagnostics->provider = descriptor;
		diagnostics->deviceAvailable = deviceAvailable;
		diagnostics->listenerCandidates = prepared.listenerCandidateCount;
		diagnostics->failures = failures;
		diagnostics->controlReceipts = controlReceipts;
		diagnostics->scheduler = {
			{"AudioSnapshot", "protected snapshot", "components/transforms/controls", "frame snapshot/receipts", "controls visible to prepare", false, authoring.Data().sources.size()},
			{"AudioPrepare", "worker prepare", "frame snapshot/prior playback", "prepared plan", "no live mutation", true, authoring.Data().sources.size()},
			{"AudioCommit", "protected commit", "prepared plan/observations", "provider/snapshots", "visible after commit", false, prepared.voices.size()}
		};
		for(const auto& voice : prepared.voices) {
			if(voice.audibility == AudioAudibility::Audible) ++diagnostics->audibleVoices;
			else ++diagnostics->virtualVoices;
		}
		PublishAudioDiagnosticsSnapshot(std::move(diagnostics));
	}
	void AudioSystem::PublishFailure(std::string category, std::string message) { const auto found = std::find_if(failures.begin(), failures.end(), [&](const auto& episode) { return episode.category == category; }); if(found == failures.end()) failures.push_back({std::move(category), {}, frame, frame, 1U, std::move(message)}); else { found->lastFrame = frame; ++found->count; } }
}
