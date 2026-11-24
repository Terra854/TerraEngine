// GraphicsSettingsController.h - ordered requests and protected publications.
#pragma once

#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"

#include <memory>

namespace TerraEngine::Tests { struct GraphicsSettingsControllerTestAccess; }

namespace TerraEngine::Renderer::GraphicsSettings
{
	// Producers only enqueue owned records. Prepare examines copied context and
	// builds an isolated candidate. The renderer calls Publish at its declared
	// protected boundary after any required backend resources are ready.
	class GraphicsSettingsController
	{
	public:
		GraphicsSettingsController();
		~GraphicsSettingsController();
		GraphicsSettingsController(const GraphicsSettingsController&) = delete;
		GraphicsSettingsController& operator=(const GraphicsSettingsController&) = delete;

		GraphicsRequestReceipt Enqueue(GraphicsSettingsRequest request);
		GraphicsRequestReceipt Reject(GraphicsSettingsRequest request, GraphicsRequestReason reason);
		std::optional<GraphicsPreparedCandidate> Prepare(const GraphicsPreparationContext& context);
		// Only the protected resource owner supplies a nonzero preparation proof.
		// A failed known view/mode qualification can report Unsupported separately
		// from allocation or pipeline creation failure without publishing changes.
		GraphicsRequestReceipt Publish(GraphicsPreparedCandidate candidate,
			const GraphicsPreparationContext& context, bool resourcesReady = true,
			std::uint64_t postProcessResourceGeneration = 0,
			GraphicsRequestReason resourceFailureReason = GraphicsRequestReason::PreparationFailed);
		// Full lifetime changes cancel backend work. Committed camera overrides
		// survive a resource rebuild while their project/session/target lives.
		void Invalidate(const GraphicsPreparationContext& context,
			std::span<const GraphicsCameraTarget> liveCameraOwners = {});
		// Retire requests for a replaced output without erasing the owner's
		// committed settings or its externally visible receipt history.
		void CancelPending(GraphicsRequestReason reason = GraphicsRequestReason::StaleLifetime);
		// Mirror native settings written through pre-existing protected paths. The
		// full value set is prepared before replacing any effective root.
		void RefreshNativeValues(const GraphicsPreparationContext& context,
			std::span<const GraphicsSettingValue> values, QualityPreset preset,
			std::uint64_t verifiedPostProcessGeneration = 0);
		void UpdateViewExtent(const GraphicsTarget& target, std::uint32_t width, std::uint32_t height);
		GraphicsSettingsSnapshot ProjectCandidateForView(const GraphicsPreparedCandidate& candidate,
			const GraphicsTarget& target) const;
		std::optional<GraphicsRequestReceipt> GetReceipt(GraphicsRequestId requestId) const;
		std::shared_ptr<const GraphicsSettingsSnapshot> Snapshot() const;
		// Camera patches apply across its live views. Query an exact view when
		// several exist; a missing or ambiguous effective target returns null.
		std::shared_ptr<const GraphicsSettingsSnapshot> Snapshot(const GraphicsTarget& target) const;
		// Internal immutable camera-owned values survive a temporary absence of
		// rendered views. This is not an effective/public view query or render proof.
		std::shared_ptr<const GraphicsSettingsSnapshot> CameraOwnerSnapshot(const GraphicsCameraTarget& camera) const;
		bool CameraOverrides(const GraphicsCameraTarget& camera, GraphicsSettingId id) const;
		bool HasPendingWork() const;

	private:
		// Tests inject allocation failure for this controller only. The callback
		// runs before candidate work; normal callers leave both fields empty.
		friend struct TerraEngine::Tests::GraphicsSettingsControllerTestAccess;
		void (*allocationCheckpoint)(void*, const char*) = nullptr;
		void* allocationCheckpointContext = nullptr;
		void AllocationCheckpoint(const char* stage) const
		{
			if(allocationCheckpoint) allocationCheckpoint(allocationCheckpointContext, stage);
		}
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
