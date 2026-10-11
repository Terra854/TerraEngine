// Invalid isolated recipes must be rejected before the existing intent queue
// publishes any control. Numeric bit patterns remain exact, including NaNs.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureDepthControls.h"
#include <array>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer;
		using LightingDepthReduction::Extent;
		std::optional<RendererEfficiencyFixtureDepthControls> ValidDepthControls()
		{
			RendererEfficiencyFixtureDepthControls controls;
			controls.oracleRecipe = LightingDepthReduction::DepthRecipe{{7,5},0x7fc01234U,{{3,1,0x3f000000U}}};
			return controls;
		}
		bool Admit(const std::optional<RendererEfficiencyFixtureDepthControls>& controls,
			bool owned = true, std::array<Extent,2> cameras = {{{7,5},{7,5}}})
		{
			std::string reason;
			return ValidateRendererEfficiencyFixtureDepthControls(controls,owned,cameras,reason);
		}
	}
	void RegisterLightingDepthFixtureControlTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.LightingDepthControls.ExactBitsAndOrdinaryFrames",[](TestContext& context){
			context.Expect(Admit(ValidDepthControls()),"Invalid numeric depth samples are intentional exact-bit oracle inputs.");
			context.Expect(Admit(std::nullopt,false),"Ordinary rendering has no validation recipe admission cost.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthControls.OwnedCameraAdmission",[](TestContext& context){
			context.Expect(!Admit(ValidDepthControls(),false),"An unowned producer cannot supply synthetic GPU input.");
			std::string reason;
			context.Expect(!ValidateRendererEfficiencyFixtureDepthControls(ValidDepthControls(),true,{},reason),"Oracle work requires actual sealed output cameras.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthControls.ExactExtent",[](TestContext& context){
			context.Expect(!Admit(ValidDepthControls(),true,{{{7,5},{8,5}}}),"One differing output cannot borrow another camera's oracle extent.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthControls.DuplicateAndOutOfBounds",[](TestContext& context){
			auto controls=ValidDepthControls(); controls->oracleRecipe->overrides.push_back({3,1,0x3f800000U});
			context.Expect(!Admit(controls),"Duplicate pixel writes make an immutable recipe ambiguous.");
			controls=ValidDepthControls(); controls->oracleRecipe->overrides.push_back({7,0,0});
			context.Expect(!Admit(controls),"Recipe coordinates must remain in the actual image.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthControls.BoundedSparseInput",[](TestContext& context){
			auto controls=ValidDepthControls(); controls->oracleRecipe->extent={1920,1080};
			controls->oracleRecipe->overrides.clear();
			for(std::uint32_t x=0;x<1025;++x) controls->oracleRecipe->overrides.push_back({x,0,0});
			context.Expect(!Admit(controls,true,{{{1920,1080},{1920,1080}}}),"Ingress bounds sparse preparation work independently of image capacity.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthControls.InvalidDimensions",[](TestContext& context){
			auto controls=ValidDepthControls(); controls->oracleRecipe->extent={0,5};
			context.Expect(!Admit(controls),"Zero-sized recipes cannot produce native oracle work.");
			controls->oracleRecipe->extent={1921,1080};
			context.Expect(!Admit(controls,true,{{{1921,1080},{1921,1080}}}),"Validation-only capacity limits do not expand through camera requests.");
		});
	}
}
