// Strict effect metadata codec and same-binary SPIR-V parameter reflection.
#include "PostProcessEffectAsset.h"
#include "File Formats/TerraEngineShaderFile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Resource
{
	namespace
	{
		using namespace Renderer;
		using namespace Renderer::GraphicsSettings;
		using Json = nlohmann::json;
		struct Failure { GraphicsRequestReason reason; std::string detail; };
		[[noreturn]] void Reject(const char* detail, GraphicsRequestReason reason = GraphicsRequestReason::InvalidValue) { throw Failure{reason, detail}; }
		void Keys(const Json& object, std::initializer_list<std::string_view> keys)
		{
			if(!object.is_object() || object.size() != keys.size()) Reject("An effect object is incomplete or has another type.");
			for(const auto& [key, value] : object.items()) {
				(void)value; if(std::find(keys.begin(), keys.end(), key) == keys.end()) Reject("An effect object contains an unknown member.");
			}
		}
		std::string Text(const Json& value)
		{
			if(!value.is_string()) Reject("A string field has another JSON type.");
			auto result = value.get<std::string>(); if(result.size() > 4096) Reject("Effect string exceeds its bound."); return result;
		}
		bool Name(std::string_view value)
		{
			if(value.empty() || value.size() > 128) return false;
			for(unsigned char c : value) if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
			return true;
		}
		std::uint32_t UInt(const Json& value)
		{
			if(!value.is_number_unsigned() || value.get<std::uint64_t>() > UINT32_MAX) Reject("Expected an exact unsigned32 integer."); return value.get<std::uint32_t>();
		}
		std::uint64_t Wide(const Json& value)
		{
			const auto text = Text(value); if(text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0')) Reject("Expected a canonical unsigned64 decimal string.");
			std::uint64_t result = 0; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
			if(parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) Reject("Unsigned64 decimal string overflows or is malformed."); return result;
		}
		bool Boolean(const Json& value) { if(!value.is_boolean()) Reject("Expected a Boolean value."); return value.get<bool>(); }
		TerraEngine::UUID Asset(const Json& value, bool nonempty)
		{
			const auto text = Text(value); const auto parsed = ParseUUID(text);
			if(text.size() != 32 || !parsed || (nonempty && *parsed == EMPTY_UUID)) Reject("Expected an exact32-digit asset UUID."); return *parsed;
		}
		void List(const Json& value, std::size_t maximum) { if(!value.is_array() || value.size() > maximum) Reject("Effect array exceeds its bound or has another type."); }
		constexpr std::array<std::string_view, 7> kKinds{"boolean", "signed32", "unsigned32", "scalar", "vector2", "vector3", "vector4"};
		constexpr std::array<std::string_view, 6> kInputs{"source-color", "depth", "normals", "roughness", "motion", "asset-texture"};
		template<std::size_t N> std::uint32_t Named(const Json& value, const std::array<std::string_view, N>& names)
		{
			const auto name = Text(value); const auto found = std::find(names.begin(), names.end(), name);
			if(found == names.end()) Reject("An effect enum name is unsupported.", GraphicsRequestReason::Unsupported); return static_cast<std::uint32_t>(found - names.begin());
		}
		float Scalar(const Json& value)
		{
			if(!value.is_number()) Reject("Expected a finite float32 value."); const auto number = value.get<double>();
			if(!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) Reject("A scalar is outside finite float32 range."); return static_cast<float>(number);
		}
		CustomPostProcessValue Value(const Json& value, CustomPostProcessParameterKind kind)
		{
			switch(kind) {
			case CustomPostProcessParameterKind::Boolean: return Boolean(value);
			case CustomPostProcessParameterKind::Signed:
				if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > INT32_MAX)
					|| (!value.is_number_unsigned() && (value.get<std::int64_t>() < INT32_MIN || value.get<std::int64_t>() > INT32_MAX))) Reject("Expected a signed32 integer.");
				return value.get<std::int32_t>();
			case CustomPostProcessParameterKind::Unsigned: return UInt(value);
			case CustomPostProcessParameterKind::Scalar: return Scalar(value);
			case CustomPostProcessParameterKind::Vector2: List(value, 2); if(value.size() != 2) Reject("Vector2 needs exactly2 components."); return std::array<float, 2>{Scalar(value[0]), Scalar(value[1])};
			case CustomPostProcessParameterKind::Vector3: List(value, 3); if(value.size() != 3) Reject("Vector3 needs exactly3 components."); return std::array<float, 3>{Scalar(value[0]), Scalar(value[1]), Scalar(value[2])};
			case CustomPostProcessParameterKind::Vector4: List(value, 4); if(value.size() != 4) Reject("Vector4 needs exactly4 components."); return std::array<float, 4>{Scalar(value[0]), Scalar(value[1]), Scalar(value[2]), Scalar(value[3])};
			} Reject("Parameter kind is unsupported.");
		}
		Json EncodeValue(const CustomPostProcessValue& value)
		{
			if(value.valueless_by_exception()) Reject("A typed parameter has no value.");
			return std::visit([](const auto& v) -> Json { return v; }, value);
		}
		bool Finite(const CustomPostProcessValue& value)
		{
			if(value.valueless_by_exception()) return false;
			return std::visit([](const auto& v) { using T = std::decay_t<decltype(v)>;
				if constexpr(std::is_same_v<T, float>) return std::isfinite(v);
				else if constexpr(requires { v.begin(); }) return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
				else return true; }, value);
		}
		bool LE(const CustomPostProcessValue& a, const CustomPostProcessValue& b)
		{
			if(a.index() != b.index()) return false;
			return std::visit([&](const auto& left) { const auto& right = std::get<std::decay_t<decltype(left)>>(b);
				if constexpr(requires { left.begin(); }) { for(std::size_t i = 0; i < left.size(); ++i) if(left[i] > right[i]) return false; return true; }
				else return left <= right; }, a);
		}
		std::uint32_t Size(CustomPostProcessParameterKind kind)
		{
			constexpr std::uint32_t bytes[]{4, 4, 4, 4, 8, 12, 16}; const auto index = static_cast<std::uint32_t>(kind); return index < std::size(bytes) ? bytes[index] : 0;
		}
		void Structure(const PostProcessCustomPassDescriptor& d)
		{
			if(d.schemaVersion != 1) Reject("Effect schema version is unsupported.", GraphicsRequestReason::Unsupported);
			if(d.assetId == EMPTY_UUID || d.shaderAssetId == EMPTY_UUID || d.assetId == d.shaderAssetId || !d.revision || !Name(d.stableId) || !Name(d.entryPoint)) Reject("Effect identity, shader dependency, revision or name is invalid.");
			const auto stage = static_cast<std::uint32_t>(d.stage);
			if(stage > 2 || d.outputAccess != CustomPostProcessOutputAccess::DistinctColorWrite
				|| (stage == 0 ? d.domain != CustomPostProcessDomain::LinearHdr : d.domain != CustomPostProcessDomain::DisplayLinear)) Reject("Effect stage, color domain or output access is invalid.");
			if(d.inputs.size() > 32 || d.parameters.size() > 32 || d.after.size() > 256 || d.assetDependencies.size() > 256 || d.parameterBytes > 128 || d.parameterBytes % 4
				|| (d.parameters.empty() != (d.parameterBytes == 0))) Reject("Effect descriptor exceeds bounded schema limits.");
			std::set<UUID> dependencies; for(const auto& id : d.assetDependencies) if(id == EMPTY_UUID || id == d.assetId || !dependencies.insert(id).second) Reject("Asset dependencies must be nonempty, unique and not self-referential.");
			std::set<std::string> ordering; for(const auto& id : d.after) if(!Name(id) || id == d.stableId || !ordering.insert(id).second) Reject("Effect ordering references must be unique, valid and not self-referential.");
			std::set<std::uint32_t> bindings, intrinsic;
			if(d.samplerBinding) bindings.insert(*d.samplerBinding);
			for(const auto& input : d.inputs) {
				const auto kind = static_cast<std::uint32_t>(input.kind);
				if(kind > 5 || input.access != CustomPostProcessAccess::SampledRead || !bindings.insert(input.binding).second) Reject("Input kind/access/binding is invalid or aliases another binding.");
				if(input.kind == CustomPostProcessInputKind::AssetTexture) { if(input.assetId == EMPTY_UUID || !dependencies.contains(input.assetId)) Reject("Texture input lacks a declared asset dependency."); }
				else if(input.assetId != EMPTY_UUID || !intrinsic.insert(kind).second) Reject("Intrinsic input has an asset or duplicate semantic.");
			}
			std::set<std::string> names;
			for(std::size_t i = 0; i < d.parameters.size(); ++i) {
				const auto& p = d.parameters[i]; const auto bytes = Size(p.layout.kind); const auto align = bytes >= 12 ? 16u : bytes;
				if(!bytes || !Name(p.layout.name) || !names.insert(p.layout.name).second || p.layout.offsetBytes > d.parameterBytes
					|| bytes > d.parameterBytes - p.layout.offsetBytes || p.layout.offsetBytes % align
					|| p.defaultValue.index() != static_cast<std::size_t>(p.layout.kind) || !Finite(p.defaultValue)) Reject("Parameter name, type, finite default, offset or alignment is invalid.");
				if(p.layout.kind == CustomPostProcessParameterKind::Boolean && (p.minimum || p.maximum)) Reject("Boolean parameters cannot declare numeric bounds.");
				for(const auto* bound : {&p.minimum, &p.maximum}) if(*bound && ((*bound)->index() != p.defaultValue.index() || !Finite(**bound))) Reject("Parameter bound has a wrong type or nonfinite value.");
				if((p.minimum && !LE(*p.minimum, p.defaultValue)) || (p.maximum && !LE(p.defaultValue, *p.maximum)) || (p.minimum && p.maximum && !LE(*p.minimum, *p.maximum))) Reject("Parameter default/bounds are inconsistent.");
				for(std::size_t j = 0; j < i; ++j) if(p.layout.offsetBytes < d.parameters[j].layout.offsetBytes + Size(d.parameters[j].layout.kind)
					&& d.parameters[j].layout.offsetBytes < p.layout.offsetBytes + bytes) Reject("Parameter byte ranges overlap.");
			}
		}
		PostProcessEffectAsset Decode(const Json& root)
		{
			Keys(root, {"assetType", "schemaVersion", "assetId", "shaderAssetId", "revision", "stableId", "entryPoint", "stage", "domain", "outputAccess", "inputs", "samplerBinding", "parameterBytes", "parameters", "after", "assetDependencies", "requiredCapabilities"});
			if(Text(root.at("assetType")) != "TerraEngine.PostProcessEffect") Reject("Not a post-process effect asset.", GraphicsRequestReason::Unsupported);
			PostProcessEffectAsset effect; auto& d = effect.descriptor; d.schemaVersion = UInt(root.at("schemaVersion"));
			if(d.schemaVersion != 1) Reject("Effect schema version is unsupported.", GraphicsRequestReason::Unsupported);
			d.assetId = Asset(root.at("assetId"), true); d.shaderAssetId = Asset(root.at("shaderAssetId"), true); d.revision = Wide(root.at("revision"));
			d.stableId = Text(root.at("stableId")); d.entryPoint = Text(root.at("entryPoint"));
			d.stage = static_cast<CustomPostProcessStage>(Named(root.at("stage"), std::array<std::string_view, 3>{"hdr-scene", "display-image", "before-ui"}));
			d.domain = static_cast<CustomPostProcessDomain>(Named(root.at("domain"), std::array<std::string_view, 2>{"linear-hdr", "display-linear"}));
			if(Text(root.at("outputAccess")) != "distinct-color-write") Reject("Effect output must be a distinct color write.");
			const auto& inputs = root.at("inputs"); List(inputs, 32);
			for(const auto& item : inputs) {
				Keys(item, {"kind", "access", "binding", "combinedSampler", "assetId"});
				CustomPostProcessInput input; input.kind = static_cast<CustomPostProcessInputKind>(Named(item.at("kind"), kInputs));
				if(Text(item.at("access")) != "sampled-read") Reject("Only sampled-read inputs are supported.");
				input.binding = UInt(item.at("binding")); input.combinedSampler = Boolean(item.at("combinedSampler")); input.assetId = Asset(item.at("assetId"), false); d.inputs.push_back(input);
			}
			if(!root.at("samplerBinding").is_null()) d.samplerBinding = UInt(root.at("samplerBinding")); d.parameterBytes = UInt(root.at("parameterBytes"));
			const auto& parameters = root.at("parameters"); List(parameters, 32);
			for(const auto& item : parameters) {
				Keys(item, {"name", "kind", "offsetBytes", "default", "minimum", "maximum"});
				CustomPostProcessParameter p; p.layout.name = Text(item.at("name")); p.layout.kind = static_cast<CustomPostProcessParameterKind>(Named(item.at("kind"), kKinds)); p.layout.offsetBytes = UInt(item.at("offsetBytes"));
				p.defaultValue = Value(item.at("default"), p.layout.kind);
				if(!item.at("minimum").is_null()) p.minimum = Value(item.at("minimum"), p.layout.kind);
				if(!item.at("maximum").is_null()) p.maximum = Value(item.at("maximum"), p.layout.kind); d.parameters.push_back(std::move(p));
			}
			List(root.at("after"), 256); for(const auto& item : root.at("after")) d.after.push_back(Text(item));
			List(root.at("assetDependencies"), 256); for(const auto& item : root.at("assetDependencies")) d.assetDependencies.push_back(Asset(item, true));
			d.requiredCapabilities = Wide(root.at("requiredCapabilities")); Structure(d); return effect;
		}
		Json Encode(const PostProcessCustomPassDescriptor& d)
		{
			Structure(d);
			constexpr std::array<std::string_view, 3> stages{"hdr-scene", "display-image", "before-ui"};
			Json root{{"assetType", "TerraEngine.PostProcessEffect"}, {"schemaVersion", d.schemaVersion}, {"assetId", ToHexString(d.assetId)}, {"shaderAssetId", ToHexString(d.shaderAssetId)},
				{"revision", std::to_string(d.revision)}, {"stableId", d.stableId}, {"entryPoint", d.entryPoint}, {"stage", stages[static_cast<std::size_t>(d.stage)]},
				{"domain", d.domain == CustomPostProcessDomain::LinearHdr ? "linear-hdr" : "display-linear"}, {"outputAccess", "distinct-color-write"}, {"inputs", Json::array()},
				{"samplerBinding", d.samplerBinding ? Json(*d.samplerBinding) : Json(nullptr)}, {"parameterBytes", d.parameterBytes}, {"parameters", Json::array()}, {"after", Json::array()},
				{"assetDependencies", Json::array()}, {"requiredCapabilities", std::to_string(d.requiredCapabilities)}};
			auto inputs = d.inputs; std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
			for(const auto& input : inputs) root["inputs"].push_back({{"kind", kInputs[static_cast<std::size_t>(input.kind)]}, {"access", "sampled-read"}, {"binding", input.binding}, {"combinedSampler", input.combinedSampler}, {"assetId", ToHexString(input.assetId)}});
			auto parameters = d.parameters; std::sort(parameters.begin(), parameters.end(), [](const auto& a, const auto& b) { return a.layout.offsetBytes < b.layout.offsetBytes; });
			for(const auto& p : parameters) root["parameters"].push_back({{"name", p.layout.name}, {"kind", kKinds[static_cast<std::size_t>(p.layout.kind)]}, {"offsetBytes", p.layout.offsetBytes}, {"default", EncodeValue(p.defaultValue)},
				{"minimum", p.minimum ? EncodeValue(*p.minimum) : Json(nullptr)}, {"maximum", p.maximum ? EncodeValue(*p.maximum) : Json(nullptr)}});
			auto after = d.after; std::sort(after.begin(), after.end()); for(const auto& id : after) root["after"].push_back(id);
			auto dependencies = d.assetDependencies; std::sort(dependencies.begin(), dependencies.end()); for(const auto& id : dependencies) root["assetDependencies"].push_back(ToHexString(id));
			return root;
		}
	}

	PostProcessEffectDecodeResult DeserializePostProcessEffectAsset(std::span<const std::byte> bytes, const UUID& expectedAssetId)
	{
		try {
			if(bytes.empty() || bytes.size() > kMaximumPostProcessEffectBytes || std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end()) Reject("Effect bytes are empty, oversized or contain literal NUL.");
			std::vector<std::set<std::string>> keys;
			auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
				if(depth > 16) Reject("Effect JSON nesting exceeds its bound.");
				if(event == Json::parse_event_t::object_start) keys.emplace_back();
				if(event == Json::parse_event_t::key && (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)) Reject("Duplicate effect JSON keys are ambiguous.");
				if(event == Json::parse_event_t::object_end) keys.pop_back(); return true;
			};
			const auto* first = reinterpret_cast<const char*>(bytes.data()); auto effect = Decode(Json::parse(first, first + bytes.size(), callback));
			if(expectedAssetId != EMPTY_UUID && effect.descriptor.assetId != expectedAssetId) Reject("Effect identity differs from the requested project/package asset.");
			return {GraphicsRequestReason::None, {}, std::make_shared<const PostProcessEffectAsset>(std::move(effect))};
		} catch(const Failure& e) { return {e.reason, e.detail, {}}; }
		catch(const Json::exception&) { return {GraphicsRequestReason::InvalidValue, "Malformed effect JSON or invalid required field.", {}}; }
		catch(const std::bad_alloc&) { return {GraphicsRequestReason::PreparationFailed, {}, {}}; }
	}
	bool SerializePostProcessEffectAsset(const PostProcessEffectAsset& effect, std::vector<std::byte>& inOutBytes, std::string& detail)
	{
		try {
			const auto text = Encode(effect.descriptor).dump(); if(text.size() > kMaximumPostProcessEffectBytes) Reject("Encoded effect exceeds its byte bound.");
			std::vector<std::byte> bytes(text.size()); std::memcpy(bytes.data(), text.data(), text.size()); inOutBytes.swap(bytes); detail.clear(); return true;
		} catch(const Failure& e) { detail = e.detail; return false; }
		catch(const Json::exception&) { detail = "Effect strings are not valid UTF8 or typed values cannot be encoded."; return false; }
		catch(const std::bad_alloc&) { detail.clear(); return false; }
	}
}

