// CPU behavior checks are supporting evidence. Native opt-in readback must
// independently execute both installed shaders before shader acceptance.
#include "TestHarness.h"
#include "Renderer/LightingDepthReduction.h"
#include <algorithm>
#include <bit>
#include <limits>
namespace TerraEngine::Tests
{
	void RegisterLightingDepthReductionTests(TestRegistry& registry)
	{
		using namespace Renderer::LightingDepthReduction;
		registry.Add("Renderer.Efficiency.DepthReduction.FloorRectanglesCoverEveryPixelOnce",[](TestContext& t){
			for(const auto extent:std::array<Extent,4>{{{1920,1080},{1919,1079},{7,5},{1,1}}}) {
				std::vector<unsigned> visits(std::size_t(extent.width)*extent.height);
				for(unsigned y=0;y<kTilesY;++y) for(unsigned x=0;x<kTilesX;++x) { TileRect r; t.Expect(BuildTileRect(extent,x,y,r),"Supported floor rectangle exists.");for(auto py=r.yBegin;py<r.yEnd;++py)for(auto px=r.xBegin;px<r.xEnd;++px)++visits[std::size_t(py)*extent.width+px]; }
				t.Expect(std::all_of(visits.begin(),visits.end(),[](auto n){return n==1;}),"No pixel is missing or duplicated by fixed floor partition.");
			}
		});
		registry.Add("Renderer.Efficiency.DepthReduction.ThinSkippedSampleAndEdgePixels",[](TestContext& t){
			DepthRecipe recipe{{1920,1080},0x3f800000U,{{1,1,0x3e800000U},{1919,1079,0x3f400000U}}};std::vector<std::uint32_t> input(1920*1080);TileResults output;
			t.Expect(MaterializeDepthRecipe(recipe,input)&&ReduceCompleteScan(recipe.extent,input,output),"Owned bit recipe materializes and reduces.");
			t.Expect(output[0].bounds==TileBounds{.25f,.25f}&&output.back().bounds==TileBounds{.75f,.75f},"Thin odd-position and final edge samples contribute.");
			std::uint64_t visited=0,eligible=0;for(auto tile:output){visited+=tile.coverage.visited;eligible+=tile.coverage.eligible;}
			t.Expect(visited==input.size()&&eligible==2,"Actual visited and accepted counts cover the complete input.");
		});
		registry.Add("Renderer.Efficiency.DepthReduction.FiniteHalfOpenAcceptanceAndEmpty",[](TestContext& t){
			Extent extent{16,9};std::vector<std::uint32_t> input(144,0x3f800000U);TileResults output;
			const std::array<float,8> values{{-.1f,0.f,.5f,1.f,1.1f,std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}};
			for(std::size_t i=0;i<values.size();++i)input[i]=std::bit_cast<std::uint32_t>(values[i]);
			t.Expect(ReduceCompleteScan(extent,input,output),"Finite policy consumes exact owned input.");
			for(std::size_t i=0;i<values.size();++i){const bool accepted=i==1||i==2;t.Expect(output[i].coverage.visited==1&&output[i].coverage.eligible==unsigned(accepted),"Only finite [0,1) samples contribute.");t.Expect(output[i].bounds==(accepted?TileBounds{values[i],values[i]}:TileBounds{1,1}),"Invalid and clear pixels leave exact empty float2.");}
		});
		registry.Add("Renderer.Efficiency.DepthReduction.CooperativeOracleMatchesCompleteOddAndSmall",[](TestContext& t){
			for(auto extent:std::array<Extent,5>{{{1919,1079},{7,5},{1,1},{1,1080},{1920,1}}}) { std::vector<std::uint32_t> input(std::size_t(extent.width)*extent.height);for(std::size_t i=0;i<input.size();++i)input[i]=std::bit_cast<std::uint32_t>(float(i%101)/101.f);TileResults a,b;t.Expect(ReduceCompleteScan(extent,input,a)&&ReduceCooperativeOracle(extent,input,b),"Both CPU numerical routes accept native extents.");std::uint64_t visited=0;for(unsigned i=0;i<144;++i){t.Expect(a[i].bounds==b[i].bounds&&a[i].coverage==b[i].coverage,"All64 lanes match the independent complete scan.");visited+=b[i].coverage.visited;}t.Expect(visited==input.size(),"Cooperative coverage is complete."); }
		});
		registry.Add("Renderer.Efficiency.DepthReduction.InvalidRecipeAndExtentAreAtomic",[](TestContext& t){
			std::vector<std::uint32_t> input(35,123);DepthRecipe recipe{{7,5},0,{{1,1,0},{1,1,1}}};t.Expect(!MaterializeDepthRecipe(recipe,input)&&input[0]==123,"Duplicate sparse overrides reject before mutation.");recipe.overrides={{7,0,0}};t.Expect(!MaterializeDepthRecipe(recipe,input)&&input[0]==123,"Outside pixels reject atomically.");TileRect rect{4,4,4,4};t.Expect(!BuildTileRect({0,5},0,0,rect)&&rect.xBegin==4,"Zero dimensions preserve output.");TileResults output;output[0].coverage.visited=99;t.Expect(!ReduceCompleteScan({7,5},std::span<const std::uint32_t>(input).first(34),output)&&output[0].coverage.visited==99,"Incomplete input cannot publish partial oracle.");
		});
		registry.Add("Renderer.Efficiency.DepthReduction.OptInCompletedReadbackRequiresExactOwnerAndCoverage",[](TestContext& t){
			std::vector<std::uint32_t> input(144,0x3f000000U);TileResults result; t.Expect(ReduceCompleteScan({16,9},input,result),"CPU expected values exist.");std::array<TileBounds,144> bounds;std::array<TileCoverage,144> coverage;for(unsigned i=0;i<144;++i){bounds[i]=result[i].bounds;coverage[i]=result[i].coverage;}
			ReadbackOwner owner{1,2,3,4,4,0,1,1,1};owner.backendGeneration=owner.queryPoolGeneration=owner.controlGeneration=owner.optionsSessionGeneration=owner.settingsGeneration=owner.mutationGeneration=1;owner.operationId="operation";owner.fixtureStepId="step";
			OracleReadback readback{owner,{16,9},true,true,true,true,input,bounds,coverage};t.Expect(ValidateCompletedOracleReadback(readback,owner),"Complete opt-in proof qualifies for numerical comparison only.");readback.completed=false;t.Expect(!ValidateCompletedOracleReadback(readback,owner),"Pending copy cannot qualify.");readback.completed=true;readback.validationOptIn=false;t.Expect(!ValidateCompletedOracleReadback(readback,owner),"Ordinary production cannot claim oracle readback.");readback.validationOptIn=true;
			// Exact identities reject stale/reassigned owners rather than just values.
			for(unsigned field=0;field<17;++field) { readback.owner=owner;switch(field) {
				case 0:++readback.owner.rendererSession;break;case 1:++readback.owner.backendGeneration;break;case 2:++readback.owner.queryPoolGeneration;break;case 3:++readback.owner.frameSerial;break;case 4:++readback.owner.commandSerial;break;case 5:++readback.owner.submitSerial;break;case 6:++readback.owner.completionSerial;break;case 7:++readback.owner.frameSlot;break;case 8:++readback.owner.snapshotSlot;break;case 9:++readback.owner.viewType;break;case 10:++readback.owner.outputId;break;case 11:++readback.owner.controlGeneration;break;case 12:++readback.owner.optionsSessionGeneration;break;case 13:++readback.owner.settingsGeneration;break;case 14:readback.owner.operationId="other";break;case 15:readback.owner.fixtureStepId="other";break;case 16:++readback.owner.mutationGeneration;break;
			} t.Expect(!ValidateCompletedOracleReadback(readback,owner),"Every exact physical/correlation identity remains authoritative."); }
			readback.owner=owner;coverage[0].visited=0;t.Expect(!ValidateCompletedOracleReadback(readback,owner),"Missing real texture load rejects.");coverage[0]=result[0].coverage;bounds[0].maxDepth=.75f;t.Expect(!ValidateCompletedOracleReadback(readback,owner),"Incorrect completed extrema reject.");
			bounds[0]=result[0].bounds;readback.coverage=std::span<const TileCoverage>(coverage).first(143);t.Expect(!ValidateCompletedOracleReadback(readback,owner),"Truncated shader coverage cannot qualify.");readback.coverage=coverage;
			std::fill(input.begin(),input.end(),0x3f800000U);t.Expect(ReduceCooperativeOracle({16,9},input,result),"All-clear input has a complete cooperative result.");for(unsigned i=0;i<144;++i){bounds[i]=result[i].bounds;coverage[i]=result[i].coverage;}t.Expect(ValidateCompletedOracleReadback(readback,owner),"Complete clear tiles require exact empty bounds and real visits.");bounds[0].minDepth=.9999999f;t.Expect(!ValidateCompletedOracleReadback(readback,owner),"Empty bounds are exact, not a tolerance-based fabricated contribution.");
		});
	}
}
