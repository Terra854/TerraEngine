// MotionDatabaseAuthoringTests.cpp - persisted editor model and async build behavior.

#include "TestHarness.h"

#include "Editor/AssetCompilers/AnimationClipCompiler.h"
#include "Editor/AssetCompilers/MotionDatabaseCompiler.h"
#include "Editor/TerraEngine-Editor/Windows/MotionDatabaseAuthoringHost.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests {
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::AssetPipeline;
	using namespace TerraEngine::Editor::Animation;

	namespace {
		AnimationGuid Guid(std::uint8_t seed) {
			AnimationGuid result;
			for(std::size_t index = 0U; index < result.value.size(); ++index) {
				result.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return result;
		}

		class ManualExecutor final : public MotionDatabaseCompileExecutor {
		public:
			void Enqueue(std::function<void()> work) override {
				work_.push_back(std::move(work));
			}

			void RunNext() {
				auto work = std::move(work_.front());
				work_.erase(work_.begin());
				work();
			}

		private:
			std::vector<std::function<void()>> work_;
		};

		struct TemporaryDirectory {
			TemporaryDirectory() {
				path = std::filesystem::temp_directory_path()
					/ ("terra-motion-authoring-" + std::to_string(
						std::chrono::steady_clock::now().time_since_epoch().count()));
				std::filesystem::create_directories(path);
			}

			~TemporaryDirectory() {
				std::error_code error;
				std::filesystem::remove_all(path, error);
			}

			std::filesystem::path path;
		};

		std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if(!stream) return {};
			const std::streamoff length = stream.tellg();
			stream.seekg(0, std::ios::beg);
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
			stream.read(reinterpret_cast<char*>(bytes.data()), length);
			return bytes;
		}

		AnimationClipAsset WriteProductionClip(TestContext& context,
			const std::filesystem::path& path, AnimationGuid assetId,
			AnimationCompatibilityFingerprint compatibility, float endDistance = 6.0F) {
			AnimationClipCompileInput input;
			input.assetId = assetId;
			input.skeletonAssetId = Guid(70U);
			input.compatibilityFingerprint = compatibility;
			input.durationTicks = 60;
			input.sampleRate = 60U;
			AnimationSourceTrack joint;
			joint.stableTargetId = 11U;
			joint.kind = AnimationClipTrackKind::Translation;
			joint.keys = {{0, {0.0F, 1.0F, 0.0F}}, {60, {1.0F, 1.0F, 0.0F}}};
			input.tracks.push_back(joint);
			AnimationSourceTrack root;
			root.stableTargetId = 99U;
			root.kind = AnimationClipTrackKind::RootTranslation;
			root.keys = {{0, {0.0F, 0.0F, 0.0F}}, {60, {endDistance, 0.0F, 3.0F}}};
			input.tracks.push_back(root);
			AnimationSourceTrack yaw;
			yaw.stableTargetId = 100U;
			yaw.kind = AnimationClipTrackKind::RootYaw;
			yaw.keys = {{0, {}, {}, 0.0F}, {60, {}, {}, 1.0F}};
			input.tracks.push_back(yaw);
			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, clip, report) == AnimationStatus::Ok,
				"The production resolver fixture must compile through the real clip codec.");
			std::vector<std::uint8_t> bytes;
			context.Expect(SerializeAnimationClipAsset(clip, bytes) == AnimationStatus::Ok,
				"The resolver fixture must use a checked .teanim envelope.");
			std::filesystem::create_directories(path.parent_path());
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			stream.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			context.Expect(static_cast<bool>(stream), "The resolver fixture should publish its test clip bytes.");
			return clip;
		}

		MotionDatabaseAuthoringDocument MakeDocument() {
			MotionDatabaseAuthoringDocument document;
			document.authoringGeneration = 7U;
			document.assetId = Guid(11U);
			document.compatibilityFingerprint = 0x12345678U;
			document.samplesPerBlock = 1U;
			document.featureWeights.fill(1U);
			document.featureWeights[0] = 5U;
			MotionDatabaseAuthoringSource source;
			source.sourceClipId = 101U;
			source.sourceClipFingerprint = 202U;
			source.sourcePath = "Assets/Motions/Walk.teanim";
			source.baseTagMask = 0x1U;
			source.baseContactMask = 0x1U;
			source.tagRanges.push_back({20, 20, 0x4U});
			source.contactRanges.push_back({10, 10, 0x2U});
			source.exclusionRanges.push_back({30, 30, 0x8U});
			document.sources.push_back(std::move(source));
			return document;
		}

		MotionDatabaseSourceClipInput MakeResolvedClip() {
			MotionDatabaseSourceClipInput clip;
			clip.sourceClipId = 101U;
			clip.sourceClipFingerprint = 202U;
			for(AnimationFixedTick tick : {10, 20, 30}) {
				MotionDatabaseClipSampleInput sample;
				sample.sourceTick = tick;
				sample.trajectory[0].positionX = static_cast<float>(tick);
				sample.posePoints[0].x = static_cast<float>(tick) * 0.5F;
				sample.velocityPoints[0].z = static_cast<float>(tick) * 0.25F;
				sample.continuityGroup = 1U;
				clip.samples.push_back(sample);
			}
			return clip;
		}

		bool SameAuthoringPayload(MotionDatabaseAuthoringDocument left,
			MotionDatabaseAuthoringDocument right) {
			// Undo and redo deliberately allocate new generations. Normalizing only
			// that field lets tests prove every editable payload field is exact.
			left.authoringGeneration = 1U;
			right.authoringGeneration = 1U;
			return left == right;
		}
	}

	void RegisterMotionDatabaseAuthoringTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.MotionDatabase.AuthoringPersistsSourcesAnnotationsAndFeatures",
			[](TestContext& context) {
				TemporaryDirectory temporary;
				const std::filesystem::path path = temporary.path / "Locomotion.temotiondb.authoring.json";
				const MotionDatabaseAuthoringDocument document = MakeDocument();
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, document)
						== AnimationStatus::Ok,
					"A valid authoring document should publish only after JSON read-back validation.");
				MotionDatabaseAuthoringDocument restored;
				context.Expect(LoadMotionDatabaseAuthoringDocument(path, restored) == AnimationStatus::Ok
						&& restored == document
						&& restored.sources.front().tagRanges.front().tagMask == 0x4U
						&& restored.sources.front().contactRanges.front().contactMask == 0x2U
						&& restored.featureWeights[0] == 5U,
					"Reload must restore the persisted source list, ranges, and per-feature query costs.");

				const std::vector<std::uint8_t> lastGood = ReadBytes(path);
				MotionDatabaseAuthoringDocument invalid = document;
				invalid.sources.front().contactRanges.front().lastTick = -1;
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, invalid)
						== AnimationStatus::InvalidArgument && ReadBytes(path) == lastGood,
					"A rejected authoring candidate must preserve the complete previous sidecar bytes.");
			});

		registry.Add("Editor.Animation.MotionDatabase.GroupedSaveCancelUndoRedoIsExactAndMonotonic",
			[](TestContext& context) {
				TemporaryDirectory temporary;
				const std::filesystem::path path = temporary.path
					/ "Grouped.temotiondb.authoring.json";
				const MotionDatabaseAuthoringDocument baseline = MakeDocument();
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, baseline)
						== AnimationStatus::Ok,
					"The grouped-history fixture should atomically create its baseline sidecar.");
				MotionDatabaseAuthoringSnapshot snapshot;
				context.Expect(LoadMotionDatabaseAuthoringSnapshot(path, snapshot)
						== AnimationStatus::Ok,
					"History must start from a typed document and its exact persisted bytes.");

				MotionDatabaseAuthoringHistory history;
				history.Reset(snapshot.document);
				MotionDatabaseAuthoringDocument grouped = snapshot.document;
				context.Expect(history.BeginEdit(grouped) == AnimationStatus::Ok,
					"A complete-document edit group should capture one immutable baseline.");
				grouped.featureWeights[0] = 17U;
				grouped.sources.front().baseTagMask = 0x55U;
				grouped.sources.front().contactRanges.push_back({40, 45, 0x8U});
				context.Expect(history.CommitEdit(grouped) == AnimationStatus::Ok
						&& grouped.authoringGeneration == baseline.authoringGeneration + 1U,
					"Several field edits should commit as one generation and one undo entry.");
				MotionDatabaseAuthoringSnapshot groupedSnapshot;
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, grouped,
						&snapshot, &groupedSnapshot) == AnimationStatus::Ok,
					"One grouped commit should cross one exact-current save boundary.");
				const std::vector<std::uint8_t> groupedBytes = ReadBytes(path);

				MotionDatabaseAuthoringDocument cancelled = grouped;
				context.Expect(history.BeginEdit(cancelled) == AnimationStatus::Ok,
					"A second group should start from the saved complete document.");
				cancelled.featureWeights[1] = 99U;
				cancelled.sources.front().sourcePath = "Assets/Motions/Cancelled.teanim";
				context.Expect(history.CancelEdit(cancelled) == AnimationStatus::Ok
						&& cancelled == grouped && ReadBytes(path) == groupedBytes,
					"Cancel must restore every field and generation exactly without changing persisted bytes.");

				MotionDatabaseAuthoringDocument undone = grouped;
				context.Expect(history.Undo(undone) == AnimationStatus::Ok
						&& SameAuthoringPayload(undone, baseline)
						&& undone.authoringGeneration > grouped.authoringGeneration,
					"Undo should restore the exact baseline payload under a newer generation.");
				MotionDatabaseAuthoringSnapshot undoneSnapshot;
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, undone,
						&groupedSnapshot, &undoneSnapshot) == AnimationStatus::Ok,
					"The undo document should atomically replace only its exact predecessor.");

				MotionDatabaseAuthoringDocument redone = undone;
				context.Expect(history.Redo(redone) == AnimationStatus::Ok
						&& SameAuthoringPayload(redone, grouped)
						&& redone.authoringGeneration > undone.authoringGeneration,
					"Redo should exactly restore the grouped payload under another newer generation.");
				MotionDatabaseAuthoringSnapshot redoneSnapshot;
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, redone,
						&undoneSnapshot, &redoneSnapshot) == AnimationStatus::Ok,
					"The redo transaction should publish through the same checked replacement path.");
				MotionDatabaseAuthoringDocument persisted;
				context.Expect(LoadMotionDatabaseAuthoringDocument(path, persisted)
						== AnimationStatus::Ok && persisted == redone,
					"The final sidecar must contain the complete redone document and newest generation.");
			});

		registry.Add("Editor.Animation.MotionDatabase.ExactCurrentSaveRejectsExternalRewrite",
			[](TestContext& context) {
				TemporaryDirectory temporary;
				const std::filesystem::path path = temporary.path
					/ "External.temotiondb.authoring.json";
				const MotionDatabaseAuthoringDocument baseline = MakeDocument();
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, baseline)
						== AnimationStatus::Ok,
					"The stale-write fixture should create one valid baseline.");
				MotionDatabaseAuthoringSnapshot snapshot;
				context.Expect(LoadMotionDatabaseAuthoringSnapshot(path, snapshot)
						== AnimationStatus::Ok,
					"The editor should retain the exact loaded baseline bytes.");

				// Trailing JSON whitespace preserves the typed generation while changing
				// the exact external revision token that the editor originally loaded.
				{
					std::ofstream external(path, std::ios::binary | std::ios::app);
					external << '\n';
				}
				const std::vector<std::uint8_t> externalBytes = ReadBytes(path);
				MotionDatabaseAuthoringDocument candidate = baseline;
				++candidate.authoringGeneration;
				candidate.featureWeights[2] = 33U;
				MotionDatabaseAuthoringSnapshot rejectedSnapshot;
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, candidate,
						&snapshot, &rejectedSnapshot) == AnimationStatus::StaleHandle
						&& ReadBytes(path) == externalBytes,
					"A same-generation external byte rewrite must reject the stale editor candidate without replacement.");
			});

		registry.Add("Editor.Animation.MotionDatabase.ConcurrentSameRevisionHasExactlyOneWinner",
			[](TestContext& context) {
				TemporaryDirectory temporary;
				const std::filesystem::path path = temporary.path
					/ "Concurrent.temotiondb.authoring.json";
				const MotionDatabaseAuthoringDocument baseline = MakeDocument();
				context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(path, baseline)
						== AnimationStatus::Ok,
					"The concurrent fixture should create one baseline revision.");
				MotionDatabaseAuthoringSnapshot snapshot;
				context.Expect(LoadMotionDatabaseAuthoringSnapshot(path, snapshot)
						== AnimationStatus::Ok,
					"Both simulated editors must begin from the same exact snapshot.");

				std::array<MotionDatabaseAuthoringDocument, 2U> candidates{baseline, baseline};
				for(auto& candidate : candidates) ++candidate.authoringGeneration;
				candidates[0].featureWeights[3] = 41U;
				candidates[1].featureWeights[3] = 42U;
				std::array<AnimationStatus, 2U> results{
					AnimationStatus::InternalError, AnimationStatus::InternalError};
				std::array<MotionDatabaseAuthoringSnapshot, 2U> published;
				std::atomic_uint32_t ready = 0U;
				std::atomic_bool start = false;
				auto writer = [&](std::size_t index) {
					ready.fetch_add(1U, std::memory_order_release);
					while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
					results[index] = SaveMotionDatabaseAuthoringDocumentAtomically(
						path, candidates[index], &snapshot, &published[index]);
				};
				std::thread first(writer, 0U);
				std::thread second(writer, 1U);
				while(ready.load(std::memory_order_acquire) != 2U) std::this_thread::yield();
				start.store(true, std::memory_order_release);
				first.join();
				second.join();

				const std::size_t winnerCount = static_cast<std::size_t>(
					results[0] == AnimationStatus::Ok)
					+ static_cast<std::size_t>(results[1] == AnimationStatus::Ok);
				const std::size_t staleCount = static_cast<std::size_t>(
					results[0] == AnimationStatus::StaleHandle)
					+ static_cast<std::size_t>(results[1] == AnimationStatus::StaleHandle);
				const std::size_t winner = results[0] == AnimationStatus::Ok ? 0U : 1U;
				MotionDatabaseAuthoringDocument finalDocument;
				context.Expect(winnerCount == 1U && staleCount == 1U
						&& LoadMotionDatabaseAuthoringDocument(path, finalDocument)
							== AnimationStatus::Ok
						&& finalDocument == candidates[winner],
					"A per-path CAS lock must allow exactly one same-baseline writer and preserve its complete document.");
			});

		registry.Add("Editor.Animation.MotionDatabase.SessionRejectsStaleAndCancelledPublication",
			[](TestContext& context) {
				TemporaryDirectory temporary;
				const std::filesystem::path output = temporary.path / "Locomotion.temotiondb";
				auto executor = std::make_shared<ManualExecutor>();
				MotionDatabaseAuthoringSession session(executor, 2U);
				MotionDatabaseAuthoringDocument requested = MakeDocument();
				const MotionDatabaseSourceClipInput resolved = MakeResolvedClip();

				context.Expect(session.RequestBuild(requested,
						std::span<const MotionDatabaseSourceClipInput>(&resolved, 1U), output)
						== AnimationStatus::Queued
						&& session.GetBuildProgress().stage == MotionDatabaseBuildStage::Queued,
					"The session should queue a copied request and expose bounded zero-to-one progress.");
				executor->RunNext();
				MotionDatabaseAuthoringDocument newer = requested;
				++newer.authoringGeneration;
				context.Expect(session.Poll(newer) == AnimationStatus::StaleHandle
						&& !std::filesystem::exists(output),
					"A completed worker result from an older authoring revision must not publish a file.");

				context.Expect(session.RequestBuild(newer,
						std::span<const MotionDatabaseSourceClipInput>(&resolved, 1U), output)
						== AnimationStatus::Queued,
					"The current authoring revision should be independently queueable.");
				session.CancelBuild();
				executor->RunNext();
				context.Expect(session.Poll(newer) == AnimationStatus::Cancelled
						&& !std::filesystem::exists(output),
					"Cancellation must be observed before the protected file publication boundary.");
			});

		registry.Add("Editor.Animation.MotionDatabase.SessionPublishesAndBindsRealCandidateScores",
			[](TestContext& context) {
				TemporaryDirectory temporary;
				const std::filesystem::path output = temporary.path / "Locomotion.temotiondb";
				auto executor = std::make_shared<ManualExecutor>();
				MotionDatabaseAuthoringSession session(executor, 2U);
				const MotionDatabaseAuthoringDocument document = MakeDocument();
				MotionDatabaseSourceClipInput resolved = MakeResolvedClip();
				context.Expect(session.RequestBuild(document,
						std::span<const MotionDatabaseSourceClipInput>(&resolved, 1U), output)
						== AnimationStatus::Queued,
					"A resolved persisted source should enter the worker-safe build queue.");
				executor->RunNext();
				context.Expect(session.GetBuildProgress().Fraction() == 1.0F
						&& session.Poll(document) == AnimationStatus::Ok
						&& std::filesystem::exists(output),
					"A complete validated candidate should atomically publish the cooked database.");
				const auto published = session.GetPublished();
				MotionDatabaseAsset decoded;
				const std::vector<std::uint8_t> bytes = ReadBytes(output);
				context.Expect(published.has_value() && published->asset
						&& DeserializeMotionDatabaseAsset(bytes,
							document.compatibilityFingerprint, decoded) == AnimationStatus::Ok
						&& decoded.samples.size() == 3U
						&& std::any_of(decoded.samples.begin(), decoded.samples.end(), [](const auto& sample) {
							return sample.contactMask == 0x3U;
						}),
					"File publication must contain the source annotations merged by the authoring model.");

				MotionMatchQuery query;
				query.database = {document.assetId, published ? published->generation : 0U};
				query.expectedSchemaFingerprint = published
					? published->asset->featureSchemaFingerprint : 0U;
				query.featureWeights = document.featureWeights;
				query.maximumCandidates = 3U;
				context.Expect(session.RunQueryPreview(query) == AnimationStatus::Ok,
					"Search preview must run the production deterministic scorer against the published generation.");
				const auto preview = session.GetQueryPreview();
				context.Expect(preview.has_value() && preview->databaseGeneration == published->generation
						&& preview->result.candidates.size() == 3U
						&& preview->result.candidates.front().sourceClipId == 101U
						&& preview->result.candidates.front().score.totalCost
							>= preview->result.candidates.front().score.perFeatureCosts[0],
					"The bound snapshot must retain actual ranked candidates, source identity, and per-feature costs.");

				const std::vector<std::uint8_t> lastGood = ReadBytes(output);
				resolved.samples.front().trajectory.front().positionX =
					std::numeric_limits<float>::quiet_NaN();
				context.Expect(session.RequestBuild(document,
						std::span<const MotionDatabaseSourceClipInput>(&resolved, 1U), output)
						== AnimationStatus::Queued,
					"Invalid sampled data should be rejected by the worker, not mutate the published file eagerly.");
				executor->RunNext();
				context.Expect(session.Poll(document) == AnimationStatus::InvalidArgument
						&& ReadBytes(output) == lastGood,
					"A failed rebuild must preserve the byte-identical last-known-good database.");
			});

		registry.Add("Editor.Animation.MotionDatabase.ProductionHostResolvesAndSamplesTeanimOnWorker",
			[](TestContext& context) {
				TemporaryDirectory temporary;
				const std::filesystem::path assetRoot = temporary.path / "Assets";
				const std::filesystem::path buildRoot = temporary.path / "Build";
				const std::filesystem::path clipPath = assetRoot / "Motions/Walk.teanim";
				const AnimationGuid clipId = Guid(44U);
				constexpr AnimationCompatibilityFingerprint compatibility = 0x12345678U;
				WriteProductionClip(context, clipPath, clipId, compatibility);
				std::filesystem::create_directories((buildRoot / "Motions"));
				std::filesystem::copy_file(clipPath, buildRoot / "Motions/Walk.teanim");

				auto executor = std::make_shared<ManualExecutor>();
				MotionDatabaseProductionAuthoringHost host(assetRoot, buildRoot, executor);
				MotionDatabaseAuthoringDocument document = MakeDocument();
				document.sources.front().sourcePath = "Motions/Walk.teanim";
				document.sources.front().sourceClipId = 0U;
				document.sources.front().sourceClipFingerprint = 0U;
				AnimationCompatibilityFingerprint discoveredCompatibility = 0U;
				context.Expect(host.ReadMotionDatabaseSourceCompatibility(
						document.sources.front(), discoveredCompatibility) == AnimationStatus::Ok
						&& discoveredCompatibility == compatibility,
					"The production host should expose the checked clip compatibility identity so a new database can adopt its first real rig without manual fingerprint entry.");
				context.Expect(host.ResolveMotionDatabaseSourceIdentity(
						document.sources.front(), document.compatibilityFingerprint) == AnimationStatus::Ok
						&& document.sources.front().sourceClipId != 0U
						&& document.sources.front().sourceClipFingerprint != 0U,
					"The production host should prefer the authoritative Assets copy over its normal Build mirror and derive stable identity from the checked clip envelope.");

				MotionDatabaseAuthoringSession session(host.GetMotionDatabaseCompileExecutor());
				const std::filesystem::path output = assetRoot / "Locomotion.temotiondb";
				const MotionDatabaseAuthoringDocument queuedDocument = document;
				context.Expect(session.RequestBuild(queuedDocument,
						host.CreateMotionDatabaseSourceResolver(queuedDocument), output)
							== AnimationStatus::Queued
						&& !std::filesystem::exists(output),
					"Clip decoding and feature extraction must remain queued rather than run in the UI request.");
				// Mutate the UI-owned copy after queuing. Both the resolver and compiler
				// must keep the immutable candidate they received by value.
				document.sources.front().sourcePath = "Motions/ChangedAfterQueue.teanim";
				document.featureWeights[0] = 99U;
				executor->RunNext();
				context.Expect(session.Poll(queuedDocument) == AnimationStatus::Ok,
					"The shared-executor adapter should publish a fully resolved production build.");
				const auto published = session.GetPublished();
				context.Expect(published && published->asset
						&& published->asset->samples.size() == 61U
						&& published->asset->samples.front().sourceClipId
							== queuedDocument.sources.front().sourceClipId,
					"Production clip evaluation should use the copied candidate and generate one immutable feature sample per 60 Hz tick.");
			});

		registry.Add("Editor.Animation.MotionDatabase.ProductionResolverRejectsStaleAndMissingClips",
			[](TestContext& context) {
				TemporaryDirectory temporary;
				const std::filesystem::path assetRoot = temporary.path / "Assets";
				const std::filesystem::path buildRoot = temporary.path / "Build";
				const std::filesystem::path clipPath = assetRoot / "Motions/Walk.teanim";
				constexpr AnimationCompatibilityFingerprint compatibility = 0x12345678U;
				WriteProductionClip(context, clipPath, Guid(44U), compatibility);
				MotionDatabaseSourceResolver resolver(assetRoot, buildRoot);
				MotionDatabaseAuthoringDocument document = MakeDocument();
				document.sources.front().sourcePath = "Motions/Walk.teanim";
				document.sources.front().sourceClipId = 0U;
				document.sources.front().sourceClipFingerprint = 0U;
				context.Expect(resolver.ResolveIdentity(document.sources.front(), compatibility)
						== AnimationStatus::Ok,
					"The stale fixture must first capture a valid persisted identity.");
				WriteProductionClip(context, clipPath, Guid(44U), compatibility, 9.0F);
				std::atomic_bool cancelled = false;
				std::vector<MotionDatabaseSourceClipInput> resolved;
				context.Expect(resolver.Resolve(document, resolved, cancelled)
						== AnimationStatus::StaleHandle && resolved.empty(),
					"Changed clip content must reject the persisted fingerprint without partial samples.");
				document.sources.front().sourcePath = "Motions/Missing.teanim";
				document.sources.front().sourceClipId = 0U;
				document.sources.front().sourceClipFingerprint = 0U;
				context.Expect(resolver.ResolveIdentity(document.sources.front(), compatibility)
						== AnimationStatus::MissingAsset,
					"A missing persisted clip path must fail closed before build work starts.");
			});
	}
}
