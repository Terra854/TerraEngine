// AnimationAssetActions.cpp - browser-reachable animation asset lifecycle actions.

#include "AnimationAssetActions.h"

#include "../Animation/AnimationControllerAuthoringService.h"
#include "Engine/Animation/Assets/AnimationClipAsset.h"
#include "Engine/Animation/Assets/AnimationMaskAsset.h"
#include "Engine/Animation/Assets/MotionDatabaseAsset.h"
#include "Engine/Animation/Assets/RagdollProfileAsset.h"
#include "Engine/Animation/Assets/RetargetProfileAsset.h"
#include "Engine/Animation/Assets/SkeletonAsset.h"
#include "Engine/Animation/Graph/AnimationGraphCompiler.h"
#include "Engine/File Formats/TerraEngineAnimationFile.h"
#include "Engine/Utilities/UUID.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <span>
#include <system_error>
#include <utility>

namespace TerraEngine::Editor::Assets {
	namespace {
		using Status = Animation::AnimationAuthoringStatus;
		using namespace TerraEngine::Animation;
		constexpr std::uint64_t kMaximumLifecycleDocumentBytes = 64U * 1024U * 1024U;
		std::atomic_uint64_t gLifecycleStageSequence{1U};

		Status WriteBytesAtomically(const std::filesystem::path& destination,
			std::span<const std::uint8_t> bytes) {
			if(destination.empty() || bytes.empty()
				|| bytes.size() > kMaximumLifecycleDocumentBytes) {
				return bytes.size() > kMaximumLifecycleDocumentBytes
					? Status::CapacityExceeded : Status::InvalidArgument;
			}
			std::error_code error;
			std::filesystem::create_directories(destination.parent_path(), error);
			if(error || std::filesystem::exists(destination, error) || error) return Status::Rejected;
			const std::uint64_t sequence = gLifecycleStageSequence.fetch_add(1U,
				std::memory_order_relaxed);
			if(sequence == 0U) return Status::Rejected;
			const std::filesystem::path staged = destination.string()
				+ ".animation-action-" + std::to_string(sequence) + ".tmp";
			std::filesystem::remove(staged, error);
			{
				std::ofstream output(staged, std::ios::binary | std::ios::trunc);
				if(!output) return Status::Rejected;
				output.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
				output.flush();
				if(!output) {
					output.close();
					std::filesystem::remove(staged, error);
					return Status::Rejected;
				}
			}
			// The destination is required to be absent. Publishing by same-volume rename
			// prevents AssetDatabase/FileWatcher from ever observing a partial document.
			std::filesystem::rename(staged, destination, error);
			if(error) {
				std::filesystem::remove(staged, error);
				return Status::Rejected;
			}
			return Status::Ok;
		}

		std::optional<AnimationAssetKind> KindForExtension(const std::filesystem::path& path) {
			const std::string extension = path.extension().string();
			for(const AnimationAssetKind kind : AnimationAssetActions::SupportedKinds()) {
				if(extension == AnimationAssetActions::Extension(kind)) return kind;
			}
			return std::nullopt;
		}

