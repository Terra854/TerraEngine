#include "TestHarness.h"

#include "Physics/Backend/IPhysicsBackend.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Tests {
	namespace {
		std::filesystem::path SourceRoot() {
			// CMake supplies the checkout root, allowing this boundary test to inspect
			// the installed-source contract without depending on the launch directory.
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR);
		}

		std::string ReadTextFile(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			if(!input) {
				throw TestFailure("Could not read the physics backend contract: " + path.string());
			}

			std::ostringstream text;
			text << input.rdbuf();
			return text.str();
		}

		std::string Lowercase(std::string text) {
			for(char& character : text) {
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			}
			return text;
		}

		template<typename Backend>
		concept ProviderNeutralBackendContract =
			std::is_abstract_v<Backend>
			&& std::has_virtual_destructor_v<Backend>;

		template<typename Backend>
		concept ProviderNeutralAdmissionContract = requires(
			Backend& backend,
			const TerraEngine::Physics::Backend::PhysicsBackendAdmissionReservation& reservation,
			TerraEngine::Physics::Backend::PhysicsBackendPreparedAdmission& prepared,
			TerraEngine::Physics::Backend::PhysicsBackendStagingToken staging,
			std::span<const TerraEngine::Physics::Backend::PhysicsBodyCreateRequest> bodies) {
			{ backend.ReserveAdmission(reservation, prepared) }
				-> std::same_as<TerraEngine::Physics::Backend::PhysicsBackendResult>;
			{ backend.CreateBodies(bodies, staging) }
				-> std::same_as<TerraEngine::Physics::Backend::PhysicsBackendResult>;
			{ backend.CommitStaging(staging) }
				-> std::same_as<TerraEngine::Physics::Backend::PhysicsBackendResult>;
			{ backend.RollbackStaging(staging) }
				-> std::same_as<TerraEngine::Physics::Backend::PhysicsBackendResult>;
		};

		bool ContainsJoltSurface(std::string_view headerText) {
			// These spellings cover provider includes, the Jolt namespace, and the
			// common Jolt macro prefix. Any occurrence would leak provider details
			// into a TerraEngine-owned header.
			constexpr std::array<std::string_view, 5> forbiddenTokens{
				"#include <jolt/",
				"#include \"jolt/",
				"jph::",
				"namespace jph",
				"jph_"
			};
			const std::string lowercaseText = Lowercase(std::string(headerText));
			for(const std::string_view token : forbiddenTokens) {
				if(lowercaseText.find(token) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		class ThreadPoolSession final {
		public:
			ThreadPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		struct AdmissionFixture final {
			ThreadPoolSession pool;
			TerraEngine::Physics::Backend::JoltPhysicsBackend backend{ThreadPool::instance()};
			TerraEngine::Physics::PhysicsWorldHandle world{{91U}, 1U};
			bool ready = false;

			explicit AdmissionFixture(std::uint32_t maximumBodies) {
				using namespace TerraEngine::Physics::Backend;
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				configuration.maximumBodies = maximumBodies;
				ready = backend.Initialize().Succeeded() && backend.CreateWorld(configuration).Succeeded();
			}

			~AdmissionFixture() {
				if(ready) (void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
			}
		};

		TerraEngine::Physics::Backend::PhysicsShapeRecipe MakeBoxRecipe() {
			TerraEngine::Physics::Backend::PhysicsShapeRecipe recipe;
			recipe.kind = TerraEngine::Physics::PhysicsShapeKind::Box;
			recipe.dimensions = {1.0F, 1.0F, 1.0F};
			return recipe;
		}

		TerraEngine::Physics::Backend::PhysicsBodyCreateRequest MakeBodyRequest(
			TerraEngine::Physics::PhysicsWorldHandle world,
			TerraEngine::Physics::PhysicsShapeHandle shape,
			std::uint32_t slot,
			bool startAwake,
			TerraEngine::Physics::PhysicsMotionType motionType = TerraEngine::Physics::PhysicsMotionType::Dynamic) {
			TerraEngine::Physics::Backend::PhysicsBodyCreateRequest request;
			request.handle = {world.worldId, world.worldGeneration, slot, 1U};
			request.objectId = TerraEngine::GenerateUUID();
			request.objectGeneration = 1U;
			request.componentGeneration = 1U;
			request.shape = shape;
			request.motionType = motionType;
			request.startAwake = startAwake;
			return request;
		}
	}

	void RegisterPhysicsBackendBoundaryTests(TestRegistry& registry) {
		registry.Add("Engine.Physics.BackendBoundary.CoarseContractIsAbstractAndProviderNeutral", [](TestContext& context) {
			using TerraEngine::Physics::Backend::IPhysicsBackend;

			// The interface is intentionally abstract: gameplay and engine systems
			// own stable TerraEngine values, while only a private implementation owns
			// the selected provider objects and their lifetime.
			static_assert(ProviderNeutralBackendContract<IPhysicsBackend>);
			std::unique_ptr<IPhysicsBackend> backend;
			context.Expect(!backend,
				"A backend boundary test must be able to hold an empty provider-neutral interface pointer without constructing a provider world.");
		});

		registry.Add("Engine.Physics.BackendBoundary.HeaderContainsNoJoltTypesOrIncludes", [](TestContext& context) {
			const std::string headerText = ReadTextFile(SourceRoot() / "Engine/Physics/Backend/IPhysicsBackend.h");
			context.Expect(!ContainsJoltSurface(headerText),
				"IPhysicsBackend.h must expose only TerraEngine-owned values and must not include or name Jolt types.");
		});

		registry.Add("Engine.Physics.BackendBoundary.CoarseLifecycleAndOperationVocabularyIsPresent", [](TestContext& context) {
			const std::string headerText = Lowercase(ReadTextFile(SourceRoot() / "Engine/Physics/Backend/IPhysicsBackend.h"));

			// These coarse verbs are deliberately checked instead of provider classes.
			// They preserve a replaceable backend while covering the lifecycle, shape,
			// body, simulation, query, and extraction responsibilities in the plan.
			constexpr std::array<std::string_view, 14> requiredOperationFragments{
				"createworld", "destroyworld", "cook", "createbody", "updatebody",
				"destroybody", "createconstraint", "destroyconstraint", "createcharacter",
				"destroycharacter", "apply", "step", "query", "extract"
			};
			for(const std::string_view operation : requiredOperationFragments) {
				context.Expect(headerText.find(operation) != std::string::npos,
					"IPhysicsBackend.h must declare the coarse provider-neutral operation '" + std::string(operation) + "'.");
			}
		});

		registry.Add("Engine.Physics.BackendBoundary.BatchAdmissionUsesCopiedReservationAndAtomicResultVocabulary", [](TestContext& context) {
			using namespace TerraEngine::Physics::Backend;
			using TerraEngine::Physics::PhysicsWorldHandle;
			static_assert(ProviderNeutralAdmissionContract<IPhysicsBackend>);
			const PhysicsWorldHandle world{{77U}, 3U};
			const PhysicsBackendAdmissionReservation empty{world};
			const PhysicsBackendAdmissionReservation mixed{world, 4U, 32U, 8U, 2U};
			const PhysicsBackendPreparedAdmission prepared{{world, 19U}, 4U, 32U, 8U, 2U, 7U};
			context.Expect(!empty.IsValid() && mixed.IsValid(),
				"A batch reservation must reject empty work yet retain copied mixed admission counts.");
			context.Expect(prepared.staging.IsValid() && prepared.reservedShapes == mixed.shapeCount
				&& prepared.reservedBodies == mixed.bodyCount
				&& prepared.reservedConstraints == mixed.constraintCount
				&& prepared.reservedCharacters == mixed.characterCount,
				"A prepared admission result must bind all reserved dependency families to one provider-neutral staging token.");
			const std::string headerText = Lowercase(ReadTextFile(SourceRoot() / "Engine/Physics/Backend/IPhysicsBackend.h"));
			for(const std::string_view operation : {"reserveadmission", "createbodies", "commitstaging", "rollbackstaging"}) {
				context.Expect(headerText.find(operation) != std::string::npos,
					"The provider-neutral batch contract must expose '" + std::string(operation) + "' without provider types.");
			}
		});

		registry.Add("Engine.Physics.BackendBoundary.MaximumCapacityRejectionCreatesNoStagingAuthority", [](TestContext& context) {
			using namespace TerraEngine::Physics::Backend;
			AdmissionFixture fixture(2U);
			context.Expect(fixture.ready, "The capacity fixture must create its bounded provider world.");
			PhysicsBackendPreparedAdmission prepared;
			const PhysicsBackendResult rejected = fixture.backend.ReserveAdmission(
				{fixture.world, 1U, 3U, 0U, 0U}, prepared);
			context.Expect(rejected.error == PhysicsBackendErrorCode::CapacityExceeded
				&& !prepared.staging.IsValid() && prepared.reservedBodies == 0U,
				"An oversized admission must reject before allocating a staging token or reserving visible body capacity.");
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			extraction.tick = 1U;
			context.Expect(fixture.backend.Extract(extraction).Succeeded()
				&& extraction.totalBodyCount == 0U && extraction.bodies.empty(),
				"Capacity rejection must leave the existing provider world and its publication source unchanged.");
		});

		registry.Add("Engine.Physics.BackendBoundary.ActivationPartitionsPreserveAwakeDormantAndStaticPolicy", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Backend;
			AdmissionFixture fixture(8U);
			context.Expect(fixture.ready, "The activation fixture must create its provider world.");
			PhysicsBackendPreparedAdmission prepared;
			context.Expect(fixture.backend.ReserveAdmission({fixture.world, 1U, 3U, 0U, 0U}, prepared).Succeeded(),
				"One bounded reservation must own the complete activation partition before staging.");
			const PhysicsShapeHandle shape{fixture.world.worldId, fixture.world.worldGeneration, 0U, 1U};
			const PhysicsBodyCreateRequest awake = MakeBodyRequest(fixture.world, shape, 0U, true);
			const PhysicsBodyCreateRequest dormant = MakeBodyRequest(fixture.world, shape, 1U, false);
			const PhysicsBodyCreateRequest staticBody = MakeBodyRequest(
				fixture.world, shape, 2U, true, PhysicsMotionType::Static);
			const std::array admissionPacket{awake, dormant, staticBody};
			context.Expect(fixture.backend.CreateShape(fixture.world, shape, MakeBoxRecipe(), prepared.staging).Succeeded()
				&& fixture.backend.CreateBodies(admissionPacket, prepared.staging).Succeeded()
				&& fixture.backend.CommitStaging(prepared.staging).Succeeded(),
				"Shapes and the immutable body packet must stage once and commit through activation partitions as one batch.");
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			extraction.tick = 1U;
			context.Expect(fixture.backend.Extract(extraction).Succeeded(),
				"The committed partition must be readable through provider-neutral extraction.");
			const auto findBody = [&extraction](PhysicsBodyHandle handle) {
				return std::find_if(extraction.bodies.begin(), extraction.bodies.end(),
					[handle](const PhysicsBackendBodyState& body) { return body.handle == handle; });
			};
			const auto awakeState = findBody(awake.handle);
			const auto dormantState = findBody(dormant.handle);
			const auto staticState = findBody(staticBody.handle);
			context.Expect(awakeState != extraction.bodies.end() && awakeState->awake
				&& dormantState != extraction.bodies.end() && !dormantState->awake
				&& staticState != extraction.bodies.end() && !staticState->awake,
				"Activation partitioning must not wake requested-dormant or static records while publishing the awake dynamic body.");
		});

		registry.Add("Engine.Physics.BackendBoundary.BatchBodyFailureRetainsReverseRollbackAuthority", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Backend;
			AdmissionFixture fixture(8U);
			context.Expect(fixture.ready, "The batch rollback fixture must create its provider world.");
			PhysicsBackendPreparedAdmission prepared;
			context.Expect(fixture.backend.ReserveAdmission({fixture.world, 1U, 2U, 0U, 0U}, prepared).Succeeded(),
				"The batch rollback fixture needs one reservation for both records.");
			const PhysicsShapeHandle shape{fixture.world.worldId, fixture.world.worldGeneration, 6U, 1U};
			const PhysicsBodyCreateRequest valid = MakeBodyRequest(fixture.world, shape, 6U, true);
			PhysicsBodyCreateRequest invalid = MakeBodyRequest(fixture.world,
				{fixture.world.worldId, fixture.world.worldGeneration, 99U, 1U}, 7U, false);
			const std::array packet{valid, invalid};
			context.Expect(fixture.backend.CreateShape(fixture.world, shape, MakeBoxRecipe(), prepared.staging).Succeeded(),
				"The batch rollback fixture must stage the shared shape before validating its body packet.");
			const PhysicsBackendResult rejected = fixture.backend.CreateBodies(packet, prepared.staging);
			context.Expect(!rejected.Succeeded() && fixture.backend.RollbackStaging(prepared.staging).Succeeded(),
				"A rejected batch member must preserve one staging token for deterministic reverse rollback.");
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			extraction.tick = 1U;
			context.Expect(fixture.backend.Extract(extraction).Succeeded()
				&& extraction.totalBodyCount == 0U && extraction.bodies.empty(),
				"Batch validation failure must leave no earlier member visible after rollback.");
		});

		registry.Add("Engine.Physics.BackendBoundary.PacketIdAssignmentFailureReclaimsParallelAllocations", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Backend;
			AdmissionFixture fixture(1U);
			context.Expect(fixture.ready, "The assignment-failure fixture must create a one-body provider world.");
			PhysicsBackendStagingToken failedStaging;
			const PhysicsShapeHandle failedShape{fixture.world.worldId, fixture.world.worldGeneration, 12U, 1U};
			const std::array failingPacket{
				MakeBodyRequest(fixture.world, failedShape, 12U, true),
				MakeBodyRequest(fixture.world, failedShape, 13U, false)};
			context.Expect(fixture.backend.CreateShape(fixture.world, failedShape, MakeBoxRecipe(), failedStaging).Succeeded(),
				"The assignment-failure fixture must stage a valid common shape before allocating the packet.");
			const PhysicsBackendResult rejected = fixture.backend.CreateBodies(failingPacket, failedStaging);
			context.Expect(rejected.error == PhysicsBackendErrorCode::CapacityExceeded
				&& fixture.backend.RollbackStaging(failedStaging).Succeeded(),
				"Exhausted sequential ID assignment must reverse-destroy every packet body while preserving rollback authority for the shape token.");

			PhysicsBackendStagingToken recoveryStaging;
			const PhysicsShapeHandle recoveryShape{fixture.world.worldId, fixture.world.worldGeneration, 14U, 1U};
			const PhysicsBodyCreateRequest recoveryBody = MakeBodyRequest(fixture.world, recoveryShape, 14U, true);
			context.Expect(fixture.backend.CreateShape(fixture.world, recoveryShape, MakeBoxRecipe(), recoveryStaging).Succeeded()
				&& fixture.backend.CreateBody(recoveryBody, recoveryStaging).Succeeded()
				&& fixture.backend.CommitStaging(recoveryStaging).Succeeded(),
				"A failed packet must return its assigned ID so the same bounded world can commit a later body.");
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			extraction.tick = 1U;
			context.Expect(fixture.backend.Extract(extraction).Succeeded()
				&& extraction.totalBodyCount == 1U && extraction.bodies.size() == 1U
				&& extraction.bodies.front().handle == recoveryBody.handle,
				"The recovery body must be the sole published record after packet-assignment rollback.");
		});

		registry.Add("Engine.Physics.BackendBoundary.PreparedPacketRollbackAbortsPrivateProviderState", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Backend;
			AdmissionFixture fixture(8U);
			context.Expect(fixture.ready, "The prepared-rollback fixture must create its provider world.");
			PhysicsBackendPreparedAdmission prepared;
			context.Expect(fixture.backend.ReserveAdmission({fixture.world, 1U, 2U, 0U, 0U}, prepared).Succeeded(),
				"The prepared-rollback fixture needs one staging token for its full packet.");
			const PhysicsShapeHandle shape{fixture.world.worldId, fixture.world.worldGeneration, 15U, 1U};
			const std::array packet{
				MakeBodyRequest(fixture.world, shape, 15U, true),
				MakeBodyRequest(fixture.world, shape, 16U, false)};
			context.Expect(fixture.backend.CreateShape(fixture.world, shape, MakeBoxRecipe(), prepared.staging).Succeeded()
				&& fixture.backend.CreateBodies(packet, prepared.staging).Succeeded()
				&& fixture.backend.RollbackStaging(prepared.staging).Succeeded(),
				"Rolling back a successful packet must abort its private prepared broad-phase state before reverse-destroying body IDs.");
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			extraction.tick = 1U;
			context.Expect(fixture.backend.Extract(extraction).Succeeded()
				&& extraction.totalBodyCount == 0U && extraction.bodies.empty(),
				"Aborted packet preparation must never publish a body after the transaction rollback completes.");
		});

		registry.Add("Engine.Physics.BackendBoundary.InvalidActivationMemberRollsBackTheEntirePartition", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Backend;
			AdmissionFixture fixture(8U);
			context.Expect(fixture.ready, "The activation rejection fixture must create its provider world.");
			PhysicsBackendPreparedAdmission prepared;
			context.Expect(fixture.backend.ReserveAdmission({fixture.world, 1U, 2U, 0U, 0U}, prepared).Succeeded(),
				"The activation rejection fixture needs one reservation for both partition members.");
			const PhysicsShapeHandle shape{fixture.world.worldId, fixture.world.worldGeneration, 3U, 1U};
			PhysicsBodyCreateRequest validAwake = MakeBodyRequest(fixture.world, shape, 3U, true);
			PhysicsBodyCreateRequest invalidDormant = MakeBodyRequest(fixture.world,
				{fixture.world.worldId, fixture.world.worldGeneration, 99U, 1U}, 4U, false);
			context.Expect(fixture.backend.CreateShape(fixture.world, shape, MakeBoxRecipe(), prepared.staging).Succeeded()
				&& fixture.backend.CreateBody(validAwake, prepared.staging).Succeeded(),
				"The fixture must stage one valid awake member before the later partition member is rejected.");
			const PhysicsBackendResult rejected = fixture.backend.CreateBody(invalidDormant, prepared.staging);
			context.Expect(!rejected.Succeeded() && fixture.backend.RollbackStaging(prepared.staging).Succeeded(),
				"An invalid dormant member must reject the partition and return ownership to reverse rollback.");
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			extraction.tick = 1U;
			context.Expect(fixture.backend.Extract(extraction).Succeeded()
				&& extraction.totalBodyCount == 0U && extraction.bodies.empty(),
				"A rejected activation partition must not expose the previously staged awake member.");
		});

		registry.Add("Engine.Physics.BackendBoundary.RollbackReversesDependentStagingWithoutVisibleRecords", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			using namespace TerraEngine::Physics::Backend;
			AdmissionFixture fixture(8U);
			context.Expect(fixture.ready, "The rollback fixture must create its provider world.");
			PhysicsBackendPreparedAdmission prepared;
			context.Expect(fixture.backend.ReserveAdmission({fixture.world, 1U, 2U, 1U, 0U}, prepared).Succeeded(),
				"The rollback fixture needs one reservation covering shape, bodies, and their dependent constraint.");
			const PhysicsShapeHandle shape{fixture.world.worldId, fixture.world.worldGeneration, 4U, 1U};
			const PhysicsBodyCreateRequest first = MakeBodyRequest(fixture.world, shape, 4U, true);
			const PhysicsBodyCreateRequest second = MakeBodyRequest(fixture.world, shape, 5U, false);
			PhysicsConstraintCreateRequest constraint;
			constraint.handle = {fixture.world.worldId, fixture.world.worldGeneration, 0U, 1U};
			constraint.ownerObjectId = TerraEngine::GenerateUUID();
			constraint.ownerObjectGeneration = 1U;
			constraint.componentGeneration = 1U;
			constraint.recordId = TerraEngine::GenerateUUID();
			constraint.recordGeneration = 1U;
			constraint.bodyA = first.handle;
			constraint.bodyB = second.handle;
			context.Expect(fixture.backend.CreateShape(fixture.world, shape, MakeBoxRecipe(), prepared.staging).Succeeded()
				&& fixture.backend.CreateBodies(std::array{first, second}, prepared.staging).Succeeded()
				&& fixture.backend.CreateConstraint(constraint, prepared.staging).Succeeded()
				&& fixture.backend.RollbackStaging(prepared.staging).Succeeded(),
				"A later constraint must abort packet preparation, then rollback must discard the dependent constraint before staged bodies and shape are released.");
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			extraction.tick = 1U;
			context.Expect(fixture.backend.Extract(extraction).Succeeded()
				&& extraction.totalBodyCount == 0U && extraction.bodies.empty(),
				"Reverse rollback must leave no staged body visible after the dependency batch is discarded.");
			context.Expect(!fixture.backend.DestroyConstraint(constraint.handle).Succeeded()
				&& !fixture.backend.DestroyBody(first.handle).Succeeded()
				&& !fixture.backend.DestroyShape(shape).Succeeded(),
				"Reverse rollback must remove dependent records before later callers can address stale handles.");
		});
}
}
