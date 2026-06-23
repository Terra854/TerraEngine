// ResourceResidencyWindow.cpp - ImGui surface for loaded resource residency.
//
// The panel reports RAM/VRAM residency decisions made by ResourceManager. It
// deliberately avoids persistent backend disk cache concepts because this spec
// only controls live loaded assets that can leave CPU/GPU memory.

#include "ResourceResidencyWindow.h"

#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Resource/ResourceResidency.h"
#include "Engine/Resource/ResourceStreaming.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <imgui.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	const char* BoolText(bool value)
	{
		return value ? "yes" : "no";
	}

	void TextStringView(std::string_view value)
	{
		ImGui::TextUnformatted(value.data(), value.data() + value.size());
	}

	std::optional<AssetType> AssetTypeFromStreamingType(TerraEngine::Resource::StreamableAssetType type)
	{
		switch (type)
		{
		case TerraEngine::Resource::StreamableAssetType::Mesh:
			return AssetType::Mesh;
		case TerraEngine::Resource::StreamableAssetType::Material:
			return AssetType::Material;
		case TerraEngine::Resource::StreamableAssetType::Texture:
			return AssetType::Texture;
		case TerraEngine::Resource::StreamableAssetType::ShaderCollection:
			return AssetType::ShaderCollection;
		case TerraEngine::Resource::StreamableAssetType::Other:
			return std::nullopt;
		}
		return std::nullopt;
	}

	std::optional<AssetType> AssetTypeFromResidencyType(TerraEngine::Resource::ResidencyAssetType type)
	{
		switch (type)
		{
		case TerraEngine::Resource::ResidencyAssetType::Mesh:
			return AssetType::Mesh;
		case TerraEngine::Resource::ResidencyAssetType::Texture:
			return AssetType::Texture;
		case TerraEngine::Resource::ResidencyAssetType::Material:
			return AssetType::Material;
		case TerraEngine::Resource::ResidencyAssetType::ShaderCollection:
			return AssetType::ShaderCollection;
		case TerraEngine::Resource::ResidencyAssetType::Shader:
		case TerraEngine::Resource::ResidencyAssetType::FontAtlas:
		case TerraEngine::Resource::ResidencyAssetType::Other:
			return std::nullopt;
		}
		return std::nullopt;
	}

	std::string FriendlyAssetLabel(const TerraEngine::UUID& id, const std::filesystem::path& path)
	{
		if (!path.empty())
		{
			const std::filesystem::path filename = path.filename();
			if (!filename.empty())
			{
				return filename.string();
			}
			return path.string();
		}
		return TerraEngine::ToHexString(id);
	}

	std::string LowercaseCopy(std::string_view text)
	{
		std::string lowered;
		lowered.reserve(text.size());
		for (char value : text)
		{
			lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
		}
		return lowered;
	}

	bool ContainsInsensitive(std::string_view haystack, std::string_view needle)
	{
		if (needle.empty())
		{
			return true;
		}
		return LowercaseCopy(haystack).find(LowercaseCopy(needle)) != std::string::npos;
	}

	std::string DiagnosticAssetLabel(const TerraEngine::UUID& id, std::optional<AssetType> assetType, const std::filesystem::path& sourceHint = {})
	{
		ResourceManager& resources = ResourceManager::Instance();
		std::filesystem::path displayPath = sourceHint;
		if (displayPath.empty() && assetType.has_value())
		{
			displayPath = resources.DescribeAssetReference(id, *assetType);
		}
		return FriendlyAssetLabel(id, displayPath);
	}

	void DrawAssetCell(const TerraEngine::UUID& id, std::optional<AssetType> assetType, const std::filesystem::path& sourceHint = {})
	{
		ResourceManager& resources = ResourceManager::Instance();
		std::filesystem::path displayPath = sourceHint;
		if (displayPath.empty() && assetType.has_value())
		{
			displayPath = resources.DescribeAssetReference(id, *assetType);
		}

		// The table is meant for quick scanning, so show a readable asset name
		// first and keep the UUID/source details available on hover.
		const std::string label = FriendlyAssetLabel(id, displayPath);
		ImGui::TextUnformatted(label.c_str());
		if (ImGui::IsItemHovered())
		{
			const std::string idText = TerraEngine::ToHexString(id);
			ImGui::BeginTooltip();
			ImGui::Text("UUID: %s", idText.c_str());
			if (!displayPath.empty())
			{
				ImGui::TextWrapped("Path: %s", displayPath.string().c_str());
			}
			ImGui::EndTooltip();
		}
	}

	std::string_view StreamingMemoryLocation(const TerraEngine::Resource::StreamingDiagnosticRecord& record)
	{
		switch (record.payloadState)
		{
		case TerraEngine::Resource::StreamablePayloadState::Registered:
		case TerraEngine::Resource::StreamablePayloadState::Deferred:
		case TerraEngine::Resource::StreamablePayloadState::LoadingRam:
		case TerraEngine::Resource::StreamablePayloadState::Released:
			return "not loaded";
		case TerraEngine::Resource::StreamablePayloadState::RamResident:
			return "RAM";
		case TerraEngine::Resource::StreamablePayloadState::UploadingVram:
			return "RAM -> VRAM";
		case TerraEngine::Resource::StreamablePayloadState::VramResident:
			// A GPU-ready asset may still keep CPU-side metadata or decoded data
			// alive. Show both locations when the estimates say both pools hold
			// memory so the diagnostics do not imply a pure VRAM-only payload.
			return record.estimatedCpuBytes > 0u ? "RAM + VRAM" : "VRAM";
		case TerraEngine::Resource::StreamablePayloadState::PendingRelease:
			return "pending unload";
		case TerraEngine::Resource::StreamablePayloadState::Failed:
			return "failed";
		}
		return "unknown";
	}

	std::string_view StreamingLoadStateLabel(TerraEngine::Resource::StreamablePayloadState state)
	{
		// LoadingRam is retained in the enum for compatibility with older
		// diagnostics/tests, but new loading no longer publishes that state.
		// Present it as deferred so the window does not imply a permanent RAM
		// load when the asset is merely waiting for demand or policy.
		if (state == TerraEngine::Resource::StreamablePayloadState::LoadingRam)
		{
			return "deferred";
		}
		return TerraEngine::Resource::ToString(state);
	}

	bool MatchesTypeFilter(TerraEngine::Resource::StreamableAssetType type, int filter)
	{
		switch (filter)
		{
		case 1:
			return type == TerraEngine::Resource::StreamableAssetType::Mesh;
		case 2:
			return type == TerraEngine::Resource::StreamableAssetType::Material;
		case 3:
			return type == TerraEngine::Resource::StreamableAssetType::Texture;
		case 4:
			return type == TerraEngine::Resource::StreamableAssetType::ShaderCollection;
		default:
			return true;
		}
	}

	bool MatchesResidencyTypeFilter(TerraEngine::Resource::ResidencyAssetType type, int filter)
	{
		switch (filter)
		{
		case 1:
			return type == TerraEngine::Resource::ResidencyAssetType::Mesh;
		case 2:
			return type == TerraEngine::Resource::ResidencyAssetType::Material;
		case 3:
			return type == TerraEngine::Resource::ResidencyAssetType::Texture;
		case 4:
			return type == TerraEngine::Resource::ResidencyAssetType::ShaderCollection;
		default:
			return true;
		}
	}

	bool MatchesMemoryFilter(std::string_view memory, int filter)
	{
		switch (filter)
		{
		case 1:
			return memory == "not loaded";
		case 2:
			return memory == "RAM";
		case 3:
			return memory == "VRAM" || memory == "RAM + VRAM" || memory == "RAM -> VRAM";
		case 4:
			return memory == "pending unload";
		default:
			return true;
		}
	}

	bool MatchesLoadStateFilter(TerraEngine::Resource::StreamablePayloadState state, int filter)
	{
		switch (filter)
		{
		case 1:
			return state == TerraEngine::Resource::StreamablePayloadState::Deferred
				|| state == TerraEngine::Resource::StreamablePayloadState::Registered
				|| state == TerraEngine::Resource::StreamablePayloadState::Released;
		case 2:
			return state == TerraEngine::Resource::StreamablePayloadState::RamResident;
		case 3:
			return state == TerraEngine::Resource::StreamablePayloadState::VramResident;
		case 4:
			return state == TerraEngine::Resource::StreamablePayloadState::Failed;
		default:
			return true;
		}
	}

	std::string MipResidencyLabel(const TerraEngine::Resource::StreamingDiagnosticRecord& record)
	{
		if (record.assetType != TerraEngine::Resource::StreamableAssetType::Texture
			|| record.textureSourceMipCount == 0u
			|| record.textureResidentMipCount == 0u)
		{
			return "-";
		}
		std::string label = std::to_string(record.textureResidentMipCount)
			+ "/"
			+ std::to_string(record.textureSourceMipCount);
		label += " from mip ";
		label += std::to_string(record.textureResidentMipOffset);
		return label;
	}

	nlohmann::json BuildResidencyPolicyJson(const TerraEngine::Resource::ResidencyPolicy& policy)
	{
		nlohmann::json object;
		object["enabled"] = policy.enabled;
		object["cleanupIntervalSeconds"] = policy.cleanupIntervalSeconds;
		object["cpu"] = {
			{ "idleSecondsThreshold", policy.cpu.idleSecondsThreshold },
			{ "memoryBudgetBytes", policy.cpu.memoryBudgetBytes },
			{ "reloadGraceSeconds", policy.cpu.reloadGraceSeconds }
		};
		object["gpu"] = {
			{ "idleSecondsThreshold", policy.gpu.idleSecondsThreshold },
			{ "memoryBudgetBytes", policy.gpu.memoryBudgetBytes },
			{ "reloadGraceSeconds", policy.gpu.reloadGraceSeconds },
			{ "strictBudgetMode", policy.gpu.strictBudgetMode }
		};
		return object;
	}

	nlohmann::json BuildStreamingPolicyJson(const TerraEngine::Resource::StreamingPolicy& policy)
	{
		nlohmann::json object;
		object["enabled"] = policy.enabled;
		object["deferScenePayloadsOnLoad"] = policy.deferScenePayloadsOnLoad;
		object["preloadDistance"] = policy.preloadDistance;
		object["visibleRetentionSeconds"] = policy.visibleRetentionSeconds;
		object["neededRetentionSeconds"] = policy.neededRetentionSeconds;
		object["maxLoadsPerUpdate"] = policy.maxLoadsPerUpdate;
		object["textureMipStreamingEnabled"] = policy.textureMipStreamingEnabled;
		object["textureMinStreamMip"] = policy.textureMinStreamMip;
		object["textureMaxStreamMip"] = policy.textureMaxStreamMip;
		object["memoryBudgetInteraction"] = std::string(TerraEngine::Resource::ToString(policy.memoryBudgetInteraction));
		return object;
	}

	int MemoryBudgetInteractionIndex(TerraEngine::Resource::MemoryBudgetInteraction interaction)
	{
		switch (interaction)
		{
		case TerraEngine::Resource::MemoryBudgetInteraction::PreferStreamingRelease:
			return 1;
		case TerraEngine::Resource::MemoryBudgetInteraction::Conservative:
			return 2;
		case TerraEngine::Resource::MemoryBudgetInteraction::UseResidencyPolicy:
			return 0;
		}
		return 0;
	}

	TerraEngine::Resource::MemoryBudgetInteraction MemoryBudgetInteractionFromIndex(int index)
	{
		switch (index)
		{
		case 1:
			return TerraEngine::Resource::MemoryBudgetInteraction::PreferStreamingRelease;
		case 2:
			return TerraEngine::Resource::MemoryBudgetInteraction::Conservative;
		default:
			return TerraEngine::Resource::MemoryBudgetInteraction::UseResidencyPolicy;
		}
	}

	void PersistResourcePoliciesToProjectManifest(const TerraEngine::Resource::ResidencyPolicy& residencyPolicy,
		const TerraEngine::Resource::StreamingPolicy& streamingPolicy)
	{
		const std::filesystem::path projectRoot = ResourceManager::Instance().GetProjectRoot();
		if (projectRoot.empty())
		{
			return;
		}

		const std::filesystem::path manifestPath = projectRoot / "game.json";
		nlohmann::json manifest = nlohmann::json::object();
		if (std::filesystem::exists(manifestPath))
		{
			std::ifstream input(manifestPath);
			if (input)
			{
				try
				{
					input >> manifest;
				}
				catch (...)
				{
					// A malformed manifest should not crash the editor window.
					// Replacing it with policy-only JSON would discard product
					// metadata, so leave the file untouched instead.
					return;
				}
			}
		}

		if (!manifest.is_object())
		{
			return;
		}

		// These policies are project/runtime settings. Persisting them beside
		// product metadata lets exports copy the same runtime contract without
		// hiding resource behavior inside a build profile.
		manifest["residencyPolicy"] = BuildResidencyPolicyJson(residencyPolicy);
		manifest["streamingPolicy"] = BuildStreamingPolicyJson(streamingPolicy);

		std::ofstream output(manifestPath);
		if (output)
		{
			output << manifest.dump(4);
		}
	}
}

