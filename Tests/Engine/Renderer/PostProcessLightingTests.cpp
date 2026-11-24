// Headless lighting contracts only. Real GPU image/timing acceptance is recorded
// separately by the installed foundation fixture; copied certificates are inputs.
#include "TestHarness.h"
#include "VulkanSourceTestUtils.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <regex>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer;
		using namespace Renderer::GraphicsSettings;
		constexpr std::array Enabled{GraphicsSettingId::GtaoEnabled,GraphicsSettingId::ScreenSpaceReflectionsEnabled,GraphicsSettingId::ScreenSpaceIndirectEnabled};
		constexpr std::array Intensity{GraphicsSettingId::GtaoIntensity,GraphicsSettingId::ScreenSpaceReflectionsIntensity,GraphicsSettingId::ScreenSpaceIndirectIntensity};
		constexpr std::array Capability{PostProcessEffectCapability::GTAO,PostProcessEffectCapability::SSR,PostProcessEffectCapability::SSGI};
		GraphicsSettingsSnapshot Snapshot(std::vector<GraphicsSettingValue> values)
		{
			GraphicsSettingsSnapshot result;
			result.values=std::move(values); result.lifecycle.backendResourceGeneration=7;
			result.postProcessResourceGeneration=7; result.sampleCount=1;
			result.internalWidth=result.outputWidth=640; result.internalHeight=result.outputHeight=360;
			return result;
		}
		// Ignore comments/formatting in supplementary source guards, while still
		// requiring the actual executable guard and replacement arithmetic tokens.
		std::string Code(std::string_view path)
		{
			auto text=VulkanSourceContracts::ReadSources({path});
			text=std::regex_replace(text,std::regex(R"(/\*[\s\S]*?\*/|//[^\r\n]*)"),"");
			std::erase_if(text,[](unsigned char c) { return std::isspace(c)!=0; });
			return text;
		}
	}

	void RegisterPostProcessLightingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.Lighting.AtmosphereAndDitheringRequireQualifiedOutputs", [](TestContext& test) {
			// A zero-density medium requests capability but creates no HDR pass.
			const auto fog = Snapshot({{GraphicsSettingId::VolumetricFogEnabled, true}, {GraphicsSettingId::VolumetricFogDensity, 0.0f}});
			PostProcessEffectSettings controls;
			test.Expect(ResolvePostProcessEffectSettings(fog.values, controls) == GraphicsRequestReason::None
				&& !controls.atmosphere.Active() && controls.RequestedCapabilities() == PostProcessEffectCapability::Atmosphere,
				"Zero-density fog must retain intent without recording work.");
			const auto shafts = BuildPostProcessGraphSettings(Snapshot({{GraphicsSettingId::SunShaftsEnabled, true}}));
			const auto dither = BuildPostProcessGraphSettings(Snapshot({{GraphicsSettingId::OutputDitheringEnabled, true}}));
			test.Expect(shafts.executionAvailable && shafts.hdrCamera && !shafts.outputDithering,
				"Shafts must independently qualify the HDR camera chain.");
			test.Expect(dither.executionAvailable && dither.outputDithering && !dither.hdrCamera && !dither.presentationEffects,
				"Dithering alone must activate the output composite.");
			const auto invalid = Snapshot({{GraphicsSettingId::OutputDitheringEnabled, true},
				{GraphicsSettingId::VolumetricFogMaxDistance, -1.0f}});
			test.Expect(ResolvePostProcessEffectSettings(invalid.values, controls) == GraphicsRequestReason::InvalidValue
				&& controls.atmosphere.fogEnabled && !controls.outputDithering,
				"An invalid medium setting must reject the complete mixed tuple.");
		});
		registry.Add("Renderer.PostProcess.Lighting.IndependentTypedToggles",[](TestContext& test) {
			for(std::uint32_t mask=0;mask<8;++mask)
			{
				std::vector<GraphicsSettingValue> values;
				for(std::size_t i=0;i<3;++i) values.push_back({Enabled[i],(mask&(1u<<i))!=0});
				PostProcessEffectSettings controls;
				test.Expect(ResolvePostProcessEffectSettings(values,controls)==GraphicsRequestReason::None,"Every independent lighting toggle tuple must resolve.");
				test.Expect(controls.gtao.Active()==bool(mask&1) && controls.reflections.Active()==bool(mask&2)
					&& controls.indirect.Active()==bool(mask&4),"One lighting toggle must not activate another effect.");
				test.Expect(static_cast<std::uint32_t>(controls.RequestedCapabilities())==mask,"Requested capabilities must retain exactly the enabled effects.");
				const auto plan=BuildPostProcessGraphSettings(Snapshot(values));
				test.Expect(plan.screenSpaceLighting==(mask!=0) && plan.executionAvailable==(mask!=0)
					&& !plan.hdrCamera && !plan.toneAndGrade && !plan.spatialAA,"Lighting alone must activate only its qualified graph stage.");
			}
		});
		registry.Add("Renderer.PostProcess.Lighting.ZeroIntensityStillRequiresCapability",[](TestContext& test) {
			for(std::size_t i=0;i<3;++i)
			{
				const auto snapshot=Snapshot({{Enabled[i],true},{Intensity[i],0.0f}});
				PostProcessEffectSettings controls;
				test.Expect(ResolvePostProcessEffectSettings(snapshot.values,controls)==GraphicsRequestReason::None && !controls.HasScreenSpace(),"Zero amplitude must avoid work without erasing requested intent.");
				test.Expect(ValidatePostProcessEffectCapabilities(controls,PostProcessEffectCapability::None)==GraphicsRequestReason::Unsupported
					&& ValidatePostProcessEffectCapabilities(controls,Capability[i])==GraphicsRequestReason::None,"Zero contribution cannot bypass native capability admission.");
				test.Expect(BuildPostProcessGraphTopologySignature(BuildPostProcessGraphSettings(snapshot))==0,"Zero contribution must allocate no graph stage.");
			}
		});
		registry.Add("Renderer.PostProcess.Lighting.InvalidMixedTuplePreservesControls",[](TestContext& test) {
			PostProcessEffectSettings controls; controls.gtao.enabled=true; controls.gtao.intensity=2;
			for(const auto bad : {-0.01f,4.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
			{
				const std::array values{GraphicsSettingValue{GraphicsSettingId::GtaoEnabled,false},
					GraphicsSettingValue{GraphicsSettingId::ScreenSpaceIndirectEnabled,true},GraphicsSettingValue{GraphicsSettingId::ScreenSpaceIndirectIntensity,bad}};
				test.Expect(ResolvePostProcessEffectSettings(values,controls)==GraphicsRequestReason::InvalidValue
					&& controls.gtao.enabled && controls.gtao.intensity==2 && !controls.indirect.enabled,"A rejected mixed tuple must not partially replace prior controls.");
			}
		});
		registry.Add("Renderer.PostProcess.Lighting.UncertifiedGraphPreservesLegacy",[](TestContext& test) {
			RenderFrameSnapshot frame; frame.viewRequests.push_back({RenderOutputKind::GameView,RenderViewType::Game,{640,360},2});
			auto passes=BuildDefaultRenderPasses(frame,{}); const auto count=passes.size(); const auto* storage=passes.data();
			auto snapshot=Snapshot({{GraphicsSettingId::GtaoEnabled,true}}); snapshot.postProcessResourceGeneration=6;
			const auto plan=BuildPostProcessGraphSettings(snapshot);
			AppendPostProcessRenderPasses(passes,frame.viewRequests.front(),plan);
			test.Expect(plan.screenSpaceLighting && !plan.executionAvailable && BuildPostProcessGraphTopologySignature(plan)==0
				&& passes.data()==storage && passes.size()==count,"An obsolete resource certificate must preserve the existing legacy graph storage.");
		});
		registry.Add("Renderer.PostProcess.Lighting.GraphReadsGeometryBeforeOverlay",[](TestContext& test) {
			RenderFrameSnapshot frame; frame.viewRequests.push_back({RenderOutputKind::GameView,RenderViewType::Game,{640,360},2});
			auto passes=BuildDefaultRenderPasses(frame,{});
			AppendPostProcessRenderPasses(passes,frame.viewRequests.front(),BuildPostProcessGraphSettings(Snapshot({{GraphicsSettingId::ScreenSpaceReflectionsEnabled,true}})));
			const auto lighting=std::find_if(passes.begin(),passes.end(),[](const auto& pass){ return pass.postProcessKind==PostProcessPassKind::ScreenSpaceLighting; });
			const auto overlay=std::find_if(passes.begin(),passes.end(),[](const auto& pass){ return pass.postProcessKind==PostProcessPassKind::OverlayComposite; });
			test.Expect(lighting!=passes.end() && overlay!=passes.end(),"Lighting and terminal overlay must both be declared.");
			for(const auto semantic : {RenderResourceSemantic::SceneDepth,RenderResourceSemantic::SceneColor})
				test.Expect(std::any_of(lighting->inputs.begin(),lighting->inputs.end(),[&](const auto& input){return input.semantic==semantic && input.accessMode==RenderAccessMode::Read;}),"Lighting must consume actual geometry depth and scene radiance.");
			test.Expect(std::find(overlay->dependencies.begin(),overlay->dependencies.end(),lighting->passId)!=overlay->dependencies.end()
				&& lighting->outputs.size()==1 && lighting->outputs[0].semantic==RenderResourceSemantic::ProcessedColor
				&& ValidateRenderGraph(passes).valid,"Lighting must precede HUD composition through a valid typed color chain.");
		});
		registry.Add("Renderer.PostProcess.Lighting.SourceContract.ReplaceOnceAndMissBypass",[](TestContext& test) {
			const auto native=Code("Engine/Renderer/RHI/Vulkan/VulkanPostProcessScreenSpaceLighting.cpp");
			const auto shader=Code("Engine/Shaders/PostProcess/ScreenSpaceComposite.comp.hlsl");
			const auto surface=Code("Engine/Shaders/Default/Default.frag.hlsl");
			test.Expect(native.find("((mask&4u)&&frame.geometry.originalAo!=ScreenSpaceOriginalAo::LegacyScreenAoOmitted)")!=std::string::npos,
				"Enhanced AO must reject a producer that retained legacy screen AO.");
			test.Expect(surface.find("#if!defined(TERRAENGINE_SCREEN_SPACE_OMIT_LEGACY_AO)screenOcclusion=SampleScreenAmbientOcclusion(input,frame);#endif")!=std::string::npos
				&& surface.find("lerp(1.0f,authoredOcclusion*screenOcclusion,saturate(aoStrength)*aoQualityEnabled)")!=std::string::npos,
				"The enhanced producer must omit only old screen AO, preserving authored occlusion and its existing quality/strength gate.");
			test.Expect(shader.find("ao=(composite.enabledMask&4u)!=0u&&(composite.inputFlags&1u)!=0u;")!=std::string::npos
				&& shader.find("color=original.rgb-originalIndirect+(originalIndirect+delta)*visibility;")!=std::string::npos,
				"Enhanced AO must replace original indirect terms once, retaining material AO in the original weights.");
			const auto initialStore=shader.find("composedHdr[pixel]=original;");
			const auto validityGuard=shader.find("if(!CompositeExtent(width,height)");
			test.Expect(shader.find("delta+=weight*(replacement-fallback);")!=std::string::npos
				&& shader.find("if(all(delta==0)&&(visibility==1||all(originalIndirect==0)))return;")!=std::string::npos
				&& initialStore!=std::string::npos && validityGuard!=std::string::npos && initialStore<validityGuard
				&& shader.find("composedHdr[pixel]=float4(color,original.a);")!=std::string::npos,
				"Miss/invalid inputs must preserve the current HDR source and alpha; successful replacement applies its material weight once.");
		});
	}
}
