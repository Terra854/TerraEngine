// ViewportTextureRegistry.h - Fence-backed Scene/Game texture registrations.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>

namespace TerraEngine::Editor
{
	enum class ViewportTextureKind { Scene, Game };

	struct ViewportTextureRegistration
	{
		ViewportTextureKind kind = ViewportTextureKind::Scene;
		void* descriptor = nullptr;
		void* imageView = nullptr;
		void* sampler = nullptr;
		std::uint64_t resourceGeneration = 0;
		std::uint64_t sessionIdentity = 0;
		std::uint64_t retireAfterSerial = 0;
	};

	class ViewportTextureRegistry
	{
	public:
		using RegisterCallback = std::function<void*(void*, void*)>;
		using ReleaseCallback = std::function<std::int32_t(void*)>;

		bool Refresh(ViewportTextureKind kind, void* imageView, void* sampler,
			std::uint64_t generation, std::uint64_t lastSubmittedSerial,
			const RegisterCallback& registerTexture);
		[[nodiscard]] void* ActiveDescriptor(ViewportTextureKind kind) const noexcept;
		void BeginDrawDataUsage();
		void MarkDrawDataUse(void* descriptor);
		void CommitDrawDataUsage(std::uint64_t submittedSerial);
		void AbandonDrawDataUsage();
		std::size_t ReleaseCompleted(std::uint64_t completedSerial, const ReleaseCallback& releaseTexture);
		void Shutdown(bool deviceLost, const ReleaseCallback& releaseTexture);

		[[nodiscard]] std::size_t RetiredCount() const noexcept { return retired_.size(); }

	private:
		ViewportTextureRegistration* Active(ViewportTextureKind kind) noexcept;
		const ViewportTextureRegistration* Active(ViewportTextureKind kind) const noexcept;
		std::optional<ViewportTextureRegistration> scene_;
		std::optional<ViewportTextureRegistration> game_;
		std::vector<ViewportTextureRegistration> retired_;
		std::unordered_set<void*> cpuDrawDataUses_;
		std::uint64_t nextSessionIdentity_ = 1;
	};
}