ResourceResidencyWindow::ResourceResidencyWindow(MainEditorSubsystem* windowOwner)
	: EditorWindow("Resource Residency", false), owner(windowOwner)
{
}

void ResourceResidencyWindow::OnGui()
{
	if (!m_IsVisible)
	{
		ResourceManager::Instance().SetResidencyDiagnosticsEnabled(false);
		ResourceManager::Instance().SetStreamingDiagnosticsEnabled(false);
		return;
	}

	ResourceManager::Instance().SetResidencyDiagnosticsEnabled(true);
	ResourceManager::Instance().SetStreamingDiagnosticsEnabled(true);
	DrawPolicyControls();
	ImGui::Spacing();
	DrawDiagnosticsFilters();
	ImGui::Spacing();
	DrawSummary();
	ImGui::Spacing();
	DrawStreamingSummary();
	ImGui::Spacing();
	DrawCandidates();
	ImGui::Spacing();
	DrawReleaseRecords();
}

bool ResourceResidencyWindow::OnCloseRequested()
{
	ResourceManager::Instance().SetResidencyDiagnosticsEnabled(false);
	ResourceManager::Instance().SetStreamingDiagnosticsEnabled(false);
	return true;
}

void ResourceResidencyWindow::DrawDiagnosticsFilters()
{
	ImGui::SeparatorText("Diagnostics Filters");
	ImGui::InputText("Search Asset/Path", m_FilterText, sizeof(m_FilterText));
	const char* typeItems[] = { "All Types", "Meshes", "Materials", "Textures", "Shader Collections" };
	ImGui::Combo("Type", &m_TypeFilter, typeItems, IM_ARRAYSIZE(typeItems));
	const char* memoryItems[] = { "All Memory", "Not Loaded", "RAM", "VRAM", "Pending Unload" };
	ImGui::Combo("Memory", &m_MemoryFilter, memoryItems, IM_ARRAYSIZE(memoryItems));
	const char* stateItems[] = { "All States", "Deferred/Released", "RAM Resident", "VRAM Resident", "Failed" };
	ImGui::Combo("Load State", &m_LoadStateFilter, stateItems, IM_ARRAYSIZE(stateItems));
	ImGui::Checkbox("Only Release Eligible", &m_ShowOnlyReleaseEligible);
}

