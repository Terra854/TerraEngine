// AnimationAbiTests.cpp - Hostile-input and copied-buffer coverage for ABI v1.

#include "../TestHarness.h"

#include "Scripting/AnimationScriptAbi.h"
#include "Scripting/ScriptGlue.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/AnimationAuthorityPublication.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		template<typename Record>
		Record MakeRecord(NativeAnimationRecordKind kind)
		{
			Record record{};
			record.header = {
				static_cast<std::uint32_t>(sizeof(Record)),
				kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(kind)};
			return record;
		}

		NativeAnimationMotionMatchQueryRequestV2 MakeMotionMatchQueryV2()
		{
			NativeAnimationMotionMatchQueryRequestV2 request =
				MakeRecord<NativeAnimationMotionMatchQueryRequestV2>(
					NativeAnimationRecordKind::MotionMatchQueryRequestV2);
			request.animator = {999999U, 1U};
			request.databaseAssetId = {11U, 22U};
			request.databaseGeneration = 1U;
			request.featureCount = kNativeAnimationMotionFeatureCount;
			request.schemaFingerprint = 0x12345678abcdef01ULL;
			request.queryId = 41U;
			request.correlationId = 42U;
			request.targetTick = 7;
			request.maximumPhaseDistance = UINT16_MAX;
			request.phaseWeight = 1U;
			request.maximumCandidates = kNativeAnimationMotionDefaultTopK;
			request.maximumSamplesToVisit = 1024U;
			request.maximumDiagnosticExclusions = 32U;
			request.queryFields =
				static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::Identity)
				| static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::Features)
				| static_cast<std::uint32_t>(
					NativeAnimationMotionMatchQueryField::FeatureWeights)
				| static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::Budgets);
			for(std::uint32_t feature = 0U;
				feature < kNativeAnimationMotionFeatureCount; ++feature) {
				request.features[feature] = static_cast<std::int16_t>(feature);
				request.featureWeights[feature] = 1U;
			}
			return request;
		}

		// ABI tests exercise the exported bridge against a known initialized runtime.
		// This fixture also removes component-owned animators and prior consumer
		// declarations so neighboring suites cannot change stale-handle outcomes.
		class AnimationAbiRuntimeFixture final {
		public:
			AnimationAbiRuntimeFixture()
				: system_(AnimationSystem::Instance())
				, previousHeadless_(system_.IsHeadlessExecutionRequired())
				, previousWorkerCount_(ThreadPool::instance().getThreadCount())
			{
				system_.Shutdown();
				AnimationConsumerRegistry::Instance().Clear();
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				system_.SetHeadlessExecutionRequired(true);
				system_.Initialize();
			}

			~AnimationAbiRuntimeFixture()
			{
				system_.Shutdown();
				AnimationConsumerRegistry::Instance().Clear();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				system_.SetHeadlessExecutionRequired(previousHeadless_);
				ThreadPool::instance().shutdown();
				if(previousWorkerCount_ != 0U)
					ThreadPool::instance().initialize(previousWorkerCount_);
			}

			[[nodiscard]] bool PublishEmptyAuthorityFrame()
			{
				AnimatorInstanceDescriptor descriptor;
				for(std::size_t index = 0U; index < descriptor.ownerObjectId.size(); ++index)
					descriptor.ownerObjectId[index] = static_cast<std::uint8_t>(0xA0U + index);
				descriptor.componentGeneration = 1U;
				const AnimatorInstanceHandle handle = system_.CreateAnimatorInstance(descriptor);
				if(!handle.IsValid()) return false;

				auto skeleton = std::make_shared<SkeletonAsset>();
				skeleton->assetId.value[0U] = 0xA6U;
				skeleton->compatibilityFingerprint = 0x414249454D505459ULL;
				skeleton->rootJointIndex = 0U;
				SkeletonJoint root;
				root.stableId = 0xA601U;
				root.parentIndex = -1;
				root.normalizedSourcePath = "/abi-empty-event-root";
				skeleton->joints.push_back(std::move(root));
				AnimatorPlaybackBinding binding;
				binding.skeleton = std::move(skeleton);
				binding.playback.timePolicy = AnimatorTimePolicy::Fixed;
				// An intentionally missing clip exercises the ordinary safe-pose
				// publication path without authored events or external asset files.
				if(system_.BindAnimatorPlayback(handle, std::move(binding)) != AnimationStatus::MissingAsset)
					return false;

				std::vector<Scheduling::FrameWorkloadDeclaration> workloads =
					system_.DeclareFrameWorkloads(1.0F / 60.0F);
				const Scheduling::FrameExecutionPlan plan =
					Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(workloads), "animation-abi-empty-runtime");
				if(!plan.valid || !plan.completed) return false;
				const auto authority = system_.LoadPublishedAuthorityFrame();
				return authority && authority->IsValid() && authority->pages
					&& !authority->pages->empty() && authority->Find(handle) && authority->sidecars
					&& authority->sidecars->events.empty();
			}

		private:
			AnimationSystem& system_;
			bool previousHeadless_ = false;
			std::size_t previousWorkerCount_ = 0U;
		};
	}

	void RegisterAnimationAbiTests(TestRegistry& registry)
	{
		registry.Add("Animation.AbiV1.HeaderAndRecordsRemainCopiedFixedWidth", [](TestContext& context) {
			context.ExpectEqual(static_cast<std::size_t>(8U), sizeof(NativeAnimationAbiHeader),
				"ABI records must retain the exact size/version/kind prefix.");
			context.ExpectEqual(static_cast<std::size_t>(0U), offsetof(NativeAnimationCommandRequest, header),
				"Command validation must begin at the common header.");
			context.ExpectEqual(static_cast<std::size_t>(0U), offsetof(NativeAnimationStateSnapshot, header),
				"Snapshot validation must begin at the common header.");
			context.ExpectEqual(static_cast<std::uint16_t>(1U), Native_Animation_GetAbiVersion(),
				"The exported bridge must advertise ABI v1 without touching runtime state.");
			context.Expect(sizeof(NativeAnimationAnimatorHandle) == 8U
				&& sizeof(NativeAnimationUuid) == 16U,
				"Generation-safe handles and UUIDs must never contain a native pointer.");
		});

		registry.Add("Animation.AbiV1.RejectsNullTruncatedVersionAndKindBeforePayload", [](TestContext& context) {
			NativeAnimationCommandReceipt receipt = MakeRecord<NativeAnimationCommandReceipt>(
				NativeAnimationRecordKind::CommandReceipt);
			const NativeAnimationCommandReceipt sentinel = receipt;
			context.Expect(Native_Animation_SubmitCommand(nullptr, &receipt) == NativeAnimationStatus::InvalidArgument,
				"A null request must fail before reading caller memory.");
			context.Expect(std::memcmp(&receipt, &sentinel, sizeof(receipt)) == 0,
				"A rejected null request must not partially overwrite its output.");

			NativeAnimationCommandRequest request = MakeRecord<NativeAnimationCommandRequest>(
				NativeAnimationRecordKind::CommandRequest);
			request.header.size = sizeof(NativeAnimationAbiHeader);
			context.Expect(Native_Animation_SubmitCommand(&request, &receipt) == NativeAnimationStatus::TruncatedInput,
				"A short record must fail before any command payload is inspected.");
			request.header.size = sizeof(request);
			request.header.version = 2U;
			context.Expect(Native_Animation_SubmitCommand(&request, &receipt) == NativeAnimationStatus::UnsupportedMajorVersion,
				"Unknown major versions must fail closed.");
			request.header.version = kNativeAnimationAbiVersion;
			request.header.kind = static_cast<std::uint16_t>(NativeAnimationRecordKind::StateSnapshot);
			context.Expect(Native_Animation_SubmitCommand(&request, &receipt) == NativeAnimationStatus::InvalidType,
				"A valid-size record with the wrong kind must not be reinterpreted.");
		});

		registry.Add("Animation.AbiV1.ValidatesEnumsIdentityAndGenerationSafeHandles", [](TestContext& context) {
			AnimationAbiRuntimeFixture runtime;
			NativeAnimationCommandRequest request = MakeRecord<NativeAnimationCommandRequest>(
				NativeAnimationRecordKind::CommandRequest);
			request.animator = {0U, 1U};
			request.command = static_cast<NativeAnimationCommandKind>(0xffffffffU);
			request.parameterType = NativeAnimationParameterType::Float;
			request.producerId = 1U;
			request.producerSequence = 1U;
			request.correlationId = 2U;
			request.idempotencyKey = 3U;
			request.cancellationToken = 4U;
			NativeAnimationCommandReceipt receipt = MakeRecord<NativeAnimationCommandReceipt>(
				NativeAnimationRecordKind::CommandReceipt);
			context.Expect(Native_Animation_SubmitCommand(&request, &receipt) == NativeAnimationStatus::InvalidArgument,
				"Unknown commands must return an explicit status instead of entering the queue.");

			request.command = NativeAnimationCommandKind::Play;
			request.animator = {0xffffffffU, 0U};
			context.Expect(Native_Animation_SubmitCommand(&request, &receipt) == NativeAnimationStatus::InvalidArgument,
				"An invalid slot/generation pair must fail before queue submission.");
			request.animator = {999999U, 1U};
			context.Expect(Native_Animation_SubmitCommand(&request, &receipt) == NativeAnimationStatus::StaleHandle,
				"A structurally valid but dead generation must receive StaleHandle.");
		});

		registry.Add("Animation.AbiV1.CountQueryAndUndersizedWritesAreAtomic", [](TestContext& context) {
			AnimationAbiRuntimeFixture runtime;
			NativeAnimationBufferInfo info = MakeRecord<NativeAnimationBufferInfo>(
				NativeAnimationRecordKind::BufferInfo);
			const NativeAnimationStatus unpublishedStatus = Native_Animation_CopyEventBatch(nullptr, 0U, &info);
			context.Expect(unpublishedStatus == NativeAnimationStatus::NotReady
				&& info.requiredCount == 0U && info.writtenCount == 0U,
				"Before its first authority publication, a zero-capacity query must report NotReady without claiming events.");
			context.Expect(runtime.PublishEmptyAuthorityFrame(),
				"The count query must then observe a real live animator publication with an empty event sidecar.");
			info = MakeRecord<NativeAnimationBufferInfo>(NativeAnimationRecordKind::BufferInfo);
			const NativeAnimationStatus emptyStatus = Native_Animation_CopyEventBatch(nullptr, 0U, &info);
			context.Expect(emptyStatus == NativeAnimationStatus::Ok
				&& info.requiredCount == 0U && info.writtenCount == 0U,
				"After an empty authority publication, a null zero-capacity query must report the exact event count.");

			NativeAnimationAnimatorHandle stale{100000U, 1U};
			info = MakeRecord<NativeAnimationBufferInfo>(NativeAnimationRecordKind::BufferInfo);
			context.Expect(Native_Animation_CopyBones(&stale, nullptr, 0U, &info) == NativeAnimationStatus::StaleHandle
				&& info.writtenCount == 0U,
				"A stale handle must not write any array prefix.");
			info = MakeRecord<NativeAnimationBufferInfo>(NativeAnimationRecordKind::Event);
			context.Expect(Native_Animation_CopyEventBatch(nullptr, 0U, &info) == NativeAnimationStatus::InvalidType,
				"The required-count output must itself carry the correct versioned kind.");
		});

		registry.Add("Animation.AbiV1.HeaderFuzzNeverReadsMalformedPayload", [](TestContext& context) {
			std::uint32_t state = 0x9e3779b9U;
			for(std::uint32_t iteration = 0U; iteration < 2048U; ++iteration) {
				state = state * 1664525U + 1013904223U;
				NativeAnimationCommandRequest request{};
				request.header.size = state & 0x3ffU;
				request.header.version = static_cast<std::uint16_t>(state >> 16U);
				request.header.kind = static_cast<std::uint16_t>(state);
				NativeAnimationCommandReceipt receipt = MakeRecord<NativeAnimationCommandReceipt>(
					NativeAnimationRecordKind::CommandReceipt);
				const NativeAnimationStatus status = Native_Animation_SubmitCommand(&request, &receipt);
				context.Expect(status == NativeAnimationStatus::TruncatedInput
					|| status == NativeAnimationStatus::UnsupportedMajorVersion
					|| status == NativeAnimationStatus::InvalidType
					|| status == NativeAnimationStatus::InvalidArgument,
					"Malformed headers must terminate in a bounded validation status.");
			}
		});

		registry.Add("Animation.MotionMatchAbiV2.AppendsFixedRecordsWithoutChangingV1", [](TestContext& context) {
			context.ExpectEqual(static_cast<std::size_t>(72U),
				sizeof(NativeAnimationMotionMatchRequest),
				"The frozen v1 motion request stride must not grow.");
			context.ExpectEqual(static_cast<std::size_t>(64U),
				sizeof(NativeAnimationMotionMatchReceipt),
				"The frozen v1 motion receipt stride must not grow.");
			context.ExpectEqual(static_cast<std::size_t>(96U),
				sizeof(NativeAnimationMotionMatchSnapshot),
				"The frozen v1 motion snapshot stride must not grow.");
			context.ExpectEqual(static_cast<std::size_t>(2432U),
				sizeof(NativeAnimationMotionMatchQueryRequestV2),
				"The V2 query must retain its copied 48-lane and 256-exclusion layout.");
			context.ExpectEqual(static_cast<std::size_t>(88U),
				sizeof(NativeAnimationMotionMatchQueryReceiptV2),
				"The V2 receipt stride is part of the managed parity contract.");
			context.ExpectEqual(static_cast<std::size_t>(272U),
				sizeof(NativeAnimationMotionMatchResultV2),
				"The V2 result must retain all nine exact block-cost lanes.");
			context.Expect(static_cast<std::uint16_t>(
				NativeAnimationRecordKind::MotionMatchQueryRequestV2) == 33U
				&& static_cast<std::uint16_t>(
					NativeAnimationRecordKind::MotionMatchQueryReceiptV2) == 34U
				&& static_cast<std::uint16_t>(
					NativeAnimationRecordKind::MotionMatchResultV2) == 35U,
				"Versioned record kinds must be append-only for old managed assemblies.");
		});

		registry.Add("Animation.MotionMatchAbiV2.RejectsMalformedCopiedQueriesAtomically", [](TestContext& context) {
			NativeAnimationMotionMatchQueryRequestV2 request = MakeMotionMatchQueryV2();
			NativeAnimationMotionMatchQueryReceiptV2 receipt =
				MakeRecord<NativeAnimationMotionMatchQueryReceiptV2>(
					NativeAnimationRecordKind::MotionMatchQueryReceiptV2);
			receipt.queryId = 0xfeedU;
			const NativeAnimationMotionMatchQueryReceiptV2 sentinel = receipt;

			request.requiredTags = 1U;
			request.excludedTags = 1U;
			context.Expect(Native_Animation_SubmitMotionMatchQueryV2(&request, &receipt)
				== NativeAnimationStatus::InvalidArgument,
				"Conflicting required and excluded tags must fail before queue admission.");
			context.Expect(std::memcmp(&receipt, &sentinel, sizeof(receipt)) == 0,
				"Rejected V2 payloads must not partially overwrite their receipts.");

			request = MakeMotionMatchQueryV2();
			request.queryFields |= static_cast<std::uint32_t>(
				NativeAnimationMotionMatchQueryField::Exclusions);
			request.excludedSampleCount = 2U;
			request.excludedSampleIds[0] = 9U;
			request.excludedSampleIds[1] = 8U;
			context.Expect(Native_Animation_SubmitMotionMatchQueryV2(&request, &receipt)
				== NativeAnimationStatus::InvalidArgument,
				"Exclusion IDs must be copied in stable, strictly increasing order.");
			context.Expect(std::memcmp(&receipt, &sentinel, sizeof(receipt)) == 0,
				"A rejected exclusion list must leave the caller's receipt untouched.");

			request = MakeMotionMatchQueryV2();
			request.header.size = sizeof(NativeAnimationAbiHeader);
			context.Expect(Native_Animation_SubmitMotionMatchQueryV2(&request, &receipt)
				== NativeAnimationStatus::TruncatedInput,
				"The complete inline query must be present before any payload is read.");
		});

		registry.Add("Animation.MotionMatchAbiV2.ValidatesThenReportsCentralAdmission", [](TestContext& context) {
			AnimationAbiRuntimeFixture runtime;
			NativeAnimationMotionMatchQueryRequestV2 request = MakeMotionMatchQueryV2();
			NativeAnimationMotionMatchQueryReceiptV2 receipt =
				MakeRecord<NativeAnimationMotionMatchQueryReceiptV2>(
					NativeAnimationRecordKind::MotionMatchQueryReceiptV2);
			const NativeAnimationStatus status =
				Native_Animation_SubmitMotionMatchQueryV2(&request, &receipt);
			context.Expect(status == NativeAnimationStatus::StaleHandle
				|| status == NativeAnimationStatus::Cancelled,
				"A valid copied query must reach the central runtime ownership check.");
			context.Expect(receipt.header.kind == static_cast<std::uint16_t>(
				NativeAnimationRecordKind::MotionMatchQueryReceiptV2)
				&& receipt.validatedFields == request.queryFields
				&& receipt.scheduledFields == 0U,
				"A failed central admission must report validation without claiming scheduling.");
			context.Expect((receipt.flags & static_cast<std::uint32_t>(
				NativeAnimationMotionMatchQueryReceiptFlag::RequiresRuntimeQueryIntegration)) == 0U,
				"The complete V2 query now has a central evaluator and must not advertise an integration gap.");
			context.Expect((receipt.flags & static_cast<std::uint32_t>(
				NativeAnimationMotionMatchQueryReceiptFlag::PayloadRetained)) == 0U,
				"A request rejected by central admission must not claim retained payload lifetime.");

			NativeAnimationMotionMatchResultV2 output{};
			std::memset(&output, 0x5a, sizeof(output));
			const NativeAnimationMotionMatchResultV2 outputSentinel = output;
			NativeAnimationBufferInfo info = MakeRecord<NativeAnimationBufferInfo>(
				NativeAnimationRecordKind::BufferInfo);
			const NativeAnimationStatus copyStatus = Native_Animation_CopyMotionMatchResultsV2(
				&request.animator, &output, 1U, &info);
			context.Expect(copyStatus == NativeAnimationStatus::StaleHandle
				|| copyStatus == NativeAnimationStatus::Cancelled,
				"A V2 read must preserve the existing generation-safe runtime check.");
			context.Expect(info.writtenCount == 0U
				&& std::memcmp(&output, &outputSentinel, sizeof(output)) == 0,
				"Failed result reads must never publish a partial V2 record.");
		});
	}
}
