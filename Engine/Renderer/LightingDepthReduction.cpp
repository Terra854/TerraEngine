#include "LightingDepthReduction.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace TerraEngine::Renderer::LightingDepthReduction
{
	static bool ValidExtent(Extent e) { return e.width && e.height && e.width<=kMaximumWidth && e.height<=kMaximumHeight; }
	bool BuildTileRect(Extent e,std::uint32_t x,std::uint32_t y,TileRect& output)
	{
		if(!ValidExtent(e)||x>=kTilesX||y>=kTilesY) return false;
		// Preserve the existing HLSL float division, multiplication and truncation.
		const float dx=float(e.width)/float(kTilesX),dy=float(e.height)/float(kTilesY);
		output={std::uint32_t(float(x)*dx),std::uint32_t(float(y)*dy),std::min(e.width,std::uint32_t(float(x+1)*dx)),std::min(e.height,std::uint32_t(float(y+1)*dy))};
		return true;
	}
	bool MaterializeDepthRecipe(const DepthRecipe& recipe,std::span<std::uint32_t> output)
	{
		if(!ValidExtent(recipe.extent)||output.size()!=std::size_t(recipe.extent.width)*recipe.extent.height) return false;
		// Validate and sort coordinates before touching caller memory. The bounded
		// product fits size_t even on 32-bit hosts; duplicate detection is O(n log n).
		if(recipe.overrides.size()>output.size()) return false;
		std::vector<std::size_t> coordinates; coordinates.reserve(recipe.overrides.size());
		for(const auto& pixel:recipe.overrides) {
			if(pixel.x>=recipe.extent.width||pixel.y>=recipe.extent.height) return false;
			coordinates.push_back(std::size_t(pixel.y)*recipe.extent.width+pixel.x);
		}
		std::sort(coordinates.begin(),coordinates.end());
		if(std::adjacent_find(coordinates.begin(),coordinates.end())!=coordinates.end()) return false;
		std::fill(output.begin(),output.end(),recipe.fillBits);
		for(const auto& pixel:recipe.overrides) output[std::size_t(pixel.y)*recipe.extent.width+pixel.x]=pixel.bits;
		return true;
	}
	bool ReduceCompleteScan(Extent extent,std::span<const std::uint32_t> input,TileResults& output)
	{
		if(!ValidExtent(extent)||input.size()!=std::size_t(extent.width)*extent.height) return false;
		TileResults result{};
		for(std::uint32_t tileY=0;tileY<kTilesY;++tileY) for(std::uint32_t tileX=0;tileX<kTilesX;++tileX) {
			TileRect rect; BuildTileRect(extent,tileX,tileY,rect); auto& tile=result[tileY*kTilesX+tileX]; float minimum=1,maximum=0;
			// The independent reference visits every texel, including odd positions.
			for(auto y=rect.yBegin;y<rect.yEnd;++y) for(auto x=rect.xBegin;x<rect.xEnd;++x) {
				++tile.coverage.visited; const float depth=std::bit_cast<float>(input[std::size_t(y)*extent.width+x]);
				if(std::isfinite(depth)&&depth>=0&&depth<1) { ++tile.coverage.eligible; minimum=std::min(minimum,depth);maximum=std::max(maximum,depth); }
			}
			if(tile.coverage.eligible) tile.bounds={minimum,maximum};
		}
		output=result; return true;
	}
	bool ReduceCooperativeOracle(Extent e,std::span<const std::uint32_t> input,TileResults& output)
	{
		if(!ValidExtent(e)||input.size()!=std::size_t(e.width)*e.height) return false;
		TileResults result{};
		for(std::uint32_t ty=0;ty<kTilesY;++ty) for(std::uint32_t tx=0;tx<kTilesX;++tx) {
			TileRect rect; BuildTileRect(e,tx,ty,rect);
			const auto width=rect.xEnd-rect.xBegin, count=width*(rect.yEnd-rect.yBegin);
			auto& tile=result[ty*kTilesX+tx]; float minimum=1,maximum=0;
			// Model independent lane work without assuming hardware subgroup width.
			// Empty lanes still reduce their neutral values; no empty-tile division occurs.
			for(std::uint32_t lane=0;lane<kLaneCount;++lane) {
				float laneMin=1,laneMax=0; std::uint32_t eligible=0,visited=0;
				for(auto local=lane;local<count;local+=kLaneCount) {
					const auto x=rect.xBegin+local%width,y=rect.yBegin+local/width;
					++visited; const float depth=std::bit_cast<float>(input[std::size_t(y)*e.width+x]);
					if(std::isfinite(depth)&&depth>=0&&depth<1) { ++eligible;laneMin=std::min(laneMin,depth);laneMax=std::max(laneMax,depth); }
				}
				tile.coverage.visited+=visited;tile.coverage.eligible+=eligible;
				minimum=std::min(minimum,laneMin);maximum=std::max(maximum,laneMax);
			}
			if(tile.coverage.eligible) tile.bounds={minimum,maximum};
		}
		output=result;return true;
	}
	bool ValidateCompletedOracleReadback(const OracleReadback& r,const ReadbackOwner& expected)
	{
		const auto& owner=r.owner;
		if(!r.validationOptIn||!r.recorded||!r.submitted||!r.completed||owner!=expected
			||!owner.rendererSession||!owner.backendGeneration||!owner.queryPoolGeneration
			||!owner.frameSerial||!owner.commandSerial||!owner.submitSerial||owner.completionSerial<owner.submitSerial
			||!owner.controlGeneration||!owner.optionsSessionGeneration||!owner.settingsGeneration||!owner.mutationGeneration
			||owner.operationId.empty()||owner.fixtureStepId.empty()||owner.frameSlot>=3||owner.snapshotSlot>=8||owner.viewType>1
			||r.bounds.size()!=kTileCount||r.coverage.size()!=kTileCount) return false;
		TileResults reference;
		if(!ReduceCompleteScan(r.extent,r.input,reference)) return false;
		// These are numerical checks of supplied completed bytes, not GPU authority.
		// Completion and correlation must be independently captured by the collector.
		for(std::size_t i=0;i<kTileCount;++i) {
			const auto b=r.bounds[i],want=reference[i].bounds;
			if(r.coverage[i]!=reference[i].coverage||!std::isfinite(b.minDepth)||!std::isfinite(b.maxDepth)
				||b.minDepth<0||b.maxDepth>1||b.minDepth>b.maxDepth) return false;
			if(!reference[i].coverage.eligible) { if(b!=TileBounds{1,1}) return false; }
			else if(std::abs(b.minDepth-want.minDepth)>1e-6f||std::abs(b.maxDepth-want.maxDepth)>1e-6f) return false;
		}
		return true;
	}
}
