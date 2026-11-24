// Strict project/package profile codec. No filesystem, scene, or renderer writes.
#include "Resource/PostProcessProfile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Resource
{
	namespace
	{
		using namespace Renderer::GraphicsSettings;
		using Json = nlohmann::json;
		constexpr std::size_t maximumFields = 1024u;
		constexpr std::size_t maximumReferences = 256u;
		constexpr std::size_t maximumString = 4096u;
		struct Failure { GraphicsRequestReason reason; std::string detail; };
		[[noreturn]] void Reject(const char* detail, GraphicsRequestReason reason = GraphicsRequestReason::InvalidValue)
		{
			throw Failure{reason, detail};
		}

		void Keys(const Json& object, std::initializer_list<std::string_view> permitted)
		{
			if (!object.is_object()) Reject("An expected JSON object has another type.");
			for (const auto& [key, value] : object.items())
			{
				(void)value;
				if (std::find(permitted.begin(), permitted.end(), key) == permitted.end()) Reject("An unknown JSON member is not supported.");
			}
		}

		std::string Text(const Json& value)
		{
			if (!value.is_string()) Reject("A string field has another JSON type.");
			const auto text = value.get<std::string>();
			if (text.size() > maximumString) Reject("A profile string exceeds its byte limit.");
			return text;
		}

		std::uint32_t UInt(const Json& value)
		{
			if (!value.is_number_unsigned() || value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
				Reject("An unsigned32 field is not an exact uint32 JSON integer.");
			return value.get<std::uint32_t>();
		}

		std::uint64_t Wide(const Json& value)
		{
			const auto text = Text(value);
			if (text.empty() || text.size() > 20u || (text.size() > 1u && text.front() == '0')
				|| !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
				Reject("An unsigned64 field requires a canonical decimal string.");
			std::uint64_t result = 0;
			const auto converted = std::from_chars(text.data(), text.data() + text.size(), result);
			if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size()) Reject("An unsigned64 decimal string overflows.");
			return result;
		}

		TerraEngine::UUID Asset(const Json& value, bool nonempty)
		{
			const auto id = ParseUUID(Text(value));
			if (!id || (nonempty && *id == EMPTY_UUID)) Reject("An asset ID must be a valid nonempty32-digit UUID.");
			return *id;
		}

		bool Boolean(const Json& value)
		{
			if (!value.is_boolean()) Reject("A Boolean field has another JSON type.");
			return value.get<bool>();
		}

		GraphicsValue DecodeValue(const Json& item)
		{
			Keys(item, {"id", "kind", "value"});
			const auto kind = Text(item.at("kind")); const auto& value = item.at("value");
			if (kind == "boolean") return Boolean(value);
			if (kind == "unsigned32") return UInt(value);
			if (kind == "unsigned64") return Wide(value);
			if (kind == "string") return Text(value);
			if (kind == "asset") return Asset(value, false);
			if (kind == "scalar")
			{
				if (!value.is_number()) Reject("A scalar field has another JSON type.");
				const auto number = value.get<double>();
				if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) Reject("A scalar is not finite float32.");
				return static_cast<float>(number);
			}
			Reject("A profile value kind is unsupported.", GraphicsRequestReason::Unsupported);
		}

		void Structure(const PostProcessProfile& profile)
		{
			if (profile.schemaVersion > PostProcessProfile::kCurrentSchemaVersion)
				Reject("The profile schema is newer than this engine.", GraphicsRequestReason::Unsupported);
			if (profile.assetId == EMPTY_UUID || profile.revision == 0u) Reject("Profile identity and revision must be nonzero.");
			if (profile.name.size() > maximumString || profile.values.size() > maximumFields || profile.overrideMask.size() > maximumFields
				|| profile.customEffects.size() > maximumReferences) Reject("The profile exceeds bounded field or reference limits.");
			std::set<GraphicsSettingId> values;
			for (const auto& field : profile.values)
			{
				if (field.value.valueless_by_exception()) Reject("A typed value has no active variant.");
				if (field.id == GraphicsSettingId::Invalid || !values.insert(field.id).second) Reject("Profile setting IDs must be nonzero and unique.");
				if (const auto* scalar = std::get_if<float>(&field.value); scalar && !std::isfinite(*scalar)) Reject("A scalar is not finite float32.");
				if (const auto* string = std::get_if<std::string>(&field.value); string && string->size() > maximumString) Reject("A profile string exceeds its byte limit.");
			}
			std::set<GraphicsSettingId> masks;
			for (const auto id : profile.overrideMask)
				if (!values.contains(id) || !masks.insert(id).second) Reject("Override masks must be unique and name present typed values.");
			std::set<TerraEngine::UUID> references;
			for (const auto& id : profile.customEffects)
				if (id == EMPTY_UUID || !references.insert(id).second) Reject("Custom effect references must be nonempty and unique.");
		}

		PostProcessProfile Decode(const Json& root)
		{
			Keys(root, {"assetType", "schemaVersion", "assetId", "revision", "name", "values", "overrideMask", "customEffects", "overrideCustomEffects"});
			if (Text(root.at("assetType")) != "TerraEngine.PostProcessProfile") Reject("This is not a post-process profile asset.", GraphicsRequestReason::Unsupported);
			PostProcessProfile profile; profile.schemaVersion = UInt(root.at("schemaVersion"));
			if (profile.schemaVersion > PostProcessProfile::kCurrentSchemaVersion)
				Reject("The profile schema is newer than this engine.", GraphicsRequestReason::Unsupported);
			profile.assetId = Asset(root.at("assetId"), true); profile.revision = Wide(root.at("revision"));
			// Version0 sparse assets inherit omitted values/references. Version1
			// requires each explicit envelope member to catch truncated authoring.
			if (profile.schemaVersion != 0u)
				for (const char* name : {"name", "values", "overrideMask", "customEffects", "overrideCustomEffects"})
					if (!root.contains(name)) Reject("The version1 profile envelope is incomplete.");
			if (root.contains("name")) profile.name = Text(root.at("name"));
			if (root.contains("values"))
			{
				const auto& list = root.at("values");
				if (!list.is_array() || list.size() > maximumFields) Reject("The typed value array exceeds its bound or has another type.");
				for (const auto& item : list)
				{
					const auto id = static_cast<GraphicsSettingId>(UInt(item.at("id")));
					profile.values.push_back({id, DecodeValue(item)});
				}
			}
			if (root.contains("overrideMask"))
			{
				const auto& list = root.at("overrideMask");
				if (!list.is_array() || list.size() > maximumFields) Reject("The override mask exceeds its bound or has another type.");
				for (const auto& item : list) profile.overrideMask.push_back(static_cast<GraphicsSettingId>(UInt(item)));
			}
			if (root.contains("customEffects"))
			{
				const auto& list = root.at("customEffects");
				if (!list.is_array() || list.size() > maximumReferences) Reject("The custom reference array exceeds its bound or has another type.");
				for (const auto& item : list) profile.customEffects.push_back(Asset(item, true));
			}
			if (root.contains("overrideCustomEffects")) profile.overrideCustomEffects = Boolean(root.at("overrideCustomEffects"));
			Structure(profile); return profile;
		}

		Json EncodeValue(const GraphicsValue& value)
		{
			static constexpr const char* kinds[] = {"boolean", "unsigned32", "unsigned64", "scalar", "string", "asset"};
			Json result; result["kind"] = kinds[value.index()];
			std::visit([&](const auto& typed) {
				using T = std::decay_t<decltype(typed)>;
				if constexpr (std::is_same_v<T, std::uint64_t>) result["value"] = std::to_string(typed);
				else if constexpr (std::is_same_v<T, TerraEngine::UUID>) result["value"] = ToHexString(typed);
				else result["value"] = typed;
			}, value);
			return result;
		}
	}

	PostProcessProfileDecodeResult DeserializePostProcessProfile(std::span<const std::byte> bytes, const TerraEngine::UUID& expectedAssetId)
	{
		try
		{
			if (bytes.empty() || bytes.size() > kMaximumPostProcessProfileBytes
				|| std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end()) Reject("Profile bytes are empty, oversized, or contain a literal NUL.");
			std::vector<std::set<std::string>> keys;
			auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
				if (depth > 16) Reject("Profile JSON nesting exceeds its bound.");
				if (event == Json::parse_event_t::object_start) keys.emplace_back();
				if (event == Json::parse_event_t::key && (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)) Reject("Duplicate JSON keys are ambiguous.");
				if (event == Json::parse_event_t::object_end) keys.pop_back();
				return true;
			};
			const auto* first = reinterpret_cast<const char*>(bytes.data());
			auto profile = Decode(Json::parse(first, first + bytes.size(), callback));
			if (expectedAssetId != EMPTY_UUID && profile.assetId != expectedAssetId) Reject("Profile identity differs from the requested project/package asset.");
			return {GraphicsRequestReason::None, {}, std::make_shared<const PostProcessProfile>(std::move(profile))};
		}
		catch (const Failure& error) { return {error.reason, error.detail, {}}; }
		catch (const Json::exception&) { return {GraphicsRequestReason::InvalidValue, "Malformed profile JSON or a required field has an invalid type.", {}}; }
	}

	bool SerializePostProcessProfile(const PostProcessProfile& profile, std::vector<std::byte>& inOutBytes, std::string& detail)
	{
		try
		{
			Structure(profile);
			Json root = {{"assetType", "TerraEngine.PostProcessProfile"}, {"schemaVersion", profile.schemaVersion},
				{"assetId", ToHexString(profile.assetId)}, {"revision", std::to_string(profile.revision)}, {"name", profile.name},
				{"values", Json::array()}, {"overrideMask", Json::array()}, {"customEffects", Json::array()}, {"overrideCustomEffects", profile.overrideCustomEffects}};
			// Sort field/mask IDs for stable project/package bytes. Custom effects
			// retain their authored order because dependency/stage validation follows.
			auto fields = profile.values; std::sort(fields.begin(), fields.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
			for (const auto& field : fields) { auto item = EncodeValue(field.value); item["id"] = static_cast<std::uint32_t>(field.id); root["values"].push_back(std::move(item)); }
			auto masks = profile.overrideMask; std::sort(masks.begin(), masks.end());
			for (const auto id : masks) root["overrideMask"].push_back(static_cast<std::uint32_t>(id));
			for (const auto& id : profile.customEffects) root["customEffects"].push_back(ToHexString(id));
			const auto text = root.dump();
			if (text.size() > kMaximumPostProcessProfileBytes) Reject("Encoded profile exceeds its byte bound.");
			std::vector<std::byte> bytes(text.size()); std::memcpy(bytes.data(), text.data(), text.size());
			inOutBytes.swap(bytes); detail.clear(); return true;
		}
		catch (const Failure& error) { detail = error.detail; return false; }
		catch (const Json::exception&) { detail = "A profile string is not valid UTF8 or a value cannot be encoded."; return false; }
	}
}
