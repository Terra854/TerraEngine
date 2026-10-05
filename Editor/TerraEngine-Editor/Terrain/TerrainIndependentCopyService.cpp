// TerrainIndependentCopyService.cpp -- Detached preparation and receipt ownership.
#include "TerrainIndependentCopyService.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

namespace TerraEngine::Editor::Terrain {
	namespace {
		namespace Authored = TerraEngine::Terrain;
		bool PlainPath(const std::filesystem::path& path);
		std::string ReadText(const std::filesystem::path& path) {
			// Reject redirected inputs and bound reads before allocating their bytes.
			if (!PlainPath(path) || std::filesystem::file_size(path) > 64U * 1024U * 1024U) throw std::runtime_error("Terrain copy input is redirected or exceeds the bounded file size.");
			std::ifstream input(path, std::ios::binary);
			if (!input) throw std::runtime_error("Terrain copy could not read an owned input.");
			return {std::istreambuf_iterator<char>(input), {}};
		}
		void WriteText(const std::filesystem::path& path, const std::string& bytes) {
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if (!output || !(output << bytes)) throw std::runtime_error("Terrain copy could not write private metadata.");
			output.close(); if (!output) throw std::runtime_error("Terrain copy metadata did not flush.");
		}
		bool Within(const std::filesystem::path& path, const std::filesystem::path& root) {
			const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
			return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
		}
		bool DescriptorIdentity(const std::filesystem::path& path, TerraEngine::UUID expected) {
			const auto descriptor = nlohmann::json::parse(ReadText(path));
			const auto id = TerraEngine::ParseUUID(descriptor.value("assetId", std::string{}));
			return id && *id == expected;
		}
		bool PlainPath(const std::filesystem::path& path) {
			for (auto current = path; !current.empty(); current = current.parent_path()) {
				std::error_code error; const auto status = std::filesystem::symlink_status(current, error);
				if (!error && std::filesystem::is_symlink(status)) return false;
#ifdef _WIN32
				const auto attributes = GetFileAttributesW(current.c_str());
				if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) return false;
#endif
				if (current == current.parent_path()) break;
			}
			return true;
		}
		bool PlainTree(const std::filesystem::path& path) {
			if (!PlainPath(path)) return false;
			if (!std::filesystem::is_directory(path)) return true;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) if (!PlainPath(entry.path())) return false;
			return true;
		}
		struct FileIdentity {
			std::uint64_t device = 0U, object = 0U; bool valid = false;
			bool operator==(const FileIdentity&) const = default;
		};
		FileIdentity CaptureFileIdentity(const std::filesystem::path& path) {
#ifdef _WIN32
			const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if (handle == INVALID_HANDLE_VALUE) return {};
			BY_HANDLE_FILE_INFORMATION info{}; const auto success = GetFileInformationByHandle(handle, &info); CloseHandle(handle);
			return {info.dwVolumeSerialNumber, (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow, success != 0};
#else
			struct stat info{}; if (stat(path.c_str(), &info) != 0) return {};
			return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), true};
