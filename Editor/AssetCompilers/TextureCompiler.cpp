#include "TextureCompiler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>
#include <ktx.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_HDR
#define STBI_ONLY_GIF
#define STBI_ONLY_PIC
#define STBI_ONLY_PNM
#include "External/stb/stb_image.h"

namespace {
	constexpr std::array<std::string_view, 11> kSupportedTextureSourceExtensions = {
		".png",
		".jpg",
		".jpeg",
		".bmp",
		".tga",
		".psd",
		".hdr",
		".gif",
		".pic",
		".ppm",
		".pgm"};

	struct KtxTextureDestroy {
		void operator()(ktxTexture2* texture) const {
			if (texture != nullptr)
			{
				ktxTexture2_Destroy(texture);
			}
		}
	};

	struct FreeBytes {
		void operator()(ktx_uint8_t* bytes) const {
			std::free(bytes);
		}
	};

	using UniqueKtxTexture2 = std::unique_ptr<ktxTexture2, KtxTextureDestroy>;
	using UniqueKtxBytes = std::unique_ptr<ktx_uint8_t, FreeBytes>;

	std::uint32_t AlignUp32(std::uint32_t value, std::uint32_t alignment) {
		const std::uint32_t mask = alignment - 1;
		return (value + mask) & ~mask;
	}

	std::string NormalizeExtension(const std::filesystem::path& filePath) {
		std::string ext = filePath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return ext;
	}

	VkFormat ChooseCompiledVkFormat(TextureFileSemantic semantic) {
		// Color textures stay in sRGB space so authored albedo maps sample
		// correctly at runtime. Data textures remain UNORM to preserve raw values.
		if (semantic == TextureFileSemantic::Color)
		{
			return VK_FORMAT_R8G8B8A8_SRGB;
		}
		return VK_FORMAT_R8G8B8A8_UNORM;
	}

	bool PopulateCurrentDescriptor(
		const VkFormat vkFormat,
		const TextureFileSemantic semantic,
		const int width,
		const int height,
		TETFDesc& desc)
	{
		if (width <= 0 || height <= 0)
		{
			std::cerr << "Texture dimensions must be positive. width=" << width << " height=" << height << "\n";
			return false;
		}

		if (width > std::numeric_limits<std::uint16_t>::max() || height > std::numeric_limits<std::uint16_t>::max())
		{
			std::cerr << "Texture dimensions exceed the current TETF descriptor limits: " << width << "x" << height << "\n";
			return false;
		}

		desc.width = static_cast<std::uint16_t>(width);
		desc.height = static_cast<std::uint16_t>(height);
		desc.depth = 1;
		desc.layers = 1;
		desc.mipCount = 1;
		desc.faces = 1;
		desc.semantic = static_cast<std::uint8_t>(semantic);
		desc.payloadChunk = 1;
		desc.userMetadataChunk = TETF_INVALID_CHUNK_INDEX;
		desc.thumbnailChunk = TETF_INVALID_CHUNK_INDEX;
		desc.vkFormat = static_cast<std::uint32_t>(vkFormat);
		return true;
	}

	bool BuildKtx2Payload(
		const std::vector<std::uint8_t>& imageData,
		const int width,
		const int height,
		const VkFormat vkFormat,
		std::vector<std::uint8_t>& outPayload)
	{
		ktxTextureCreateInfo createInfo{};
		createInfo.vkFormat = static_cast<ktx_uint32_t>(vkFormat);
		createInfo.baseWidth = static_cast<ktx_uint32_t>(width);
		createInfo.baseHeight = static_cast<ktx_uint32_t>(height);
		createInfo.baseDepth = 1;
		createInfo.numDimensions = 2;
		createInfo.numLevels = 1;
		createInfo.numLayers = 1;
		createInfo.numFaces = 1;
		createInfo.isArray = KTX_FALSE;
		createInfo.generateMipmaps = KTX_FALSE;

		ktxTexture2* rawTexture = nullptr;
		const KTX_error_code createResult = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &rawTexture);
		if (createResult != KTX_SUCCESS || rawTexture == nullptr)
		{
			std::cerr << "Failed to create KTX2 texture: " << ktxErrorString(createResult) << "\n";
			return false;
		}

		UniqueKtxTexture2 texture(rawTexture);

		const KTX_error_code imageResult = ktxTexture_SetImageFromMemory(
			ktxTexture(texture.get()),
			0,
			0,
			0,
			imageData.data(),
			imageData.size());
		if (imageResult != KTX_SUCCESS)
		{
			std::cerr << "Failed to copy image data into KTX2 texture: " << ktxErrorString(imageResult) << "\n";
			return false;
		}

		ktx_uint8_t* rawBytes = nullptr;
		ktx_size_t rawSize = 0;
		const KTX_error_code writeResult = ktxTexture2_WriteToMemory(texture.get(), &rawBytes, &rawSize);
		if (writeResult != KTX_SUCCESS || rawBytes == nullptr || rawSize == 0)
		{
			std::cerr << "Failed to serialize KTX2 payload: " << ktxErrorString(writeResult) << "\n";
			return false;
		}

		UniqueKtxBytes ownedBytes(rawBytes);
		outPayload.assign(ownedBytes.get(), ownedBytes.get() + rawSize);
		return true;
	}
}

