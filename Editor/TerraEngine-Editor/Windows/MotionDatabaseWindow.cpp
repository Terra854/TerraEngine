// MotionDatabaseWindow.cpp - persisted motion-database authoring and search preview.

#include "MotionDatabaseWindow.h"

#include "AnimationAssetWindowSupport.h"
#include "Engine/Animation/Assets/MotionDatabaseAsset.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <imgui.h>
#include <optional>
#include <string>
#include <utility>

namespace {
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::AssetPipeline;

	std::filesystem::path AuthoringPathFor(const std::filesystem::path& cookedPath) {
		return cookedPath.string() + ".authoring.json";
	}

	const MotionDatabaseFeatureBlock& FeatureBlockForLane(std::size_t lane) {
		const auto found = std::find_if(kMotionDatabaseDefaultFeatureBlocks.begin(),
			kMotionDatabaseDefaultFeatureBlocks.end(), [lane](const auto& block) {
				return lane >= block.firstFeature
					&& lane < static_cast<std::size_t>(block.firstFeature + block.featureCount);
			});
		return found == kMotionDatabaseDefaultFeatureBlocks.end()
			? kMotionDatabaseDefaultFeatureBlocks.front() : *found;
	}

	void CopyPathToBuffer(std::string_view path, std::array<char, 1024U>& buffer) {
		buffer.fill('\0');
		const std::size_t count = std::min(path.size(), buffer.size() - 1U);
		std::memcpy(buffer.data(), path.data(), count);
	}
}

struct MotionDatabaseWindow::State {
	TerraEngine::Editor::Animation::CookedAnimationAssetSelection<MotionDatabaseAsset> selection;
	MotionDatabaseAuthoringDocument document;
	MotionDatabaseAuthoringHistory history;
	std::optional<MotionDatabaseAuthoringSnapshot> persistedSnapshot;
	// This extra value makes the UI's ownership boundary explicit. The resolver
	// and session receive copies of it, so later widgets can only mutate document.
	std::optional<MotionDatabaseAuthoringDocument> inFlightBuildDocument;
	std::filesystem::path observedCookedPath;
	std::filesystem::path authoringPath;
	std::unique_ptr<MotionDatabaseAuthoringSession> session;
	std::unique_ptr<TerraEngine::Editor::Animation::MotionDatabaseProductionAuthoringHost> defaultHost;
	std::optional<MotionDatabaseQueryPreviewState> queryPreview;
	MotionMatchQuery query;
	std::array<char, 1024U> sourcePathBuffer{};
	int selectedSource = -1;
	int selectedCandidate = 0;
	bool documentAvailable = false;
	bool dirty = false;
	std::string message;
};

MotionDatabaseWindow::MotionDatabaseWindow(MainEditorSubsystem* owner)
	: EditorWindow("Motion Database", false, "Animation/Authoring"),
	  owner_(owner),
	  state_(std::make_unique<State>()) {
	state_->defaultHost = std::make_unique<
		TerraEngine::Editor::Animation::MotionDatabaseProductionAuthoringHost>();
	SetAuthoringHost(state_->defaultHost.get());
}

MotionDatabaseWindow::~MotionDatabaseWindow() = default;

void MotionDatabaseWindow::SetAuthoringHost(
	TerraEngine::Editor::Animation::MotionDatabaseAuthoringHost* host) {
	authoringHost_ = host != nullptr ? host : state_->defaultHost.get();
	state_->session.reset();
	if(authoringHost_ != nullptr) {
		state_->session = std::make_unique<MotionDatabaseAuthoringSession>(
			authoringHost_->GetMotionDatabaseCompileExecutor());
	}
}

void MotionDatabaseWindow::SetRuntimeQueryPreview(MotionDatabaseQueryPreviewState snapshot) {
	state_->queryPreview = std::move(snapshot);
}

