// Generic native contact/reflection scene. Only the validation executable links
// this materializer; all GPU draws still pass through ordinary indexed routing.
#include "PostProcessScreenLightingGeometry.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/VirtualGeometry/Geometry/VirtualGeometryPackedVertex.h"
#include "Renderer/RHI/Vulkan/VulkanIndexedGeometry.h"
#include "Resource/ResourceManager.h"
#include "Terrain/TerrainMaterial.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace TerraEngine::TestSupport
{
	namespace
	{
		using Quad = std::array<glm::vec3, 4>;
		void Check(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }

		std::unique_ptr<Mesh> MeshFromQuads(const std::vector<Quad>& quads)
		{
			Check(!quads.empty() && quads.size() <= 16, "Fixture mesh must fit one honest 64-vertex meshlet.");
			auto mesh = std::make_unique<Mesh>();
			auto& layout = mesh->runtimeLayout;
			std::memcpy(layout.magic, "TEMF", 4);
			layout.version = kMeshCurrentVersion;
			layout.headerSize = sizeof(MeshletRuntimeLayout);
			layout.layout.strideBytes = kMeshletPackedVertexStrideBytes;
			layout.layout.attributeCount = 4;
			layout.layout.attributes[0] = {static_cast<std::uint32_t>(AttributeSemantic::Position), static_cast<std::uint32_t>(AttributeFormat::R16G16B16Unorm), 0};
			layout.layout.attributes[1] = {static_cast<std::uint32_t>(AttributeSemantic::Normal), static_cast<std::uint32_t>(AttributeFormat::Oct12x2), 6};
			layout.layout.attributes[2] = {static_cast<std::uint32_t>(AttributeSemantic::Tangent), static_cast<std::uint32_t>(AttributeFormat::Oct10x2TangentSign), 9};
			layout.layout.attributes[3] = {static_cast<std::uint32_t>(AttributeSemantic::TexCoord0), static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm), 12};
			layout.meshletVertexRemapEncoding = static_cast<std::uint32_t>(MeshletRemapEncoding::Uint16);
			layout.meshletFlags = kMeshletHeaderFlagHasBounds;
			glm::vec3 lo = quads.front().front(), hi = lo;
			for(const auto& quad : quads) for(const auto& p : quad) { lo = glm::min(lo,p); hi = glm::max(hi,p); }
			const glm::vec3 center = (lo+hi)*0.5f;
			for(int axis = 0; axis < 3; ++axis)
			{
				mesh->localBoundsMin[axis] = lo[axis]; mesh->localBoundsMax[axis] = hi[axis];
				layout.quantization.positionOffset[axis] = center[axis];
				layout.quantization.positionScale[axis] = std::max((hi[axis]-lo[axis])*0.5f, 1e-4f);
			}
			layout.quantization.uvScale[0] = layout.quantization.uvScale[1] = 1;
			std::vector<std::uint8_t> triangles;
			for(const auto& q : quads)
			{
				const glm::vec3 tangent = glm::normalize(q[1]-q[0]);
				const glm::vec3 cross = glm::cross(q[1]-q[0],q[2]-q[0]);
				Check(glm::dot(cross,cross) > 1e-8f,"Fixture contains a degenerate authored quad.");
				const glm::vec3 n = glm::normalize(cross);
				const auto base = static_cast<std::uint8_t>(mesh->meshletVertexRemapDecoded.size());
				for(std::size_t corner = 0; corner < 4; ++corner)
				{
					std::array<float,3> p{};
					for(int axis=0;axis<3;++axis) p[axis]=(q[corner][axis]-center[axis])/layout.quantization.positionScale[axis];
					const auto packed=PackVirtualGeometryVertex(p,{n.x,n.y,n.z},{tangent.x,tangent.y,tangent.z},1,
						{corner==1 || corner==2 ? 1.f : 0.f, corner>=2 ? 1.f : 0.f},16,12,10,16);
					const auto offset=mesh->reconstructedVertexData.size();
					mesh->reconstructedVertexData.resize(offset+sizeof(packed));
					std::memcpy(mesh->reconstructedVertexData.data()+offset,&packed,sizeof(packed));
					mesh->meshletVertexRemapDecoded.push_back(static_cast<std::uint32_t>(mesh->meshletVertexRemapDecoded.size()));
				}
				for(const auto local : {0,1,2,0,2,3}) triangles.push_back(static_cast<std::uint8_t>(base+local));
			}
			const auto count=static_cast<std::uint32_t>(mesh->meshletVertexRemapDecoded.size());
			mesh->meshletRecords.push_back({0,count,0,static_cast<std::uint32_t>(triangles.size())});
			mesh->meshletSections.push_back({0,1});
			MeshletBounds bounds{}; bounds.center[0]=center.x; bounds.center[1]=center.y; bounds.center[2]=center.z;
			bounds.radius=glm::length(hi-lo)*0.5f; bounds.coneCutoff=2; mesh->meshletBounds.push_back(bounds);
			layout.meshletVertexCount=layout.meshletVertexRemapCount=count;
			layout.meshletPrimitiveIndexCount=static_cast<std::uint32_t>(triangles.size());
			layout.meshletCount=layout.meshletSectionCount=layout.meshletBoundsCount=1;
			const auto remapBytes=count*sizeof(std::uint16_t);
			mesh->storage.resize(remapBytes+triangles.size());
			for(std::uint16_t i=0;i<count;++i) std::memcpy(mesh->storage.data()+i*sizeof(i),&i,sizeof(i));
			std::memcpy(mesh->storage.data()+remapBytes,triangles.data(),triangles.size());
			mesh->meshletVertexData=mesh->reconstructedVertexData;
			mesh->meshletVertexRemapData={mesh->storage.data(),remapBytes};
			mesh->meshletPrimitiveIndexData={mesh->storage.data()+remapBytes,triangles.size()};
			mesh->hasLocalBounds=true; mesh->sourcePath="generated/native-screen-lighting-fixture";
			// All bounded streams above were constructed and range checked here.
			// This is the same generated-resident proof used by ordinary terrain;
			// no serialized loader header or GPU completion is manufactured.
			std::uint64_t content=1469598103934665603ull;
			for(const auto byte : mesh->reconstructedVertexData) { content^=byte; content*=1099511628211ull; }
			for(const auto byte : mesh->storage) { content^=byte; content*=1099511628211ull; }
			mesh->generatedDirectMeshletRoute.sourceContentIdentity=content ? content : 1;
			mesh->generatedDirectMeshletRoute.generationIdentity=1;
			mesh->generatedDirectMeshletRoute.residentPayloadBytes=mesh->storage.size()+mesh->reconstructedVertexData.size()
				+sizeof(MeshletRecord)+sizeof(MeshletSection)+sizeof(MeshletBounds);
			mesh->generatedDirectMeshletRoute.validated=true;
			return mesh;
		}
	}

	ScreenLightingGeometry CreateScreenLightingGeometry()
	{
		auto& resources=ResourceManager::Instance();
		resources.LoadDefaultResources();
		ScreenLightingGeometry result;
		const auto add=[&](const std::vector<Quad>& quads, glm::vec3 color,float metallic,float roughness,glm::vec3 emissive) {
			ScreenLightingGeometry::Surface surface;
			surface.objectId=GenerateUUID();
			const auto key="native-screen-lighting/"+ToHexString(surface.objectId);
			auto material=Terrain::CreateTerrainMaterial(resources,{color,metallic,roughness});
			for(int axis=0;axis<3;++axis) material->header.emissiveFactor[axis]=emissive[axis];
			surface.materialId=resources.RegisterMaterial(key+"/material",std::move(material));
			surface.meshId=resources.RegisterMesh(key+"/mesh",MeshFromQuads(quads));
			const auto mesh=resources.AcquireMeshSnapshot(surface.meshId);
			const auto mat=resources.AcquireMaterialSnapshot(surface.materialId);
			Check(mesh && mat && mesh->publicationGeneration && mat->publicationGeneration,"Generated ordinary resources were not published.");
			Renderer::Vulkan::IndexedCornerGeometry topology;
			std::string reason;
			Check(Renderer::Vulkan::TryBuildIndexedCornerGeometry(mesh,topology,reason),"Generated fixture failed actual indexed topology validation.");
			Check(!topology.firstIndexByMeshlet.empty() && topology.firstIndexByMeshlet.back()==quads.size()*6,"Generated fixture indexed corner count is incomplete.");
			surface.meshGeneration=mesh->publicationGeneration; surface.materialGeneration=mat->publicationGeneration;
			result.surfaces.push_back(surface);
		};
		// The floor normal points toward the camera and upward. Rays reflected
		// from it enter the visible colored wall; contact geometry is within .5m.
		add({Quad{{{-2.7f,-1.5f,-3},{2.7f,-1.5f,-3},{2.7f,0.3f,-8},{-2.7f,0.3f,-8}}}},
			{0.65f,0.65f,0.65f},0.65f,0.12f,{});
		add({Quad{{{-2.7f,0.3f,-8},{2.7f,0.3f,-8},{2.7f,1.5f,-8},{-2.7f,1.5f,-8}}}},
			{0.7f,0.08f,0.025f},0,0.6f,{0.45f,0.03f,0.01f});
		// A five-face block intersects the sloped floor near its center. Its
		// authored faces provide both depth discontinuities and bent-normal hits.
		const float x0=-.5f,x1=.5f,y0=-.82f,y1=-.12f,z0=-5.4f,z1=-4.7f;
		add({Quad{{{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}}},
			Quad{{{x1,y0,z0},{x0,y0,z0},{x0,y1,z0},{x1,y1,z0}}},
			Quad{{{x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}}},
			Quad{{{x1,y0,z1},{x1,y0,z0},{x1,y1,z0},{x1,y1,z1}}},
			Quad{{{x0,y1,z1},{x1,y1,z1},{x1,y1,z0},{x0,y1,z0}}}},
			{0.12f,0.55f,0.2f},0,0.65f,{0.02f,0.06f,0.01f});
		return result;
	}

	std::shared_ptr<const Renderer::RenderFramePublication> PublishScreenLightingGeometry(
		const ScreenLightingGeometry& geometry,Renderer::RenderViewPresentationInput input,
		std::uint64_t frame,std::uint64_t session,std::uint64_t backendGeneration)
	{
		using namespace Renderer;
		auto root=std::make_shared<RenderPersistentTopologyRoot>();
		auto dynamic=std::make_shared<RenderDynamicStateRoot>();
		auto page=std::make_shared<RenderPersistentPage>(); auto lanes=std::make_shared<RenderDynamicPage>();
		root->generation=dynamic->generation=page->generation=lanes->generation=frame;
		root->rendererSessionGeneration=dynamic->rendererSessionGeneration=session;
		RenderViewSelection selection;
		selection.generation=frame; selection.outputId=input.output.outputId;
		selection.outputKind=input.output.kind; selection.viewType=input.output.diagnosticsView;
		for(std::uint32_t i=0;i<geometry.surfaces.size();++i)
		{
			const auto& surface=geometry.surfaces[i]; RenderPersistentSlot slot;
			slot.key={i,1}; slot.objectId=surface.objectId; slot.meshId=surface.meshId; slot.materialId=surface.materialId;
			slot.meshGeneration=surface.meshGeneration; slot.materialGeneration=surface.materialGeneration;
			slot.resourceGeneration=1; slot.packetTemplateIdentity=i+1; slot.occupied=true;
			slot.routeTemplate=static_cast<std::uint32_t>(GeometryRoute::DirectMeshlet); page->slots.push_back(slot);
			RenderDynamicLane lane; lane.key=slot.key; lane.occupied=true; lane.resourceGeneration=1;
			// Vertex positions already contain the authored room coordinates, so
			// each surface's real object pose is the fixed world origin, identity
			// rotation and unit scale. Retain that same immutable canonical pose
			// in every publication; moving the camera never moves these objects.
			lane.canonicalWorldPose={};
			lane.canonicalWorldPose.transformRevision=1;
			lane.canonicalWorldPose.coordinateFrameRevision=1;
			lane.hasCanonicalWorldPose=true;
			const auto localized=BuildLocalizedRenderPoseMatrices(lane.canonicalWorldPose,input.referenceFrame);
			Check(localized.valid,"Generated static fixture canonical pose cannot be localized against its actual view.");
			// At the fixture's zero reference origin this remains exactly the
			// identity matrix used by the existing lighting image comparisons.
			lane.instance.modelMatrix=localized.currentMatrix; lane.instance.objectIdentity.x=i+1;
			lanes->lanes.push_back(lane); lanes->changedLanes.push_back(true);
			selection.visibleOrdinals.push_back(slot.key); selection.selectedLods.push_back(0);
			selection.routeSelectors.push_back(slot.routeTemplate); selection.packetTemplateIdentities.push_back(slot.packetTemplateIdentity);
			selection.tintSelectors.push_back(0); selection.editorTints.push_back(glm::vec4(0));
			RenderViewCommandLane command; command.meshDataIndex=command.textureIndex=0xffffffffu;
			command.virtualMeshletCount=1; command.taskShaderCullingEnabled=false;
			input.commandLanes.push_back(command); input.commandSelectionIndices.push_back(i);
			input.drawResourcePreparationCommandIndices.push_back(i);
		}
		root->pages.push_back(page); dynamic->pages.push_back(lanes);
		auto publication=std::make_shared<RenderFramePublication>();
		publication->frameId=publication->generation=frame; publication->rendererSessionGeneration=session;
		publication->backendResourceGeneration=backendGeneration; publication->sceneGeneration=1;
		publication->persistent=root; publication->dynamic=dynamic;
		auto backend=std::make_shared<RenderBackendWorkPublication>();
		backend->frameId=backend->animationWork.frameId=frame; backend->valid=backend->animationWork.valid=true;
		publication->backendWork=backend; publication->views.push_back(selection);
		publication->referenceFrames.push_back(input.referenceFrame); publication->presentationInputs.push_back(std::move(input));
		const auto sealed=SealRenderFramePublication(publication,session);
		Check(sealed.Receipt().IsSuccess() && sealed.Publication(),"Actual lighting geometry publication failed defensive sealing.");
		return sealed.Publication();
	}
}
