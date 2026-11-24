// Pure CPU contract tests: no Vulkan submission, image, or quality claim.
#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanPostProcessHistory.h"
#include "Renderer/Graph/RenderFramePublication.h"

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::Vulkan;
		struct HistoryFixture
		{
			PostProcessHistory owner;
			PostProcessHistoryScope scope;
			PostProcessHistorySignature signature;
			HistoryFixture()
			{
				scope.target.view = Renderer::GraphicsSettings::GraphicsViewIdentity{};
				scope.target.view->lifetimeGeneration = 1;
				scope.lifecycle = {1, 1, 1, 1};
				signature = {640, 360, 640, 360, 97, 126, 83, 1, 1, 1, 1, 1, 1, 1};
			}
			std::shared_ptr<Renderer::RenderFramePublication> Publication(std::uint64_t frame)
			{
				auto root = std::make_shared<Renderer::RenderFramePublication>();
				root->frameId = frame;
				root->rendererSessionGeneration = scope.lifecycle.rendererSessionGeneration;
				root->backendResourceGeneration = scope.lifecycle.backendResourceGeneration;
				auto settings = std::make_shared<Renderer::GraphicsSettings::GraphicsSettingsSnapshot>();
				settings->lifecycle = scope.lifecycle;
				Renderer::RenderViewPresentationInput view;
				view.graphicsView = scope.target.view;
				view.graphicsCamera = scope.target.camera;
				view.graphicsSettings = std::move(settings);
				view.output.kind = scope.target.view->outputKind;
				view.output.diagnosticsView = scope.target.view->viewType;
				view.output.outputId = scope.target.view->outputId;
				view.output.targetSize = {signature.outputWidth, signature.outputHeight};
				root->presentationInputs.push_back(std::move(view));
				return root;
			}
			auto Record(std::uint64_t token, std::uint64_t frame)
			{
				return owner.Record(token, scope, signature, Publication(frame), 0, frame, 0);
			}
		};
	}

	void RegisterPostProcessHistoryTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.History.AcceptedViewNotProducerTick", [](TestContext& c) {
			HistoryFixture f;
			auto first = f.Record(1, 1);
			c.Expect(first && !first->previous && !f.owner.Previous(f.scope, f.signature), "Recording must not publish history.");
			c.Expect(!f.Record(2, 2), "A second unresolved recording cannot silently share the same predecessor.");
			c.Expect(f.owner.Accept(1), "Exact successful submission can publish first history.");
			auto skipped = f.Record(2, 9);
			c.Expect(skipped && skipped->previous == first->current, "Skipped producer ticks retain the accepted view predecessor.");
			f.owner.Discard(2);
			c.Expect(f.owner.Previous(f.scope, f.signature) == first->current, "Abandoned recording cannot advance history.");
			auto next = f.Record(3, 14);
			c.Expect(next && next->previous == first->current && f.owner.Accept(3), "Next actual render uses accepted predecessor.");
			c.Expect(!f.owner.Accept(3) && !f.Record(3, 15), "Submission and recording tokens cannot replay.");
		});
		registry.Add("Renderer.PostProcess.History.CompletionAndRetirement", [](TestContext& c) {
			HistoryFixture f;
			auto first = f.Record(1, 1);
			c.Expect(first && f.owner.Accept(1), "First submission is retained without a fence.");
			f.owner.Complete(0, 5);
			c.Expect(f.owner.RetainedUseCount() == 1 && !f.owner.AttachCompletion(1, 0, 5), "Old signaled epochs cannot qualify unfenced work.");
			c.Expect(f.owner.AttachCompletion(1, 0, 6), "A newly armed epoch can own the submission.");
			auto second = f.Record(2, 2);
			std::weak_ptr<const Renderer::RenderFramePublication> root = first->current->publication;
			first.reset();
			f.owner.Retire(f.scope);
			c.Expect(!f.owner.Previous(f.scope, f.signature), "Retired view loses lookup eligibility.");
			f.owner.Complete(1, 100);
			c.Expect(f.owner.RetainedUseCount() == 2, "Another slot cannot release this submission.");
			f.owner.Complete(0, 6);
			c.Expect(f.owner.RetainedUseCount() == 1 && !root.expired(), "Recorded borrower retains its predecessor after producer completion.");
			f.owner.CompleteDeviceIdle();
			c.Expect(f.owner.RetainedUseCount() == 1, "Device idle cannot discard never-submitted borrowers.");
			f.owner.Discard(2); second.reset();
			c.Expect(root.expired(), "Final abandoned borrower releases retired publication.");
		});
		registry.Add("Renderer.PostProcess.History.ExactCompatibility", [](TestContext& c) {
			HistoryFixture f;
			c.Expect(f.Record(1, 1).has_value() && f.owner.Accept(1), "Seed accepted history.");
			const auto check = [&](PostProcessHistorySignature other) { c.Expect(!f.owner.Previous(f.scope, other), "A changed compatibility field resets history."); };
			auto s = f.signature; ++s.internalWidth; check(s);
			s = f.signature; ++s.outputHeight; check(s);
			s = f.signature; ++s.colorFormat; check(s);
			s = f.signature; ++s.depthFormat; check(s);
			s = f.signature; ++s.motionFormat; check(s);
			s = f.signature; ++s.scaleNumerator; check(s);
			s = f.signature; ++s.scaleDenominator; check(s);
			s = f.signature; ++s.reconstructionGeneration; check(s);
			s = f.signature; ++s.projectionGeneration; check(s);
			s = f.signature; ++s.cutGeneration; check(s);
			s = f.signature; ++s.worldOriginGeneration; check(s);
			s = f.signature; ++s.surfaceGeneration; check(s);
			auto scope = f.scope; ++scope.target.view->lifetimeGeneration;
			c.Expect(!f.owner.Previous(scope, f.signature), "View lifetimes never alias.");
			scope = f.scope; ++scope.lifecycle.backendResourceGeneration;
			c.Expect(!f.owner.Previous(scope, f.signature), "Backend lifetimes never alias.");
			scope = f.scope; ++scope.lifecycle.playSessionGeneration;
			c.Expect(!f.owner.Previous(scope, f.signature), "Play sessions never alias.");
		});
		registry.Add("Renderer.PostProcess.History.PublicationProofAndIdle", [](TestContext& c) {
			HistoryFixture f;
			auto root = f.Publication(1);
			++root->presentationInputs[0].graphicsView->lifetimeGeneration;
			c.Expect(!f.owner.Record(1, f.scope, f.signature, root, 0, 1, 0), "Copied caller labels cannot replace consumed view provenance.");
			root = f.Publication(1);
			c.Expect(!f.owner.Record(1, f.scope, f.signature, root, 1, 1, 0), "Absent presentation index is rejected.");
			c.Expect(!f.owner.Record(1, f.scope, f.signature, root, 0, 2, 0), "Frame must match the immutable publication.");
			c.Expect(f.Record(1, 1).has_value() && f.owner.Accept(1), "Rejected recordings do not consume token.");
			f.owner.Discard(1);
			c.Expect(f.owner.RetainedUseCount() == 1, "Discard cannot revoke accepted unfenced ownership.");
			f.owner.CompleteDeviceIdle();
			c.Expect(f.owner.RetainedUseCount() == 0 && f.owner.Previous(f.scope, f.signature), "Proven idle releases uses while preserving last accepted state.");
		});
	}
}
