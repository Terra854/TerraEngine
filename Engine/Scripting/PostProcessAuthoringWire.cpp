// Bounded JSON is a producer boundary only. The renderer hot path stays typed.
#include "PostProcessAuthoringWire.h"
#include "ScriptGlue.h"
#include "Resource/PostProcessEffectAsset.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace TerraEngine::Scripting::PostProcessAuthoringWire
{
	using Json = nlohmann::json;
	using Status = NativeGraphicsSettingsStatus;
	using Reason = Settings::GraphicsRequestReason;
	namespace
	{
		void Require(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
		bool Fail(Diagnostic& diagnostic, const char* detail, Reason reason = Reason::InvalidValue, Status status = Status::InvalidArgument)
		{
			diagnostic = {status, reason, detail}; return false;
		}
		void Fields(const Json& object, std::initializer_list<std::string_view> required, std::initializer_list<std::string_view> optional = {})
		{
			Require(object.is_object(), "Expected an object.");
			for (auto name : required) Require(object.contains(std::string(name)), "Missing required member.");
			for (auto it = object.begin(); it != object.end(); ++it)
				Require(std::find(required.begin(), required.end(), it.key()) != required.end()
					|| std::find(optional.begin(), optional.end(), it.key()) != optional.end(), "Unknown member.");
		}
		std::uint64_t Unsigned(const Json& value)
		{
			Require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0), "Expected an unsigned integer.");
			return value.get<std::uint64_t>();
		}
		UUID Asset(const Json& value)
		{
			Require(value.is_string(), "Expected a raw asset UUID.");
			const auto parsed = ParseUUID(value.get_ref<const std::string&>());
			Require(parsed && *parsed != EMPTY_UUID, "Asset UUID must be nonzero raw32hex."); return *parsed;
		}
		bool Name(std::string_view value)
		{
			return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
			});
		}
		float Scalar(const Json& value)
		{
			Require(value.is_number(), "Expected a finite scalar."); const auto number = value.get<double>();
			Require(std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max(), "Scalar exceeds finite float32.");
			return static_cast<float>(number);
		}
		template<std::size_t N> std::array<float, N> Vector(const Json& value)
		{
			Require(value.is_array() && value.size() == N, "Wrong vector extent."); std::array<float, N> result{};
			for (std::size_t i = 0; i < N; ++i) result[i] = Scalar(value[i]); return result;
		}
		Renderer::CustomPostProcessParameterValue Parameter(const Json& value)
		{
			Fields(value, {"name", "kind", "value"});
			Require(value["name"].is_string() && value["kind"].is_string(), "Invalid parameter name or kind.");
			Renderer::CustomPostProcessParameterValue result; result.name = value["name"].get<std::string>();
			Require(Name(result.name), "Invalid parameter name."); const auto kind = value["kind"].get<std::string>(); const auto& v = value["value"];
			if (kind == "boolean") { Require(v.is_boolean(), "Expected Boolean."); result.value = v.get<bool>(); }
			else if (kind == "signed") {
				Require(v.is_number_integer(), "Expected signed32.");
				if (v.is_number_unsigned()) Require(v.get<std::uint64_t>() <= INT32_MAX, "Signed integer overflow.");
				const auto n = v.get<std::int64_t>(); Require(n >= INT32_MIN && n <= INT32_MAX, "Signed integer overflow."); result.value = static_cast<std::int32_t>(n);
			}
			else if (kind == "unsigned") { const auto n = Unsigned(v); Require(n <= UINT32_MAX, "Unsigned integer overflow."); result.value = static_cast<std::uint32_t>(n); }
			else if (kind == "scalar") result.value = Scalar(v);
			else if (kind == "vector2") result.value = Vector<2>(v);
			else if (kind == "vector3") result.value = Vector<3>(v);
			else if (kind == "vector4") result.value = Vector<4>(v);
			else throw std::invalid_argument("Unknown parameter kind.");
			return result;
		}
		Json Parameters(const std::vector<Renderer::CustomPostProcessParameterValue>& values)
		{
			Require(values.size() <= 32, "Too many parameters."); Json result = Json::array(); std::set<std::string> names;
			static constexpr const char* kinds[]{"boolean", "signed", "unsigned", "scalar", "vector2", "vector3", "vector4"};
			for (const auto& value : values) {
				Require(Name(value.name) && names.insert(value.name).second, "Invalid or duplicate parameter name.");
				Json item{{"name", value.name}, {"kind", kinds[value.value.index()]}};
				item["value"] = std::visit([](const auto& v) -> Json { return v; }, value.value);
				// Reuse the request decoder so nonfinite native values cannot become
				// misleading JSON nulls or silently change a declared parameter type.
				Parameter(item); result.push_back(std::move(item));
			} return result;
		}
		Json Target(const Settings::GraphicsTarget& target)
		{
			Json result{{"scope", target.scope}, {"camera", nullptr}, {"view", nullptr}};
			if (target.camera) { const auto& c = *target.camera; result["camera"] = {{"cameraId", ToHexString(c.cameraId)}, {"sceneGeneration", c.sceneGeneration}, {"objectLifetimeGeneration", c.objectLifetimeGeneration}, {"componentGeneration", c.componentGeneration}, {"bindingGeneration", c.bindingGeneration}}; }
			if (target.view) { const auto& v = *target.view; result["view"] = {{"viewType", v.viewType}, {"outputKind", v.outputKind}, {"outputId", v.outputId}, {"lifetimeGeneration", v.lifetimeGeneration}}; }
			return result;
		}
		Json Lifecycle(const Settings::GraphicsLifecycleStamp& l)
		{
			return {{"projectGeneration", l.projectGeneration}, {"playSessionGeneration", l.playSessionGeneration}, {"rendererSessionGeneration", l.rendererSessionGeneration}, {"backendResourceGeneration", l.backendResourceGeneration}};
		}
		Json Effects(const std::vector<EffectState>& values)
		{
			Require(values.size() <= 64, "Too many effects."); Json result = Json::array(); std::set<UUID> ids;
			for (const auto& v : values) { Require(v.assetId != EMPTY_UUID && ids.insert(v.assetId).second, "Invalid or duplicate effect."); result.push_back({{"assetId", ToHexString(v.assetId)}, {"enabled", v.enabled}, {"parameters", Parameters(v.parameters)}}); }
			return result;
		}
		bool Finish(Json value, std::string& output, Diagnostic& diagnostic)
		{
			auto text = value.dump(); Require(text.size() < kMaximumBytes, "Copied JSON exceeds the 1MiB buffer limit.");
			output = std::move(text); diagnostic = {}; return true;
		}
	}

	bool DecodeTarget(const NativeGraphicsSettingsTargetPacket* packet, Settings::GraphicsTarget& output, Diagnostic& diagnostic)
	{
		if (!packet) return Fail(diagnostic, "Missing camera target.", Reason::InvalidScope);
		const auto& p = *packet;
		if (p.scope != 1 || !(p.flags & 1) || (p.flags & ~3u) || p.reserved)
			return Fail(diagnostic, "Authoring requires an exact camera target.", Reason::InvalidScope);
		Settings::GraphicsTarget target; target.scope = Settings::GraphicsScope::Camera;
		Settings::GraphicsCameraTarget camera; std::memcpy(camera.cameraId.data(), p.cameraId, 16);
		camera.sceneGeneration = p.sceneGeneration; camera.objectLifetimeGeneration = p.objectLifetimeGeneration;
		camera.componentGeneration = p.componentGeneration; camera.bindingGeneration = p.bindingGeneration;
		if (camera.cameraId == EMPTY_UUID || !camera.sceneGeneration || !camera.objectLifetimeGeneration || !camera.componentGeneration || !camera.bindingGeneration)
			return Fail(diagnostic, "Camera lifetime is incomplete.", Reason::StaleLifetime);
		target.camera = camera;
		if (p.flags & 2) {
			if (p.viewType > 3 || p.outputKind > 4 || !p.viewLifetimeGeneration) return Fail(diagnostic, "Invalid concrete view.", Reason::InvalidScope);
			target.view = Settings::GraphicsViewIdentity{static_cast<RenderViewType>(p.viewType), p.outputId, p.viewLifetimeGeneration, static_cast<RenderOutputKind>(p.outputKind)};
		} else if (p.viewType || p.outputKind || p.outputId || p.viewLifetimeGeneration) return Fail(diagnostic, "Unexpected hidden view fields.", Reason::InvalidScope);
		output = std::move(target); diagnostic = {}; return true;
	}

	bool DecodeRequest(const NativeGraphicsSettingsTargetPacket* target, std::span<const std::byte> bytes, Request& output, Diagnostic& diagnostic)
	{
		Request candidate;
		if (!DecodeTarget(target, candidate.target, diagnostic)) return false;
		if (bytes.empty() || bytes.size() >= kMaximumBytes || std::find(bytes.begin(), bytes.end(), std::byte{}) != bytes.end()) return Fail(diagnostic, "Invalid JSON byte extent.");
		try {
			// A separate key set for each nested object rejects duplicate names
			// before the DOM could replace one value with a later conflicting value.
			std::vector<std::set<std::string>> keys;
			auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
				Require(depth <= 16, "JSON nesting exceeds limit.");
				if (event == Json::parse_event_t::object_start) keys.emplace_back();
				if (event == Json::parse_event_t::key) Require(!keys.empty() && keys.back().insert(parsed.get<std::string>()).second, "Duplicate JSON member.");
				if (event == Json::parse_event_t::object_end) keys.pop_back(); return true;
			};
			const auto* begin = reinterpret_cast<const char*>(bytes.data()); const auto json = Json::parse(begin, begin + bytes.size(), callback);
			Require(json.is_object() && json.contains("abiVersion"), "Missing authoring version.");
			if (Unsigned(json["abiVersion"]) != kAbiVersion) return Fail(diagnostic, "Unsupported authoring protocol version.", Reason::Unsupported, Status::VersionMismatch);
			Require(json.contains("operation") && json["operation"].is_string(), "Missing operation."); const auto operation = json["operation"].get<std::string>();
			if (operation == "setCameraProfile") { candidate.operation = Operation::SetCameraProfile; Fields(json, {"abiVersion", "operation", "lifecycle", "baseRevision", "profileAssetId"}); if (!json["profileAssetId"].is_null()) candidate.profileAssetId = Asset(json["profileAssetId"]); }
			else if (operation == "patchCustomEffects") {
				candidate.operation = Operation::PatchCustomEffects; Fields(json, {"abiVersion", "operation", "lifecycle", "baseRevision", "effects"});
				const auto& effects = json["effects"]; Require(effects.is_array() && !effects.empty() && effects.size() <= 64, "Invalid effect patch count."); std::set<UUID> ids;
				for (const auto& effect : effects) {
					Fields(effect, {"assetId", "parameters", "resetParameters"}, {"enabled"}); EffectPatch patch; patch.assetId = Asset(effect["assetId"]);
					Require(ids.insert(patch.assetId).second, "Duplicate effect patch.");
					if (effect.contains("enabled")) { Require(effect["enabled"].is_boolean(), "Expected enabled Boolean."); patch.enabled = effect["enabled"].get<bool>(); }
					const auto& values = effect["parameters"]; const auto& resets = effect["resetParameters"];
					Require(values.is_array() && values.size() <= 32 && resets.is_array() && resets.size() <= 32, "Invalid parameter extent."); std::set<std::string> names;
					for (const auto& value : values) { auto p = Parameter(value); Require(names.insert(p.name).second, "Duplicate parameter patch."); patch.parameters.push_back(std::move(p)); }
					for (const auto& name : resets) { Require(name.is_string(), "Expected reset name."); auto n = name.get<std::string>(); Require(Name(n) && names.insert(n).second, "Duplicate or conflicting parameter reset."); patch.resetParameters.push_back(std::move(n)); }
					Require(patch.enabled.has_value() || !values.empty() || !resets.empty(), "Empty effect patch."); candidate.effects.push_back(std::move(patch));
				}
			} else if (operation == "resetCustomEffects") {
				candidate.operation = Operation::ResetCustomEffects; Fields(json, {"abiVersion", "operation", "lifecycle", "baseRevision", "effectAssetIds", "clearAll"});
				Require(json["clearAll"].is_boolean(), "Expected clearAll Boolean."); candidate.clearAll = json["clearAll"].get<bool>(); const auto& ids = json["effectAssetIds"];
				Require(ids.is_array() && ids.size() <= 64 && (candidate.clearAll ? ids.empty() : !ids.empty()), "Reset must select clearAll or explicit effects."); std::set<UUID> seen;
				for (const auto& value : ids) { const auto id = Asset(value); Require(seen.insert(id).second, "Duplicate reset effect."); candidate.resetEffectAssetIds.push_back(id); }
			} else throw std::invalid_argument("Unknown authoring operation.");
			const auto& l = json["lifecycle"]; Fields(l, {"projectGeneration", "playSessionGeneration", "rendererSessionGeneration", "backendResourceGeneration"});
			candidate.lifecycle = {Unsigned(l["projectGeneration"]), Unsigned(l["playSessionGeneration"]), Unsigned(l["rendererSessionGeneration"]), Unsigned(l["backendResourceGeneration"])};
			Require(candidate.lifecycle.projectGeneration && candidate.lifecycle.rendererSessionGeneration && candidate.lifecycle.backendResourceGeneration, "Incomplete lifecycle.");
			candidate.baseRevision = Unsigned(json["baseRevision"]); Require(candidate.baseRevision != 0, "Missing base revision.");
			output = std::move(candidate); diagnostic = {}; return true;
		} catch (const std::exception&) { return Fail(diagnostic, "Malformed authoring request, unknown field, or invalid typed value."); }
	}

	bool SerializeSnapshot(const Snapshot& snapshot, std::string& output, Diagnostic& diagnostic)
	{
		try {
			Require(snapshot.descriptors.size() <= 64, "Too many descriptors."); Json descriptors = Json::array(); std::set<UUID> ids;
			for (const auto& d : snapshot.descriptors) {
				Require(d.effect && ids.insert(d.effect->descriptor.assetId).second, "Invalid or duplicate descriptor owner.");
				std::vector<std::byte> bytes; std::string detail; Require(Resource::SerializePostProcessEffectAsset(*d.effect, bytes, detail), "Invalid descriptor metadata.");
				const auto* begin = reinterpret_cast<const char*>(bytes.data()); auto item = Json::parse(begin, begin + bytes.size());
				item["available"] = d.available; item["reason"] = d.reason; item["detail"] = d.detail; descriptors.push_back(std::move(item));
			}
			return Finish({{"abiVersion", kAbiVersion}, {"target", Target(snapshot.target)}, {"lifecycle", Lifecycle(snapshot.lifecycle)}, {"revision", snapshot.revision},
				{"authoredProfileAssetId", snapshot.authoredProfileAssetId == EMPTY_UUID ? Json(nullptr) : Json(ToHexString(snapshot.authoredProfileAssetId))},
				{"visibleFrameId", snapshot.sceneVisibleFrameId}, {"effectiveFrameId", snapshot.effectiveFrameId}, {"descriptors", std::move(descriptors)},
				{"requestedEffects", Effects(snapshot.requestedEffects)}, {"effectiveEffects", Effects(snapshot.effectiveEffects)}, {"reason", snapshot.reason}, {"detail", snapshot.detail}}, output, diagnostic);
		} catch (const std::exception&) { return Fail(diagnostic, "Cannot serialize authoring snapshot.", Reason::PreparationFailed); }
	}
	bool SerializeReceipt(const Receipt& receipt, std::string& output, Diagnostic& diagnostic)
	{
		try {
			const auto& r = receipt.request;
			Json request{{"requestId", r.requestId}, {"sequence", r.sequence}, {"state", r.state}, {"activation", r.activation}, {"reason", r.reason}, {"affectedSettings", r.affectedSettings}, {"detail", r.detail}, {"requestedRevision", r.requestedRevision}, {"effectiveRevision", r.effectiveRevision}, {"appliedFrameId", r.appliedFrameId}};
			return Finish({{"abiVersion", kAbiVersion}, {"request", std::move(request)}, {"sceneVisibleFrameId", receipt.sceneVisibleFrameId}, {"effectiveFrameId", receipt.effectiveFrameId}, {"reason", r.reason}, {"detail", r.detail}}, output, diagnostic);
		} catch (const std::exception&) { return Fail(diagnostic, "Cannot serialize authoring receipt.", Reason::PreparationFailed); }
	}
	Status CopyJson(std::string_view json, char* destination, std::uint32_t capacity, std::uint32_t* required)
	{
		if (!required) return Status::InvalidArgument;
		*required = 0;
		if (json.size() >= kMaximumBytes || (!destination && capacity)) return Status::InvalidArgument;
		*required = static_cast<std::uint32_t>(json.size() + 1);
		if (!destination || capacity < *required) return Status::BufferTooSmall;
		std::memcpy(destination, json.data(), json.size()); destination[json.size()] = '\0'; return Status::Success;
	}
	Status ValidateReceiptOutput(const NativeGraphicsSettingsReceiptPacket* output)
	{
		if (!output) return Status::InvalidArgument;
		return output->header.abiVersion == kNativeGraphicsSettingsAbiVersion && output->header.structSize == sizeof(*output) ? Status::Success : Status::VersionMismatch;
	}
	Status WriteReceipt(const Settings::GraphicsRequestReceipt& source, NativeGraphicsSettingsReceiptPacket* output)
	{
		const auto valid = ValidateReceiptOutput(output); if (valid != Status::Success) return valid;
		NativeGraphicsSettingsReceiptPacket result{}; result.header = {kNativeGraphicsSettingsAbiVersion, sizeof(result)};
		result.state = static_cast<std::uint32_t>(source.state); result.activation = static_cast<std::uint32_t>(source.activation); result.reason = static_cast<std::uint32_t>(source.reason);
		result.affectedCount = static_cast<std::uint32_t>(source.affectedSettings.size()); result.detailBytes = static_cast<std::uint32_t>(source.detail.size());
		result.requestId = source.requestId; result.sequence = source.sequence; result.requestedRevision = source.requestedRevision; result.effectiveRevision = source.effectiveRevision; result.appliedFrameId = source.appliedFrameId;
		*output = result;
		if (source.state == Settings::GraphicsRequestState::Rejected || source.state == Settings::GraphicsRequestState::RestartRequired) return Status::Rejected;
		if (source.state == Settings::GraphicsRequestState::Applied || source.state == Settings::GraphicsRequestState::Cancelled) return Status::Success;
		return Status::Accepted;
	}
}
