// InspectorDisplayNames.h - stable, user-facing labels for reflected identifiers.

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace TerraEngine::Editor
{
	namespace Detail
	{
		[[nodiscard]] inline bool IsInspectorNameSeparator(char value) noexcept
		{
			return value == '_' || value == '-' || std::isspace(static_cast<unsigned char>(value)) != 0;
		}

		[[nodiscard]] inline std::string FormatInspectorNameToken(std::string token)
		{
			std::string normalized = token;
			std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](char value) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
			});
			// Common technical abbreviations are easier to scan when their established
			// capitalization is preserved instead of appearing as ordinary words.
			if(normalized == "id") return "ID";
			if(normalized == "uuid") return "UUID";
			if(normalized == "fov") return "FOV";
			if(normalized == "lod") return "LOD";
			if(normalized == "ui") return "UI";
			if(normalized == "gpu") return "GPU";
			if(normalized == "cpu") return "CPU";
			if(normalized == "api") return "API";
			if(normalized == "json") return "JSON";
			if(normalized == "xml") return "XML";
			if(normalized == "hrtf") return "HRTF";
			if(normalized == "pcm") return "PCM";
			if(normalized == "fps") return "FPS";
			if(normalized == "rgb") return "RGB";
			if(normalized == "rgba") return "RGBA";
			if(normalized == "hdr") return "HDR";
			if(normalized == "lfe") return "LFE";
			if(!normalized.empty())
				normalized.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(normalized.front())));
			return normalized;
		}
	}

	// Convert camelCase, PascalCase, snake_case, and acronym-bearing reflection
	// identifiers into labels without changing the stable serialized key.
	[[nodiscard]] inline std::string BuildInspectorIdentifierLabel(std::string_view identifier)
	{
		std::string result;
		std::string token;
		const auto flushToken = [&]() {
			if(token.empty()) return;
			if(!result.empty()) result.push_back(' ');
			result += Detail::FormatInspectorNameToken(std::move(token));
			token.clear();
		};

		for(std::size_t index = 0U; index < identifier.size(); ++index)
		{
			const char current = identifier[index];
			if(Detail::IsInspectorNameSeparator(current))
			{
				flushToken();
				continue;
			}

			const bool hasPrevious = index > 0U && !Detail::IsInspectorNameSeparator(identifier[index - 1U]);
			const bool hasNext = index + 1U < identifier.size() && !Detail::IsInspectorNameSeparator(identifier[index + 1U]);
			const unsigned char currentByte = static_cast<unsigned char>(current);
			const unsigned char previousByte = hasPrevious ? static_cast<unsigned char>(identifier[index - 1U]) : 0U;
			const unsigned char nextByte = hasNext ? static_cast<unsigned char>(identifier[index + 1U]) : 0U;
			const bool startsCapitalizedWord = hasPrevious && std::isupper(currentByte) != 0
				&& (std::islower(previousByte) != 0 || std::isdigit(previousByte) != 0);
			const bool endsAcronym = hasPrevious && hasNext && std::isupper(currentByte) != 0
				&& std::isupper(previousByte) != 0 && std::islower(nextByte) != 0;
			const bool changesBetweenTextAndNumber = hasPrevious
				&& ((std::isdigit(currentByte) != 0) != (std::isdigit(previousByte) != 0));
			if(startsCapitalizedWord || endsAcronym || changesBetweenTextAndNumber) flushToken();
			token.push_back(current);
		}
		flushToken();
		return result;
	}

	// A small set of authoring labels explains intent better than a mechanical
	// identifier split. Every unlisted property still receives the generic label.
	[[nodiscard]] inline std::string BuildStandardInspectorPropertyLabel(
		std::string_view componentName, std::string_view propertyName)
	{
		if(componentName == "AudioSourceComponent")
		{
			if(propertyName == "clipAssetId") return "Audio Clip";
			if(propertyName == "maxDistance") return "Maximum Distance";
			if(propertyName == "mixGroupId") return "Mixer Group";
		}
		if(componentName == "AudioListenerComponent")
		{
			if(propertyName == "eligible") return "Use For Playback";
		}
		if(componentName == "CameraComponent")
		{
			if(propertyName == "verticalFovDegrees") return "Vertical FOV (Degrees)";
			if(propertyName == "nearPlane") return "Near Clipping Plane";
			if(propertyName == "farPlane") return "Far Clipping Plane";
			if(propertyName == "up") return "Up Direction";
		}
		if(componentName == "TerrainComponent")
		{
			if(propertyName == "terrainAssetId") return "Terrain Asset";
			if(propertyName == "castsShadows") return "Cast Shadows";
		}
		if(componentName == "Transform")
		{
			if(propertyName == "worldPose") return "Canonical World Pose";
			if(propertyName == "parentRelativePose") return "Bounded Parent Transform Space";
		}
		return BuildInspectorIdentifierLabel(propertyName);
	}
}
