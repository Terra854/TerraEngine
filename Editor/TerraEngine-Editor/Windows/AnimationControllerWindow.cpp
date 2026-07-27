// AnimationControllerWindow.cpp - detached graph editing with checked publication.

#include "AnimationControllerWindow.h"

#include "AnimationAssetWindowSupport.h"
#include "../Animation/AnimationControllerAuthoringService.h"
#include "../Assets/AssetDatabase.h"
#include "Engine/ThreadPool/ThreadPool.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace {
	using namespace TerraEngine::Animation;
	using TerraEngine::Editor::Animation::AnimationAuthoringStatus;
	using TerraEngine::Editor::Animation::AnimationControllerCompileProduct;

	const char* NodeKindName(AnimationControllerNodeKind kind) {
		switch(kind) {
		case AnimationControllerNodeKind::Clip: return "Clip";
		case AnimationControllerNodeKind::BlendTree1D: return "Blend Tree 1D";
		case AnimationControllerNodeKind::BlendTree2D: return "Blend Tree 2D";
		case AnimationControllerNodeKind::Subgraph: return "Subgraph";
		case AnimationControllerNodeKind::MotionMatch: return "Motion Match";
		case AnimationControllerNodeKind::ProceduralPose: return "Procedural Pose";
		}
		return "Unknown";
	}

	const char* ParameterTypeName(AnimationControllerParameterType type) {
		switch(type) {
		case AnimationControllerParameterType::Float: return "Float";
		case AnimationControllerParameterType::Integer: return "Integer";
		case AnimationControllerParameterType::Boolean: return "Boolean";
		case AnimationControllerParameterType::Trigger: return "Trigger";
		case AnimationControllerParameterType::Vector: return "Vector";
		case AnimationControllerParameterType::Transform: return "Transform";
		case AnimationControllerParameterType::Tag: return "Tag";
		}
		return "Unknown";
	}

	AnimationControllerParameterValue DefaultParameterValue(AnimationControllerParameterType type) {
		switch(type) {
		case AnimationControllerParameterType::Float: return 0.0F;
		case AnimationControllerParameterType::Integer: return std::int32_t{0};
		case AnimationControllerParameterType::Boolean: return false;
		case AnimationControllerParameterType::Trigger: return std::monostate{};
		case AnimationControllerParameterType::Vector: return AnimationFloat3{};
		case AnimationControllerParameterType::Transform: return AnimationTransform{};
		case AnimationControllerParameterType::Tag: return std::string{"Default"};
		}
		return 0.0F;
	}

	bool EditString(const char* label, std::string& value) {
		std::array<char, 256U> buffer{};
		const std::size_t copied = std::min(value.size(), buffer.size() - 1U);
		std::memcpy(buffer.data(), value.data(), copied);
		if(!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
		value = buffer.data();
		return true;
	}

	bool EditGuid(const char* label, AnimationGuid& value) {
		std::array<char, 64U> buffer{};
		const std::string text = TerraEngine::ToHexString(value.value);
		std::memcpy(buffer.data(), text.data(), std::min(text.size(), buffer.size() - 1U));
		if(!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
		const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(buffer.data());
		if(!parsed) return false;
		value = AnimationGuid{*parsed};
		return true;
	}

	template<typename Record>
	Record* FindById(std::vector<Record>& records, std::uint64_t stableId) {
		const auto found = std::find_if(records.begin(), records.end(), [stableId](const Record& record) {
			return record.stableId == stableId;
		});
		return found == records.end() ? nullptr : &*found;
	}

	struct AsyncCompileState {
		std::mutex mutex;
		std::optional<AnimationControllerCompileProduct> completed;
		std::atomic_bool running{false};
	};
}

struct AnimationControllerWindow::State {
	TerraEngine::Editor::Animation::CookedAnimationAssetSelection<AnimationControllerAsset> selection;
	TerraEngine::Editor::Animation::AnimationControllerAuthoringService authoring;
	std::shared_ptr<AsyncCompileState> compile = std::make_shared<AsyncCompileState>();
	std::vector<std::string> injectedNodes;
	std::uint64_t selectedNode = 0U;
	std::uint64_t selectedState = 0U;
	std::uint64_t selectedTransition = 0U;
	std::uint64_t selectedParameter = 0U;
	std::uint64_t selectedGraphVariable = 0U;
	std::uint64_t selectedBinding = 0U;
	std::uint64_t selectedLayer = 0U;
	std::uint64_t selectedBlendSample = 0U;
	std::uint64_t selectedSyncGroup = 0U;
	std::uint64_t selectedSyncMarker = 0U;
	std::uint64_t selectedSubgraphDependency = 0U;
	std::uint64_t observedGeneration = 0U;
};

AnimationControllerWindow::AnimationControllerWindow(MainEditorSubsystem* owner)
	: EditorWindow("Animation Controller", false, "Animation/Authoring"),
	  owner_(owner),
	  state_(std::make_unique<State>()) {}

AnimationControllerWindow::~AnimationControllerWindow() {
	if(state_ && state_->authoring.HasCandidate()) (void)state_->authoring.Cancel();
}

void AnimationControllerWindow::SetGraphNodes(std::vector<std::string> nodes) {
	state_->injectedNodes = std::move(nodes);
}

void AnimationControllerWindow::OnGui() {
	using namespace TerraEngine::Editor::Animation;
	state_->selection.Synchronize(owner_, ".teanimctrl", &DeserializeAnimationControllerAsset);
	const AnimationControllerAsset* selected = state_->selection.Get();
	if(!state_->selection.IsSelectedForWindow()) {
		ImGui::TextWrapped("Select a .teanimctrl asset to edit parameters, nodes, states, transitions, and layers.");
		for(const std::string& node : state_->injectedNodes) ImGui::BulletText("%s", node.c_str());
		return;
	}
	if(selected == nullptr) {
		ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s", state_->selection.Message().c_str());
		return;
	}

	// A changed external selection is opened only while no private edit or worker
	// packet exists. This prevents a file watcher notification from replacing an
	// artist's detached candidate before they explicitly save or cancel it.
	const bool compileRunning = state_->compile->running.load(std::memory_order_acquire);
	const auto* published = state_->authoring.Published();
	if(!state_->authoring.HasCandidate() && !compileRunning
		&& (published == nullptr || state_->authoring.Path() != state_->selection.Path()
			|| *published != *selected)) {
		state_->observedGeneration = std::max(
			state_->observedGeneration, state_->authoring.Generation()) + 1U;
		(void)state_->authoring.Open(state_->selection.Path(), state_->observedGeneration);
	}

	// Worker completion is consumed only here, on the editor/UI boundary. The
	// worker compiled copied bytes and never touched the open document or file.
	std::optional<AnimationControllerCompileProduct> completed;
	{
		std::scoped_lock lock(state_->compile->mutex);
		if(state_->compile->completed) {
			completed = std::move(state_->compile->completed);
			state_->compile->completed.reset();
		}
	}
	if(completed) {
		const AnimationAuthoringStatus status = state_->authoring.CompleteCommit(*completed);
		if(status == AnimationAuthoringStatus::Ok) {
			// Publication happens only after worker validation. Reimport is requested at
			// this protected UI boundary so the runtime either adopts the complete cooked
			// generation or keeps its previous last-known-good resource.
			AssetDatabase::Instance().ImportAssetWithProgress(
				state_->authoring.Path(), true);
		}
		state_->compile->running.store(false, std::memory_order_release);
	}

	const AnimationControllerAsset* controller = state_->authoring.Candidate();
	if(controller == nullptr) controller = state_->authoring.Published();
	if(controller == nullptr) {
		ImGui::TextWrapped("%s", state_->authoring.Diagnostic().c_str());
		return;
	}

	ImGui::TextWrapped("%s", state_->selection.Path().string().c_str());
	ImGui::Text("Asset: %s", TerraEngine::ToHexString(controller->assetId.value).c_str());
	ImGui::Text("Generation: %llu", static_cast<unsigned long long>(state_->authoring.Generation()));
	ImGui::TextWrapped("%s", state_->authoring.Diagnostic().c_str());

	const bool editing = state_->authoring.HasCandidate();
	if(!editing) {
		if(ImGui::Button("Edit Graph")) {
			(void)state_->authoring.BeginEdit(state_->authoring.AssetId(), state_->authoring.Generation());
		}
		ImGui::SameLine();
		if(ImGui::Button("Undo")) (void)state_->authoring.Undo();
		ImGui::SameLine();
		if(ImGui::Button("Redo")) (void)state_->authoring.Redo();
	} else {
		if(!compileRunning && ImGui::Button("Validate & Save")) {
			AnimationAuthoringCompileRequest request;
			if(state_->authoring.PrepareCommit(request) == AnimationAuthoringStatus::Ok) {
				state_->compile->running.store(true, std::memory_order_release);
				const std::shared_ptr<AsyncCompileState> shared = state_->compile;
				auto work = [shared, request]() {
					AnimationControllerCompileProduct product =
						AnimationControllerAuthoringService::CompileCandidate(request);
					std::scoped_lock lock(shared->mutex);
					shared->completed = std::move(product);
				};
				if(ThreadPool::instance().getThreadCount() == 0U) {
					// Headless/data-only editor configurations have no worker pool. The copied
					// packet still preserves the same worker/editor publication separation.
					work();
				} else {
					ThreadPool::instance().enqueue(ThreadPool::instance().createTask(std::move(work)), 1U);
				}
			}
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel Edit")) (void)state_->authoring.Cancel();
	}
	if(compileRunning) ImGui::TextDisabled("Validating and compiling a copied controller candidate...");

	AnimationControllerAsset* editable = state_->authoring.MutableCandidate();
	if(compileRunning) ImGui::BeginDisabled();
	if(!ImGui::BeginTabBar("AnimationControllerAuthoringTabs")) {
		if(compileRunning) ImGui::EndDisabled();
		return;
	}

	if(ImGui::BeginTabItem("Document")) {
		ImGui::TextWrapped("External dependency identities are editable as GUID/generation/fingerprint triples. Validation rejects unresolved or incompatible combinations before save.");
		if(editable) {
			(void)EditGuid("Skeleton asset", editable->skeletonAssetId);
			ImGui::InputScalar("Compatibility fingerprint", ImGuiDataType_U64,
				&editable->compatibilityFingerprint);
		} else {
			ImGui::Text("Skeleton: %s", TerraEngine::ToHexString(controller->skeletonAssetId.value).c_str());
			ImGui::Text("Compatibility: %llu",
				static_cast<unsigned long long>(controller->compatibilityFingerprint));
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Parameters")) {
		if(editable && ImGui::Button("Add Parameter")) ImGui::OpenPopup("AddParameterType");
		if(editable && ImGui::BeginPopup("AddParameterType")) {
			for(int type = static_cast<int>(AnimationControllerParameterType::Float);
				type <= static_cast<int>(AnimationControllerParameterType::Tag); ++type) {
				const auto typed = static_cast<AnimationControllerParameterType>(type);
				if(ImGui::MenuItem(ParameterTypeName(typed))) {
					(void)state_->authoring.AddParameter({}, typed, state_->selectedParameter);
				}
			}
			ImGui::EndPopup();
		}
		for(const AnimationControllerParameter& parameter : controller->parameters) {
			const std::string label = parameter.name + "##parameter" + std::to_string(parameter.stableId);
			if(ImGui::Selectable(label.c_str(), state_->selectedParameter == parameter.stableId)) {
				state_->selectedParameter = parameter.stableId;
			}
		}
		if(editable) {
			if(auto* parameter = FindById(editable->parameters, state_->selectedParameter)) {
				ImGui::SeparatorText("Selected Parameter");
				(void)EditString("Name", parameter->name);
				int type = static_cast<int>(parameter->type);
				if(ImGui::Combo("Type", &type, "Float\0Integer\0Boolean\0Trigger\0Vector\0Transform\0Tag\0")) {
					parameter->type = static_cast<AnimationControllerParameterType>(type);
					parameter->defaultValue = DefaultParameterValue(parameter->type);
				}
				if(auto* value = std::get_if<float>(&parameter->defaultValue)) ImGui::InputFloat("Default", value);
				if(auto* value = std::get_if<std::int32_t>(&parameter->defaultValue)) ImGui::InputInt("Default", value);
				if(auto* value = std::get_if<bool>(&parameter->defaultValue)) ImGui::Checkbox("Default", value);
				if(auto* value = std::get_if<std::string>(&parameter->defaultValue)) (void)EditString("Default", *value);
				if(auto* value = std::get_if<AnimationFloat3>(&parameter->defaultValue)) ImGui::InputFloat3("Default", &value->x);
				if(auto* value = std::get_if<AnimationTransform>(&parameter->defaultValue)) {
					ImGui::InputFloat3("Default translation", &value->translation.x);
					ImGui::InputFloat4("Default rotation", &value->rotation.x);
					ImGui::InputFloat3("Default scale", &value->scale.x);
				}
				if(ImGui::Button("Remove Parameter")) (void)state_->authoring.RemoveParameter(parameter->stableId);
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Variables / Bindings")) {
		if(editable && ImGui::Button("Add Float Variable")) {
			const std::uint64_t id = state_->authoring.NextStableId();
			if(id != 0U) {
				editable->graphVariables.push_back({id, "Variable " + std::to_string(id),
					AnimationControllerParameterType::Float, 0.0F});
				state_->selectedGraphVariable = id;
			}
		}
		for(const auto& variable : controller->graphVariables) {
			const std::string label = variable.name + "##variable" + std::to_string(variable.stableId);
			if(ImGui::Selectable(label.c_str(), state_->selectedGraphVariable == variable.stableId)) state_->selectedGraphVariable = variable.stableId;
		}
		if(editable) {
			if(auto* variable = FindById(editable->graphVariables, state_->selectedGraphVariable)) {
				(void)EditString("Variable name", variable->name);
				int type = static_cast<int>(variable->type);
				if(ImGui::Combo("Variable type", &type, "Float\0Integer\0Boolean\0Trigger\0Vector\0Transform\0Tag\0")) {
					variable->type = static_cast<AnimationControllerParameterType>(type);
					variable->defaultValue = DefaultParameterValue(variable->type);
				}
				if(auto* value = std::get_if<float>(&variable->defaultValue)) ImGui::InputFloat("Variable default", value);
				if(auto* value = std::get_if<std::int32_t>(&variable->defaultValue)) ImGui::InputInt("Variable default", value);
				if(auto* value = std::get_if<bool>(&variable->defaultValue)) ImGui::Checkbox("Variable default", value);
				if(auto* value = std::get_if<std::string>(&variable->defaultValue)) (void)EditString("Variable default", *value);
				if(auto* value = std::get_if<AnimationFloat3>(&variable->defaultValue)) ImGui::InputFloat3("Variable default", &value->x);
				if(auto* value = std::get_if<AnimationTransform>(&variable->defaultValue)) {
					ImGui::InputFloat3("Variable translation", &value->translation.x);
					ImGui::InputFloat4("Variable rotation", &value->rotation.x);
					ImGui::InputFloat3("Variable scale", &value->scale.x);
				}
				if(ImGui::Button("Remove Variable")) {
					const std::uint64_t id = variable->stableId;
					editable->graphVariables.erase(std::remove_if(editable->graphVariables.begin(), editable->graphVariables.end(),
						[id](const auto& value) { return value.stableId == id; }), editable->graphVariables.end());
					state_->selectedGraphVariable = 0U;
				}
			}
			if(!editable->parameters.empty() && !editable->graphVariables.empty()
				&& ImGui::Button("Add Reusable Binding")) {
				const std::uint64_t id = state_->authoring.NextStableId();
				if(id != 0U) {
					editable->reusableBindings.push_back({id, editable->parameters.front().stableId,
						editable->graphVariables.front().stableId});
					state_->selectedBinding = id;
				}
			}
		}
		for(const auto& binding : controller->reusableBindings) {
			const std::string label = "Binding #" + std::to_string(binding.stableId) + ": "
				+ std::to_string(binding.sourceParameterId) + " -> "
				+ std::to_string(binding.destinationVariableId);
			if(ImGui::Selectable(label.c_str(), state_->selectedBinding == binding.stableId)) state_->selectedBinding = binding.stableId;
		}
		if(editable) {
			if(auto* binding = FindById(editable->reusableBindings, state_->selectedBinding)) {
				ImGui::InputScalar("Source parameter", ImGuiDataType_U64, &binding->sourceParameterId);
				ImGui::InputScalar("Destination variable", ImGuiDataType_U64, &binding->destinationVariableId);
				if(ImGui::Button("Remove Reusable Binding")) {
					const std::uint64_t id = binding->stableId;
					editable->reusableBindings.erase(std::remove_if(editable->reusableBindings.begin(), editable->reusableBindings.end(),
						[id](const auto& value) { return value.stableId == id; }), editable->reusableBindings.end());
					state_->selectedBinding = 0U;
				}
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Nodes / States")) {
		if(editable) {
			if(ImGui::Button("Add Clip")) (void)state_->authoring.AddNode(AnimationControllerNodeKind::Clip, state_->selectedNode);
			ImGui::SameLine();
			if(ImGui::Button("Add Blend 1D")) (void)state_->authoring.AddNode(AnimationControllerNodeKind::BlendTree1D, state_->selectedNode);
			ImGui::SameLine();
			if(ImGui::Button("Add Blend 2D")) (void)state_->authoring.AddNode(AnimationControllerNodeKind::BlendTree2D, state_->selectedNode);
			ImGui::SameLine();
			if(ImGui::Button("Add Procedural Pose")) (void)state_->authoring.AddNode(AnimationControllerNodeKind::ProceduralPose, state_->selectedNode);
			ImGui::SameLine();
			if(ImGui::Button("Add Subgraph")) (void)state_->authoring.AddNode(AnimationControllerNodeKind::Subgraph, state_->selectedNode);
			ImGui::SameLine();
			if(ImGui::Button("Add Motion Match")) (void)state_->authoring.AddNode(AnimationControllerNodeKind::MotionMatch, state_->selectedNode);
		}
		if(ImGui::BeginTable("NodeStateColumns", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
			ImGui::TableSetupColumn("Nodes");
			ImGui::TableSetupColumn("States");
			ImGui::TableHeadersRow();
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			for(const auto& node : controller->nodes) {
				const std::string label = std::string(NodeKindName(node.kind)) + " #" + std::to_string(node.stableId);
				if(ImGui::Selectable(label.c_str(), state_->selectedNode == node.stableId)) state_->selectedNode = node.stableId;
			}
			ImGui::TableNextColumn();
			for(const auto& graphState : controller->states) {
				const std::string label = "State #" + std::to_string(graphState.stableId) + " -> node #" + std::to_string(graphState.nodeId);
				if(ImGui::Selectable(label.c_str(), state_->selectedState == graphState.stableId)) state_->selectedState = graphState.stableId;
			}
			ImGui::EndTable();
		}
		if(editable) {
			if(auto* node = FindById(editable->nodes, state_->selectedNode)) {
				ImGui::SeparatorText("Selected Node");
				ImGui::Text("Kind: %s", NodeKindName(node->kind));
				ImGui::InputScalar("Flags", ImGuiDataType_U32, &node->flags);
				ImGui::InputScalar("Output fingerprint", ImGuiDataType_U64, &node->outputCompatibilityFingerprint);
				if(node->kind == AnimationControllerNodeKind::Clip
					|| node->kind == AnimationControllerNodeKind::Subgraph
					|| node->kind == AnimationControllerNodeKind::MotionMatch) {
					(void)EditGuid("Dependency asset", node->dependencyAssetId);
					ImGui::InputScalar("Dependency generation", ImGuiDataType_U32, &node->dependencyGeneration);
					ImGui::InputScalar("Dependency fingerprint", ImGuiDataType_U64, &node->dependencyFingerprint);
				}
				int policy = static_cast<int>(node->outOfDomainPolicy);
				if(ImGui::Combo("Out-of-domain", &policy, "Clamp\0Extrapolate\0")) node->outOfDomainPolicy = static_cast<AnimationControllerBlendOutOfDomainPolicy>(policy);
				if(node->kind == AnimationControllerNodeKind::BlendTree1D
					|| node->kind == AnimationControllerNodeKind::BlendTree2D) {
					if(ImGui::Button("Add Blend Sample")) {
						const std::uint64_t id = state_->authoring.NextStableId();
						if(id != 0U) {
							node->blendSamples.push_back({id});
							state_->selectedBlendSample = id;
						}
					}
					for(const auto& sample : node->blendSamples) {
						const std::string sampleLabel = "Sample #" + std::to_string(sample.stableId)
							+ " -> node #" + std::to_string(sample.childNodeId);
						if(ImGui::Selectable(sampleLabel.c_str(), state_->selectedBlendSample == sample.stableId)) {
							state_->selectedBlendSample = sample.stableId;
						}
					}
					if(auto* sample = FindById(node->blendSamples, state_->selectedBlendSample)) {
						ImGui::InputScalar("Sample child node", ImGuiDataType_U64, &sample->childNodeId);
						ImGui::InputFloat("Sample X", &sample->positionX);
						ImGui::InputFloat("Sample Y", &sample->positionY);
						if(ImGui::Button("Remove Blend Sample")) {
							node->blendSamples.erase(std::remove_if(node->blendSamples.begin(), node->blendSamples.end(),
								[id = sample->stableId](const auto& value) { return value.stableId == id; }), node->blendSamples.end());
							state_->selectedBlendSample = 0U;
						}
					}
				}
				if(ImGui::Button("Add State For Node")) (void)state_->authoring.AddState(node->stableId, editable->states.empty(), state_->selectedState);
				ImGui::SameLine();
				if(ImGui::Button("Remove Node")) (void)state_->authoring.RemoveNode(node->stableId);
			}
			if(auto* graphState = FindById(editable->states, state_->selectedState)) {
				ImGui::SeparatorText("Selected State");
				ImGui::InputScalar("Node ID", ImGuiDataType_U64, &graphState->nodeId);
				ImGui::InputScalar("Parent state ID", ImGuiDataType_U64, &graphState->parentStateId);
				ImGui::Checkbox("Entry", &graphState->entry);
				ImGui::SameLine(); ImGui::Checkbox("Exit", &graphState->exit);
				ImGui::SameLine(); ImGui::Checkbox("Any State", &graphState->anyState);
				if(ImGui::Button("Remove State")) (void)state_->authoring.RemoveState(graphState->stableId);
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Transitions")) {
		if(editable && editable->states.size() >= 2U && ImGui::Button("Add Transition")) {
			(void)state_->authoring.AddTransition(editable->states[0].stableId,
				editable->states[1].stableId, state_->selectedTransition);
		}
		for(const auto& transition : controller->transitions) {
			const std::string label = "#" + std::to_string(transition.stableId) + "  "
				+ std::to_string(transition.sourceStateId) + " -> " + std::to_string(transition.destinationStateId);
			if(ImGui::Selectable(label.c_str(), state_->selectedTransition == transition.stableId)) state_->selectedTransition = transition.stableId;
		}
		if(editable) {
			if(auto* transition = FindById(editable->transitions, state_->selectedTransition)) {
				ImGui::SeparatorText("Selected Transition");
				ImGui::InputScalar("Source state", ImGuiDataType_U64, &transition->sourceStateId);
				ImGui::InputScalar("Destination state", ImGuiDataType_U64, &transition->destinationStateId);
				ImGui::InputScalar("Condition parameter", ImGuiDataType_U64, &transition->conditionParameterId);
				ImGui::InputScalar("Duration ticks", ImGuiDataType_S64, &transition->durationTicks);
				ImGui::InputScalar("Exit time ticks", ImGuiDataType_S64, &transition->exitTimeTicks);
				ImGui::SliderFloat("Destination offset", &transition->destinationOffsetNormalized, 0.0F, 1.0F);
				ImGui::InputScalar("Priority", ImGuiDataType_U32, &transition->priority);
				int interruptionSource = static_cast<int>(transition->interruptionSource);
				if(ImGui::Combo("Interruption source", &interruptionSource, "None\0Current\0Next\0")) transition->interruptionSource = static_cast<AnimationControllerInterruptionSource>(interruptionSource);
				int interruptionPolicy = static_cast<int>(transition->interruptionPolicy);
				if(ImGui::Combo("Interruption policy", &interruptionPolicy, "Reject\0Replace\0")) transition->interruptionPolicy = static_cast<AnimationControllerInterruptionPolicy>(interruptionPolicy);
				int consumption = static_cast<int>(transition->triggerConsumption);
				if(ImGui::Combo("Trigger consumption", &consumption, "Single consumer\0Multi consumer\0")) transition->triggerConsumption = static_cast<AnimationControllerTriggerConsumption>(consumption);
				int reset = static_cast<int>(transition->triggerReset);
				if(ImGui::Combo("Trigger reset", &reset, "End step\0Manual\0")) transition->triggerReset = static_cast<AnimationControllerTriggerReset>(reset);
				int failure = static_cast<int>(transition->triggerFailure);
				if(ImGui::Combo("Trigger failure", &failure, "Preserve\0Reset\0")) transition->triggerFailure = static_cast<AnimationControllerTriggerFailure>(failure);
				if(ImGui::Button("Remove Transition")) (void)state_->authoring.RemoveTransition(transition->stableId);
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Layers")) {
		if(editable && !editable->states.empty() && ImGui::Button("Add Layer")) {
			(void)state_->authoring.AddLayer(editable->states.front().stableId, state_->selectedLayer);
		}
		for(const auto& layer : controller->layers) {
			const std::string label = "Layer #" + std::to_string(layer.stableId) + " -> state #" + std::to_string(layer.rootStateId);
			if(ImGui::Selectable(label.c_str(), state_->selectedLayer == layer.stableId)) state_->selectedLayer = layer.stableId;
		}
		if(editable) {
			if(auto* layer = FindById(editable->layers, state_->selectedLayer)) {
				ImGui::SeparatorText("Selected Layer");
				ImGui::InputScalar("Root state", ImGuiDataType_U64, &layer->rootStateId);
				ImGui::SliderFloat("Default weight", &layer->defaultWeight, 0.0F, 1.0F);
				ImGui::Checkbox("Additive", &layer->additive);
				(void)EditGuid("Mask asset", layer->maskAssetId);
				ImGui::InputScalar("Mask generation", ImGuiDataType_U32, &layer->maskGeneration);
				ImGui::InputScalar("Mask fingerprint", ImGuiDataType_U64, &layer->maskFingerprint);
				if(ImGui::Button("Remove Layer")) (void)state_->authoring.RemoveLayer(layer->stableId);
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Sync Groups")) {
		if(editable && ImGui::Button("Add Sync Group")) {
			const std::uint64_t id = state_->authoring.NextStableId();
			if(id != 0U) {
				editable->syncGroups.push_back({id, "Sync Group " + std::to_string(id)});
				state_->selectedSyncGroup = id;
			}
		}
		for(const auto& group : controller->syncGroups) {
			const std::string label = group.name + "##sync" + std::to_string(group.stableId);
			if(ImGui::Selectable(label.c_str(), state_->selectedSyncGroup == group.stableId)) state_->selectedSyncGroup = group.stableId;
		}
		if(editable) {
			if(auto* group = FindById(editable->syncGroups, state_->selectedSyncGroup)) {
				(void)EditString("Group name", group->name);
				ImGui::InputScalar("Leader state", ImGuiDataType_U64, &group->leaderStateId);
				if(ImGui::Button("Add Marker")) {
					const std::uint64_t id = state_->authoring.NextStableId();
					if(id != 0U) {
						group->markers.push_back({id, "Marker " + std::to_string(id)});
						state_->selectedSyncMarker = id;
					}
				}
				for(const auto& marker : group->markers) {
					const std::string label = marker.name + "##marker" + std::to_string(marker.stableId);
					if(ImGui::Selectable(label.c_str(), state_->selectedSyncMarker == marker.stableId)) state_->selectedSyncMarker = marker.stableId;
				}
				if(auto* marker = FindById(group->markers, state_->selectedSyncMarker)) {
					(void)EditString("Marker name", marker->name);
					ImGui::InputScalar("Clip event ID", ImGuiDataType_U64, &marker->clipEventId);
					ImGui::InputScalar("Marker state ID", ImGuiDataType_U64, &marker->stateId);
					if(ImGui::Button("Remove Marker")) {
						group->markers.erase(std::remove_if(group->markers.begin(), group->markers.end(),
							[id = marker->stableId](const auto& value) { return value.stableId == id; }), group->markers.end());
						state_->selectedSyncMarker = 0U;
					}
				}
				if(ImGui::Button("Remove Sync Group")) {
					const std::uint64_t id = group->stableId;
					editable->syncGroups.erase(std::remove_if(editable->syncGroups.begin(), editable->syncGroups.end(),
						[id](const auto& value) { return value.stableId == id; }), editable->syncGroups.end());
					state_->selectedSyncGroup = 0U;
				}
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Subgraphs")) {
		if(editable && ImGui::Button("Add Subgraph Dependency")) {
			const std::uint64_t id = state_->authoring.NextStableId();
			if(id != 0U) {
				editable->subgraphDependencies.push_back({id});
				state_->selectedSubgraphDependency = id;
			}
		}
		for(const auto& dependency : controller->subgraphDependencies) {
			const std::string label = "Dependency #" + std::to_string(dependency.stableId);
			if(ImGui::Selectable(label.c_str(), state_->selectedSubgraphDependency == dependency.stableId)) state_->selectedSubgraphDependency = dependency.stableId;
		}
		if(editable) {
			if(auto* dependency = FindById(editable->subgraphDependencies, state_->selectedSubgraphDependency)) {
				(void)EditGuid("Controller asset", dependency->controllerAssetId);
				ImGui::InputScalar("Controller generation", ImGuiDataType_U32, &dependency->generation);
				ImGui::InputScalar("Controller fingerprint", ImGuiDataType_U64, &dependency->compatibilityFingerprint);
				if(ImGui::Button("Remove Subgraph Dependency")) {
					const std::uint64_t id = dependency->stableId;
					editable->subgraphDependencies.erase(std::remove_if(editable->subgraphDependencies.begin(), editable->subgraphDependencies.end(),
						[id](const auto& value) { return value.stableId == id; }), editable->subgraphDependencies.end());
					state_->selectedSubgraphDependency = 0U;
				}
			}
		}
		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
	if(compileRunning) ImGui::EndDisabled();
}
