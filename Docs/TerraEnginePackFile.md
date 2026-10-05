# TerraEngine Pack File (TEPAK)

TEPAK is the read-only archive used by runtime content mounts and exported games. The current writer and `Editor/Export/model/PackModel.h` define format 1.0. Runtime and editor readers use the same persisted layout and acceptance rules. Existing cooked assets remain separate logical files inside the archive.

## Binary layout

All integer fields use little-endian representation. The current 64-bit record ABI uses the sizes below. The writer emits complete record structures, including their internal padding; readers hash those exact bytes rather than reconstructing structures for hashing.

```text
BinaryHeader                       144 bytes
BinaryDirectoryRecord[count]        40 bytes each
BinaryEntryRecord[count]            64 bytes each
BinaryExtentRecord[count]           48 bytes each
PathTable                           UTF-8 bytes
DictionaryData                      optional bytes
ExtentPayloads                      independently encoded bytes
```

Metadata section starts and dictionary/data starts are aligned to eight bytes by the writer. Payload extents are written consecutively; four-kilobyte payload alignment and 64-kilobyte dictionary alignment are not format requirements. Every stored offset is absolute from the beginning of the archive. Path offsets are relative to the path table. Alignment padding is excluded from metadata hashing.

## Header

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | magic: `TPAK` |
| 4 | 4 | versionMajor: 1 |
| 8 | 4 | versionMinor: 0 |
| 12 | 4 | headerSize: 144 |
| 16 | 4 | flags: bit 0 indicates dictionary presence |
| 20 | 4 | directoryCount |
| 24 | 4 | fileCount |
| 28 | 4 | extentCount |
| 32 | 4 | defaultExtentSize |
| 36 | 4 | rootDirectoryId: 0 |
| 40 | 8 | pathTableBytes |
| 48 | 8 | directoryTableOffset |
| 56 | 8 | fileTableOffset |
| 64 | 8 | extentTableOffset |
| 72 | 8 | pathTableOffset |
| 80 | 8 | dictionaryOffset |
| 88 | 8 | dictionarySize |
| 96 | 8 | dataRegionOffset |
| 104 | 8 | archiveSize |
| 112 | 8 | dictionaryHash |
| 120 | 8 | metadataHash |
| 128 | 8 | codecMask |
| 136 | 8 | archiveIdentifier |

An absent dictionary has zero size, an aligned dictionary offset, and the real XXH3 hash of empty bytes. Its hash is not a zero sentinel. No header hash is stored; physical bounds and structural validation therefore remain necessary even when metadata hashes match.

## Directory records

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | pathHash |
| 8 | 4 | pathOffset |
| 12 | 4 | pathLength |
| 16 | 4 | parentDirectoryId |
| 20 | 4 | firstChildDirectoryIndex |
| 24 | 4 | childDirectoryCount |
| 28 | 4 | firstChildFileIndex |
| 32 | 4 | childFileCount |
| 36 | 4 | record padding |

Directory zero is the root: its path is empty and its parent is `0xffffffff`. Direct children occupy contiguous table spans. Empty spans may start at the end of their table. Every other directory and every file has exactly one owner reachable from root, and its stored parent/path agree with that owner. Validation and subtree traversal use bounded iterative walks, allowing deep valid trees without recursion or repeated visits.

## File records

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | parentDirectoryId |
| 4 | 4 | record padding |
| 8 | 8 | pathHash |
| 16 | 4 | pathOffset |
| 20 | 4 | pathLength |
| 24 | 4 | entryFlags |
| 28 | 4 | assetClass |
| 32 | 4 | firstExtentIndex |
| 36 | 4 | entryExtentCount |
| 40 | 8 | logicalSize |
| 48 | 8 | storedSize |
| 56 | 8 | entryHash |

Asset-class values follow the persisted `ArchiveAssetClass` enum; existing values must not be renumbered. Paths are canonical relative UTF-8 names using `/`, with no rooted names, empty segments, traversal, alternate separators, streams, device names, or filesystem aliases. Lookup normalization does not repair stored archive paths.

## Extent records and codecs

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | fileOffset |
| 8 | 8 | storedOffset |
| 16 | 8 | storedSize |
| 24 | 8 | uncompressedSize |
| 32 | 8 | extentHash |
| 40 | 4 | codec |
| 44 | 4 | extentFlags |

Codec 0 is Raw identity storage, codec 1 is Zstd, and codec 2 is Zstd using the archive's prepared trained dictionary. Dictionary compression uses the existing Zstd dictionary APIs; it is not a mutable shared prefix stream. Each extent decodes independently and must produce exactly its declared length. Raw stored and decoded sizes must match. Compressed storage requires nonempty encoded bytes.

Each file owns a unique ordered contiguous extent run covering exactly `[0, logicalSize)`. The writer represents an empty file with one Raw extent having zero stored and decoded sizes. Paged production TEMF files may use a metadata-prefix extent followed by extents matching their declared contiguous encoded page ranges.

## Integrity and publication

All hashes use XXH3-64 without a placeholder bypass:

| Hash | Exact input |
| --- | --- |
| Metadata | Serialized directory records, file records, extent records, then path bytes |
| Dictionary | Dictionary bytes, including empty bytes when absent |
| Path | Stored canonical UTF-8 path bytes |
| Extent | Exact decoded extent bytes, including empty bytes |
| Entry | Complete decoded file bytes in logical order |

Metadata excludes the header, section alignment padding, dictionary, and payload. Mount checks metadata/dictionary integrity and complete structure before publishing a candidate; a failed replacement retains the prior mount. Payload integrity remains lazy: range reads verify only touched extents, while full reads and extraction also verify the complete entry. In-bounds zero-length range reads succeed; full empty-file reads still verify empty hashes.

Extraction preflights the complete selection and anchors its destination directories. Windows anchors reject reparse points and deny write/delete sharing; final symbolic/hard links are rejected. Verified bytes are staged privately and renamed through the still-exclusive staging handle, so failure cannot truncate the prior destination or publish substituted staging bytes. POSIX source uses directory-relative no-follow operations; Linux execution and concurrent same-user staging-name replacement validation remain deferred.

## Reproducing validation

Use the repository's bundled dependencies and configured C++20 toolchain. From a clean checkout:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target install
ctest --test-dir out/build/RelWithDebInfo -R 'ContentArchiveSafety|PackRoundTrip' --output-on-failure
```

See `Docs/Testing.md` for the current harness/CTest workflow and the maintained archive watchdog for individual malformed/deep tree cases. Writer-produced Raw, Zstd, dictionary, paged TEMF, and empty-file corpora are the compatibility oracle; root-only/deep fixtures are explicitly derived serialized metadata because the writer requires at least one eligible staged file.
