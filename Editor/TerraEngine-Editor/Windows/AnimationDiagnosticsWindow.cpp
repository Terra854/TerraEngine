// AnimationDiagnosticsWindow.cpp - accessible read-only diagnostic rendering.

#include "AnimationDiagnosticsWindow.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Animation/Runtime/PoseBuffer.h"
#include "Engine/Renderer/RenderSystem.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

using TerraEngine::Editor::Animation::AnimationDiagnosticsCategory;
using TerraEngine::Editor::Animation::AnimationDiagnosticsRow;
using TerraEngine::Editor::Animation::AnimationDiagnosticsStatus;
using TerraEngine::Editor::Animation::AnimationDiagnosticsWindowSnapshot;

namespace {
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::Editor::Animation;

	std::string FormatCount(std::uint64_t value) {
		return std::to_string(value);
	}

	std::string FormatTimings(const AnimationDiagnosticsTimings& timings) {
		std::ostringstream output;
		output << std::fixed << std::setprecision(3)
			<< "prepare " << timings.preparationMs << " ms, evaluate "
			<< timings.evaluationMs << " ms, publish " << timings.publicationMs
			<< " ms, present " << timings.presentationMs << " ms";
		return output.str();
	}

	std::string FormatBounds(
		const AnimationFloat3& minimum,
		const AnimationFloat3& maximum,
		bool available) {
		if(!available) return "No sealed model-space joint transforms are published.";
		std::ostringstream output;
		output << std::fixed << std::setprecision(3)
			<< "min (" << minimum.x << ", " << minimum.y << ", " << minimum.z
			<< "), max (" << maximum.x << ", " << maximum.y << ", " << maximum.z << ")";
		return output.str();
	}

	void AppendRow(
		std::vector<AnimationDiagnosticsRow>& rows,
		std::size_t maximumRows,
		AnimationDiagnosticsCategory category,
		std::uint64_t stableId,
		std::string label,
		std::string value,
		bool rendererDependent = false) {
		if(rows.size() >= maximumRows) return;
		AnimationDiagnosticsRow row;
		row.category = category;
		row.stableInstanceId = stableId;
		row.label = std::move(label);
		row.value = std::move(value);
		row.rendererDependent = rendererDependent;
		row.accessibilityText = std::string(ToString(category)) + ", "
			+ row.label + ": " + row.value;
		rows.push_back(std::move(row));
	}

	template<typename Snapshot, typename Copy>
	std::uint32_t CountPublishedRecords(
		AnimatorInstanceHandle animator,
		Copy&& copy) {
		std::uint32_t requiredCount = 0U;
		(void)copy(animator, std::span<Snapshot>{}, requiredCount);
		return requiredCount;
	}

	struct CopiedRuntimeDiagnostics final {
		AnimationProductionIntegrationSnapshot integration;
		std::optional<AnimationDetailedDiagnosticsSnapshot> detailed;
		std::vector<std::shared_ptr<const PublishedAnimatorPose>> poses;
	};

