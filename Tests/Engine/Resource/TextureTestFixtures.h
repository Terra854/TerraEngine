// Fresh legal texture wire bytes and independently authored image expectations.
#pragma once
#include "File Formats/TerraEngineTextureFile.h"
#include "Tests/Engine/Runtime/CorrectnessTestSupport.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Tests::TextureFixtures {
	using Pixel=std::array<std::uint8_t,4>;
	inline constexpr Pixel Red{255,0,0,255}, Blue{0,0,255,255}, Green{0,255,0,255}, Yellow{255,255,0,255};
	using ColorAt=std::function<Pixel(unsigned,unsigned,unsigned)>;
	using Buffer=std::vector<std::byte>;
	struct Image {
		Buffer bytes;
		std::vector<std::vector<Pixel>> expected;
		template<class T> T Read(std::uint64_t offset) const {
			if(offset>bytes.size() || sizeof(T)>bytes.size()-offset) throw std::runtime_error("Texture fixture field exceeds owned bytes.");
			T value{}; std::memcpy(&value,bytes.data()+static_cast<std::size_t>(offset),sizeof(value)); return value;
		}
		template<class T> void Write(std::uint64_t offset,const T& value) {
			if(offset>bytes.size() || sizeof(T)>bytes.size()-offset) throw std::runtime_error("Texture fixture write exceeds owned bytes.");
			std::memcpy(bytes.data()+static_cast<std::size_t>(offset),&value,sizeof(value));
		}
		TETFHeader Header() const { return Read<TETFHeader>(0); }
		TETFChunkEntry Chunk(unsigned index) const { const auto h=Header(); if(index>=h.chunkCount) throw std::runtime_error("Texture fixture chunk missing."); return Read<TETFChunkEntry>(h.tocOffset+std::uint64_t(index)*sizeof(TETFChunkEntry)); }
		void SetChunk(unsigned index,const TETFChunkEntry& value) { const auto h=Header(); if(index>=h.chunkCount) throw std::runtime_error("Texture fixture chunk missing."); Write(h.tocOffset+std::uint64_t(index)*sizeof(TETFChunkEntry),value); }
		TETFDesc Desc() const { return Read<TETFDesc>(Chunk(0).offset); }
		void SetDesc(const TETFDesc& value) { Write(Chunk(0).offset,value); }
		void Checksum() {
			auto h=Header(); h.totalSize=bytes.size(); h.fileCrc32=0; Write(0,h);
			std::uint32_t crc=0xffffffffU; for(const auto b:bytes) { crc^=std::to_integer<std::uint8_t>(b); for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320U:0); }
			h.fileCrc32=~crc; Write(0,h);
		}
		void Save(const std::filesystem::path& path) const {
			std::filesystem::create_directories(path.parent_path()); std::ofstream out(path,std::ios::binary|std::ios::trunc);
			out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())); out.close(); if(!out) throw std::runtime_error("Could not write owned texture fixture.");
		}
	};
	inline std::array<std::uint8_t,16> SolidBlock(Pixel color) {
		// Bundled UASTC mode 8 has five-bit Huffman code 0x17 followed by
		// four eight-bit solid RGBA values. All remaining legal hints are zero.
		std::array<std::uint8_t,16> block{}; unsigned bit=0;
		const auto append=[&](std::uint32_t value,unsigned count) { for(unsigned i=0;i<count;++i,++bit) block[bit/8]|=static_cast<std::uint8_t>(((value>>i)&1U)<<(bit%8)); };
		append(0x17,5); for(const auto value:color) append(value,8); return block;
	}
	inline Image Make(unsigned width=4,unsigned height=4,unsigned mips=1,ColorAt color=[](unsigned,unsigned,unsigned){return Red;},std::uint32_t format=37,bool srgb=false,bool optional=false) {
		if(width==0 || height==0 || width>65535 || height>65535 || mips==0 || mips>std::bit_width(std::max(width,height))) throw std::runtime_error("Authored texture fixture dimensions/mips must be legal.");
		Image image; TETFHeader h{}; h.magic=kMagic; h.version=kVersion; h.headerSize=sizeof(h); h.alignment=1; h.chunkCount=optional?3:2; h.tocOffset=sizeof(h); h.flags=srgb?1:0;
		const std::uint64_t payloadBase=sizeof(h)+std::uint64_t(h.chunkCount)*sizeof(TETFChunkEntry);
		TETFChunkEntry desc{kChunkDESC,0,payloadBase,sizeof(TETFDesc)+std::uint64_t(mips)*sizeof(TETFLevelEntry)};
		TETFChunkEntry uast{kChunkUAST,0,payloadBase+desc.size,0};
		std::vector<std::array<std::uint8_t,16>> blocks; std::vector<TETFLevelEntry> levels;
		for(unsigned mip=0;mip<mips;++mip) {
			const unsigned w=std::max(1U,width>>mip),hgt=std::max(1U,height>>mip); const auto begin=blocks.size();
			// Expected pixels come directly from authored coordinates, never the
			// production transcoder or its block-to-row scatter implementation.
			std::vector<Pixel> pixels; pixels.reserve(std::size_t(w)*hgt); for(unsigned y=0;y<hgt;++y) for(unsigned x=0;x<w;++x) pixels.push_back(color(mip,x,y)); image.expected.push_back(std::move(pixels));
			for(unsigned by=0;by<(hgt+3)/4;++by) for(unsigned bx=0;bx<(w+3)/4;++bx) blocks.push_back(SolidBlock(color(mip,bx*4,by*4)));
			levels.push_back({std::uint64_t(begin)*16,std::uint64_t(blocks.size()-begin)*16});
		}
		uast.size=std::uint64_t(blocks.size())*16; TETFChunkEntry meta{kChunkMETA,0,uast.offset+uast.size,4}; h.totalSize=uast.offset+uast.size+(optional?4:0); image.bytes.resize(static_cast<std::size_t>(h.totalSize));
		image.Write(0,h); image.SetChunk(0,desc); image.SetChunk(1,uast); if(optional) image.SetChunk(2,meta);
		TETFDesc d{}; d.width=static_cast<std::uint16_t>(width); d.height=static_cast<std::uint16_t>(height); d.depth=1; d.layers=1; d.faces=1; d.mipCount=static_cast<std::uint8_t>(mips); d.blockWidth=4; d.blockHeight=4; d.blockBytes=16; d.payloadChunkIndex=1; d.userMetadataChunk=optional?2:TETF_INVALID_CHUNK_INDEX; d.thumbnailChunk=TETF_INVALID_CHUNK_INDEX; d.vkFormat=format; d.semantic=srgb?1:2; image.SetDesc(d);
		for(unsigned i=0;i<mips;++i) image.Write(desc.offset+sizeof(d)+std::uint64_t(i)*sizeof(TETFLevelEntry),levels[i]);
		for(std::size_t i=0;i<blocks.size();++i) std::memcpy(image.bytes.data()+static_cast<std::size_t>(uast.offset)+i*16,blocks[i].data(),16);
		image.Checksum(); return image;
	}
	inline Image Witness() { return Make(8,4,1,[](unsigned,unsigned x,unsigned){return x<4?Red:Blue;}); }
	inline Image Quadrants(unsigned width=8,unsigned height=8) { return Make(width,height,1,[](unsigned,unsigned x,unsigned y){return y<4?(x<4?Red:Blue):(x<4?Green:Yellow);}); }
}
