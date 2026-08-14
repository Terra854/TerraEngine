// VirtualGeometrySchemas.h - validation entry points for production manifests.
//
// Tooling, tests, and the installed benchmark share this small API so a schema
// failure has the same path and explanation everywhere.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tools::VirtualGeometry
{
	enum class SchemaDocumentKind
	{
		Workloads,
		HardwareMatrix,
		QualityProfiles,
		BenchmarkCapture,
		ReferenceCapture,
		ComparativeEvidence
	};

	struct SchemaValidationIssue
	{
		std::string path;
		std::string message;
	};

	struct SchemaValidationResult
	{
		bool valid = false;
		std::vector<SchemaValidationIssue> issues;
		std::string canonicalJson;
	};

	[[nodiscard]] SchemaValidationResult ValidateSchemaDocument(
		std::string_view jsonText,
		SchemaDocumentKind kind);

	[[nodiscard]] SchemaValidationResult LoadAndValidateSchemaDocument(
		const std::filesystem::path& path,
		SchemaDocumentKind kind);
}
