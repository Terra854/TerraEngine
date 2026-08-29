// TerrainCompiler.cpp -- 16-bit lossless terrain source decoding.

#include "TerrainCompiler.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "External/stb/stb_image.h"

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		bool Report(const TerrainCompileProgressCallback& callback, float progress, std::string_view stage, std::string_view message)
		{
			return !callback || callback(progress, stage, message);
		}

		bool ValidateRequest(const TerrainImportRequest& request, std::string& error)
		{
			if(request.assetId == TerraEngine::EMPTY_UUID)
			{
				error = "Terrain creation requires a stable parent asset UUID before decoding begins.";
				return false;
			}
			if(!std::isfinite(request.heightScaleMetres) || !std::isfinite(request.heightOffsetMetres) || !std::isfinite(request.sampleSpacingMetres)
				|| request.heightScaleMetres <= 0.0F || request.sampleSpacingMetres <= 0.0F)
			{
				error = "Terrain scale, offset, and sample spacing must be finite; scale and spacing must be positive.";
				return false;
			}
			if(request.sourceFormat != TerrainSourceFormat::Flat && request.sourcePath.empty())
			{
				error = "An image or RAW source path is required for terrain import.";
				return false;
			}
			if(request.sourceFormat == TerrainSourceFormat::Flat
				&& (request.flatWidth == 0U || request.flatDepth == 0U))
			{
				error = "Flat terrain width and depth must both be positive.";
				return false;
			}
			const std::uint64_t requestedSamples = request.sourceFormat == TerrainSourceFormat::Flat
				? static_cast<std::uint64_t>(request.flatWidth) * request.flatDepth
				: static_cast<std::uint64_t>(request.rawWidth) * request.rawHeight;
			if (requestedSamples > request.maximumSamples || requestedSamples > (std::numeric_limits<std::size_t>::max)())
			{
				error = "Terrain dimensions exceed the configured creation sample budget.";
				return false;
			}
			if(request.sourceFormat == TerrainSourceFormat::Raw16LittleEndian && (request.rawWidth == 0U || request.rawHeight == 0U))
			{
				error = "RAW16 imports require declared non-zero width and height.";
				return false;
			}
			return true;
		}

		TerrainCompileResult ScaleSamples(std::vector<std::uint16_t> source, std::uint32_t width, std::uint32_t height, const TerrainImportRequest& request)
		{
			TerrainCompileResult result;
			result.heightfield.width = width;
			result.heightfield.height = height;
			result.heightfield.heightsMm.reserve(source.size());
			for(const std::uint16_t sample : source)
			{
				const double metres = static_cast<double>(request.heightOffsetMetres) + (static_cast<double>(sample) / 65535.0) * request.heightScaleMetres;
				const double millimetres = metres * 1000.0;
				if(millimetres < static_cast<double>(std::numeric_limits<std::int32_t>::min()) || millimetres > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
				{
					result.message = "The declared height range exceeds the terrain millimetre representation.";
					return result;
				}
				result.heightfield.heightsMm.push_back(static_cast<std::int32_t>(std::llround(millimetres)));
			}
			result.succeeded = true;
			result.message = "Heightfield decoded without precision remapping.";
			return result;
		}
	}

	TerrainCompileResult CompileTerrainHeightfield(const TerrainImportRequest& request, TerrainCompileProgressCallback progress)
	{
		TerrainCompileResult result;
		if(!ValidateRequest(request, result.message)) return result;
		if(!Report(progress, 0.05F, "Terrain Validate", "Validated source dimensions and height range.")) { result.cancelled = true; result.message = "Terrain import cancelled before decoding."; return result; }
		if(request.sourceFormat == TerrainSourceFormat::Flat)
		{
			std::vector<std::uint16_t> flat(static_cast<std::size_t>(request.flatWidth) * request.flatDepth, 0U);
			result = ScaleSamples(std::move(flat), request.flatWidth, request.flatDepth, request);
			if(result.succeeded && !Report(progress, 1.0F, "Terrain Complete", "Created a variable-size flat terrain.")) { result = {}; result.cancelled = true; result.message = "Terrain creation cancelled."; }
			return result;
		}

		std::vector<std::uint16_t> samples;
		std::uint32_t width = request.rawWidth, height = request.rawHeight;
		if(request.sourceFormat == TerrainSourceFormat::Png16)
		{
			if(!stbi_is_16_bit(request.sourcePath.string().c_str())) { result.message = "PNG terrain import requires a true 16-bit source; 8-bit images are rejected to avoid precision remapping."; return result; }
			int decodedWidth = 0, decodedHeight = 0, components = 0;
			stbi_us* pixels = stbi_load_16(request.sourcePath.string().c_str(), &decodedWidth, &decodedHeight, &components, 1);
			if(!pixels || decodedWidth <= 0 || decodedHeight <= 0) { result.message = "PNG16 decoding failed; the source must be a readable 16-bit grayscale PNG."; if(pixels) stbi_image_free(pixels); return result; }
			width = static_cast<std::uint32_t>(decodedWidth); height = static_cast<std::uint32_t>(decodedHeight);
			const std::uint64_t decodedSamples = static_cast<std::uint64_t>(width) * height;
			if(decodedSamples > request.maximumSamples || decodedSamples > (std::numeric_limits<std::size_t>::max)())
			{
				stbi_image_free(pixels);
				result.message = "The decoded PNG dimensions exceed the configured terrain sample budget.";
				return result;
			}
			samples.assign(pixels, pixels + static_cast<std::size_t>(width) * height);
			stbi_image_free(pixels);
		}
		else
		{
			std::ifstream input(request.sourcePath, std::ios::binary);
			if(!input) { result.message = "RAW16 source could not be opened."; return result; }
			const std::uint64_t expectedBytes = static_cast<std::uint64_t>(width) * height * 2U;
			input.seekg(0, std::ios::end); const std::uint64_t actualBytes = static_cast<std::uint64_t>(input.tellg()); input.seekg(0, std::ios::beg);
			if(actualBytes != expectedBytes) { result.message = "RAW16 byte count does not match width x height x 2; no terrain was registered."; return result; }
			samples.resize(static_cast<std::size_t>(width) * height);
			for(std::uint16_t& sample : samples) { const int low = input.get(), high = input.get(); if(low < 0 || high < 0) { result.message = "RAW16 ended before the declared sample count."; return result; } sample = static_cast<std::uint16_t>(low | (high << 8)); }
		}
		if(!Report(progress, 0.80F, "Terrain Remap", "Applying the declared height scale and origin.")) { result.cancelled = true; result.message = "Terrain import cancelled before publication."; return result; }
		result = ScaleSamples(std::move(samples), width, height, request);
		if(result.succeeded && !Report(progress, 1.0F, "Terrain Complete", "Decoded a precise 16-bit heightfield.")) { result = {}; result.cancelled = true; result.message = "Terrain import cancelled."; }
		return result;
	}

	std::optional<std::pair<std::uint32_t, std::uint32_t>>
		TerrainCompatibilityDimensions(TerraEngine::Terrain::TerrainSampleBounds bounds) noexcept
	{
		if(!bounds.IsValid() || bounds.minX >= bounds.maxXExclusive
			|| bounds.minZ >= bounds.maxZExclusive) return std::nullopt;
		const long double width = static_cast<long double>(bounds.maxXExclusive)
			- static_cast<long double>(bounds.minX);
		const long double depth = static_cast<long double>(bounds.maxZExclusive)
			- static_cast<long double>(bounds.minZ);
		if(width <= 0.0L || depth <= 0.0L
			|| width > static_cast<long double>((std::numeric_limits<std::uint32_t>::max)())
			|| depth > static_cast<long double>((std::numeric_limits<std::uint32_t>::max)())) return std::nullopt;
		return std::pair{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(depth)};
	}
}
