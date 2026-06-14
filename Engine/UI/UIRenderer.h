// UIRenderer.h — Backend-agnostic UI batch generator.
//
// This file defines the UIRenderer class, which traverses the UI visual tree
// and generates flattened render batches. It handles text shaping using STB
// TrueType and prepares geometry and material data for the engine's main
// renderer.

#pragma once

#include "UIElement.h"
#include "Renderer/RenderTypes.h"

#include <filesystem>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct UIFontGlyph
{
	glm::vec2 uvMin = glm::vec2(0.0f, 0.0f);
	glm::vec2 uvMax = glm::vec2(0.0f, 0.0f);
	glm::vec2 size = glm::vec2(0.0f, 0.0f);
	glm::vec2 bearing = glm::vec2(0.0f, 0.0f);
	float advance = 0.0f;
};

// struct UIFontAtlas groups related state and operations so callers have one documented contract.
struct UIFontAtlas
{
	bool ready = false;
	int width = 0;
	int height = 0;
	float pixelHeight = 32.0f;
	std::vector<unsigned char> pixels;
	std::unordered_map<char, UIFontGlyph> glyphs;
};

// class UIRenderer groups related state and operations so callers have one documented contract.
class UIRenderer
{
public:
	UIRenderBatch BuildBatch(
		const UIElement::Ptr& root,
		const glm::vec2& viewportSize,
		const std::vector<UIImmediateQuad>& immediateQuads = {}) const;

	const UIFontAtlas& EnsureSignedDistanceFontAtlas(const std::filesystem::path& fontPath, float pixelHeight) const;
	const UIFontAtlas& EnsureSignedDistanceFontAtlasFromBytes(const std::string& fontName, const std::vector<unsigned char>& fontBytes, float pixelHeight) const;

private:
	void CollectDraws(const UIElement::Ptr& element, UIRenderBatch& batch) const;
	UITextRun BuildTextRun(const UIElement& element) const;
	UIImageRun BuildImageRun(const UIElement& element) const;

	mutable std::mutex fontAtlasMutex_;
	mutable std::unordered_map<std::string, UIFontAtlas> fontAtlases_;
};
