// Coverage/temporal settings contracts. These headless tests use the real typed
// catalog/controller; installed native fixtures separately prove raster draws,
// actual samples/extents, motion, history and image quality on the device.
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
		GraphicsCatalogCapabilities QualifiedCapabilities()
		{
			GraphicsCatalogCapabilities capabilities;
			capabilities.spatialAAModeMask = 15u;
			capabilities.indexedRasterSupported = capabilities.msaa4Supported = capabilities.taaSupported = true;
			capabilities.temporalUpscalingSupported = capabilities.temporalSharpeningSupported = true;
			return capabilities;
		}

		std::vector<GraphicsSettingValue> Tuple(std::uint32_t aa, std::uint32_t upscale, float scale,
			std::uint32_t raster = 1u)
		{
			return {{GraphicsSettingId::AntiAliasingMode, aa}, {GraphicsSettingId::UpscalingMode, upscale},
				{GraphicsSettingId::InternalResolutionScale, scale}, {GraphicsSettingId::GeometryRasterMode, raster}};
		}

		bool Option(std::span<const GraphicsSettingDescriptor> catalog, GraphicsSettingId id, GraphicsValue value)
		{
			const auto* descriptor = FindGraphicsSetting(catalog, id);
			return descriptor && ValidateGraphicsAASettingValue(*descriptor, value) == GraphicsRequestReason::None;
		}

		struct TemporalFixture
		{
			std::vector<GraphicsSettingDescriptor> catalog;
			std::array<GraphicsTarget, 2> targets{};
			GraphicsPreparationContext context;
			GraphicsSettingsController controller;

			explicit TemporalFixture(GraphicsCatalogCapabilities capabilities = QualifiedCapabilities())
			{
				RenderSettings defaults{};
				defaults.variableRateShading = 0u;
				catalog = BuildGraphicsSettingsCatalog(defaults, true, {}, {}, capabilities);
				GraphicsCameraTarget camera;
				camera.cameraId[0] = 21u; camera.sceneGeneration = 22u;
				camera.objectLifetimeGeneration = 23u; camera.componentGeneration = 24u; camera.bindingGeneration = 25u;
				targets[1].scope = GraphicsScope::Camera; targets[1].camera = camera;
				targets[1].view = GraphicsViewIdentity{RenderViewType::Game, 2u, 26u, RenderOutputKind::GameView};
				context = {{1u, 2u, 3u, 4u}, catalog, targets, 10u};
				controller.Invalidate(context);
				controller.UpdateViewExtent({}, 640u, 360u);
			}

			GraphicsSettingsRequest Request(std::vector<GraphicsSettingValue> fields, GraphicsTarget target = {}) const
			{
				GraphicsSettingsRequest request;
				request.lifecycle = context.lifecycle; request.target = target; request.patches = std::move(fields);
				const auto current = controller.Snapshot(target);
				request.baseRevision = current ? current->revision : 0u;
				return request;
			}

			GraphicsRequestReceipt Commit(TestContext& test, std::vector<GraphicsSettingValue> fields, GraphicsTarget target = {})
			{
				controller.Enqueue(Request(std::move(fields), target));
				auto candidate = controller.Prepare(context);
				test.Expect(candidate.has_value(), "A qualified typed tuple must prepare.");
				const auto receipt = controller.Publish(std::move(*candidate), context, true, context.lifecycle.backendResourceGeneration);
				test.Expect(receipt.state == GraphicsRequestState::Applied, "A qualified native completion must publish atomically.");
				++context.frameId;
				return receipt;
			}
		};
	}

	void RegisterPostProcessTemporalTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.Temporal.Contract.DefaultCapabilitiesDoNotAdvertiseImplementations", [](TestContext& test) {
			TemporalFixture fixture(GraphicsCatalogCapabilities{});
			test.Expect(Option(fixture.catalog, GraphicsSettingId::AntiAliasingMode, std::uint32_t{0})
				&& Option(fixture.catalog, GraphicsSettingId::GeometryRasterMode, std::uint32_t{0})
				&& Option(fixture.catalog, GraphicsSettingId::UpscalingMode, std::uint32_t{0})
				&& Option(fixture.catalog, GraphicsSettingId::InternalResolutionScale, 1.0f), "Legacy inactive defaults must remain usable.");
			test.Expect(!Option(fixture.catalog, GraphicsSettingId::AntiAliasingMode, std::uint32_t{4})
				&& !Option(fixture.catalog, GraphicsSettingId::AntiAliasingMode, std::uint32_t{5})
				&& !Option(fixture.catalog, GraphicsSettingId::GeometryRasterMode, std::uint32_t{1})
				&& !Option(fixture.catalog, GraphicsSettingId::UpscalingMode, std::uint32_t{1})
				&& !Option(fixture.catalog, GraphicsSettingId::InternalResolutionScale, 0.5f)
				&& !FindGraphicsSetting(fixture.catalog, GraphicsSettingId::UpscalingSharpness)->available,
				"Device-independent defaults cannot advertise unimplemented coverage, temporal or indexed paths.");
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.IndependentCapabilitiesRequireIndexedQualification", [](TestContext& test) {
			auto capabilities = QualifiedCapabilities(); capabilities.indexedRasterSupported = false;
			TemporalFixture missingRaster(capabilities);
			test.Expect(!Option(missingRaster.catalog, GraphicsSettingId::AntiAliasingMode, std::uint32_t{4})
				&& !Option(missingRaster.catalog, GraphicsSettingId::AntiAliasingMode, std::uint32_t{5})
				&& !Option(missingRaster.catalog, GraphicsSettingId::UpscalingMode, std::uint32_t{1}), "Shader flags alone cannot qualify an absent indexed renderer.");
			capabilities.indexedRasterSupported = true; capabilities.taaSupported = false;
			TemporalFixture supported(capabilities);
			test.Expect(Option(supported.catalog, GraphicsSettingId::AntiAliasingMode, std::uint32_t{4})
				&& !Option(supported.catalog, GraphicsSettingId::AntiAliasingMode, std::uint32_t{5})
				&& Option(supported.catalog, GraphicsSettingId::UpscalingMode, std::uint32_t{1}), "Coverage and upscaling must not depend on an unrelated TAA flag.");
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.RequiredTuplesAndSpatialDefaultsResolve", [](TestContext& test) {
			for(const auto& fields : {Tuple(4u, 0u, 1.0f), Tuple(5u, 0u, 1.0f), Tuple(0u, 1u, 0.5f)})
			{
				GraphicsAAConfiguration configuration;
				test.Expect(ResolveGraphicsAAConfiguration(fields, configuration) == GraphicsRequestReason::None, "Each required tuple must resolve without changing its modes.");
				test.Expect(configuration.sampleCount == (configuration.antiAliasing == GraphicsAntiAliasingMode::MSAA ? 4u : 1u), "Samples must derive from standalone coverage mode.");
				test.Expect(ResolveGraphicsAAInternalDimension(640u, configuration) == (configuration.upscaling == GraphicsUpscalingMode::Temporal ? 320u : 640u), "Half reconstruction must derive a distinct internal extent.");
			}
			for(std::uint32_t mode = 0; mode <= 3u; ++mode)
			{
				GraphicsAAConfiguration configuration;
				test.Expect(ResolveGraphicsAAConfiguration(Tuple(mode, 0u, 1.0f, 0u), configuration) == GraphicsRequestReason::None
					&& configuration.sampleCount == 1u, "Existing automatic spatial/default behavior must remain unchanged.");
			}
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.IncompatibleCombinationsAreUnsupported", [](TestContext& test) {
			for(const auto& fields : {Tuple(4u, 1u, 0.5f), Tuple(5u, 1u, 0.5f), Tuple(1u, 1u, 0.5f),
				Tuple(4u, 0u, 1.0f, 0u), Tuple(0u, 0u, 0.5f), Tuple(0u, 1u, 1.0f)})
			{
				GraphicsAAConfiguration configuration;
				test.Expect(ResolveGraphicsAAConfiguration(fields, configuration) == GraphicsRequestReason::Unsupported, "Known incompatible tuples must report Unsupported.");
			}
			auto fields = Tuple(0u, 0u, 1.0f); fields.push_back({GraphicsSettingId::VariableRateShading, true});
			GraphicsAAConfiguration configuration;
			test.Expect(ResolveGraphicsAAConfiguration(fields, configuration) == GraphicsRequestReason::Unsupported, "Initial indexed reference path must reject VRS rather than silently disable it.");
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.UnknownAndNonfiniteValuesAreInvalid", [](TestContext& test) {
			TemporalFixture fixture;
			for(const auto field : {GraphicsSettingValue{GraphicsSettingId::AntiAliasingMode, std::uint32_t{6}},
				GraphicsSettingValue{GraphicsSettingId::UpscalingMode, std::uint32_t{2}},
				GraphicsSettingValue{GraphicsSettingId::GeometryRasterMode, true},
				GraphicsSettingValue{GraphicsSettingId::UpscalingSharpness, std::numeric_limits<float>::quiet_NaN()}})
			{
				const auto* descriptor = FindGraphicsSetting(fixture.catalog, field.id);
				test.Expect(ValidateGraphicsAASettingValue(*descriptor, field.value) == GraphicsRequestReason::InvalidValue, "Unknown enum, wrong variant or nonfinite scalar must be malformed.");
			}
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.ControllerPublishesRequiredSamplesAndExtents", [](TestContext& test) {
			TemporalFixture fixture;
			for(const auto& fields : {Tuple(4u, 0u, 1.0f), Tuple(5u, 0u, 1.0f), Tuple(0u, 1u, 0.5f)})
			{
				fixture.Commit(test, fields);
				const auto snapshot = fixture.controller.Snapshot();
				GraphicsAAConfiguration configuration;
				test.Expect(ResolveGraphicsAAConfiguration(snapshot->values, configuration) == GraphicsRequestReason::None
					&& snapshot->sampleCount == configuration.sampleCount && snapshot->internalWidth == ResolveGraphicsAAInternalDimension(640u, configuration)
					&& snapshot->internalHeight == ResolveGraphicsAAInternalDimension(360u, configuration)
					&& snapshot->outputWidth == 640u && snapshot->outputHeight == 360u,
					"Protected publication must carry canonical derived samples and real output/internal dimensions.");
			}
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.MixedRejectedBatchRetainsOriginalSnapshot", [](TestContext& test) {
			TemporalFixture fixture;
			fixture.Commit(test, Tuple(0u, 1u, 0.5f));
			const auto before = fixture.controller.Snapshot();
			auto fields = Tuple(4u, 1u, 0.5f); fields.push_back({GraphicsSettingId::ShadowFilteringSize, std::uint32_t{7}});
			const auto request = fixture.controller.Enqueue(fixture.Request(std::move(fields)));
			test.Expect(!fixture.controller.Prepare(fixture.context), "Combined MSAA/upscaling must never produce a native candidate.");
			const auto rejected = fixture.controller.GetReceipt(request.requestId);
			test.Expect(rejected && rejected->reason == GraphicsRequestReason::Unsupported && fixture.controller.Snapshot() == before,
				"A late tuple rejection must also roll back earlier valid native fields.");
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.NativeStartupSeedDerivesCanonicalMetadata", [](TestContext& test) {
			TemporalFixture fixture;
			fixture.controller.RefreshNativeValues(fixture.context, Tuple(4u, 0u, 1.0f), QualityPreset::Custom, 4u);
			test.Expect(fixture.controller.Snapshot()->sampleCount == 4u && fixture.controller.Snapshot()->internalWidth == 640u,
				"A verified startup seed must publish actual coverage samples.");
			fixture.controller.RefreshNativeValues(fixture.context, Tuple(0u, 1u, 0.5f), QualityPreset::Custom, 4u);
			test.Expect(fixture.controller.Snapshot()->sampleCount == 1u && fixture.controller.Snapshot()->internalWidth == 320u
				&& fixture.controller.Snapshot()->internalHeight == 180u, "A verified upscaling seed must publish distinct native internal extents.");
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.IndexedOffNeedsTrustedNativePreparation", [](TestContext& test) {
			TemporalFixture fixture;
			const auto before = fixture.controller.Snapshot();
			fixture.controller.Enqueue(fixture.Request(Tuple(0u, 0u, 1.0f)));
			auto candidate = fixture.controller.Prepare(fixture.context); test.Expect(candidate.has_value(), "Supported indexed Off must prepare.");
			candidate->snapshot.postProcessResourceGeneration = 4u;
			const auto rejected = fixture.controller.Publish(std::move(*candidate), fixture.context);
			test.Expect(rejected.reason == GraphicsRequestReason::PreparationFailed && fixture.controller.Snapshot() == before,
				"Same-raster Off reference cannot claim an indexed backend from editable candidate metadata.");
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.CameraOwnsIndependentSampleCount", [](TestContext& test) {
			TemporalFixture fixture;
			fixture.Commit(test, Tuple(0u, 0u, 1.0f));
			fixture.Commit(test, {{GraphicsSettingId::AntiAliasingMode, std::uint32_t{1}}}, fixture.targets[1]);
			fixture.controller.Enqueue(fixture.Request(Tuple(4u, 0u, 1.0f)));
			auto candidate = fixture.controller.Prepare(fixture.context); test.Expect(candidate.has_value(), "Game MSAA prepares independently.");
			const auto projected = fixture.controller.ProjectCandidateForView(*candidate, fixture.targets[1]);
			test.Expect(projected.sampleCount == 1u, "Camera FXAA preflight cannot borrow Game MSAA sample metadata.");
			const auto receipt = fixture.controller.Publish(std::move(*candidate), fixture.context, true, 4u);
			test.Expect(receipt.state == GraphicsRequestState::Applied && fixture.controller.Snapshot()->sampleCount == 4u
				&& fixture.controller.Snapshot(fixture.targets[1])->sampleCount == 1u, "Camera and Game sample counts must remain isolated after publication.");
		});

		registry.Add("Renderer.PostProcess.Temporal.Contract.CameraConflictRejectsWholeGamePublication", [](TestContext& test) {
			TemporalFixture fixture;
			fixture.Commit(test, Tuple(0u, 0u, 1.0f));
			fixture.Commit(test, {{GraphicsSettingId::AntiAliasingMode, std::uint32_t{1}}}, fixture.targets[1]);
			const auto gameBefore = fixture.controller.Snapshot(), cameraBefore = fixture.controller.Snapshot(fixture.targets[1]);
			fixture.controller.Enqueue(fixture.Request(Tuple(0u, 1u, 0.5f)));
			auto candidate = fixture.controller.Prepare(fixture.context); test.Expect(candidate.has_value(), "The root tuple is independently valid.");
			const auto rejected = fixture.controller.Publish(std::move(*candidate), fixture.context, true, 4u);
			test.Expect(rejected.state == GraphicsRequestState::Rejected && rejected.reason == GraphicsRequestReason::Unsupported
				&& fixture.controller.Snapshot() == gameBefore && fixture.controller.Snapshot(fixture.targets[1]) == cameraBefore,
				"Inherited temporal upscaling cannot partially replace Game or a conflicting camera AA override.");
		});
	}
}
