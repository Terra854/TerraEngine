// VirtualGeometryPhysicalPageTests.cpp - exact static-page range IO contracts.

#include "Resource/ResourceStreaming.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "TestHarness.h"
#include "TEPAK/PackWriter.h"

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

		registry.Add("VirtualGeometry.Streaming.StaticPageReadUsesMountedTepakRange",
			[](TestContext &context)
			{
				const std::filesystem::path root = std::filesystem::temp_directory_path()
					/ "TerraEngineEngineTests" / "MountedVirtualGeometryPage";
				const std::filesystem::path contentRoot = root / "Content";
				const std::filesystem::path packPath = root / "Output" / "game.tepak";
				std::error_code ignored;
				ContentMount::Instance().Unmount();
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(contentRoot / "Meshes");
				std::filesystem::create_directories(packPath.parent_path());
				const std::vector<std::uint8_t> fileBytes{ 1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u };
				{
					std::ofstream stream(contentRoot / "Meshes" / "Page.temf", std::ios::binary);
					stream.write(reinterpret_cast<const char*>(fileBytes.data()),
						static_cast<std::streamsize>(fileBytes.size()));
				}

				tepak::PackagingRequest packRequest{};
				packRequest.inputRoot = contentRoot;
				packRequest.outputPackPath = packPath;
				packRequest.extentSizingBytes = 4096u;
				packRequest.workerCount = 1u;
				packRequest.enableCompression = false;
				packRequest.enableDictionaryTraining = false;
				packRequest.overwriteExisting = true;
				(void)tepak::PackWriter::Create(packRequest);
				context.Expect(ContentMount::Instance().Mount(packPath),
					"The page-read fixture archive must mount before worker IO.");

				const std::vector<std::uint8_t> expected(fileBytes.begin() + 2, fileBytes.begin() + 6);
				Resource::VirtualGeometryStaticPageReadRequest request{};
				// Packaged reads only need the logical path; no developer filesystem
				// location should be required for a mounted TEPAK asset.
				request.path.clear();
				request.pageId = 7u;
				request.byteOffset = 2u;
				request.byteSize = expected.size();
				request.expectedHash = Resource::HashVirtualGeometryPageBytes(expected);
				request.requestGeneration = 4u;
				request.mountedLogicalPath = "Meshes/Page.temf";
				request.mountedContentRequired = true;
				const auto mountedRead = Resource::ReadVirtualGeometryStaticPageRange(request, 4u);
				context.Expect(mountedRead.status == Resource::VirtualGeometryStaticPageReadStatus::Completed,
					"Packaged page IO must use ContentMount when no loose TEMF exists.");
				context.Expect(mountedRead.bytes == expected,
					"Mounted page IO must preserve the exact logical TEMF byte range.");

				request.mountedLogicalPath = "Meshes/Missing.temf";
				request.path = contentRoot / "Meshes" / "Page.temf";
				const auto rejectedFallback = Resource::ReadVirtualGeometryStaticPageRange(request, 4u);
				context.Expect(rejectedFallback.status == Resource::VirtualGeometryStaticPageReadStatus::IoFailure,
					"An export-authoritative request must not fall back to a developer loose file.");

				ContentMount::Instance().Unmount();
				std::filesystem::remove_all(root, ignored);
			});
	}
} // namespace TerraEngine::Tests
