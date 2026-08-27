// TerrainDiagnostics.cpp -- Diagnostics remain value-only until a system owns them.

#include "Terrain/TerrainDiagnostics.h"

#include "Physics/Terrain/TerrainCollisionSystem.h"

#include <algorithm>

namespace TerraEngine::Terrain
{
	TerrainLayerValidationDiagnosticRecord MakeTerrainLayerValidationDiagnosticRecord(
		TerrainResultCode resultCode,
		TerrainRootDiagnosticCode diagnosticCode,
		std::uint32_t definitionCount,
		std::uint32_t enabledCount,
		std::string message)
	{
		TerrainLayerValidationDiagnosticRecord record{};
		record.resultCode = resultCode;
		record.diagnosticCode = diagnosticCode;
		record.definitionCount = definitionCount;
		record.enabledCount = enabledCount;
		record.accepted = resultCode == TerrainResultCode::Ok
			&& diagnosticCode == TerrainRootDiagnosticCode::None;
		record.legacyRootRejected = diagnosticCode
			== TerrainRootDiagnosticCode::UnsupportedTerrainRoot;
		record.message = std::move(message);
		if(record.message.empty() && !record.accepted)
		{
			record.message = record.legacyRootRejected
				? "Only canonical TERRAROOT1 terrain is supported; recreate or recook this asset."
				: "Terrain layer validation rejected the detached candidate.";
		}
		return record;
	}

	std::string_view ToString(TerrainSamplerFallbackReason reason) noexcept
	{
		switch(reason)
		{
		case TerrainSamplerFallbackReason::None: return "None";
		case TerrainSamplerFallbackReason::RequestedLevelClamped: return "RequestedLevelClamped";
		case TerrainSamplerFallbackReason::AnisotropyUnsupported: return "AnisotropyUnsupported";
		case TerrainSamplerFallbackReason::InvalidDeviceLimit: return "InvalidDeviceLimit";
		case TerrainSamplerFallbackReason::DescriptorCapacityExceeded: return "DescriptorCapacityExceeded";
		}
		return "Unknown";
	}

	void AppendTerrainLayerReferenceDiagnostics(
		TerrainDiagnosticSnapshot& snapshot,
		const TerrainResolvedWeightSample& resolved,
		bool requiredControlMissing)
	{
		if(!requiredControlMissing && !resolved.usedDiagnosticFallback) return;
		TerrainDiagnosticItem item{};
		item.category = TerrainDiagnosticCategory::Source;
		item.severity = TerrainDiagnosticSeverity::Warning;
		item.impact = TerrainDiagnosticImpact::LastKnownGoodActive;
		item.action = TerrainDiagnosticAction::ReloadSource;
		item.message = requiredControlMissing
			? "Required terrain control data is missing; the deterministic diagnostic surface is active."
			: "Paint references a missing, disabled, or unsupported stable layer ID; only that contribution uses the diagnostic surface.";
		snapshot.items.push_back(std::move(item));
	}

	// Terrain diagnostics intentionally have no mutable singleton. Future systems
	// publish this value as one immutable snapshot at their declared boundary.
	// Layer fallback records are also values, so an AssetDatabase event can never
	// mutate authored weight data while a render frame is reading it.
	TerrainCollisionRecord MakeTerrainCollisionRecord(
		const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& snapshot,
		std::uint64_t ageMilliseconds, std::uint64_t buildLatencyMilliseconds)
	{
		TerrainCollisionRecord record;
		record.sourceRootHash = snapshot.sourceRootHash;
		record.sourcePublicationGeneration = snapshot.sourcePublicationGeneration;
		record.ageMilliseconds = ageMilliseconds != 0u ? ageMilliseconds
			: static_cast<std::uint64_t>(std::max(0.0, snapshot.ageMilliseconds));
		record.buildLatencyMilliseconds = buildLatencyMilliseconds != 0u ? buildLatencyMilliseconds
			: static_cast<std::uint64_t>(std::max(0.0, snapshot.buildLatencyMilliseconds));
		record.lastKnownGoodActive = snapshot.status != TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
			&& snapshot.lastKnownGoodRecipe.IsComplete();
		const auto& active = snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
			? snapshot.currentRecipe : snapshot.lastKnownGoodRecipe;
		record.coordinate = {active.areaX, active.areaZ};
		record.activeSourcePublicationGeneration = active.authoredGeneration;
		// A recipe carries a compact hash for provider construction, while the
		// snapshot preserves the full authored root identity for editor repair UI.
		record.activeSourceRootHash = snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
			? snapshot.sourceRootHash : snapshot.lastKnownGoodSourceRootHash;
		if(!record.activeSourceRootHash.IsValid()) record.activeSourceRootHash = {active.authoredHash, 0u};
		if(snapshot.status != TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
			&& snapshot.lastKnownGoodSourcePublicationGeneration != 0u)
			record.activeSourcePublicationGeneration = snapshot.lastKnownGoodSourcePublicationGeneration;
		record.message = snapshot.message;
		switch(snapshot.status)
		{
		case TerraEngine::Physics::Terrain::TerrainCollisionStatus::Pending: record.status = TerrainCollisionStatus::Pending; break;
		case TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current: record.status = TerrainCollisionStatus::Current; break;
		case TerraEngine::Physics::Terrain::TerrainCollisionStatus::Stale: record.status = TerrainCollisionStatus::Stale; break;
		case TerraEngine::Physics::Terrain::TerrainCollisionStatus::Failed: record.status = TerrainCollisionStatus::Failed; break;
		default: record.status = TerrainCollisionStatus::Unavailable; break;
		}
		return record;
	}