namespace TerraEngine::Resource
{
	namespace
	{
		struct SpirvParameterType
		{
			std::uint32_t opcode = 0, width = 0, sign = 0, element = 0, count = 0;
			std::vector<std::uint32_t> members;
		};
		std::string SpirvString(const std::uint32_t* words, std::size_t count)
		{
			const auto* bytes = reinterpret_cast<const char*>(words);
			const auto* end = static_cast<const char*>(std::memchr(bytes, 0, count * 4));
			if(!end || end - bytes > 128) Reject("SPIR-V member/entry name is missing a bounded terminator.");
			std::string result(bytes, end); if(!Name(result)) Reject("SPIR-V member/entry name is not a supported identifier."); return result;
		}
		PostProcessEffectParameterReflection Reflect(const Shader& shader)
		{
			if(shader.binary.format != static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV)
				|| shader.bytecode.size() < 20 || shader.bytecode.size() > 16u * 1024u * 1024u || shader.bytecode.size() % 4
				|| shader.binary.bytecodeSize != shader.bytecode.size()
				|| shader.reflection.source.stage != static_cast<std::uint32_t>(ShaderStage::Fragment)) Reject("Parameter reflection requires the actual compiled fragment SPIR-V payload.");
			std::vector<std::uint32_t> words(shader.bytecode.size() / 4); std::memcpy(words.data(), shader.bytecode.data(), shader.bytecode.size());
			if(words[0] != 0x07230203u || words[1] < 0x00010000u || words[1] > 0x00010600u || words[4] != 0 || words[3] < 1 || words[3] > 16u * 1024u * 1024u) Reject("SPIR-V header/version/bound is invalid.");
			const auto checkId = [&](std::uint32_t id) { if(!id || id >= words[3]) Reject("SPIR-V declaration references an out-of-bound ID."); };
			std::map<std::uint32_t, SpirvParameterType> types;
			std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> pointers;
			std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> offsets;
			std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> names;
			std::set<std::uint32_t> blocks, functions, declarationIds;
			std::uint32_t pushPointer = 0, pushCount = 0, entries = 0, entryFunction = 0, memoryModels = 0;
			std::uint32_t openFunctions = 0; std::string entry;
			const auto declare = [&](std::uint32_t id) { checkId(id); if(!declarationIds.insert(id).second) Reject("SPIR-V relevant declaration ID is duplicated."); };
			for(std::size_t i = 5; i < words.size();) {
				const auto count = words[i] >> 16, op = words[i] & 65535u;
				if(!count || count > words.size() - i) Reject("SPIR-V instruction is truncated or empty.");
				if(op == 14) { if(count != 3 || ++memoryModels != 1) Reject("SPIR-V memory model is malformed or duplicated."); }
				else if(op == 15) {
					if(count < 5 || ++entries != 1 || words[i + 1] != 4) Reject("Effect SPIR-V requires one fragment entry point.");
					entryFunction = words[i + 2]; checkId(entryFunction); entry = SpirvString(words.data() + i + 3, count - 3);
				} else if(op == 6) {
					if(count < 4) Reject("SPIR-V member-name instruction is malformed."); checkId(words[i + 1]);
					if(!names.emplace(std::pair{words[i + 1], words[i + 2]}, SpirvString(words.data() + i + 3, count - 3)).second) Reject("SPIR-V member name is duplicated.");
				} else if(op == 71 && count >= 3 && words[i + 2] == 2) {
					if(count != 3 || !blocks.insert(words[i + 1]).second) Reject("SPIR-V block decoration is malformed or duplicated."); checkId(words[i + 1]);
				} else if(op == 72 && count >= 4 && words[i + 3] == 35) {
					if(count != 5 || !offsets.emplace(std::pair{words[i + 1], words[i + 2]}, words[i + 4]).second) Reject("SPIR-V member offset is malformed or duplicated."); checkId(words[i + 1]);
				} else if(op >= 20 && op <= 24) {
					const auto expected = op == 20 ? 2u : op == 22 ? 3u : 4u;
					if(count != expected) Reject("SPIR-V scalar/vector/matrix type is malformed."); declare(words[i + 1]);
					SpirvParameterType type; type.opcode = op;
					if(op == 21) { type.width = words[i + 2]; type.sign = words[i + 3]; }
					if(op == 22) type.width = words[i + 2];
					if(op == 23 || op == 24) { type.element = words[i + 2]; type.count = words[i + 3]; checkId(type.element); }
					types.emplace(words[i + 1], std::move(type));
				} else if(op == 30) {
					if(count < 2) Reject("SPIR-V struct type is malformed."); declare(words[i + 1]);
					SpirvParameterType type; type.opcode = op;
					for(std::uint32_t j = 2; j < count; ++j) { checkId(words[i + j]); type.members.push_back(words[i + j]); }
					types.emplace(words[i + 1], std::move(type));
				} else if(op == 32) {
					if(count != 4) Reject("SPIR-V pointer type is malformed."); declare(words[i + 1]); checkId(words[i + 3]); pointers.emplace(words[i + 1], std::pair{words[i + 2], words[i + 3]});
				} else if(op == 59 && count >= 4 && words[i + 3] == 9) {
					if(count != 4 || ++pushCount != 1) Reject("SPIR-V has multiple or malformed push-constant variables."); declare(words[i + 2]); checkId(words[i + 1]); pushPointer = words[i + 1];
				} else if(op == 54) {
					if(count != 5 || openFunctions++) Reject("SPIR-V function nesting is malformed."); declare(words[i + 2]); functions.insert(words[i + 2]);
				} else if(op == 56) { if(count != 1 || !openFunctions) Reject("SPIR-V function end is malformed."); --openFunctions; }
				i += count;
			}
			if(entries != 1 || memoryModels != 1 || openFunctions || !functions.contains(entryFunction)) Reject("SPIR-V lacks a complete selected fragment function.");
			const auto metadataEntry = [&](std::uint32_t offset) {
				if(offset >= shader.stringTable.size()) return std::string_view{}; auto result = shader.stringTable.substr(offset); const auto end = result.find('\0'); return end == std::string_view::npos ? std::string_view{} : result.substr(0, end);
			};
			if(entry != metadataEntry(shader.binary.entryPointOffset) || entry != metadataEntry(shader.reflection.source.entryPointOffset)) Reject("SPIR-V entry differs from the loaded shader metadata.");
			PostProcessEffectParameterReflection result;
			if(!pushCount) { if(!shader.pushConstants.empty()) Reject("Shader metadata invents an absent push-constant block."); return result; }
			if(!pointers.contains(pushPointer) || pointers.at(pushPointer).first != 9) Reject("SPIR-V push variable has the wrong pointer storage class.");
			const auto structure = pointers.at(pushPointer).second;
			if(!types.contains(structure) || types.at(structure).opcode != 30 || !blocks.contains(structure)
				|| types.at(structure).members.empty() || types.at(structure).members.size() > 32) Reject("SPIR-V push block is not a bounded flat parameter struct.");
			const auto& members = types.at(structure).members; std::set<std::string> parameterNames;
			for(std::uint32_t i = 0; i < members.size(); ++i) {
				const auto key = std::pair{structure, i};
				if(!names.contains(key) || !offsets.contains(key) || !types.contains(members[i])) Reject("A compiled push member lacks its name, type, or exact offset.");
				if(!parameterNames.insert(names.at(key)).second) Reject("Compiled parameter names are duplicated.");
				const auto& type = types.at(members[i]); CustomPostProcessParameterKind kind;
				if(type.opcode == 20) kind = CustomPostProcessParameterKind::Boolean;
				else if(type.opcode == 21 && type.width == 32 && type.sign <= 1) kind = type.sign ? CustomPostProcessParameterKind::Signed : CustomPostProcessParameterKind::Unsigned;
				else if(type.opcode == 22 && type.width == 32) kind = CustomPostProcessParameterKind::Scalar;
				else if(type.opcode == 23 && type.count >= 2 && type.count <= 4 && types.contains(type.element)
					&& types.at(type.element).opcode == 22 && types.at(type.element).width == 32) kind = static_cast<CustomPostProcessParameterKind>(type.count + 2);
				else Reject("Compiled parameter is an unsupported width, vector, array, matrix or nested struct.", GraphicsRequestReason::Unsupported);
				const auto bytes = Size(kind), offset = offsets.at(key); if(offset > 128 || bytes > 128 - offset || offset % 4) Reject("Compiled parameter offset exceeds the bounded block.");
				for(const auto& old : result.parameters) if(offset < old.offsetBytes + Size(old.kind) && old.offsetBytes < offset + bytes) Reject("Compiled push members overlap.");
				result.parameterBytes = std::max(result.parameterBytes, offset + bytes); result.parameters.push_back({names.at(key), kind, offset});
			}
			// Reject extraneous offsets/names on the selected block rather than
			// silently reflecting a second conflicting declaration of its layout.
			for(const auto& [key, value] : offsets) { (void)value; if(key.first == structure && key.second >= members.size()) Reject("Compiled block has an out-of-range member offset."); }
			for(const auto& [key, value] : names) { (void)value; if(key.first == structure && key.second >= members.size()) Reject("Compiled block has an out-of-range member name."); }
			if(shader.pushConstants.size() != 1 || shader.pushConstants[0].offsetBytes != 0 || shader.pushConstants[0].sizeBytes != result.parameterBytes
				|| shader.pushConstants[0].visibility != StageToMask(ShaderStage::Fragment)) Reject("Compiled parameter extent differs from the loaded shader push range.");
			return result;
		}
	}

	bool ReflectPostProcessEffectParameters(const Shader& shader, PostProcessEffectParameterReflection& inOutReflection, std::string& detail)
	{
		try { auto candidate = Reflect(shader); inOutReflection = std::move(candidate); detail.clear(); return true; }
		catch(const Failure& e) { detail = e.detail; return false; }
		catch(const std::bad_alloc&) { detail.clear(); return false; }
	}
	static_assert(std::is_nothrow_move_assignable_v<PostProcessEffectParameterReflection>);
	static_assert(std::is_nothrow_move_assignable_v<PostProcessCustomPassRegistration>);
	bool PreparePostProcessEffectRegistration(const PostProcessEffectAsset& effect,
		const UUID& loadedShaderAssetId, std::shared_ptr<const Shader> shader, bool enabled,
		std::span<const CustomPostProcessParameterValue> values, PostProcessCustomPassRegistration& inOutRegistration, std::string& detail)
	{
		try {
			Structure(effect.descriptor);
			if(!shader || loadedShaderAssetId != effect.descriptor.shaderAssetId) Reject("Shader lookup owner differs from the declared immutable shader asset.");
			auto reflection = Reflect(*shader);
			if(reflection.parameterBytes != effect.descriptor.parameterBytes || reflection.parameters.size() != effect.descriptor.parameters.size()) Reject("Declared parameter byte extent or member count differs from the compiled binary.");
			for(const auto& declared : effect.descriptor.parameters)
				if(std::count(reflection.parameters.begin(), reflection.parameters.end(), declared.layout) != 1) Reject("Declared parameter name/type/offset differs from the actual compiled SPIR-V member.");
			PostProcessCustomPassRegistration candidate; candidate.descriptor = effect.descriptor; candidate.shader = std::move(shader);
			candidate.reflectedParameters = std::move(reflection.parameters); candidate.enabled = enabled; candidate.values.assign(values.begin(), values.end());
			// Live patches still pass the full graph validator before publication.
			inOutRegistration = std::move(candidate); detail.clear(); return true;
		} catch(const Failure& e) { detail = e.detail; return false; }
		catch(const std::bad_alloc&) { detail.clear(); return false; }
	}
}
