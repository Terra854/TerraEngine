# Terra Engine Pack File (TEPAK) Proposal

TEPAK is a read-only export archive for TerraEngine assets. It wraps existing engine-native files such as `TEMF`, `TETF`, scene files, materials, shaders, and other cooked content into one compressed package that mounts as an engine virtual filesystem.

The format is built around one archive-level immutable dictionary plus independently compressed extents. That is the key design choice: TEPAK intentionally does not use an `xz` or `lzma` style solid stream with a constantly mutating history window. Each extent resets to the same shared dictionary, so any thread can decompress any extent without first decoding earlier extents.

Recommended file extension: `.tepak`
Required byte order: little-endian

## Design Goals
- Balance compression ratio and decompression speed for shipped game assets.
- Keep random access practical for filesystem-style mounting.
- Allow many decompression threads to share one in-memory dictionary safely.
- Allow many export worker threads to compress extents in parallel against the same dictionary.
- Remain read-only after export so the container can be heavily optimized for load-time behavior.
- Preserve asset-local access patterns such as texture mip reads and mesh stream reads.

## Non-Goals
- TEPAK is not an in-place editable archive format.
- TEPAK is not meant to be a solid archival format that maximizes ratio at the cost of random access.
- TEPAK is not a replacement for domain-specific codecs such as BC7, ASTC, or audio/video codecs.
- TEPAK is not intended to be exposed as a true OS-level zero-copy filesystem. It should be mounted as an engine VFS.

## Core Compression Model

### One Shared Immutable Dictionary
- A TEPAK archive contains exactly zero or one archive dictionary.
- When present, the dictionary is stored once in the archive as one contiguous byte array.
- The dictionary is immutable after the archive is written.
- All extents compressed with the shared-dictionary codec use the exact same dictionary bytes.
- No extent may depend on decompressed output from any other extent.

This is the important difference from `xz`-style solid streams:
- `xz` improves ratio by letting later data depend on earlier decoded history.
- TEPAK improves parallelism and random access by making every extent depend only on the archive dictionary plus its own compressed payload.

### Threading Rule
- Runtime decompression threads may read the same dictionary concurrently.
- Export compression threads may also reference the same dictionary concurrently.
- No thread may mutate the live archive dictionary.

If you try to make the dictionary itself mutable during compression, you recreate the same coupling problems that make solid archives hard to stream and hard to parallelize. TEPAK explicitly forbids that.

### Recommended Codec Backend
- Required codecs:
  - `RAW`
  - `ZSTD_SHARED_PREFIX`
- Optional codec:
  - `ZSTD`

`ZSTD_SHARED_PREFIX` means each extent payload is a normal Zstandard frame, but the decoder must be initialized with the TEPAK archive dictionary as an out-of-band raw prefix. The dictionary bytes are not duplicated inside each extent.

This gives a practical middle ground:
- Better ratio than plain per-file compression.
- Much better decompression latency and random access than a solid `xz` stream.
- Decode speed remains close to normal Zstandard decode speed.

## Dictionary Guidance

### Size
- Allowed dictionary size: any exact byte count from `0` to `64 MiB`

The dictionary does not need to use a power-of-two size. Only the dictionary start offset is aligned; `dictionarySize` stores the exact byte count.

Large dictionaries improve cross-file reuse, but they also increase mount-time cost and cache pressure. In practice, a moderate dictionary that stays resident is usually better than a huge dictionary that thrashes.

### Selection Policy
The exporter may choose dictionary size and dictionary contents algorithmically during export.

Recommended workflow:
1. Build a representative training corpus from the cooked asset set.
2. Train candidate dictionaries at multiple exact byte sizes.
3. Validate each candidate on held-out asset data instead of the training samples.
4. Score candidates using a cost function that considers compression ratio, decompression speed, and memory cost.
5. Select the best candidate, or select no dictionary when the measured benefit is too small.

The selected dictionary must still be immutable once written into the archive. TEPAK allows `dictionarySize = 0`, so the exporter may choose to omit the dictionary entirely.

