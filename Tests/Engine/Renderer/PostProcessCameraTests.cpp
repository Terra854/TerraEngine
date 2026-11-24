// Camera-effect settings contracts. Copied test descriptors represent qualified
// capabilities; only installed native image fixtures certify GPU execution.
#include "TestHarness.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsCatalog.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsController.h"
#include "Renderer/RenderTypes.h"

#include <algorithm>
#include <array>
#include <limits>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::GraphicsSettings;
		// These IDs and ranges are the camera-effects contract. Production enum names
		// can arrive later without making a test-only public settings namespace.
		constexpr auto Dof = static_cast<GraphicsSettingId>(104), Distance = static_cast<GraphicsSettingId>(105),
			Range = static_cast<GraphicsSettingId>(106), Blur = static_cast<GraphicsSettingId>(107),
			Motion = static_cast<GraphicsSettingId>(108), MotionStrength = static_cast<GraphicsSettingId>(109),
			CameraMotion = static_cast<GraphicsSettingId>(110), ObjectMotion = static_cast<GraphicsSettingId>(111);
		constexpr std::array<GraphicsSettingId, 4> DofFields{Dof, Distance, Range, Blur};
		constexpr std::array<const char*, 8> Names{"postProcess.depthOfFieldEnabled", "postProcess.depthOfFieldFocusDistance",
			"postProcess.depthOfFieldFocusRange", "postProcess.depthOfFieldBlurStrength", "postProcess.motionBlurEnabled",
			"postProcess.motionBlurStrength", "postProcess.motionBlurCameraContribution", "postProcess.motionBlurObjectContribution"};

		std::vector<GraphicsSettingDescriptor> CameraDescriptors()
		{
			std::vector<GraphicsSettingDescriptor> result;
			for(std::uint32_t index = 0; index != 8u; ++index)
			{
				GraphicsSettingDescriptor descriptor;
				descriptor.id = static_cast<GraphicsSettingId>(104u + index);
				descriptor.name = Names[index];
				descriptor.gameScope = descriptor.cameraScope = descriptor.available = true;
				descriptor.activation = GraphicsActivation::Rebuild;
				const bool boolean = index == 0u || index == 4u;
				descriptor.valueKind = boolean ? GraphicsValueKind::Boolean : GraphicsValueKind::Scalar;
				if(boolean) descriptor.defaultValue = false;
				else
				{
					descriptor.defaultValue = index == 1u ? 10.0f : index == 2u ? 5.0f : index == 3u ? 8.0f : 1.0f;
					descriptor.minimum = index == 1u || index == 2u ? 0.01f : 0.0f;
					descriptor.maximum = index == 1u || index == 2u ? 10000.0f : index == 3u ? 64.0f : index == 5u ? 2.0f : 1.0f;
				}
				result.push_back(std::move(descriptor));
			}
			return result;
		}

		struct CameraFixture
		{
			std::vector<GraphicsSettingDescriptor> descriptors = CameraDescriptors();
			std::array<GraphicsTarget, 3> targets{};
			GraphicsPreparationContext preparation;
			GraphicsSettingsController controller;
			CameraFixture()
			{
				for(std::size_t index = 1; index < targets.size(); ++index)
				{
					GraphicsCameraTarget camera;
					camera.cameraId[0] = static_cast<std::uint8_t>(index);
					camera.sceneGeneration = 2u; camera.objectLifetimeGeneration = 3u;
					camera.componentGeneration = 4u; camera.bindingGeneration = 5u;
					targets[index].scope = GraphicsScope::Camera; targets[index].camera = camera;
					targets[index].view = GraphicsViewIdentity{RenderViewType::Game, 2u, 10u + index, RenderOutputKind::GameView};
				}
				preparation = {{11u, 12u, 13u, 14u}, descriptors, targets, 20u};
				controller.Invalidate(preparation);
				std::vector<GraphicsSettingValue> defaults;
				for(const auto& descriptor : descriptors) defaults.push_back({descriptor.id, descriptor.defaultValue});
				controller.RefreshNativeValues(preparation, defaults, QualityPreset::Custom);
				// Materialize both actual typed camera owners. GPU work is not claimed
				// by this settings-only setup or by its copied readiness certificate.
				for(std::size_t index = 1; index < targets.size(); ++index)
				{
					controller.Enqueue(Request({{Dof, false}}, targets[index]));
					auto candidate = controller.Prepare(preparation);
					if(!candidate || controller.Publish(std::move(*candidate), preparation, true, 14u).state != GraphicsRequestState::Applied)
						throw TestFailure("Camera contract setup could not materialize its typed owners.");
				}
			}
			GraphicsSettingsRequest Request(std::vector<GraphicsSettingValue> values, GraphicsTarget target = {}) const
			{
				GraphicsSettingsRequest request;
				request.lifecycle = preparation.lifecycle; request.target = target; request.patches = std::move(values);
				const auto snapshot = controller.Snapshot(target); request.baseRevision = snapshot ? snapshot->revision : 0u;
				return request;
			}
			GraphicsRequestReceipt Commit(TestContext& test, GraphicsSettingsRequest request)
			{
				controller.Enqueue(std::move(request));
				auto candidate = controller.Prepare(preparation); test.Expect(candidate.has_value(), "Valid camera parameters must prepare together.");
				return controller.Publish(std::move(*candidate), preparation, true, 14u);
			}
		};

		const GraphicsValue& Value(const GraphicsSettingsSnapshot& snapshot, GraphicsSettingId id)
		{
			const auto found = std::find_if(snapshot.values.begin(), snapshot.values.end(), [id](const auto& field) { return field.id == id; });
			if(found == snapshot.values.end()) throw TestFailure("Expected effective camera field is missing.");
			return found->value;
		}
	}

	void RegisterPostProcessCameraTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.Camera.Contract.CatalogMatchesAgreedCameraFields", [](TestContext& test) {
			const auto expected = CameraDescriptors();
			const auto catalog = BuildGraphicsSettingsCatalog(RenderSettings{}, true, {}, {}, {});
			for(const auto& descriptor : expected)
			{
				const auto* actual = FindGraphicsSetting(catalog, descriptor.name);
				test.Expect(actual && actual->id == descriptor.id && actual->valueKind == descriptor.valueKind
					&& actual->defaultValue == descriptor.defaultValue && actual->minimum == descriptor.minimum && actual->maximum == descriptor.maximum
					&& actual->gameScope && actual->cameraScope && actual->activation == GraphicsActivation::Rebuild,
					"Production discovery must expose exact typed/default/range contracts; availability remains actual native policy.");
			}
		});

		registry.Add("Renderer.PostProcess.Camera.Contract.FocusRangeBoundariesPublish", [](TestContext& test) {
			CameraFixture fixture;
			for(const auto focus : {0.01f, 10000.0f})
			{
				const auto receipt = fixture.Commit(test, fixture.Request({{Dof, true}, {Distance, focus}, {Range, focus}, {Blur, 64.0f}}, fixture.targets[1]));
				test.Expect(receipt.state == GraphicsRequestState::Applied && Value(*fixture.controller.Snapshot(fixture.targets[1]), Distance) == GraphicsValue{focus},
					"Inclusive focus distance/range boundaries must remain selectable.");
			}
		});

		registry.Add("Renderer.PostProcess.Camera.Contract.InvalidFocusBatchRetainsBothCameras", [](TestContext& test) {
			for(const auto invalid : {GraphicsSettingValue{Distance, 0.0f}, GraphicsSettingValue{Range, 10001.0f},
				GraphicsSettingValue{Blur, 65.0f}, GraphicsSettingValue{Distance, std::uint32_t{3}},
				GraphicsSettingValue{Range, std::numeric_limits<float>::quiet_NaN()}})
			{
				CameraFixture fixture;
				const auto a = fixture.controller.Snapshot(fixture.targets[1]), b = fixture.controller.Snapshot(fixture.targets[2]);
				const auto admission = fixture.controller.Enqueue(fixture.Request({{Dof, true}, invalid}, fixture.targets[1]));
				test.Expect(!fixture.controller.Prepare(fixture.preparation), "Invalid focus must reject before native work.");
				test.Expect(fixture.controller.GetReceipt(admission.requestId)->reason == GraphicsRequestReason::InvalidValue
					&& fixture.controller.Snapshot(fixture.targets[1]) == a && fixture.controller.Snapshot(fixture.targets[2]) == b,
					"A late invalid field cannot enable blur or change the independent camera.");
			}
		});

		registry.Add("Renderer.PostProcess.Camera.Contract.ResetRestoresInheritanceAndKeepsOtherCamera", [](TestContext& test) {
			CameraFixture fixture;
			fixture.Commit(test, fixture.Request({{Dof, true}, {Distance, 3.0f}, {Range, 1.0f}}, fixture.targets[1]));
			fixture.Commit(test, fixture.Request({{Dof, true}, {Distance, 12.0f}}, fixture.targets[2]));
			const auto pinnedA = fixture.controller.Snapshot(fixture.targets[1]), b = fixture.controller.Snapshot(fixture.targets[2]);
			auto reset = fixture.Request({}, fixture.targets[1]); reset.operation = GraphicsRequestOperation::Reset;
			reset.resetSettings.assign(DofFields.begin(), DofFields.end());
			test.Expect(fixture.Commit(test, std::move(reset)).state == GraphicsRequestState::Applied, "Camera reset must settle at the protected boundary.");
			fixture.Commit(test, fixture.Request({{Distance, 20.0f}}));
			test.Expect(Value(*fixture.controller.Snapshot(fixture.targets[1]), Distance) == GraphicsValue{20.0f}
				&& Value(*fixture.controller.Snapshot(fixture.targets[2]), Distance) == GraphicsValue{12.0f}
				&& Value(*pinnedA, Distance) == GraphicsValue{3.0f} && Value(*b, Distance) == GraphicsValue{12.0f},
				"Reset removes ownership, preserves other camera overrides and never mutates retained snapshots.");
		});

		registry.Add("Renderer.PostProcess.Camera.Contract.MotionAndFocusControlsAreIndependent", [](TestContext& test) {
			CameraFixture fixture;
			fixture.Commit(test, fixture.Request({{Dof, true}, {Distance, 3.0f}}, fixture.targets[1]));
			fixture.Commit(test, fixture.Request({{Motion, true}, {MotionStrength, 1.0f}, {CameraMotion, 1.0f}, {ObjectMotion, 0.0f}}, fixture.targets[1]));
			fixture.Commit(test, fixture.Request({{Dof, false}}, fixture.targets[1]));
			const auto a = fixture.controller.Snapshot(fixture.targets[1]), b = fixture.controller.Snapshot(fixture.targets[2]);
			test.Expect(Value(*a, Motion) == GraphicsValue{true} && Value(*a, CameraMotion) == GraphicsValue{1.0f}
				&& Value(*a, ObjectMotion) == GraphicsValue{0.0f} && Value(*a, Distance) == GraphicsValue{3.0f}
				&& Value(*b, Motion) == GraphicsValue{false}, "Disabling DoF cannot alter motion contributions or another camera.");
		});

		registry.Add("Renderer.PostProcess.Camera.Contract.InvalidMotionContributionRollsBack", [](TestContext& test) {
			CameraFixture fixture;
			const auto before = fixture.controller.Snapshot(fixture.targets[1]);
			const auto admission = fixture.controller.Enqueue(fixture.Request({{Motion, true}, {ObjectMotion, 1.01f}}, fixture.targets[1]));
			test.Expect(!fixture.controller.Prepare(fixture.preparation) && fixture.controller.GetReceipt(admission.requestId)->reason == GraphicsRequestReason::InvalidValue
				&& fixture.controller.Snapshot(fixture.targets[1]) == before, "Invalid motion contribution cannot partially enable the effect.");
		});

		registry.Add("Renderer.PostProcess.Camera.Contract.NativeFailurePreservesEffectiveFocus", [](TestContext& test) {
			CameraFixture fixture;
			const auto before = fixture.controller.Snapshot(fixture.targets[1]);
			fixture.controller.Enqueue(fixture.Request({{Dof, true}, {Distance, 3.0f}}, fixture.targets[1]));
			auto candidate = fixture.controller.Prepare(fixture.preparation); test.Expect(candidate.has_value(), "Typed focus can prepare without becoming visible.");
			const auto rejected = fixture.controller.Publish(std::move(*candidate), fixture.preparation, false);
			test.Expect(rejected.reason == GraphicsRequestReason::PreparationFailed && fixture.controller.Snapshot(fixture.targets[1]) == before,
				"Failed depth/input/pass preparation must preserve the prior effective camera settings.");
		});

		registry.Add("Renderer.PostProcess.Camera.Contract.UnavailableCameraEffectRejectsAtomically", [](TestContext& test) {
			CameraFixture fixture;
			fixture.descriptors[0].available = false; fixture.descriptors[0].unavailableReason = GraphicsRequestReason::Unsupported;
			const auto before = fixture.controller.Snapshot(fixture.targets[1]);
			const auto request = fixture.controller.Enqueue(fixture.Request({{Dof, true}, {Distance, 3.0f}}, fixture.targets[1]));
			test.Expect(!fixture.controller.Prepare(fixture.preparation) && fixture.controller.GetReceipt(request.requestId)->reason == GraphicsRequestReason::Unsupported
				&& fixture.controller.Snapshot(fixture.targets[1]) == before, "Unsupported native effects cannot be relabeled as a successful no-op.");
		});
	}
}
