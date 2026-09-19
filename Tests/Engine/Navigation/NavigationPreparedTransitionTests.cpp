// NavigationPreparedTransitionTests.cpp - prepared-route rebase regressions.

#include "TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/Integration/NavigationRuntimeSceneCapture.h"
#include "Navigation/NavigationSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {

	namespace {
		using namespace TerraEngine::Navigation;
		using namespace TerraEngine::Scheduling;

		// Build a copied, provider-neutral location. The regression deliberately avoids
		// ObjectManager so worker preparation cannot accidentally read live ECS state.
		[[nodiscard]] NavigationLocation Location(NavigationWorldHandle world, double x, double z = 0.0) {
			NavigationLocation result{};
			result.world = world;
			result.position.offsetMetres = {x, 0.0, z};
			return result;
		}

		// The terrain supplies a small, complete support plane. The mesh is a finite
		// copied source whose receipt bounds provide the dynamic swept-area certificate.
		[[nodiscard]] NavigationEligibleSourceCandidate Source(
			NavigationSourceKind kind, NavigationGeneration revision, double originX,
			std::shared_ptr<const NavigationGeometrySnapshot>& geometry,
			const NavigationEligibleSourceIdentity* retainedIdentity = nullptr) {
			NavigationEligibleSourceCandidate candidate{};
			if(retainedIdentity != nullptr) candidate.identity = *retainedIdentity;
			else {
				candidate.identity.sourceId = GenerateUUID();
				candidate.identity.objectId = GenerateUUID();
				candidate.identity.objectLifetimeGeneration = 1U;
				candidate.identity.assetId = GenerateUUID();
				candidate.identity.kind = kind;
			}
			candidate.revisions = {1U, revision, 1U, 1U, 1U};

			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			if(kind == NavigationSourceKind::Terrain) {
				document.vertices = {{-32.0F, 0.0F, -32.0F}, {32.0F, 0.0F, 32.0F},
					{32.0F, 0.0F, -32.0F}, {-32.0F, 0.0F, 32.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			}
			else {
				document.vertices = {{-1.0F, 0.0F, -1.0F}, {1.0F, 2.0F, -1.0F},
					{1.0F, 0.0F, 1.0F}, {-1.0F, 2.0F, 1.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 2U, 3U}};
			}

			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceUuid.value = candidate.identity.sourceId;
			descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(
				candidate.identity, candidate.revisions);
			descriptor.canonicalPlacementRevision = revision;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = kind;
			descriptor.canonicalFrameOrigin.offsetMetres.x = originX;
			geometry = NavigationGeometrySnapshot::Create(descriptor, std::move(document)).snapshot;
			candidate.geometry = geometry;
			candidate.activeInHierarchy = true;
			candidate.physicalBackingCurrent = true;
			return candidate;
		}

		// This support is deliberately split by a gap wider than the default
		// capsule, but remains wholly inside one positive canonical shard. Both
		// endpoints remain on valid ground, so NearestReachable may publish a Partial
		// corridor on the left island without a cross-shard portal failure masking the
		// provider result.
		[[nodiscard]] NavigationEligibleSourceCandidate DisconnectedTerrainSource(
			NavigationGeneration revision, std::shared_ptr<const NavigationGeometrySnapshot>& geometry) {
			NavigationEligibleSourceCandidate candidate = Source(
				NavigationSourceKind::Terrain, revision, 0.0, geometry);
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {
				{0.0F, 0.0F, 0.0F}, {30.0F, 0.0F, 64.0F},
				{30.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 64.0F},
				{34.0F, 0.0F, 0.0F}, {64.0F, 0.0F, 64.0F},
				{64.0F, 0.0F, 0.0F}, {34.0F, 0.0F, 64.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U},
				{4U, 5U, 6U}, {4U, 7U, 5U}};
			geometry = NavigationGeometrySnapshot::Create(
				candidate.geometry->Descriptor(), std::move(document)).snapshot;
			candidate.geometry = geometry;
			return candidate;
		}

		// Runtime capture accepts analytic sphere colliders as valid immutable geometry.
		// Recast deliberately supports only boxes today, so this is a deterministic
		// provider capability failure rather than a synthetic invalid-packet hook.
		[[nodiscard]] NavigationEligibleSourceCandidate SphereColliderSource(
			NavigationGeneration revision, double originX,
			std::shared_ptr<const NavigationGeometrySnapshot>& geometry,
			const NavigationEligibleSourceIdentity* retainedIdentity = nullptr) {
			NavigationEligibleSourceCandidate candidate = Source(NavigationSourceKind::Collider,
				revision, originX, geometry, retainedIdentity);
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			NavigationGeometryPrimitive sphere{};
			sphere.kind = NavigationGeometryPrimitiveKind::Sphere;
			sphere.radius = 1.0F;
			document.primitives.push_back(sphere);
			geometry = NavigationGeometrySnapshot::Create(
				candidate.geometry->Descriptor(), std::move(document)).snapshot;
			candidate.geometry = geometry;
			return candidate;
		}

		[[nodiscard]] NavigationSystem::InitialGroundBuildInput Build(
			NavigationProfile profile, const NavigationEligibleSourceCandidate& terrain,
			const NavigationEligibleSourceCandidate& changed) {
			const std::array<NavigationEligibleSourceCandidate, 2U> candidates{{terrain, changed}};
			NavigationEligibleSourceIndexBuildResult indexed = BuildNavigationEligibleSourceIndex(candidates);
			NavigationSystem::InitialGroundBuildInput build{};
			build.profile = std::move(profile);
			build.sources = {terrain.geometry, changed.geometry};
			build.sourceIndex = {{terrain.identity.sourceId, terrain.geometry->Descriptor().sourceRevision,
				terrain.revisions.geometryRevision, true, terrain.identity, terrain.revisions},
				{changed.identity.sourceId, changed.geometry->Descriptor().sourceRevision,
				changed.revisions.geometryRevision, true, changed.identity, changed.revisions}};
			build.eligibleSourceIndex = indexed.accepted
				? std::make_shared<const NavigationEligibleSourceIndex>(std::move(indexed.index)) : nullptr;
			build.terrainPublicationGeneration = 1U;
			return build;
		}

		[[nodiscard]] const FrameWorkloadDeclaration* Workload(
			const std::vector<FrameWorkloadDeclaration>& workloads, std::string_view id) {
			const auto found = std::find_if(workloads.begin(), workloads.end(), [id](const auto& workload) {
				return workload.stableDescriptorId == id;
			});
			return found == workloads.end() ? nullptr : &*found;
		}

		// Capture and prepare exactly one route, but intentionally stop before its
		// protected publish. This is the scheduler-visible interleaving a real scene
		// receipt can hit when a worker search completes alongside a later mutation.
		[[nodiscard]] bool PrepareRouteWithoutPublishing(NavigationSystem& system) {
			system.SetContractTestWorkAvailable(true);
			const std::vector<FrameWorkloadDeclaration> workloads = system.DeclareFrameWorkloads(0.0F);
			const FrameWorkloadDeclaration* capture = Workload(workloads,
				"NavigationSystem/Navigation/Capture");
			const FrameWorkloadDeclaration* prepare = Workload(workloads,
				"NavigationSystem/Navigation/Prepare");
			if(capture == nullptr || prepare == nullptr || !capture->work || !prepare->rangeWork) return false;

			JobGraphContext context{};
			capture->work(context);
			prepare->rangeWork({.partitionIndex = 0U, .partitionCount = 1U,
				.itemBegin = 0U, .itemEnd = 1U}, context);
			return !context.IsCancellationRequested();
		}

		// Publish only the already captured route packet. It intentionally omits a
		// second capture/prepare pass, leaving the queued automatic replacement
		// staged but its root unexchanged—the precise protected-boundary window that
		// must reject both Complete and Partial guidance crossing changed geometry.
		[[nodiscard]] bool PublishPreparedRoutesWithoutPreparingGround(NavigationSystem& system) {
			const std::vector<FrameWorkloadDeclaration> workloads = system.DeclareFrameWorkloads(0.0F);
			const FrameWorkloadDeclaration* publish = Workload(workloads,
				"NavigationSystem/Navigation/Publish");
			if(publish == nullptr || !publish->work) return false;
			JobGraphContext context{};
			publish->work(context);
			return !context.IsCancellationRequested();
		}

		// Scheduler-path dynamic packets require a real pool, while most native tests
		// deliberately run the synchronous compatibility path. Own only a pool this
		// fixture started so neighbouring tests retain their lifecycle authority.
		class NavigationWorkerPoolSession final {
		public:
			NavigationWorkerPoolSession() {
				if(ThreadPool::instance().getThreadCount() == 0U) {
					ThreadPool::instance().initialize(1U);
					ownsPool_ = true;
				}
			}

			~NavigationWorkerPoolSession() {
				if(ownsPool_) ThreadPool::instance().shutdown();
			}

			NavigationWorkerPoolSession(const NavigationWorkerPoolSession&) = delete;
			NavigationWorkerPoolSession& operator=(const NavigationWorkerPoolSession&) = delete;

		private:
			bool ownsPool_ = false;
		};

		// Run exactly the protected capture edge. Keeping publication separate exposes
		// the real completed-but-not-yet-published window without a test-only system API.
		[[nodiscard]] bool CaptureNavigationWorkWithoutPublishing(NavigationSystem& system) {
			system.SetContractTestWorkAvailable(true);
			const std::vector<FrameWorkloadDeclaration> workloads = system.DeclareFrameWorkloads(0.0F);
			const FrameWorkloadDeclaration* capture = Workload(workloads,
				"NavigationSystem/Navigation/Capture");
			if(capture == nullptr || !capture->work) return false;
			JobGraphContext context{};
			capture->work(context);
			return !context.IsCancellationRequested();
		}

		[[nodiscard]] bool PublishNavigationWorkWithoutCapturing(NavigationSystem& system) {
			const std::vector<FrameWorkloadDeclaration> workloads = system.DeclareFrameWorkloads(0.0F);
			const FrameWorkloadDeclaration* publish = Workload(workloads,
				"NavigationSystem/Navigation/Publish");
			if(publish == nullptr || !publish->work) return false;
			JobGraphContext context{};
			publish->work(context);
			return !context.IsCancellationRequested();
		}

		[[nodiscard]] std::uint64_t DynamicBuildCompletionCount(const NavigationSystem& system) {
			const std::shared_ptr<const NavigationDiagnosticSnapshot> diagnostics = system.PublishedDiagnostics();
			return diagnostics ? diagnostics->asyncBuildDynamicCompletionCount : 0U;
		}

		[[nodiscard]] std::uint64_t GroundBuildCompletionCount(const NavigationSystem& system) {
			const std::shared_ptr<const NavigationDiagnosticSnapshot> diagnostics = system.PublishedDiagnostics();
			return diagnostics ? diagnostics->asyncBuildGroundCompletionCount : 0U;
		}

		[[nodiscard]] bool CaptureUntilDynamicCompletion(NavigationSystem& system,
			std::uint64_t expectedCompletionCount) {
			// Yielding polls a real detached worker completion; it never advances a
			// synthetic clock or sleeps long enough to disguise scheduling regressions.
			// Use a bounded elapsed-time deadline rather than an iteration count: a
			// single-worker host can legitimately need more than 10,000 scheduler polls
			// while Recast finishes a copied provider packet.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do {
				if(!CaptureNavigationWorkWithoutPublishing(system)) return false;
				if(DynamicBuildCompletionCount(system) >= expectedCompletionCount) return true;
				std::this_thread::yield();
			} while(std::chrono::steady_clock::now() < deadline);
			return DynamicBuildCompletionCount(system) >= expectedCompletionCount;
		}

		[[nodiscard]] bool CaptureUntilGroundCompletion(NavigationSystem& system,
			std::uint64_t expectedCompletionCount) {
			// This mirrors the dynamic completion poll above, but leaves Publish untouched.
			// The resulting completed-ground window is a real protected-boundary state, not
			// a test-only scheduler injection, and is the ordering edge this regression owns.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do {
				if(!CaptureNavigationWorkWithoutPublishing(system)) return false;
				if(GroundBuildCompletionCount(system) >= expectedCompletionCount) return true;
				std::this_thread::yield();
			} while(std::chrono::steady_clock::now() < deadline);
			return GroundBuildCompletionCount(system) >= expectedCompletionCount;
		}

		[[nodiscard]] NavigationRequestHandle SubmitRoute(
			NavigationSystem& system, NavigationWorldHandle world, std::uint64_t owner,
			double start, double destination, bool allowPartial = false,
			const NavigationProfile& profile = NavigationProfile::Defaults()) {
			Ground::GroundRouteQuery::Request request{};
			request.owner = {world, NavigationOwnerId{owner}, 1U};
			request.start = Location(world, start);
			request.destination = Location(world, destination);
			request.profile = profile;
			request.allowPartial = allowPartial;
			request.maxExpansions = 32'768U;
			return system.SubmitOneShotRoute(std::move(request));
		}

		// A distinct, still walkable profile gives the on-demand path a separate
		// coverage key without changing the simple flat-terrain fixture's topology.
		[[nodiscard]] NavigationProfile AlternateProfile() {
			NavigationProfile profile = NavigationProfile::Defaults();
			profile.heightMetres += 0.1;
			return profile;
		}

		[[nodiscard]] NavigationProfile ProfileVariant(std::uint32_t ordinal) {
			NavigationProfile profile = NavigationProfile::Defaults();
			// Height is part of the profile build key while this flat-terrain fixture
			// remains traversable for every variant. This lets one receipt exercise a
			// real multi-profile group without inventing provider-specific test input.
			profile.heightMetres += static_cast<double>(ordinal) * 0.1;
			return profile;
		}

		// Direct Update is the documented synchronous compatibility lifecycle for
		// these small native fixtures. Bound the loop so a broken recovery cannot
		// make the regression test wait indefinitely.
		[[nodiscard]] bool DriveUntilTerminal(NavigationSystem& system,
			NavigationRequestHandle request, std::size_t maximumFrames = 8U) {
			for(std::size_t frame = 0U; frame < maximumFrames; ++frame) {
				const auto read = system.ReadRequest(request);
				if(read.record && read.record->lifecycle == NavigationRequestLifecycle::Terminal) return true;
				system.Update(0.0F);
			}
			const auto read = system.ReadRequest(request);
			return read.record && read.record->lifecycle == NavigationRequestLifecycle::Terminal;
		}

		[[nodiscard]] const NavigationSourceIndexRecord* FindSource(
			const NavigationSystem::InitialGroundBuildInput& build, const TerraEngine::UUID& sourceId) {
			const auto found = std::find_if(build.sourceIndex.begin(), build.sourceIndex.end(),
				[&sourceId](const NavigationSourceIndexRecord& source) { return source.sourceId == sourceId; });
			return found == build.sourceIndex.end() ? nullptr : &*found;
		}

		// Submit a complete staged replacement and its matching committed receipt. The
		// source row and copied snapshot must agree before the dynamic root can change.
		[[nodiscard]] bool CommitReplacement(NavigationSystem& system,
			const NavigationSystem::InitialGroundBuildInput& build,
			const NavigationEligibleSourceCandidate& replacement,
			std::shared_ptr<const NavigationGeometrySnapshot> snapshot,
			NavigationGeneration sequence, double oldMinimum, double oldMaximum,
			double newMinimum, double newMaximum) {
			const NavigationSourceIndexRecord* source = FindSource(build, replacement.identity.sourceId);
			if(source == nullptr || snapshot == nullptr) return false;

			NavigationSystem::DynamicGroundSourceReplacement staged{};
			staged.source = *source;
			staged.snapshot = std::move(snapshot);
			staged.committedChangeSequence = sequence;
			if(!system.StageDynamicGroundSourceReplacements({std::move(staged)})) return false;

			Scene::NavigationSourceMutationReceipt receipt{};
			receipt.accepted = true;
			receipt.transactionStatus = Scene::TransactionStatus::Committed;
			receipt.intent.sourceId = replacement.identity.sourceId;
			receipt.intent.operation = Scene::NavigationSourceMutationOperation::Move;
			receipt.intent.oldBounds = {{{0, 0, 0}, {oldMinimum, -1.0, -2.0}},
				{{0, 0, 0}, {oldMaximum, 3.0, 2.0}}, true};
			receipt.intent.newBounds = {{{0, 0, 0}, {newMinimum, -1.0, -2.0}},
				{{0, 0, 0}, {newMaximum, 3.0, 2.0}}, true};
			receipt.intent.committedSourceRevision = source->sourceRevision;
			receipt.intent.committedChangeSequence = sequence;
			receipt.intent.physicalReadyRevision = 1U;
			receipt.intent.oldEnabled = true;
			receipt.intent.newEnabled = true;
			return system.ObserveSceneMutationReceipts(
				std::span<const Scene::NavigationSourceMutationReceipt>{&receipt, 1U}) == 1U;
		}

		[[nodiscard]] bool IsTerminal(const NavigationSystem& system, NavigationRequestHandle request,
			NavigationOutcome outcome, NavigationReason reason) {
			const auto read = system.ReadRequest(request);
			return read.record.has_value() && read.record->lifecycle == NavigationRequestLifecycle::Terminal
				&& read.record->outcome == outcome && read.record->reason == reason;
		}

		// A real typed scene transform must be committed before the host bridge sees it.
		// This helper deliberately uses an ordinary static collider which is absent from
		// the synthetic navigation root, so bridge admission must fence that old root
		// rather than treating the newly committed source as safely represented.
		[[nodiscard]] bool CommitStaticColliderTransform(
			ObjectManager& objects, Scene::SceneMutationQueue& mutations, const Object& object,
			double destinationX, Scene::SceneMutationBatchResult& batch) {
			const std::optional<Scene::WorldPose> currentPose = objects.GetCanonicalWorldPose(object);
			if(!currentPose) return false;
			Scene::WorldPose destination = *currentPose;
			destination.position.offsetMetres.x = destinationX;
			Scene::TransactionalMutationBuilder transaction(
				"navigation-unavailable-fence", Scene::TransactionOrigin::Test, "navigation-test");
			transaction.SetWorldTransform(objects, objects.getUUID(object), destination);
			mutations.EnqueueTransaction(transaction.Build());
			batch = mutations.Apply(objects, "navigation-unavailable-fence");
			return batch.applied && !batch.transactionResults.empty()
				&& batch.transactionResults.front().status == Scene::TransactionStatus::Committed;
		}

		[[nodiscard]] bool InitializeFixture(NavigationSystem& system,
			NavigationEligibleSourceCandidate& terrain, NavigationEligibleSourceCandidate& changed,
			std::shared_ptr<const NavigationGeometrySnapshot>& terrainGeometry,
			std::shared_ptr<const NavigationGeometrySnapshot>& changedGeometry,
			NavigationWorldHandle& world) {
			system.Shutdown();
			system.Initialize();
			terrain = Source(NavigationSourceKind::Terrain, 1U, 0.0, terrainGeometry);
			changed = Source(NavigationSourceKind::Mesh, 1U, -10.0, changedGeometry);
			const NavigationSystem::InitialGroundBuildInput initial = Build(
				NavigationProfile::Defaults(), terrain, changed);
			if(terrainGeometry == nullptr || changedGeometry == nullptr || initial.eligibleSourceIndex == nullptr
				|| !system.QueueInitialGroundBuilds({initial})) return false;
			system.Update(0.0F);
			const std::shared_ptr<const NavigationWorldRoot> root = system.PublishedWorld();
			if(!root) return false;
			world = root->world;
			return true;
		}

		// The terrain crosses the canonical shard boundary between tiles 7 and 8.
		// A narrow changed mesh remains in tile 7, letting admission distinguish an
		// endpoint in Dirty shard 0 from an unrelated endpoint in Ready shard 1.
		[[nodiscard]] bool InitializeEndpointAdmissionFixture(NavigationSystem& system,
			NavigationEligibleSourceCandidate& terrain, NavigationEligibleSourceCandidate& changed,
			std::shared_ptr<const NavigationGeometrySnapshot>& terrainGeometry,
			std::shared_ptr<const NavigationGeometrySnapshot>& changedGeometry,
			NavigationWorldHandle& world, NavigationSourceKind changedKind = NavigationSourceKind::Mesh) {
			system.Shutdown();
			system.Initialize();
			terrain = Source(NavigationSourceKind::Terrain, 1U, 256.0, terrainGeometry);
			changed = Source(changedKind, 1U, 252.0, changedGeometry);
			const NavigationSystem::InitialGroundBuildInput initial = Build(
				NavigationProfile::Defaults(), terrain, changed);
			if(terrainGeometry == nullptr || changedGeometry == nullptr || initial.eligibleSourceIndex == nullptr
				|| !system.QueueInitialGroundBuilds({initial})) return false;
			system.Update(0.0F);
			const std::shared_ptr<const NavigationWorldRoot> root = system.PublishedWorld();
			if(!root) return false;
			world = root->world;
			return true;
		}

		[[nodiscard]] bool HasCoverageState(const std::shared_ptr<const NavigationWorldRoot>& root,
			const NavigationProfile& profile, std::int64_t shardX, NavigationCoverageState state) {
			if(!root) return false;
			const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
			return std::any_of(root->coverage.begin(), root->coverage.end(),
				[profileBuildKey, shardX, state](const NavigationCoverageRecord& coverage) {
					return coverage.key.profileBuildKey == profileBuildKey && coverage.key.shardX == shardX
						&& coverage.key.shardZ == 0LL && coverage.key.verticalBand == 0
						&& coverage.state == state
						// Ready is useful to route admission only when its shard also carries
						// a complete provider result; Dirty intentionally has no such claim.
						&& (state != NavigationCoverageState::Ready || coverage.complete);
				});
		}

		[[nodiscard]] bool IsDeferredForDirtyCoverage(const NavigationSystem& system,
			NavigationRequestHandle request) {
			const auto read = system.ReadRequest(request);
			return read.record.has_value()
				&& read.record->lifecycle == NavigationRequestLifecycle::PendingPreparation
				&& read.record->outcome == NavigationOutcome::PendingPreparation
				&& read.record->reason == NavigationReason::DirtyCoverage;
		}

		[[nodiscard]] std::uint64_t Counter(const NavigationSystem& system,
			std::uint64_t NavigationDiagnosticSnapshot::* member) {
			const std::shared_ptr<const NavigationDiagnosticSnapshot> diagnostics = system.PublishedDiagnostics();
			return diagnostics ? diagnostics.get()->*member : 0U;
		}

		// Dynamic counters become externally observable at a declared protected
		// boundary. Tests publish that snapshot explicitly instead of depending on an
		// incidental later frame or a scheduler diagnostic side effect.
		void PublishDiagnostics(NavigationSystem& system) {
			system.ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd, 1U);
		}
	}

	void RegisterNavigationPreparedTransitionTests(TestRegistry& registry) {
		registry.Add("Navigation.Dynamic.PreparedTransition.UsesRouteSegmentsForBentCorridors", [](TestContext& context) {
			const NavigationWorldHandle world{};
			const NavigationProfile profile = NavigationProfile::Defaults();
			const NavigationGeometryBounds changedBounds{{4.5F, -1.0F, 4.5F}, {5.5F, 1.0F, 5.5F}};
			const std::array<NavigationLocation, 3U> bentRoute{{
				Location(world, 0.0, 0.0), Location(world, 0.0, 10.0), Location(world, 10.0, 10.0)}};
			const std::array<NavigationLocation, 2U> crossingRoute{{
				Location(world, 0.0, 0.0), Location(world, 10.0, 10.0)}};
			const NavigationGeometryBounds invertedBounds{{6.0F, -1.0F, 4.5F}, {5.0F, 1.0F, 5.5F}};

			// The old whole-route AABB covered the central box for both routes. Only the
			// diagonal actually enters the profile-expanded changed geometry; the bent
			// corridor must remain available across a local dynamic publication.
			const bool bentIntersects = RouteSegmentsIntersectLocalizedBounds(
				bentRoute, profile, Scene::WorldPosition{}, changedBounds);
			const bool crossingIntersects = RouteSegmentsIntersectLocalizedBounds(
				crossingRoute, profile, Scene::WorldPosition{}, changedBounds);
			const bool invertedIntersects = RouteSegmentsIntersectLocalizedBounds(
				bentRoute, profile, Scene::WorldPosition{}, invertedBounds);
			context.Expect(!bentIntersects && crossingIntersects && invertedIntersects,
				"Dynamic route certificates must reject crossings and malformed bounds while preserving a bent corridor whose enclosing AABB alone overlaps changed geometry.");
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.RebasesAcrossAdditiveProfilePublication", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const std::shared_ptr<const NavigationWorldRoot> rootBefore = system.PublishedWorld();
			const NavigationRequestHandle preparedRoute = initialized
				? SubmitRoute(system, world, 70U, 8.0, 16.0) : NavigationRequestHandle{};
			const bool prepared = preparedRoute.IsValid() && PrepareRouteWithoutPublishing(system);
			const NavigationProfile alternate = AlternateProfile();
			// This request is the ordinary public on-demand profile admission. It creates
			// only pB coverage while the pA route remains prepared against root X.
			const NavigationRequestHandle alternateRoute = prepared
				? SubmitRoute(system, world, 71U, 8.0, 16.0, false, alternate) : NavigationRequestHandle{};
			const auto alternateBefore = system.ReadRequest(alternateRoute);
			const bool alternateDeferred = alternateBefore.record.has_value()
				&& alternateBefore.record->lifecycle == NavigationRequestLifecycle::PendingPreparation
				&& alternateBefore.record->outcome == NavigationOutcome::PendingPreparation;
			const std::uint64_t missingLinkBefore = Counter(system,
				&NavigationDiagnosticSnapshot::preparedRouteTransitionMissingLinkCount);
			// Ground publication precedes prepared-route publication within the normal
			// compatibility boundary. Therefore this call exercises pA's rebase from X to
			// X+1 after pB's root-plus-portal tuple becomes authoritative.
			if(alternateRoute.IsValid()) system.Update(0.0F);
			PublishDiagnostics(system);

			const std::shared_ptr<const NavigationWorldRoot> rootAfter = system.PublishedWorld();
			const NavigationRouteHandle route = system.PublishedRouteForRequest(preparedRoute);
			const auto observation = system.ReadCurrentRouteObservation(route);
			const bool pARebased = IsTerminal(system, preparedRoute, NavigationOutcome::Complete, NavigationReason::None)
				&& route.IsValid() && system.ValidateRoute(route).usable && observation.has_value()
				&& rootBefore != nullptr && rootAfter != nullptr
				&& observation->worldPublicationGeneration == rootAfter->rootGeneration
				&& rootAfter->rootGeneration > rootBefore->rootGeneration;
			const bool profilePublicationWasCertified = Counter(system,
				&NavigationDiagnosticSnapshot::preparedRouteTransitionRebasedCount) > 0U
				&& Counter(system, &NavigationDiagnosticSnapshot::preparedRouteTransitionMissingLinkCount)
					== missingLinkBefore;
			context.Expect(initialized && prepared && alternateRoute.IsValid() && alternateDeferred
				&& pARebased && profilePublicationWasCertified,
				"Adding a distinct on-demand profile must certify an unchanged prepared pA route across the new root instead of reporting a missing transition link.");
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.RebasesPreparedRouteOutsideChangedCoverage", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const NavigationRequestHandle request = initialized ? SubmitRoute(system, world, 1U, 8.0, 16.0)
				: NavigationRequestHandle{};
			const bool prepared = request.IsValid() && PrepareRouteWithoutPublishing(system);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(
				NavigationProfile::Defaults(), terrain, replacement);
			const bool committed = prepared && CommitReplacement(system, replacementBuild, replacement,
				replacementGeometry, 101U, -12.0, -8.0, -10.0, -6.0);
			// Compatibility Update runs the same dynamic worker/publish code after the
			// scheduler handoff above; it completes the profile rebuild before route publish.
			if(committed) system.Update(0.0F);
			PublishDiagnostics(system);

			const NavigationRouteHandle route = system.PublishedRouteForRequest(request);
			const bool completeAndCurrent = IsTerminal(system, request, NavigationOutcome::Complete, NavigationReason::None)
				&& route.IsValid() && system.ValidateRoute(route).usable;
			const bool rebased = Counter(system, &NavigationDiagnosticSnapshot::preparedRouteTransitionRebasedCount) > 0U;
			context.Expect(initialized && prepared && committed && completeAndCurrent && rebased,
				"A complete prepared route outside every old/new changed bound must rebase after the receipt and profile rebuild.");
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.StalesPreparedRouteCrossingChangedCoverage", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const NavigationRequestHandle request = initialized ? SubmitRoute(system, world, 2U, -14.0, -4.0)
				: NavigationRequestHandle{};
			const bool prepared = request.IsValid() && PrepareRouteWithoutPublishing(system);
			const std::uint64_t rebasedBefore = Counter(system,
				&NavigationDiagnosticSnapshot::preparedRouteTransitionRebasedCount);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(
				NavigationProfile::Defaults(), terrain, replacement);
			const bool committed = prepared && CommitReplacement(system, replacementBuild, replacement,
				replacementGeometry, 102U, -12.0, -8.0, -10.0, -6.0);
			if(committed) system.Update(0.0F);
			PublishDiagnostics(system);

			const NavigationRouteHandle route = system.PublishedRouteForRequest(request);
			const bool staleAndUnusable = IsTerminal(system, request, NavigationOutcome::Stale, NavigationReason::DirtyCoverage)
				&& !route.IsValid();
			const bool intersected = Counter(system, &NavigationDiagnosticSnapshot::preparedRouteTransitionIntersectedCount) > 0U;
			const bool noAdditiveProfileExemption = Counter(system,
				&NavigationDiagnosticSnapshot::preparedRouteTransitionRebasedCount) == rebasedBefore;
			context.Expect(initialized && prepared && committed && staleAndUnusable && intersected
				&& noAdditiveProfileExemption,
				"A same-profile replacement that changes the route's topology must not receive the additive-profile certificate; it must fail closed as stale and never publish guidance.");
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.StalesPublishablePartialAcrossStagedAutomaticReplacement", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			const NavigationEligibleSourceCandidate terrain = DisconnectedTerrainSource(1U, terrainGeometry);
			const NavigationEligibleSourceCandidate changed = Source(
				NavigationSourceKind::Mesh, 1U, 22.0, changedGeometry);
			const NavigationSystem::InitialGroundBuildInput initial = Build(
				NavigationProfile::Defaults(), terrain, changed);
			const bool initialized = terrainGeometry != nullptr && changedGeometry != nullptr
				&& initial.eligibleSourceIndex != nullptr && system.QueueInitialGroundBuilds({initial});
			if(initialized) system.Update(0.0F);
			const std::shared_ptr<const NavigationWorldRoot> beforeReplacement = system.PublishedWorld();
			const NavigationWorldHandle world = beforeReplacement ? beforeReplacement->world : NavigationWorldHandle{};

			// Prove the same fixture produces a real provider Partial before staging an
			// automatic replacement. This prevents an unavailable shard/portal from
			// making the staged assertion pass for the wrong terminal reason.
			const NavigationRequestHandle control = beforeReplacement
				? SubmitRoute(system, world, 50U, 12.0, 52.0, true) : NavigationRequestHandle{};
			const bool controlPrepared = control.IsValid() && PrepareRouteWithoutPublishing(system);
			const bool controlPublished = controlPrepared && PublishPreparedRoutesWithoutPreparingGround(system);
			const NavigationStoreReadResult<NavigationRequestRecord> controlRead = system.ReadRequest(control);
			const NavigationRouteHandle controlRoute = system.PublishedRouteForRequest(control);
			const bool controlIsPartial = controlRead.record.has_value()
				&& controlRead.record->outcome == NavigationOutcome::Partial
				&& controlRoute.IsValid() && system.ValidateRoute(controlRoute).usable;

			// The left-island corridor crosses the mesh's old/new swept bounds. It is
			// publishable without the staged replacement and must be stale with it.
			const NavigationRequestHandle request = controlIsPartial
				? SubmitRoute(system, world, 51U, 12.0, 52.0, true) : NavigationRequestHandle{};
			const bool prepared = request.IsValid() && PrepareRouteWithoutPublishing(system);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, 24.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(
				NavigationProfile::Defaults(), terrain, replacement);
			// Queuing is sufficient to establish automatic replacement evidence. Do not
			// run Update: this verifies the earlier same-root publication boundary.
			const bool staged = prepared && replacementGeometry != nullptr
				&& replacementBuild.eligibleSourceIndex != nullptr
				&& system.QueueInitialGroundBuilds({replacementBuild});
			const bool published = staged && PublishPreparedRoutesWithoutPreparingGround(system);
			const std::shared_ptr<const NavigationWorldRoot> afterRoutePublish = system.PublishedWorld();
			const NavigationRouteHandle route = system.PublishedRouteForRequest(request);
			const NavigationStoreReadResult<NavigationRequestRecord> requestRead = system.ReadRequest(request);
			const bool staleAndUnpublished = requestRead.record.has_value()
				&& requestRead.record->outcome == NavigationOutcome::Stale
				&& requestRead.record->reason == NavigationReason::DirtyCoverage && !route.IsValid();
			const std::string failureDetail = " (initialized=" + std::to_string(initialized)
				+ ", controlPrepared=" + std::to_string(controlPrepared)
				+ ", controlPublished=" + std::to_string(controlPublished)
				+ ", controlOutcome="
				+ std::string(controlRead.record ? ToString(controlRead.record->outcome) : "Missing")
				+ ", controlReason="
				+ std::string(controlRead.record ? ToString(controlRead.record->reason) : "Missing")
				+ ", controlUsable=" + std::to_string(controlRoute.IsValid() && system.ValidateRoute(controlRoute).usable)
				+ ", prepared=" + std::to_string(prepared) + ", staged=" + std::to_string(staged)
				+ ", published=" + std::to_string(published) + ", sameRoot="
				+ std::to_string(afterRoutePublish == beforeReplacement) + ", outcome="
				+ std::string(requestRead.record ? ToString(requestRead.record->outcome) : "Missing")
				+ ", reason=" + std::string(requestRead.record ? ToString(requestRead.record->reason) : "Missing") + ")";

			context.Expect(initialized && beforeReplacement != nullptr && controlPrepared && controlPublished
				&& controlIsPartial && prepared && staged && published
				&& afterRoutePublish == beforeReplacement && staleAndUnpublished,
				"A publishable Partial corridor intersecting a staged automatic replacement must fail closed before root exchange, just like Complete guidance."
				+ failureDetail);
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.DefersDirtyProfileIntoMatchingRecovery", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const std::shared_ptr<const NavigationWorldRoot> rootBefore = system.PublishedWorld();

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(
				NavigationProfile::Defaults(), terrain, replacement);
			// The committed receipt marks pA's coverage dirty before any request is
			// admitted. A later pA request must join this exact recovery group rather
			// than enqueueing a second ordinary profile build. The terrain spans four
			// canonical shards, so the conservative receipt covers all of them: otherwise
			// a route entirely in a still-Ready shard should correctly search immediately.
			const bool committed = initialized && replacementGeometry != nullptr
				&& CommitReplacement(system, replacementBuild, replacement, replacementGeometry,
					301U, -40.0, 40.0, -40.0, 40.0);
			const NavigationRequestHandle request = committed
				? SubmitRoute(system, world, 72U, 8.0, 16.0) : NavigationRequestHandle{};
			const auto beforeRecovery = system.ReadRequest(request);
			const NavigationSystem::StoreUsageSnapshot beforeUsage = system.PublishedStoreUsage();
			const bool joinedRecovery = beforeRecovery.record.has_value()
				&& beforeRecovery.record->lifecycle == NavigationRequestLifecycle::PendingPreparation
				&& beforeRecovery.record->outcome == NavigationOutcome::PendingPreparation
				&& beforeRecovery.record->reason == NavigationReason::DirtyCoverage
				&& beforeUsage.deferredRoutes == 1U && beforeUsage.pendingRoutes == 0U;
			const std::uint64_t missingLinkBefore = Counter(system,
				&NavigationDiagnosticSnapshot::preparedRouteTransitionMissingLinkCount);
			const bool reachedTerminal = request.IsValid() && DriveUntilTerminal(system, request);
			PublishDiagnostics(system);

			const std::shared_ptr<const NavigationWorldRoot> rootAfter = system.PublishedWorld();
			const NavigationRouteHandle route = system.PublishedRouteForRequest(request);
			const auto afterRecovery = system.ReadRequest(request);
			const NavigationSystem::StoreUsageSnapshot afterUsage = system.PublishedStoreUsage();
			const bool completedFromRecovery = IsTerminal(system, request, NavigationOutcome::Complete, NavigationReason::None)
				&& route.IsValid() && system.ValidateRoute(route).usable
				&& rootBefore != nullptr && rootAfter != nullptr
				// Receipt admission first publishes the Dirty root; the matching dynamic pA
				// root-plus-portal recovery is the only later exchange. A third publication
				// would expose the redundant ordinary pA build this regression forbids.
				&& rootAfter->rootGeneration == rootBefore->rootGeneration + 2U;
			const bool recoveryKeptTransitionCoherent = Counter(system,
				&NavigationDiagnosticSnapshot::preparedRouteTransitionMissingLinkCount) == missingLinkBefore;
			// Keep every value in this diagnostic bounded and public. A failed assertion
			// can then distinguish incorrect fixture sequencing from a route that either
			// never joined recovery or was not promoted after its root committed.
			const std::string failureDetail = " (committed=" + std::to_string(committed)
				+ ", beforeLifecycle=" + std::string(beforeRecovery.record ? ToString(beforeRecovery.record->lifecycle) : "Missing")
				+ ", beforeOutcome=" + std::string(beforeRecovery.record ? ToString(beforeRecovery.record->outcome) : "Missing")
				+ ", beforeReason=" + std::string(beforeRecovery.record ? ToString(beforeRecovery.record->reason) : "Missing")
				+ ", beforeDeferred=" + std::to_string(beforeUsage.deferredRoutes)
				+ ", beforePending=" + std::to_string(beforeUsage.pendingRoutes)
				+ ", reachedTerminal=" + std::to_string(reachedTerminal)
				+ ", afterLifecycle=" + std::string(afterRecovery.record ? ToString(afterRecovery.record->lifecycle) : "Missing")
				+ ", afterOutcome=" + std::string(afterRecovery.record ? ToString(afterRecovery.record->outcome) : "Missing")
				+ ", afterReason=" + std::string(afterRecovery.record ? ToString(afterRecovery.record->reason) : "Missing")
				+ ", afterDeferred=" + std::to_string(afterUsage.deferredRoutes)
				+ ", afterPending=" + std::to_string(afterUsage.pendingRoutes)
				+ ", beforeRoot=" + std::to_string(rootBefore ? rootBefore->rootGeneration : 0U)
				+ ", afterRoot=" + std::to_string(rootAfter ? rootAfter->rootGeneration : 0U)
				+ ", routeUsable=" + std::to_string(route.IsValid() && system.ValidateRoute(route).usable)
				+ ", missingLinkDelta=" + std::to_string(Counter(system,
					&NavigationDiagnosticSnapshot::preparedRouteTransitionMissingLinkCount) - missingLinkBefore) + ")";
			context.Expect(initialized && committed && request.IsValid() && joinedRecovery
				&& reachedTerminal && completedFromRecovery && recoveryKeptTransitionCoherent,
				"A request for a dirty profile with a matching dynamic recovery must remain pending until that root-plus-portal recovery commits, then complete without a redundant profile build."
				+ failureDetail);
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.DefersOnlyEndpointsInDirtyShard", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const NavigationProfile profile = NavigationProfile::Defaults();
			const bool initialized = InitializeEndpointAdmissionFixture(system, terrain, changed,
				terrainGeometry, changedGeometry, world);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, 250.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(profile, terrain, replacement);
			// The old/new swept bounds and profile halo remain inside tile 7 (shard 0).
			// Tile 8 (shard 1) is deliberately still Ready when the three requests arrive.
			const bool committed = initialized && replacementGeometry != nullptr
				&& CommitReplacement(system, replacementBuild, replacement, replacementGeometry,
					401U, 251.0, 253.0, 249.0, 251.0);
			const std::shared_ptr<const NavigationWorldRoot> dirtyRoot = system.PublishedWorld();
			const NavigationRequestHandle dirtyStart = committed
				? SubmitRoute(system, world, 80U, 244.0, 268.0, false, profile) : NavigationRequestHandle{};
			const NavigationRequestHandle dirtyDestination = committed
				? SubmitRoute(system, world, 81U, 268.0, 244.0, false, profile) : NavigationRequestHandle{};
			const NavigationRequestHandle readyControl = committed
				? SubmitRoute(system, world, 82U, 268.0, 276.0, false, profile) : NavigationRequestHandle{};
			const auto readyRead = system.ReadRequest(readyControl);
			const NavigationSystem::StoreUsageSnapshot usage = system.PublishedStoreUsage();
			const bool endpointsAreDistinct = HasCoverageState(dirtyRoot, profile, 0LL, NavigationCoverageState::Dirty)
				&& HasCoverageState(dirtyRoot, profile, 1LL, NavigationCoverageState::Ready);
			const bool onlyDirtyEndpointsDeferred = IsDeferredForDirtyCoverage(system, dirtyStart)
				&& IsDeferredForDirtyCoverage(system, dirtyDestination)
				&& readyRead.record.has_value()
				&& readyRead.record->lifecycle == NavigationRequestLifecycle::Queued
				&& readyRead.record->outcome == NavigationOutcome::Queued
				&& usage.deferredRoutes == 2U && usage.pendingRoutes == 1U;
			context.Expect(initialized && committed && dirtyStart.IsValid() && dirtyDestination.IsValid()
				&& readyControl.IsValid() && endpointsAreDistinct && onlyDirtyEndpointsDeferred,
				"A matching recovery must defer only a request whose start or destination maps to Dirty coverage; an unrelated Ready-shard route using the same profile must enter normal route work.");
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.CancellationPreventsDeferredRePromotion", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const NavigationProfile profile = NavigationProfile::Defaults();
			const bool initialized = InitializeEndpointAdmissionFixture(system, terrain, changed,
				terrainGeometry, changedGeometry, world);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, 250.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(profile, terrain, replacement);
			const bool committed = initialized && replacementGeometry != nullptr
				&& CommitReplacement(system, replacementBuild, replacement, replacementGeometry,
					402U, 251.0, 253.0, 249.0, 251.0);
			const std::shared_ptr<const NavigationWorldRoot> dirtyRoot = system.PublishedWorld();
			const NavigationRequestHandle promotedRequest = committed
				? SubmitRoute(system, world, 83U, 244.0, 268.0, false, profile) : NavigationRequestHandle{};
			const NavigationRequestHandle cancelledRequest = committed
				? SubmitRoute(system, world, 84U, 268.0, 244.0, false, profile) : NavigationRequestHandle{};
			const bool bothDeferred = IsDeferredForDirtyCoverage(system, promotedRequest)
				&& IsDeferredForDirtyCoverage(system, cancelledRequest);
			const bool cancelled = cancelledRequest.IsValid() && system.CancelRequest(cancelledRequest);
			const bool cancellationVisible = IsTerminal(system, cancelledRequest,
				NavigationOutcome::Cancelled, NavigationReason::Cancelled)
				&& !system.PublishedRouteForRequest(cancelledRequest).IsValid();
			const bool promotedTerminal = promotedRequest.IsValid() && DriveUntilTerminal(system, promotedRequest, 12U);
			const std::shared_ptr<const NavigationWorldRoot> recoveredRoot = system.PublishedWorld();
			const NavigationRouteHandle promotedRoute = system.PublishedRouteForRequest(promotedRequest);
			const bool promotedOnlyAfterRecovery = IsTerminal(system, promotedRequest,
				NavigationOutcome::Complete, NavigationReason::None)
				&& promotedRoute.IsValid() && system.ValidateRoute(promotedRoute).usable
				&& dirtyRoot != nullptr && recoveredRoot != nullptr
				&& recoveredRoot->rootGeneration > dirtyRoot->rootGeneration
				&& HasCoverageState(recoveredRoot, profile, 0LL, NavigationCoverageState::Ready);
			const bool cancellationSurvivedRecovery = IsTerminal(system, cancelledRequest,
				NavigationOutcome::Cancelled, NavigationReason::Cancelled)
				&& !system.PublishedRouteForRequest(cancelledRequest).IsValid();
			context.Expect(initialized && committed && bothDeferred && cancelled && cancellationVisible
				&& promotedTerminal && promotedOnlyAfterRecovery && cancellationSurvivedRecovery,
				"A deferred route may promote only after its Dirty endpoint coverage recovers; cancelling a sibling while deferred must remain terminal and must never requeue when that protected recovery publishes.");
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.FailsDeferredEndpointsWhenRecastRejectsCapturedSphere", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const NavigationProfile profile = NavigationProfile::Defaults();
			const bool initialized = InitializeEndpointAdmissionFixture(system, terrain, changed,
				terrainGeometry, changedGeometry, world, NavigationSourceKind::Collider);

			std::shared_ptr<const NavigationGeometrySnapshot> sphereGeometry;
			const NavigationEligibleSourceCandidate sphereReplacement = SphereColliderSource(2U, 250.0,
				sphereGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(profile, terrain, sphereReplacement);
			const bool sphereIsValidImmutableInput = sphereGeometry != nullptr
				&& sphereGeometry->Triangles().empty() && sphereGeometry->Primitives().size() == 1U
				&& sphereGeometry->Primitives().front().kind == NavigationGeometryPrimitiveKind::Sphere;
			const bool committed = initialized && sphereIsValidImmutableInput
				&& CommitReplacement(system, replacementBuild, sphereReplacement, sphereGeometry,
					403U, 251.0, 253.0, 249.0, 251.0);
			const std::shared_ptr<const NavigationWorldRoot> dirtyRoot = system.PublishedWorld();
			const NavigationRequestHandle failedRequest = committed
				? SubmitRoute(system, world, 85U, 244.0, 268.0, false, profile) : NavigationRequestHandle{};
			const NavigationRequestHandle cancelledRequest = committed
				? SubmitRoute(system, world, 86U, 268.0, 244.0, false, profile) : NavigationRequestHandle{};
			const bool bothDeferred = IsDeferredForDirtyCoverage(system, failedRequest)
				&& IsDeferredForDirtyCoverage(system, cancelledRequest);
			const bool cancelled = cancelledRequest.IsValid() && system.CancelRequest(cancelledRequest);
			const bool reachedTerminal = failedRequest.IsValid() && DriveUntilTerminal(system, failedRequest, 12U);
			PublishDiagnostics(system);
			const std::shared_ptr<const NavigationWorldRoot> afterFailureRoot = system.PublishedWorld();
			const NavigationSystem::StoreUsageSnapshot usage = system.PublishedStoreUsage();
			const bool providerFailureIsVisible = IsTerminal(system, failedRequest,
				NavigationOutcome::Failed, NavigationReason::ProviderFailure)
				&& !system.PublishedRouteForRequest(failedRequest).IsValid()
				&& usage.deferredRoutes == 0U;
			const bool cancelledSiblingStayedTerminal = IsTerminal(system, cancelledRequest,
				NavigationOutcome::Cancelled, NavigationReason::Cancelled)
				&& !system.PublishedRouteForRequest(cancelledRequest).IsValid();
			// The accepted receipt published one Dirty root. A permanent provider failure
			// must not exchange another root or a partial shard/portal tuple afterwards.
			const bool failurePreservedDirtyTuple = dirtyRoot != nullptr && afterFailureRoot == dirtyRoot
				&& HasCoverageState(afterFailureRoot, profile, 0LL, NavigationCoverageState::Dirty);
			context.Expect(initialized && sphereIsValidImmutableInput && committed && bothDeferred
				&& cancelled && reachedTerminal && providerFailureIsVisible && cancelledSiblingStayedTerminal
				&& failurePreservedDirtyTuple,
				"A valid captured sphere that Recast cannot represent must terminal deferred endpoints as Failed/ProviderFailure, drain the deferred queue, preserve the receipt's Dirty root without partial shard publication, and never revive a cancelled sibling.");
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.PublishesTwoCompletedProfilesAsOneAtomicGroup", [](TestContext& context) {
			NavigationWorkerPoolSession workers;
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const NavigationProfile alternate = AlternateProfile();
			// Re-register both copied profiles as one automatic stage. The replacement
			// below must capture both children from the same dirty root and publish them
			// together, never as two partially updated profile roots.
			const bool twoProfilesRegistered = initialized && system.QueueInitialGroundBuilds({
				Build(NavigationProfile::Defaults(), terrain, changed), Build(alternate, terrain, changed)});
			for(std::uint32_t frame = 0U; twoProfilesRegistered && frame < 8U; ++frame) system.Update(0.0F);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(
				NavigationProfile::Defaults(), terrain, replacement);
			const bool receiptCommitted = twoProfilesRegistered && replacementGeometry != nullptr
				&& CommitReplacement(system, replacementBuild, replacement, replacementGeometry,
					951U, -12.0, -8.0, -10.0, -6.0);
			// Capture the Dirty receipt root before any child starts. Comparing only after
			// completion would make the no-interim-publication assertion tautological.
			const std::shared_ptr<const NavigationWorldRoot> dirtyRoot = system.PublishedWorld();
			const std::uint64_t completionsBefore = DynamicBuildCompletionCount(system);
			const bool childrenDispatched = receiptCommitted && CaptureNavigationWorkWithoutPublishing(system);
			const std::shared_ptr<const NavigationDiagnosticSnapshot> afterDispatch = system.PublishedDiagnostics();
			const bool bothChildrenCurrent = childrenDispatched && afterDispatch != nullptr
				&& afterDispatch->asyncBuildCurrentCount == 2U;
			const bool bothChildrenCompleted = bothChildrenCurrent && CaptureUntilDynamicCompletion(
				system, completionsBefore + 2U);
			const std::uint64_t completionsObserved = DynamicBuildCompletionCount(system);
			const std::shared_ptr<const NavigationDiagnosticSnapshot> beforePublication = system.PublishedDiagnostics();
			// Completion is deliberately visible to Capture before Publish. The old root
			// must remain intact until every child is terminal and the one group commit
			// swaps roots and portals together at the protected boundary.
			const bool completedGroupHeld = beforePublication != nullptr
				&& beforePublication->asyncBuildDynamicCompletionCount == completionsBefore + 2U
				&& beforePublication->asyncBuildCurrentCount == 0U;
			const bool noInterimRootExchange = dirtyRoot != nullptr && system.PublishedWorld() == dirtyRoot;
			const bool published = completedGroupHeld && PublishNavigationWorkWithoutCapturing(system);
			const std::shared_ptr<const NavigationWorldRoot> finalRoot = system.PublishedWorld();
			const bool oneMergedExchange = published && dirtyRoot != nullptr && finalRoot != nullptr
				&& finalRoot != dirtyRoot && finalRoot->rootGeneration == dirtyRoot->rootGeneration + 1U;
			const bool bothProfilesReady = HasCoverageState(finalRoot, NavigationProfile::Defaults(), 0LL,
				NavigationCoverageState::Ready) && HasCoverageState(finalRoot, alternate, 0LL,
				NavigationCoverageState::Ready);

			const std::string failureDetail = " (initialized=" + std::to_string(initialized)
				+ ", registered=" + std::to_string(twoProfilesRegistered)
				+ ", receipt=" + std::to_string(receiptCommitted)
				+ ", dispatch=" + std::to_string(childrenDispatched)
				+ ", bothCurrent=" + std::to_string(bothChildrenCurrent)
				+ ", completed=" + std::to_string(bothChildrenCompleted)
				+ ", completionCount=" + std::to_string(completionsObserved)
				+ ", held=" + std::to_string(completedGroupHeld)
				+ ", noInterim=" + std::to_string(noInterimRootExchange)
				+ ", publish=" + std::to_string(published)
				+ ", merged=" + std::to_string(oneMergedExchange)
				+ ", ready=" + std::to_string(bothProfilesReady)
				+ ", dirtyRoot=" + std::to_string(dirtyRoot ? dirtyRoot->rootGeneration : 0U)
				+ ", finalRoot=" + std::to_string(finalRoot ? finalRoot->rootGeneration : 0U) + ")";
			context.Expect(initialized && twoProfilesRegistered && receiptCommitted
				&& bothChildrenCurrent && bothChildrenCompleted && completedGroupHeld && noInterimRootExchange
				&& oneMergedExchange && bothProfilesReady,
				"A dynamic profile group must retain completed children until one protected root/portal exchange publishes every successful profile."
				+ failureDetail);
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.WaitsForNormalGroundPublicationBeforeCapturingDynamicGroup", [](TestContext& context) {
			NavigationWorkerPoolSession workers;
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const NavigationProfile defaultProfile = NavigationProfile::Defaults();
			const NavigationProfile alternate = AlternateProfile();

			// Commit the source mutation first. It publishes the Dirty root and leaves a
			// default-profile recovery pending, which is the same ordering a real scene
			// receipt has before an on-demand profile is requested.
			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(
				defaultProfile, terrain, replacement);
			const bool receiptCommitted = initialized && replacementGeometry != nullptr
				&& CommitReplacement(system, replacementBuild, replacement, replacementGeometry,
					954U, -40.0, 40.0, -40.0, 40.0);
			const std::shared_ptr<const NavigationWorldRoot> dirtyRoot = system.PublishedWorld();
			const NavigationRequestHandle deferredRequest = receiptCommitted
				? SubmitRoute(system, world, 954U, 8.0, 16.0, false, defaultProfile)
				: NavigationRequestHandle{};
			const bool defaultRequestDeferred = IsDeferredForDirtyCoverage(system, deferredRequest);

			// This ordinary alternate-profile build was captured from the current Dirty
			// source tuple. Once its worker completes it must publish before a dynamic
			// group snapshots a root: that group changes the same shared root/portal tuple.
			const NavigationSystem::InitialGroundBuildInput alternateBuild = Build(alternate, terrain, replacement);
			const std::uint64_t groundBefore = GroundBuildCompletionCount(system);
			const std::uint64_t dynamicBefore = DynamicBuildCompletionCount(system);
			const std::uint64_t staleBefore = system.PublishedDiagnostics()
				? system.PublishedDiagnostics()->staleDropCount : 0U;
			const bool normalQueued = receiptCommitted && system.QueueInitialGroundBuilds({alternateBuild});
			const bool normalCaptured = normalQueued && CaptureNavigationWorkWithoutPublishing(system);
			const bool normalCompleted = normalCaptured && CaptureUntilGroundCompletion(system, groundBefore + 1U);
			const bool dirtyRootHeld = normalCompleted && dirtyRoot != nullptr && system.PublishedWorld() == dirtyRoot;

			// Only Publish may exchange the completed normal packet. The following Capture
			// is intentionally withheld until then: an old implementation created a group
			// here from dirtyRoot, forcing its later child result to be discarded as stale.
			const bool normalPublished = dirtyRootHeld && PublishNavigationWorkWithoutCapturing(system);
			const std::shared_ptr<const NavigationWorldRoot> normalRoot = system.PublishedWorld();
			const bool normalExchange = normalPublished && normalRoot != nullptr && dirtyRoot != nullptr
				&& normalRoot != dirtyRoot && normalRoot->rootGeneration == dirtyRoot->rootGeneration + 1U
				&& HasCoverageState(normalRoot, alternate, 0LL, NavigationCoverageState::Ready);

			const bool dynamicCaptured = normalExchange && CaptureNavigationWorkWithoutPublishing(system);
			const std::shared_ptr<const NavigationDiagnosticSnapshot> afterDynamicCapture = system.PublishedDiagnostics();
			const bool twoChildrenCurrent = dynamicCaptured && afterDynamicCapture != nullptr
				&& afterDynamicCapture->asyncBuildCurrentCount == 2U;
			const bool dynamicCompleted = twoChildrenCurrent && CaptureUntilDynamicCompletion(system, dynamicBefore + 2U);
			const bool dynamicPublished = dynamicCompleted && PublishNavigationWorkWithoutCapturing(system);
			const std::shared_ptr<const NavigationWorldRoot> recoveredRoot = system.PublishedWorld();
			const bool oneCurrentRecovery = dynamicPublished && recoveredRoot != nullptr && normalRoot != nullptr
				&& recoveredRoot != normalRoot && recoveredRoot->rootGeneration == normalRoot->rootGeneration + 1U
				&& HasCoverageState(recoveredRoot, defaultProfile, 0LL, NavigationCoverageState::Ready)
				&& HasCoverageState(recoveredRoot, alternate, 0LL, NavigationCoverageState::Ready);
			const std::uint64_t staleAfterRecovery = system.PublishedDiagnostics()
				? system.PublishedDiagnostics()->staleDropCount : 0U;
			const bool deferredRouteCompleted = oneCurrentRecovery && DriveUntilTerminal(system, deferredRequest)
				&& IsTerminal(system, deferredRequest, NavigationOutcome::Complete, NavigationReason::None)
				&& system.PublishedRouteForRequest(deferredRequest).IsValid();
			const std::string failureDetail = " (initialized=" + std::to_string(initialized)
				+ ", receipt=" + std::to_string(receiptCommitted)
				+ ", deferred=" + std::to_string(defaultRequestDeferred)
				+ ", normalQueued=" + std::to_string(normalQueued)
				+ ", normalCompleted=" + std::to_string(normalCompleted)
				+ ", normalExchange=" + std::to_string(normalExchange)
				+ ", dynamicCurrent=" + std::to_string(twoChildrenCurrent)
				+ ", dynamicCompleted=" + std::to_string(dynamicCompleted)
				+ ", oneRecovery=" + std::to_string(oneCurrentRecovery)
				+ ", route=" + std::to_string(deferredRouteCompleted)
				+ ", roots=" + std::to_string(dirtyRoot ? dirtyRoot->rootGeneration : 0U)
				+ "/" + std::to_string(normalRoot ? normalRoot->rootGeneration : 0U)
				+ "/" + std::to_string(recoveredRoot ? recoveredRoot->rootGeneration : 0U)
				+ ", completions=" + std::to_string(GroundBuildCompletionCount(system))
				+ "/" + std::to_string(DynamicBuildCompletionCount(system))
				+ ", stale=" + std::to_string(staleBefore) + "->" + std::to_string(staleAfterRecovery) + ")";
			context.Expect(initialized && receiptCommitted && defaultRequestDeferred && normalQueued && normalCompleted
				&& normalExchange && twoChildrenCurrent && dynamicCompleted && oneCurrentRecovery
				&& staleAfterRecovery == staleBefore && deferredRouteCompleted,
				"A completed normal ground packet must publish before dynamic-group capture, so the first dynamic recovery uses the current root rather than wasting a stale provider wave."
				+ failureDetail);
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.BoundedProfileGroupPublishesAfterThreeWaves", [](TestContext& context) {
			NavigationWorkerPoolSession workers;
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			std::array<NavigationProfile, 5U> profiles{};
			std::vector<NavigationSystem::InitialGroundBuildInput> profileBuilds;
			profileBuilds.reserve(profiles.size());
			for(std::uint32_t index = 0U; index < profiles.size(); ++index) {
				profiles[index] = ProfileVariant(index);
				profileBuilds.push_back(Build(profiles[index], terrain, changed));
			}
			const bool profilesRegistered = initialized && system.QueueInitialGroundBuilds(std::move(profileBuilds));
			for(std::uint32_t frame = 0U; profilesRegistered && frame < 12U; ++frame) system.Update(0.0F);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(profiles.front(), terrain, replacement);
			const bool receiptCommitted = profilesRegistered && replacementGeometry != nullptr
				&& CommitReplacement(system, replacementBuild, replacement, replacementGeometry,
					952U, -12.0, -8.0, -10.0, -6.0);
			const std::shared_ptr<const NavigationWorldRoot> dirtyRoot = system.PublishedWorld();
			const std::uint64_t completionsBefore = DynamicBuildCompletionCount(system);

			const bool firstWaveStarted = receiptCommitted && CaptureNavigationWorkWithoutPublishing(system);
			const auto firstWave = system.PublishedDiagnostics();
			const bool firstWaveBounded = firstWaveStarted && firstWave != nullptr
				&& firstWave->asyncBuildCurrentCount == 2U;
			const bool firstWaveCompleted = firstWaveBounded && CaptureUntilDynamicCompletion(system, completionsBefore + 2U);
			// Drain completion selects the next bounded wave in that same Capture. Do not
			// invoke another Capture here: a very fast provider could otherwise finish the
			// second wave and start the third before this test observes the 2/2/1 policy.
			const auto secondWave = system.PublishedDiagnostics();
			const bool secondWaveBounded = firstWaveCompleted && secondWave != nullptr
				&& secondWave->asyncBuildCurrentCount == 2U;
			const bool secondWaveCompleted = secondWaveBounded && CaptureUntilDynamicCompletion(system, completionsBefore + 4U);
			const auto thirdWave = system.PublishedDiagnostics();
			const bool thirdWaveBounded = secondWaveCompleted && thirdWave != nullptr
				&& thirdWave->asyncBuildCurrentCount == 1U;
			const bool thirdWaveCompleted = thirdWaveBounded && CaptureUntilDynamicCompletion(system, completionsBefore + 5U);
			const std::uint64_t completionsObserved = DynamicBuildCompletionCount(system);
			const bool noInterimRootExchange = dirtyRoot != nullptr && system.PublishedWorld() == dirtyRoot;
			const bool published = thirdWaveCompleted && PublishNavigationWorkWithoutCapturing(system);
			const auto finalRoot = system.PublishedWorld();
			const bool oneMergedExchange = published && dirtyRoot != nullptr && finalRoot != nullptr
				&& finalRoot->rootGeneration == dirtyRoot->rootGeneration + 1U;
			const bool everyProfileReady = finalRoot != nullptr && std::all_of(profiles.begin(), profiles.end(),
				[&finalRoot](const NavigationProfile& profile) {
					return HasCoverageState(finalRoot, profile, 0LL, NavigationCoverageState::Ready);
				});
			const std::string failureDetail = " (registered=" + std::to_string(profilesRegistered)
				+ ", receipt=" + std::to_string(receiptCommitted)
				+ ", waves=" + std::to_string(firstWaveBounded) + "/" + std::to_string(secondWaveBounded)
				+ "/" + std::to_string(thirdWaveBounded)
				+ ", complete=" + std::to_string(firstWaveCompleted) + "/" + std::to_string(secondWaveCompleted)
				+ "/" + std::to_string(thirdWaveCompleted)
				+ ", completionCount=" + std::to_string(completionsObserved)
				+ ", noInterim=" + std::to_string(noInterimRootExchange)
				+ ", merged=" + std::to_string(oneMergedExchange)
				+ ", ready=" + std::to_string(everyProfileReady) + ")";
			context.Expect(initialized && profilesRegistered && receiptCommitted
				&& firstWaveBounded && firstWaveCompleted && secondWaveBounded && secondWaveCompleted
				&& thirdWaveBounded && thirdWaveCompleted && noInterimRootExchange
				&& oneMergedExchange && everyProfileReady,
				"Five required profiles must execute in bounded two-child waves and publish only one coherent root after every child is terminal."
				+ failureDetail);
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.RejectsUnsupportedProfileBeforeCommittedReceiptPublication", [](TestContext& context) {
			NavigationWorkerPoolSession workers;
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const NavigationProfile defaultProfile = NavigationProfile::Defaults();
			NavigationProfile unsupportedProfile = NavigationProfile::Defaults();
			unsupportedProfile.horizontalResolutionMetres = 0.001;
			const std::shared_ptr<const NavigationWorldRoot> rootBefore = system.PublishedWorld();
			const std::uint64_t completionsBefore = DynamicBuildCompletionCount(system);
			// The profile is valid authored data but its Recast grid exceeds the bounded
			// provider envelope. Receipt admission must reject before it can dirty a root
			// or launch a mixed dynamic group from an under-expanded halo.
			const bool unsupportedStaged = initialized && unsupportedProfile.Validate().accepted
				&& system.StageDynamicGroundSources(unsupportedProfile, {terrainGeometry, changedGeometry});

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(defaultProfile, terrain, replacement);
			const bool receiptRejected = unsupportedStaged && replacementGeometry != nullptr
				&& !CommitReplacement(system, replacementBuild, replacement, replacementGeometry,
					953U, -12.0, -8.0, -10.0, -6.0);
			const bool captureCompleted = receiptRejected && CaptureNavigationWorkWithoutPublishing(system);
			const std::shared_ptr<const NavigationDiagnosticSnapshot> afterCapture = system.PublishedDiagnostics();
			const bool noDirtyWorkerState = captureCompleted && afterCapture != nullptr
				&& afterCapture->asyncBuildCurrentCount == 0U
				&& afterCapture->asyncBuildDynamicCompletionCount == completionsBefore
				&& system.DynamicDirtyIndexForTesting().PendingPreparationCount() == 0U
				&& system.DynamicDirtyIndexForTesting().DirtyCoverage().empty();
			const bool originalRootPreserved = rootBefore != nullptr && system.PublishedWorld() == rootBefore
				&& HasCoverageState(rootBefore, defaultProfile, 0LL, NavigationCoverageState::Ready);
			const NavigationRequestHandle defaultRequest = originalRootPreserved
				? SubmitRoute(system, world, 953U, 8.0, 16.0, false, defaultProfile)
				: NavigationRequestHandle{};
			const bool defaultRouteTerminal = defaultRequest.IsValid()
				&& DriveUntilTerminal(system, defaultRequest)
				&& IsTerminal(system, defaultRequest, NavigationOutcome::Complete, NavigationReason::None)
				&& system.PublishedRouteForRequest(defaultRequest).IsValid();
			const std::string failureDetail = " (staged=" + std::to_string(unsupportedStaged)
				+ ", rejected=" + std::to_string(receiptRejected)
				+ ", capture=" + std::to_string(captureCompleted)
				+ ", noDirtyWorker=" + std::to_string(noDirtyWorkerState)
				+ ", rootPreserved=" + std::to_string(originalRootPreserved)
				+ ", defaultRoute=" + std::to_string(defaultRouteTerminal) + ")";
			context.Expect(initialized && unsupportedStaged && receiptRejected && noDirtyWorkerState
				&& originalRootPreserved && defaultRouteTerminal,
				"An unsupported active profile must reject the committed source receipt atomically before dirty/root/worker publication, while the existing supported profile remains routable."
				+ failureDetail);
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.UnavailableFenceRetiresOldGroundAndDynamicPackets", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const NavigationSystem::InitialGroundBuildInput repeatedGround = Build(
				NavigationProfile::Defaults(), terrain, changed);
			// Queue a copied ground packet without starting a global worker. The public
			// unit lane deliberately has no scheduler-owned pool lifetime, so leaving an
			// unjoinable background task here would leak into later singleton fixtures.
			// The unavailable boundary must still discard this old packet before it can
			// restore Ready support on a later compatibility update.
			const bool queuedGround = initialized && repeatedGround.eligibleSourceIndex != nullptr
				&& system.QueueInitialGroundBuilds({repeatedGround});

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = Build(
				NavigationProfile::Defaults(), terrain, replacement);
			// The receipt leaves a dynamic recovery packet pending. Do not run Update
			// before fencing: it is the second old-work lane which must never republish.
			const bool queuedDynamic = queuedGround && replacementGeometry != nullptr
				&& CommitReplacement(system, replacementBuild, replacement, replacementGeometry,
					901U, -12.0, -8.0, -10.0, -6.0);
			const bool fenced = queuedDynamic && system.PublishUnavailableTerrain(nullptr, false);
			for(std::uint32_t frame = 0U; fenced && frame < 8U; ++frame) system.Update(0.0F);

			const std::shared_ptr<const NavigationWorldRoot> unavailable = system.PublishedWorld();
			const NavigationRequestHandle request = fenced
				? SubmitRoute(system, world, 890U, 8.0, 16.0) : NavigationRequestHandle{};
			const bool terminal = request.IsValid() && DriveUntilTerminal(system, request);
			const NavigationRouteHandle route = system.PublishedRouteForRequest(request);
			const bool remainsUnavailable = unavailable != nullptr
				&& unavailable->terrainState == NavigationTerrainState::Unavailable
				&& unavailable->coverage.empty() && unavailable->shards.empty()
				&& system.PublishedGroundShards().empty()
				&& IsTerminal(system, request, NavigationOutcome::UnavailableCoverage,
					NavigationReason::MissingTerrain) && !route.IsValid();
			context.Expect(initialized && queuedGround && queuedDynamic && fenced && terminal && remainsUnavailable,
				"An unavailable terrain boundary must retire queued or prepared old ground and dynamic packets; later protected work may not restore Ready coverage or answer a route from stale support.");
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.UnavailableFencePreservesCancelledRequest", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const NavigationRequestHandle request = initialized
				? SubmitRoute(system, world, 893U, 8.0, 16.0) : NavigationRequestHandle{};
			// A completed request is terminal and deliberately cannot be cancelled. Cancel
			// this queued request instead, then prove a later unavailable fence does not
			// rewrite the caller's terminal intent while clearing any internal work.
			const bool cancelled = request.IsValid() && system.CancelRequest(request);
			const bool cancellationVisible = IsTerminal(system, request,
				NavigationOutcome::Cancelled, NavigationReason::Cancelled)
				&& !system.PublishedRouteForRequest(request).IsValid();
			const auto cancelledRead = system.ReadRequest(request);
			const bool fenced = cancelled && system.PublishUnavailableTerrain(nullptr, false);
			system.Update(0.0F);
			const bool cancellationPreserved = IsTerminal(system, request,
				NavigationOutcome::Cancelled, NavigationReason::Cancelled)
				&& !system.PublishedRouteForRequest(request).IsValid();
			const auto afterFence = system.ReadRequest(request);
			const std::string failureDetail = " (initialized=" + std::to_string(initialized)
				+ ", cancelled=" + std::to_string(cancelled)
				+ ", beforeOutcome=" + std::string(cancelledRead.record ? ToString(cancelledRead.record->outcome) : "Missing")
				+ ", afterOutcome=" + std::string(afterFence.record ? ToString(afterFence.record->outcome) : "Missing")
				+ ", afterReason=" + std::string(afterFence.record ? ToString(afterFence.record->reason) : "Missing")
				+ ", mappingAfter=" + std::to_string(system.PublishedRouteForRequest(request).IsValid()) + ")";
			context.Expect(initialized && cancelled
				&& cancellationVisible && fenced && cancellationPreserved,
				"Unavailable cleanup must never overwrite a caller-cancelled request with a terrain failure."
				+ failureDetail);
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.CommittedUnrepresentedColliderFencesReadyRoot", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Scene::SceneMutationQueue mutations;
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const std::shared_ptr<const NavigationWorldRoot> readyRoot = system.PublishedWorld();

			const Object collider = objects.createObject("Committed unrepresented navigation collider");
			const Transform* transform = objects.AddComponent<Transform>(collider);
			objects.setObjectStatic(collider, true);
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(collider);
			Physics::ColliderRecord record{};
			record.recordId = GenerateUUID();
			record.recordVersion = 1U;
			record.shape = Physics::BoxColliderShape{};
			if(colliders != nullptr) colliders->colliders.push_back(record);

			Scene::SceneMutationBatchResult committedBatch{};
			const bool committed = initialized && transform != nullptr && colliders != nullptr
				&& CommitStaticColliderTransform(objects, mutations, collider, 4.0, committedBatch);
			const NavigationCommittedSourceMutationObservation observation = committed
				? ObserveCommittedNavigationSourceMutations(system, objects, mutations, committedBatch)
				: NavigationCommittedSourceMutationObservation{};
			// Hosts consume this request at their next Frame End. It must be one-shot:
			// otherwise a single failed committed bridge would repeatedly bootstrap while
			// the scene remains unavailable.
			const bool freshCaptureRequested = observation.fenced
				&& system.ConsumeFreshAutomaticCaptureRequest()
				&& !system.ConsumeFreshAutomaticCaptureRequest();
			const std::shared_ptr<const NavigationWorldRoot> unavailable = system.PublishedWorld();
			const NavigationRequestHandle request = observation.fenced
				? SubmitRoute(system, world, 891U, 8.0, 16.0) : NavigationRequestHandle{};
			const bool terminal = request.IsValid() && DriveUntilTerminal(system, request);
			const bool oldReadyWasFenced = unavailable != nullptr
				&& unavailable != readyRoot && unavailable->terrainState == NavigationTerrainState::Unavailable
				&& unavailable->coverage.empty() && unavailable->shards.empty()
				&& IsTerminal(system, request, NavigationOutcome::UnavailableCoverage,
					NavigationReason::MissingTerrain);
			context.Expect(committed && observation.committedSourceCount == 1U
				&& !observation.staged && observation.observedReceiptCount == 0U && observation.fenced
				&& freshCaptureRequested && terminal && oldReadyWasFenced,
				"A real committed static-collider transform that navigation cannot admit must fence the old Ready root before any later route can consume stale support.");
			system.Shutdown();
			objects.clear();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.RejectedSceneBatchDoesNotFenceReadyRoot", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Scene::SceneMutationQueue mutations;
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const std::shared_ptr<const NavigationWorldRoot> readyRoot = system.PublishedWorld();

			const Object collider = objects.createObject("Rejected navigation collider transform");
			const Transform* transform = objects.AddComponent<Transform>(collider);
			objects.setObjectStatic(collider, true);
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(collider);
			Physics::ColliderRecord record{};
			record.recordId = GenerateUUID();
			record.recordVersion = 1U;
			record.shape = Physics::BoxColliderShape{};
			if(colliders != nullptr) colliders->colliders.push_back(record);

			Scene::WorldPose destination{};
			destination.position.offsetMetres.x = 4.0;
			Scene::TransactionalMutationBuilder rejected(
				"navigation-rejected-scene-batch", Scene::TransactionOrigin::Test, "navigation-test");
			rejected.SetWorldTransform(objects, objects.getUUID(collider), destination)
				.InjectCommitFailureForTesting(0U);
			mutations.EnqueueTransaction(rejected.Build());
			const Scene::SceneMutationBatchResult rejectedBatch = mutations.Apply(objects, "navigation-rejected-scene-batch");
			const NavigationCommittedSourceMutationObservation observation = initialized && transform != nullptr
				&& colliders != nullptr
				? ObserveCommittedNavigationSourceMutations(system, objects, mutations, rejectedBatch)
				: NavigationCommittedSourceMutationObservation{};
			const bool freshCaptureNotRequested = !system.ConsumeFreshAutomaticCaptureRequest();
			const NavigationRequestHandle request = initialized
				? SubmitRoute(system, world, 892U, 8.0, 16.0) : NavigationRequestHandle{};
			const bool terminal = request.IsValid() && DriveUntilTerminal(system, request);
			const bool unchanged = !rejectedBatch.applied && !rejectedBatch.transactionResults.empty()
				&& rejectedBatch.transactionResults.front().status == Scene::TransactionStatus::Rejected
				&& observation.committedSourceCount == 0U && !observation.fenced
				&& freshCaptureNotRequested
				&& system.PublishedWorld() == readyRoot
				&& IsTerminal(system, request, NavigationOutcome::Complete, NavigationReason::None);
			const auto rejectedRead = system.ReadRequest(request);
			const std::string failureDetail = " (batchApplied=" + std::to_string(rejectedBatch.applied)
				+ ", transactionCount=" + std::to_string(rejectedBatch.transactionResults.size())
				+ ", transactionStatus=" + std::string(!rejectedBatch.transactionResults.empty()
					? ToString(rejectedBatch.transactionResults.front().status) : "Missing")
				+ ", committedSources=" + std::to_string(observation.committedSourceCount)
				+ ", staged=" + std::to_string(observation.staged)
				+ ", observed=" + std::to_string(observation.observedReceiptCount)
				+ ", fenced=" + std::to_string(observation.fenced)
				+ ", sameRoot=" + std::to_string(system.PublishedWorld() == readyRoot)
				+ ", outcome=" + std::string(rejectedRead.record ? ToString(rejectedRead.record->outcome) : "Missing") + ")";
			context.Expect(initialized && transform != nullptr && colliders != nullptr && terminal && unchanged,
				"An ordinary rejected scene transaction never exposes a committed source receipt, so the host must retain the existing Ready root instead of publishing an unnecessary unavailable fence."
				+ failureDetail);
			system.Shutdown();
			objects.clear();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.UnionsTwoReceiptsBeforePublication", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			// This route misses the first receipt but crosses the second, proving a later
			// root transition cannot discard the earlier prepared-route certificate.
			const NavigationRequestHandle request = initialized ? SubmitRoute(system, world, 3U, 4.0, 18.0)
				: NavigationRequestHandle{};
			const bool prepared = request.IsValid() && PrepareRouteWithoutPublishing(system);

			std::shared_ptr<const NavigationGeometrySnapshot> firstGeometry;
			const NavigationEligibleSourceCandidate first = Source(NavigationSourceKind::Mesh, 2U, -8.0,
				firstGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput firstBuild = Build(NavigationProfile::Defaults(), terrain, first);
			const bool firstCommitted = prepared && CommitReplacement(system, firstBuild, first, firstGeometry,
				201U, -12.0, -8.0, -10.0, -6.0);

			std::shared_ptr<const NavigationGeometrySnapshot> secondGeometry;
			const NavigationEligibleSourceCandidate second = Source(NavigationSourceKind::Mesh, 3U, 10.0,
				secondGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput secondBuild = Build(NavigationProfile::Defaults(), terrain, second);
			const bool secondCommitted = firstCommitted && CommitReplacement(system, secondBuild, second, secondGeometry,
				202U, 8.0, 12.0, 10.0, 14.0);
			if(secondCommitted) system.Update(0.0F);
			PublishDiagnostics(system);

			const NavigationRouteHandle route = system.PublishedRouteForRequest(request);
			const bool staleAndUnusable = IsTerminal(system, request, NavigationOutcome::Stale, NavigationReason::DirtyCoverage)
				&& !route.IsValid();
			const bool intersected = Counter(system, &NavigationDiagnosticSnapshot::preparedRouteTransitionIntersectedCount) > 0U;
			const auto read = system.ReadRequest(request);
			const std::string failureDetail = " (initialized=" + std::to_string(initialized)
				+ ", prepared=" + std::to_string(prepared)
				+ ", firstCommitted=" + std::to_string(firstCommitted)
				+ ", secondCommitted=" + std::to_string(secondCommitted)
				+ ", outcome=" + std::string(read.record ? ToString(read.record->outcome) : "Missing")
				+ ", reason=" + std::string(read.record ? ToString(read.record->reason) : "Missing")
				+ ", route=" + std::to_string(route.IsValid())
				+ ", intersected=" + std::to_string(Counter(system,
					&NavigationDiagnosticSnapshot::preparedRouteTransitionIntersectedCount))
				+ ", unknown=" + std::to_string(Counter(system,
					&NavigationDiagnosticSnapshot::preparedRouteTransitionUnknownCount)) + ")";
			context.Expect(initialized && prepared && firstCommitted && secondCommitted && staleAndUnusable && intersected,
				"Two receipt transitions before one route publish must union their changed bounds and stale a route intersecting either one."
				+ failureDetail);
			system.Shutdown();
		});

		registry.Add("Navigation.Dynamic.PreparedTransition.FailsClosedWhenTransitionJournalOverflows", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationEligibleSourceCandidate terrain{};
			NavigationEligibleSourceCandidate changed{};
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			NavigationWorldHandle world{};
			const bool initialized = InitializeFixture(system, terrain, changed, terrainGeometry, changedGeometry, world);
			const NavigationRequestHandle request = initialized ? SubmitRoute(system, world, 4U, 8.0, 16.0)
				: NavigationRequestHandle{};
			const bool prepared = request.IsValid() && PrepareRouteWithoutPublishing(system);

			bool committedEveryTransition = prepared;
			for(NavigationGeneration sequence = 1U; committedEveryTransition && sequence <= 65U; ++sequence) {
				// These direct receipt rows intentionally omit a replacement snapshot. The
				// journal still records each protected root transition, and its fixed 64-entry
				// retention must turn the missing first link into an explicit unknown result.
				Scene::NavigationSourceMutationReceipt receipt{};
				receipt.accepted = true;
				receipt.transactionStatus = Scene::TransactionStatus::Committed;
				receipt.intent.sourceId = changed.identity.sourceId;
				receipt.intent.operation = Scene::NavigationSourceMutationOperation::Move;
				receipt.intent.oldBounds = {{{0, 0, 0}, {-12.0, -1.0, -2.0}},
					{{0, 0, 0}, {-8.0, 3.0, 2.0}}, true};
				receipt.intent.newBounds = {{{0, 0, 0}, {-10.0, -1.0, -2.0}},
					{{0, 0, 0}, {-6.0, 3.0, 2.0}}, true};
				receipt.intent.committedSourceRevision = sequence;
				receipt.intent.committedChangeSequence = 1'000U + sequence;
				receipt.intent.physicalReadyRevision = 1U;
				receipt.intent.oldEnabled = true;
				receipt.intent.newEnabled = true;
				committedEveryTransition = system.ObserveSceneMutationReceipts(
					std::span<const Scene::NavigationSourceMutationReceipt>{&receipt, 1U}) == 1U;
			}
			if(committedEveryTransition) system.Update(0.0F);
			PublishDiagnostics(system);

			const NavigationRouteHandle route = system.PublishedRouteForRequest(request);
			const bool staleAndUnusable = IsTerminal(system, request, NavigationOutcome::Stale, NavigationReason::DirtyCoverage)
				&& !route.IsValid();
			const bool unknown = Counter(system, &NavigationDiagnosticSnapshot::preparedRouteTransitionUnknownCount) > 0U;
			const auto read = system.ReadRequest(request);
			const std::string failureDetail = " (initialized=" + std::to_string(initialized)
				+ ", prepared=" + std::to_string(prepared)
				+ ", committedEveryTransition=" + std::to_string(committedEveryTransition)
				+ ", outcome=" + std::string(read.record ? ToString(read.record->outcome) : "Missing")
				+ ", reason=" + std::string(read.record ? ToString(read.record->reason) : "Missing")
				+ ", route=" + std::to_string(route.IsValid())
				+ ", unknown=" + std::to_string(Counter(system,
					&NavigationDiagnosticSnapshot::preparedRouteTransitionUnknownCount))
				+ ", missingLink=" + std::to_string(Counter(system,
					&NavigationDiagnosticSnapshot::preparedRouteTransitionMissingLinkCount)) + ")";
			context.Expect(initialized && prepared && committedEveryTransition && staleAndUnusable && unknown,
				"A prepared route with a transition-journal retention gap must fail closed instead of skipping unknown changed coverage."
				+ failureDetail);
			system.Shutdown();
		});
	}
}
