// AnimationControllerAuthoringService.cpp - checked model, compiler, and file boundary.

#include "AnimationControllerAuthoringService.h"

#include "AnimationAssetAuthoringDecode.h"
#include "Engine/Animation/Graph/EvaluationProgram.h"
#include "Engine/Utilities/UUID.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace TerraEngine::Editor::Animation {
	namespace {
		constexpr std::size_t kMaximumControllerBytes = 64U * 1024U * 1024U;
		std::atomic_uint64_t g_ControllerStageSequence{1U};
		// Controller saves are editor-boundary operations, so serializing their short
		// compare-and-replace window is preferable to allowing two tabs or automation
		// clients to both pass the same byte comparison and overwrite one another.
		std::mutex g_ControllerPublicationMutex;

		bool ReadFileBounded(
			const std::filesystem::path& path,
			std::string& outBytes,
			std::string& outDiagnostic) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if(!input) {
				outDiagnostic = "The controller document could not be opened.";
				return false;
			}
			const std::streamoff size = input.tellg();
			if(size <= 0 || static_cast<std::uint64_t>(size) > kMaximumControllerBytes) {
				outDiagnostic = "The controller document is empty or exceeds the bounded authoring size.";
				return false;
			}
			outBytes.resize(static_cast<std::size_t>(size));
			input.seekg(0, std::ios::beg);
			if(!input.read(outBytes.data(), size)) {
				outDiagnostic = "The complete controller document could not be read.";
				return false;
			}
			return true;
		}

		bool WriteStagedFile(
			const std::filesystem::path& path,
			std::string_view bytes,
			std::string& outDiagnostic) {
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if(!output) {
				outDiagnostic = "The staged controller document could not be created.";
				return false;
			}
			output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			output.flush();
			if(!output) {
				outDiagnostic = "The complete staged controller document could not be written.";
				return false;
			}
			return true;
		}

		bool ReplaceFileAtomically(
			const std::filesystem::path& staged,
			const std::filesystem::path& destination,
			std::string& outDiagnostic) {
			std::error_code error;
#if defined(_WIN32)
			const bool destinationExists = std::filesystem::exists(destination, error) && !error;
			const BOOL replaced = destinationExists
				? ::ReplaceFileW(destination.c_str(), staged.c_str(), nullptr,
					REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
				: ::MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);
			if(replaced == FALSE) {
				outDiagnostic = "Windows rejected the same-volume atomic controller replacement (error "
					+ std::to_string(::GetLastError()) + ").";
				return false;
			}
#else
			std::filesystem::rename(staged, destination, error);
			if(error) {
				outDiagnostic = "The platform rejected the atomic controller rename: " + error.message();
				return false;
			}
#endif
			return true;
		}

		template<typename Record>
		bool EraseById(std::vector<Record>& records, std::uint64_t stableId) {
			const auto found = std::find_if(records.begin(), records.end(), [stableId](const Record& record) {
				return record.stableId == stableId;
			});
			if(found == records.end()) return false;
			records.erase(found);
			return true;
		}

		TerraEngine::Animation::AnimationControllerParameterValue DefaultValue(
			TerraEngine::Animation::AnimationControllerParameterType type) {
			using namespace TerraEngine::Animation;
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
	}

	AnimationControllerAuthoringService::AnimationControllerAuthoringService()
		: transaction_("Controller graph edit") {}

	TerraEngine::Animation::AnimationStatus AnimationControllerAuthoringService::CreateStarterDocument(
		TerraEngine::Animation::AnimationControllerAsset& outController,
		std::vector<std::uint8_t>& outBytes) {
		using namespace TerraEngine::Animation;
		AnimationControllerAsset controller;
		controller.assetId = AnimationGuid{TerraEngine::GenerateUUID()};
		controller.skeletonAssetId = AnimationGuid{TerraEngine::GenerateUUID()};
		controller.compatibilityFingerprint = 1U;
		// A procedural node avoids inventing a clip dependency while still yielding
		// one compiler-valid entry state and base layer.
		controller.nodes.push_back({1U, AnimationControllerNodeKind::ProceduralPose});
		controller.states.push_back({2U, 1U, 0U, true, false, false});
		controller.layers.push_back({3U, 2U, {}, 0U, 0U, false, 1.0F});
		EvaluationProgram program;
		const AnimationGraphCompilationResult compiled = CompileAnimationControllerGraph(controller, {}, program);
		if(compiled.status != AnimationStatus::Ok) return compiled.status;
		const AnimationStatus serialized = SerializeAnimationControllerAsset(controller, outBytes);
		if(serialized != AnimationStatus::Ok) return serialized;
		AnimationControllerAsset decoded;
		const AnimationStatus decodedStatus = DeserializeAnimationAssetForAuthoring(
			outBytes, decoded, DeserializeAnimationControllerAsset);
		if(decodedStatus != AnimationStatus::Ok) return decodedStatus;
		outController = std::move(decoded);
		return AnimationStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::Open(
		const std::filesystem::path& path,
		std::uint64_t generation) noexcept {
		std::string bytes;
		if(path.empty() || generation == 0U || !ReadFileBounded(path, bytes, diagnostic_)) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		return OpenBytes(path, generation,
			std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::OpenBytes(
		const std::filesystem::path& path,
		std::uint64_t generation,
		std::span<const std::uint8_t> bytes) noexcept {
		using namespace TerraEngine::Animation;
		if(generation == 0U || bytes.empty() || bytes.size() > kMaximumControllerBytes) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		AnimationControllerAsset decoded;
		const AnimationStatus status = DeserializeAnimationAssetForAuthoring(
			bytes, decoded, DeserializeAnimationControllerAsset);
		if(status != AnimationStatus::Ok) {
			diagnostic_ = "Checked controller decoding failed: " + std::string(ToString(status)) + ".";
			return AnimationAuthoringStatus::ValidationFailed;
		}
		path_ = path.lexically_normal();
		published_ = {decoded.assetId, generation,
			std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())};
		publishedController_ = std::move(decoded);
		candidateController_ = {};
		history_.Clear();
		editing_ = false;
		diagnostic_ = "Checked controller document opened.";
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::BeginEdit(
		TerraEngine::Animation::AnimationGuid expectedAssetId,
		std::uint64_t expectedGeneration) noexcept {
		if(!HasOpenDocument()) return AnimationAuthoringStatus::Rejected;
		const AnimationAuthoringStatus status = transaction_.Begin(
			published_, expectedAssetId, expectedGeneration);
		if(status != AnimationAuthoringStatus::Ok) return status;
		candidateController_ = publishedController_;
		editing_ = true;
		diagnostic_ = "Detached controller candidate ready.";
		return AnimationAuthoringStatus::Ok;
	}

	const TerraEngine::Animation::AnimationControllerAsset*
	AnimationControllerAuthoringService::Published() const noexcept {
		return HasOpenDocument() ? &publishedController_ : nullptr;
	}

	const TerraEngine::Animation::AnimationControllerAsset*
	AnimationControllerAuthoringService::Candidate() const noexcept {
		return editing_ ? &candidateController_ : nullptr;
	}

	TerraEngine::Animation::AnimationControllerAsset*
	AnimationControllerAuthoringService::MutableCandidate() noexcept {
		return editing_ ? &candidateController_ : nullptr;
	}

	std::uint64_t AnimationControllerAuthoringService::NextStableId() const noexcept {
		const auto* controller = Candidate();
		if(controller == nullptr) return 0U;
		std::uint64_t largest = 0U;
		const auto accumulate = [&largest](const auto& records) {
			for(const auto& record : records) largest = (std::max)(largest, record.stableId);
		};
		accumulate(controller->parameters);
		accumulate(controller->graphVariables);
		accumulate(controller->reusableBindings);
		accumulate(controller->nodes);
		accumulate(controller->states);
		accumulate(controller->transitions);
		accumulate(controller->layers);
		accumulate(controller->syncGroups);
		accumulate(controller->subgraphDependencies);
		for(const auto& node : controller->nodes) accumulate(node.blendSamples);
		for(const auto& group : controller->syncGroups) accumulate(group.markers);
		return largest == (std::numeric_limits<std::uint64_t>::max)() ? 0U : largest + 1U;
	}

	std::string AnimationControllerAuthoringService::DefaultParameterName(
		TerraEngine::Animation::AnimationControllerParameterType type) {
		using TerraEngine::Animation::AnimationControllerParameterType;
		switch(type) {
		case AnimationControllerParameterType::Float: return "Float";
		case AnimationControllerParameterType::Integer: return "Integer";
		case AnimationControllerParameterType::Boolean: return "Boolean";
		case AnimationControllerParameterType::Trigger: return "Trigger";
		case AnimationControllerParameterType::Vector: return "Vector";
		case AnimationControllerParameterType::Transform: return "Transform";
		case AnimationControllerParameterType::Tag: return "Tag";
		}
		return "Parameter";
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::AddParameter(
		std::string name,
		TerraEngine::Animation::AnimationControllerParameterType type,
		std::uint64_t& outStableId) noexcept {
		auto* controller = MutableCandidate();
		const std::uint64_t stableId = NextStableId();
		if(controller == nullptr || stableId == 0U
			|| type > TerraEngine::Animation::AnimationControllerParameterType::Tag) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		if(name.empty()) name = DefaultParameterName(type) + " " + std::to_string(stableId);
		controller->parameters.push_back({stableId, std::move(name), type, DefaultValue(type)});
		outStableId = stableId;
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::AddNode(
		TerraEngine::Animation::AnimationControllerNodeKind kind,
		std::uint64_t& outStableId) noexcept {
		auto* controller = MutableCandidate();
		const std::uint64_t stableId = NextStableId();
		if(controller == nullptr || stableId == 0U
			|| kind > TerraEngine::Animation::AnimationControllerNodeKind::ProceduralPose) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		TerraEngine::Animation::AnimationControllerNode node;
		node.stableId = stableId;
		node.kind = kind;
		controller->nodes.push_back(node);
		outStableId = stableId;
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::AddState(
		std::uint64_t nodeId,
		bool entry,
		std::uint64_t& outStableId) noexcept {
		auto* controller = MutableCandidate();
		const std::uint64_t stableId = NextStableId();
		if(controller == nullptr || stableId == 0U || !std::any_of(
			controller->nodes.begin(), controller->nodes.end(), [nodeId](const auto& node) {
				return node.stableId == nodeId;
			})) return AnimationAuthoringStatus::InvalidArgument;
		controller->states.push_back({stableId, nodeId, 0U, entry, false, false});
		outStableId = stableId;
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::AddTransition(
		std::uint64_t sourceStateId,
		std::uint64_t destinationStateId,
		std::uint64_t& outStableId) noexcept {
		auto* controller = MutableCandidate();
		const std::uint64_t stableId = NextStableId();
		const auto hasState = [controller](std::uint64_t id) {
			return controller != nullptr && std::any_of(controller->states.begin(), controller->states.end(),
				[id](const auto& state) { return state.stableId == id; });
		};
		if(stableId == 0U || !hasState(sourceStateId) || !hasState(destinationStateId)) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		TerraEngine::Animation::AnimationControllerTransition transition;
		transition.stableId = stableId;
		transition.sourceStateId = sourceStateId;
		transition.destinationStateId = destinationStateId;
		controller->transitions.push_back(transition);
		outStableId = stableId;
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::AddLayer(
		std::uint64_t rootStateId,
		std::uint64_t& outStableId) noexcept {
		auto* controller = MutableCandidate();
		const std::uint64_t stableId = NextStableId();
		if(controller == nullptr || stableId == 0U || !std::any_of(
			controller->states.begin(), controller->states.end(), [rootStateId](const auto& state) {
				return state.stableId == rootStateId;
			})) return AnimationAuthoringStatus::InvalidArgument;
		controller->layers.push_back({stableId, rootStateId, {}, 0U, 0U, false, 1.0F});
		outStableId = stableId;
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::RemoveParameter(std::uint64_t stableId) noexcept {
		auto* controller = MutableCandidate();
		if(controller == nullptr || !EraseById(controller->parameters, stableId)) return AnimationAuthoringStatus::InvalidArgument;
		std::erase_if(controller->transitions, [stableId](const auto& value) { return value.conditionParameterId == stableId; });
		std::erase_if(controller->reusableBindings, [stableId](const auto& value) { return value.sourceParameterId == stableId; });
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::RemoveNode(std::uint64_t stableId) noexcept {
		auto* controller = MutableCandidate();
		if(controller == nullptr || !EraseById(controller->nodes, stableId)) return AnimationAuthoringStatus::InvalidArgument;
		std::vector<std::uint64_t> states;
		for(const auto& state : controller->states) if(state.nodeId == stableId) states.push_back(state.stableId);
		for(const std::uint64_t stateId : states) (void)RemoveState(stateId);
		for(auto& node : controller->nodes) {
			std::erase_if(node.blendSamples, [stableId](const auto& sample) { return sample.childNodeId == stableId; });
		}
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::RemoveState(std::uint64_t stableId) noexcept {
		auto* controller = MutableCandidate();
		if(controller == nullptr || !EraseById(controller->states, stableId)) return AnimationAuthoringStatus::InvalidArgument;
		std::erase_if(controller->transitions, [stableId](const auto& value) {
			return value.sourceStateId == stableId || value.destinationStateId == stableId;
		});
		std::erase_if(controller->layers, [stableId](const auto& value) { return value.rootStateId == stableId; });
		for(auto& state : controller->states) if(state.parentStateId == stableId) state.parentStateId = 0U;
		for(auto& group : controller->syncGroups) {
			std::erase_if(group.markers, [stableId](const auto& marker) { return marker.stateId == stableId; });
		}
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::RemoveTransition(std::uint64_t stableId) noexcept {
		auto* controller = MutableCandidate();
		return controller != nullptr && EraseById(controller->transitions, stableId)
			? AnimationAuthoringStatus::Ok : AnimationAuthoringStatus::InvalidArgument;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::RemoveLayer(std::uint64_t stableId) noexcept {
		auto* controller = MutableCandidate();
		return controller != nullptr && EraseById(controller->layers, stableId)
			? AnimationAuthoringStatus::Ok : AnimationAuthoringStatus::InvalidArgument;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::PrepareCommit(
		AnimationAuthoringCompileRequest& outRequest) noexcept {
		using namespace TerraEngine::Animation;
		if(!editing_) return AnimationAuthoringStatus::Rejected;
		std::vector<std::uint8_t> bytes;
		const AnimationStatus status = SerializeAnimationControllerAsset(candidateController_, bytes);
		if(status != AnimationStatus::Ok) {
			diagnostic_ = "The complete controller candidate could not be serialized: "
				+ std::string(ToString(status)) + ".";
			return AnimationAuthoringStatus::ValidationFailed;
		}
		const AnimationAuthoringStatus replaced = transaction_.Replace(
			std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
		if(replaced != AnimationAuthoringStatus::Ok) return replaced;
		diagnostic_ = "Controller candidate copied for worker validation and compilation.";
		return transaction_.PrepareCompile(outRequest);
	}

	AnimationControllerCompileProduct AnimationControllerAuthoringService::CompileCandidate(
		const AnimationAuthoringCompileRequest& request) noexcept {
		using namespace TerraEngine::Animation;
		AnimationControllerCompileProduct product;
		product.authoring.assetId = request.assetId;
		product.authoring.expectedGeneration = request.expectedGeneration;
		product.authoring.requestId = request.requestId;
		const auto canceled = [&request]() {
			return request.cancellationRequested
				&& request.cancellationRequested->load(std::memory_order_acquire);
		};
		if(canceled()) {
			product.authoring.canceled = true;
			product.authoring.diagnostic = "Controller compilation was canceled before decoding.";
			return product;
		}
		AnimationControllerAsset controller;
		const auto bytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(request.candidateBytes.data()),
			request.candidateBytes.size());
		const AnimationStatus decoded = DeserializeAnimationAssetForAuthoring(
			bytes, controller, DeserializeAnimationControllerAsset);
		if(decoded != AnimationStatus::Ok || controller.assetId != request.assetId) {
			product.authoring.diagnostic = "Candidate round-trip decoding or GUID validation failed: "
				+ std::string(ToString(decoded)) + ".";
			return product;
		}
		if(canceled()) {
			product.authoring.canceled = true;
			product.authoring.diagnostic = "Controller compilation was canceled before graph compilation.";
			return product;
		}
		EvaluationProgram stagedProgram;
		product.graph = CompileAnimationControllerGraph(controller, {}, stagedProgram);
		if(product.graph.status != AnimationStatus::Ok) {
			product.authoring.diagnostic = "The complete controller graph did not compile: "
				+ std::string(ToString(product.graph.status)) + ".";
			return product;
		}
		if(canceled()) {
			product.authoring.canceled = true;
			product.authoring.diagnostic = "Controller compilation was canceled before publication handoff.";
			return product;
		}
		product.authoring.validatedBytes = request.candidateBytes;
		product.authoring.succeeded = true;
		product.authoring.diagnostic = "Controller serializer, decoder, and graph compiler accepted the complete candidate.";
		return product;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::CompleteCommit(
		const AnimationControllerCompileProduct& product) noexcept {
		AnimationAuthoringRevision published;
		std::string publicationDiagnostic;
		const AnimationAuthoringRevision before = published_;
		const AnimationAuthoringStatus status = transaction_.CompleteCompile(
			product.authoring, published_,
			[this](const auto& expected, const auto& replacement, std::string& diagnostic) {
				return PublishRevision(expected, replacement, diagnostic);
			}, published, publicationDiagnostic);
		if(status != AnimationAuthoringStatus::Ok) {
			diagnostic_ = publicationDiagnostic.empty() ? product.authoring.diagnostic : publicationDiagnostic;
			return status;
		}
		const AnimationAuthoringStatus historyStatus = history_.Commit(before, published);
		if(historyStatus != AnimationAuthoringStatus::Ok) {
			diagnostic_ = "The bytes were published but the grouped history entry was rejected.";
			return historyStatus;
		}
		editing_ = false;
		const AnimationAuthoringStatus adopted = AdoptPublishedRevision(std::move(published));
		if(adopted == AnimationAuthoringStatus::Ok) diagnostic_ = product.authoring.diagnostic;
		return adopted;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::Cancel() noexcept {
		const AnimationAuthoringStatus status = transaction_.Cancel();
		if(status == AnimationAuthoringStatus::Canceled) {
			candidateController_ = {};
			editing_ = false;
			diagnostic_ = "The detached controller edit was canceled; published bytes were unchanged.";
		}
		return status;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::Undo() noexcept {
		AnimationAuthoringRevision published;
		std::string publicationDiagnostic;
		const AnimationAuthoringStatus status = history_.Undo(
			published_, [this](const auto& expected, const auto& replacement, std::string& diagnostic) {
				return PublishRevision(expected, replacement, diagnostic);
			}, published, publicationDiagnostic);
		if(status != AnimationAuthoringStatus::Ok) {
			diagnostic_ = publicationDiagnostic;
			return status;
		}
		const AnimationAuthoringStatus adopted = AdoptPublishedRevision(std::move(published));
		if(adopted == AnimationAuthoringStatus::Ok) diagnostic_ = "Undo atomically published the complete before revision.";
		return adopted;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::Redo() noexcept {
		AnimationAuthoringRevision published;
		std::string publicationDiagnostic;
		const AnimationAuthoringStatus status = history_.Redo(
			published_, [this](const auto& expected, const auto& replacement, std::string& diagnostic) {
				return PublishRevision(expected, replacement, diagnostic);
			}, published, publicationDiagnostic);
		if(status != AnimationAuthoringStatus::Ok) {
			diagnostic_ = publicationDiagnostic;
			return status;
		}
		const AnimationAuthoringStatus adopted = AdoptPublishedRevision(std::move(published));
		if(adopted == AnimationAuthoringStatus::Ok) diagnostic_ = "Redo atomically published the complete after revision.";
		return adopted;
	}

	bool AnimationControllerAuthoringService::PublishRevision(
		const AnimationAuthoringRevision& expectedCurrent,
		const AnimationAuthoringRevision& replacement,
		std::string& outDiagnostic) noexcept {
		using namespace TerraEngine::Animation;
		if(!expectedCurrent.assetId.IsValid() || replacement.assetId != expectedCurrent.assetId
			|| replacement.generation != expectedCurrent.generation + 1U
			|| replacement.bytes.empty() || replacement.bytes.size() > kMaximumControllerBytes) {
			outDiagnostic = "The controller publication revision is incomplete or stale.";
			return false;
		}
		AnimationControllerAsset decoded;
		const auto replacementBytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(replacement.bytes.data()), replacement.bytes.size());
		if(DeserializeAnimationAssetForAuthoring(
			replacementBytes, decoded, DeserializeAnimationControllerAsset) != AnimationStatus::Ok
			|| decoded.assetId != expectedCurrent.assetId) {
			outDiagnostic = "The replacement controller failed final GUID/decoder validation.";
			return false;
		}
		EvaluationProgram stagedProgram;
		if(CompileAnimationControllerGraph(decoded, {}, stagedProgram).status != AnimationStatus::Ok) {
			outDiagnostic = "The replacement controller failed final graph compilation.";
			return false;
		}

		// Detached tests may supply no path; the same compare-and-swap still updates
		// only this service after every validator has accepted the bytes.
		if(path_.empty()) return published_.bytes == expectedCurrent.bytes;
		// Keep the disk comparison and atomic replacement in one process-wide critical
		// section. The OS rename is atomic, but without this lock two publishers that
		// began from the same revision could both win the preceding comparison.
		std::scoped_lock publicationLock(g_ControllerPublicationMutex);
		std::string diskBytes;
		if(!ReadFileBounded(path_, diskBytes, outDiagnostic) || diskBytes != expectedCurrent.bytes) {
			outDiagnostic = "The controller file changed after editing began; publication was rejected as stale.";
			return false;
		}
		const std::filesystem::path staged = path_.parent_path()
			/ (path_.filename().string() + ".authoring-"
				+ std::to_string(g_ControllerStageSequence.fetch_add(1U, std::memory_order_relaxed)) + ".tmp");
		std::error_code cleanupError;
		if(!WriteStagedFile(staged, replacement.bytes, outDiagnostic)) {
			std::filesystem::remove(staged, cleanupError);
			return false;
		}
		if(!ReplaceFileAtomically(staged, path_, outDiagnostic)) {
			std::filesystem::remove(staged, cleanupError);
			return false;
		}
		return true;
	}

	AnimationAuthoringStatus AnimationControllerAuthoringService::AdoptPublishedRevision(
		AnimationAuthoringRevision revision) noexcept {
		using namespace TerraEngine::Animation;
		AnimationControllerAsset decoded;
		const auto bytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(revision.bytes.data()), revision.bytes.size());
		if(DeserializeAnimationAssetForAuthoring(
			bytes, decoded, DeserializeAnimationControllerAsset) != AnimationStatus::Ok
			|| decoded.assetId != revision.assetId) {
			return AnimationAuthoringStatus::ValidationFailed;
		}
		published_ = std::move(revision);
		publishedController_ = std::move(decoded);
		candidateController_ = {};
		editing_ = false;
		return AnimationAuthoringStatus::Ok;
	}
}