### Composition
Because TEPAK uses one dictionary for the whole archive, the exporter should build a stitched dictionary from representative samples rather than blindly feeding it every byte type equally.

Recommended bias:
- High priority: scene data, material data, shader blobs, metadata headers, small structured binary tables.
- Medium priority: mesh index streams, meshlet tables, vertex streams with repeated patterns, animation curves.
- Low priority: already block-compressed texture top mips.
- Very low priority: already compressed audio/video payloads.

For textures, the dictionary is usually most helpful for:
- texture headers
- small mip tails
- UI textures or uncompressed look-up textures

It is usually less helpful for:
- large BCn or ASTC top mips

## Binary Layout (all little-endian)
```
TEPAKHeader               // fixed 128 bytes
FileTable[fileCount]      // fixed 56 bytes each
ExtentTable[extentCount]  // fixed 40 bytes each
PathTable                 // UTF-8 path bytes
DictionaryData            // optional, 64 KiB aligned
ExtentPayloads...         // each payload 4 KiB aligned
```

All offsets are absolute byte offsets from the start of the TEPAK file.

## TEPAKHeader (128 bytes)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `TPAK` |
| 4 | 2 | versionMajor | Start at `1` |
| 6 | 2 | versionMinor | Start at `0` |
| 8 | 4 | headerSize | Size of this header, must be `128` for version `1.0` |
| 12 | 4 | flags | Bitfield described below |
| 16 | 4 | fileCount | Number of logical files in the archive |
| 20 | 4 | extentCount | Number of stored extents |
| 24 | 4 | defaultExtentSize | Exporter hint in uncompressed bytes |
| 28 | 4 | pathTableBytes | Total bytes in `PathTable` |
| 32 | 8 | fileTableOffset | Offset to first `FileEntry` |
| 40 | 8 | extentTableOffset | Offset to first `ExtentEntry` |
| 48 | 8 | pathTableOffset | Offset to `PathTable` |
| 56 | 8 | dictionaryOffset | Offset to `DictionaryData`, `0` if absent |
| 64 | 8 | dictionarySize | Exact byte count in `DictionaryData`, `0` if absent |
| 72 | 8 | dataRegionOffset | Offset to first extent payload |
| 80 | 8 | archiveSize | Total TEPAK file size in bytes |
| 88 | 8 | dictionaryHash64 | `XXH3_64` of dictionary bytes, `0` if absent |
| 96 | 8 | metadataHash64 | `XXH3_64` of header + tables + path data with this field zeroed |
| 104 | 4 | codecMask | Bit 0=`RAW`, Bit 1=`ZSTD`, Bit 2=`ZSTD_SHARED_PREFIX` |
| 108 | 4 | reserved | Must be `0` |
| 112 | 16 | archiveId | 128-bit build identifier or content hash prefix |

Header flags:
- Bit 0: archive contains a dictionary
- Bit 1: `FileTable` is sorted by normalized path hash then full path bytes
- Bit 2: path lookups are case-insensitive after normalization
- Bit 3: per-extent hashes are present and should be validated when enabled by the loader
- Bits 4-31: reserved, must be `0`

## FileEntry (56 bytes)
Each file is a logical read-only byte stream assembled from one or more extents.

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 8 | pathHash64 | `XXH3_64` of normalized path bytes |
| 8 | 4 | pathOffset | Offset into `PathTable` |
| 12 | 4 | pathLength | UTF-8 bytes in the path |
| 16 | 4 | flags | Per-file hints described below |
| 20 | 2 | assetClass | Optional asset class enum |
| 22 | 2 | reserved | Must be `0` |
| 24 | 4 | firstExtent | Index into `ExtentTable` |
| 28 | 4 | extentCount | Number of extents for this file |
| 32 | 8 | uncompressedSize | Logical file size in bytes |
| 40 | 8 | storedSize | Total stored bytes across all extents |
| 48 | 8 | uncompressedHash64 | `XXH3_64` of the full logical file, `0` if unused |

File flags:
- Bit 0: preload hint
- Bit 1: streaming hint
- Bit 2: file is likely already compressed, so raw storage is acceptable
- Bits 3-31: reserved

