// These tests exercise copied camera inputs and existing bounded world math;
// no editor panel, graphics driver, authored component or extra flush is needed.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureCameras.h"
#include <limits>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer;
		RendererEfficiencyFixtureIntent FixtureCameras()
		{
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId = "owned-camera-operation";
			RendererEfficiencyFixtureCamera scene; scene.viewType = RenderViewType::Scene; scene.outputId = 1;
			scene.cameraPosition.offsetMetres = {0, 4, 12}; scene.lookAtTarget.offsetMetres = {0, 1, 0};
			RendererEfficiencyFixtureCamera game = scene; game.viewType = RenderViewType::Game; game.outputId = 2;
			game.cameraPosition.offsetMetres = {12, 6, 8}; game.referenceOrigin.offsetMetres = {100, 0, 0};
			intent.cameras = {scene, game}; return intent;
		}
		void UnchangedReject(TestContext& context, const RendererEfficiencyFixtureIntent& intent)
		{
			std::vector<RendererEfficiencyResolvedFixtureCamera> output(1); output[0].outputId = 99; std::string error;
			context.Expect(!ResolveRendererEfficiencyFixtureCameras(intent, output, error) && !error.empty(), "Invalid camera batch must reject with a diagnostic.");
			context.Expect(output.size() == 1 && output[0].outputId == 99, "Rejected second camera must leave the whole prior batch unchanged.");
		}
	}
	void RegisterRendererEfficiencyFixtureCameraTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.FixtureCameras.FixedViewsNativeExtentAndVulkanProjection", [](TestContext& context) {
			const auto intent = FixtureCameras(); std::vector<RendererEfficiencyResolvedFixtureCamera> output; std::string error;
			const bool valid = ResolveRendererEfficiencyFixtureCameras(intent, output, error); context.Expect(valid, error);
			context.Expect(output.size() == 2 && output[0].outputId == 1 && output[1].outputId == 2 && output[0].nativeExtent == glm::uvec2(1920,1080), "Fixed descriptors must retain both real output identities and native extent.");
			context.Expect(output[0].cameraLocalPosition == glm::vec3(0,4,12) && output[1].cameraLocalPosition == glm::vec3(-88,6,8), "Camera positions must localize against their sealed independent reference origins.");
			for(const auto& camera : output) {
				const auto nearClip = camera.projectionMatrix * glm::vec4(0,0,-0.1F,1), farClip = camera.projectionMatrix * glm::vec4(0,0,-250.F,1);
				context.Expect(std::abs(nearClip.z/nearClip.w) < 0.00001F && std::abs(farClip.z/farClip.w-1.F) < 0.00001F && camera.projectionMatrix[1][1] < 0, "Projection must use Vulkan 0..1 depth and inverted Y.");
				const auto eye = camera.viewMatrix * glm::vec4(camera.cameraLocalPosition,1); context.Expect(glm::length(glm::vec3(eye)) < 0.0001F, "View matrix must use the actual localized camera position.");
			}
		});
		registry.Add("Renderer.Efficiency.FixtureCameras.HugeCanonicalCellsAndPerViewRebase", [](TestContext& context) {
			auto intent = FixtureCameras(); const Scene::WorldCell cell{std::numeric_limits<std::int64_t>::max()-10, -9000000000000LL, 7};
			for(auto& camera:intent.cameras){camera.cameraPosition.cell=cell;camera.lookAtTarget.cell=cell;camera.referenceOrigin.cell=cell;}
			intent.viewOrigins = {{RenderViewType::Game,2,Scene::WorldDelta{{},{10,0,0}}}};
			std::vector<RendererEfficiencyResolvedFixtureCamera> output; std::string error;
			const bool valid=ResolveRendererEfficiencyFixtureCameras(intent,output,error);context.Expect(valid,error);
			context.Expect(output[1].referenceOrigin.cell==cell && output[1].referenceOrigin.offsetMetres.x==110 && output[1].cameraLocalPosition.x==-98, "Rebase must shift only the sealed origin while preserving astronomical canonical camera cells.");
			// The generated multiview recipe uses a nearby 100m origin, not a
			// 65km cell jump that would exceed the native localization boundary.
			for(const auto type : {RenderViewType::Scene,RenderViewType::Game}) {
				auto bounded=FixtureCameras();bounded.viewOrigins={{type,type==RenderViewType::Scene?1U:2U,Scene::WorldDelta{{},{100,0,0}}}};
				context.Expect(ValidateRendererEfficiencyFixtureCameras(bounded,error),error);
			}
		});
		registry.Add("Renderer.Efficiency.FixtureCameras.InvalidSecondDescriptorIsAtomic", [](TestContext& context) {
			const auto original=FixtureCameras();
			for(unsigned invalid=0;invalid<9;++invalid){auto intent=original;auto& camera=intent.cameras[1];
				switch(invalid){case 0:camera.up={0,0,0};break;case 1:camera.lookAtTarget=camera.cameraPosition;break;case 2:camera.verticalFovDegrees=180;break;case 3:camera.nearPlane=0;break;case 4:camera.farPlane=camera.nearPlane;break;case 5:camera.nativeExtent={0,1080};break;case 6:camera.up.x=std::numeric_limits<double>::quiet_NaN();break;case 7:camera.referenceOrigin.cell.x=1;break;case 8:camera.cameraPosition.offsetMetres.x=Scene::kWorldCellHalfEdgeMetres;break;}
				UnchangedReject(context,intent);
			}
		});
		registry.Add("Renderer.Efficiency.FixtureCameras.OwnedExactViewIdentityRequired", [](TestContext& context) {
			auto intent=FixtureCameras();intent.correlation.operationId.clear();UnchangedReject(context,intent);
			intent=FixtureCameras();intent.cameras[1]=intent.cameras[0];UnchangedReject(context,intent);
			intent=FixtureCameras();intent.cameras[1].outputId=9000;UnchangedReject(context,intent);
			intent=FixtureCameras();intent.cameras.push_back(intent.cameras[0]);UnchangedReject(context,intent);
		});
		registry.Add("Renderer.Efficiency.FixtureCameras.SingleSelectedSceneSuppressesUnrequestedGame", [](TestContext& context) {
			auto intent=FixtureCameras();intent.cameras.pop_back();std::vector<RendererEfficiencyResolvedFixtureCamera> output;std::string error;
			const bool valid=ResolveRendererEfficiencyFixtureCameras(intent,output,error);context.Expect(valid,error);
			context.Expect(output.size()==1 && output[0].viewType==RenderViewType::Scene && output[0].outputId==1,"A Scene-only frozen recipe must not invent a second Game output.");
			intent.viewOrigins={{RenderViewType::Game,2,Scene::WorldDelta{}}};UnchangedReject(context,intent);
		});
		registry.Add("Renderer.Efficiency.FixtureCameras.EmptyIntentPreservesNormalViews", [](TestContext& context) {
			RendererEfficiencyFixtureIntent intent;std::vector<RendererEfficiencyResolvedFixtureCamera> output;std::string error;
			const bool valid=ResolveRendererEfficiencyFixtureCameras(intent,output,error);context.Expect(valid,error);context.Expect(output.empty(),"Uncontrolled frames have no fabricated camera overrides.");
		});
	}
}
