// AnimationAssetWindowSupport.h - shared checked loading for animation windows.
//
// The Project Browser exposes paths while the runtime formats expose typed,
// validated records. This helper bridges those two contracts without retaining
// mapped pointers or re-reading an unchanged asset on every ImGui frame.

#pragma once

#include "../Animation/AnimationAssetAuthoringDecode.h"
#include "../Subsystems/MainEditorSubsystem.h"

#include "Engine/Animation/AnimationTypes.h"
#include "Engine/Resource/ResourceManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace TerraEngine::Editor::Animation {
	template<typename Asset>
	class CookedAnimationAssetSelection final {
	public:
		using Decoder = TerraEngine::Animation::AnimationStatus (*)(
			std::span<const std::uint8_t>,
			TerraEngine::Animation::AnimationCompatibilityFingerprint,
			Asset&);

		// Synchronize performs only bounded file parsing after the selected path or
		// its write time changes. A corrupt reimport leaves the prior complete copy
		// available and marks it stale, matching runtime last-known-good behavior.
		void Synchronize(
			MainEditorSubsystem* owner,
			std::string_view requiredExtension,
			Decoder decoder) {
			if(owner == nullptr || decoder == nullptr) {
				return;
			}

			const std::optional<std::filesystem::path> selected = owner->GetSelectedAsset();
			if(!selected || LowerExtension(selected->extension().string()) != requiredExtension) {
				selectedForWindow_ = false;
				path_.clear();
				asset_.reset();
				message_.clear();
				attempted_ = false;
				stale_ = false;
				return;
			}

			selectedForWindow_ = true;
			const std::filesystem::path normalized = selected->lexically_normal();
			std::error_code writeError;
			const std::filesystem::file_time_type writeTime =
				std::filesystem::last_write_time(normalized, writeError);
			const bool hasWriteTime = !writeError;
			const bool pathChanged = path_ != normalized;
			const bool timeChanged = hasWriteTime_ != hasWriteTime
				|| (hasWriteTime && writeTime_ != writeTime);
			if(attempted_ && !pathChanged && !timeChanged) {
				return;
			}

			if(pathChanged) {
				// A prior asset must never be shown under a different selected path.
				asset_.reset();
			}
			path_ = normalized;
			writeTime_ = writeTime;
			hasWriteTime_ = hasWriteTime;
			attempted_ = true;
			stale_ = false;

			const std::vector<std::uint8_t> bytes =
				ResourceManager::Instance().ReadFileBytes(normalized);
			if(bytes.empty()) {
				message_ = "The selected cooked asset is empty or could not be read.";
				stale_ = asset_.has_value();
				return;
			}

			Asset candidate;
			const TerraEngine::Animation::AnimationStatus status =
				DeserializeAnimationAssetForAuthoring(
					std::span<const std::uint8_t>(bytes.data(), bytes.size()),
					candidate,
					decoder);
			if(status != TerraEngine::Animation::AnimationStatus::Ok) {
				message_ = "Checked decoding rejected the selected asset: ";
				message_ += TerraEngine::Animation::ToString(status);
				stale_ = asset_.has_value();
				return;
			}

			asset_ = std::move(candidate);
			message_ = "Checked cooked asset loaded.";
		}

		[[nodiscard]] const Asset* Get() const noexcept {
			return asset_ ? &*asset_ : nullptr;
		}
		[[nodiscard]] bool IsSelectedForWindow() const noexcept { return selectedForWindow_; }
		[[nodiscard]] bool IsStale() const noexcept { return stale_; }
		[[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }
		[[nodiscard]] const std::string& Message() const noexcept { return message_; }

	private:
		[[nodiscard]] static std::string LowerExtension(std::string extension) {
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
			return extension;
		}

		std::optional<Asset> asset_;
		std::filesystem::path path_;
		std::filesystem::file_time_type writeTime_{};
		std::string message_;
		bool hasWriteTime_ = false;
		bool attempted_ = false;
		bool selectedForWindow_ = false;
		bool stale_ = false;
	};
}