#endif
		}
		bool DataIdentity(const std::filesystem::path& path, TerraEngine::UUID expected, Authored::ContentHash expectedHash) {
			std::istringstream current(ReadText(path / "CURRENT")); std::string hash; current >> hash;
			if (hash != Authored::ToHexString(expectedHash)) return false;
			const auto root = Authored::DecodeTerrainRootManifest(ReadText(path / "roots" / (hash + ".root")));
			return root.Succeeded() && root.root && root.root->assetUuid == expected
				&& root.contentHash == expectedHash && Authored::ToHexString(root.contentHash) == hash;
		}
	}

	struct TerrainIndependentCopyService::Impl {
		struct Job {
			TerrainCopyPrepared prepared;
			TerrainCopyStatus status;
			TerraEngine::Scene::Transaction transaction;
			std::atomic_bool cancellation{false};
			std::future<bool> future;
			bool workerConsumed = false, workerSucceeded = false, workSubmitted = false;
			bool installedData = false, installedDescriptor = false, registrationAttempted = false;
			bool desiredCancellation = false, stagingCreated = false, markerCreated = false;
			FileIdentity stagingIdentity, markerIdentity;
			std::string workerDiagnostic;
#ifdef TERRAENGINE_EDITOR_TESTS
			TestHook hook;
#endif
		};
		TerrainCopyCallbacks callbacks;
		std::map<TerraEngine::UUID, std::shared_ptr<Job>> jobs;
		std::array<TerrainCopyStatus, 16U> history;
		std::size_t historyBegin = 0U, historyCount = 0U;
#ifdef TERRAENGINE_EDITOR_TESTS
		TestHook hook;
#endif
		explicit Impl(TerrainCopyCallbacks supplied) : callbacks(std::move(supplied)) {}
		static bool Prepare(const std::shared_ptr<Job>& job) {
			try {
#ifdef TERRAENGINE_EDITOR_TESTS
				if (job->hook) job->hook(Boundary::BeforePreparation);
#endif
				if (job->cancellation.load()) return false;
				auto& prepared = job->prepared;
				const auto& capture = prepared.sourceCapture;
				const auto sourceData = std::filesystem::path(capture.sourceDescriptorPath.string() + ".data");
				const auto sourceRoot = Authored::DecodeTerrainRootManifest(ReadText(sourceData / "roots" / (Authored::ToHexString(capture.sourceRootHash) + ".root")));
				if (!sourceRoot.Succeeded() || !sourceRoot.root || sourceRoot.contentHash != capture.sourceRootHash
					|| sourceRoot.root->assetUuid != capture.sourceAssetId) throw std::runtime_error("Captured terrain root failed identity validation.");
				Authored::TerrainAssetRoot detached = *sourceRoot.root;
				detached.assetUuid = prepared.ownership.freshAssetId;
				// A new asset begins its own durable generation chain; authored edit
				// generations, coordinates, layers and unrelated material IDs survive.
				detached.publicationGeneration = detached.persistedRootGeneration = 1U;
				std::vector<std::pair<Authored::ContentHash, std::vector<std::uint8_t>>> blobs;
				for (auto& shard : detached.directoryShards) for (auto& [coordinate, descriptor] : shard.descriptors) {
					std::string identity;
					const auto rewrite = [&](Authored::ContentHash oldHash, Authored::TerrainChannelKind kind, std::uint16_t group) {
						if (job->cancellation.load()) throw std::runtime_error("Terrain copy preparation was cancelled.");
						const auto text = ReadText(sourceData / "blobs" / (Authored::ToHexString(oldHash) + ".tearea"));
						Authored::DecodedTerrainArea decoded;
						if (!Authored::DecodeTerrainArea(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), decoded).Succeeded()
							|| decoded.header.envelopeHash != oldHash || decoded.header.assetUuid != capture.sourceAssetId
							|| decoded.header.area != coordinate || decoded.header.channelKind != kind || decoded.header.weightGroup != group)
							throw std::runtime_error("Terrain copy rejected a mismatched channel envelope.");
						decoded.header.assetUuid = prepared.ownership.freshAssetId;
						Authored::EncodedTerrainArea encoded;
						if (!Authored::EncodeTerrainArea(decoded.header, decoded.canonicalPayload, encoded).Succeeded()) throw std::runtime_error("Terrain channel reidentification failed.");
						const auto hash = encoded.header.envelopeHash;
						identity += std::to_string(static_cast<unsigned>(kind)) + ":" + std::to_string(group) + ":" + Authored::ToHexString(hash) + ";";
						blobs.emplace_back(hash, std::move(encoded.bytes)); return hash;
					};
					if (descriptor.channels.heightHash) descriptor.channels.heightHash = rewrite(*descriptor.channels.heightHash, Authored::TerrainChannelKind::Height, 0U);
					if (descriptor.channels.holeHash) descriptor.channels.holeHash = rewrite(*descriptor.channels.holeHash, Authored::TerrainChannelKind::Holes, 0U);
					for (auto& [group, hash] : descriptor.channels.weightGroupHashes) hash = rewrite(hash, Authored::TerrainChannelKind::WeightGroup, group);
					if (!identity.empty()) descriptor.authoredContentHash = Authored::HashTerrainBytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()));
				}
				// Existing canonical candidate construction refreshes every shard/root
				// identity instead of inventing a second durable manifest vocabulary.
				Authored::TerrainAssetStore temporary(std::make_shared<const Authored::TerrainAssetRoot>(detached));
				std::shared_ptr<const Authored::TerrainAssetRoot> canonical;
				if (!temporary.BuildCandidateRoot(detached, {}, {}, canonical).Succeeded()) throw std::runtime_error("Detached terrain root failed validation.");
				Authored::TerrainAssetStore store(canonical);
				for (auto& [hash, bytes] : blobs) if (!store.Blobs().PutVerified(hash, std::move(bytes))) throw std::runtime_error("Detached terrain blob failed verification.");
				if (!PlainPath(prepared.ownership.stagingDirectory)
					|| ReadText(prepared.ownership.stagingDirectory / "OWNER") != TerraEngine::ToHexString(prepared.ownership.operationId))
					throw std::runtime_error("Private terrain preparation ownership changed.");
