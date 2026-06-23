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
#include <fstream>
#include <filesystem>
#include <imgui.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>

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

	nlohmann::json BuildResidencyPolicyJson(const TerraEngine::Resource::ResidencyPolicy& policy)
	{
		nlohmann::json object;
		object["enabled"] = policy.enabled;
		object["idleSecondsThreshold"] = policy.idleSecondsThreshold;
		object["memoryBudgetBytes"] = policy.memoryBudgetBytes;
		object["cleanupIntervalSeconds"] = policy.cleanupIntervalSeconds;
		object["reloadGraceSeconds"] = policy.reloadGraceSeconds;
		object["strictBudgetMode"] = policy.strictBudgetMode;
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

	float idleSeconds = static_cast<float>(residencyPolicy.idleSecondsThreshold);
	if (ImGui::InputFloat("Idle Seconds", &idleSeconds, 0.5f, 1.0f, "%.2f"))
	{
		residencyPolicy.idleSecondsThreshold = static_cast<double>(std::max(0.0f, idleSeconds));
		residencyChanged = true;
	}

	const std::uint64_t memoryBudgetMb64 = residencyPolicy.memoryBudgetBytes / (1024ull * 1024ull);
	int memoryBudgetMb = static_cast<int>(std::min<std::uint64_t>(memoryBudgetMb64, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
	if (ImGui::InputInt("Memory Budget MB (0=off)", &memoryBudgetMb))
	{
		residencyPolicy.memoryBudgetBytes = static_cast<std::uint64_t>(std::max(0, memoryBudgetMb)) * 1024ull * 1024ull;
		residencyChanged = true;
	}

	float cleanupInterval = static_cast<float>(residencyPolicy.cleanupIntervalSeconds);
	if (ImGui::InputFloat("Cleanup Interval Seconds", &cleanupInterval, 0.25f, 1.0f, "%.2f"))
	{
		residencyPolicy.cleanupIntervalSeconds = static_cast<double>(std::max(0.01f, cleanupInterval));
		residencyChanged = true;
	}

	float reloadGrace = static_cast<float>(residencyPolicy.reloadGraceSeconds);
	if (ImGui::InputFloat("Reload Grace Seconds", &reloadGrace, 0.1f, 0.5f, "%.2f"))
	{
		residencyPolicy.reloadGraceSeconds = static_cast<double>(std::max(0.0f, reloadGrace));
		residencyChanged = true;
	}

	if (ImGui::Checkbox("Strict Budget Mode", &residencyPolicy.strictBudgetMode))
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

	if (summary.records.empty())
	{
		ImGui::TextDisabled("No streaming diagnostic records.");
		return;
	}

	if (ImGui::BeginTable("ResourceStreamingRecords", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Asset");
		ImGui::TableSetupColumn("Type");
		ImGui::TableSetupColumn("Memory");
		ImGui::TableSetupColumn("Load State");
		ImGui::TableSetupColumn("CPU Bytes");
		ImGui::TableSetupColumn("GPU Bytes");
		ImGui::TableSetupColumn("Protection");
		ImGui::TableSetupColumn("Needs");
		ImGui::TableSetupColumn("Eligibility");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Resource::StreamingDiagnosticRecord& record : summary.records)
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
			TextStringView(StreamingMemoryLocation(record));
			ImGui::TableSetColumnIndex(3);
			TextStringView(StreamingLoadStateLabel(record.payloadState));
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.estimatedCpuBytes));
			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.estimatedGpuBytes));
			ImGui::TableSetColumnIndex(6);
			TextStringView(TerraEngine::Resource::ToString(record.protectionReason));
			ImGui::TableSetColumnIndex(7);
			ImGui::Text("%zu", record.needSources.size());
			ImGui::TableSetColumnIndex(8);
			ImGui::TextWrapped("%s", record.releaseEligibility.c_str());
		}
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawCandidates()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary summary = ResourceManager::Instance().GetResidencyDiagnosticsSummary();
	ImGui::SeparatorText("Release Candidates");
	if (summary.candidates.empty())
	{
		ImGui::TextDisabled("No candidates.");
		return;
	}

	if (ImGui::BeginTable("ResourceResidencyCandidates", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Asset");
		ImGui::TableSetupColumn("Type");
		ImGui::TableSetupColumn("Idle Seconds");
		ImGui::TableSetupColumn("Bytes");
		ImGui::TableSetupColumn("Reason");
		ImGui::TableSetupColumn("Source");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Resource::ReleaseCandidate& candidate : summary.candidates)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			DrawAssetCell(candidate.assetId, AssetTypeFromResidencyType(candidate.assetType), candidate.sourceKey);
			ImGui::TableSetColumnIndex(1);
			TextStringView(TerraEngine::Resource::ToString(candidate.assetType));
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%.2f", candidate.idleSeconds);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%llu", static_cast<unsigned long long>(candidate.estimatedReclaimableBytes));
			ImGui::TableSetColumnIndex(4);
			TextStringView(TerraEngine::Resource::ToString(candidate.releaseReason));
			ImGui::TableSetColumnIndex(5);
			ImGui::TextWrapped("%s", candidate.sourceKey.c_str());
		}
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawReleaseRecords()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary summary = ResourceManager::Instance().GetResidencyDiagnosticsSummary();
	ImGui::SeparatorText("Releases");
	if (summary.records.empty())
	{
		ImGui::TextDisabled("No release records.");
		return;
	}

	if (ImGui::BeginTable("ResourceResidencyRecords", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Frame");
		ImGui::TableSetupColumn("Asset");
		ImGui::TableSetupColumn("Type");
		ImGui::TableSetupColumn("Result");
		ImGui::TableSetupColumn("Bytes");
		ImGui::TableSetupColumn("Explanation");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Resource::ReleaseRecord& record : summary.records)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.frameId));
			ImGui::TableSetColumnIndex(1);
			DrawAssetCell(record.assetId, AssetTypeFromResidencyType(record.assetType));
			ImGui::TableSetColumnIndex(2);
			TextStringView(TerraEngine::Resource::ToString(record.assetType));
			ImGui::TableSetColumnIndex(3);
			TextStringView(TerraEngine::Resource::ToString(record.result));
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.bytesReclaimed));
			ImGui::TableSetColumnIndex(5);
			ImGui::TextWrapped("%s", record.explanation.c_str());
		}
		ImGui::EndTable();
	}
}
