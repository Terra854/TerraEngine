# Unbounded Scene Formats

Only these versioned formats are accepted:

- `TerraSceneUnboundedV1`
- `TerraRegionUnboundedV1`
- `TerraPrefabUnboundedV1`
- `TerraSpatialTransactionV1`

Cell axes are JSON decimal strings, not JSON numbers, so all signed 64-bit values remain exact. Offsets are finite JSON numbers and must already be normalized to `[-32768, 32768)`. Loaders reject unversioned documents, unknown versions, numeric cell fields, non-finite or unnormalized offsets, invalid hashes, mismatched cache lineage, and legacy three-number positions before live objects are created.

An exact position is encoded as:

```json
{
  "cell": { "x": "15258789062", "y": "-15258789063", "z": "0" },
  "offsetMetres": [1.25, -2.5, 0.001]
}
```

Minimal scene, prefab, and transaction envelopes are:

```json
{ "format": "TerraSceneUnboundedV1", "objects": [] }
```

```json
{ "format": "TerraPrefabUnboundedV1", "sourceId": "0123456789abcdef0123456789abcdef", "objects": [] }
```

```json
{ "format": "TerraSpatialTransactionV1", "id": "move-observer-42", "operations": [] }
```

Region artifacts use `TerraRegionUnboundedV1`, exact signed region-key strings, a content hash, and one source generation. Derived spatial caches use the new unbounded lineage and are discarded when their format, coordinate model, source hash, or generation does not match.

There is no migration path and no dual writing. Legacy scene, prefab, transaction, camera, and derived-cache payloads must be recreated rather than converted.
