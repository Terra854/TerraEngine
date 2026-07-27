// AnimationControllerAuthoringService.h - UI-independent controller editing.
//
// The window, tests, and future automation adapters can edit the same detached
// model. Worker preparation is value-only; publication stays on the editor
// boundary and uses a GUID/generation/byte compare before atomic replacement.

#pragma once

#include "AnimationAuthoringTransaction.h"
#include "Engine/Animation/Assets/AnimationControllerAsset.h"
#include "Engine/Animation/Graph/AnimationGraphCompiler.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Animation {
	struct AnimationControllerCompileProduct {
		AnimationAuthoringCompileResult authoring;
		TerraEngine::Animation::AnimationGraphCompilationResult graph;
	};

	class AnimationControllerAuthoringService final {
	public:
		AnimationControllerAuthoringService();

		// A new controller is immediately decodable and compiler-valid. The generated
		// skeleton identity is an explicit placeholder that artists replace when they
		// bind the graph to an imported skeleton.
		[[nodiscard]] static TerraEngine::Animation::AnimationStatus CreateStarterDocument(
			TerraEngine::Animation::AnimationControllerAsset& outController,
			std::vector<std::uint8_t>& outBytes);

		AnimationAuthoringStatus Open(
			const std::filesystem::path& path,
			std::uint64_t generation) noexcept;
		AnimationAuthoringStatus OpenBytes(
			const std::filesystem::path& path,
			std::uint64_t generation,
			std::span<const std::uint8_t> bytes) noexcept;
		AnimationAuthoringStatus BeginEdit(
			TerraEngine::Animation::AnimationGuid expectedAssetId,
			std::uint64_t expectedGeneration) noexcept;

		[[nodiscard]] const TerraEngine::Animation::AnimationControllerAsset* Published() const noexcept;
		[[nodiscard]] const TerraEngine::Animation::AnimationControllerAsset* Candidate() const noexcept;
		// MutableCandidate changes only the private edit copy. PrepareCommit performs
		// complete serializer, round-trip decoder, and graph compiler validation.
		[[nodiscard]] TerraEngine::Animation::AnimationControllerAsset* MutableCandidate() noexcept;
		[[nodiscard]] std::uint64_t NextStableId() const noexcept;

		AnimationAuthoringStatus AddParameter(
			std::string name,
			TerraEngine::Animation::AnimationControllerParameterType type,
			std::uint64_t& outStableId) noexcept;
		AnimationAuthoringStatus AddNode(
			TerraEngine::Animation::AnimationControllerNodeKind kind,
			std::uint64_t& outStableId) noexcept;
		AnimationAuthoringStatus AddState(
			std::uint64_t nodeId,
			bool entry,
			std::uint64_t& outStableId) noexcept;
		AnimationAuthoringStatus AddTransition(
			std::uint64_t sourceStateId,
			std::uint64_t destinationStateId,
			std::uint64_t& outStableId) noexcept;
		AnimationAuthoringStatus AddLayer(
			std::uint64_t rootStateId,
			std::uint64_t& outStableId) noexcept;
		AnimationAuthoringStatus RemoveParameter(std::uint64_t stableId) noexcept;
		AnimationAuthoringStatus RemoveNode(std::uint64_t stableId) noexcept;
		AnimationAuthoringStatus RemoveState(std::uint64_t stableId) noexcept;
		AnimationAuthoringStatus RemoveTransition(std::uint64_t stableId) noexcept;
		AnimationAuthoringStatus RemoveLayer(std::uint64_t stableId) noexcept;

		AnimationAuthoringStatus PrepareCommit(AnimationAuthoringCompileRequest& outRequest) noexcept;
		// CompileCandidate is worker-safe: it reads only the copied request and writes
		// only its returned product. File publication is deliberately absent here.
		[[nodiscard]] static AnimationControllerCompileProduct CompileCandidate(
			const AnimationAuthoringCompileRequest& request) noexcept;
		AnimationAuthoringStatus CompleteCommit(
			const AnimationControllerCompileProduct& product) noexcept;
		AnimationAuthoringStatus Cancel() noexcept;
		AnimationAuthoringStatus Undo() noexcept;
		AnimationAuthoringStatus Redo() noexcept;

		[[nodiscard]] bool HasOpenDocument() const noexcept { return published_.assetId.IsValid(); }
		[[nodiscard]] bool HasCandidate() const noexcept { return editing_; }
		[[nodiscard]] std::uint64_t Generation() const noexcept { return published_.generation; }
		[[nodiscard]] TerraEngine::Animation::AnimationGuid AssetId() const noexcept { return published_.assetId; }
		[[nodiscard]] std::size_t UndoDepth() const noexcept { return history_.UndoDepth(); }
		[[nodiscard]] std::size_t RedoDepth() const noexcept { return history_.RedoDepth(); }
		[[nodiscard]] const std::string& Diagnostic() const noexcept { return diagnostic_; }
		[[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

	private:
		[[nodiscard]] bool PublishRevision(
			const AnimationAuthoringRevision& expectedCurrent,
			const AnimationAuthoringRevision& replacement,
			std::string& outDiagnostic) noexcept;
		AnimationAuthoringStatus AdoptPublishedRevision(
			AnimationAuthoringRevision revision) noexcept;
		[[nodiscard]] static std::string DefaultParameterName(
			TerraEngine::Animation::AnimationControllerParameterType type);

		std::filesystem::path path_;
		AnimationAuthoringRevision published_;
		TerraEngine::Animation::AnimationControllerAsset publishedController_;
		TerraEngine::Animation::AnimationControllerAsset candidateController_;
		AnimationAuthoringTransaction transaction_;
		AnimationAuthoringHistory history_;
		std::string diagnostic_;
		bool editing_ = false;
	};
}
