// PostProcessAATests.cpp - spatial AA publication contracts, without a GPU.
// These cases exercise real controller admission and protected publication using
// copied capabilities. They do not certify shader images, timing, or a device.
#include "TestHarness.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsController.h"
#include "Renderer/RenderTypes.h"

#include <algorithm>
#include <array>
#include <optional>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::GraphicsSettings;

		// Stable wire values deliberately precede the production enum promotion.
		// Keeping their UInt32 representation explicit also checks variant typing.
		constexpr std::uint32_t Off = 0u, FXAA = 1u, SMAA = 2u, CMAA2 = 3u;

		struct AAFixture
		{
			std::array<GraphicsSettingDescriptor, 3> descriptors;
			std::array<GraphicsTarget, 2> targets{};
			GraphicsPreparationContext preparation;
			GraphicsSettingsController controller;

			AAFixture()
			{
				auto& aa = descriptors[0];
				aa.id = GraphicsSettingId::AntiAliasingMode;
				aa.valueKind = GraphicsValueKind::Unsigned32;
				aa.defaultValue = Off;
				aa.options = {Off, FXAA, SMAA, CMAA2};
				aa.activation = GraphicsActivation::Rebuild;
				aa.cameraScope = true;
				auto& scale = descriptors[1];
				scale.id = GraphicsSettingId::InternalResolutionScale;
				scale.valueKind = GraphicsValueKind::Scalar;
				scale.defaultValue = 1.0f;
				scale.minimum = 0.5f;
				scale.maximum = 2.0f;
				scale.activation = GraphicsActivation::Rebuild;
				scale.cameraScope = true;
				auto& filter = descriptors[2];
				filter.id = GraphicsSettingId::ShadowFilteringSize;
				filter.valueKind = GraphicsValueKind::Unsigned32;
				filter.defaultValue = std::uint32_t{3};
				filter.minimum = std::uint32_t{1};
				filter.maximum = std::uint32_t{16};
				for(auto& descriptor : descriptors)
				{
					descriptor.gameScope = true;
					descriptor.available = true;
				}
				GraphicsCameraTarget camera;
				camera.cameraId[0] = 31u;
				camera.sceneGeneration = 32u;
				camera.objectLifetimeGeneration = 33u;
				camera.componentGeneration = 34u;
				camera.bindingGeneration = 35u;
				targets[1].scope = GraphicsScope::Camera;
				targets[1].camera = camera;
				targets[1].view = GraphicsViewIdentity{RenderViewType::Game, 2u, 36u, RenderOutputKind::GameView};
				preparation.lifecycle = {11u, 12u, 13u, 14u};
				preparation.descriptors = descriptors;
				preparation.validTargets = targets;
				preparation.frameId = 51u;
				controller.Invalidate(preparation);
			}

			GraphicsSettingsRequest Request(GraphicsValue mode, GraphicsTarget target = {}) const
			{
				GraphicsSettingsRequest request;
				request.target = target;
				request.lifecycle = preparation.lifecycle;
				const auto current = controller.Snapshot(target);
				request.baseRevision = current ? current->revision : 0u;
				request.patches = {{GraphicsSettingId::AntiAliasingMode, std::move(mode)}};
				return request;
			}
		};

		std::optional<std::uint32_t> Mode(const GraphicsSettingsSnapshot& snapshot)
		{
			const auto found = std::find_if(snapshot.values.begin(), snapshot.values.end(), [](const auto& field) {
				return field.id == GraphicsSettingId::AntiAliasingMode;
			});
			if(found == snapshot.values.end()) return std::nullopt;
			if(const auto value = std::get_if<std::uint32_t>(&found->value)) return *value;
			return std::nullopt;
		}

		GraphicsRequestReceipt Commit(TestContext& test, AAFixture& fixture, GraphicsSettingsRequest request)
		{
			fixture.controller.Enqueue(std::move(request));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			test.Expect(candidate.has_value(), "An available typed AA request must prepare.");
			return fixture.controller.Publish(std::move(*candidate), fixture.preparation, true,
				fixture.preparation.lifecycle.backendResourceGeneration);
		}

		void ExpectRejected(TestContext& test, AAFixture& fixture, GraphicsSettingsRequest request,
			GraphicsRequestReason reason)
		{
			const auto before = fixture.controller.Snapshot();
			const auto receipt = fixture.controller.Enqueue(std::move(request));
			test.Expect(!fixture.controller.Prepare(fixture.preparation), "Invalid AA batches must not become backend candidates.");
			const auto terminal = fixture.controller.GetReceipt(receipt.requestId);
			test.Expect(terminal && terminal->state == GraphicsRequestState::Rejected && terminal->reason == reason,
				"The retained terminal receipt must distinguish invalid, unavailable, and stale inputs.");
			test.Expect(fixture.controller.Snapshot() == before, "Rejected batches must preserve the complete immutable root.");
		}
	}

	void RegisterPostProcessAATests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.AA.Contract.SameSessionOrderedModeTransitions", [](TestContext& test) {
			AAFixture fixture;
			GraphicsRequestId previousId = 0;
			std::uint64_t previousSequence = 0;
			for(const auto mode : {Off, CMAA2, Off, FXAA, SMAA})
			{
				const auto before = fixture.controller.Snapshot();
				const auto admission = fixture.controller.Enqueue(fixture.Request(mode));
				test.Expect(admission.requestId > previousId && admission.sequence > previousSequence,
					"Every same-session switch must retain a distinct ordered receipt.");
				auto candidate = fixture.controller.Prepare(fixture.preparation);
				test.Expect(candidate && candidate->activation == GraphicsActivation::Rebuild,
					"Every AA switch, including Off, uses the resource rebuild boundary.");
				test.Expect(fixture.controller.Snapshot() == before, "Preparation cannot expose a requested AA mode.");
				const auto pending = fixture.controller.GetReceipt(admission.requestId);
				test.Expect(pending && pending->state == GraphicsRequestState::PendingRebuild, "AA stays pending until native preparation.");
				const auto applied = fixture.controller.Publish(std::move(*candidate), fixture.preparation, true,
					fixture.preparation.lifecycle.backendResourceGeneration);
				const auto snapshot = fixture.controller.Snapshot();
				test.Expect(applied.state == GraphicsRequestState::Applied && Mode(*snapshot) == mode
					&& snapshot->revision > before->revision && snapshot->sampleCount == 1u,
					"The protected commit publishes exactly the requested mode without MSAA.");
				test.Expect(snapshot->lifecycle == fixture.preparation.lifecycle
					&& snapshot->postProcessResourceGeneration == fixture.preparation.lifecycle.backendResourceGeneration,
					"Same-session AA must carry the actual native preparation generation.");
				previousId = admission.requestId;
				previousSequence = admission.sequence;
				++fixture.preparation.frameId;
			}
		});

		registry.Add("Renderer.PostProcess.AA.Contract.GameAndCameraRemainIndependent", [](TestContext& test) {
			AAFixture fixture;
			test.Expect(Commit(test, fixture, fixture.Request(FXAA)).state == GraphicsRequestState::Applied, "Game AA must apply.");
			test.Expect(Commit(test, fixture, fixture.Request(CMAA2, fixture.targets[1])).state == GraphicsRequestState::Applied,
				"A real camera scope may override Game AA.");
			const auto pinned = fixture.controller.Snapshot(fixture.targets[1]);
			test.Expect(pinned && Mode(*pinned) == CMAA2 && Mode(*fixture.controller.Snapshot()) == FXAA,
				"Camera publication cannot replace the Game owner.");
			Commit(test, fixture, fixture.Request(SMAA));
			test.Expect(Mode(*fixture.controller.Snapshot(fixture.targets[1])) == CMAA2 && Mode(*pinned) == CMAA2,
				"Later Game changes must preserve authored camera AA and old immutable snapshots.");
		});

		registry.Add("Renderer.PostProcess.AA.Contract.KnownUnavailableIsUnsupported", [](TestContext& test) {
			AAFixture fixture;
			fixture.descriptors[0].options = {Off, FXAA};
			ExpectRejected(test, fixture, fixture.Request(CMAA2), GraphicsRequestReason::Unsupported);
		});

		registry.Add("Renderer.PostProcess.AA.Contract.UnknownAndWrongTypedModesAreInvalid", [](TestContext& test) {
			for(const GraphicsValue mode : {GraphicsValue{std::uint32_t{99}}, GraphicsValue{true}, GraphicsValue{1.0f}})
			{
				AAFixture fixture;
				ExpectRejected(test, fixture, fixture.Request(mode), GraphicsRequestReason::InvalidValue);
			}
		});

		registry.Add("Renderer.PostProcess.AA.Contract.MixedInvalidBatchRollsBack", [](TestContext& test) {
			AAFixture fixture;
			auto request = fixture.Request(CMAA2);
			request.patches.push_back({GraphicsSettingId::ShadowFilteringSize, std::uint32_t{99}});
			ExpectRejected(test, fixture, std::move(request), GraphicsRequestReason::InvalidValue);
		});

		registry.Add("Renderer.PostProcess.AA.Contract.NonNativeScaleBatchIsUnsupported", [](TestContext& test) {
			AAFixture fixture;
			auto request = fixture.Request(SMAA);
			request.patches.push_back({GraphicsSettingId::InternalResolutionScale, 0.75f});
			ExpectRejected(test, fixture, std::move(request), GraphicsRequestReason::Unsupported);
		});

		registry.Add("Renderer.PostProcess.AA.Contract.StaleLifetimeAndInvalidScopeReject", [](TestContext& test) {
			AAFixture fixture;
			auto stale = fixture.Request(FXAA);
			++stale.lifecycle.rendererSessionGeneration;
			ExpectRejected(test, fixture, std::move(stale), GraphicsRequestReason::StaleLifetime);
			auto invalid = fixture.Request(SMAA);
			invalid.target.view = fixture.targets[1].view;
			ExpectRejected(test, fixture, std::move(invalid), GraphicsRequestReason::InvalidScope);
		});

		registry.Add("Renderer.PostProcess.AA.Contract.UntrustedCandidateCannotCertifyResources", [](TestContext& test) {
			AAFixture fixture;
			const auto before = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(CMAA2));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			test.Expect(candidate.has_value(), "Available CMAA2 must prepare before proof validation.");
			candidate->snapshot.postProcessResourceGeneration = fixture.preparation.lifecycle.backendResourceGeneration;
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.preparation);
			test.Expect(receipt.state == GraphicsRequestState::Rejected && receipt.reason == GraphicsRequestReason::PreparationFailed
				&& fixture.controller.Snapshot() == before, "Caller-editable metadata must not substitute for actual native AA preparation.");
		});

		registry.Add("Renderer.PostProcess.AA.Contract.RetiredCameraViewCannotPublish", [](TestContext& test) {
			AAFixture fixture;
			const auto game = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(CMAA2, fixture.targets[1]));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			test.Expect(candidate.has_value(), "The original exact camera view must prepare.");
			// A new view can reuse a physical output, but not the old view lifetime.
			++fixture.targets[1].view->lifetimeGeneration;
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.preparation, true,
				fixture.preparation.lifecycle.backendResourceGeneration);
			test.Expect(receipt.state == GraphicsRequestState::Rejected && !fixture.controller.Snapshot(fixture.targets[1])
				&& fixture.controller.Snapshot() == game, "Retired-view work cannot materialize a new camera or alter Game settings.");
		});

		registry.Add("Renderer.PostProcess.AA.Contract.FailedNativePreparationKeepsWorkingMode", [](TestContext& test) {
			AAFixture fixture;
			Commit(test, fixture, fixture.Request(FXAA));
			const auto before = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(CMAA2));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			test.Expect(candidate.has_value(), "Backend preparation starts only after typed validation.");
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.preparation, false);
			test.Expect(receipt.state == GraphicsRequestState::Rejected && receipt.reason == GraphicsRequestReason::PreparationFailed
				&& fixture.controller.Snapshot() == before && Mode(*before) == FXAA,
				"Native allocation or pipeline failure must retain the previous working AA publication.");
		});

		registry.Add("Renderer.PostProcess.AA.Contract.StaleResourceTokenPreservesPriorMode", [](TestContext& test) {
			AAFixture fixture;
			Commit(test, fixture, fixture.Request(FXAA));
			const auto before = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(SMAA));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			test.Expect(candidate.has_value(), "The replacement must prepare against current immutable settings.");
			const auto rejected = fixture.controller.Publish(std::move(*candidate), fixture.preparation, true,
				fixture.preparation.lifecycle.backendResourceGeneration + 1u);
			test.Expect(rejected.state == GraphicsRequestState::Rejected && rejected.reason == GraphicsRequestReason::StaleLifetime
				&& fixture.controller.Snapshot() == before && Mode(*before) == FXAA,
				"A foreign native resource token cannot replace the last working AA mode.");
		});
	}
}
