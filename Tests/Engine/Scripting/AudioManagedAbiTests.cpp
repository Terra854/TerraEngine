// AudioManagedAbiTests.cpp - fixed-width native bridge layout checks.

#include "../TestHarness.h"
#include "Scripting/ScriptGlue.h"

namespace TerraEngine::Tests {
	void RegisterAudioManagedAbiTests(TestRegistry& registry) {
		registry.Add("Audio.ManagedAbi.VersionAndLayoutsAreFixed", [](TestContext& context) {
			context.ExpectEqual(1U, Native_Audio_GetAbiVersion(), "The managed bridge must report audio ABI v1.");
			context.Expect(sizeof(NativeAudioAbiHeader) == 8U && offsetof(NativeAudioCommandRequest, header) == 0U,
				"Every audio record must begin with the fixed-width v1 header.");
			NativeAudioCommandRequest invalid{};
			NativeAudioCommandReceipt receipt{};
			context.Expect(Native_Audio_SubmitCommand(&invalid, &receipt) == NativeAudioStatus::VersionMismatch,
				"Mismatched managed records must be rejected before touching live state.");
		});
	}
}
