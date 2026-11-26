// PostProcessFoundationTests.cpp - production settings publication and graph regressions.
// These headless cases inspect the real controller and render declarations. GPU
// image correctness and native target retirement remain installed-renderer checks.
#include "TestHarness.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsController.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/RHI/Vulkan/VulkanPostProcessTargets.h"
#include "Renderer/RHI/Vulkan/VulkanPostProcessPasses.h"
#include "Renderer/RHI/Vulkan/VulkanReadbackConversion.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>

namespace TerraEngine::Tests
{
	// The production controller exposes one friend-only checkpoint per owner.
	// Allocation faults therefore affect this fixture without replacing global
	// allocation or disturbing unrelated runtime tasks in the test process.
	struct GraphicsSettingsControllerTestAccess
	{
		struct AllocationFailure
		{
			std::string_view stage;
			std::size_t occurrence = 1u;
			std::size_t observed = 0u;
		};

		static void Inject(Renderer::GraphicsSettings::GraphicsSettingsController& controller, AllocationFailure& failure)
		{
			controller.allocationCheckpointContext = &failure;
			controller.allocationCheckpoint = [](void* state, const char* stage) {
				auto& fault = *static_cast<AllocationFailure*>(state);
				if(fault.stage == stage && ++fault.observed == fault.occurrence) throw std::bad_alloc{};
			};
		}

		static void Clear(Renderer::GraphicsSettings::GraphicsSettingsController& controller)
		{
			controller.allocationCheckpoint = nullptr;
			controller.allocationCheckpointContext = nullptr;
		}
	};

	namespace
	{
		using namespace Renderer::GraphicsSettings;

		// Both descriptors use real renderer setting IDs. Their small fixture
		// ranges keep admission and atomicity checks independent from device limits.
		struct SettingsFixture
		{
			std::array<GraphicsSettingDescriptor, 2> descriptors;
			std::array<GraphicsTarget, 1> targets{};
			GraphicsPreparationContext context;
			GraphicsSettingsController controller;

			SettingsFixture()
			{
				descriptors[0].id = GraphicsSettingId::ShadowResolution;
				descriptors[1].id = GraphicsSettingId::MaxUploadedLights;
				for(auto& descriptor : descriptors)
				{
					descriptor.valueKind = GraphicsValueKind::Unsigned32;
					descriptor.defaultValue = std::uint32_t{2};
					descriptor.minimum = std::uint32_t{1};
					descriptor.maximum = std::uint32_t{16};
					descriptor.gameScope = true;
					descriptor.available = true;
				}
				context.lifecycle = {1u, 2u, 3u, 4u};
				context.descriptors = descriptors;
				context.validTargets = targets;
				context.frameId = 11u;
				controller.Invalidate(context);
			}

			GraphicsSettingsRequest Request(GraphicsSettingId id, GraphicsValue value) const
			{
				GraphicsSettingsRequest request;
				request.lifecycle = context.lifecycle;
				request.baseRevision = controller.Snapshot()->revision;
				request.patches.push_back({id, std::move(value)});
				return request;
			}

			void ConfigureFixedExposure(GraphicsActivation activation)
			{
				auto& descriptor = descriptors[0];
				descriptor.id = GraphicsSettingId::FixedExposureCompensation;
				descriptor.valueKind = GraphicsValueKind::Scalar;
				descriptor.defaultValue = 0.0f;
				descriptor.minimum = -8.0f;
				descriptor.maximum = 8.0f;
				descriptor.activation = activation;
			}
		};

		std::optional<std::uint32_t> ReadUnsigned(const GraphicsSettingsSnapshot& snapshot, GraphicsSettingId id)
		{
			const auto found = std::find_if(snapshot.values.begin(), snapshot.values.end(), [id](const auto& value) {
				return value.id == id;
			});
			if(found == snapshot.values.end()) return std::nullopt;
			if(const auto value = std::get_if<std::uint32_t>(&found->value)) return *value;
			return std::nullopt;
		}

		GraphicsRequestReceipt PublishNext(TestContext& context, SettingsFixture& fixture)
		{
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate.has_value(), "An admitted valid request must produce a candidate.");
			return fixture.controller.Publish(std::move(*candidate), fixture.context);
		}

		bool SameResourceState(const Renderer::RenderResourceState& left, const Renderer::RenderResourceState& right)
		{
			return left.resourceId == right.resourceId && left.accessMode == right.accessMode
				&& left.stage == right.stage && left.ownerPassId == right.ownerPassId && left.semantic == right.semantic;
		}

		bool SamePass(const Renderer::RenderPassNode& left, const Renderer::RenderPassNode& right)
		{
			// Compare every declaration field so a disabled append cannot silently
			// rewrite ownership, dependencies or an existing geometry resource.
			return left.passId == right.passId && left.viewType == right.viewType
				&& std::equal(left.inputs.begin(), left.inputs.end(), right.inputs.begin(), right.inputs.end(), SameResourceState)
				&& std::equal(left.outputs.begin(), left.outputs.end(), right.outputs.begin(), right.outputs.end(), SameResourceState)
				&& left.dependencies == right.dependencies && left.workPolicy == right.workPolicy
				&& left.failurePolicy == right.failurePolicy && left.ownership == right.ownership
				&& left.canOverlap == right.canOverlap && left.fallbackReason == right.fallbackReason
				&& left.profilingLabel == right.profilingLabel && left.postProcessKind == right.postProcessKind;
		}

		Renderer::PostProcessGraphSettings SpatialSettings()
		{
			Renderer::PostProcessGraphSettings settings;
			settings.executionAvailable = true;
			settings.spatialAA = true;
			settings.antiAliasingMode = 1u;
			settings.internalSize = settings.outputSize = {640u, 360u};
			settings.resourceFormatGeneration = 3u;
			return settings;
		}

		Renderer::Vulkan::PostProcessFormatSupport CompleteColorFormatSupport()
		{
			// This copied query result exercises production admission policy only.
			// It does not create a Vulkan image or claim a device supports the format.
			Renderer::Vulkan::PostProcessFormatSupport support;
			support.optimalFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
				| VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
				| VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
				| VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			support.imageQueryResult = VK_SUCCESS;
			support.imageProperties.maxExtent = {640u, 360u, 1u};
			support.imageProperties.maxMipLevels = 1u;
			support.imageProperties.maxArrayLayers = 1u;
			support.imageProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
			return support;
		}

		constexpr VkImageUsageFlags kFixtureColorUsage = VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