Suggested `assetClass` values:
- `0` unknown
- `1` mesh
- `2` texture
- `3` shader
- `4` scene
- `5` material
- `6` animation
- `7` audio
- `8` script
- `9` table/blob

Paths:
- UTF-8
- relative to archive root
- use `/` as separator
- no leading `/`
- no `.` or `..` segments
- directory structure is implicit from file paths

## ExtentEntry (40 bytes)
Each extent represents one independently retrievable uncompressed range within a file.

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 8 | fileOffset | Logical byte offset within the owning file |
| 8 | 8 | storedOffset | Absolute byte offset to the payload |
| 16 | 4 | storedSize | Bytes on disk, excluding alignment padding |
| 20 | 4 | uncompressedSize | Bytes produced after decode |
| 24 | 8 | uncompressedHash64 | `XXH3_64` of the decoded extent, `0` if unused |
| 32 | 2 | codec | Codec enum |
| 34 | 2 | flags | Reserved for future use, set `0` |
| 36 | 4 | reserved | Must be `0` |

Extent rules:
- Extents belonging to one file must be sorted by `fileOffset`.
- Extents must not overlap in logical file space.
- `storedOffset` must be aligned to `4096` bytes.
- Extents are independent decode units.
- Exporters should keep extents large enough for ratio but small enough for random reads.

Codec enum:
- `0` = `RAW`
- `1` = `ZSTD`
- `2` = `ZSTD_SHARED_PREFIX`

Codec behavior:
- `RAW`: payload bytes are returned exactly as stored.
- `ZSTD`: payload is a standard Zstandard frame without the archive dictionary.
- `ZSTD_SHARED_PREFIX`: payload is a standard Zstandard frame decoded with the archive dictionary as an immutable prefix.

An archive with `dictionarySize == 0` must not contain `ZSTD_SHARED_PREFIX` extents.

## Alignment Rules
- `DictionaryData` should start on a `64 KiB` boundary.
- Each extent payload must start on a `4 KiB` boundary.
- Alignment padding bytes must be zero.
- `storedSize` excludes any trailing alignment padding.

These rules make async file IO and direct-storage style batching easier without forcing a solid-stream layout.

## Mounting Model

### Recommendation
Mounting TEPAK as an engine virtual filesystem with the dictionary loaded into memory is a good idea.

It becomes a bad idea only if you expect TEPAK to behave like a true zero-copy memory-mapped filesystem for arbitrary byte ranges. TEPAK is chunk-addressable, not raw-byte-addressable.

### Runtime Mount Steps
1. Read and validate the header.
2. Read `FileTable`, `ExtentTable`, and `PathTable`.
3. Load or memory-map the dictionary if present.
4. Build the archive VFS index.
5. Serve file reads by locating extents, issuing async IO, and decompressing into a cache.

### Recommended Runtime Behavior
- Keep metadata resident.
- Keep the dictionary resident if memory budget allows.
- Use a decompressed extent cache keyed by archive id plus extent index.
- Give each worker thread its own decompression context.
- Share only the immutable dictionary bytes across threads.

This lets one thread decompress a mesh while another thread decompresses texture mip data from the same archive without synchronization on the dictionary itself.

## Export Pipeline
TEPAK should be written in two passes.

### Pass 1: Prepare Data and Build the Dictionary
1. Serialize each asset into its engine-native exported byte stream first.
2. Decide extent boundaries using asset-aware rules.
3. Sample representative bytes across the export set.
4. Train and select zero or one immutable archive dictionary.

The important point is that the dictionary is finalized before extent compression starts.

### Pass 2: Compress Extents in Parallel
1. Split each file into extents.
2. Compress extents independently on multiple worker threads.
3. Use the same dictionary for every `ZSTD_SHARED_PREFIX` extent.
4. Fall back to `RAW` when compression gain is too small.
5. Write tables and payloads deterministically.

Because every extent is independent, export workers scale well across cores.

## Recommended Extent Sizing
TEPAK should be asset-aware instead of forcing one fixed block size on every asset class.

