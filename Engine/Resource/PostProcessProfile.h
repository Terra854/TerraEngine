// Versioned authored values, without renderer resources or live scene pointers.
#pragma once
#include "Core/Export.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Resource
{
	// The catalog supplies legacy defaults for fields absent in older profiles.
	// Values are typed owned copies; the mask distinguishes inheritance from an
	// explicit default value. Asset loading/scene authoring must publish const roots.
	struct PostProcessProfile
	{
		static constexpr std::uint32_t kCurrentSchemaVersion = 1u;
		std::uint32_t schemaVersion = kCurrentSchemaVersion;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t revision = 0u;
		std::string name;
		std::vector<Renderer::GraphicsSettings::GraphicsSettingValue> values;
		std::vector<Renderer::GraphicsSettings::GraphicsSettingId> overrideMask;
		std::vector<TerraEngine::UUID> customEffects;
		// Explicit false means inherit; true plus an empty list clears the list.
		bool overrideCustomEffects = false;
	};
	inline constexpr std::size_t kMaximumPostProcessProfileBytes = 1024u * 1024u;

	struct PostProcessProfileDecodeResult
	{
		Renderer::GraphicsSettings::GraphicsRequestReason reason = Renderer::GraphicsSettings::GraphicsRequestReason::None;
		std::string detail;
		// Failure never replaces a previously loaded immutable asset root.
		std::shared_ptr<const PostProcessProfile> profile;
	};

	// Project-file and package loaders feed the same bounded UTF8 JSON bytes.
	// An expected asset ID prevents a package/path mixup from relabeling a root.
	// Structural decoding does not certify device support or resolve dependencies;
	// copied-catalog resolution and compiled custom-pass validation follow loading.
	TERRAENGINE_API PostProcessProfileDecodeResult DeserializePostProcessProfile(std::span<const std::byte> bytes,
		const TerraEngine::UUID& expectedAssetId = TerraEngine::EMPTY_UUID);

	// Canonical versioned JSON. Failure leaves inOutBytes completely unchanged.
	TERRAENGINE_API bool SerializePostProcessProfile(const PostProcessProfile& profile,
		std::vector<std::byte>& inOutBytes, std::string& detail);

}