		GraphicsTarget FixtureCameraTarget(std::uint8_t cameraByte, std::uint32_t outputId)
		{
			GraphicsCameraTarget camera;
			camera.cameraId[0] = cameraByte;
			camera.sceneGeneration = 3u;
			camera.objectLifetimeGeneration = 4u;
			camera.componentGeneration = 5u;
			camera.bindingGeneration = 6u;
			GraphicsTarget target;
			target.scope = GraphicsScope::Camera;
			target.camera = camera;
			target.view = GraphicsViewIdentity{RenderViewType::Game, outputId, 8u};
			return target;
		}
	}

	void RegisterPostProcessFoundationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.Foundation.AdmissionPreservesPublishedSnapshot", [](TestContext& context) {
			SettingsFixture fixture;
			const auto original = fixture.controller.Snapshot();
			const auto admitted = fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
			context.Expect(admitted.requestId != 0u && admitted.sequence != 0u,
				"Admission must assign stable request and order identities.");
			context.Expect(fixture.controller.Snapshot() == original,
				"A producer must not publish settings while admitting a request.");
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate.has_value() && fixture.controller.Snapshot() == original,
				"Worker preparation must retain the prior immutable frame publication.");
			context.Expect(ReadUnsigned(candidate->snapshot, GraphicsSettingId::ShadowResolution) == 4u,
				"The prepared candidate must contain the complete requested value.");
		});

		registry.Add("Renderer.PostProcess.Foundation.SequenceOrder", [](TestContext& context) {
			SettingsFixture fixture;
			// Revision zero is the public no-precondition sentinel. Seed a real
			// publication so both following producers name the same exact revision.
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 3u));
			context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
				"The fixture must publish a nonzero baseline revision before testing stale requests.");
			const auto first = fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
			const auto second = fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 8u));
			context.Expect(second.sequence > first.sequence && second.requestId != first.requestId,
				"Independent requests must retain a deterministic admission sequence.");
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate.has_value() && candidate->requestId == first.requestId,
				"Preparation must consume the oldest pending request first.");
			const auto applied = fixture.controller.Publish(std::move(*candidate), fixture.context);
			context.Expect(applied.state == GraphicsRequestState::Applied,
				"The first complete candidate must publish successfully.");
			// Both queued requests named the old revision. Applying the first must
			// leave the second unable to overwrite it with an obsolete baseline.
			fixture.controller.Prepare(fixture.context);
			const auto receipt = fixture.controller.GetReceipt(second.requestId);
			context.Expect(receipt && receipt->state == GraphicsRequestState::Rejected
				&& receipt->reason == GraphicsRequestReason::StaleRevision,
				"An older revision precondition must fail after an intervening publication.");
			context.Expect(ReadUnsigned(*fixture.controller.Snapshot(), GraphicsSettingId::ShadowResolution) == 4u,
				"The stale queued request must preserve the first applied value.");
		});

		registry.Add("Renderer.PostProcess.Foundation.RejectedBatchIsAtomic", [](TestContext& context) {
			SettingsFixture fixture;
			const auto original = fixture.controller.Snapshot();
			auto request = fixture.Request(GraphicsSettingId::ShadowResolution, 4u);
			request.patches.push_back({GraphicsSettingId::MaxUploadedLights, std::uint32_t{99}});
			const auto admitted = fixture.controller.Enqueue(std::move(request));
			const auto candidate = fixture.controller.Prepare(fixture.context);
			const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(!candidate && receipt && receipt->state == GraphicsRequestState::Rejected
				&& receipt->reason == GraphicsRequestReason::InvalidValue,
				"One out-of-range field must reject the whole settings batch.");
			context.Expect(fixture.controller.Snapshot() == original,
				"Rejecting a batch must retain the original snapshot and revision.");
		});

		registry.Add("Renderer.PostProcess.Foundation.StaleLifecycleRejected", [](TestContext& context) {
			SettingsFixture fixture;
			const auto original = fixture.controller.Snapshot();
			auto request = fixture.Request(GraphicsSettingId::ShadowResolution, 4u);
			++request.lifecycle.rendererSessionGeneration;
			const auto admitted = fixture.controller.Enqueue(std::move(request));
			context.Expect(!fixture.controller.Prepare(fixture.context),
				"A request from a different renderer lifetime must not prepare.");
			const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(receipt && receipt->reason == GraphicsRequestReason::StaleLifetime
				&& (receipt->state == GraphicsRequestState::Rejected || receipt->state == GraphicsRequestState::Cancelled),
				"A stale request must expose a terminal lifetime reason.");
			context.Expect(fixture.controller.Snapshot() == original,
				"An obsolete renderer request cannot change the active view.");
		});

		registry.Add("Renderer.PostProcess.Foundation.DuplicateFieldsCannotPartiallyApply", [](TestContext& context) {
			SettingsFixture fixture;
			const auto original = fixture.controller.Snapshot();
			auto request = fixture.Request(GraphicsSettingId::ShadowResolution, 4u);
			request.patches.push_back({GraphicsSettingId::ShadowResolution, std::uint32_t{8}});
			const auto admitted = fixture.controller.Enqueue(std::move(request));
			context.Expect(!fixture.controller.Prepare(fixture.context),
				"A duplicated setting must reject the batch instead of choosing an implicit winner.");
			const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(receipt && receipt->state == GraphicsRequestState::Rejected,
				"The caller must be able to observe that the duplicate patch never applied.");
			context.Expect(fixture.controller.Snapshot() == original,
				"Conflicting values in one request must preserve the prior publication.");
		});

		registry.Add("Renderer.PostProcess.Foundation.WrongValueKindRejectsWholeBatch", [](TestContext& context) {
			SettingsFixture fixture;
			const auto original = fixture.controller.Snapshot();
			auto request = fixture.Request(GraphicsSettingId::ShadowResolution, 4u);
			request.patches.push_back({GraphicsSettingId::MaxUploadedLights, 8.0f});
			const auto admitted = fixture.controller.Enqueue(std::move(request));
			context.Expect(!fixture.controller.Prepare(fixture.context),
				"A floating point patch cannot silently stand in for the catalog's unsigned budget.");
			const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(receipt && receipt->state == GraphicsRequestState::Rejected
				&& receipt->reason == GraphicsRequestReason::InvalidValue,
				"A wrong typed value must be reported without normalizing the new API's request.");
			context.Expect(fixture.controller.Snapshot() == original,
				"A type error in the second field cannot publish the first valid field.");
		});

		registry.Add("Renderer.PostProcess.Foundation.PreparedLifetimeRecheckedAtPublish", [](TestContext& context) {
			SettingsFixture fixture;
			const auto original = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate.has_value(), "The current lifetime must prepare before the rebuild race.");
			++fixture.context.lifecycle.backendResourceGeneration;
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context);
			context.Expect(receipt.state == GraphicsRequestState::Rejected
				&& receipt.reason == GraphicsRequestReason::StaleLifetime,
				"Publishing must recheck backend lifetime after preparation.");
			context.Expect(fixture.controller.Snapshot() == original,
				"A prepared candidate cannot publish resources from an obsolete generation.");
		});

		registry.Add("Renderer.PostProcess.Foundation.ResourceFailurePreservesSnapshot", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.descriptors[0].activation = GraphicsActivation::Rebuild;
			const auto original = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate && candidate->activation == GraphicsActivation::Rebuild,
				"The candidate must retain the actual required resource activation class.");
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context, false);
			context.Expect(receipt.state == GraphicsRequestState::Rejected
				&& receipt.reason == GraphicsRequestReason::PreparationFailed,
				"A failed backend preparation must produce an actionable terminal receipt.");
			context.Expect(fixture.controller.Snapshot() == original,
				"The renderer must keep its previous coherent settings after resource preparation fails.");
		});

		registry.Add("Renderer.PostProcess.Foundation.PublishedSnapshotRemainsImmutable", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
			const auto first = PublishNext(context, fixture);
			const auto retained = fixture.controller.Snapshot();
			context.Expect(first.state == GraphicsRequestState::Applied
				&& first.appliedFrameId == fixture.context.frameId,
				"An applied receipt must name the frame that published it.");
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 8u));
			++fixture.context.frameId;
			const auto second = PublishNext(context, fixture);
			const auto current = fixture.controller.Snapshot();
			context.Expect(second.state == GraphicsRequestState::Applied && current != retained
				&& current->revision > retained->revision,
				"A later commit must create a separately owned publication with a newer revision.");
			context.Expect(ReadUnsigned(*retained, GraphicsSettingId::ShadowResolution) == 4u
				&& ReadUnsigned(*current, GraphicsSettingId::ShadowResolution) == 8u,
				"A consumer retaining an older snapshot must keep its original effective values.");
		});

		registry.Add("Renderer.PostProcess.Foundation.CameraTargetsPublishIndependently", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.descriptors[0].cameraScope = true;
			GraphicsCameraTarget camera;
			camera.cameraId[0] = 7u;
			camera.sceneGeneration = 3u;
			camera.objectLifetimeGeneration = 4u;
			camera.componentGeneration = 5u;
			camera.bindingGeneration = 6u;
			GraphicsTarget target;
			target.scope = GraphicsScope::Camera;
			target.camera = camera;
			target.view = GraphicsViewIdentity{RenderViewType::Game, 2u, 8u};
			std::array validTargets{fixture.targets[0], target};
			fixture.context.validTargets = validTargets;
			const auto game = fixture.controller.Snapshot();
			auto request = fixture.Request(GraphicsSettingId::ShadowResolution, 4u);
			request.target = target;
			fixture.controller.Enqueue(std::move(request));
			auto prepared = fixture.controller.Prepare(fixture.context);
			context.Expect(prepared && !fixture.controller.Snapshot(target),
				"Preparing a camera patch must not create a published camera snapshot.");
			const auto receipt = fixture.controller.Publish(std::move(*prepared), fixture.context);
			const auto publishedCamera = fixture.controller.Snapshot(target);
			context.Expect(receipt.state == GraphicsRequestState::Applied && publishedCamera
				&& publishedCamera->target == target
				&& ReadUnsigned(*publishedCamera, GraphicsSettingId::ShadowResolution) == 4u,
				"The protected commit must publish only the selected camera's values.");
			context.Expect(fixture.controller.Snapshot() == game,
				"A camera-scoped publication must retain the game-wide snapshot.");
			++target.camera->objectLifetimeGeneration;
			context.Expect(!fixture.controller.Snapshot(target),
				"A recycled object generation must not resolve an old camera publication.");
		});

		registry.Add("Renderer.PostProcess.Foundation.InvalidatedPendingWorkGetsTerminalReceipts", [](TestContext& context) {
			SettingsFixture fixture;
			const auto first = fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
			const auto second = fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::MaxUploadedLights, 8u));
			const auto prepared = fixture.controller.Prepare(fixture.context);
			context.Expect(prepared && fixture.controller.HasPendingWork(),
				"The fixture must retain one prepared head and one queued successor before invalidation.");
			++fixture.context.lifecycle.projectGeneration;
			fixture.controller.Invalidate(fixture.context);
			for(const auto id : {first.requestId, second.requestId})
			{
				const auto receipt = fixture.controller.GetReceipt(id);
				context.Expect(receipt && receipt->state == GraphicsRequestState::Cancelled
					&& receipt->reason == GraphicsRequestReason::StaleLifetime,
					"Both prepared and queued old-project requests must report cancellation.");
			}
			context.Expect(!fixture.controller.HasPendingWork(),
				"Project invalidation must not leave an unpublishable head blocking the new owner.");
			context.Expect(fixture.controller.Snapshot()->lifecycle == fixture.context.lifecycle,
				"The new project must receive a publication owned by its own lifecycle.");
		});

		registry.Add("Renderer.PostProcess.Foundation.CameraPatchProjectsToEachLiveView", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.descriptors[0].cameraScope = true;
			GraphicsCameraTarget camera;
			camera.cameraId[0] = 11u;
			camera.sceneGeneration = 3u;
			camera.objectLifetimeGeneration = 4u;
			camera.componentGeneration = 5u;
			camera.bindingGeneration = 6u;
			GraphicsTarget cameraTarget;
			cameraTarget.scope = GraphicsScope::Camera;
			cameraTarget.camera = camera;
			auto firstTarget = cameraTarget;
			firstTarget.view = GraphicsViewIdentity{RenderViewType::Game, 2u, 8u};
			auto secondTarget = cameraTarget;
			secondTarget.view = GraphicsViewIdentity{RenderViewType::Scene, 1u, 9u};
			std::array targets{firstTarget, secondTarget};
			fixture.context.validTargets = targets;
			auto request = fixture.Request(GraphicsSettingId::ShadowResolution, 8u);
			request.target = cameraTarget;
			fixture.controller.Enqueue(std::move(request));
			context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
				"A camera-scoped patch must apply even when the camera renders several views.");
			const auto first = fixture.controller.Snapshot(firstTarget);
			const auto second = fixture.controller.Snapshot(secondTarget);
			context.Expect(first && second && first->target == firstTarget && second->target == secondTarget
				&& ReadUnsigned(*first, GraphicsSettingId::ShadowResolution) == 8u
				&& ReadUnsigned(*second, GraphicsSettingId::ShadowResolution) == 8u,
				"Selected effective queries must retain exact view identity while sharing the camera override.");
			context.Expect(!fixture.controller.Snapshot(cameraTarget),
				"An unqualified effective query must report ambiguity when several views are live.");

			// Retiring one output does not destroy the camera that owns the patch.
			// Keep the second view live, then reset the camera through the same API.
			std::array remainingTargets{secondTarget};
			fixture.context.validTargets = remainingTargets;
			fixture.controller.Invalidate(fixture.context);
			const auto remaining = fixture.controller.Snapshot(secondTarget);
			context.Expect(!fixture.controller.Snapshot(firstTarget) && remaining
				&& ReadUnsigned(*remaining, GraphicsSettingId::ShadowResolution) == 8u,
				"Retiring one view must preserve the live camera's override on the remaining view.");
			GraphicsSettingsRequest reset;
			reset.operation = GraphicsRequestOperation::Reset;
			reset.target = cameraTarget;
			reset.lifecycle = fixture.context.lifecycle;
			reset.baseRevision = remaining->revision;
			reset.resetSettings.push_back(GraphicsSettingId::ShadowResolution);
			fixture.controller.Enqueue(std::move(reset));
			context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
				"The same camera scope must reset its override after output retirement.");
			const auto inherited = fixture.controller.Snapshot(secondTarget);
			context.Expect(inherited && ReadUnsigned(*inherited, GraphicsSettingId::ShadowResolution) == 2u
				&& ReadUnsigned(*second, GraphicsSettingId::ShadowResolution) == 8u,
				"Reset restores inheritance for the live view and preserves already-captured frame snapshots.");
		});

		registry.Add("Renderer.PostProcess.Foundation.BackendResourceRebuildRetainsCommittedSettings", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.descriptors[0].cameraScope = true;
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
			context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
				"The game settings must have a committed value before the backend resource rebuild.");
			GraphicsCameraTarget camera;
			camera.cameraId[0] = 9u;
			camera.sceneGeneration = 3u;
			camera.objectLifetimeGeneration = 4u;
			camera.componentGeneration = 5u;
			camera.bindingGeneration = 6u;
			GraphicsTarget cameraTarget;
			cameraTarget.scope = GraphicsScope::Camera;
			cameraTarget.camera = camera;
			cameraTarget.view = GraphicsViewIdentity{RenderViewType::Game, 2u, 8u};
			std::array targets{fixture.targets[0], cameraTarget};
			fixture.context.validTargets = targets;
			auto cameraRequest = fixture.Request(GraphicsSettingId::ShadowResolution, 8u);
			cameraRequest.target = cameraTarget;
			cameraRequest.baseRevision = 0u;
			fixture.controller.Enqueue(std::move(cameraRequest));
			context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
				"The live camera must have its own committed override before resource invalidation.");

			// A new resource generation invalidates prepared GPU ownership without
			// ending the game, scene, or camera that owns these effective choices.
			const auto retainedGame = fixture.controller.Snapshot();
			const auto retainedCamera = fixture.controller.Snapshot(cameraTarget);
			context.Expect(retainedCamera != nullptr, "The camera override must be published before the rebuild race.");
			const auto preparedRequest = fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 12u));
			const auto queuedRequest = fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::MaxUploadedLights, 14u));
			const auto prepared = fixture.controller.Prepare(fixture.context);
			context.Expect(prepared && prepared->requestId == preparedRequest.requestId,
				"The fixture must retain both a prepared head and a queued request from the old resource generation.");
			++fixture.context.lifecycle.backendResourceGeneration;
			fixture.controller.Invalidate(fixture.context);
			for(const auto id : {preparedRequest.requestId, queuedRequest.requestId})
			{
				const auto receipt = fixture.controller.GetReceipt(id);
				context.Expect(receipt && receipt->state == GraphicsRequestState::Cancelled
					&& receipt->reason == GraphicsRequestReason::StaleLifetime,
					"Resource invalidation must cancel both kinds of outstanding old-resource work.");
			}
			const auto currentGame = fixture.controller.Snapshot();
			const auto currentCamera = fixture.controller.Snapshot(cameraTarget);
			context.Expect(currentCamera && !fixture.controller.HasPendingWork()
				&& ReadUnsigned(*currentGame, GraphicsSettingId::ShadowResolution) == 4u
				&& ReadUnsigned(*currentCamera, GraphicsSettingId::ShadowResolution) == 8u,
				"Rebuilding backend resources must preserve committed game values and still-live camera overrides.");
			context.Expect(retainedGame->lifecycle.backendResourceGeneration + 1u == fixture.context.lifecycle.backendResourceGeneration
				&& retainedCamera->lifecycle.backendResourceGeneration == retainedGame->lifecycle.backendResourceGeneration
				&& ReadUnsigned(*retainedGame, GraphicsSettingId::ShadowResolution) == 4u
				&& ReadUnsigned(*retainedCamera, GraphicsSettingId::ShadowResolution) == 8u,
				"Snapshots already captured by frame consumers must retain their old lifetime metadata and values.");

			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 10u));
			++fixture.context.frameId;
			context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
				"The new resource generation must admit and publish fresh work after stale work is cancelled.");
			context.Expect(ReadUnsigned(*fixture.controller.Snapshot(), GraphicsSettingId::ShadowResolution) == 10u
				&& ReadUnsigned(*fixture.controller.Snapshot(cameraTarget), GraphicsSettingId::ShadowResolution) == 8u,
				"Fresh game settings must retain the surviving camera's explicit override.");
		});

		registry.Add("Renderer.PostProcess.Foundation.PublicationBoundaryDeclaresSettingsAccess", [](TestContext& context) {
			// Scripts submit settings outside rendering, while this protected snapshot
			// boundary owns the point at which a complete configuration becomes visible.
			auto& renderer = RenderSystem::Instance();
			Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(renderer, renderer.SchedulerDeclaration());
			context.Expect(scheduler.BuildSchedule().valid,
				"The production scheduler must recognize every graphics settings resource declared by the renderer.");
			const auto workloads = renderer.DeclareFrameWorkloads(0.0f);
			const auto capture = std::find_if(workloads.begin(), workloads.end(), [](const auto& workload) {
				return workload.workloadName == "RenderPublicationCapture";
			});
			context.Expect(capture != workloads.end(), "The renderer must declare its settings publication boundary.");
			if(capture == workloads.end()) return;
			context.Expect(capture->phase == "Render" && capture->mainThreadRequired && !capture->workerEligible
				&& capture->hardBoundary
				&& capture->boundaryClassification == Scheduling::HardBoundaryClassification::Protected,
				"Settings publication must occur at the declared protected Render boundary.");
			context.Expect(std::find(capture->reads.begin(), capture->reads.end(), Scheduling::kResourceRuntimeRenderSettingsQueue) != capture->reads.end(),
				"Publication must declare the queued settings it consumes.");
			context.Expect(std::find(capture->writes.begin(), capture->writes.end(), Scheduling::kResourceRuntimeRenderSettings) != capture->writes.end(),
				"Publication must declare exclusive access to the effective settings it changes.");
			context.Expect(std::find(capture->reads.begin(), capture->reads.end(), "GraphicsSettingsRequestQueue") != capture->reads.end()
				&& std::find(capture->writes.begin(), capture->writes.end(), "GraphicsSettingsEffectiveSnapshots") != capture->writes.end(),
				"The typed settings queue and effective snapshots belong to the same protected boundary.");
		});

		registry.Add("Renderer.PostProcess.Foundation.DisabledProfileAddsNoEffectPasses", [](TestContext& context) {
			// A default snapshot uses the legacy effect-disabled profile. Its graph
			// must retain just the existing geometry chain for every requested view.
			Renderer::RenderFrameSnapshot snapshot;
			snapshot.viewRequests.push_back({RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(640u, 360u), 1u});
			snapshot.viewRequests.push_back({RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(640u, 360u), 2u});
			const Renderer::BackendCapabilityProfile capabilities;
			const auto passes = Renderer::BuildDefaultRenderPasses(snapshot, capabilities);
			context.ExpectEqual(std::size_t{8}, passes.size(), "Disabled effects must add no effect-only graph passes across either view.");
			constexpr std::array<std::string_view, 4> suffixes{
				".collect", ".virtual-geometry-main-occlusion", ".draw", ".virtual-geometry-post-occlusion"};
			for(std::size_t index = 0; index < passes.size(); ++index)
			{
				context.Expect(passes[index].passId.ends_with(suffixes[index % suffixes.size()]),
					"The disabled profile must keep the original geometry pass order without a processing bypass draw.");
			}
			context.Expect(Renderer::ValidateRenderGraph(passes).valid,
				"The disabled multi-view graph must retain valid, isolated resource dependencies.");
		});

		registry.Add("Renderer.PostProcess.Foundation.SharedPhysicalOutputKeepsIndependentGraphInstances", [](TestContext& context) {
			Renderer::RenderFrameSnapshot snapshot;
			Renderer::RenderFrameViewRequest first{RenderOutputKind::GameView, RenderViewType::Game, {640u, 360u}, 2u};
			auto second = first; second.viewInstanceIndex = 1u;
			snapshot.viewRequests = {first, second};
			auto passes = Renderer::BuildDefaultRenderPasses(snapshot, {});
			Renderer::AppendPostProcessRenderPasses(passes, first, SpatialSettings());
			Renderer::AppendPostProcessRenderPasses(passes, second, SpatialSettings());
			context.Expect(passes.size() == 12u && passes[2].passId != passes[6].passId
				&& passes[8].passId != passes[10].passId && Renderer::ValidateRenderGraph(passes).valid,
				"Two authored cameras sharing physical output two must retain independent geometry and effect resources through their planner instance ordinals.");
		});

		registry.Add("Renderer.PostProcess.Foundation.DisabledAppendPreservesGraphStorageAndIdentity", [](TestContext& context) {
			Renderer::RenderFrameSnapshot snapshot;
			snapshot.viewRequests.push_back({RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(640u, 360u), 2u});
			auto passes = Renderer::BuildDefaultRenderPasses(snapshot, {});
			const auto original = passes;
			const auto* storage = passes.data();
			const auto capacity = passes.capacity();
			const auto* firstOutputs = passes.front().outputs.data();
			Renderer::PostProcessGraphSettings disabled;
			disabled.executionAvailable = true;
			Renderer::AppendPostProcessRenderPasses(passes, snapshot.viewRequests.front(), disabled);
			auto unavailable = SpatialSettings();
			unavailable.executionAvailable = false;
			Renderer::AppendPostProcessRenderPasses(passes, snapshot.viewRequests.front(), unavailable);
			context.Expect(passes.data() == storage && passes.capacity() == capacity
				&& passes.front().outputs.data() == firstOutputs && passes.size() == original.size(),
				"Disabled and unavailable effects must retain graph storage and existing resource-vector identity.");
			context.Expect(std::equal(passes.begin(), passes.end(), original.begin(), original.end(), SamePass),
				"A disabled append must preserve every field of the existing graph declarations.");
			context.Expect(Renderer::BuildPostProcessGraphTopologySignature(disabled) == 0u
				&& Renderer::BuildPostProcessGraphTopologySignature(unavailable) == 0u,
				"Disabled and unavailable processing must contribute no independent effect topology key.");
		});

		registry.Add("Renderer.PostProcess.Foundation.EnabledGraphDeclaresTypedColorChain", [](TestContext& context) {
			Renderer::RenderFrameSnapshot snapshot;
			snapshot.viewRequests.push_back({RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(640u, 360u), 2u});
			snapshot.viewRequests.push_back({RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(640u, 360u), 1u});
			auto passes = Renderer::BuildDefaultRenderPasses(snapshot, {});
			const auto untouchedSceneDraw = passes[6];
			const auto settings = SpatialSettings();
			Renderer::AppendPostProcessRenderPasses(passes, snapshot.viewRequests.front(), settings);
			context.ExpectEqual(std::size_t{10}, passes.size(),
				"One spatial effect must append its AA pass and terminal overlay composite to the selected view.");
			const auto& draw = passes[2];
			const auto& aa = passes[8];
			const auto& composite = passes[9];
			context.Expect(aa.postProcessKind == Renderer::PostProcessPassKind::SpatialAA
				&& composite.postProcessKind == Renderer::PostProcessPassKind::OverlayComposite,
				"The appended passes must carry executable stage identities independent from profiling labels.");
			const auto sceneColor = std::find_if(draw.outputs.begin(), draw.outputs.end(), [](const auto& resource) {
				return resource.semantic == Renderer::RenderResourceSemantic::SceneColor;
			});
			context.Expect(sceneColor != draw.outputs.end(), "The selected geometry pass must publish typed scene color.");
			context.Expect(std::any_of(aa.inputs.begin(), aa.inputs.end(), [&](const auto& resource) {
				return resource.resourceId == sceneColor->resourceId
					&& resource.semantic == Renderer::RenderResourceSemantic::SceneColor
					&& resource.accessMode == Renderer::RenderAccessMode::Read;
			}), "The spatial effect must explicitly consume the selected draw's scene color.");
			context.Expect(std::find(aa.dependencies.begin(), aa.dependencies.end(), draw.passId) != aa.dependencies.end()
				&& std::find(composite.dependencies.begin(), composite.dependencies.end(), aa.passId) != composite.dependencies.end(),
				"The effect and overlay must declare the draw-to-AA-to-composite ordering.");
			context.Expect(aa.outputs.size() == 1u && composite.outputs.size() == 1u
				&& aa.outputs.front().semantic == Renderer::RenderResourceSemantic::ProcessedColor
				&& composite.outputs.front().semantic == Renderer::RenderResourceSemantic::CompositedColor,
				"Processed and composited outputs must remain distinguishable by typed resource semantics.");
			context.Expect(std::any_of(composite.inputs.begin(), composite.inputs.end(), [&](const auto& resource) {
				return resource.resourceId == aa.outputs.front().resourceId
					&& resource.semantic == Renderer::RenderResourceSemantic::ProcessedColor;
			}), "The overlay composite must consume the processed camera image.");
			context.Expect(SamePass(passes[6], untouchedSceneDraw),
				"Enabling an effect in Game View cannot modify the Scene View's draw declarations.");
			context.Expect(Renderer::ValidateRenderGraph(passes).valid,
				"The appended typed camera chain must pass production graph validation.");
		});

		registry.Add("Renderer.PostProcess.Foundation.EffectTopologyKeyTracksOnlyResolvedConfiguration", [](TestContext& context) {
			const auto original = SpatialSettings();
			const auto signature = Renderer::BuildPostProcessGraphTopologySignature(original);
			context.Expect(signature != 0u && signature == Renderer::BuildPostProcessGraphTopologySignature(original),
				"An enabled resolved configuration must have a stable independent effect key.");
			std::array<Renderer::PostProcessGraphSettings, 6> changed;
			changed.fill(original);
			changed[0].antiAliasingMode = 2u;
			changed[1].sampleCount = 4u;
			changed[2].internalSize.x /= 2u;
			changed[3].outputSize.y *= 2u;
			++changed[4].resourceFormatGeneration;
			changed[5].toneAndGrade = true;
			for(const auto& settings : changed)
			{
				context.Expect(Renderer::BuildPostProcessGraphTopologySignature(settings) != signature,
					"Changing a resolved stage, mode, sample count, extent or resource generation must invalidate the effect topology.");
			}
		});

		registry.Add("Renderer.PostProcess.Foundation.ViewSettingsRespectOutputKindAndLifetime", [](TestContext& context) {
			Renderer::RenderFrameSnapshot frame;
			auto game = std::make_shared<GraphicsSettingsSnapshot>();
			game->lifecycle = {1u, 2u, 3u, 4u};
			frame.graphicsSettings = game;
			auto visible = std::make_shared<GraphicsSettingsSnapshot>();
			visible->lifecycle = game->lifecycle;
			visible->target.scope = GraphicsScope::Camera;
			GraphicsCameraTarget camera;
			camera.cameraId[0] = 7u;
			camera.sceneGeneration = 1u; camera.objectLifetimeGeneration = 2u;
			camera.componentGeneration = 3u; camera.bindingGeneration = 4u;
			visible->target.camera = camera;
			visible->target.view = GraphicsViewIdentity{RenderViewType::Scene, 1u, 5u, RenderOutputKind::SceneView};
			auto offscreen = std::make_shared<GraphicsSettingsSnapshot>(*visible);
			offscreen->target.view->outputKind = RenderOutputKind::OffscreenTexture;
			frame.cameraGraphicsSettings = {offscreen, visible};
			frame.graphicsCameraIdentities = {camera};
			frame.publishedGraphicsCameraViews = {{visible->target, 640u, 360u}, {offscreen->target, 640u, 360u}};
			Renderer::RenderFrameViewRequest view{RenderOutputKind::SceneView,
				RenderViewType::Scene, glm::uvec2(640u, 360u), 1u};
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(Renderer::SelectGraphicsSettingsForView(frame, view) == visible,
				"A visible view cannot inherit an offscreen snapshot that reuses its view type and numeric ID.");
			view.outputKind = RenderOutputKind::OffscreenTexture;
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(Renderer::SelectGraphicsSettingsForView(frame, view) == offscreen,
				"The offscreen view must retain its separately published settings identity.");
			auto stale = std::make_shared<GraphicsSettingsSnapshot>(*offscreen);
			++stale->lifecycle.rendererSessionGeneration;
			frame.cameraGraphicsSettings.front() = stale;
			context.Expect(Renderer::SelectGraphicsSettingsForView(frame, view) == game,
				"A camera snapshot from an obsolete renderer lifetime must fall back to the current game publication.");
			view.outputId = 8u;
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(Renderer::SelectGraphicsSettingsForView(frame, view) == game,
				"An unrelated output must use the game settings instead of another camera's overrides.");
		});

		registry.Add("Renderer.PostProcess.Foundation.ViewSettingsRequireExactCameraProvenance", [](TestContext& context) {
			Renderer::RenderFrameSnapshot frame;
			auto game = std::make_shared<GraphicsSettingsSnapshot>();
			game->lifecycle = {1u, 2u, 3u, 4u}; frame.graphicsSettings = game;
			GraphicsCameraTarget first;
			first.cameraId[0] = 11u; first.sceneGeneration = 12u;
			first.objectLifetimeGeneration = 13u; first.componentGeneration = 14u; first.bindingGeneration = 15u;
			auto second = first; second.cameraId[0] = 21u;
			frame.graphicsCameraIdentities = {first, second};
			const GraphicsViewIdentity output{RenderViewType::Game, 2u, 31u, RenderOutputKind::GameView};
			auto firstSettings = std::make_shared<GraphicsSettingsSnapshot>();
			firstSettings->lifecycle = game->lifecycle; firstSettings->target.scope = GraphicsScope::Camera;
			firstSettings->target.camera = first; firstSettings->target.view = output;
			auto secondSettings = std::make_shared<GraphicsSettingsSnapshot>(*firstSettings);
			secondSettings->target.camera = second;
			frame.cameraGraphicsSettings = {firstSettings, secondSettings};
			frame.publishedGraphicsCameraViews = {{firstSettings->target, 640u, 360u}, {secondSettings->target, 640u, 360u}};
			Renderer::RenderFrameViewRequest view{RenderOutputKind::GameView, RenderViewType::Game, {640u, 360u}, 2u};
			Renderer::BindGraphicsCameraViewProvenance(view, frame, first);
			context.Expect(Renderer::SelectGraphicsSettingsForView(frame, view) == firstSettings,
				"The first camera must select its own complete lifetime even when another camera reused physical output two.");
			Renderer::BindGraphicsCameraViewProvenance(view, frame, second);
			context.Expect(Renderer::SelectGraphicsSettingsForView(frame, view) == secondSettings,
				"The second camera must select its own override independently of insertion order and output number.");
			for(std::uint32_t stale = 0u; stale != 3u; ++stale)
			{
				auto prior = second;
				if(stale == 0u) ++prior.componentGeneration;
				if(stale == 1u) ++prior.bindingGeneration;
				Renderer::BindGraphicsCameraViewProvenance(view, frame, prior);
				if(stale == 2u) ++view.graphicsView->lifetimeGeneration;
				context.Expect(Renderer::SelectGraphicsSettingsForView(frame, view) == game,
					"A stale component, script binding, or view lifetime must never inherit a replacement camera's settings.");
			}
			Renderer::BindGraphicsCameraViewProvenance(view, frame, std::nullopt);
			context.Expect(!view.graphicsCamera && !view.graphicsView && Renderer::SelectGraphicsSettingsForView(frame, view) == game,
				"Custom outputs without protected camera provenance must use game settings.");
		});

		registry.Add("Renderer.PostProcess.Foundation.ViewBindingRequiresAcceptedExtentAndUniquePriorLifetime", [](TestContext& context) {
			Renderer::RenderFrameSnapshot frame;
			GraphicsCameraTarget camera;
			camera.cameraId[0] = 1u; camera.sceneGeneration = 2u; camera.objectLifetimeGeneration = 3u;
			camera.componentGeneration = 4u; camera.bindingGeneration = 5u;
			frame.graphicsCameraIdentities = {camera};
			Renderer::RenderFrameViewRequest view{RenderOutputKind::GameView, RenderViewType::Game, {640u, 360u}, 2u};
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(view.graphicsCamera == camera && !view.graphicsView,
				"A camera's first submitted view may identify its owner but cannot invent an accepted view lifetime.");
			GraphicsTarget target; target.scope = GraphicsScope::Camera; target.camera = camera;
			target.view = GraphicsViewIdentity{RenderViewType::Game, 2u, 9u, RenderOutputKind::GameView};
			frame.publishedGraphicsCameraViews = {{target, 640u, 360u}};
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(view.graphicsView == target.view, "Only an accepted same-extent registration may supply the prior lifetime.");
			view.targetSize.x = 1280u;
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(view.graphicsCamera == camera && !view.graphicsView, "A resized target must not reuse the previous extent's view lifetime.");
			// Protected planning can issue a private lifetime for the resized view
			// before temporal raster submits its first accepted frame.
			auto resize = std::make_shared<GraphicsSettingsSnapshot>();
			resize->target = target;
			resize->target.view->lifetimeGeneration = 10u;
			resize->outputWidth = 1280u;
			resize->outputHeight = 360u;
			resize->internalWidth = 640u;
			resize->internalHeight = 180u;
			resize->values = {
				{GraphicsSettingId::UpscalingMode, std::uint32_t{1}}, {GraphicsSettingId::InternalResolutionScale, 0.5f}};
			frame.cameraGraphicsSettings = {resize};
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(view.graphicsView == resize->target.view && Renderer::SelectGraphicsSettingsForView(frame, view) == resize
				&& frame.publishedGraphicsCameraViews[0].target.view == target.view,
				"An exact private resize plan can record temporal history without replacing the accepted registration.");
			resize->internalWidth = 1280u;
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(!view.graphicsView, "An incompatible resize plan cannot bind a temporal view.");
			view.targetSize.x = 640u;
			frame.publishedGraphicsCameraViews.push_back(frame.publishedGraphicsCameraViews.front());
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(!view.graphicsView, "Duplicate prior registrations cannot prove one authoritative view binding.");
		});

		registry.Add("Renderer.PostProcess.Foundation.HalfExtentCaptureRequiresMatchingTypedConfiguration", [](TestContext& context) {
			Renderer::RenderFrameSnapshot frame;
			GraphicsCameraTarget camera;
			camera.cameraId[0] = 1u; camera.sceneGeneration = 2u; camera.objectLifetimeGeneration = 3u;
			camera.componentGeneration = 4u; camera.bindingGeneration = 5u;
			frame.graphicsCameraIdentities = {camera};
			frame.graphicsSettingsLifecycle = {2u, 3u, 4u, 5u};
			auto planned = std::make_shared<GraphicsSettingsSnapshot>();
			planned->target = {GraphicsScope::Camera, camera, GraphicsViewIdentity{RenderViewType::Game, 7u, 8u, RenderOutputKind::OffscreenTexture}};
			planned->lifecycle = frame.graphicsSettingsLifecycle;
			planned->values = { {GraphicsSettingId::UpscalingMode, std::uint32_t{1}},
				{GraphicsSettingId::InternalResolutionScale, 0.5f}};
			planned->outputWidth = 640u; planned->outputHeight = 360u;
			planned->internalWidth = 320u; planned->internalHeight = 180u;
			frame.cameraGraphicsSettings = {planned};
			Renderer::RenderFrameViewRequest view{RenderOutputKind::OffscreenTexture, RenderViewType::Game, {640u, 360u}, 7u};
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(view.graphicsView == planned->target.view, "A verified half-resolution plan may bind to its full-sized dedicated output.");
			// Stale output-sized inputs must not masquerade as half-resolution attachments.
			planned->internalWidth = 640u;
			Renderer::BindGraphicsCameraViewProvenance(view, frame, camera);
			context.Expect(!view.graphicsView, "The exact internal extent must follow the typed reconstruction configuration.");
		});

		registry.Add("Renderer.PostProcess.Foundation.CameraTargetsComeFromActualPresentationEvidence", [](TestContext& context) {
			Renderer::RenderFramePublication publication;
			Renderer::RenderViewPresentationInput presentation;
			GraphicsCameraTarget camera;
			camera.cameraId[0] = 1u; camera.sceneGeneration = 2u; camera.objectLifetimeGeneration = 3u;
			camera.componentGeneration = 4u; camera.bindingGeneration = 5u;
			presentation.graphicsCamera = camera;
			presentation.output.kind = RenderOutputKind::GameView;
			presentation.output.diagnosticsView = RenderViewType::Game;
			presentation.output.outputId = 2u; presentation.output.targetSize = {640u, 360u};
			publication.presentationInputs = {presentation};
			auto targets = Renderer::CollectPublishedGraphicsCameraTargets(publication);
			context.Expect(targets.size() == 1u && targets[0].target.camera == camera && targets[0].target.view
				&& targets[0].target.view->lifetimeGeneration == 0u && targets[0].width == 640u && targets[0].height == 360u,
				"The first accepted physical presentation must register its actual camera and extent without inventing a prior view lifetime.");
			presentation.graphicsView = GraphicsViewIdentity{RenderViewType::Game, 2u, 9u, RenderOutputKind::GameView};
			publication.presentationInputs = {presentation, presentation};
			auto custom = presentation; custom.graphicsCamera.reset(); publication.presentationInputs.push_back(custom);
			auto empty = presentation; empty.output.targetSize.x = 0u; publication.presentationInputs.push_back(empty);
			targets = Renderer::CollectPublishedGraphicsCameraTargets(publication);
			context.Expect(targets.size() == 2u && targets[0].target.view == presentation.graphicsView
				&& targets[1].target.view == presentation.graphicsView && targets[0].publicationIndex == 0u && targets[1].publicationIndex == 1u,
				"Each eligible presentation must retain its actual source ordinal for native admission, while anonymous or zero-extent outputs cannot register camera ownership.");
		});

		registry.Add("Renderer.PostProcess.Foundation.TargetFormatRequiresEveryRequestedFeature", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			const auto complete = CompleteColorFormatSupport();
			context.Expect(QualifiesPostProcessFormat(complete, {640u, 360u}, kFixtureColorUsage),
				"A successful exact image query with the full feature set must admit its maximum valid extent.");
			constexpr std::array features{VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
				VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
				VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
				VK_FORMAT_FEATURE_TRANSFER_DST_BIT};
			for(const auto feature : features)
			{
				auto missing = complete;
				missing.optimalFeatures &= ~feature;
				context.Expect(!QualifiesPostProcessFormat(missing, {640u, 360u}, kFixtureColorUsage),
					"Omitting any sampled, filtered, storage, attachment or transfer feature must reject the image candidate.");
			}
		});

		registry.Add("Renderer.PostProcess.Foundation.TargetFormatUsesTheCompleteImageQuery", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			for(const auto result : {VK_ERROR_FORMAT_NOT_SUPPORTED, VK_ERROR_OUT_OF_DEVICE_MEMORY})
			{
				auto rejected = CompleteColorFormatSupport();
				rejected.imageQueryResult = result;
				context.Expect(!QualifiesPostProcessFormat(rejected, {640u, 360u}, kFixtureColorUsage),
					"Feature flags cannot override a failed query for the full image usage combination.");
			}
			auto sampledOnly = CompleteColorFormatSupport();
			sampledOnly.optimalFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
			context.Expect(QualifiesPostProcessFormat(sampledOnly, {640u, 360u}, VK_IMAGE_USAGE_SAMPLED_BIT),
				"Admission must require only the features declared by the exact supplied image usage.");
			context.Expect(!QualifiesPostProcessFormat(sampledOnly, {640u, 360u}, kFixtureColorUsage),
				"A sampled-only format cannot qualify for the storage and copy processing path.");
		});

		registry.Add("Renderer.PostProcess.Foundation.TargetFormatRejectsInvalidExtents", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			const auto support = CompleteColorFormatSupport();
			constexpr std::array<VkExtent2D, 4> invalidExtents{{{0u, 360u}, {640u, 0u}, {641u, 360u}, {640u, 361u}}};
			for(const auto extent : invalidExtents)
			{
				context.Expect(!QualifiesPostProcessFormat(support, extent, kFixtureColorUsage),
					"Zero and device-limit-exceeding extents must be rejected before candidate allocation.");
			}
			context.Expect(QualifiesPostProcessFormat(support, {1u, 1u}, kFixtureColorUsage),
				"A valid one-pixel extent must remain supported by the target contract.");
		});

		registry.Add("Renderer.PostProcess.Foundation.TargetFormatRequiresSingleSampleImageShape", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			const auto complete = CompleteColorFormatSupport();
			std::array<PostProcessFormatSupport, 4> invalid;
			invalid.fill(complete);
			invalid[0].imageProperties.maxExtent.depth = 0u;
			invalid[1].imageProperties.maxMipLevels = 0u;
			invalid[2].imageProperties.maxArrayLayers = 0u;
			invalid[3].imageProperties.sampleCounts = VK_SAMPLE_COUNT_4_BIT;
			for(const auto& support : invalid)
			{
				context.Expect(!QualifiesPostProcessFormat(support, {640u, 360u}, kFixtureColorUsage),
					"The processing image requires at least one depth slice, mip, array layer and single-sample support.");
			}
		});

		registry.Add("Renderer.PostProcess.Foundation.TargetRetirementWaitsForEveryBorrower", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			// Null native handles make this a pure test of the production retirement
			// decision. Submission epochs model copied completion observations only.
			PostProcessTargetBundle target;
			target.lastUseByFrame = {3u, 5u, 7u};
			std::array<PostProcessFrameCompletion, 3> completion{};
			completion[0].completed = 3u;
			completion[1].completed = 5u;
			completion[2].completed = 6u;
			context.Expect(!CanRetirePostProcessTarget(target, completion, true),
				"A retired target must remain owned while any borrowing frame slot has an incomplete submission.");
			completion[2].completed = 7u;
			target.editorDescriptorAcknowledged = false;
			context.Expect(!CanRetirePostProcessTarget(target, completion, true),
				"Completed GPU work cannot retire image views that a retained editor descriptor still borrows.");
			target.editorDescriptorAcknowledged = true;
			context.Expect(!CanRetirePostProcessTarget(target, completion, false),
				"A pending readback must retain the old target after GPU and UI borrowers release it.");
			context.Expect(CanRetirePostProcessTarget(target, completion, true),
				"Only completed frame, editor descriptor, and readback ownership permits target retirement.");
		});

		registry.Add("Renderer.PostProcess.Foundation.TargetRetirementUsesCompletedSubmissionEpoch", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			PostProcessTargetBundle target;
			target.lastUseByFrame[1] = 5u;
			std::array<PostProcessFrameCompletion, 3> completion{};
			completion[1].submitted = 20u;
			completion[1].completed = 4u;
			context.Expect(!CanRetirePostProcessTarget(target, completion, true),
				"A newer submitted epoch provides no completion proof for the target's earlier borrow.");
			completion[1].completed = 5u;
			context.Expect(CanRetirePostProcessTarget(target, completion, true),
				"Completing the actual borrowing epoch releases the target without waiting for unrelated later submissions.");
		});

		registry.Add("Renderer.PostProcess.Foundation.ReplacementTargetGenerationsRetireIndependently", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			PostProcessTargetBundle previous;
			PostProcessTargetBundle replacement;
			previous.key = replacement.key = {2u, 9u, 3u};
			previous.generation = 1u;
			replacement.generation = 2u;
			previous.lastUseByFrame[0] = 2u;
			replacement.lastUseByFrame[0] = 4u;
			std::array<PostProcessFrameCompletion, 3> completion{};
			completion[0].completed = 2u;
			context.Expect(CanRetirePostProcessTarget(previous, completion, true)
				&& !CanRetirePostProcessTarget(replacement, completion, true),
				"Reusing the view key must not merge the completion epochs of old and replacement target owners.");
			context.Expect(previous.generation == 1u && previous.lastUseByFrame[0] == 2u,
				"Checking the replacement cannot mutate the older generation's retirement evidence.");
		});

		registry.Add("Renderer.PostProcess.Foundation.RecordedAndUnfencedUsesPreventRetirement", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			PostProcessTargetBundle target;
			std::array<PostProcessFrameCompletion, 3> completion{};
			context.Expect(CanRetirePostProcessTarget(target, completion, true),
				"The unborrowed target must be eligible before adding recorded use bookkeeping.");
			// The copied use carries no native handle and grants no GPU submission
			// evidence. Its presence alone must conservatively retain the owner.
			target.pendingUses.push_back({VK_NULL_HANDLE, 0u, false});
			context.Expect(!CanRetirePostProcessTarget(target, completion, true),
				"A recorded command must retain its target even when every older frame epoch is complete.");
			target.pendingUses.front().submittedUnfenced = true;
			context.Expect(!CanRetirePostProcessTarget(target, completion, true),
				"A submitted command without covering completion proof must retain its target as well.");
			target.pendingUses.clear();
			context.Expect(CanRetirePostProcessTarget(target, completion, true),
				"The empty pending-use guard must release after the owner has recorded actual completion or discard.");
		});

		registry.Add("Renderer.PostProcess.Foundation.InvalidRecordedUseCannotEnterBorrowerList", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			PostProcessTargetBundle target;
			context.Expect(!RecordPostProcessTargetUse(target, VK_NULL_HANDLE, 0u)
				&& target.pendingUses.empty(),
				"A missing command cannot admit a target borrower or claim successful command ownership.");
		});

		registry.Add("Renderer.PostProcess.Foundation.DeviceIdleKeepsNeverSubmittedRecordings", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			PostProcessTargetBundle target;
			// Distinct frame slots identify copied bookkeeping without manufacturing
			// command handles or treating this policy check as native idle evidence.
			target.pendingUses = {{VK_NULL_HANDLE, 0u, false},
				{VK_NULL_HANDLE, 1u, true}, {VK_NULL_HANDLE, 2u, false}};
			CompletePostProcessTargetSubmittedUses(target);
			context.Expect(target.pendingUses.size() == 2u
				&& target.pendingUses[0].frameSlot == 0u && !target.pendingUses[0].submittedUnfenced
				&& target.pendingUses[1].frameSlot == 2u && !target.pendingUses[1].submittedUnfenced,
				"Native idle cleanup must erase accepted unfenced uses while preserving commands that may still be submitted.");
			std::array<PostProcessFrameCompletion, 3> completion{};
			context.Expect(!CanRetirePostProcessTarget(target, completion, true),
				"Completed submitted work must not release image owners still referenced by an unsubmitted recording.");
			CompletePostProcessTargetSubmittedUses(target);
			context.Expect(target.pendingUses.size() == 2u,
				"Repeated idle observations must keep the unsubmitted recordings until an explicit discard.");
			target.pendingUses.front().submittedUnfenced = true;
			target.pendingUses.back().submittedUnfenced = true;
			CompletePostProcessTargetSubmittedUses(target);
			context.Expect(target.pendingUses.empty() && CanRetirePostProcessTarget(target, completion, true),
				"Once all recorded uses have been accepted, the production idle cleanup can release their pending guard.");
		});

		registry.Add("Renderer.PostProcess.Foundation.ExplicitLinearReadbackEncodesColorOnce", [](TestContext& context) {
			using namespace Renderer;
			const std::array<std::uint8_t, 4> rgba{128u, 64u, 255u, 77u};
			const std::array<std::uint8_t, 4> bgra{255u, 64u, 128u, 77u};
			const std::vector<std::uint8_t> expected{188u, 137u, 255u, 77u};
			const auto converted = VulkanReadback::ConvertToRgba8(rgba, 1u, 1u,
				VK_FORMAT_R8G8B8A8_UNORM, RenderCaptureColorEncoding::Linear);
			const auto swizzled = VulkanReadback::ConvertToRgba8(bgra, 1u, 1u,
				VK_FORMAT_B8G8R8A8_UNORM, RenderCaptureColorEncoding::Linear);
			context.Expect(converted == expected && swizzled == expected,
				"Linear UNORM RGB must use the sRGB display curve once with BGRA corrected and alpha left as coverage.");
			context.Expect(VulkanReadback::ConvertToRgba8(converted, 1u, 1u,
				VK_FORMAT_R8G8B8A8_SRGB, RenderCaptureColorEncoding::Srgb) == expected,
				"Already encoded processed pixels must retain their values when delivered to the PNG path.");
		});

		registry.Add("Renderer.PostProcess.Foundation.ExplicitSrgbReadbackPreservesStoredBytes", [](TestContext& context) {
			using namespace Renderer;
			const std::array<std::uint8_t, 4> rgba{128u, 64u, 255u, 77u};
			const std::array<std::uint8_t, 4> bgra{255u, 64u, 128u, 77u};
			const std::vector<std::uint8_t> expected(rgba.begin(), rgba.end());
			for(const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB})
			{
				context.Expect(VulkanReadback::ConvertToRgba8(rgba, 1u, 1u, format,
					RenderCaptureColorEncoding::Srgb) == expected,
					"Explicitly encoded RGBA source color must reach PNG without a second display conversion.");
			}
			for(const auto format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB})
			{
				context.Expect(VulkanReadback::ConvertToRgba8(bgra, 1u, 1u, format,
					RenderCaptureColorEncoding::Srgb) == expected,
					"Encoded BGRA source must change channel order while preserving color and alpha values.");
			}
		});

		registry.Add("Renderer.PostProcess.Foundation.FloatReadbackHonorsSourceEncoding", [](TestContext& context) {
			using namespace Renderer;
			const std::array<float, 4> channels{0.5f, 0.25f, 1.0f, 0.25f};
			std::array<std::uint8_t, sizeof(channels)> bytes{};
			std::memcpy(bytes.data(), channels.data(), bytes.size());
			context.ExpectEqual(16u, VulkanReadback::BytesPerPixel(VK_FORMAT_R32G32B32A32_SFLOAT),
				"Full-float source stride must include all four 32-bit channels.");
			context.Expect(VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R32G32B32A32_SFLOAT,
				RenderCaptureColorEncoding::Linear) == std::vector<std::uint8_t>{188u, 137u, 255u, 64u},
				"Linear floating point color must use the declared transfer while alpha keeps its linear value.");
			context.Expect(VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R32G32B32A32_SFLOAT,
				RenderCaptureColorEncoding::Srgb) == std::vector<std::uint8_t>{128u, 64u, 255u, 64u},
				"Encoded floating point color must quantize directly without applying another transfer.");
		});

		registry.Add("Renderer.PostProcess.Foundation.LegacyHalfReadbackRemainsByteExact", [](TestContext& context) {
			using namespace Renderer;
			// Little-endian IEEE binary16 values are RGB 0.5 and alpha 0.25.
			const std::array<std::uint8_t, 8> bytes{0u, 0x38u, 0u, 0x38u, 0u, 0x38u, 0u, 0x34u};
			const std::vector<std::uint8_t> legacy{186u, 186u, 186u, 64u};
			context.Expect(VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R16G16B16A16_SFLOAT) == legacy
				&& VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R16G16B16A16_SFLOAT,
					RenderCaptureColorEncoding::Legacy) == legacy,
				"The existing readback overload and explicit legacy encoding must preserve their exact gamma-2.2 bytes.");
			context.Expect(VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R16G16B16A16_SFLOAT,
				RenderCaptureColorEncoding::Linear) == std::vector<std::uint8_t>{188u, 188u, 188u, 64u}
				&& VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R16G16B16A16_SFLOAT,
					RenderCaptureColorEncoding::Srgb) == std::vector<std::uint8_t>{128u, 128u, 128u, 64u},
				"Explicit source metadata must govern half-float processing without changing legacy callers.");
		});

		registry.Add("Renderer.PostProcess.Foundation.ReadbackRejectsContradictoryEncodingAndByteCounts", [](TestContext& context) {
			using namespace Renderer;
			const std::array<std::uint8_t, 4> bytes{128u, 64u, 255u, 77u};
			const auto expectInvalid = [&context](const auto& operation, std::string_view message) {
				bool rejected = false;
				try { operation(); }
				catch(const std::invalid_argument&) { rejected = true; }
				context.Expect(rejected, message);
			};
			expectInvalid([&] {
				(void)VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R8G8B8A8_SRGB,
					RenderCaptureColorEncoding::Linear);
			}, "Stored sRGB bytes cannot be declared linear and encoded twice.");
			expectInvalid([&] {
				(void)VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R8G8B8A8_UNORM,
					static_cast<RenderCaptureColorEncoding>(255u));
			}, "An unknown encoding value must fail before conversion.");
			expectInvalid([&] {
				(void)VulkanReadback::ConvertToRgba8(bytes, 1u, 1u, VK_FORMAT_R32G32B32A32_SFLOAT,
					RenderCaptureColorEncoding::Linear);
			}, "A truncated float pixel cannot be read using a larger declared format.");
			expectInvalid([&] {
				(void)VulkanReadback::ConvertToRgba8({}, std::numeric_limits<std::uint32_t>::max(),
					std::numeric_limits<std::uint32_t>::max(), VK_FORMAT_R32G32B32A32_SFLOAT,
					RenderCaptureColorEncoding::Linear);
			}, "Image byte-count overflow must fail before attempting allocation or reading source memory.");
		});

		registry.Add("Renderer.PostProcess.Foundation.GraphSettingsRequireMatchingPreparedResources", [](TestContext& context) {
			GraphicsSettingsSnapshot snapshot;
			snapshot.lifecycle.backendResourceGeneration = 7u;
			snapshot.internalWidth = snapshot.outputWidth = 640u;
			snapshot.internalHeight = snapshot.outputHeight = 360u;
			snapshot.values = {{GraphicsSettingId::FixedExposureCompensation, 1.0f}};
			const auto unprepared = Renderer::BuildPostProcessGraphSettings(snapshot);
			context.Expect(unprepared.hdrCamera && !unprepared.executionAvailable
				&& Renderer::BuildPostProcessGraphTopologySignature(unprepared) == 0u,
				"A stored exposure request must remain inactive before native resources qualify its generation.");
			snapshot.postProcessResourceGeneration = 7u;
			const auto prepared = Renderer::BuildPostProcessGraphSettings(snapshot);
			context.Expect(prepared.executionAvailable && prepared.hdrCamera
				&& Renderer::BuildPostProcessGraphTopologySignature(prepared) != 0u,
				"Matching nonzero resource ownership must qualify the implemented fixed-exposure pipeline.");
			snapshot.postProcessResourceGeneration = 6u;
			context.Expect(!Renderer::BuildPostProcessGraphSettings(snapshot).executionAvailable,
				"A certificate from an older backend resource generation cannot enable the pipeline.");
			snapshot.postProcessResourceGeneration = 7u;
			// Installed spatial modes have their own positive tests. An unknown
			// persisted value cannot borrow another effect's native certificate.
			snapshot.values.push_back({GraphicsSettingId::AntiAliasingMode, std::uint32_t{0xFFFFFFFFu}});
			context.Expect(!Renderer::BuildPostProcessGraphSettings(snapshot).executionAvailable,
				"The fixed-exposure certificate cannot enable an AA route that has not been qualified.");
			snapshot.values.back() = {GraphicsSettingId::InternalResolutionScale, 0.5f};
			context.Expect(!Renderer::BuildPostProcessGraphSettings(snapshot).executionAvailable,
				"The fixed native-extent path must remain unavailable for an unimplemented scaling combination.");
		});

		registry.Add("Renderer.PostProcess.Foundation.CaptureStageRequiresAcceptedCurrentPublication", [](TestContext& context) {
			using namespace Renderer::Vulkan;
			// Opaque tokens exercise the copied metadata comparison only. This case
			// passes neither token to Vulkan and proves no native queue acceptance.
			const auto image = reinterpret_cast<VkImage>(std::uintptr_t{1});
			const auto otherImage = reinterpret_cast<VkImage>(std::uintptr_t{2});
			PostProcessOutputPublication output;
			output.sourceImage = image;
			output.sourceTargetGeneration = 3u;
			output.publicationFrameId = 7u;
			context.Expect(!IsPostProcessOutputCurrent(output, image, 3u, 7u),
				"Recording stage metadata cannot make an unaccepted submission available for capture.");
			output.submissionAccepted = true;
			context.Expect(IsPostProcessOutputCurrent(output, image, 3u, 7u),
				"An accepted publication must match the exact source target and frame before capture selects it.");
			context.Expect(!IsPostProcessOutputCurrent(output, otherImage, 3u, 7u)
				&& !IsPostProcessOutputCurrent(output, image, 4u, 7u)
				&& !IsPostProcessOutputCurrent(output, image, 3u, 8u),
				"A replaced target or newer source frame must make the older processed stage unavailable.");
			context.Expect(!IsPostProcessOutputCurrent(output, VK_NULL_HANDLE, 3u, 7u)
				&& !IsPostProcessOutputCurrent(output, image, 3u, 0u),
				"Missing source identity or frame metadata cannot qualify a processed capture stage.");
		});

		registry.Add("Renderer.PostProcess.Foundation.AdmissionAllocationFailureLeavesNoAcceptedWork", [](TestContext& context) {
			for(const auto stage : {"enqueue-receipt", "enqueue-queue"})
			{
				SettingsFixture fixture;
				const auto original = fixture.controller.Snapshot();
				GraphicsSettingsControllerTestAccess::AllocationFailure failure{stage};
				GraphicsSettingsControllerTestAccess::Inject(fixture.controller, failure);
				bool rejected = false;
				try { fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u)); }
				catch(const std::bad_alloc&) { rejected = true; }
				GraphicsSettingsControllerTestAccess::Clear(fixture.controller);
				context.Expect(rejected && failure.observed == 1u && !fixture.controller.HasPendingWork()
					&& !fixture.controller.GetReceipt(1u) && fixture.controller.Snapshot() == original,
					"Failure at either admission allocation must leave no accepted receipt, queued owner or settings change.");
				fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 8u));
				context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
					"Failed admission must not block a later normal request.");
			}
		});

		registry.Add("Renderer.PostProcess.Foundation.PreparationAllocationFailureLeavesNoStrandedHead", [](TestContext& context) {
			for(const auto stage : {"prepare-canonical-copy", "prepare-result-copy"})
			{
				SettingsFixture fixture;
				const auto original = fixture.controller.Snapshot();
				const auto admitted = fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
				GraphicsSettingsControllerTestAccess::AllocationFailure failure{stage};
				GraphicsSettingsControllerTestAccess::Inject(fixture.controller, failure);
				const auto candidate = fixture.controller.Prepare(fixture.context);
				GraphicsSettingsControllerTestAccess::Clear(fixture.controller);
				const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
				context.Expect(failure.observed == 1u && !candidate && receipt
					&& receipt->state == GraphicsRequestState::Rejected && receipt->reason == GraphicsRequestReason::PreparationFailed
					&& !fixture.controller.HasPendingWork() && fixture.controller.Snapshot() == original,
					"A failed candidate or caller-result copy must preserve the old root and settle its prepared gate.");
				fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 8u));
				context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
					"Preparation failure must allow the next valid request to prepare and publish.");
			}
		});

		registry.Add("Renderer.PostProcess.Foundation.PublicationAllocationFailureKeepsPriorRoot", [](TestContext& context) {
			for(const auto stage : {"publish-canonical-copy", "publish-proposed-roots", "publish-result-copy"})
			{
				SettingsFixture fixture;
				const auto original = fixture.controller.Snapshot();
				fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
				auto candidate = fixture.controller.Prepare(fixture.context);
				context.Expect(candidate.has_value(), "The publication failure fixture must first prepare valid settings.");
				GraphicsSettingsControllerTestAccess::AllocationFailure failure{stage};
				GraphicsSettingsControllerTestAccess::Inject(fixture.controller, failure);
				const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context);
				GraphicsSettingsControllerTestAccess::Clear(fixture.controller);
				context.Expect(failure.observed == 1u && receipt.state == GraphicsRequestState::Rejected
					&& receipt.reason == GraphicsRequestReason::PreparationFailed
					&& fixture.controller.Snapshot() == original && !fixture.controller.HasPendingWork(),
					"Failure before any published-root or return-receipt copy completes must preserve the original root and settle the request.");
				fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 8u));
				context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
					"A failed publication cannot leave a prepared owner blocking later work.");
			}
		});

		registry.Add("Renderer.PostProcess.Foundation.InheritedCameraAllocationFailureIsAtomic", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.descriptors[0].cameraScope = true;
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 4u));
			context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
				"A game baseline must exist before the inherited-camera failure.");
			const auto firstTarget = FixtureCameraTarget(13u, 2u);
			const auto secondTarget = FixtureCameraTarget(14u, 3u);
			std::array targets{firstTarget, secondTarget};
			fixture.context.validTargets = targets;
			for(const auto& target : targets)
			{
				auto request = fixture.Request(GraphicsSettingId::ShadowResolution, 8u);
				request.target = target;
				request.baseRevision = 0u;
				fixture.controller.Enqueue(std::move(request));
				context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
					"Both cameras must be published before forcing the second inheritance copy to fail.");
			}
			const auto game = fixture.controller.Snapshot();
			const auto first = fixture.controller.Snapshot(firstTarget);
			const auto second = fixture.controller.Snapshot(secondTarget);
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::ShadowResolution, 10u));
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate.has_value(), "The candidate must prepare before testing inheritance allocation.");
			GraphicsSettingsControllerTestAccess::AllocationFailure failure{"publish-inherited-camera", 2u};
			GraphicsSettingsControllerTestAccess::Inject(fixture.controller, failure);
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context);
			GraphicsSettingsControllerTestAccess::Clear(fixture.controller);
			context.Expect(failure.observed == 2u && receipt.state == GraphicsRequestState::Rejected
				&& receipt.reason == GraphicsRequestReason::PreparationFailed
				&& fixture.controller.Snapshot() == game && fixture.controller.Snapshot(firstTarget) == first
				&& fixture.controller.Snapshot(secondTarget) == second,
				"Failure after preparing the first inherited camera must leave all live game and camera roots untouched.");
		});

		registry.Add("Renderer.PostProcess.Foundation.ViewProjectionAllocationFailureKeepsAllViews", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.descriptors[0].cameraScope = true;
			auto firstTarget = FixtureCameraTarget(15u, 2u);
			auto secondTarget = firstTarget;
			secondTarget.view->outputId = 3u;
			std::array targets{firstTarget, secondTarget};
			fixture.context.validTargets = targets;
			auto cameraScope = firstTarget;
			cameraScope.view.reset();
			auto request = fixture.Request(GraphicsSettingId::ShadowResolution, 8u);
			request.target = cameraScope;
			fixture.controller.Enqueue(std::move(request));
			context.Expect(PublishNext(context, fixture).state == GraphicsRequestState::Applied,
				"The camera must have two effective views before testing projected-root failure.");
			const auto first = fixture.controller.Snapshot(firstTarget);
			const auto second = fixture.controller.Snapshot(secondTarget);
			auto change = fixture.Request(GraphicsSettingId::ShadowResolution, 10u);
			change.target = cameraScope;
			change.baseRevision = first->revision;
			fixture.controller.Enqueue(std::move(change));
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate.has_value(), "A camera patch must prepare before testing its second projected view.");
			GraphicsSettingsControllerTestAccess::AllocationFailure failure{"camera-view-projection", 2u};
			GraphicsSettingsControllerTestAccess::Inject(fixture.controller, failure);
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context);
			GraphicsSettingsControllerTestAccess::Clear(fixture.controller);
			context.Expect(failure.observed == 2u && receipt.state == GraphicsRequestState::Rejected
				&& receipt.reason == GraphicsRequestReason::PreparationFailed
				&& fixture.controller.Snapshot(firstTarget) == first && fixture.controller.Snapshot(secondTarget) == second,
				"A failed second view projection must preserve every previously visible view of the camera.");
		});

		registry.Add("Renderer.PostProcess.Foundation.CandidateCopyCannotCertifyNativeResources", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.ConfigureFixedExposure(GraphicsActivation::Rebuild);
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::FixedExposureCompensation, 1.0f));
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate.has_value(), "The exposure candidate must prepare before caller-copy qualification is tested.");
			// Callers own this copy, so writing readiness into it cannot become
			// evidence that a protected backend resource owner completed preparation.
			candidate->snapshot.postProcessResourceGeneration = fixture.context.lifecycle.backendResourceGeneration;
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context);
			const auto published = fixture.controller.Snapshot();
			context.Expect(receipt.state == GraphicsRequestState::Applied
				&& published->postProcessResourceGeneration == 0u
				&& !Renderer::BuildPostProcessGraphSettings(*published).executionAvailable,
				"A caller-edited candidate certificate cannot activate the owned exposure publication.");
		});

		registry.Add("Renderer.PostProcess.Foundation.RebuildPublicationAcceptsMatchingBackendCertificate", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.ConfigureFixedExposure(GraphicsActivation::Rebuild);
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::FixedExposureCompensation, 1.0f));
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate && candidate->activation == GraphicsActivation::Rebuild,
				"The controller must classify exposure resource preparation before publication.");
			const auto generation = fixture.context.lifecycle.backendResourceGeneration;
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context, true, generation);
			const auto published = fixture.controller.Snapshot();
			context.Expect(receipt.state == GraphicsRequestState::Applied
				&& published->postProcessResourceGeneration == generation
				&& Renderer::BuildPostProcessGraphSettings(*published).executionAvailable,
				"Only the explicit matching backend completion may qualify the implemented exposure path.");
		});

		registry.Add("Renderer.PostProcess.Foundation.StaleBackendCertificateRejectsAtomically", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.ConfigureFixedExposure(GraphicsActivation::Rebuild);
			const auto original = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::FixedExposureCompensation, 1.0f));
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate.has_value(), "The stale certificate fixture must prepare valid requested values.");
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context, true,
				fixture.context.lifecycle.backendResourceGeneration + 1u);
			context.Expect(receipt.state == GraphicsRequestState::Rejected
				&& receipt.reason == GraphicsRequestReason::StaleLifetime
				&& fixture.controller.Snapshot() == original && !fixture.controller.HasPendingWork(),
				"Qualification for another backend generation must leave the previous complete root active.");
		});

		registry.Add("Renderer.PostProcess.Foundation.LivePublicationCannotClaimRebuildCertificate", [](TestContext& context) {
			SettingsFixture fixture;
			fixture.ConfigureFixedExposure(GraphicsActivation::Live);
			const auto original = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(GraphicsSettingId::FixedExposureCompensation, 1.0f));
			auto candidate = fixture.controller.Prepare(fixture.context);
			context.Expect(candidate && candidate->activation == GraphicsActivation::Live,
				"The fixture must be a live parameter publication rather than a prepared rebuild.");
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context, true,
				fixture.context.lifecycle.backendResourceGeneration);
			context.Expect(receipt.state == GraphicsRequestState::Rejected
				&& receipt.reason == GraphicsRequestReason::StaleLifetime
				&& fixture.controller.Snapshot() == original && !fixture.controller.HasPendingWork(),
				"An ordinary live request cannot attach new resource qualification or partially change effective settings.");
		});
	}
}