#ifdef TERRAENGINE_EDITOR_TESTS
				if (job->hook) job->hook(Boundary::BeforePersistence);
#endif
				if (!store.PersistAtomically({prepared.stagedData, &job->cancellation}).Succeeded()) throw std::runtime_error("Independent terrain persistence failed.");
				Authored::TerrainAssetStore reopened(canonical);
				if (!reopened.OpenPersistedRoot(prepared.stagedData).Succeeded()) throw std::runtime_error("Independent terrain durable reopening failed.");
				prepared.root = reopened.CaptureRoot().root;
				if (!prepared.root || prepared.root->assetUuid != prepared.ownership.freshAssetId) throw std::runtime_error("Independent terrain durable identity differs.");
				auto descriptor = nlohmann::json::parse(capture.descriptorBytes);
				if (!descriptor.is_object() || descriptor.value("format", std::string{}) != "TerraEngineTerrain"
					|| !descriptor.contains("assetId") || TerraEngine::ParseUUID(descriptor["assetId"].get<std::string>()) != std::optional<TerraEngine::UUID>{capture.sourceAssetId})
					throw std::runtime_error("Captured terrain descriptor identity differs.");
				descriptor["assetId"] = TerraEngine::ToHexString(prepared.ownership.freshAssetId);
				WriteText(prepared.stagedDescriptor, descriptor.dump(4));
				return !job->cancellation.load();
			} catch (const std::exception& error) {
				try { job->workerDiagnostic = error.what(); } catch (...) {}
			} catch (...) { try { job->workerDiagnostic = "Independent terrain preparation failed."; } catch (...) {} }
			return false;
		}
		void CleanupPrivate(const Job& job) {
			const auto& path = job.prepared.ownership.stagingDirectory;
			if (!std::filesystem::exists(path)) return;
			if (!PlainTree(path)) throw std::runtime_error("Private terrain cleanup ownership changed.");
			const auto marker = path / "OWNER";
			const auto expected = TerraEngine::ToHexString(job.prepared.ownership.operationId);
			// Before enqueue, only this owner's exclusive empty directory and its
			// captured marker file can be cleaned. Never recursively erase a partial
			// admission or accept a replaced marker merely because its name matches.
			if (job.workerConsumed && !job.workSubmitted && !job.installedData && !job.installedDescriptor) {
				if (!job.stagingCreated || !job.stagingIdentity.valid || CaptureFileIdentity(path) != job.stagingIdentity)
					throw std::runtime_error("Private terrain staging reservation changed.");
				for (const auto& child : std::filesystem::directory_iterator(path))
					if (child.path() != marker) throw std::runtime_error("Partial terrain admission contains foreign files.");
				if (std::filesystem::exists(marker)) {
					if (!job.markerCreated || !job.markerIdentity.valid || CaptureFileIdentity(marker) != job.markerIdentity
						|| !expected.starts_with(ReadText(marker))) throw std::runtime_error("Partial terrain owner marker changed.");
					std::filesystem::remove(marker);
				}
				if (!std::filesystem::remove(path)) throw std::runtime_error("Partial terrain admission is not empty.");
				return;
			}
			if (ReadText(marker) != expected) throw std::runtime_error("Private terrain cleanup ownership changed.");
			std::error_code error; std::filesystem::remove_all(path, error);
			if (error) throw std::runtime_error(error.message());
		}
		bool Cleanup(Job& job) {
			try {
#ifdef TERRAENGINE_EDITOR_TESTS
				if (job.hook) job.hook(Boundary::BeforeCleanup);
#endif
				const auto& owner = job.prepared.ownership;
				if (!PlainTree(owner.stagingDirectory) || !PlainTree(owner.dataDirectory) || !PlainPath(owner.descriptorPath) || !PlainPath(owner.descriptorPath.string() + ".meta")
					|| (job.installedDescriptor && std::filesystem::exists(owner.descriptorPath) && !DescriptorIdentity(owner.descriptorPath, owner.freshAssetId))
					|| (job.installedData && std::filesystem::exists(owner.dataDirectory) && !DataIdentity(owner.dataDirectory, owner.freshAssetId, job.prepared.root->rootContentHash))) {
					CleanupPrivate(job);
					job.status.state = TerrainCopyState::Failed; job.status.diagnostic = "Terrain copy output ownership changed; cleanup refused."; return true;
				}
				const auto metadataPath = std::filesystem::path(owner.descriptorPath.string() + ".meta");
				if (std::filesystem::exists(metadataPath)) {
					const auto metadata = nlohmann::json::parse(ReadText(metadataPath));
					if (TerraEngine::ParseUUID(metadata.value("guid", std::string{})) != std::optional<TerraEngine::UUID>{owner.freshAssetId}) {
						CleanupPrivate(job); job.status.state = TerrainCopyState::Failed;
						job.status.diagnostic = "Terrain copy metadata ownership changed; cleanup refused."; return true;
					}
				}
				if (job.registrationAttempted) {
					const auto outcome = callbacks.rollbackProvisional(owner);
					if (outcome.disposition == TerrainCopyCleanupDisposition::OwnershipLost) {
						CleanupPrivate(job);
						job.status.state = TerrainCopyState::Failed; job.status.diagnostic = outcome.diagnostic; return true;
					}
					if (outcome.disposition == TerrainCopyCleanupDisposition::Retry) throw std::runtime_error(outcome.diagnostic);
				}
				// Refuse deletion if a descriptor or private CURRENT has been rebound.
				// A missing owned path is already clean; never chase an unrelated path.
				std::error_code error;
				if (job.installedDescriptor) {
					std::filesystem::remove(owner.descriptorPath, error); if (error) throw std::runtime_error(error.message());
					std::filesystem::remove(owner.descriptorPath.string() + ".meta", error); if (error) throw std::runtime_error(error.message());
				}
				if (job.installedData) { std::filesystem::remove_all(owner.dataDirectory, error); if (error) throw std::runtime_error(error.message()); }
				CleanupPrivate(job);
				job.status.state = job.desiredCancellation ? TerrainCopyState::Cancelled : TerrainCopyState::Failed; return true;
			} catch (const std::exception& error) {
				job.status.state = TerrainCopyState::CleanupPending;
				try { job.status.diagnostic = error.what(); } catch (...) {} return false;
			} catch (...) { job.status.state = TerrainCopyState::CleanupPending; return false; }
		}
		void Remember(TerrainCopyStatus&& status) noexcept {
			const auto destination = (historyBegin + historyCount) % history.size();
			history[destination] = std::move(status);
			if (historyCount < history.size()) ++historyCount;
			else historyBegin = (historyBegin + 1U) % history.size();
		}
		bool FinalizeCommitted(Job& job) {
			// Receipt commitment is irrevocable. A failing finalizer retains a retry
			// obligation and must never enter the rollback/delete path.
			job.status.state = TerrainCopyState::FinalizePending;
			try {
				callbacks.finalizeProvisional(job.prepared.ownership);
				CleanupPrivate(job);
				job.status.diagnostic = "Independent terrain copy committed.";
				job.status.state = TerrainCopyState::Committed; return true;
			} catch (...) { try { job.status.diagnostic = "Committed terrain copy retains a finalization recovery obligation."; } catch (...) {} return false; }
		}
	};

	TerrainIndependentCopyService::TerrainIndependentCopyService(TerrainCopyCallbacks callbacks) : implementation(std::make_unique<Impl>(std::move(callbacks))) {}
	TerrainIndependentCopyService::~TerrainIndependentCopyService() { try { (void)DrainAtShutdown(); } catch (...) {} }
	TerrainCopySubmission TerrainIndependentCopyService::Begin(TerrainCopyCapture capture) {
		auto& owner = *implementation;
		TerrainCopySubmission result;
		std::shared_ptr<Impl::Job> job;
		try {
			if (!owner.callbacks.identityAvailable || !owner.callbacks.validateSource || !owner.callbacks.registerProvisional
				|| !owner.callbacks.rollbackProvisional || !owner.callbacks.finalizeProvisional || ThreadPool::instance().getThreadCount() == 0U)
				throw std::runtime_error("Terrain copy owner or task executor is unavailable.");
			if (capture.projectEpoch == 0U || capture.selectedObjectId == TerraEngine::EMPTY_UUID || !capture.sourceRootHash.IsValid()
				|| !capture.sourceDescriptorPath.is_absolute() || !capture.assetRoot.is_absolute() || !capture.privateStagingRoot.is_absolute()
				|| !Within(capture.sourceDescriptorPath, capture.assetRoot) || Within(capture.privateStagingRoot, capture.assetRoot)
				|| !PlainPath(capture.sourceDescriptorPath) || !PlainPath(capture.privateStagingRoot))
				throw std::runtime_error("Terrain copy capture paths or durable observation are invalid.");
			if (capture.observedReadSet.sceneGeneration == 0U
				|| std::none_of(capture.observedReadSet.objects.begin(), capture.observedReadSet.objects.end(), [&](const auto& object) { return object.objectId == capture.selectedObjectId && object.lifetimeGeneration != 0U; }))
				throw std::runtime_error("Terrain copy requires an observed scene and selected object lifetime.");
			job = std::make_shared<Impl::Job>();
			job->prepared.sourceCapture = std::move(capture);
			auto& ownership = job->prepared.ownership;
			ownership.projectEpoch = job->prepared.sourceCapture.projectEpoch;
			ownership.operationId = TerraEngine::GenerateUUID();
			bool identityReserved = false;
			for (unsigned attempt = 0U; attempt < 128U && !identityReserved; ++attempt) {
				ownership.freshAssetId = TerraEngine::GenerateUUID();
				identityReserved = owner.callbacks.identityAvailable(ownership.freshAssetId);
				for (const auto& [id, other] : owner.jobs) { (void)id; if (other->prepared.ownership.freshAssetId == ownership.freshAssetId) identityReserved = false; }
			}
			if (!identityReserved) throw std::runtime_error("No unclaimed terrain identity could be reserved.");
			const auto& source = job->prepared.sourceCapture.sourceDescriptorPath;
			for (unsigned index = 1U; ; ++index) {
				ownership.descriptorPath = source.parent_path() / (source.stem().string() + " Copy " + std::to_string(index) + source.extension().string());
				ownership.dataDirectory = ownership.descriptorPath.string() + ".data";
				bool reserved = false; for (const auto& [id, other] : owner.jobs) { (void)id; reserved |= other->prepared.ownership.descriptorPath == ownership.descriptorPath; }
				if (!reserved && !std::filesystem::exists(ownership.descriptorPath) && !std::filesystem::exists(ownership.dataDirectory)
					&& !std::filesystem::exists(ownership.descriptorPath.string() + ".meta")) break;
			}
			ownership.stagingDirectory = job->prepared.sourceCapture.privateStagingRoot / TerraEngine::ToHexString(ownership.operationId);
			job->prepared.stagedDescriptor = ownership.stagingDirectory / "copy.teterrain";
			job->prepared.stagedData = ownership.stagingDirectory / "copy.teterrain.data";
			job->status = {job->prepared.sourceCapture.selectedObjectId, ownership.operationId, ownership.freshAssetId, TerrainCopyState::Preparing,
				"terrain-copy-" + TerraEngine::ToHexString(ownership.operationId), "Preparing an independent durable terrain."};
			job->transaction.id = job->status.transactionId; job->transaction.origin = TerraEngine::Scene::TransactionOrigin::Editor;
			job->transaction.originName = job->transaction.undoGroupName = "Terrain Independent Copy";
			job->transaction.readSet = job->prepared.sourceCapture.observedReadSet;
			TerraEngine::Scene::TransactionWriteOperation operation;
			operation.kind = TerraEngine::Scene::TransactionWriteKind::UpdateComponentProperty;
			operation.target.objectId = job->prepared.sourceCapture.selectedObjectId;
			operation.componentName = "TerrainComponent"; operation.propertyName = "terrainAssetId";
			operation.propertyValue = TerraEngine::Reflection::Variant{ownership.freshAssetId};
			job->transaction.writeSet.operations.push_back(std::move(operation));
#ifdef TERRAENGINE_EDITOR_TESTS
			job->hook = owner.hook;
#endif
			if (!owner.jobs.emplace(ownership.operationId, job).second) throw std::runtime_error("Terrain operation identity was already claimed.");
			result.operationId = ownership.operationId;
			try {
				std::filesystem::create_directories(job->prepared.sourceCapture.privateStagingRoot);
				if (!std::filesystem::create_directory(ownership.stagingDirectory)) throw std::runtime_error("Private terrain staging destination was already claimed.");
				job->stagingCreated = true; job->stagingIdentity = CaptureFileIdentity(ownership.stagingDirectory);
#ifdef TERRAENGINE_EDITOR_TESTS
				if (job->hook) job->hook(Boundary::AfterStageReservation);
#endif
				const auto marker = ownership.stagingDirectory / "OWNER";
				{ std::ofstream initial(marker, std::ios::binary); if (!initial) throw std::runtime_error("Private terrain owner marker could not be created."); }
				job->markerCreated = true; job->markerIdentity = CaptureFileIdentity(marker);
#ifdef TERRAENGINE_EDITOR_TESTS
				if (job->hook) job->hook(Boundary::AfterOwnerMarkerCreation);
#endif
				WriteText(marker, TerraEngine::ToHexString(ownership.operationId));
				result.message = job->status.diagnostic;
				auto task = ThreadPool::instance().createTask([job] { return Impl::Prepare(job); });
				job->future = task->getFuture(); ThreadPool::instance().enqueue(task); job->workSubmitted = true;
			} catch (...) {
				job->future = {}; job->workerConsumed = true;
				if (owner.Cleanup(*job)) { owner.Remember(std::move(job->status)); owner.jobs.erase(ownership.operationId); }
				throw;
			}
			result.accepted = true;
		} catch (const std::exception& error) { result.message = error.what(); }
		return result;
	}
	void TerrainIndependentCopyService::PollPrepared(std::uint64_t currentProjectEpoch) {
		auto& owner = *implementation;
		for (auto iterator = owner.jobs.begin(); iterator != owner.jobs.end();) {
			auto& job = *iterator->second;
			if (job.status.state != TerrainCopyState::Preparing) { ++iterator; continue; }
			if (job.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++iterator; continue; }
			try { job.workerSucceeded = job.future.get(); } catch (...) { job.workerSucceeded = false; }
			job.workerConsumed = true;
			if (!job.workerDiagnostic.empty()) job.status.diagnostic = std::move(job.workerDiagnostic);
			try {
				if (!job.workerSucceeded || job.cancellation.load() || currentProjectEpoch != job.prepared.ownership.projectEpoch) throw std::runtime_error("Terrain copy preparation retired or failed.");
				std::string diagnostic;
				if (!owner.callbacks.validateSource(job.prepared.sourceCapture, diagnostic) || !owner.callbacks.identityAvailable(job.prepared.ownership.freshAssetId)) throw std::runtime_error(diagnostic.empty() ? "Terrain copy source observation changed." : diagnostic);
				const auto& token = job.prepared.ownership;
				if (!PlainPath(token.descriptorPath) || !PlainTree(job.prepared.stagedData)) throw std::runtime_error("Terrain destination or staging was redirected.");
				if (std::filesystem::exists(token.descriptorPath) || std::filesystem::exists(token.dataDirectory) || std::filesystem::exists(token.descriptorPath.string() + ".meta")) throw std::runtime_error("Reserved terrain destination was claimed by another owner.");
				std::filesystem::rename(job.prepared.stagedData, token.dataDirectory); job.installedData = true;
				std::filesystem::rename(job.prepared.stagedDescriptor, token.descriptorPath); job.installedDescriptor = true;
				job.registrationAttempted = true;
				if (!owner.callbacks.registerProvisional(job.prepared, token, diagnostic)) throw std::runtime_error(diagnostic.empty() ? "Independent terrain registration failed." : diagnostic);
				if (!owner.callbacks.validateSource(job.prepared.sourceCapture, diagnostic)) throw std::runtime_error("Terrain source changed during provisional registration.");
#ifdef TERRAENGINE_EDITOR_TESTS
				if (job.hook) job.hook(Boundary::BeforeEnqueue);
#endif
				// The tracked transaction and all rollback ownership exist before queue
				// admission. Enqueue itself does not make the selected binding visible.
				job.status.diagnostic = "Independent terrain rebind is pending the protected scene boundary.";
				TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(job.transaction));
				job.status.state = TerrainCopyState::RebindPending;
			} catch (const std::exception& error) { try { job.status.diagnostic = error.what(); } catch (...) {} (void)owner.Cleanup(job); }
			catch (...) { (void)owner.Cleanup(job); }
			if (job.status.state == TerrainCopyState::Failed || job.status.state == TerrainCopyState::Cancelled) { owner.Remember(std::move(job.status)); iterator = owner.jobs.erase(iterator); }
			else ++iterator;
		}
	}
	bool TerrainIndependentCopyService::HandleTransactionResult(const TerraEngine::Scene::TransactionResult& result) {
		auto& owner = *implementation;
		for (auto iterator = owner.jobs.begin(); iterator != owner.jobs.end(); ++iterator) {
			auto& job = *iterator->second;
			if (job.status.transactionId != result.transactionId || !job.workerConsumed) continue;
			if (result.status == TerraEngine::Scene::TransactionStatus::Pending) return true;
			if (result.status == TerraEngine::Scene::TransactionStatus::Committed) {
				// A committed receipt wins even if cancellation arrived after flush.
				(void)owner.FinalizeCommitted(job);
			} else { try { job.status.diagnostic = result.message; } catch (...) {} (void)owner.Cleanup(job); }
			if (job.status.state != TerrainCopyState::CleanupPending && job.status.state != TerrainCopyState::FinalizePending) { owner.Remember(std::move(job.status)); owner.jobs.erase(iterator); }
			return true;
		}
		return false;
	}
	void TerrainIndependentCopyService::Cancel(TerraEngine::UUID operationId) {
		auto& owner = *implementation; const auto found = owner.jobs.find(operationId); if (found == owner.jobs.end()) return;
		auto& job = *found->second; job.desiredCancellation = true; job.cancellation.store(true);
		if (job.status.state == TerrainCopyState::RebindPending || job.status.state == TerrainCopyState::AwaitingReceipt) {
			// False may mean a committed receipt is waiting in the parent's sole
			// channel. Never remove its asset merely because cancellation was late.
			if (TerraEngine::Scene::SceneMutationQueue::Instance().CancelTransaction(job.status.transactionId)) {
				if (owner.Cleanup(job)) { owner.Remember(std::move(job.status)); owner.jobs.erase(found); }
			}
			else job.status.state = TerrainCopyState::AwaitingReceipt;
		}
	}
	void TerrainIndependentCopyService::RetireProject(std::uint64_t projectEpoch) {
		// Advance first because Cancel may erase a physically settled operation.
		for (auto iterator = implementation->jobs.begin(); iterator != implementation->jobs.end();) {
			const auto current = iterator++;
			if (current->second->prepared.ownership.projectEpoch == projectEpoch) Cancel(current->first);
		}
	}
	void TerrainIndependentCopyService::RetryCleanup(std::uint64_t currentProjectEpoch) {
		(void)currentProjectEpoch;
		auto& owner = *implementation;
		for (auto iterator = owner.jobs.begin(); iterator != owner.jobs.end();) {
			auto& job = *iterator->second;
			if (job.status.state == TerrainCopyState::CleanupPending) (void)owner.Cleanup(job);
			if (job.status.state == TerrainCopyState::FinalizePending) (void)owner.FinalizeCommitted(job);
			if (job.status.state == TerrainCopyState::Failed || job.status.state == TerrainCopyState::Cancelled || job.status.state == TerrainCopyState::Committed) { owner.Remember(std::move(job.status)); iterator = owner.jobs.erase(iterator); }
			else ++iterator;
		}
	}
	std::vector<TerrainCopyStatus> TerrainIndependentCopyService::SnapshotStatus() const {
		std::vector<TerrainCopyStatus> output;
		for (std::size_t index = 0U; index < implementation->historyCount; ++index)
			output.push_back(implementation->history[(implementation->historyBegin + index) % implementation->history.size()]);
		for (const auto& [id, job] : implementation->jobs) { (void)id; output.push_back(job->status); } return output;
	}
	bool TerrainIndependentCopyService::DrainAtShutdown() {
		for (auto iterator = implementation->jobs.begin(); iterator != implementation->jobs.end();) {
			const auto current = iterator++; Cancel(current->first);
		}
		for (const auto& [id, job] : implementation->jobs) { (void)id; if (job->future.valid()) job->future.wait(); }
		PollPrepared(0U); RetryCleanup(0U); return implementation->jobs.empty();
	}
#ifdef TERRAENGINE_EDITOR_TESTS
	void TerrainIndependentCopyService::SetTestHook(TestHook hook) { implementation->hook = std::move(hook); }
#endif
}
