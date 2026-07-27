// AnimationOverlays.cpp - bounded worker preparation and protected publication.

#include "AnimationOverlays.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace TerraEngine::Editor::Animation {
	namespace {
		[[nodiscard]] bool IsFinite(AnimationOverlayVec3 value) noexcept {
			return std::isfinite(value.x)
				&& std::isfinite(value.y)
				&& std::isfinite(value.z);
		}

		[[nodiscard]] bool IsFiniteColor(AnimationOverlayColor value) noexcept {
			return std::isfinite(value.red)
				&& std::isfinite(value.green)
				&& std::isfinite(value.blue)
				&& std::isfinite(value.alpha)
				&& value.red >= 0.0F && value.red <= 1.0F
				&& value.green >= 0.0F && value.green <= 1.0F
				&& value.blue >= 0.0F && value.blue <= 1.0F
				&& value.alpha >= 0.0F && value.alpha <= 1.0F;
		}

		[[nodiscard]] bool IsKnownType(AnimationOverlayType type) noexcept {
			return type >= AnimationOverlayType::Skeleton
				&& type <= AnimationOverlayType::VirtualGeometryCoverage;
		}

		[[nodiscard]] bool IsValidPrimitive(
			const AnimationOverlayPrimitive& primitive) noexcept {
			return IsKnownType(primitive.type)
				&& primitive.stableId != 0U
				&& IsFinite(primitive.start)
				&& IsFinite(primitive.end)
				&& IsFiniteColor(primitive.color)
				&& !primitive.label.empty()
				&& primitive.label.size() <= 256U
				&& primitive.accessibilityText.size() <= 768U;
		}
	}

	AnimationOverlays::~AnimationOverlays() {
		Clear();
	}

	void AnimationOverlays::Configure(AnimationOverlayOptions options) noexcept {
		try {
			std::scoped_lock lock(mutex_);
			options.enabledKinds &= kAllAnimationOverlayKinds;
			options.maximumPrimitives = std::clamp<std::size_t>(
				options.maximumPrimitives, 1U, kMaximumAnimationOverlayPrimitives);
			options_ = options;
			if(settingsGeneration_ != std::numeric_limits<std::uint64_t>::max()) {
				++settingsGeneration_;
			}
		} catch(...) {
			// Configuration owns no renderer handle or runtime state. Keeping the
			// prior complete options is safer than exposing a partial update.
		}
	}

	AnimationOverlayStatus AnimationOverlays::Prepare(
		AnimationOverlaySourceHeader source,
		const AnimationOverlayCollector& collector,
		AnimationOverlayPreparedPacket& output) const noexcept {
		try {
			AnimationOverlayOptions options;
			std::uint64_t settingsGeneration = 0U;
			{
				std::scoped_lock lock(mutex_);
				options = options_;
				settingsGeneration = settingsGeneration_;
			}

			AnimationOverlayPreparedPacket staged;
			staged.expectedSettingsGeneration = settingsGeneration;
			staged.snapshot.settingsGeneration = settingsGeneration;
			staged.snapshot.source = source;
			staged.snapshot.options = options;
			if(!options.enabled) {
				staged.snapshot.status = AnimationOverlayStatus::Disabled;
				staged.snapshot.accessibilityText =
					"Animation overlays disabled; primitive collection is inactive.";
				output = std::move(staged);
				return AnimationOverlayStatus::Disabled;
			}
			if(!options.expanded) {
				staged.snapshot.status = AnimationOverlayStatus::Collapsed;
				staged.snapshot.accessibilityText =
					"Animation overlays collapsed; primitive collection is inactive.";
				output = std::move(staged);
				return AnimationOverlayStatus::Collapsed;
			}
			if(!options.rendererAvailable) {
				staged.snapshot.status = AnimationOverlayStatus::RendererUnavailable;
				staged.snapshot.accessibilityText =
					"Animation overlays: renderer unavailable; no render primitives were collected.";
				output = std::move(staged);
				return AnimationOverlayStatus::RendererUnavailable;
			}
			if(source.sourceGeneration == 0U || source.rendererGeneration == 0U
				|| options.enabledKinds == 0U || !collector) {
				return AnimationOverlayStatus::InvalidArgument;
			}

			std::vector<AnimationOverlayPrimitive> collected;
			staged.snapshot.instrumentation.collectionRequests = 1U;
			const AnimationOverlayStatus collectionStatus = collector(
				options.enabledKinds,
				options.maximumPrimitives,
				collected);
			if(collectionStatus != AnimationOverlayStatus::Ok) {
				return collectionStatus;
			}
			if(collected.size() > kMaximumAnimationOverlayCollectorPrimitives) {
				return AnimationOverlayStatus::CapacityExceeded;
			}

			staged.snapshot.primitives.reserve(
				std::min(collected.size(), options.maximumPrimitives));
			for(AnimationOverlayPrimitive& primitive : collected) {
				// Accessibility text is optional at the producer boundary. Derive the
				// stable fallback before validation so a well-formed primitive is not
				// rejected merely because the collector relied on its visible label.
				if(primitive.accessibilityText.empty()) {
					primitive.accessibilityText = std::string(ToString(primitive.type))
						+ ": " + primitive.label;
				}
				if(!IsValidPrimitive(primitive)) {
					return AnimationOverlayStatus::InvalidArgument;
				}
				if((options.enabledKinds & AnimationOverlayTypeMask(primitive.type)) == 0U) {
					++staged.snapshot.instrumentation.filteredPrimitives;
					continue;
				}
				if(staged.snapshot.primitives.size() == options.maximumPrimitives) {
					++staged.snapshot.instrumentation.droppedPrimitives;
					continue;
				}
				staged.snapshot.primitives.push_back(std::move(primitive));
				++staged.snapshot.instrumentation.copiedPrimitives;
			}

			staged.snapshot.status = AnimationOverlayStatus::Ok;
			staged.snapshot.accessibilityText =
				"Animation Scene View overlays expanded with immutable copied primitives.";
			output = std::move(staged);
			return AnimationOverlayStatus::Ok;
		} catch(...) {
			return AnimationOverlayStatus::InternalError;
		}
	}

	AnimationOverlayStatus AnimationOverlays::Publish(
		const AnimationOverlayPreparedPacket& prepared) noexcept {
		try {
			std::scoped_lock lock(mutex_);
			if(prepared.schemaVersion != kAnimationOverlaySchemaVersion
				|| prepared.snapshot.schemaVersion != kAnimationOverlaySchemaVersion) {
				return AnimationOverlayStatus::InvalidArgument;
			}
			if(prepared.expectedSettingsGeneration != settingsGeneration_) {
				return AnimationOverlayStatus::Stale;
			}
			AnimationOverlaySnapshot staged = prepared.snapshot;
			if(publicationGeneration_ != std::numeric_limits<std::uint64_t>::max()) {
				++publicationGeneration_;
			}
			staged.publicationGeneration = publicationGeneration_;
			snapshot_ = std::move(staged);
			return snapshot_.status;
		} catch(...) {
			return AnimationOverlayStatus::InternalError;
		}
	}

	void AnimationOverlays::Clear() noexcept {
		try {
			std::scoped_lock lock(mutex_);
			options_.enabled = false;
			if(settingsGeneration_ != std::numeric_limits<std::uint64_t>::max()) {
				++settingsGeneration_;
			}
			if(publicationGeneration_ != std::numeric_limits<std::uint64_t>::max()) {
				++publicationGeneration_;
			}
			snapshot_ = {};
			snapshot_.status = AnimationOverlayStatus::Disabled;
			snapshot_.settingsGeneration = settingsGeneration_;
			snapshot_.publicationGeneration = publicationGeneration_;
			snapshot_.options = options_;
			snapshot_.accessibilityText =
				"Animation overlays cleared; primitive collection is inactive.";
		} catch(...) {
		}
	}

	AnimationOverlaySnapshot AnimationOverlays::Snapshot() const noexcept {
		try {
			std::scoped_lock lock(mutex_);
			return snapshot_;
		} catch(...) {
			return {};
		}
	}
}
