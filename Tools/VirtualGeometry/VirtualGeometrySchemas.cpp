// VirtualGeometrySchemas.cpp - deterministic JSON manifest validation.

#include "VirtualGeometrySchemas.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace TerraEngine::Tools::VirtualGeometry
{
	namespace
	{
		using Json = nlohmann::ordered_json;

		void AddIssue(
			SchemaValidationResult& result,
			std::string path,
			std::string message)
		{
			result.issues.push_back({
				std::move(path),
				std::move(message)
			});
		}

		void ValidateVersion(const Json& document, SchemaValidationResult& result)
		{
			const auto version = document.find("schemaVersion");
			if(version == document.end() || !version->is_number_unsigned())
			{
				AddIssue(result, "$.schemaVersion",
					"schemaVersion must be a positive unsigned integer.");
				return;
			}
			if(version->get<std::uint64_t>() != 1u)
			{
				AddIssue(result, "$.schemaVersion",
					"This executable supports schemaVersion 1.");
			}
		}

		void ValidateIdentifiedArray(
			const Json& document,
			std::string_view memberName,
			SchemaValidationResult& result)
		{
			const std::string member(memberName);
			const auto values = document.find(member);
			if(values == document.end() || !values->is_array() || values->empty())
			{
				AddIssue(result, "$." + member,
					member + " must be a non-empty array.");
				return;
			}

			std::set<std::string> identifiers;
			for(std::size_t index = 0u; index < values->size(); ++index)
			{
				const Json& value = (*values)[index];
				const std::string path =
					"$." + member + "[" + std::to_string(index) + "]";
				if(!value.is_object())
				{
					AddIssue(result, path, "Each entry must be an object.");
					continue;
				}

				const auto id = value.find("id");
				if(id == value.end() || !id->is_string() || id->get_ref<const std::string&>().empty())
				{
					AddIssue(result, path + ".id",
						"Each entry must have a non-empty string id.");
					continue;
				}
				if(!identifiers.insert(id->get<std::string>()).second)
				{
					AddIssue(result, path + ".id",
						"Entry ids must be unique within the manifest.");
				}
			}
		}

		void RequireObject(
			const Json& document,
			std::string_view memberName,
			SchemaValidationResult& result)
		{
			const std::string member(memberName);
			const auto value = document.find(member);
			if(value == document.end() || !value->is_object())
			{
				AddIssue(result, "$." + member,
					member + " must be an object.");
			}
		}

		void RequireArray(
			const Json& document,
			std::string_view memberName,
			SchemaValidationResult& result)
		{
			const std::string member(memberName);
			const auto value = document.find(member);
			if(value == document.end() || !value->is_array())
			{
				AddIssue(result, "$." + member,
					member + " must be an array.");
			}
		}

		void ValidateBenchmarkCapture(
			const Json& document,
			SchemaValidationResult& result)
		{
			constexpr std::string_view objectSections[] = {
				"identity",
				"environment",
				"workload",
				"warmupAndSampling",
				"statistics",
				"geometry",
				"residencyAndStreaming",
				"visibilityAndShading",
				"deformation",
				"auxiliaryViews",
				"fallbacksAndFailures",
				"baselineAndComparability",
				"artifacts",
				"integrity"
			};
			for(const std::string_view section : objectSections)
			{
				RequireObject(document, section, result);
			}
			RequireArray(document, "capabilities", result);
			RequireArray(document, "rawSamples", result);

			const auto identity = document.find("identity");
			if(identity != document.end() && identity->is_object())
			{
				const auto version = identity->find("schemaVersion");
				const bool supportedString =
					version != identity->end()
					&& version->is_string()
					&& version->get_ref<const std::string&>() == "1";
				const bool supportedNumber =
					version != identity->end()
					&& version->is_number_unsigned()
					&& version->get<std::uint64_t>() == 1u;
				if(!supportedString && !supportedNumber)
				{
					AddIssue(result, "$.identity.schemaVersion",
						"Benchmark identity must use schemaVersion 1.");
				}
			}
		}
	}

	SchemaValidationResult ValidateSchemaDocument(
		std::string_view jsonText,
		SchemaDocumentKind kind)
	{
		SchemaValidationResult result;
		Json document;
		try
		{
			document = Json::parse(jsonText.begin(), jsonText.end());
		}
		catch(const Json::parse_error& error)
		{
			AddIssue(result, "$",
				"JSON parsing failed at byte "
					+ std::to_string(error.byte) + ": " + error.what());
			return result;
		}

		if(!document.is_object())
		{
			AddIssue(result, "$", "The schema document root must be an object.");
			return result;
		}

		if(kind != SchemaDocumentKind::BenchmarkCapture)
		{
			ValidateVersion(document, result);
		}
		switch(kind)
		{
		case SchemaDocumentKind::Workloads:
			ValidateIdentifiedArray(document, "workloads", result);
			break;
		case SchemaDocumentKind::HardwareMatrix:
			ValidateIdentifiedArray(document, "hardwareProfiles", result);
			break;
		case SchemaDocumentKind::QualityProfiles:
			ValidateIdentifiedArray(document, "qualityProfiles", result);
			break;
		case SchemaDocumentKind::BenchmarkCapture:
			ValidateBenchmarkCapture(document, result);
			break;
		}

		std::sort(result.issues.begin(), result.issues.end(),
			[](const SchemaValidationIssue& left,
				const SchemaValidationIssue& right)
			{
				if(left.path != right.path)
				{
					return left.path < right.path;
				}
				return left.message < right.message;
			});
		result.valid = result.issues.empty();
		if(result.valid)
		{
			// ordered_json preserves authored key order while dump(2) gives every
			// tool the same newline/indent representation for hashing.
			result.canonicalJson = document.dump(2) + '\n';
		}
		return result;
	}

	SchemaValidationResult LoadAndValidateSchemaDocument(
		const std::filesystem::path& path,
		SchemaDocumentKind kind)
	{
		std::ifstream input(path, std::ios::binary);
		if(!input)
		{
			SchemaValidationResult result;
			AddIssue(result, "$",
				"Unable to open schema document: " + path.string());
			return result;
		}

		std::ostringstream bytes;
		bytes << input.rdbuf();
		if(input.bad())
		{
			SchemaValidationResult result;
			AddIssue(result, "$",
				"Unable to read the complete schema document: " + path.string());
			return result;
		}
		return ValidateSchemaDocument(bytes.str(), kind);
	}
}