	AnimationDiagnosticsStatus CollectRuntimeRows(
		const AnimationDiagnosticsSnapshot& summary,
		const CopiedRuntimeDiagnostics& runtime,
		std::uint32_t enabledCategories,
		std::size_t maximumRows,
		std::vector<AnimationDiagnosticsRow>& rows) {
		rows.clear();
		rows.reserve(std::min<std::size_t>(maximumRows, 64U));
		std::uint64_t stableId = 1U;
		auto append = [&](AnimationDiagnosticsCategory category, std::string label,
			std::string value, bool rendererDependent = false) {
			if((enabledCategories & AnimationDiagnosticsCategoryMask(category)) == 0U) return;
			AppendRow(rows, maximumRows, category, stableId++, std::move(label),
				std::move(value), rendererDependent);
		};

		append(AnimationDiagnosticsCategory::Profiler, "Frame timings", FormatTimings(summary.timings));
		append(AnimationDiagnosticsCategory::State, "Animator states",
			"active " + FormatCount(summary.counters.activeAnimators)
			+ ", evaluated " + FormatCount(summary.counters.evaluatedAnimators)
			+ ", suspended " + FormatCount(summary.counters.suspendedAnimators)
			+ ", degraded " + FormatCount(summary.counters.degradedAnimators));

		std::uint64_t publishedJointCount = 0U;
		std::uint64_t publishedMorphCount = 0U;
		std::uint64_t publishedCurveCount = 0U;
		std::uint64_t publishedEventCount = 0U;
		std::uint64_t publishedRootMotionCount = 0U;
		std::uint64_t safePoseCount = 0U;
		std::uint64_t proceduralResultCount = 0U;
		std::uint64_t retargetSnapshotCount = 0U;
		std::uint64_t motionMatchSnapshotCount = 0U;
		std::uint64_t ragdollSnapshotCount = 0U;
		AnimationFloat3 minimum{
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()};
		AnimationFloat3 maximum{
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest()};
		bool hasBounds = false;
		AnimationSystem& animation = AnimationSystem::Instance();
		for(const std::shared_ptr<const PublishedAnimatorPose>& publication : runtime.poses) {
			if(!publication) continue;
			if(publication->safePose) ++safePoseCount;
			if(publication->snapshot && publication->snapshot->pose) {
				const PoseBuffer& pose = *publication->snapshot->pose;
				publishedJointCount += pose.Layout().jointCount;
				publishedMorphCount += pose.Layout().morphCount;
				publishedCurveCount += pose.Layout().curveCount;
				publishedEventCount += pose.Events().size();
				publishedRootMotionCount += pose.RootMotionDelta().has_value() ? 1U : 0U;
				for(const AnimationTransform& transform : pose.ModelTransforms()) {
					minimum.x = std::min(minimum.x, transform.translation.x);
					minimum.y = std::min(minimum.y, transform.translation.y);
					minimum.z = std::min(minimum.z, transform.translation.z);
					maximum.x = std::max(maximum.x, transform.translation.x);
					maximum.y = std::max(maximum.y, transform.translation.y);
					maximum.z = std::max(maximum.z, transform.translation.z);
					hasBounds = true;
				}
			}

			proceduralResultCount += CountPublishedRecords<NativeAnimationProceduralResultSnapshot>(
				publication->animator,
				[&animation](AnimatorInstanceHandle animator,
					std::span<NativeAnimationProceduralResultSnapshot> destination,
					std::uint32_t& requiredCount) {
					return animation.CopyProceduralResults(animator, destination, requiredCount);
				});
			retargetSnapshotCount += CountPublishedRecords<NativeAnimationRetargetSnapshot>(
				publication->animator,
				[&animation](AnimatorInstanceHandle animator,
					std::span<NativeAnimationRetargetSnapshot> destination,
					std::uint32_t& requiredCount) {
					return animation.CopyRetargetSnapshots(animator, destination, requiredCount);
				});
			motionMatchSnapshotCount += CountPublishedRecords<NativeAnimationMotionMatchSnapshot>(
				publication->animator,
				[&animation](AnimatorInstanceHandle animator,
					std::span<NativeAnimationMotionMatchSnapshot> destination,
					std::uint32_t& requiredCount) {
					return animation.CopyMotionMatchSnapshots(animator, destination, requiredCount);
				});
			ragdollSnapshotCount += CountPublishedRecords<NativeAnimationRagdollSnapshot>(
				publication->animator,
				[&animation](AnimatorInstanceHandle animator,
					std::span<NativeAnimationRagdollSnapshot> destination,
					std::uint32_t& requiredCount) {
					return animation.CopyRagdollSnapshots(animator, destination, requiredCount);
				});
		}

		append(AnimationDiagnosticsCategory::Pose, "Immutable pose publications",
			FormatCount(runtime.poses.size()) + " poses, " + FormatCount(publishedJointCount)
			+ " joints, " + FormatCount(publishedMorphCount) + " morphs, "
			+ FormatCount(publishedCurveCount) + " curves, " + FormatCount(safePoseCount)
			+ " safe-pose fallbacks");
		append(AnimationDiagnosticsCategory::Event, "Published event records",
			FormatCount(publishedEventCount) + " current-pose events; queue depth "
			+ FormatCount(summary.counters.eventQueueDepth));
		append(AnimationDiagnosticsCategory::RootMotion, "Published root motion",
			FormatCount(publishedRootMotionCount) + " current deltas; queue depth "
			+ FormatCount(summary.counters.rootMotionQueueDepth));
		append(AnimationDiagnosticsCategory::Ik, "Procedural and IK observations",
			FormatCount(proceduralResultCount) + " copied result records");
		append(AnimationDiagnosticsCategory::Retarget, "Retarget observations",
			FormatCount(retargetSnapshotCount) + " copied result records");
		append(AnimationDiagnosticsCategory::MotionMatching, "Motion-match observations",
			FormatCount(motionMatchSnapshotCount) + " copied result records");
		append(AnimationDiagnosticsCategory::Ragdoll, "Ragdoll observations",
			FormatCount(ragdollSnapshotCount) + " copied result records; physics handoffs "
			+ FormatCount(summary.counters.physicsHandoffs));
		append(AnimationDiagnosticsCategory::Bounds, "Published joint-position bounds",
			FormatBounds(minimum, maximum, hasBounds), false);
		append(AnimationDiagnosticsCategory::Residency, "Animation residency",
			"hits " + FormatCount(summary.counters.residencyHits)
			+ ", misses " + FormatCount(summary.counters.residencyMisses)
			+ ", requests " + FormatCount(summary.counters.residencyRequests)
			+ ", memory " + FormatCount(runtime.integration.memory.totalBytesInUse) + " B");
		append(AnimationDiagnosticsCategory::Mode, "Execution modes",
			"GPU visual " + FormatCount(summary.counters.gpuVisualAnimators)
			+ ", hybrid " + FormatCount(summary.counters.hybridAuthoritativeAnimators)
			+ ", CPU fallback " + FormatCount(summary.counters.cpuFallbackAnimators)
			+ ", switches " + FormatCount(summary.counters.modeSwitchCount));

		if(runtime.detailed) {
			for(const AnimationAnimatorDiagnosticRecord& record : runtime.detailed->animators) {
				append(AnimationDiagnosticsCategory::Mode,
					"Animator " + std::to_string(record.stableInstanceId),
					std::string(ToString(record.effectiveMode)) + ", selection "
					+ std::string(ToString(record.selectionReason)) + ", fallback "
					+ std::string(ToString(record.fallbackReason)) + ", GPU stages 0x"
					+ [&record]() {
						std::ostringstream output;
						output << std::hex << record.gpuStageMask;
						return output.str();
					}());
				if(rows.size() >= maximumRows) break;
			}
		}
		return AnimationDiagnosticsStatus::Ok;
	}
}

