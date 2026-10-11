#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanTextureDescriptorAdmission.h"

namespace TerraEngine::Tests
{
	void RegisterVulkanTextureDescriptorAdmissionTests(TestRegistry& registry)
	{
		using namespace Renderer;
		registry.Add("Renderer.Efficiency.TextureDescriptors.BootstrapWritesBeforeReady", [](TestContext& context) {
			TextureDescriptorBootstrap state;
			int waits = 0, writes = 0;
			{
				ScopedTextureDescriptorBootstrap grant(state, 42, 7, true);
				TextureDescriptorAdmissionProof proof{ false, true, false, true, false,
					state.Matches(42, 7) };
				context.Expect(!state.Matches(43, 7) && !state.Matches(42, 8), "Another device or session cannot use this capability.");
				bool wrongThreadMatched = true;
				std::thread other([&] { wrongThreadMatched = state.Matches(42, 7); });
				other.join();
				context.Expect(!wrongThreadMatched, "A worker cannot borrow the initialization grant.");
				context.Expect(!state.RequiredWritesComplete(), "Readiness cannot precede actual reserved writes.");
				for (int slot = 0; slot < 3; ++slot)
					context.Expect(ApplyTextureDescriptorAdmission(DecideTextureDescriptorAdmission(proof),
						[&] { ++waits; return true; }, [&] { ++writes; state.RecordWrite(slot, true); }),
						"Fallback descriptors must be written before initialized becomes true.");
				context.ExpectEqual(7, int(state.reservedWrites), "All three reserved slots are required.");
				context.Expect(state.RequiredWritesComplete(), "Successful native write callbacks permit readiness.");
				state.RecordWrite(5, false);
				context.Expect(!state.RequiredWritesComplete(), "An environment descriptor failure must prevent readiness too.");
			}
			context.ExpectEqual(3, writes, "Each reserved slot must be written exactly once.");
			context.ExpectEqual(0, waits, "Bootstrap has no in-flight work and must not add an idle wait.");
			context.Expect(!state.active && state.reservedWrites == 0, "RAII must revoke the grant.");
			try {
				ScopedTextureDescriptorBootstrap grant(state, 42, 7, true);
				throw std::runtime_error("Simulated bootstrap allocation failure.");
			} catch (const std::runtime_error&) {}
			context.Expect(!state.active, "Exception unwinding must revoke bootstrap permission too.");
		});
		registry.Add("Renderer.Efficiency.TextureDescriptors.RejectsUnsafeBootstrapAndPreservesRuntimeWait", [](TestContext& context) {
			TextureDescriptorAdmissionProof proof{ false, true, false, true, false, true };
			int writes = 0, waits = 0;
			for (int fault = 0; fault < 5; ++fault) {
				auto bad = proof;
				if (fault == 0) bad.deviceReady = false;
				if (fault == 1) bad.deviceLost = true;
				if (fault == 2) bad.setsReady = false;
				if (fault == 3) bad.pendingWork = true;
				if (fault == 4) bad.bootstrapOwnerCurrent = false;
				context.Expect(!ApplyTextureDescriptorAdmission(DecideTextureDescriptorAdmission(bad),
					[&] { ++waits; return true; }, [&] { ++writes; }), "Unsafe bootstrap must reject without a write.");
			}
			proof.initialized = true;
			context.Expect(!ApplyTextureDescriptorAdmission(DecideTextureDescriptorAdmission(proof),
				[&] { ++waits; return false; }, [&] { ++writes; }), "A failed runtime wait must prevent the write.");
			context.ExpectEqual(1, waits, "Only runtime admission calls the existing wait.");
			context.ExpectEqual(0, writes, "Rejected paths must not publish descriptors.");
			context.Expect(ApplyTextureDescriptorAdmission(DecideTextureDescriptorAdmission(proof),
				[&] { ++waits; return true; }, [&] { ++writes; }), "Successful runtime idle proof must still permit its write.");
			context.ExpectEqual(2, waits, "Runtime success must perform the existing wait before writing.");
			context.ExpectEqual(1, writes, "Runtime success publishes one descriptor write.");
		});
	}
}
