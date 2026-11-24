// PostProcessCustomPass.cpp - deterministic candidate validation, no live writes.
#include "PostProcessCustomPass.h"
#include "File Formats/TerraEngineShaderFile.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Renderer
{
	namespace
	{
		template<std::size_t N> void CopyText(std::array<char, N>& destination, std::string_view source) noexcept
		{
			const auto count = std::min(source.size(), N - 1);
			std::copy_n(source.data(), count, destination.data()); destination[count] = '\0';
		}
		bool Fail(CustomPostProcessDiagnostic& d, CustomPostProcessFailure reason,
			std::string_view id, std::string_view detail) noexcept
		{
			d.reason = reason; CopyText(d.stableId, id); CopyText(d.detail, detail); return false;
		}
		bool Identifier(std::string_view name) noexcept
		{
			if(name.empty() || name.size() > 128) return false;
			for(const unsigned char c : name)
				if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
					|| (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
			return true;
		}
		bool Finite(const CustomPostProcessValue& value)
		{
			return std::visit([](const auto& v) {
				using T = std::decay_t<decltype(v)>;
				if constexpr(std::is_same_v<T, float>) return std::isfinite(v);
				else if constexpr(requires { v.begin(); }) {
					return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
				} else return true;
			}, value);
		}
		bool LessEqual(const CustomPostProcessValue& left, const CustomPostProcessValue& right)
		{
			if(left.index() != right.index()) return false;
			return std::visit([&](const auto& a) {
				const auto& b = std::get<std::decay_t<decltype(a)>>(right);
				if constexpr(requires { a.begin(); }) {
					for(std::size_t i = 0; i < a.size(); ++i) if(a[i] > b[i]) return false;
					return true;
				} else return a <= b;
			}, left);
		}
		bool ValueValid(const CustomPostProcessParameter& schema, const CustomPostProcessValue& value)
		{
			return value.index() == static_cast<std::size_t>(schema.layout.kind) && Finite(value)
				&& (!schema.minimum || LessEqual(*schema.minimum, value))
				&& (!schema.maximum || LessEqual(value, *schema.maximum));
		}
		std::uint32_t ParameterSize(CustomPostProcessParameterKind kind) noexcept
		{
			constexpr std::uint32_t sizes[]{4, 4, 4, 4, 8, 12, 16};
			const auto index = static_cast<std::uint32_t>(kind);
			return index < std::size(sizes) ? sizes[index] : 0;
		}
		bool HasAsset(const CustomPostProcessAvailability& available, const TerraEngine::UUID& id)
		{
			return id != TerraEngine::EMPTY_UUID
				&& std::find(available.availableAssets.begin(), available.availableAssets.end(), id) != available.availableAssets.end();
		}
		std::string_view ShaderString(const Shader& shader, std::uint32_t offset) noexcept
		{
			if(offset >= shader.stringTable.size()) return {};
			const auto tail = shader.stringTable.substr(offset);
			const auto end = tail.find('\0');
			return end == std::string_view::npos ? std::string_view{} : tail.substr(0, end);
		}
		enum class ShaderOwnerRequirement { Published, DetachedPackage };

		bool ValidateShader(const PostProcessCustomPassRegistration& registration, ShaderOwnerRequirement requirement)
		{
			const auto& d = registration.descriptor;
			const auto& shader = *registration.shader;
			if((requirement == ShaderOwnerRequirement::Published && !shader.publicationGeneration) || shader.bytecode.empty()
				|| shader.bytecode.size() != shader.binary.bytecodeSize
				|| shader.reflection.source.stage != static_cast<std::uint32_t>(ShaderStage::Fragment)
				|| ShaderString(shader, shader.reflection.source.entryPointOffset) != d.entryPoint
				|| ShaderString(shader, shader.binary.entryPointOffset) != d.entryPoint) return false;
			if(shader.binary.format == static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV)) {
				std::uint32_t magic = 0;
				if(shader.bytecode.size() < 20 || shader.bytecode.size() % 4) return false;
				std::memcpy(&magic, shader.bytecode.data(), sizeof(magic));
				if(magic != 0x07230203u) return false;
			} else if(shader.binary.format == static_cast<std::uint32_t>(ShaderBinaryFormat::DXIL)) {
				if(shader.bytecode.size() < 32 || std::memcmp(shader.bytecode.data(), "DXBC", 4)) return false;
			} else return false;
			if(shader.outputs.size() != 1 || shader.outputs[0].location != 0
				|| shader.outputs[0].componentCount != 4
				|| shader.outputs[0].componentType != static_cast<std::uint32_t>(IoComponentType::Float)) return false;
			// The established fullscreen vertex stage supplies only optional UV0.
			if(shader.inputs.size() > 1 || (!shader.inputs.empty()
				&& (shader.inputs[0].location != 0 || shader.inputs[0].componentCount != 2
					|| shader.inputs[0].componentType != static_cast<std::uint32_t>(IoComponentType::Float)))) return false;
			if(d.parameterBytes == 0) {
				if(!shader.pushConstants.empty()) return false;
			} else if(shader.pushConstants.size() != 1 || shader.pushConstants[0].offsetBytes != 0
				|| shader.pushConstants[0].sizeBytes != d.parameterBytes
				|| shader.pushConstants[0].visibility != StageToMask(ShaderStage::Fragment)) return false;
			if(registration.reflectedParameters.size() != d.parameters.size()) return false;
			for(const auto& parameter : d.parameters) {
				const auto matches = std::count(registration.reflectedParameters.begin(), registration.reflectedParameters.end(), parameter.layout);
				if(matches != 1) return false;
			}
			const std::size_t expectedBindings = d.inputs.size() + (d.samplerBinding ? 1u : 0u);
			if(shader.descriptorBindings.size() != expectedBindings || shader.descriptorSets.size() > 1) return false;
			if(expectedBindings == 0) return shader.descriptorSets.empty();
			if(shader.descriptorSets.size() != 1 || shader.descriptorSets[0].setIndex != 0
				|| shader.descriptorSets[0].firstBinding != 0 || shader.descriptorSets[0].bindingCount != expectedBindings) return false;
			for(const auto& binding : shader.descriptorBindings) {
				if(binding.arrayCount != 1 || binding.visibility != StageToMask(ShaderStage::Fragment)) return false;
				if(std::count_if(shader.descriptorBindings.begin(), shader.descriptorBindings.end(),
					[&](const auto& other) { return binding.binding == other.binding; }) != 1) return false;
				const bool sampler = d.samplerBinding && *d.samplerBinding == binding.binding;
				if(sampler) { if(binding.type != static_cast<std::uint32_t>(DescriptorType::Sampler)) return false; }
				else {
					const auto input = std::find_if(d.inputs.begin(), d.inputs.end(), [&](const auto& item) { return item.binding == binding.binding; });
					if(input == d.inputs.end() || binding.type != static_cast<std::uint32_t>(input->combinedSampler
						? DescriptorType::CombinedImageSampler : DescriptorType::SampledImage)) return false;
				}
			}
			return true;
		}
	}

	static bool TryPreparePostProcessCustomPassesImpl(std::span<const PostProcessCustomPassRegistration> registrations,
		const CustomPostProcessAvailability& available,
		std::shared_ptr<const PreparedPostProcessCustomPassSet>& accepted,
		CustomPostProcessDiagnostic& diagnostic, ShaderOwnerRequirement requirement) noexcept
	{
		diagnostic = {};
		try
		{
			if(registrations.size() > 256) return Fail(diagnostic, CustomPostProcessFailure::InvalidDescriptor, {}, "Custom pass count exceeds the bounded contract.");
			auto candidate = std::make_shared<PreparedPostProcessCustomPassSet>();
			candidate->passes.reserve(registrations.size());
			for(const auto& registration : registrations)
			{
				const auto& d = registration.descriptor;
				const auto stage = static_cast<std::uint32_t>(d.stage);
				if(d.schemaVersion != 1 || d.assetId == TerraEngine::EMPTY_UUID || d.shaderAssetId == TerraEngine::EMPTY_UUID
					|| !d.revision || !Identifier(d.stableId) || !Identifier(d.entryPoint) || stage >= 3
					|| d.inputs.size() > 32 || d.parameters.size() > 32 || d.after.size() > 256 || d.assetDependencies.size() > 256
					|| d.outputAccess != CustomPostProcessOutputAccess::DistinctColorWrite
					|| (d.stage == CustomPostProcessStage::HdrScene ? d.domain != CustomPostProcessDomain::LinearHdr
						: d.domain != CustomPostProcessDomain::DisplayLinear))
					return Fail(diagnostic, CustomPostProcessFailure::InvalidDescriptor, d.stableId, "Invalid identity, stage, color domain, or distinct-output access.");
				for(const auto& old : candidate->passes)
					if(old.descriptor.stableId == d.stableId || old.descriptor.assetId == d.assetId)
						return Fail(diagnostic, CustomPostProcessFailure::DuplicateIdentity, d.stableId, "Effect and asset identities must be unique.");
				if(!registration.shader) return Fail(diagnostic, CustomPostProcessFailure::MissingShader, d.stableId, "Compiled immutable shader owner is unavailable.");
				if(registration.enabled && (d.requiredCapabilities & ~available.supportedCapabilities))
					return Fail(diagnostic, CustomPostProcessFailure::UnsupportedCapability, d.stableId, "Required native capability is unavailable.");
				for(std::size_t i = 0; i < d.assetDependencies.size(); ++i) {
					const auto& asset = d.assetDependencies[i];
					if(asset == TerraEngine::EMPTY_UUID || std::find(d.assetDependencies.begin(), d.assetDependencies.begin() + i, asset) != d.assetDependencies.begin() + i)
						return Fail(diagnostic, CustomPostProcessFailure::InvalidDescriptor, d.stableId, "Asset dependencies must be unique nonempty IDs.");
					if(registration.enabled && !HasAsset(available, asset))
						return Fail(diagnostic, CustomPostProcessFailure::MissingDependency, d.stableId, "Declared asset dependency is unavailable.");
				}
				for(std::size_t i = 0; i < d.inputs.size(); ++i) {
					const auto& input = d.inputs[i]; const auto kind = static_cast<std::uint32_t>(input.kind);
					if(kind > static_cast<std::uint32_t>(CustomPostProcessInputKind::AssetTexture)
						|| input.access != CustomPostProcessAccess::SampledRead
						|| (d.samplerBinding && input.binding == *d.samplerBinding))
						return Fail(diagnostic, CustomPostProcessFailure::InvalidDescriptor, d.stableId, "Inputs must declare disjoint sampled-read bindings.");
					for(std::size_t j = 0; j < i; ++j)
						if(d.inputs[j].binding == input.binding || (input.kind != CustomPostProcessInputKind::AssetTexture && d.inputs[j].kind == input.kind))
							return Fail(diagnostic, CustomPostProcessFailure::InvalidDescriptor, d.stableId, "Input binding or intrinsic semantic is duplicated.");
					const bool texture = input.kind == CustomPostProcessInputKind::AssetTexture;
					if(texture ? input.assetId == TerraEngine::EMPTY_UUID
						|| std::find(d.assetDependencies.begin(), d.assetDependencies.end(), input.assetId) == d.assetDependencies.end()
						: input.assetId != TerraEngine::EMPTY_UUID)
						return Fail(diagnostic, CustomPostProcessFailure::InvalidDescriptor, d.stableId, "Texture inputs need declared asset dependencies; intrinsic inputs cannot name assets.");
					if(registration.enabled && (!(available.inputMasks[stage] & (1u << kind)) || (texture && !HasAsset(available, input.assetId))))
						return Fail(diagnostic, CustomPostProcessFailure::MissingInput, d.stableId, "A declared sampled input is unavailable at this insertion point.");
				}
				if(d.parameterBytes > available.maximumParameterBytes || d.parameterBytes % 4 || (d.parameters.empty() != (d.parameterBytes == 0)))
					return Fail(diagnostic, CustomPostProcessFailure::InvalidParameter, d.stableId, "Parameter block exceeds limits or does not match the schema.");
				PreparedPostProcessCustomPass prepared{d, registration.shader, registration.enabled, {}};
				prepared.values.reserve(d.parameters.size());
				for(std::size_t i = 0; i < d.parameters.size(); ++i) {
					const auto& parameter = d.parameters[i]; const auto bytes = ParameterSize(parameter.layout.kind);
					const auto alignment = bytes >= 12 ? 16u : bytes;
					if(!Identifier(parameter.layout.name) || !bytes || parameter.layout.offsetBytes > d.parameterBytes
						|| bytes > d.parameterBytes - parameter.layout.offsetBytes || parameter.layout.offsetBytes % alignment
						|| (parameter.layout.kind == CustomPostProcessParameterKind::Boolean && (parameter.minimum || parameter.maximum))
						|| (parameter.minimum && (!Finite(*parameter.minimum) || parameter.minimum->index() != parameter.defaultValue.index()))
						|| (parameter.maximum && (!Finite(*parameter.maximum) || parameter.maximum->index() != parameter.defaultValue.index()))
						|| (parameter.minimum && parameter.maximum && !LessEqual(*parameter.minimum, *parameter.maximum))
						|| !ValueValid(parameter, parameter.defaultValue))
						return Fail(diagnostic, CustomPostProcessFailure::InvalidParameter, d.stableId, "Parameter type, finite range, alignment, or default is invalid.");
					for(std::size_t j = 0; j < i; ++j) {
						const auto& old = d.parameters[j];
						if(old.layout.name == parameter.layout.name || (parameter.layout.offsetBytes < old.layout.offsetBytes + ParameterSize(old.layout.kind)
							&& old.layout.offsetBytes < parameter.layout.offsetBytes + bytes))
							return Fail(diagnostic, CustomPostProcessFailure::InvalidParameter, d.stableId, "Parameter names or byte ranges overlap.");
					}
					prepared.values.push_back({parameter.layout.name, parameter.defaultValue});
				}
				for(std::size_t i = 0; i < registration.values.size(); ++i) {
					const auto& value = registration.values[i];
					const auto schema = std::find_if(d.parameters.begin(), d.parameters.end(), [&](const auto& parameter) { return parameter.layout.name == value.name; });
					if(schema == d.parameters.end() || !ValueValid(*schema, value.value)
						|| std::any_of(registration.values.begin(), registration.values.begin() + i, [&](const auto& old) { return old.name == value.name; }))
						return Fail(diagnostic, CustomPostProcessFailure::InvalidParameter, d.stableId, "Unknown, duplicate, wrong-type, or out-of-range parameter patch.");
					prepared.values[static_cast<std::size_t>(schema - d.parameters.begin())].value = value.value;
				}
				if(!ValidateShader(registration, requirement)) return Fail(diagnostic, CustomPostProcessFailure::InterfaceMismatch, d.stableId, "Compiled entry, fullscreen IO, sampled bindings, or parameter reflection disagrees with the descriptor.");
				candidate->passes.push_back(std::move(prepared));
			}
			// An explicit dependency cannot point backward across fixed stages.
			// Kahn selection uses (stage,stableId), preserving repeatable output when
			// independent workers register descriptors in a different arrival order.
			std::vector<std::vector<std::size_t>> edges(registrations.size());
			std::vector<std::size_t> indegree(registrations.size(), 0);
			for(std::size_t i = 0; i < registrations.size(); ++i) {
				const auto& d = registrations[i].descriptor;
				for(std::size_t k = 0; k < d.after.size(); ++k) {
					const auto& dependency = d.after[k];
					const auto found = std::find_if(registrations.begin(), registrations.end(), [&](const auto& other) { return other.descriptor.stableId == dependency; });
					if(found == registrations.end()) return Fail(diagnostic, CustomPostProcessFailure::MissingDependency, d.stableId, "A declared effect dependency is absent.");
					const auto j = static_cast<std::size_t>(found - registrations.begin());
					if(j == i || registrations[j].descriptor.stage > d.stage
						|| std::find(d.after.begin(), d.after.begin() + k, dependency) != d.after.begin() + k)
						return Fail(diagnostic, CustomPostProcessFailure::InvalidOrdering, d.stableId, "Dependency is self-referential, duplicated, or crosses insertion points backward.");
					edges[j].push_back(i); ++indegree[i];
				}
			}
			std::vector<PreparedPostProcessCustomPass> ordered; ordered.reserve(registrations.size());
			std::vector<bool> emitted(registrations.size(), false);
			while(ordered.size() != registrations.size()) {
				std::size_t selected = registrations.size();
				for(std::size_t i = 0; i < registrations.size(); ++i) {
					if(emitted[i] || indegree[i]) continue;
					const auto& d = registrations[i].descriptor;
					if(selected == registrations.size() || d.stage < registrations[selected].descriptor.stage
						|| (d.stage == registrations[selected].descriptor.stage && d.stableId < registrations[selected].descriptor.stableId)) selected = i;
				}
				if(selected == registrations.size()) return Fail(diagnostic, CustomPostProcessFailure::InvalidOrdering, {}, "Effect dependency graph contains a cycle.");
				emitted[selected] = true; ordered.push_back(std::move(candidate->passes[selected]));
				for(const auto next : edges[selected]) --indegree[next];
			}
			candidate->passes = std::move(ordered);
			accepted = std::move(candidate); // No-throw publication after all allocations.
			return true;
		}
		catch(...) { return Fail(diagnostic, CustomPostProcessFailure::AllocationFailed, {}, "Custom pass preparation failed; previous accepted set is retained."); }
	}

	bool TryPreparePostProcessCustomPasses(std::span<const PostProcessCustomPassRegistration> registrations,
		const CustomPostProcessAvailability& available, std::shared_ptr<const PreparedPostProcessCustomPassSet>& accepted,
		CustomPostProcessDiagnostic& diagnostic) noexcept
	{
		return TryPreparePostProcessCustomPassesImpl(registrations, available, accepted, diagnostic, ShaderOwnerRequirement::Published);
	}

	bool ValidateDetachedPostProcessCustomPassAssets(std::span<const PostProcessCustomPassRegistration> registrations,
		CustomPostProcessDiagnostic& diagnostic) noexcept
	{
		for(const auto& registration : registrations)
			if(registration.enabled)
				return Fail(diagnostic, CustomPostProcessFailure::InvalidDescriptor, registration.descriptor.stableId,
					"Detached validation cannot authorize an active renderer pass.");
		std::shared_ptr<const PreparedPostProcessCustomPassSet> validated;
		return TryPreparePostProcessCustomPassesImpl(registrations, {}, validated, diagnostic, ShaderOwnerRequirement::DetachedPackage);
	}
}
