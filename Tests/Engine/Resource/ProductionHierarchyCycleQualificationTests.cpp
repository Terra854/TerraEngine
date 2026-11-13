// Actual CPU loader hierarchy qualification. Cyclic files never enter a GPU
// packet: the existing bounded production loader must reject before publication.
#include "TestHarness.h"
#include "Resource/VirtualGeometryCodec.h"
#include "Resource/ResourceManager.h"
#include "Utilities/PlatformUtils.h"
#include "Editor/AssetCompilers/VirtualGeometryProductionBlocks.h"
#include "Tests/Editor/TestSha256.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
namespace TerraEngine::Tests {
namespace {
	using namespace Resource;
	std::uint64_t BaselineSkeletonFingerprint(TestContext& context, const std::vector<std::uint8_t>& bytes) {
		// Read the real importer-derived identity from the hash-pinned mesh root.
		const auto valid=ValidateTemfProductionFile(bytes,{}); context.Expect(valid.IsComplete(),"Validate the pinned identity source first.");
		std::uint64_t fingerprint=0U; unsigned roots=0U;
		for(const auto& d:valid.directory.blocks) if(d.streamKind==static_cast<std::uint16_t>(TemfStreamKind::Metadata) && d.attributeMask==kTemfMetadataDeformableRoot) {
			context.Expect(d.decodedBytes==sizeof(TemfDeformableMetadata),"Read only one bounded root record.");
			const auto decoded=DecodeTemfProductionBlock({d,std::span(bytes).subspan(static_cast<std::size_t>(d.encodedPayloadOffset),static_cast<std::size_t>(d.encodedBytes)),valid.directory.quantizationDomains,kVirtualGeometryCodecCapabilityMeshoptimizer},{});
			context.Expect(decoded.IsComplete() && decoded.bytes.size()==sizeof(TemfDeformableMetadata),"Decode only the valid pinned root.");
			TemfDeformableMetadata root{};std::memcpy(&root,decoded.bytes.data(),sizeof(root));
			context.Expect(root.sourceSkeletonJointCount==1U && root.skeletonCompatibilityFingerprint!=0U,"The real companion has exactly one identified joint."); fingerprint=root.skeletonCompatibilityFingerprint; ++roots;
		}
		context.Expect(roots==1U,"Pinned fixture contains one canonical deformable root."); return fingerprint;
	}
	std::vector<std::uint8_t> BaselineBytes(TestContext& context, std::filesystem::path& file) {
		file = PlatformUtils::GetExecutableDirectory() / "TerraEngineProductionShapeFixture.temf";
		std::ifstream in(file,std::ios::binary|std::ios::ate); const auto length = in ? in.tellg() : std::streampos(-1);
		context.Expect(length > 0 && length <= 4*1024*1024, "Install the bounded generated canonical fixture beside this runner.");
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length)); in.seekg(0); in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
		context.Expect(in.good(), "The complete pinned baseline fixture must be readable.");
		std::ifstream manifest(file.string()+".sha256"); std::string expected; manifest >> expected;
		context.Expect(expected.size() == 64U && TerraEngine::EditorTests::ComputeTestSha256(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())) == expected,
			"The generated fixture hash must match its installed build receipt."); return bytes;
	}
	// Reconstruct real valid streams with the production writer. Only hierarchy
	// links change; no copied loader or test-defined semantic validator is used.
	std::vector<std::uint8_t> HierarchyVariant(TestContext& context,const std::vector<std::uint8_t>& bytes,unsigned cycleSize) {
		TemfProductionValidationLimits limits;limits.maximumDecodedBlockBytes=256U*1024U;limits.maximumBlockCount=256U;
		const auto valid=ValidateTemfProductionFile(bytes,limits);
		context.Expect(valid.IsComplete() && (valid.directory.header.representationFlags&kTemfRepresentationVirtualGeometry)!=0U,"The installed real fixture must contain a valid virtual hierarchy.");
		context.Expect(valid.directory.blocks.size()<=256U,"Bound block reconstruction before allocating scratch streams.");
		std::vector<std::vector<std::uint8_t>> storage;storage.reserve(valid.directory.blocks.size());
		std::uint64_t totalDecoded=0U;std::uint32_t clusters=0U;
		for(const auto& d:valid.directory.blocks) {
			context.Expect(d.decodedBytes<=8U*1024U*1024U-totalDecoded,"Bound total valid-source decoding to eight MiB.");totalDecoded+=d.decodedBytes;
			// These are original validated descriptors, never malicious codec shapes.
			const auto decoded=DecodeTemfProductionBlock({d,std::span(bytes).subspan(static_cast<std::size_t>(d.encodedPayloadOffset),static_cast<std::size_t>(d.encodedBytes)),valid.directory.quantizationDomains,kVirtualGeometryCodecCapabilityMeshoptimizer},{});
			context.Expect(decoded.IsComplete(),"Decode only the unmodified hash-pinned source.");storage.push_back(decoded.bytes);
			if(d.streamKind!=static_cast<std::uint16_t>(TemfStreamKind::Metadata) || d.attributeMask!=kTemfMetadataVirtualClusters) continue;
			context.Expect(d.decodedStride==sizeof(VirtualGeometryClusterRecord) && storage.back().size()%sizeof(VirtualGeometryClusterRecord)==0U,"Cluster metadata is a complete typed stream.");
			for(std::size_t offset=0U;offset<storage.back().size();offset+=sizeof(VirtualGeometryClusterRecord)) {
				context.Expect(clusters<4096U,"Bound the existing CPU parent-walk qualification to4096clusters.");
				VirtualGeometryClusterRecord cluster{};std::memcpy(&cluster,storage.back().data()+offset,sizeof(cluster));
				context.Expect(cluster.clusterId==clusters,"Preserve the production sequential cluster identity contract.");
				// Remove original reciprocal references in the common acyclic control.
				// Every other bounds/page/group/material/fallback field stays unchanged.
				cluster.parentClusterId=kVirtualGeometryInvalidIndex;cluster.firstChildClusterId=kVirtualGeometryInvalidIndex;cluster.childCount=0U;
				if(clusters==0U && cycleSize==1U) {cluster.parentClusterId=0U;cluster.firstChildClusterId=0U;cluster.childCount=1U;}
				if(clusters<2U && cycleSize==2U) {const auto other=1U-clusters;cluster.parentClusterId=other;cluster.firstChildClusterId=other;cluster.childCount=1U;}
				std::memcpy(storage.back().data()+offset,&cluster,sizeof(cluster));++clusters;
			}
		}
		context.Expect(clusters>=2U,"The canonical fixture must provide at least two clusters; do not silently skip cyclic qualification.");
		std::vector<TerraEngine::Editor::TemfProductionStreamInput> streams;streams.reserve(storage.size());
		for(std::size_t i=0U;i<storage.size();++i) {
			const auto& d=valid.directory.blocks[i];context.Expect(d.decodedStride>0U && storage[i].size()%d.decodedStride==0U,"Preserve complete element boundaries.");
			const bool blob=d.streamKind==static_cast<std::uint16_t>(TemfStreamKind::ProceduralUpdate) && d.attributeMask==kTemfMetadataDeformableDemandedPageBlobs;
			streams.push_back({static_cast<TemfStreamKind>(d.streamKind),TemfCodecProfile::ExpandedProduction,d.attributeMask,d.quantizationDomainId,static_cast<std::uint32_t>(storage[i].size()/d.decodedStride),d.decodedStride,storage[i],blob?d.logicalPageId:kTemfDirectResidentPageId});
		}
		const auto& h=valid.directory.header;TerraEngine::Editor::TemfProductionBuildRequest request;
		request.representationFlags=h.representationFlags;request.logicalPageSizeBytes=h.logicalPageSizeBytes;request.assetIdentity=h.assetIdentity;request.sourceContentHash=h.sourceContentHash;
		request.buildRecipeHash=h.buildRecipeHash;request.toolIdentity=h.toolIdentity;request.generationIdentity=h.generationIdentity;request.selectedQuantizationProfileId=h.selectedQuantizationProfileId;
		request.quantizationDomains=valid.directory.quantizationDomains;request.streams=streams;
		auto rebuilt=TerraEngine::Editor::BuildTemfProductionBlocks(request);
		context.Expect(rebuilt.IsComplete(),"Actual production writer must reconstruct the bounded variant: "+rebuilt.error);
		context.Expect(ValidateTemfProductionFile(rebuilt.bytes,{}).IsComplete(),"Directory/payload integrity must pass BEFORE hierarchy rejection is tested.");return std::move(rebuilt.bytes);
	}
	struct TemporaryRoot {
		std::filesystem::path path;
		TemporaryRoot() {
			path=std::filesystem::temp_directory_path()/("TerraHierarchyCycle-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			if(!std::filesystem::create_directory(path))throw std::runtime_error("Cannot reserve owned hierarchy qualification staging.");
		}
		~TemporaryRoot(){std::error_code error;std::filesystem::remove_all(path,error);}
	};
	std::filesystem::path WriteVariant(TestContext& context,const std::filesystem::path& root,const char* name,const std::vector<std::uint8_t>& bytes) {
		const auto file=root/name;std::ofstream stream(file,std::ios::binary);stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
		context.Expect(stream.good(),"Persist all production-writer output before the actual loader reads it.");return file;
	}
	void Qualify(TestContext& context,unsigned cycleSize) {
		std::filesystem::path baseline;const auto bytes=BaselineBytes(context,baseline);const auto fingerprint=BaselineSkeletonFingerprint(context,bytes);
		auto& resources=ResourceManager::Instance();resources.ClearCaches();struct Reset{~Reset(){ResourceManager::Instance().ClearCaches();}} reset;
		TemporaryRoot root;
		context.Expect(resources.ValidateDeformableMeshFile(baseline,1U,fingerprint).Passed(),"The original hash-pinned canonical mesh must be accepted.");
		const auto control=WriteVariant(context,root.path,"acyclic.temf",HierarchyVariant(context,bytes,0U));
		context.Expect(resources.ValidateDeformableMeshFile(control,1U,fingerprint).Passed(),"Acyclic normalization must pass the actual semantic loader; stop if another invariant prevents qualification.");
		const auto controlId=resources.LoadMesh(control);
		context.Expect(controlId!=TerraEngine::EMPTY_UUID,"The actual public LoadMesh path must publish the valid acyclic control.");
		if(cycleSize==0U)return;
		const auto malformed=WriteVariant(context,root.path,cycleSize==1U?"self-cycle.temf":"two-cycle.temf",HierarchyVariant(context,bytes,cycleSize));
		context.Expect(!resources.ValidateDeformableMeshFile(malformed,1U,fingerprint).Passed(),"The actual semantic loader must reject reciprocal in-range cluster cycles.");
		context.Expect(resources.LoadMesh(malformed)==TerraEngine::EMPTY_UUID,"Cyclic mesh rejection must occur before public resource publication.");
	}
}
void RegisterProductionHierarchyCycleQualificationTests(TestRegistry& registry) {
	registry.Add("Resource.ProductionHierarchy.LoaderAcceptsAcyclicControl",[](TestContext& context){Qualify(context,0U);});
	registry.Add("Resource.ProductionHierarchy.LoaderRejectsSelfCycle",[](TestContext& context){Qualify(context,1U);});
	registry.Add("Resource.ProductionHierarchy.LoaderRejectsTwoNodeCycle",[](TestContext& context){Qualify(context,2U);});
}
}
