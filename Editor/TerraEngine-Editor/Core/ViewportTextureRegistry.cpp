// ViewportTextureRegistry.cpp - Descriptor lifetime tracking implementation.

#include "ViewportTextureRegistry.h"

#include <algorithm>

namespace TerraEngine::Editor
{
	ViewportTextureRegistration* ViewportTextureRegistry::Active(ViewportTextureKind kind) noexcept
	{
		auto& registration = kind == ViewportTextureKind::Scene ? scene_ : game_;
		return registration ? &*registration : nullptr;
	}

	const ViewportTextureRegistration* ViewportTextureRegistry::Active(ViewportTextureKind kind) const noexcept
	{
		const auto& registration = kind == ViewportTextureKind::Scene ? scene_ : game_;
		return registration ? &*registration : nullptr;
	}

	bool ViewportTextureRegistry::Refresh(ViewportTextureKind kind, void* imageView, void* sampler,
		std::uint64_t generation, std::uint64_t lastSubmittedSerial,
		const RegisterCallback& registerTexture)
	{
		ViewportTextureRegistration* current = Active(kind);
		if (current && current->imageView == imageView && current->sampler == sampler
			&& current->resourceGeneration == generation) return true;
		if (imageView == nullptr || sampler == nullptr || !registerTexture) return false;

		// Register before retiring the active descriptor. Transient pool failure
		// therefore leaves the last valid descriptor/resource pair published.
		void* descriptor = registerTexture(imageView, sampler);
		if (descriptor == nullptr) return false;

		ViewportTextureRegistration replacement{
			kind, descriptor, imageView, sampler, generation,
			nextSessionIdentity_++, lastSubmittedSerial
		};
		auto& activeSlot = kind == ViewportTextureKind::Scene ? scene_ : game_;
		if (activeSlot) {
			activeSlot->retireAfterSerial = std::max(activeSlot->retireAfterSerial, lastSubmittedSerial);
			retired_.push_back(*activeSlot);
		}
		activeSlot = replacement;
		return true;
	}

	void* ViewportTextureRegistry::ActiveDescriptor(ViewportTextureKind kind) const noexcept
	{
		const ViewportTextureRegistration* registration = Active(kind);
		return registration ? registration->descriptor : nullptr;
	}

	void ViewportTextureRegistry::BeginDrawDataUsage()
	{
		cpuDrawDataUses_.clear();
	}

	void ViewportTextureRegistry::MarkDrawDataUse(void* descriptor)
	{
		if (descriptor != nullptr) cpuDrawDataUses_.insert(descriptor);
	}

	void ViewportTextureRegistry::CommitDrawDataUsage(std::uint64_t submittedSerial)
	{
		auto extend = [&](ViewportTextureRegistration& registration) {
			if (cpuDrawDataUses_.contains(registration.descriptor)) {
				registration.retireAfterSerial = std::max(registration.retireAfterSerial, submittedSerial);
			}
		};
		if (scene_) extend(*scene_);
		if (game_) extend(*game_);
		for (ViewportTextureRegistration& registration : retired_) extend(registration);
		cpuDrawDataUses_.clear();
	}

	void ViewportTextureRegistry::AbandonDrawDataUsage()
	{
		cpuDrawDataUses_.clear();
	}

	std::size_t ViewportTextureRegistry::ReleaseCompleted(
		std::uint64_t completedSerial, const ReleaseCallback& releaseTexture)
	{
		std::size_t released = 0;
		auto iterator = retired_.begin();
		while (iterator != retired_.end()) {
			if (iterator->retireAfterSerial > completedSerial) {
				++iterator;
				continue;
			}
			if (!releaseTexture || releaseTexture(iterator->descriptor) == 0) {
				iterator = retired_.erase(iterator);
				++released;
			} else {
				++iterator;
			}
		}
		return released;
	}

	void ViewportTextureRegistry::Shutdown(bool deviceLost, const ReleaseCallback& releaseTexture)
	{
		// Device-loss teardown owns the descriptor pool; Vulkan descriptor frees
		// are unsafe after the terminal latch and are intentionally skipped.
		if (!deviceLost && releaseTexture) {
			if (scene_) releaseTexture(scene_->descriptor);
			if (game_) releaseTexture(game_->descriptor);
			for (const ViewportTextureRegistration& registration : retired_) {
				releaseTexture(registration.descriptor);
			}
		}
		scene_.reset();
		game_.reset();
		retired_.clear();
		cpuDrawDataUses_.clear();
	}
}
