# Effectively Unbounded Scene Fixtures

This directory is the version-1 fixture inventory for the unbounded-world contract. Fixtures are
authored directly in the canonical `WorldCellD64V1` model; no legacy decoder or
position conversion is part of the test contract.

## Inventory

| ID | Format | Purpose | Evidence requirement |
| --- | --- | --- | --- |
| `scene-origin-sparse-v1` | `TerraSceneUnboundedV1` | Small resident scene near the origin with sparse region metadata. | Exact cell strings and normalized offsets survive repeated load/save. |
| `scene-extreme-negative-v1` | `TerraSceneUnboundedV1` | Resident content around `-10^15 m` on all axes. | Millimetre local deltas and negative carry normalization remain stable. |
| `scene-extreme-positive-v1` | `TerraSceneUnboundedV1` | Resident content around `+10^15 m` on all axes. | Positive carry normalization matches the origin fixture. |
| `scene-sparse-extent-1000x-v1` | `TerraSceneUnboundedV1` | Same resident objects with region keys expanded by 1,000x. | Resident memory follows the working set, not authored extent. |
| `scene-crossing-seed-v1` | `TerraSceneUnboundedV1` | Deterministic route seed for 10,000 region/frame crossings. | Stable crossing hash, no duplicate/lost publication, and bounded queues. |
| `legacy-float-position` | Unsupported legacy payload | Deliberately old scene with a three-number float position. | Rejected before object or snapshot creation with `UnsupportedFormat`. |
| `legacy-prefab-position` | Unsupported legacy payload | Deliberately old prefab transform payload. | Rejected before live prefab state exists. |
| `legacy-transaction-position` | Unsupported legacy payload | Deliberately old transaction with an absolute float translation. | Rejected before mutation preparation. |

All new-format fixture identities, hashes, region keys, and expected rejection
reasons are recorded in `fixture-manifest.json`. Scenario runners should copy
these IDs and content hashes into their evidence, rather than inventing names at
runtime.
