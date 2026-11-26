# Terra Shader Program File (TSPF) Specification

TSPF stores a single shader stage (source metadata, reflection, and compiled bytecode) in one blob so pipelines can be built without re-reading SPIR-V/DXIL at runtime. All fields are little-endian.

## Creating shader stages and collections

Build and install the editor with `cmake --preset RelWithDebInfo` and
`cmake --build --preset RelWithDebInfo`. In the Project Browser, open
**Add → Shaders** and choose **Task Shader**, **Mesh Shader**, or
**Fragment Shader**. Each entry creates a single HLSL file seeded with
the corresponding engine shader and uses a `.task.hlsl`, `.mesh.hlsl`, or
`.frag.hlsl` suffix. The editor compiles it in the background on
creation and on save. The import status and editor log show compile errors.
Successful compiles write a TSPF containing SPIR-V under project `Build/Assets`.

Choose **Add → Shaders → Shader Collection** to create a single
`.teshadercollection` asset. Assign it to a material and use the collection's
Inspector to link the stages you want to override. Each empty field inherits
the corresponding engine stage. Custom stages must preserve the default
stage's renderer interface. If a linked stage changes that interface, the
editor shows a **Shader update rejected** banner above the workspace. The
banner names the mismatch and stays visible until dismissed. A previously
loaded collection keeps rendering while you fix the source.

Select **MSAA** with VRS off to
rasterize the same mesh shader collection at four samples. Keep the fragment
template's `TERRAENGINE_CONVENTIONAL_RETURN` entry-point wrapper so the compiler
can emit the object's picking ID alongside the custom color. The editor
recompiles existing fragment assets to add that variant.

The mesh temporal pass records final clip positions from matched draws. To
include custom vertex motion, keep the template's `TE_WRITE_MESH_VERTEX`
call and modify `outVertex.position` before it. Use stable vertex slots
within each meshlet so consecutive frames can pair the same corners.

