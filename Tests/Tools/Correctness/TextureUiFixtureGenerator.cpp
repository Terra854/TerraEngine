// Generate deterministic, legal texture bytes for the installed UI/GPU witness.
// Usage: TerraEngineTextureUiFixtureGenerator --output-root <absolute fresh directory>
#include "Tests/Engine/Resource/TextureTestFixtures.h"
#include <iostream>
#include <string_view>

int main(int argc,char** argv)
{
	using namespace TerraEngine::Tests::TextureFixtures;
	try {
		if(argc!=3 || std::string_view(argv[1])!="--output-root") throw std::runtime_error("Usage: --output-root <absolute fresh directory>");
		const std::filesystem::path root=argv[2];
		// A fresh caller-owned destination prevents replacing a project's assets.
		if(!root.is_absolute() || std::filesystem::is_symlink(root) || (std::filesystem::exists(root) && (!std::filesystem::is_directory(root) || !std::filesystem::is_empty(root)))) throw std::runtime_error("Output root must be an absolute fresh directory.");
		std::filesystem::create_directories(root);
		Witness().Save(root/"witness-linear.tetf");
		Quadrants().Save(root/"quadrants-linear.tetf");
		Make(5,3,1,[](unsigned,unsigned x,unsigned){return x<4?Red:Blue;}).Save(root/"edges-linear.tetf");
		Make(8,8,1,[](unsigned,unsigned x,unsigned y){return y<4?(x<4?Pixel{255,0,0,0}:Pixel{0,0,255,64}):(x<4?Pixel{0,255,0,128}:Pixel{255,255,0,255});}).Save(root/"alpha-linear.tetf");
		Make(8,4,1,[](unsigned,unsigned x,unsigned){return x<4?Pixel{64,128,192,255}:Pixel{192,96,32,128};},43,true).Save(root/"midtone-srgb.tetf");
		Make(8,4,4,[](unsigned mip,unsigned,unsigned){constexpr std::array<Pixel,4> colors{{{31,63,95,127},{96,128,160,64},{180,40,90,200},{17,33,65,0}}};return colors[mip];}).Save(root/"mips-linear.tetf");
		const auto quadrants=[](unsigned,unsigned x,unsigned y){return y<4?(x<4?Red:Blue):(x<4?Green:Yellow);};
		Make(8,8,1,quadrants,145).Save(root/"control-bc7.tetf");
		Make(8,8,1,quadrants,157).Save(root/"control-astc.tetf");
		// Requested RGBA descriptors select the ordinary uncompressed runtime
		// path, so the visible witnesses exercise row scatter rather than BC7.
		std::cout<<"Generated eight textures in "<<root<<"\n"; return 0;
	} catch(const std::exception& error) { std::cerr<<error.what()<<"\n"; return 1; }
}
