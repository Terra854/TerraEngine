// MotionMatchIntegrationTests.cpp - Selection/diagnostic seams for later graph integration.

#include "TestHarness.h"

#include "Animation/Graph/MotionMatchNodeEvaluator.h"
#include "Animation/MotionMatching/MotionMatchDiagnostics.h"

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		AnimationGuid Guid() { AnimationGuid result; result.value[0] = 1U; return result; }
		MotionDatabaseAsset Database() { MotionDatabaseAsset result; result.assetId = Guid(); result.runtimeGeneration = 2U;
			result.compatibilityFingerprint = 3U; result.sourceFingerprint = 4U; result.quantizationScales.fill(1.0F);
			MotionDatabaseSample sample; sample.stableSampleId = 1U; sample.sourceClipId = 9U; sample.sourceTick = 30;
			result.samples.push_back(sample); result.featureSchemaFingerprint = ComputeMotionDatabaseSchemaFingerprint(result); return result; }
		GraphPoseContribution Pose(float translation) {
			GraphPoseContribution pose;
			pose.compatibilityFingerprint = 77U;
			pose.jointCount = 1U;
			pose.jointStableIds = {101U};
			AnimationTransform joint;
			joint.translation.x = translation;
			pose.joints = {joint};
			pose.morphs = {{500U, translation}};
			return pose;
		}
	}

	void RegisterMotionMatchIntegrationTests(TestRegistry& registry) {
		registry.Add("Animation.MotionMatch.SelectionRemainsCpuAuthoritativeAcrossGpuFallback",
			[](TestContext& context) {
				const MotionDatabaseAsset database = Database(); MotionMatchQuery query;
				query.database = {database.assetId, database.runtimeGeneration}; query.expectedSchemaFingerprint = database.featureSchemaFingerprint;
				query.featureWeights.fill(1U); MotionMatchSelection selection;
				MotionMatchGpuReadback stale; stale.generation = {{database.assetId, database.runtimeGeneration + 1U}, database.featureSchemaFingerprint, 1U}; stale.status = AnimationStatus::Ok;
				context.Expect(SelectMotionMatchAuthoritatively(database, query, &stale, selection) == AnimationStatus::Ok
					&& !selection.usedGpuCandidates && selection.chosen.stableSampleId == 1U,
					"Graph/rollback integration receives one CPU-authoritative candidate even when a GPU result is stale.");
				MotionMatchGpuReadback matching; matching.generation = {{database.assetId, database.runtimeGeneration}, database.featureSchemaFingerprint, 2U};
				matching.status = AnimationStatus::Ok; matching.candidates = {{1U, 999U, 999, {}}};
				context.Expect(SelectMotionMatchAuthoritatively(database, query, &matching, selection) == AnimationStatus::Ok
					&& selection.usedGpuCandidates && selection.chosen.stableSampleId == 1U
					&& selection.authoritativeScores.visitedSamples == 1U,
					"A matching GPU top-K only bounds candidate IDs; CPU integer scoring remains authoritative.");
			});

		registry.Add("Animation.MotionMatch.DiagnosticsRetainsBoundedCopiedHistory",
			[](TestContext& context) {
				const MotionDatabaseAsset database = Database(); MotionMatchDiagnostics diagnostics;
				MotionMatchGpuGeneration generation{{database.assetId, database.runtimeGeneration}, database.featureSchemaFingerprint, 1U};
				MotionMatchQuery query;
				query.database = {database.assetId, database.runtimeGeneration};
				query.expectedSchemaFingerprint = database.featureSchemaFingerprint;
				query.featureWeights.fill(1U);
				MotionMatchSelection selection;
				context.Expect(SelectMotionMatchAuthoritatively(database, query, nullptr, selection) == AnimationStatus::Ok,
					"A diagnostic record requires a complete authoritative selection snapshot.");
				MotionMatchDiagnosticRecord first = MakeMotionMatchDiagnosticRecord(generation, 1, selection);
				MotionMatchDiagnosticRecord second = MakeMotionMatchDiagnosticRecord(generation, 2, selection);
				context.Expect(diagnostics.Configure(1U) == AnimationStatus::Ok && diagnostics.Publish(first) == AnimationStatus::Ok
					&& diagnostics.Publish(second) == AnimationStatus::Ok && diagnostics.AcquireCopy()->records.size() == 1U
					&& diagnostics.AcquireCopy()->records.front().tick == 2
					&& diagnostics.AcquireCopy()->records.front().chosen.sourceClipId == 9U
					&& diagnostics.AcquireCopy()->records.front().chosen.sourceTick == 30
					&& diagnostics.AcquireCopy()->records.front().selectionSource == MotionMatchSelectionSource::CpuFullSearch,
					"Later graph, recording, and rollback consumers receive a copied bounded diagnostic history rather than live evaluator storage.");
			});

		registry.Add("Animation.MotionMatch.GraphComposesSelectionAndSnapshotsInFixedOrder",
			[](TestContext& context) {
				const MotionDatabaseAsset database = Database();
				MotionMatchQuery query;
				query.database = {database.assetId, database.runtimeGeneration};
				query.expectedSchemaFingerprint = database.featureSchemaFingerprint;
				query.featureWeights.fill(1U);
				const GraphPoseContribution current = Pose(0.0F);
				const GraphPoseContribution selected = Pose(2.0F);
				MotionMatchGraphNodeRequest request;
				request.database = &database;
				request.query = &query;
				request.currentPose = &current;
				request.resolvedPose = {MotionMatchCandidate{1U, 9U, 30, {}}, &selected};
				request.transitionWeight = 0.5F;
				request.evaluationTick = 45;
				request.retargetFollows = true;
				request.proceduralFollows = true;
				request.ragdollFollows = true;
				request.recordingRequested = true;
				request.rollbackRequested = true;
				MotionMatchGraphNodeResult result;
				context.Expect(EvaluateMotionMatchGraphNode(request, result) == AnimationStatus::Ok
					&& result.selection.chosen.stableSampleId == 1U
					&& result.pose.joints.front().translation.x == 1.0F
					&& result.recordingSnapshot.has_value()
					&& result.rollbackSnapshot.has_value()
					&& result.recordingSnapshot->evaluationTick == 45,
					"The CPU-selected sample must transition into one complete recordable and rollback-safe graph pose.");
				const std::vector<MotionMatchCompositionStage> expected{
					MotionMatchCompositionStage::Selection,
					MotionMatchCompositionStage::TransitionBlend,
					MotionMatchCompositionStage::Retarget,
					MotionMatchCompositionStage::Procedural,
					MotionMatchCompositionStage::Ragdoll,
					MotionMatchCompositionStage::Recording};
				context.Expect(result.orderedStages == expected,
					"Motion selection, blend, retarget, IK, ragdoll, and recording must retain their fixed semantic order.");
			});

		registry.Add("Animation.MotionMatch.GraphUsesCompleteFallbackWhenEverySampleIsExcluded",
			[](TestContext& context) {
				const MotionDatabaseAsset database = Database();
				const std::uint64_t excluded = 1U;
				MotionMatchQuery query;
				query.database = {database.assetId, database.runtimeGeneration};
				query.expectedSchemaFingerprint = database.featureSchemaFingerprint;
				query.featureWeights.fill(1U);
				query.excludedSampleIds = {&excluded, 1U};
				const GraphPoseContribution current = Pose(0.0F);
				const GraphPoseContribution fallback = Pose(-2.0F);
				MotionMatchGraphNodeRequest request;
				request.database = &database;
				request.query = &query;
				request.currentPose = &current;
				request.fallbackPose = &fallback;
				request.evaluationTick = 46;
				MotionMatchGraphNodeResult result;
				context.Expect(EvaluateMotionMatchGraphNode(request, result) == AnimationStatus::Ok
					&& result.usedFallbackState
					&& result.selection.status == AnimationStatus::NotReady
					&& result.pose == fallback,
					"An empty candidate set must select the authored complete fallback state without exposing a partial search result.");
			});
	}
}
