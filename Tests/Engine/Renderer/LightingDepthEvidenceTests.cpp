#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanLightingDepthEvidence.h"

namespace TerraEngine::Tests
{
	namespace
	{
		Renderer::RendererEfficiencyEvidence CapturedDepth()
		{
			Renderer::RendererEfficiencyEvidence row;
			row.kind = Renderer::RendererEfficiencyEvidenceKind::LightingDepth;
			row.frameSerial = 10; row.commandSerial = 11; row.submitSerial = 12; row.completionSerial = 13;
			row.viewType = RenderViewType::Scene; row.viewId = 1;
			row.effectiveOptions.rendererSessionGeneration = 2; row.effectiveOptions.settingsGeneration = 3;
			row.effectiveOptions.optionsSessionGeneration = 4; row.fixtureControlGeneration = 5;
			row.fixtureCorrelation.operationId = "owned-operation"; row.fixtureCorrelation.fixtureStepId = "owned-step";
			row.fixtureCorrelation.stepFrame = 60; row.fixtureCorrelation.mutationGeneration = 6;
			row.fixtureCorrelation.settingsGeneration = 3; row.fixtureCorrelation.rendererSessionGeneration = 2;
			return row;
		}
		Renderer::RendererEfficiencyEvidence CompletedColor(const Renderer::RendererEfficiencyEvidence& capture)
		{
			auto row = capture; row.kind = Renderer::RendererEfficiencyEvidenceKind::Lighting;
			row.status = Renderer::RendererEfficiencyEvidenceStatus::Valid;
			auto artifacts = std::make_shared<Renderer::RendererEfficiencyArtifacts>();
			auto color = std::make_shared<Renderer::RendererEfficiencyFrameColor>(); color->width = color->height = 1; color->bytes = {1,2,3,4};
			artifacts->frameColor = std::move(color); row.artifacts = std::move(artifacts); return row;
		}
	}
	void RegisterLightingDepthEvidenceTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.LightingDepthEvidence.DiscardedFirstFrameCanRetry", [](TestContext& t) {
			const auto first = CapturedDepth(); auto next = first; ++next.frameSerial; ++next.commandSerial;
			Renderer::LightingDepthSampleIntent token{first.viewType, first.viewId, first.fixtureCorrelation, first.fixtureControlGeneration, first.frameSerial};
			t.Expect(Renderer::LightingDepthSampleCanRecord(token, first) && !Renderer::LightingDepthSampleCanRecord(token, next), "A retained token freezes its first frame.");
			t.Expect(!Renderer::ReleaseLightingDepthSampleOnDiscard(token, first, true) && !Renderer::LightingDepthSampleCanRecord(token, next), "An accepted submission must retain its first-frame owner.");
			auto stale = first; ++stale.fixtureControlGeneration;
			t.Expect(!Renderer::ReleaseLightingDepthSampleOnDiscard(token, stale, false), "A stale control discard must not release another token.");
			t.Expect(Renderer::ReleaseLightingDepthSampleOnDiscard(token, first, false) && Renderer::LightingDepthSampleCanRecord(token, next), "Actual unsubmitted discard must permit the next first observing frame.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthEvidence.ExactCompletedColorOwner", [](TestContext& t) {
			const auto capture = CapturedDepth(); auto color = CompletedColor(capture);
			t.Expect(Renderer::LightingDepthColorOwnerMatches(capture, color), "The actual same-command completed color owner must qualify.");
			for (unsigned fault = 0; fault < 9; ++fault)
			{
				color = CompletedColor(capture);
				if (fault == 0) ++color.frameSerial; if (fault == 1) ++color.commandSerial;
				if (fault == 2) ++color.submitSerial; if (fault == 3) ++color.completionSerial;
				if (fault == 4) color.viewType = RenderViewType::Game; if (fault == 5) ++color.viewId;
				if (fault == 6) ++color.effectiveOptions.rendererSessionGeneration;
				if (fault == 7) ++color.effectiveOptions.settingsGeneration; if (fault == 8) ++color.effectiveOptions.optionsSessionGeneration;
				t.Expect(!Renderer::LightingDepthColorOwnerMatches(capture, color), "A nearby frame or reassigned native owner cannot attach color.");
			}
		});
		registry.Add("Renderer.Efficiency.LightingDepthEvidence.ExactControlsAndOptions", [](TestContext& t) {
			const auto capture = CapturedDepth();
			for (unsigned fault = 0; fault < 12; ++fault)
			{
				auto color = CompletedColor(capture);
				if (fault == 0) ++color.fixtureControlGeneration;
				if (fault == 1) color.fixtureCorrelation.operationId += "-other";
				if (fault == 2) color.fixtureCorrelation.fixtureStepId += "-other";
				if (fault == 3) ++color.fixtureCorrelation.stepFrame;
				if (fault == 4) ++color.fixtureCorrelation.mutationGeneration;
				if (fault == 5) ++color.fixtureCorrelation.settingsGeneration;
				if (fault == 6) ++color.fixtureCorrelation.rendererSessionGeneration;
				if (fault == 7) color.effectiveOptions.options.shadowReuse = Renderer::RendererEfficiencyShadowReuse::AlwaysRefresh;
				if (fault == 8) color.effectiveOptions.options.lightingUploads = Renderer::RendererEfficiencyLightingUploads::Full;
				if (fault == 9) color.effectiveOptions.options.lightingDepth = Renderer::RendererEfficiencyLightingDepth::CompleteScan;
				if (fault == 10) ++color.effectiveOptions.options.staticShadowCacheBudgetBytes;
				if (fault == 11) color.effectiveOptions.options.validationPreparation = Renderer::RendererEfficiencyPreparation::Serial;
				t.Expect(!Renderer::LightingDepthColorOwnerMatches(capture, color), "Captured controls, actual option values and all correlation fields must match.");
			}
		});
		registry.Add("Renderer.Efficiency.LightingDepthEvidence.NoReservationOrIncompleteColorProof", [](TestContext& t) {
			const auto original = CapturedDepth();
			for (unsigned fault = 0; fault < 6; ++fault)
			{
				auto capture = original; auto color = CompletedColor(capture);
				if (fault == 0) color.status = Renderer::RendererEfficiencyEvidenceStatus::Pending;
				if (fault == 1) color.status = Renderer::RendererEfficiencyEvidenceStatus::Unavailable;
				if (fault == 2) color.artifacts.reset();
				if (fault == 3) color.artifacts = std::make_shared<Renderer::RendererEfficiencyArtifacts>();
				if (fault == 4) capture.submitSerial = color.submitSerial = 0;
				if (fault == 5) capture.completionSerial = color.completionSerial = 11;
				t.Expect(!Renderer::LightingDepthColorOwnerMatches(capture, color), "Recording, missing color or an incomplete fence is not completed color proof.");
			}
		});
	}
}
