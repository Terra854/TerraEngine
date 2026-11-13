// These cases use the protected database API and its existing after-scan hook.
// Private temporary roots bound all destructive probes to fixture-owned files.
#include "../TestHarness.h"
#include "AssetDatabase.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#undef min
#undef max
#endif

namespace TerraEngine::EditorTests {
	namespace {
		struct OrganizationGate {
			std::mutex mutex; std::condition_variable changed; bool arrived{}, released{};
			void Hold() { std::unique_lock lock(mutex); arrived = true; changed.notify_all(); changed.wait(lock, [&] { return released; }); }
			bool Wait() { std::unique_lock lock(mutex); return changed.wait_for(lock, std::chrono::seconds(10), [&] { return arrived; }); }
			void Release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
		};
		struct OrganizationWorkspace {
			std::filesystem::path root;
			std::shared_ptr<OrganizationGate> gate = std::make_shared<OrganizationGate>();
			OrganizationWorkspace() {
				const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
				for(unsigned i = 0;; ++i) {
					root = std::filesystem::temp_directory_path() / ("TerraOrganization-" + token + "-" + std::to_string(i));
					if(std::filesystem::create_directory(root)) break;
				}
				AssetDatabase::Instance().Shutdown(); std::filesystem::create_directories(root / "Assets");
			}
			~OrganizationWorkspace() {
				// Release a held worker before shutdown joins it, including assertion failure.
				gate->Release(); auto& db = AssetDatabase::Instance(); db.SetProjectReconcileHookForTests({}); db.Shutdown();
				std::error_code ignored; std::filesystem::remove_all(root, ignored);
			}
			void Write(const std::filesystem::path& relative, std::string_view bytes) {
				std::filesystem::create_directories((root / relative).parent_path());
				std::ofstream(root / relative, std::ios::binary | std::ios::trunc) << bytes;
			}
			std::string Read(const std::filesystem::path& relative) const {
				std::ifstream input(root / relative, std::ios::binary); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			}
			void Open() { auto& db = AssetDatabase::Instance(); db.Initialize(root); db.PauseWatcherForTests(); }
			bool Converge() {
				auto& db = AssetDatabase::Instance(); const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
				do { db.Update(); const auto d = db.GetDiscoveryDiagnostics(); if(!d.pending && !d.scanning && d.pendingImports == 0U) return true; std::this_thread::yield(); } while(std::chrono::steady_clock::now() < end);
				return false;
			}
		};
	}
	namespace {
		bool MakeOrganizationRedirect(const std::filesystem::path& link, const std::filesystem::path& target, bool junction) {
#ifdef _WIN32
			if(!junction) return CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
			// Junctions require no symbolic-link privilege and exercise reparse
			// protection independently from the optional symlink capability.
			std::filesystem::create_directory(link);
			HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
			if(handle==INVALID_HANDLE_VALUE) return false;
			struct JunctionData { DWORD tag; WORD length,reserved,subOffset,subLength,printOffset,printLength; wchar_t paths[2048]; } data{};
			const auto substitute=L"\\??\\"+std::filesystem::absolute(target).native(); const auto print=target.native();
			if(substitute.size()+print.size()+2>=2048){ CloseHandle(handle); return false; }
			data.tag=IO_REPARSE_TAG_MOUNT_POINT; data.subLength=static_cast<WORD>(substitute.size()*sizeof(wchar_t)); data.printOffset=data.subLength+sizeof(wchar_t); data.printLength=static_cast<WORD>(print.size()*sizeof(wchar_t));
			std::copy(substitute.begin(),substitute.end(),data.paths); std::copy(print.begin(),print.end(),data.paths+substitute.size()+1);
			data.length=static_cast<WORD>(8+(substitute.size()+print.size()+2)*sizeof(wchar_t)); DWORD returned=0;
			const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,8+data.length,nullptr,0,&returned,nullptr)!=FALSE; CloseHandle(handle); return ok;
#else
			(void)junction; std::error_code error; std::filesystem::create_directory_symlink(target,link,error); return !error;
#endif
		}
	}
	void RegisterAssetOrganizationAtomicityTests(TestRegistry& registry) {
		registry.Add("Editor.AssetOrganization.ExtensionlessDependentsMoveAndRenameAtomically", [](TestContext& context) {
			OrganizationWorkspace w; w.Write("Assets/Source.txt", "source"); w.Open();
			auto& db = AssetDatabase::Instance(); context.Expect(w.Converge(), "Initial catalog must settle.");
			const auto metadata = w.Read("Assets/Source.txt.meta");
			const auto guid = nlohmann::json::parse(metadata)["guid"].get<std::string>();
			const std::string suffix = "\nguid=" + guid + "\nother bytes remain exact\n";
			w.Write("Assets/Reference", "reference=Assets/Source.txt" + suffix);
			const auto move = db.PrepareAssetOrganization("Assets/Source.txt", "move", "Assets/Moved.txt");
			context.Expect(move.valid && std::find(move.dependents.begin(), move.dependents.end(), "Assets/Reference") != move.dependents.end(),
				"Actual reference discovery must retain extensionless contained dependents.");
			context.Expect(db.PublishAssetOrganization("Assets/Source.txt", "Assets/Moved.txt", "move", move.fingerprint).published,
				"A contained reference document does not need an importable-source extension.");
			const auto movedMetadata = nlohmann::json::parse(w.Read("Assets/Moved.txt.meta"));
			context.Expect(w.Read("Assets/Reference") == "reference=Assets/Moved.txt" + suffix && movedMetadata["guid"] == guid
				&& movedMetadata["settings"] == nlohmann::json::parse(metadata)["settings"] && w.Read("Assets/Moved.txt") == "source",
				"Move must repair only reference path bytes and preserve source content, settings and GUID.");
			const auto rename = db.PrepareAssetOrganization("Assets/Moved.txt", "rename", "Assets/Renamed.txt");
			context.Expect(rename.valid && db.PublishAssetOrganization("Assets/Moved.txt", "Assets/Renamed.txt", "rename", rename.fingerprint).published,
				"Rename must use the same contained extensionless reference journal.");
			context.Expect(w.Read("Assets/Reference") == "reference=Assets/Renamed.txt" + suffix
				&& nlohmann::json::parse(w.Read("Assets/Renamed.txt.meta"))["guid"] == guid,
				"Rename must preserve all unrelated reference bytes and the asset GUID.");
			w.Write("Outside/sentinel", "outside bytes");
			const auto before = w.Read("Assets/Reference");
			for(const auto* destination : {"Assets/../Outside/escape.txt", "Outside/escape.txt"})
				context.Expect(!db.PublishAssetOrganization("Assets/Renamed.txt", destination, "move", {}).published,
					"Separating reference extensions must not weaken containment of source or destination.");
			context.Expect(!db.PrepareAssetOrganization("Assets/Reference", "move", "Assets/Ref.txt").valid,
				"Extensionless dependents must not implicitly broaden source-import policy.");
			context.Expect(w.Read("Outside/sentinel") == "outside bytes" && w.Read("Assets/Reference") == before
				&& !std::filesystem::exists(w.root / "Outside/escape.txt"), "Rejected escape must leave outside and reference bytes unchanged.");
		});
		registry.Add("Editor.AssetOrganization.DanglingMetadataRedirectRejectsBeforePublication", [](TestContext& context) {
			OrganizationWorkspace w; w.Write("Assets/Source.txt", "source"); w.Open(); auto& db = AssetDatabase::Instance();
			context.Expect(w.Converge(), "Settle the original asset before the redirect probe.");
			const auto target = w.root / "Outside/missing"; std::filesystem::create_directories(target);
			const auto meta = w.root / "Assets/Copy.txt.meta";
			context.Expect(MakeOrganizationRedirect(meta, target, true), "The contained fixture must create its mandatory redirect.");
			std::filesystem::remove(target); // Keep the directory entry dangling.
			const auto impact = db.PrepareAssetOrganization("Assets/Source.txt", "duplicate", "Assets/Copy.txt");
			const auto result = db.PublishAssetOrganization("Assets/Source.txt", "Assets/Copy.txt", "duplicate", impact.fingerprint);
			context.Expect(!result.published && result.errorCategory == "conflict" && !std::filesystem::exists(w.root / "Assets/Copy.txt")
				&& !std::filesystem::exists(target), "Dangling metadata redirect must be occupied before primary publication.");
			std::error_code ignored; std::filesystem::remove(meta, ignored);
		});
		registry.Add("Editor.AssetOrganization.DanglingGeneratedRedirectRollsBackDuplicate", [](TestContext& context) {
			OrganizationWorkspace w; w.Write("Assets/Source.txt", "source"); w.Write("Assets/Source_Generated/child.txt", "child"); w.Open();
			auto& db = AssetDatabase::Instance(); context.Expect(w.Converge(), "Settle original generated identities before reserving a duplicate.");
			const auto metadata = w.Read("Assets/Source.txt.meta"), childMetadata = w.Read("Assets/Source_Generated/child.txt.meta");
			const auto target = w.root / "Outside/missing-generation"; std::filesystem::create_directories(target);
			const auto redirect = w.root / "Assets/Copy_Generated";
			context.Expect(MakeOrganizationRedirect(redirect, target, true), "Create the mandatory fixture-owned generated-root redirect.");
			std::filesystem::remove(target); // A dangling entry is still not owned by the duplicate.
			const auto impact = db.PrepareAssetOrganization("Assets/Source.txt", "duplicate", "Assets/Copy.txt");
			const auto result = db.PublishAssetOrganization("Assets/Source.txt", "Assets/Copy.txt", "duplicate", impact.fingerprint);
			context.Expect(!result.published && result.rollback == "completed" && !std::filesystem::exists(w.root / "Assets/Copy.txt")
				&& !std::filesystem::exists(target) && w.Read("Assets/Source.txt.meta") == metadata
				&& w.Read("Assets/Source_Generated/child.txt.meta") == childMetadata,
				"Exclusive generated-root reservation must reject a dangling redirect and roll back the candidate without touching original identities.");
			std::error_code ignored; std::filesystem::remove(redirect, ignored);
		});
		registry.Add("Editor.AssetOrganization.DuplicateDoesNotCopyGeneratedChildIdentities", [](TestContext& context) {
			OrganizationWorkspace w; w.Write("Assets/Source.txt", "source"); w.Write("Assets/Source_Generated/child.txt", "child source"); w.Open();
			auto& db = AssetDatabase::Instance(); context.Expect(w.Converge(), "Settle original generated child metadata.");
			const auto before = w.Read("Assets/Source_Generated/child.txt.meta"); context.Expect(!before.empty(), "The child must own a real imported identity.");
			const auto id = TerraEngine::ParseUUID(nlohmann::json::parse(before)["guid"].get<std::string>()); context.Expect(id.has_value(), "The child identity must parse.");
			const auto impact = db.PrepareAssetOrganization("Assets/Source.txt", "duplicate", "Assets/Copy.txt");
			context.Expect(db.PublishAssetOrganization("Assets/Source.txt", "Assets/Copy.txt", "duplicate", impact.fingerprint).published, "Publish the real duplicate.");
			context.Expect(w.Converge() && w.Read("Assets/Source_Generated/child.txt.meta") == before && db.FindAssetByGuid(*id)
				&& !std::filesystem::exists(w.root / "Assets/Copy_Generated/child.txt.meta"), "Original child identity must remain registered while the new parent regenerates its children.");
		});
		registry.Add("Editor.AssetOrganization.DuplicatePreservesDependentBytes", [](TestContext& context) {
			OrganizationWorkspace w; w.Write("Assets/Source.txt", "source"); w.Write("Assets/Dependent.txt", "reference=Assets/Source.txt"); w.Open();
			auto& db = AssetDatabase::Instance(); context.Expect(w.Converge(), "Initial catalog must settle.");
			const auto metadata = w.Read("Assets/Source.txt.meta"); const auto originalGuid = nlohmann::json::parse(metadata)["guid"].get<std::string>();
			w.Write("Assets/Dependent.txt", "reference=Assets/Source.txt\nguid=" + originalGuid);
			const auto before = w.Read("Assets/Dependent.txt");
			const auto impact = db.PrepareAssetOrganization("Assets/Source.txt", "duplicate", "Assets/Copy.txt");
			context.Expect(impact.valid, "Duplicate preflight must accept contained files.");
			const auto result = db.PublishAssetOrganization("Assets/Source.txt", "Assets/Copy.txt", "duplicate", impact.fingerprint);
			context.Expect(result.published, "Duplicate must publish its copy.");
			context.ExpectEqual(before, w.Read("Assets/Dependent.txt"), "Duplicate must not redirect existing users.");
			context.ExpectEqual(metadata, w.Read("Assets/Source.txt.meta"), "Duplicate must preserve the original metadata identity.");
			context.Expect(nlohmann::json::parse(w.Read("Assets/Copy.txt.meta"))["guid"] != originalGuid, "A duplicate must receive a fresh GUID.");
			context.Expect(result.repairedReferences.empty(), "Duplicate must report no original references repaired.");
			const auto moved = db.PublishAssetOrganization("Assets/Source.txt", "Assets/Moved.txt", "move", {});
			context.Expect(moved.published && w.Read("Assets/Dependent.txt").find("Assets/Moved.txt") != std::string::npos, "Move must still repair references.");
			const auto renamed = db.PublishAssetOrganization("Assets/Moved.txt", "Assets/Renamed.txt", "rename", {});
			context.Expect(renamed.published && w.Read("Assets/Dependent.txt").find("Assets/Renamed.txt") != std::string::npos, "Rename must still repair path references.");
			context.ExpectEqual(originalGuid, nlohmann::json::parse(w.Read("Assets/Renamed.txt.meta"))["guid"].get<std::string>(), "Move and rename must preserve original identity.");
			context.Expect(w.Read("Assets/Dependent.txt").find(originalGuid) != std::string::npos, "Move and rename must retain UUID references.");
		});
		registry.Add("Editor.AssetOrganization.GeneratedFailureRollsBackPrimary", [](TestContext& context) {
			OrganizationWorkspace w; w.Write("Assets/Source.txt", "source"); w.Write("Assets/Dependent.txt", "reference=Assets/Source.txt");
			w.Write("Assets/Source_Generated/child.bin", "child"); w.Write("Assets/Copy_Generated", "blocking file"); w.Open();
			auto& db = AssetDatabase::Instance(); context.Expect(w.Converge(), "Initial catalog must settle.");
			const auto sourceMetadata = w.Read("Assets/Source.txt.meta");
			const auto sourceGuid = nlohmann::json::parse(sourceMetadata)["guid"].get<std::string>();
			// The failure occurs after primary publication but before reference repair;
			// retain real dependent bytes and UUID identity across that rollback.
			w.Write("Assets/Dependent.txt", "reference=Assets/Source.txt\nguid=" + sourceGuid);
			const auto dependentBytes = w.Read("Assets/Dependent.txt");
			const auto dependentMetadata = w.Read("Assets/Dependent.txt.meta");
			context.Expect(!dependentMetadata.empty(), "Rollback probe must begin with a persisted dependent identity.");
			const auto impact = db.PrepareAssetOrganization("Assets/Source.txt", "move", "Assets/Copy.txt");
			context.Expect(impact.valid && !impact.dependents.empty(), "Rollback probe must discover a real dependent before publication.");
			const auto result = db.PublishAssetOrganization("Assets/Source.txt", "Assets/Copy.txt", "move", impact.fingerprint);
			context.Expect(!result.published && result.rollback == "completed", "Generated-path failure must roll back the published prefix.");
			context.Expect(!std::filesystem::exists(w.root / "Assets/Copy.txt"), "Failed publication must remove only its candidate primary.");
			context.ExpectEqual(std::string("source"), w.Read("Assets/Source.txt"), "Rollback must retain source bytes.");
			context.ExpectEqual(std::string("blocking file"), w.Read("Assets/Copy_Generated"), "Rollback must preserve the pre-existing blocker.");
			context.ExpectEqual(sourceMetadata, w.Read("Assets/Source.txt.meta"), "Rollback must preserve original metadata bytes and GUID.");
			context.ExpectEqual(dependentBytes, w.Read("Assets/Dependent.txt"), "Rollback must preserve every dependent path and UUID byte.");
			context.ExpectEqual(dependentMetadata, w.Read("Assets/Dependent.txt.meta"), "Rollback must preserve the dependent's own metadata identity.");
			context.Expect(!std::filesystem::exists(w.root / "Assets/Copy.txt.meta"), "Rollback must leave no duplicate identity sidecar.");
		});
#ifdef _WIN32
		for(const bool extensionless : {false, true}) registry.Add(extensionless
			? "Editor.AssetOrganization.ExtensionlessReferenceFailureRollsBackPublishedPrefix"
			: "Editor.AssetOrganization.ReferenceFailureRollsBackPublishedPrefix", [extensionless](TestContext& context) {
			OrganizationWorkspace w; w.Write("Assets/Source.txt", "source");
			w.Write(extensionless ? "Assets/A" : "Assets/A.txt", "reference=Assets/Source.txt");
			w.Write(extensionless ? "Assets/B" : "Assets/B.txt", "reference=Assets/Source.txt");
			w.Write("Assets/Source_Generated/child.bin", "generated sentinel"); w.Open(); auto& db = AssetDatabase::Instance();
			context.Expect(w.Converge(), "Initial rollback catalog must settle.");
			const auto sourceMetadata = w.Read("Assets/Source.txt.meta");
			const auto impact = db.PrepareAssetOrganization("Assets/Source.txt", "move", "Assets/Moved.txt");
			context.ExpectEqual(std::size_t{2}, impact.dependents.size(), "Probe must find two real reference writes.");
			const auto firstBytes = w.Read(impact.dependents[0]), secondBytes = w.Read(impact.dependents[1]);
			const auto firstMeta = w.Read(impact.dependents[0] + ".meta"), secondMeta = w.Read(impact.dependents[1] + ".meta");
			const auto firstTime = std::filesystem::last_write_time(w.root / impact.dependents[0]);
			// Share reads, but deny rename/delete on the second actual discovery entry.
			// The first replacement succeeds; the second fails without truncating it.
			struct LockedReference {
				HANDLE handle = INVALID_HANDLE_VALUE;
				~LockedReference() { if(handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
			} locked;
			locked.handle = CreateFileW((w.root / impact.dependents[1]).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			context.Expect(locked.handle != INVALID_HANDLE_VALUE, "Second reference lock must be acquired before publication.");
			const auto result = db.PublishAssetOrganization("Assets/Source.txt", "Assets/Moved.txt", "move", impact.fingerprint);
			context.Expect(!result.published && result.rollback == "completed", "Reference failure must roll back the entire published prefix.");
			context.Expect(result.repairedReferences.empty(), "Rejected publication must report no committed reference repairs.");
			context.ExpectEqual(firstBytes, w.Read(impact.dependents[0]), "Earlier dependent replacement must restore original bytes.");
			context.ExpectEqual(secondBytes, w.Read(impact.dependents[1]), "Locked dependent must retain original bytes.");
			context.ExpectEqual(firstMeta, w.Read(impact.dependents[0] + ".meta"), "Earlier dependent metadata identity must survive rollback.");
			context.ExpectEqual(secondMeta, w.Read(impact.dependents[1] + ".meta"), "Locked dependent metadata identity must survive rollback.");
			context.ExpectEqual(firstTime, std::filesystem::last_write_time(w.root / impact.dependents[0]), "Rollback must restore the earlier dependent timestamp.");
			context.ExpectEqual(sourceMetadata, w.Read("Assets/Source.txt.meta"), "Source metadata and GUID must return with the primary.");
			context.ExpectEqual(std::string("source"), w.Read("Assets/Source.txt"), "Source bytes must return after failed move.");
			context.ExpectEqual(std::string("generated sentinel"), w.Read("Assets/Source_Generated/child.bin"), "Generated children must return after failed move.");
			context.Expect(!std::filesystem::exists(w.root / "Assets/Moved.txt") && !std::filesystem::exists(w.root / "Assets/Moved.txt.meta") && !std::filesystem::exists(w.root / "Assets/Moved_Generated"), "Failed move must leave no destination prefix.");
		});
#endif
		registry.Add("Editor.AssetOrganization.RecreatedOrphanSurvivesReconcile", [](TestContext& context) {
			OrganizationWorkspace w; w.Write("Assets/Source.txt", "source"); w.Open(); auto& db = AssetDatabase::Instance();
			context.Expect(w.Converge(), "Initial discovery must settle.");
			const auto sourceTime = std::filesystem::last_write_time(w.root / "Assets/Source.txt");
			// A private compiled sentinel proves cleanup preserves outputs as well as
			// identity. Restore the source timestamp to avoid a legitimate reimport.
			auto meta = nlohmann::json::parse(w.Read("Assets/Source.txt.meta"));
			meta["compiledPath"] = "orphan-compiled-sentinel.bin";
			w.Write("Assets/Source.txt.meta", meta.dump(4)); w.Write("Build/Assets/orphan-compiled-sentinel.bin", "compiled sentinel");
			const auto metadata = w.Read("Assets/Source.txt.meta");
			context.Expect(!metadata.empty(), "An orphan probe requires actual persisted metadata.");
			std::filesystem::remove(w.root / "Assets/Source.txt");
			db.SetProjectReconcileHookForTests([gate = w.gate](bool after) { if(after) gate->Hold(); });
			db.InjectWatcherCompletionForTests({}, true); db.Update(); context.Expect(w.gate->Wait(), "Worker must capture the orphan before recreation.");
			w.Write("Assets/Source.txt", "source"); std::filesystem::last_write_time(w.root / "Assets/Source.txt", sourceTime);
			db.SetProjectReconcileHookForTests({}); w.gate->Release();
			context.Expect(w.Converge(), "Recreated source reconciliation must settle.");
			context.Expect(std::filesystem::exists(w.root / "Assets/Source.txt.meta"), "Recreated source metadata must not be deleted.");
			// Import may normalize metadata formatting and the generic compiled path;
			// durable identity and output bytes, rather than formatting, are the contract.
			context.ExpectEqual(meta["guid"], nlohmann::json::parse(w.Read("Assets/Source.txt.meta"))["guid"], "A recreated source must retain its GUID.");
			context.ExpectEqual(std::string("compiled sentinel"), w.Read("Build/Assets/orphan-compiled-sentinel.bin"), "A recreated source must retain its compiled output.");
		});
	}
}
