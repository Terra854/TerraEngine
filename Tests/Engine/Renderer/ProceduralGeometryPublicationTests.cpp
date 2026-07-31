#include "TestHarness.h"

#include "Renderer/VirtualGeometry/Geometry/ProceduralGeometryPublication.h"

namespace TerraEngine::Tests
{
	void RegisterProceduralGeometryPublicationTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.ProceduralPublication.TransactionalIngressAndFlushVisibility",
			[](TestContext &context)
			{
				Renderer::ProceduralGeometryPublication publication;
				const auto transaction = publication.BeginTransaction(7U);
				context.Expect(publication.SubmitSourceEdit(transaction, Renderer::ProceduralGeometryEdit::ControlPointChanged),
					"Source edits must enter through a typed deferred transaction.");
				context.Expect(!publication.IsVisible(7U), "Queued source edits must remain invisible before the protected scene flush.");
				publication.FlushSceneMutations();
				context.Expect(publication.IsVisible(7U), "Queued source edits become visible only at the declared protected flush boundary.");
			});

		registry.Add("VirtualGeometry.ProceduralPublication.StaleConflictCancelRollbackAndFenceRetirement",
			[](TestContext &context)
			{
				Renderer::ProceduralGeometryPublication publication;
				const auto base = publication.CurrentGeneration(11U);
				const auto stale = publication.PrepareCandidate(11U, base - 1U);
				context.Expect(!publication.Publish(stale), "Stale candidates must be rejected atomically.");
				context.Expect(publication.CurrentGeneration(11U) == base, "Stale rejection must retain the last-known-good generation.");
				const auto conflict = publication.PrepareCandidate(11U, base);
				publication.Cancel(conflict);
				context.Expect(!publication.Publish(conflict), "Canceled candidates must never publish partially.");
				context.Expect(publication.RejectConflictingTransaction(12U), "Conflicting transactions must report rejection without partial visibility.");
				context.Expect(publication.RetireAfterFence(11U, base), "Retirement must wait for the declared device fence.");
			});

		registry.Add("VirtualGeometry.ProceduralPublication.AtomicSwapAndCrossViewGeneration",
			[](TestContext &context)
			{
				Renderer::ProceduralGeometryPublication publication;
				const auto oldGeneration = publication.CurrentGeneration(21U);
				const auto candidate = publication.PrepareCandidate(21U, oldGeneration);
				context.Expect(publication.Publish(candidate), "A complete candidate must atomically replace the prior generation.");
				const auto current = publication.CurrentGeneration(21U);
				context.Expect(current != oldGeneration, "A successful publication must advance generation identity.");
				context.Expect(publication.ViewGeneration(21U, Renderer::GeometryViewKind::Camera) == current &&
					publication.ViewGeneration(21U, Renderer::GeometryViewKind::Editor) == current,
					"All consuming views must observe one accepted geometry generation.");
			});
	}
}
