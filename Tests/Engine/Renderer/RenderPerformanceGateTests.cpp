#include "TestHarness.h"
#include "Core/Object/ObjectManager.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Graph/RenderDiagnostics.h"

#include <string>

namespace TerraEngine::Tests {

	void RegisterRenderPerformanceGateTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PerformanceGate.FailsRegressionOverThreshold", [](TestContext& context) {
			const auto result = TerraEngine::Renderer::EvaluateRenderPerformanceGate("small", 10.0f, 11.0f, 5.0f);
			context.Expect(!result.passed, "Performance gates should fail when latest cost exceeds the allowed threshold.");
		});

		registry.Add("Renderer.PerformanceGate.VirtualGeometryThresholdHelpersReportExpectedActual", [](TestContext& context) {
			const PerformanceGateResult gate = TerraEngine::Renderer::EvaluateVirtualGeometryPerformanceGate(
				"dense-cluster-batch-ratio",
				"virtual-geometry-dense-batching",
				"submittedBatchRatio",
				0.1f,
				0.2f,
				"ratio",
				true);

			context.Expect(gate.status == VirtualGeometryPerformanceGateStatus::Fail, "Virtual geometry gates should fail when lower-is-better metrics exceed the threshold.");
			context.ExpectEqual(std::string("virtual-geometry-dense-batching"), gate.fixtureId, "Virtual geometry gates should identify the fixture.");
			context.Expect(gate.diagnostic.find("actual") != std::string::npos, "Virtual geometry gate diagnostics should include the actual value.");
			context.Expect(gate.diagnostic.find("max") != std::string::npos, "Virtual geometry gate diagnostics should explain the threshold direction.");
		});

		registry.Add("Renderer.WorldTransformCache.ReusesStableHierarchyAndInvalidatesChangedBranches", [](TestContext& context) {
			auto& objects = ObjectManager::instance();
			objects.clear();
			Object parent = objects.createObject("Transform Cache Parent");
			Object child = objects.createObject("Transform Cache Child");
			context.Expect(objects.setParent(child, parent), "The cache fixture must create a parent-child hierarchy.");

			Transform* parentTransform = objects.GetComponent<Transform>(parent);
			Transform* childTransform = objects.GetComponent<Transform>(child);
			context.Expect(parentTransform != nullptr && childTransform != nullptr, "Created cache fixture objects must expose Transform components.");
			if (parentTransform == nullptr || childTransform == nullptr)
			{
				objects.clear();
				return;
			}
			parentTransform->position.x = 10.0f;
			childTransform->position.x = 2.0f;
			objects.MarkObjectTransformDirty(parent);
			objects.MarkObjectTransformDirty(child);

			TerraEngine::Renderer::RenderWorldTransformCache cache;
			TerraEngine::Renderer::RenderTransformCacheDiagnostics diagnostics;
			const auto& initialWorldTransforms = cache.Refresh(objects, diagnostics);
			const auto initialChild = initialWorldTransforms.find(child.EntityId());
			context.Expect(diagnostics.rebuiltForHierarchy, "The first protected render snapshot must build its hierarchy cache.");
			context.Expect(initialChild != initialWorldTransforms.end(), "The first transform snapshot must include the child entity.");
			if (initialChild != initialWorldTransforms.end())
			{
				context.Expect(initialChild->second.position.x == 12.0f, "The cache must preserve parent-relative world transform composition.");
			}

			cache.Refresh(objects, diagnostics);
			context.Expect(diagnostics.reusedWithoutPropagation, "An unchanged frame must reuse the protected world transform snapshot.");
			context.Expect(diagnostics.changedObjectCount == 0U, "An unchanged frame must not mark transform branches dirty.");

			childTransform->position.x = 4.0f;
			objects.MarkObjectTransformDirty(child);
			const auto& changedWorldTransforms = cache.Refresh(objects, diagnostics);
			const auto changedChild = changedWorldTransforms.find(child.EntityId());
			context.Expect(!diagnostics.rebuiltForHierarchy, "A local transform edit must not rebuild unchanged parent-child topology.");
			context.Expect(diagnostics.changedObjectCount == 1U, "A local transform edit must identify exactly the changed branch root.");
			context.Expect(diagnostics.propagatedObjectCount >= 1U, "A local transform edit must propagate the changed branch before rendering.");
			if (changedChild != changedWorldTransforms.end())
			{
				context.Expect(changedChild->second.position.x == 14.0f, "The cached child world transform must refresh after its authored local value changes.");
			}

			parentTransform->position.x = 20.0f;
			objects.MarkObjectTransformDirty(parent);
			const auto& parentChangedWorldTransforms = cache.Refresh(objects, diagnostics);
			const auto descendantAfterParentMove = parentChangedWorldTransforms.find(child.EntityId());
			context.Expect(diagnostics.changedObjectCount == 1U,
				"A parent-only local edit must identify the parent as the single dirty branch root.");
			context.Expect(diagnostics.propagatedObjectCount >= 2U,
				"A parent local edit must propagate through both the parent and its child.");
			context.Expect(descendantAfterParentMove != parentChangedWorldTransforms.end()
				&& descendantAfterParentMove->second.position.x == 24.0f,
				"Moving a parent must update the cached world transform of its child while preserving the child's local offset.");

			Object newRoot = objects.createObject("Transform Cache New Root");
			cache.Refresh(objects, diagnostics);
			context.Expect(diagnostics.rebuiltForHierarchy, "A structural change must rebuild the protected hierarchy snapshot.");
			context.Expect(cache.Refresh(objects, diagnostics).contains(newRoot.EntityId()), "A rebuilt snapshot must include objects introduced by a hierarchy revision.");
			objects.clear();

			Object flatRoot = objects.createObject("Flat Transform Cache Root");
			Transform* flatTransform = objects.GetComponent<Transform>(flatRoot);
			context.Expect(flatTransform != nullptr, "The flat cache fixture must expose a Transform component.");
			if (flatTransform != nullptr)
			{
				flatTransform->position.x = 3.0f;
				objects.MarkObjectTransformDirty(flatRoot);
				cache.Clear();
				cache.Refresh(objects, diagnostics);
				flatTransform->position.x = 7.0f;
				objects.MarkObjectTransformDirty(flatRoot);
				const auto& flatWorldTransforms = cache.Refresh(objects, diagnostics);
				const auto flatRootWorldTransform = flatWorldTransforms.find(flatRoot.EntityId());
				context.Expect(diagnostics.usedFlatHierarchyFastPath, "Independent root bodies must bypass generic hierarchy propagation planning.");
				context.Expect(flatRootWorldTransform != flatWorldTransforms.end() && flatRootWorldTransform->second.position.x == 7.0f, "The flat cache fast path must publish changed local transforms as world transforms.");
			}
			objects.clear();
		});
	}

}
