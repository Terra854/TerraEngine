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
#include "Engine/Scene/WorldStreamingSystem.h"
#include "Engine/Renderer/Graph/RenderDiagnostics.h"
#include "Engine/Renderer/RenderSystem.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <imgui.h>
#include <limits>
#include <memory>
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
			// These totals originate from the protected render publication, not the
			// cooked page catalog. Calling them published avoids implying that every
			// page listed in a TEMF is physically usable by this frame.
			std::string label = std::to_string(summary->residentPages) + " published";
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
	: EditorWindow("Resource Residency", false, "Rendering"), owner(windowOwner), m_DiagnosticsCache(std::make_unique<DiagnosticsCache>())
{
}

ResourceResidencyWindow::~ResourceResidencyWindow() = default;

void ResourceResidencyWindow::OnGui()
{
	if (!m_IsVisible)
	{
		ResourceManager::Instance().SetResidencyDiagnosticsEnabled(false);
		ResourceManager::Instance().SetStreamingDiagnosticsEnabled(false);
		RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(false);
		InvalidateDiagnosticsCache();
		return;
	}

	ResourceManager::Instance().SetResidencyDiagnosticsEnabled(true);
	ResourceManager::Instance().SetStreamingDiagnosticsEnabled(true);
	RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(true);
	RefreshDiagnosticsCacheIfNeeded(false);
	ImGui::Spacing();
	DrawDiagnosticsFilters();
	ImGui::Spacing();
	DrawRendererRecoverySummary();
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
	RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(false);
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

void ResourceResidencyWindow::DrawRendererRecoverySummary()
{
	const TerraEngine::Renderer::RenderDiagnosticsSummary renderDiagnostics = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
	const TerraEngine::Renderer::RendererRecoverySnapshot& recovery = renderDiagnostics.rendererRecovery;
	if (!TerraEngine::Renderer::IsRendererRecoveryActive(recovery.state)
		&& recovery.finalOutcome == TerraEngine::Renderer::RendererRecoveryOutcome::None)
	{
		return;
	}

	ImGui::SeparatorText("Renderer Recovery");
	if (ImGui::BeginTable("ResourceResidencyRendererRecovery", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Incident");
		ImGui::TableSetupColumn("Failure");
		ImGui::TableSetupColumn("Safe Actions");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%s", TerraEngine::Renderer::ToString(recovery.state));
		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted(recovery.incidentId.empty() ? "-" : recovery.incidentId.c_str());
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%s  %s", TerraEngine::Renderer::ToString(recovery.firstFailureStage), recovery.firstFailureResult.c_str());
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("save=%s", BoolText(recovery.canSaveProjectData));
		ImGui::EndTable();
	}
	if (!recovery.message.empty())
	{
		ImGui::TextWrapped("%s", recovery.message.c_str());
	}
	if (!recovery.crashDumpPath.empty())
	{
		ImGui::TextWrapped("Crash dump: %s", recovery.crashDumpPath.c_str());
	}
}

void ResourceResidencyWindow::DrawStreamingSummary()
{
	const TerraEngine::Resource::StreamingDiagnosticsSummary& summary = m_DiagnosticsCache->visibleStreamingSummary;
	ImGui::SeparatorText("Streaming");
	const TerraEngine::Scene::PublishedWorldRegionResidency residency =
		TerraEngine::Scene::WorldStreamingSystem::Instance().CurrentResidency();
	const TerraEngine::Scene::RegionInterestPlan plan =
		TerraEngine::Scene::WorldStreamingSystem::Instance().CurrentInterestPlan();
	ImGui::TextDisabled(
		"Regions: resident=%zu generation=%llu requested=%zu retirements=%zu plan=%llu",
		residency.residentRegions.size(),
		static_cast<unsigned long long>(residency.generation),
		plan.requested.size(),
		plan.retirements.size(),
		static_cast<unsigned long long>(plan.generation));
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

	if(ImGui::BeginTable(
		"ResourceAnimatedVirtualGeometrySummary",
		10,
		ImGuiTableFlags_Borders
			| ImGuiTableFlags_RowBg
			| ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("View");
		ImGui::TableSetupColumn("Animated Mode");
		ImGui::TableSetupColumn("Demand/Dispatch");
		ImGui::TableSetupColumn("Reuse/Fallback");
		ImGui::TableSetupColumn("Current Bytes");
		ImGui::TableSetupColumn("History Bytes");
		ImGui::TableSetupColumn("Bounds Bytes");
		ImGui::TableSetupColumn("High Water");
		ImGui::TableSetupColumn("Diagnostics E/C/D");
		ImGui::TableSetupColumn("Shader/Fallback");
		ImGui::TableHeadersRow();
		for(const auto& snapshot : snapshots)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(ToString(snapshot.viewType));
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(
				snapshot.animatedPageDemandCount == 0u
					? "static/dormant"
					: "compute + streamed pages");
			ImGui::TableSetColumnIndex(2);
			ImGui::Text(
				"%u/%u",
				snapshot.animatedPageDemandCount,
				snapshot.animatedUniqueDispatchCount);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text(
				"%u/%u",
				snapshot.animatedReuseCount,
				snapshot.animatedFallbackCount);
			ImGui::TableSetColumnIndex(4);
			ImGui::Text(
				"%llu",
				static_cast<unsigned long long>(
					snapshot.deformationCurrentBytes));
			ImGui::TableSetColumnIndex(5);
			ImGui::Text(
				"%llu",
				static_cast<unsigned long long>(
					snapshot.deformationHistoryBytes));
			ImGui::TableSetColumnIndex(6);
			ImGui::Text(
				"%llu",
				static_cast<unsigned long long>(
					snapshot.deformationBoundsBytes));
			ImGui::TableSetColumnIndex(7);
			ImGui::Text(
				"%llu",
				static_cast<unsigned long long>(
					snapshot.deformationHighWaterBytes));
			ImGui::TableSetColumnIndex(8);
			ImGui::Text(
				"%u/%u/%u",
				snapshot.animatedDiagnosticEmitted,
				snapshot.animatedDiagnosticCoalesced,
				snapshot.animatedDiagnosticDropped);
			ImGui::TableSetColumnIndex(9);
			ImGui::Text(
				"%s%s%s",
				snapshot.animatedShaderContractValid
					? "contract ok"
					: "contract rejected",
				snapshot.animatedFallbackReason.empty()
					? ""
					: ": ",
				snapshot.animatedFallbackReason.c_str());
		}
		ImGui::EndTable();
	}

	if (ImGui::BeginTable("ResourceVirtualGeometryRuntimeCounters", 11, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("View");
		ImGui::TableSetupColumn("Source");
		ImGui::TableSetupColumn("Meshlets S/D/C");
		ImGui::TableSetupColumn("Hi-Z");
		ImGui::TableSetupColumn("Clusters Cmp/Rej/Open");
		ImGui::TableSetupColumn("Batches");
		ImGui::TableSetupColumn("GPU Packets");
		ImGui::TableSetupColumn("Generated");
		ImGui::TableSetupColumn("Compaction");
		ImGui::TableSetupColumn("Fallback");
		ImGui::TableSetupColumn("Warnings");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot : snapshots)
		{
			// Runtime counters are displayed here instead of in the Game View so
			// developers can inspect VG submission, Hi-Z, and readback state
			// without covering the rendered camera output.
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(ToString(snapshot.viewType));
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(ToString(snapshot.counterSource));
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%u/%u/%u", snapshot.selectedMeshlets, snapshot.dispatchedMeshlets, snapshot.culledMeshlets);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%s  %u culled  %s",
				snapshot.hiZActive ? "active" : "off",
				snapshot.culledByHiZ,
				snapshot.hiZBaselineMatched ? "baseline ok" : "baseline risk");
			if (!snapshot.hiZInvalidReason.empty() && ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", snapshot.hiZInvalidReason.c_str());
			}
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%u/%u/%u", snapshot.compactedClusterCount, snapshot.rejectedClusterCount, snapshot.failOpenClusterCount);
			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%u submitted, %u single-packet", snapshot.submittedBatchCount, snapshot.singlePacketFallbackBatchCount);
			ImGui::TableSetColumnIndex(6);
			ImGui::Text("%u packets, %u descriptors", snapshot.gpuPacketRecordCount, snapshot.gpuBatchDescriptorCount);
			ImGui::TableSetColumnIndex(7);
			ImGui::Text("%u valid / %u invalid", snapshot.validGeneratedSubmissionCount, snapshot.invalidGeneratedSubmissionCount);
			ImGui::TableSetColumnIndex(8);
			ImGui::Text("%u dispatches, %u readbacks", snapshot.gpuCompactionDispatchCount, snapshot.gpuCounterReadbackCount);
			ImGui::TableSetColumnIndex(9);
			ImGui::Text("%u packets, %u batches", snapshot.gpuFallbackAffectedPackets, snapshot.gpuFallbackAffectedBatches);
			if (!snapshot.gpuFallbackReason.empty() && ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", snapshot.gpuFallbackReason.c_str());
			}
			ImGui::TableSetColumnIndex(10);
			ImGui::Text("%zu", snapshot.warnings.size());
		}
		ImGui::EndTable();
	}

	if (ImGui::BeginTable("ResourceVirtualGeometryPageSources", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("View");
		ImGui::TableSetupColumn("Payloads H/F/E");
		ImGui::TableSetupColumn("Emitted H/F/E");
		ImGui::TableSetupColumn("Missing Sources");
		ImGui::TableSetupColumn("Material Pages");
		ImGui::TableSetupColumn("Requests/Retires");
		ImGui::TableSetupColumn("Payload Bytes R/F/E");
		ImGui::TableSetupColumn("Counter Frames");
		ImGui::TableSetupColumn("Latency");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot : snapshots)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(ToString(snapshot.viewType));
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%u/%u/%u", snapshot.pageSources.highDetailPayloads, snapshot.pageSources.fallbackPayloads, snapshot.pageSources.embeddedFallbackPayloads);
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%u/%u/%u", snapshot.pageSources.emittedHighDetailSources, snapshot.pageSources.emittedFallbackSources, snapshot.pageSources.emittedEmbeddedFallbackSources);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%u", snapshot.pageSources.missingSources);
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%u pairs, %u multi", snapshot.pageSources.emittedMaterialPagePairs, snapshot.pageSources.emittedMultiMaterialPages);
			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%u/%u", snapshot.pageSources.pageRequests, snapshot.pageSources.pageRetirements);
			ImGui::TableSetColumnIndex(6);
			ImGui::Text("%llu/%llu/%llu",
				static_cast<unsigned long long>(snapshot.pageSources.residentPayloadBytes),
				static_cast<unsigned long long>(snapshot.pageSources.fallbackPayloadBytes),
				static_cast<unsigned long long>(snapshot.pageSources.embeddedFallbackBytes));
			ImGui::TableSetColumnIndex(7);
			ImGui::Text("%llu/%llu",
				static_cast<unsigned long long>(snapshot.counterPreparedFrameId),
				static_cast<unsigned long long>(snapshot.counterReadbackFrameId));
			ImGui::TableSetColumnIndex(8);
			ImGui::Text("%u frames", snapshot.counterReadbackLatencyFrames);
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
