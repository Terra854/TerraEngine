#include "TestHarness.h"

namespace TerraEngine::Tests {
	void RegisterAnimatorComponentTests(TestRegistry& registry);
	void RegisterAnimatorComponentBindingTests(TestRegistry& registry);
	void RegisterAnimationAssetFormatTests(TestRegistry& registry);
	void RegisterAnimationCodecTests(TestRegistry& registry);
	void RegisterAnimationFormatContractTests(TestRegistry& registry);
	void RegisterAnimationSchedulerContractTests(TestRegistry& registry);
	void RegisterAnimationMutationContractTests(TestRegistry& registry);
	void RegisterAnimationDormantPerformanceTests(TestRegistry& registry);
	void RegisterAnimationSamplingTests(TestRegistry& registry);
	void RegisterAnimationBlendTests(TestRegistry& registry);
	void RegisterAnimationEventCurveTests(TestRegistry& registry);
	void RegisterAnimationPlaybackLifecycleTests(TestRegistry& registry);
	void RegisterAnimationCpuDeterminismTests(TestRegistry& registry);
	void RegisterAnimationRenderFallbackTests(TestRegistry& registry);
	void RegisterAnimationControllerAssetTests(TestRegistry& registry);
	void RegisterAnimationGraphValidationTests(TestRegistry& registry);
	void RegisterAnimationStateMachineTests(TestRegistry& registry);
	void RegisterAnimationProgramPublisherTests(TestRegistry& registry);
	void RegisterAnimationGraphEvaluationTests(TestRegistry& registry);
	void RegisterAnimationExecutionModeTests(TestRegistry& registry);
	void RegisterAnimationProcessingClassTests(TestRegistry& registry);
	void RegisterAnimationCompactOwnershipTests(TestRegistry& registry);
	void RegisterAnimationCompactCaptureContractTests(TestRegistry& registry);
	void RegisterAnimationDiagnosticsAtomicTests(TestRegistry& registry);
	void RegisterAnimationClassTransitionTests(TestRegistry& registry);
	void RegisterAnimationPublicationRollbackTests(TestRegistry& registry);
	void RegisterAnimationLifecycleRecoveryTests(TestRegistry& registry);
	void RegisterGpuAnimationTopologyTests(TestRegistry& registry);
	void RegisterGpuAnimationShaderLayoutTests(TestRegistry& registry);
	void RegisterGpuAnimationPerformanceGateTests(TestRegistry& registry);
	void RegisterGpuAnimationParityTests(TestRegistry& registry);
	void RegisterGpuAnimationRecoveryTests(TestRegistry& registry);
	void RegisterRootMotionIntegrationTests(TestRegistry& registry);
	void RegisterAnimationAttachmentTests(TestRegistry& registry);
	void RegisterAnimationGameplayCommandTests(TestRegistry& registry);
	void RegisterAnimationSchedulingStressTests(TestRegistry& registry);
	void RegisterAnimationCachingTests(TestRegistry& registry);
	void RegisterAnimationMemoryPressureTests(TestRegistry& registry);
	void RegisterAnimationLifecycleStressTests(TestRegistry& registry);
	void RegisterAnimationDiagnosticsPerformanceTests(TestRegistry& registry);
	void RegisterAnimationCrowdPerformanceTests(TestRegistry& registry);
	void RegisterAnimationScaleFoundationIntegrationTests(TestRegistry& registry);
	void RegisterTwoBoneIkTests(TestRegistry& registry);
	void RegisterChainIkTests(TestRegistry& registry);
	void RegisterProceduralAnimationTests(TestRegistry& registry);
	void RegisterProceduralCommandStoreTests(TestRegistry& registry);
	void RegisterRetargetProfileAssetTests(TestRegistry& registry);
	void RegisterRetargetValidationTests(TestRegistry& registry);
	void RegisterRetargetEvaluationTests(TestRegistry& registry);
	void RegisterMotionDatabaseAssetTests(TestRegistry& registry);
	void RegisterMotionMatchCpuTests(TestRegistry& registry);
	void RegisterMotionMatchGpuTests(TestRegistry& registry);
	void RegisterMotionMatchIntegrationTests(TestRegistry& registry);
	void RegisterRagdollProfileAssetTests(TestRegistry& registry);
	void RegisterRagdollActivationTests(TestRegistry& registry);
	void RegisterRagdollBlendTests(TestRegistry& registry);
	void RegisterRagdollRecoveryTests(TestRegistry& registry);
	void RegisterAnimationResidencyTests(TestRegistry& registry);
	void RegisterAnimationHotReloadTests(TestRegistry& registry);
	void RegisterAnimationRuntimeParityTests(TestRegistry& registry);
	void RegisterAnimationCrossPlatformDeterminismTests(TestRegistry& registry);
	void RegisterAnimationRollbackSnapshotTests(TestRegistry& registry);
	void RegisterAnimationRollbackTests(TestRegistry& registry);
	void RegisterAnimationReplicationTests(TestRegistry& registry);
	void RegisterAnimationDivergenceTests(TestRegistry& registry);

