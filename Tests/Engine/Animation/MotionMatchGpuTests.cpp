// MotionMatchGpuTests.cpp - Non-timed top-K/readback layout contracts.

#include "TestHarness.h"

#include "Animation/MotionMatching/MotionMatchGpu.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		AnimationGuid Guid(std::uint8_t value) { AnimationGuid result; result.value[0] = value; return result; }
		MotionDatabaseAsset Database() {
			MotionDatabaseAsset result; result.assetId = Guid(1U); result.runtimeGeneration = 1U;
			result.compatibilityFingerprint = 2U; result.sourceFingerprint = 3U; result.quantizationScales.fill(1.0F);
			for(std::uint64_t id = 1U; id <= 4U; ++id) { MotionDatabaseSample sample; sample.stableSampleId = id;
				sample.sourceClipId = id; sample.sourceTick = id; result.samples.push_back(sample); }
			result.featureSchemaFingerprint = ComputeMotionDatabaseSchemaFingerprint(result); return result;
		}
		MotionMatchQuery Query(const MotionDatabaseAsset& database) { MotionMatchQuery query;
			query.database = {database.assetId, database.runtimeGeneration}; query.expectedSchemaFingerprint = database.featureSchemaFingerprint;
			query.featureWeights.fill(1U); return query; }

		// The animation queue owns only the abstract submission/fence protocol. This
		// deterministic backend emulates renderer-owned command submission without
		// adding a production-only "complete now" escape hatch to the queue itself.
		class ManualMotionMatchBackend final : public MotionMatchGpuBackend {
		public:
			AnimationStatus Submit(const MotionMatchGpuSubmission& submission,
				MotionMatchGpuBackendTicket& ticket) noexcept override {
				try {
					Entry entry;
					entry.submission = submission;
					entry.ticket.submissionId = nextSubmission_++;
					entry.ticket.signalFenceValue = entry.ticket.submissionId * 10U;
					entry.ticket.readbackCapacity = submission.plan.topK;
					entry.ticket.readbackStrideBytes = sizeof(MotionMatchGpuCandidateRecord);
					ticket = entry.ticket;
					entries_.push_back(std::move(entry));
					return AnimationStatus::Queued;
				} catch(...) {
					return AnimationStatus::InternalError;
				}
			}

			AnimationStatus TryReadback(const MotionMatchGpuBackendTicket& ticket,
				std::span<MotionMatchGpuCandidateRecord> destination,
				std::uint32_t& candidateCount,
				std::uint64_t& completedFenceValue) noexcept override {
				Entry* entry = Find(ticket.submissionId);
				if(entry == nullptr) return AnimationStatus::StaleHandle;
				if(entry->cancelled) return AnimationStatus::Cancelled;
				if(!entry->ready) {
					completedFenceValue = ticket.signalFenceValue - 1U;
					return AnimationStatus::NotReady;
				}
				if(destination.size() < entry->records.size()) {
					return AnimationStatus::HardLimitExceeded;
				}
				std::copy(entry->records.begin(), entry->records.end(), destination.begin());
				candidateCount = static_cast<std::uint32_t>(entry->records.size());
				completedFenceValue = ticket.signalFenceValue;
				return AnimationStatus::Ok;
			}

			void Cancel(const MotionMatchGpuBackendTicket& ticket) noexcept override {
				if(Entry* entry = Find(ticket.submissionId); entry != nullptr) entry->cancelled = true;
			}

			void CompleteLatest(const MotionDatabaseAsset& database,
				const MotionMatchQuery& query) {
				MotionMatchCpuResult cpu;
				if(ScoreMotionMatchCpu(database, query, cpu) != AnimationStatus::Ok) return;
				Entry& entry = entries_.back();
				entry.records.clear();
				for(const MotionMatchCandidate& candidate : cpu.candidates) {
					entry.records.push_back(MakeMotionMatchGpuCandidateRecord(candidate));
				}
				entry.ready = true;
			}

			[[nodiscard]] const MotionMatchGpuSubmission& LatestSubmission() const {
				return entries_.back().submission;
			}

		private:
			struct Entry {
				MotionMatchGpuSubmission submission{};
				MotionMatchGpuBackendTicket ticket{};
				std::vector<MotionMatchGpuCandidateRecord> records;
				bool ready = false;
				bool cancelled = false;
			};

			Entry* Find(std::uint64_t submissionId) noexcept {
				auto found = std::find_if(entries_.begin(), entries_.end(),
					[submissionId](const Entry& entry) {
						return entry.ticket.submissionId == submissionId;
					});
				return found == entries_.end() ? nullptr : &*found;
			}

			std::vector<Entry> entries_;
			std::uint64_t nextSubmission_ = 1U;
		};

		std::string Read(std::string_view path) {
			// CTest starts the executable from the build tree, while focused local
			// runs often start at the repository root. Resolve immutable fixtures and
			// shader contracts from the configured source tree in both cases.
			std::ifstream input{
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / std::string(path),
				std::ios::binary};
			return {std::istreambuf_iterator<char>(input), {}};
		}
	}

	void RegisterMotionMatchGpuTests(TestRegistry& registry) {
		registry.Add("Animation.MotionMatch.GpuTopKIsBoundedAsyncAndGenerationSafe",
			[](TestContext& context) {
				const MotionDatabaseAsset database = Database(); const MotionMatchQuery query = Query(database);
				auto backend = std::make_shared<ManualMotionMatchBackend>();
				MotionMatchGpuQueue queue{backend}; MotionMatchGpuGeneration generation; MotionMatchGpuDispatchPlan plan;
				MotionMatchGpuReadback pendingReadback;
				context.Expect(queue.Submit(database, query, 1U, generation, plan) == AnimationStatus::Queued
					&& plan.topK == kMotionDatabaseTopKDefault
					&& plan.readbackBytes == kMotionDatabaseTopKDefault * sizeof(MotionMatchGpuCandidateRecord)
					&& queue.TryConsume(generation, pendingReadback) == AnimationStatus::NotReady
					&& queue.PollBackends({database.assetId, database.runtimeGeneration}, database.featureSchemaFingerprint) == AnimationStatus::NotReady
					&& queue.State(generation) == MotionMatchGpuRequestState::AwaitingFence,
					"Submitting resident work must return immediately and never wait for the current frame's top-K readback.");
				backend->CompleteLatest(database, query);
				MotionMatchGpuReadback readback;
				context.Expect(queue.PollBackends({database.assetId, database.runtimeGeneration}, database.featureSchemaFingerprint) == AnimationStatus::Ok
					&& queue.TryConsume(generation, readback) == AnimationStatus::Ok
					&& readback.state == MotionMatchGpuRequestState::ReadbackReady
					&& readback.completedFenceValue != 0U
					&& readback.candidates.size() <= kMotionDatabaseTopKDefault
					&& backend->LatestSubmission().query.features[0] == query.features.values[0],
					"A signalled renderer fence must expose only the copied, bounded top-K candidate payload.");
				MotionMatchGpuGeneration cancelled; MotionMatchGpuDispatchPlan cancelledPlan;
				static_cast<void>(queue.Submit(database, query, 1U, cancelled, cancelledPlan)); queue.Cancel(cancelled);
				context.Expect(queue.TryConsume(cancelled, readback) == AnimationStatus::Cancelled && readback.cancelled,
					"Cancelled requests must never produce a selectable candidate set.");
				MotionMatchGpuGeneration stale; MotionMatchGpuDispatchPlan stalePlan;
				static_cast<void>(queue.Submit(database, query, 1U, stale, stalePlan));
				context.Expect(queue.PollBackends({database.assetId, database.runtimeGeneration + 1U},
					database.featureSchemaFingerprint) == AnimationStatus::StaleHandle
					&& queue.TryConsume(stale, readback) == AnimationStatus::StaleHandle
					&& readback.state == MotionMatchGpuRequestState::Stale,
					"Hot-reloaded generations must cancel stale GPU work before its candidates become visible.");
			});

		registry.Add("Animation.MotionMatch.GpuFrozenManifestAndShaderBoundsAreNonTimed",
			[](TestContext& context) {
				const std::string manifest = Read("Tests/Fixtures/Animation/motion-matching/manifest.json");
				const std::string score = Read("Engine/Shaders/Animation/MotionMatchScore.comp.hlsl");
				const std::string topK = Read("Engine/Shaders/Animation/MotionMatchTopK.comp.hlsl");
				context.Expect(manifest.find("250000") != std::string::npos && manifest.find("100000") != std::string::npos
					&& manifest.find("48") != std::string::npos && manifest.find("32") != std::string::npos
					&& manifest.find("stable-tie") != std::string::npos
					&& manifest.find("hard-exclusion") != std::string::npos
					&& score.find("gExpectedDatabaseGeneration") != std::string::npos
					&& score.find("EXCLUSION_MISSING_REQUIRED_TAGS") != std::string::npos
					&& topK.find("MOTION_MAX_TOP_K = 256") != std::string::npos
					&& topK.find("MOTION_REDUCTION_TILE_SIZE = 1024") != std::string::npos,
					"The frozen workload and shaders must pin counts, generation checks, and top-K bounds without running timing acceptance.");
			});
	}
}
