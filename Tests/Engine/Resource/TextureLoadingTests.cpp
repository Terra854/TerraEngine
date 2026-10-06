// Production loading and independent inspection share fresh owned wire corpora.
#include "TestHarness.h"
#include "TextureTestFixtures.h"
#include "Resource/ResourceManager.h"
#include "Runtime/ArtifactValidation.h"
#include "ThreadPool/ThreadPool.h"
#include <chrono>
#include <future>

namespace TerraEngine::Tests {
	namespace {
		using namespace TextureFixtures;
		bool Inspect(const Image& image) { return Runtime::ValidateArtifact({image.bytes,Runtime::ArtifactKind::Texture,Runtime::NativeImageTarget::Unsupported,"Textures/fresh.tetf"}).IsValid(); }
		class Session {
		public:
			Correctness::DisposableFixturePath owned;
			ResourceManager& resources=ResourceManager::Instance();
			Session() : previousRoot(resources.GetAssetRoot()) {
				ThreadPool::instance().shutdown(); resources.BeginRequestSession(); resources.ClearCaches(); resources.SetAssetRoot(owned.Root()); ThreadPool::instance().initialize();
			}
			~Session() { ThreadPool::instance().shutdown(); resources.ClearCaches(); resources.SetAssetRoot(previousRoot); resources.BeginRequestSession(); }
			std::filesystem::path Save(const Image& image) { const auto path=owned.Child("texture-"+std::to_string(next++)+".tetf"); image.Save(path); return path; }
		private:
			std::filesystem::path previousRoot;
			unsigned next=0;
		};
		void Rejected(TestContext& context,Session& session,Image image) {
			image.Checksum(); const auto path=session.Save(image); const bool inspected=Inspect(image); const auto id=session.resources.LoadTexture(path);
			context.Expect(!inspected && id==TerraEngine::EMPTY_UUID,"Inspector and loader must independently reject malformed wire metadata.");
		}
		void Positive(TestContext& context,Session& session,const Image& image,bool async=false) {
			context.Expect(Inspect(image),"Fresh legal codec bytes must pass structural inspection."); const auto path=session.Save(image);
			TerraEngine::UUID id;
			if(async) { auto future=session.resources.LoadTextureAsync(path); context.Expect(future.wait_for(Correctness::kDefaultTimeout)==std::future_status::ready,"Async texture request must become terminal."); id=future.get(); }
			else id=session.resources.LoadTexture(path);
			context.Expect(id!=TerraEngine::EMPTY_UUID && session.resources.GetTexture(id)!=nullptr,"Legal texture must publish through normal resource loading.");
		}
		void ExactPixels(TestContext& context,Session& session,const Image& image) {
			const auto id=session.resources.LoadTexture(session.Save(image)); context.Expect(id!=TerraEngine::EMPTY_UUID,"Authored RGBA texture must publish."); const auto* texture=session.resources.GetTexture(id); context.Expect(texture!=nullptr,"Published RGBA payload must be accessible.");
			context.Expect(texture->descriptor.blockWidth==1 && texture->descriptor.blockHeight==1 && texture->descriptor.blockBytes==4,"RGBA descriptor must describe one four-byte pixel.");
			context.Expect(texture->descriptor.vkFormat==((image.Header().flags&1)?43U:37U),"Header color choice determines RGBA format without CPU conversion.");
			std::vector<std::uint8_t> expected; std::size_t offset=0;
			context.Expect(texture->levelTable.size()==image.expected.size(),"All authored mips must remain present.");
			for(std::size_t mip=0;mip<image.expected.size();++mip) {
				const auto bytes=image.expected[mip].size()*4; context.Expect(texture->levelTable[mip].offset==offset && texture->levelTable[mip].size==bytes,"Decoded mip spans must be exact contiguous cropped pixel bytes.");
				for(const auto& pixel:image.expected[mip]) expected.insert(expected.end(),pixel.begin(),pixel.end()); offset+=bytes;
			}
			context.Expect(texture->transcodedData==expected,"Every decoded pixel must match the independently authored row-major grid.");
		}

	}
	void RegisterTextureLoadingTests(TestRegistry& registry) {
		registry.Add("Engine.Resource.Texture.Bounds.WrappedDescriptorRejected",[](TestContext& context) {
			Session session; auto image=Make(); const auto base=image.Chunk(0).offset;
			// Four real prefix bytes make the descriptor's relative start nonzero.
			// The otherwise valid DESC length then wraps the old local addition.
			image.bytes.insert(image.bytes.begin()+static_cast<std::ptrdiff_t>(base),4,std::byte{});
			for(unsigned index=0;index<2;++index) { auto chunk=image.Chunk(index); chunk.offset+=4; image.SetChunk(index,chunk); }
			auto desc=image.Chunk(0); desc.size=std::numeric_limits<std::uint64_t>::max()-3; image.SetChunk(0,desc); Rejected(context,session,std::move(image));
		});
		registry.Add("Engine.Resource.Texture.Bounds.TocCorpusRejected",[](TestContext& context) {
			Session session;
			for(unsigned row=0;row<4;++row) { auto image=Make(); auto h=image.Header();
				if(row==0) h.tocOffset=sizeof(h)-1; else if(row==1) h.tocOffset=std::numeric_limits<std::uint64_t>::max(); else if(row==2) h.chunkCount=std::numeric_limits<std::uint32_t>::max(); else image.bytes.resize(sizeof(h)+2*sizeof(TETFChunkEntry)-1);
				image.Write(0,h); Rejected(context,session,std::move(image)); }
		});
		registry.Add("Engine.Resource.Texture.Bounds.ChunkCorpusRejected",[](TestContext& context) {
			Session session;
			for(unsigned row=0;row<4;++row) { auto image=Make(); auto chunk=image.Chunk(1);
				if(row==0) chunk.offset=image.Header().headerSize; else if(row==1) chunk.offset=std::numeric_limits<std::uint64_t>::max(); else if(row==2) chunk.size=std::numeric_limits<std::uint64_t>::max(); 
				if(row==3) image.bytes.pop_back(); image.SetChunk(1,chunk); Rejected(context,session,std::move(image)); }
		});
		registry.Add("Engine.Resource.Texture.Bounds.RequiredChunksAndMappingRejected",[](TestContext& context) {
			Session session;
			for(unsigned row=0;row<5;++row) { auto image=Make(); auto chunk=image.Chunk(row==0?0:1);
				if(row<2) chunk.id=MakeFourCC("NONE"); else if(row==2) chunk.size=0; else if(row==3) { chunk=image.Chunk(0); chunk.size=sizeof(TETFDesc); } else chunk.offset=image.bytes.size()+1;
				image.SetChunk(row==0 || row==3?0:1,chunk); Rejected(context,session,std::move(image)); }
		});
		registry.Add("Engine.Resource.Texture.Bounds.OptionalAbsentAndMalformedPresent",[](TestContext& context) {
			Session session; Positive(context,session,Make()); Positive(context,session,Make(4,4,1,[](unsigned,unsigned,unsigned){return Blue;},37,false,true));
			auto image=Make(4,4,1,[](unsigned,unsigned,unsigned){return Red;},37,false,true); auto chunk=image.Chunk(2); chunk.offset=std::numeric_limits<std::uint64_t>::max(); image.SetChunk(2,chunk); Rejected(context,session,std::move(image));
		});
		registry.Add("Engine.Resource.Texture.Bounds.LegalMipDomain",[](TestContext& context) {
			Session session; Positive(context,session,Make(8,4,2)); Positive(context,session,Make(8,4,4));
			for(unsigned row=0;row<5;++row) { auto image=row==4?Make(8,8,4):Make(); auto desc=image.Desc();
				if(row==0) desc.width=0; else if(row==1) desc.height=0; else if(row==2) desc.mipCount=0; else if(row==3) desc.mipCount=17; else { desc.width=4; desc.height=4; }
				image.SetDesc(desc); Rejected(context,session,std::move(image)); }
		});
		registry.Add("Engine.Resource.Texture.Bounds.SyncAsyncExactEndControls",[](TestContext& context) {
			Session session; auto image=Witness(); context.Expect(image.Chunk(1).offset+image.Chunk(1).size==image.bytes.size(),"Positive UAST range ends exactly at file boundary."); Positive(context,session,image); Positive(context,session,image,true);
		});
		registry.Add("Engine.Resource.Texture.Bounds.ReloadRetainsLastGoodPayload",[](TestContext& context) {
			Session session; auto image=Witness(); const auto path=session.Save(image); const auto id=session.resources.LoadTexture(path); context.Expect(id!=TerraEngine::EMPTY_UUID,"Fresh reload control must publish.");
			const auto* before=session.resources.GetTexture(id); const auto pixels=before->transcodedData; const auto descriptor=before->descriptor; const auto accounting=session.resources.BuildResidencySnapshot(0);
			auto desc=image.Chunk(0); desc.size=std::numeric_limits<std::uint64_t>::max(); image.SetChunk(0,desc); image.Checksum(); image.Save(path);
			auto reload=session.resources.ReloadTexture(id); context.Expect(reload.wait_for(Correctness::kDefaultTimeout)==std::future_status::ready,"Rejected reload must terminate."); reload.get(); const auto* after=session.resources.GetTexture(id);
			context.Expect(after && after->transcodedData==pixels && std::memcmp(&after->descriptor,&descriptor,sizeof(descriptor))==0,"Reload rejection retains all prior pixels and descriptor.");
			context.Expect(session.resources.LoadTexture(path)==id,"Resident identity survives malformed disk replacement."); const auto retained=session.resources.BuildResidencySnapshot(0);
			context.Expect(accounting.residentAssets.size()==retained.residentAssets.size(),"Rejected reload preserves resident accounting cardinality.");
			for(const auto& entry:accounting.residentAssets) if(entry.assetId==id) { const auto found=std::find_if(retained.residentAssets.begin(),retained.residentAssets.end(),[&](const auto& value){return value.assetId==id;}); context.Expect(found!=retained.residentAssets.end() && found->estimatedCpuBytes==entry.estimatedCpuBytes && found->estimatedGpuBytes==entry.estimatedGpuBytes,"Rejected reload preserves accounted resource bytes."); }
		});
		registry.Add("Engine.Resource.Texture.Rgba.HorizontalWitnessEveryPixel",[](TestContext& context) { Session session; ExactPixels(context,session,Witness()); });
		registry.Add("Engine.Resource.Texture.Rgba.QuadrantsAndRectangularGrids",[](TestContext& context) {
			Session session; for(const auto dimensions:std::array<std::pair<unsigned,unsigned>,3>{{{8,8},{12,8},{8,12}}}) ExactPixels(context,session,Quadrants(dimensions.first,dimensions.second));
		});
		registry.Add("Engine.Resource.Texture.Rgba.SmallAndPartialEdges",[](TestContext& context) {
			Session session; for(const unsigned side:{1U,2U,3U,4U,5U,8U}) ExactPixels(context,session,Quadrants(side,side)); ExactPixels(context,session,Quadrants(5,3)); ExactPixels(context,session,Quadrants(3,5));
		});
		registry.Add("Engine.Resource.Texture.Rgba.MipsAlphaAndColorSelection",[](TestContext& context) {
			Session session; const std::array<Pixel,4> colors{{{31,63,95,127},{96,128,160,64},{180,40,90,200},{17,33,65,0}}};
			for(const bool srgb:{false,true}) { ExactPixels(context,session,Make(8,4,4,[&](unsigned mip,unsigned,unsigned){return colors[mip];},srgb?43:37,srgb)); ExactPixels(context,session,Make(8,4,2,[&](unsigned mip,unsigned,unsigned){return colors[mip];},srgb?43:37,srgb)); }
		});
		registry.Add("Engine.Resource.Texture.Rgba.CompressedControlsAndDecoderFailure",[](TestContext& context) {
			Session session;
			for(const std::uint32_t format:{145U,157U}) for(const bool srgb:{false,true}) {
				const auto image=Make(5,3,2,[](unsigned,unsigned x,unsigned){return x<4?Red:Blue;},format,srgb); const auto id=session.resources.LoadTexture(session.Save(image)); context.Expect(id!=TerraEngine::EMPTY_UUID,"Legal compressed control must publish."); const auto* texture=session.resources.GetTexture(id);
				context.Expect(texture && texture->descriptor.vkFormat==format+(srgb?1:0) && texture->descriptor.blockWidth==4 && texture->descriptor.blockHeight==4 && texture->descriptor.blockBytes==16,"BC7/ASTC retain selected format and four-by-four sixteen-byte blocks.");
				context.Expect(texture->transcodedData.size()==48 && texture->levelTable[0].offset==0 && texture->levelTable[0].size==32 && texture->levelTable[1].offset==32 && texture->levelTable[1].size==16,"Compressed control mip spans retain tight block layout.");
			}
			const auto before=session.resources.BuildResidencySnapshot(0);
			for(const std::uint32_t format:{37U,145U,157U}) {
				auto broken=Make(8,4,1,[](unsigned,unsigned x,unsigned){return x<4?Red:Blue;},format); broken.bytes[static_cast<std::size_t>(broken.Chunk(1).offset)+16]=std::byte{0x45}; broken.Checksum();
				context.Expect(Inspect(broken),"Structural gate must not pretend to transcode decoder-invalid blocks."); const auto path=session.Save(broken); const auto id=session.resources.LoadTexture(path); context.Expect(id==TerraEngine::EMPTY_UUID,"Failure after an earlier decoded block must reject the entire candidate.");
				auto future=session.resources.LoadTextureAsync(path); context.Expect(future.wait_for(Correctness::kDefaultTimeout)==std::future_status::ready && future.get()==TerraEngine::EMPTY_UUID,"Async decoder failure must be terminal without partial publication.");
			}
			context.Expect(before.residentAssets.size()==session.resources.BuildResidencySnapshot(0).residentAssets.size(),"Decoder failure cannot add a partial resident texture.");
		});

	}
}
