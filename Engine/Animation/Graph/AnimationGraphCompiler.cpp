// AnimationGraphCompiler.cpp - Deterministic validation and staged program publication.

#include "Animation/Graph/AnimationGraphCompiler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace TerraEngine::Animation {
namespace {
constexpr std::uint32_t kScratchBytesPerInstruction = 64U;
constexpr std::uint32_t kMaximumProgramScratchBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumAuthoredStringBytes = 4096U;
std::uint64_t StableNameHash(std::string_view value) noexcept { std::uint64_t hash=14695981039346656037ULL; for(const unsigned char c:value){hash^=c;hash*=1099511628211ULL;} return hash; }

static_assert(static_cast<std::uint8_t>(AnimationControllerNodeKind::Clip) == static_cast<std::uint8_t>(EvaluationOpcode::SampleClip));
static_assert(static_cast<std::uint8_t>(AnimationControllerNodeKind::BlendTree1D) ==
			  static_cast<std::uint8_t>(EvaluationOpcode::BlendTree1D));
static_assert(static_cast<std::uint8_t>(AnimationControllerNodeKind::BlendTree2D) ==
			  static_cast<std::uint8_t>(EvaluationOpcode::BlendTree2D));
static_assert(static_cast<std::uint8_t>(AnimationControllerNodeKind::Subgraph) ==
			  static_cast<std::uint8_t>(EvaluationOpcode::CallSubgraph));
static_assert(static_cast<std::uint8_t>(AnimationControllerNodeKind::MotionMatch) ==
			  static_cast<std::uint8_t>(EvaluationOpcode::MotionMatch));
static_assert(static_cast<std::uint8_t>(AnimationControllerNodeKind::ProceduralPose) ==
			  static_cast<std::uint8_t>(EvaluationOpcode::ProceduralPose));

void AddDiagnostic(std::vector<AnimationGraphDiagnostic> &diagnostics, AnimationGraphDiagnosticCode code, std::uint64_t id) {
	diagnostics.push_back({code, id});
}

bool IsExternalNode(AnimationControllerNodeKind kind) noexcept {
	return kind == AnimationControllerNodeKind::Clip || kind == AnimationControllerNodeKind::Subgraph ||
		   kind == AnimationControllerNodeKind::MotionMatch;
}

EvaluationOpcode ToOpcode(AnimationControllerNodeKind kind) noexcept {
	return static_cast<EvaluationOpcode>(static_cast<std::uint8_t>(kind));
}

bool KnownParameterType(AnimationControllerParameterType type) noexcept {
	return type <= AnimationControllerParameterType::Tag;
}

bool KnownNodeKind(AnimationControllerNodeKind kind) noexcept {
	return kind <= AnimationControllerNodeKind::ProceduralPose;
}

// Graph variables and parameters share a durable variant. Check every arm here
// so managed and native authoring fail at the same compiler publication gate.
bool HasValidDefault(const AnimationControllerParameter &value) noexcept {
	switch (value.type) {
	case AnimationControllerParameterType::Float:
		return std::holds_alternative<float>(value.defaultValue) && std::isfinite(std::get<float>(value.defaultValue));
	case AnimationControllerParameterType::Integer:
		return std::holds_alternative<std::int32_t>(value.defaultValue);
	case AnimationControllerParameterType::Boolean:
		return std::holds_alternative<bool>(value.defaultValue);
	case AnimationControllerParameterType::Trigger:
		return std::holds_alternative<std::monostate>(value.defaultValue);
	case AnimationControllerParameterType::Vector:
		return std::holds_alternative<AnimationFloat3>(value.defaultValue) && IsFinite(std::get<AnimationFloat3>(value.defaultValue));
	case AnimationControllerParameterType::Transform:
		return std::holds_alternative<AnimationTransform>(value.defaultValue) &&
			   IsFinite(std::get<AnimationTransform>(value.defaultValue)) &&
			   IsNormalized(std::get<AnimationTransform>(value.defaultValue).rotation);
	case AnimationControllerParameterType::Tag:
		return std::holds_alternative<std::string>(value.defaultValue) && !std::get<std::string>(value.defaultValue).empty() &&
			   std::get<std::string>(value.defaultValue).size() <= kMaximumAuthoredStringBytes;
	}
	return false;
}

bool CheckedAdd(std::size_t &total, std::size_t value) noexcept {
	if (total > std::numeric_limits<std::size_t>::max() - value)
		return false;
	total += value;
	return true;
}

bool ExceedsGraphHardLimits(const AnimationControllerAsset &controller) noexcept {
	if (ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, controller.nodes.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, controller.states.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, controller.transitions.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerLayers, controller.layers.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, controller.parameters.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, controller.graphVariables.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, controller.reusableBindings.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, controller.syncGroups.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, controller.subgraphDependencies.size()) != AnimationStatus::Ok)
		return true;
	std::size_t markerTotal = 0U;
	for (const AnimationControllerSyncGroup &group : controller.syncGroups) {
		if (!CheckedAdd(markerTotal, group.markers.size()) ||
			ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, group.markers.size()) != AnimationStatus::Ok)
			return true;
	}
	if (ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, markerTotal) != AnimationStatus::Ok)
		return true;
	for (const AnimationControllerState &state : controller.states) {
		const std::size_t outgoing = static_cast<std::size_t>(std::count_if(
			controller.transitions.begin(), controller.transitions.end(),
			[&state](const AnimationControllerTransition &transition) { return transition.sourceStateId == state.stableId; }));
		if (ValidateAnimationHardLimit(AnimationLimit::TransitionsPerState, outgoing) != AnimationStatus::Ok)
			return true;
	}
	return false;
}

