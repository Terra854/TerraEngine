// Real sealed-frame validation and V1 writer/reader/codec drive these cases.
// Tests mutate only the production-generated CBOR map, never recreate serializers.
#include "TestHarness.h"
#include "Profiling/ProfilerCapture.h"
#include "Profiling/ProfilerJournalV1.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "External/zstd/lib/zstd.h"
#include <tuple>

namespace TerraEngine::Tests {
	namespace {
		using namespace Profiling;
		struct JournalWorkspace {
			std::filesystem::path root;
			JournalWorkspace() {
				static std::atomic_uint64_t next{0}; const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
				for(unsigned attempt = 0; attempt < 100; ++attempt) {
					root = std::filesystem::temp_directory_path() / ("TerraJournalEvidence-" + std::to_string(seed) + "-" + std::to_string(next++));
					if(std::filesystem::create_directory(root)) return;
				}
				throw std::runtime_error("Could not exclusively reserve journal workspace.");
			}
			~JournalWorkspace() { std::error_code error; std::filesystem::remove_all(root, error); }
		};
		auto ViewFacts(const ProfilerRenderOptimizationViewSummary& v) { return std::tuple{v.viewType, v.frameGeneration, v.topologyGeneration, v.viewGeneration, v.rendererGeneration, v.topologyBuildNanoseconds, v.dynamicMaterializationNanoseconds, v.instanceStreamUploadNanoseconds, v.groupedCommandRecordingNanoseconds, v.visibleInstanceCount, v.groupedInstanceCount, v.fallbackInstanceCount, v.groupedSubmissionCount, v.fallbackSubmissionCount, v.avoidedSubmissionCount, v.instanceStreamBytes, v.instanceStreamCapacityBytes, v.topologyReuseHits, v.topologyInvalidations, v.viewEvidenceReuseHits, v.viewEvidenceInvalidations, v.rejectedIncompleteKeyCount, v.rejectedUnsupportedVariantCount, v.rejectedOutOfRangeCount, v.rejectedStaleGenerationCount, v.fallbackAllocationFailureCount, v.fallbackRecoveryCount, v.valid}; }
		auto DurationFacts(const TelemetryDuration& d) { return std::tuple{d.availability, d.nanoseconds, d.unavailableReason}; }
		auto WorkFacts(const WorkloadExecutionRecord& w) {
			return std::tuple{w.frameId, w.descriptorId, w.descriptorGeneration, w.ownerSystem, w.phase, w.workloadName, w.activationPredicateKey, w.active, w.ready, w.selectedPath, w.status, w.workerIdentityAvailable, w.workerId, w.queueDepthAtEnqueue, w.queueDepthAtStart, w.dispatchCount, w.usefulWorkItemCount, w.dependencyWave, w.completionSequence, w.partitionIndex, w.partitionCount, w.itemBegin, w.itemEnd, w.sameFrameVisibilityRequired, DurationFacts(w.queueDuration), DurationFacts(w.runDuration), DurationFacts(w.coordinatorWaitDuration), DurationFacts(w.ownershipWaitDuration), DurationFacts(w.flushDuration), DurationFacts(w.publicationDuration)};
		}
		ProfilerFrameRecord EvidenceFrame() {
			ProfilerFrameRecord frame; frame.frameIndex = 17; frame.durationMs = 16;
			ProfilerFramePublicationEvidence e; e.frameGeneration = 91; e.sceneGeneration = 4; e.viewCount = 2;
			for(unsigned i = 0; i < 2; ++i) {
				auto& v = e.views[i]; v.viewType = i; v.frameGeneration = 91; v.topologyGeneration = 3 + i;
				v.viewGeneration = 7 + i; v.rendererGeneration = 4; v.valid = true;
				v.topologyBuildNanoseconds = 100 + i; v.dynamicMaterializationNanoseconds = 200 + i;
				v.instanceStreamUploadNanoseconds = 300 + i; v.groupedCommandRecordingNanoseconds = 400 + i;
				v.visibleInstanceCount = 20 + i; v.groupedInstanceCount = 10; v.fallbackInstanceCount = 10 + i;
				v.groupedSubmissionCount = 2; v.fallbackSubmissionCount = 3 + i; v.avoidedSubmissionCount = 8;
				v.instanceStreamBytes = 512 + i; v.instanceStreamCapacityBytes = 1024;
				v.topologyReuseHits = 5 + i; v.topologyInvalidations = 7 + i; v.viewEvidenceReuseHits = 9 + i; v.viewEvidenceInvalidations = 11 + i;
				v.rejectedIncompleteKeyCount = 13 + i; v.rejectedUnsupportedVariantCount = 15 + i; v.rejectedOutOfRangeCount = 17 + i;
				v.rejectedStaleGenerationCount = 19 + i; v.fallbackAllocationFailureCount = 21 + i; v.fallbackRecoveryCount = 23 + i;
				WorkloadExecutionRecord w; w.frameId = "frame17"; w.descriptorId = "descriptor" + std::to_string(i); w.descriptorGeneration = 29 + i;
				w.ownerSystem = "ReviewSystem"; w.phase = "Prepare"; w.workloadName = "partition" + std::to_string(i); w.activationPredicateKey = "active";
				w.active = true; w.ready = true; w.selectedPath = WorkloadExecutionPath::Worker; w.status = WorkloadExecutionStatus::Completed;
				w.workerIdentityAvailable = i == 0; w.workerId = i; w.queueDepthAtEnqueue = 31 + i; w.queueDepthAtStart = 33 + i;
				w.dispatchCount = 1; w.usefulWorkItemCount = 4; w.dependencyWave = 2 + i; w.completionSequence = 3 + i;
				w.partitionIndex = i; w.partitionCount = 2; w.itemBegin = i * 4; w.itemEnd = (i + 1) * 4; w.sameFrameVisibilityRequired = i == 1;
				w.queueDuration = {TelemetryAvailability::Available, 100 + i, {}}; w.runDuration = {TelemetryAvailability::Available, 200 + i, {}};
				w.coordinatorWaitDuration = {TelemetryAvailability::Unavailable, 0, "not observed"};
				w.ownershipWaitDuration = {TelemetryAvailability::Available, 300 + i, {}}; w.flushDuration = {TelemetryAvailability::Available, 400 + i, {}};
				w.publicationDuration = {TelemetryAvailability::Available, 500 + i, {}}; frame.workloadExecutions.push_back(std::move(w));
			}
			frame.framePublication = e; frame.workloadExecutionsAvailable = true; return frame;
		}
		std::uint64_t WireUnsigned(TestContext& context, std::string_view wire, std::size_t offset, unsigned width) {
			context.Expect(width <= 8 && offset <= wire.size() && width <= wire.size() - offset, "Independent V1 oracle must bound every wire read.");
			std::uint64_t result = 0; for(unsigned i = 0; i < width; ++i) result |= std::uint64_t(static_cast<unsigned char>(wire[offset + i])) << (8 * i); return result;
		}
		void ExpectWrittenLossRows(TestContext& context, const std::filesystem::path& path, const ProfilerJournalV1OpenResult& reopened) {
			// Use the public validated IDX1 offset, never a substring search for a
			// magic spelling. Public reopen already checked the committed digest.
			context.Expect(reopened.index.size() == 1, "Single-frame wire oracle requires its actual public index entry.");
			std::ifstream input(path, std::ios::binary | std::ios::ate); const auto fileBytes = input.tellg();
			context.Expect(fileBytes > 0 && fileBytes <= 1024 * 1024, "Written fixture journal is bounded to1MiB before reading.");
			std::string file(static_cast<std::size_t>(fileBytes), '\0'); input.seekg(0); input.read(file.data(), file.size()); context.Expect(bool(input), "Read actual written journal bytes.");
			const auto segmentOffset = reopened.index[0].segmentOffset;
			context.Expect(segmentOffset <= file.size() && file.size() - segmentOffset >= 57, "Public segment offset must bound fixed SEG1 header.");
			const std::string_view segment(file.data() + static_cast<std::size_t>(segmentOffset), file.size() - static_cast<std::size_t>(segmentOffset));
			// V1 SEG1: magic4, sequence8, first8, last8, count4, codec1,
			// uncompressed8, stored8, predecessor8; payload begins at57.
			context.Expect(segment.substr(0, 4) == "SEG1" && WireUnsigned(context, segment, 28, 4) == 1
				&& WireUnsigned(context, segment, 32, 1) == 1, "Actual indexed segment must contain one Zstd-coded frame.");
			const auto rawBytes = WireUnsigned(context, segment, 33, 8), storedBytes = WireUnsigned(context, segment, 41, 8);
			context.Expect(rawBytes > 0 && rawBytes <= 65536 && storedBytes > 0 && storedBytes <= segment.size() - 57,
				"Bound both actual compressed and decompressed fixture bytes before allocation.");
			std::string raw(static_cast<std::size_t>(rawBytes), '\0');
			const auto decoded = ZSTD_decompress(raw.data(), raw.size(), segment.data() + 57, static_cast<std::size_t>(storedBytes));
			context.Expect(!ZSTD_isError(decoded) && decoded == raw.size(), "Bundled decoder must expand actual stored bytes exactly.");
			context.Expect(segment.size() - 57 - storedBytes >= 4 && segment.substr(57 + static_cast<std::size_t>(storedBytes), 4) == "CMT1", "Actual frame segment must have its following commit boundary.");
			// V1 frame payload: frame8, owned8, lossCount4, then34-byte loss rows.
			context.Expect(WireUnsigned(context, raw, 0, 8) == 17 && WireUnsigned(context, raw, 16, 4) == 2, "Actual frame owns exactly two persisted loss rows.");
			for(unsigned i = 0; i < 2; ++i) {
				const std::size_t row = 20 + i * 34;
				context.Expect(WireUnsigned(context, raw, row, 8) == 17 && WireUnsigned(context, raw, row + 8, 8) == 17, "Each actual loss row stays on its sealed frame.");
				const auto expectedClass = i == 0 ? ProfilerCaptureLossClass::AnimationDetails : ProfilerCaptureLossClass::SampleLeaves;
				context.Expect(WireUnsigned(context, raw, row + 16, 1) == static_cast<std::uint8_t>(expectedClass)
					&& WireUnsigned(context, raw, row + 17, 1) == static_cast<std::uint8_t>(ProfilerCaptureLossReason::QueuePressure)
					&& WireUnsigned(context, raw, row + 18, 8) == (i == 0 ? 2U : 3U) && WireUnsigned(context, raw, row + 26, 8) == (i == 0 ? 7U : 11U),
					"Independent wire oracle must see each distinct emitted class/reason/count, not only aggregate totals.");
			}
			const auto frameBytes = WireUnsigned(context, raw, 88, 8);
			context.Expect(raw.size() >= 96 && frameBytes == raw.size() - 96 && std::string_view(raw).substr(96, 4) == "FRM1", "Loss rows must end at the actual bounded FRM1 payload boundary.");
			const auto cborBytes = WireUnsigned(context, raw, 100, 8);
			context.Expect(cborBytes <= frameBytes - 12, "Bound actual frame CBOR before bundled decoding.");
			const auto envelope = nlohmann::json::from_cbor(raw.begin() + 108, raw.begin() + 108 + static_cast<std::size_t>(cborBytes));
			context.Expect(envelope["frame"]["i"] == 17, "Actual bundled CBOR decode must identify the frame following the observed loss rows.");
		}
		std::string Encode(TestContext& context, const ProfilerFrameRecord& frame) {
			std::string bytes, error; context.Expect(EncodeProfilerJournalFrame(frame, {}, bytes, error), "Actual V1 encode: " + error); return bytes;
		}
		nlohmann::json Envelope(TestContext& context, const std::string& bytes, std::size_t& end) {
			context.Expect(bytes.size() >= 16 && bytes.substr(0, 4) == "FRM1", "Actual codec retains FRM1.");
			std::uint64_t count = 0; for(unsigned i = 0; i < 8; ++i) count |= std::uint64_t(static_cast<unsigned char>(bytes[4 + i])) << (8 * i);
			context.Expect(count <= bytes.size() - 16, "CBOR length leaves sample count."); end = 12 + static_cast<std::size_t>(count);
			return nlohmann::json::from_cbor(bytes.begin() + 12, bytes.begin() + end);
		}
		std::string ReplaceEnvelope(const std::string& original, std::size_t end, const nlohmann::json& envelope) {
			const auto encoded = nlohmann::json::to_cbor(envelope); std::string result = "FRM1";
			for(unsigned i = 0; i < 8; ++i) result.push_back(static_cast<char>((std::uint64_t(encoded.size()) >> (8 * i)) & 255));
			result.append(reinterpret_cast<const char*>(encoded.data()), encoded.size()); result.append(original.substr(end)); return result;
		}
	}
	void RegisterJournalEvidenceRoundTripTests(TestRegistry& registry) {
		registry.Add("Engine.JournalEvidence.InvalidOptionalWorkloadDoesNotTerminateRecording", [](TestContext& context) {
			JournalWorkspace workspace;
			ProfilerSealedFrame damaged;
			damaged.frame = EvidenceFrame();
			damaged.frame.workloadExecutions[0].status = static_cast<WorkloadExecutionStatus>(255U);
			std::string bytes, error;
			context.Expect(!EncodeProfilerJournalFrame(damaged.frame, {}, bytes, error),
				"The standalone wire codec must still reject malformed records rather than invent valid evidence.");
			// Call the exact sealing guard used by the live profiler, then persist a
			// damaged frame and its healthy successor through the actual V1 writer.
			PrepareProfilerJournalWorkloadEvidence(damaged);
			context.Expect(!damaged.frame.workloadExecutionsAvailable && damaged.frame.workloadExecutions.empty()
				&& damaged.losses.size() == 1U && damaged.losses[0].discardedRecordCount == 2U
				&& damaged.losses[0].reason == ProfilerCaptureLossReason::InvalidEvidence,
				"Withhold the entire incomplete workload family with explicit loss accounting.");
			damaged.ownedByteCount = EstimateProfilerFrameOwnedBytes(damaged.frame);
			ProfilerSealedFrame healthy; healthy.frame = EvidenceFrame(); healthy.frame.frameIndex = 18U;
			for(auto& row : healthy.frame.workloadExecutions) row.frameId = "frame18";
			PrepareProfilerJournalWorkloadEvidence(healthy);
			healthy.ownedByteCount = EstimateProfilerFrameOwnedBytes(healthy.frame);
			context.Expect(healthy.losses.empty() && healthy.frame.workloadExecutionsAvailable,
				"A later valid frame must retain all of its measured workload evidence.");
			ProfilerJournalV1Writer writer; const auto path = workspace.root / "optional-workload-loss.teprof";
			context.Expect(writer.Open(path, {}, error) == ProfilerJournalV1ResultCode::Ok, error);
			context.Expect(writer.AppendCommittedSegment(std::span(&damaged, 1), error) == ProfilerJournalV1ResultCode::Ok, error);
			context.Expect(writer.AppendCommittedSegment(std::span(&healthy, 1), error) == ProfilerJournalV1ResultCode::Ok, error);
			context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok, error);
			ProfilerCaptureDocument document; ProfilerJournalV1OpenResult opened;
			context.Expect(OpenProfilerJournalV1(path, document, opened, error) == ProfilerJournalV1ResultCode::Ok
				&& opened.summary.frameCount == 2U && opened.summary.droppedDetailRecordCount == 2U,
				"Both actual frames must remain readable after accounting for optional loss: " + error);
			ProfilerFrameRecord decoded;
			context.Expect(ReadProfilerJournalV1SelectedFrame(path, 17U, decoded, error) == ProfilerJournalV1ResultCode::Ok
				&& !decoded.workloadExecutionsAvailable && decoded.workloadExecutions.empty() && decoded.framePublication.has_value(),
				"A damaged workload family is unavailable, while unrelated publication/core evidence survives.");
			context.Expect(ReadProfilerJournalV1SelectedFrame(path, 18U, decoded, error) == ProfilerJournalV1ResultCode::Ok
				&& decoded.workloadExecutionsAvailable && decoded.workloadExecutions.size() == 2U,
				"The same journal must continue with healthy successor data.");
		});
		registry.Add("Engine.JournalEvidence.InMemorySaveQuarantinesOnlyInvalidWorkloadFamily", [](TestContext& context) {
			JournalWorkspace workspace;
			ProfilerCaptureDocument source;
			source.frames = {EvidenceFrame(), EvidenceFrame()};
			source.frames[0].workloadExecutions[0].status = static_cast<WorkloadExecutionStatus>(255U);
			source.frames[1].frameIndex = 18U;
			for(auto& row : source.frames[1].workloadExecutions) row.frameId = "frame18";
			const auto path = workspace.root / "in-memory-loss.teprof";
			std::string error;
			// Exercise the public in-memory save path, rather than preparing the
			// sealed frame here; callers must receive the same optional-loss policy.
			context.Expect(SaveCaptureDocument(source, path, error), "In-memory save must retain core evidence: " + error);
			ProfilerCaptureDocument reopened; ProfilerJournalV1OpenResult opened;
			context.Expect(OpenProfilerJournalV1(path, reopened, opened, error) == ProfilerJournalV1ResultCode::Ok
				&& opened.summary.frameCount == 2U && opened.summary.droppedDetailRecordCount == 2U, "Persist exact optional loss and both frames: " + error);
			ProfilerFrameRecord decoded;
			context.Expect(ReadProfilerJournalV1SelectedFrame(path, 17U, decoded, error) == ProfilerJournalV1ResultCode::Ok
				&& !decoded.workloadExecutionsAvailable && decoded.workloadExecutions.empty() && decoded.framePublication.has_value(), "Invalid optional family must not discard frame publication.");
			context.Expect(ReadProfilerJournalV1SelectedFrame(path, 18U, decoded, error) == ProfilerJournalV1ResultCode::Ok
				&& decoded.workloadExecutionsAvailable && decoded.workloadExecutions.size() == 2U, "A healthy successor must retain measured workloads.");
			context.Expect(source.frames[0].workloadExecutionsAvailable && source.frames[0].workloadExecutions.size() == 2U
				&& source.frames[0].workloadExecutions[0].status == static_cast<WorkloadExecutionStatus>(255U), "Saving works on owned copies without mutating the caller's document.");
		});
		registry.Add("Engine.JournalEvidence.SameFrameLossClassesCommitAndRead", [](TestContext& context) {
			JournalWorkspace workspace; ProfilerSealedFrame sealed; sealed.frame.frameIndex = 17; sealed.frame.durationMs = 16;
			sealed.losses = {{17, 17, ProfilerCaptureLossClass::AnimationDetails, ProfilerCaptureLossReason::QueuePressure, 2, 7},
				{17, 17, ProfilerCaptureLossClass::SampleLeaves, ProfilerCaptureLossReason::QueuePressure, 3, 11}};
			sealed.ownedByteCount = EstimateProfilerFrameOwnedBytes(sealed.frame); std::string error;
			context.Expect(ValidateProfilerSealedFrame(sealed, error), "Distinct same-frame losses must be valid: " + error);
			ProfilerJournalV1Writer writer; const auto path = workspace.root / "loss.teprof";
			context.Expect(writer.Open(path, {}, error) == ProfilerJournalV1ResultCode::Ok, error);
			context.Expect(writer.AppendCommittedSegment(std::span(&sealed, 1), error) == ProfilerJournalV1ResultCode::Ok, error);
			context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok, error);
			ProfilerCaptureDocument document; ProfilerJournalV1OpenResult reopened;
			context.Expect(OpenProfilerJournalV1(path, document, reopened, error) == ProfilerJournalV1ResultCode::Ok, error);
			context.Expect(reopened.summary.droppedDetailRecordCount == 5 && reopened.summary.droppedDetailByteCount == 18,
				"Actual reader must accept both serialized classes and conserve their loss totals.");
			ExpectWrittenLossRows(context, path, reopened);
		});
		registry.Add("Engine.JournalEvidence.MalformedSealedLossRangesStillReject", [](TestContext& context) {
			ProfilerSealedFrame sealed; sealed.frame.frameIndex = 17; sealed.ownedByteCount = EstimateProfilerFrameOwnedBytes(sealed.frame); std::string error;
			for(const auto loss : {ProfilerCaptureLossRange{18, 17, ProfilerCaptureLossClass::SampleLeaves, ProfilerCaptureLossReason::QueuePressure, 1, 1},
				ProfilerCaptureLossRange{16, 16, ProfilerCaptureLossClass::SampleLeaves, ProfilerCaptureLossReason::QueuePressure, 1, 1},
				ProfilerCaptureLossRange{17, 17, ProfilerCaptureLossClass::None, ProfilerCaptureLossReason::QueuePressure, 1, 1},
				ProfilerCaptureLossRange{17, 17, ProfilerCaptureLossClass::SampleLeaves, ProfilerCaptureLossReason::None, 1, 1}}) {
				sealed.losses = {loss}; context.Expect(!ValidateProfilerSealedFrame(sealed, error) && !error.empty(), "Malformed loss evidence must reject.");
			}
		});
		registry.Add("Engine.JournalEvidence.DifferingViewsAndWorkloadsRoundTrip", [](TestContext& context) {
			const auto expected = EvidenceFrame(); const auto bytes = Encode(context, expected); ProfilerFrameRecord decoded; std::string error;
			context.Expect(DecodeProfilerJournalFrame(bytes, decoded, error), error);
			context.Expect(decoded.framePublication && decoded.framePublication->viewCount == 2 && decoded.workloadExecutionsAvailable && decoded.workloadExecutions.size() == 2,
				"Actual codec retains differing view and workload rows, not only scalar counts.");
			for(unsigned i = 0; i < 2; ++i) {
				context.Expect(ViewFacts(expected.framePublication->views[i]) == ViewFacts(decoded.framePublication->views[i]), "Every view field survives.");
				context.Expect(WorkFacts(expected.workloadExecutions[i]) == WorkFacts(decoded.workloadExecutions[i]), "Every persistent workload field/duration survives.");
			}
		});
		registry.Add("Engine.JournalEvidence.LegacyAbsentExtensionsStayUnavailable", [](TestContext& context) {
			const auto bytes = Encode(context, EvidenceFrame()); std::size_t end; auto envelope = Envelope(context, bytes, end);
			context.Expect(envelope["frame"]["f"].size() == 22, "V1 f remains exactly22 fields."); envelope["frame"].erase("fv"); envelope["frame"].erase("we");
			ProfilerFrameRecord decoded; std::string error; context.Expect(DecodeProfilerJournalFrame(ReplaceEnvelope(bytes, end, envelope), decoded, error), error);
			context.Expect(decoded.framePublication && decoded.framePublication->viewCount == 2 && !decoded.workloadExecutionsAvailable && decoded.workloadExecutions.empty(), "Legacy scalar counts remain; workload detail stays absent.");
			for(unsigned i = 0; i < 2; ++i) context.Expect(!decoded.framePublication->views[i].valid, "Missing view detail is unavailable, never valid zero.");
		});
		registry.Add("Engine.JournalEvidence.MalformedOptionalRowsReject", [](TestContext& context) {
			const auto bytes = Encode(context, EvidenceFrame()); std::size_t end; const auto original = Envelope(context, bytes, end);
			for(unsigned variant = 0; variant < 5; ++variant) {
				auto envelope = original;
				if(variant == 0) envelope["frame"]["fv"] = nlohmann::json::array({false});
				if(variant == 1) envelope["frame"]["fv"] = nlohmann::json::array(); // mismatch scalar count2
				if(variant == 2) envelope["frame"]["fv"] = std::vector<unsigned>(9, 0); // maximum8
				if(variant == 3) envelope["frame"]["we"] = nlohmann::json::array({false});
				if(variant == 4) envelope["frame"]["we"] = std::vector<unsigned>(kMaximumWorkloadExecutionRecordsPerFrame + 1, 0);
				ProfilerFrameRecord decoded; std::string error;
				context.Expect(!DecodeProfilerJournalFrame(ReplaceEnvelope(bytes, end, envelope), decoded, error) && !error.empty(), "Malformed optional rows reject; variant=" + std::to_string(variant));
			}
		});
		registry.Add("Engine.JournalEvidence.FixedScalarArrayStillRejectsExtension", [](TestContext& context) {
			const auto bytes = Encode(context, EvidenceFrame()); std::size_t end; auto envelope = Envelope(context, bytes, end); envelope["frame"]["f"].push_back(0);
			ProfilerFrameRecord decoded; std::string error; context.Expect(!DecodeProfilerJournalFrame(ReplaceEnvelope(bytes, end, envelope), decoded, error), "Optional evidence never extends fixed V1 f array.");
		});
		registry.Add("Engine.JournalEvidence.ObservedEmptyWorkloadsDifferFromLegacyAbsent", [](TestContext& context) {
			auto observed = EvidenceFrame(); observed.workloadExecutions.clear(); observed.workloadExecutionsAvailable = true;
			const auto bytes = Encode(context, observed); std::size_t end; auto envelope = Envelope(context, bytes, end);
			context.Expect(envelope["frame"].contains("we") && envelope["frame"]["we"].is_array() && envelope["frame"]["we"].empty(),
				"Explicit observed empty workload set must have an empty optional row array.");
			ProfilerFrameRecord decoded; std::string error;
			context.Expect(DecodeProfilerJournalFrame(bytes, decoded, error) && decoded.workloadExecutionsAvailable && decoded.workloadExecutions.empty(),
				"Present empty workload evidence is available zero, not omitted legacy detail.");
			envelope["frame"].erase("we");
			context.Expect(DecodeProfilerJournalFrame(ReplaceEnvelope(bytes, end, envelope), decoded, error)
				&& !decoded.workloadExecutionsAvailable && decoded.workloadExecutions.empty(),
				"Legacy absence must reset availability even when reusing an output that held observed evidence.");
		});
		registry.Add("Engine.JournalEvidence.UnavailablePublicationViewRetainsDiagnosticMetadata", [](TestContext& context) {
			// The real sealing producer copies identity/counters even for unavailable views.
			// Availability must survive without erasing those useful diagnostics.
			auto expected = EvidenceFrame(); expected.framePublication->views[1].valid = false;
			const auto bytes = Encode(context, expected); ProfilerFrameRecord decoded; std::string error;
			const bool decodedOk = DecodeProfilerJournalFrame(bytes, decoded, error);
			context.Expect(decodedOk, "Unavailable publication diagnostic metadata remains compatible: " + error);
			if (!decodedOk || !decoded.framePublication) return;
			context.Expect(!decoded.framePublication->views[1].valid
				&& ViewFacts(expected.framePublication->views[1]) == ViewFacts(decoded.framePublication->views[1]),
				"Unavailable publication rows retain every actual identity/counter field without becoming valid.");
		});
	}
}
