// GraphicsPreferenceStore.h — explicit player choices, separate from authored data.
#pragma once
#include "Core/Export.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"
#include <filesystem>
#include <memory>

namespace TerraEngine::Scripting
{
	struct GraphicsPreferenceSnapshot
	{
		std::uint32_t schemaVersion = 1;
		std::uint64_t revision = 0;
		std::vector<Renderer::GraphicsSettings::GraphicsSettingValue> values;
	};
	using GraphicsPreferenceStoreResult = Renderer::GraphicsSettings::GraphicsStartupStoreResult;
	class GraphicsPreferenceStore
	{
	public:
		virtual ~GraphicsPreferenceStore() = default;
		virtual GraphicsPreferenceStoreResult Read(const std::string& name, GraphicsPreferenceSnapshot&) = 0;
		virtual GraphicsPreferenceStoreResult Save(const std::string& name,
			const std::vector<Renderer::GraphicsSettings::GraphicsSettingValue>& values,
			std::uint64_t expectedRevision) = 0;
	};
	// Restore is deliberately a separate validated renderer request owned by the
	// caller. Reading this store never changes effective settings or scene data.
	TERRAENGINE_API std::unique_ptr<GraphicsPreferenceStore> OpenGraphicsPreferenceStore(
		const std::filesystem::path& gameDataRoot, std::string productIdentity);
}
