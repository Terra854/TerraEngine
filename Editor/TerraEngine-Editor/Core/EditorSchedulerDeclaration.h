#pragma once

#include "Engine/Core/Scheduling/SystemScheduler.h"

namespace TerraEngine::Editor
{
// Share the real protected editor contract with headless scheduler validation.
// This declaration contains no UI state; all live work remains in EditorSystem.
inline TerraEngine::Scheduling::SystemDeclaration MakeEditorSchedulerDeclaration() {
	TerraEngine::Scheduling::SystemDeclaration declaration;
	declaration.name = "EditorSystem";
	declaration.phase = "PostFrame";
	declaration.reads = {
		"RendererOutput", "PresentationState", "Window", "WindowSnapshot", "InputSnapshot",
		// Validation observes immutable renderer copies after protected publication.
		"RendererEfficiencyOptions", "RendererEfficiencyEvidence",
		"RendererRecoveryStateSnapshot",
		"ProjectState", "EditorState", "ImGuiDrawData", "EditorAutomationQueue",
		"EditorAutomationSession", "EditorExportWorkerResults", "AnimationRuntimeState",
		"LocomotionAuthority", "LocomotionObservations", "LocomotionReceipts",
		"PhysicsCharacterMotionLeases", "LocomotionAnimatorBindings",
		"Resources", "SceneGraph", "ObjectManager", "AnimationRagdollMappings",
		"TerrainCopyWorkerResults", "TerrainDurableRootMetadata", "EditorTransactionReceipts"
	};
	declaration.writes = {
		"EditorUiPresentation", "ViewportTextureRegistry", "EditorSubmissionCompletion",
		// Validation-owned device recreation releases UI bindings before replacing
		// native outputs and publishes real session/recovery identities here.
		"RendererOutput", "PresentationState", "RendererRecoveryStateSnapshot",
		"EditorDiagnostics", "EditorAutomationState", "EditorAutomationSession",
		"SceneMutationQueue", "EditorProfilerTiming", "EditorProfilerWorkerCompletions",
		// Requests enter the typed queue here and become visible at the next capture.
		"RendererEfficiencyOptionsQueue",
		"EditorProfilerHealthPublication", "EditorExportOperationState",
		"TerrainCopyOperationState", "TerrainCopyAssetRegistration", "TerrainCopyPrivateOutputs",
		"ProjectState", "SceneGraph", "ObjectManager", "Resources",
		// Editor commands only queue intent; Play/project lifecycle also retires
		// leases and root consumers before Physics or authored state is destroyed.
		"LocomotionCommands", "LocomotionAuthority", "LocomotionObservations", "LocomotionReceipts",
		"PhysicsWorld", "PhysicsCharacterMotionLeases", "LocomotionAnimatorBindings",
		"AnimationConsumerMasks", "RootMotionIntervals", "RootMotionResults"
	};
	declaration.mainThread = true;
	declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
	declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::Protected;
	declaration.hardBoundary = true;
	declaration.safeSubwork = "Terrain copy workers load captured durable roots, re-encode fresh identities and persist private staging without live scene or asset registration; automation schema validation and immutable request preparation may run on workers; immutable export requests and owned subprocesses, reference-impact scans, log filtering, editor subsystem snapshots, and profiler workers prepare owned results without touching live scene, scripting assemblies or ImGui.";
	declaration.protectedCommit = "Consume native window-close intent and gate project teardown on settled receipts and cleanup; validation-owned device recreation drains resource workers, proves healthy native idle, releases old ImGui descriptors/backend before actual Vulkan replacement, and rebinds UI before drawing; terrain copy source validation, provisional asset registration, typed rebind admission, sole-receipt settlement and conditional cleanup with existing frame-end visibility; export admission, coherent result adoption and captured launch, immutable animation recording capture, profiler completion drain and health publication, prepared automation request commit at post_editor_automation_flush, SceneMutationQueue publication, project and Play Mode transitions, ImGui frame mutation, checked Vulkan recording and presentation.";
	declaration.protectedReason = declaration.protectedCommit;
	declaration.profilingLabel = declaration.name;
	declaration.usesDefaultMetadata = false;
	return declaration;
}

}
