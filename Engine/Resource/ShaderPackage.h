// Detached TSPF decoding for checked package/export inputs.
#pragma once
#include "Core/Export.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

struct Shader;
namespace TerraEngine::Resource
{
	inline constexpr std::size_t kMaximumShaderPackageBytes = 64u * 1024u * 1024u;
	// Decode exactly the supplied bytes with the production TSPF structural
	// parser. diagnosticPath labels errors only: no file/cache/asset lookup, UUID
	// assignment, logging, or runtime publication occurs here. The returned owner
	// retains every reflection/bytecode span after the input buffer is released.
	// Its publicationGeneration remains zero; metadata/package binding supplies
	// asset identity separately. Registration applies the intended shader ABI.
	TERRAENGINE_API std::shared_ptr<const Shader> DecodeShaderPackage(
		std::span<const std::uint8_t> checkedBytes,
		const std::filesystem::path& diagnosticPath, std::string& error);
}
