#include "TestHarness.h"

#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationTypes.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Navigation;

	void RegisterNavigationTypesTests(TestRegistry& registry) {
		registry.Add("Engine.Navigation.Types.RequestHandlesKeepOwnerLifetimeAndNeverResurrect", [](TestContext& context) {
			// Runtime handles carry a world lifetime, owner generation, slot, and
			// 64-bit generation. Reusing a slot must invalidate the old value.
			const NavigationWorldHandle world{NavigationWorldId{71U}, 11U};
			const NavigationOwnerHandle owner{world, NavigationOwnerId{902U}, 5U};
			NavigationHandleAllocator<NavigationRequestHandle> requests{2U};

			const NavigationRequestHandle first = requests.Allocate(owner);
			context.Expect(requests.Status(first, owner) == NavigationHandleStatus::Alive,
				"A freshly allocated request handle must be alive for its owner lifetime.");
			context.Expect(requests.Release(first), "Releasing an alive request must succeed once.");
			context.Expect(requests.Status(first, owner) == NavigationHandleStatus::Destroyed,
				"A released request must remain a distinguishable destroyed tombstone before reuse.");

			const NavigationRequestHandle replacement = requests.Allocate(owner);
			context.ExpectEqual(first.slotIndex, replacement.slotIndex,
				"Bounded request storage must reuse released slots rather than growing without bound.");
			context.Expect(first.slotGeneration != replacement.slotGeneration,
				"Reusing a request slot must advance its 64-bit generation.");
			context.Expect(requests.Status(first, owner) == NavigationHandleStatus::StaleSlot,
				"A prior request generation must never address a replacement result.");

			const NavigationOwnerHandle replacementOwner{world, owner.ownerId, owner.ownerGeneration + 1U};
			context.Expect(requests.Status(replacement, replacementOwner) == NavigationHandleStatus::StaleOwner,
				"Script reload or owner replacement must reject requests from the prior owner lifetime.");
			context.Expect(requests.Status(replacement, NavigationOwnerHandle{}) == NavigationHandleStatus::InvalidOwner,
				"An invalid owner must fail closed before consulting a recycled request slot.");
		});

		registry.Add("Engine.Navigation.Types.GenerationOverflowRetiresSlotsInsteadOfWrapping", [](TestContext& context) {
			const NavigationWorldHandle world{NavigationWorldId{72U}, 3U};
			const NavigationOwnerHandle owner{world, NavigationOwnerId{903U}, 1U};
			NavigationHandleAllocator<NavigationRouteHandle> routes{1U};

			const NavigationRouteHandle nearOverflow = routes.AllocateForTesting(owner,
				std::numeric_limits<NavigationGeneration>::max());
			context.Expect(routes.Release(nearOverflow), "The controlled near-overflow route must release successfully.");
			const NavigationRouteHandle afterOverflow = routes.Allocate(owner);
			context.Expect(!afterOverflow.IsValid(),
				"A slot whose generation would wrap must retire instead of resurrecting a stale route handle.");
			context.Expect(routes.Status(nearOverflow, owner) == NavigationHandleStatus::Retired,
				"Generation overflow must leave an explicit retired-slot status for diagnostics.");
		});

		registry.Add("Engine.Navigation.Types.LifecycleOutcomesAndReasonsRemainExplicit", [](TestContext& context) {
			context.Expect(IsTerminalNavigationOutcome(NavigationOutcome::Complete),
				"A complete route outcome must be terminal.");
			context.Expect(IsTerminalNavigationOutcome(NavigationOutcome::Partial),
				"An opt-in partial route outcome must be terminal, not silently promoted to complete.");
			context.Expect(IsTerminalNavigationOutcome(NavigationOutcome::BudgetExhausted),
				"A clipped search must finish explicitly as BudgetExhausted rather than Unreachable.");
			context.Expect(!IsTerminalNavigationOutcome(NavigationOutcome::PendingPreparation),
				"Pending preparation is not a usable route outcome.");
			context.Expect(NavigationReason::MissingSupport != NavigationReason::InvalidDestination,
				"Loss of physical support and an invalid destination need distinct gameplay-visible reasons.");
			context.Expect(NavigationReason::SupersededRequest != NavigationReason::SceneReplaced,
				"Intent replacement and scene-lifetime replacement need distinct stale-work reasons.");
			context.Expect(ToString(NavigationOutcome::UnavailableCoverage) == "UnavailableCoverage",
				"Provider-neutral outcome strings must preserve coverage-unavailable semantics.");
			context.Expect(ToString(NavigationReason::NodeLimit) == "NodeLimit",
				"Provider node exhaustion must map to an explicit engine reason.");
		});

		registry.Add("Engine.Navigation.Types.CanonicalLocationsRequireWorldIdentityAndFiniteNormalizedOffsets", [](TestContext& context) {
			NavigationLocation location{};
			location.world = NavigationWorldHandle{NavigationWorldId{73U}, 9U};
			location.position.cell = {4, -2, 7};
			location.position.offsetMetres = {0.25, -0.5, 1.0};
			context.Expect(ValidateNavigationLocation(location).accepted,
				"A finite normalized WorldPosition with an explicit navigation world must be accepted.");

			location.world = {};
			context.Expect(ValidateNavigationLocation(location).code == NavigationLocationValidationCode::InvalidWorld,
				"A canonical location without world lifetime identity must reject before localizing geometry.");
			location.world = NavigationWorldHandle{NavigationWorldId{73U}, 9U};
			location.position.offsetMetres.x = std::numeric_limits<double>::quiet_NaN();
			context.Expect(ValidateNavigationLocation(location).code == NavigationLocationValidationCode::NonFinitePosition,
				"Non-finite canonical offsets must reject instead of entering provider-local float conversion.");
			location.position.offsetMetres = {32768.0, 0.0, 0.0};
			context.Expect(ValidateNavigationLocation(location).code == NavigationLocationValidationCode::UnnormalizedPosition,
				"Locations must be normalized through WorldPosition semantics before they are used by navigation.");
		});

		registry.Add("Engine.Navigation.Types.ProfilesRejectInvalidCapsulesAndAreaPreferences", [](TestContext& context) {
			NavigationProfile profile = NavigationProfile::Defaults();
			context.Expect(profile.Validate().accepted, "The frozen default ground profile must be valid.");

			profile.radiusMetres = 0.0;
			context.Expect(profile.Validate().code == NavigationProfileValidationCode::InvalidRadius,
				"An upright ground capsule requires a finite radius greater than zero.");
			profile = NavigationProfile::Defaults();
			profile.heightMetres = profile.radiusMetres * 2.0 - 0.001;
			context.Expect(profile.Validate().code == NavigationProfileValidationCode::InvalidHeight,
				"Ground capsule height must be at least twice its radius.");
			profile = NavigationProfile::Defaults();
			profile.stepHeightMetres = -0.01;
			context.Expect(profile.Validate().code == NavigationProfileValidationCode::InvalidStepHeight,
				"Negative step height must reject instead of becoming an implicit traversal action.");
			profile = NavigationProfile::Defaults();
			profile.maxSlopeDegrees = 90.001;
			context.Expect(profile.Validate().code == NavigationProfileValidationCode::InvalidSlope,
				"Ground slope must remain in the inclusive [0, 90) range.");
			profile = NavigationProfile::Defaults();
			profile.clearanceMarginMetres = -0.001;
			context.Expect(profile.Validate().code == NavigationProfileValidationCode::InvalidClearanceMargin,
				"Negative clearance margins must reject atomically.");
			profile = NavigationProfile::Defaults();
			profile.areaPenalties[0] = -0.25;
			context.Expect(profile.Validate().code == NavigationProfileValidationCode::InvalidAreaPenalty,
				"Area penalties must be nonnegative so traversal cost multipliers stay valid.");
		});

		registry.Add("Engine.Navigation.Types.FrozenLimitsRejectOverCapacityAdmissionBeforeMutation", [](TestContext& context) {
			NavigationSettings settings = NavigationSettings::Defaults();
			context.Expect(settings.Validate().accepted, "The reference navigation limits must validate as a complete bounded configuration.");
			context.ExpectEqual(16U, settings.maxActiveProfileBuilds,
				"The frozen reference configuration permits exactly sixteen active profile build keys.");
			context.ExpectEqual(4096U, settings.maxActiveAgents,
				"The frozen reference configuration limits a world to 4,096 active agents.");
			context.ExpectEqual(1024U, settings.maxAdmittedRequests,
				"The frozen reference configuration limits a world to 1,024 admitted requests.");
			context.ExpectEqual(32768U, settings.maxDetourNodesPerQuery,
				"A provider query must cap node expansion at the frozen 32,768-node limit.");
			context.ExpectEqual(4096U, settings.maxRoutePoints,
				"Copied route output must cap at the frozen 4,096-point limit.");

			NavigationAdmissionCounter admission{settings.maxAdmittedRequests};
			context.Expect(admission.TryReserve(settings.maxAdmittedRequests),
				"Admission may reserve the complete configured request capacity.");
			context.Expect(!admission.TryReserve(1U),
				"Over-capacity request admission must fail before publishing any partial intent mutation.");
			context.ExpectEqual(settings.maxAdmittedRequests, admission.ReservedCount(),
				"A rejected admission must preserve the existing complete reservation count.");
			settings.maxAdmittedRequests = 0U;
			context.Expect(settings.Validate().code == NavigationSettingsValidationCode::InvalidRequestCapacity,
				"Zero request capacity is invalid instead of an implicit unbounded or unusable configuration.");
		});
	}
}
