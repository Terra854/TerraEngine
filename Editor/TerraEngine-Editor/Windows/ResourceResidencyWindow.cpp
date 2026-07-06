// ResourceResidencyWindow.cpp - ImGui surface for loaded resource residency.
//
// The panel reports RAM/VRAM residency decisions made by ResourceManager. It
// deliberately avoids persistent backend disk cache concepts because this spec
// only controls live loaded assets that can leave CPU/GPU memory.

#include "ResourceResidencyWindow.h"
#include "ResourceResidencyWindowModel.h"

#include "AssetDatabase.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Resource/ResourceResidency.h"
#include "Engine/Resource/ResourceStreaming.h"
#include "Engine/Renderer/Graph/RenderDiagnostics.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <imgui.h>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr double kDiagnosticsRefreshIntervalSeconds = 0.25;

	const char* BoolText(bool value)
	{
		return value ? "yes" : "no";
	}

	void TextStringView(std::string_view value)
	{
		ImGui::TextUnformatted(value.data(), value.data() + value.size());
	}

	void DrawExactByteCell(const TerraEngine::Resource::ExactResidencyByteValue& value, std::string_view unavailableDomain)
	{
		const TerraEngine::Editor::ResourceResidencyByteDisplay display =
			TerraEngine::Editor::MakeResourceResidencyByteDisplay(value, unavailableDomain);
		ImGui::TextUnformatted(display.label.c_str());
		if (!display.tooltip.empty() && ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::TextWrapped("%s", display.tooltip.c_str());
			ImGui::EndTooltip();
		}
	}

	std::uint64_t AddExactDisplayBytes(std::uint64_t left, std::uint64_t right)
	{
		constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
		return left > max - right ? max : left + right;
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

	struct ResolvedAssetDisplay
	{
		std::string label;
		std::filesystem::path displayPath;
		bool generatedChildName = false;
	};

	bool IsUuidSourceKey(std::string_view sourceKey)
	{
		return sourceKey.size() == 32u && TerraEngine::ParseUUID(sourceKey).has_value();
	}

	std::optional<GeneratedModelPackageChildKind> GeneratedChildKindFromAssetType(std::optional<AssetType> assetType)
	{
		if (!assetType.has_value())
		{
			return std::nullopt;
		}
		switch (*assetType)
		{
		case AssetType::Mesh:
			return GeneratedModelPackageChildKind::Mesh;
		case AssetType::Material:
			return GeneratedModelPackageChildKind::Material;
		case AssetType::Texture:
			return GeneratedModelPackageChildKind::Texture;
		case AssetType::ShaderCollection:
			return std::nullopt;
		}
		return std::nullopt;
	}

	bool IsGeneratedModelPackageParentMesh(const TerraEngine::UUID& id, std::optional<AssetType> assetType)
	{
		if (!assetType.has_value() || *assetType != AssetType::Mesh)
		{
			return false;
		}

		std::vector<TerraEngine::UUID> generatedMeshIds;
		// Package parents are logical import containers. They may resolve to a
		// TEMF path for compatibility, but the resident renderable payloads are
		// owned by the generated child mesh GUIDs listed in the package sidecar.
		return AssetDatabase::Instance().TryGetGeneratedModelPackageChildIds(id, GeneratedModelPackageChildKind::Mesh, generatedMeshIds)
			&& !generatedMeshIds.empty();
	}

	bool IsGeneratedModelPackageParentRecord(const TerraEngine::Resource::StreamingDiagnosticRecord& record)
	{
		return IsGeneratedModelPackageParentMesh(record.assetId, AssetTypeFromStreamingType(record.assetType));
	}

	bool IsGeneratedModelPackageParentCandidate(const TerraEngine::Resource::ReleaseCandidate& candidate)
	{
		return IsGeneratedModelPackageParentMesh(candidate.assetId, AssetTypeFromResidencyType(candidate.assetType));
	}

	bool IsGeneratedModelPackageParentReleaseRecord(const TerraEngine::Resource::ReleaseRecord& record)
	{
		return IsGeneratedModelPackageParentMesh(record.assetId, AssetTypeFromResidencyType(record.assetType));
	}

	bool TryResolveGeneratedChildLabel(
		const TerraEngine::UUID& id,
		std::optional<AssetType> assetType,
		std::string& outName)
	{
		const std::optional<GeneratedModelPackageChildKind> childKind = GeneratedChildKindFromAssetType(assetType);
		if (childKind.has_value())
		{
			return AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(id, *childKind, outName);
		}

		const GeneratedModelPackageChildKind candidateKinds[] = {
			GeneratedModelPackageChildKind::Mesh,
			GeneratedModelPackageChildKind::Material,
			GeneratedModelPackageChildKind::Texture
		};
		for (GeneratedModelPackageChildKind candidateKind : candidateKinds)
		{
			if (AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(id, candidateKind, outName))
			{
				return true;
			}
		}
		return false;
	}

	std::filesystem::path EffectiveSourceHint(
		const TerraEngine::UUID& id,
		std::optional<AssetType> assetType,
		const std::filesystem::path& sourceHint)
	{
		ResourceManager& resources = ResourceManager::Instance();
		std::filesystem::path displayPath = sourceHint;
		const std::string sourceHintString = sourceHint.string();
		if ((displayPath.empty() || IsUuidSourceKey(sourceHintString)) && assetType.has_value())
		{
			const std::filesystem::path describedPath = resources.DescribeAssetReference(id, *assetType);
			if (!describedPath.empty())
			{
				displayPath = describedPath;
			}
		}
		return displayPath;
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

	bool ContainsInsensitiveLoweredNeedle(std::string_view haystack, std::string_view loweredNeedle)
	{
		if (loweredNeedle.empty())
		{
			return true;
		}
		return LowercaseCopy(haystack).find(loweredNeedle) != std::string::npos;
	}

	ResolvedAssetDisplay ResolveAssetDisplay(
		const TerraEngine::UUID& id,
		std::optional<AssetType> assetType,
		const std::filesystem::path& sourceHint = {})
	{
		std::string generatedChildName;
		if (TryResolveGeneratedChildLabel(id, assetType, generatedChildName))
		{
			ResolvedAssetDisplay display;
			display.label = generatedChildName;
			display.displayPath = EffectiveSourceHint(id, assetType, sourceHint);
			display.generatedChildName = true;
			return display;
		}

		const std::filesystem::path displayPath = EffectiveSourceHint(id, assetType, sourceHint);
		ResolvedAssetDisplay display;
		display.label = FriendlyAssetLabel(id, displayPath);
		display.displayPath = displayPath;
		return display;
	}

	std::string DiagnosticAssetLabel(const TerraEngine::UUID& id, std::optional<AssetType> assetType, const std::filesystem::path& sourceHint = {})
	{
		return ResolveAssetDisplay(id, assetType, sourceHint).label;
	}

	void DrawResolvedAssetCell(const TerraEngine::UUID& id, const ResolvedAssetDisplay& display)
	{
		ImGui::TextUnformatted(display.label.c_str());
		if (ImGui::IsItemHovered())
		{
			// The table is meant for quick scanning, so show a readable asset
			// name first and keep the GUID/source details available on hover.
			// Generated GLB/FBX child names come from the package sidecar, while
			// the path identifies the compiled payload shared by the parent row.
			const std::string idText = TerraEngine::ToHexString(id);
			ImGui::BeginTooltip();
			ImGui::Text("UUID: %s", idText.c_str());
			if (display.generatedChildName)
			{
				ImGui::TextWrapped("Generated child: %s", display.label.c_str());
			}
			if (!display.displayPath.empty())
			{
				ImGui::TextWrapped("Path: %s", display.displayPath.string().c_str());
			}
			ImGui::EndTooltip();
		}
	}

	void DrawAssetCell(const TerraEngine::UUID& id, std::optional<AssetType> assetType, const std::filesystem::path& sourceHint = {})
	{
		DrawResolvedAssetCell(id, ResolveAssetDisplay(id, assetType, sourceHint));
	}

	struct StreamingDisplayRecord
	{
		TerraEngine::Resource::StreamingDiagnosticRecord record;
		ResolvedAssetDisplay assetDisplay;
		std::string sourceHintText;
		std::string memory;
		std::string mipLabel;
		TerraEngine::Editor::ResourceResidencyByteDisplay cpuBytesDisplay;
		TerraEngine::Editor::ResourceResidencyByteDisplay gpuBytesDisplay;
		bool releaseEligible = false;
	};

	struct CandidateDisplayRecord
	{
		TerraEngine::Resource::ReleaseCandidate candidate;
		ResolvedAssetDisplay assetDisplay;
	};

	struct ReleaseDisplayRecord
	{
		TerraEngine::Resource::ReleaseRecord record;
		ResolvedAssetDisplay assetDisplay;
	};

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
			return memory == "RAM"
				|| memory == "RAM + VG"
				|| memory == "RAM + VRAM"
				|| memory == "RAM + VRAM + VG"
				|| memory == "RAM + shared VRAM";
		case 3:
			return memory == "VRAM"
				|| memory == "VG"
				|| memory == "RAM + VRAM"
				|| memory == "RAM + VG"
				|| memory == "VRAM + VG"
				|| memory == "RAM + VRAM + VG"
				|| memory == "shared VRAM"
				|| memory == "RAM + shared VRAM"
				|| memory == "RAM -> VRAM";
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

	struct VirtualGeometryAssetResidencySummary
	{
		std::uint64_t residentBytes = 0u;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t reclaimableBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
		std::uint64_t residentHighDetailBytes = 0u;
		std::uint64_t residentFallbackBytes = 0u;
		std::uint64_t embeddedFallbackBytes = 0u;
		std::uint64_t retiringBytes = 0u;
		std::uint64_t owningMeshFallbackBytes = 0u;
		std::uint32_t residentPages = 0u;
		std::uint32_t pinnedFallbackPages = 0u;
		std::uint32_t missingPages = 0u;
	};

	bool HasRenderableVirtualGeometryPages(const VirtualGeometryAssetResidencySummary& summary)
	{
		return summary.residentPages > 0u
			|| summary.pinnedFallbackPages > 0u
			|| summary.residentBytes > 0u
			|| summary.pinnedFallbackBytes > 0u;
	}

	std::unordered_map<TerraEngine::UUID, VirtualGeometryAssetResidencySummary> BuildVirtualGeometryResidencyByAsset(
		const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot>& snapshots)
	{
		std::unordered_map<TerraEngine::UUID, VirtualGeometryAssetResidencySummary> summaries;
		for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot : snapshots)
		{
			for (const TerraEngine::Renderer::VirtualGeometryBudgetContributor& contributor : snapshot.budgetContributors)
			{
				VirtualGeometryAssetResidencySummary& summary = summaries[contributor.assetId];
				// Multiple editor views can report the same asset in one frame.
				// Keep the largest observed values so Scene View and Game View do
				// not double-count a mesh that is visible in both.
				summary.residentBytes = std::max(summary.residentBytes, contributor.residentBytes);
				summary.requestedBytes = std::max(summary.requestedBytes, contributor.requestedBytes);
				summary.reclaimableBytes = std::max(summary.reclaimableBytes, contributor.reclaimableBytes);
				summary.pinnedFallbackBytes = std::max(summary.pinnedFallbackBytes, contributor.pinnedFallbackBytes);
				summary.residentHighDetailBytes = std::max(summary.residentHighDetailBytes, contributor.residentHighDetailBytes);
				summary.residentFallbackBytes = std::max(summary.residentFallbackBytes, contributor.residentFallbackBytes);
				summary.embeddedFallbackBytes = std::max(summary.embeddedFallbackBytes, contributor.embeddedFallbackBytes);
				summary.retiringBytes = std::max(summary.retiringBytes, contributor.retiringBytes);
				summary.owningMeshFallbackBytes = std::max(summary.owningMeshFallbackBytes, contributor.owningMeshFallbackBytes);
				summary.residentPages = std::max(summary.residentPages, contributor.residentPages);
				summary.pinnedFallbackPages = std::max(summary.pinnedFallbackPages, contributor.pinnedFallbackPages);
				summary.missingPages = std::max(summary.missingPages, contributor.missingPages);
			}
		}
		return summaries;
	}

	std::string VirtualGeometryPageResidencyLabel(const VirtualGeometryAssetResidencySummary* summary)
	{
		if (summary == nullptr)
		{
			return "-";
		}
		if (HasRenderableVirtualGeometryPages(*summary))
		{
			std::string label = std::to_string(summary->residentPages) + " resident";
			if (summary->pinnedFallbackPages > 0u)
			{
				label += ", ";
				label += std::to_string(summary->pinnedFallbackPages);
				label += " pinned";
			}
			if (summary->missingPages > 0u)
			{
				label += ", ";
				label += std::to_string(summary->missingPages);
				label += " missing";
			}
			return label;
		}
		if (summary->requestedBytes > 0u)
		{
			return "requested";
		}
		if (summary->missingPages > 0u)
		{
			return std::to_string(summary->missingPages) + " missing";
		}
		return "-";
	}

	std::unordered_map<TerraEngine::UUID, TerraEngine::Resource::ExactResidencyByteValue> BuildBackendExactGpuBytesByAsset(
		const TerraEngine::Renderer::BackendResourceResidencySnapshot& backendResidency)
	{
		std::unordered_map<TerraEngine::UUID, TerraEngine::Resource::ExactResidencyByteValue> byAsset;
		for (const TerraEngine::Renderer::BackendResourceResidencyRecord& record : backendResidency.records)
		{
			if (record.kind != TerraEngine::Renderer::BackendResourceResidencyKind::Mesh
				&& record.kind != TerraEngine::Renderer::BackendResourceResidencyKind::Texture)
			{
				continue;
			}

			const bool exactRecord =
				record.byteStatus == "exact"
				|| (record.byteStatus.empty() && (record.exactGpuBytes > 0u || record.gpuBytes > 0u));
			if (!exactRecord)
			{
				byAsset[record.assetId] = TerraEngine::Resource::MakeUnavailableResidencyBytes(
					record.unavailableReason.empty() ? "backend GPU allocation snapshot is unavailable" : record.unavailableReason);
				continue;
			}

			const std::uint64_t recordBytes = record.exactGpuBytes > 0u
				? record.exactGpuBytes
				: record.gpuBytes;
			TerraEngine::Resource::ExactResidencyByteValue& total = byAsset[record.assetId];
			if (total.status == TerraEngine::Resource::ExactResidencyByteStatus::Unavailable)
			{
				total = TerraEngine::Resource::MakeExactResidencyBytes(recordBytes);
			}
			else
			{
				total.status = TerraEngine::Resource::ExactResidencyByteStatus::Exact;
				total.bytes = AddExactDisplayBytes(total.bytes, recordBytes);
				total.unavailableReason.clear();
			}
		}
		return byAsset;
	}

	TerraEngine::Resource::ExactResidencyByteValue ResolveExactRenderGpuBytes(
		const TerraEngine::Resource::StreamingDiagnosticRecord& record,
		const std::unordered_map<TerraEngine::UUID, TerraEngine::Resource::ExactResidencyByteValue>& backendGpuBytesByAsset)
	{
		if (const auto backendIt = backendGpuBytesByAsset.find(record.assetId); backendIt != backendGpuBytesByAsset.end())
		{
			return backendIt->second;
		}
		return record.exactGpuBytes;
	}

	bool OwnsStreamingResidentMemory(TerraEngine::Resource::StreamablePayloadState state)
	{
		return state == TerraEngine::Resource::StreamablePayloadState::RamResident
			|| state == TerraEngine::Resource::StreamablePayloadState::VramResident;
	}

	TerraEngine::Resource::StreamingDiagnosticsSummary BuildVisibleStreamingSummary(
		const TerraEngine::Resource::StreamingDiagnosticsSummary& rawSummary)
	{
		TerraEngine::Resource::StreamingDiagnosticsSummary visibleSummary;
		visibleSummary.frameId = rawSummary.frameId;
		visibleSummary.diagnosticsEnabled = rawSummary.diagnosticsEnabled;
		visibleSummary.records.reserve(rawSummary.records.size());
		for (const TerraEngine::Resource::StreamingDiagnosticRecord& record : rawSummary.records)
		{
			if (!TerraEngine::Editor::ShouldDisplayStreamingResidencyRecord(record, IsGeneratedModelPackageParentRecord(record)))
			{
				continue;
			}

			visibleSummary.trackedAssetCount++;
			switch (record.payloadState)
			{
			case TerraEngine::Resource::StreamablePayloadState::Deferred:
			case TerraEngine::Resource::StreamablePayloadState::Released:
			case TerraEngine::Resource::StreamablePayloadState::LoadingRam:
				visibleSummary.deferredAssetCount++;
				break;
			case TerraEngine::Resource::StreamablePayloadState::UploadingVram:
				visibleSummary.loadingAssetCount++;
				break;
			case TerraEngine::Resource::StreamablePayloadState::RamResident:
			case TerraEngine::Resource::StreamablePayloadState::VramResident:
				visibleSummary.residentAssetCount++;
				break;
			case TerraEngine::Resource::StreamablePayloadState::PendingRelease:
				visibleSummary.releaseEligibleCount++;
				break;
			case TerraEngine::Resource::StreamablePayloadState::Failed:
				visibleSummary.failedAssetCount++;
				break;
			case TerraEngine::Resource::StreamablePayloadState::Registered:
				break;
			}
			if (record.protectionReason != TerraEngine::Resource::StreamingProtectionReason::None)
			{
				visibleSummary.protectedAssetCount++;
			}
			visibleSummary.activeNeedCount += record.needSources.size();
			if (OwnsStreamingResidentMemory(record.payloadState))
			{
				visibleSummary.estimatedResidentBytes +=
					TerraEngine::Resource::ExactResidencyBytesOrZero(record.exactCpuBytes)
					+ TerraEngine::Resource::ExactResidencyBytesOrZero(record.exactGpuBytes);
			}
			visibleSummary.records.push_back(record);
		}
		return visibleSummary;
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
		object["streamingDistance"] = policy.streamingDistance;
		object["visibleRetentionSeconds"] = policy.visibleRetentionSeconds;
		object["neededRetentionSeconds"] = policy.neededRetentionSeconds;
		object["maxLoadsPerUpdate"] = policy.maxLoadsPerUpdate;
		object["textureMipStreamingEnabled"] = policy.textureMipStreamingEnabled;
		object["textureMinStreamMip"] = policy.textureMinStreamMip;
		object["textureMaxStreamMip"] = policy.textureMaxStreamMip;
		object["virtualGeometryPageMemoryBudgetBytes"] = policy.virtualGeometryPageMemoryBudgetBytes;
		object["virtualGeometryPageUploadBudgetBytes"] = policy.virtualGeometryPageUploadBudgetBytes;
		object["virtualGeometryPageRetirementBudgetBytes"] = policy.virtualGeometryPageRetirementBudgetBytes;
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

struct ResourceResidencyWindow::DiagnosticsCache
{
	TerraEngine::Resource::ResidencyDiagnosticsSummary residencySummary;
	TerraEngine::Resource::StreamingDiagnosticsSummary rawStreamingSummary;
	TerraEngine::Resource::StreamingDiagnosticsSummary visibleStreamingSummary;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Resource::ExactResidencyByteValue> backendGpuBytesByAsset;
	std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> virtualGeometrySnapshots;
	std::vector<StreamingDisplayRecord> streamingRows;
	std::vector<CandidateDisplayRecord> candidateRows;
	std::vector<ReleaseDisplayRecord> releaseRows;
	double lastRefreshSeconds = -1.0;
	bool valid = false;
};

ResourceResidencyWindow::ResourceResidencyWindow(MainEditorSubsystem* windowOwner)
	: EditorWindow("Resource Residency", false), owner(windowOwner), m_DiagnosticsCache(std::make_unique<DiagnosticsCache>())
{
}

ResourceResidencyWindow::~ResourceResidencyWindow() = default;

void ResourceResidencyWindow::OnGui()
{
	if (!m_IsVisible)
	{
		ResourceManager::Instance().SetResidencyDiagnosticsEnabled(false);
		ResourceManager::Instance().SetStreamingDiagnosticsEnabled(false);
		InvalidateDiagnosticsCache();
		return;
	}

	ResourceManager::Instance().SetResidencyDiagnosticsEnabled(true);
	ResourceManager::Instance().SetStreamingDiagnosticsEnabled(true);
	DrawPolicyControls();
	RefreshDiagnosticsCacheIfNeeded(false);
	ImGui::Spacing();
	DrawDiagnosticsFilters();
	ImGui::Spacing();
	DrawSummary();
	ImGui::Spacing();
	DrawStreamingSummary();
	ImGui::Spacing();
	DrawVirtualGeometrySummary();
	ImGui::Spacing();
	DrawCandidates();
	ImGui::Spacing();
	DrawReleaseRecords();
}

bool ResourceResidencyWindow::OnCloseRequested()
{
	ResourceManager::Instance().SetResidencyDiagnosticsEnabled(false);
	ResourceManager::Instance().SetStreamingDiagnosticsEnabled(false);
	InvalidateDiagnosticsCache();
	return true;
}

void ResourceResidencyWindow::InvalidateDiagnosticsCache()
{
	if (!m_DiagnosticsCache)
	{
		return;
	}
	m_DiagnosticsCache->valid = false;
	m_DiagnosticsCache->lastRefreshSeconds = -1.0;
	m_DiagnosticsCache->streamingRows.clear();
	m_DiagnosticsCache->candidateRows.clear();
	m_DiagnosticsCache->releaseRows.clear();
	m_DiagnosticsCache->backendGpuBytesByAsset.clear();
	m_DiagnosticsCache->virtualGeometrySnapshots.clear();
}

void ResourceResidencyWindow::RefreshDiagnosticsCacheIfNeeded(bool forceRefresh)
{
	if (!m_DiagnosticsCache)
	{
		return;
	}

	const double nowSeconds = ImGui::GetTime();
	if (!forceRefresh
		&& m_DiagnosticsCache->valid
		&& nowSeconds - m_DiagnosticsCache->lastRefreshSeconds < kDiagnosticsRefreshIntervalSeconds)
	{
		return;
	}

	ResourceManager& resourceManager = ResourceManager::Instance();
	DiagnosticsCache& cache = *m_DiagnosticsCache;
	cache.residencySummary = resourceManager.GetResidencyDiagnosticsSummary();
	cache.rawStreamingSummary = resourceManager.GetStreamingDiagnosticsSummary();
	cache.visibleStreamingSummary = cache.rawStreamingSummary.records.empty()
		? cache.rawStreamingSummary
		: BuildVisibleStreamingSummary(cache.rawStreamingSummary);
	cache.backendGpuBytesByAsset =
		BuildBackendExactGpuBytesByAsset(TerraEngine::Renderer::LatestBackendResourceResidencySnapshot());
	cache.virtualGeometrySnapshots = TerraEngine::Renderer::LatestVirtualGeometryDiagnosticsSnapshots();

	cache.streamingRows.clear();
	cache.streamingRows.reserve(cache.visibleStreamingSummary.records.size());
	for (const TerraEngine::Resource::StreamingDiagnosticRecord& record : cache.visibleStreamingSummary.records)
	{
		if (!TerraEngine::Editor::ShouldDisplayStreamingResidencyRecord(record, IsGeneratedModelPackageParentRecord(record)))
		{
			continue;
		}

		StreamingDisplayRecord row;
		row.record = record;
		row.record.exactGpuBytes = ResolveExactRenderGpuBytes(record, cache.backendGpuBytesByAsset);
		const std::optional<AssetType> assetType = AssetTypeFromStreamingType(record.assetType);
		const std::filesystem::path sourceHint(record.sourceKey);
		row.assetDisplay = ResolveAssetDisplay(record.assetId, assetType, sourceHint);
		row.sourceHintText = sourceHint.string();
		row.memory = TerraEngine::Editor::ExactStreamingMemoryLocation(row.record);
		row.mipLabel = MipResidencyLabel(row.record);
		row.cpuBytesDisplay = TerraEngine::Editor::MakeResourceResidencyByteDisplay(row.record.exactCpuBytes, "Stream CPU Bytes");
		row.gpuBytesDisplay = TerraEngine::Editor::MakeResourceResidencyByteDisplay(row.record.exactGpuBytes, "Render GPU Bytes");
		row.releaseEligible = record.needSources.empty()
			&& record.protectionReason == TerraEngine::Resource::StreamingProtectionReason::None;
		cache.streamingRows.push_back(std::move(row));
	}

	cache.candidateRows.clear();
	cache.candidateRows.reserve(cache.residencySummary.candidates.size());
	for (const TerraEngine::Resource::ReleaseCandidate& candidate : cache.residencySummary.candidates)
	{
		if (IsGeneratedModelPackageParentCandidate(candidate))
		{
			continue;
		}
		CandidateDisplayRecord row;
		row.candidate = candidate;
		row.assetDisplay = ResolveAssetDisplay(candidate.assetId, AssetTypeFromResidencyType(candidate.assetType), candidate.sourceKey);
		cache.candidateRows.push_back(std::move(row));
	}

	cache.releaseRows.clear();
	cache.releaseRows.reserve(cache.residencySummary.records.size());
	for (const TerraEngine::Resource::ReleaseRecord& record : cache.residencySummary.records)
	{
		if (IsGeneratedModelPackageParentReleaseRecord(record))
		{
			continue;
		}
		ReleaseDisplayRecord row;
		row.record = record;
		row.assetDisplay = ResolveAssetDisplay(record.assetId, AssetTypeFromResidencyType(record.assetType));
		cache.releaseRows.push_back(std::move(row));
	}

	// The Resource Residency window is a diagnostic dashboard, so a quarter
	// second cache keeps the values live enough for humans while preventing the
	// asset database and render backend from being queried dozens of times per
	// second just because ImGui repaints.
	cache.lastRefreshSeconds = nowSeconds;
	cache.valid = true;
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

	float streamingDistance = static_cast<float>(streamingPolicy.streamingDistance);
	if (ImGui::InputFloat("Streaming Distance", &streamingDistance, 1.0f, 10.0f, "%.2f"))
	{
		streamingPolicy.streamingDistance = static_cast<double>(std::max(0.0f, streamingDistance));
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

		ImGui::SeparatorText("Virtual Geometry Page Budgets");
		const auto bytesToMbInput = [](std::uint64_t bytes) {
			const std::uint64_t mb = bytes / (1024ull * 1024ull);
			return static_cast<int>(std::min<std::uint64_t>(mb, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
		};
		int vgPageBudgetMb = bytesToMbInput(streamingPolicy.virtualGeometryPageMemoryBudgetBytes);
		if (ImGui::InputInt("VG Page Memory Budget MB (0=off)", &vgPageBudgetMb))
		{
			streamingPolicy.virtualGeometryPageMemoryBudgetBytes = static_cast<std::uint64_t>(std::max(0, vgPageBudgetMb)) * 1024ull * 1024ull;
			streamingChanged = true;
		}
		int vgUploadBudgetMb = bytesToMbInput(streamingPolicy.virtualGeometryPageUploadBudgetBytes);
		if (ImGui::InputInt("VG Page Upload Budget MB/frame (0=off)", &vgUploadBudgetMb))
		{
			streamingPolicy.virtualGeometryPageUploadBudgetBytes = static_cast<std::uint64_t>(std::max(0, vgUploadBudgetMb)) * 1024ull * 1024ull;
			streamingChanged = true;
		}
		int vgRetirementBudgetMb = bytesToMbInput(streamingPolicy.virtualGeometryPageRetirementBudgetBytes);
		if (ImGui::InputInt("VG Page Retirement Budget MB/frame (0=off)", &vgRetirementBudgetMb))
		{
			streamingPolicy.virtualGeometryPageRetirementBudgetBytes = static_cast<std::uint64_t>(std::max(0, vgRetirementBudgetMb)) * 1024ull * 1024ull;
			streamingChanged = true;
		}
		ImGui::TextDisabled("Fallback pages stay protected; page pressure targets high-detail pages first.");

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
		InvalidateDiagnosticsCache();
	}
}

void ResourceResidencyWindow::DrawSummary()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary& summary = m_DiagnosticsCache->residencySummary;
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
	const TerraEngine::Resource::StreamingDiagnosticsSummary& summary = m_DiagnosticsCache->visibleStreamingSummary;
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

	const std::string loweredFilter = LowercaseCopy(m_FilterText);
	std::vector<const StreamingDisplayRecord*> records;
	records.reserve(m_DiagnosticsCache->streamingRows.size());
	for (const StreamingDisplayRecord& row : m_DiagnosticsCache->streamingRows)
	{
		const TerraEngine::Resource::StreamingDiagnosticRecord& record = row.record;
		if (!ContainsInsensitiveLoweredNeedle(row.assetDisplay.label, loweredFilter)
			&& !ContainsInsensitiveLoweredNeedle(row.sourceHintText, loweredFilter))
		{
			continue;
		}
		if (!MatchesTypeFilter(record.assetType, m_TypeFilter)
			|| !MatchesMemoryFilter(row.memory, m_MemoryFilter)
			|| !MatchesLoadStateFilter(record.payloadState, m_LoadStateFilter)
			|| (m_ShowOnlyReleaseEligible && !row.releaseEligible))
		{
			continue;
		}
		records.push_back(&row);
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
		ImGui::TableSetupColumn("Stream Memory");
		ImGui::TableSetupColumn("Load State");
		ImGui::TableSetupColumn("Mips");
		ImGui::TableSetupColumn("Stream CPU Bytes");
		ImGui::TableSetupColumn("Render GPU Bytes");
		ImGui::TableSetupColumn("Protection");
		ImGui::TableSetupColumn("Needs");
		ImGui::TableSetupColumn("Eligibility");
		ImGui::TableHeadersRow();
		if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0)
		{
			const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
			const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
			std::sort(records.begin(), records.end(), [&](const StreamingDisplayRecord* leftRow, const StreamingDisplayRecord* rightRow) {
				const TerraEngine::Resource::StreamingDiagnosticRecord& left = leftRow->record;
				const TerraEngine::Resource::StreamingDiagnosticRecord& right = rightRow->record;
				int result = 0;
				switch (spec.ColumnIndex)
				{
				case 0:
					result = leftRow->assetDisplay.label.compare(rightRow->assetDisplay.label);
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
				{
					result = leftRow->memory.compare(rightRow->memory);
					break;
				}
				case 5:
					result = static_cast<int>(left.payloadState) - static_cast<int>(right.payloadState);
					break;
				case 6:
					result = static_cast<int>(left.textureResidentMipCount) - static_cast<int>(right.textureResidentMipCount);
					break;
				case 7:
				{
					const std::uint64_t leftBytes = leftRow->cpuBytesDisplay.sortKey;
					const std::uint64_t rightBytes = rightRow->cpuBytesDisplay.sortKey;
					result = leftBytes < rightBytes ? -1 : (leftBytes > rightBytes ? 1 : 0);
					break;
				}
				case 8:
				{
					const std::uint64_t leftBytes = leftRow->gpuBytesDisplay.sortKey;
					const std::uint64_t rightBytes = rightRow->gpuBytesDisplay.sortKey;
					result = leftBytes < rightBytes ? -1 : (leftBytes > rightBytes ? 1 : 0);
					break;
				}
				case 10:
					result = static_cast<int>(left.needSources.size()) - static_cast<int>(right.needSources.size());
					break;
				case 11:
					result = left.releaseEligibility.compare(right.releaseEligibility);
					break;
				default:
					result = std::string(TerraEngine::Resource::ToString(left.protectionReason)).compare(std::string(TerraEngine::Resource::ToString(right.protectionReason)));
					break;
				}
				return ascending ? result < 0 : result > 0;
			});
		}

		// Diagnostic projects can contain many generated GLB/FBX children.
		// Submit only the visible table rows so hovering or scrolling this
		// window does not format every asset every editor frame.
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(records.size()));
		while (clipper.Step())
		{
			for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
			{
				const StreamingDisplayRecord& row = *records[static_cast<std::size_t>(rowIndex)];
				const TerraEngine::Resource::StreamingDiagnosticRecord& record = row.record;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				DrawResolvedAssetCell(record.assetId, row.assetDisplay);
				ImGui::TableSetColumnIndex(1);
				TextStringView(TerraEngine::Resource::ToString(record.assetType));
				ImGui::TableSetColumnIndex(2);
				TextStringView(TerraEngine::Resource::ToString(record.assetPolicy.priorityClass));
				ImGui::TableSetColumnIndex(3);
				TextStringView(TerraEngine::Resource::ToString(record.assetPolicy.residencyTier));
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(row.memory.c_str());
				ImGui::TableSetColumnIndex(5);
				TextStringView(StreamingLoadStateLabel(record.payloadState));
				ImGui::TableSetColumnIndex(6);
				ImGui::TextUnformatted(row.mipLabel.c_str());
				ImGui::TableSetColumnIndex(7);
				DrawExactByteCell(record.exactCpuBytes, "Stream CPU Bytes");
				ImGui::TableSetColumnIndex(8);
				DrawExactByteCell(record.exactGpuBytes, "Render GPU Bytes");
				ImGui::TableSetColumnIndex(9);
				TextStringView(TerraEngine::Resource::ToString(record.protectionReason));
				ImGui::TableSetColumnIndex(10);
				ImGui::Text("%zu", record.needSources.size());
				ImGui::TableSetColumnIndex(11);
				ImGui::TextUnformatted(record.releaseEligibility.c_str());
				if (!record.releaseEligibility.empty() && ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::TextWrapped("%s", record.releaseEligibility.c_str());
					ImGui::EndTooltip();
				}
			}
		}
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawVirtualGeometrySummary()
{
	const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot>& snapshots =
		m_DiagnosticsCache->virtualGeometrySnapshots;
	ImGui::SeparatorText("Virtual Geometry");
	if (snapshots.empty())
	{
		ImGui::TextDisabled("No virtual geometry diagnostics published.");
		return;
	}

	if (ImGui::BeginTable("ResourceVirtualGeometrySummary", 14, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("View");
		ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Resident");
		ImGui::TableSetupColumn("Requested");
		ImGui::TableSetupColumn("IO/Upload");
		ImGui::TableSetupColumn("Missing");
		ImGui::TableSetupColumn("Failed");
		ImGui::TableSetupColumn("Evict/Evicted");
		ImGui::TableSetupColumn("Pinned");
		ImGui::TableSetupColumn("Pinned Page Bytes");
		ImGui::TableSetupColumn("Page Bytes H/F/E");
		ImGui::TableSetupColumn("Pending/Fail/Evict/Retire Bytes");
		ImGui::TableSetupColumn("Owner Fallback Bytes");
		ImGui::TableSetupColumn("Budget/Pressure Bytes");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot : snapshots)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", ToString(snapshot.viewType));
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%s", snapshot.active ? "active" : ToString(snapshot.inactiveReason));
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%u", snapshot.residentPages);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%u", snapshot.requestedPages);
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%u/%u", snapshot.pageResidency.pendingIoPages, snapshot.pageResidency.pendingUploadPages);
			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%u", snapshot.missingPages);
			ImGui::TableSetColumnIndex(6);
			ImGui::Text("%u", snapshot.failedPages);
			ImGui::TableSetColumnIndex(7);
			ImGui::Text("%u/%u", snapshot.evictingPages, snapshot.evictedPages);
			ImGui::TableSetColumnIndex(8);
			ImGui::Text("%u", snapshot.pinnedFallbackPages);
			ImGui::TableSetColumnIndex(9);
			ImGui::Text("%llu", static_cast<unsigned long long>(snapshot.pageResidency.pinnedFallbackBytes));
			ImGui::TableSetColumnIndex(10);
			ImGui::Text("%llu/%llu/%llu",
				static_cast<unsigned long long>(snapshot.pageResidency.residentHighDetailBytes),
				static_cast<unsigned long long>(snapshot.pageResidency.residentFallbackBytes),
				static_cast<unsigned long long>(snapshot.pageResidency.embeddedFallbackBytes));
			ImGui::TableSetColumnIndex(11);
			ImGui::Text("%llu/%llu/%llu/%llu",
				static_cast<unsigned long long>(snapshot.pageResidency.pendingIoBytes + snapshot.pageResidency.pendingUploadBytes),
				static_cast<unsigned long long>(snapshot.pageResidency.failedBytes),
				static_cast<unsigned long long>(snapshot.pageResidency.evictingBytes + snapshot.pageResidency.evictedBytes),
				static_cast<unsigned long long>(snapshot.pageResidency.retiringBytes));
			ImGui::TableSetColumnIndex(12);
			ImGui::Text("%llu", static_cast<unsigned long long>(snapshot.pageResidency.owningMeshFallbackBytes));
			ImGui::TableSetColumnIndex(13);
			ImGui::Text("%llu/%llu",
				static_cast<unsigned long long>(snapshot.geometryBudgetBytes),
				static_cast<unsigned long long>(snapshot.budgetPressureBytes));
		}
		ImGui::EndTable();
	}

	std::uint32_t protectedFallbackPages = 0u;
	std::uint64_t protectedFallbackBytes = 0u;
	for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot : snapshots)
	{
		protectedFallbackPages += snapshot.pinnedFallbackPages;
		protectedFallbackBytes += snapshot.pageResidency.pinnedFallbackBytes;
	}
	if (protectedFallbackPages > 0u)
	{
		ImGui::Text("Fallback protection: %u pinned pages, %llu bytes kept renderable",
			protectedFallbackPages,
			static_cast<unsigned long long>(protectedFallbackBytes));
	}

	const std::unordered_map<TerraEngine::UUID, VirtualGeometryAssetResidencySummary> virtualGeometryByAsset =
		BuildVirtualGeometryResidencyByAsset(snapshots);
	std::vector<std::pair<TerraEngine::UUID, VirtualGeometryAssetResidencySummary>> assetSummaries;
	assetSummaries.reserve(virtualGeometryByAsset.size());
	for (const auto& [assetId, summary] : virtualGeometryByAsset)
	{
		assetSummaries.emplace_back(assetId, summary);
	}
	std::sort(assetSummaries.begin(), assetSummaries.end(), [](const auto& left, const auto& right) {
		if (left.second.reclaimableBytes != right.second.reclaimableBytes)
		{
			return left.second.reclaimableBytes > right.second.reclaimableBytes;
		}
		if (left.second.residentBytes != right.second.residentBytes)
		{
			return left.second.residentBytes > right.second.residentBytes;
		}
		return TerraEngine::ToHexString(left.first) < TerraEngine::ToHexString(right.first);
	});

	if (!assetSummaries.empty()
	&& ImGui::BeginTable("ResourceVirtualGeometryPressureAssets", 11, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Mesh");
		ImGui::TableSetupColumn("VG Page GPU Bytes");
		ImGui::TableSetupColumn("High Detail Page Bytes");
		ImGui::TableSetupColumn("Fallback Page Bytes");
		ImGui::TableSetupColumn("Embedded/Owner Fallback Bytes");
		ImGui::TableSetupColumn("Requested Page Bytes");
		ImGui::TableSetupColumn("Reclaimable Page Bytes");
		ImGui::TableSetupColumn("Pinned Fallback Bytes");
		ImGui::TableSetupColumn("Retiring Page Bytes");
		ImGui::TableSetupColumn("VG Pages");
		ImGui::TableSetupColumn("Missing Pages");
		ImGui::TableHeadersRow();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(assetSummaries.size()));
		while (clipper.Step())
		{
			for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
			{
				const auto& [assetId, summary] = assetSummaries[static_cast<std::size_t>(rowIndex)];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				DrawAssetCell(assetId, AssetType::Mesh);
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%llu", static_cast<unsigned long long>(summary.residentBytes));
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%llu", static_cast<unsigned long long>(summary.residentHighDetailBytes));
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%llu", static_cast<unsigned long long>(summary.residentFallbackBytes));
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%llu/%llu",
					static_cast<unsigned long long>(summary.embeddedFallbackBytes),
					static_cast<unsigned long long>(summary.owningMeshFallbackBytes));
				ImGui::TableSetColumnIndex(5);
				ImGui::Text("%llu", static_cast<unsigned long long>(summary.requestedBytes));
				ImGui::TableSetColumnIndex(6);
				ImGui::Text("%llu", static_cast<unsigned long long>(summary.reclaimableBytes));
				ImGui::TableSetColumnIndex(7);
				ImGui::Text("%llu", static_cast<unsigned long long>(summary.pinnedFallbackBytes));
				ImGui::TableSetColumnIndex(8);
				ImGui::Text("%llu", static_cast<unsigned long long>(summary.retiringBytes));
				ImGui::TableSetColumnIndex(9);
				const std::string pageLabel = VirtualGeometryPageResidencyLabel(&summary);
				ImGui::TextUnformatted(pageLabel.c_str());
				ImGui::TableSetColumnIndex(10);
				ImGui::Text("%u", summary.missingPages);
			}
		}
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawCandidates()
{
	ImGui::SeparatorText("Release Candidates");
	const std::string loweredFilter = LowercaseCopy(m_FilterText);
	std::vector<const CandidateDisplayRecord*> candidates;
	candidates.reserve(m_DiagnosticsCache->candidateRows.size());
	for (const CandidateDisplayRecord& row : m_DiagnosticsCache->candidateRows)
	{
		const TerraEngine::Resource::ReleaseCandidate& candidate = row.candidate;
		if (!MatchesResidencyTypeFilter(candidate.assetType, m_TypeFilter))
		{
			continue;
		}
		if (!ContainsInsensitiveLoweredNeedle(row.assetDisplay.label, loweredFilter)
			&& !ContainsInsensitiveLoweredNeedle(candidate.sourceKey, loweredFilter))
		{
			continue;
		}
		candidates.push_back(&row);
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
			std::sort(candidates.begin(), candidates.end(), [&](const CandidateDisplayRecord* leftRow, const CandidateDisplayRecord* rightRow) {
				const TerraEngine::Resource::ReleaseCandidate& left = leftRow->candidate;
				const TerraEngine::Resource::ReleaseCandidate& right = rightRow->candidate;
				int result = 0;
				switch (spec.ColumnIndex)
				{
				case 0:
					result = leftRow->assetDisplay.label.compare(rightRow->assetDisplay.label);
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

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(candidates.size()));
		while (clipper.Step())
		{
			for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
			{
				const CandidateDisplayRecord& row = *candidates[static_cast<std::size_t>(rowIndex)];
				const TerraEngine::Resource::ReleaseCandidate& candidate = row.candidate;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				DrawResolvedAssetCell(candidate.assetId, row.assetDisplay);
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
				ImGui::TextUnformatted(candidate.sourceKey.c_str());
			}
		}
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawReleaseRecords()
{
	ImGui::SeparatorText("Releases");
	const std::string loweredFilter = LowercaseCopy(m_FilterText);
	std::vector<const ReleaseDisplayRecord*> records;
	records.reserve(m_DiagnosticsCache->releaseRows.size());
	for (const ReleaseDisplayRecord& row : m_DiagnosticsCache->releaseRows)
	{
		const TerraEngine::Resource::ReleaseRecord& record = row.record;
		if (!MatchesResidencyTypeFilter(record.assetType, m_TypeFilter))
		{
			continue;
		}
		if (!ContainsInsensitiveLoweredNeedle(row.assetDisplay.label, loweredFilter)
			&& !ContainsInsensitiveLoweredNeedle(record.explanation, loweredFilter))
		{
			continue;
		}
		records.push_back(&row);
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
			std::sort(records.begin(), records.end(), [&](const ReleaseDisplayRecord* leftRow, const ReleaseDisplayRecord* rightRow) {
				const TerraEngine::Resource::ReleaseRecord& left = leftRow->record;
				const TerraEngine::Resource::ReleaseRecord& right = rightRow->record;
				int result = 0;
				switch (spec.ColumnIndex)
				{
				case 0:
					result = left.frameId < right.frameId ? -1 : (left.frameId > right.frameId ? 1 : 0);
					break;
				case 1:
					result = leftRow->assetDisplay.label.compare(rightRow->assetDisplay.label);
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

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(records.size()));
		while (clipper.Step())
		{
			for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
			{
				const ReleaseDisplayRecord& row = *records[static_cast<std::size_t>(rowIndex)];
				const TerraEngine::Resource::ReleaseRecord& record = row.record;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%llu", static_cast<unsigned long long>(record.frameId));
				ImGui::TableSetColumnIndex(1);
				DrawResolvedAssetCell(record.assetId, row.assetDisplay);
				ImGui::TableSetColumnIndex(2);
				TextStringView(TerraEngine::Resource::ToString(record.assetType));
				ImGui::TableSetColumnIndex(3);
				TextStringView(TerraEngine::Resource::ToString(record.releaseDomain));
				ImGui::TableSetColumnIndex(4);
				TextStringView(TerraEngine::Resource::ToString(record.result));
				ImGui::TableSetColumnIndex(5);
				ImGui::Text("%llu", static_cast<unsigned long long>(record.bytesReclaimed));
				ImGui::TableSetColumnIndex(6);
				ImGui::TextUnformatted(record.explanation.c_str());
				if (!record.explanation.empty() && ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::TextWrapped("%s", record.explanation.c_str());
					ImGui::EndTooltip();
				}
			}
		}
		ImGui::EndTable();
	}
}