bool IsSupportedTextureSourceExtension(const std::filesystem::path& filePath) {
	const std::string ext = NormalizeExtension(filePath);
	return std::find(
		kSupportedTextureSourceExtensions.begin(),
		kSupportedTextureSourceExtensions.end(),
		std::string_view(ext)) != kSupportedTextureSourceExtensions.end();
}

std::string GetSupportedTextureSourceExtensionsText() {
	std::string result;
	for (std::size_t index = 0; index < kSupportedTextureSourceExtensions.size(); ++index) {
		if (!result.empty()) {
			result += ", ";
		}
		result += kSupportedTextureSourceExtensions[index];
	}
	return result;
}

std::string GetCompiledTextureOutputDescription() {
	return std::string(kCompiledTextureExtension) + " containing an embedded KTX2 payload";
}

std::uint32_t ComputeCrc32(const std::uint8_t* data, size_t size) {
	static std::uint32_t table[256];
	static bool initialized = false;
	if (!initialized) {
		for (std::uint32_t i = 0; i < 256; ++i) {
			std::uint32_t crc = i;
			for (int j = 0; j < 8; ++j) {
				const bool bit = crc & 1u;
				crc = (crc >> 1) ^ (bit ? 0xEDB88320u : 0u);
			}
			table[i] = crc;
		}
		initialized = true;
	}

	std::uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i) {
		const std::uint8_t byte = data[i];
		crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFu;
}

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic) {
	if (!IsSupportedTextureSourceExtension(input))
	{
		std::cerr << "Unsupported texture source extension: " << input.extension().string() << "\n";
		std::cerr << "Supported source formats: " << GetSupportedTextureSourceExtensionsText() << "\n";
		return false;
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	std::uint8_t* pixels = stbi_load(input.string().c_str(), &width, &height, &channels, 4);
	if (!pixels) {
		std::cerr << "Failed to load image: " << input << " (" << stbi_failure_reason() << ")\n";
		return false;
	}

	const std::size_t mipSize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
	std::vector<std::uint8_t> imageData(pixels, pixels + mipSize);
	stbi_image_free(pixels);

	const VkFormat compiledVkFormat = ChooseCompiledVkFormat(semantic);

	TETFDesc desc{};
	if (!PopulateCurrentDescriptor(compiledVkFormat, semantic, width, height, desc))
	{
		return false;
	}

	std::vector<std::uint8_t> ktx2Payload;
	if (!BuildKtx2Payload(imageData, width, height, compiledVkFormat, ktx2Payload))
	{
		return false;
	}

	TETFHeader header{};
	header.magic = kMagic;
	header.versionMajor = kVersionMajor;
	header.headerSize = static_cast<std::uint16_t>(sizeof(TETFHeader));
	// The current wrapper contract is inferred from version + chunk layout, so
	// flags stay zero until a future direct-replacement format needs them.
	header.flags = 0;
	header.alignment = 16;
	header.chunkCount = 2;
	header.fileCrc32 = 0;
	header.totalSize = 0;
	header.tocOffset = AlignUp32(sizeof(TETFHeader), header.alignment);

	const std::uint32_t tocSize = header.chunkCount * static_cast<std::uint32_t>(sizeof(TETFChunkEntry));
	const std::uint32_t descOffset = AlignUp32(static_cast<std::uint32_t>(header.tocOffset) + tocSize, header.alignment);
	const std::uint32_t descSize = static_cast<std::uint32_t>(sizeof(TETFDesc));
	const std::uint32_t ktx2Offset = AlignUp32(descOffset + descSize, header.alignment);
	const std::uint32_t totalSize = AlignUp32(ktx2Offset + static_cast<std::uint32_t>(ktx2Payload.size()), header.alignment);
	header.totalSize = totalSize;

	std::vector<TETFChunkEntry> entries;
	entries.resize(header.chunkCount);
	entries[0] = {kDescChunkId, 0, descOffset, descSize};
	entries[1] = {kKtx2ChunkId, 0, ktx2Offset, static_cast<std::uint64_t>(ktx2Payload.size())};

	std::vector<std::uint8_t> fileData;
	fileData.resize(totalSize, 0);
	std::memcpy(fileData.data(), &header, sizeof(header));
	std::memcpy(fileData.data() + header.tocOffset, entries.data(), entries.size() * sizeof(TETFChunkEntry));
	std::memcpy(fileData.data() + descOffset, &desc, sizeof(desc));
	std::memcpy(fileData.data() + ktx2Offset, ktx2Payload.data(), ktx2Payload.size());

	const std::uint32_t crc = ComputeCrc32(fileData.data(), fileData.size());
	std::memcpy(fileData.data() + offsetof(TETFHeader, fileCrc32), &crc, sizeof(crc));

	if (output.has_parent_path())
	{
		std::filesystem::create_directories(output.parent_path());
	}

	std::ofstream out(output, std::ios::binary);
	if (!out) {
		std::cerr << "Failed to open output path: " << output << "\n";
		return false;
	}

	// The output remains a .tetf wrapper so authored references do not change,
	// but the payload bytes now come from libktx instead of TerraEngine's legacy
	// raw mip-chunk layout.
	out.write(reinterpret_cast<const char*>(fileData.data()), static_cast<std::streamsize>(fileData.size()));
	if (!out.good())
	{
		std::cerr << "Failed to write compiled texture: " << output << "\n";
		return false;
	}

	return true;
}