void ResourceResidencyWindow::DrawPolicyControls()
{
	ResourceManager& resourceManager = ResourceManager::Instance();
	TerraEngine::Resource::ResidencyPolicy residencyPolicy = resourceManager.GetResidencyPolicy();
	TerraEngine::Resource::StreamingPolicy streamingPolicy = resourceManager.GetStreamingPolicy();
	bool residencyChanged = false;
	bool streamingChanged = false;

	ImGui::SeparatorText("Policy");
	if (ImGui::Checkbox("Enable Loaded Resource Cleanup", &residencyPolicy.enabled))
	{
		residencyChanged = true;
	}

	float cleanupInterval = static_cast<float>(residencyPolicy.cleanupIntervalSeconds);
	if (ImGui::InputFloat("Cleanup Interval Seconds", &cleanupInterval, 0.25f, 1.0f, "%.2f"))
	{
		residencyPolicy.cleanupIntervalSeconds = static_cast<double>(std::max(0.01f, cleanupInterval));
		residencyChanged = true;
	}

	ImGui::SeparatorText("RAM Residency");
	float cpuIdleSeconds = static_cast<float>(residencyPolicy.cpu.idleSecondsThreshold);
	if (ImGui::InputFloat("RAM Idle Seconds", &cpuIdleSeconds, 0.5f, 1.0f, "%.2f"))
	{
		residencyPolicy.cpu.idleSecondsThreshold = static_cast<double>(std::max(0.0f, cpuIdleSeconds));
		residencyChanged = true;
	}

	const std::uint64_t cpuBudgetMb64 = residencyPolicy.cpu.memoryBudgetBytes / (1024ull * 1024ull);
	int cpuBudgetMb = static_cast<int>(std::min<std::uint64_t>(cpuBudgetMb64, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
	if (ImGui::InputInt("RAM Budget MB (0=off)", &cpuBudgetMb))
	{
		residencyPolicy.cpu.memoryBudgetBytes = static_cast<std::uint64_t>(std::max(0, cpuBudgetMb)) * 1024ull * 1024ull;
		residencyChanged = true;
	}

	float cpuReloadGrace = static_cast<float>(residencyPolicy.cpu.reloadGraceSeconds);
	if (ImGui::InputFloat("RAM Reload Grace Seconds", &cpuReloadGrace, 0.1f, 0.5f, "%.2f"))
	{
		residencyPolicy.cpu.reloadGraceSeconds = static_cast<double>(std::max(0.0f, cpuReloadGrace));
		residencyChanged = true;
	}

	ImGui::SeparatorText("VRAM Residency");
	float gpuIdleSeconds = static_cast<float>(residencyPolicy.gpu.idleSecondsThreshold);
	if (ImGui::InputFloat("VRAM Idle Seconds", &gpuIdleSeconds, 0.5f, 1.0f, "%.2f"))
	{
		residencyPolicy.gpu.idleSecondsThreshold = static_cast<double>(std::max(0.0f, gpuIdleSeconds));
		residencyChanged = true;
	}

	const std::uint64_t gpuBudgetMb64 = residencyPolicy.gpu.memoryBudgetBytes / (1024ull * 1024ull);
	int gpuBudgetMb = static_cast<int>(std::min<std::uint64_t>(gpuBudgetMb64, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
	if (ImGui::InputInt("VRAM Budget MB (0=off)", &gpuBudgetMb))
	{
		residencyPolicy.gpu.memoryBudgetBytes = static_cast<std::uint64_t>(std::max(0, gpuBudgetMb)) * 1024ull * 1024ull;
		residencyChanged = true;
	}

	float gpuReloadGrace = static_cast<float>(residencyPolicy.gpu.reloadGraceSeconds);
	if (ImGui::InputFloat("VRAM Reload Grace Seconds", &gpuReloadGrace, 0.1f, 0.5f, "%.2f"))
	{
		residencyPolicy.gpu.reloadGraceSeconds = static_cast<double>(std::max(0.0f, gpuReloadGrace));
		residencyChanged = true;
	}

	if (ImGui::Checkbox("Strict VRAM Budget Mode", &residencyPolicy.gpu.strictBudgetMode))
	{
		residencyChanged = true;
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Visibility Streaming");
	if (ImGui::Checkbox("Enable Visibility Streaming", &streamingPolicy.enabled))
	{
		streamingChanged = true;
	}

	if (ImGui::Checkbox("Defer Scene Payloads On Load", &streamingPolicy.deferScenePayloadsOnLoad))
	{
		streamingChanged = true;
	}

	float preloadDistance = static_cast<float>(streamingPolicy.preloadDistance);
	if (ImGui::InputFloat("Preload Distance", &preloadDistance, 1.0f, 10.0f, "%.2f"))
	{
		streamingPolicy.preloadDistance = static_cast<double>(std::max(0.0f, preloadDistance));
		streamingChanged = true;
	}

	float visibleRetention = static_cast<float>(streamingPolicy.visibleRetentionSeconds);
	if (ImGui::InputFloat("Visible Retention Seconds", &visibleRetention, 0.25f, 1.0f, "%.2f"))
	{
		streamingPolicy.visibleRetentionSeconds = static_cast<double>(std::max(0.0f, visibleRetention));
		streamingChanged = true;
	}

	float neededRetention = static_cast<float>(streamingPolicy.neededRetentionSeconds);
	if (ImGui::InputFloat("Needed Retention Seconds", &neededRetention, 0.25f, 1.0f, "%.2f"))
	{
		streamingPolicy.neededRetentionSeconds = static_cast<double>(std::max(0.0f, neededRetention));
		streamingChanged = true;
	}

	if (ImGui::Checkbox("Enable Texture Mip Streaming", &streamingPolicy.textureMipStreamingEnabled))
	{
		streamingChanged = true;
	}

	int minStreamMip = static_cast<int>(streamingPolicy.textureMinStreamMip);
	if (ImGui::InputInt("Texture Min Stream Mip (near)", &minStreamMip))
	{
		streamingPolicy.textureMinStreamMip = static_cast<std::uint32_t>(std::max(0, minStreamMip));
		streamingChanged = true;
	}

	int maxStreamMip = static_cast<int>(streamingPolicy.textureMaxStreamMip);
	if (ImGui::InputInt("Texture Max Stream Mip (far)", &maxStreamMip))
	{
		streamingPolicy.textureMaxStreamMip = static_cast<std::uint32_t>(std::max(0, maxStreamMip));
		streamingChanged = true;
	}
	ImGui::TextDisabled("Mip 0 is full resolution; larger mip indices stream lower-detail texture tails.");

	int maxLoads = static_cast<int>(streamingPolicy.maxLoadsPerUpdate);
	if (ImGui::InputInt("Max Streaming Loads (0=unlimited)", &maxLoads))
	{
		streamingPolicy.maxLoadsPerUpdate = static_cast<std::uint32_t>(std::max(0, maxLoads));
		streamingChanged = true;
	}

	const char* streamingBudgetItems[] = { "Use Residency Policy", "Prefer Streaming Release", "Conservative" };
	int streamingBudgetIndex = MemoryBudgetInteractionIndex(streamingPolicy.memoryBudgetInteraction);
	if (ImGui::Combo("Streaming Memory Interaction", &streamingBudgetIndex, streamingBudgetItems, IM_ARRAYSIZE(streamingBudgetItems)))
	{
		streamingPolicy.memoryBudgetInteraction = MemoryBudgetInteractionFromIndex(streamingBudgetIndex);
		streamingChanged = true;
	}

	if (residencyChanged)
	{
		resourceManager.SetResidencyPolicy(residencyPolicy);
	}

	if (streamingChanged)
	{
		resourceManager.SetStreamingPolicy(streamingPolicy);
	}

	if (residencyChanged || streamingChanged)
	{
		PersistResourcePoliciesToProjectManifest(resourceManager.GetResidencyPolicy(), resourceManager.GetStreamingPolicy());
	}
}

void ResourceResidencyWindow::DrawSummary()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary summary = ResourceManager::Instance().GetResidencyDiagnosticsSummary();
	const TerraEngine::Resource::ResidencyPolicy policy = ResourceManager::Instance().GetResidencyPolicy();
	ImGui::SeparatorText("Summary");
	if (ImGui::BeginTable("ResourceResidencySummary", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Resident");
		ImGui::TableSetupColumn("Active");
		ImGui::TableSetupColumn("Pinned");
		ImGui::TableSetupColumn("CPU+GPU Bytes");
		ImGui::TableSetupColumn("Policy");
		ImGui::TableSetupColumn("Reason");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%zu", summary.residentAssetCount);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu", summary.activeAssetCount);
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", summary.pinnedAssetCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%llu", static_cast<unsigned long long>(summary.estimatedResidentBytes));
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("enabled=%s diagnostics=%s", BoolText(policy.enabled), BoolText(summary.diagnosticsEnabled));
		ImGui::TableSetColumnIndex(5);
		TextStringView(TerraEngine::Resource::ToString(summary.cleanupReason));
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawStreamingSummary()
{
	const TerraEngine::Resource::StreamingDiagnosticsSummary summary = ResourceManager::Instance().GetStreamingDiagnosticsSummary();
	ImGui::SeparatorText("Streaming");
	if (ImGui::BeginTable("ResourceStreamingSummary", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Tracked");
		ImGui::TableSetupColumn("Deferred");
		ImGui::TableSetupColumn("Loading");
		ImGui::TableSetupColumn("Loaded");
		ImGui::TableSetupColumn("Protected");
		ImGui::TableSetupColumn("Eligible");
		ImGui::TableSetupColumn("Failed");
		ImGui::TableSetupColumn("Needs");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%zu", summary.trackedAssetCount);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu", summary.deferredAssetCount);
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", summary.loadingAssetCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%zu", summary.residentAssetCount);
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("%zu", summary.protectedAssetCount);
		ImGui::TableSetColumnIndex(5);
		ImGui::Text("%zu", summary.releaseEligibleCount);
		ImGui::TableSetColumnIndex(6);
		ImGui::Text("%zu", summary.failedAssetCount);
		ImGui::TableSetColumnIndex(7);
		ImGui::Text("%zu", summary.activeNeedCount);
		ImGui::EndTable();
	}

	std::vector<TerraEngine::Resource::StreamingDiagnosticRecord> records;
	records.reserve(summary.records.size());
	for (const TerraEngine::Resource::StreamingDiagnosticRecord& record : summary.records)
	{
		const std::optional<AssetType> assetType = AssetTypeFromStreamingType(record.assetType);
		std::filesystem::path sourceHint;
		if (const std::optional<TerraEngine::Resource::StreamableAssetRecord> asset = ResourceManager::Instance().GetStreamableAsset(record.assetId))
		{
			sourceHint = asset->sourceKey;
		}
		const std::string label = DiagnosticAssetLabel(record.assetId, assetType, sourceHint);
		const std::string memory = std::string(StreamingMemoryLocation(record));
		const bool releaseEligible = record.needSources.empty()
			&& record.protectionReason == TerraEngine::Resource::StreamingProtectionReason::None;
		if (!ContainsInsensitive(label, m_FilterText) && !ContainsInsensitive(sourceHint.string(), m_FilterText))
		{
			continue;
		}
		if (!MatchesTypeFilter(record.assetType, m_TypeFilter)
			|| !MatchesMemoryFilter(memory, m_MemoryFilter)
			|| !MatchesLoadStateFilter(record.payloadState, m_LoadStateFilter)
			|| (m_ShowOnlyReleaseEligible && !releaseEligible))
		{
			continue;
		}
		records.push_back(record);
	}

	if (records.empty())
	{
		ImGui::TextDisabled("No streaming diagnostic records.");
		return;
	}

	if (ImGui::BeginTable("ResourceStreamingRecords", 12, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable))
	{
		ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_DefaultSort);
		ImGui::TableSetupColumn("Type");
		ImGui::TableSetupColumn("Priority");
		ImGui::TableSetupColumn("Tier");
		ImGui::TableSetupColumn("Memory");
		ImGui::TableSetupColumn("Load State");
		ImGui::TableSetupColumn("Mips");
		ImGui::TableSetupColumn("CPU Bytes");
		ImGui::TableSetupColumn("GPU Bytes");
		ImGui::TableSetupColumn("Protection");
		ImGui::TableSetupColumn("Needs");
		ImGui::TableSetupColumn("Eligibility");
		ImGui::TableHeadersRow();
		if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0)
		{
			const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
			const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
			std::sort(records.begin(), records.end(), [&](const auto& left, const auto& right) {
				int result = 0;
				switch (spec.ColumnIndex)
				{
				case 0:
					result = DiagnosticAssetLabel(left.assetId, AssetTypeFromStreamingType(left.assetType)).compare(DiagnosticAssetLabel(right.assetId, AssetTypeFromStreamingType(right.assetType)));
					break;
				case 1:
					result = std::string(TerraEngine::Resource::ToString(left.assetType)).compare(std::string(TerraEngine::Resource::ToString(right.assetType)));
					break;
				case 2:
					result = static_cast<int>(left.assetPolicy.priorityClass) - static_cast<int>(right.assetPolicy.priorityClass);
					break;
				case 3:
					result = static_cast<int>(left.assetPolicy.residencyTier) - static_cast<int>(right.assetPolicy.residencyTier);
					break;
				case 4:
					result = std::string(StreamingMemoryLocation(left)).compare(std::string(StreamingMemoryLocation(right)));
					break;
				case 5:
					result = static_cast<int>(left.payloadState) - static_cast<int>(right.payloadState);
					break;
				case 6:
					result = static_cast<int>(left.textureResidentMipCount) - static_cast<int>(right.textureResidentMipCount);
					break;
				case 7:
					result = left.estimatedCpuBytes < right.estimatedCpuBytes ? -1 : (left.estimatedCpuBytes > right.estimatedCpuBytes ? 1 : 0);
					break;
				case 8:
					result = left.estimatedGpuBytes < right.estimatedGpuBytes ? -1 : (left.estimatedGpuBytes > right.estimatedGpuBytes ? 1 : 0);
					break;
				case 10:
					result = static_cast<int>(left.needSources.size()) - static_cast<int>(right.needSources.size());
					break;
				default:
					result = std::string(TerraEngine::Resource::ToString(left.protectionReason)).compare(std::string(TerraEngine::Resource::ToString(right.protectionReason)));
					break;
				}
				return ascending ? result < 0 : result > 0;
			});
		}
		for (const TerraEngine::Resource::StreamingDiagnosticRecord& record : records)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			std::filesystem::path sourceHint;
			if (const std::optional<TerraEngine::Resource::StreamableAssetRecord> asset = ResourceManager::Instance().GetStreamableAsset(record.assetId))
			{
				sourceHint = asset->sourceKey;
			}
			DrawAssetCell(record.assetId, AssetTypeFromStreamingType(record.assetType), sourceHint);
			ImGui::TableSetColumnIndex(1);
			TextStringView(TerraEngine::Resource::ToString(record.assetType));
			ImGui::TableSetColumnIndex(2);
			TextStringView(TerraEngine::Resource::ToString(record.assetPolicy.priorityClass));
			ImGui::TableSetColumnIndex(3);
			TextStringView(TerraEngine::Resource::ToString(record.assetPolicy.residencyTier));
			ImGui::TableSetColumnIndex(4);
			TextStringView(StreamingMemoryLocation(record));
			ImGui::TableSetColumnIndex(5);
			TextStringView(StreamingLoadStateLabel(record.payloadState));
			ImGui::TableSetColumnIndex(6);
			const std::string mipLabel = MipResidencyLabel(record);
			ImGui::TextUnformatted(mipLabel.c_str());
			ImGui::TableSetColumnIndex(7);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.estimatedCpuBytes));
			ImGui::TableSetColumnIndex(8);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.estimatedGpuBytes));
			ImGui::TableSetColumnIndex(9);
			TextStringView(TerraEngine::Resource::ToString(record.protectionReason));
			ImGui::TableSetColumnIndex(10);
			ImGui::Text("%zu", record.needSources.size());
			ImGui::TableSetColumnIndex(11);
			ImGui::TextWrapped("%s", record.releaseEligibility.c_str());
		}
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawCandidates()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary summary = ResourceManager::Instance().GetResidencyDiagnosticsSummary();
	ImGui::SeparatorText("Release Candidates");
	std::vector<TerraEngine::Resource::ReleaseCandidate> candidates;
	candidates.reserve(summary.candidates.size());
	for (const TerraEngine::Resource::ReleaseCandidate& candidate : summary.candidates)
	{
		if (!MatchesResidencyTypeFilter(candidate.assetType, m_TypeFilter))
		{
			continue;
		}
		const std::string label = DiagnosticAssetLabel(candidate.assetId, AssetTypeFromResidencyType(candidate.assetType), candidate.sourceKey);
		if (!ContainsInsensitive(label, m_FilterText) && !ContainsInsensitive(candidate.sourceKey, m_FilterText))
		{
			continue;
		}
		candidates.push_back(candidate);
	}

	if (candidates.empty())
	{
		ImGui::TextDisabled("No candidates.");
		return;
	}

	if (ImGui::BeginTable("ResourceResidencyCandidates", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable))
	{
		ImGui::TableSetupColumn("Asset");
		ImGui::TableSetupColumn("Type");
		ImGui::TableSetupColumn("Priority");
		ImGui::TableSetupColumn("Tier");
		ImGui::TableSetupColumn("Memory");
		ImGui::TableSetupColumn("Idle Seconds");
		ImGui::TableSetupColumn("Bytes");
		ImGui::TableSetupColumn("Reason");
		ImGui::TableSetupColumn("Source");
		ImGui::TableHeadersRow();
		if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0)
		{
			const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
			const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
			std::sort(candidates.begin(), candidates.end(), [&](const auto& left, const auto& right) {
				int result = 0;
				switch (spec.ColumnIndex)
				{
				case 0:
					result = DiagnosticAssetLabel(left.assetId, AssetTypeFromResidencyType(left.assetType), left.sourceKey).compare(DiagnosticAssetLabel(right.assetId, AssetTypeFromResidencyType(right.assetType), right.sourceKey));
					break;
				case 1:
					result = std::string(TerraEngine::Resource::ToString(left.assetType)).compare(std::string(TerraEngine::Resource::ToString(right.assetType)));
					break;
				case 2:
					result = static_cast<int>(left.priorityClass) - static_cast<int>(right.priorityClass);
					break;
				case 3:
					result = static_cast<int>(left.residencyTier) - static_cast<int>(right.residencyTier);
					break;
				case 4:
					result = static_cast<int>(left.releaseDomain) - static_cast<int>(right.releaseDomain);
					break;
				case 5:
					result = left.idleSeconds < right.idleSeconds ? -1 : (left.idleSeconds > right.idleSeconds ? 1 : 0);
					break;
				case 6:
					result = left.estimatedReclaimableBytes < right.estimatedReclaimableBytes ? -1 : (left.estimatedReclaimableBytes > right.estimatedReclaimableBytes ? 1 : 0);
					break;
				default:
					result = std::string(TerraEngine::Resource::ToString(left.releaseReason)).compare(std::string(TerraEngine::Resource::ToString(right.releaseReason)));
					break;
				}
				return ascending ? result < 0 : result > 0;
			});
		}
		for (const TerraEngine::Resource::ReleaseCandidate& candidate : candidates)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			DrawAssetCell(candidate.assetId, AssetTypeFromResidencyType(candidate.assetType), candidate.sourceKey);
			ImGui::TableSetColumnIndex(1);
			TextStringView(TerraEngine::Resource::ToString(candidate.assetType));
			ImGui::TableSetColumnIndex(2);
			TextStringView(TerraEngine::Resource::ToString(candidate.priorityClass));
			ImGui::TableSetColumnIndex(3);
			TextStringView(TerraEngine::Resource::ToString(candidate.residencyTier));
			ImGui::TableSetColumnIndex(4);
			TextStringView(TerraEngine::Resource::ToString(candidate.releaseDomain));
			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%.2f", candidate.idleSeconds);
			ImGui::TableSetColumnIndex(6);
			ImGui::Text("%llu", static_cast<unsigned long long>(candidate.estimatedReclaimableBytes));
			ImGui::TableSetColumnIndex(7);
			TextStringView(TerraEngine::Resource::ToString(candidate.releaseReason));
			ImGui::TableSetColumnIndex(8);
			ImGui::TextWrapped("%s", candidate.sourceKey.c_str());
		}
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawReleaseRecords()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary summary = ResourceManager::Instance().GetResidencyDiagnosticsSummary();
	ImGui::SeparatorText("Releases");
	std::vector<TerraEngine::Resource::ReleaseRecord> records;
	records.reserve(summary.records.size());
	for (const TerraEngine::Resource::ReleaseRecord& record : summary.records)
	{
		if (!MatchesResidencyTypeFilter(record.assetType, m_TypeFilter))
		{
			continue;
		}
		const std::string label = DiagnosticAssetLabel(record.assetId, AssetTypeFromResidencyType(record.assetType));
		if (!ContainsInsensitive(label, m_FilterText) && !ContainsInsensitive(record.explanation, m_FilterText))
		{
			continue;
		}
		records.push_back(record);
	}

	if (records.empty())
	{
		ImGui::TextDisabled("No release records.");
		return;
	}

	if (ImGui::BeginTable("ResourceResidencyRecords", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable))
	{
		ImGui::TableSetupColumn("Frame");
		ImGui::TableSetupColumn("Asset");
		ImGui::TableSetupColumn("Type");
		ImGui::TableSetupColumn("Memory");
		ImGui::TableSetupColumn("Result");
		ImGui::TableSetupColumn("Bytes");
		ImGui::TableSetupColumn("Explanation");
		ImGui::TableHeadersRow();
		if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0)
		{
			const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
			const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
			std::sort(records.begin(), records.end(), [&](const auto& left, const auto& right) {
				int result = 0;
				switch (spec.ColumnIndex)
				{
				case 0:
					result = left.frameId < right.frameId ? -1 : (left.frameId > right.frameId ? 1 : 0);
					break;
				case 1:
					result = DiagnosticAssetLabel(left.assetId, AssetTypeFromResidencyType(left.assetType)).compare(DiagnosticAssetLabel(right.assetId, AssetTypeFromResidencyType(right.assetType)));
					break;
				case 2:
					result = std::string(TerraEngine::Resource::ToString(left.assetType)).compare(std::string(TerraEngine::Resource::ToString(right.assetType)));
					break;
				case 3:
					result = static_cast<int>(left.releaseDomain) - static_cast<int>(right.releaseDomain);
					break;
				case 4:
					result = static_cast<int>(left.result) - static_cast<int>(right.result);
					break;
				case 5:
					result = left.bytesReclaimed < right.bytesReclaimed ? -1 : (left.bytesReclaimed > right.bytesReclaimed ? 1 : 0);
					break;
				default:
					result = left.explanation.compare(right.explanation);
					break;
				}
				return ascending ? result < 0 : result > 0;
			});
		}
		for (const TerraEngine::Resource::ReleaseRecord& record : records)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.frameId));
			ImGui::TableSetColumnIndex(1);
			DrawAssetCell(record.assetId, AssetTypeFromResidencyType(record.assetType));
			ImGui::TableSetColumnIndex(2);
			TextStringView(TerraEngine::Resource::ToString(record.assetType));
			ImGui::TableSetColumnIndex(3);
			TextStringView(TerraEngine::Resource::ToString(record.releaseDomain));
			ImGui::TableSetColumnIndex(4);
			TextStringView(TerraEngine::Resource::ToString(record.result));
			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.bytesReclaimed));
			ImGui::TableSetColumnIndex(6);
			ImGui::TextWrapped("%s", record.explanation.c_str());
		}
		ImGui::EndTable();
	}
}
