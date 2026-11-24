// GraphicsPreferenceStore.cpp — explicit, revision-checked named player saves.
#include "GraphicsPreferenceStore.h"
#include "Renderer/GraphicsSettings/GraphicsStartupStore.h"
#include <utility>

namespace TerraEngine::Scripting
{
	namespace
	{
		class FilePreferences final : public GraphicsPreferenceStore
		{
			std::filesystem::path root;
			std::string identity;
		public:
			FilePreferences(std::filesystem::path path, std::string product)
				: root(std::move(path)), identity(std::move(product)) {}
			GraphicsPreferenceStoreResult Read(const std::string& name, GraphicsPreferenceSnapshot& output) override
			{
				// Publish the caller's snapshot only after a complete checked decode.
				GraphicsPreferenceSnapshot candidate;
				auto result = Renderer::GraphicsSettings::PersistenceDetail::ReadPreference(
					root, identity, name, candidate.values, candidate.revision);
				if(result.succeeded) output = std::move(candidate);
				return result;
			}
			GraphicsPreferenceStoreResult Save(const std::string& name,
				const std::vector<Renderer::GraphicsSettings::GraphicsSettingValue>& values,
				std::uint64_t revision) override
			{
				return Renderer::GraphicsSettings::PersistenceDetail::SavePreference(root, identity, name, values, revision);
			}
		};
	}
	std::unique_ptr<GraphicsPreferenceStore> OpenGraphicsPreferenceStore(
		const std::filesystem::path& root, std::string identity)
	{
		return std::make_unique<FilePreferences>(root, std::move(identity));
	}
}
