// AnimationDiagnosticsWindowTests.cpp - copied diagnostic and overlay contracts.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Animation/AnimationOverlays.h"
#include "Editor/TerraEngine-Editor/Windows/AnimationDiagnosticsWindow.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	using namespace TerraEngine::Editor::Animation;

	namespace {
		TerraEngine::Animation::AnimationDiagnosticsSnapshot MakeSummary() {
			TerraEngine::Animation::AnimationDiagnosticsSnapshot summary;
			summary.frameIndex = 44U;
			summary.fixedTick = 120;
			summary.detailLevel = TerraEngine::Animation::AnimationDiagnosticsDetailLevel::Detailed;
			summary.dormant = false;
			summary.counters.activeAnimators = 2U;
			summary.counters.evaluatedAnimators = 2U;
			summary.counters.gpuVisualAnimators = 1U;
			summary.counters.cpuFallbackAnimators = 1U;
			return summary;
		}

		AnimationDiagnosticsRow MakeRow(
			AnimationDiagnosticsCategory category,
			std::string label,
			std::string value,
			bool rendererDependent = false) {
			AnimationDiagnosticsRow row;
			row.category = category;
			row.stableInstanceId = 77U;
			row.label = std::move(label);
			row.value = std::move(value);
			row.accessibilityText = row.label + ": " + row.value;
			row.rendererDependent = rendererDependent;
			return row;
		}

		AnimationOverlayPrimitive MakePrimitive(AnimationOverlayType type, std::uint64_t id) {
			AnimationOverlayPrimitive primitive;
			primitive.type = type;
			primitive.stableId = id;
			primitive.start = {static_cast<float>(id), 1.0F, 2.0F};
			primitive.end = {static_cast<float>(id + 1U), 3.0F, 4.0F};
			primitive.color = {0.25F, 0.5F, 0.75F, 1.0F};
			primitive.label = std::string(ToString(type));
			primitive.accessibilityText = primitive.label + " animation overlay";
			return primitive;
		}
	}

	void RegisterAnimationDiagnosticsWindowTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.Diagnostics.DisabledAndCollapsedRequestZeroDetailedWork", [](TestContext& context) {
			AnimationDiagnosticsPresentationModel model;
			AnimationDiagnosticsOptions options;
			options.enabled = false;
			options.collapsed = false;
			options.rendererAvailable = true;
			model.Configure(options);
			std::uint32_t collectorCalls = 0U;
			const AnimationDiagnosticsCollector collector = [&](
				std::uint32_t,
				std::size_t,
				std::vector<AnimationDiagnosticsRow>& rows) {
				++collectorCalls;
				rows.push_back(MakeRow(AnimationDiagnosticsCategory::State, "State", "Walk"));
				return AnimationDiagnosticsStatus::Ok;
			};
			AnimationDiagnosticsPreparedSnapshot prepared;
			context.Expect(model.Prepare(MakeSummary(), 9U, collector, prepared)
				== AnimationDiagnosticsStatus::Disabled
				&& collectorCalls == 0U
				&& prepared.snapshot.rows.empty()
				&& prepared.snapshot.instrumentation.detailedCollectionRequests == 0U
				&& !prepared.snapshot.instrumentation.performanceTimingAttempted,
				"Disabled diagnostics must prove zero detailed collection without running a performance timer.");
			context.Expect(model.Publish(prepared) == AnimationDiagnosticsStatus::Disabled,
				"The disabled result schema should still publish an explicit inspectable state.");

			options.enabled = true;
			options.collapsed = true;
			model.Configure(options);
			context.Expect(model.Prepare(MakeSummary(), 10U, collector, prepared)
				== AnimationDiagnosticsStatus::Collapsed
				&& collectorCalls == 0U
				&& prepared.snapshot.rows.empty()
				&& prepared.snapshot.work.workerSafePreparation
				&& prepared.snapshot.work.protectedPublication,
				"Collapsed diagnostics must skip expanded collection and expose scheduler boundary declarations.");
			context.Expect(prepared.snapshot.schemaVersion == kAnimationDiagnosticsEditorSchemaVersion
				&& prepared.snapshot.instrumentation.resultSchemaValidated,
				"Intermediate tests validate the bounded result schema without executing final-phase timing acceptance.");
		});

		registry.Add("Editor.Animation.Diagnostics.AllCategoriesCopyImmutablyAndRemainBounded", [](TestContext& context) {
			std::vector<AnimationDiagnosticsRow> sourceRows{
				MakeRow(AnimationDiagnosticsCategory::Profiler, "Profiler", "2 evaluated"),
				MakeRow(AnimationDiagnosticsCategory::State, "State", "Locomotion"),
				MakeRow(AnimationDiagnosticsCategory::Pose, "Pose source", "BlendTree"),
				MakeRow(AnimationDiagnosticsCategory::Event, "Event", "Footstep"),
				MakeRow(AnimationDiagnosticsCategory::RootMotion, "Root motion", "0.2m"),
				MakeRow(AnimationDiagnosticsCategory::Ik, "IK error", "0.01"),
				MakeRow(AnimationDiagnosticsCategory::Retarget, "Retarget error", "0.02"),
				MakeRow(AnimationDiagnosticsCategory::MotionMatching, "MM candidate", "clip 4"),
				MakeRow(AnimationDiagnosticsCategory::Ragdoll, "Ragdoll", "Recovering"),
				MakeRow(AnimationDiagnosticsCategory::Bounds, "Bounds", "Conservative", true),
				MakeRow(AnimationDiagnosticsCategory::Residency, "Residency", "GPU resident", true),
				MakeRow(AnimationDiagnosticsCategory::Mode, "Mode", "GPU Visual", true)};

			AnimationDiagnosticsPresentationModel model;
			AnimationDiagnosticsOptions options;
			options.enabled = true;
			options.collapsed = false;
			options.rendererAvailable = true;
			options.maximumRows = sourceRows.size();
			model.Configure(options);
			AnimationDiagnosticsPreparedSnapshot prepared;
			context.Expect(model.Prepare(MakeSummary(), 11U,
				[&](std::uint32_t, std::size_t, std::vector<AnimationDiagnosticsRow>& rows) {
					rows = sourceRows;
					return AnimationDiagnosticsStatus::Ok;
				}, prepared) == AnimationDiagnosticsStatus::Ok,
				"Expanded opt-in diagnostics should copy every required editor category from one immutable source generation.");
			sourceRows.front().value = "producer mutated";
			context.Expect(model.Publish(prepared) == AnimationDiagnosticsStatus::Ok,
				"A complete prepared diagnostic snapshot should publish at the protected UI boundary.");
			const AnimationDiagnosticsWindowSnapshot retained = model.Snapshot();
			context.Expect(retained.rows.size() == 12U
				&& retained.rows.front().value == "2 evaluated"
				&& retained.sourceGeneration == 11U
				&& retained.summary.frameIndex == 44U,
				"Published diagnostics must not retain producer-owned row or summary storage.");
			bool accessible = !retained.accessibilityText.empty();
			std::uint32_t categoryMask = 0U;
			for(const AnimationDiagnosticsRow& row : retained.rows) {
				accessible = accessible && !row.accessibilityText.empty();
				categoryMask |= AnimationDiagnosticsCategoryMask(row.category);
			}
			context.Expect(accessible && categoryMask == kAllAnimationDiagnosticsCategories,
				"Every profiler/state/pose/event/root/IK/retarget/MM/ragdoll/bounds/residency/mode row needs accessible text.");
		});

		registry.Add("Editor.Animation.Diagnostics.RendererLossStalePublicationAndLifetimeCleanup", [](TestContext& context) {
			AnimationDiagnosticsPresentationModel model;
			AnimationDiagnosticsOptions options;
			options.enabled = true;
			options.collapsed = false;
			options.rendererAvailable = false;
			options.maximumRows = 3U;
			model.Configure(options);
			AnimationDiagnosticsPreparedSnapshot prepared;
			context.Expect(model.Prepare(MakeSummary(), 12U,
				[](std::uint32_t, std::size_t, std::vector<AnimationDiagnosticsRow>& rows) {
					rows = {
						MakeRow(AnimationDiagnosticsCategory::State, "State", "Walk"),
						MakeRow(AnimationDiagnosticsCategory::Bounds, "GPU bounds", "Unavailable", true),
						MakeRow(AnimationDiagnosticsCategory::Event, "Event", "Land"),
						MakeRow(AnimationDiagnosticsCategory::Ik, "IK", "Stable")};
					return AnimationDiagnosticsStatus::Ok;
				}, prepared) == AnimationDiagnosticsStatus::RendererUnavailable
				&& prepared.snapshot.rows.size() == 3U
				&& prepared.snapshot.instrumentation.rendererRowsSuppressed == 1U
				&& prepared.snapshot.instrumentation.rowsDropped == 0U
				&& prepared.snapshot.accessibilityText.find("renderer unavailable") != std::string::npos,
				"Renderer loss should remain explicit while preserving bounded CPU-authoritative diagnostics.");
			context.Expect(model.Publish(prepared) == AnimationDiagnosticsStatus::RendererUnavailable,
				"Renderer-unavailable diagnostics remain a valid complete publication.");

			AnimationDiagnosticsPreparedSnapshot stale;
			context.Expect(model.Prepare(MakeSummary(), 13U,
				[](std::uint32_t, std::size_t, std::vector<AnimationDiagnosticsRow>&) {
					return AnimationDiagnosticsStatus::Ok;
				}, stale) == AnimationDiagnosticsStatus::RendererUnavailable,
				"A second immutable source may prepare against the current presentation revision.");
			options.maximumRows = 2U;
			model.Configure(options);
			const AnimationDiagnosticsWindowSnapshot beforeStale = model.Snapshot();
			context.Expect(model.Publish(stale) == AnimationDiagnosticsStatus::Stale,
				"A prepared snapshot from an old window configuration must reject without replacing the retained value.");
			const AnimationDiagnosticsWindowSnapshot afterStale = model.Snapshot();
			context.Expect(afterStale.publicationGeneration == beforeStale.publicationGeneration
				&& afterStale.sourceGeneration == beforeStale.sourceGeneration
				&& afterStale.status == beforeStale.status
				&& afterStale.rows == beforeStale.rows,
				"Stale diagnostic publication must preserve the complete last-known-good presentation snapshot.");
			model.Close();
			const AnimationDiagnosticsWindowSnapshot closed = model.Snapshot();
			context.Expect(closed.status == AnimationDiagnosticsStatus::Disabled
				&& closed.rows.empty()
				&& closed.instrumentation.detailedCollectionRequests == 0U,
				"Closing the window must release detailed rows and leave no expensive collection request active.");
		});

		registry.Add("Editor.Animation.Overlays.DisabledCollapsedAndRendererLossCollectZeroPrimitives", [](TestContext& context) {
			AnimationOverlays overlays;
			AnimationOverlayOptions options;
			options.enabled = false;
			options.expanded = true;
			options.rendererAvailable = true;
			overlays.Configure(options);
			std::uint32_t collectorCalls = 0U;
			const AnimationOverlayCollector collector = [&](
				std::uint32_t,
				std::size_t,
				std::vector<AnimationOverlayPrimitive>& primitives) {
				++collectorCalls;
				primitives.push_back(MakePrimitive(AnimationOverlayType::Skeleton, 1U));
				return AnimationOverlayStatus::Ok;
			};
			AnimationOverlayPreparedPacket prepared;
			const AnimationOverlaySourceHeader source{7U, 8U};
			context.Expect(overlays.Prepare(source, collector, prepared) == AnimationOverlayStatus::Disabled
				&& collectorCalls == 0U
				&& prepared.snapshot.primitives.empty(),
				"Disabled overlays must not invoke the expensive Scene View primitive collector.");

			options.enabled = true;
			options.expanded = false;
			overlays.Configure(options);
			context.Expect(overlays.Prepare(source, collector, prepared) == AnimationOverlayStatus::Collapsed
				&& collectorCalls == 0U
				&& prepared.snapshot.instrumentation.collectionRequests == 0U,
				"Collapsed overlay controls must retain zero expanded collection work.");

			options.expanded = true;
			options.rendererAvailable = false;
			overlays.Configure(options);
			context.Expect(overlays.Prepare(source, collector, prepared) == AnimationOverlayStatus::RendererUnavailable
				&& collectorCalls == 0U
				&& prepared.snapshot.accessibilityText.find("renderer unavailable") != std::string::npos,
				"Renderer loss must skip render primitive collection and publish an accessible explicit unavailable state.");
		});

		registry.Add("Editor.Animation.Overlays.AllKindsAreCopiedFilteredAndBounded", [](TestContext& context) {
			std::vector<AnimationOverlayPrimitive> source{
				MakePrimitive(AnimationOverlayType::Skeleton, 1U),
				MakePrimitive(AnimationOverlayType::Socket, 2U),
				MakePrimitive(AnimationOverlayType::RootTrajectory, 3U),
				MakePrimitive(AnimationOverlayType::IkTarget, 4U),
				MakePrimitive(AnimationOverlayType::Contact, 5U),
				MakePrimitive(AnimationOverlayType::Ragdoll, 6U),
				MakePrimitive(AnimationOverlayType::MorphBounds, 7U),
				MakePrimitive(AnimationOverlayType::SkinnedBounds, 8U),
				MakePrimitive(AnimationOverlayType::VirtualGeometryCoverage, 9U)};
			// Production collectors may rely on the visible label. The publisher must
			// derive an accessible fallback before validation rather than rejecting a
			// complete primitive solely because that optional text was omitted.
			source.back().accessibilityText.clear();
			AnimationOverlays overlays;
			AnimationOverlayOptions options;
			options.enabled = true;
			options.expanded = true;
			options.rendererAvailable = true;
			options.enabledKinds = kAllAnimationOverlayKinds;
			options.maximumPrimitives = source.size();
			overlays.Configure(options);
			AnimationOverlayPreparedPacket prepared;
			context.Expect(overlays.Prepare({21U, 22U},
				[&](std::uint32_t, std::size_t, std::vector<AnimationOverlayPrimitive>& primitives) {
					primitives = source;
					return AnimationOverlayStatus::Ok;
				}, prepared) == AnimationOverlayStatus::Ok,
				"Every opt-in animation overlay kind should prepare from one immutable renderer generation.");
			source.front().label = "producer mutated";
			context.Expect(overlays.Publish(prepared) == AnimationOverlayStatus::Ok,
				"A complete bounded overlay packet should publish at the Scene View protected boundary.");
			const AnimationOverlaySnapshot retained = overlays.Snapshot();
			std::uint32_t retainedKinds = 0U;
			bool accessible = retained.primitives.size() == 9U;
			for(const AnimationOverlayPrimitive& primitive : retained.primitives) {
				retainedKinds |= AnimationOverlayTypeMask(primitive.type);
				accessible = accessible && !primitive.accessibilityText.empty();
			}
			context.Expect(retainedKinds == kAllAnimationOverlayKinds
				&& retained.primitives.front().label == "Skeleton"
				&& accessible
				&& retained.work.workerSafePreparation
				&& retained.work.protectedPublication,
				"The published packet must be an accessible copy with visible worker/protected boundaries.");

			options.enabledKinds = AnimationOverlayTypeMask(AnimationOverlayType::Skeleton)
				| AnimationOverlayTypeMask(AnimationOverlayType::Socket)
				| AnimationOverlayTypeMask(AnimationOverlayType::Contact);
			options.maximumPrimitives = 2U;
			overlays.Configure(options);
			context.Expect(overlays.Prepare({23U, 24U},
				[&](std::uint32_t, std::size_t, std::vector<AnimationOverlayPrimitive>& primitives) {
					primitives = source;
					return AnimationOverlayStatus::Ok;
				}, prepared) == AnimationOverlayStatus::Ok
				&& prepared.snapshot.primitives.size() == 2U
				&& prepared.snapshot.instrumentation.filteredPrimitives == 6U
				&& prepared.snapshot.instrumentation.droppedPrimitives == 1U,
				"Overlay filtering and truncation must enforce reviewed category and packet bounds deterministically.");
		});

		registry.Add("Editor.Animation.Overlays.InvalidAndStalePacketsPreserveLastGoodThenCleanup", [](TestContext& context) {
			AnimationOverlays overlays;
			AnimationOverlayOptions options;
			options.enabled = true;
			options.expanded = true;
			options.rendererAvailable = true;
			overlays.Configure(options);
			AnimationOverlayPreparedPacket good;
			context.Expect(overlays.Prepare({31U, 32U},
				[](std::uint32_t, std::size_t, std::vector<AnimationOverlayPrimitive>& primitives) {
					primitives.push_back(MakePrimitive(AnimationOverlayType::Skeleton, 1U));
					return AnimationOverlayStatus::Ok;
				}, good) == AnimationOverlayStatus::Ok
				&& overlays.Publish(good) == AnimationOverlayStatus::Ok,
				"The fixture requires one last-known-good overlay packet.");
			const AnimationOverlaySnapshot retained = overlays.Snapshot();

			AnimationOverlayPreparedPacket invalid;
			context.Expect(overlays.Prepare({33U, 34U},
				[](std::uint32_t, std::size_t, std::vector<AnimationOverlayPrimitive>& primitives) {
					AnimationOverlayPrimitive value = MakePrimitive(AnimationOverlayType::IkTarget, 2U);
					value.start.x = std::numeric_limits<float>::quiet_NaN();
					primitives.push_back(value);
					return AnimationOverlayStatus::Ok;
				}, invalid) == AnimationOverlayStatus::InvalidArgument
				&& overlays.Snapshot() == retained,
				"A non-finite primitive must be rejected before publication without replacing the last complete packet.");

			AnimationOverlayPreparedPacket stale;
			context.Expect(overlays.Prepare({35U, 36U},
				[](std::uint32_t, std::size_t, std::vector<AnimationOverlayPrimitive>& primitives) {
					primitives.push_back(MakePrimitive(AnimationOverlayType::Contact, 3U));
					return AnimationOverlayStatus::Ok;
				}, stale) == AnimationOverlayStatus::Ok,
				"A valid packet should retain the settings generation used during preparation.");
			options.enabledKinds = AnimationOverlayTypeMask(AnimationOverlayType::Contact);
			overlays.Configure(options);
			context.Expect(overlays.Publish(stale) == AnimationOverlayStatus::Stale
				&& overlays.Snapshot() == retained,
				"Changing overlay settings must invalidate in-flight worker packets atomically.");
			overlays.Clear();
			context.Expect(overlays.Snapshot().primitives.empty()
				&& overlays.Snapshot().status == AnimationOverlayStatus::Disabled,
				"Scene or renderer teardown must release retained overlay primitives and invalidate old packets.");
		});
	}
}