const AnimationControllerAsset *FindController(const AnimationGuid &id, const AnimationControllerAsset &root,
											   std::span<const AnimationControllerAsset> available) noexcept {
	if (root.assetId == id)
		return &root;
	const auto iterator = std::find_if(available.begin(), available.end(),
									   [&id](const AnimationControllerAsset &candidate) { return candidate.assetId == id; });
	return iterator == available.end() ? nullptr : &*iterator;
}

bool ContainsState(const AnimationControllerAsset &controller, std::uint64_t id) noexcept {
	return std::any_of(controller.states.begin(), controller.states.end(),
					   [id](const AnimationControllerState &state) { return state.stableId == id; });
}

bool ContainsParameter(const AnimationControllerAsset &controller, std::uint64_t id) noexcept {
	return std::any_of(controller.parameters.begin(), controller.parameters.end(),
					   [id](const AnimationControllerParameter &parameter) { return parameter.stableId == id; });
}

bool CheckSubgraphTree(const AnimationControllerAsset &current, const AnimationControllerAsset &root,
					   std::span<const AnimationControllerAsset> available, std::set<AnimationGuid> &active, std::size_t depth,
					   std::vector<AnimationGraphDiagnostic> &diagnostics) {
	if (depth > GetAnimationRuntimeLimit(AnimationLimit::SubgraphCallDepth).hardMaximum) {
		AddDiagnostic(diagnostics, AnimationGraphDiagnosticCode::SubgraphDepthExceeded, 0U);
		return false;
	}
	if (!active.insert(current.assetId).second) {
		AddDiagnostic(diagnostics, AnimationGraphDiagnosticCode::SubgraphCycle, 0U);
		return false;
	}
	bool valid = true;
	for (const AnimationControllerSubgraphDependency &dependency : current.subgraphDependencies) {
		const AnimationControllerAsset *next = FindController(dependency.controllerAssetId, root, available);
		if (next == nullptr) {
			// A missing controller is an asset compatibility error, not an authored cycle.
			AddDiagnostic(diagnostics, AnimationGraphDiagnosticCode::IncompatibleAsset, dependency.stableId);
			valid = false;
			continue;
		}
		valid = CheckSubgraphTree(*next, root, available, active, depth + 1U, diagnostics) && valid;
	}
	active.erase(current.assetId);
	return valid;
}

bool HasParameterType(const AnimationControllerAsset &controller, std::uint64_t parameterId,
					  AnimationControllerParameterType expected) noexcept {
	const auto iterator =
		std::find_if(controller.parameters.begin(), controller.parameters.end(),
					 [parameterId](const AnimationControllerParameter &parameter) { return parameter.stableId == parameterId; });
	return iterator != controller.parameters.end() && iterator->type == expected;
}
} // namespace

