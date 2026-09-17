// ExportVTable.h — Contains declarations for the Export V Table file-specific types, helper functions, and module integration code used by native script glue, exported C# vtables, and scripting system lifecycle state.

// ExportVTable.h — C-compatible interop contract for NativeAOT exported builds.
//
// In exported (NativeAOT) builds the managed gameplay layer compiles ahead-of-time
// into a shared library with a retained legacy initializer and a capacity-aware
// current initializer. The selected function populates this ManagedVTable with
// operations the C++ engine calls directly, bypassing hostfxr entirely.
//
// Both sides (C++ and C#) must agree on the struct layout. The C# mirror lives in
// Engine/Managed/Source/Export/ExportVTable.cs. Any change to this header must be
// reflected there and vice versa.

#pragma once

#include <cstddef>
#include <cstdint>

#pragma pack(push, 8)

// Blittable field descriptor crossing the native–managed boundary (AC-004).
// Each ExportFieldDescriptor describes one serializable field on a script type
// and carries enough metadata for the engine to resolve scene bindings at load
// time and to read/write field values at runtime via the VTable getters/setters.
enum class ExportFieldValueKind : int32_t {
	Bool = 0,
	Int = 1,
	Float = 2,
	EntityReference = 3,
	Vector2 = 4,
	Vector3 = 5,
	Vector4 = 6,
	Quaternion = 7,
	String = 8
};

// struct ExportFieldDescriptor groups related state and operations so callers have one documented contract.
struct ExportFieldDescriptor {
	int32_t   fieldIndex;       // Zero-based index within the script type
	int32_t   valueKind;        // ExportFieldValueKind value for this field
	int32_t   valueSize;        // Size in bytes of the blittable representation
	uint64_t  nameHash;         // Deterministic FNV-1a hash of the field name
	char      nameUtf8[64];     // Null-terminated UTF-8 field name (truncated if >63 chars)
};

struct ExportScriptBatchItem {
	void* handle;
	float deltaTime;
	uint64_t frameOrder;
	uint64_t objectGeneration;
	uint64_t componentGeneration;
	uint64_t bindingGeneration;
	uint32_t objectId;
	int32_t workerDispatch;
};

// Function-pointer table populated by the NativeAOT managed library during its
// selected initializer. The C++ engine consumes this struct exclusively in
// exported builds — no other interop channel is used (AC-003).
//
// Versioning: `version` is set by the managed side and checked by the engine.
// New function pointers are appended to the end of the struct so older
// engines can safely ignore fields they don't know about.
struct ManagedVTable {
	uint64_t version;           // Struct version for forward compatibility (v1 = 1)

	// --- Script registry queries ---
	// How many script types did the source generator discover at compile time.
#ifdef _WIN32
	int32_t  (__cdecl* getScriptCount)();
	// Fill outBuffer with up to capacity script IDs (deterministic FNV-1a hashes
	// of each script type's fully qualified name).
	void     (__cdecl* getScriptIds)(uint64_t* outBuffer, int32_t capacity);

	// --- Script instance lifecycle ---
	// Create one script instance by its type ID and associated engine object ID.
	// Returns an opaque handle the engine stores alongside the ScriptComponent.
	void*    (__cdecl* createScript)(uint64_t scriptId, uint32_t objectId);
	// Destroy a previously created script instance.
	void     (__cdecl* destroyScript)(void* handle);
	// Lifecycle callbacks dispatched by ScriptingSystem in the same batched order
	// as the editor runtime. The engine guarantees Awake→Start→Update ordering.
	void     (__cdecl* invokeAwake)(void* handle);
	void     (__cdecl* invokeStart)(void* handle);
	void     (__cdecl* invokeUpdate)(void* handle, float deltaTime);
	void     (__cdecl* invokeDisable)(void* handle);