		bool ReadCandidateBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if(!input) return false;
			const std::streamoff size = input.tellg();
			if(size <= 0 || size > static_cast<std::streamoff>(64U * 1024U * 1024U)) return false;
			bytes.resize(static_cast<std::size_t>(size));
			input.seekg(0, std::ios::beg);
			return static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), size));
		}

		// Compare canonical path components instead of string prefixes. This keeps a
		// destination such as AssetsBackup from being mistaken for a child of Assets,
		// and lets callers reject generated sources outside the current build root.
		bool IsContainedPath(const std::filesystem::path& candidate,
			const std::filesystem::path& root) {
			std::error_code error;
			const std::filesystem::path canonicalCandidate =
				std::filesystem::weakly_canonical(candidate, error);
			if(error) return false;
			const std::filesystem::path canonicalRoot =
				std::filesystem::weakly_canonical(root, error);
			if(error || canonicalRoot.empty()) return false;
			auto candidatePart = canonicalCandidate.begin();
			for(auto rootPart = canonicalRoot.begin(); rootPart != canonicalRoot.end();
				++rootPart, ++candidatePart) {
				if(candidatePart == canonicalCandidate.end() || *candidatePart != *rootPart) {
					return false;
				}
			}
			return true;
		}

		AnimationStatus DeserializeSkeletonDocument(
			std::span<const std::uint8_t> bytes,
			SkeletonAsset& asset) {
			// Skeleton decoding intentionally requires an exact compatibility fingerprint,
			// unlike the other asset decoders whose zero argument means "accept the file's
			// fingerprint." Copy only the fixed prefix to obtain that expected value; the
			// production decoder still checks the complete envelope, hash, and payload once.
			if(bytes.size() < sizeof(AnimationAssetEnvelope)) return AnimationStatus::TruncatedInput;
			AnimationAssetEnvelope envelope{};
			std::memcpy(&envelope, bytes.data(), sizeof(envelope));
			const AnimationCompatibilityFingerprint fingerprint =
				envelope.compatibilityFingerprint;
			if(fingerprint == 0U) return AnimationStatus::IncompatibleAsset;
			return DeserializeSkeletonAsset(bytes, fingerprint, asset);
		}

		AnimationStatus ValidateTypedDocument(
			AnimationAssetKind kind,
			std::span<const std::uint8_t> bytes) {
			switch(kind) {
			case AnimationAssetKind::Skeleton: {
				SkeletonAsset asset;
				return DeserializeSkeletonDocument(bytes, asset);
			}
			case AnimationAssetKind::Clip: {
				AnimationClipAsset asset;
				return DeserializeAnimationClipAsset(bytes, 0U, asset);
			}
			case AnimationAssetKind::Controller: {
				AnimationControllerAsset asset;
				const AnimationStatus decoded = DeserializeAnimationControllerAsset(bytes, 0U, asset);
				if(decoded != AnimationStatus::Ok) return decoded;
				EvaluationProgram program;
				return CompileAnimationControllerGraph(asset, {}, program).status;
			}
			case AnimationAssetKind::Mask: {
				AnimationMaskAsset asset;
				return DeserializeAnimationMaskAsset(bytes, 0U, asset);
			}
			case AnimationAssetKind::RetargetProfile: {
				RetargetProfileAsset asset;
				return DeserializeRetargetProfileAsset(bytes, 0U, asset);
			}
			case AnimationAssetKind::MotionDatabase: {
				MotionDatabaseAsset asset;
				return DeserializeMotionDatabaseAsset(bytes, 0U, asset);
			}
			case AnimationAssetKind::RagdollProfile: {
				RagdollProfileAsset asset;
				return DeserializeRagdollProfileAsset(bytes, 0U, asset);
			}
			}
			return AnimationStatus::InvalidArgument;
		}

		AnimationGuid NewGuid() {
			return AnimationGuid{TerraEngine::GenerateUUID()};
		}

		AnimationMatrix4x4 IdentityMatrix() {
			AnimationMatrix4x4 value{};
			value.values = {1.0F, 0.0F, 0.0F, 0.0F,
				0.0F, 1.0F, 0.0F, 0.0F,
				0.0F, 0.0F, 1.0F, 0.0F,
				0.0F, 0.0F, 0.0F, 1.0F};
			return value;
		}

		AnimationStatus BuildStarterBytes(
			AnimationAssetKind kind,
			std::vector<std::uint8_t>& bytes) {
			// Dependency-bound starters use explicit generated identities and a nonzero
			// fingerprint. They are structurally valid authoring documents whose typed
			// editors can replace those bindings before runtime publication.
			switch(kind) {
			case AnimationAssetKind::Skeleton: {
				SkeletonAsset asset;
				asset.assetId = NewGuid();
				asset.compatibilityFingerprint = 1U;
				asset.referencePoseHash = 1U;
				asset.source.sourcePackagePath = "Editor/Created";
				asset.joints.push_back({1U, -1, {}, true, "Root", "/root", "root"});
				asset.inverseBindMatrices.push_back(IdentityMatrix());
				return SerializeSkeletonAsset(asset, bytes);
			}
			case AnimationAssetKind::Clip: {
				AnimationClipAsset asset;
				asset.assetId = NewGuid();
				asset.skeletonAssetId = NewGuid();
				asset.compatibilityFingerprint = 1U;
				asset.compression.decoderVersion = 1U;
				return SerializeAnimationClipAsset(asset, bytes);
			}
			case AnimationAssetKind::Controller: {
				AnimationControllerAsset controller;
				return Animation::AnimationControllerAuthoringService::CreateStarterDocument(
					controller, bytes);
			}
			case AnimationAssetKind::Mask: {
				AnimationMaskAsset asset;
				asset.assetId = NewGuid();
				asset.skeletonAssetId = NewGuid();
				asset.compatibilityFingerprint = 1U;
				return SerializeAnimationMaskAsset(asset, bytes);
			}
			case AnimationAssetKind::RetargetProfile: {
				RetargetProfileAsset asset;
				asset.assetId = NewGuid();
				asset.compatibilityFingerprint = 1U;
				asset.sourceSkeletonAssetId = NewGuid();
				asset.targetSkeletonAssetId = NewGuid();
				asset.sourceSkeletonFingerprint = 1U;
				asset.targetSkeletonFingerprint = 1U;
				asset.sourceReferencePoseHash = 1U;
				asset.targetReferencePoseHash = 1U;
				return SerializeRetargetProfileAsset(asset, bytes);
			}
			case AnimationAssetKind::MotionDatabase: {
				MotionDatabaseAsset asset;
				asset.assetId = NewGuid();
				asset.compatibilityFingerprint = 1U;
				asset.sourceFingerprint = 1U;
				asset.quantizationScales.fill(1.0F);
				return SerializeMotionDatabaseAsset(asset, bytes);
			}
			case AnimationAssetKind::RagdollProfile: {
				RagdollProfileAsset asset;
				asset.assetId = NewGuid();
				asset.skeletonAssetId = NewGuid();
				asset.skeletonFingerprint = 1U;
				asset.referencePoseHash = 1U;
				asset.physicsProviderFingerprint = 1U;
				asset.rootBodyId = 1U;
				asset.expectedTotalMassKilograms = 1.0F;
				RagdollColliderRecord collider;
				collider.colliderId = 2U;
				RagdollBodyRecord body;
				body.bodyId = 1U;
				body.stableJointId = 1U;
				body.colliders.push_back(collider);
				asset.bodies.push_back(std::move(body));
				return SerializeRagdollProfileAsset(asset, bytes);
			}
			}
			return AnimationStatus::InvalidArgument;
		}

		AnimationStatus CloneDocumentWithNewIdentity(
			AnimationAssetKind kind,
			std::span<const std::uint8_t> sourceBytes,
			std::vector<std::uint8_t>& clonedBytes) {
			// A duplicated source file is a distinct authored asset. Keep all dependency
			// identities intact, but replace its own GUID before checked serialization so
			// AssetDatabase never sees two documents claiming the same asset identity.
			switch(kind) {
			case AnimationAssetKind::Skeleton: {
				SkeletonAsset asset;
				if(const AnimationStatus status = DeserializeSkeletonDocument(sourceBytes, asset);
					status != AnimationStatus::Ok) return status;
				asset.assetId = NewGuid();
				return SerializeSkeletonAsset(asset, clonedBytes);
			}
			case AnimationAssetKind::Clip: {
				AnimationClipAsset asset;
				if(const AnimationStatus status = DeserializeAnimationClipAsset(sourceBytes, 0U, asset);
					status != AnimationStatus::Ok) return status;
				asset.assetId = NewGuid();
				return SerializeAnimationClipAsset(asset, clonedBytes);
			}
			case AnimationAssetKind::Controller: {
				AnimationControllerAsset asset;
				if(const AnimationStatus status = DeserializeAnimationControllerAsset(sourceBytes, 0U, asset);
					status != AnimationStatus::Ok) return status;
				asset.assetId = NewGuid();
				return SerializeAnimationControllerAsset(asset, clonedBytes);
			}
			case AnimationAssetKind::Mask: {
				AnimationMaskAsset asset;
				if(const AnimationStatus status = DeserializeAnimationMaskAsset(sourceBytes, 0U, asset);
					status != AnimationStatus::Ok) return status;
				asset.assetId = NewGuid();
				return SerializeAnimationMaskAsset(asset, clonedBytes);
			}
			case AnimationAssetKind::RetargetProfile: {
				RetargetProfileAsset asset;
				if(const AnimationStatus status = DeserializeRetargetProfileAsset(sourceBytes, 0U, asset);
					status != AnimationStatus::Ok) return status;
				asset.assetId = NewGuid();
				return SerializeRetargetProfileAsset(asset, clonedBytes);
			}
			case AnimationAssetKind::MotionDatabase: {
				MotionDatabaseAsset asset;
				if(const AnimationStatus status = DeserializeMotionDatabaseAsset(sourceBytes, 0U, asset);
					status != AnimationStatus::Ok) return status;
				asset.assetId = NewGuid();
				return SerializeMotionDatabaseAsset(asset, clonedBytes);
			}
			case AnimationAssetKind::RagdollProfile: {
				RagdollProfileAsset asset;
				if(const AnimationStatus status = DeserializeRagdollProfileAsset(sourceBytes, 0U, asset);
					status != AnimationStatus::Ok) return status;
				asset.assetId = NewGuid();
				return SerializeRagdollProfileAsset(asset, clonedBytes);
			}
			}
			return AnimationStatus::InvalidArgument;
		}
	}

	AnimationAssetActions::AnimationAssetActions(std::filesystem::path assetRoot)
		: assetRoot_(std::move(assetRoot)) {}

	Status AnimationAssetActions::SetAssetRoot(std::filesystem::path assetRoot) noexcept {
		if(!assets_.empty() || assetRoot.empty()) return Status::Rejected;
		assetRoot_ = std::move(assetRoot);
		return Status::Ok;
	}

	Status AnimationAssetActions::Create(AnimationAssetKind kind, std::string name,
		std::string folder, AnimationAssetEntry& out) noexcept {
		if(!IsValidName(name) || !IsSafeFolder(folder)) return Status::InvalidArgument;
		AnimationAssetEntry entry;
		entry.id = nextId_++;
		entry.kind = kind;
		entry.name = std::move(name);
		entry.folder = std::move(folder);
		entry.sourcePath = MakeUniqueSourcePath(entry);
		if(const Status written = WriteCreatedDocument(entry); written != Status::Ok) return written;
		assets_.emplace(entry.id, entry);
		CommitInverse(std::nullopt, entry);
		out = entry;
		lastDiagnostic_ = "Created a production-decoder-valid typed starter document.";
		return Status::Ok;
	}

	Status AnimationAssetActions::Import(const std::filesystem::path& source,
		std::string destinationFolder, AnimationAssetEntry& out) noexcept {
		if(assetRoot_.empty() || !std::filesystem::is_regular_file(source)
			|| !IsSafeFolder(destinationFolder)) return Status::InvalidArgument;
		const std::optional<AnimationAssetKind> kind = KindForExtension(source);
		if(!kind) return Status::InvalidArgument;
		AnimationAssetEntry entry;
		entry.id = nextId_++;
		entry.kind = *kind;
		entry.name = source.stem().string();
		entry.folder = std::move(destinationFolder);
		entry.sourcePath = MakeUniqueSourcePath(entry);
		std::vector<std::uint8_t> candidateBytes;
		if(!ReadCandidateBytes(source, candidateBytes)
			|| ValidateTypedDocument(*kind, candidateBytes) != AnimationStatus::Ok) {
			lastDiagnostic_ = "Import rejected the complete source before publication; no destination bytes changed.";
			return Status::ValidationFailed;
		}
		if(const Status published = WriteBytesAtomically(entry.sourcePath, candidateBytes);
			published != Status::Ok) return published;
		assets_.emplace(entry.id, entry);
		CommitInverse(std::nullopt, entry);
		out = entry;
		lastDiagnostic_ = "Validated and atomically imported the complete animation document.";
		return Status::Ok;
	}

	Status AnimationAssetActions::ExtractReadOnlyPackageChild(
		AnimationAssetKind kind, const std::filesystem::path& generatedSource,
		const std::filesystem::path& generatedRoot, std::string name,
		std::string destinationFolder, AnimationAssetEntry& out) noexcept {
		// Only package-owned payloads with a source-independent runtime format are
		// extractable. Other animation types are authored as loose files already.
		if(assetRoot_.empty() || (kind != AnimationAssetKind::Skeleton
			&& kind != AnimationAssetKind::Clip) || !IsValidName(name)
			|| !IsSafeFolder(destinationFolder) || generatedRoot.empty()
			|| !std::filesystem::is_regular_file(generatedSource)
			|| !IsContainedPath(generatedSource, generatedRoot)
			|| KindForExtension(generatedSource) != kind) {
			lastDiagnostic_ = "Editable-copy extraction rejected an unsafe or unsupported package child.";
			return Status::InvalidArgument;
		}

		AnimationAssetEntry entry;
		entry.id = nextId_++;
		entry.kind = kind;
		entry.name = std::move(name);
		entry.folder = std::move(destinationFolder);
		entry.sourcePath = MakeUniqueSourcePath(entry);
		// Check the yet-to-be-published output path before creating parent folders.
		// weakly_canonical resolves existing symlink ancestors, so an editable copy
		// cannot be staged outside Assets through a pre-existing linked directory.
		if(!IsContainedPath(entry.sourcePath, assetRoot_)) {
			lastDiagnostic_ = "Editable-copy extraction rejected a destination outside Assets.";
			return Status::InvalidArgument;
		}

		std::vector<std::uint8_t> sourceBytes;
		if(!ReadCandidateBytes(generatedSource, sourceBytes)
			|| ValidateTypedDocument(kind, sourceBytes) != AnimationStatus::Ok) {
			lastDiagnostic_ = "The generated package child did not pass production decoding; no editable copy was created.";
			return Status::ValidationFailed;
		}
		std::vector<std::uint8_t> editableBytes;
		if(CloneDocumentWithNewIdentity(kind, sourceBytes, editableBytes)
				!= AnimationStatus::Ok
			|| ValidateTypedDocument(kind, editableBytes) != AnimationStatus::Ok) {
			lastDiagnostic_ = "The editable copy could not be reserialized and was not published.";
			return Status::ValidationFailed;
		}
		if(const Status published = WriteBytesAtomically(entry.sourcePath, editableBytes);
			published != Status::Ok) {
			lastDiagnostic_ = "The editable copy could not be atomically published without overwriting another asset.";
			return published;
		}
		assets_.emplace(entry.id, entry);
		CommitInverse(std::nullopt, entry);
		out = entry;
		lastDiagnostic_ = "Created an editable loose copy with a new asset identity; the read-only package child is unchanged.";
		return Status::Ok;
	}

	Status AnimationAssetActions::AdoptExisting(
		const std::filesystem::path& source,
		AnimationAssetEntry& out) noexcept {
		if(assetRoot_.empty()) return Status::InvalidArgument;
		const std::optional<AnimationAssetKind> kind = KindForExtension(source);
		std::vector<std::uint8_t> bytes;
		if(!kind || !ReadCandidateBytes(source, bytes)
			|| ValidateTypedDocument(*kind, bytes) != AnimationStatus::Ok) {
			lastDiagnostic_ = "Existing animation document failed checked decoding/compilation and was not adopted.";
			return Status::ValidationFailed;
		}
		std::error_code error;
		const std::filesystem::path relative = std::filesystem::relative(
			std::filesystem::absolute(source), std::filesystem::absolute(assetRoot_), error);
		if(error || relative.empty() || std::any_of(relative.begin(), relative.end(), [](const auto& segment) {
			return segment == "..";
		})) return Status::InvalidArgument;
		AnimationAssetEntry entry;
		entry.id = nextId_++;
		entry.kind = *kind;
		entry.name = source.stem().string();
		entry.folder = relative.parent_path().generic_string();
		entry.sourcePath = source.lexically_normal();
		entry.validation = AnimationAssetValidationState::Valid;
		entry.valid = true;
		entry.diagnostic = "Adopted after checked decoding and compilation.";
		assets_.emplace(entry.id, entry);
		out = entry;
		lastDiagnostic_ = entry.diagnostic;
		return Status::Ok;
	}

	Status AnimationAssetActions::Duplicate(std::uint64_t sourceId, std::string name,
		AnimationAssetEntry& out) noexcept {
		const auto found = assets_.find(sourceId);
		if(found == assets_.end() || !IsValidName(name)) return Status::InvalidArgument;
		AnimationAssetEntry copy = found->second;
		copy.id = nextId_++;
		copy.name = std::move(name);
		copy.generation = 1U;
		copy.validation = AnimationAssetValidationState::NotValidated;
		copy.valid = false;
		copy.diagnostic.clear();
		copy.sourcePath = MakeUniqueSourcePath(copy);
		if(const Status copied = CopyDocument(found->second, copy); copied != Status::Ok) return copied;
		assets_.emplace(copy.id, copy);
		CommitInverse(std::nullopt, copy);
		out = copy;
		return Status::Ok;
	}

	Status AnimationAssetActions::Rename(std::uint64_t id, std::string name) noexcept {
		auto found = assets_.find(id);
		if(found == assets_.end() || !IsValidName(name)) return Status::InvalidArgument;
		const AnimationAssetEntry before = found->second;
		AnimationAssetEntry after = before;
		after.name = std::move(name);
		after.sourcePath = MakeUniqueSourcePath(after);
		++after.generation;
		if(const Status moved = MoveDocument(before, after); moved != Status::Ok) return moved;
		found->second = after;
		CommitInverse(before, after);
		return Status::Ok;
	}

	Status AnimationAssetActions::Move(std::uint64_t id, std::string folder) noexcept {
		auto found = assets_.find(id);
		if(found == assets_.end() || !IsSafeFolder(folder)) return Status::InvalidArgument;
		const AnimationAssetEntry before = found->second;
		AnimationAssetEntry after = before;
		after.folder = std::move(folder);
		after.sourcePath = MakeUniqueSourcePath(after);
		++after.generation;
		if(const Status moved = MoveDocument(before, after); moved != Status::Ok) return moved;
		found->second = after;
		CommitInverse(before, after);
		return Status::Ok;
	}

	Status AnimationAssetActions::Delete(std::uint64_t id) noexcept {
		const auto found = assets_.find(id);
		if(found == assets_.end()) return Status::InvalidArgument;
		const AnimationAssetEntry before = found->second;
		std::string deletedSourceBytes;
		if(!assetRoot_.empty() && !before.sourcePath.empty()) {
			{
				std::ifstream source(before.sourcePath, std::ios::binary | std::ios::ate);
				if(!source) return Status::Rejected;
				const std::streamoff size = source.tellg();
				if(size < 0 || size > static_cast<std::streamoff>(kMaximumLifecycleDocumentBytes)) {
					return Status::CapacityExceeded;
				}
				deletedSourceBytes.resize(static_cast<std::size_t>(size));
				source.seekg(0, std::ios::beg);
				if(!deletedSourceBytes.empty() && !source.read(deletedSourceBytes.data(), size)) {
					return Status::Rejected;
				}
			}
			// Release the reader before removing the source. Windows does not grant delete
			// sharing to this stream, so keeping it alive would reject every durable delete.
			std::error_code error;
			const bool removed = std::filesystem::remove(before.sourcePath, error);
			if(error || !removed) return Status::Rejected;
		}
		assets_.erase(found);
		validationRequests_.erase(id);
		undo_.push_back({before, std::nullopt, std::move(deletedSourceBytes)});
		redo_.clear();
		return Status::Ok;
	}

	const AnimationAssetEntry* AnimationAssetActions::Inspect(std::uint64_t id) const noexcept {
		const auto found = assets_.find(id);
		return found == assets_.end() ? nullptr : &found->second;
	}

	const AnimationAssetEntry* AnimationAssetActions::InspectByPath(
		const std::filesystem::path& source) const noexcept {
		const std::filesystem::path normalized = std::filesystem::absolute(source).lexically_normal();
		const auto found = std::find_if(assets_.begin(), assets_.end(), [&normalized](const auto& pair) {
			return std::filesystem::absolute(pair.second.sourcePath).lexically_normal() == normalized;
		});
		return found == assets_.end() ? nullptr : &found->second;
	}

	Status AnimationAssetActions::EnsureTracked(
		const std::filesystem::path& source,
		AnimationAssetEntry& out) noexcept {
		if(const AnimationAssetEntry* existing = InspectByPath(source)) {
			out = *existing;
			return Status::Ok;
		}
		return AdoptExisting(source, out);
	}

	Status AnimationAssetActions::BeginValidation(std::uint64_t id,
		AnimationAssetValidationRequest& outRequest) noexcept {
		auto found = assets_.find(id);
		if(found == assets_.end()) return Status::InvalidArgument;
		AnimationAssetEntry& entry = found->second;
		entry.validation = AnimationAssetValidationState::Pending;
		entry.diagnostic.clear();
		const std::uint64_t requestId = nextValidationRequestId_++;
		validationRequests_[id] = requestId;
		outRequest = {id, entry.generation, requestId, entry.sourcePath, entry.kind};
		return Status::Ok;
	}

	AnimationAssetValidationResult AnimationAssetActions::ValidateCandidate(
		const AnimationAssetValidationRequest& request) noexcept {
		AnimationAssetValidationResult result{request.assetId, request.expectedGeneration,
			request.requestId, false, {}};
		std::vector<std::uint8_t> bytes;
		if(!ReadCandidateBytes(request.sourcePath, bytes)) {
			result.diagnostic = "The complete validation candidate could not be read.";
			return result;
		}
		const AnimationStatus status = ValidateTypedDocument(request.kind, bytes);
		result.succeeded = status == AnimationStatus::Ok;
		result.diagnostic = result.succeeded ? "Checked decoding and compilation succeeded."
			: "Checked decoding or compilation failed: " + std::string(ToString(status)) + ".";
		return result;
	}

	Status AnimationAssetActions::CompleteValidation(const AnimationAssetValidationResult& result) noexcept {
		auto found = assets_.find(result.assetId);
		const auto active = validationRequests_.find(result.assetId);
		if(found == assets_.end() || active == validationRequests_.end()
			|| active->second != result.requestId
			|| found->second.generation != result.expectedGeneration) return Status::Stale;
		AnimationAssetEntry before = found->second;
		AnimationAssetEntry after = before;
		after.validation = result.succeeded ? AnimationAssetValidationState::Valid
			: AnimationAssetValidationState::Invalid;
		after.valid = result.succeeded;
		after.diagnostic = result.diagnostic;
		if(result.succeeded) ++after.generation;
		// A failing worker result updates diagnostics only. It deliberately does not
		// publish bytes or discard the last known good source document.
		found->second = after;
		validationRequests_.erase(active);
		if(result.succeeded) CommitInverse(before, after);
		lastDiagnostic_ = after.diagnostic;
		return result.succeeded ? Status::Ok : Status::ValidationFailed;
	}

	Status AnimationAssetActions::CancelValidation(const AnimationAssetValidationRequest& request) noexcept {
		auto found = assets_.find(request.assetId);
		const auto active = validationRequests_.find(request.assetId);
		if(found == assets_.end() || active == validationRequests_.end()
			|| active->second != request.requestId) return Status::Stale;
		found->second.validation = AnimationAssetValidationState::Canceled;
		found->second.diagnostic = "Validation was canceled before publication.";
		validationRequests_.erase(active);
		return Status::Canceled;
	}

	Status AnimationAssetActions::Undo() noexcept {
		if(undo_.empty()) return Status::Rejected;
		const HistoryEntry entry = undo_.back();
		if(const Status status = ApplyHistory(entry, false); status != Status::Ok) return status;
		undo_.pop_back();
		redo_.push_back(entry);
		return Status::Ok;
	}

	Status AnimationAssetActions::Redo() noexcept {
		if(redo_.empty()) return Status::Rejected;
		const HistoryEntry entry = redo_.back();
		if(const Status status = ApplyHistory(entry, true); status != Status::Ok) return status;
		redo_.pop_back();
		undo_.push_back(entry);
		return Status::Ok;
	}

	std::vector<AnimationAssetKind> AnimationAssetActions::SupportedKinds() {
		return {AnimationAssetKind::Skeleton, AnimationAssetKind::Clip,
			AnimationAssetKind::Controller, AnimationAssetKind::Mask,
			AnimationAssetKind::RetargetProfile, AnimationAssetKind::MotionDatabase,
			AnimationAssetKind::RagdollProfile};
	}

	const char* AnimationAssetActions::DisplayName(AnimationAssetKind kind) noexcept {
		switch(kind) {
		case AnimationAssetKind::Skeleton: return "Skeleton";
		case AnimationAssetKind::Clip: return "Animation Clip";
		case AnimationAssetKind::Controller: return "Animation Controller";
		case AnimationAssetKind::Mask: return "Animation Mask";
		case AnimationAssetKind::RetargetProfile: return "Retarget Profile";
		case AnimationAssetKind::MotionDatabase: return "Motion Database";
		case AnimationAssetKind::RagdollProfile: return "Ragdoll Profile";
		}
		return "Animation Asset";
	}

	const char* AnimationAssetActions::Extension(AnimationAssetKind kind) noexcept {
		switch(kind) {
		case AnimationAssetKind::Skeleton: return ".teskel";
		case AnimationAssetKind::Clip: return ".teanim";
		case AnimationAssetKind::Controller: return ".teanimctrl";
		case AnimationAssetKind::Mask: return ".teanimmask";
		case AnimationAssetKind::RetargetProfile: return ".teretarget";
		case AnimationAssetKind::MotionDatabase: return ".temotiondb";
		case AnimationAssetKind::RagdollProfile: return ".teragdoll";
		}
		return "";
	}

	std::optional<AnimationAssetKind> AnimationAssetActions::KindForPath(
		const std::filesystem::path& path) noexcept {
		return KindForExtension(path);
	}

	bool AnimationAssetActions::IsValidName(const std::string& value) const noexcept {
		return !value.empty() && value.find_first_of("\\/:*?\"<>|") == std::string::npos
			&& value != "." && value != "..";
	}

	bool AnimationAssetActions::IsSafeFolder(const std::string& value) const noexcept {
		const std::filesystem::path folder(value);
		return !folder.is_absolute() && !folder.empty()
			&& std::none_of(folder.begin(), folder.end(), [](const auto& segment) {
				return segment == "..";
			});
	}

	std::filesystem::path AnimationAssetActions::BuildSourcePath(const AnimationAssetEntry& entry) const {
		return assetRoot_.empty() ? std::filesystem::path{}
			: (assetRoot_ / entry.folder / (entry.name + Extension(entry.kind))).lexically_normal();
	}

	std::filesystem::path AnimationAssetActions::MakeUniqueSourcePath(const AnimationAssetEntry& entry) const {
		std::filesystem::path candidate = BuildSourcePath(entry);
		if(candidate.empty()) return candidate;
		for(std::uint32_t suffix = 1U; std::filesystem::exists(candidate); ++suffix) {
			candidate = BuildSourcePath(entry);
			candidate.replace_filename(entry.name + " (" + std::to_string(suffix) + ")" + Extension(entry.kind));
		}
		return candidate;
	}

	Status AnimationAssetActions::WriteCreatedDocument(AnimationAssetEntry& entry) noexcept {
		if(assetRoot_.empty()) return Status::Ok;
		// Creation publishes a real starter document through the production codec.
		std::vector<std::uint8_t> bytes;
		const TerraEngine::Animation::AnimationStatus created = BuildStarterBytes(entry.kind, bytes);
		if(created != TerraEngine::Animation::AnimationStatus::Ok || bytes.empty()) {
			return Status::ValidationFailed;
		}
		return WriteBytesAtomically(entry.sourcePath, bytes);
	}

	Status AnimationAssetActions::CopyDocument(const AnimationAssetEntry& source,
		AnimationAssetEntry& destination) noexcept {
		if(assetRoot_.empty()) return Status::Ok;
		if(source.sourcePath.empty() || !std::filesystem::is_regular_file(source.sourcePath)) return Status::Rejected;
		std::vector<std::uint8_t> bytes;
		if(!ReadCandidateBytes(source.sourcePath, bytes)
			|| ValidateTypedDocument(source.kind, bytes) != AnimationStatus::Ok) {
			return Status::ValidationFailed;
		}
		std::vector<std::uint8_t> clonedBytes;
		if(CloneDocumentWithNewIdentity(source.kind, bytes, clonedBytes) != AnimationStatus::Ok
			|| ValidateTypedDocument(source.kind, clonedBytes) != AnimationStatus::Ok) {
			return Status::ValidationFailed;
		}
		return WriteBytesAtomically(destination.sourcePath, clonedBytes);
	}

	Status AnimationAssetActions::MoveDocument(const AnimationAssetEntry& before,
		AnimationAssetEntry& after) noexcept {
		if(assetRoot_.empty() || before.sourcePath == after.sourcePath) return Status::Ok;
		if(before.sourcePath.empty() || !std::filesystem::is_regular_file(before.sourcePath)) return Status::Rejected;
		std::error_code error;
		std::filesystem::create_directories(after.sourcePath.parent_path(), error);
		if(error) return Status::Rejected;
		std::filesystem::rename(before.sourcePath, after.sourcePath, error);
		return error ? Status::Rejected : Status::Ok;
	}

	void AnimationAssetActions::CommitInverse(std::optional<AnimationAssetEntry> before,
		std::optional<AnimationAssetEntry> after) noexcept {
		std::string publishedBytes;
		if(!before && after && !after->sourcePath.empty()) {
			// Creation/duplication redo must restore the exact durable GUID and authored
			// bytes, never invoke a starter factory that would create a different asset.
			std::ifstream source(after->sourcePath, std::ios::binary | std::ios::ate);
			if(source) {
				const std::streamoff size = source.tellg();
				if(size >= 0 && size <= static_cast<std::streamoff>(kMaximumLifecycleDocumentBytes)) {
					publishedBytes.resize(static_cast<std::size_t>(size));
					source.seekg(0, std::ios::beg);
					if(!publishedBytes.empty() && !source.read(publishedBytes.data(), size)) publishedBytes.clear();
				}
			}
		}
		undo_.push_back({std::move(before), std::move(after), std::move(publishedBytes)});
		redo_.clear();
	}

	Status AnimationAssetActions::ApplyHistory(const HistoryEntry& entry, bool applyAfter) noexcept {
		const std::optional<AnimationAssetEntry>& target = applyAfter ? entry.after : entry.before;
		const std::optional<AnimationAssetEntry>& replaced = applyAfter ? entry.before : entry.after;
		if(replaced && target && replaced->sourcePath != target->sourcePath) {
			AnimationAssetEntry ignored = *target;
			if(const Status moved = MoveDocument(*replaced, ignored); moved != Status::Ok) return moved;
		}
		else if(replaced && !target && !assetRoot_.empty() && !replaced->sourcePath.empty()) {
			std::error_code error;
			const bool removed = std::filesystem::remove(replaced->sourcePath, error);
			// A missing/replaced source is a history conflict. Keep the stack and catalog
			// untouched so the caller can resolve it without a partial logical replay.
			if(error || !removed) return Status::Rejected;
		}
		else if(!replaced && target && !assetRoot_.empty()) {
			if(entry.deletedSourceBytes.empty()) {
				AnimationAssetEntry recreated = *target;
				if(const Status created = WriteCreatedDocument(recreated); created != Status::Ok) return created;
				if(target) assets_[target->id] = recreated;
				return Status::Ok;
			}
			const auto restoredBytes = std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t*>(entry.deletedSourceBytes.data()),
				entry.deletedSourceBytes.size());
			if(const Status restored = WriteBytesAtomically(target->sourcePath, restoredBytes);
				restored != Status::Ok) return restored;
		}
		if(target) assets_[target->id] = *target;
		else if(replaced) assets_.erase(replaced->id);
		return Status::Ok;
	}
}
