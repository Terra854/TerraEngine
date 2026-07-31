// VirtualGeometryPhysicalPageTests.cpp - exact static-page range IO contracts.

#include "Resource/ResourceStreaming.h"
#include "TestHarness.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryPhysicalPageTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Streaming.StaticPageReadChecksExactRangeAndCancellation",
			[](TestContext &context)
			{
				const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineStaticPageRange.bin";
				const std::vector<std::uint8_t> bytes{ 2u, 4u, 6u, 8u };
				{
					std::ofstream stream(path, std::ios::binary);
					stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				}
				Resource::VirtualGeometryStaticPageReadRequest request;
				request.path = path;
				request.pageId = 3u;
				request.byteSize = bytes.size();
				request.expectedHash = Resource::HashVirtualGeometryPageBytes(bytes);
				request.requestGeneration = 9u;
				const auto read = Resource::ReadVirtualGeometryStaticPageRange(request, 9u);
				context.Expect(
					read.status == Resource::VirtualGeometryStaticPageReadStatus::Completed, "Exact page ranges must validate before decode work begins.");
				context.ExpectEqual(bytes.size(), read.bytes.size(), "Completed reads return owned page bytes.");
				auto cancelled = std::make_shared<std::atomic_bool>(true);
				request.cancellation = std::move(cancelled);
				const auto cancelledRead = Resource::ReadVirtualGeometryStaticPageRange(request, 9u);
				context.Expect(cancelledRead.status == Resource::VirtualGeometryStaticPageReadStatus::Cancelled,
					"Cancelled physical-page IO must not leak bytes into a later decode candidate.");
				std::filesystem::remove(path);
			});
	}
} // namespace TerraEngine::Tests
