// NavigationRuntimeSceneCapture.h - protected live-scene navigation capture.
//
// This is the only navigation integration boundary allowed to inspect the live
// ObjectManager and ResourceManager. Call it at a protected flush/publication
// edge, then pass only its returned immutable result to navigation workers.

#pragma once

#include "Navigation/Integration/NavigationSceneBootstrap.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Scene/SceneMutationQueue.h"

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

class ObjectManager;
class ResourceManager;

namespace TerraEngine::Navigation {
	class NavigationSystem;
	struct NavigationRuntimeSceneCaptureSettings {
		NavigationProfile defaultProfile = NavigationProfile::Defaults();
		std::vector<NavigationProfileRegistration> registeredProfiles;
		// Typed producers publish these complete immutable values at their own
		// protected boundary. Runtime capture copies them alongside scene geometry
		// and never accepts an untyped renderer or script pointer.
		std::vector<NavigationEligibleSourceCandidate> proceduralSources;
	};

	struct NavigationRuntimeSceneCapture {
		bool accepted = false;
		std::uint64_t sourceCaptureRevision = 0U;
		std::string reason;
		NavigationSceneBootstrapCapture bootstrap{};
		// These protected-boundary counters explain why an otherwise current terrain
		// collision publication did not become navigation coverage. They describe
		// only the copied capture attempt and never expose live ObjectManager state
		// to navigation workers.
		std::size_t terrainComponentsObserved = 0U;
		std::size_t terrainComponentsWithCanonicalPose = 0U;
		std::size_t terrainComponentsWithAsset = 0U;
		std::size_t terrainCandidatesWithCurrentPhysics = 0U;
		std::string terrainCandidateRejectionReason;
	};

	struct NavigationRuntimeSceneSubmission {
		bool accepted = false;
		std::size_t queuedProfileBuilds = 0U;
		std::string reason;
	};

	struct NavigationDynamicSourceReplacement {
		NavigationSourceIndexRecord source{};
		std::shared_ptr<const NavigationGeometrySnapshot> snapshot;
		// The transaction queue assigns this monotonically increasing value only
		// after commit. It binds a staged snapshot to one exact dirty receipt even
		// when content fingerprints repeat after a source moves back to an old pose.
		std::uint64_t committedChangeSequence = 0U;
	};

	struct NavigationCommittedSourceMutationCapture {
		// Receipts are normalized from the post-commit immutable source snapshot:
		// content hashes identify the replacement while queue sequences order it.
		std::vector<Scene::NavigationSourceMutationReceipt> receipts;
		std::vector<NavigationDynamicSourceReplacement> replacements;
		// A typed receipt lacking a safe static-collider replacement must fall back
		// to the automatic structural capture path. It is never partially staged.
		std::vector<TerraEngine::UUID> unhandledSourceIds;
	};

	// One protected host call owns source normalization, staged replacement
	// admission, receipt observation, and the conservative unavailable fallback.
	// `fenced` is true only when a committed source mutation could not be fully
	// represented by navigation; ordinary rejected scene batches never set it.
	struct NavigationCommittedSourceMutationObservation {
		std::size_t committedSourceCount = 0U;
		std::size_t forwardedReceiptCount = 0U;
		std::size_t observedReceiptCount = 0U;
		std::size_t unhandledSourceCount = 0U;
		bool staged = false;
		bool fenced = false;
	};

	// Captures active terrain, collider, mesh-companion, and typed procedural
	// physical values. It never queues worker work itself, so a caller can
	// revalidate the captured component revision before admitting the build.
	[[nodiscard]] TERRAENGINE_API NavigationRuntimeSceneCapture CaptureNavigationRuntimeScene(
		ObjectManager& objects, ResourceManager& resources,
		const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& terrainCollision,
		const NavigationRuntimeSceneCaptureSettings& settings = {});
	// Stable source identity is shared by full capture and the protected dynamic
	// bridge. It is value-only and never exposes a component pointer to workers.
	[[nodiscard]] TERRAENGINE_API TerraEngine::UUID MakeNavigationRuntimeSourceId(
		const TerraEngine::UUID& objectId, const TerraEngine::UUID& assetId,
		NavigationSourceKind kind, std::uint32_t subshapeIndex) noexcept;
	// Distinguishes real navigation-source membership/authored changes from the
	// global component token, which also advances for unrelated agents and UI.
	[[nodiscard]] TERRAENGINE_API bool NavigationAutomaticSourceStructureChanged(
		ObjectManager& objects, std::span<const NavigationSourceIndexRecord> sources);
	// Invoke only after the caller has compared sourceCaptureRevision with its last
	// admitted capture revision. The NavigationSystem copies every snapshot again
	// before returning, so no live ECS/resource state reaches worker execution.
	[[nodiscard]] TERRAENGINE_API NavigationRuntimeSceneSubmission SubmitNavigationRuntimeSceneCapture(
		NavigationSystem& navigation, const NavigationRuntimeSceneCapture& capture);
	// Examines only already-committed transactions at a protected boundary. Every
	// ordinary static-collider world-transform write receives a typed local source
	// receipt automatically; callers do not need a navigation tag or component.
	[[nodiscard]] TERRAENGINE_API NavigationCommittedSourceMutationCapture
	CaptureCommittedStaticColliderNavigationSourceMutations(
		ObjectManager& objects, Scene::SceneMutationQueue& mutations,
		std::span<const Scene::TransactionResult> committedTransactions,
		std::span<const Scene::NavigationSourceMutationReceipt> explicitReceipts);
	// Hosts call this only after SceneMutationQueue committed the supplied batch at
	// a protected boundary. It never mutates ObjectManager after the capture; the
	// returned data records whether navigation had to fence stale coverage and ask
	// the normal Frame End capture for one fresh immutable bootstrap packet.
	[[nodiscard]] TERRAENGINE_API NavigationCommittedSourceMutationObservation
	ObserveCommittedNavigationSourceMutations(NavigationSystem& navigation, ObjectManager& objects,
		Scene::SceneMutationQueue& mutations, const Scene::SceneMutationBatchResult& batch);
}
