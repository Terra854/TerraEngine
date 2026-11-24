// PostProcessEffectAsset.h - versioned project/package fullscreen-effect metadata.
#pragma once
#include "Core/Export.h"
#include "Renderer/Graph/PostProcessCustomPass.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct Shader;
namespace TerraEngine::Resource
{
	inline constexpr std::size_t kMaximumPostProcessEffectBytes = 1024u * 1024u;
	struct PostProcessEffectAsset
	{
		// The shader is an immutable dependency asset, never an embedded shader
		// string or an authoring path. ResourceManager assigns publication identity.
		Renderer::PostProcessCustomPassDescriptor descriptor;
	};
	struct PostProcessEffectDecodeResult
	{
		Renderer::GraphicsSettings::GraphicsRequestReason reason = Renderer::GraphicsSettings::GraphicsRequestReason::None;
		std::string detail;
		std::shared_ptr<const PostProcessEffectAsset> effect;
	};
	struct PostProcessEffectParameterReflection
	{
		std::uint32_t parameterBytes = 0;
		std::vector<Renderer::CustomPostProcessParameterLayout> parameters;
	};

	// Both loose project assets and checked package entries use this bounded
	// UTF8 JSON envelope. Decode failure returns no root; serialization failure
	// leaves caller bytes unchanged. Neither API accesses files or live state.
	TERRAENGINE_API PostProcessEffectDecodeResult DeserializePostProcessEffectAsset(
		std::span<const std::byte> bytes, const TerraEngine::UUID& expectedAssetId = TerraEngine::EMPTY_UUID);
	TERRAENGINE_API bool SerializePostProcessEffectAsset(const PostProcessEffectAsset& effect,
		std::vector<std::byte>& inOutBytes, std::string& detail);

	// Read actual named push-constant members/types/offsets from this Shader's
	// compiled SPIR-V. Arrays, matrices, nested structs and unsupported scalar
	// widths fail closed. Logical Boolean is never guessed from an unsigned type.
	// Failure preserves the caller's previous reflection result.
	TERRAENGINE_API bool ReflectPostProcessEffectParameters(const Shader& shader,
		PostProcessEffectParameterReflection& inOutReflection, std::string& detail);

	// Joins metadata and the SAME immutable shader owner used for reflection.
	// Parameter offsets, physical types, names and byte size must match exactly.
	// Runtime dependency/input/capability and native pipeline checks still follow.
	// Failure leaves the previous registration unchanged.
	TERRAENGINE_API bool PreparePostProcessEffectRegistration(const PostProcessEffectAsset& effect,
		const TerraEngine::UUID& loadedShaderAssetId, std::shared_ptr<const Shader> shader, bool enabled,
		std::span<const Renderer::CustomPostProcessParameterValue> values,
		Renderer::PostProcessCustomPassRegistration& inOutRegistration, std::string& detail);
}