	void RegisterAnimationTestScaffolding(TestRegistry& registry)
	{
		// This small entry proves that the animation registry is linked through the
		// shared harness before feature-specific test files begin registering cases.
		registry.Add("Animation.RegistrationAvailable", [](TestContext& context) {
			context.Expect(true, "Animation tests should be registered through the shared engine harness.");
		});

		// Contract tests share this registry so one focused test executable covers
		// both durable formats and authored component/runtime ownership boundaries.
		RegisterAnimatorComponentTests(registry);
		RegisterAnimatorComponentBindingTests(registry);
		RegisterAnimationAssetFormatTests(registry);
		RegisterAnimationCodecTests(registry);
		RegisterAnimationFormatContractTests(registry);
		RegisterAnimationSchedulerContractTests(registry);
		RegisterAnimationMutationContractTests(registry);
		RegisterAnimationDormantPerformanceTests(registry);
		RegisterAnimationSamplingTests(registry);
		RegisterAnimationBlendTests(registry);
		RegisterAnimationEventCurveTests(registry);
		RegisterAnimationPlaybackLifecycleTests(registry);
		RegisterAnimationCpuDeterminismTests(registry);
		RegisterAnimationRenderFallbackTests(registry);
		RegisterAnimationControllerAssetTests(registry);
		RegisterAnimationGraphValidationTests(registry);
		RegisterAnimationStateMachineTests(registry);
		RegisterAnimationProgramPublisherTests(registry);
		RegisterAnimationGraphEvaluationTests(registry);
		RegisterAnimationExecutionModeTests(registry);
		RegisterAnimationProcessingClassTests(registry);
		RegisterAnimationCompactOwnershipTests(registry);
		RegisterAnimationCompactCaptureContractTests(registry);
		RegisterAnimationDiagnosticsAtomicTests(registry);
		RegisterAnimationClassTransitionTests(registry);
		RegisterAnimationPublicationRollbackTests(registry);
		RegisterAnimationLifecycleRecoveryTests(registry);
		RegisterGpuAnimationTopologyTests(registry);
		RegisterGpuAnimationShaderLayoutTests(registry);
		RegisterGpuAnimationPerformanceGateTests(registry);
		RegisterGpuAnimationParityTests(registry);
		RegisterGpuAnimationRecoveryTests(registry);
		RegisterRootMotionIntegrationTests(registry);
		RegisterAnimationAttachmentTests(registry);
		RegisterAnimationGameplayCommandTests(registry);
		// The scale-foundation tests share this registry so the normal engine test
		// executable exercises the same linked runtime code as production builds.
		RegisterAnimationSchedulingStressTests(registry);
		RegisterAnimationCachingTests(registry);
		RegisterAnimationMemoryPressureTests(registry);
		RegisterAnimationLifecycleStressTests(registry);
		RegisterAnimationScaleFoundationIntegrationTests(registry);
		// These performance-named cases validate frozen definitions and fail-closed
		// result schemas only. They deliberately do not execute timing acceptance.
		RegisterAnimationDiagnosticsPerformanceTests(registry);
		RegisterAnimationCrowdPerformanceTests(registry);
		// Procedural solvers share the production engine-test executable so their
		// generation and allocation behavior is exercised with normal runtime code.
		RegisterTwoBoneIkTests(registry);
		RegisterChainIkTests(registry);
		RegisterProceduralAnimationTests(registry);
		RegisterProceduralCommandStoreTests(registry);
		RegisterRetargetProfileAssetTests(registry);
		RegisterRetargetValidationTests(registry);
		RegisterRetargetEvaluationTests(registry);
		RegisterMotionDatabaseAssetTests(registry);
		RegisterMotionMatchCpuTests(registry);
		RegisterMotionMatchGpuTests(registry);
		RegisterMotionMatchIntegrationTests(registry);
		RegisterRagdollProfileAssetTests(registry);
		RegisterRagdollActivationTests(registry);
		RegisterRagdollBlendTests(registry);
		RegisterRagdollRecoveryTests(registry);
		RegisterAnimationResidencyTests(registry);
		RegisterAnimationHotReloadTests(registry);
		RegisterAnimationRuntimeParityTests(registry);
		RegisterAnimationCrossPlatformDeterminismTests(registry);
		RegisterAnimationRollbackSnapshotTests(registry);
		RegisterAnimationRollbackTests(registry);
		RegisterAnimationReplicationTests(registry);
		RegisterAnimationDivergenceTests(registry);
	}

}
