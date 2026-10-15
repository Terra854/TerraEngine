// AudioMixConfiguration.cpp - validated immutable snapshots for flat mixing.

#include "AudioMixConfiguration.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <limits>
#include <type_traits>

namespace TerraEngine::Audio {
	namespace {
		std::string FoldName(std::string name) {
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
				return static_cast<char>(std::tolower(value));
			});
			return name;
		}
	}

	AudioMixConfiguration::AudioMixConfiguration() {
		AudioMixGroupState defaultGroup;
		defaultGroup.id[15] = 1U;
		defaultGroup.name = "Default";
		m_State.groups.push_back(std::move(defaultGroup));
	}

	AudioMixSnapshot AudioMixConfiguration::Snapshot() const {
		std::shared_lock lock(m_Mutex);
		return m_State;
	}

	AudioMixConfiguration::PreparedPublication::PreparedPublication(AudioMixConfiguration& owner,
		std::unique_lock<std::shared_mutex> lock, AudioMixSnapshot candidate) noexcept
		: m_Owner(&owner), m_Lock(std::move(lock)), m_Candidate(std::move(candidate)) {}

	AudioMixConfiguration::PreparedPublication::PreparedPublication(PreparedPublication&& other) noexcept
		: m_Owner(std::exchange(other.m_Owner, nullptr)), m_Lock(std::move(other.m_Lock)), m_Candidate(std::move(other.m_Candidate)) {}

	void AudioMixConfiguration::PreparedPublication::Commit() noexcept {
		static_assert(std::is_nothrow_swappable_v<AudioMixSnapshot>);
		if (!m_Owner) return;
		// Swapping owned vectors transfers their allocations rather than allocating
		// after scene state has committed. Retain the lock through companion adoption.
		using std::swap;
		swap(m_Owner->m_State, m_Candidate);
		m_Owner = nullptr;
	}

	std::optional<AudioMixConfiguration::PreparedPublication> AudioMixConfiguration::PrepareReplace(
		AudioMixSnapshot candidate, std::uint64_t expectedGeneration, std::string& diagnostic) {
		if (!Validate(candidate, diagnostic)) return std::nullopt;
		candidate.master.volume = NormalizeVolume(candidate.master.volume);
		for (auto& group : candidate.groups) group.volume = NormalizeVolume(group.volume);
		std::unique_lock lock(m_Mutex);
		if (expectedGeneration == 0U || expectedGeneration != m_State.generation) {
			diagnostic = "Audio mix changed after control preparation.";
			return std::nullopt;
		}
		if (m_State.generation == std::numeric_limits<std::uint64_t>::max()) {
			diagnostic = "Audio mix publication generation is exhausted.";
			return std::nullopt;
		}
		candidate.generation = m_State.generation + 1U;
		diagnostic.clear();
		return PreparedPublication(*this, std::move(lock), std::move(candidate));
	}

	float AudioMixConfiguration::NormalizeVolume(float volume) noexcept {
		if(!std::isfinite(volume))
			return 0.0F;
		return std::clamp(volume, 0.0F, 1.0F);
	}

	bool AudioMixConfiguration::Validate(const AudioMixSnapshot& candidate, std::string& diagnostic) {
		if(!std::isfinite(candidate.master.volume)) {
			diagnostic = "Master audio volume must be finite.";
			return false;
		}
		std::unordered_set<std::string> names;
		std::unordered_set<TerraEngine::UUID> identities;
		for(const AudioMixGroupState& group : candidate.groups) {
			if(group.id == TerraEngine::EMPTY_UUID || !identities.insert(group.id).second) {
				diagnostic = "Audio mix groups must have unique non-empty identities.";
				return false;
			}
			if(group.name.empty()) {
				diagnostic = "Audio mix group names cannot be empty.";
				return false;
			}
			if(!std::isfinite(group.volume)) {
				diagnostic = "Audio mix group volume must be finite.";
				return false;
			}
			if(!names.insert(FoldName(group.name)).second) {
				diagnostic = "Audio mix group names must be unique ignoring case.";
				return false;
			}
		}
		diagnostic.clear();
		return true;
	}

	bool AudioMixConfiguration::Replace(AudioMixSnapshot candidate, std::string& diagnostic) {
		if(!Validate(candidate, diagnostic))
			return false;
		candidate.master.volume = NormalizeVolume(candidate.master.volume);
		for(AudioMixGroupState& group : candidate.groups)
			group.volume = NormalizeVolume(group.volume);
		std::unique_lock lock(m_Mutex);
		candidate.generation = m_State.generation + 1U;
		m_State = std::move(candidate);
		return true;
	}

	bool AudioMixConfiguration::SetMasterVolume(float volume, std::string& diagnostic) {
		if(!std::isfinite(volume)) {
			diagnostic = "Master audio volume must be finite.";
			return false;
		}
		std::unique_lock lock(m_Mutex);
		m_State.master.volume = NormalizeVolume(volume);
		++m_State.master.generation;
		++m_State.generation;
		diagnostic.clear();
		return true;
	}

	bool AudioMixConfiguration::SetMasterMuted(bool muted) {
		std::unique_lock lock(m_Mutex);
		if(m_State.master.muted == muted)
			return true;
		m_State.master.muted = muted;
		++m_State.master.generation;
		++m_State.generation;
		return true;
	}

	bool AudioMixConfiguration::SetGroupVolume(const TerraEngine::UUID& id, float volume, std::string& diagnostic) {
		if(!std::isfinite(volume)) {
			diagnostic = "Audio mix group volume must be finite.";
			return false;
		}
		std::unique_lock lock(m_Mutex);
		const auto found = std::find_if(m_State.groups.begin(), m_State.groups.end(), [&](const AudioMixGroupState& group) {
			return group.id == id;
		});
		if(found == m_State.groups.end()) {
			diagnostic = "Audio mix group does not exist.";
			return false;
		}
		found->volume = NormalizeVolume(volume);
		++found->generation;
		++m_State.generation;
		diagnostic.clear();
		return true;
	}

	bool AudioMixConfiguration::SetGroupMuted(const TerraEngine::UUID& id, bool muted, std::string& diagnostic) {
		std::unique_lock lock(m_Mutex);
		const auto found = std::find_if(m_State.groups.begin(), m_State.groups.end(), [&](const AudioMixGroupState& group) {
			return group.id == id;
		});
		if(found == m_State.groups.end()) {
			diagnostic = "Audio mix group does not exist.";
			return false;
		}
		if(found->muted != muted) {
			found->muted = muted;
			++found->generation;
			++m_State.generation;
		}
		diagnostic.clear();
		return true;
	}

	bool AudioMixConfiguration::RenameGroup(const TerraEngine::UUID& id, std::string name, std::string& diagnostic) {
		std::unique_lock lock(m_Mutex);
		if(name.empty() || name.size() > 64U) {
			diagnostic = "Audio mix group names must contain between 1 and 64 characters.";
			return false;
		}
		const std::string foldedName = FoldName(name);
		const auto duplicate = std::find_if(m_State.groups.begin(), m_State.groups.end(), [&](const auto& group) {
			return group.id != id && FoldName(group.name) == foldedName;
		});
		if(duplicate != m_State.groups.end()) {
			diagnostic = "Audio mix group names must be unique ignoring case.";
			return false;
		}
		const auto found = std::find_if(m_State.groups.begin(), m_State.groups.end(), [&](const auto& group) {
			return group.id == id;
		});
		if(found == m_State.groups.end()) {
			diagnostic = "Audio mix group does not exist.";
			return false;
		}
		if(found->name != name) {
			found->name = std::move(name);
			++found->generation;
			++m_State.generation;
		}
		diagnostic.clear();
		return true;
	}
}
