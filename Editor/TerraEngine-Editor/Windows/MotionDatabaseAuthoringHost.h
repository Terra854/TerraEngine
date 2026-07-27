// MotionDatabaseAuthoringHost.h - production task and clip-resolution adapter.
#pragma once

#include "Editor/AssetCompilers/MotionDatabaseCompiler.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace TerraEngine::Editor::Animation {
	class MotionDatabaseAuthoringHost {
	public:
		virtual ~MotionDatabaseAuthoringHost() = default;
		virtual std::shared_ptr<AssetPipeline::MotionDatabaseCompileExecutor>
			GetMotionDatabaseCompileExecutor() = 0;
		// Identity resolution reads only the fixed animation envelope and is safe for
		// an editor button. Full clip decode/evaluation is returned as worker work.
		virtual TerraEngine::Animation::AnimationStatus ResolveMotionDatabaseSourceIdentity(
			AssetPipeline::MotionDatabaseAuthoringSource& source,
			TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibility) = 0;
		// The editor uses this lightweight envelope read when a new database needs to
		// adopt the rig identity of its first real source clip.
		virtual TerraEngine::Animation::AnimationStatus ReadMotionDatabaseSourceCompatibility(
			const AssetPipeline::MotionDatabaseAuthoringSource& source,
			TerraEngine::Animation::AnimationCompatibilityFingerprint& compatibility) = 0;
		virtual AssetPipeline::MotionDatabaseWorkerSourceResolver CreateMotionDatabaseSourceResolver(
			const AssetPipeline::MotionDatabaseAuthoringDocument& document) = 0;
		virtual void NotifyMotionDatabasePublished(
			const std::filesystem::path& outputPath,
			TerraEngine::Animation::AnimationGuid assetId) = 0;
	};

	// This resolver is independently testable with temporary project roots. It
	// accepts only .teanim files inside the configured Assets or Build trees.
	class MotionDatabaseSourceResolver final {
	public:
		MotionDatabaseSourceResolver(std::filesystem::path assetRoot,
			std::filesystem::path buildRoot);
		[[nodiscard]] TerraEngine::Animation::AnimationStatus ResolveIdentity(
			AssetPipeline::MotionDatabaseAuthoringSource& source,
			TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibility) const;
		[[nodiscard]] TerraEngine::Animation::AnimationStatus ReadCompatibility(
			const AssetPipeline::MotionDatabaseAuthoringSource& source,
			TerraEngine::Animation::AnimationCompatibilityFingerprint& compatibility) const;
		[[nodiscard]] TerraEngine::Animation::AnimationStatus Resolve(
			const AssetPipeline::MotionDatabaseAuthoringDocument& document,
			std::vector<AssetPipeline::MotionDatabaseSourceClipInput>& output,
			const std::atomic_bool& cancellationRequested) const;

	private:
		[[nodiscard]] TerraEngine::Animation::AnimationStatus ResolvePath(
			const std::filesystem::path& authoredPath,
			std::filesystem::path& output) const;

		std::filesystem::path assetRoot_;
		std::filesystem::path buildRoot_;
	};

	// The default window host uses the process-wide task pool and AssetDatabase.
	// Tests may inject roots and an executor without initializing editor globals.
	class MotionDatabaseProductionAuthoringHost final : public MotionDatabaseAuthoringHost {
	public:
		MotionDatabaseProductionAuthoringHost();
		MotionDatabaseProductionAuthoringHost(std::filesystem::path assetRoot,
			std::filesystem::path buildRoot,
			std::shared_ptr<AssetPipeline::MotionDatabaseCompileExecutor> executor = {});
		~MotionDatabaseProductionAuthoringHost() override;

		std::shared_ptr<AssetPipeline::MotionDatabaseCompileExecutor>
			GetMotionDatabaseCompileExecutor() override;
		TerraEngine::Animation::AnimationStatus ResolveMotionDatabaseSourceIdentity(
			AssetPipeline::MotionDatabaseAuthoringSource& source,
			TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibility) override;
		TerraEngine::Animation::AnimationStatus ReadMotionDatabaseSourceCompatibility(
			const AssetPipeline::MotionDatabaseAuthoringSource& source,
			TerraEngine::Animation::AnimationCompatibilityFingerprint& compatibility) override;
		AssetPipeline::MotionDatabaseWorkerSourceResolver CreateMotionDatabaseSourceResolver(
			const AssetPipeline::MotionDatabaseAuthoringDocument& document) override;
		void NotifyMotionDatabasePublished(const std::filesystem::path& outputPath,
			TerraEngine::Animation::AnimationGuid assetId) override;

	private:
		struct State;
		std::unique_ptr<State> state_;
	};
}
