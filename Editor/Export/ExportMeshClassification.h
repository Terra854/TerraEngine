// ExportMeshClassification.h - classifies canonical TEMF payloads for export metadata.

#pragma once

#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/VirtualGeometryCodec.h"

#include <cstdint>
#include <span>
#include <string>

namespace TerraEngine::Editor::Export
{
	struct MeshRepresentationClassification
	{
		bool valid = false;
		bool requiresVirtualGeometry = false;
		std::uint32_t representationFlags = 0u;
		std::string diagnostic;
	};

	// Classify the representation declared by the cooked payload itself. A TEMF
	// extension identifies the container format, but small meshes legitimately
	// contain only direct-resident meshlets and do not require virtual pages.
	inline MeshRepresentationClassification ClassifyMeshRepresentation(
		std::span<const std::uint8_t> payload)
	{
		MeshRepresentationClassification result;
		const Resource::TemfProductionValidationResult validated =
			Resource::ValidateTemfProductionFile(payload, {});
		if (!validated.IsComplete())
		{
			result.diagnostic = validated.diagnostic.empty()
				? "The staged mesh is not a complete canonical TEMFPRD1 payload."
				: validated.diagnostic;
			return result;
		}

		result.valid = true;
		result.representationFlags = validated.directory.header.representationFlags;
		result.requiresVirtualGeometry =
			(result.representationFlags & kTemfRepresentationVirtualGeometry) != 0u;
		return result;
	}
}
