#include "UIRenderer.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace
{
glm::vec4 ApplyOpacity(const glm::vec4& color, float opacity)
{
	glm::vec4 tinted = color;
	tinted.w *= opacity;
	return tinted;
}

glm::vec2 ResolvePaddingOffset(const UIStyle& style)
{
	if (!style.padding.has_value())
	{
		return glm::vec2(8.0f, 8.0f);
	}

	const UIEdgeValues& padding = *style.padding;
	return glm::vec2(padding.left.value, padding.top.value);
}
}

UIRenderBatch UIRenderer::BuildBatch(
	const UIElement::Ptr& root,
	const glm::vec2& viewportSize,
	const std::vector<UIImmediateQuad>& immediateQuads) const
{
	UIRenderBatch batch;
	batch.viewportSize = viewportSize;
	batch.immediateQuads = immediateQuads;

	if (root)
	{
		CollectDraws(root, batch);
	}

	return batch;
}

const UIFontAtlas& UIRenderer::EnsureSignedDistanceFontAtlas(const std::filesystem::path& fontPath, float pixelHeight) const
{
	const std::string cacheKey = fontPath.generic_string() + "#" + std::to_string(pixelHeight);

	std::lock_guard<std::mutex> lock(fontAtlasMutex_);
	if (auto existing = fontAtlases_.find(cacheKey); existing != fontAtlases_.end())
	{
		return existing->second;
	}

	UIFontAtlas atlas;
	atlas.pixelHeight = pixelHeight;

	std::ifstream input(fontPath, std::ios::binary);
	if (!input)
	{
		fontAtlases_[cacheKey] = atlas;
		return fontAtlases_[cacheKey];
	}

	std::vector<unsigned char> fontBytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	stbtt_fontinfo fontInfo{};
	if (stbtt_InitFont(&fontInfo, fontBytes.data(), 0) == 0)
	{
		fontAtlases_[cacheKey] = atlas;
		return fontAtlases_[cacheKey];
	}

	struct PendingGlyph
	{
		char character = 0;
		int width = 0;
		int height = 0;
		int offsetX = 0;
		int offsetY = 0;
		int advanceWidth = 0;
		int leftSideBearing = 0;
		std::vector<unsigned char> pixels;
	};

	const float scale = stbtt_ScaleForPixelHeight(&fontInfo, pixelHeight);
	const int atlasWidth = 512;
	int cursorX = 0;
	int cursorY = 0;
	int rowHeight = 0;
	std::vector<PendingGlyph> pendingGlyphs;

	for (int codepoint = 32; codepoint <= 126; ++codepoint)
	{
		int width = 0;
		int height = 0;
		int offsetX = 0;
		int offsetY = 0;
		unsigned char* sdfBitmap = stbtt_GetCodepointSDF(&fontInfo, scale, codepoint, 6, 128, 64.0f, &width, &height, &offsetX, &offsetY);
		if (sdfBitmap == nullptr || width <= 0 || height <= 0)
		{
			continue;
		}

		PendingGlyph glyph;
		glyph.character = static_cast<char>(codepoint);
		glyph.width = width;
		glyph.height = height;
		glyph.offsetX = offsetX;
		glyph.offsetY = offsetY;
		glyph.pixels.assign(sdfBitmap, sdfBitmap + (width * height));
		stbtt_FreeSDF(sdfBitmap, nullptr);

		stbtt_GetCodepointHMetrics(&fontInfo, codepoint, &glyph.advanceWidth, &glyph.leftSideBearing);
		pendingGlyphs.push_back(std::move(glyph));
	}

	for (const PendingGlyph& glyph : pendingGlyphs)
	{
		if (cursorX + glyph.width + 1 >= atlasWidth)
		{
			cursorX = 0;
			cursorY += rowHeight + 1;
			rowHeight = 0;
		}

		rowHeight = std::max(rowHeight, glyph.height);
		cursorX += glyph.width + 1;
	}

	const int atlasHeight = std::max(1, cursorY + rowHeight + 1);
	atlas.width = atlasWidth;
	atlas.height = atlasHeight;
	atlas.pixels.assign(static_cast<std::size_t>(atlasWidth * atlasHeight), 0);

	cursorX = 0;
	cursorY = 0;
	rowHeight = 0;
	for (const PendingGlyph& glyph : pendingGlyphs)
	{
		if (cursorX + glyph.width + 1 >= atlasWidth)
		{
			cursorX = 0;
			cursorY += rowHeight + 1;
			rowHeight = 0;
		}

		for (int row = 0; row < glyph.height; ++row)
		{
			std::memcpy(
				atlas.pixels.data() + ((cursorY + row) * atlasWidth) + cursorX,
				glyph.pixels.data() + (row * glyph.width),
				static_cast<std::size_t>(glyph.width));
		}

		UIFontGlyph glyphInfo;
		glyphInfo.uvMin = glm::vec2(
			static_cast<float>(cursorX) / static_cast<float>(atlasWidth),
			static_cast<float>(cursorY) / static_cast<float>(atlasHeight));
		glyphInfo.uvMax = glm::vec2(
			static_cast<float>(cursorX + glyph.width) / static_cast<float>(atlasWidth),
			static_cast<float>(cursorY + glyph.height) / static_cast<float>(atlasHeight));
		glyphInfo.size = glm::vec2(static_cast<float>(glyph.width), static_cast<float>(glyph.height));
		glyphInfo.bearing = glm::vec2(static_cast<float>(glyph.offsetX), static_cast<float>(glyph.offsetY));
		glyphInfo.advance = static_cast<float>(glyph.advanceWidth) * scale;
		atlas.glyphs[glyph.character] = glyphInfo;

		rowHeight = std::max(rowHeight, glyph.height);
		cursorX += glyph.width + 1;
	}

	atlas.ready = !atlas.glyphs.empty();
	fontAtlases_[cacheKey] = atlas;
	return fontAtlases_[cacheKey];
}

