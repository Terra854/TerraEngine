// PhysicsMaterial.cpp - versioned JSON loading, migration, and bounded reports.

#include "PhysicsMaterial.h"

#include <cmath>
#include <utility>

namespace TerraEngine::Physics
{
	namespace
	{
		bool IsEmpty(const TerraEngine::UUID& id)
		{
			return id == TerraEngine::EMPTY_UUID;
		}

		bool IsKnownCombineMode(PhysicsCombineMode mode)
		{
			return mode == PhysicsCombineMode::Average || mode == PhysicsCombineMode::Minimum
				|| mode == PhysicsCombineMode::Multiply || mode == PhysicsCombineMode::Maximum;
		}

		bool ParseCombineMode(const nlohmann::json& value, PhysicsCombineMode& destination)
		{
			if(!value.is_string())
				return false;
			const std::string mode = value.get<std::string>();
			if(mode == "Average") { destination = PhysicsCombineMode::Average; return true; }
			if(mode == "Minimum") { destination = PhysicsCombineMode::Minimum; return true; }
			if(mode == "Multiply") { destination = PhysicsCombineMode::Multiply; return true; }
			if(mode == "Maximum") { destination = PhysicsCombineMode::Maximum; return true; }
			return false;
		}

		bool ReadFiniteFloat(const nlohmann::json& object, std::string_view key, float& destination, PhysicsMaterialDiagnostics& diagnostics)
		{
			const auto iterator = object.find(std::string(key));
			if(iterator == object.end())
				return true; // Absent optional values retain documented Defaults().
			if(!iterator->is_number())
			{
				diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "invalid_field_type", std::string(key) + " must be a JSON number.");
				return false;
			}
			destination = iterator->get<float>();
			if(!std::isfinite(destination))
			{
				diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "nonfinite_value", std::string(key) + " must be finite.");
				return false;
			}
			return true;
		}
	}

	void PhysicsMaterialDiagnostics::Add(PhysicsMaterialDiagnosticSeverity severity, std::string code, std::string message)
	{
		if(entries.size() >= kPhysicsMaterialMaximumDiagnostics)
		{
			++suppressedCount;
			return;
		}
		entries.push_back({severity, std::move(code), std::move(message)});
	}

	bool PhysicsMaterialDiagnostics::HasErrors() const noexcept
	{
		for(const PhysicsMaterialDiagnostic& entry : entries)
		{
			if(entry.severity == PhysicsMaterialDiagnosticSeverity::Error)
				return true;
		}
		return false;
	}

	PhysicsMaterial PhysicsMaterial::Defaults(TerraEngine::UUID id)
	{
		PhysicsMaterial material;
		material.assetId = id;
		return material;
	}

	PhysicsMaterialValidationResult PhysicsMaterial::Validate() const
	{
		if(schemaVersion != kPhysicsMaterialSchemaVersion)
			return {false, PhysicsMaterialValidationCode::UnsupportedSchema, "Physics material schema version requires an explicit migration."};
		if(IsEmpty(assetId))
			return {false, PhysicsMaterialValidationCode::MissingAssetId, "Physics material requires a stable asset GUID."};
		if(!std::isfinite(staticFriction) || staticFriction < 0.0F || staticFriction > 100.0F)
			return {false, PhysicsMaterialValidationCode::InvalidStaticFriction, "Static friction must be finite, nonnegative, and within calibrated bounds."};
		if(!std::isfinite(dynamicFriction) || dynamicFriction < 0.0F || dynamicFriction > 100.0F)
			return {false, PhysicsMaterialValidationCode::InvalidDynamicFriction, "Dynamic friction must be finite, nonnegative, and within calibrated bounds."};
		if(!std::isfinite(restitution) || restitution < 0.0F || restitution > 1.0F)
			return {false, PhysicsMaterialValidationCode::InvalidRestitution, "Restitution must be finite and between zero and one."};
		if(!std::isfinite(density) || density <= 0.0F || density > 1000000.0F)
			return {false, PhysicsMaterialValidationCode::InvalidDensity, "Density must be finite, positive, and within calibrated bounds."};
		if(!IsKnownCombineMode(frictionCombineMode))
			return {false, PhysicsMaterialValidationCode::InvalidFrictionCombineMode, "Friction combine mode is not an engine-owned value."};
		if(!IsKnownCombineMode(restitutionCombineMode))
			return {false, PhysicsMaterialValidationCode::InvalidRestitutionCombineMode, "Restitution combine mode is not an engine-owned value."};
		if(surfaceTag.size() > kPhysicsMaterialMaximumSurfaceTagLength)
			return {false, PhysicsMaterialValidationCode::InvalidSurfaceTag, "Surface tag exceeds the bounded physics-material limit."};
		return {true, PhysicsMaterialValidationCode::None, {}};
	}

	PhysicsMaterialRuntime PhysicsMaterial::BuildRuntime() const
	{
		return {assetId, staticFriction, dynamicFriction, restitution, density, frictionCombineMode, restitutionCombineMode, surfaceTag};
	}

	nlohmann::json PhysicsMaterial::ToJson() const
	{
		return {
			{"schemaVersion", schemaVersion},
			{"assetId", TerraEngine::ToHexString(assetId)},
			{"staticFriction", staticFriction},
			{"dynamicFriction", dynamicFriction},
			{"restitution", restitution},
			{"density", density},
			{"frictionCombineMode", ToString(frictionCombineMode)},
			{"restitutionCombineMode", ToString(restitutionCombineMode)},
			{"surfaceTag", surfaceTag}
		};
	}

	bool PhysicsMaterial::MigrateJson(nlohmann::json& document, PhysicsMaterialDiagnostics& diagnostics)
	{
		if(!document.is_object())
		{
			diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "invalid_document", "Physics material document must be a JSON object.");
			return false;
		}

		const std::uint32_t sourceVersion = document.value("schemaVersion", 0U);
		if(sourceVersion > kPhysicsMaterialSchemaVersion)
		{
			diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "unsupported_future_schema", "Physics material uses a newer schema and cannot be partially loaded.");
			return false;
		}
		if(sourceVersion == kPhysicsMaterialSchemaVersion)
			return true;

		// Version zero is the only legacy entry point: unversioned early assets
		// gain the v1 defaults before validation. Future migrations extend this
		// explicit ladder rather than guessing at a provider's old representation.
		if(sourceVersion == 0U)
		{
			if(!document.contains("frictionCombineMode")) document["frictionCombineMode"] = "Average";
			if(!document.contains("restitutionCombineMode")) document["restitutionCombineMode"] = "Average";
			if(!document.contains("surfaceTag")) document["surfaceTag"] = "";
			document["schemaVersion"] = kPhysicsMaterialSchemaVersion;
			diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Info, "migrated_v0_to_v1", "Migrated an unversioned physics material using version-one defaults.");
			return true;
		}

		diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "unsupported_schema", "Physics material schema has no registered migration path.");
		return false;
	}

	bool PhysicsMaterial::LoadJson(const nlohmann::json& source, PhysicsMaterial& destination, PhysicsMaterialDiagnostics& diagnostics)
	{
		nlohmann::json document = source;
		if(!MigrateJson(document, diagnostics))
			return false;

		PhysicsMaterial parsed = Defaults();
		parsed.schemaVersion = document.value("schemaVersion", 0U);
		const auto idIterator = document.find("assetId");
		if(idIterator == document.end() || !idIterator->is_string())
		{
			diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "missing_asset_id", "Physics material must contain a string assetId GUID.");
			return false;
		}
		const auto assetId = TerraEngine::ParseUUID(idIterator->get<std::string>());
		if(!assetId)
		{
			diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "invalid_asset_id", "Physics material assetId must be a 32-character hexadecimal GUID.");
			return false;
		}
		parsed.assetId = *assetId;

		if(!ReadFiniteFloat(document, "staticFriction", parsed.staticFriction, diagnostics)
			|| !ReadFiniteFloat(document, "dynamicFriction", parsed.dynamicFriction, diagnostics)
			|| !ReadFiniteFloat(document, "restitution", parsed.restitution, diagnostics)
			|| !ReadFiniteFloat(document, "density", parsed.density, diagnostics))
			return false;

		if(const auto iterator = document.find("frictionCombineMode"); iterator != document.end() && !ParseCombineMode(*iterator, parsed.frictionCombineMode))
		{
			diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "invalid_friction_combine_mode", "Friction combine mode must be an engine-owned symbolic value.");
			return false;
		}
		if(const auto iterator = document.find("restitutionCombineMode"); iterator != document.end() && !ParseCombineMode(*iterator, parsed.restitutionCombineMode))
		{
			diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "invalid_restitution_combine_mode", "Restitution combine mode must be an engine-owned symbolic value.");
			return false;
		}
		if(const auto iterator = document.find("surfaceTag"); iterator != document.end())
		{
			if(!iterator->is_string())
			{
				diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, "invalid_surface_tag", "Surface tag must be a string.");
				return false;
			}
			parsed.surfaceTag = iterator->get<std::string>();
		}

		const PhysicsMaterialValidationResult validation = parsed.Validate();
		if(!validation.accepted)
		{
			diagnostics.Add(PhysicsMaterialDiagnosticSeverity::Error, std::string(ToString(validation.code)), validation.message);
			return false;
		}
		destination = std::move(parsed);
		return true;
	}

	std::string_view ToString(PhysicsMaterialDiagnosticSeverity severity)
	{
		switch(severity)
		{
		case PhysicsMaterialDiagnosticSeverity::Info: return "Info";
		case PhysicsMaterialDiagnosticSeverity::Warning: return "Warning";
		case PhysicsMaterialDiagnosticSeverity::Error: return "Error";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsMaterialValidationCode code)
	{
		switch(code)
		{
		case PhysicsMaterialValidationCode::None: return "None";
		case PhysicsMaterialValidationCode::UnsupportedSchema: return "UnsupportedSchema";
		case PhysicsMaterialValidationCode::MissingAssetId: return "MissingAssetId";
		case PhysicsMaterialValidationCode::InvalidStaticFriction: return "InvalidStaticFriction";
		case PhysicsMaterialValidationCode::InvalidDynamicFriction: return "InvalidDynamicFriction";
		case PhysicsMaterialValidationCode::InvalidRestitution: return "InvalidRestitution";
		case PhysicsMaterialValidationCode::InvalidDensity: return "InvalidDensity";
		case PhysicsMaterialValidationCode::InvalidFrictionCombineMode: return "InvalidFrictionCombineMode";
		case PhysicsMaterialValidationCode::InvalidRestitutionCombineMode: return "InvalidRestitutionCombineMode";
		case PhysicsMaterialValidationCode::InvalidSurfaceTag: return "InvalidSurfaceTag";
		}
		return "Unknown";
	}

}