void MotionDatabaseWindow::OnGui() {
	state_->selection.Synchronize(owner_, ".temotiondb", &DeserializeMotionDatabaseAsset);
	const MotionDatabaseAsset* database = state_->selection.Get();
	if(!state_->selection.IsSelectedForWindow()) {
		ImGui::TextWrapped("Select a .temotiondb asset to edit its persisted sources, tags, contacts, feature costs, build it on a worker, and inspect real candidate scores.");
		return;
	}

	ImGui::TextWrapped("%s", state_->selection.Path().string().c_str());
	if(database == nullptr) {
		ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
			state_->selection.Message().c_str());
		return;
	}
	if(state_->selection.IsStale()) {
		ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.2F, 1.0F),
			"The latest cooked file was rejected; showing the prior complete decoded copy.");
	}

	// Selection changes are the safe point for swapping the detached authoring
	// model. A malformed sidecar never mutates the decoded last-known-good asset.
	if(state_->observedCookedPath != state_->selection.Path()) {
		state_->observedCookedPath = state_->selection.Path();
		state_->authoringPath = AuthoringPathFor(state_->observedCookedPath);
		state_->document = {};
		state_->persistedSnapshot.reset();
		state_->inFlightBuildDocument.reset();
		state_->queryPreview.reset();
		state_->selectedSource = -1;
		state_->dirty = false;
		MotionDatabaseAuthoringSnapshot loadedSnapshot;
		const AnimationStatus loaded = LoadMotionDatabaseAuthoringSnapshot(
			state_->authoringPath, loadedSnapshot);
		if(loaded == AnimationStatus::MissingAsset) {
			state_->document.assetId = database->assetId;
			state_->document.compatibilityFingerprint = database->compatibilityFingerprint;
			state_->document.samplesPerBlock = database->storageLayout.samplesPerBlock;
			state_->document.featureWeights.fill(1U);
			state_->documentAvailable = true;
			state_->dirty = true;
			state_->history.Reset(state_->document);
			state_->message = "No authoring sidecar exists yet. Save to create one without changing the cooked asset.";
		} else {
			if(loaded == AnimationStatus::Ok) {
				state_->document = loadedSnapshot.document;
				state_->persistedSnapshot = std::move(loadedSnapshot);
			}
			state_->documentAvailable = loaded == AnimationStatus::Ok
				&& state_->document.assetId == database->assetId;
			if(state_->documentAvailable) state_->history.Reset(state_->document);
			state_->message = state_->documentAvailable
				? "Loaded persisted motion-database authoring metadata."
				: "The authoring sidecar is invalid or belongs to another asset; the cooked asset was preserved.";
		}
	}

	const AnimationStatus validation = ValidateMotionDatabaseAsset(*database);
	ImGui::Text("Asset: %s", TerraEngine::ToHexString(database->assetId.value).c_str());
	ImGui::Text("Cooked generation: %u   samples: %zu", database->runtimeGeneration,
		database->samples.size());
	ImGui::Text("Content fingerprint: %llu   validation: %s",
		static_cast<unsigned long long>(ComputeMotionDatabaseContentFingerprint(*database)),
		std::string(ToString(validation)).c_str());
	if(!state_->message.empty()) ImGui::TextWrapped("%s", state_->message.c_str());

	if(!state_->documentAvailable) {
		if(ImGui::Button("Start a fresh authoring document")) {
			state_->document = {};
			state_->document.assetId = database->assetId;
			state_->document.compatibilityFingerprint = database->compatibilityFingerprint;
			state_->document.samplesPerBlock = database->storageLayout.samplesPerBlock;
			state_->document.featureWeights.fill(1U);
			state_->documentAvailable = true;
			state_->dirty = true;
			state_->persistedSnapshot.reset();
			state_->history.Reset(state_->document);
			state_->message = "Created an unsaved fresh document. An existing rejected sidecar remains protected by exact-byte compare-and-swap.";
		}
		return;
	}
	auto markModified = [this]() {
		// Widgets mutate the detached candidate first. The history baseline comes
		// from its immutable committed copy, so the first field edit still records
		// the complete pre-edit document and later fields join the same group.
		if(!state_->history.IsEditActive()) {
			const AnimationStatus begun = state_->history.BeginEdit(state_->history.Current());
			if(begun != AnimationStatus::Ok) {
				// The widget already wrote into the detached candidate. Roll it back if
				// the history baseline could not be captured so no untracked edit survives.
				state_->document = state_->history.Current();
				state_->selectedSource = -1;
				state_->message = "Could not begin an authoring edit group ("
					+ std::string(ToString(begun)) + ").";
				return;
			}
		}
		state_->dirty = true;
		state_->queryPreview.reset();
	};
	auto saveGroupedEdit = [this]() {
		MotionDatabaseAuthoringHistory stagedHistory = state_->history;
		MotionDatabaseAuthoringDocument candidate = state_->document;
		if(stagedHistory.IsEditActive()) {
			const AnimationStatus committed = stagedHistory.CommitEdit(candidate);
			if(committed == AnimationStatus::NotReady) {
				candidate = stagedHistory.Current();
				if(state_->persistedSnapshot) {
					state_->history = std::move(stagedHistory);
					state_->document = state_->history.Current();
					state_->dirty = false;
					return committed;
				}
			}
			if(committed != AnimationStatus::Ok
				&& committed != AnimationStatus::NotReady) return committed;
		} else if(state_->persistedSnapshot) {
			return AnimationStatus::NotReady;
		}

		MotionDatabaseAuthoringSnapshot publishedSnapshot;
		const AnimationStatus saved = SaveMotionDatabaseAuthoringDocumentAtomically(
			state_->authoringPath, candidate,
			state_->persistedSnapshot ? &*state_->persistedSnapshot : nullptr,
			&publishedSnapshot);
		if(saved != AnimationStatus::Ok) return saved;

		// Install history and the CAS token only after the bytes are durable. A
		// stale external writer therefore cannot consume the local undo transaction.
		state_->history = std::move(stagedHistory);
		state_->document = std::move(candidate);
		state_->persistedSnapshot = std::move(publishedSnapshot);
		state_->dirty = false;
		state_->queryPreview.reset();
		if(state_->session) state_->session->CancelBuild();
		state_->inFlightBuildDocument.reset();
		return AnimationStatus::Ok;
	};
	auto persistHistoryRestore = [this](bool undo) {
		if(!state_->persistedSnapshot || state_->history.IsEditActive()) {
			return AnimationStatus::NotReady;
		}
		MotionDatabaseAuthoringHistory stagedHistory = state_->history;
		MotionDatabaseAuthoringDocument restored = state_->document;
		const AnimationStatus prepared = undo
			? stagedHistory.Undo(restored) : stagedHistory.Redo(restored);
		if(prepared != AnimationStatus::Ok) return prepared;
		MotionDatabaseAuthoringSnapshot publishedSnapshot;
		const AnimationStatus saved = SaveMotionDatabaseAuthoringDocumentAtomically(
			state_->authoringPath, restored, &*state_->persistedSnapshot,
			&publishedSnapshot);
		if(saved != AnimationStatus::Ok) return saved;
		state_->history = std::move(stagedHistory);
		state_->document = std::move(restored);
		state_->persistedSnapshot = std::move(publishedSnapshot);
		state_->dirty = false;
		state_->selectedSource = -1;
		state_->queryPreview.reset();
		if(state_->session) state_->session->CancelBuild();
		state_->inFlightBuildDocument.reset();
		return AnimationStatus::Ok;
	};

	if(!ImGui::BeginTabBar("MotionDatabaseTabs")) return;
	if(ImGui::BeginTabItem("Sources / Build")) {
		ImGui::Text("Authoring revision: %llu%s",
			static_cast<unsigned long long>(state_->document.authoringGeneration),
			state_->dirty ? " (unsaved)" : "");
		ImGui::Text("Rig compatibility: %llu",
			static_cast<unsigned long long>(state_->document.compatibilityFingerprint));
		if(ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Every source clip in this database must use this skeleton compatibility identity. Use 'Adopt selected clip rig' after choosing the first source; no manual fingerprint entry is required.");
		}
		if(ImGui::Button("Add source")) {
			MotionDatabaseAuthoringSource source;
			source.sourcePath = "Assets/AnimationClip.teanim";
			state_->document.sources.push_back(std::move(source));
			state_->selectedSource = static_cast<int>(state_->document.sources.size() - 1U);
			CopyPathToBuffer(state_->document.sources.back().sourcePath, state_->sourcePathBuffer);
			markModified();
		}
		ImGui::SameLine();
		if(ImGui::Button("Remove selected") && state_->selectedSource >= 0
			&& static_cast<std::size_t>(state_->selectedSource) < state_->document.sources.size()) {
			state_->document.sources.erase(state_->document.sources.begin() + state_->selectedSource);
			state_->selectedSource = -1;
			markModified();
		}
		if(ImGui::BeginTable("AuthoredSources", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
			for(const char* heading : {"Use", "Clip ID", "Fingerprint", "Path", "Annotations"}) ImGui::TableSetupColumn(heading);
			ImGui::TableHeadersRow();
			for(std::size_t index = 0U; index < state_->document.sources.size(); ++index) {
				auto& source = state_->document.sources[index];
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); if(ImGui::Checkbox("##enabled", &source.enabled)) markModified();
				ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(source.sourceClipId));
				ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(source.sourceClipFingerprint));
				ImGui::TableNextColumn();
				if(ImGui::Selectable(source.sourcePath.c_str(), state_->selectedSource == static_cast<int>(index))) {
					state_->selectedSource = static_cast<int>(index);
					CopyPathToBuffer(source.sourcePath, state_->sourcePathBuffer);
				}
				ImGui::TableNextColumn(); ImGui::Text("%zu tags / %zu contacts / %zu exclusions",
					source.tagRanges.size(), source.contactRanges.size(), source.exclusionRanges.size());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		if(state_->selectedSource >= 0
			&& static_cast<std::size_t>(state_->selectedSource) < state_->document.sources.size()) {
			auto& source = state_->document.sources[static_cast<std::size_t>(state_->selectedSource)];
			ImGui::SeparatorText("Selected source metadata");
			bool changed = false;
			changed |= ImGui::InputScalar("Clip ID", ImGuiDataType_U64, &source.sourceClipId);
			changed |= ImGui::InputScalar("Clip fingerprint", ImGuiDataType_U64, &source.sourceClipFingerprint);
			if(ImGui::InputText("Source path", state_->sourcePathBuffer.data(), state_->sourcePathBuffer.size())) {
				source.sourcePath = state_->sourcePathBuffer.data();
				changed = true;
			}
			changed |= ImGui::InputScalar("Base tag mask", ImGuiDataType_U64, &source.baseTagMask,
				nullptr, nullptr, "0x%016llx", ImGuiInputTextFlags_CharsHexadecimal);
			changed |= ImGui::InputScalar("Base contact mask", ImGuiDataType_U32, &source.baseContactMask,
				nullptr, nullptr, "0x%08x", ImGuiInputTextFlags_CharsHexadecimal);
			if(changed) markModified();

			if(ImGui::Button("Add tag range")) { source.tagRanges.push_back({0, 0, 1U}); markModified(); }
			ImGui::SameLine();
			if(ImGui::Button("Add contact range")) { source.contactRanges.push_back({0, 0, 1U}); markModified(); }
			ImGui::SameLine();
			if(ImGui::Button("Add exclusion range")) { source.exclusionRanges.push_back({0, 0, 1U}); markModified(); }
			if(ImGui::Button("Remove last tag") && !source.tagRanges.empty()) { source.tagRanges.pop_back(); markModified(); }
			ImGui::SameLine();
			if(ImGui::Button("Remove last contact") && !source.contactRanges.empty()) { source.contactRanges.pop_back(); markModified(); }
			ImGui::SameLine();
			if(ImGui::Button("Remove last exclusion") && !source.exclusionRanges.empty()) { source.exclusionRanges.pop_back(); markModified(); }
			for(std::size_t index = 0U; index < source.tagRanges.size(); ++index) {
				auto& range = source.tagRanges[index];
				ImGui::PushID(static_cast<int>(index));
				bool rangeChanged = ImGui::InputScalar("Tag first", ImGuiDataType_S64, &range.firstTick);
				ImGui::SameLine(); rangeChanged |= ImGui::InputScalar("Tag last", ImGuiDataType_S64, &range.lastTick);
				ImGui::SameLine(); rangeChanged |= ImGui::InputScalar("Tag mask", ImGuiDataType_U64, &range.tagMask);
				if(rangeChanged) markModified();
				ImGui::PopID();
			}
			for(std::size_t index = 0U; index < source.contactRanges.size(); ++index) {
				auto& range = source.contactRanges[index];
				ImGui::PushID(static_cast<int>(10000U + index));
				bool rangeChanged = ImGui::InputScalar("Contact first", ImGuiDataType_S64, &range.firstTick);
				ImGui::SameLine(); rangeChanged |= ImGui::InputScalar("Contact last", ImGuiDataType_S64, &range.lastTick);
				ImGui::SameLine(); rangeChanged |= ImGui::InputScalar("Contact mask", ImGuiDataType_U32, &range.contactMask);
				if(rangeChanged) markModified();
				ImGui::PopID();
			}
			for(std::size_t index = 0U; index < source.exclusionRanges.size(); ++index) {
				auto& range = source.exclusionRanges[index];
				ImGui::PushID(static_cast<int>(20000U + index));
				bool rangeChanged = ImGui::InputScalar("Exclusion first", ImGuiDataType_S64, &range.firstTick);
				ImGui::SameLine(); rangeChanged |= ImGui::InputScalar("Exclusion last", ImGuiDataType_S64, &range.lastTick);
				ImGui::SameLine(); rangeChanged |= ImGui::InputScalar("Flags", ImGuiDataType_U32, &range.exclusionFlags);
				if(rangeChanged) markModified();
				ImGui::PopID();
			}
		}

		if(ImGui::Button("Adopt selected clip rig")) {
			AnimationStatus status = authoringHost_ != nullptr
				? AnimationStatus::Ok : AnimationStatus::NotReady;
			MotionDatabaseAuthoringDocument resolved = state_->document;
			// Compatibility is part of the document identity and cannot change after
			// a sidecar has been published. A brand-new unsaved document may adopt
			// its first source rig, then becomes the new history baseline.
			if(status == AnimationStatus::Ok && state_->persistedSnapshot) {
				status = AnimationStatus::Rejected;
			} else if(status == AnimationStatus::Ok && state_->selectedSource >= 0
				&& static_cast<std::size_t>(state_->selectedSource) < resolved.sources.size()) {
				AnimationCompatibilityFingerprint compatibility = 0U;
				status = authoringHost_->ReadMotionDatabaseSourceCompatibility(
					resolved.sources[static_cast<std::size_t>(state_->selectedSource)],
					compatibility);
				if(status == AnimationStatus::Ok) {
					resolved.compatibilityFingerprint = compatibility;
					for(auto& source : resolved.sources) {
						if(!source.enabled) continue;
						status = authoringHost_->ResolveMotionDatabaseSourceIdentity(
							source, compatibility);
						if(status != AnimationStatus::Ok) break;
					}
				}
			} else if(status == AnimationStatus::Ok) {
				status = AnimationStatus::InvalidArgument;
			}
			if(status == AnimationStatus::Ok) {
				state_->document = std::move(resolved);
				state_->selectedSource = -1;
				state_->history.Reset(state_->document);
				state_->dirty = true;
				state_->queryPreview.reset();
			}
			state_->message = status == AnimationStatus::Ok
				? "Adopted the selected clip rig and resolved every enabled source identity."
				: (state_->persistedSnapshot
					? "A saved database keeps its rig compatibility identity. Create a new database to use a different rig."
					: "Selected clip rig adoption: " + std::string(ToString(status)));
		}
		if(ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Reads the selected clip's checked asset envelope, adopts its skeleton compatibility identity, and atomically resolves every enabled source. Incompatible sources leave the document unchanged.");
		}
		ImGui::SameLine();
		if(ImGui::Button("Resolve source identities")) {
			MotionDatabaseAuthoringDocument resolved = state_->document;
			AnimationStatus status = authoringHost_ != nullptr
				? AnimationStatus::Ok : AnimationStatus::NotReady;
			for(auto& source : resolved.sources) {
				if(status != AnimationStatus::Ok || !source.enabled) continue;
				status = authoringHost_->ResolveMotionDatabaseSourceIdentity(
					source, resolved.compatibilityFingerprint);
			}
			if(status == AnimationStatus::Ok) {
				state_->document = std::move(resolved);
				state_->selectedSource = -1;
				markModified();
			}
			state_->message = "Source identity resolution: " + std::string(ToString(status));
		}
		ImGui::SameLine();
		if(ImGui::Button("Save")) {
			const AnimationStatus saved = saveGroupedEdit();
			state_->message = saved == AnimationStatus::Ok
				? "Saved one grouped authoring transaction with exact-byte compare-and-swap."
				: (saved == AnimationStatus::NotReady
					? "There are no grouped authoring edits to save."
					: "Authoring save rejected; the previous sidecar was preserved ("
						+ std::string(ToString(saved)) + ").");
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel edit")) {
			const AnimationStatus cancelled = state_->history.CancelEdit(state_->document);
			if(cancelled == AnimationStatus::Ok) {
				state_->dirty = !state_->persistedSnapshot.has_value();
				state_->selectedSource = -1;
				state_->queryPreview.reset();
			}
			state_->message = cancelled == AnimationStatus::Ok
				? "Cancelled the complete unsaved edit group."
				: "There is no active edit group to cancel.";
		}
		ImGui::SameLine();
		if(ImGui::Button("Undo")) {
			const AnimationStatus undone = persistHistoryRestore(true);
			state_->message = undone == AnimationStatus::Ok
				? "Undid and atomically saved the prior complete document as a new generation."
				: "Undo was not applied (" + std::string(ToString(undone)) + ").";
		}
		ImGui::SameLine();
		if(ImGui::Button("Redo")) {
			const AnimationStatus redone = persistHistoryRestore(false);
			state_->message = redone == AnimationStatus::Ok
				? "Redid and atomically saved the complete document as a new generation."
				: "Redo was not applied (" + std::string(ToString(redone)) + ").";
		}
		ImGui::SameLine();
		if(ImGui::Button("Build database")) {
			if(authoringHost_ == nullptr || state_->session == nullptr) {
				state_->message = "Build integration requires MotionDatabaseAuthoringHost to resolve immutable clip samples and provide the editor task executor.";
			} else if(state_->dirty) {
				state_->message = "Save valid authoring metadata before starting a build.";
			} else {
				state_->inFlightBuildDocument = state_->document;
				MotionDatabaseWorkerSourceResolver resolver =
					authoringHost_->CreateMotionDatabaseSourceResolver(
						*state_->inFlightBuildDocument);
				const AnimationStatus status = state_->session->RequestBuild(
					*state_->inFlightBuildDocument, std::move(resolver),
					state_->observedCookedPath);
				if(status != AnimationStatus::Queued) state_->inFlightBuildDocument.reset();
				state_->message = "Build request: " + std::string(ToString(status));
			}
		}
		if(state_->session != nullptr) {
			ImGui::SameLine();
			if(ImGui::Button("Cancel build")) state_->session->CancelBuild();
			const AnimationStatus polled = state_->session->Poll(state_->document);
			const MotionDatabaseBuildProgress progress = state_->session->GetBuildProgress();
			ImGui::ProgressBar(progress.Fraction(), ImVec2(-1.0F, 0.0F));
			if(polled != AnimationStatus::NotReady) {
				state_->message = "Build completion: " + std::string(ToString(polled));
				state_->inFlightBuildDocument.reset();
				if(polled == AnimationStatus::Ok && authoringHost_ != nullptr) {
					const auto published = state_->session->GetPublished();
					if(published && published->asset) {
						authoringHost_->NotifyMotionDatabasePublished(
							state_->observedCookedPath, published->asset->assetId);
					}
				}
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Feature Configuration")) {
		ImGui::TextWrapped("Feature weights are persisted authoring data and seed the real CPU/GPU-compatible search query. Zero disables a lane's soft cost without changing the fixed cooked schema.");
		if(ImGui::BeginTable("FeatureWeights", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("Lane"); ImGui::TableSetupColumn("Semantic"); ImGui::TableSetupColumn("Query weight");
			ImGui::TableHeadersRow();
			for(std::size_t lane = 0U; lane < kMotionDatabaseFeatureCount; ++lane) {
				ImGui::PushID(static_cast<int>(lane));
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::Text("%zu", lane);
				ImGui::TableNextColumn(); ImGui::Text("%s", std::string(ToString(FeatureBlockForLane(lane).semantic)).c_str());
				ImGui::TableNextColumn();
				if(ImGui::InputScalar("##weight", ImGuiDataType_U16, &state_->document.featureWeights[lane])) markModified();
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Query / Candidate Scores")) {
		// Loose cooked files do not carry a live ResourceManager generation. Use a
		// detached generation only for this immutable preview; runtime-bound packets
		// retain their real generation through SetRuntimeQueryPreview.
		const AnimationGeneration previewGeneration = std::max(database->runtimeGeneration, 1U);
		state_->query.database = {database->assetId, previewGeneration};
		state_->query.expectedSchemaFingerprint = database->featureSchemaFingerprint;
		state_->query.featureWeights = state_->document.featureWeights;
		ImGui::InputScalar("Required tag mask", ImGuiDataType_U64, &state_->query.requiredTags,
			nullptr, nullptr, "0x%016llx", ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::InputScalar("Excluded tag mask", ImGuiDataType_U64, &state_->query.excludedTags,
			nullptr, nullptr, "0x%016llx", ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::InputScalar("Required contacts", ImGuiDataType_U32, &state_->query.requiredContacts,
			nullptr, nullptr, "0x%08x", ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::InputScalar("Preferred phase", ImGuiDataType_U16, &state_->query.preferredPhase);
		std::uint32_t candidateLimit = static_cast<std::uint32_t>(state_->query.maximumCandidates);
		if(ImGui::InputScalar("Candidate limit", ImGuiDataType_U32, &candidateLimit)) {
			state_->query.maximumCandidates = std::clamp<std::size_t>(candidateLimit, 1U, kMotionDatabaseMaximumTopK);
		}
		if(ImGui::Button("Run deterministic search preview")) {
			MotionDatabaseAsset previewDatabase = *database;
			previewDatabase.runtimeGeneration = previewGeneration;
			MotionDatabaseQueryPreviewState snapshot;
			snapshot.databaseGeneration = previewGeneration;
			snapshot.query = state_->query;
			const AnimationStatus status = ScoreMotionMatchCpu(
				previewDatabase, snapshot.query, snapshot.result);
			if(status == AnimationStatus::Ok) state_->queryPreview = std::move(snapshot);
			state_->message = "Search preview: " + std::string(ToString(status));
		}

		if(state_->queryPreview) {
			const auto& preview = *state_->queryPreview;
			ImGui::Text("Database generation: %llu   visited/evaluated/excluded: %u / %u / %u",
				static_cast<unsigned long long>(preview.databaseGeneration), preview.result.visitedSamples,
				preview.result.evaluatedSamples, preview.result.excludedSamples);
			ImGui::Text("Fallback: %s%s", std::string(ToString(preview.result.fallbackReason)).c_str(),
				preview.result.candidateBudgetExhausted ? " (candidate budget exhausted)" : "");
			if(ImGui::BeginTable("CandidateScores", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				for(const char* heading : {"Rank", "Sample", "Source clip", "Source tick", "Total cost"}) ImGui::TableSetupColumn(heading);
				ImGui::TableHeadersRow();
				for(std::size_t index = 0U; index < preview.result.candidates.size(); ++index) {
					const MotionMatchCandidate& candidate = preview.result.candidates[index];
					ImGui::PushID(static_cast<int>(index)); ImGui::TableNextRow();
					ImGui::TableNextColumn(); if(ImGui::Selectable(std::to_string(index).c_str(), state_->selectedCandidate == static_cast<int>(index))) state_->selectedCandidate = static_cast<int>(index);
					ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(candidate.stableSampleId));
					ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(candidate.sourceClipId));
					ImGui::TableNextColumn(); ImGui::Text("%lld", static_cast<long long>(candidate.sourceTick));
					ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(candidate.score.totalCost));
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if(state_->selectedCandidate >= 0
				&& static_cast<std::size_t>(state_->selectedCandidate) < preview.result.candidates.size()) {
				const auto& score = preview.result.candidates[static_cast<std::size_t>(state_->selectedCandidate)].score;
				ImGui::Text("Metadata costs - phase %llu, tag %llu, contact %llu, continuity %llu, transition %llu",
					static_cast<unsigned long long>(score.phaseCost), static_cast<unsigned long long>(score.tagCost),
					static_cast<unsigned long long>(score.contactCost), static_cast<unsigned long long>(score.continuityCost),
					static_cast<unsigned long long>(score.transitionCost));
				for(std::size_t lane = 0U; lane < score.perFeatureCosts.size(); ++lane) {
					ImGui::BulletText("Lane %zu (%s): %llu", lane,
						std::string(ToString(FeatureBlockForLane(lane).semantic)).c_str(),
						static_cast<unsigned long long>(score.perFeatureCosts[lane]));
				}
			}
			for(const MotionMatchExcludedCandidate& excluded : preview.result.exclusionDetails) {
				ImGui::BulletText("Excluded sample %llu: reason mask 0x%08x",
					static_cast<unsigned long long>(excluded.stableSampleId),
					static_cast<unsigned int>(excluded.reasons));
			}
		} else {
			ImGui::TextDisabled("Run a search or bind a copied runtime query/result snapshot to inspect real candidate scores.");
		}
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}
