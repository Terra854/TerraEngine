// Native loader fixture: exercises the released v1 table protocol without a .NET installation.
#include "Scripting/ExportVTable.h"

#include <cstring>

namespace {
	// An identifiable real function proves the loaded pointer is callable.
	int32_t ScriptCount() { return 17; }
	void ScriptIds(uint64_t* output, int32_t capacity) { if(output != nullptr && capacity > 0) output[0] = 0xF17U; }
	void* CreateScript(uint64_t, uint32_t) { return nullptr; }
	void DestroyScript(void*) {}
	void Lifecycle(void*) {}
	void Update(void*, float) {}
	int32_t FieldCount(uint64_t) { return 0; }
	void FieldDescriptors(uint64_t, ExportFieldDescriptor*, int32_t) {}
	void ReadField(void*, int32_t, void*, int32_t) {}
	void WriteField(void*, int32_t, const void*, int32_t) {}
	void UpdateBatch(const ExportScriptBatchItem*, int32_t) {}
}

// Only the capacity-aware entry point is exported, matching the production
// loader's preferred ABI. V1 needs no optional physics/animation callbacks.
#ifdef _WIN32
extern "C" __declspec(dllexport) int32_t __cdecl InitManagedRuntimeV4(ManagedVTable* output, uint64_t capacity) {
#else
extern "C" __attribute__((visibility("default"))) int32_t InitManagedRuntimeV4(ManagedVTable* output, uint64_t capacity) {
#endif
	if(output == nullptr || capacity < kManagedVTableV1Size) return 1;
	ManagedVTable table{};
	table.version = kManagedVTableMinVersion;
	table.getScriptCount = ScriptCount;
	table.getScriptIds = ScriptIds;
	table.createScript = CreateScript;
	table.destroyScript = DestroyScript;
	table.invokeAwake = Lifecycle;
	table.invokeStart = Lifecycle;
	table.invokeUpdate = Update;
	table.invokeDisable = Lifecycle;
	table.getFieldCount = FieldCount;
	table.getFieldDescriptors = FieldDescriptors;
	table.getFieldValue = ReadField;
	table.setFieldValue = WriteField;
	table.invokeUpdateBatch = UpdateBatch;
	// Copy exactly the released prefix so this fixture also respects smaller hosts.
	std::memcpy(output, &table, kManagedVTableV1Size);
	return 0;
}
