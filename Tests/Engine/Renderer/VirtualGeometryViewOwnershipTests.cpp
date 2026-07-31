#include "Renderer/VirtualGeometry/Selection/VirtualGeometryViewPolicy.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryViewOwnershipTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.ViewOwnership.SharesCompatibleGeometry",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryViewPolicyState camera{ VirtualGeometryViewKind::Camera, 1, 4, 5 }, shadow{ VirtualGeometryViewKind::Shadow, 2, 4, 6 };
				const VirtualGeometryViewPolicyDecision decision = EvaluateVirtualGeometryViewPolicy(shadow, camera);
				c.Expect(decision.sharesGeometry && !decision.ownsHistory
					&& !decision.historyValid && decision.pass == VirtualGeometryOcclusionPass::Main,
					"Compatible shadow views share immutable geometry but never borrow camera occlusion history.");
			});
		r.Add("VirtualGeometry.ViewOwnership.CaptureReflectionAndEditorPoliciesStayExplicit",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryViewPolicyState camera{ VirtualGeometryViewKind::Camera, 1, 4, 5 };
				VirtualGeometryViewPolicyState capture{ VirtualGeometryViewKind::Capture, 2, 4, 5 };
				VirtualGeometryViewPolicyState reflection{ VirtualGeometryViewKind::Reflection, 3, 4, 5 };
				VirtualGeometryViewPolicyState editor{ VirtualGeometryViewKind::Editor, 4, 4, 5 };
				editor.historyOwnerViewId = editor.viewId;
				c.Expect(EvaluateVirtualGeometryViewPolicy(capture, camera).sharesGeometry
					&& !EvaluateVirtualGeometryViewPolicy(capture, camera).ownsHistory,
					"Capture views must use a main pass even when their geometry generation matches.");
				c.Expect(EvaluateVirtualGeometryViewPolicy(reflection, camera).sharesGeometry
					&& !EvaluateVirtualGeometryViewPolicy(reflection, camera).ownsHistory,
					"Reflection views must keep independent history ownership.");
				c.Expect(EvaluateVirtualGeometryViewPolicy(editor, editor).ownsHistory
					&& EvaluateVirtualGeometryViewPolicy(editor, editor).historyValid,
					"An unchanged editor view can reuse only its own warm history.");
			});
	}
} // namespace TerraEngine::Tests
