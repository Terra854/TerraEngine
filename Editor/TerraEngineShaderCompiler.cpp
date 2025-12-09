#include <array>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <cstring>
#include <unordered_map>

// Offline TSPF compiler: wraps DXC to compile HLSL into TerraEngine's shader container
// (SPIR-V by default) so editor users do not need to install toolchains separately.

#include "Engine/File Formats/TerraEngineShaderFile.h"

namespace
{
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::string entryPoint = "main";
		std::string stage;
		std::string targetProfile;
		std::filesystem::path dxcPathOverride;
	};

	std::string ToLower(std::string s)
	{
		for(char& c : s)
		{
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string flag{argv[i]};
			if((flag == "-i" || flag == "--input") && i + 1 < argc)
			{
				args.input = argv[++i];
			}
			else if((flag == "-o" || flag == "--output") && i + 1 < argc)
			{
				args.output = argv[++i];
			}
			else if((flag == "-e" || flag == "--entry") && i + 1 < argc)
			{
				args.entryPoint = argv[++i];
			}
			else if((flag == "-s" || flag == "--stage") && i + 1 < argc)
			{
				args.stage = argv[++i];
			}
			else if((flag == "-t" || flag == "--target") && i + 1 < argc)
			{
				args.targetProfile = argv[++i];
			}
			else if(flag == "--dxc" && i + 1 < argc)
			{
				args.dxcPathOverride = argv[++i];
			}
			else
			{
				std::cerr << "Unrecognized or incomplete argument: " << flag << "\n";
				return false;
			}
		}

		if(args.input.empty() || args.output.empty() || args.stage.empty())
		{
			std::cerr << "Usage: TerraEngineShaderCompiler -i <input.hlsl> -o <output.tspf> -s <vs|ps|cs|gs|hs|ds|ms>\n";
			return false;
		}

		return true;
	}

	std::string DefaultProfileForStage(const std::string& stage)
	{
		const std::string s = ToLower(stage);
		if(s == "vs") return "vs_6_8";
		if(s == "ps" || s == "fs") return "ps_6_8";
		if(s == "cs") return "cs_6_8";
		if(s == "gs") return "gs_6_8";
		if(s == "hs") return "hs_6_8";
		if(s == "ds") return "ds_6_8";
		if(s == "ms") return "ms_6_8";
		if(s == "ts" || s == "as") return "as_6_8";
		return "vs_6_8";
	}

	std::optional<ShaderStage> StageEnum(const std::string& stage)
	{
		const std::string s = ToLower(stage);
		if(s == "vs") return ShaderStage::Vertex;
		if(s == "ps" || s == "fs") return ShaderStage::Fragment;
		if(s == "cs") return ShaderStage::Compute;
		if(s == "gs") return ShaderStage::Geometry;
		if(s == "hs") return ShaderStage::TessellationControl;
		if(s == "ds") return ShaderStage::TessellationEvaluation;
		if(s == "ms") return ShaderStage::Mesh;
		if(s == "ts" || s == "as") return ShaderStage::Task;
		return std::nullopt;
	}

	std::filesystem::path DefaultDxcPath()
	{
#if defined(_WIN32)
		return std::filesystem::path("External/dxc/windows/bin/x64/dxc.exe");
#else
		return std::filesystem::path("External/dxc/linux/bin/dxc");
#endif
	}

	std::string Quote(const std::filesystem::path& p)
	{
		std::ostringstream oss;
		oss << "\"" << p.string() << "\"";
		return oss.str();
	}

	bool RunDxc(const CommandLine& args, const std::filesystem::path& dxcPath, const std::filesystem::path& tempSpv)
	{
		// Rely on the vendored DXC binaries shipped with the repo so the tool stays self-contained.
		const std::string target = args.targetProfile.empty() ? DefaultProfileForStage(args.stage) : args.targetProfile;
		std::ostringstream cmd;
		// Target Vulkan 1.3 to match the bundled DXC capabilities and remain portable across platforms.
		cmd << Quote(dxcPath) << " -spirv -fspv-target-env=vulkan1.3"
			<< " -T " << target
			<< " -E " << args.entryPoint
			<< " -Fo " << Quote(tempSpv)
			<< " " << Quote(args.input);

		std::cout << "Running: " << cmd.str() << "\n";
		const int code = std::system(cmd.str().c_str());
		if(code != 0)
		{
			std::cerr << "dxc failed with exit code " << code << "\n";
			return false;
		}
		return true;
	}

	struct StringTableBuilder
	{
		std::vector<char> data;

		std::uint32_t Add(const std::string& value)
		{
			// Return the offset before appending so other structures can reference the string table.
			const std::uint32_t offset = static_cast<std::uint32_t>(data.size());
			data.insert(data.end(), value.begin(), value.end());
			data.push_back('\0');
			return offset;
		}
	};

	struct TypeInfo
	{
		enum class Kind
		{
			Unknown,
			Bool,
			Int,
			UInt,
			Float,
			Vector,
			Matrix,
			Array,
			Struct,
			Pointer,
			Sampler,
			Image,
			SampledImage
		};

		Kind kind = Kind::Unknown;
		IoComponentType component = IoComponentType::Unknown;
		std::uint32_t componentCount = 1;
		std::uint32_t typeId = 0; // Points to a child type (e.g., vector component, pointer pointee, array element).
		std::uint32_t arrayLengthId = 0;
		std::vector<std::uint32_t> members;
		std::uint32_t sampledKind = 0; // SPIR-V OpTypeImage sampled operand (1 = sampled, 2 = storage).
	};

	struct Decorations
	{
		std::optional<std::uint32_t> location;
		std::optional<std::uint32_t> binding;
		std::optional<std::uint32_t> descriptorSet;
		bool builtIn = false;
	};

	struct DescriptorBindingEntry
	{
		std::uint32_t set = 0;
		DescriptorBinding binding{};
		std::string name;
	};

	struct IoEntry
	{
		std::uint32_t location = 0;
		IoComponentType componentType = IoComponentType::Unknown;
		std::uint32_t componentCount = 0;
		std::string name;
	};

	struct ReflectionResult
	{
		std::vector<IoEntry> inputs;
		std::vector<IoEntry> outputs;
		std::vector<DescriptorBindingEntry> descriptorBindings;
		// Push constants are intentionally omitted for now; DXC does not emit them
		// in the SPIR-V we consume and would need extra layout handling.
	};

	// Lightweight SPIR-V walker that extracts locations, descriptor bindings, and names.
	class SpirvReflection
	{
	public:
		bool Reflect(const std::vector<std::uint8_t>& bytecode, ShaderStage stage, ReflectionResult& out) const
		{
			if(bytecode.size() < sizeof(std::uint32_t) * 5)
			{
				std::cerr << "SPIR-V is too small to reflect.\n";
				return false;
			}

			const std::size_t wordCount = bytecode.size() / sizeof(std::uint32_t);
			const std::uint32_t* words = reinterpret_cast<const std::uint32_t*>(bytecode.data());

			std::unordered_map<std::uint32_t, TypeInfo> types;
			std::unordered_map<std::uint32_t, Decorations> idDecorations;
			std::unordered_map<std::uint32_t, std::vector<Decorations>> memberDecorations;
			std::unordered_map<std::uint32_t, std::string> idNames;
			std::unordered_map<std::uint32_t, std::vector<std::string>> memberNames;
			std::unordered_map<std::uint32_t, std::uint32_t> constants;

			struct VariableInfo
			{
				std::uint32_t id = 0;
				std::uint32_t typeId = 0;
				std::uint32_t storageClass = 0;
			};
			std::vector<VariableInfo> variables;

			const auto readString = [&](std::size_t startWord) -> std::string {
				const char* begin = reinterpret_cast<const char*>(words + startWord);
				const char* end = reinterpret_cast<const char*>(words + wordCount);
				return std::string(begin, std::find(begin, end, '\0'));
			};

			// Walk the module to collect type graph, names, and decorations.
			for(std::size_t i = 5; i < wordCount;)
			{
				const std::uint32_t opWord = words[i];
				const std::uint16_t op = static_cast<std::uint16_t>(opWord & 0xFFFFu);
				const std::uint16_t count = static_cast<std::uint16_t>(opWord >> 16);
				if(count == 0)
				{
					std::cerr << "Malformed SPIR-V: zero word count.\n";
					return false;
				}

				switch(op)
				{
				case 5: // OpName
					if(count >= 3)
					{
						const std::uint32_t targetId = words[i + 1];
						idNames[targetId] = readString(i + 2);
					}
					break;
				case 6: // OpMemberName
					if(count >= 4)
					{
						const std::uint32_t typeId = words[i + 1];
						const std::uint32_t memberIndex = words[i + 2];
						if(memberNames[typeId].size() <= memberIndex)
						{
							memberNames[typeId].resize(memberIndex + 1);
						}
						memberNames[typeId][memberIndex] = readString(i + 3);
					}
					break;
				case 71: // OpDecorate
					if(count >= 3)
					{
						const std::uint32_t targetId = words[i + 1];
						const std::uint32_t decoration = words[i + 2];
						Decorations& dec = idDecorations[targetId];
						if(decoration == 30 && count >= 4) // Location
						{
							dec.location = words[i + 3];
						}
						else if(decoration == 11) // BuiltIn
						{
							dec.builtIn = true;
						}
						else if(decoration == 33 && count >= 4) // Binding
						{
							dec.binding = words[i + 3];
						}
						else if(decoration == 34 && count >= 4) // DescriptorSet
						{
							dec.descriptorSet = words[i + 3];
						}
					}
					break;
				case 72: // OpMemberDecorate
					if(count >= 5)
					{
						const std::uint32_t typeId = words[i + 1];
						const std::uint32_t memberIndex = words[i + 2];
						const std::uint32_t decoration = words[i + 3];
						if(memberDecorations[typeId].size() <= memberIndex)
						{
							memberDecorations[typeId].resize(memberIndex + 1);
						}
						Decorations& dec = memberDecorations[typeId][memberIndex];
						if(decoration == 30 && count >= 5) // Location
						{
							dec.location = words[i + 4];
						}
						else if(decoration == 11) // BuiltIn
						{
							dec.builtIn = true;
						}
					}
					break;
				case 59: // OpVariable
					if(count >= 4)
					{
						VariableInfo var{};
						var.typeId = words[i + 1];
						var.id = words[i + 2];
						var.storageClass = words[i + 3];
						variables.push_back(var);
					}
					break;
				case 20: // OpTypeBool
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Bool;
					info.component = IoComponentType::Bool;
					types[words[i + 1]] = info;
					break;
				}
				case 21: // OpTypeInt
				{
					TypeInfo info{};
					info.kind = (words[i + 3] == 0) ? TypeInfo::Kind::UInt : TypeInfo::Kind::Int;
					info.component = (words[i + 3] == 0) ? IoComponentType::UInt : IoComponentType::Int;
					types[words[i + 1]] = info;
					break;
				}
				case 22: // OpTypeFloat
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Float;
					info.component = IoComponentType::Float;
					types[words[i + 1]] = info;
					break;
				}
				case 23: // OpTypeVector
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Vector;
					info.typeId = words[i + 2];
					info.componentCount = words[i + 3];
					if(const auto found = types.find(info.typeId); found != types.end())
					{
						info.component = found->second.component;
					}
					types[words[i + 1]] = info;
					break;
				}
				case 24: // OpTypeMatrix
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Matrix;
					info.typeId = words[i + 2];
					info.componentCount = words[i + 3];
					types[words[i + 1]] = info;
					break;
				}
				case 28: // OpTypeArray
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Array;
					info.typeId = words[i + 2];
					info.arrayLengthId = words[i + 3];
					types[words[i + 1]] = info;
					break;
				}
				case 30: // OpTypeStruct
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Struct;
					info.members.reserve(count > 2 ? count - 2 : 0);
					for(std::uint16_t m = 2; m < count; ++m)
					{
						info.members.push_back(words[i + m]);
					}
					types[words[i + 1]] = info;
					break;
				}
				case 32: // OpTypePointer
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Pointer;
					info.typeId = words[i + 3];
					types[words[i + 1]] = info;
					break;
				}
				case 26: // OpTypeSampler
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Sampler;
					types[words[i + 1]] = info;
					break;
				}
				case 25: // OpTypeImage
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::Image;
					info.typeId = words[i + 2]; // Sampled type
					if(count >= 9)
					{
						info.sampledKind = words[i + 7];
					}
					types[words[i + 1]] = info;
					break;
				}
				case 27: // OpTypeSampledImage
				{
					TypeInfo info{};
					info.kind = TypeInfo::Kind::SampledImage;
					info.typeId = words[i + 2];
					types[words[i + 1]] = info;
					break;
				}
				case 43: // OpConstant
					if(count >= 4)
					{
						const std::uint32_t id = words[i + 2];
						const std::uint32_t value = words[i + 3];
						constants[id] = value;
					}
					break;
				default:
					break;
				}

				i += count;
			}

			const auto resolveScalar = [&](std::uint32_t typeId) -> std::optional<std::pair<IoComponentType, std::uint32_t>> {
				auto iter = types.find(typeId);
				if(iter == types.end())
				{
					return std::nullopt;
				}
				const TypeInfo& info = iter->second;
				switch(info.kind)
				{
				case TypeInfo::Kind::Bool:
				case TypeInfo::Kind::Int:
				case TypeInfo::Kind::UInt:
				case TypeInfo::Kind::Float:
					return std::make_pair(info.component, 1u);
				case TypeInfo::Kind::Vector:
				{
					if(info.component == IoComponentType::Unknown)
					{
						if(const auto base = resolveScalar(info.typeId))
						{
							return std::make_pair(base->first, info.componentCount);
						}
						return std::nullopt;
					}
					return std::make_pair(info.component, info.componentCount);
				}
				case TypeInfo::Kind::Array:
					return resolveScalar(info.typeId);
				case TypeInfo::Kind::Pointer:
					return resolveScalar(info.typeId);
				default:
					return std::nullopt;
				}
			};

			const StageVisibilityMask visibilityMask = StageToMask(stage);

			// Extract descriptor bindings and IO variables from collected metadata.
			for(const VariableInfo& var : variables)
			{
				const Decorations varDeco = idDecorations[var.id];
				if(varDeco.builtIn)
				{
					continue;
				}

				// Descriptors live in descriptor sets with binding numbers.
				if(varDeco.binding && varDeco.descriptorSet && (var.storageClass == 2 || var.storageClass == 0 || var.storageClass == 12))
				{
					const auto typeIter = types.find(var.typeId);
					if(typeIter == types.end())
					{
						continue;
					}

					const TypeInfo& pointerType = typeIter->second;
					if(pointerType.kind != TypeInfo::Kind::Pointer)
					{
						continue;
					}

					const auto pointeeIter = types.find(pointerType.typeId);
					if(pointeeIter == types.end())
					{
						continue;
					}

					const TypeInfo& pointee = pointeeIter->second;
					DescriptorBindingEntry entry{};
					entry.set = *varDeco.descriptorSet;
					entry.binding.binding = *varDeco.binding;
					entry.binding.arrayCount = 1;
					entry.binding.visibility = visibilityMask;

					if(pointee.kind == TypeInfo::Kind::SampledImage)
					{
						entry.binding.type = static_cast<std::uint32_t>(DescriptorType::CombinedImageSampler);
					}
					else if(pointee.kind == TypeInfo::Kind::Sampler)
					{
						entry.binding.type = static_cast<std::uint32_t>(DescriptorType::Sampler);
					}
					else if(pointee.kind == TypeInfo::Kind::Image)
					{
						entry.binding.type = static_cast<std::uint32_t>(pointee.sampledKind == 2 ? DescriptorType::StorageImage : DescriptorType::SampledImage);
					}
					else if(var.storageClass == 12) // StorageBuffer
					{
						entry.binding.type = static_cast<std::uint32_t>(DescriptorType::StorageBuffer);
					}
					else // Uniform / cbuffer
					{
						entry.binding.type = static_cast<std::uint32_t>(DescriptorType::UniformBuffer);
					}

					if(pointee.kind == TypeInfo::Kind::Array && constants.count(pointee.arrayLengthId) > 0)
					{
						entry.binding.arrayCount = constants[pointee.arrayLengthId];
					}

					const auto nameIter = idNames.find(var.id);
					if(nameIter != idNames.end())
					{
						entry.name = nameIter->second;
					}
					out.descriptorBindings.push_back(entry);
				}

				// Stage inputs/outputs live in the interface block for this stage.
				if(var.storageClass != 1 && var.storageClass != 3) // Input = 1, Output = 3
				{
					continue;
				}

				std::vector<IoEntry>& target = (var.storageClass == 1) ? out.inputs : out.outputs;
				const auto varTypeIter = types.find(var.typeId);
				if(varTypeIter == types.end())
				{
					continue;
				}

				const TypeInfo& varType = varTypeIter->second;
				if(varType.kind == TypeInfo::Kind::Pointer)
				{
					const auto baseIter = types.find(varType.typeId);
					if(baseIter == types.end())
					{
						continue;
					}

					const TypeInfo& base = baseIter->second;
					if(base.kind == TypeInfo::Kind::Struct)
					{
						for(std::size_t memberIndex = 0; memberIndex < base.members.size(); ++memberIndex)
						{
							const auto memberInfoIter = types.find(base.members[memberIndex]);
							if(memberInfoIter == types.end())
							{
								continue;
							}

							const Decorations memberDeco = (memberIndex < memberDecorations[varType.typeId].size()) ? memberDecorations[varType.typeId][memberIndex] : Decorations{};
							if(memberDeco.builtIn || !memberDeco.location)
							{
								continue;
							}

							const auto scalar = resolveScalar(base.members[memberIndex]);
							if(!scalar)
							{
								continue;
							}

							IoEntry entry{};
							entry.location = *memberDeco.location;
							entry.componentType = scalar->first;
							entry.componentCount = scalar->second;
							if(memberIndex < memberNames[varType.typeId].size())
							{
								entry.name = memberNames[varType.typeId][memberIndex];
							}
							target.push_back(entry);
						}
					}
					else
					{
						const auto scalar = resolveScalar(varType.typeId);
						if(!scalar || !varDeco.location)
						{
							continue;
						}

						IoEntry entry{};
						entry.location = *varDeco.location;
						entry.componentType = scalar->first;
						entry.componentCount = scalar->second;
						if(const auto nameIter = idNames.find(var.id); nameIter != idNames.end())
						{
							entry.name = nameIter->second;
						}
						target.push_back(entry);
					}
				}
			}

			return true;
		}
	};
}

