#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"

#include <filesystem>
#include <fstream>

namespace TerraEngine::Tests
{
	void RegisterProfilerCaptureV2Tests(TestRegistry& registry)
	{
		registry.Add("Engine.ProfilerCaptureV2.RecoversCommittedTailAndRejectsCorruption", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineEngineTests" / "ProfilerCaptureV2";
			std::filesystem::create_directories(root);
			const std::filesystem::path path = root / "recoverable.teprof";
			ProfilerCaptureDocument document;
			document.metadata.settings.maximumChunkFrames = 1u;
			for (std::uint64_t index = 0u; index < 2u; ++index)
			{
				ProfilerFrameRecord frame;
				frame.frameIndex = index;
				frame.complete = true;
				frame.physics = ProfilerPhysicsFrameCounters{index, 8u, 0u, 0u, 0u, 0u, 0u, 0u};
				document.frames.push_back(std::move(frame));
			}
			std::string error;
			context.Expect(SaveCaptureDocument(document, path, error), "V2 test setup must save a capture: " + error);
			std::ifstream input(path, std::ios::binary);
			std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			const std::size_t finalLine = text.find_last_of('\n', text.size() > 1u ? text.size() - 2u : 0u);
			context.Expect(finalLine != std::string::npos, "A v2 capture must include a final-index line.");
			if (finalLine != std::string::npos)
			{
				text.resize(finalLine + 1u);
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output << text;
			}
			ProfilerCaptureDocument recovered;
			context.Expect(LoadCaptureDocument(path, recovered, error), "Committed v2 chunks should survive a missing final index: " + error);
			context.ExpectEqual(2ULL, static_cast<unsigned long long>(recovered.frames.size()), "Recovery should retain every checksummed committed frame.");
			context.Expect(recovered.recoveredIncompleteTail && !recovered.complete, "Recovery state must remain explicit to consumers.");

			std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
			corrupt << "{\"format\":\"TerraEngineProfilerCapture\",\"payloadVersion\":2,\"metadata\":{},\"settings\":{},\"stringTable\":[]}";
			corrupt.close();
			ProfilerCaptureDocument rejected;
			context.Expect(!LoadCaptureDocument(path, rejected, error), "Malformed v2 string tables must reject rather than invent labels.");
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		});
	}
}
