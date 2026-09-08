// NavigationSceneBootstrap.cpp - provider-neutral scene bootstrap capture.

#include "Navigation/Integration/NavigationSceneBootstrap.h"

#include <algorithm>
#include <tuple>

namespace TerraEngine::Navigation {
	namespace {
		[[nodiscard]] bool IsMeshLike(NavigationSourceKind kind) noexcept {
			return kind == NavigationSourceKind::Auto || kind == NavigationSourceKind::Mesh
				|| kind == NavigationSourceKind::Collider;
		}

		void AddProfile(std::vector<NavigationProfile>& profiles, const NavigationProfile& profile) {
			const auto duplicate = std::find_if(profiles.begin(), profiles.end(), [&profile](const NavigationProfile& current) {
				return current.profileId == profile.profileId && current.revision == profile.revision;
			});
			if(duplicate == profiles.end()) profiles.push_back(profile);
		}

		[[nodiscard]] NavigationSourceIndexRecord MakeSourceIndex(
			const NavigationGeometrySnapshot& snapshot) {
			const NavigationGeometrySnapshotDescriptor& descriptor = snapshot.Descriptor();
			return {descriptor.sourceUuid.value, descriptor.sourceRevision,
				descriptor.sourceRevision, true};
		}
	}

	NavigationSceneBootstrapCapture CaptureNavigationSceneBootstrap(
		const NavigationSceneBootstrapInput& input) {
		NavigationSceneBootstrapCapture result{};
		const NavigationProfileResolver profiles = NavigationProfileResolver::Create(
			input.defaultProfile, input.registeredProfiles);
		if(!profiles.IsConfigurationValid()) {
			result.reason = "Navigation profile configuration is invalid or contains duplicate UUIDs.";
			return result;
		}

		std::vector<::NavigationSourceComponent> authoredSources;
		authoredSources.reserve(input.sources.size());
		for(const NavigationBootstrapSourceInput& source : input.sources) {
			if(source.component.IsPhysicallyEligible()) authoredSources.push_back(source.component);
		}
		const NavigationSourceValidationResult sourceValidation =
			::NavigationSourceComponent::ValidateSelection(authoredSources);
		if(!sourceValidation.accepted) {
			result.reason = sourceValidation.message;
			return result;
		}

		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> snapshots;
		std::vector<MeshColliderNavigationSourceInput> meshInputs;
		for(const NavigationBootstrapSourceInput& source : input.sources) {
			if(!source.component.IsPhysicallyEligible()) continue;
			const NavigationSourceKind kind = source.component.sourceKind;
			if(kind == NavigationSourceKind::Terrain) {
				if(!source.terrain) {
					result.reason = "Enabled terrain navigation source is missing physically published terrain samples.";
					return result;
				}
				if(source.terrain->descriptor.sourceUuid.value != source.component.sourceId
					|| source.terrain->descriptor.sourceRevision != source.component.geometryRevision) {
					result.reason = "Terrain packet identity does not match its authored navigation source component.";
					return result;
				}
				const TerrainNavigationCaptureResult terrain = CaptureTerrainNavigationSource(*source.terrain);
				if(!terrain.accepted) {
					result.reason = "Terrain navigation capture rejected: " + terrain.reason;
					return result;
				}
				if(terrain.snapshot) snapshots.push_back(terrain.snapshot);
				continue;
			}
			if(!IsMeshLike(kind)) {
				result.reason = "Enabled navigation source has no matching immutable mesh or collider packet.";
				return result;
			}
			if(source.immutableGeometry) {
				const NavigationGeometrySnapshotDescriptor& descriptor = source.immutableGeometry->Descriptor();
				if(descriptor.sourceUuid.value != source.component.sourceId
					|| descriptor.sourceRevision != source.component.geometryRevision) {
					result.reason = "Registered immutable geometry identity does not match its authored navigation source component.";
					return result;
				}
				snapshots.push_back(source.immutableGeometry);
				continue;
			}
			if(!source.meshOrCollider) {
				result.reason = "Enabled navigation source has no matching immutable mesh or collider packet.";
				return result;
			}
			if(source.meshOrCollider->descriptor.sourceUuid.value != source.component.sourceId
				|| source.meshOrCollider->descriptor.sourceRevision != source.component.geometryRevision) {
				result.reason = "Mesh/collider packet identity does not match its authored navigation source component.";
				return result;
			}
			meshInputs.push_back(*source.meshOrCollider);
		}

		if(!meshInputs.empty()) {
			const MeshColliderNavigationCaptureResult mesh = CaptureMeshColliderNavigationSources(meshInputs);
			if(!mesh.accepted) {
				result.reason = "Mesh/collider navigation capture rejected: " + mesh.reason;
				return result;
			}
			for(const MeshColliderNavigationSourceSnapshot& captured : mesh.sources) {
				if(captured.geometry) snapshots.push_back(captured.geometry);
			}
		}
		if(snapshots.empty()) {
			result.reason = "No eligible navigation geometry was captured from the published scene.";
			return result;
		}

		std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right) {
			return left->Descriptor().sourceUuid.value < right->Descriptor().sourceUuid.value;
		});
		for(std::size_t index = 1U; index < snapshots.size(); ++index) {
			if(snapshots[index - 1U]->Descriptor().sourceUuid == snapshots[index]->Descriptor().sourceUuid) {
				result.reason = "Navigation capture produced duplicate immutable source UUIDs.";
				return result;
			}
		}

		std::vector<NavigationProfile> resolvedProfiles;
		AddProfile(resolvedProfiles, profiles.DefaultProfile());
		for(const NavigationBootstrapAgentInput& agent : input.agents) {
			NavigationBootstrapAgentResolution resolution{};
			resolution.objectId = agent.objectId;
			if(!agent.component.enabled) {
				resolution.resolution = {false, NavigationProfileResolutionCode::InvalidAgent, {},
					"NavigationAgentComponent is disabled."};
			} else {
				resolution.resolution = profiles.Resolve(agent.component);
				if(resolution.resolution.accepted) AddProfile(resolvedProfiles, resolution.resolution.profile);
			}
			result.agents.push_back(std::move(resolution));
		}

		std::vector<NavigationSourceIndexRecord> sourceIndex;
		sourceIndex.reserve(snapshots.size());
		for(const auto& snapshot : snapshots) sourceIndex.push_back(MakeSourceIndex(*snapshot));
		for(const NavigationProfile& profile : resolvedProfiles) {
			if(!profile.Validate().Succeeded()) {
				result.reason = "A resolved navigation profile is invalid.";
				result.builds.clear();
				return result;
			}
			result.builds.push_back({profile, snapshots, sourceIndex});
		}
		result.accepted = !result.builds.empty();
		result.reason = result.accepted ? "Captured immutable navigation bootstrap inputs." : "No valid navigation profile was resolved.";
		return result;
	}
}