int main(int argc, char** argv)
{
	CommandLine args{};
	if(!ParseCommandLine(argc, argv, args))
	{
		return 1;
	}

	const auto stageOpt = StageEnum(args.stage);
	if(!stageOpt)
	{
		std::cerr << "Unsupported stage: " << args.stage << "\n";
		return 1;
	}

	const std::filesystem::path dxcPath = args.dxcPathOverride.empty() ? DefaultDxcPath() : args.dxcPathOverride;
	if(!std::filesystem::exists(dxcPath))
	{
		std::cerr << "Cannot find dxc at: " << dxcPath << "\n";
		return 1;
	}

	const std::filesystem::path tempSpv = std::filesystem::path(args.output).replace_extension(".spv");
	if(!RunDxc(args, dxcPath, tempSpv))
	{
		return 1;
	}

	std::ifstream spvFile(tempSpv, std::ios::binary | std::ios::ate);
	if(!spvFile)
	{
		std::cerr << "Failed to read compiled bytecode: " << tempSpv << "\n";
		return 1;
	}
	const std::streamsize spvSize = spvFile.tellg();
	spvFile.seekg(0);
	std::vector<std::uint8_t> bytecode(static_cast<std::size_t>(spvSize));
	spvFile.read(reinterpret_cast<char*>(bytecode.data()), spvSize);
	std::filesystem::remove(tempSpv);

	StringTableBuilder strings;
	// Always emit the source metadata strings first so the offsets remain stable even when reflection data grows.
	const std::uint32_t pathOffset = strings.Add(args.input.string());
	const std::uint32_t entryOffset = strings.Add(args.entryPoint);
	const std::string targetProfile = args.targetProfile.empty() ? DefaultProfileForStage(args.stage) : args.targetProfile;
	const std::uint32_t targetOffset = strings.Add(targetProfile);

	// Reflect the compiled SPIR-V so TSPF carries interface and descriptor metadata.
	SpirvReflection reflector;
	ReflectionResult reflected{};
	if(!reflector.Reflect(bytecode, *stageOpt, reflected))
	{
		std::cerr << "Reflection failed; writing minimal metadata instead.\n";
	}

	std::vector<StageIOAttribute> stageInputs;
	stageInputs.reserve(reflected.inputs.size());
	for(const IoEntry& input : reflected.inputs)
	{
		StageIOAttribute attr{};
		attr.location = input.location;
		attr.componentCount = input.componentCount;
		attr.componentType = static_cast<std::uint32_t>(input.componentType);
		attr.nameOffset = input.name.empty() ? kInvalidStringOffset : strings.Add(input.name);
		stageInputs.push_back(attr);
	}

	std::vector<StageIOAttribute> stageOutputs;
	stageOutputs.reserve(reflected.outputs.size());
	for(const IoEntry& output : reflected.outputs)
	{
		StageIOAttribute attr{};
		attr.location = output.location;
		attr.componentCount = output.componentCount;
		attr.componentType = static_cast<std::uint32_t>(output.componentType);
		attr.nameOffset = output.name.empty() ? kInvalidStringOffset : strings.Add(output.name);
		stageOutputs.push_back(attr);
	}

	// Assemble descriptor bindings grouped by set so runtime code can build layouts quickly.
	std::vector<DescriptorBindingEntry> descriptorEntries = reflected.descriptorBindings;
	std::sort(descriptorEntries.begin(), descriptorEntries.end(), [](const DescriptorBindingEntry& a, const DescriptorBindingEntry& b) {
		return (a.set == b.set) ? (a.binding.binding < b.binding.binding) : (a.set < b.set);
	});

	std::vector<DescriptorBinding> descriptorBindings;
	std::vector<DescriptorSetLayoutDesc> descriptorSets;
	for(const DescriptorBindingEntry& entry : descriptorEntries)
	{
		if(descriptorSets.empty() || descriptorSets.back().setIndex != entry.set)
		{
			DescriptorSetLayoutDesc setDesc{};
			setDesc.setIndex = entry.set;
			setDesc.firstBinding = static_cast<std::uint32_t>(descriptorBindings.size());
			setDesc.bindingCount = 0;
			descriptorSets.push_back(setDesc);
		}

		DescriptorBinding binding = entry.binding;
		binding.nameOffset = entry.name.empty() ? kInvalidStringOffset : strings.Add(entry.name);
		descriptorBindings.push_back(binding);
		descriptorSets.back().bindingCount++;
	}

	ShaderReflection reflection{};
	reflection.source.language = static_cast<std::uint32_t>(ShaderSourceLanguage::HLSL);
	reflection.source.stage = static_cast<std::uint32_t>(*stageOpt);
	reflection.source.pathOffset = pathOffset;
	reflection.source.entryPointOffset = entryOffset;
	reflection.source.targetProfileOffset = targetOffset;

	CompiledStageBinary binary{};
	binary.format = static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV);
	binary.entryPointOffset = entryOffset;
	binary.targetProfileOffset = targetOffset;
	binary.bytecodeSize = static_cast<std::uint32_t>(bytecode.size());

	const std::uint32_t headerSize = static_cast<std::uint32_t>(sizeof(ShaderHeader));
	const std::uint32_t reflectionOffset = headerSize;
	std::vector<std::uint8_t> storage;
	// Reserve space for the reflection block at the start of the payload; fill it once all offsets are known.
	storage.resize(sizeof(ShaderReflection));

	const auto appendVector = [&](const auto& src) -> std::uint32_t {
		using T = typename std::decay_t<decltype(src)>::value_type;
		if(src.empty())
		{
			return 0;
		}
		const std::uint32_t offset = headerSize + static_cast<std::uint32_t>(storage.size());
		const std::uint8_t* data = reinterpret_cast<const std::uint8_t*>(src.data());
		storage.insert(storage.end(), data, data + src.size() * sizeof(T));
		return offset;
	};

	const std::uint32_t descriptorSetsOffset = appendVector(descriptorSets);
	const std::uint32_t descriptorBindingsOffset = appendVector(descriptorBindings);
	const std::uint32_t pushConstantsOffset = 0; // Push constant reflection would be added here once supported.
	const std::uint32_t stageInputsOffset = appendVector(stageInputs);
	const std::uint32_t stageOutputsOffset = appendVector(stageOutputs);

	const std::uint32_t compiledBinaryOffset = headerSize + static_cast<std::uint32_t>(storage.size());
	binary.bytecodeOffset = compiledBinaryOffset + static_cast<std::uint32_t>(sizeof(CompiledStageBinary));
	const std::size_t binaryStart = storage.size();
	storage.resize(binaryStart + sizeof(CompiledStageBinary));
	std::memcpy(storage.data() + binaryStart, &binary, sizeof(CompiledStageBinary));
	storage.insert(storage.end(), bytecode.begin(), bytecode.end());

	const std::uint32_t stringOffset = headerSize + static_cast<std::uint32_t>(storage.size());
	const std::uint32_t stringSize = static_cast<std::uint32_t>(strings.data.size());
	storage.insert(storage.end(), reinterpret_cast<const std::uint8_t*>(strings.data.data()), reinterpret_cast<const std::uint8_t*>(strings.data.data()) + strings.data.size());

	ShaderHeader header{};
	header.magic[0] = 'T';
	header.magic[1] = 'S';
	header.magic[2] = 'P';
	header.magic[3] = 'F';
	header.version = kShaderCurrentVersion;
	header.headerSize = static_cast<std::uint16_t>(sizeof(ShaderHeader));
	header.descriptorSetCount = static_cast<std::uint32_t>(descriptorSets.size());
	header.descriptorBindingCount = static_cast<std::uint32_t>(descriptorBindings.size());
	header.pushConstantCount = 0;
	header.stageInputCount = static_cast<std::uint32_t>(stageInputs.size());
	header.stageOutputCount = static_cast<std::uint32_t>(stageOutputs.size());
	header.stringTableOffset = stringOffset;
	header.stringTableSize = stringSize;
	header.reflectionOffset = reflectionOffset;
	header.descriptorSetsOffset = descriptorSetsOffset;
	header.descriptorBindingsOffset = descriptorBindingsOffset;
	header.pushConstantsOffset = pushConstantsOffset;
	header.stageInputsOffset = stageInputsOffset;
	header.stageOutputsOffset = stageOutputsOffset;
	header.compiledBinaryOffset = compiledBinaryOffset;
	header.compiledBinarySize = binary.bytecodeSize;

	reflection.descriptorSetOffset = descriptorSetsOffset;
	reflection.descriptorSetCount = header.descriptorSetCount;
	reflection.pushConstantOffset = pushConstantsOffset;
	reflection.pushConstantCount = header.pushConstantCount;
	reflection.inputOffset = stageInputsOffset;
	reflection.inputCount = header.stageInputCount;
	reflection.outputOffset = stageOutputsOffset;
	reflection.outputCount = header.stageOutputCount;

	// Patch the reflection block now that offsets are finalized.
	std::memcpy(storage.data(), &reflection, sizeof(reflection));

	std::ofstream out(args.output, std::ios::binary);
	if(!out)
	{
		std::cerr << "Failed to open output file: " << args.output << "\n";
		return 1;
	}
	out.write(reinterpret_cast<const char*>(&header), sizeof(header));
	out.write(reinterpret_cast<const char*>(storage.data()), static_cast<std::streamsize>(storage.size()));
	std::cout << "Wrote TSPF: " << args.output << " (" << bytecode.size() << " bytes SPIR-V)\n";
	return 0;
}
