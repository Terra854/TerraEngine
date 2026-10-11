#pragma once

#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class ResourceManager;

namespace TerraEngine::Editor::Automation
{
	// Classification uses actual owned receipt identity, never discarded error
	// strings. Only a current texture cook failure may request bounded reimport.
	enum class RendererEfficiencyImportDisposition { Pending, Ready, RetryableCookFailure, Invalid };
	struct RendererEfficiencyImportObservation
	{
		RendererEfficiencyImportDisposition disposition = RendererEfficiencyImportDisposition::Pending;
		std::uint64_t generation = 0;
		std::string reason;
	};
	enum class RendererEfficiencyImportDecision { Wait, PrepareBindings, Reimport, Fail };
	RendererEfficiencyImportObservation ClassifyRendererEfficiencyImportReceipt(const nlohmann::json& receipt,
		const UUID& expectedPackage, std::string_view actualSourceHash);
	RendererEfficiencyImportDecision DecideRendererEfficiencyImportAdmission(const RendererEfficiencyImportObservation& observation,
		std::uint32_t admittedImports, std::uint64_t lastFailedGeneration);
	RendererEfficiencyImportObservation InspectRendererEfficiencyOwnedImport(const std::filesystem::path& root,
		const std::filesystem::path& source, const UUID& package, std::string_view expectedSha256);
	// Detached preparation contains only copied identities and contained paths.
	// A job may inspect importer files, but cannot load resources or edit objects.
	struct RendererEfficiencyFixtureBindingCandidate
	{
		RendererEfficiencyPublishedBinding binding;
		std::uint32_t minimumLods = 0, minimumMeshlets = 0;
	};
	struct RendererEfficiencyFixtureBindingPreparation
	{
		std::filesystem::path projectRoot;
		std::vector<RendererEfficiencyFixtureBindingCandidate> candidates;
	};
	bool PrepareRendererEfficiencyFixtureBindings(const std::filesystem::path& projectRoot,
		const nlohmann::json& sealedSteps, RendererEfficiencyFixtureBindingPreparation& output, std::string& error);
	// The protected caller only inspects compact already-published cache snapshots.
	// false leaves output unchanged; unavailable publication can be retried later.
	bool ResolveRendererEfficiencyFixtureBindings(const RendererEfficiencyFixtureBindingPreparation& preparation,
		ResourceManager& resources, std::vector<RendererEfficiencyPublishedBinding>& output, std::string& error);
}