	TerrainDiagnosticSnapshot BuildTerrainDiagnosticSnapshot(TerrainDiagnosticBuildInput input)
	{
		TerrainDiagnosticSnapshot result = std::move(input.snapshot);
		// Each input record becomes one bounded, structured fact.  This lets jobs
		// and automation report failures without reading mutable terrain state.
		for(const TerrainConflictRecord& conflict : result.conflicts)
		{
			TerrainDiagnosticItem item; item.category = TerrainDiagnosticCategory::Conflict; item.severity = TerrainDiagnosticSeverity::Error;
			item.impact = conflict.code == TerrainResultCode::Conflict ? TerrainDiagnosticImpact::MutationRejected : TerrainDiagnosticImpact::Unavailable;
			item.action = conflict.code == TerrainResultCode::Conflict ? TerrainDiagnosticAction::ResolveConflict : TerrainDiagnosticAction::Retry;
			item.message = conflict.message; result.items.push_back(std::move(item));
		}
		for(const TerrainResourcePressureRecord& pressure : result.resourcePressure)
		{
			if(pressure.budgetBytes == 0u || pressure.usedBytes <= pressure.budgetBytes) continue;
			TerrainDiagnosticItem item; item.category = TerrainDiagnosticCategory::Residency; item.severity = pressure.evictionRequired ? TerrainDiagnosticSeverity::Error : TerrainDiagnosticSeverity::Warning;
			item.impact = pressure.evictionRequired ? TerrainDiagnosticImpact::Unavailable : TerrainDiagnosticImpact::DelayedDerivedData;
			item.action = TerrainDiagnosticAction::IncreaseBudget; item.message = pressure.resource + " exceeds its supplied residency budget."; result.items.push_back(std::move(item));
		}
		for(const TerrainLayerValidationDiagnosticRecord& validation : result.layerValidations)
		{
			if(validation.accepted) continue;
			TerrainDiagnosticItem item{};
			item.category = TerrainDiagnosticCategory::LayerValidation;
			item.severity = TerrainDiagnosticSeverity::Error;
			item.impact = validation.legacyRootRejected
				? TerrainDiagnosticImpact::Unavailable
				: TerrainDiagnosticImpact::MutationRejected;
			item.action = validation.legacyRootRejected
				? TerrainDiagnosticAction::Rebuild
				: TerrainDiagnosticAction::ResolveConflict;
			item.message = validation.message.empty()
				? "Terrain layer validation rejected the detached candidate."
				: validation.message;
			result.items.push_back(std::move(item));
		}
		for(const TerrainSamplerQualityDiagnosticRecord& sampler : result.samplerQuality)
		{
			if(sampler.fallbackReason == TerrainSamplerFallbackReason::None) continue;
			TerrainDiagnosticItem item{};
			item.category = TerrainDiagnosticCategory::Sampler;
			item.severity = sampler.fallbackReason
				== TerrainSamplerFallbackReason::DescriptorCapacityExceeded
				? TerrainDiagnosticSeverity::Error : TerrainDiagnosticSeverity::Warning;
			item.impact = sampler.fallbackReason
				== TerrainSamplerFallbackReason::DescriptorCapacityExceeded
				? TerrainDiagnosticImpact::Unavailable : TerrainDiagnosticImpact::None;
			item.action = sampler.fallbackReason
				== TerrainSamplerFallbackReason::DescriptorCapacityExceeded
				? TerrainDiagnosticAction::IncreaseBudget : TerrainDiagnosticAction::None;
			item.message = sampler.message.empty()
				? "Terrain sampler used fallback: " + std::string(ToString(sampler.fallbackReason)) + "."
				: sampler.message;
			result.items.push_back(std::move(item));
		}
		if(result.textureStreaming
			&& result.textureStreaming->descriptorCapacityExhausted)
		{
			TerrainDiagnosticItem item{};
			item.category = TerrainDiagnosticCategory::Residency;
			item.severity = TerrainDiagnosticSeverity::Error;
			item.impact = TerrainDiagnosticImpact::Unavailable;
			item.action = TerrainDiagnosticAction::IncreaseBudget;
			item.message = "Terrain sampled-texture aliases exhausted descriptor capacity ("
				+ std::to_string(result.textureStreaming->descriptors.descriptorSlotsUsed)
				+ "/" + std::to_string(result.textureStreaming->descriptors.descriptorCapacity)
				+ " slots used).";
			result.items.push_back(std::move(item));
		}
		for(const TerrainLayerFallbackRecord& fallback : result.layerFallbacks)
		{
			if(fallback.state == TerrainLayerFallbackState::Healthy) continue;
			TerrainDiagnosticItem item; item.category = TerrainDiagnosticCategory::Source; item.severity = TerrainDiagnosticSeverity::Warning;
			item.impact = TerrainDiagnosticImpact::LastKnownGoodActive; item.action = TerrainDiagnosticAction::ReloadSource; item.message = fallback.message; result.items.push_back(std::move(item));
		}
		for(const TerrainSeamRecord& seam : result.seams)
		{
			TerrainDiagnosticItem item; item.category = TerrainDiagnosticCategory::Seam; item.severity = TerrainDiagnosticSeverity::Warning;
			item.impact = TerrainDiagnosticImpact::DelayedDerivedData; item.action = TerrainDiagnosticAction::Rebuild; item.location = seam.coordinate; item.message = seam.message; result.items.push_back(std::move(item));
		}
		for(const TerrainAreaLodDiagnosticRecord& area : result.areaLod)
		{
			if(!area.rolledBack && !area.missingOuterHalo
				&& area.byteState != TerrainPageByteState::Missing
				&& area.byteState != TerrainPageByteState::Rejected) continue;
			TerrainDiagnosticItem item{};
			item.category = area.missingOuterHalo
				? TerrainDiagnosticCategory::Seam : TerrainDiagnosticCategory::Residency;
			item.location = area.coordinate;
			item.severity = area.rolledBack || area.byteState == TerrainPageByteState::Rejected
				? TerrainDiagnosticSeverity::Error : TerrainDiagnosticSeverity::Warning;
			item.impact = area.rolledBack
				? TerrainDiagnosticImpact::LastKnownGoodActive : TerrainDiagnosticImpact::DelayedDerivedData;
			item.action = area.missingOuterHalo
				? TerrainDiagnosticAction::LocateArea : TerrainDiagnosticAction::Retry;
			item.message = area.message.empty() ? area.stableCode : area.message;
			result.items.push_back(std::move(item));
		}
		for(const TerrainCollisionRecord& collision : result.collisions)
		{
			if(collision.status == TerrainCollisionStatus::Current) continue;
			TerrainDiagnosticItem item; item.category = TerrainDiagnosticCategory::Collision; item.location = collision.coordinate; item.sourceHash = collision.sourceRootHash; item.sourceGeneration = collision.sourcePublicationGeneration; item.message = collision.message;
			item.severity = collision.status == TerrainCollisionStatus::Unavailable ? TerrainDiagnosticSeverity::Error : TerrainDiagnosticSeverity::Warning;
			item.impact = collision.lastKnownGoodActive ? TerrainDiagnosticImpact::LastKnownGoodActive : (collision.status == TerrainCollisionStatus::Unavailable ? TerrainDiagnosticImpact::Unavailable : TerrainDiagnosticImpact::DelayedDerivedData);
			item.action = collision.status == TerrainCollisionStatus::Stale ? TerrainDiagnosticAction::Rebuild : TerrainDiagnosticAction::Retry; result.items.push_back(std::move(item));
		}
		for(const TerrainJobRecord& job : result.jobs)
		{
			if(job.status != TerrainDerivedStatus::RejectedStale && job.status != TerrainDerivedStatus::Failed && job.status != TerrainDerivedStatus::Cancelled) continue;
			TerrainDiagnosticItem item; item.category = TerrainDiagnosticCategory::Derived; item.severity = job.status == TerrainDerivedStatus::Failed ? TerrainDiagnosticSeverity::Error : TerrainDiagnosticSeverity::Warning;
			item.impact = TerrainDiagnosticImpact::LastKnownGoodActive; item.action = job.status == TerrainDerivedStatus::RejectedStale ? TerrainDiagnosticAction::Rebuild : TerrainDiagnosticAction::Retry;
			item.message = "Derived terrain job did not publish a current result."; result.items.push_back(std::move(item));
		}
		for(auto& [stage, samples] : input.stageSamples)
		{
			if(samples.empty()) continue;
			std::sort(samples.begin(), samples.end());
			// Use the nearest-rank definition: P95 of five observations is the
			// fifth supplied value, never an interpolated or fabricated duration.
			const auto percentile = [&](std::size_t numerator) {
				const std::size_t rank = (samples.size() * numerator + 99u) / 100u;
				return samples[std::max<std::size_t>(1u, rank) - 1u];
			};
			result.stagePercentiles.push_back({std::move(stage), percentile(50u), percentile(95u), samples.size()});
		}
		std::sort(result.items.begin(), result.items.end(), [](const TerrainDiagnosticItem& a, const TerrainDiagnosticItem& b) { return static_cast<unsigned>(a.category) < static_cast<unsigned>(b.category); });
		return result;
	}
}
