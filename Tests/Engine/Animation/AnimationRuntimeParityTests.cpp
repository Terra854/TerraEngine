// AnimationRuntimeParityTests.cpp - archive-only animation content regressions.

#include "TestHarness.h"

#include "AnimationContentResolver.h"
#include "ContentMount.h"
#include "TEPAK/PackWriter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		struct RuntimeArchiveWorkspace final
		{
			RuntimeArchiveWorkspace()
			{
				const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
				root = std::filesystem::temp_directory_path()
					/ ("terra-animation-runtime-" + std::to_string(nonce));
				firstContent = root / "first";
				secondContent = root / "second";
				firstArchive = root / "first.tepak";
				secondArchive = root / "second.tepak";
				std::filesystem::create_directories(firstContent);
				std::filesystem::create_directories(secondContent);
			}

			~RuntimeArchiveWorkspace()
			{
				std::error_code error;
				std::filesystem::remove_all(root, error);
			}

			std::filesystem::path root;
			std::filesystem::path firstContent;
			std::filesystem::path secondContent;
			std::filesystem::path firstArchive;
			std::filesystem::path secondArchive;
		};

		bool WriteBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
		{
			std::filesystem::create_directories(path.parent_path());
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return output.good();
		}

		void Pack(const std::filesystem::path& content, const std::filesystem::path& archive)
		{
			tepak::PackagingRequest request;
			request.inputRoot = content;
			request.outputPackPath = archive;
			request.workerCount = 1U;
			request.enableDictionaryTraining = false;
			request.enableCompression = false;
			request.overwriteExisting = true;
			(void)tepak::PackWriter::Create(request);
		}
	}

	void RegisterAnimationRuntimeParityTests(TestRegistry& registry)
	{
		registry.Add("Animation.RuntimeParity.ArchiveDiscoveryUsesCookedMetadataOnly", [](TestContext& context) {
			ContentMount unmounted;
			context.Expect(AnimationContentResolver::Discover(unmounted).empty(),
				"An unmounted runtime cannot discover loose animation files.");
			const ResolvedAnimationContent unmountedResult =
				AnimationContentResolver::Resolve(unmounted, "animations/hero.teanim");
			context.Expect(unmountedResult.status == AnimationContentStatus::NotMounted && !unmountedResult.present,
				"Archive-only resolution must report an unmounted source without filesystem fallback.");

			RuntimeArchiveWorkspace workspace;
			const std::array<std::pair<const char*, const char*>, 7U> assets{{
				{"characters/hero.teskel", "skeleton"},
				{"characters/walk.teanim", "animation-clip"},
				{"characters/hero.teanimctrl", "animation-controller"},
				{"characters/upper.teanimmask", "animation-mask"},
				{"characters/humanoid.teretarget", "retarget-profile"},
				{"characters/locomotion.temotiondb", "motion-database"},
				{"characters/hero.teragdoll", "ragdoll-profile"}
			}};
			for(std::size_t index = 0U; index < assets.size(); ++index)
			{
				const std::array<std::uint8_t, 4U> bytes{{
					static_cast<std::uint8_t>(index), 2U, 3U, 4U}};
				context.Expect(WriteBytes(workspace.firstContent / assets[index].first, bytes),
					"Each cooked class fixture must be written.");
			}
			const std::array<std::uint8_t, 4U> futureClassBytes{{9U, 8U, 7U, 6U}};
			context.Expect(WriteBytes(
				workspace.firstContent / "characters/future.futureanim", futureClassBytes),
				"The unknown future-class fixture must be present in the archive only.");
			Pack(workspace.firstContent, workspace.firstArchive);

			ContentMount mount;
			context.Expect(mount.Mount(workspace.firstArchive),
				"A valid TEPAK fixture must mount before runtime discovery.");
			const std::vector<ResolvedAnimationContent> discovered = AnimationContentResolver::Discover(mount);
			context.Expect(discovered.size() == assets.size(),
				"Discovery must expose every append-only cooked animation class and no loose files.");
			for(const auto& [path, assetClass] : assets)
			{
				const ResolvedAnimationContent resolved = AnimationContentResolver::Resolve(mount, path);
				context.Expect(resolved.Succeeded() && resolved.assetClass == assetClass,
					"Mounted metadata must preserve the stable class for " + std::string(path) + ".");
			}
			const AnimationContentReadResult range = AnimationContentResolver::ReadRange(
				mount, "CHARACTERS/WALK.TEANIM", 1U, 2U, 2U);
			context.Expect(range.Succeeded() && range.bytes == std::vector<std::uint8_t>({2U, 3U}),
				"Archive reads must normalize paths and return only the requested bounded range.");
			context.Expect(AnimationContentResolver::ReadRange(
				mount, "characters/walk.teanim", 0U, 3U, 2U).content.status
					== AnimationContentStatus::ReadLimitExceeded,
				"Caller limits must reject oversized reads before allocation.");
			context.Expect(AnimationContentResolver::Resolve(mount, "source/hero.fbx").status
				== AnimationContentStatus::ForbiddenRuntimePath,
				"Runtime resolution must reject source paths even when a caller supplies one directly.");
			context.Expect(AnimationContentResolver::Resolve(mount, "characters/missing.teanim").status
				== AnimationContentStatus::NotFound,
				"Missing archive content must never fall back to project or build-tree files.");
			context.Expect(AnimationContentResolver::Resolve(
				mount, "characters/future.futureanim").status == AnimationContentStatus::NotAnimationAsset,
				"An unknown append-only class must remain generic and never masquerade as animation data.");
		});

		registry.Add("Animation.RuntimeParity.RemountIsAtomicAndFailedCandidatePreservesLastGood", [](TestContext& context) {
			RuntimeArchiveWorkspace workspace;
			const std::array<std::uint8_t, 4U> firstBytes{{1U, 1U, 1U, 1U}};
			const std::array<std::uint8_t, 4U> secondBytes{{2U, 2U, 2U, 2U}};
			context.Expect(
				WriteBytes(workspace.firstContent / "characters/walk.teanim", firstBytes)
					&& WriteBytes(workspace.secondContent / "characters/walk.teanim", secondBytes),
				"Both remount candidates must be staged completely.");
			Pack(workspace.firstContent, workspace.firstArchive);
			Pack(workspace.secondContent, workspace.secondArchive);

			ContentMount mount;
			context.Expect(mount.Mount(workspace.firstArchive), "The first archive must mount.");
			context.Expect(!mount.Mount(workspace.root / "corrupt-or-missing.tepak")
				&& mount.GetArchivePath() == workspace.firstArchive,
				"A failed remount must preserve the prior complete archive snapshot.");

			std::atomic_bool invalidRead = false;
			std::vector<std::thread> readers;
			for(std::size_t worker = 0U; worker < 4U; ++worker)
			{
				readers.emplace_back([&mount, &invalidRead, firstBytes, secondBytes]() {
					for(std::size_t iteration = 0U; iteration < 128U; ++iteration)
					{
						const AnimationContentReadResult read = AnimationContentResolver::ReadRange(
							mount, "characters/walk.teanim", 0U, 4U, 4U);
						if(!read.Succeeded()
							|| (read.bytes != std::vector<std::uint8_t>(firstBytes.begin(), firstBytes.end())
								&& read.bytes != std::vector<std::uint8_t>(secondBytes.begin(), secondBytes.end())))
						{
							invalidRead.store(true, std::memory_order_release);
							return;
						}
					}
				});
			}
			context.Expect(mount.Mount(workspace.secondArchive),
				"A fully validated replacement archive must publish as one remount operation.");
			for(std::thread& reader : readers)
			{
				reader.join();
			}
			context.Expect(!invalidRead.load(std::memory_order_acquire),
				"Concurrent readers may observe either complete generation but never mixed bytes or missing state.");
			const AnimationContentReadResult active = AnimationContentResolver::ReadRange(
				mount, "characters/walk.teanim", 0U, 4U, 4U);
			context.Expect(active.bytes == std::vector<std::uint8_t>(secondBytes.begin(), secondBytes.end()),
				"Reads after the protected remount boundary must observe the new generation.");
		});
	}
}
