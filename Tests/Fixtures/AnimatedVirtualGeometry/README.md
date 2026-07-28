# Animated Virtual Geometry Fixture

`manifest.json` describes a deterministic procedural test asset. Tests generate
the compact geometry at runtime so the repository does not need to store an
opaque binary mesh. The fixture spans parent and child pages, uses both skinning
and a morph target, and exercises history invalidation and fallback coverage.

The `Shaders` directory contains one supported surface shader plus intentionally
invalid contract fixtures used by fail-closed validation tests.
