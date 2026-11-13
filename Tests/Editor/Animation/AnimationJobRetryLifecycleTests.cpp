// AnimationJobRetryLifecycleTests.cpp - bounded receipts and real recording admission.
#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Automation/EditorAutomationService.h"
#include "Editor/TerraEngine-Editor/Automation/AnimationAutomationJobs.h"
#include "Editor/TerraEngine-Editor/Automation/AnimationRuntimeOperations.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Utilities/UUID.h"
#include "Editor/TerraEngine-Editor/Windows/MotionDatabaseAuthoringHost.h"
#include "Editor/AssetCompilers/AnimationClipCompiler.h"
#include "Editor/AssetCompilers/MotionDatabaseCompiler.h"
#include "EditorContext.h"
#include "ProjectManager.h"
#include <fstream>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include "ThreadPool/ThreadPool.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <algorithm>
#include <stdexcept>

namespace TerraEngine::EditorTests {
namespace {
using namespace Editor::Automation;
using namespace TerraEngine::Animation;
using namespace TerraEngine::AssetPipeline;
using namespace TerraEngine::Editor::Animation;

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

			// Deterministic ordering models worker completion; these cases do not
			// claim multithreaded executor or hardware timing qualification.
			void RunAt(std::size_t index) {
				auto work = std::move(work_.at(index));
				work_.erase(work_.begin() + static_cast<std::ptrdiff_t>(index));
				work();
			}
			void RunNext() { RunAt(0U); }
			void RunAll() { while(!work_.empty()) RunNext(); }

		private:
			std::vector<std::function<void()>> work_;
		};

