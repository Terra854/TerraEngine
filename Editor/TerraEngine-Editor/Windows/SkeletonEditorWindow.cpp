// SkeletonEditorWindow.cpp - selected cooked skeleton inspection UI.

#include "SkeletonEditorWindow.h"

#include "AnimationAssetWindowSupport.h"
#include "Engine/Animation/Assets/SkeletonAsset.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <string>
#include <utility>
#include <vector>

namespace {
	using TerraEngine::Animation::AnimationTransform;
	using TerraEngine::Animation::SkeletonAsset;

	void DrawTransform(const AnimationTransform& transform) {
		ImGui::Text("Translation  %.4f  %.4f  %.4f",
			transform.translation.x, transform.translation.y, transform.translation.z);
		ImGui::Text("Rotation     %.4f  %.4f  %.4f  %.4f",
			transform.rotation.x, transform.rotation.y,
			transform.rotation.z, transform.rotation.w);
		ImGui::Text("Scale        %.4f  %.4f  %.4f",
			transform.scale.x, transform.scale.y, transform.scale.z);
	}

	void DrawSummaryRow(const char* label, const std::string& value) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextWrapped("%s", value.c_str());
	}
}

struct SkeletonEditorWindow::State {
	TerraEngine::Editor::Animation::CookedAnimationAssetSelection<SkeletonAsset> selection;
	std::vector<std::string> injectedJoints;
	int selectedJoint = -1;
	int selectedSocket = -1;
	bool deformingOnly = false;
};

SkeletonEditorWindow::SkeletonEditorWindow(MainEditorSubsystem* owner)
	: EditorWindow("Skeleton Editor", false, "Animation/Authoring"),
	  owner_(owner),
	  state_(std::make_unique<State>()) {
}

SkeletonEditorWindow::~SkeletonEditorWindow() = default;

void SkeletonEditorWindow::SetJoints(std::vector<std::string> joints) {
	state_->injectedJoints = std::move(joints);
}

const std::vector<std::string>& SkeletonEditorWindow::Joints() const noexcept {
	return state_->injectedJoints;
}