From the repository root, run
`pwsh -File Tests/Tools/ShaderAuthoring/TestInstalledShaderCollections.ps1`
to verify parallel stage imports, default inheritance, and reload after a save.
To check the banner interactively, link a valid task shader, then change the
array length of one member of its task payload and save. The banner should
identify the task-to-mesh payload mismatch. Dismiss the banner and restore the
original array length to verify that the collection reloads.
Run `pwsh -File Tests/Tools/ShaderAuthoring/TestInstalledMeshMsaaPicking.ps1`
on a supported Vulkan GPU to verify native picking on built-in and displaced
custom meshes with MSAA off and on. Add -IncludePostProcessModes to exercise FXAA, SMAA and TAA. Add -Compatibility to run the built-in mesh compatibility renderer.

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
StageIOAttribute[]        // stage inputs then outputs; header counts/offsets
PushConstantRange[]       // count = header.pushConstantCount
CompiledStageBinary       // single instance
Bytecode blob             // size = header.compiledBinarySize
String table              // size = header.stringTableSize
Provenance extension      // optional TSPV envelope and backend-variant record
Picking fragment          // optional TSPI envelope plus SPIR-V with the identity output
```

### Header (68 bytes)

The packed v1 `ShaderHeader` has alignment 1. Offsets are file-relative; string
references inside stage/reflection records are relative to the UTF-8 string table.

| Offset | Size | Field |
|---|---:|---|
| 0 | 4 | `magic` |
| 4 | 2 | `version` |
| 6 | 2 | `headerSize` |
| 8 | 4 | `descriptorSetCount` |
| 12 | 4 | `descriptorBindingCount` |
| 16 | 4 | `pushConstantCount` |
| 20 | 4 | `stageInputCount` |
| 24 | 4 | `stageOutputCount` |
| 28 | 4 | `stringTableOffset` |
| 32 | 4 | `stringTableSize` |
| 36 | 4 | `reflectionOffset` |
| 40 | 4 | `descriptorSetsOffset` |
| 44 | 4 | `descriptorBindingsOffset` |
| 48 | 4 | `pushConstantsOffset` |
| 52 | 4 | `stageInputsOffset` |
| 56 | 4 | `stageOutputsOffset` |
| 60 | 4 | `compiledBinaryOffset` |
| 64 | 4 | `compiledBinarySize` |

`magic` is `TSPF`, `version` is `1`, and `headerSize` is `68`.

### Sub-structures (packed)
- **ShaderStageSource** (20 bytes): `language` (ShaderSourceLanguage), `stage` (ShaderStage), `pathOffset`, `entryPointOffset`, `targetProfileOffset` (all string offsets).
- **DescriptorBinding** (20 bytes): `binding` index, `type` (DescriptorType), `arrayCount`, `visibility` (bitmask of ShaderStage), `nameOffset` (string table).
- **DescriptorSetLayoutDesc** (12 bytes): `setIndex`, `firstBinding` (into DescriptorBinding array), `bindingCount`.
- **PushConstantRange** (12 bytes): `offsetBytes`, `sizeBytes`, `visibility` (bitmask of ShaderStage).
- **ShaderReflection** (52 bytes): `source` (20-byte ShaderStageSource) followed by `descriptorSetOffset`/`descriptorSetCount`, `pushConstantOffset`/`pushConstantCount`, `inputOffset`/`inputCount`, and `outputOffset`/`outputCount` (all file-relative offsets/counts).
- **CompiledStageBinary** (20 bytes): `format` (ShaderBinaryFormat), `entryPointOffset`, `targetProfileOffset`, `bytecodeOffset`, `bytecodeSize`.

### Reflection offsets (52 bytes)

| Offset | Size | Field |
|---|---:|---|
| 0 | 20 | `source` |
| 20 | 4 | `descriptorSetOffset` |
| 24 | 4 | `descriptorSetCount` |
| 28 | 4 | `pushConstantOffset` |
| 32 | 4 | `pushConstantCount` |
| 36 | 4 | `inputOffset` |
| 40 | 4 | `inputCount` |
| 44 | 4 | `outputOffset` |
| 48 | 4 | `outputCount` |

A current emitted TSPF places reflection at byte 68 and its descriptor-set table
at byte 120 (`68 + 52`). Other tables follow their recorded offsets. Compare
counts/offsets in header and reflection before using them; validate input/output
arrays as well as descriptor/push-constant ranges. The actual packed declarations
are in [TerraEngineShaderFile.h](../Engine/File%20Formats/TerraEngineShaderFile.h).

String offsets reference the UTF-8 table located at `stringTableOffset`. `kInvalidStringOffset (0xFFFFFFFF)` indicates absence.

### Optional provenance trailer

The current compiler appends provenance after the string table: an 8-byte
`ShaderProvenanceExtensionHeader` (`TSPV` magic and payload size), followed by the
64-byte version-1 `ShaderProvenanceBackendVariantExtension`. This separately tagged
record describes backend/variant identity, interface and bytecode hashes, and
binary availability without changing the v1 header or reflection layout. Older
files may end after the string table. Validate the trailer's bounds, magic, size
and version before using it; it is not part of the bytecode or string-table range.
Compatible custom fragments add a `TSPI` record after provenance. An 8-byte
little-endian header stores the magic and byte count. The payload is SPIR-V
for the same fragment entry point with its integer identity target enabled.
For mesh stages, a `TSPM` record uses the same layout to store the mesh
entry point with temporal position capture enabled.
The emitted table order and final trailer write are implemented in
[ShaderCompiler.cpp](../Editor/AssetCompilers/ShaderCompiler.cpp).

### Loader checklist
- Validate magic/version/headerSize before following offsets.
- Map arrays using counts and offsets; verify they fit inside the blob.
- Ensure `bytecodeOffset + bytecodeSize` stays within file bounds; format distinguishes SPIR-V vs DXIL.
- Use `StageToMask(stage)` when comparing visibility bits for descriptor bindings and push constants.
