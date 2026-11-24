// GraphicsStartupStore.h — game-owned durable startup transactions.
#pragma once
#include "Core/Export.h"
#include "GraphicsSettingsTypes.h"
#include <filesystem>
#include <memory>

namespace TerraEngine::Renderer::GraphicsSettings
{
	// Opening has no renderer side effects. Every operation obtains the durable
	// file lock, reloads the revision, and replaces a complete document atomically.
	TERRAENGINE_API std::unique_ptr<GraphicsStartupStore> OpenGraphicsStartupStore(
		const std::filesystem::path& gameDataRoot, std::string productIdentity);

	// A nonblocking replay lease spans the whole candidate lifetime, including
	// deferred native initialization. Only its owner may interpret a surviving
	// claimed record as interrupted; the short document lock alone cannot prove it.
	class GraphicsStartupReplayLease
	{
	public:
		virtual ~GraphicsStartupReplayLease() = default;
	};
	TERRAENGINE_API std::unique_ptr<GraphicsStartupReplayLease> TryAcquireGraphicsStartupReplayLease(
		const std::filesystem::path& gameDataRoot, const std::string& productIdentity,
		GraphicsStartupStoreResult& result);

	namespace PersistenceDetail
	{
		// Shared native persistence implementation for ordinary named preferences.
		// These values are never interpreted as an authored or effective profile.
		GraphicsStartupStoreResult ReadPreference(const std::filesystem::path&, const std::string&,
			const std::string&, std::vector<GraphicsSettingValue>&, std::uint64_t&);
		GraphicsStartupStoreResult SavePreference(const std::filesystem::path&, const std::string&,
			const std::string&, const std::vector<GraphicsSettingValue>&, std::uint64_t);
	}
}