AnimationGraphCompilationResult CompileAnimationControllerGraph(const AnimationControllerAsset &controller,
																const AnimationGraphCompilationOptions &options,
																EvaluationProgram &output) {
	AnimationGraphCompilationResult result{};
	result.status = AnimationStatus::Rejected;

	// These count checks run before graph maps/sets allocate based on authored input.
	if (ExceedsGraphHardLimits(controller)) {
		AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::HardLimitExceeded, 0U);
		result.status = AnimationStatus::HardLimitExceeded;
		return result;
	}

	if (!controller.assetId.IsValid() || !controller.skeletonAssetId.IsValid() || controller.compatibilityFingerprint == 0U) {
		AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, 0U);
		return result;
	}

	std::map<std::uint64_t, const AnimationControllerState *> states;
	for (const AnimationControllerState &state : controller.states) {
		if (state.stableId == 0U || !states.emplace(state.stableId, &state).second) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, state.stableId);
		}
	}
	std::set<std::uint64_t> nodeIds;
	for (const AnimationControllerNode &node : controller.nodes) {
		if (node.stableId == 0U || !KnownNodeKind(node.kind) || !nodeIds.insert(node.stableId).second) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, node.stableId);
			continue;
		}
		if (IsExternalNode(node.kind) && (!node.dependencyAssetId.IsValid() || node.dependencyGeneration == 0U ||
										  node.dependencyFingerprint != controller.compatibilityFingerprint)) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::IncompatibleAsset, node.stableId);
		}
		if ((node.kind == AnimationControllerNodeKind::BlendTree1D || node.kind == AnimationControllerNodeKind::BlendTree2D) &&
			node.outOfDomainPolicy > AnimationControllerBlendOutOfDomainPolicy::Extrapolate) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, node.stableId);
		}
		if (ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, node.blendSamples.size()) != AnimationStatus::Ok) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::HardLimitExceeded, node.stableId);
		}
	}
	for (const AnimationControllerParameter &parameter : controller.parameters) {
		if (parameter.stableId == 0U || parameter.name.empty() || parameter.name.size() > kMaximumAuthoredStringBytes ||
			!KnownParameterType(parameter.type) || !HasValidDefault(parameter)) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidParameter, parameter.stableId);
		}
	}
	std::set<std::uint64_t> parameterIds;
	for (const AnimationControllerParameter &parameter : controller.parameters) {
		if (parameter.stableId == 0U || !parameterIds.insert(parameter.stableId).second)
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidParameter, parameter.stableId);
	}
	std::map<std::uint64_t, const AnimationControllerGraphVariable *> variables;
	for (const AnimationControllerGraphVariable &variable : controller.graphVariables) {
		if (variable.stableId == 0U || variable.name.empty() || variable.name.size() > kMaximumAuthoredStringBytes ||
			!KnownParameterType(variable.type) || !HasValidDefault(variable) || !variables.emplace(variable.stableId, &variable).second) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidParameter, variable.stableId);
		}
	}
	std::set<std::uint64_t> bindingIds;
	for (const AnimationControllerReusableBinding &binding : controller.reusableBindings) {
		const auto parameter = std::find_if(controller.parameters.begin(), controller.parameters.end(),
											[&binding](const auto &candidate) { return candidate.stableId == binding.sourceParameterId; });
		const auto variable = variables.find(binding.destinationVariableId);
		if (binding.stableId == 0U || !bindingIds.insert(binding.stableId).second || parameter == controller.parameters.end() ||
			variable == variables.end() || parameter->type != variable->second->type) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidParameter, binding.stableId);
		}
	}
	for (const AnimationControllerState &state : controller.states) {
		if (nodeIds.find(state.nodeId) == nodeIds.end())
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, state.stableId);
	}
	for (const AnimationControllerNode &node : controller.nodes) {
		if (node.kind != AnimationControllerNodeKind::BlendTree1D && node.kind != AnimationControllerNodeKind::BlendTree2D)
			continue;
		std::set<std::uint64_t> sampleIds;
		for (std::size_t left = 0U; left < node.blendSamples.size(); ++left) {
			const AnimationControllerBlendSample &sample = node.blendSamples[left];
			if (sample.stableId == 0U || !sampleIds.insert(sample.stableId).second || sample.childNodeId == node.stableId ||
				nodeIds.find(sample.childNodeId) == nodeIds.end() || !std::isfinite(sample.positionX) || !std::isfinite(sample.positionY)) {
				AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, sample.stableId);
			}
			for (std::size_t right = left + 1U; right < node.blendSamples.size(); ++right) {
				const auto &other = node.blendSamples[right];
				if ((node.kind == AnimationControllerNodeKind::BlendTree1D && sample.positionX == other.positionX) ||
					(node.kind == AnimationControllerNodeKind::BlendTree2D && sample.positionX == other.positionX && sample.positionY == other.positionY)) {
					AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::AmbiguousTransition, std::min(sample.stableId, other.stableId));
				}
			}
		}
	}

	// Follow each parent chain independently. Re-entering a state proves a direct or indirect hierarchy cycle.
	for (const AnimationControllerState &state : controller.states) {
		std::set<std::uint64_t> chain;
		const AnimationControllerState *current = &state;
		std::size_t depth = 0U;
		while (current != nullptr && current->parentStateId != 0U) {
			if (++depth > GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum) {
				AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::StateMachineDepthExceeded, state.stableId);
				break;
			}
			if (!chain.insert(current->stableId).second) {
				AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::StateParentCycle, current->stableId);
				break;
			}
			const auto parent = states.find(current->parentStateId);
			if (parent == states.end()) {
				AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, current->stableId);
				break;
			}
			current = parent->second;
		}
	}

	std::map<std::uint64_t, std::size_t> entriesByScope;
	for (const AnimationControllerState &state : controller.states)
		if (state.entry)
			++entriesByScope[state.parentStateId];
	std::set<std::uint64_t> scopes{0U};
	for (const AnimationControllerState &state : controller.states)
		if (state.parentStateId != 0U)
			scopes.insert(state.parentStateId);
	for (const std::uint64_t scope : scopes)
		if (entriesByScope[scope] != 1U)
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::MissingDefaultEntry, scope);

	std::set<std::uint64_t> layerIds;
	for (const AnimationControllerLayer &layer : controller.layers) {
		if (layer.stableId == 0U || !layerIds.insert(layer.stableId).second || !ContainsState(controller, layer.rootStateId)) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, layer.stableId);
		}
		const bool masked = layer.maskAssetId.IsValid();
		if ((masked && (layer.maskGeneration == 0U || layer.maskFingerprint != controller.compatibilityFingerprint)) ||
			(!masked && (layer.maskGeneration != 0U || layer.maskFingerprint != 0U))) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidMask, layer.stableId);
		}
		if (!std::isfinite(layer.defaultWeight) || layer.defaultWeight < 0.0F || layer.defaultWeight > 1.0F)
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::NonFiniteField, layer.stableId);
	}

	std::map<std::uint64_t, std::vector<std::uint64_t>> outgoing;
	std::set<std::uint64_t> transitionIds;
	for (const AnimationControllerTransition &transition : controller.transitions) {
		if (transition.stableId == 0U || !transitionIds.insert(transition.stableId).second || transition.durationTicks < 0 ||
			transition.exitTimeTicks < 0 || transition.destinationOffsetNormalized < 0.0F ||
			transition.destinationOffsetNormalized > 1.0F || !ContainsState(controller, transition.sourceStateId) ||
			!ContainsState(controller, transition.destinationStateId) ||
			(transition.conditionParameterId != 0U && !HasParameterType(controller, transition.conditionParameterId,
				AnimationControllerParameterType::Trigger)) ||
			transition.interruptionSource > AnimationControllerInterruptionSource::Next ||
			transition.interruptionPolicy > AnimationControllerInterruptionPolicy::Replace ||
			transition.triggerConsumption > AnimationControllerTriggerConsumption::MultiConsumer ||
			transition.triggerReset > AnimationControllerTriggerReset::Manual || transition.triggerFailure > AnimationControllerTriggerFailure::Reset) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, transition.stableId);
		}
		if (!std::isfinite(transition.destinationOffsetNormalized))
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::NonFiniteField, transition.stableId);
		outgoing[transition.sourceStateId].push_back(transition.destinationStateId);
	}
	for (std::size_t left = 0U; left < controller.transitions.size(); ++left)
		for (std::size_t right = left + 1U; right < controller.transitions.size(); ++right) {
			const auto &a = controller.transitions[left];
			const auto &b = controller.transitions[right];
			if (a.sourceStateId == b.sourceStateId && a.conditionParameterId == b.conditionParameterId && a.priority == b.priority &&
				a.destinationStateId != b.destinationStateId)
				AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::AmbiguousTransition, std::min(a.stableId, b.stableId));
		}

	std::set<std::uint64_t> reachable;
	std::vector<std::uint64_t> pending;
	for (const AnimationControllerState &state : controller.states)
		if (state.entry)
			pending.push_back(state.stableId);
	while (!pending.empty()) {
		const std::uint64_t current = pending.back();
		pending.pop_back();
		if (!reachable.insert(current).second)
			continue;
		const auto iterator = outgoing.find(current);
		if (iterator != outgoing.end())
			pending.insert(pending.end(), iterator->second.begin(), iterator->second.end());
	}
	for (const AnimationControllerState &state : controller.states)
		if (!reachable.contains(state.stableId))
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::UnreachableState, state.stableId);

	std::set<std::uint64_t> syncGroupIds;
	for (const AnimationControllerSyncGroup &group : controller.syncGroups) {
		if (group.stableId == 0U || group.name.empty() || group.name.size() > kMaximumAuthoredStringBytes ||
			!syncGroupIds.insert(group.stableId).second || !ContainsState(controller, group.leaderStateId)) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, group.stableId);
		}
		std::set<std::uint64_t> markerIds;
		for (const AnimationControllerSyncMarker &marker : group.markers) {
			if (marker.stableId == 0U || marker.name.empty() || marker.name.size() > kMaximumAuthoredStringBytes ||
				marker.clipEventId == 0U || !ContainsState(controller, marker.stateId) || !markerIds.insert(marker.stableId).second) {
				AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::InvalidStructure, marker.stableId);
			}
		}
	}
	std::set<std::uint64_t> subgraphIds;
	for (const AnimationControllerSubgraphDependency &dependency : controller.subgraphDependencies) {
		if (dependency.stableId == 0U || !dependency.controllerAssetId.IsValid() || dependency.generation == 0U ||
			dependency.compatibilityFingerprint != controller.compatibilityFingerprint || !subgraphIds.insert(dependency.stableId).second) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::IncompatibleAsset, dependency.stableId);
		}
	}

	std::set<AnimationGuid> active;
	CheckSubgraphTree(controller, controller, options.availableControllers, active, 0U, result.diagnostics);
	if (!result.diagnostics.empty()) {
		std::sort(result.diagnostics.begin(), result.diagnostics.end(), [](const auto &left, const auto &right) {
			return left.code != right.code ? left.code < right.code : left.elementStableId < right.elementStableId;
		});
		result.diagnostics.erase(std::unique(result.diagnostics.begin(), result.diagnostics.end()), result.diagnostics.end());
		return result;
	}

	EvaluationProgram staged{};
	staged.controllerAssetId = controller.assetId;
	staged.compatibilityFingerprint = controller.compatibilityFingerprint;
	std::vector<AnimationControllerNode> nodes = controller.nodes;
	std::sort(nodes.begin(), nodes.end(), [](const auto &left, const auto &right) { return left.stableId < right.stableId; });
	for (std::size_t index = 0U; index < nodes.size(); ++index) {
		if (index > std::numeric_limits<std::uint32_t>::max() / kScratchBytesPerInstruction) {
			AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::HardLimitExceeded, nodes[index].stableId);
			result.status = AnimationStatus::HardLimitExceeded;
			return result;
		}
		staged.instructions.push_back({ToOpcode(nodes[index].kind), nodes[index].stableId, nodes[index].dependencyAssetId,
									   nodes[index].dependencyGeneration, nodes[index].dependencyFingerprint,
									   static_cast<std::uint32_t>(index * kScratchBytesPerInstruction)});
		staged.bytecode.push_back(static_cast<std::uint8_t>(ToOpcode(nodes[index].kind)));
	}
	staged.scratchPlan.byteCount = static_cast<std::uint32_t>(staged.instructions.size() * kScratchBytesPerInstruction);
	if (staged.scratchPlan.byteCount > kMaximumProgramScratchBytes) {
		AddDiagnostic(result.diagnostics, AnimationGraphDiagnosticCode::HardLimitExceeded, 0U);
		result.status = AnimationStatus::HardLimitExceeded;
		return result;
	}
	for (const AnimationControllerState &state : controller.states) {
		const auto node = std::find_if(controller.nodes.begin(), controller.nodes.end(),
			[&state](const AnimationControllerNode &candidate) { return candidate.stableId == state.nodeId; });
		staged.states.push_back({state.stableId, state.parentStateId, state.entry, state.exit, state.anyState, state.nodeId,
			node == controller.nodes.end() ? EvaluationOpcode::ProceduralPose : ToOpcode(node->kind)});
	}
	std::sort(staged.states.begin(), staged.states.end(),
			  [](const auto &left, const auto &right) { return left.stableId < right.stableId; });
	for (const AnimationControllerTransition &transition : controller.transitions) {
		const auto source =
			std::find_if(controller.states.begin(), controller.states.end(),
						 [&transition](const AnimationControllerState &state) { return state.stableId == transition.sourceStateId; });
		staged.transitions.push_back({transition.sourceStateId, transition.destinationStateId, transition.conditionParameterId,
									  transition.durationTicks, transition.exitTimeTicks, transition.destinationOffsetNormalized,
									  transition.priority, transition.stableId, source != controller.states.end() && source->anyState,
									  static_cast<EvaluationTransitionInterruptionSource>(transition.interruptionSource), static_cast<EvaluationTransitionInterruptionPolicy>(transition.interruptionPolicy),
									  static_cast<EvaluationTriggerConsumption>(transition.triggerConsumption), transition.triggerReset == AnimationControllerTriggerReset::EndStep,
									  transition.triggerFailure == AnimationControllerTriggerFailure::Reset});
	}
	std::sort(staged.transitions.begin(), staged.transitions.end(), [](const auto &left, const auto &right) {
		return left.priority != right.priority ? left.priority < right.priority : left.stableId < right.stableId;
	});
	for (const AnimationControllerSyncGroup &group : controller.syncGroups)
		for (const AnimationControllerSyncMarker &marker : group.markers)
			staged.syncMarkers.push_back({group.stableId, marker.stableId, marker.clipEventId, marker.stateId, StableNameHash(marker.name)});
	std::sort(staged.syncMarkers.begin(), staged.syncMarkers.end(), [](const auto &left, const auto &right) {
		return left.groupStableId != right.groupStableId ? left.groupStableId < right.groupStableId
														 : left.markerStableId < right.markerStableId;
	});
	for (const AnimationControllerNode &node : nodes) {
		if (node.kind != AnimationControllerNodeKind::BlendTree1D && node.kind != AnimationControllerNodeKind::BlendTree2D)
			continue;
		EvaluationBlendNode blend{};
		blend.nodeStableId = node.stableId;
		blend.opcode = ToOpcode(node.kind);
		blend.outOfDomainPolicy = static_cast<AnimationBlendOutOfDomainPolicy>(node.outOfDomainPolicy);
		blend.outputCompatibilityFingerprint = node.outputCompatibilityFingerprint == 0U ? controller.compatibilityFingerprint : node.outputCompatibilityFingerprint;
		for (const auto &sample : node.blendSamples)
			blend.samples.push_back({sample.stableId, sample.childNodeId, sample.positionX, sample.positionY});
		std::sort(blend.samples.begin(), blend.samples.end(), [](const auto &left, const auto &right) { return left.stableId < right.stableId; });
		staged.blendNodes.push_back(std::move(blend));
	}
	for (const AnimationControllerLayer &layer : controller.layers)
		staged.layers.push_back({layer.stableId,layer.rootStateId,layer.maskAssetId,layer.maskGeneration,layer.maskFingerprint,layer.additive,layer.defaultWeight});
	for (const AnimationControllerSyncGroup &group : controller.syncGroups)
		staged.syncGroups.push_back({group.stableId,group.leaderStateId});
	staged.stableHash = ComputeEvaluationProgramStableHash(staged);
	output = std::move(staged);
	result.status = AnimationStatus::Ok;
	return result;
}
} // namespace TerraEngine::Animation
