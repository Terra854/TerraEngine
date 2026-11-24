// GraphicsSettingsPersistence.h — off-frame, game-owned persistence coordination.
#pragma once
#include "GraphicsSettingsTypes.h"
#include "Core/Export.h"
#include "Scripting/GraphicsPreferenceStore.h"
#include <filesystem>
#include <memory>

namespace TerraEngine::Renderer::GraphicsSettings
{
	// Validate copied catalog data only. Admission cannot touch native resources,
	// authored profiles, live cameras, or a renderer from its worker callback.
	TERRAENGINE_API GraphicsRequestReason ValidateGraphicsStartupTransaction(
		const GraphicsStartupTransaction&, std::span<const GraphicsSettingDescriptor>);
	struct GraphicsPreferenceRestore
	{
		GraphicsRequestId requestId = 0;
		GraphicsSettingsRequest request;
	};
	class TERRAENGINE_API GraphicsSettingsPersistence final
	{
	public:
		// A standalone loader supplies its already-consumed startup snapshot so
		// original durable receipts exist before the first managed callback.
		GraphicsSettingsPersistence(const std::filesystem::path& gameDataRoot,
			std::string productIdentity, bool allowStartup,
			std::optional<GraphicsStartupStoreSnapshot> initialStartup = {});
		~GraphicsSettingsPersistence();
		GraphicsSettingsPersistence(const GraphicsSettingsPersistence&) = delete;
		GraphicsSettingsPersistence& operator=(const GraphicsSettingsPersistence&) = delete;
		GraphicsRequestReceipt Stage(GraphicsStartupTransaction,
			std::vector<GraphicsSettingDescriptor> copiedCatalog);
		GraphicsRequestReceipt Cancel(GraphicsRequestId stagedRequestId);
		GraphicsRequestReceipt Save(std::string name, std::vector<GraphicsSettingValue>, std::uint64_t expectedStoreRevision);
		GraphicsRequestReceipt Read(std::string name);
		GraphicsRequestReceipt Restore(std::string name, GraphicsSettingsRequest copiedRequestTemplate);
		// A bootstrap storage failure is launch-local evidence, not a successful
		// empty disk snapshot. It remains queryable under the original request ID.
		void RecordStartupFailure(GraphicsStartupStoreResult failure,
			std::optional<GraphicsRequestReceipt> originalReceipt = {});
		bool Ready() const;
		GraphicsStartupStoreResult InitializationResult() const;
		std::optional<GraphicsStartupStoreSnapshot> StartupSnapshot() const;
		std::optional<Scripting::GraphicsPreferenceSnapshot> PreferenceSnapshot(const std::string& name) const;
		std::optional<GraphicsRequestReceipt> GetReceipt(GraphicsRequestId) const;
		// Loading is conservatively conflicting. An accepted Stage immediately
		// reserves its IDs, before the worker performs the durable file write.
		bool Conflicts(std::span<const GraphicsSettingId>) const;
		// Protected renderer code enqueues these through its ordinary controller.
		// Its returned low-bit ID stays internal; copy outcomes back under the
		// original high-bit preference request ID, including terminal failures.
		std::vector<GraphicsPreferenceRestore> DrainRestores();
		void UpdateRestoreReceipt(GraphicsRequestId originalId, const GraphicsRequestReceipt& nativeReceipt);
	private:
		struct State;
		std::shared_ptr<State> state;
	};
}
