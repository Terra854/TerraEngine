// AnimationAutomationTransactions.h - at-most-once animation mutation receipts.

#pragma once

#include "EditorAutomationTypes.h"

#include <functional>
#include <map>
#include <mutex>

namespace TerraEngine::Editor::Automation
{
	// A mutation carries the generation observed during preparation. Publication
	// is accepted only while that generation is still current. The payload is
	// retained only as a deterministic fingerprint, never as mutable live state.
	struct AnimationAutomationMutation
	{
		std::string operation;
		std::string idempotencyKey;
		std::string correlationId;
		std::uint64_t expectedGeneration = 0U;
		bool dryRun = false;
		nlohmann::json payload = nlohmann::json::object();
	};

	class AnimationAutomationTransactions final
	{
	public:
		// Execute validates a prepared mutation and retains its terminal receipt.
		// A matching retry receives the prior receipt with an explicit replay marker;
		// reusing a key for different work is an atomic conflict.
		[[nodiscard]] AutomationDispatchResult Execute(
			const AnimationAutomationMutation& request,
			std::uint64_t currentGeneration);
		// ExecuteWithCommit holds the receipt boundary while one short typed commit
		// runs. A failed callback is never retained, so retries cannot replay success
		// for work that was rejected by its owning runtime or authoring service.
		[[nodiscard]] AutomationDispatchResult ExecuteWithCommit(
			const AnimationAutomationMutation& request,
			std::uint64_t currentGeneration,
			const std::function<AutomationDispatchResult()>& commit);
		[[nodiscard]] std::size_t RetainedCount() const noexcept;
		// A service resets receipts when the open-project generation changes. Keys
		// from one project must never suppress work in a subsequently opened project.
		void Reset() noexcept;

	private:
		struct Receipt
		{
			std::string fingerprint;
			AutomationDispatchResult result;
		};

		static constexpr std::size_t kCapacity = 256U;
		[[nodiscard]] static std::string BuildFingerprint(const AnimationAutomationMutation& request);

		mutable std::mutex mutex_;
		std::map<std::string, Receipt> receipts_;
	};
}
