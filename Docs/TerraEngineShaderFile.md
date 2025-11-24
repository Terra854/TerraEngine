# Terra Shader Program File (TSPF) Specification

TSPF stores a single shader stage (source metadata, reflection, and compiled bytecode) in one blob so pipelines can be built without re-reading SPIR-V/DXIL at runtime. All fields are little-endian.

## Design goals
- Keep stage metadata, reflection, and compiled binary together for cache-friendly loading.
- Use string-table offsets instead of raw pointers to remain relocatable in archives.
- Remain versioned and compact to support memory mapping on constrained platforms.

## Binary layout (all little-endian)
```
Header                    // fixed-size, packed
ShaderReflection          // single instance
DescriptorSetLayoutDesc[] // count = header.descriptorSetCount
DescriptorBinding[]       // count = header.descriptorBindingCount
PushConstantRange[]       // count = header.pushConstantCount
CompiledStageBinary       // single instance
Bytecode blob             // size = header.compiledBinarySize
String table              // size = header.stringTableSize
```

### Header (52 bytes)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `TSPF` |
| 4 | 2 | version | Current version = 1 |
| 6 | 2 | headerSize | Always `sizeof(Header)` (52) |
| 8 | 4 | descriptorSetCount | Number of descriptor sets |
| 12 | 4 | descriptorBindingCount | Total bindings across all sets |
| 16 | 4 | pushConstantCount | Number of push constant ranges |
| 20 | 4 | stringTableOffset | Byte offset to UTF-8 string table |
| 24 | 4 | stringTableSize | Size of string table in bytes |
| 28 | 4 | reflectionOffset | Byte offset to `ShaderReflection` |
| 32 | 4 | descriptorSetsOffset | Byte offset to `DescriptorSetLayoutDesc` array |
| 36 | 4 | descriptorBindingsOffset | Byte offset to `DescriptorBinding` array |
| 40 | 4 | pushConstantsOffset | Byte offset to `PushConstantRange` array |
| 44 | 4 | compiledBinaryOffset | Byte offset to `CompiledStageBinary` |
| 48 | 4 | compiledBinarySize | Length of compiled bytecode payload |

### Sub-structures (packed)
- **ShaderStageSource** (20 bytes): `language` (ShaderSourceLanguage), `stage` (ShaderStage), `pathOffset`, `entryPointOffset`, `targetProfileOffset` (all string offsets).
- **DescriptorBinding** (20 bytes): `binding` index, `type` (DescriptorType), `arrayCount`, `visibility` (bitmask of ShaderStage), `nameOffset` (string table).
- **DescriptorSetLayoutDesc** (12 bytes): `setIndex`, `firstBinding` (into DescriptorBinding array), `bindingCount`.
- **PushConstantRange** (12 bytes): `offsetBytes`, `sizeBytes`, `visibility` (bitmask of ShaderStage).
- **ShaderReflection** (24 bytes): `source` (ShaderStageSource), `descriptorSetOffset`, `descriptorSetCount`, `pushConstantOffset`, `pushConstantCount` (all file-relative offsets/counts).
- **CompiledStageBinary** (20 bytes): `format` (ShaderBinaryFormat), `entryPointOffset`, `targetProfileOffset`, `bytecodeOffset`, `bytecodeSize`.

String offsets reference the UTF-8 table located at `stringTableOffset`. `kInvalidStringOffset (0xFFFFFFFF)` indicates absence.

### Loader checklist
- Validate magic/version/headerSize before following offsets.
- Map arrays using counts and offsets; verify they fit inside the blob.
- Ensure `bytecodeOffset + bytecodeSize` stays within file bounds; format distinguishes SPIR-V vs DXIL.
- Use `StageToMask(stage)` when comparing visibility bits for descriptor bindings and push constants.