void SkeletonEditorWindow::OnGui() {
	state_->selection.Synchronize(
		owner_, ".teskel", &TerraEngine::Animation::DeserializeSkeletonAsset);

	const SkeletonAsset* skeleton = state_->selection.Get();
	if(!state_->selection.IsSelectedForWindow()) {
		ImGui::TextWrapped("Select a .teskel asset in the Project Browser to inspect its checked hierarchy, bind pose, sockets, and compatibility contract.");
		if(!state_->injectedJoints.empty()) {
			ImGui::SeparatorText("Preview Hierarchy");
			for(const std::string& joint : state_->injectedJoints) {
				ImGui::BulletText("%s", joint.c_str());
			}
		}
		return;
	}

	ImGui::TextWrapped("%s", state_->selection.Path().string().c_str());
	if(skeleton == nullptr) {
		ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
			state_->selection.Message().c_str());
		return;
	}
	if(state_->selection.IsStale()) {
		ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.2F, 1.0F),
			"The latest file was rejected; showing the prior complete decoded copy.");
	}

	if(ImGui::BeginTable("SkeletonSummary", 2,
		ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		DrawSummaryRow("Asset", TerraEngine::ToHexString(skeleton->assetId.value));
		DrawSummaryRow("Format", std::to_string(skeleton->formatVersion));
		DrawSummaryRow("Compatibility", std::to_string(skeleton->compatibilityFingerprint));
		DrawSummaryRow("Reference Pose Hash", std::to_string(skeleton->referencePoseHash));
		DrawSummaryRow("Joints / Sockets", std::to_string(skeleton->joints.size())
			+ " / " + std::to_string(skeleton->sockets.size()));
		DrawSummaryRow("Source", skeleton->source.sourcePackagePath.empty()
			? "Cooked source path not recorded" : skeleton->source.sourcePackagePath);
		DrawSummaryRow("Unit Scale", std::to_string(skeleton->source.unitScaleMeters) + " metres");
		ImGui::EndTable();
	}

	if(!ImGui::BeginTabBar("SkeletonEditorTabs")) {
		return;
	}

	if(ImGui::BeginTabItem("Hierarchy")) {
		ImGui::Checkbox("Deforming joints only", &state_->deformingOnly);
		ImGui::BeginChild("SkeletonHierarchy", ImVec2(0.0F, 300.0F), true);
		std::vector<std::vector<std::size_t>> children(skeleton->joints.size());
		std::vector<std::size_t> roots;
		for(std::size_t index = 0U; index < skeleton->joints.size(); ++index) {
			const std::int32_t parent = skeleton->joints[index].parentIndex;
			if(parent < 0 || static_cast<std::size_t>(parent) >= skeleton->joints.size()) {
				roots.push_back(index);
			} else {
				children[static_cast<std::size_t>(parent)].push_back(index);
			}
		}

		std::function<void(std::size_t)> drawJoint = [&](std::size_t index) {
			const auto& joint = skeleton->joints[index];
			const bool hideSelf = state_->deformingOnly && !joint.deformsMesh;
			const bool hasVisibleChild = std::any_of(children[index].begin(), children[index].end(),
				[&](std::size_t child) { return !state_->deformingOnly || skeleton->joints[child].deformsMesh; });
			if(hideSelf && !hasVisibleChild) {
				return;
			}
			if(hideSelf) {
				for(const std::size_t child : children[index]) {
					drawJoint(child);
				}
				return;
			}

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
				| ImGuiTreeNodeFlags_SpanAvailWidth;
			if(children[index].empty()) {
				flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			}
			if(state_->selectedJoint == static_cast<int>(index)) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}
			const std::string label = joint.displayName.empty()
				? "Joint " + std::to_string(index) : joint.displayName;
			const bool open = ImGui::TreeNodeEx(
				reinterpret_cast<void*>(static_cast<std::uintptr_t>(index + 1U)),
				flags, "%s%s", label.c_str(), joint.deformsMesh ? "  [deforms]" : "");
			if(ImGui::IsItemClicked()) {
				state_->selectedJoint = static_cast<int>(index);
			}
			if(open && (flags & ImGuiTreeNodeFlags_NoTreePushOnOpen) == 0) {
				for(const std::size_t child : children[index]) {
					drawJoint(child);
				}
				ImGui::TreePop();
			}
		};
		for(const std::size_t root : roots) {
			drawJoint(root);
		}
		ImGui::EndChild();

		if(state_->selectedJoint >= 0
			&& static_cast<std::size_t>(state_->selectedJoint) < skeleton->joints.size()) {
			const auto& joint = skeleton->joints[static_cast<std::size_t>(state_->selectedJoint)];
			ImGui::SeparatorText("Selected Joint");
			ImGui::Text("Stable ID: %llu", static_cast<unsigned long long>(joint.stableId));
			ImGui::Text("Parent index: %d", joint.parentIndex);
			ImGui::Text("Semantic role: %s", joint.semanticRole.empty() ? "(none)" : joint.semanticRole.c_str());
			ImGui::TextWrapped("Source path: %s", joint.normalizedSourcePath.empty()
				? "(not recorded)" : joint.normalizedSourcePath.c_str());
			DrawTransform(joint.localBindTransform);
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Bind / Current Pose")) {
		ImGui::TextWrapped("Bind transforms and inverse-bind matrices are immutable cooked data. Current pose values are generation-safe runtime publications and appear in the Animator Inspector when a scene instance is selected.");
		ImGui::Text("Bind transforms: %zu", skeleton->joints.size());
		ImGui::Text("Inverse-bind matrices: %zu", skeleton->inverseBindMatrices.size());
		ImGui::Text("Root joint index: %u", skeleton->rootJointIndex);
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Sockets")) {
		if(skeleton->sockets.empty()) {
			ImGui::TextDisabled("This skeleton has no cooked sockets.");
		}
		for(std::size_t index = 0U; index < skeleton->sockets.size(); ++index) {
			const auto& socket = skeleton->sockets[index];
			ImGui::PushID(static_cast<int>(index));
			if(ImGui::Selectable(socket.displayName.empty() ? "Unnamed socket" : socket.displayName.c_str(),
				state_->selectedSocket == static_cast<int>(index))) {
				state_->selectedSocket = static_cast<int>(index);
			}
			ImGui::PopID();
		}
		if(state_->selectedSocket >= 0
			&& static_cast<std::size_t>(state_->selectedSocket) < skeleton->sockets.size()) {
			const auto& socket = skeleton->sockets[static_cast<std::size_t>(state_->selectedSocket)];
			ImGui::SeparatorText("Socket Transform");
			ImGui::Text("Stable ID: %llu", static_cast<unsigned long long>(socket.stableId));
			ImGui::Text("Joint stable ID: %llu", static_cast<unsigned long long>(socket.jointStableId));
			ImGui::Text("Semantic role: %s", socket.semanticRole.empty() ? "(none)" : socket.semanticRole.c_str());
			DrawTransform(socket.localTransform);
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Limits / Influences")) {
		const std::size_t deforming = static_cast<std::size_t>(std::count_if(
			skeleton->joints.begin(), skeleton->joints.end(),
			[](const auto& joint) { return joint.deformsMesh; }));
		ImGui::Text("Deforming joints: %zu / %zu", deforming, skeleton->joints.size());
		ImGui::Text("Inverse-bind coverage: %zu / %zu",
			skeleton->inverseBindMatrices.size(), skeleton->joints.size());
		ImGui::TextWrapped("Joint limits are profile-owned constraints, not fields in the immutable skeleton format. Select a matching controller, procedural graph, or ragdoll profile to inspect its limit records without inventing skeleton data.");
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Compatibility")) {
		ImGui::Text("Asset GUID: %s", TerraEngine::ToHexString(skeleton->assetId.value).c_str());
		ImGui::Text("Compatibility fingerprint: %llu",
			static_cast<unsigned long long>(skeleton->compatibilityFingerprint));
		ImGui::Text("Reference pose hash: %llu",
			static_cast<unsigned long long>(skeleton->referencePoseHash));
		ImGui::TextWrapped("Clips, controllers, retarget profiles, deformable meshes, and ragdolls must publish these matching identities before the runtime binds them.");
		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
}
