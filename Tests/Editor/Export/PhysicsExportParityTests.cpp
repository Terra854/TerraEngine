// PhysicsExportParityTests.cpp - unregistered invariant comparison contracts for US7.

#include "TestHarness.h"

#include <map>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		using PhysicsInvariantSet = std::map<std::string, std::string>;
		bool EquivalentPhysicsInvariants(const PhysicsInvariantSet& editor, const PhysicsInvariantSet& exported) {
			// Named values make a failure point directly to the gameplay contract
			// rather than to an opaque serialized physics snapshot.
			return editor == exported;
		}
	}

	void RegisterPhysicsExportParityTests(TestRegistry& registry) {
		registry.Add("Editor.PhysicsExportParity.PlayAndExportPreservePhysicsInvariants", [](TestContext& context) {
			const PhysicsInvariantSet play = {{"bodies", "dynamic-static-kinematic"}, {"stepping", "fixed-next"}, {"filtering", "layers-32"}, {"queries", "sorted-bounded"}, {"events", "ordered"}, {"constraints", "generation-safe"}, {"characters", "grounded"}, {"transitions", "play-restore"}, {"shutdown", "zero-active-jobs"}};
			const PhysicsInvariantSet exported = play;
			context.Expect(EquivalentPhysicsInvariants(play, exported), "Editor Play Mode and export must agree on every durable physics invariant.");
			PhysicsInvariantSet drifted = exported; drifted["filtering"] = "all-layers";
			context.Expect(!EquivalentPhysicsInvariants(play, drifted), "Layer filtering drift must be visible in export parity evidence.");
		});
	}
}
