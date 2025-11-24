# Terra Engine Material File (TMAT) Specification

TMAT groups physically based material parameters and texture bindings in a single, versioned blob. All data is little-endian and packed for straightforward loading into render backends.

## Design goals
- Stable layout with explicit offsets for arrays to allow direct memory mapping.
- Compact header that captures common PBR parameters without external JSON.
- Texture bindings reference TETF assets by UUID to keep resources decoupled from paths.

## Binary layout (all little-endian)
```
Header               // fixed-size, packed
TextureBinding[]     // optional; count = Header.textureCount
FloatParameter[]     // optional; count = Header.floatParameterCount
String/data tables   // none required; structs are self-contained
```

### Header (84 bytes)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `TMAT` |
| 4 | 2 | version | Current version = 1 |
| 6 | 2 | headerSize | Always `sizeof(Header)` (84) |
| 8 | 4 | flags | Bitmask: 1=DoubleSided, 2=UseVertexColor, 4=ReceivesShadows |
| 12 | 4 | shadingModel | 0=Unlit, 1=MetallicRoughness, 2=SpecularGlossiness |
| 16 | 4 | alphaMode | 0=Opaque, 1=Mask, 2=Blend |
| 20 | 16 | baseColorFactor | RGBA multiplier (float[4]) |
| 36 | 12 | emissiveFactor | RGB emissive (float[3]) |
| 48 | 4 | metallicFactor | Float |
| 52 | 4 | roughnessFactor | Float |
| 56 | 4 | normalScale | Float |
| 60 | 4 | occlusionStrength | Float |
| 64 | 4 | alphaCutoff | Float (used when alphaMode = Mask) |
| 68 | 4 | textureCount | Number of `TextureBinding` entries |
| 72 | 4 | floatParameterCount | Number of `FloatParameter` entries |
| 76 | 4 | textureArrayOffset | Byte offset to first `TextureBinding` |
| 80 | 4 | floatParameterOffset | Byte offset to first `FloatParameter` |

### TextureBinding (56 bytes each, tightly packed)
- `semantic` (uint32): 0=BaseColor, 1=Normal, 2=MetallicRoughness, 3=Occlusion, 4=Emissive, 5=Height
- `textureUuid` (16 bytes): TerraEngine::UUID referencing a TETF asset
- `samplerFilter` (uint32): 0=Nearest, 1=Bilinear, 2=Trilinear, 3=Anisotropic
- `addressU/V/W` (uint32 each): 0=Repeat, 1=MirroredRepeat, 2=ClampToEdge, 3=ClampToBorder
- `uvSetIndex` (uint32): Which UV channel to sample (0 or 1)
- `uvScale` (float[2]): Multiplicative UV scale
- `uvOffset` (float[2]): Additive UV offset after scaling

Bindings are stored contiguously starting at `textureArrayOffset`; there is no per-entry padding beyond the packed struct layout.

### FloatParameter (24 bytes each)
- `nameId` (uint32): Stable identifier for the target uniform/constant slot
- `componentCount` (uint32): 1–4
- `value` (float[4]): Only first `componentCount` values are consumed

Float parameters start at `floatParameterOffset` and are tightly packed.

### Loader checklist
- Validate magic/version/headerSize and ensure offsets fall inside the blob.
- Bounds-check `textureCount` and `floatParameterCount` before slicing the arrays.
- When sampling, prefer SRGB-aware textures based on material semantics; TMAT itself does not enforce color space beyond the semantic hints.
