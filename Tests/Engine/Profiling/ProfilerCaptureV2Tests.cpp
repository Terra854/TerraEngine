#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		// FindMetric keeps the compact-codec assertions focused on public profiler
		// semantics instead of exposing the codec's private JSON layout to callers.
		const Profiling::ProfilerMetric* FindMetric(
			const Profiling::ProfilerModuleSummary& module,
			const std::string_view name)
		{
			for (const Profiling::ProfilerMetric& metric : module.metrics)
			{
				if (metric.name == name)
				{
					return &metric;
				}
			}
			return nullptr;
		}

		// CountAnimationMetrics proves that the capture keeps the established public
		// three-metric contract without duplicating it in the compact V2 payload.
		std::size_t CountAnimationMetrics(const Profiling::ProfilerModuleSummary& module)
		{
			return static_cast<std::size_t>(std::count_if(module.metrics.begin(), module.metrics.end(), [](const Profiling::ProfilerMetric& metric) {
				return metric.name.starts_with("Animation.");
			}));
		}

		// Fnv1a32 mirrors the V2 chunk checksum algorithm so the malformed-fact
		// regression reaches compact-field validation rather than failing earlier.
		std::uint32_t Fnv1a32(const std::string& value)
		{
			std::uint32_t hash = 2166136261u;
			for (const unsigned char byte : value)
			{
				hash ^= byte;
				hash *= 16777619u;
			}
			return hash;
		}
	}

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

		registry.Add("Engine.ProfilerCaptureV2.DormantAnimationFactsRoundTripCompactly", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineEngineTests" / "ProfilerCaptureV2DormantAnimation";
			std::filesystem::create_directories(root);
			const std::filesystem::path path = root / "dormant-animation.teprof";
			ProfilerCaptureDocument document;
			document.metadata.settings.detailLevel = ProfilerCaptureDetailLevel::Detailed;
			document.metadata.settings.maximumChunkFrames = 1u;

			ProfilerFrameRecord frame;
			frame.frameIndex = 17u;
			frame.complete = true;
			frame.animationDiagnostics = ProfilerAnimationDiagnosticsFacts{1u, true, true};
			ProfilerModuleSummary cpu;
			cpu.moduleId = ProfilerModuleId::CpuEngine;
			cpu.available = true;
			cpu.metrics = {
				{"Animation.SchedulerMembershipChecks", 1.0, "count"},
				{"Animation.Dormant", 1.0, "boolean"},
				{"Animation.BaselineIdentityMatched", 1.0, "boolean"}};
			frame.moduleSummaries.push_back(std::move(cpu));
			document.frames.push_back(std::move(frame));

			std::string error;
			context.Expect(SaveCaptureDocument(document, path, error), "Dormant compact-codec setup must save a capture: " + error);
			std::ifstream input(path, std::ios::binary);
			std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			context.Expect(text.find("\"x\":[1,true,true]") != std::string::npos,
				"V2 captures must encode dormant animation facts in one compact typed field.");
			context.Expect(text.find("Animation.SchedulerMembershipChecks") == std::string::npos
				&& text.find("Animation.Dormant") == std::string::npos
				&& text.find("Animation.BaselineIdentityMatched") == std::string::npos,
				"The compact V2 payload must not duplicate dormant facts as string-based module metrics.");

			ProfilerCaptureDocument loaded;
			context.Expect(LoadCaptureDocument(path, loaded, error), "Compact dormant facts must load successfully: " + error);
			context.ExpectEqual(1ULL, static_cast<unsigned long long>(loaded.frames.size()), "The compact dormant capture must preserve its frame.");
			if (!loaded.frames.empty())
			{
				const ProfilerFrameRecord& loadedFrame = loaded.frames.front();
				context.Expect(loadedFrame.animationDiagnostics.has_value()
					&& loadedFrame.animationDiagnostics->schedulerMembershipChecks == 1u
					&& loadedFrame.animationDiagnostics->dormant
					&& loadedFrame.animationDiagnostics->baselineIdentityMatched,
					"The compact V2 field must restore all typed dormant animation facts.");
				context.ExpectEqual(1ULL, static_cast<unsigned long long>(loadedFrame.moduleSummaries.size()), "The CPU module must survive the compact V2 round trip.");
				if (!loadedFrame.moduleSummaries.empty())
				{
					const ProfilerModuleSummary& loadedCpu = loadedFrame.moduleSummaries.front();
					const ProfilerMetric* membership = FindMetric(loadedCpu, "Animation.SchedulerMembershipChecks");
					const ProfilerMetric* dormant = FindMetric(loadedCpu, "Animation.Dormant");
					const ProfilerMetric* baseline = FindMetric(loadedCpu, "Animation.BaselineIdentityMatched");
					context.Expect(CountAnimationMetrics(loadedCpu) == 3u
						&& membership != nullptr && membership->value == 1.0 && membership->unit == "count"
						&& dormant != nullptr && dormant->value == 1.0 && dormant->unit == "boolean"
						&& baseline != nullptr && baseline->value == 1.0 && baseline->unit == "boolean",
						"Loading compact dormant facts must restore exactly the public three-metric Animation contract.");
				}
			}

			std::vector<std::string> lines;
			std::istringstream lineReader(text);
			for (std::string line; std::getline(lineReader, line);)
			{
				if (!line.empty())
				{
					lines.push_back(std::move(line));
				}
			}
			context.Expect(lines.size() >= 3u, "The compact V2 fixture must contain header, chunk, and final-index lines.");
			if (lines.size() >= 3u)
			{
				nlohmann::json chunk = nlohmann::json::parse(lines[1]);
				chunk["frames"][0]["x"] = nlohmann::json::array({1u, true});
				chunk["checksum"] = Fnv1a32(chunk["frames"].dump());
				lines[1] = chunk.dump();
				std::ofstream malformed(path, std::ios::binary | std::ios::trunc);
				for (const std::string& line : lines)
				{
					malformed << line << '\n';
				}
				malformed.close();
				ProfilerCaptureDocument rejected;
				context.Expect(!LoadCaptureDocument(path, rejected, error),
					"Malformed compact animation facts must reject even when their containing chunk checksum is valid.");
			}

			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		});
	}
}
