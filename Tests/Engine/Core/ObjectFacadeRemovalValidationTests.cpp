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
				// CTest may launch from the binary tree. Object.h is the maintained
				// source-level contract; historical feature-spec folders are not required
				// in a clean checkout after their implementation has been integrated.
				if (std::filesystem::exists(current / "Engine/Core/Object/Object.h"))
				{
					return current;
				}
				// Windows drive roots are their own parent (for example, C:\\ maps
				// back to C:\\). Stop there so a missing contract reports a normal
				// assertion failure instead of making the unfiltered CTest process spin.
				const std::filesystem::path parent = current.parent_path();
				if (parent == current)
				{
					break;
				}
				current = parent;
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
			const std::string contract = ReadTextFile(root / "Engine/Core/Object/Object.h");
			context.Expect(!contract.empty(), "The maintained Object identity contract should be available from the repository root.");
			context.Expect(contract.find("small value, not a component facade") != std::string::npos,
				"Object.h should document that the removed component facade is not part of the compact identity.");
			context.Expect(contract.find("validated through ObjectManager before mutable access") != std::string::npos,
				"Object.h should document the supported mutable-access boundary that replaced facade members.");
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
