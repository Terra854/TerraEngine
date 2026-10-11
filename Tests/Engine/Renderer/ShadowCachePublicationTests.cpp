// ShadowCachePublicationTests.cpp - submission-qualified physical shadow ownership.

#include "TestHarness.h"
#include "Renderer/ShadowWorkPlan.h"

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Renderer;

		ShadowViewKey PublicationView(std::uint32_t viewId = 1u, std::uint64_t region = 1u)
		{
			ShadowViewKey key;
			key.rendererSessionGeneration = 1u;
			key.viewId = viewId;
			key.allocationGeneration = 1u;
			key.regionId = region;
			key.lightId[0] = 1u;
			key.extent = {1024u, 1024u};
			key.depthFormat = 126u; // Vulkan D32_SFLOAT identity; policy does not call Vulkan.
			key.lightProjectionStamp.semanticRevision = 1u;
			for(std::size_t index = 0u; index < 4u; ++index)
				key.lightProjectionStamp.viewProjection[index * 5u] = 1.0;
			key.lightProjectionStamp.cubeProjectionEvidenceComplete = true;
			for(auto& face : key.lightProjectionStamp.cubeViewProjections)
				for(std::size_t index = 0u; index < 4u; ++index)
					face[index * 5u] = 1.0;
			return key;
		}

		ShadowRefreshCandidate Candidate(const ShadowViewKey& view, std::uint64_t token,
			std::uint64_t command, std::uint8_t recordedMask = 1u, std::uint8_t requiredMask = 1u)
		{
			ShadowRefreshCandidate candidate;
			candidate.view = view;
			if(requiredMask == 0x3fu)
				candidate.view.resourceKind = static_cast<ShadowResourceKind>(1u); // PointCube.
			candidate.membership.reusable = true; // Resolved empty caster set is intentional.
			candidate.contentToken = token;
			candidate.frameSerial = 10u;
			candidate.commandSerial = command;
			candidate.requiredFaceMask = requiredMask;
			candidate.recordedFaceMask = recordedMask;
			return candidate;
		}

		void Submit(TestContext& context, ShadowCachePublicationLedger& ledger,
			const ShadowRefreshCandidate& candidate, std::uint64_t submission)
		{
			context.Expect(ledger.Record(candidate), "A valid candidate must record before submission.");
			context.Expect(ledger.ConfirmSubmission(candidate.commandSerial, submission, true), "A successful relevant submit must publish ownership.");
		}

		ShadowCasterMembership AliasMembership()
		{
			ShadowCasterStamp row;
			row.objectId[0] = 1u;
			row.lifetimeGeneration = 1u;
			row.segmentId = 1u;
			row.meshId[0] = 2u;
			row.assetId[0] = 3u;
			row.materialId[0] = 4u;
			row.shaderId[0] = 5u;
			row.textureId[0] = 6u;
			row.meshRevision = row.assetRevision = row.materialRevision = 1u;
			row.shaderRevision = row.textureRevision = 1u;
			row.meshletCount = 1u;
			row.evidenceMask = kCompleteShadowCasterEvidence;
			ShadowCasterMembership membership;
			membership.rows.push_back(row);
			membership.digest = 123u;
			membership.reusable = true;
			return membership;
		}

		template<typename Change>
		void ExpectAliasMismatch(TestContext& context, Change change)
		{
			ShadowCachePublicationLedger ledger(1u);
			const auto source = PublicationView(1u);
			auto candidate = Candidate(source, 101u, 11u);
			candidate.membership = AliasMembership();
			Submit(context, ledger, candidate, 21u);
			auto destination = PublicationView(2u);
			auto membership = candidate.membership;
			change(destination, membership);
			context.Expect(!ledger.AliasSubmittedContent(destination, 101u, membership), "An exact submitted-content mismatch must reject qualified aliasing.");
			context.Expect(!ledger.CanReuse(destination, 101u), "A rejected alias cannot obtain a logical reuse claim.");
			context.Expect(ledger.CanReuse(source, 101u), "Rejected aliasing cannot change the original physical owner.");
		}
	}

	void RegisterShadowCachePublicationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.ShadowPublication.ObservedPoseCountersPreserveExactSubmittedInputs", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1);
			const auto view = PublicationView(1);
			auto candidate = Candidate(view, 101, 11);
			candidate.membership = AliasMembership();
			Submit(context, ledger, candidate, 21);
			auto observed = candidate.membership;
			observed.rows.front().poseGeneration = 100;
			observed.rows.front().morphGeneration = 200;
			context.Expect(ExactShadowCasterMembershipEqual(candidate.membership, observed),
				"Republished counters do not change exact submitted deformation content.");
			context.Expect(ledger.AliasSubmittedContent(PublicationView(2), 101, observed),
				"A qualified view alias can reuse submitted exact content across observed counter changes.");
			context.Expect(ledger.CanReuse(view, 101), "Observation-only changes cannot revoke physical submitted ownership.");
			observed.rows.front().evidenceMask &= ~static_cast<std::uint64_t>(ShadowCasterEvidence::Deformation);
			context.Expect(!ledger.AliasSubmittedContent(PublicationView(3), 101, observed),
				"Missing exact deformation proof still rejects reuse regardless of diagnostic counters.");
		});
		registry.Add("Renderer.Efficiency.ShadowPublication.PartialSceneCubeObservesActualGameOwner", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1U);
			auto game = PublicationView(2U);
			game.viewType = static_cast<RenderViewType>(1U);
			game.resourceKind = static_cast<ShadowResourceKind>(1U); // PointCube.
			Submit(context, ledger, Candidate(game, 101U, 11U, 0x3fU, 0x3fU), 21U);
			auto scene = game; scene.viewType = static_cast<RenderViewType>(0U); scene.viewId = 1U;
			// Validation can inspect the real submitted physical owner even though
			// this different qualified Scene key has never acquired reuse authority.
			const auto observed = ledger.FindSubmittedPhysicalOwner(scene);
			context.Expect(observed && observed->view == game && observed->contentToken == 101U,
				"Overlapping Scene query must copy the actual submitted Game owner.");
			context.Expect(!ledger.CanReuse(scene, 101U), "Physical inspection must not authorize unqualified reuse.");
			context.Expect(!ledger.Record(Candidate(scene, 102U, 12U, 0x1fU, 0x3fU)), "Five recorded faces cannot establish atomic cube proof.");
			context.Expect(!ledger.CanReuse(game, 101U), "Recorded partial overwrite immediately revokes the previous full Game owner.");
			context.Expect(!ledger.ConfirmSubmission(12U, 22U), "A submitted incomplete cube cannot publish a healthy owner.");
			context.Expect(!ledger.CanReuse(game, 101U) && !ledger.CanReuse(scene, 102U), "No old or incomplete cube may regain a claim.");
			const auto healthy = Candidate(scene, 103U, 13U, 0x3fU, 0x3fU);
			context.Expect(ledger.Record(healthy), "A later complete cube can prepare healthy content.");
			context.Expect(!ledger.CanReuse(scene, 103U), "Complete recording still requires a successful real submit.");
			context.Expect(ledger.ConfirmSubmission(13U, 23U), "Healthy successful submission restores atomic ownership.");
			context.Expect(ledger.CanReuse(scene, 103U) && !ledger.CanReuse(game, 101U), "Only the healthy newly submitted qualified owner can reuse.");
		});
		registry.Add("Renderer.Efficiency.ShadowPublication.PreparedAndRecordedAreNeverReusable", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto view = PublicationView();
			const auto candidate = Candidate(view, 101u, 11u);
			context.Expect(ledger.Prepare(candidate), "A valid immutable candidate can be prepared.");
			context.Expect(!ledger.CanReuse(view, 101u), "CPU preparation cannot authorize depth reuse.");
			const auto prepared = ledger.Find(view);
			context.Expect(prepared.has_value() && prepared->status == ShadowCachePublicationState::Prepared, "Diagnostics distinguish prepared content.");
			context.Expect(ledger.Record(candidate), "Complete recording can create a recorded candidate.");
			context.Expect(!ledger.CanReuse(view, 101u), "Recording alone cannot authorize GPU depth reuse.");
			const auto recorded = ledger.Find(view);
			context.Expect(recorded.has_value() && recorded->status == ShadowCachePublicationState::Recorded, "Diagnostics distinguish recorded content.");
			context.ExpectEqual(std::uint64_t{0}, recorded->submitSerial, "Recorded work cannot manufacture a submission receipt.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.SubmissionAndCompletionHaveSeparateReceipts", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto view = PublicationView();
			Submit(context, ledger, Candidate(view, 101u, 11u), 21u);
			context.Expect(ledger.CanReuse(view, 101u), "Submitted depth is eligible with ordered graphics visibility.");
			auto submitted = ledger.Find(view);
			context.Expect(submitted && submitted->status == ShadowCachePublicationState::Submitted, "Successful submit publishes submitted state.");
			context.ExpectEqual(std::uint64_t{21}, submitted->submitSerial, "Submitted state carries its actual receipt.");
			context.ExpectEqual(std::uint64_t{0}, submitted->completionSerial, "Submission does not invent completion evidence.");
			ledger.Complete(21u, 31u);
			const auto completed = ledger.Find(view);
			context.Expect(completed && completed->status == ShadowCachePublicationState::Complete, "Existing completion proof advances lifetime state.");
			context.ExpectEqual(std::uint64_t{31}, completed->completionSerial, "Completion uses the supplied completion receipt.");
			context.Expect(ledger.CanReuse(view, 101u), "Completion preserves compatible content ownership.");
			// Copied diagnostics cannot give an external reader write access to the
			// internal owner ledger.
			submitted->contentToken = 999u;
			context.ExpectEqual(std::uint64_t{101}, ledger.Find(view)->contentToken, "Entry inspection returns a copy.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.UnknownOrZeroSubmitCannotPromote", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto view = PublicationView();
			context.Expect(!ledger.ConfirmSubmission(11u, 21u, true), "An unrelated submission cannot invent a candidate.");
			context.Expect(ledger.Record(Candidate(view, 101u, 11u)), "The candidate can record.");
			context.Expect(!ledger.ConfirmSubmission(11u, 0u, true), "A zero serial is not a real successful-submit receipt.");
			context.Expect(!ledger.CanReuse(view, 101u), "Invalid submission identity cannot authorize reuse.");
			ledger.Complete(21u, 31u);
			context.Expect(!ledger.CanReuse(view, 101u), "Completion of unknown submission cannot promote recorded content.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.RecordedOverwriteBlocksSameFramePriorOwner", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto viewA = PublicationView(1u);
			const auto viewB = PublicationView(2u);
			Submit(context, ledger, Candidate(viewA, 101u, 11u), 21u);
			context.Expect(ledger.CanReuse(viewA, 101u), "View A owns the submitted physical region initially.");
			context.Expect(ledger.Record(Candidate(viewB, 102u, 12u)), "View B records a write into the same allocation region.");
			// This is the same-frame B-write then A-lookup hazard. The last submit
			// still belongs to A, but its physical contents are scheduled to change.
			context.Expect(!ledger.CanReuse(viewA, 101u), "Recording B immediately blocks A's stale owner hit before B submits.");
			context.Expect(!ledger.CanReuse(viewB, 102u), "B's pending writer is not a submitted global owner.");
			context.Expect(ledger.ConfirmSubmission(12u, 22u, true), "B's successful relevant submit publishes its ownership.");
			context.Expect(ledger.CanReuse(viewB, 102u), "B can reuse after successful submission.");
			context.Expect(!ledger.CanReuse(viewA, 101u), "The overwritten logical A key cannot retain a valid claim.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.IndependentRegionsDoNotInvalidateEachOther", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto viewA = PublicationView(1u, 1u);
			const auto viewB = PublicationView(2u, 2u);
			Submit(context, ledger, Candidate(viewA, 101u, 11u), 21u);
			context.Expect(ledger.Record(Candidate(viewB, 102u, 12u)), "An independent physical region can record.");
			context.Expect(ledger.CanReuse(viewA, 101u), "A pending write in another region cannot invalidate A.");
			context.Expect(ledger.ConfirmSubmission(12u, 22u, true), "The independent region can submit.");
			context.Expect(ledger.CanReuse(viewA, 101u) && ledger.CanReuse(viewB, 102u), "Distinct physical regions retain separate submitted owners.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.FailedSubmitDiscardsReplacementAndOldClaim", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto view = PublicationView();
			Submit(context, ledger, Candidate(view, 101u, 11u), 21u);
			context.Expect(ledger.Record(Candidate(view, 102u, 12u)), "Changed content records a replacement candidate.");
			context.Expect(!ledger.ConfirmSubmission(12u, 22u, false), "Failed queue submission must reject promotion.");
			context.Expect(!ledger.CanReuse(view, 102u), "Failed replacement content cannot become reusable.");
			context.Expect(!ledger.CanReuse(view, 101u), "A possibly overwritten allocation cannot revive its previous valid claim.");
			context.Expect(!ledger.ConfirmSubmission(12u, 23u, true), "Discarded failed work cannot be confirmed later without a fresh recording.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.SkippedRuntimeAndEditorWorkIsDiscarded", [](TestContext& context) {
			// Runtime fences and editor-owned completion proofs feed the same typed
			// lifecycle. Neither path may promote skipped recording on a later submit.
			for(const std::uint64_t command : {11u, 1011u})
			{
				ShadowCachePublicationLedger ledger(1u);
				const auto view = PublicationView();
				context.Expect(ledger.Record(Candidate(view, 101u, command)), "The selected submission path records a candidate.");
				ledger.Discard(command);
				context.Expect(!ledger.CanReuse(view, 101u), "Skipped runtime/editor work cannot authorize reuse.");
				context.Expect(!ledger.ConfirmSubmission(command, command + 100u, true), "Discarded work cannot be promoted by a later success receipt.");
			}
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.CubeFacesPublishAtomically", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			auto cube = PublicationView();
			cube.resourceKind = static_cast<ShadowResourceKind>(1u);
			const auto candidate = Candidate(cube, 101u, 11u, 0x3fu, 0x3fu);
			context.Expect(ledger.Record(candidate), "All six validated faces can record one cube candidate.");
			context.Expect(!ledger.CanReuse(cube, 101u), "Complete CPU face recording is still not submitted GPU content.");
			context.Expect(ledger.ConfirmSubmission(11u, 21u, true), "All six faces publish on one successful relevant submit.");
			context.Expect(ledger.CanReuse(cube, 101u), "An atomically submitted cube can be reused.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.IncompleteCubeCannotPublishOrRetainOldOwner", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			auto cube = PublicationView();
			cube.resourceKind = static_cast<ShadowResourceKind>(1u);
			Submit(context, ledger, Candidate(cube, 101u, 11u, 0x3fu, 0x3fu), 21u);
			// Five recorded faces imply a possibly modified physical cube. Record
			// may reject immediately or retain a non-promotable pending candidate.
			(void)ledger.Record(Candidate(cube, 102u, 12u, 0x1fu, 0x3fu));
			context.Expect(!ledger.CanReuse(cube, 101u), "Incomplete overwrite must conservatively revoke the old cube claim.");
			context.Expect(!ledger.ConfirmSubmission(12u, 22u, true), "Five of six faces cannot publish a cube owner.");
			context.Expect(!ledger.CanReuse(cube, 102u), "Incomplete cube content is never reusable.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.PendingWriterOrderPreventsEarlierPromotionHits", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto viewA = PublicationView(1u);
			const auto viewB = PublicationView(2u);
			context.Expect(ledger.Record(Candidate(viewA, 101u, 11u)), "First ordered command records A.");
			context.Expect(ledger.Record(Candidate(viewB, 102u, 12u)), "A later ordered command records B into the same region.");
			// Confirming A while a later overwrite is pending cannot resurrect an
			// A hit. Only B can become the last visible physical owner.
			(void)ledger.ConfirmSubmission(11u, 21u, true);
			context.Expect(!ledger.CanReuse(viewA, 101u), "An earlier successful submit cannot outrun a later pending overwrite.");
			context.Expect(ledger.ConfirmSubmission(12u, 22u, true), "The latest recorded writer can publish successfully.");
			context.Expect(ledger.CanReuse(viewB, 102u), "Only the latest visible physical owner can be reused.");
			context.Expect(!ledger.CanReuse(viewA, 101u), "Earlier logical content remains invalid after the overwrite submits.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.RecoveryDiscardsSubmittedAndPendingOwners", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto oldView = PublicationView();
			Submit(context, ledger, Candidate(oldView, 101u, 11u), 21u);
			context.Expect(ledger.Record(Candidate(PublicationView(2u), 102u, 12u)), "Pre-recovery pending work records.");
			ledger.Reset(2u);
			context.Expect(!ledger.CanReuse(oldView, 101u), "Recovery invalidates submitted old-session ownership.");
			context.Expect(!ledger.ConfirmSubmission(12u, 22u, true), "Recovery discards pending old-session candidates.");
			context.Expect(!ledger.Record(Candidate(oldView, 103u, 13u)), "Old renderer-session keys cannot record after recovery.");
			auto newView = oldView;
			newView.rendererSessionGeneration = 2u;
			Submit(context, ledger, Candidate(newView, 201u, 21u), 31u);
			context.Expect(ledger.CanReuse(newView, 201u), "The recovered session can establish fresh ownership.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.AliasPreservesSubmittedTokenAndReceiptsWithoutWrite", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto source = PublicationView(1u);
			auto destination = PublicationView(2u);
			destination.viewType = static_cast<RenderViewType>(1u); // Scene instead of Game.
			auto candidate = Candidate(source, 101u, 11u);
			candidate.membership = AliasMembership();
			Submit(context, ledger, candidate, 21u);
			context.Expect(ledger.AliasSubmittedContent(destination, 101u, candidate.membership), "An exact compatible view can alias actually submitted physical content.");
			context.Expect(ledger.CanReuse(source, 101u) && ledger.CanReuse(destination, 101u), "Both qualified logical keys resolve the same physical submitted content.");
			const auto alias = ledger.Find(destination);
			context.Expect(alias && alias->status == ShadowCachePublicationState::Submitted, "The alias reports submitted state without pretending to record a write.");
			context.ExpectEqual(std::uint64_t{101}, alias->contentToken, "Alias preserves the actual content token.");
			context.ExpectEqual(std::uint64_t{11}, alias->commandSerial, "Alias preserves the original recorded command receipt.");
			context.ExpectEqual(std::uint64_t{21}, alias->submitSerial, "Alias preserves the original successful submission receipt.");
			context.ExpectEqual(std::uint64_t{0}, alias->completionSerial, "Alias cannot manufacture completion.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.AliasRejectsExactViewContentMismatch", [](TestContext& context) {
			ExpectAliasMismatch(context, [](auto& view, auto&) { ++view.referenceOrigin.cell[0]; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { view.referenceOrigin.offsetMetres[0] = 0.25; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { ++view.lightId[0]; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { view.lightProjectionStamp.viewProjection[0] = 2.0; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { view.lightProjectionStamp.lightParameters[0] = 0.5; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { view.lightProjectionStamp.shadowFilterParameters[0] = 3.0; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { ++view.lightProjectionStamp.semanticRevision; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { ++view.extent[0]; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { ++view.depthFormat; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { ++view.allocationGeneration; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { ++view.regionId; });
			ExpectAliasMismatch(context, [](auto& view, auto&) { view.resourceKind = static_cast<ShadowResourceKind>(2u); });
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.AliasRejectsEqualDigestWithUnequalSemanticRows", [](TestContext& context) {
			ExpectAliasMismatch(context, [](auto&, auto& membership) { membership.rows[0].deformationPalette.push_back({}); membership.rows[0].deformationPalette.back()[3] = 0.5; });
			ExpectAliasMismatch(context, [](auto&, auto& membership) { ++membership.rows[0].segmentId; });
			ExpectAliasMismatch(context, [](auto&, auto& membership) { ++membership.rows[0].lifetimeGeneration; });
			ExpectAliasMismatch(context, [](auto&, auto& membership) { membership.rows.clear(); });
			ExpectAliasMismatch(context, [](auto&, auto& membership) { membership.reusable = false; });
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.AliasRequiresSubmittedOwnerExactTokenAndSession", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto source = PublicationView(1u);
			const auto destination = PublicationView(2u);
			auto candidate = Candidate(source, 101u, 11u);
			candidate.membership = AliasMembership();
			context.Expect(ledger.Prepare(candidate), "The original work prepares.");
			context.Expect(!ledger.AliasSubmittedContent(destination, 101u, candidate.membership), "Prepared content cannot be aliased.");
			context.Expect(ledger.Record(candidate), "The original work records.");
			context.Expect(!ledger.AliasSubmittedContent(destination, 101u, candidate.membership), "Recorded content cannot be aliased.");
			context.Expect(ledger.ConfirmSubmission(11u, 21u, true), "The original work submits successfully.");
			context.Expect(!ledger.AliasSubmittedContent(destination, 999u, candidate.membership), "An incorrect expected physical token cannot alias.");
			auto wrongSession = destination;
			wrongSession.rendererSessionGeneration = 2u;
			context.Expect(!ledger.AliasSubmittedContent(wrongSession, 101u, candidate.membership), "Another renderer session cannot alias current content.");
			context.Expect(ledger.AliasSubmittedContent(destination, 101u, candidate.membership), "Matching submitted owner, token and session authorize aliasing.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.AliasPendingOverwriteRevokesEveryLogicalClaim", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto source = PublicationView(1u);
			const auto aliasView = PublicationView(2u);
			auto candidate = Candidate(source, 101u, 11u);
			candidate.membership = AliasMembership();
			Submit(context, ledger, candidate, 21u);
			context.Expect(ledger.AliasSubmittedContent(aliasView, 101u, candidate.membership), "The initial exact alias is admitted.");
			const auto writer = PublicationView(3u);
			context.Expect(ledger.Record(Candidate(writer, 102u, 12u)), "A third view records an overwrite of the shared region.");
			context.Expect(!ledger.CanReuse(source, 101u) && !ledger.CanReuse(aliasView, 101u), "Recorded overwrite immediately revokes both owner and alias claims.");
			context.Expect(!ledger.AliasSubmittedContent(PublicationView(4u), 101u, candidate.membership), "Pending writes forbid new aliases of the old submitted token.");
			ledger.Discard(12u);
			context.Expect(!ledger.CanReuse(source, 101u) && !ledger.CanReuse(aliasView, 101u), "Discard cannot revive claims after possible physical modification.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.AliasCompletionPropagatesOriginalProof", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto source = PublicationView(1u);
			const auto destination = PublicationView(2u);
			auto candidate = Candidate(source, 101u, 11u);
			candidate.membership = AliasMembership();
			Submit(context, ledger, candidate, 21u);
			context.Expect(ledger.AliasSubmittedContent(destination, 101u, candidate.membership), "An alias is admitted before delayed completion.");
			ledger.Complete(21u, 31u);
			const auto alias = ledger.Find(destination);
			context.Expect(alias && alias->status == ShadowCachePublicationState::Complete, "Completion proof propagates to qualified aliases of its submitted content.");
			context.ExpectEqual(std::uint64_t{31}, alias->completionSerial, "Alias retains the actual supplied completion receipt.");
			const auto later = PublicationView(3u);
			context.Expect(ledger.AliasSubmittedContent(later, 101u, candidate.membership), "Already completed exact content remains aliasable.");
			context.ExpectEqual(std::uint64_t{31}, ledger.Find(later)->completionSerial, "Later aliases copy existing completion proof.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.AliasCubeRequiresAllSixExactProjectionProofs", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			auto source = PublicationView(1u);
			source.resourceKind = static_cast<ShadowResourceKind>(1u);
			auto candidate = Candidate(source, 101u, 11u, 0x3fu, 0x3fu);
			candidate.membership = AliasMembership();
			Submit(context, ledger, candidate, 21u);
			auto destination = source;
			destination.viewId = 2u;
			context.Expect(ledger.AliasSubmittedContent(destination, 101u, candidate.membership), "A complete six-face cube with exact projections can alias.");
			for(std::size_t face = 0u; face < 6u; ++face)
			{
				auto changed = source;
				changed.viewId = static_cast<std::uint32_t>(3u + face);
				changed.lightProjectionStamp.cubeViewProjections[face][0] = 2.0;
				context.Expect(!ledger.AliasSubmittedContent(changed, 101u, candidate.membership), "Every cube face projection participates in exact alias compatibility.");
			}
			auto unresolved = destination;
			unresolved.viewId = 10u;
			unresolved.lightProjectionStamp.cubeProjectionEvidenceComplete = false;
			context.Expect(!ledger.AliasSubmittedContent(unresolved, 101u, candidate.membership), "Unavailable cube projection proof disables alias reuse.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.BatchSharesOneActualSubmitReceiptAcrossCommands", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto viewA = PublicationView(1u, 1u);
			const auto viewB = PublicationView(2u, 2u);
			context.Expect(ledger.Record(Candidate(viewA, 101u, 11u)), "First view command records.");
			context.Expect(ledger.Record(Candidate(viewB, 102u, 12u)), "Second view command records independently.");
			const std::array<std::uint64_t, 2> commands{11u, 12u};
			context.Expect(ledger.ConfirmSubmissionBatch(commands, 21u, true), "One actual Vulkan submission promotes every covered command with the same receipt.");
			context.Expect(ledger.CanReuse(viewA, 101u) && ledger.CanReuse(viewB, 102u), "Both independent submitted regions become reusable.");
			context.ExpectEqual(std::uint64_t{21}, ledger.Find(viewA)->submitSerial, "First command retains the actual shared submit receipt.");
			context.ExpectEqual(std::uint64_t{21}, ledger.Find(viewB)->submitSerial, "Second command does not manufacture a separate submit event.");
			ledger.Complete(21u, 31u);
			context.ExpectEqual(std::uint64_t{31}, ledger.Find(viewA)->completionSerial, "Completion resolves the first command.");
			context.ExpectEqual(std::uint64_t{31}, ledger.Find(viewB)->completionSerial, "The same completion proof resolves the second command.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.BatchPhysicalOwnerFollowsActualGpuCommandOrder", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto viewA = PublicationView(1u);
			const auto viewB = PublicationView(2u);
			context.Expect(ledger.Record(Candidate(viewA, 101u, 11u)), "CPU first records A.");
			context.Expect(ledger.Record(Candidate(viewB, 102u, 12u)), "CPU next records B into the same image.");
			// Submission array order can differ from recording order. The final
			// actual GPU writer, not CPU scheduling history, owns the physical depth.
			const std::array<std::uint64_t, 2> commands{12u, 11u};
			context.Expect(ledger.ConfirmSubmissionBatch(commands, 21u, true), "A successful batch uses its supplied actual command order.");
			context.Expect(ledger.CanReuse(viewA, 101u), "A is the final writer in the actual submitted command array.");
			context.Expect(!ledger.CanReuse(viewB, 102u), "B cannot reuse content overwritten later by A on the GPU.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.BatchFailureDiscardsEveryCoveredCommand", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto viewA = PublicationView(1u, 1u);
			const auto viewB = PublicationView(2u, 2u);
			Submit(context, ledger, Candidate(viewA, 101u, 11u), 21u);
			Submit(context, ledger, Candidate(viewB, 102u, 12u), 22u);
			context.Expect(ledger.Record(Candidate(viewA, 103u, 13u)), "A replacement records.");
			context.Expect(ledger.Record(Candidate(viewB, 104u, 14u)), "B replacement records.");
			const std::array<std::uint64_t, 2> commands{13u, 14u};
			context.Expect(!ledger.ConfirmSubmissionBatch(commands, 23u, false), "A failed actual submit cannot promote any covered command.");
			context.Expect(!ledger.CanReuse(viewA, 101u) && !ledger.CanReuse(viewA, 103u), "Failure cannot retain old or replacement ownership for A.");
			context.Expect(!ledger.CanReuse(viewB, 102u) && !ledger.CanReuse(viewB, 104u), "Failure cannot retain old or replacement ownership for B.");
			context.Expect(!ledger.ConfirmSubmissionBatch(commands, 24u, true), "Discarded failed commands require fresh recording before later success.");
			context.Expect(!ledger.ConfirmSubmission(13u, 24u, true), "Failure removes the first pending command even from singleton confirmation.");
			context.Expect(!ledger.ConfirmSubmission(14u, 25u, true), "Failure removes every covered pending command from singleton confirmation.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.BatchRejectsOlderAndReplayedSubmitReceipts", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			Submit(context, ledger, Candidate(PublicationView(1u, 1u), 101u, 11u), 21u);
			const auto candidateView = PublicationView(2u, 2u);
			context.Expect(ledger.Record(Candidate(candidateView, 102u, 12u)), "An independent replacement records.");
			const std::array<std::uint64_t, 1> replay{12u};
			context.Expect(!ledger.ConfirmSubmissionBatch(replay, 21u, true), "Replaying a receipt for new commands cannot invent another accepted submit.");
			context.Expect(!ledger.CanReuse(candidateView, 102u), "Replayed receipt cannot establish ownership.");
			context.Expect(ledger.Record(Candidate(candidateView, 103u, 13u)), "A fresh candidate records after replay rejection.");
			const std::array<std::uint64_t, 1> older{13u};
			context.Expect(!ledger.ConfirmSubmissionBatch(older, 20u, true), "An older actual submit serial cannot supersede later ownership.");
			context.Expect(!ledger.CanReuse(candidateView, 103u), "An older receipt cannot establish replacement ownership.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.BatchUnknownOrDuplicateCommandsNeverPartiallyPromote", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto view = PublicationView();
			context.Expect(ledger.Record(Candidate(view, 101u, 11u)), "One known command records before malformed confirmation.");
			const std::array<std::uint64_t, 2> unknown{11u, 99u};
			context.Expect(!ledger.ConfirmSubmissionBatch(unknown, 21u, true), "An unknown covered command rejects the entire confirmation batch.");
			context.Expect(!ledger.CanReuse(view, 101u), "Malformed command coverage cannot partially promote its known member.");
			context.Expect(ledger.Record(Candidate(view, 102u, 12u)), "A fresh candidate records for duplicate validation.");
			const std::array<std::uint64_t, 2> duplicate{12u, 12u};
			context.Expect(!ledger.ConfirmSubmissionBatch(duplicate, 22u, true), "Duplicate command coverage is not a valid ordered submission receipt.");
			context.Expect(!ledger.CanReuse(view, 102u), "Duplicate coverage cannot establish ownership.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.BatchKeepsIndependentCompleteLightsSeparateFromIncompleteCube", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto viewA = PublicationView(1u, 1u);
			const auto viewB = PublicationView(2u, 2u);
			auto cube = PublicationView(3u, 3u);
			cube.resourceKind = static_cast<ShadowResourceKind>(1u);
			context.Expect(ledger.Record(Candidate(viewA, 101u, 11u)), "A complete independent light records in the shared command.");
			context.Expect(ledger.Record(Candidate(viewB, 102u, 11u)), "Another independent light records in that same command.");
			context.Expect(!ledger.Record(Candidate(cube, 103u, 11u, 0x1fu, 0x3fu)), "An incomplete cube cannot become a promotable candidate.");
			const std::array<std::uint64_t, 1> commands{11u};
			context.Expect(ledger.ConfirmSubmissionBatch(commands, 21u, true), "Successful submission can publish independent complete lights from the shared command.");
			context.Expect(ledger.CanReuse(viewA, 101u) && ledger.CanReuse(viewB, 102u), "Both fully recorded independent physical regions retain their proof.");
			context.Expect(!ledger.CanReuse(cube, 103u), "A successful shared command cannot invent the cube's missing face.");
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.RetentionReplacesChangingContentWithinLogicalSlot", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto original = PublicationView();
			Submit(context, ledger, Candidate(original, 101u, 11u), 21u);
			for(std::uint64_t step = 0u; step < 300u; ++step)
			{
				auto current = original;
				current.referenceOrigin.offsetMetres[0] = static_cast<double>(step) + 0.25;
				current.lightProjectionStamp.viewProjection[0] = 2.0 + static_cast<double>(step);
				current.extent[0] += static_cast<std::uint32_t>(step) + 1u;
				Submit(context, ledger, Candidate(current, 1000u + step, 100u + step), 200u + step);
				context.Expect(ledger.CanReuse(current, 1000u + step), "The current exact content remains reusable after logical-slot replacement.");
				context.ExpectEqual(std::size_t{1}, ledger.RetainedLogicalEntryCount(), "Moving origin/projection/shape cannot retain historical caster arrays in one logical slot.");
				context.Expect(!ledger.Find(original).has_value(), "An obsolete exact key is removed when its logical slot is replaced.");
				context.Expect(!ledger.CanReuse(original, 101u), "Historical exact content cannot regain an owner claim.");
			}
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.RetentionBoundsChangingOwnerAndSecondViewAlias", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto original = PublicationView(1u);
			for(std::uint64_t step = 0u; step < 300u; ++step)
			{
				auto current = original;
				current.referenceOrigin.offsetMetres[1] = static_cast<double>(step) + 0.25;
				current.lightProjectionStamp.viewProjection[5] = 2.0 + static_cast<double>(step);
				current.extent[1] += static_cast<std::uint32_t>(step);
				auto candidate = Candidate(current, 1000u + step, 100u + step);
				candidate.membership = AliasMembership();
				Submit(context, ledger, candidate, 200u + step);
				auto alias = current;
				alias.viewId = 2u;
				context.Expect(ledger.AliasSubmittedContent(alias, candidate.contentToken, candidate.membership), "Each current exact frame content can acquire its second-view alias.");
				context.Expect(ledger.CanReuse(current, candidate.contentToken) && ledger.CanReuse(alias, candidate.contentToken), "Both current logical keys retain physical proof.");
				context.ExpectEqual(std::size_t{2}, ledger.RetainedLogicalEntryCount(), "Changing content retains one slot per logical view rather than every past alias stamp.");
			}
		});

		registry.Add("Renderer.Efficiency.ShadowPublication.RetentionReplacesReassignedLightsInSamePhysicalViewSlot", [](TestContext& context) {
			ShadowCachePublicationLedger ledger(1u);
			const auto original = PublicationView();
			Submit(context, ledger, Candidate(original, 101u, 11u), 21u);
			for(std::uint64_t step = 0u; step < 300u; ++step)
			{
				auto reassigned = original;
				reassigned.lightId[0] = static_cast<std::uint8_t>(step % 255u + 1u);
				reassigned.lightId[1] = static_cast<std::uint8_t>(step / 255u + 1u);
				Submit(context, ledger, Candidate(reassigned, 1000u + step, 100u + step), 200u + step);
				context.Expect(ledger.CanReuse(reassigned, 1000u + step), "The current assigned light owns this view's physical region.");
				context.ExpectEqual(std::size_t{1}, ledger.RetainedLogicalEntryCount(), "Light identity churn cannot retain stale caster arrays in one physical view slot.");
				context.Expect(!ledger.Find(original).has_value(), "The previous exact light key is absent after slot reassignment.");
				context.Expect(!ledger.CanReuse(original, 101u), "A prior light cannot reuse reassigned physical content.");
			}
		});
	}
}
