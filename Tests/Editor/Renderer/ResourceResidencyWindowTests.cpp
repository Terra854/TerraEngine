#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Windows/ResourceResidencyWindowModel.h"
#include "Resource/ResourceResidency.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		TerraEngine::Resource::StreamingDiagnosticRecord MakeResidencyWindowRecord(std::uint64_t cpuBytes, std::uint64_t gpuBytes)
		{
			TerraEngine::Resource::StreamingDiagnosticRecord record;
			record.payloadState = TerraEngine::Resource::StreamablePayloadState::VramResident;
			record.exactCpuBytes = cpuBytes > 0u
				? TerraEngine::Resource::MakeExactResidencyBytes(cpuBytes)
				: TerraEngine::Resource::MakeZeroByStateResidencyBytes();
			record.exactGpuBytes = gpuBytes > 0u
				? TerraEngine::Resource::MakeExactResidencyBytes(gpuBytes)
				: TerraEngine::Resource::MakeZeroByStateResidencyBytes();
			return record;
		}

		std::string ReadResourceResidencyWindowSource()
		{
			// The Virtual Geometry section is immediate-mode UI. Reading the
			// source lets this regression test protect the visible column labels
			// without requiring an editor window to be opened by the unit test.
			const auto read = [](const std::string& candidate) {
				std::ifstream file(candidate, std::ios::binary);
				if (!file)
				{
					return std::string();
				}
				std::ostringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			};
			const std::string path = "Editor/TerraEngine-Editor/Windows/ResourceResidencyWindow.cpp";
			std::string text = read(path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../") + path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../../") + path);
			if (!text.empty())
			{
				return text;
			}
			const std::string thisFile = __FILE__;
			const auto marker = thisFile.find("Tests\\Editor\\Renderer\\ResourceResidencyWindowTests.cpp");
			if (marker != std::string::npos)
			{
				return read(thisFile.substr(0u, marker) + path);
			}
			const auto slashMarker = thisFile.find("Tests/Editor/Renderer/ResourceResidencyWindowTests.cpp");
			if (slashMarker != std::string::npos)
			{
				return read(thisFile.substr(0u, slashMarker) + path);
			}
			return {};
		}

		std::size_t CountOccurrences(const std::string& text, const std::string& needle)
		{
			std::size_t count = 0u;
			std::size_t offset = 0u;
			while (!needle.empty())
			{
				offset = text.find(needle, offset);
				if (offset == std::string::npos)
				{
					break;
				}
				++count;
				offset += needle.size();
			}
			return count;
		}
	}

	void RegisterResourceResidencyWindowTests(TestRegistry& registry)
	{
		registry.Add("ResourceResidencyWindow.ExactBytes.DisplayNumericZeroAndUnavailable", [](TestContext& context) {
			const TerraEngine::Editor::ResourceResidencyByteDisplay exact =
				TerraEngine::Editor::MakeResourceResidencyByteDisplay(TerraEngine::Resource::MakeExactResidencyBytes(512u), "Render GPU Bytes");
			const TerraEngine::Editor::ResourceResidencyByteDisplay zero =
				TerraEngine::Editor::MakeResourceResidencyByteDisplay(TerraEngine::Resource::MakeZeroByStateResidencyBytes(), "Render GPU Bytes");
			const TerraEngine::Editor::ResourceResidencyByteDisplay unavailable =
				TerraEngine::Editor::MakeResourceResidencyByteDisplay(TerraEngine::Resource::MakeUnavailableResidencyBytes("backend snapshot missing"), "Render GPU Bytes");

			context.ExpectEqual(std::string("512"), exact.label, "Exact byte values should display as numeric text.");
			context.Expect(exact.numeric, "Exact byte values should be sortable as numbers.");
			context.ExpectEqual(std::string("0"), zero.label, "Zero-by-state should display as a numeric zero.");
			context.Expect(zero.numeric, "Zero-by-state should sort with numeric values.");
			context.ExpectEqual(std::string("unavailable"), unavailable.label, "Unavailable values should not look like real zero bytes.");
			context.Expect(!unavailable.numeric, "Unavailable values should not be treated as measured bytes.");
			context.Expect(unavailable.tooltip.find("backend snapshot missing") != std::string::npos, "Unavailable labels should preserve the producer reason for tooltips.");
		});

		registry.Add("ResourceResidencyWindow.ExactBytes.SortUsesMeasuredBytes", [](TestContext& context) {
			std::vector<TerraEngine::Resource::ExactResidencyByteValue> values = {
				TerraEngine::Resource::MakeExactResidencyBytes(128u),
				TerraEngine::Resource::MakeZeroByStateResidencyBytes(),
				TerraEngine::Resource::MakeExactResidencyBytes(64u)
			};
			std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
				return TerraEngine::Editor::MakeResourceResidencyByteDisplay(left, "bytes").sortKey
					< TerraEngine::Editor::MakeResourceResidencyByteDisplay(right, "bytes").sortKey;
			});

			context.ExpectEqual(0ull, values[0].bytes, "Exact zero should sort before positive byte counts.");
			context.ExpectEqual(64ull, values[1].bytes, "Measured byte values should sort by byte count.");
			context.ExpectEqual(128ull, values[2].bytes, "Measured byte values should keep ascending order.");
		});

		registry.Add("ResourceResidencyWindow.PackageParents.HideUnlessTheyOwnExactBytes", [](TestContext& context) {
			TerraEngine::Resource::StreamingDiagnosticRecord emptyParent = MakeResidencyWindowRecord(0u, 0u);
			TerraEngine::Resource::StreamingDiagnosticRecord gpuParent = MakeResidencyWindowRecord(0u, 2048u);
			TerraEngine::Resource::StreamingDiagnosticRecord child = MakeResidencyWindowRecord(0u, 0u);

			context.Expect(!TerraEngine::Editor::ShouldDisplayStreamingResidencyRecord(emptyParent, true), "Generated package parents without exact owned bytes should be hidden from Resource Residency.");
			context.Expect(TerraEngine::Editor::ShouldDisplayStreamingResidencyRecord(gpuParent, true), "Generated package parents with their own exact allocation should remain visible.");
			context.Expect(TerraEngine::Editor::ShouldDisplayStreamingResidencyRecord(child, false), "Generated child rows should remain visible even when currently unloaded.");
		});

		registry.Add("ResourceResidencyWindow.MemoryLocation.UsesExactByteStatus", [](TestContext& context) {
			context.ExpectEqual(std::string("RAM + VRAM"), TerraEngine::Editor::ExactStreamingMemoryLocation(MakeResidencyWindowRecord(32u, 64u)), "Rows with exact CPU and GPU bytes should show both domains.");
			context.ExpectEqual(std::string("RAM"), TerraEngine::Editor::ExactStreamingMemoryLocation(MakeResidencyWindowRecord(32u, 0u)), "Rows with only CPU bytes should show RAM.");
			context.ExpectEqual(std::string("VRAM"), TerraEngine::Editor::ExactStreamingMemoryLocation(MakeResidencyWindowRecord(0u, 64u)), "Rows with only GPU bytes should show VRAM.");
			context.ExpectEqual(std::string("not loaded"), TerraEngine::Editor::ExactStreamingMemoryLocation(MakeResidencyWindowRecord(0u, 0u)), "Rows with exact zero in both domains should show not loaded.");
		});

		registry.Add("ResourceResidencyWindow.VirtualGeometrySection.ExposesExactPageBuckets", [](TestContext& context) {
			const std::string source = ReadResourceResidencyWindowSource();

			context.Expect(source.find("ImGui::SeparatorText(\"Virtual Geometry\")") != std::string::npos, "VG page diagnostics should live under the Virtual Geometry section.");
			context.Expect(source.find("Page Bytes H/F/E") != std::string::npos, "The view summary should expose high-detail, fallback, and embedded byte buckets.");
			context.Expect(source.find("Pending/Fail/Evict/Retire Bytes") != std::string::npos, "The view summary should expose pending, failed, evicted, and retiring byte buckets.");
			context.Expect(source.find("Owner Fallback Bytes") != std::string::npos, "The view summary should expose owning-mesh fallback bytes.");
			context.Expect(source.find("High Detail Page Bytes") != std::string::npos, "The per-asset VG table should expose high-detail page bytes.");
			context.Expect(source.find("Fallback Page Bytes") != std::string::npos, "The per-asset VG table should expose fallback page bytes.");
			context.Expect(source.find("Embedded/Owner Fallback Bytes") != std::string::npos, "The per-asset VG table should expose embedded and owning fallback byte buckets.");
			context.Expect(source.find("Retiring Page Bytes") != std::string::npos, "The per-asset VG table should expose deferred safe-retirement bytes.");
		});

		registry.Add("ResourceResidencyWindow.Diagnostics.CachesAndClipsHotRows", [](TestContext& context) {
			const std::string source = ReadResourceResidencyWindowSource();

			context.Expect(source.find("kDiagnosticsRefreshIntervalSeconds") != std::string::npos, "Resource Residency should throttle diagnostic refreshes while the window is visible.");
			context.Expect(source.find("RefreshDiagnosticsCacheIfNeeded") != std::string::npos, "The window should centralize expensive diagnostic collection in one cache refresh path.");
			context.ExpectEqual(1ull, static_cast<unsigned long long>(CountOccurrences(source, "GetResidencyDiagnosticsSummary()")), "Residency diagnostics should be fetched once per cache refresh, not once per table.");
			context.ExpectEqual(1ull, static_cast<unsigned long long>(CountOccurrences(source, "GetStreamingDiagnosticsSummary()")), "Streaming diagnostics should be fetched once per cache refresh, not once per table.");
			context.ExpectEqual(1ull, static_cast<unsigned long long>(CountOccurrences(source, "LatestBackendResourceResidencySnapshot()")), "Backend GPU byte snapshots should be fetched once per cache refresh.");
			context.ExpectEqual(1ull, static_cast<unsigned long long>(CountOccurrences(source, "LatestVirtualGeometryDiagnosticsSnapshots()")), "Virtual geometry diagnostics should be fetched once per cache refresh.");
			context.Expect(source.find("std::vector<const StreamingDisplayRecord*>") != std::string::npos, "Streaming rows should sort/filter cached display rows instead of copying and re-resolving asset labels.");
			context.Expect(CountOccurrences(source, "ImGuiListClipper clipper") >= 3u, "Large Resource Residency tables should clip visible rows before drawing.");
		});
	}
}