AnimationDiagnosticsWindow::AnimationDiagnosticsWindow()
	: EditorWindow("Animation Diagnostics", false, "Animation/Diagnostics") {
}

AnimationDiagnosticsWindow::~AnimationDiagnosticsWindow() {
	model_.Close();
	detailedRequest_.Reset();
}

bool AnimationDiagnosticsWindow::OnCloseRequested() {
	// Closing releases this window's scoped request and its copied rows immediately.
	// Other consumers retain their own leases, while the runtime observes the final
	// release at its normal protected animation publication boundary.
	model_.Close();
	detailedRequest_.Reset();
	return true;
}

TerraEngine::Editor::Animation::AnimationDiagnosticsStatus
AnimationDiagnosticsWindow::RefreshPublishedSnapshots(bool rendererAvailable) noexcept {
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::Editor::Animation;
	try {
		if(!detailedRequest_.IsActive()) {
			detailedRequest_ = AcquireAnimationDetailedDiagnosticsRequest();
			if(!detailedRequest_.IsActive()) return AnimationDiagnosticsStatus::InternalError;
		}

		AnimationDiagnosticsOptions options;
		options.enabled = true;
		options.collapsed = false;
		options.rendererAvailable = rendererAvailable;
		options.enabledCategories = kAllAnimationDiagnosticsCategories;
		options.maximumRows = kMaximumAnimationDiagnosticsEditorRows;
		model_.Configure(options);

		const std::optional<AnimationDiagnosticsSnapshot> summary =
			AcquireAnimationDiagnosticsSnapshot();
		if(!summary) return AnimationDiagnosticsStatus::InvalidArgument;

		CopiedRuntimeDiagnostics runtime;
		runtime.integration = AnimationSystem::Instance().ReadProductionIntegrationSnapshot();
		runtime.detailed = AcquireAnimationDetailedDiagnosticsSnapshot();
		AnimationSystem::Instance().CopyPublishedAnimatorPoses(runtime.poses);
		const std::uint64_t sourceGeneration = std::max<std::uint64_t>(1U,
			std::max(summary->frameIndex,
				AnimationSystem::Instance().AuthorityPublicationGeneration()));

		AnimationDiagnosticsPreparedSnapshot prepared;
		const AnimationDiagnosticsStatus preparedStatus = model_.Prepare(
			*summary,
			sourceGeneration,
			[summary = *summary, runtime = std::move(runtime)](
				std::uint32_t enabledCategories,
				std::size_t requestedMaximumRows,
				std::vector<AnimationDiagnosticsRow>& output) {
				return CollectRuntimeRows(summary, runtime, enabledCategories,
					requestedMaximumRows, output);
			},
			prepared);
		if(preparedStatus != AnimationDiagnosticsStatus::Ok
			&& preparedStatus != AnimationDiagnosticsStatus::RendererUnavailable) {
			return preparedStatus;
		}
		return model_.Publish(prepared);
	} catch(...) {
		return AnimationDiagnosticsStatus::InternalError;
	}
}

