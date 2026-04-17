// ExportVTable.h — C-compatible interop contract for NativeAOT exported builds.
//
// In exported (NativeAOT) builds the managed gameplay layer compiles ahead-of-time
// into a shared library that exposes a single C entry point: InitManagedRuntime().
// That function populates this ManagedVTable struct with pointers to all operations
// the C++ engine needs. The engine then calls these function pointers directly,
// bypassing the hostfxr runtime entirely.
//
// Both sides (C++ and C#) must agree on the struct layout. The C# mirror lives in
// Engine/Managed/Source/Export/ExportVTable.cs. Any change to this header must be
// reflected there and vice versa.

#pragma once

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

struct ExportFieldDescriptor {
	int32_t   fieldIndex;       // Zero-based index within the script type
	int32_t   valueKind;        // ExportFieldValueKind value for this field
	int32_t   valueSize;        // Size in bytes of the blittable representation
	uint64_t  nameHash;         // Deterministic FNV-1a hash of the field name
	char      nameUtf8[64];     // Null-terminated UTF-8 field name (truncated if >63 chars)
};

// Function-pointer table populated by the NativeAOT managed library during
// InitManagedRuntime(). The C++ engine consumes this struct exclusively in
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
#endif
};

#pragma pack(pop)

// The single C-exported entry point from the NativeAOT shared library.
// The engine calls this exactly once during startup to populate the VTable.
// Returns 0 on success, non-zero on failure.
#ifdef _WIN32
using InitManagedRuntimeFn = int32_t(__cdecl*)(ManagedVTable* outTable);
#else
using InitManagedRuntimeFn = int32_t(*)(ManagedVTable* outTable);
#endif

// Minimum VTable version the engine requires for correct operation.
inline constexpr uint32_t kManagedVTableMinVersion = 1;
