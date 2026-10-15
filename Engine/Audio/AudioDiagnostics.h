// AudioDiagnostics.h - immutable copies exposed to diagnostics readers.

#pragma once

#include "Audio/AudioCommands.h"
#include "Audio/AudioPreparationPlan.h"

#include <memory>
#include <mutex>
#include <vector>

namespace TerraEngine::Audio {
	struct AudioFailureEpisode { std::string category; AudioSourceIdentity source; AudioFrame firstFrame = 0U, lastFrame = 0U; std::uint64_t count = 0U; std::string message; };
	struct AudioSchedulerDeclaration { std::string workload, phase, reads, writes, boundary; bool workerSafe = false; std::uint64_t estimatedItems = 0U; };
	// Logical status and prepared voices belong to the same provider-accepted
	// publication, including when a failed packet preserves the previous frame.
	struct AudioPlaybackSnapshot { AudioFrame frame = 0U; std::vector<AudioPreparedVoice> voices; std::vector<AudioPlaybackSeed> logicalPlayback; std::vector<AudioSourceBinding> sourceBindings; };
	struct AudioSourcePlaybackObservation { AudioFrame frame = 0U; AudioPreparedVoice voice; AudioPlaybackState state = AudioPlaybackState::Stopped; };
	struct AudioDiagnosticsSnapshot { AudioProviderDescriptor provider; bool deviceAvailable = false; std::uint32_t listenerCandidates = 0U, audibleVoices = 0U, virtualVoices = 0U; std::vector<AudioFailureEpisode> failures; std::vector<AudioControlReceipt> controlReceipts; std::vector<AudioSchedulerDeclaration> scheduler; };
	TERRAENGINE_API void PublishAudioDiagnosticsSnapshot(std::shared_ptr<const AudioDiagnosticsSnapshot> snapshot);
	TERRAENGINE_API std::shared_ptr<const AudioDiagnosticsSnapshot> AcquireAudioDiagnosticsSnapshot();
}