void UIRenderer::CollectDraws(const UIElement::Ptr& element, UIRenderBatch& batch) const
{
	if (!element)
	{
		return;
	}

	const UIStyle& style = element->displayStyle;
	const float opacity = style.opacity.value_or(1.0f);
	const glm::vec2 translate = style.transform.translate.value_or(glm::vec2(0.0f, 0.0f));
	const float scale = style.transform.scale.value_or(1.0f);

	if (style.HasRenderableBackground() && element->calculatedSize.x > 0.0f && element->calculatedSize.y > 0.0f)
	{
		UIStaticQuad quad;
		quad.position = element->calculatedPosition + translate;
		quad.size = element->calculatedSize * scale;
		quad.color = ApplyOpacity(*style.backgroundColor, opacity);
		quad.cornerRadius = style.borderRadius.value_or(0.0f);
		batch.staticQuads.push_back(quad);
	}

	if ((element->type == UIElementType::Label || element->type == UIElementType::Button) && !element->text.empty())
	{
		batch.textRuns.push_back(BuildTextRun(*element));
	}

	if (element->type == UIElementType::Image && !element->source.empty())
	{
		batch.imageRuns.push_back(BuildImageRun(*element));
	}

	for (const UIElement::Ptr& child : element->children)
	{
		CollectDraws(child, batch);
	}
}

UITextRun UIRenderer::BuildTextRun(const UIElement& element) const
{
	UITextRun run;
	const UIStyle& style = element.displayStyle;
	const glm::vec2 translate = style.transform.translate.value_or(glm::vec2(0.0f, 0.0f));
	const float fontSize = style.fontSize.value_or(element.type == UIElementType::Button ? 18.0f : 16.0f);
	const glm::vec2 paddingOffset = ResolvePaddingOffset(style);

	run.text = element.text;
	run.fontSize = fontSize;
	run.color = ApplyOpacity(style.color.value_or(glm::vec4(1.0f)), style.opacity.value_or(1.0f));
	run.bounds = element.calculatedSize;
	run.position = element.calculatedPosition + translate + paddingOffset;

	if (element.type == UIElementType::Button)
	{
		const float estimatedWidth = static_cast<float>(element.text.size()) * fontSize * 0.55f;
		run.position.x = element.calculatedPosition.x + ((element.calculatedSize.x - estimatedWidth) * 0.5f);
		run.position.y = element.calculatedPosition.y + ((element.calculatedSize.y - fontSize) * 0.5f);
	}

	return run;
}

UIImageRun UIRenderer::BuildImageRun(const UIElement& element) const
{
	UIImageRun run;
	const UIStyle& style = element.displayStyle;
	const glm::vec2 translate = style.transform.translate.value_or(glm::vec2(0.0f, 0.0f));
	const float scale = style.transform.scale.value_or(1.0f);

	run.texturePath = element.source;
	run.position = element.calculatedPosition + translate;
	run.size = element.calculatedSize * scale;
	run.color = ApplyOpacity(style.color.value_or(glm::vec4(1.0f)), style.opacity.value_or(1.0f));
	run.cornerRadius = style.borderRadius.value_or(0.0f);

	return run;
}