	// --- Field introspection and access ---
	// Returns how many serializable fields exist for a given script type.
	int32_t  (__cdecl* getFieldCount)(uint64_t scriptId);
	// Fill outBuffer with FieldDescriptor metadata for up to capacity fields.
	void     (__cdecl* getFieldDescriptors)(uint64_t scriptId, ExportFieldDescriptor* outBuffer, int32_t capacity);
	// Read one field's blittable value from a live script instance.
	void     (__cdecl* getFieldValue)(void* handle, int32_t fieldIndex, void* outValue, int32_t valueSize);
	// Write one field's blittable value into a live script instance.
	void     (__cdecl* setFieldValue)(void* handle, int32_t fieldIndex, const void* inValue, int32_t valueSize);
	void     (__cdecl* invokeUpdateBatch)(const ExportScriptBatchItem* items, int32_t itemCount);
	// v2 tail: appended so v1 offsets remain ABI-compatible. tableSize lets the
	// host reject a truncated table before it reads these optional physics hooks.
	uint64_t tableSize;
	uint32_t (__cdecl* getPhysicsAbiVersion)();
	void     (__cdecl* dispatchPhysicsEventBatch)(const void* events, uint32_t eventCount, uint64_t worldId, uint32_t worldGeneration, uint64_t tick);
	// v3 tail: copied animation events use the same protected callback shape in
	// hosted and NativeAOT runtimes. Both version and tableSize must authorize
	// these fields before the engine reads either pointer.
	uint16_t (__cdecl* getAnimationAbiVersion)();
	void     (__cdecl* dispatchAnimationEventBatch)(const void* events, uint32_t eventCount, int64_t sourceTick,
		uint64_t publicationGeneration, uint32_t overflowed, uint32_t droppedCountLowerBound);
	// v4 tail: this probe roots the managed navigation facade in NativeAOT and
	// lets the host reject a mixed native/managed ABI before any route packet is
	// copied across the boundary.
	uint32_t (__cdecl* getNavigationAbiVersion)();
#else
	int32_t  (*getScriptCount)();
	void     (*getScriptIds)(uint64_t* outBuffer, int32_t capacity);
	void*    (*createScript)(uint64_t scriptId, uint32_t objectId);
	void     (*destroyScript)(void* handle);
	void     (*invokeAwake)(void* handle);
	void     (*invokeStart)(void* handle);
	void     (*invokeUpdate)(void* handle, float deltaTime);
	void     (*invokeDisable)(void* handle);
	int32_t  (*getFieldCount)(uint64_t scriptId);
	void     (*getFieldDescriptors)(uint64_t scriptId, ExportFieldDescriptor* outBuffer, int32_t capacity);
	void     (*getFieldValue)(void* handle, int32_t fieldIndex, void* outValue, int32_t valueSize);
	void     (*setFieldValue)(void* handle, int32_t fieldIndex, const void* inValue, int32_t valueSize);
	void     (*invokeUpdateBatch)(const ExportScriptBatchItem* items, int32_t itemCount);
	uint64_t tableSize;
	uint32_t (*getPhysicsAbiVersion)();
	void     (*dispatchPhysicsEventBatch)(const void* events, uint32_t eventCount, uint64_t worldId, uint32_t worldGeneration, uint64_t tick);
	uint16_t (*getAnimationAbiVersion)();
	void     (*dispatchAnimationEventBatch)(const void* events, uint32_t eventCount, int64_t sourceTick,
		uint64_t publicationGeneration, uint32_t overflowed, uint32_t droppedCountLowerBound);
	uint32_t (*getNavigationAbiVersion)();
#endif
};

#pragma pack(pop)

// NativeAOT libraries retain the v1-only legacy export for old hosts and add a
// Capacity-aware exports preserve their named released sizes. The host prefers
// v4, then v3, and finally the legacy v1 prefix; each initializer returns 0 on
// success without writing past the capacity it was given.
#ifdef _WIN32
using InitManagedRuntimeFn = int32_t(__cdecl*)(ManagedVTable* outTable);
using InitManagedRuntimeV3Fn = int32_t(__cdecl*)(ManagedVTable* outTable, uint64_t hostCapacity);
using InitManagedRuntimeV4Fn = int32_t(__cdecl*)(ManagedVTable* outTable, uint64_t hostCapacity);
#else
using InitManagedRuntimeFn = int32_t(*)(ManagedVTable* outTable);
using InitManagedRuntimeV3Fn = int32_t(*)(ManagedVTable* outTable, uint64_t hostCapacity);
using InitManagedRuntimeV4Fn = int32_t(*)(ManagedVTable* outTable, uint64_t hostCapacity);
#endif

// V1 and V2 sizes stay pinned for older exports. V3 appends the fixed-width
// animation event bridge; tableSize guards every tail before dereferencing it.
inline constexpr uint32_t kManagedVTableMinVersion = 1;
inline constexpr uint32_t kManagedVTablePhysicsVersion = 2;
inline constexpr uint32_t kManagedVTableAnimationVersion = 3;
inline constexpr uint32_t kManagedVTableNavigationVersion = 4;
inline constexpr uint32_t kManagedPhysicsAbiVersion = 1;
inline constexpr uint16_t kManagedAnimationAbiVersion = 1;
inline constexpr uint32_t kManagedNavigationAbiVersion = 2;
inline constexpr uint64_t kManagedVTableV1Size = 112;
inline constexpr uint64_t kManagedVTableV2Size = 136;
inline constexpr uint64_t kManagedVTableV3Size = 152;
inline constexpr uint64_t kManagedVTableV4Size = 160;

// Pin every released tail independently. A later v4 append must introduce its
// own size constant instead of silently changing what a v3 host accepts.
static_assert(offsetof(ManagedVTable, getAnimationAbiVersion) == kManagedVTableV2Size);
static_assert(offsetof(ManagedVTable, dispatchAnimationEventBatch) == 144U);
static_assert(offsetof(ManagedVTable, getNavigationAbiVersion) == kManagedVTableV3Size);
static_assert(sizeof(ManagedVTable) == kManagedVTableV4Size);
