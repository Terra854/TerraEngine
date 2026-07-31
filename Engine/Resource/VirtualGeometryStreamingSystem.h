// VirtualGeometryStreamingSystem.h - scheduler-owned virtual geometry page streaming lifecycle.

#pragma once

#include "Core/SystemBase.h"
#include "Resource/VirtualGeometryPagePublication.h"
#include "Resource/VirtualGeometryStreamingDiagnostics.h"
#include "Resource/VirtualGeometryStreamingPolicy.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace TerraEngine::Resource
{
	// VirtualGeometryStreamingSystem owns the boundary between immutable
	// prior-frame feedback, worker-safe planning, and protected page publication.
	// It intentionally stores values only; renderer and RHI owners provide already
	// verified candidates and consume the accepted immutable publication snapshot.
	class TERRAENGINE_API VirtualGeometryStreamingSystem final : public SystemsBase
	{
	public:
		static VirtualGeometryStreamingSystem& Instance();

		VirtualGeometryStreamingSystem(const VirtualGeometryStreamingSystem&) = delete;
		VirtualGeometryStreamingSystem& operator=(const VirtualGeometryStreamingSystem&) = delete;

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		// Callers submit copied prior-frame feedback. This method never publishes
		// pages; workers consume the private queue during StreamingPrepare.
		void SubmitPriorFrameFeedback(std::vector<VirtualGeometryStreamingFeedback> feedback);
		// Only verified decode/upload producers may enqueue these complete values.
		// The queue is private until PagePublish validates the complete batch.
		void SubmitCompletedPublicationCandidates(
			std::vector<VirtualGeometryPagePublicationCandidate> candidates);
		[[nodiscard]] VirtualGeometryStreamingPlan LastPreparedPlan() const;
		[[nodiscard]] std::vector<VirtualGeometryPagePublication> PublishedPages() const;
		[[nodiscard]] VirtualGeometryPhysicalPageResidencySnapshot ResidencySnapshot() const;
		[[nodiscard]] VirtualGeometryStreamingDiagnostics DiagnosticsSnapshot() const;

	private:
		VirtualGeometryStreamingSystem() = default;

		// The frame counters order value-only private preparation before its single
		// protected commit. They are not page generations; candidates carry those.
		std::atomic<std::uint64_t> preparedFrameCount{ 0u };
		std::atomic<std::uint64_t> publishedFrameCount{ 0u };
		std::atomic<bool> initialized{ false };
		mutable std::mutex candidateMutex;
		std::vector<VirtualGeometryStreamingFeedback> priorFrameFeedback;
		std::vector<VirtualGeometryPagePublicationCandidate> pendingPublicationCandidates;
		std::vector<VirtualGeometryPagePublicationCandidate> preparedPublicationCandidates;
		VirtualGeometryStreamingPlan lastPreparedPlan;
		VirtualGeometryPagePublicationStore publicationStore;
		VirtualGeometryPhysicalPageResidencyStore residencyStore;
		VirtualGeometryStreamingDiagnostics diagnostics;
	};
} // namespace TerraEngine::Resource