| Asset Type | Recommended Uncompressed Extent Size | Notes |
| --- | --- | --- |
| Mesh / animation | `128 KiB` to `512 KiB` | Prefer boundaries at stream or meshlet table boundaries |
| Texture | Per-mip or `256 KiB` groups | Never split inside a compressed block; merge tiny mip tails |
| Scene / material / shader / script | `64 KiB` to `256 KiB` | Usually compresses well with the shared dictionary |
| Audio / video | Raw or domain codec | Do not expect TEPAK compression to beat dedicated media codecs |

General rule:
- Small extents improve random access and reduce over-read.
- Large extents improve ratio.
- A good default for mixed archives is `256 KiB`.

## Texture-Specific Guidance
Textures need special handling because many game textures are already GPU-compressed.

Recommended rules:
- Keep mip boundaries visible to the extent splitter.
- Store very small mip tails together.
- Prefer `RAW` when a BCn or ASTC mip compresses poorly.
- Use `ZSTD_SHARED_PREFIX` mainly for headers, low mips, UI textures, and uncompressed tables.

This avoids wasting CPU time decompressing data that was already close to its entropy limit.

## Mesh-Specific Guidance
Meshes usually benefit more from the shared dictionary than GPU-ready textures do.

Recommended rules:
- Keep vertex, index, meshlet, and bounds streams on clean boundaries when possible.
- Avoid mixing unrelated large streams into one giant extent just for ratio.
- Preserve small repetitive metadata near the start of the file where the dictionary is most likely to help.
- Use the archive cache to keep frequently streamed mesh extents warm.

## Compression Fallback Rule
The exporter should not force compression on incompressible data.

Recommended rule:
- If `storedSize` is not at least `5%` smaller than `uncompressedSize`, store the extent as `RAW`.

This keeps decode cost honest and avoids pretending every asset benefits from archive compression.

## Integrity and Validation
Loaders should validate:
- magic, version, and header size
- all table offsets and sizes stay inside `archiveSize`
- `dictionaryOffset` and `dictionarySize` are valid when the dictionary flag is set
- every file path range is inside `PathTable`
- extents for each file are sorted and non-overlapping
- `storedOffset + storedSize` stays inside the archive
- codec values are known

Optional validation:
- verify `metadataHash64` during mount
- verify `dictionaryHash64` after loading the dictionary
- verify `uncompressedHash64` per extent or per file in debug, QA, or corruption-sensitive builds

## Filesystem Semantics
TEPAK archives are read-only after export.

Recommended exposed operations:
- `Open(path)`
- `Read(handle, offset, size)`
- `GetSize(path)`
- `List(directory)`
- `Exists(path)`

Not recommended:
- mutable append
- in-place overwrite
- direct writable memory mapping of logical file bytes

## Why This Is Better Than a Solid `xz` Archive for Runtime
- Random reads do not require replaying the whole archive history.
- Multiple worker threads can decode unrelated files at the same time.
- The shared dictionary captures cross-file redundancy without coupling extents to one another.
- Export can compress many extents in parallel.

The tradeoff is simple:
- A solid archive can still win on absolute ratio.
- TEPAK is usually much better for real game runtime behavior.

For an engine-mounted asset package, that trade is usually correct.

## Versioning
- Bump `versionMinor` for additive, backward-compatible changes.
- Bump `versionMajor` for breaking layout or semantic changes.
- Unknown future flags must be rejected unless explicitly marked ignorable by a later version of the spec.

## Example Minimal Layout
```
00000000  TEPAK header
00000080  FileTable
000001C0  ExtentTable
00000300  PathTable
00010000  DictionaryData               // 64 KiB aligned
00810000  Extent payload 0             // 4 KiB aligned
00814000  Extent payload 1
0081C000  Extent payload 2
...
```

## Final Recommendation
If you want one dictionary and a mounted archive filesystem, keep the dictionary immutable and global, then make every stored extent independent.

That gives you:
- one dictionary in memory
- parallel reads
- parallel export compression
- solid enough compression for real asset packs
- predictable runtime behavior

That is the right compromise for a game engine archive format. Avoid a mutable sliding dictionary once the archive is finalized.
