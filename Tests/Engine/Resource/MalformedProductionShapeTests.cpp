// Safe validator-first shape rejection. Never invoke vendor decode for malformed
// shapes: the pre-fix decoder can assert or write beyond its bounded destination.
#include "TestHarness.h"
#include "Resource/VirtualGeometryCodecReviewAccess.h"
#include "Resource/VirtualGeometryCodec.h"
#include "Resource/ResourceManager.h"
#include "Utilities/PlatformUtils.h"
#include "Editor/AssetCompilers/VirtualGeometryProductionBlocks.h"
#include "Tests/Editor/TestSha256.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace TerraEngine::Tests {
namespace {
	using namespace Resource;
	std::vector<std::uint8_t> BuildSmall(TestContext& context, TemfCodecProfile profile, bool topology, bool expandedByteStride = false, bool expandedLargeStride = false) {
		const std::array<std::uint32_t,3> values{0U,1U,2U};
		const std::array<std::uint8_t,3> expanded{0U,1U,2U};
		std::vector<std::uint8_t> payload;
		if(expandedLargeStride) payload.assign(260U,0U);
		else if(expandedByteStride) payload.assign(expanded.begin(), expanded.end());
		else { payload.resize(sizeof(values)); std::memcpy(payload.data(), values.data(), payload.size()); }
		TemfQuantizationDomain domain{}; domain.domainVersion = 1U; domain.domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
		domain.streamKind = static_cast<std::uint16_t>(topology ? TemfStreamKind::Topology : TemfStreamKind::Position); domain.componentCount = 1U; domain.domainHash = 1U;
		TerraEngine::Editor::TemfProductionStreamInput stream{topology ? TemfStreamKind::Topology : TemfStreamKind::Position, profile,0U,0U,expandedLargeStride ? 1U : 3U,expandedLargeStride ? 260U : expandedByteStride ? 1U : 4U,payload};
		std::vector<TemfQuantizationDomain> domains{domain};
		std::vector<TerraEngine::Editor::TemfProductionStreamInput> streams{stream};
		std::vector<std::uint8_t> remapBytes;
		if(topology) {
			// Real topology always depends on a stable vertex identity stream. Keep
			// topology first so every controlled shape mutation still targets it.
			remapBytes.resize(sizeof(values));std::memcpy(remapBytes.data(),values.data(),remapBytes.size());
			auto remapDomain=domain;remapDomain.domainId=1U;remapDomain.streamKind=static_cast<std::uint16_t>(TemfStreamKind::VertexRemapStableIdentity);remapDomain.domainHash=2U;domains.push_back(remapDomain);
			streams.push_back({TemfStreamKind::VertexRemapStableIdentity,TemfCodecProfile::ExpandedProduction,0U,1U,3U,4U,remapBytes});
		}
		TerraEngine::Editor::TemfProductionBuildRequest request; request.logicalPageSizeBytes = kTemfPageSize32KiB; request.quantizationDomains = domains; request.streams = streams;
		auto result = TerraEngine::Editor::BuildTemfProductionBlocks(request);
		context.Expect(result.IsComplete(), "The actual production block writer must construct the tiny valid baseline: " + result.error);
		context.Expect(ValidateTemfProductionFile(result.bytes, {}).IsComplete(), "Baseline hashes/tables/profile must pass before a shape-only mutation."); return std::move(result.bytes);
	}
	std::vector<std::uint8_t> MutateShape(std::vector<std::uint8_t> bytes, std::uint32_t count, std::uint32_t stride) {
		TemfProductionHeader header{}; std::memcpy(&header,bytes.data(),sizeof(header));
		TemfBlockDescriptor block{}; std::memcpy(&block,bytes.data()+header.blockTableOffset,sizeof(block));
		TemfPageDescriptor page{}; std::memcpy(&page,bytes.data()+header.pageTableOffset,sizeof(page));
		// Preserve the untouched remap contribution on the shared physical page.
		const auto previousDecodedBytes=block.decodedBytes;
		block.decodedElementCount = count; block.decodedStride = stride; block.decodedBytes = static_cast<std::uint64_t>(count) * stride;
		page.decodedBytes = page.decodedBytes - previousDecodedBytes + block.decodedBytes;
		std::memcpy(bytes.data()+header.blockTableOffset,&block,sizeof(block)); std::memcpy(bytes.data()+header.pageTableOffset,&page,sizeof(page));
		header.directoryHash = CalculateVirtualGeometryCompactHash(std::span<const std::uint8_t>(bytes).subspan(sizeof(header),static_cast<std::size_t>(header.directoryBytes)));
		std::memcpy(bytes.data(),&header,sizeof(header)); return bytes;
	}
	void ExpectShapeRejected(TestContext& context, const std::vector<std::uint8_t>& bytes) {
		TemfProductionValidationLimits limits; limits.maximumDecodedBlockBytes = 1024U;
		const auto result = ValidateTemfProductionFile(bytes,limits);
		context.Expect(!result.IsComplete() && (result.code == TemfProductionValidationCode::ProfileMismatch || result.code == TemfProductionValidationCode::LimitExceeded),
			"The file boundary must reject a codec-specific shape before unsafe decoder allocation/indexing.");
	}
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
	std::vector<std::uint8_t> ExtraEntry(TestContext& context, const std::vector<std::uint8_t>& bytes) {
		TemfProductionValidationLimits limits; limits.maximumDecodedBlockBytes=256U*1024U; limits.maximumBlockCount=256U;
		const auto validated = ValidateTemfProductionFile(bytes,limits); context.Expect(validated.IsComplete(), "Only a valid canonical source may be mutated.");
		context.Expect(validated.directory.blocks.size() <= 256U, "Keep reconstruction bounded to the known small dense fixture.");
		std::vector<std::vector<std::uint8_t>> storage; std::vector<TemfBlockDescriptor> descriptors;
		storage.reserve(validated.directory.blocks.size()); descriptors.reserve(validated.directory.blocks.size());
		std::uint64_t totalDecoded=0;
		for(const auto& descriptor : validated.directory.blocks) {
			context.Expect(descriptor.decodedBytes <= 8U*1024U*1024U-totalDecoded,"Keep aggregate fixture decode below eight MiB.");totalDecoded+=descriptor.decodedBytes;
			const auto decoded = DecodeTemfProductionBlock({descriptor,std::span(bytes).subspan(static_cast<std::size_t>(descriptor.encodedPayloadOffset),static_cast<std::size_t>(descriptor.encodedBytes)),validated.directory.quantizationDomains,kVirtualGeometryCodecCapabilityMeshoptimizer},{});
			context.Expect(decoded.IsComplete(), "Decode only the unmodified writer-generated valid source."); storage.push_back(decoded.bytes); descriptors.push_back(descriptor);
		}
		unsigned roots = 0, directories = 0, entries = 0;
		for(std::size_t i = 0; i < storage.size(); ++i) {
			auto& part = storage[i]; const auto& descriptor = descriptors[i]; if(descriptor.streamKind != static_cast<std::uint16_t>(TemfStreamKind::Metadata)) continue;
			if(descriptor.attributeMask == kTemfMetadataDeformableRoot) { context.Expect(part.size()==sizeof(TemfDeformableMetadata), "Root is one bounded record."); TemfDeformableMetadata root{}; std::memcpy(&root,part.data(),sizeof(root)); ++root.demandedPageCount; std::memcpy(part.data(),&root,sizeof(root)); ++roots; }
			if(descriptor.attributeMask == kTemfMetadataDeformableDemandedPageDirectory) { context.Expect(part.size()==sizeof(TemfDeformableDemandedPageDirectory), "Directory is one bounded record."); TemfDeformableDemandedPageDirectory dir{}; std::memcpy(&dir,part.data(),sizeof(dir)); context.Expect(dir.entryCount==dir.blobCount && dir.entryCount>0, "Original fixture has matching nonzero dimensions."); ++dir.entryCount; std::memcpy(part.data(),&dir,sizeof(dir)); ++directories; }
			if(descriptor.attributeMask == kTemfMetadataDeformableDemandedPageEntries) {
				context.Expect(!part.empty() && part.size()%sizeof(TemfDeformableDemandedPageEntry)==0U,"Entries are complete typed records.");
				TemfDeformableDemandedPageEntry added{}; std::uint32_t maximum = 0;
				for(std::size_t offset=0; offset<part.size(); offset+=sizeof(added)) { std::memcpy(&added,part.data()+offset,sizeof(added)); maximum=std::max(maximum,added.pageId); }
				context.Expect(maximum < kVirtualGeometryInvalidIndex-1U,"Reserve one non-sentinel unique entry identity.");
				added.pageId=maximum+1U; added.parentPageId=kVirtualGeometryInvalidIndex; added.fallbackPageId=added.pageId; added.dependencyOffset=0U; added.dependencyCount=0U;
				added.blobId=part.size()/sizeof(added)+1U; const auto old=part.size(); part.resize(old+sizeof(added)); std::memcpy(part.data()+old,&added,sizeof(added)); ++entries;
			}
		}
		context.Expect(roots==1U && directories==1U && entries==1U,"The fixed fixture has one unsplit root/directory/entry metadata stream.");
		std::vector<TerraEngine::Editor::TemfProductionStreamInput> streams;
		for(std::size_t i=0;i<storage.size();++i) { const auto& d=descriptors[i]; context.Expect(d.decodedStride>0U && storage[i].size()%d.decodedStride==0U,"Rebuilt streams retain complete element boundaries.");
			const bool blob=d.streamKind==static_cast<std::uint16_t>(TemfStreamKind::ProceduralUpdate) && d.attributeMask==kTemfMetadataDeformableDemandedPageBlobs;
			streams.push_back({static_cast<TemfStreamKind>(d.streamKind),TemfCodecProfile::ExpandedProduction,d.attributeMask,d.quantizationDomainId,static_cast<std::uint32_t>(storage[i].size()/d.decodedStride),d.decodedStride,storage[i],blob ? d.logicalPageId : kTemfDirectResidentPageId}); }
		const auto& h=validated.directory.header; TerraEngine::Editor::TemfProductionBuildRequest request;
		request.representationFlags=h.representationFlags; request.logicalPageSizeBytes=h.logicalPageSizeBytes; request.assetIdentity=h.assetIdentity; request.sourceContentHash=h.sourceContentHash;
		request.buildRecipeHash=h.buildRecipeHash; request.toolIdentity=h.toolIdentity; request.generationIdentity=h.generationIdentity; request.selectedQuantizationProfileId=h.selectedQuantizationProfileId; request.quantizationDomains=validated.directory.quantizationDomains; request.streams=streams;
		auto rebuilt=TerraEngine::Editor::BuildTemfProductionBlocks(request); context.Expect(rebuilt.IsComplete(),"Actual production writer must rehash/reassemble controlled metadata: "+rebuilt.error);
		context.Expect(ValidateTemfProductionFile(rebuilt.bytes,{}).IsComplete(),"The mismatched file must pass hashes/tables before semantic shape rejection."); return std::move(rebuilt.bytes);
	}
}
void RegisterMalformedProductionShapeTests(TestRegistry& registry) {
	registry.Add("Resource.MalformedProductionShape.ValidCodecAndExpandedControls",[](TestContext& context) {
		for(auto profile:{TemfCodecProfile::MeshoptimizerProductionStatic,TemfCodecProfile::MeshoptimizerProductionUpdateable,TemfCodecProfile::ExpandedProduction}) for(bool topology:{false,true}) {
			const auto bytes=BuildSmall(context,profile,topology,profile==TemfCodecProfile::ExpandedProduction && topology); const auto valid=ValidateTemfProductionFile(bytes,{}); const auto& d=valid.directory.blocks.front();
			context.Expect(DecodeTemfProductionBlock({d,std::span(bytes).subspan(static_cast<std::size_t>(d.encodedPayloadOffset),static_cast<std::size_t>(d.encodedBytes)),valid.directory.quantizationDomains,kVirtualGeometryCodecCapabilityMeshoptimizer},{}).IsComplete(),"Only valid shapes enter the actual vendor decoder; expanded byte topology remains supported.");
		}
		// Large ExpandedProduction strides are legal byte copies, not the vendor
		// vertex codec's fixed256-byte internal storage contract.
		const auto large=BuildSmall(context,TemfCodecProfile::ExpandedProduction,false,false,true);const auto checked=ValidateTemfProductionFile(large,{});const auto& d=checked.directory.blocks.front();
		const auto decoded=DecodeTemfProductionBlock({d,std::span(large).subspan(static_cast<std::size_t>(d.encodedPayloadOffset),static_cast<std::size_t>(d.encodedBytes)),checked.directory.quantizationDomains,0U},{});
		context.Expect(decoded.IsComplete() && decoded.bytes.size()==260U,"Do not apply meshoptimizer-only stride constraints to ExpandedProduction.");
	});
	registry.Add("Resource.MalformedProductionShape.RejectsMeshoptimizerTopologyStrideOne",[](TestContext& context) { for(auto profile:{TemfCodecProfile::MeshoptimizerProductionStatic,TemfCodecProfile::MeshoptimizerProductionUpdateable}) ExpectShapeRejected(context,MutateShape(BuildSmall(context,profile,true),3U,1U)); });
	registry.Add("Resource.MalformedProductionShape.RejectsNonTriangleIndexCount",[](TestContext& context) { for(auto profile:{TemfCodecProfile::MeshoptimizerProductionStatic,TemfCodecProfile::MeshoptimizerProductionUpdateable}) ExpectShapeRejected(context,MutateShape(BuildSmall(context,profile,true),4U,4U)); });
	registry.Add("Resource.MalformedProductionShape.RejectsUnsafeVertexStrides",[](TestContext& context) { for(auto profile:{TemfCodecProfile::MeshoptimizerProductionStatic,TemfCodecProfile::MeshoptimizerProductionUpdateable}) for(unsigned stride:{2U,260U}) ExpectShapeRejected(context,MutateShape(BuildSmall(context,profile,false),1U,stride)); });
	registry.Add("Resource.MalformedProductionShape.RejectsExtraEntryWithoutUnsafeBlobIndex",[](TestContext& context) {
		std::filesystem::path baseline; const auto bytes=BaselineBytes(context,baseline); const auto fingerprint=BaselineSkeletonFingerprint(context,bytes); auto& resources=ResourceManager::Instance();
		struct Reset { ~Reset(){ResourceManager::Instance().ClearCaches();} } reset; resources.ClearCaches();
		context.Expect(resources.ValidateDeformableMeshFile(baseline,1U,fingerprint).status==Animation::AnimationStatus::Ok,"Pinned dense baseline must load through the actual semantic reader before mutation.");
		const auto malformed=ExtraEntry(context,bytes);
		const auto root=std::filesystem::temp_directory_path()/("TerraMalformedShape-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		context.Expect(std::filesystem::create_directory(root),"Reserve owned malformed fixture staging."); struct Remove {std::filesystem::path path;~Remove(){std::error_code error;std::filesystem::remove_all(path,error);}} remove{root};
		const auto output=root/"extra-entry.temf"; {std::ofstream file(output,std::ios::binary);file.write(reinterpret_cast<const char*>(malformed.data()),static_cast<std::streamsize>(malformed.size()));context.Expect(file.good(),"Persist the complete malformed file.");}
		// Entries > blobs is deliberately the SAFE reverse mismatch: the current
		// blob loop cannot overrun entries. Never run blobs > entries before repair.
		context.Expect(resources.ValidateDeformableMeshFile(output,1U,fingerprint).status!=Animation::AnimationStatus::Ok,"The semantic reader must reject unequal entry/blob/block dimensions.");
		context.Expect(resources.LoadDeformableMesh(output,1U,fingerprint)==TerraEngine::EMPTY_UUID,"A rejected malformed directory must never publish a mesh.");
	});
}
}

