// NavigationSceneBootstrap.cpp - automatic immutable navigation bootstrap.

#include "Navigation/Integration/NavigationSceneBootstrap.h"

#include <algorithm>

namespace TerraEngine::Navigation {
	namespace {
		void AddProfile(std::vector<NavigationProfile>& profiles, const NavigationProfile& profile) {
			const auto duplicate = std::find_if(profiles.begin(), profiles.end(), [&profile](const NavigationProfile& current) {
				return current == profile;
			});
			if(duplicate == profiles.end()) profiles.push_back(profile);
		}

		[[nodiscard]] NavigationSourceIndexRecord MakeSourceIndex(
			const NavigationEligibleSourceRecord& source) {
			const NavigationGeometrySnapshotDescriptor& descriptor = source.geometry->Descriptor();
			NavigationSourceIndexRecord result{};
			result.sourceId = source.identity.sourceId;
			result.sourceRevision = descriptor.sourceRevision;
			result.geometryRevision = source.revisions.geometryRevision;
			result.enabled = true;
			result.identity = source.identity;
			result.revisionEvidence = source.revisions;
			return result;
		}
	}

	NavigationSceneBootstrapCapture CaptureNavigationSceneBootstrap(
		const NavigationSceneBootstrapInput& input) {
		NavigationSceneBootstrapCapture result{};
		const NavigationProfileResolver profiles = NavigationProfileResolver::Create(
			input.defaultProfile, input.registeredProfiles);
		if(!profiles.IsConfigurationValid()) {
			result.reason = "Navigation profile configuration is invalid or contains duplicate registered IDs.";
			return result;
		}

		// Terrain conversion owns a new immutable snapshot. All non-terrain packets
		// have already been copied at the protected source-capture boundary.
		std::vector<NavigationEligibleSourceCandidate> candidates;
		candidates.reserve(input.sources.size());
		for(const NavigationBootstrapSourceInput& source : input.sources) {
			NavigationEligibleSourceCandidate candidate = source.candidate;
			if(candidate.identity.kind == NavigationSourceKind::Terrain) {
				if(!source.terrain) {
					candidate.physicalBackingCurrent = false;
					candidates.push_back(std::move(candidate));
					continue;
				}
				const TerrainNavigationCaptureResult terrain = CaptureTerrainNavigationSource(*source.terrain);
				if(!terrain.accepted || !terrain.snapshot) {
					candidate.physicalBackingCurrent = false;
					candidates.push_back(std::move(candidate));
					continue;
				}
				candidate.geometry = terrain.snapshot;
			}
			candidates.push_back(std::move(candidate));
		}

		NavigationEligibleSourceIndexBuildResult indexed = BuildNavigationEligibleSourceIndex(candidates);
		if(!indexed.accepted) {
			result.reason = indexed.reason.empty() ? "Automatic navigation source indexing was rejected." : indexed.reason;
			return result;
		}
		result.eligibleSourceIndex = std::make_shared<const NavigationEligibleSourceIndex>(std::move(indexed.index));
		result.terrainAvailable = result.eligibleSourceIndex->HasCurrentTerrain();
		if(!result.terrainAvailable) {
			result.reason = "NavigationUnavailableMissingTerrain";
			return result;
		}

		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> snapshots;
		std::vector<NavigationSourceIndexRecord> sourceIndex;
		snapshots.reserve(result.eligibleSourceIndex->Records().size());
		sourceIndex.reserve(result.eligibleSourceIndex->Records().size());
		for(const NavigationEligibleSourceRecord& source : result.eligibleSourceIndex->Records()) {
			snapshots.push_back(source.geometry);
			sourceIndex.push_back(MakeSourceIndex(source));
		}
		NavigationGeneration terrainPublicationGeneration = 0U;
		for(const NavigationEligibleSourceRecord& source : result.eligibleSourceIndex->Records()) {
			if(source.identity.kind == NavigationSourceKind::Terrain) {
				terrainPublicationGeneration = std::max(
					terrainPublicationGeneration, source.revisions.physicalPublicationGeneration);
			}
		}

		// Profiles are project configuration values copied at capture. Route
		// admission independently resolves caller-supplied explicit/registered
		// profiles and never restores object-owned profile selection.
		std::vector<NavigationProfile> resolvedProfiles;
		AddProfile(resolvedProfiles, profiles.DefaultProfile());
		for(const NavigationProfileRegistration& registration : profiles.Registrations()) {
			AddProfile(resolvedProfiles, registration.profile);
		}
		for(const NavigationProfile& profile : resolvedProfiles) {
			if(!profile.Validate().Succeeded()) {
				result.reason = "A copied navigation profile is invalid.";
				result.builds.clear();
				return result;
			}
			result.builds.push_back({profile, snapshots, sourceIndex, result.eligibleSourceIndex,
				terrainPublicationGeneration});
		}
		result.accepted = !result.builds.empty();
		result.reason = result.accepted ? "Captured automatic terrain-backed navigation bootstrap inputs."
			: "No copied navigation profile is available for terrain-backed bootstrap.";
		return result;
	}
}
