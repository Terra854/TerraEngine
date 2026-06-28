#include "../TestHarness.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		std::filesystem::path RepositoryRoot()
		{
			std::filesystem::path current = std::filesystem::current_path();
			while (!current.empty())
			{
				// CTest may launch from the binary tree, which can also contain copied
				// Engine/Tests folders. Require source-only contract files so static
				// migration checks always inspect the checkout rather than build output.
				if (std::filesystem::exists(current / "Engine/Core/Object/Object.h")
					&& std::filesystem::exists(current / "specs/023-compact-object-handles/contracts/migration-validation.md"))
				{
					return current;
				}
				current = current.parent_path();
			}
			return std::filesystem::current_path();
		}

		std::string ReadTextFile(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				return {};
			}
			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		bool ContainsAny(std::string_view text, const std::vector<std::string_view>& needles)
		{
			for (std::string_view needle : needles)
			{
				if (text.find(needle) != std::string_view::npos)
				{
					return true;
				}
			}
			return false;
		}
	}

	void RegisterObjectFacadeRemovalValidationTests(TestRegistry& registry)
	{
		registry.Add("Engine.ObjectFacadeRemovalValidation.DocumentsFinalRemovalContract", [](TestContext& context) {
			const std::filesystem::path root = RepositoryRoot();
			const std::string contract = ReadTextFile(root / "specs/023-compact-object-handles/contracts/migration-validation.md");
			context.Expect(!contract.empty(), "Migration validation contract should be available from the repository root.");
			context.Expect(contract.find("No old facade member functions exist.") != std::string::npos, "Contract should document final facade member removal.");
			context.Expect(contract.find("No old facade source file remains.") != std::string::npos, "Contract should document final facade source removal.");
		});

		registry.Add("Engine.ObjectFacadeRemovalValidation.FinalObjectHeaderRemovesFacadeMembers", [](TestContext& context) {
			const std::filesystem::path root = RepositoryRoot();
			const std::string objectHeader = ReadTextFile(root / "Engine/Core/Object/Object.h");
			context.Expect(!objectHeader.empty(), "Object.h should exist as the final compact identity header.");

			const bool legacyFacadeStillPresent = ContainsAny(objectHeader, {
				"addComponent",
				"getComponent",
				"removeComponent",
				"setName",
				"setActive"
			});
			context.Expect(!legacyFacadeStillPresent, "Final Object identity should not expose legacy facade member functions.");
			context.Expect(objectHeader.find("std::uint32_t entityValue") != std::string::npos, "Final Object should expose the compact entity value.");
			context.Expect(objectHeader.find("std::uint64_t stableIdHash") != std::string::npos, "Final Object should expose the compact stable identity hash.");
		});
	}
}
