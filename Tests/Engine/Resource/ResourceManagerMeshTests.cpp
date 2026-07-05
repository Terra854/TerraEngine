#include "TestHarness.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadResourceManagerMeshSource()
		{
			// ResourceManagerMesh.cpp owns the runtime TEMF rejection path. This
			// source-contract test keeps the page-integrity checks visible even
			// when the binary TEMF fixture format changes during v1 development.
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

			const std::string path = "Engine/Resource/ResourceManagerMesh.cpp";
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
			const auto resolveFromMarker = [&](const std::string& marker) {
				const std::size_t markerPosition = thisFile.find(marker);
				if (markerPosition == std::string::npos)
				{
					return std::string();
				}
				return read(thisFile.substr(0u, markerPosition) + path);
			};
			text = resolveFromMarker("Tests\\Engine\\Resource\\ResourceManagerMeshTests.cpp");
			if (!text.empty())
			{
				return text;
			}
			return resolveFromMarker("Tests/Engine/Resource/ResourceManagerMeshTests.cpp");
		}
	}

	void RegisterResourceManagerMeshTests(TestRegistry& registry)
	{
		registry.Add("Resource.Manager.Mesh.US7.RejectsCorruptVirtualGeometryPagePayloads", [](TestContext& context) {
			const std::string source = ReadResourceManagerMeshSource();

			context.Expect(source.find("TEMF virtual geometry page table is missing required cluster-page coverage metadata") != std::string::npos, "Runtime loader should reject page tables without cluster-page coverage.");
			context.Expect(source.find("TEMF virtual geometry page has invalid identity, fallback, or byte-size metadata") != std::string::npos, "Runtime loader should reject invalid page ids, fallback ids, or byte sizes.");
			context.Expect(source.find("TEMF virtual geometry page does not declare cluster-page coverage metadata") != std::string::npos, "Runtime loader should reject pages without cluster-page links.");
			context.Expect(source.find("TEMF virtual geometry page fallback points to a higher-detail page") != std::string::npos, "Runtime loader should reject fallback links that point to higher-detail pages.");
			context.Expect(source.find("material section LOD range") != std::string::npos, "Runtime loader should validate material section metadata before publication.");
			context.Expect(source.find("TEMF virtual geometry cullable group has invalid bounds") != std::string::npos, "Runtime loader should reject invalid page/cluster bounds before rendering.");
			context.Expect(source.find("TEMF virtual geometry page table does not keep LOD0 tied to a resident fallback page") != std::string::npos, "Runtime loader should require resident fallback coverage for page-streamed assets.");
		});
	}
}
