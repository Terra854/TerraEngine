// TerrainResize.cpp -- Global-coordinate resize validation and height policy math.

#include "Terrain/TerrainResize.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace TerraEngine::Terrain
{
	namespace
	{
		bool ContainsBounds(TerrainSampleBounds outer, TerrainSampleBounds inner) noexcept
		{
			return outer.minX <= inner.minX && outer.minZ <= inner.minZ
				&& outer.maxXExclusive >= inner.maxXExclusive && outer.maxZExclusive >= inner.maxZExclusive;
		}

		std::int32_t SaturateHeight(std::int64_t value) noexcept
		{
			return static_cast<std::int32_t>(std::clamp(value,
				static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)()),
				static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)())));
		}

		std::int64_t RoundDivideSigned(std::int64_t numerator, std::int64_t denominator) noexcept
		{
			// Ties round away from zero. Applying the sign after an unsigned magnitude
			// calculation avoids the negative-value bias of adding denominator / 2.
			if(denominator <= 0) return 0;
			const bool negative = numerator < 0;
			const std::uint64_t magnitude = negative
				? std::uint64_t{0} - static_cast<std::uint64_t>(numerator)
				: static_cast<std::uint64_t>(numerator);
			const std::uint64_t rounded = magnitude / static_cast<std::uint64_t>(denominator)
				+ (magnitude % static_cast<std::uint64_t>(denominator)
					>= (static_cast<std::uint64_t>(denominator) + 1u) / 2u ? 1u : 0u);
			if(!negative) return rounded > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
				? (std::numeric_limits<std::int64_t>::max)() : static_cast<std::int64_t>(rounded);
			return rounded >= (std::uint64_t{1} << 63u)
				? (std::numeric_limits<std::int64_t>::min)()
				: -static_cast<std::int64_t>(rounded);
		}

		std::optional<std::uint64_t> SampleCount(TerrainSampleBounds bounds) noexcept
		{
			if (!bounds.IsValid() || bounds.minX == bounds.maxXExclusive || bounds.minZ == bounds.maxZExclusive)
				return std::nullopt;
			const std::uint64_t width = static_cast<std::uint64_t>(bounds.maxXExclusive)
				- static_cast<std::uint64_t>(bounds.minX);
			const std::uint64_t depth = static_cast<std::uint64_t>(bounds.maxZExclusive)
				- static_cast<std::uint64_t>(bounds.minZ);
			if (width != 0u && depth > (std::numeric_limits<std::uint64_t>::max)() / width)
				return std::nullopt;
			return width * depth;
		}

		std::size_t CountPartialAreas(TerrainSampleBounds bounds, const TerrainAreaBounds& areas) noexcept
		{
			std::size_t result = 0u;
			for (std::int64_t z = areas.minZ; z < areas.maxZExclusive; ++z)
				for (std::int64_t x = areas.minX; x < areas.maxXExclusive; ++x)
				{
					const auto active = TerrainActiveRectForArea(bounds, {x, z});
					if (active && (active->minX != 0u || active->minZ != 0u
						|| active->maxXExclusive != kTerrainAreaExtentSamples
						|| active->maxZExclusive != kTerrainAreaExtentSamples)) ++result;
				}
			return result;
		}

		bool IsStrictlySorted(const std::vector<TerrainAreaCoordinate>& coordinates) noexcept
		{
			for (std::size_t index = 1u; index < coordinates.size(); ++index)
				if (!(coordinates[index - 1u] < coordinates[index])) return false;
			return true;
		}
	}

	TerrainCodecResult ValidateTerrainResizeDelta(const TerrainResizeDelta& delta, const TerrainAssetRoot& root)
	{
		if (!root.authoredBounds.IsValid() || !delta.previousBounds.IsValid() || !delta.proposedBounds.IsValid()
			|| delta.previousBounds != root.authoredBounds)
			return {TerrainResultCode::InvalidArgument, "Terrain resize bounds must be valid and match the captured root."};
		const bool expands = ContainsBounds(delta.proposedBounds, delta.previousBounds)
			&& delta.proposedBounds != delta.previousBounds;
		const bool crops = ContainsBounds(delta.previousBounds, delta.proposedBounds)
			&& delta.proposedBounds != delta.previousBounds;
		if ((delta.kind == TerrainResizeKind::Expand && !expands)
			|| (delta.kind == TerrainResizeKind::Crop && !crops))
			return {TerrainResultCode::InvalidArgument, "A resize must exclusively expand or crop the captured bounds; mixed moves are not allowed."};
		if (delta.kind == TerrainResizeKind::Expand && delta.expansionPolicy == TerrainExpansionPolicy::EdgeBlend
			&& delta.transitionSamples == 0u)
			return {TerrainResultCode::InvalidArgument, "Edge Blend requires a positive transition sample count."};
		if (!IsStrictlySorted(delta.descriptorRemovals)
			|| !IsStrictlySorted(delta.forcedRetirements)
			|| !IsStrictlySorted(delta.affectedOneRing))
			return {TerrainResultCode::InvalidArgument, "Resize coordinate lists must be sorted and unique."};
		std::set<TerrainAreaCoordinate> removals;
		for (const auto coordinate : delta.descriptorRemovals)
			if (!removals.insert(coordinate).second)
				return {TerrainResultCode::InvalidArgument, "Resize descriptor removals must be unique."};
		std::set<TerrainAreaCoordinate> upserts;
		for (std::size_t index = 0u; index < delta.descriptorUpserts.size(); ++index)
		{
			const TerrainAreaDescriptor& descriptor = delta.descriptorUpserts[index];
			if (index != 0u && !(delta.descriptorUpserts[index - 1u].coordinate < descriptor.coordinate))
				return {TerrainResultCode::InvalidArgument, "Resize descriptor upserts must be sorted and unique."};
			if (!upserts.insert(descriptor.coordinate).second || removals.contains(descriptor.coordinate))
				return {TerrainResultCode::InvalidArgument, "Resize descriptor upserts must be unique and disjoint from removals."};
			const auto active = TerrainActiveRectForArea(delta.proposedBounds, descriptor.coordinate);
			if (!active) return {TerrainResultCode::OutOfBounds, "A resize descriptor lies outside the proposed authored bounds."};
		}
		for (const auto& [coordinate, active] : delta.partialAreaBounds)
		{
			const auto expected = TerrainActiveRectForArea(delta.proposedBounds, coordinate);
			if (!active.IsValid() || !expected || active != *expected)
				return {TerrainResultCode::InvalidArgument, "Resize partial-area metadata must match the proposed authored bounds."};
		}
		return {};
	}

	TerrainResizePreview PreviewTerrainResize(const TerrainAssetRoot& root, TerrainResizeDelta delta, std::size_t maximumAreas)
	{
		TerrainResizeRequest request{};
		request.transactionId = TerraEngine::GenerateUUID();
		request.targetAssetUuid = root.assetUuid;
		request.expectedRootHash = root.rootContentHash;
		request.expectedRootGeneration = root.publicationGeneration;
		request.delta = std::move(delta);
		request.budget.maximumScannedAreas = maximumAreas;
		request.budget.maximumGeneratedAreas = maximumAreas;
		request.budget.maximumGeneratedSamples = maximumAreas > (std::numeric_limits<std::size_t>::max)() / kTerrainAreaSampleCount
			? (std::numeric_limits<std::size_t>::max)() : maximumAreas * kTerrainAreaSampleCount;
		return PreviewTerrainResize(root, request);
	}

	TerrainResizePreview PreviewTerrainResize(const TerrainAssetRoot& root, const TerrainResizeRequest& request)
	{
		TerrainResizePreview preview{};
		preview.delta = request.delta;
		preview.budget = request.budget;
		if (request.targetAssetUuid != root.assetUuid
			|| request.expectedRootHash != root.rootContentHash
			|| request.expectedRootGeneration != root.publicationGeneration)
		{
			preview.code = TerrainResultCode::Conflict;
			preview.message = "The resize request no longer matches the captured terrain root.";
			return preview;
		}
		const TerrainCodecResult validation = ValidateTerrainResizeDelta(preview.delta, root);
		if (!validation.Succeeded()) { preview.code = validation.code; preview.message = validation.message; return preview; }
		const auto previous = TerrainAreasForSamples(preview.delta.previousBounds);
		const auto proposed = TerrainAreasForSamples(preview.delta.proposedBounds);
		const auto previousCount = previous ? previous->AreaCount() : std::nullopt;
		const auto proposedCount = proposed ? proposed->AreaCount() : std::nullopt;
		if (!previousCount || !proposedCount || *previousCount > request.budget.maximumScannedAreas
			|| *proposedCount > request.budget.maximumScannedAreas)
		{
			preview.code = TerrainResultCode::BudgetExceeded;
			preview.message = "The proposed terrain extent exceeds the configured area budget.";
			return preview;
		}
		preview.previousAreaCount = *previousCount;
		preview.proposedAreaCount = *proposedCount;
		preview.addedAreaCount = *proposedCount > *previousCount ? *proposedCount - *previousCount : 0u;
		preview.removedAreaCount = *previousCount > *proposedCount ? *previousCount - *proposedCount : 0u;
		const auto previousSamples = SampleCount(preview.delta.previousBounds);
		const auto proposedSamples = SampleCount(preview.delta.proposedBounds);
		if (!previousSamples || !proposedSamples)
		{
			preview.code = TerrainResultCode::Overflow;
			preview.message = "The resize sample footprint exceeds the supported checked range.";
			return preview;
		}
		preview.retainedSampleCount = std::min(*previousSamples, *proposedSamples);
		preview.addedSampleCount = *proposedSamples > *previousSamples ? *proposedSamples - *previousSamples : 0u;
		preview.discardedSampleCount = *previousSamples > *proposedSamples ? *previousSamples - *proposedSamples : 0u;
		preview.partialAreaCount = CountPartialAreas(preview.delta.proposedBounds, *proposed);
		preview.fullAreaCount = preview.proposedAreaCount - preview.partialAreaCount;
		preview.estimatedWork.scannedAreas = preview.previousAreaCount;
		preview.estimatedWork.generatedAreas = preview.delta.descriptorUpserts.size();
		preview.estimatedWork.generatedSamples = preview.delta.kind == TerrainResizeKind::Expand
			? preview.addedSampleCount : preview.discardedSampleCount;
		if (preview.estimatedWork.generatedAreas > request.budget.maximumGeneratedAreas
			|| preview.estimatedWork.generatedSamples > request.budget.maximumGeneratedSamples)
		{
			preview.code = TerrainResultCode::BudgetExceeded;
			preview.message = "The proposed resize exceeds its generated-area or sample budget.";
			return preview;
		}
		const long double width = static_cast<long double>(preview.delta.proposedBounds.maxXExclusive)
			- static_cast<long double>(preview.delta.proposedBounds.minX);
		const long double depth = static_cast<long double>(preview.delta.proposedBounds.maxZExclusive)
			- static_cast<long double>(preview.delta.proposedBounds.minZ);
		const long double physicalWidth = width * root.sampleSpacingMetres;
		const long double physicalDepth = depth * root.sampleSpacingMetres;
		if (!std::isfinite(static_cast<double>(physicalWidth)) || !std::isfinite(static_cast<double>(physicalDepth)))
		{
			preview.code = TerrainResultCode::Overflow;
			preview.message = "The proposed physical terrain footprint overflows the supported range.";
			return preview;
		}
		preview.physicalWidthMetres = static_cast<double>(physicalWidth);
		preview.physicalDepthMetres = static_cast<double>(physicalDepth);
		preview.previousPhysicalWidthMetres = static_cast<double>(
			(static_cast<long double>(preview.delta.previousBounds.maxXExclusive)
				- static_cast<long double>(preview.delta.previousBounds.minX)) * root.sampleSpacingMetres);
		preview.previousPhysicalDepthMetres = static_cast<double>(
			(static_cast<long double>(preview.delta.previousBounds.maxZExclusive)
				- static_cast<long double>(preview.delta.previousBounds.minZ)) * root.sampleSpacingMetres);
		preview.requiresDataLossConfirmation = preview.delta.kind == TerrainResizeKind::Crop;
		preview.cropLoss.discardedSamples = preview.discardedSampleCount;
		preview.cropLoss.removedDescriptors = preview.delta.descriptorRemovals.size();
		preview.cropLoss.partialDescriptors = preview.delta.partialAreaBounds.size();
		preview.proposalFingerprint = FingerprintTerrainResizeProposal(root, request, preview);
		preview.canCommit = !preview.boundaryHoleChoiceRequired
			&& (!preview.requiresDataLossConfirmation || request.confirmDiscard);
		preview.message = preview.delta.kind == TerrainResizeKind::Expand
			? "Terrain expansion preview is ready." : "Terrain crop preview requires explicit data-loss confirmation.";
		return preview;
	}

	ContentHash FingerprintTerrainResizeProposal(const TerrainAssetRoot& root,
		const TerrainResizeRequest& request, const TerrainResizePreview& preview)
	{
		// The canonical text contains only locale-independent integer tokens and
		// content hashes. It deliberately excludes presentation strings.
		std::ostringstream stream;
		stream.imbue(std::locale::classic());
		stream << "terrain-resize-v1|" << TerraEngine::ToHexString(root.assetUuid) << '|'
			<< ToHexString(root.rootContentHash) << '|' << root.publicationGeneration << '|'
			<< static_cast<unsigned>(request.delta.kind) << '|'
			<< request.delta.previousBounds.minX << '|' << request.delta.previousBounds.minZ << '|'
			<< request.delta.previousBounds.maxXExclusive << '|' << request.delta.previousBounds.maxZExclusive << '|'
			<< request.delta.proposedBounds.minX << '|' << request.delta.proposedBounds.minZ << '|'
			<< request.delta.proposedBounds.maxXExclusive << '|' << request.delta.proposedBounds.maxZExclusive << '|'
			<< static_cast<unsigned>(request.delta.expansionPolicy) << '|' << request.delta.transitionSamples << '|'
			<< static_cast<unsigned>(request.delta.holePolicy) << '|' << request.holePolicyExplicit << '|'
			<< request.budget.maximumScannedAreas << '|' << request.budget.maximumGeneratedAreas << '|'
			<< request.budget.maximumGeneratedSamples << '|' << request.budget.maximumPreparedBytes << '|'
			<< request.budget.maximumHistoryBytes << '|' << request.budget.maximumResidentAreas << '|'
			<< preview.cropLoss.discardedSamples << '|' << preview.cropLoss.nonDefaultHeightSamples << '|'
			<< preview.cropLoss.holeSamples << '|' << preview.cropLoss.paintedSamples << '|'
			<< preview.cropLoss.removedDescriptors << '|' << preview.cropLoss.retainedHistoryBytes;
		for (const TerrainResizeObservation& observation : preview.observations)
			stream << "|o:" << observation.coordinate.x << ':' << observation.coordinate.z << ':'
				<< observation.editGeneration << ':' << ToHexString(observation.authoredContentHash);
		const std::string canonical = stream.str();
		return HashTerrainBytes(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()));
	}

	std::string_view ToString(TerrainResizeKind value) noexcept
	{
		return value == TerrainResizeKind::Expand ? "expand" : "crop";
	}

	std::string_view ToString(TerrainExpansionPolicy value) noexcept
	{
		switch (value)
		{
		case TerrainExpansionPolicy::EdgeBlend: return "edge-blend";
		case TerrainExpansionPolicy::ExtendEdge: return "extend-edge";
		case TerrainExpansionPolicy::FlatDefault: return "flat-default";
		}
		return "unknown";
	}

	std::string_view ToString(TerrainBoundaryHolePolicy value) noexcept
	{
		return value == TerrainBoundaryHolePolicy::CloseAtFormerEdge
			? "close-at-former-edge" : "extend-connected-boundary-holes";
	}

	std::int32_t InitializeExpandedTerrainHeight(const TerrainAssetRoot& root, const TerrainResizeDelta& delta,
		TerrainSampleCoordinate sample, const TerrainHeightSampler& source) noexcept
	{
		if (delta.kind != TerrainResizeKind::Expand || delta.previousBounds.Contains(sample)
			|| !delta.proposedBounds.Contains(sample) || !source) return root.defaultHeightMm;
		if (delta.expansionPolicy == TerrainExpansionPolicy::FlatDefault) return root.defaultHeightMm;
		const auto& old = delta.previousBounds;
		const TerrainSampleCoordinate anchor{
			std::clamp(sample.x, old.minX, old.maxXExclusive - 1),
			std::clamp(sample.z, old.minZ, old.maxZExclusive - 1)};
		const std::int32_t anchorHeight = source(anchor).value_or(root.defaultHeightMm);
		if (delta.expansionPolicy == TerrainExpansionPolicy::ExtendEdge) return anchorHeight;
		std::int64_t projected = anchorHeight;
		std::uint64_t distance = 0u;
		auto projectAxis = [&](std::int64_t coordinate, std::int64_t minimum, std::int64_t maximum,
			TerrainSampleCoordinate inward) {
			if (coordinate >= minimum && coordinate < maximum) return;
			const std::uint64_t axisDistance = coordinate < minimum
				? static_cast<std::uint64_t>(minimum - coordinate)
				: static_cast<std::uint64_t>(coordinate - (maximum - 1));
			distance = std::max(distance, axisDistance);
			const std::int32_t inwardHeight = source(inward).value_or(anchorHeight);
			projected += static_cast<std::int64_t>(anchorHeight - inwardHeight)
				* static_cast<std::int64_t>(std::min<std::uint64_t>(axisDistance, delta.transitionSamples));
		};
		TerrainSampleCoordinate inwardX = anchor;
		if (sample.x < old.minX) inwardX.x = std::min(old.maxXExclusive - 1, old.minX + 1);
		else if (sample.x >= old.maxXExclusive) inwardX.x = std::max(old.minX, old.maxXExclusive - 2);
		projectAxis(sample.x, old.minX, old.maxXExclusive, inwardX);
		TerrainSampleCoordinate inwardZ = anchor;
		if (sample.z < old.minZ) inwardZ.z = std::min(old.maxZExclusive - 1, old.minZ + 1);
		else if (sample.z >= old.maxZExclusive) inwardZ.z = std::max(old.minZ, old.maxZExclusive - 2);
		projectAxis(sample.z, old.minZ, old.maxZExclusive, inwardZ);
		const std::uint64_t transition = std::max<std::uint64_t>(1u, delta.transitionSamples);
		const std::uint64_t retained = distance >= transition ? 0u : transition - distance;
		const std::int64_t blendedNumerator = projected * static_cast<std::int64_t>(retained)
			+ static_cast<std::int64_t>(root.defaultHeightMm) * static_cast<std::int64_t>(transition - retained);
		const std::int64_t blended = RoundDivideSigned(blendedNumerator, static_cast<std::int64_t>(transition));
		return SaturateHeight(blended);
	}

	TerrainWeightSample InitializeExpandedTerrainSurface(const TerrainAssetRoot& root,
		const TerrainResizeDelta& delta, TerrainSampleCoordinate sample,
		const TerrainSurfaceSampler& source) noexcept
	{
		const std::array<TerrainWeightContribution, 1u> fallback{{{root.defaultLayerId, 255u}}};
		const TerrainWeightSample defaultSample = NormalizeTerrainWeights(fallback);
		if(delta.kind != TerrainResizeKind::Expand || delta.previousBounds.Contains(sample)
			|| !delta.proposedBounds.Contains(sample) || !source) return defaultSample;
		const auto& old = delta.previousBounds;
		const TerrainSampleCoordinate anchor{
			std::clamp(sample.x, old.minX, old.maxXExclusive - 1),
			std::clamp(sample.z, old.minZ, old.maxZExclusive - 1)};
		const auto boundary = source(anchor);
		if(!boundary || delta.expansionPolicy == TerrainExpansionPolicy::FlatDefault) return defaultSample;
		if(delta.expansionPolicy == TerrainExpansionPolicy::ExtendEdge) return *boundary;
		const std::uint64_t distanceX = sample.x < old.minX ? static_cast<std::uint64_t>(old.minX - sample.x)
			: sample.x >= old.maxXExclusive ? static_cast<std::uint64_t>(sample.x - old.maxXExclusive + 1) : 0u;
		const std::uint64_t distanceZ = sample.z < old.minZ ? static_cast<std::uint64_t>(old.minZ - sample.z)
			: sample.z >= old.maxZExclusive ? static_cast<std::uint64_t>(sample.z - old.maxZExclusive + 1) : 0u;
		const std::uint64_t transition = std::max<std::uint64_t>(1u, delta.transitionSamples);
		const std::uint64_t retained = std::max(distanceX, distanceZ) >= transition
			? 0u : transition - std::max(distanceX, distanceZ);
		std::vector<TerrainWeightContribution> contributions;
		contributions.reserve(boundary->slots.size() + 1u);
		for(const TerrainWeightSlot& slot : boundary->slots)
			contributions.push_back({slot.layerId, static_cast<std::uint32_t>(slot.weight) * static_cast<std::uint32_t>(retained)});
		contributions.push_back({root.defaultLayerId, static_cast<std::uint32_t>(255u * (transition - retained))});
		return NormalizeTerrainWeights(contributions);
	}

	bool InitializeExpandedTerrainHole(const TerrainResizeDelta& delta,
		TerrainSampleCoordinate sample, const TerrainHoleSampler& source) noexcept
	{
		if(delta.kind != TerrainResizeKind::Expand || delta.previousBounds.Contains(sample)
			|| !delta.proposedBounds.Contains(sample) || !source
			|| delta.holePolicy != TerrainBoundaryHolePolicy::ExtendConnectedBoundaryHoles) return false;
		const TerrainSampleCoordinate anchor{
			std::clamp(sample.x, delta.previousBounds.minX, delta.previousBounds.maxXExclusive - 1),
			std::clamp(sample.z, delta.previousBounds.minZ, delta.previousBounds.maxZExclusive - 1)};
		return source(anchor);
	}

	std::vector<std::int32_t> CanonicalizeCroppedTerrainHeights(
		std::span<const std::int32_t> source, TerrainAreaActiveRect active)
	{
		if(source.size() != kTerrainAreaSampleCount || !active.IsValid()) return {};
		std::vector<std::int32_t> result(kTerrainAreaSampleCount);
		for(std::uint32_t z = 0u; z < 256u; ++z)
			for(std::uint32_t x = 0u; x < 256u; ++x)
			{
				const std::uint32_t retainedX = std::clamp(x, static_cast<std::uint32_t>(active.minX),
					static_cast<std::uint32_t>(active.maxXExclusive - 1u));
				const std::uint32_t retainedZ = std::clamp(z, static_cast<std::uint32_t>(active.minZ),
					static_cast<std::uint32_t>(active.maxZExclusive - 1u));
				result[static_cast<std::size_t>(z) * 256u + x]
					= source[static_cast<std::size_t>(retainedZ) * 256u + retainedX];
			}
		return result;
	}

	std::vector<std::uint8_t> CanonicalizeCroppedTerrainHoles(
		std::span<const std::uint8_t> source, TerrainAreaActiveRect active)
	{
		if(source.size() != kTerrainAreaSampleCount / 8u || !active.IsValid()) return {};
		std::vector<std::uint8_t> result(source.begin(), source.end());
		for(std::uint32_t z = 0u; z < 256u; ++z)
			for(std::uint32_t x = 0u; x < 256u; ++x)
				if(x < active.minX || x >= active.maxXExclusive || z < active.minZ || z >= active.maxZExclusive)
					result[(static_cast<std::size_t>(z) * 256u + x) / 8u]
						&= static_cast<std::uint8_t>(~(1u << (x & 7u)));
		return result;
	}

	std::vector<TerrainWeightSample> CanonicalizeCroppedTerrainSurfaces(
		std::span<const TerrainWeightSample> source, TerrainAreaActiveRect active,
		std::uint16_t defaultLayerId)
	{
		if(source.size() != kTerrainAreaSampleCount || !active.IsValid()) return {};
		const std::array<TerrainWeightContribution, 1u> fallback{{{defaultLayerId, 255u}}};
		const TerrainWeightSample defaultSample = NormalizeTerrainWeights(fallback);
		std::vector<TerrainWeightSample> result(source.begin(), source.end());
		for(std::uint32_t z = 0u; z < 256u; ++z)
			for(std::uint32_t x = 0u; x < 256u; ++x)
				if(x < active.minX || x >= active.maxXExclusive || z < active.minZ || z >= active.maxZExclusive)
					result[static_cast<std::size_t>(z) * 256u + x] = defaultSample;
		return result;
	}
}
