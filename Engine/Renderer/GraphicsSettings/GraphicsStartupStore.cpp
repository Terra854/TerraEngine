// GraphicsStartupStore.cpp — checked game-local persistence, never live renderer state.
#include "GraphicsStartupStore.h"
#include "Utilities/AtomicFileWrite.h"
#include <nlohmann/json.hpp>
#include <xxhash.h>
#include <bit>
#include <cmath>
#include <fstream>
#include <format>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <cerrno>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace
	{
		using Json = nlohmann::json;
		void Check(bool condition, const char* detail) { if(!condition) throw std::runtime_error(detail); }
		// A fixed-size key accommodates long editor project paths while keeping
		// filenames portable. Documents verify the exact identity and name so a
		// hash collision cannot be read as another product's settings.
		std::string Name(const std::string& value)
		{
			Check(!value.empty(), "The graphics settings identity or preference name is empty.");
			const auto hash = XXH3_128bits(value.data(), value.size());
			return std::format("id-{:016x}{:016x}", hash.high64, hash.low64);
		}
		std::uint64_t UInt(const Json& value)
		{
			Check(value.is_number_unsigned(), "Expected an exact unsigned integer.");
			return value.get<std::uint64_t>();
		}
		std::uint32_t U32(const Json& value)
		{
			const auto number = UInt(value);
			Check(number <= std::numeric_limits<std::uint32_t>::max(), "Unsigned32 value overflow.");
			return static_cast<std::uint32_t>(number);
		}
		template<class T> T Enum(const Json& value, std::uint32_t maximum)
		{
			const auto number = U32(value); Check(number <= maximum, "Unknown persisted enum value.");
			return static_cast<T>(number);
		}
		void ValidateValues(const std::vector<GraphicsSettingValue>& values)
		{
			std::set<GraphicsSettingId> ids;
			for(const auto& field : values)
			{
				Check(field.id != GraphicsSettingId::Invalid && ids.insert(field.id).second, "Invalid or duplicate setting ID.");
				if(const auto scalar = std::get_if<float>(&field.value)) Check(std::isfinite(*scalar), "Non-finite graphics value.");
			}
		}
		Json Values(const std::vector<GraphicsSettingValue>& values)
		{
			ValidateValues(values);
			auto result = Json::array();
			for(const auto& field : values)
			{
				Json item{{"id", static_cast<std::uint32_t>(field.id)}, {"kind", field.value.index()}};
				std::visit([&](const auto& value) {
					using T = std::decay_t<decltype(value)>;
					if constexpr(std::is_same_v<T, float>) item["value"] = std::bit_cast<std::uint32_t>(value);
					else if constexpr(std::is_same_v<T, UUID>) item["value"] = ToHexString(value);
					else item["value"] = value;
				}, field.value);
				result.push_back(std::move(item));
			}
			return result;
		}
		std::vector<GraphicsSettingValue> ParseValues(const Json& json)
		{
			Check(json.is_array(), "Expected settings array.");
			std::vector<GraphicsSettingValue> result;
			for(const auto& item : json)
			{
				GraphicsSettingValue field; field.id = static_cast<GraphicsSettingId>(U32(item.at("id")));
				const auto& value = item.at("value");
				switch(U32(item.at("kind")))
				{
				case 0: Check(value.is_boolean(), "Expected boolean."); field.value = value.get<bool>(); break;
				case 1: field.value = U32(value); break;
				case 2: field.value = UInt(value); break;
				case 3: field.value = std::bit_cast<float>(U32(value)); break;
				case 4: field.value = value.get<std::string>(); break;
				case 5: { auto id = ParseUUID(value.get<std::string>()); Check(id.has_value(), "Invalid asset UUID."); field.value = *id; break; }
				default: throw std::runtime_error("Unknown persisted value kind.");
				}
				result.push_back(std::move(field));
			}
			ValidateValues(result); return result;
		}
		Json Request(const GraphicsSettingsRequest& request)
		{
			Check(request.target == GraphicsTarget{}, "Startup persistence accepts only game-scoped requests.");
			Json reset = Json::array(); for(auto id : request.resetSettings) reset.push_back(static_cast<std::uint32_t>(id));
			Json result{{"operation", static_cast<std::uint32_t>(request.operation)}, {"baseRevision", request.baseRevision},
				{"lifecycle", {request.lifecycle.projectGeneration, request.lifecycle.playSessionGeneration,
					request.lifecycle.rendererSessionGeneration, request.lifecycle.backendResourceGeneration}},
				{"patches", Values(request.patches)}, {"reset", reset}, {"preset", nullptr}};
			if(request.preset) result["preset"] = static_cast<std::uint32_t>(*request.preset);
			return result;
		}
		GraphicsSettingsRequest ParseRequest(const Json& json)
		{
			GraphicsSettingsRequest result;
			result.operation = Enum<GraphicsRequestOperation>(json.at("operation"), 3);
			result.baseRevision = UInt(json.at("baseRevision"));
			const auto& life = json.at("lifecycle"); Check(life.is_array() && life.size() == 4, "Invalid lifecycle.");
			result.lifecycle = {UInt(life.at(0)), UInt(life.at(1)), UInt(life.at(2)), UInt(life.at(3))};
			result.patches = ParseValues(json.at("patches"));
			for(const auto& id : json.at("reset")) result.resetSettings.push_back(static_cast<GraphicsSettingId>(U32(id)));
			if(!json.at("preset").is_null()) result.preset = static_cast<QualityPreset>(U32(json.at("preset")));
			return result;
		}
		Json Receipt(const GraphicsRequestReceipt& receipt)
		{
			Json affected = Json::array(); for(auto id : receipt.affectedSettings) affected.push_back(static_cast<std::uint32_t>(id));
			return {{"id", receipt.requestId}, {"sequence", receipt.sequence}, {"state", static_cast<std::uint32_t>(receipt.state)},
				{"activation", static_cast<std::uint32_t>(receipt.activation)}, {"reason", static_cast<std::uint32_t>(receipt.reason)},
				{"affected", affected}, {"detail", receipt.detail}, {"requestedRevision", receipt.requestedRevision},
				{"effectiveRevision", receipt.effectiveRevision}, {"frame", receipt.appliedFrameId}};
		}
		GraphicsRequestReceipt ParseReceipt(const Json& json)
		{
			GraphicsRequestReceipt result;
			result.requestId = UInt(json.at("id")); result.sequence = UInt(json.at("sequence"));
			result.state = Enum<GraphicsRequestState>(json.at("state"), 7);
			result.activation = Enum<GraphicsActivation>(json.at("activation"), 4);
			result.reason = Enum<GraphicsRequestReason>(json.at("reason"), 15);
			for(const auto& id : json.at("affected")) result.affectedSettings.push_back(static_cast<GraphicsSettingId>(U32(id)));
			result.detail = json.at("detail").get<std::string>();
			result.requestedRevision = UInt(json.at("requestedRevision")); result.effectiveRevision = UInt(json.at("effectiveRevision"));
			result.appliedFrameId = UInt(json.at("frame")); return result;
		}
		// A stable sibling lock survives atomic destination replacement. Holding
		// only the destination inode/file would allow competing replacement locks.
		struct ReplayBusy final : std::runtime_error { ReplayBusy() : std::runtime_error("Another game startup owns replay.") {} };
		class ProcessLock
		{
#if defined(_WIN32)
			HANDLE handle = INVALID_HANDLE_VALUE;
#else
			int handle = -1;
#endif
		public:
			explicit ProcessLock(const std::filesystem::path& path, bool wait = true)
			{
#if defined(_WIN32)
				handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
					nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				Check(handle != INVALID_HANDLE_VALUE, "Cannot open graphics store lock.");
				OVERLAPPED overlap{};
				if(!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0, &overlap))
				{
					const auto error = GetLastError(); CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
					if(!wait && error == ERROR_LOCK_VIOLATION) throw ReplayBusy();
					throw std::runtime_error("Cannot lock graphics store.");
				}
#else
				handle = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
				if(handle < 0 || ::flock(handle, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0)
				{
					const auto error = errno; if(handle >= 0) ::close(handle); handle = -1;
					if(!wait && (error == EWOULDBLOCK || error == EAGAIN)) throw ReplayBusy();
					throw std::runtime_error("Cannot lock graphics store.");
				}
#endif
			}
			~ProcessLock()
			{
#if defined(_WIN32)
				if(handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
				if(handle >= 0) ::close(handle);
#endif
			}
			ProcessLock(const ProcessLock&) = delete;
			ProcessLock& operator=(const ProcessLock&) = delete;
		};
		class ReplayLease final : public GraphicsStartupReplayLease
		{
			ProcessLock lock;
		public:
			explicit ReplayLease(const std::filesystem::path& path) : lock(path, false) {}
		};
		class FileStore final : public GraphicsStartupStore
		{
			std::filesystem::path path;
			std::string identity, kind;
			GraphicsStartupStoreSnapshot Load()
			{
				if(!std::filesystem::exists(path)) return {};
				std::ifstream input(path, std::ios::binary); Check(input.is_open(), "Cannot read graphics store.");
				const auto json = Json::parse(input);
				Check(U32(json.at("schemaVersion")) == 1 && json.at("identity") == identity
					&& json.at("kind") == kind, "Graphics store schema or identity mismatch.");
				GraphicsStartupStoreSnapshot result; result.storeRevision = UInt(json.at("revision"));
				result.committedProfile = ParseValues(json.at("profile"));
				if(!json.at("pending").is_null())
				{
					const auto& item = json.at("pending"); GraphicsStartupRecord pending;
					pending.transaction.requestId = UInt(item.at("id")); pending.transaction.request = ParseRequest(item.at("request"));
					pending.transaction.fieldPreconditions = ParseValues(item.at("preconditions"));
					pending.claimState = Enum<GraphicsStartupClaimState>(item.at("state"), 1);
					pending.claimGeneration = UInt(item.at("claim"));
					Check(pending.transaction.requestId != 0 && ((pending.claimState == GraphicsStartupClaimState::Claimed)
						== (pending.claimGeneration != 0)), "Invalid startup claim identity.");
					result.pending = std::move(pending);
				}
				std::set<GraphicsRequestId> ids;
				for(const auto& item : json.at("terminal"))
				{
					auto receipt = ParseReceipt(item);
					Check(receipt.requestId != 0 && ids.insert(receipt.requestId).second
						&& (receipt.state == GraphicsRequestState::Applied || receipt.state == GraphicsRequestState::Rejected
							|| receipt.state == GraphicsRequestState::Cancelled), "Invalid terminal receipt.");
					result.terminalReceipts.push_back(std::move(receipt));
				}
				Check(!result.pending || !ids.contains(result.pending->transaction.requestId), "Pending request already terminal.");
				return result;
			}
			void Write(const GraphicsStartupStoreSnapshot& value)
			{
				Json json{{"schemaVersion", 1u}, {"identity", identity}, {"kind", kind}, {"revision", value.storeRevision},
					{"profile", Values(value.committedProfile)}, {"pending", nullptr}, {"terminal", Json::array()}};
				if(value.pending)
				{
					const auto& pending = *value.pending;
					json["pending"] = {{"id", pending.transaction.requestId}, {"request", Request(pending.transaction.request)},
						{"preconditions", Values(pending.transaction.fieldPreconditions)},
						{"state", static_cast<std::uint32_t>(pending.claimState)}, {"claim", pending.claimGeneration}};
				}
				for(const auto& receipt : value.terminalReceipts) json["terminal"].push_back(Receipt(receipt));
				Utilities::AtomicFileWriteTransaction transaction(path);
				const auto result = transaction.Commit(json.dump()); Check(result.committed, result.reason.c_str());
			}
			template<class Mutation> GraphicsStartupStoreResult Update(std::uint64_t revision, Mutation mutation)
			{
				GraphicsStartupStoreResult result;
				try
				{
					std::filesystem::create_directories(path.parent_path()); ProcessLock lock(path.wstring() + L".lock");
					auto current = Load(); result.storeRevision = current.storeRevision;
					if(revision != current.storeRevision) { result.reason = GraphicsRequestReason::StaleRevision; result.detail = "Graphics store revision changed."; return result; }
					if(const auto reason = mutation(current); reason != GraphicsRequestReason::None)
					{ result.reason = reason; result.detail = "Graphics store transition rejected."; return result; }
					Check(current.storeRevision < std::numeric_limits<std::uint64_t>::max(), "Graphics store revision exhausted.");
					++current.storeRevision; Write(current);
					result.storeRevision = current.storeRevision; result.succeeded = true;
				}
				catch(const std::exception& error) { result.reason = GraphicsRequestReason::StorageFailed; result.detail = error.what(); }
				return result;
			}
			GraphicsStartupStoreResult Terminal(GraphicsRequestId id, std::uint64_t revision,
				const GraphicsRequestReceipt& receipt, GraphicsRequestState state, std::optional<std::uint64_t> claim,
				const std::vector<GraphicsSettingValue>* profile)
			{
				return Update(revision, [&](auto& current) {
					if(!current.pending || current.pending->transaction.requestId != id) return GraphicsRequestReason::ConflictingRequest;
					if(receipt.requestId != id || receipt.state != state) return GraphicsRequestReason::InvalidValue;
					if((state == GraphicsRequestState::Applied && receipt.reason != GraphicsRequestReason::None)
						|| (state == GraphicsRequestState::Rejected && receipt.reason == GraphicsRequestReason::None)
						|| (state == GraphicsRequestState::Cancelled && receipt.reason != GraphicsRequestReason::ExplicitCancellation))
						return GraphicsRequestReason::InvalidValue;
					if(profile) { try { ValidateValues(*profile); } catch(...) { return GraphicsRequestReason::InvalidValue; } }
					if(state == GraphicsRequestState::Cancelled && current.pending->claimState != GraphicsStartupClaimState::Pending)
						return GraphicsRequestReason::ConflictingRequest;
					if(claim && (*claim == 0 || current.pending->claimState != GraphicsStartupClaimState::Claimed
						|| current.pending->claimGeneration != *claim)) return GraphicsRequestReason::StaleLifetime;
					if(profile) current.committedProfile = *profile;
					current.terminalReceipts.push_back(receipt); current.pending.reset(); return GraphicsRequestReason::None;
				});
			}
		public:
			FileStore(const std::filesystem::path& root, std::string product, std::string document = "startup", std::string name = {})
				: identity(std::move(product)), kind(std::move(document))
			{
				path = root / "graphics-settings" / Name(identity) / (kind == "startup" ? "startup.json" : Name(name) + ".preferences.json");
			}
			GraphicsStartupStoreResult Read(GraphicsStartupStoreSnapshot& output) override
			{
				try
				{
					std::filesystem::create_directories(path.parent_path()); ProcessLock lock(path.wstring() + L".lock");
					auto current = Load(); const auto revision = current.storeRevision; output = std::move(current);
					return {true, revision, GraphicsRequestReason::None, {}};
				}
				catch(const std::exception& error) { return {false, 0, GraphicsRequestReason::StorageFailed, error.what()}; }
			}
			GraphicsStartupStoreResult Stage(const GraphicsStartupTransaction& transaction, std::uint64_t revision) override
			{
				return Update(revision, [&](auto& current) {
					if(current.pending) return GraphicsRequestReason::ConflictingRequest;
					if(transaction.requestId == 0 || transaction.request.target != GraphicsTarget{}
						|| transaction.request.operation != GraphicsRequestOperation::StageNextLaunch
						|| transaction.request.patches.empty() || !transaction.request.resetSettings.empty() || transaction.request.preset)
						return GraphicsRequestReason::InvalidValue;
					for(const auto& receipt : current.terminalReceipts)
						if(receipt.requestId == transaction.requestId) return GraphicsRequestReason::ConflictingRequest;
					try { ValidateValues(transaction.request.patches); ValidateValues(transaction.fieldPreconditions); }
					catch(...) { return GraphicsRequestReason::InvalidValue; }
					current.pending = GraphicsStartupRecord{transaction}; return GraphicsRequestReason::None;
				});
			}
			GraphicsStartupStoreResult Claim(GraphicsRequestId id, std::uint64_t generation, std::uint64_t revision) override
			{
				return Update(revision, [&](auto& current) {
					if(!current.pending || current.pending->transaction.requestId != id
						|| current.pending->claimState != GraphicsStartupClaimState::Pending) return GraphicsRequestReason::ConflictingRequest;
					if(generation == 0) return GraphicsRequestReason::InvalidValue;
					current.pending->claimState = GraphicsStartupClaimState::Claimed;
					current.pending->claimGeneration = generation; return GraphicsRequestReason::None;
				});
			}
			GraphicsStartupStoreResult Promote(GraphicsRequestId id, std::uint64_t claim, std::uint64_t revision,
				const std::vector<GraphicsSettingValue>& profile, const GraphicsRequestReceipt& receipt) override
			{ return Terminal(id, revision, receipt, GraphicsRequestState::Applied, claim, &profile); }
			GraphicsStartupStoreResult Reject(GraphicsRequestId id, std::uint64_t revision, const GraphicsRequestReceipt& receipt) override
			{ return Terminal(id, revision, receipt, GraphicsRequestState::Rejected, {}, nullptr); }
			GraphicsStartupStoreResult Cancel(GraphicsRequestId id, std::uint64_t revision, const GraphicsRequestReceipt& receipt) override
			{ return Terminal(id, revision, receipt, GraphicsRequestState::Cancelled, {}, nullptr); }
			GraphicsStartupStoreResult SaveProfile(const std::vector<GraphicsSettingValue>& values, std::uint64_t revision)
			{
				return Update(revision, [&](auto& current) {
					try { ValidateValues(values); } catch(...) { return GraphicsRequestReason::InvalidValue; }
					current.committedProfile = values; return GraphicsRequestReason::None;
				});
			}
		};
	}
	std::unique_ptr<GraphicsStartupStore> OpenGraphicsStartupStore(const std::filesystem::path& root, std::string identity)
	{ return std::make_unique<FileStore>(root, std::move(identity)); }
	std::unique_ptr<GraphicsStartupReplayLease> TryAcquireGraphicsStartupReplayLease(
		const std::filesystem::path& root, const std::string& identity, GraphicsStartupStoreResult& result)
	{
		result = {};
		try
		{
			const auto directory = root / "graphics-settings" / Name(identity);
			std::filesystem::create_directories(directory);
			auto lease = std::make_unique<ReplayLease>(directory / "startup.replay.lock");
			result.succeeded = true; return lease;
		}
		catch(const ReplayBusy& error) { result.reason = GraphicsRequestReason::ConflictingRequest; result.detail = error.what(); }
		catch(const std::exception& error) { result.reason = GraphicsRequestReason::StorageFailed; result.detail = error.what(); }
		return {};
	}
	namespace PersistenceDetail
	{
		GraphicsStartupStoreResult ReadPreference(const std::filesystem::path& root, const std::string& identity,
			const std::string& name, std::vector<GraphicsSettingValue>& values, std::uint64_t& revision)
		{
			try
			{
				FileStore store(root, identity, "preferences", name); GraphicsStartupStoreSnapshot snapshot;
				auto result = store.Read(snapshot);
				if(result.succeeded) { values = std::move(snapshot.committedProfile); revision = snapshot.storeRevision; }
				return result;
			}
			catch(const std::exception& error) { return {false, 0, GraphicsRequestReason::StorageFailed, error.what()}; }
		}
		GraphicsStartupStoreResult SavePreference(const std::filesystem::path& root, const std::string& identity,
			const std::string& name, const std::vector<GraphicsSettingValue>& values, std::uint64_t revision)
		{
			try { return FileStore(root, identity, "preferences", name).SaveProfile(values, revision); }
			catch(const std::exception& error) { return {false, 0, GraphicsRequestReason::StorageFailed, error.what()}; }
		}
	}
}
