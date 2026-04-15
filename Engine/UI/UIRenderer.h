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

struct UIFontAtlas
{
	bool ready = false;
	int width = 0;
	int height = 0;
	float pixelHeight = 32.0f;
	std::vector<unsigned char> pixels;
	std::unordered_map<char, UIFontGlyph> glyphs;
};

class UIRenderer
{
public:
	UIRenderBatch BuildBatch(
		const UIElement::Ptr& root,
		const glm::vec2& viewportSize,
		const std::vector<UIImmediateQuad>& immediateQuads = {}) const;

	const UIFontAtlas& EnsureSignedDistanceFontAtlas(const std::filesystem::path& fontPath, float pixelHeight) const;

private:
	void CollectDraws(const UIElement::Ptr& element, UIRenderBatch& batch) const;
	UITextRun BuildTextRun(const UIElement& element) const;
	UIImageRun BuildImageRun(const UIElement& element) const;

	mutable std::mutex fontAtlasMutex_;
	mutable std::unordered_map<std::string, UIFontAtlas> fontAtlases_;
};