		// This executor dispatches the supplied production preparation closure, not
		// a surrogate cook. The gate lets the submitting adapter disappear first.
		class ObservedWorkerExecutor final : public MotionDatabaseCompileExecutor {
		public:
			ObservedWorkerExecutor() : pool_(ThreadPool::CreateOwned(1U)) {}
			~ObservedWorkerExecutor() override { ReleaseAndDrain(); }
			void ReleaseAndDrain() { Release(); pool_->shutdown(); }
			void Enqueue(std::function<void()> work) override {
				if(completed_.valid()) throw std::logic_error("The observed fixture admits one real cook.");
				auto task = pool_->createTask([this, work = std::move(work)] {
					{
						std::unique_lock lock(mutex_);
						entered_ = true;
						changed_.notify_all();
						if(!changed_.wait_for(lock, std::chrono::seconds(30), [this] { return released_; }))
							throw std::runtime_error("The real motion preparation release timed out.");
					}
					beforeWork = std::this_thread::get_id();
					work();
					afterWork = std::this_thread::get_id();
				});
				completed_ = task->getFuture();
				pool_->enqueue(task);
			}
			bool WaitForEntry() {
				std::unique_lock lock(mutex_);
				return changed_.wait_for(lock, std::chrono::seconds(10), [this] { return entered_; });
			}
			void Release() {
				{ std::lock_guard lock(mutex_); released_ = true; }
				changed_.notify_all();
			}
			bool JoinPreparation() {
				if(!completed_.valid() || completed_.wait_for(std::chrono::seconds(20)) != std::future_status::ready) return false;
				completed_.get();
				return true;
			}
			// Read only after JoinPreparation's future synchronization.
			std::thread::id beforeWork, afterWork;
		private:
			std::mutex mutex_;
			std::condition_variable changed_;
			bool entered_ = false, released_ = false;
			std::future<void> completed_;
			std::unique_ptr<ThreadPool> pool_;
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


// Retain the actual editor service so the real protected-frame entry point can
// advance the same store used by this adapter, without a client job.get call.
struct MotionJobFixture {
	std::filesystem::path root = std::filesystem::temp_directory_path()
		/ ("TerraEngine-MotionJobRetry-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
	EditorAutomationService service;
	AnimationAutomationJobs jobs{service.GetJobService()};
	std::shared_ptr<ManualExecutor> executor = std::make_shared<ManualExecutor>();
	MotionDatabaseAuthoringDocument document;
	std::size_t starts = 0U;
	std::filesystem::path authoring = root / "Locomotion.authoring.json";
	std::filesystem::path output = root / "Assets/Locomotion.temotiondb";
	~MotionJobFixture() {
		jobs.CancelAllRecordings();
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}
	void Prepare(TestContext& context) {
		const auto assets = root / "Assets";
		WriteProductionClip(context, assets / "Motions/Walk.teanim", Guid(44U), 0x12345678U);
		document = MakeDocument();
		document.sources.front().sourcePath = "Motions/Walk.teanim";
		document.sources.front().sourceClipId = 0U;
		document.sources.front().sourceClipFingerprint = 0U;
		MotionDatabaseProductionAuthoringHost host(assets, root / "Build", executor);
		context.Expect(host.ResolveMotionDatabaseSourceIdentity(document.sources.front(), document.compatibilityFingerprint) == AnimationStatus::Ok,
			"The durable job must use a real checked clip identity.");
		context.Expect(SaveMotionDatabaseAuthoringDocumentAtomically(authoring, document) == AnimationStatus::Ok,
			"The actual job adapter needs a checked persisted authoring document.");
	}
	AutomationDispatchResult Start(std::filesystem::path destination = {}) {
		AutomationJobCreateRequest request;
		request.kind = "motion_database_build";
		request.capability = "animation.motion_database.build";
		request.idempotencyKey = "motion-job-protected-pump-" + std::to_string(++starts);
		request.cancellable = true;
		return jobs.StartMotionDatabaseBuild(request, authoring, destination.empty() ? output : destination,
			std::make_unique<MotionDatabaseProductionAuthoringHost>(root / "Assets", root / "Build", executor));
	}
};

// Small typed compiler input isolates publication ownership from editor source IO.
MotionDatabaseCompileInput PacketInput() {
	MotionDatabaseCompileInput input;
	input.assetId = Guid(10U);
	input.compatibilityFingerprint = 0x1234U;
	input.sourceFingerprint = 0x5678U;
	input.authoringGeneration = 7U;
	for(std::uint64_t id = 1U; id <= 3U; ++id) {
		MotionDatabaseSourceSample sample;
		sample.stableSampleId = id; sample.sourceClipId = 100U + id;
		sample.sourceTick = static_cast<AnimationFixedTick>(id * 10U);
		sample.continuityGroup = static_cast<std::uint32_t>(id);
		for(std::size_t feature = 0U; feature < sample.features.size(); ++feature)
			sample.features[feature] = static_cast<float>(id * 10U + feature) / 100.0F;
		input.samples.push_back(sample);
	}
	return input;
}
std::size_t StagedArtifactCount(const std::filesystem::path& output) {
	std::size_t count = 0U;
	for(const auto& entry : std::filesystem::directory_iterator(output.parent_path()))
		if(entry.path().filename().string().starts_with(output.filename().string() + ".staged.")) ++count;
	return count;
}
std::size_t SuccessfulJobs(EditorAutomationJobService& store) {
	std::size_t count = 0U;
	for(const auto& job : store.List(256U)) if(job.state == AutomationJobState::Succeeded) ++count;
	return count;
}

AnimationAutomationMutation Mutation(std::size_t index, bool dryRun = false) {
	return {"animation.test.capacity", "capacity-" + std::to_string(index),
		"capacity-transport", 7U, dryRun, {{"index", index}}};
}

// These owners keep durable recording sessions alive and cancel them before
// the animator or unique temporary destination disappears after any assertion.
struct PublishedRecordingFixture {
	AnimationSystem& system = AnimationSystem::Instance();
	EditorAutomationJobService store;
	AnimationAutomationJobs jobs{store};
	AnimationAutomationTransactions transactions;
	AnimatorInstanceHandle handle{};
	std::filesystem::path root = std::filesystem::temp_directory_path()
		/ ("TerraEngine-JobRetry-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
	PublishedRecordingFixture() { std::filesystem::create_directories(root); }
	~PublishedRecordingFixture() {
		jobs.CancelAllRecordings();
		if(handle.IsValid()) (void)system.InvalidateAnimatorInstance(handle);
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}
	void Publish(TestContext& context) {
		system.Initialize();
		AnimatorInstanceDescriptor descriptor;
		descriptor.ownerObjectId[0U] = 87U;
		descriptor.componentGeneration = 1U;
		handle = system.CreateAnimatorInstance(descriptor);
		context.Expect(handle.IsValid(), "The retry fixture needs a live animator.");
		auto skeleton = std::make_shared<SkeletonAsset>();
		skeleton->assetId.value[0U] = 91U;
		skeleton->compatibilityFingerprint = 0xA11CEULL;
		skeleton->rootJointIndex = 0U;
		SkeletonJoint joint;
		joint.stableId = 101U; joint.parentIndex = -1;
		joint.normalizedSourcePath = "/root";
		skeleton->joints.push_back(joint);
		AnimatorPlaybackBinding binding;
		binding.skeleton = std::move(skeleton);
		binding.jointStableIds = {101U};
		binding.fallbackPolicy = AnimationFallbackPolicy::BindPose;
		context.Expect(system.BindAnimatorPlayback(handle, binding) == AnimationStatus::MissingAsset,
			"Missing clip fallback must still produce a safe published bind pose.");
		const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
			system.DeclareFrameWorkloads(1.0F / 60.0F), "recording-job-retry");
		context.Expect(plan.valid && plan.completed, "The real animation publication boundary must finish.");
		const auto authority = system.LoadPublishedAuthorityFrame();
		const auto* pose = authority ? authority->Find(handle) : nullptr;
		context.Expect(pose != nullptr && pose->snapshot != nullptr
			&& pose->skeleton.assetId.IsValid() && pose->ownerObjectId != TerraEngine::EMPTY_UUID
			&& pose->skeletonCompatibilityFingerprint == 0xA11CEULL
			&& pose->jointStableIds == std::vector<std::uint64_t>{101U},
			"Recording retry needs a published pose with the exact bound skeleton, owner and joint metadata.");
	}
	nlohmann::json Arguments() const {
		// The production request schema distinguishes unsigned JSON integers from
		// signed values; use its released wire types for every non-negative bound.
		return {
			{"target", {{"animator", {{"slotIndex", handle.slotIndex}, {"generation", handle.generation}}}}},
			{"assetId", TerraEngine::ToHexString(TerraEngine::GenerateUUID())},
			{"skeletonAssetId", TerraEngine::ToHexString(TerraEngine::GenerateUUID())},
			{"sourceAnimatorId", TerraEngine::ToHexString(TerraEngine::GenerateUUID())},
			{"compatibilityFingerprint", 0xA11CEULL},
			{"bindCurrentPublication", true}, {"publicationGeneration", 0U},
			{"jointStableIds", nlohmann::json::array({101U})},
			{"destinationPath", "Captured/Retry.teanim"},
			{"firstTick", 0U}, {"lastTick", 4U}, {"sampleRate", 60U},
			{"clock", "fixed_simulation"}, {"segmentDurationTicks", 4U},
			{"channels", nlohmann::json::array({{{"stableChannelId", 1U}, {"stableTargetId", 101U},
				{"name", "Root Translation"}, {"kind", "bone_translation"}, {"interpolation", "linear"}}})},
			{"sourceSceneIdentity", "job-retry-fixture"}, {"sourceSessionIdentity", "job-retry-session"},
			{"idempotencyKey", "recording-job-retry"}, {"correlationId", "transport-one"},
			{"expectedGeneration", 1U}
		};
	}
};
}

void RegisterAnimationJobRetryLifecycleTests(TestRegistry& registry) {
	registry.Add("Editor.Animation.Automation.Retry.RealWorkerCompletesAfterSubmittingAdapterIsReleased", [](TestContext& context) {
		auto executor = std::make_shared<ObservedWorkerExecutor>();
		MotionJobFixture fixture; fixture.Prepare(context);
		// Unwind this guard before the fixture cancels sessions/removes its files.
		// The compiler closure may still borrow those private paths on any failure.
		struct DrainBeforeFixtureCleanup {
			std::shared_ptr<ObservedWorkerExecutor> executor;
			~DrainBeforeFixtureCleanup() { executor->ReleaseAndDrain(); }
		} drain{executor};
		const auto ownerThread = std::this_thread::get_id();
		AutomationDispatchResult started;
		{
			AnimationAutomationJobs submittingAdapter(fixture.service.GetJobService());
			AutomationJobCreateRequest request;
			request.kind = "motion_database_build";
			request.capability = "animation.motion_database.build";
			request.idempotencyKey = "real-worker-disconnected-adapter";
			request.cancellable = true;
			started = submittingAdapter.StartMotionDatabaseBuild(request, fixture.authoring, fixture.output,
				std::make_unique<MotionDatabaseProductionAuthoringHost>(fixture.root / "Assets", fixture.root / "Build", executor));
		} // The submitting adapter is gone; the service retains the real session.
		context.Expect(started.ok && executor->WaitForEntry(), "Actual motion preparation must enter its owned worker after admission.");
		executor->Release();
		context.Expect(executor->JoinPreparation(), "The actual checked motion cook must settle within the coordination bound.");
		context.Expect(!std::filesystem::exists(fixture.output), "Finished worker preparation must remain private before the protected owner pump.");
		const auto pumpThread = std::this_thread::get_id();
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		// There has been no job Get/List, adapter poll, or transport request since
		// start. Observe the store only after the real protected service boundary.
		const auto retained = fixture.service.GetJobService().Get(started.data.value("jobId", ""));
		MotionDatabaseAsset decoded;
		const auto bytes = ReadBytes(fixture.output);
		std::cout << "Real motion preparation identities: owner=" << ownerThread << " worker=" << executor->beforeWork
			<< ':' << executor->afterWork << " protected-pump=" << pumpThread << " no-client-polls=1\n";
		context.Expect(executor->beforeWork != std::thread::id{} && executor->beforeWork != ownerThread
			&& executor->afterWork == executor->beforeWork && pumpThread == ownerThread,
			"Actual source preparation/cook must execute on the observed worker and publication on its original owner.");
		context.Expect(retained && retained->state == AutomationJobState::Succeeded
			&& DeserializeMotionDatabaseAsset(bytes, fixture.document.compatibilityFingerprint, decoded) == AnimationStatus::Ok
			&& decoded.assetId == fixture.document.assetId && !decoded.samples.empty(),
			"The protected owner must finish the unpolled retained job and publish its real checked database after adapter release.");
		// A new real session can use the same retained service after completion.
		// This observes released admission/session ownership without peeking into it.
		const auto next = fixture.Start();
		context.Expect(next.ok, "A completed disconnected job must leave the same service usable for another real build.");
		context.Expect(fixture.jobs.Cancel(next.data.value("jobId", "")).ok, "Cleanup must cancel the newly admitted real session.");
		fixture.executor->RunNext();
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
	});

	registry.Add("Editor.Animation.Automation.Retry.WorkerPacketStaysPrivateUntilSharedInstall", [](TestContext& context) {
		auto executor = std::make_shared<ManualExecutor>();
		MotionDatabaseCompiler compiler(executor);
		const auto input = PacketInput();
		context.Expect(compiler.RequestBuild(input) == AnimationStatus::Queued && !compiler.GetPublished(),
			"Admission must not publish a mutable or unfinished worker product.");
		executor->RunNext();
		context.Expect(compiler.GetBuildProgress().stage == MotionDatabaseBuildStage::ReadyToPublish && !compiler.GetPublished(),
			"A validated worker packet must remain private until the explicit publication boundary.");
		const MotionDatabaseAsset* validatedAddress = nullptr;
		const MotionDatabaseSample* validatedSamples = nullptr;
		const auto status = compiler.PollAndPublish(input.assetId, input.compatibilityFingerprint, input.authoringGeneration,
			[&](const MotionDatabaseAsset& asset, const std::filesystem::path& path) {
				validatedAddress = &asset; validatedSamples = asset.samples.data();
				return path.empty() && asset.runtimeGeneration == 1U ? AnimationStatus::Ok : AnimationStatus::Rejected;
			});
		const auto published = compiler.GetPublished();
		context.Expect(status == AnimationStatus::Ok && published && published->asset.get() == validatedAddress
			&& published->asset->samples.data() == validatedSamples && published->generation == 1U
			&& published->contentFingerprint == ComputeMotionDatabaseContentFingerprint(*published->asset),
			"Installation must share the validator's immutable owner and sample storage, with the worker's checked content.");
	});
	registry.Add("Editor.Animation.Automation.Retry.InvertedWorkersPreserveNewestAndSuccessfulGenerations", [](TestContext& context) {
		auto executor = std::make_shared<ManualExecutor>();
		MotionDatabaseCompiler compiler(executor);
		auto input = PacketInput();
		context.Expect(compiler.RequestBuild(input) == AnimationStatus::Queued, "The older worker must queue.");
		input.authoringGeneration = 8U; input.sourceFingerprint++;
		context.Expect(compiler.RequestBuild(input) == AnimationStatus::Queued, "The newer worker must queue before the older finishes.");
		executor->RunAt(1U);
		context.Expect(compiler.PollAndPublish(input.assetId, input.compatibilityFingerprint, input.authoringGeneration) == AnimationStatus::Ok,
			"A completed newest worker may publish while its older peer is unfinished.");
		const auto first = compiler.GetPublished();
		context.Expect(first && first->generation == 1U && first->asset->runtimeGeneration == 1U, "Unpublished old work must not consume a generation.");
		input.authoringGeneration = 9U; input.sourceFingerprint++;
		context.Expect(compiler.RequestBuild(input) == AnimationStatus::Queued, "A subsequent request must capture the next successful generation.");
		executor->RunNext();
		executor->RunNext();
		context.Expect(compiler.PollAndPublish(input.assetId, input.compatibilityFingerprint, input.authoringGeneration) == AnimationStatus::StaleHandle
			&& compiler.GetPublished()->asset == first->asset, "Late old completion must leave the first immutable owner untouched.");
		context.Expect(compiler.PollAndPublish(input.assetId, input.compatibilityFingerprint, input.authoringGeneration) == AnimationStatus::Ok,
			"The subsequent matching worker must still publish after obsolete work retires.");
		const auto second = compiler.GetPublished();
		context.Expect(second && second->generation == 2U && second->asset->runtimeGeneration == 2U
			&& first->asset->runtimeGeneration == 1U, "Successful generation allocation must remain contiguous without mutating pinned readers.");
	});
	registry.Add("Editor.Animation.Automation.Retry.ReadyPacketCancellationPreservesLastGoodAndCleansStage", [](TestContext& context) {
		MotionJobFixture fixture; fixture.Prepare(context);
		MotionDatabaseProductionAuthoringHost host(fixture.root / "Assets", fixture.root / "Build", fixture.executor);
		MotionDatabaseAuthoringSession session(fixture.executor);
		auto request = [&] { return session.RequestBuild(fixture.document, host.CreateMotionDatabaseSourceResolver(fixture.document), fixture.output); };
		context.Expect(request() == AnimationStatus::Queued, "The last-good compiler request must queue.");
		fixture.executor->RunNext();
		context.Expect(session.Poll(fixture.document) == AnimationStatus::Ok, "A real complete last-good database must publish.");
		const auto lastGood = ReadBytes(fixture.output);
		const auto owner = session.GetPublished()->asset;
		context.Expect(request() == AnimationStatus::Queued, "A subsequent database request must queue.");
		fixture.executor->RunNext();
		context.Expect(StagedArtifactCount(fixture.output) == 1U && ReadBytes(fixture.output) == lastGood,
			"Ready worker output must have one private staged file and preserve visible last-good bytes.");
		session.CancelBuild();
		context.Expect(session.Poll(fixture.document) == AnimationStatus::Cancelled && StagedArtifactCount(fixture.output) == 0U
			&& ReadBytes(fixture.output) == lastGood && session.GetPublished()->asset == owner,
			"Cancel after readiness must retire the private artifact while preserving the pinned last-good owner and file.");
	});
	registry.Add("Editor.Animation.Automation.Retry.ReadyPacketOwnerResetCleansStageWithoutPublishing", [](TestContext& context) {
		MotionJobFixture fixture; fixture.Prepare(context);
		MotionDatabaseProductionAuthoringHost host(fixture.root / "Assets", fixture.root / "Build", fixture.executor);
		auto session = std::make_unique<MotionDatabaseAuthoringSession>(fixture.executor);
		auto request = [&] { return session->RequestBuild(fixture.document, host.CreateMotionDatabaseSourceResolver(fixture.document), fixture.output); };
		context.Expect(request() == AnimationStatus::Queued, "The initial database request must queue.");
		fixture.executor->RunNext();
		context.Expect(session->Poll(fixture.document) == AnimationStatus::Ok, "The reset fixture needs an actual last-good database.");
		const auto lastGood = ReadBytes(fixture.output);
		const auto pinned = session->GetPublished()->asset;
		context.Expect(request() == AnimationStatus::Queued, "The private replacement must queue.");
		fixture.executor->RunNext();
		context.Expect(StagedArtifactCount(fixture.output) == 1U, "The completed replacement must still be private before reset.");
		session.reset();
		context.Expect(StagedArtifactCount(fixture.output) == 0U && ReadBytes(fixture.output) == lastGood
			&& pinned && !pinned->samples.empty(), "Reset must release abandoned staging while pinned immutable readers and last-good output remain valid.");
	});
	registry.Add("Editor.Animation.Automation.Retry.FairPumpAdvancesReadyJobsPastUnreadyAndNewArrival", [](TestContext& context) {
		MotionJobFixture fixture; fixture.Prepare(context);
		const auto first = fixture.Start(fixture.output.parent_path() / "Slow.temotiondb");
		const auto second = fixture.Start(fixture.output.parent_path() / "ReadyB.temotiondb");
		const auto third = fixture.Start(fixture.output.parent_path() / "ReadyC.temotiondb");
		context.Expect(first.ok && second.ok && third.ok, "Three independent real motion jobs must queue.");
		fixture.executor->RunAt(1U); fixture.executor->RunAt(1U);
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		context.Expect(SuccessfulJobs(fixture.service.GetJobService()) == 0U, "The first fair turn must not block waiting for the unfinished first worker.");
		const auto arriving = fixture.Start(fixture.output.parent_path() / "NewArrival.temotiondb");
		context.Expect(arriving.ok, "An arriving job must join the same durable fairness scope.");
		fixture.executor->RunAt(1U);
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		context.Expect(fixture.service.GetJobService().Get(second.data.value("jobId", ""))->state == AutomationJobState::Succeeded
			&& fixture.service.GetJobService().Get(third.data.value("jobId", ""))->state == AutomationJobState::Succeeded,
			"An unfinished earlier job and a new arrival must not starve jobs already waiting for publication.");
		fixture.executor->RunNext();
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		context.Expect(SuccessfulJobs(fixture.service.GetJobService()) == 4U, "Every retained real job must eventually receive its fair protected turn.");
	});
	registry.Add("Editor.Animation.Automation.Retry.BurstPublicationUsesOneTurnAndReleasesAdmissionBudget", [](TestContext& context) {
		MotionJobFixture fixture; fixture.Prepare(context);
		for(std::size_t index = 0U; index < 16U; ++index) {
			const auto started = fixture.Start(fixture.output.parent_path() / ("Burst-" + std::to_string(index) + ".temotiondb"));
			context.Expect(started.ok, "The documented 16-job concurrent budget must be admitted.");
		}
		const auto full = fixture.Start(fixture.output.parent_path() / "OverBudget.temotiondb");
		context.Expect(!full.ok && full.error == ErrorCategory::QueueFull, "Ready-but-unpublished jobs must still own their admission slots.");
		fixture.executor->RunAll();
		context.Expect(SuccessfulJobs(fixture.service.GetJobService()) == 0U, "A complete worker burst must remain invisible before protected turns.");
		for(std::size_t turn = 1U; turn <= 16U; ++turn) {
			fixture.service.PumpAnimationRecordingsAtProtectedFrame();
			context.Expect(SuccessfulJobs(fixture.service.GetJobService()) == turn,
				"Each protected frame may settle exactly one ready publication transaction from the retained burst.");
			if(turn == 1U) context.Expect(fixture.Start(fixture.output.parent_path() / "AdmittedAfterTerminal.temotiondb").ok,
				"A terminal protected acknowledgement must release its concurrent admission slot.");
		}
	});

	registry.Add("Editor.Animation.Automation.Retry.MotionBuildCompletesWithoutClientPolling", [](TestContext& context) {
		MotionJobFixture fixture; fixture.Prepare(context);
		const auto started = fixture.Start();
		context.Expect(started.ok, "The real motion job must enqueue before its worker executes.");
		const std::string jobId = started.data.value("jobId", "");
		fixture.executor->RunNext();
		context.Expect(!std::filesystem::exists(fixture.output), "Worker completion must wait for protected atomic publication.");
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		// Read the store directly. AnimationAutomationJobs::Get pumps pre-fix and
		// would conceal the disconnected-client lifetime defect being tested.
		const auto retained = fixture.service.GetJobService().Get(jobId);
		context.Expect(retained && retained->state == AutomationJobState::Succeeded,
			"Completed motion jobs must become terminal at the editor protected frame without a client poll.");
		const auto bytes = ReadBytes(fixture.output);
		MotionDatabaseAsset decoded;
		context.Expect(DeserializeMotionDatabaseAsset(bytes, fixture.document.compatibilityFingerprint, decoded) == AnimationStatus::Ok
			&& decoded.assetId == fixture.document.assetId && !decoded.samples.empty(),
			"The protected pump must publish the checked requested asset, not merely mark its job complete.");
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		context.Expect(ReadBytes(fixture.output) == bytes, "Another protected frame must preserve the same completed artifact.");
	});
	registry.Add("Editor.Animation.Automation.Retry.CanceledMotionBuildSettlesWithoutClientPolling", [](TestContext& context) {
		MotionJobFixture fixture; fixture.Prepare(context);
		// Establish a real checked last-good artifact through the current explicit
		// pump before simulating client disconnection for the subsequent rebuild.
		const auto initial = fixture.Start();
		context.Expect(initial.ok, "The last-good setup job must be admitted.");
		fixture.executor->RunNext();
		(void)fixture.jobs.PumpMotionDatabaseBuild(initial.data.value("jobId", ""));
		const auto lastGood = ReadBytes(fixture.output);
		context.Expect(!lastGood.empty(), "Cancellation must preserve a real compiled last-good database.");
		const auto started = fixture.Start();
		context.Expect(started.ok, "The cancellable motion job must start through its real adapter.");
		const std::string jobId = started.data.value("jobId", "");
		context.Expect(fixture.jobs.Cancel(jobId).ok, "Cancellation must reach the real owned session.");
		fixture.executor->RunNext();
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		const auto retained = fixture.service.GetJobService().Get(jobId);
		context.Expect(retained && retained->state == AutomationJobState::Canceled
			&& ReadBytes(fixture.output) == lastGood,
			"A disconnected canceled job must settle at the protected frame while preserving its previous file.");
	});

	registry.Add("Editor.Animation.Automation.Retry.CapacityPreservesReplayAndReset", [](TestContext& context) {
		AnimationAutomationTransactions transactions;
		std::size_t callbacks = 0U;
		auto commit = [&]() {
			++callbacks;
			AutomationDispatchResult result; result.ok = true;
			result.data = {{"callbackOrdinal", callbacks}};
			return result;
		};
		for(std::size_t index = 0U; index < 256U; ++index) {
			// A dry-run receipt still occupies the documented retention window.
			context.Expect(transactions.ExecuteWithCommit(Mutation(index, index % 2U == 0U), 7U, commit).ok,
				"All 256 bounded receipts must be admitted.");
		}
		context.ExpectEqual(std::size_t{256U}, callbacks, "Each unique accepted request invokes its callback once.");
		const auto overflow = transactions.ExecuteWithCommit(Mutation(256U), 7U, commit);
		context.Expect(!overflow.ok && overflow.error == ErrorCategory::QueueFull && callbacks == 256U,
			"Capacity rejection must occur before the 257th callback.");
		auto replay = Mutation(0U, true);
		replay.correlationId = "transport-two";
		replay.payload["correlationId"] = "transport-two";
		const auto retained = transactions.ExecuteWithCommit(replay, 7U, commit);
		context.Expect(retained.ok && retained.data.value("idempotencyDisposition", "") == "replay"
			&& callbacks == 256U && transactions.RetainedCount() == 256U,
			"Full admission must not evict or reexecute a retained receipt.");
		replay.payload["index"] = 999U;
		const auto conflict = transactions.ExecuteWithCommit(replay, 7U, commit);
		context.Expect(!conflict.ok && conflict.error == ErrorCategory::Conflict && callbacks == 256U,
			"Conflicting retries must stay rejected even at full capacity.");
		transactions.Reset();
		context.Expect(transactions.RetainedCount() == 0U
			&& transactions.ExecuteWithCommit(Mutation(0U, true), 7U, commit).ok && callbacks == 257U,
			"Explicit project/session reset starts a new documented receipt scope.");
	});
	registry.Add("Editor.Animation.Automation.Retry.FullServiceReceiptsPreserveJobReadListAndCancel", [](TestContext& context) {
		EditorContext editor;
		MotionJobFixture fixture;
		editor.projectManager = std::make_shared<ProjectManager>();
		std::string projectError;
		context.Expect(editor.projectManager->CreateProject(fixture.root, "ReceiptCapacity", ProjectTemplate::Blank, &projectError),
			"The public service capacity check needs an actual open disposable project: " + projectError);
		fixture.service.SetEditorContext(&editor);
		fixture.Prepare(context);
		PublishedRecordingFixture published; published.Publish(context);
		const auto call = [&](std::string_view operation, const nlohmann::json& arguments, const std::string& request) {
			return fixture.service.HandleTool(operation, arguments, WriteMode::WritesEnabled, request);
		};
		// The first public animation call adopts this real project generation before
		// the cancellable job starts; otherwise project reset would cancel that job.
		const auto initialized = call("animation.runtime.diagnostics", nlohmann::json::object(), "capacity-project-adoption");
		context.Expect(initialized.value("ok", false), "The public animation service must adopt its actual project: " + initialized.dump());
		const auto started = fixture.Start();
		context.Expect(started.ok, "A real cancellable motion job must exist before filling mutation receipts.");
		const std::string jobId = started.data.value("jobId", "");
		const auto request = [&](std::size_t index) {
			return nlohmann::json{
				{"target", {{"animator", {{"slotIndex", published.handle.slotIndex}, {"generation", published.handle.generation}}}}},
				{"commands", nlohmann::json::array({{{"kind", "play"}}})},
				{"idempotencyKey", "public-capacity-" + std::to_string(index)},
				{"correlationId", "public-capacity-transport"}, {"expectedGeneration", 1U}, {"dryRun", true}};
		};
		const auto authorityBefore = published.system.LoadPublishedAuthorityFrame();
		for(std::size_t index = 0U; index < 256U; ++index) {
			const auto accepted = call("animation.runtime.parameters", request(index), "capacity-fill-" + std::to_string(index));
			context.Expect(accepted.value("ok", false) && accepted["data"].value("dryRun", false)
				&& accepted["data"].value("visibilityBoundary", "") == "none_dry_run",
				"Every public dry run must reach real typed command validation and retain its receipt: " + accepted.dump());
		}
		const auto expectFull = [&](const nlohmann::json& response) {
			context.Expect(!response.value("ok", true) && response.contains("error")
				&& response["error"].value("category", "") == "queue_full",
				"The 257th public mutation must reject at retained receipt admission: " + response.dump());
		};
		expectFull(call("animation.runtime.parameters", request(256U), "capacity-overflow-before-reads"));
		const auto read = call("animation.job.get", {{"jobId", jobId}}, "capacity-job-read");
		const auto listed = call("jobs.list", {{"limit", 256U}}, "capacity-job-list");
		context.Expect(read.value("ok", false) && listed.value("ok", false)
			&& listed["data"].contains("jobs") && listed["data"]["jobs"].is_array()
			&& std::any_of(listed["data"]["jobs"].begin(), listed["data"]["jobs"].end(),
				[&](const nlohmann::json& job) { return job.value("jobId", "") == jobId; }),
			"Public Get/List must still return the actual retained job at full mutation capacity. Read=" + read.dump() + " List=" + listed.dump());
		const auto canceled = call("animation.job.cancel", {{"jobId", jobId}}, "capacity-job-cancel");
		context.Expect(canceled.value("ok", false), "Public cancellation must progress at full receipt capacity: " + canceled.dump());
		fixture.executor->RunNext();
		fixture.service.PumpAnimationRecordingsAtProtectedFrame();
		const auto terminal = call("animation.job.get", {{"jobId", jobId}}, "capacity-job-terminal");
		const auto terminalJob = fixture.service.GetJobService().Get(jobId);
		context.Expect(terminal.value("ok", false) && terminalJob && terminalJob->state == AutomationJobState::Canceled
			&& !std::filesystem::exists(fixture.output) && published.system.LoadPublishedAuthorityFrame() == authorityBefore,
			"Full-capacity cancellation must settle the real session without output or dry-run animation publication: " + terminal.dump());
		expectFull(call("animation.runtime.parameters", request(256U), "capacity-overflow-after-cancel"));
		const auto replay = call("animation.runtime.parameters", request(0U), "capacity-retained-retry");
		context.Expect(replay.value("ok", false) && replay["data"].value("idempotencyDisposition", "") == "replay",
			"Read/cancel progress must not evict retained public mutation receipts: " + replay.dump());
		std::cout << "Public animation receipt scope: admitted=256 overflow-rejected-before-and-after-cancel=1 Get/List/Cancel=completed job="
			<< jobId << " state=canceled owner=" << std::this_thread::get_id() << '\n';
	});
	registry.Add("Editor.Animation.Automation.Retry.RecordingStartIsAtMostOnce", [](TestContext& context) {
		PublishedRecordingFixture fixture; fixture.Publish(context);
		auto args = fixture.Arguments();
		const auto first = ExecuteAnimationRuntimeOperation("animation.recording.start", args,
			fixture.transactions, 1U, {}, &fixture.jobs, fixture.root);
		context.Expect(first.ok && fixture.store.Size() == 1U,
			"First start must admit one real recording job. Error=" + std::to_string(static_cast<int>(first.error))
			+ " Message=" + first.message + " Data=" + first.data.dump()
			+ " Jobs=" + std::to_string(fixture.store.Size()));
		args["correlationId"] = "transport-two";
		const auto retry = ExecuteAnimationRuntimeOperation("animation.recording.start", args,
			fixture.transactions, 1U, {}, &fixture.jobs, fixture.root);
		context.Expect(retry.ok && first.data.value("jobId", "") == retry.data.value("jobId", "")
			&& fixture.store.Size() == 1U && fixture.transactions.RetainedCount() == 1U,
			"A transport retry must replay the same durable job and never create another recorder.");
	});
	registry.Add("Editor.Animation.Automation.Retry.StaleRecordingStartCreatesNoJob", [](TestContext& context) {
		PublishedRecordingFixture fixture; fixture.Publish(context);
		auto args = fixture.Arguments(); args["expectedGeneration"] = 99U;
		const auto rejected = ExecuteAnimationRuntimeOperation("animation.recording.start", args,
			fixture.transactions, 1U, {}, &fixture.jobs, fixture.root);
		context.Expect(!rejected.ok && rejected.error == ErrorCategory::StaleTarget
			&& fixture.store.Size() == 0U && fixture.transactions.RetainedCount() == 0U,
			"A stale generation must reject before job or receipt publication. Error="
			+ std::to_string(static_cast<int>(rejected.error)) + " Message=" + rejected.message
			+ " Data=" + rejected.data.dump() + " Jobs=" + std::to_string(fixture.store.Size()));
	});
}
}
