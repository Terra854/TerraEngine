#include "ShaderCompiler.h"
#include <array>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <functional>
#include <cstring>
#include <unordered_map>
#include <system_error>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "Engine/File Formats/TerraEngineShaderFile.h"

namespace {
	constexpr std::uint32_t kSpirvMagicNumber = 0x07230203u;
	constexpr std::uint16_t kSpirvOpEntryPoint = 15u;
	constexpr std::uint16_t kSpirvOpVariable = 59u;
	constexpr std::uint16_t kSpirvOpEmitMeshTasksEXT = 5294u;
	constexpr std::uint32_t kSpirvExecutionModelTaskEXT = 5364u;
	constexpr std::uint32_t kSpirvStorageClassTaskPayloadWorkgroupEXT = 5402u;

	bool PatchTaskEmitPayload(std::vector<std::uint8_t>& bytecode) {
		// DXC currently emits OpEmitMeshTasksEXT without payload id. Vulkan validation
		// requires the task payload variable to be passed explicitly when mesh stage
		// consumes TaskPayloadWorkgroupEXT, so patch those instructions in-place.
		if (bytecode.size() < sizeof(std::uint32_t) * 5 || (bytecode.size() % sizeof(std::uint32_t)) != 0) {
			return false;
		}

		const std::size_t wordCount = bytecode.size() / sizeof(std::uint32_t);
		const auto ReadWord = [&](std::size_t index) -> std::uint32_t {
			std::uint32_t value = 0;
			std::memcpy(&value, bytecode.data() + (index * sizeof(std::uint32_t)), sizeof(value));
			return value;
		};

		if (ReadWord(0) != kSpirvMagicNumber) {
			return false;
		}

		std::vector<std::uint32_t> taskPayloadVariables;
		bool hasTaskEntryPoint = false;
		bool requiresPatch = false;

		for (std::size_t i = 5; i < wordCount;) {
			const std::uint32_t instructionWord = ReadWord(i);
			const std::uint16_t opcode = static_cast<std::uint16_t>(instructionWord & 0xFFFFu);
			const std::uint16_t instructionWordCount = static_cast<std::uint16_t>(instructionWord >> 16);
			if (instructionWordCount == 0 || (i + instructionWordCount) > wordCount) {
				return false;
			}

			if (opcode == kSpirvOpEntryPoint && instructionWordCount >= 3 && ReadWord(i + 1) == kSpirvExecutionModelTaskEXT) {
				hasTaskEntryPoint = true;
			} else if (opcode == kSpirvOpVariable && instructionWordCount >= 4 && ReadWord(i + 3) == kSpirvStorageClassTaskPayloadWorkgroupEXT) {
				taskPayloadVariables.push_back(ReadWord(i + 2));
			} else if (opcode == kSpirvOpEmitMeshTasksEXT && instructionWordCount == 4) {
				requiresPatch = true;
			}

			i += instructionWordCount;
		}

		if (!requiresPatch) {
			return true;
		}
		if (!hasTaskEntryPoint || taskPayloadVariables.empty()) {
			return false;
		}

		const std::uint32_t payloadVariableId = taskPayloadVariables.front();
		std::vector<std::uint32_t> patchedWords;
		patchedWords.reserve(wordCount + 16);
		for (std::size_t headerWord = 0; headerWord < 5; ++headerWord) {
			patchedWords.push_back(ReadWord(headerWord));
		}

		for (std::size_t i = 5; i < wordCount;) {
			const std::uint32_t instructionWord = ReadWord(i);
			const std::uint16_t opcode = static_cast<std::uint16_t>(instructionWord & 0xFFFFu);
			const std::uint16_t instructionWordCount = static_cast<std::uint16_t>(instructionWord >> 16);

			if (opcode == kSpirvOpEmitMeshTasksEXT && instructionWordCount == 4) {
				patchedWords.push_back((5u << 16) | kSpirvOpEmitMeshTasksEXT);
				patchedWords.push_back(ReadWord(i + 1));
				patchedWords.push_back(ReadWord(i + 2));
				patchedWords.push_back(ReadWord(i + 3));
				patchedWords.push_back(payloadVariableId);
			} else {
				for (std::size_t wordIndex = 0; wordIndex < instructionWordCount; ++wordIndex) {
					patchedWords.push_back(ReadWord(i + wordIndex));
				}
			}

			i += instructionWordCount;
		}

		bytecode.resize(patchedWords.size() * sizeof(std::uint32_t));
		std::memcpy(bytecode.data(), patchedWords.data(), bytecode.size());
		return true;
	}