void AnimationDiagnosticsWindow::OnGui() {
	const bool rendererAvailable = RenderSystem::Instance().GetContext() != nullptr;
	const AnimationDiagnosticsStatus refreshStatus = RefreshPublishedSnapshots(rendererAvailable);
	const AnimationDiagnosticsWindowSnapshot snapshot = model_.Snapshot();
	if(refreshStatus == AnimationDiagnosticsStatus::InvalidArgument
		&& !snapshot.summaryAvailable) {
		ImGui::TextDisabled("Waiting for an immutable animation diagnostics snapshot.");
		return;
	}
	ImGui::TextUnformatted(snapshot.accessibilityText.c_str());
	ImGui::Text("Schema %u | source %llu | publication %llu",
		snapshot.schemaVersion,
		static_cast<unsigned long long>(snapshot.sourceGeneration),
		static_cast<unsigned long long>(snapshot.publicationGeneration));

	if(snapshot.status == AnimationDiagnosticsStatus::Disabled
		|| snapshot.status == AnimationDiagnosticsStatus::Collapsed) {
		ImGui::TextDisabled("Detailed collection requests: 0");
		return;
	}
	if(snapshot.status == AnimationDiagnosticsStatus::RendererUnavailable) {
		ImGui::TextDisabled(
			"Renderer unavailable; renderer-dependent rows are suppressed while CPU authority remains inspectable.");
	}
	if(!snapshot.summaryAvailable) {
		ImGui::TextDisabled("Waiting for an immutable animation diagnostics snapshot.");
		return;
	}

	ImGui::Text("Frame %llu | tick %lld | active %llu | evaluated %llu",
		static_cast<unsigned long long>(snapshot.summary.frameIndex),
		static_cast<long long>(snapshot.summary.fixedTick),
		static_cast<unsigned long long>(snapshot.summary.counters.activeAnimators),
		static_cast<unsigned long long>(snapshot.summary.counters.evaluatedAnimators));
	ImGui::Text("Modes: GPU %llu | Hybrid %llu | CPU fallback %llu | fallbacks %llu",
		static_cast<unsigned long long>(snapshot.summary.counters.gpuVisualAnimators),
		static_cast<unsigned long long>(snapshot.summary.counters.hybridAuthoritativeAnimators),
		static_cast<unsigned long long>(snapshot.summary.counters.cpuFallbackAnimators),
		static_cast<unsigned long long>(snapshot.summary.counters.fallbackCount));
	ImGui::Text("Residency: hits %llu | misses %llu | requests %llu",
		static_cast<unsigned long long>(snapshot.summary.counters.residencyHits),
		static_cast<unsigned long long>(snapshot.summary.counters.residencyMisses),
		static_cast<unsigned long long>(snapshot.summary.counters.residencyRequests));

	if(ImGui::BeginTable(
		"AnimationDiagnosticsRows",
		3,
		ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Category");
		ImGui::TableSetupColumn("Diagnostic");
		ImGui::TableSetupColumn("Value");
		ImGui::TableHeadersRow();
		for(const AnimationDiagnosticsRow& row : snapshot.rows) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(TerraEngine::Editor::Animation::ToString(row.category).data());
			ImGui::TableSetColumnIndex(1);
			ImGui::TextWrapped("%s", row.label.c_str());
			if(ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", row.accessibilityText.c_str());
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::TextWrapped("%s", row.value.c_str());
		}
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Collection Contract");
	ImGui::Text("Rows copied %llu | dropped %llu | renderer rows suppressed %llu",
		static_cast<unsigned long long>(snapshot.instrumentation.rowsCopied),
		static_cast<unsigned long long>(snapshot.instrumentation.rowsDropped),
		static_cast<unsigned long long>(snapshot.instrumentation.rendererRowsSuppressed));
	ImGui::TextWrapped("Phase: %s | Reads: %s | Writes: %s | Boundary: %s",
		snapshot.work.phase.data(),
		snapshot.work.reads.data(),
		snapshot.work.writes.data(),
		snapshot.work.protectedBoundary.data());
}