namespace TerraEngine::Tests {
// Separate mandatory registration keeps direct decode qualification distinct
// from the five file-validator cases; observation never substitutes rejection.
void RegisterMalformedProductionShapeDirectObserverTests(TestRegistry& registry) {
	registry.Add("Resource.MalformedProductionShape.DirectDecodeRejectsBeforeObservedAllocation",[](TestContext& context) {
		for(auto profile:{TemfCodecProfile::MeshoptimizerProductionStatic,TemfCodecProfile::MeshoptimizerProductionUpdateable}) for(unsigned shape=0;shape<3U;++shape) {
			const bool topology=shape<2U; const auto original=BuildSmall(context,profile,topology); const auto valid=Resource::ValidateTemfProductionFile(original,{});
			const auto malformed=MutateShape(original,shape==1U?4U:shape==0U?3U:1U,shape==0U?1U:shape==1U?4U:260U);
			TemfProductionHeader header{};std::memcpy(&header,malformed.data(),sizeof(header));TemfBlockDescriptor descriptor{};std::memcpy(&descriptor,malformed.data()+header.blockTableOffset,sizeof(descriptor));
			unsigned reached=0U;
			const auto stop=[](const TemfBlockDescriptor&,void* user) noexcept {++*static_cast<unsigned*>(user);return false;};
			struct RestoreObserver {Resource::Review::DecodeBoundaryObserver previous;~RestoreObserver(){Resource::Review::SetDecodeBoundaryObserver(previous);}} restore{Resource::Review::SetDecodeBoundaryObserver({stop,&reached})};
			const auto decoded=Resource::DecodeTemfProductionBlock({descriptor,std::span(malformed).subspan(static_cast<std::size_t>(descriptor.encodedPayloadOffset),static_cast<std::size_t>(descriptor.encodedBytes)),valid.directory.quantizationDomains,kVirtualGeometryCodecCapabilityMeshoptimizer},{});
			// An observer-aborted call is always RED, even though it returns no bytes.
			// Only genuine production rejection before this boundary can pass.
			context.Expect(reached==0U,"Malformed shape reached allocation/vendor boundary; observer safely prevented unsafe execution.");
			context.Expect(!decoded.IsComplete() && (decoded.code==Resource::TemfProductionValidationCode::ProfileMismatch || decoded.code==Resource::TemfProductionValidationCode::LimitExceeded),"Production must reject codec shape independently of the safety observer.");
		}
	});
}
}