    std::string ToLower(std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    std::string DefaultProfileForStage(const std::string& stage) {
        const std::string s = ToLower(stage);
        if (s == "vs") return "vs_6_8";
        if (s == "ps" || s == "fs") return "ps_6_8";
        if (s == "cs") return "cs_6_8";
        if (s == "gs") return "gs_6_8";
        if (s == "hs") return "hs_6_8";
        if (s == "ds") return "ds_6_8";
        if (s == "ms") return "ms_6_8";
        if (s == "ts" || s == "as") return "as_6_8";
        return "vs_6_8";
    }

    std::optional<ShaderStage> StageEnum(const std::string& stage) {
        const std::string s = ToLower(stage);
        if (s == "vs") return ShaderStage::Vertex;
        if (s == "ps" || s == "fs") return ShaderStage::Fragment;
        if (s == "cs") return ShaderStage::Compute;
        if (s == "gs") return ShaderStage::Geometry;
        if (s == "hs") return ShaderStage::TessellationControl;
        if (s == "ds") return ShaderStage::TessellationEvaluation;
        if (s == "ms") return ShaderStage::Mesh;
        if (s == "ts" || s == "as") return ShaderStage::Task;
        return std::nullopt;
    }

    std::string Quote(const std::filesystem::path& p) {
        std::ostringstream oss;
        oss << "\"" << p.string() << "\"";
        return oss.str();
    }

    bool RunDxc(const std::filesystem::path& input, const std::filesystem::path& dxcPath, const std::filesystem::path& tempSpv, const std::string& target, const std::string& entryPoint) {
#if defined(_WIN32)
        auto ToWide = [](const std::filesystem::path& p) -> std::wstring {
            return p.wstring();
        };
        auto ToWideStr = [](const std::string& s) -> std::wstring {
            return std::wstring(s.begin(), s.end());
        };

        std::vector<std::wstring> wideArgs;
        wideArgs.push_back(ToWide(dxcPath));
        wideArgs.push_back(L"-spirv");
        wideArgs.push_back(L"-fspv-target-env=vulkan1.3");
        wideArgs.push_back(L"-T");
        wideArgs.push_back(ToWideStr(target));
        wideArgs.push_back(L"-E");
        wideArgs.push_back(ToWideStr(entryPoint));
        wideArgs.push_back(L"-Fo");
        wideArgs.push_back(ToWide(tempSpv));
        wideArgs.push_back(ToWide(input));

        std::vector<const wchar_t*> cargs;
        cargs.reserve(wideArgs.size() + 1);
        for (const std::wstring& w : wideArgs) {
            cargs.push_back(w.c_str());
        }
        cargs.push_back(nullptr);

        const intptr_t code = _wspawnv(_P_WAIT, dxcPath.wstring().c_str(), cargs.data());
        if (code != 0) {
            std::cerr << "dxc failed with exit code " << code << "\n";
            return false;
        }
        return true;
#else
        std::ostringstream cmd;
        cmd << Quote(dxcPath) << " -spirv -fspv-target-env=vulkan1.3"
            << " -T " << target
            << " -E " << entryPoint
            << " -Fo " << Quote(tempSpv)
            << " " << Quote(input);

        const int code = std::system(cmd.str().c_str());
        if (code != 0) {
            std::cerr << "dxc failed with exit code " << code << "\n";
            return false;
        }
        return true;
#endif
    }

    struct StringTableBuilder {
        std::vector<char> data;

        std::uint32_t Add(const std::string& value) {
            const std::uint32_t offset = static_cast<std::uint32_t>(data.size());
            data.insert(data.end(), value.begin(), value.end());
            data.push_back('\0');
            return offset;
        }
    };

    // ... SpirvReflection and other structs ...
    // Since SpirvReflection is quite long and internal, I will include it here.
    // Copying TypeInfo, Decorations, etc. from original file.
    
    struct TypeInfo {
        enum class Kind { Unknown, Bool, Int, UInt, Float, Vector, Matrix, Array, Struct, Pointer, Sampler, Image, SampledImage };
        Kind kind = Kind::Unknown;
        IoComponentType component = IoComponentType::Unknown;
        std::uint32_t componentCount = 1;
        std::uint32_t typeId = 0;
        std::uint32_t arrayLengthId = 0;
        std::vector<std::uint32_t> members;
        std::uint32_t sampledKind = 0;
    };

    struct Decorations {
        std::optional<std::uint32_t> location;
        std::optional<std::uint32_t> binding;
        std::optional<std::uint32_t> descriptorSet;
        bool builtIn = false;
    };

    struct DescriptorBindingEntry {
        std::uint32_t set = 0;
        DescriptorBinding binding{};
        std::string name;
    };

    struct IoEntry {
        std::uint32_t location = 0;
        IoComponentType componentType = IoComponentType::Unknown;
        std::uint32_t componentCount = 0;
        std::string name;
    };

    struct ReflectionResult {
        std::vector<IoEntry> inputs;
        std::vector<IoEntry> outputs;
        std::vector<DescriptorBindingEntry> descriptorBindings;
    };

    class SpirvReflection {
    public:
        bool Reflect(const std::vector<std::uint8_t>& bytecode, ShaderStage stage, ReflectionResult& out) const {
            if (bytecode.size() < sizeof(std::uint32_t) * 5) return false;

            const std::size_t wordCount = bytecode.size() / sizeof(std::uint32_t);
            const std::uint32_t* words = reinterpret_cast<const std::uint32_t*>(bytecode.data());

            std::unordered_map<std::uint32_t, TypeInfo> types;
            std::unordered_map<std::uint32_t, Decorations> idDecorations;
            std::unordered_map<std::uint32_t, std::vector<Decorations>> memberDecorations;
            std::unordered_map<std::uint32_t, std::string> idNames;
            std::unordered_map<std::uint32_t, std::vector<std::string>> memberNames;
            std::unordered_map<std::uint32_t, std::uint32_t> constants;

            struct VariableInfo {
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

            for (std::size_t i = 5; i < wordCount;) {
                const std::uint32_t opWord = words[i];
                const std::uint16_t op = static_cast<std::uint16_t>(opWord & 0xFFFFu);
                const std::uint16_t count = static_cast<std::uint16_t>(opWord >> 16);
                if (count == 0) return false;

                switch (op) {
                case 5: // OpName
                    if (count >= 3) idNames[words[i + 1]] = readString(i + 2);
                    break;
                case 6: // OpMemberName
                    if (count >= 4) {
                        uint32_t tid = words[i + 1];
                        uint32_t midx = words[i + 2];
                        if (memberNames[tid].size() <= midx) memberNames[tid].resize(midx + 1);
                        memberNames[tid][midx] = readString(i + 3);
                    }
                    break;
                case 71: // OpDecorate
                    if (count >= 3) {
                        Decorations& dec = idDecorations[words[i + 1]];
                        uint32_t d = words[i + 2];
                        if (d == 30 && count >= 4) dec.location = words[i + 3];
                        else if (d == 11) dec.builtIn = true;
                        else if (d == 33 && count >= 4) dec.binding = words[i + 3];
                        else if (d == 34 && count >= 4) dec.descriptorSet = words[i + 3];
                    }
                    break;
                case 72: // OpMemberDecorate
                    if (count >= 5) {
                        uint32_t tid = words[i + 1];
                        uint32_t midx = words[i + 2];
                        if (memberDecorations[tid].size() <= midx) memberDecorations[tid].resize(midx + 1);
                        Decorations& dec = memberDecorations[tid][midx];
                        if (words[i + 3] == 30 && count >= 5) dec.location = words[i + 4];
                        else if (words[i + 3] == 11) dec.builtIn = true;
                    }
                    break;
                case 59: // OpVariable
                    if (count >= 4) variables.push_back({ words[i + 2], words[i + 1], words[i + 3] });
                    break;
                case 20: types[words[i + 1]] = { TypeInfo::Kind::Bool, IoComponentType::Bool }; break;
                case 21: types[words[i + 1]] = { (words[i + 3] == 0) ? TypeInfo::Kind::UInt : TypeInfo::Kind::Int, (words[i + 3] == 0) ? IoComponentType::UInt : IoComponentType::Int }; break;
                case 22: types[words[i + 1]] = { TypeInfo::Kind::Float, IoComponentType::Float }; break;
                case 23: // Vector
                {
                    TypeInfo info{}; info.kind = TypeInfo::Kind::Vector; info.typeId = words[i + 2]; info.componentCount = words[i + 3];
                    if (types.count(info.typeId)) info.component = types[info.typeId].component;
                    types[words[i + 1]] = info;
                    break;
                }
                case 24: // Matrix
                {
                    TypeInfo info{}; info.kind = TypeInfo::Kind::Matrix; info.typeId = words[i + 2]; info.componentCount = words[i + 3];
                    types[words[i + 1]] = info;
                    break;
                }
                case 28: // Array
                {
                    TypeInfo info{}; info.kind = TypeInfo::Kind::Array; info.typeId = words[i + 2]; info.arrayLengthId = words[i + 3];
                    types[words[i + 1]] = info;
                    break;
                }
                case 30: // Struct
                {
                    TypeInfo info{}; info.kind = TypeInfo::Kind::Struct;
                    for (uint16_t m = 2; m < count; ++m) info.members.push_back(words[i + m]);
                    types[words[i + 1]] = info;
                    break;
                }
                case 32: // Pointer
                {
                    TypeInfo info{}; info.kind = TypeInfo::Kind::Pointer; info.typeId = words[i + 3];
                    types[words[i + 1]] = info;
                    break;
                }
                case 26: types[words[i + 1]] = { TypeInfo::Kind::Sampler }; break;
                case 25: 
                {
                    TypeInfo info{}; info.kind = TypeInfo::Kind::Image; info.typeId = words[i + 2];
                    if (count >= 9) info.sampledKind = words[i + 7];
                    types[words[i + 1]] = info;
                    break;
                }
                case 27: 
                {
                    TypeInfo info{}; info.kind = TypeInfo::Kind::SampledImage; info.typeId = words[i + 2];
                    types[words[i + 1]] = info;
                    break;
                }
                case 43: if (count >= 4) constants[words[i + 2]] = words[i + 3]; break;
                }
                i += count;
            }

            std::function<std::optional<std::pair<IoComponentType, std::uint32_t>>(std::uint32_t)> resolveScalar;
			resolveScalar = [&](std::uint32_t typeId) -> std::optional<std::pair<IoComponentType, std::uint32_t>> {
				auto iter = types.find(typeId);
				if(iter == types.end()) return std::nullopt;
				const TypeInfo& info = iter->second;
				switch(info.kind) {
				case TypeInfo::Kind::Bool: case TypeInfo::Kind::Int: case TypeInfo::Kind::UInt: case TypeInfo::Kind::Float: return std::make_pair(info.component, 1u);
				case TypeInfo::Kind::Vector:
					if(info.component == IoComponentType::Unknown) {
						if(const auto base = resolveScalar(info.typeId)) return std::make_pair(base->first, info.componentCount);
						return std::nullopt;
					}
					return std::make_pair(info.component, info.componentCount);
				case TypeInfo::Kind::Array: case TypeInfo::Kind::Pointer: return resolveScalar(info.typeId);
				default: return std::nullopt;
				}
			};

            const StageVisibilityMask visibilityMask = StageToMask(stage);

            for (const VariableInfo& var : variables) {
                const Decorations varDeco = idDecorations[var.id];
                if (varDeco.builtIn) continue;

                if (varDeco.binding && varDeco.descriptorSet && (var.storageClass == 2 || var.storageClass == 0 || var.storageClass == 12)) {
                    if (!types.count(var.typeId)) continue;
                    const TypeInfo& ptrType = types[var.typeId];
                    if (ptrType.kind != TypeInfo::Kind::Pointer) continue;
                    if (!types.count(ptrType.typeId)) continue;
                    const TypeInfo& pointee = types[ptrType.typeId];

                    DescriptorBindingEntry entry{};
                    entry.set = *varDeco.descriptorSet;
                    entry.binding.binding = *varDeco.binding;
                    entry.binding.arrayCount = 1;
                    entry.binding.visibility = visibilityMask;

                    if (pointee.kind == TypeInfo::Kind::SampledImage) entry.binding.type = static_cast<uint32_t>(DescriptorType::CombinedImageSampler);
                    else if (pointee.kind == TypeInfo::Kind::Sampler) entry.binding.type = static_cast<uint32_t>(DescriptorType::Sampler);
                    else if (pointee.kind == TypeInfo::Kind::Image) entry.binding.type = static_cast<uint32_t>(pointee.sampledKind == 2 ? DescriptorType::StorageImage : DescriptorType::SampledImage);
                    else if (var.storageClass == 12) entry.binding.type = static_cast<uint32_t>(DescriptorType::StorageBuffer);
                    else entry.binding.type = static_cast<uint32_t>(DescriptorType::UniformBuffer);

                    if (pointee.kind == TypeInfo::Kind::Array && constants.count(pointee.arrayLengthId)) entry.binding.arrayCount = constants[pointee.arrayLengthId];
                    if (idNames.count(var.id)) entry.name = idNames[var.id];
                    out.descriptorBindings.push_back(entry);
                }

                if (var.storageClass != 1 && var.storageClass != 3) continue;
                std::vector<IoEntry>& target = (var.storageClass == 1) ? out.inputs : out.outputs;
                if (!types.count(var.typeId)) continue;
                const TypeInfo& varType = types[var.typeId];

                if (varType.kind == TypeInfo::Kind::Pointer) {
                    if (!types.count(varType.typeId)) continue;
                    const TypeInfo& base = types[varType.typeId];
                    if (base.kind == TypeInfo::Kind::Struct) {
                        for (size_t m = 0; m < base.members.size(); ++m) {
                            if (!types.count(base.members[m])) continue;
                            const Decorations mDeco = (m < memberDecorations[varType.typeId].size()) ? memberDecorations[varType.typeId][m] : Decorations{};
                            if (mDeco.builtIn || !mDeco.location) continue;
                            const auto scalar = resolveScalar(base.members[m]);
                            if (!scalar) continue;
                            IoEntry entry{}; entry.location = *mDeco.location; entry.componentType = scalar->first; entry.componentCount = scalar->second;
                            if (m < memberNames[varType.typeId].size()) entry.name = memberNames[varType.typeId][m];
                            target.push_back(entry);
                        }
                    } else {
                        const auto scalar = resolveScalar(varType.typeId);
                        if (scalar && varDeco.location) {
                            IoEntry entry{}; entry.location = *varDeco.location; entry.componentType = scalar->first; entry.componentCount = scalar->second;
                            if (idNames.count(var.id)) entry.name = idNames[var.id];
                            target.push_back(entry);
                        }
                    }
                }
            }
            return true;
        }
    };
} // namespace

bool CompileShader(const std::filesystem::path& input, const std::filesystem::path& output, const std::string& stage, const std::string& targetProfile, const std::string& entryPoint, const std::filesystem::path& dxcPath) {
    if (!std::filesystem::exists(dxcPath)) {
        std::cerr << "Cannot find dxc: " << dxcPath << "\n";
        return false;
    }

    const auto stageOpt = StageEnum(stage);
    if (!stageOpt) {
        std::cerr << "Unsupported stage: " << stage << "\n";
        return false;
    }

    const std::filesystem::path tempSpv = std::filesystem::path(output).replace_extension(".spv");
    const std::string target = targetProfile.empty() ? DefaultProfileForStage(stage) : targetProfile;

    if (!RunDxc(input, dxcPath, tempSpv, target, entryPoint)) {
        return false;
    }

    const auto CleanupTempSpv = [&tempSpv]() {
        std::error_code removeErr;
        std::filesystem::remove(tempSpv, removeErr);
    };

    std::ifstream spvFile(tempSpv, std::ios::binary | std::ios::ate);
    if (!spvFile) {
        std::cerr << "Failed to read compiled bytecode: " << tempSpv << "\n";
        CleanupTempSpv();
        return false;
    }
    const std::streamsize spvSize = spvFile.tellg();
    spvFile.seekg(0);
    std::vector<std::uint8_t> bytecode(static_cast<std::size_t>(spvSize));
    spvFile.read(reinterpret_cast<char*>(bytecode.data()), spvSize);
    if (!spvFile) {
        std::cerr << "Failed to load compiled SPIR-V bytes: " << tempSpv << "\n";
        CleanupTempSpv();
        return false;
    }
    // Close the temporary SPIR-V file before attempting to delete it later.
    // Windows keeps the path locked while the stream stays open, which would
    // leave behind stale .spv siblings next to every generated .tspf shader.
    spvFile.close();
	if (*stageOpt == ShaderStage::Task) {
		if (!PatchTaskEmitPayload(bytecode)) {
			std::cerr << "Failed to patch task-shader payload linkage in SPIR-V: " << input << "\n";
			CleanupTempSpv();
			return false;
		}
	}

    StringTableBuilder strings;
    const uint32_t pathOffset = strings.Add(input.string());
    const uint32_t entryOffset = strings.Add(entryPoint);
    const uint32_t targetOffset = strings.Add(target);

    SpirvReflection reflector;
    ReflectionResult reflected{};
    if (!reflector.Reflect(bytecode, *stageOpt, reflected)) {
        std::cerr << "Reflection failed; writing minimal metadata instead.\n";
    }

    std::vector<StageIOAttribute> stageInputs;
    for (const IoEntry& inp : reflected.inputs) {
        StageIOAttribute attr{}; attr.location = inp.location; attr.componentCount = inp.componentCount; attr.componentType = static_cast<uint32_t>(inp.componentType);
        attr.nameOffset = inp.name.empty() ? kInvalidStringOffset : strings.Add(inp.name);
        stageInputs.push_back(attr);
    }

    std::vector<StageIOAttribute> stageOutputs;
    for (const IoEntry& outp : reflected.outputs) {
        StageIOAttribute attr{}; attr.location = outp.location; attr.componentCount = outp.componentCount; attr.componentType = static_cast<uint32_t>(outp.componentType);
        attr.nameOffset = outp.name.empty() ? kInvalidStringOffset : strings.Add(outp.name);
        stageOutputs.push_back(attr);
    }

    std::vector<DescriptorBindingEntry> descriptorEntries = reflected.descriptorBindings;
    std::sort(descriptorEntries.begin(), descriptorEntries.end(), [](const DescriptorBindingEntry& a, const DescriptorBindingEntry& b) {
        return (a.set == b.set) ? (a.binding.binding < b.binding.binding) : (a.set < b.set);
    });

    std::vector<DescriptorBinding> descriptorBindings;
    std::vector<DescriptorSetLayoutDesc> descriptorSets;
    for (const DescriptorBindingEntry& entry : descriptorEntries) {
        if (descriptorSets.empty() || descriptorSets.back().setIndex != entry.set) {
            DescriptorSetLayoutDesc setDesc{}; setDesc.setIndex = entry.set; setDesc.firstBinding = static_cast<uint32_t>(descriptorBindings.size());
            descriptorSets.push_back(setDesc);
        }
        DescriptorBinding binding = entry.binding;
        binding.nameOffset = entry.name.empty() ? kInvalidStringOffset : strings.Add(entry.name);
        descriptorBindings.push_back(binding);
        descriptorSets.back().bindingCount++;
    }

    const uint32_t headerSize = static_cast<uint32_t>(sizeof(ShaderHeader));
    const std::uint64_t descriptorSetsBytes = static_cast<std::uint64_t>(descriptorSets.size()) * sizeof(DescriptorSetLayoutDesc);
    const std::uint64_t descriptorBindingsBytes = static_cast<std::uint64_t>(descriptorBindings.size()) * sizeof(DescriptorBinding);
    const std::uint64_t stageInputsBytes = static_cast<std::uint64_t>(stageInputs.size()) * sizeof(StageIOAttribute);
    const std::uint64_t stageOutputsBytes = static_cast<std::uint64_t>(stageOutputs.size()) * sizeof(StageIOAttribute);
    const std::uint64_t bytecodeBytes = static_cast<std::uint64_t>(bytecode.size());
    const std::uint64_t stringsBytes = static_cast<std::uint64_t>(strings.data.size());

    std::uint64_t payloadCursor = headerSize + sizeof(ShaderReflection);
    const auto ReservePayloadBlock = [&payloadCursor](std::uint64_t byteCount, std::uint32_t& outOffset) -> bool {
        if (byteCount == 0) {
            outOffset = 0;
            return true;
        }

        if (payloadCursor > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        outOffset = static_cast<std::uint32_t>(payloadCursor);
        payloadCursor += byteCount;
        return payloadCursor <= (static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u);
    };

    std::uint32_t descriptorSetsOffset = 0;
    std::uint32_t descriptorBindingsOffset = 0;
    std::uint32_t stageInputsOffset = 0;
    std::uint32_t stageOutputsOffset = 0;
    std::uint32_t compiledBinaryOffset = 0;
    std::uint32_t stringOffset = 0;
    if (!ReservePayloadBlock(descriptorSetsBytes, descriptorSetsOffset)
        || !ReservePayloadBlock(descriptorBindingsBytes, descriptorBindingsOffset)
        || !ReservePayloadBlock(stageInputsBytes, stageInputsOffset)
        || !ReservePayloadBlock(stageOutputsBytes, stageOutputsOffset)
        || !ReservePayloadBlock(sizeof(CompiledStageBinary) + bytecodeBytes, compiledBinaryOffset)
        || !ReservePayloadBlock(stringsBytes, stringOffset)) {
        std::cerr << "Shader payload layout overflowed 32-bit file offsets for " << input << "\n";
        CleanupTempSpv();
        return false;
    }

    ShaderReflection reflection{};
    reflection.source.language = static_cast<uint32_t>(ShaderSourceLanguage::HLSL);
    reflection.source.stage = static_cast<uint32_t>(*stageOpt);
    reflection.source.pathOffset = pathOffset;
    reflection.source.entryPointOffset = entryOffset;
    reflection.source.targetProfileOffset = targetOffset;
    reflection.descriptorSetOffset = descriptorSetsOffset;
    reflection.descriptorSetCount = static_cast<std::uint32_t>(descriptorSets.size());
    reflection.inputOffset = stageInputsOffset;
    reflection.inputCount = static_cast<std::uint32_t>(stageInputs.size());
    reflection.outputOffset = stageOutputsOffset;
    reflection.outputCount = static_cast<std::uint32_t>(stageOutputs.size());

    CompiledStageBinary binary{};
    binary.format = static_cast<uint32_t>(ShaderBinaryFormat::SPIRV);
    binary.entryPointOffset = entryOffset;
    binary.targetProfileOffset = targetOffset;
    binary.bytecodeOffset = compiledBinaryOffset + static_cast<std::uint32_t>(sizeof(CompiledStageBinary));
    binary.bytecodeSize = static_cast<uint32_t>(bytecode.size());

    ShaderHeader header{};
    header.magic[0] = 'T'; header.magic[1] = 'S'; header.magic[2] = 'P'; header.magic[3] = 'F';
    header.version = kShaderCurrentVersion;
    header.headerSize = static_cast<uint16_t>(sizeof(ShaderHeader));
    header.descriptorSetCount = static_cast<uint32_t>(descriptorSets.size());
    header.descriptorBindingCount = static_cast<uint32_t>(descriptorBindings.size());
    header.stageInputCount = static_cast<uint32_t>(stageInputs.size());
    header.stageOutputCount = static_cast<uint32_t>(stageOutputs.size());
    header.stringTableOffset = stringOffset;
    header.stringTableSize = static_cast<uint32_t>(strings.data.size());
    header.reflectionOffset = headerSize;
    header.descriptorSetsOffset = descriptorSetsOffset;
    header.descriptorBindingsOffset = descriptorBindingsOffset;
    header.stageInputsOffset = stageInputsOffset;
    header.stageOutputsOffset = stageOutputsOffset;
    header.compiledBinaryOffset = compiledBinaryOffset;
    header.compiledBinarySize = binary.bytecodeSize;

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Failed to open output file: " << output << "\n";
        CleanupTempSpv();
        return false;
    }

    // Stream the file in on-disk order so the compiler does not keep a second
    // complete TSPF payload buffer alive next to the SPIR-V bytecode in RAM.
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&reflection), sizeof(reflection));
    if (!descriptorSets.empty()) {
        out.write(reinterpret_cast<const char*>(descriptorSets.data()), static_cast<std::streamsize>(descriptorSetsBytes));
    }
    if (!descriptorBindings.empty()) {
        out.write(reinterpret_cast<const char*>(descriptorBindings.data()), static_cast<std::streamsize>(descriptorBindingsBytes));
    }
    if (!stageInputs.empty()) {
        out.write(reinterpret_cast<const char*>(stageInputs.data()), static_cast<std::streamsize>(stageInputsBytes));
    }
    if (!stageOutputs.empty()) {
        out.write(reinterpret_cast<const char*>(stageOutputs.data()), static_cast<std::streamsize>(stageOutputsBytes));
    }
    out.write(reinterpret_cast<const char*>(&binary), sizeof(binary));
    if (!bytecode.empty()) {
        out.write(reinterpret_cast<const char*>(bytecode.data()), static_cast<std::streamsize>(bytecodeBytes));
    }
    if (!strings.data.empty()) {
        out.write(strings.data.data(), static_cast<std::streamsize>(strings.data.size()));
    }

    CleanupTempSpv();
    return static_cast<bool>(out);
}
