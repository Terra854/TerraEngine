#include "TestHarness.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::filesystem::path FixtureRoot(TestContext& context)
		{
			// Optional external data is opt-in. Once configured, missing or invalid
			// data is a failed prerequisite, never a successful skip or personal path.
			const char* configured = std::getenv("TERRAENGINE_DENSE_ANIMATED_VG_ROOT");
			if (configured == nullptr)
				context.Skip("Optional fixture not configured: set TERRAENGINE_DENSE_ANIMATED_VG_ROOT to its root.");
			context.Expect(*configured != '\0', "Configured TERRAENGINE_DENSE_ANIMATED_VG_ROOT must not be empty.");
			const std::filesystem::path root(configured);
			context.Expect(root.is_absolute() && std::filesystem::is_directory(root),
				"Configured TERRAENGINE_DENSE_ANIMATED_VG_ROOT must name an existing absolute fixture directory: " + root.string());
			return root;
		}

		std::string ReadText(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
		}

		bool Has(const std::string& text, const char* token)
		{
			return text.find(token) != std::string::npos;
		}

		std::size_t FindUsefulDetailLevelCount(const nlohmann::json& value)
		{
			std::size_t result = 0U;
			if (value.is_object())
			{
				for (const auto& [name, child] : value.items())
				{
					if ((name == "detailLevelCount" || name == "detailLevels"
							|| name == "lodCount" || name == "lodLevels")
						&& child.is_number_integer() && child.get<std::int64_t>() >= 0)
					{
						result = std::max(result, child.get<std::size_t>());
					}
					else if ((name == "detailLevels" || name == "lodLevels") && child.is_array())
					{
						result = std::max(result, child.size());
					}
					result = std::max(result, FindUsefulDetailLevelCount(child));
				}
			}
			else if (value.is_array())
			{
				for (const nlohmann::json& child : value)
				{
					result = std::max(result, FindUsefulDetailLevelCount(child));
				}
			}
			return result;
		}
	}

	void RegisterDenseAnimatedVirtualGeometryFixtureTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.DenseAnimatedFixture.ProvenanceAndSource", [](TestContext& context)
		{
			const auto root = FixtureRoot(context);
			context.Expect(std::filesystem::is_regular_file(root / "CesiumMan.glb")
				&& std::filesystem::is_regular_file(root / "source-provenance.json"),
				"Configured dense fixture must contain CesiumMan.glb and source-provenance.json.");
			const auto provenance = ReadText(root / "source-provenance.json");
			const auto glb = ReadText(root / "CesiumMan.glb");
			context.Expect(!provenance.empty(), "Dense animated fixture provenance must be present.");
			context.Expect(!glb.empty(), "Dense animated fixture GLB must be present.");
			context.Expect(Has(provenance, "CC-BY-4.0"), "Fixture license must be explicitly CC-BY-4.0.");
			context.Expect(Has(provenance, "B7001EAEEA8254BD44773BCD247E78696D94169388FBB2A1800FC69434E777D9"), "Fixture SHA-256 provenance must match the selected source.");
			context.Expect(Has(provenance, "sourceTriangles\": 4672"), "Fixture provenance must record more than 4,096 source triangles.");
			context.Expect(Has(provenance, "skinCount\": 1") && Has(provenance, "jointCount\": 19"), "Fixture provenance must record skin and joint facts.");
			context.Expect(Has(provenance, "unnamed_0"), "Fixture provenance must record the animation clip list.");
			context.Expect(glb.size() > 20u && glb.compare(0u, 4u, "glTF") == 0, "Fixture must be a binary glTF payload.");
		});

		registry.Add("VirtualGeometry.Editor.DenseAnimatedFixture.CookedOutput", [](TestContext& context)
		{
			const auto root = FixtureRoot(context);

			std::size_t usefulLevels = 0u;
			bool deformablePages = false;
			bool cookedOutput = false;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
			{
				if (!entry.is_regular_file())
					continue;
				const auto extension = entry.path().extension().string();
				if (extension != ".json" && extension != ".meta" && extension != ".temf")
					continue;
				if (extension == ".temf")
				{
					cookedOutput = true;
					continue;
				}
				const auto text = ReadText(entry.path());
				if (Has(text, "deformable") && (Has(text, "page") || Has(text, "virtualGeometry")))
					deformablePages = true;
				if (Has(text, "cooked\": true"))
					cookedOutput = true;
				try
				{
					usefulLevels = std::max(usefulLevels,
						FindUsefulDetailLevelCount(nlohmann::json::parse(text)));
				}
				catch (const nlohmann::json::parse_error&)
				{
					// Not every metadata sidecar is JSON. Its typed evidence cannot
					// satisfy the detail-level gate, but another valid sidecar may.
				}
			}
			// Fail closed until the installed editor has produced current-format
			// deformable pages and at least five useful detail levels.
			context.Expect(cookedOutput, "Dense animated fixture must have a current-format cooked output.");
			context.Expect(deformablePages, "Dense animated fixture must have deformable virtual-geometry pages.");
			context.Expect(usefulLevels >= 5u, "Dense animated fixture must expose at least five useful detail levels.");
		});
	}
}
