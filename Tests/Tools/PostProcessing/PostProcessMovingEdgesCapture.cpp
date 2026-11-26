// Native spatial-AA and independent temporal reconstruction from an imported scene.
// See MovingEdgesCaptureHost.md for preparation, build, and collection commands.
// Mesh captures verify current command-side draw and temporal evidence. The
// archived indexed image suite remains separate from mesh qualification.
#include <Windows.h>
#include <bcrypt.h>
#include "Animation/AnimationSystem.h"
#include "Components/AnimatorComponent.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Core/MainEngine.h"
#include "Core/Object/ObjectManager.h"
#include "DecodedCaptureAssertions.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsCatalog.h"
#include "Renderer/GraphicsSettings/PostProcessDiagnostics.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Renderer/VirtualGeometry/Geometry/VirtualGeometryPackedVertex.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneManager.h"
#include <GLFW/glfw3.h>
#include <vulkan/vulkan_core.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
	using Json = nlohmann::json;
	using namespace TerraEngine::Renderer;
	using namespace TerraEngine::Renderer::GraphicsSettings;
	using namespace TerraEngine::Renderer::Vulkan;
	constexpr std::string_view kFixtureHash = "2ab4d4557f4f9a38069d21dfc314388248c153580bbd7d5662e32190caf60787";
	constexpr std::array<std::string_view,4> kNames{"slanted-panel","thin-rigid-foreground","deforming-ribbon","frozen-background"};
	void Check(bool ok, std::string_view reason) { if(!ok) throw std::runtime_error(std::string(reason)); }
	std::filesystem::path Path(std::string_view value)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));
	}
	std::vector<std::uint8_t> Bytes(const std::filesystem::path& path)
	{
		std::ifstream stream(path,std::ios::binary);
		Check(stream.good(),"Cannot read input file.");
		return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
	}
	std::string Hash(std::span<const std::uint8_t> bytes)
	{
		// Windows' SHA implementation is already part of the required platform;
		// its owned handles are released even when an input or allocation fails.
		struct Algorithm { BCRYPT_ALG_HANDLE value{}; ~Algorithm() { if(value) BCryptCloseAlgorithmProvider(value,0); } } algorithm;
		struct Digest { BCRYPT_HASH_HANDLE value{}; ~Digest() { if(value) BCryptDestroyHash(value); } } digest;
		Check(BCryptOpenAlgorithmProvider(&algorithm.value,BCRYPT_SHA256_ALGORITHM,nullptr,0) >= 0,"SHA provider unavailable.");
		Check(BCryptCreateHash(algorithm.value,&digest.value,nullptr,0,nullptr,0,0) >= 0,"SHA allocation failed.");
		for(std::size_t offset=0;offset<bytes.size();)
		{
			const auto count=static_cast<ULONG>(std::min<std::size_t>(bytes.size()-offset,65536u));
			Check(BCryptHashData(digest.value,const_cast<PUCHAR>(bytes.data()+offset),count,0) >= 0,"SHA update failed."); offset+=count;
		}
		std::array<std::uint8_t,32> result{};
		Check(BCryptFinishHash(digest.value,result.data(),static_cast<ULONG>(result.size()),0) >= 0,"SHA finish failed.");
		constexpr char hex[]="0123456789abcdef"; std::string text;
		for(auto value:result) { text+=hex[value>>4u]; text+=hex[value&15u]; }
		return text;
	}
	Json ReadJson(const std::filesystem::path& path) { const auto bytes=Bytes(path); return Json::parse(bytes); }
	void Write(const std::filesystem::path& path,std::span<const std::uint8_t> bytes)
	{
		Check(!std::filesystem::exists(path),"Evidence file already exists.");
		std::ofstream stream(path,std::ios::binary); Check(stream.good(),"Cannot create evidence file.");
		stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
		stream.flush(); Check(stream.good(),"Cannot flush evidence file.");
	}
	void WriteJson(const std::filesystem::path& path,const Json& value)
	{
		const auto text=value.dump(2)+"\n";
		Write(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
	}
	Json Matrix(const glm::mat4& value)
	{
		Json result=Json::array(); for(int column=0;column!=4;++column) for(int row=0;row!=4;++row) result.push_back(value[column][row]); return result;
	}
	bool Same(const glm::mat4& left,const glm::mat4& right)
	{
		for(int column=0;column!=4;++column) for(int row=0;row!=4;++row)
			if(left[column][row]!=right[column][row]) return false;
		return true;
	}
	std::vector<std::uint8_t> DecodeBase64(std::string_view input)
	{
		Check(!input.empty() && input.size()%4u==0u,"Invalid PNG base64 length.");
		constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::vector<std::uint8_t> output; output.reserve(input.size()/4u*3u);
		for(std::size_t offset=0;offset<input.size();offset+=4u)
		{
			std::uint32_t bits=0u; unsigned padding=0u;
			for(unsigned lane=0;lane!=4u;++lane)
			{
				const auto character=input[offset+lane];
				if(character=='=') { Check(offset+4u==input.size() && lane>=2u,"Invalid base64 padding."); ++padding; bits<<=6u; }
				else { const auto value=alphabet.find(character); Check(value!=std::string_view::npos && padding==0u,"Invalid base64 symbol."); bits=(bits<<6u)|static_cast<std::uint32_t>(value); }
			}
			Check(padding<=2u,"Invalid base64 padding count.");
			output.push_back(static_cast<std::uint8_t>(bits>>16u));
			if(padding<2u) output.push_back(static_cast<std::uint8_t>(bits>>8u));
			if(padding==0u) output.push_back(static_cast<std::uint8_t>(bits));
		}
		return output;
	}
	struct Options
	{
		std::filesystem::path install,project,scene,fixture,output;
		std::string mode,temporalSequence,temporalRole;
		bool depthOfFieldNative=false,temporalUpscale=false,gameView=false;
		std::uint32_t aa=0u;
		std::uint32_t samples=1u;
		glm::uvec2 extent{640u,360u};
	};
	RenderViewType CaptureView(const Options& options) noexcept { return options.gameView ? RenderViewType::Game : RenderViewType::Scene; }
	RenderOutputKind CaptureOutput(const Options& options) noexcept { return options.gameView ? RenderOutputKind::GameView : RenderOutputKind::SceneView; }
	bool Temporal(const Options& options) noexcept { return options.aa==5u || options.temporalUpscale; }
	glm::uvec2 InternalExtent(const Options& options)
	{
		Check(!options.temporalUpscale || (options.extent.x%2u==0u && options.extent.y%2u==0u),"Half-resolution temporal capture requires even native output dimensions.");
		return options.temporalUpscale ? options.extent/2u : options.extent;
	}
	void SelectMode(Options& result)
	{
		if(result.mode=="Reference4x") result.extent={2560u,1440u};
		else if(result.mode=="OffNative") {}
		else if(result.mode=="OffHalf") { result.extent={320u,180u}; }
		else if(result.mode=="FXAA") result.aa=1u;
		else if(result.mode=="SMAA") result.aa=2u;
		else if(result.mode=="CMAA2") result.aa=3u;
		else if(result.mode=="MSAA") { result.aa=4u; result.samples=4u; }
		else if(result.mode=="TAA") { result.aa=5u; }
		else if(result.mode=="TemporalUpscale") { result.temporalUpscale=true; }
		else throw std::runtime_error("Supported runs are Reference4x, OffNative, OffHalf, FXAA, SMAA, CMAA2, MSAA, TAA, or TemporalUpscale.");
	}
	std::vector<GraphicsSettingValue> SceneRequestValues(std::uint32_t mode,bool upscale=false)
	{
		// Submit AA and reconstruction together so both attachments agree.
		std::vector<GraphicsSettingValue> values{{GraphicsSettingId::AntiAliasingMode,mode}};
		// Temporal reconstruction is a distinct native mode: standalone AA Off,
		// Scene temporal upscaling on, and actual half-size raster attachments.
		if(upscale) { values.push_back({GraphicsSettingId::UpscalingMode,std::uint32_t{1}}); values.push_back({GraphicsSettingId::InternalResolutionScale,0.5f}); }
		return values;
	}
	Options Parse(int count,char** args)
	{
		Options result; std::set<std::string> seen;
		for(int index=1;index<count;index+=2)
		{
			const std::string name=args[index]; Check(seen.insert(name).second,"Duplicate option.");
			if(name=="--depth-of-field-native") { result.depthOfFieldNative=true; --index; continue; }
			Check(index+1<count,"Every ordinary option needs a value."); const std::string_view value=args[index+1];
			if(name=="--install-root") result.install=Path(value);
			else if(name=="--project-root") result.project=Path(value);
			else if(name=="--scene") result.scene=Path(value);
			else if(name=="--fixture") result.fixture=Path(value);
			else if(name=="--output-root") result.output=Path(value);
			else if(name=="--mode") result.mode=value;
			else if(name=="--view") { Check(value=="scene" || value=="game","View must be scene or game."); result.gameView=value=="game"; }
			else if(name=="--temporal-sequence") result.temporalSequence=value;
			else if(name=="--temporal-role") result.temporalRole=value;
			else throw std::runtime_error("Unknown host option.");
		}
		Check(!result.install.empty() && !result.project.empty() && !result.scene.empty() && !result.fixture.empty() && !result.output.empty(),"Missing host input.");
		if(result.depthOfFieldNative && result.mode.empty()) result.mode="OffNative";
		SelectMode(result);
		// Collect Off and the oversampled reference through the same Game output
		// as the upscaler so score differences reflect reconstruction alone.
		const bool gameMode = result.temporalUpscale || result.mode=="OffHalf" || result.mode=="OffNative" || result.mode=="Reference4x";
		Check(!result.gameView || (gameMode && !result.depthOfFieldNative),"Independent native Game lane requires TemporalUpscale or its Off references.");
		Check(!result.depthOfFieldNative || (result.mode=="OffNative" && result.temporalSequence.empty()),"Native depth-of-field requires OffNative and no temporal sequence.");
		Check(result.temporalSequence.empty()==result.temporalRole.empty(),"Temporal sequence and role must be supplied together.");
		if(!result.temporalSequence.empty()) Check(Temporal(result) && (result.temporalSequence=="cut" || result.temporalSequence=="resize")
			&& (result.temporalRole=="reset" || result.temporalRole=="fresh"),"Separate temporal lanes require TAA/TemporalUpscale, cut/resize and reset/fresh.");
		Check(!std::filesystem::exists(result.output),"Output directory must be fresh.");
		Check(result.scene.is_relative(),"Scene must be project-relative.");
		for(const auto& part:result.scene) Check(part!="..","Scene escapes project.");
		return result;
	}
	Json RegisterCooked(const Options& options)
	{
		// Normal native Engine does not run the editor AssetDatabase. Register the
		// importer's actual cooked UUIDs without inventing or recooking payloads.
		const std::map<std::string,AssetType> kinds{{"Mesh",AssetType::Mesh},{"DeformableMesh",AssetType::Mesh},
			{"Material",AssetType::Material},{"Skeleton",AssetType::Skeleton},{"AnimationClip",AssetType::AnimationClip}};
		Json result=Json::array(); std::map<std::string,std::filesystem::path> owners;
		const auto assets=options.project/"Assets"; Check(std::filesystem::is_directory(assets),"Missing imported Assets directory.");
		for(const auto& entry:std::filesystem::recursive_directory_iterator(assets))
		{
			if(!entry.is_regular_file() || !entry.path().filename().string().ends_with(".import.json")) continue;
			const auto document=ReadJson(entry.path());
			const Json* manifest=&document;
			// Package import receipts wrap the authoritative published manifest;
			// standalone animation manifests expose the same subAssets at the root.
			if(document.contains("animationManifest")) manifest=&document.at("animationManifest");
			if(!manifest->contains("subAssets")) continue;
			Check(!document.contains("status") || document.at("status")=="Succeeded","Import receipt does not describe a successful cook.");
			for(const auto& row:manifest->at("subAssets"))
			{
				const auto kind=row.at("kind").get<std::string>(); if(!kinds.contains(kind)) continue;
				const auto idText=row.at("guid").get<std::string>(); const auto id=TerraEngine::ParseUUID(idText); Check(id.has_value(),"Cooked GUID is invalid.");
				const auto relative=Path(row.at("compiledPath").get<std::string>()); Check(relative.is_relative(),"Cooked path is not relative.");
				for(const auto& part:relative) Check(part!="..","Cooked path escapes Assets.");
				const auto file=options.project/"Build/Assets"/relative; Check(std::filesystem::is_regular_file(file),"Cooked import payload is absent.");
				if(owners.contains(idText)) { Check(owners.at(idText)==file,"Cooked GUID has conflicting payloads."); continue; }
				owners.emplace(idText,file); ResourceManager::Instance().RegisterAssetPath(*id,file,kinds.at(kind));
				result.push_back({{"guid",idText},{"kind",kind},{"file",file.generic_string()},{"sha256",Hash(Bytes(file))},
					{"importReceipt",entry.path().generic_string()},{"importReceiptSha256",Hash(Bytes(entry.path()))}});
			}
			// Generated mesh/material summaries must identify the same actual
			// payloads as the atomically published package manifest, not old aliases.
			for(const auto field:{"generatedMeshes","generatedMaterials"}) if(document.contains(field))
				for(const auto& row:document.at(field))
				{
					const auto id=row.at("guid").get<std::string>();
					Check(owners.contains(id) && owners.at(id)==options.project/"Build/Assets"/Path(row.at("compiledPath").get<std::string>()),
						"Generated import summary disagrees with its published cooked resource manifest.");
				}
		}
		Check(!result.empty(),"No real cooked import records were found."); return result;
	}
	struct Inputs
	{
		std::map<std::string,Object> objects;
		std::map<std::string,TerraEngine::Scene::WorldPose> basePoses;
		std::vector<Object> animators;
		std::optional<Object> gameCamera;
	};
	Inputs DiscoverObjects()
	{
		Inputs result; auto& objects=ObjectManager::instance();
		for(const auto object:objects.getObjectsWithComponents<MeshRenderComponent>())
		{
			const auto name=objects.getObjectName(object);
			Check(std::find(kNames.begin(),kNames.end(),name)!=kNames.end(),"Unexpected mesh in the imported fixture scene.");
			Check(result.objects.emplace(name,object).second,"Ambiguous imported mesh name.");
			const auto pose=objects.GetCanonicalWorldPose(object); Check(pose.has_value(),"Imported mesh has no canonical pose."); result.basePoses.emplace(name,*pose);
		}
		Check(result.objects.size()==kNames.size(),"The scene must contain the four exact imported moving-edge mesh objects.");
		result.animators=objects.getObjectsWithComponents<AnimatorComponent>(); Check(result.animators.size()==1u,"Imported fixture must have exactly one real two-joint animation owner.");
		for(const auto object:result.animators)
		{
			// The normal authoritative producer makes its real palette inspectable.
			// This changes evaluation placement, not geometry, shading, or AA quality.
			auto* component=objects.GetComponent<AnimatorComponent>(object);
			Check(component->defaultClipAssetId!=TerraEngine::EMPTY_UUID,"Imported animator lacks its cooked clip.");
			component->playAutomatically=false; component->defaultClipStartFrame=0u; component->loopDefaultClip=false;
			component->requestedMode=TerraEngine::Animation::AnimationExecutionPolicy::ForceCpuDeterministicFallback;
		}
		return result;
	}
	GraphicsCameraTarget GameCameraIdentity(const Object& object)
	{
		// Same real authority tuple as the ordinary editor Game camera producer.
		// Capture only between joined Engine frames on this fixture owner thread.
		auto& objects=ObjectManager::instance();
		Check(objects.isObjectValid(object) && objects.GetComponent<CameraComponent>(object)
			&& objects.GetComponent<Transform>(object),"Runtime Game camera authority is missing.");
		return {objects.getUUID(object),objects.GetSceneGeneration(),objects.getObjectLifetimeGeneration(object),
			objects.getObjectComponentGeneration(object),objects.getObjectBindingGeneration(object)};
	}
	struct PoseInput { glm::mat4 view{1.0f},projection{1.0f}; TerraEngine::Scene::WorldPosition camera; };
	bool ExactGamePose(const RenderViewPresentationInput& input,const PoseInput& pose,glm::uvec2 extent,std::uint64_t frame)
	{
		if(input.output.kind!=RenderOutputKind::GameView || input.output.targetSize!=extent || input.poseSequence!=0u
			|| input.referenceFrame.frameId!=frame || input.referenceFrame.canonicalOrigin.cell!=TerraEngine::Scene::WorldCell{}) return false;
		const auto expected=glm::translate(glm::mat4(1.0f),glm::vec3(input.referenceFrame.canonicalOrigin.offsetMetres-pose.camera.offsetMetres));
		return Same(input.projectionMatrix,pose.projection) && Same(input.viewMatrix,expected);
	}
	PoseInput ApplyPose(const Inputs& inputs,const Json& pose,std::uint64_t sequence)
	{
		auto& objects=ObjectManager::instance(); const auto phase=pose.at("simulationStep").get<std::uint32_t>();
		const auto& ticks=pose.at("cameraPositionTicks");
		PoseInput result; result.camera.offsetMetres={ticks.at(0).get<double>()/4096.0,ticks.at(1).get<double>()/4096.0,ticks.at(2).get<double>()/4096.0};
		// Native override view matrices are reference-local. The engine localizes
		// geometry against this separate canonical camera position; translating
		// the matrix by the canonical position here would apply motion twice.
		result.view=glm::mat4(1.0f);
		result.projection=glm::orthoRH_ZO(-320.0f,320.0f,-180.0f,180.0f,0.1f,16.0f); result.projection[1][1]*=-1.0f;
		// Owner-thread writes happen between joined Engine frames. They are visible
		// before the next declared capture phase, never from a worker callback.
		auto rail=inputs.basePoses.at("thin-rigid-foreground");
		const auto& vertices=pose.at("geometry").at(1).at("verticesTicks");
		rail.position.offsetMetres.x+=static_cast<double>(vertices.at(0).at(0).get<std::int64_t>()-1372160)/4096.0;
		Check(objects.SetCanonicalWorldPose(inputs.objects.at("thin-rigid-foreground"),rail)==TerraEngine::Scene::WorldCoordinateError::None,"Frozen rail pose was rejected.");
		auto& animation=TerraEngine::Animation::AnimationSystem::Instance();
		for(const auto object:inputs.animators)
		{
			const auto handle=animation.ResolveAnimatorForObject(objects.getUUID(object));
			Check(animation.IsAnimatorInstanceAlive(handle),"Imported animator has not reached its normal binding boundary.");
			TerraEngine::Animation::AnimationCommand command; command.target=handle; command.kind=TerraEngine::Animation::AnimationCommandKind::Seek;
			command.playbackTick=phase; command.producerId=0x414151u; command.producerSequence=sequence; command.correlationId=sequence;
			const auto receipt=animation.SubmitCommand(command);
			Check(receipt.status==TerraEngine::Animation::AnimationStatus::Queued,"Exact fixed-phase native animation seek was not queued.");
		}
		return result;
	}

	// The additive observer never fabricates an accepted frame if linked against
	// an older library. The parent supplies this read-only, exact-frame seam.
	template<class Context> std::shared_ptr<const RenderFramePublication> Accepted(Context& context,std::uint64_t frame)
	{
		if constexpr(requires { context.ReadAcceptedRenderFramePublication(frame); }) return context.ReadAcceptedRenderFramePublication(frame);
		else throw std::runtime_error("Installed native renderer lacks the accepted-publication observer.");
	}
	void CompleteEditor(VulkanContext& context,Json& receipts)
	{
		const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
		for(;;)
		{
			glfwPollEvents(); const auto before=context.GetEditorSubmissionProgress();
			bool prepared=false,recorded=false;
			const auto receipt=context.PresentEditor([&] { prepared=true; },[&](void*) { recorded=true; });
			const auto after=context.GetEditorSubmissionProgress();
			receipts.push_back({{"outcome",static_cast<unsigned>(receipt.outcome)},{"stage",static_cast<unsigned>(receipt.stage)},
				{"nativeResult",receipt.nativeResult},{"submissionSerial",std::to_string(receipt.submissionSerial)},
				{"uiPrepared",prepared},{"uiRecorded",recorded},{"lastSubmittedSerial",std::to_string(after.lastSubmittedSerial)},
				{"presentationGeneration",std::to_string(after.presentationGeneration)}});
			if((receipt.outcome==EditorPresentOutcome::Submitted || receipt.outcome==EditorPresentOutcome::RecreationRequested)
				&& receipt.stage==EditorUiFrameStage::Present && (receipt.nativeResult==VK_SUCCESS || receipt.nativeResult==VK_SUBOPTIMAL_KHR)
				&& receipt.submissionSerial>before.lastSubmittedSerial && after.lastSubmittedSerial==receipt.submissionSerial && prepared && recorded && !after.deviceLost)
			{ context.WaitIdle(); return; }
			Check(receipt.outcome==EditorPresentOutcome::Skipped && receipt.stage==EditorUiFrameStage::AcquireImage
				&& (receipt.nativeResult==VK_TIMEOUT || receipt.nativeResult==VK_NOT_READY)
				&& !prepared && !recorded && after.lastSubmittedSerial==before.lastSubmittedSerial
				&& after.presentationGeneration==before.presentationGeneration && !after.deviceLost
				&& std::chrono::steady_clock::now()<deadline,"Actual editor completion submission failed.");
		}
	}
	RenderCaptureResult Capture(VulkanContext& context,std::uint64_t frame,glm::uvec2 extent,RenderCaptureImageStage stage,std::string id,RenderViewType view=RenderViewType::Scene)
	{
		RenderCaptureRequest request; request.requestId=std::move(id); request.sourceKind=RenderCaptureSourceKind::GameView;
		request.viewType=view; request.targetSize=extent; request.submittedFrameId=frame;
		request.allowBackendFallback=false; request.imageStage=stage; request.timeoutMs=10000u;
		const auto admission=context.ReadbackCapture(request);
		Check(admission.ok && admission.pendingAsync && admission.backendReadback,"Native frame readback was not admitted.");
		const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
		for(;;)
		{
			for(auto result:context.PollCaptureReadbacks())
			{
				Check(result.requestId==request.requestId && result.ok && result.backendReadback && !result.deterministicFallback
					&& result.backendTarget && result.frameId==frame && result.imageStage==stage && result.size==extent && !result.overlaysIncluded
					&& result.backendTarget->sourceWidth==extent.x && result.backendTarget->sourceHeight==extent.y,
					"Readback did not identify the exact accepted native Scene frame/stage.");
				Check(result.sourceColorEncoding==RenderCaptureColorEncoding::Linear && result.deliveredColorEncoding==RenderCaptureColorEncoding::Srgb,
					"Readback did not use the explicit single linear-to-sRGB transfer.");
				return result;
			}
			Check(std::chrono::steady_clock::now()<deadline,"Native readback timed out."); std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
	float Value(const GraphicsSettingsSnapshot& snapshot,GraphicsSettingId id,float fallback)
	{
		for(const auto& value:snapshot.values) if(value.id==id)
		{
			if(const auto scalar=std::get_if<float>(&value.value)) return *scalar;
			if(const auto integer=std::get_if<std::uint32_t>(&value.value)) return static_cast<float>(*integer);
			if(const auto flag=std::get_if<bool>(&value.value)) return *flag ? 1.0f : 0.0f;
			throw std::runtime_error("Unexpected typed effective setting value.");
		}
		return fallback;
	}
	Json ReceiptEvidence(const GraphicsRequestReceipt& receipt)
	{
		// Keep the real typed rejection intact. A generic "settings rejected"
		// message conceals whether the caller supplied a bad variant or the native
		// resource owner failed to prepare a valid request.
		Json affected=Json::array(); for(const auto id:receipt.affectedSettings) affected.push_back(static_cast<std::uint32_t>(id));
		return {{"requestId",std::to_string(receipt.requestId)},{"sequence",std::to_string(receipt.sequence)},
			{"state",static_cast<unsigned>(receipt.state)},{"reason",static_cast<unsigned>(receipt.reason)},
			{"activation",static_cast<unsigned>(receipt.activation)},{"detail",receipt.detail},{"affectedSettings",affected},
			{"requestedRevision",std::to_string(receipt.requestedRevision)},{"effectiveRevision",std::to_string(receipt.effectiveRevision)},
			{"appliedFrameId",std::to_string(receipt.appliedFrameId)}};
	}
	void RequireViableReceipt(const GraphicsRequestReceipt& receipt,std::string_view operation)
	{
		if(receipt.state==GraphicsRequestState::Rejected || receipt.state==GraphicsRequestState::Cancelled)
			throw std::runtime_error(std::string(operation)+": "+ReceiptEvidence(receipt).dump());
	}
	std::string_view CommonControlName(GraphicsSettingId id)
	{
		switch(id)
		{
		case GraphicsSettingId::ExposureMode: return "postProcess.exposureMode";
		case GraphicsSettingId::FixedExposureCompensation: return "postProcess.fixedExposureCompensation";
		case GraphicsSettingId::ToneMappingMode: return "postProcess.toneMappingMode";
		case GraphicsSettingId::BloomEnabled: return "postProcess.bloomEnabled";
		case GraphicsSettingId::ColorGradingEnabled: return "postProcess.colorGradingEnabled";
		case GraphicsSettingId::EnableIBL: return "enableIBL";
		case GraphicsSettingId::IndirectStrength: return "indirectStrength";
		case GraphicsSettingId::VirtualGeometryMaxLodDrop: return "virtualGeometry.maxLodDrop";
		case GraphicsSettingId::VirtualGeometryLodErrorScale: return "virtualGeometry.lodErrorScale";
		default: return "unknown fixture control";
		}
	}
	Json ValueEvidence(const GraphicsValue& value) { return std::visit([](const auto& item)->Json { return item; },value); }
	bool WithinBound(const GraphicsValue& value,const std::optional<GraphicsValue>& bound,bool minimum)
	{
		if(!bound) return true;
		if(value.index()!=bound->index()) return false;
		return std::visit([&](const auto& item)
		{
			using Type=std::decay_t<decltype(item)>;
			if constexpr(std::is_arithmetic_v<Type>) return minimum ? item>=std::get<Type>(*bound) : item<=std::get<Type>(*bound);
			else return false;
		},value);
	}
	Json VerifyPrepared(RenderSystem& renderer,const RenderFramePublication& publication,const Inputs& inputs,
		const Json& frozen,const PoseInput& pose,const Options& options,std::uint64_t sequence)
	{
		std::optional<std::size_t> index;
		for(std::size_t candidate=0;candidate<publication.presentationInputs.size();++candidate)
			if(publication.presentationInputs[candidate].output.kind==CaptureOutput(options))
			{ Check(!index,"Ambiguous requested native output."); index=candidate; }
		Check(index.has_value(),"Accepted publication has no requested native output.");
		const auto& view=publication.presentationInputs[*index];
		Check(view.referenceFrame.canonicalOrigin.cell==TerraEngine::Scene::WorldCell{},"Unexpected nonlocal fixture reference cell.");
		const auto expectedView=glm::translate(glm::mat4(1.0f),glm::vec3(view.referenceFrame.canonicalOrigin.offsetMetres-pose.camera.offsetMetres));
		const Json cameraCheck{{"expectedPoseSequence",std::to_string(options.gameView ? 0u : sequence)},{"actualPoseSequence",std::to_string(view.poseSequence)},
			{"actualOrthographic",view.sceneViewOrthographic},{"actualGridVisible",view.sceneViewGridVisible},
			{"expectedExtent",{options.extent.x,options.extent.y}},{"actualExtent",{view.output.targetSize.x,view.output.targetSize.y}},
			{"expectedProjectionFloat32",Matrix(pose.projection)},{"actualProjectionFloat32",Matrix(view.projectionMatrix)},
			{"expectedViewFloat32",Matrix(expectedView)},{"actualViewFloat32",Matrix(view.viewMatrix)},
			{"canonicalCamera",{pose.camera.offsetMetres.x,pose.camera.offsetMetres.y,pose.camera.offsetMetres.z}},
			{"referenceOrigin",{view.referenceFrame.canonicalOrigin.offsetMetres.x,view.referenceFrame.canonicalOrigin.offsetMetres.y,view.referenceFrame.canonicalOrigin.offsetMetres.z}},
			{"actualStaticOverlayQuads",view.uiBatch.staticQuads.size()},{"actualIBL",view.settings.enableIBL},
			{"actualIndirectStrength",view.settings.indirectStrength},{"actualMaxLodDrop",view.settings.virtualGeometry.maxLodDrop},
			{"actualLodScale",view.settings.virtualGeometry.lodErrorScale}};
		// Game overrides have no pose-sequence ingress or orthographic flag. Their
		// actual frozen projection and reference-local camera matrices remain exact;
		// never relabel local fixture sequence as a native Game camera stamp.
		Check(view.poseSequence==(options.gameView ? 0u : sequence)
			&& (options.gameView ? ExactGamePose(view,pose,options.extent,publication.frameId) : (view.sceneViewOrthographic && !view.sceneViewGridVisible))
			&& view.output.targetSize==options.extent && Same(view.projectionMatrix,pose.projection) && Same(view.viewMatrix,expectedView)
			&& view.uiBatch.staticQuads.empty() && view.settings.enableIBL==0u && view.settings.indirectStrength==0.0f
			&& view.settings.virtualGeometry.maxLodDrop==0u && view.settings.virtualGeometry.lodErrorScale==64.0f,
			"Accepted frozen camera state mismatch: "+cameraCheck.dump());
		Check(view.graphicsView && view.graphicsView->lifetimeGeneration && view.graphicsSettings,"Accepted native owner lifetime/settings are missing.");
		const auto& settings=*view.graphicsSettings;
		Check(settings.sampleCount==options.samples && Value(settings,GraphicsSettingId::AntiAliasingMode,0)==static_cast<float>(options.aa)
			&& Value(settings,GraphicsSettingId::ExposureMode,0)==1.0f && Value(settings,GraphicsSettingId::FixedExposureCompensation,99)==0.0f
			&& Value(settings,GraphicsSettingId::ToneMappingMode,0)==0.0f && Value(settings,GraphicsSettingId::BloomEnabled,0)==0.0f
			&& Value(settings,GraphicsSettingId::ColorGradingEnabled,0)==0.0f && Value(settings,GraphicsSettingId::InternalResolutionScale,1)==(options.temporalUpscale ? 0.5f : 1.0f)
			&& Value(settings,GraphicsSettingId::UpscalingMode,0)==(options.temporalUpscale ? 1.0f : 0.0f)
			&& Value(settings,GraphicsSettingId::UpscalingSharpness,0)==0.0f,
			"Actual prepared mode, sample count, exposure, tone, bloom, grade, or scale differs from controls.");
		const auto plan=BuildPostProcessGraphSettings(settings);
		Check(plan.executionAvailable && plan.antiAliasingMode==options.aa && plan.spatialAA==(options.aa>=1u && options.aa<=3u)
			&& plan.coverageAA==(options.aa==4u) && plan.temporalResolve==Temporal(options) && plan.sampleCount==options.samples
			&& plan.internalSize==InternalExtent(options) && plan.outputSize==options.extent,
			"Accepted native AA/reconstruction plan differs from exact requested internal/output extents.");
		Json proof{{"viewMatrixFloat32",Matrix(view.viewMatrix)},{"projectionMatrixFloat32",Matrix(view.projectionMatrix)},
			{"poseSequence",std::to_string(view.poseSequence)},{"settingsRevision",std::to_string(settings.revision)},
			{"nativeRendererSession",std::to_string(publication.rendererSessionGeneration)},
			{"nativeViewId",std::to_string(view.graphicsView->outputId)},
			{"nativeViewGeneration",std::to_string(view.graphicsView->lifetimeGeneration)},
			{"referenceOrigin",{view.referenceFrame.canonicalOrigin.offsetMetres.x,view.referenceFrame.canonicalOrigin.offsetMetres.y,view.referenceFrame.canonicalOrigin.offsetMetres.z}},
			{"actualGeometryRoute","ordinary-direct-mesh-task"},
			{"frozenIndexedRouteSatisfied",false},{"cameraChecks",cameraCheck},{"geometry",Json::array()}};
		if(options.gameView)
		{
			Check(inputs.gameCamera && view.graphicsCamera && *view.graphicsCamera==GameCameraIdentity(*inputs.gameCamera),
				"Accepted Game camera does not retain the exact real scene/object/component/binding authority.");
			proof["cameraAuthorityId"]=TerraEngine::ToHexString(view.graphicsCamera->cameraId);
			proof["fixtureIngressSequence"]=std::to_string(sequence);
			proof["nativeViewType"]="Game";
			proof["nativePoseJoin"]="ExactAcceptedFrameProjectionViewAndCanonicalOrigin";
			proof["nativePoseSequenceSupported"]=false;
		}
		if(Temporal(options))
		{
			Check(Value(settings,GraphicsSettingId::UpscalingMode,0)==(options.temporalUpscale ? 1.0f : 0.0f),"Native temporal reconstruction selector differs.");
			proof["acceptedSettingsGraphPlan"]={{"temporalResolve",plan.temporalResolve},{"spatialAA",plan.spatialAA},
				{"coverageAA",plan.coverageAA},{"sampleCount",plan.sampleCount},{"standaloneAAMode",plan.antiAliasingMode},
				{"upscalingMode",Value(settings,GraphicsSettingId::UpscalingMode,0)},
				{"internalExtent",{plan.internalSize.x,plan.internalSize.y}},{"outputExtent",{plan.outputSize.x,plan.outputSize.y}}};
		}
		{
			const auto* native=dynamic_cast<VulkanContext*>(renderer.GetContext()); Check(native,"Native mesh proof requires Vulkan.");
			const auto recorded=native->ReadRecordedGraphicsViews(publication.frameId);
			const auto actual=std::find_if(recorded.begin(),recorded.end(),[&](const auto& item) { return item.publicationIndex==*index; });
			Check(actual!=recorded.end() && actual->width==options.extent.x && actual->height==options.extent.y && actual->indexedDrawCount==0u
				&& actual->internalWidth==InternalExtent(options).x && actual->internalHeight==InternalExtent(options).y,
				"Actual accepted native Scene did not record ordinary mesh geometry at its frozen extent.");
			// The snapshot describes effective intent; this independent native
			// observation comes from the pipeline/pass variant actually recorded.
			Check(actual->rasterSampleCount==options.samples,"Recorded native raster sample count differs from effective capture configuration.");
			proof["nativeIndexedDrawCount"]=actual->indexedDrawCount;
			proof["nativeRasterSampleCount"]=actual->rasterSampleCount;
			proof["nativeInternalExtent"]={actual->internalWidth,actual->internalHeight};
			proof["nativeOutputExtent"]={actual->width,actual->height};
			if(Temporal(options))
			{
				// These are facts from the exact recorded native command, not motion
				// pixel assertions. Measured rows join them to completed readbacks.
				Check(actual->recordedMotionDrawCount==4u && actual->temporalRecordingId==publication.frameId,
					"Native temporal reconstruction did not record exact four-draw motion and the current temporal command identity.");
				Check(actual->temporalPreviousFrameId==0u || actual->temporalPreviousFrameId<publication.frameId,
					"Native temporal predecessor must be zero or an earlier accepted frame.");
				proof["nativeMotionDrawCount"]=actual->recordedMotionDrawCount;
				proof["nativeTemporalRecordingId"]=std::to_string(actual->temporalRecordingId);
				proof["nativeTemporalPreviousFrameId"]=std::to_string(actual->temporalPreviousFrameId);
				proof["nativeTemporalJitterPhase"]=actual->temporalJitterPhase;
			}
		}
		auto& animation=TerraEngine::Animation::AnimationSystem::Instance();
		const auto animator=animation.ResolveAnimatorForObject(ObjectManager::instance().getUUID(inputs.animators.front()));
		const auto playback=animation.LoadAnimatorPlaybackState(animator);
		const auto animationReceipt=animation.ReadCommandReceipt(0x414151u,sequence,sequence);
		const auto animationSnapshot=animation.LoadAnimatorSnapshot(animator);
		Check(animationReceipt.status==TerraEngine::Animation::AnimationStatus::Ok
			&& playback.resolvedTick==frozen.at("simulationStep").get<std::int64_t>()
			&& animationSnapshot && animationSnapshot->pose && animationSnapshot->pose->IsSealed()
			&& animationSnapshot->pose->LocalTransforms().size()==2u,"Actual imported animator did not publish the exact frozen two-joint phase.");
		proof["animation"]={{"resolvedTick",playback.resolvedTick},{"absoluteTick",playback.absoluteTick},
			{"runState",static_cast<unsigned>(playback.runState)},{"snapshotGeneration",std::to_string(animationSnapshot->publicationGeneration)},
			{"commandProducerSequence",std::to_string(sequence)},{"commandStatus",static_cast<unsigned>(animationReceipt.status)},
			{"localJointTransformsFloat32",Json::array()}};
		for(const auto& joint:animationSnapshot->pose->LocalTransforms())
			proof["animation"]["localJointTransformsFloat32"].push_back({{"translation",{joint.translation.x,joint.translation.y,joint.translation.z}},
				{"rotation",{joint.rotation.x,joint.rotation.y,joint.rotation.z,joint.rotation.w}},{"scale",{joint.scale.x,joint.scale.y,joint.scale.z}}});
		const auto renderables=renderer.GetLatestRenderablesSnapshot();
		auto commands=MakeRenderViewCommandResolver(publication,*index); Check(commands.Count()==4u,"Accepted camera does not select exactly the four frozen meshes.");
		std::set<std::string> selected;
		for(std::size_t commandIndex=0;commandIndex<commands.Count();++commandIndex)
		{
			RenderItemRecord command; Check(commands.Resolve(commandIndex,command),"Accepted draw command could not be resolved.");
			const auto object=ObjectManager::instance().getObject(command.objectId); Check(object.has_value(),"Accepted mesh owner retired unexpectedly.");
			const auto name=ObjectManager::instance().getObjectName(*object);
			Check(inputs.objects.contains(name) && selected.insert(name).second,"Accepted draw uses an unexpected/duplicate object.");
			Check(command.geometryRoute==GeometryRoute::DirectMeshlet,"An exploratory direct mesh unexpectedly changed geometry representation.");
			auto* mesh=ResourceManager::Instance().GetMesh(command.meshId); auto* material=ResourceManager::Instance().GetMaterial(command.materialId);
			Check(mesh && material && command.selectedVirtualLod==0u && command.virtualMeshletCount==1u
				&& command.virtualMeshletOffset<mesh->meshletRecords.size(),"Accepted draw no longer selects exactly its full-detail source meshlet.");
			const auto& sourceMeshlet=mesh->meshletRecords[command.virtualMeshletOffset];
			Check(sourceMeshlet.vertexCount==4u && sourceMeshlet.primitiveIndexCount==6u,
				"Selected full-detail meshlet no longer has its four native vertices and two triangles.");
			const Json* geometry=nullptr;
			for(const auto& row:frozen.at("geometry")) if(row.at("id")==name) geometry=&row;
			const std::array<float,3> expectedRadiance=name=="frozen-background" ? std::array<float,3>{.04f,.04f,.04f}
				: geometry->at("radiance").get<std::array<float,3>>();
			Check(material->header.alphaMode==static_cast<std::uint32_t>(AlphaMode::Opaque) && (material->header.flags&DoubleSided)!=0u
				&& material->header.baseColorFactor[0]==0.0f && material->header.baseColorFactor[1]==0.0f && material->header.baseColorFactor[2]==0.0f
				&& material->header.baseColorFactor[3]==1.0f && material->textures.empty(),"Actual cooked material is not opaque double-sided emission with zero reflected base color.");
			for(unsigned channel=0;channel!=3u;++channel)
				Check(material->header.emissiveFactor[channel]==expectedRadiance[channel],"Native material emission differs from the frozen float32 value.");
			Json row{{"name",name},{"objectId",TerraEngine::ToHexString(command.objectId)},{"meshId",TerraEngine::ToHexString(command.meshId)},
				{"materialId",TerraEngine::ToHexString(command.materialId)},{"modelMatrixFloat32",Matrix(command.modelMatrix)},
				{"canonicalModelMatrixFloat32",Matrix(glm::translate(glm::mat4(1.0f),glm::vec3(view.referenceFrame.canonicalOrigin.offsetMetres))*command.modelMatrix)},
				{"meshGeneration",std::to_string(mesh->publicationGeneration)},{"materialGeneration",std::to_string(material->publicationGeneration)},
				{"selectedLod",command.selectedVirtualLod},{"selectedMeshletOffset",command.virtualMeshletOffset},{"selectedMeshletCount",command.virtualMeshletCount},
				{"packedVertexSha256",Hash(mesh->meshletVertexData)},{"primitiveIndexSha256",Hash(mesh->meshletPrimitiveIndexData)},
				{"materialEmissionFloat32",expectedRadiance},{"nativeWorldVerticesFloat32",Json::array()}};
			const auto record=std::find_if(renderables.begin(),renderables.end(),[&](const auto& item) { return item.objectId==command.objectId; });
			Check(record!=renderables.end(),"Accepted draw has no matching prepared resource/animation record.");
			if(name=="deforming-ribbon") Check(record->animationBinding.HasCpuDeformation(),"Real imported ribbon animation did not publish its authoritative native palette.");
			for(std::size_t localVertex=0;localVertex!=4u;++localVertex)
			{
				const auto remapIndex=sourceMeshlet.vertexOffset+localVertex;
				Check(remapIndex<mesh->meshletVertexRemapDecoded.size(),"Selected meshlet vertex remap is truncated.");
				const auto vertexIndex=mesh->meshletVertexRemapDecoded[remapIndex];
				Check((vertexIndex+1u)*sizeof(TerraEngine::VirtualGeometryPackedVertex)<=mesh->meshletVertexData.size(),"Cooked packed vertex span is truncated.");
				TerraEngine::VirtualGeometryPackedVertex packed{}; std::memcpy(&packed,mesh->meshletVertexData.data()+vertexIndex*sizeof(packed),sizeof(packed));
				const auto decoded=TerraEngine::DecodeVirtualGeometryPackedPosition(packed); glm::vec3 position;
				for(unsigned axis=0;axis!=3u;++axis) position[axis]=decoded[axis]*mesh->runtimeLayout.quantization.positionScale[axis]+mesh->runtimeLayout.quantization.positionOffset[axis];
				if(name=="deforming-ribbon")
				{
					Check(mesh->meshletSkinningData.size()>=(vertexIndex+1u)*8u,"Cooked ribbon skin stream is absent.");
					glm::vec3 deformed(0.0f); float weightSum=0.0f;
					const auto& palette=record->animationBinding.cpuDeformation->paletteMatrices;
					for(unsigned lane=0;lane!=4u;++lane)
					{
						const auto joint=mesh->meshletSkinningData[vertexIndex*8u+lane]; const float weight=mesh->meshletSkinningData[vertexIndex*8u+4u+lane]/255.0f;
						if(weight==0.0f) continue; Check(joint<palette.size(),"Ribbon joint is outside the actual prepared palette.");
						const auto& matrix=palette[joint]; const glm::vec4 source(position,1.0f);
						deformed+=weight*glm::vec3(glm::dot(glm::vec4(matrix.row0[0],matrix.row0[1],matrix.row0[2],matrix.row0[3]),source),
							glm::dot(glm::vec4(matrix.row1[0],matrix.row1[1],matrix.row1[2],matrix.row1[3]),source),
							glm::dot(glm::vec4(matrix.row2[0],matrix.row2[1],matrix.row2[2],matrix.row2[3]),source)); weightSum+=weight;
					}
					Check(weightSum>0.0f,"Ribbon skin has no positive influence."); position=deformed/weightSum;
				}
				const auto local=glm::vec3(command.modelMatrix*glm::vec4(position,1.0f));
				const auto world=local+glm::vec3(view.referenceFrame.canonicalOrigin.offsetMetres);
				row["nativeWorldVerticesFloat32"].push_back({world.x,world.y,world.z});
				// Cooking uses the production UNORM16 position representation. Verify
				// membership against the frozen vertices within its declared step;
				// retain the actual float32 values and error instead of replacing them.
				if(geometry)
				{
					float closest=std::numeric_limits<float>::max();
					for(const auto& ticks:geometry->at("verticesTicks"))
					{
						const glm::vec3 expected(ticks.at(0).get<float>()/4096.0f-320.0f,180.0f-ticks.at(1).get<float>()/4096.0f,-1.0f-geometry->at("depth").get<float>());
						closest=std::min(closest,glm::length(world-expected));
					}
					const float step=2.0f*std::max({mesh->runtimeLayout.quantization.positionScale[0],mesh->runtimeLayout.quantization.positionScale[1],mesh->runtimeLayout.quantization.positionScale[2]})/65535.0f;
					Check(closest<=2.0f*step+0.0001f,"Actual native camera/rigid/deformed vertices disagree with the frozen pose beyond the cooked position step.");
					row["frozenVertexDistances"].push_back(closest); row["nativePositionStep"]=step;
				}
				else
				{
					// Background is ordinary geometry too. Verify the actual large quad
					// rather than allowing the unrelated editor clear color to qualify.
					const float step=2048.0f/65535.0f;
					Check(std::abs(std::abs(world.x)-1024.0f)<=step && std::abs(std::abs(world.y)-1024.0f)<=step
						&& std::abs(world.z+8.0f)<=step,"Actual ordinary background quad differs from the frozen source implementation.");
				}
			}
			row["posePublicationGeneration"]=std::to_string(record->animationBinding.posePublicationGeneration);
			if(name=="deforming-ribbon")
			{
				row["nativePaletteFloat32"]=Json::array();
				for(const auto& palette:record->animationBinding.cpuDeformation->paletteMatrices)
					row["nativePaletteFloat32"].push_back({palette.row0,palette.row1,palette.row2});
			}
			row["deformationContentIdLow"]=std::to_string(command.animation.deformationContentIdLow);
			row["deformationContentIdHigh"]=std::to_string(command.animation.deformationContentIdHigh);
			proof["geometry"].push_back(std::move(row));
		}
		const auto stats=renderer.GetLatestRenderStats(CaptureView(options));
		// The profiler publishes fence-completed statistics asynchronously. An
		// older profiler row cannot stand in for the exact indexed draw counts
		// already checked against this publication above.
		const bool exactStats=stats.publicationFrameId==publication.frameId
			&& stats.rendererSessionGeneration==publication.rendererSessionGeneration
			&& stats.outputKind==view.output.kind && stats.outputId==view.output.outputId;
		proof["completedStatsMatchCapturedFrame"]=exactStats;
		proof["observedStatsFrameId"]=std::to_string(stats.publicationFrameId);
		if(exactStats)
		{
			Check(stats.drawCalls>0u && stats.triangleCount>=8u,"Completed native view has no actual geometry draw/triangle evidence.");
			proof["actualDrawCalls"]=stats.drawCalls; proof["actualTriangles"]=stats.triangleCount;
		}
		// Reset/fresh comparisons hash copied state, not queue counters or native
		// resource generations. Canonical matrices remove only reference-origin
		// bookkeeping; every numeric pose/palette/control value remains actual.
		if(!options.temporalSequence.empty()) {
		Json simulation{{"canonicalViewMatrixFloat32",Matrix(view.viewMatrix*glm::translate(glm::mat4(1.0f),-glm::vec3(view.referenceFrame.canonicalOrigin.offsetMetres)))},
			{"projectionMatrixFloat32",Matrix(view.projectionMatrix)},{"animation",{{"resolvedTick",playback.resolvedTick},
			{"runState",static_cast<unsigned>(playback.runState)},{"localJointTransformsFloat32",proof.at("animation").at("localJointTransformsFloat32")}}},{"geometry",Json::object()}};
		for(const auto& geometry:proof.at("geometry")) {
			Json normalized; for(const auto key:{"canonicalModelMatrixFloat32","nativeWorldVerticesFloat32","materialEmissionFloat32","selectedLod","selectedMeshletCount","packedVertexSha256","primitiveIndexSha256"}) normalized[key]=geometry.at(key);
			if(geometry.contains("nativePaletteFloat32")) normalized["nativePaletteFloat32"]=geometry.at("nativePaletteFloat32");
			simulation["geometry"][geometry.at("name").get<std::string>()]=std::move(normalized);
		}
		Json controls{{"sampleCount",settings.sampleCount},{"internalExtent",{plan.internalSize.x,plan.internalSize.y}},
			{"outputExtent",{plan.outputSize.x,plan.outputSize.y}},{"effectiveValues",Json::object()},
			{"nativeRasterSampleCount",proof.value("nativeRasterSampleCount",1u)},{"nativeSettings",{{"enableIBL",view.settings.enableIBL},
			{"indirectStrength",view.settings.indirectStrength},{"ambientOcclusionStrength",view.settings.ambientOcclusionStrength},
			{"ambientOcclusionQuality",view.settings.ambientOcclusionQuality},{"variableRateShading",view.settings.variableRateShading},
			{"debugEnabled",view.settings.debugEnabled},{"maxLodDrop",view.settings.virtualGeometry.maxLodDrop},{"lodErrorScale",view.settings.virtualGeometry.lodErrorScale}}}};
		for(const auto& setting:settings.values) controls["effectiveValues"][std::to_string(static_cast<std::uint32_t>(setting.id))]={{"kind",setting.value.index()},{"value",ValueEvidence(setting.value)}};
		proof["normalizedSimulationState"]=simulation; proof["normalizedEffectiveSettings"]=controls;
		const auto simulationText=simulation.dump(),controlsText=controls.dump();
		proof["simulationStateSha256"]=Hash({reinterpret_cast<const std::uint8_t*>(simulationText.data()),simulationText.size()});
		proof["effectiveSettingsSha256"]=Hash({reinterpret_cast<const std::uint8_t*>(controlsText.data()),controlsText.size()});
		}
		const auto serialized=proof.dump(); proof["preparedStateSha256"]=Hash({reinterpret_cast<const std::uint8_t*>(serialized.data()),serialized.size()});
		return proof;
	}
	Json ValidateInputs(const std::filesystem::path& path)
	{
		const auto bytes=Bytes(path); Check(Hash(bytes)==kFixtureHash,"Frozen fixture hash differs.");
		const auto fixture=Json::parse(bytes); Check(fixture.at("fixtureId")=="moving-edges-v1" && fixture.at("simulationHz")==60
			&& fixture.at("warmupCount")==64 && fixture.at("continuousPhases").size()==120u,"Frozen fixture timing differs.");
		const auto& controls=fixture.at("controls"); Check(controls.at("renderRoute")=="indexed-raster" && controls.at("fixedExposureEV")==0
			&& controls.at("material")=="opaque-emissive" && controls.at("doubleSided")==true && controls.at("overlays")==false
			&& controls.at("bloom")==false && controls.at("grading")==false && controls.at("toneMode")=="Legacy","Frozen controls differ.");
		for(std::uint32_t phase=0;phase!=184u;++phase)
		{
			const auto& record=fixture.at("poses").at(phase); const auto& pose=record.at("pose");
			Check(record.at("phase")==phase && record.at("sequence")=="continuous" && record.at("viewGeneration")==1
				&& pose.at("simulationStep")==phase && pose.at("projection")=="orthographic" && pose.at("verticalSpanWorldUnits")==360
				&& pose.at("geometry").size()==3u,"Frozen continuous pose shape differs.");
			for(const auto& geometry:pose.at("geometry"))
			{
				Check(geometry.at("verticesTicks").size()==4u,"Frozen quad no longer contains four vertices.");
				for(const auto& vertex:geometry.at("verticesTicks"))
					for(const auto& coordinate:vertex) { const auto value=coordinate.get<std::int64_t>(); Check(static_cast<std::int64_t>(static_cast<float>(value)/4096.0f*4096.0f)==value,"Frozen coordinate is not exactly represented by the float32 input grid."); }
			}
		}
		Check(DecodeBase64("AAEC/w==")==std::vector<std::uint8_t>({0u,1u,2u,255u}),"Lossless capture transport decoder changed bytes.");
		return {{"status","PassedInputValidationOnly"},{"fixtureSha256",kFixtureHash},{"continuousPosesChecked",184},
			{"geometryVertexCoordinatesChecked",4416},{"gpuExecuted",false},{"qualificationComplete",false}};
	}
	void AssembleSpatial(const std::filesystem::path& root)
	{
		// This is transport assembly, not image synthesis or scoring. Each source
		// collection must already prove the real indexed draw count at every pose.
		// Exploratory captures cannot be upgraded by changing a manifest label.
		Check(std::filesystem::is_directory(root) && !std::filesystem::exists(root/"manifest.json"),"Assembly needs an owned directory and fresh manifest path.");
		Json manifest{{"schemaVersion",1},{"evidenceKind","NativeBackendReadback"},{"fixtureFile","MovingEdgesPoses.json"},
			{"fixtureSha256",kFixtureHash},{"runs",Json::object()},{"resetPairs",Json::array()},{"sourceCollections",Json::object()},
			{"qualificationComplete",false}};
		std::string commonProfile;
		for(const std::string name:{"Reference4x","OffNative","FXAA","SMAA","CMAA2"})
		{
			const auto directory=root/Path(name); const auto collection=ReadJson(directory/"collection.json");
			Check(collection.at("status")=="CapturedNativeIndexedSpatialRun" && collection.at("controlsVerified")==true
				&& collection.at("evidenceKind")=="NativeBackendReadback" && collection.at("mode")==name
				&& collection.at("frozenIndexedRouteSatisfied")==true && collection.at("actualRoute")=="indexed-raster"
				&& collection.at("fixtureSha256").get<std::string>()==kFixtureHash,"A source run lacks real frozen indexed spatial evidence: "+name);
			const auto frozen=Bytes(directory/"MovingEdgesPoses.json"); Check(Hash(frozen)==kFixtureHash,"Collected frozen asset differs.");
			const auto fixture=Json::parse(frozen); const bool reference=name=="Reference4x";
			const Json expected{{"mode",reference || name=="OffNative" ? "Off" : name},{"sampleCount",1},
				{"internalExtent",reference ? Json{2560,1440} : Json{640,360}},{"outputExtent",reference ? Json{2560,1440} : Json{640,360}}};
			Check(collection.at("effective")==expected && collection.at("controlsSha256")==fixture.at("controlsSha256"),"Native mode/extents/control hash differ from the requested run: "+name);
			if(commonProfile.empty()) commonProfile=collection.at("commonSourceProfileSha256").get<std::string>();
			Check(collection.at("commonSourceProfileSha256")==commonProfile,"Reference/candidate source profiles differ.");
			Check(collection.at("warmupRenderReceipts").size()==64u && collection.at("frames").size()==120u,"Incomplete native sequence: "+name);
			Json run{{"effective",collection.at("effective")},{"controlsSha256",collection.at("controlsSha256")},
				{"warmupRenderReceipts",collection.at("warmupRenderReceipts")},{"frames",Json::array()}};
			for(std::size_t index=0;index!=184u;++index)
			{
				const auto& record=index<64u ? collection.at("warmupRenderReceipts").at(index) : collection.at("frames").at(index-64u);
				Check(record.at("phase")==index && record.at("nativeRenderAccepted")==true && record.at("effective")==run.at("effective")
					&& record.at("poseSha256")==fixture.at("poses").at(index).at("poseSha256")
					&& record.at("preparedState").at("frozenIndexedRouteSatisfied")==true
					&& record.at("preparedState").at("nativeIndexedDrawCount")==4u
					&& record.at("preparedState").at("effectiveGeometryRasterMode")==1.0f,
					"Native indexed phase/configuration proof is missing: "+name+" phase "+std::to_string(index));
				if(index<64u) continue;
				for(const auto stage:{"raw","processed"})
				{
					const auto& image=record.at(stage); const auto expected=std::string(stage)+"-"+std::to_string(index)+".png";
					Check(image.at("file")==expected && image.at("nativeBackendReadback")==true && image.at("deterministicFallback")==false
						&& image.at("backendFrameId")==record.at("backendFrameId")
						&& Hash(Bytes(directory/Path(expected)))==image.at("sha256"),"Native PNG bytes/provenance changed: "+name);
				}
				Json frame=record.at("processed"); frame["file"]=name+"/"+frame.at("file").get<std::string>();
				for(const auto field:{"sequence","phase","viewGeneration","poseSha256","nativeRendererSession","nativeViewId","nativeViewGeneration","effective"})
					frame[field]=record.at(field);
				frame["preparedStateSha256"]=record.at("preparedState").at("preparedStateSha256");
				run["frames"].push_back(std::move(frame));
			}
			manifest["runs"][name]=std::move(run);
			manifest["sourceCollections"][name]={{"file",name+"/collection.json"},{"sha256",Hash(Bytes(directory/"collection.json"))}};
			if(!std::filesystem::exists(root/"MovingEdgesPoses.json")) Write(root/"MovingEdgesPoses.json",frozen);
			else Check(Hash(Bytes(root/"MovingEdgesPoses.json"))==kFixtureHash,"Owned assembly fixture differs.");
		}
		WriteJson(root/"manifest.json",manifest);
	}
}

int main(int count,char** arguments)
{
	Json evidence{{"schemaVersion",1},{"evidenceKind","NativeExploratoryDirectMeshReadback"},{"qualificationComplete",false},
		{"controlsVerified",false},{"frozenIndexedRouteSatisfied",false},{"status","Incomplete"},{"warmupRenderReceipts",Json::array()},
		{"frames",Json::array()},{"setupRenderReceipts",Json::array()},{"editorPresentationReceipts",Json::array()},
		{"commonControlReceipts",Json::array()},{"sceneAAReceipts",Json::array()}};
	std::filesystem::path output;
	try
	{
		if(count==3 && (std::string_view(arguments[1])=="--describe-capture-mode" || std::string_view(arguments[1])=="--describe-game-capture-mode"))
		{
			Options options;options.mode=arguments[2];options.gameView=std::string_view(arguments[1])=="--describe-game-capture-mode";SelectMode(options);
			Check(!options.gameView || options.temporalUpscale,"Independent native Game description requires TemporalUpscale.");
			Json result{{"mode",options.mode},{"engineInitialized",false},{"gpuExecuted",false},
				{"internalExtent",{InternalExtent(options).x,InternalExtent(options).y}},{"outputExtent",{options.extent.x,options.extent.y}},
				{"sampleCount",options.samples},{"standaloneAAMode",options.aa},{"temporalUpscale",options.temporalUpscale},
				{"projectionWorldSpan",{640,360}},{"values",Json::array()}};
			if(options.gameView) { result["scope"]="Game"; result["nativePoseSequenceSupported"]=false; result["nativePoseJoin"]="ExactAcceptedFrameProjectionViewAndCanonicalOrigin"; }
			for(const auto& patch:SceneRequestValues(options.aa,options.temporalUpscale))
				result["values"].push_back({{"id",static_cast<std::uint32_t>(patch.id)},{"variant",patch.value.index()},{"value",ValueEvidence(patch.value)}});
			std::cout<<result.dump(2)<<'\n';return 0;
		}
		if(count==3 && std::string_view(arguments[1])=="--assemble-spatial")
		{
			AssembleSpatial(Path(arguments[2])); std::cout<<"Assembled verified native indexed spatial captures; image scoring remains separate.\n"; return 0;
		}
		if(count==3 && std::string_view(arguments[1])=="--validate-inputs")
		{
			std::cout<<ValidateInputs(Path(arguments[2])).dump(2)<<'\n'; return 0;
		}
		if(count==3 && std::string_view(arguments[1])=="--validate-cooked")
		{
			Options options; options.project=Path(arguments[2]);
			const auto resources=RegisterCooked(options);
			std::cout<<Json{{"status","PassedCookedRegistrationOnly"},{"gpuExecuted",false},{"registeredResources",resources}}.dump(2)<<'\n'; return 0;
		}
		auto options=Parse(count,arguments); output=options.output; std::filesystem::create_directories(output);
		evidence["inputValidation"]=ValidateInputs(options.fixture);
		const auto fixtureBytes=Bytes(options.fixture); Check(Hash(fixtureBytes)==kFixtureHash,"Frozen pose asset byte hash differs.");
		const auto fixture=Json::parse(fixtureBytes); Check(fixture.at("warmupCount")==64 && fixture.at("continuousPhases").size()==120u,"Frozen phase counts differ.");
		Write(output/"MovingEdgesPoses.json",fixtureBytes);
		const bool separate=!options.temporalSequence.empty(),freshReference=options.temporalRole=="fresh";
		const Json* separateInput=separate ? &fixture.at("separateSequences").at(options.temporalSequence) : nullptr;
		if(separate) {
			Check(separateInput->at("resetPhase")==0 && separateInput->at("frames").size()==4u,"Frozen reset sequence shape differs.");
			if(freshReference) { const auto& e=separateInput->at("frames").at(0).at("pose").at("outputExtent"); options.extent={e.at(0).get<std::uint32_t>(),e.at(1).get<std::uint32_t>()}; }
			evidence["temporalResetSequence"]={{"name",options.temporalSequence},{"role",options.temporalRole},{"crop",separateInput->at("crop")},
				{"freshLogicalViewGeneration",separateInput->at("freshLogicalViewGeneration")},{"resetPhase",0},{"pixelComparisonPerformed",false},{"skippedBoundaryFrames",Json::array()}};
		}
		if(options.gameView)
		{
			evidence["nativeOutputLane"]="Game"; evidence["graphicsSettingsScope"]="Game";
			evidence["nativePoseSequenceSupported"]=false;
			evidence["nativePoseJoin"]="ExactAcceptedFrameProjectionViewAndCanonicalOrigin";
			evidence["managedPlayAcceptanceVerified"]=false; evidence["exportRuntimeAcceptanceVerified"]=false;
			evidence["gameAAReceipts"]=Json::array();
		}
		evidence["mode"]=options.mode; evidence["fixtureSha256"]=kFixtureHash; evidence["controlsSha256"]=fixture.at("controlsSha256");
		evidence["effective"]={{"mode",options.temporalUpscale ? "TemporalUpscale" : options.aa==0u ? "Off" : options.mode},{"sampleCount",options.samples},{"internalExtent",{InternalExtent(options).x,InternalExtent(options).y}},{"outputExtent",{options.extent.x,options.extent.y}}};
		evidence["actualRoute"]="ordinary-direct-mesh-task";
		evidence["qualificationBlockers"]=Json::array();
		evidence["qualificationBlockers"].push_back("Frozen controls require indexed-raster; exploratory lane uses ordinary native mesh-task draws.");
		evidence["qualificationBlockers"].push_back(Temporal(options)
			? "Continuous temporal command/capture joins do not certify motion pixels, disocclusion, reset correctness or temporal image-quality metrics."
			: "Current spatial capture does not certify temporal previous-state continuity.");
		if(Temporal(options))
			evidence["temporalContinuousEvidence"]={{"warmupPhasesWithAcceptedPredecessor",Json::array()},
				{"measuredPhasesWithAcceptedPredecessor",Json::array()},{"zeroPreviousFramePhases",Json::array()},
				{"motionPixelsVerified",false},{"disocclusionVerified",false},{"historyResetVerified",false},{"temporalImageQualityScored",false}};
		auto& resources=ResourceManager::Instance(); resources.SetEngineRoot(options.install/"Editor"); resources.SetAssetRoot(options.project/"Build/Assets");
		Check(glfwInit()==GLFW_TRUE,"GLFW initialization failed."); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
		Engine engine; struct Shutdown { Engine& engine; ~Shutdown() { engine.Shutdown(); } } shutdown{engine}; engine.Initialize();
		evidence["cookedAssets"]=RegisterCooked(options);
		evidence["scene"]={{"path",options.scene.generic_string()},{"sha256",Hash(Bytes(options.project/options.scene))}};
		Json commonSource{{"fixtureSha256",kFixtureHash},{"sceneSha256",evidence["scene"]["sha256"]},{"cookedPayloads",Json::object()},
			{"projectionWorldSpan",{640,360}},{"nearFar",{0.1f,16.0f}},{"fixedExposureEV",0},{"toneMode","Legacy"},
			{"grade",false},{"bloom",false},{"sampleCount",options.samples},{"internalResolutionScale",options.temporalUpscale ? 0.5f : 1.0f},{"meshLodScale",64.0f},
			{"rasterRoute","ordinary-direct-mesh-task"}};
		for(const auto& asset:evidence["cookedAssets"]) commonSource["cookedPayloads"][asset.at("guid").get<std::string>()]=asset.at("sha256");
		if(options.gameView) commonSource["nativeOutputLane"]="Game";
		const auto sourceText=commonSource.dump(); evidence["commonSourceProfile"]=commonSource;
		evidence["commonSourceProfileSha256"]=Hash({reinterpret_cast<const std::uint8_t*>(sourceText.data()),sourceText.size()});
		auto& scenes=SceneManager::Instance(); Check(scenes.SetActiveScene(scenes.LoadScene(options.project/options.scene)),"Imported fixture scene activation failed.");
		auto inputs=DiscoverObjects();
		if(options.gameView)
		{
			// Ordinary runtime-only scene authoring at the joined owner boundary.
			// This supplies genuine camera authority without changing cooked meshes,
			// imported payloads or the saved scene. No renderer identity is invented.
			auto& objects=ObjectManager::instance();
			inputs.gameCamera=objects.createObject("MovingEdges Native Game Camera");
			auto* camera=objects.AddComponent<CameraComponent>(*inputs.gameCamera);
			Check(camera,"Could not create ordinary runtime Game CameraComponent.");
			camera->nearPlane=0.1f;camera->farPlane=16.0f;camera->aspectRatio=640.0f/360.0f;
			const auto authority=GameCameraIdentity(*inputs.gameCamera);
			evidence["gameCameraAuthority"]={{"cameraId",TerraEngine::ToHexString(authority.cameraId)},
				{"sceneGeneration",std::to_string(authority.sceneGeneration)},{"objectLifetimeGeneration",std::to_string(authority.objectLifetimeGeneration)},
				{"componentGeneration",std::to_string(authority.componentGeneration)},{"bindingGeneration",std::to_string(authority.bindingGeneration)},
				{"runtimeOnly",true},{"savedSceneWritten",false}};
		}
		auto& renderer=RenderSystem::Instance();
		renderer.SetEditorModeEnabled(true);
		if(options.gameView) renderer.SetGameViewTargetSize(options.extent);
		else { renderer.SetSceneViewTargetSize(options.extent); renderer.SetSceneViewGridVisible(false); }
		Check(!renderer.GetVirtualGeometryValidationRouteOverride(),"Fixture must not inherit a diagnostic raster override.");
		auto* context=dynamic_cast<VulkanContext*>(renderer.GetContext()); Check(context,"Native Vulkan context is unavailable."); context->EnableEditorMode();
		const auto adapter=context->GetLatestRendererRecoverySnapshot(); evidence["adapter"]={{"name",adapter.hardwareName},{"driver",adapter.driverVersion},{"api",adapter.apiVersion}};
		const auto capability=context->ReadGraphicsCatalogCapabilities(false);
		Check(options.temporalUpscale ? capability.temporalUpscalingSupported
			: options.aa==4u ? capability.msaa4Supported
			: Temporal(options) ? capability.taaSupported
			: (capability.spatialAAModeMask&(1u<<options.aa))!=0u,"Requested AA/reconstruction mode is not natively available.");
		std::uint64_t sequence=0u,lastAcceptedSceneFrame=0u;
		std::set<std::uint64_t> acceptedSceneFrames;
		// Binding/settings setup is explicitly separate from the 64 frozen warmup
		// renders. Animation is held, so setup cannot consume measured pose steps.
		const auto& initial=freshReference ? separateInput->at("frames").at(0).at("pose") : fixture.at("poses").at(0).at("pose");
		const auto cameraTicks=initial.at("cameraPositionTicks"); TerraEngine::Scene::WorldPosition initialPosition;
		initialPosition.offsetMetres={cameraTicks.at(0).get<double>()/4096.0,0.0,0.0};
		auto projection=glm::orthoRH_ZO(-320.0f,320.0f,-180.0f,180.0f,0.1f,16.0f); projection[1][1]*=-1.0f;
		auto update=[&](const glm::mat4& view,const glm::mat4& proj,const TerraEngine::Scene::WorldPosition& position,float dt,bool measured=false)
		{
			// Viewport demand is one-frame ingress, just like the override camera.
			// RenderSystem clears extent, grid, and suppression at frame completion;
			// the native host must reissue them as an actual visible panel would.
			if(options.gameView)
			{
				renderer.SetGameViewTargetSize(options.extent); renderer.SetGameViewSuppressed(false);
				Check(inputs.gameCamera.has_value(),"Native Game camera object is absent.");
				auto& objects=ObjectManager::instance();auto pose=objects.GetCanonicalWorldPose(*inputs.gameCamera);
				Check(pose.has_value(),"Native Game camera has no canonical Transform.");pose->position=position;
				Check(objects.SetCanonicalWorldPose(*inputs.gameCamera,*pose)==TerraEngine::Scene::WorldCoordinateError::None,
					"Frozen native Game camera canonical pose was rejected.");
				renderer.SetGameViewCamera(view,proj,position,GameCameraIdentity(*inputs.gameCamera)); ++sequence;
			}
			else
			{
				renderer.SetSceneViewTargetSize(options.extent);
				renderer.SetSceneViewGridVisible(false);
				renderer.SetGameViewSuppressed(true);
				renderer.SetSceneViewCamera(view,proj,position,true,0.0f,++sequence);
			}
			Check(engine.Update(dt),"Normal Engine frame failed."); CompleteEditor(*context,evidence["editorPresentationReceipts"]);
			const auto reference=renderer.GetLatestRenderReferenceFrame(CaptureView(options));
			const auto nativeFrame=options.gameView ? (reference ? reference->frameId : 0u) : context->GetSceneViewPostProcessPreview().frameId;
			const auto accepted=Accepted(*context,nativeFrame);
			bool fresh=accepted && nativeFrame>lastAcceptedSceneFrame && accepted->frameId==nativeFrame;
			bool matchingPose=false;
			if(accepted) for(const auto& input:accepted->presentationInputs)
				if(input.output.kind==CaptureOutput(options) && input.output.targetSize==options.extent && input.referenceFrame.frameId==accepted->frameId)
				{
					if(!options.gameView) matchingPose=input.poseSequence==sequence;
					else
					{
						matchingPose=ExactGamePose(input,PoseInput{view,proj,position},options.extent,accepted->frameId);
					}
				}
			fresh=fresh && matchingPose;
			if(!measured)
			{
				evidence["setupRenderReceipts"].push_back({{options.gameView ? "fixtureIngressSequence" : "poseSequence",std::to_string(sequence)},
					{"rendererReferenceFrameId",std::to_string(reference ? reference->frameId : 0u)},
					{options.gameView ? "nativeGameFrameId" : "nativeSceneFrameId",std::to_string(nativeFrame)},{"exactAcceptedPose",matchingPose},
					{"newNativeRenderAccepted",fresh},{"scheduledDeltaSeconds",dt}});
				// A UI-only completion never counts as a new camera render. Setup
				// keeps the real imported animation stopped at authored phase zero.
				auto& animation=TerraEngine::Animation::AnimationSystem::Instance();
				for(const auto object:inputs.animators)
				{
					const auto animator=animation.ResolveAnimatorForObject(ObjectManager::instance().getUUID(object));
					if(animation.IsAnimatorInstanceAlive(animator))
						Check(animation.LoadAnimatorPlaybackState(animator).resolvedTick==0,"Settings setup advanced the frozen animation pose.");
				}
			}
			else Check(fresh,"Measured fixed-step Engine update did not publish a fresh native Scene frame with its exact authored pose.");
			if(fresh) { lastAcceptedSceneFrame=nativeFrame; acceptedSceneFrames.insert(nativeFrame); }
			return nativeFrame;
		};
		if(freshReference) {
			const auto finalExtent=options.extent; evidence["temporalResetSequence"]["retiredSetupViews"]=Json::array();
			std::set<std::uint64_t> setupLifetimes;
			// These are real Off renders at distinct extents, not fabricated native
			// lifetime IDs. Only after retiring them do we configure final TAA.
			for(const auto extent:std::array{glm::uvec2{638u,358u},glm::uvec2{642u,362u},glm::uvec2{636u,356u}}) {
				options.extent=extent; const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20); bool observed=false;
				while(std::chrono::steady_clock::now()<deadline) {
					const auto frame=update(glm::mat4(1.0f),projection,initialPosition,1.0f/60.0f);
					const auto accepted=Accepted(*context,frame); if(!accepted) continue;
					for(std::size_t i=0u;i<accepted->presentationInputs.size();++i) {
						const auto& view=accepted->presentationInputs[i];
						const bool matches=view.output.kind==CaptureOutput(options) && view.output.targetSize==extent
							&& view.poseSequence==(options.gameView ? 0u : sequence) && view.graphicsView;
						if(!matches)
						{
							// Preserve bounded actual evidence of the first failing predicate.
							// A successful matrix join alone cannot invent a graphics lifetime.
							if(options.gameView)
							{
								auto& witnesses=evidence["temporalResetSequence"]["rejectedSetupViewWitnesses"];
								if(witnesses.is_null())witnesses=Json::array();
								if(witnesses.size()<8u)
								{
									Json witness{{"frameId",std::to_string(frame)},
										{"actualOutputKind",static_cast<unsigned>(view.output.kind)},{"expectedOutputKind",static_cast<unsigned>(CaptureOutput(options))},
										{"actualExtent",{view.output.targetSize.x,view.output.targetSize.y}},{"expectedExtent",{extent.x,extent.y}},
										{"actualPoseSequence",std::to_string(view.poseSequence)},{"hasGraphicsCamera",view.graphicsCamera.has_value()},
										{"hasGraphicsView",view.graphicsView.has_value()}};
									if(view.graphicsCamera) witness["actualCameraId"]=TerraEngine::ToHexString(view.graphicsCamera->cameraId);
									if(inputs.gameCamera) witness["expectedCameraId"]=TerraEngine::ToHexString(GameCameraIdentity(*inputs.gameCamera).cameraId);
									witnesses.push_back(std::move(witness));
								}
							}
							continue;
						}
						const auto native=context->ReadRecordedGraphicsViews(frame); const auto actual=std::find_if(native.begin(),native.end(),[&](const auto& v){return v.publicationIndex==i;});
						Check(actual!=native.end() && actual->temporalRecordingId==0u && actual->rasterSampleCount==1u,"Fresh setup unexpectedly recorded TAA or a non-native raster target.");
						Check(setupLifetimes.insert(view.graphicsView->lifetimeGeneration).second,"Fresh setup resize did not retire its real Scene view lifetime.");
						evidence["temporalResetSequence"]["retiredSetupViews"].push_back({{"frameId",std::to_string(frame)},{"nativeRendererSession",std::to_string(accepted->rendererSessionGeneration)},
							{"nativeViewId",std::to_string(view.graphicsView->outputId)},{"nativeViewGeneration",std::to_string(view.graphicsView->lifetimeGeneration)},{"extent",{extent.x,extent.y}},{"temporalRecordingId","0"}});
						observed=true; break;
					}
					if(observed) break;
				}
				Check(observed,"Could not prove a real fresh-reference Off setup target.");
			}
			options.extent=finalExtent;
		}
		update(glm::mat4(1.0f),projection,initialPosition,1.0f/60.0f);
		GraphicsSettingsRequest request; request.lifecycle=renderer.GetGraphicsSettingsLifecycleSnapshot();
		request.patches={{GraphicsSettingId::ExposureMode,std::uint32_t{1}},{GraphicsSettingId::FixedExposureCompensation,0.0f},
			{GraphicsSettingId::ToneMappingMode,std::uint32_t{0}},{GraphicsSettingId::BloomEnabled,false},{GraphicsSettingId::ColorGradingEnabled,false},
			{GraphicsSettingId::EnableIBL,false},{GraphicsSettingId::IndirectStrength,0.0f},
			{GraphicsSettingId::VirtualGeometryMaxLodDrop,std::uint32_t{0}},{GraphicsSettingId::VirtualGeometryLodErrorScale,64.0f}};
		const auto catalog=renderer.GetGraphicsSettingsCatalogSnapshot(request.target);
		evidence["commonControlCatalogPreflight"]=Json::array();
		for(const auto& patch:request.patches)
		{
			const auto label=std::to_string(static_cast<std::uint32_t>(patch.id))+" ("+std::string(CommonControlName(patch.id))+")";
			const auto* descriptor=FindGraphicsSetting(catalog,patch.id);
			if(!descriptor)
			{
				evidence["commonControlCatalogPreflight"].push_back({{"id",static_cast<std::uint32_t>(patch.id)},
					{"expectedName",CommonControlName(patch.id)},{"missingFromActualDiscovery",true},{"submittedVariant",patch.value.index()}});
				throw std::runtime_error("Common control missing from actual discovery: "+label);
			}
			Json row{{"id",static_cast<std::uint32_t>(patch.id)},{"name",descriptor->name},
				{"actualValueKind",static_cast<unsigned>(descriptor->valueKind)},{"submittedVariant",patch.value.index()},
				{"available",descriptor->available},{"gameScope",descriptor->gameScope},{"detail",descriptor->detail},
				{"submittedValue",ValueEvidence(patch.value)},{"minimum",descriptor->minimum ? ValueEvidence(*descriptor->minimum) : Json(nullptr)},
				{"maximum",descriptor->maximum ? ValueEvidence(*descriptor->maximum) : Json(nullptr)},{"options",Json::array()}};
			for(const auto& option:descriptor->options) row["options"].push_back(ValueEvidence(option));
			evidence["commonControlCatalogPreflight"].push_back(row);
			// Discovery is authoritative: never submit a future setting ID merely
			// because its enum or a validator placeholder already exists in source.
			Check(patch.value.index()==static_cast<std::size_t>(descriptor->valueKind),"Common control caller variant differs from discovery: "+label);
			Check(descriptor->available && descriptor->gameScope,"Common control unavailable in Game scope: "+label+" "+descriptor->detail);
			Check(WithinBound(patch.value,descriptor->minimum,true) && WithinBound(patch.value,descriptor->maximum,false),"Common control exceeds actual catalog bounds: "+label);
			Check(descriptor->options.empty() || std::find(descriptor->options.begin(),descriptor->options.end(),patch.value)!=descriptor->options.end(),"Common control is outside actual catalog options: "+label);
		}
		const auto gameReceipt=renderer.QueueGraphicsSettingsRequest(request);
		evidence["commonControlReceipts"].push_back({{"phase","admission"},{"receipt",ReceiptEvidence(gameReceipt)}});
		RequireViableReceipt(gameReceipt,"Common color controls admission failed"); Check(gameReceipt.requestId!=0u,"Control settings were not admitted.");
		bool settingsReady=false;
		std::uint64_t activationFrame=0u,activationSequence=0u; PoseInput activationPose;
		const auto colorDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
		while(std::chrono::steady_clock::now()<colorDeadline)
		{
			const auto frame=update(glm::mat4(1.0f),projection,initialPosition,1.0f/60.0f);
			const auto receipt=renderer.GetGraphicsSettingsReceipt(gameReceipt.requestId);
			if(receipt)
			{
				evidence["commonControlReceipts"].push_back({{"phase","polled"},{"frameId",std::to_string(frame)},{"receipt",ReceiptEvidence(*receipt)}});
				RequireViableReceipt(*receipt,"Common color controls rejected");
			}
			if(receipt && receipt->state==GraphicsRequestState::Applied) { settingsReady=true; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Check(settingsReady,"Color controls never reached an actual effective publication.");
		const auto patch=SceneRequestValues(options.aa,options.temporalUpscale);
		GraphicsRequestReceipt aaReceipt;
		if(options.gameView)
		{
			GraphicsSettingsRequest gameRequest; gameRequest.lifecycle=renderer.GetGraphicsSettingsLifecycleSnapshot(); gameRequest.patches=patch;
			aaReceipt=renderer.QueueGraphicsSettingsRequest(gameRequest);
		}
		else
		{
			const auto sceneState=renderer.ReadSceneViewGraphicsSettings(); Check(sceneState.has_value(),"Actual Scene settings owner is unavailable.");
			aaReceipt=renderer.QueueSceneViewGraphicsSettings(sceneState->lifecycle,sceneState->view,sceneState->revision,patch);
		}
		const auto* aaReceiptKey=options.gameView ? "gameAAReceipts" : "sceneAAReceipts";
		evidence[aaReceiptKey].push_back({{"phase","admission"},{"receipt",ReceiptEvidence(aaReceipt)}});
		RequireViableReceipt(aaReceipt,options.gameView ? "Game reconstruction admission failed" : "Scene AA admission failed");
		settingsReady=false;
		const auto aaDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
		while(std::chrono::steady_clock::now()<aaDeadline)
		{
			const auto pose=freshReference ? ApplyPose(inputs,initial,sequence+1u) : PoseInput{glm::mat4(1.0f),projection,initialPosition};
			const auto frame=update(pose.view,pose.projection,pose.camera,1.0f/60.0f);
			const auto receipt=options.gameView ? renderer.GetGraphicsSettingsReceipt(aaReceipt.requestId) : renderer.ReadSceneViewGraphicsSettingsReceipt(aaReceipt.requestId);
			if(receipt)
			{
				evidence[aaReceiptKey].push_back({{"phase","polled"},{"frameId",std::to_string(frame)},{"receipt",ReceiptEvidence(*receipt)}});
				RequireViableReceipt(*receipt,options.gameView ? "Game reconstruction rejected" : "Scene AA rejected");
			}
			if(receipt && receipt->state==GraphicsRequestState::Applied) { settingsReady=true; activationFrame=frame; activationSequence=sequence; activationPose=pose; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Check(settingsReady,options.gameView ? "Game reconstruction did not reach an actual effective publication." : "Scene AA did not reach an actual effective publication.");
		if(options.depthOfFieldNative)
		{
			// All cases use the exact same frozen static pose and real opaque
			// geometry/depth. Only typed lens controls change between cases.
			struct LensCase { const char* name; bool enabled; float focus,strength; };
			const std::array cases{LensCase{"baseline-off",false,1.0f,12.0f},LensCase{"near-focus",true,1.0f,12.0f},
				LensCase{"far-focus",true,8.0f,12.0f},LensCase{"zero-strength",true,8.0f,0.0f},LensCase{"disabled",false,8.0f,12.0f}};
			evidence["depthOfFieldNative"]={{"cases",Json::array()},{"hudUnaffectedVerified",false},{"pickingUnaffectedVerified",false},
				{"poseSha256",fixture.at("poses").at(0).at("poseSha256")},{"fixedSimulationStep",0},
				{"knownDepthBands",{{"nearRibbonMetres",1},{"rigidLineMetres",2},{"panelMetres",3},{"flatBackgroundMetres",8}}},
				{"farBackgroundEdgeFocusVerified",false},{"qualityAcceptancePerformed",false}};
			std::vector<std::uint8_t> referenceRaw,nearImage,farImage;
			for(const auto& lens:cases)
			{
				GraphicsSettingsRequest lensRequest; lensRequest.lifecycle=renderer.GetGraphicsSettingsLifecycleSnapshot();
				lensRequest.patches={{GraphicsSettingId::DepthOfFieldEnabled,lens.enabled},{GraphicsSettingId::DepthOfFieldFocusDistance,lens.focus},
					{GraphicsSettingId::DepthOfFieldFocusRange,0.25f},{GraphicsSettingId::DepthOfFieldBlurStrength,lens.strength}};
				const auto admission=renderer.QueueGraphicsSettingsRequest(lensRequest); RequireViableReceipt(admission,"Native DOF admission failed");
				Check(admission.requestId!=0u,"Native DOF did not receive a real request ID.");
				Json row{{"case",lens.name},{"admission",ReceiptEvidence(admission)},{"focusDistanceMetres",lens.focus},
					{"focusRangeMetres",.25f},{"blurStrengthPixels",lens.strength},{"enabled",lens.enabled}};
				const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
				std::uint64_t frame=0u; Json proof; bool ready=false;
				while(std::chrono::steady_clock::now()<deadline)
				{
					const auto pose=ApplyPose(inputs,initial,sequence+1u); frame=update(pose.view,pose.projection,pose.camera,1.0f/60.0f,true);
					const auto receipt=renderer.GetGraphicsSettingsReceipt(admission.requestId); if(!receipt) continue;
					RequireViableReceipt(*receipt,"Native DOF request rejected"); if(receipt->state!=GraphicsRequestState::Applied) continue;
					const auto accepted=Accepted(*context,frame); Check(static_cast<bool>(accepted),"Native DOF accepted publication is absent.");
					const auto view=std::find_if(accepted->presentationInputs.begin(),accepted->presentationInputs.end(),[](const auto& v){return v.output.kind==RenderOutputKind::SceneView;});
					if(view==accepted->presentationInputs.end() || !view->graphicsSettings) continue;
					const auto& settings=*view->graphicsSettings;
					if(Value(settings,GraphicsSettingId::DepthOfFieldEnabled,-1)!=(lens.enabled ? 1.0f : 0.0f)
						|| Value(settings,GraphicsSettingId::DepthOfFieldFocusDistance,-1)!=lens.focus
						|| Value(settings,GraphicsSettingId::DepthOfFieldFocusRange,-1)!=.25f
						|| Value(settings,GraphicsSettingId::DepthOfFieldBlurStrength,-1)!=lens.strength) continue;
					Check(receipt->appliedFrameId!=0u && receipt->effectiveRevision!=0u && frame>=receipt->appliedFrameId,"DOF terminal receipt lacks actual publication.");
					// Neutral exposure can keep the shared HDR group active when DOF is Off.
					// The exact copied DOF tuple above and named completed GPU timing below
					// establish DOF activation independently from unrelated HDR stages.
					proof=VerifyPrepared(renderer,*accepted,inputs,initial,pose,options,sequence);
					row["receipt"]=ReceiptEvidence(*receipt); row["preparedState"]=proof;
					row["backendFrameId"]=std::to_string(frame); ready=true; break;
				}
				Check(ready,"DOF never became effective on the exact frozen native Scene view.");
				const auto raw=Capture(*context,frame,options.extent,RenderCaptureImageStage::Raw,std::string(lens.name)+"-raw");
				const auto processed=Capture(*context,frame,options.extent,RenderCaptureImageStage::Processed,std::string(lens.name)+"-processed");
				Check(raw.backendTarget->rendererSessionGeneration==processed.backendTarget->rendererSessionGeneration
					&& raw.backendTarget->targetGeneration==processed.backendTarget->targetGeneration
					&& std::to_string(raw.graphicsSettingsRevision)==proof.at("settingsRevision")
					&& raw.graphicsSettingsRevision==processed.graphicsSettingsRevision,"DOF native images have different source/settings provenance.");
				const auto rawDecoded=TerraEngine::TestSupport::DecodeCaptureBase64(raw.base64Data);
				const auto processedDecoded=TerraEngine::TestSupport::DecodeCaptureBase64(processed.base64Data);
				Check(rawDecoded.width==640u && rawDecoded.height==360u && processedDecoded.width==640u && processedDecoded.height==360u,"DOF decoded extent differs.");
				if(referenceRaw.empty()) referenceRaw=rawDecoded.rgba;
				Check(referenceRaw==rawDecoded.rgba,"DOF controls changed the frozen raw geometry/color input."); row["rawMatchesBaseline"]=true;
				if(!lens.enabled || lens.strength==0.0f) { Check(rawDecoded.rgba==processedDecoded.rgba,"Off/zero DOF changed decoded native pixels."); row["offOrZeroNativePixelParity"]=true; }
				else { Check(rawDecoded.rgba!=processedDecoded.rgba,"Enabled DOF did not change any actual native image pixels."); if(lens.focus==1.0f) nearImage=processedDecoded.rgba; else farImage=processedDecoded.rgba; }
				for(const auto* capture:{&raw,&processed}) {
					const auto bytes=DecodeBase64(capture->base64Data); const auto name=std::string(lens.name)+(capture==&raw ? "-raw.png" : "-processed.png"); Write(output/Path(name),bytes);
					row[capture==&raw ? "raw" : "processed"]={{"file",name},{"sha256",Hash(bytes)},{"backendFrameId",std::to_string(frame)},
						{"sourceTargetGeneration",std::to_string(capture->backendTarget->targetGeneration)},{"rendererSession",std::to_string(capture->backendTarget->rendererSessionGeneration)},
						{"settingsRevision",std::to_string(capture->graphicsSettingsRevision)},{"nativeBackendReadback",true},{"extent",{640,360}},{"encoding","srgb"}};
				}
				// A completed exact-frame query is real GPU work. Absence is never
				// reported as zero milliseconds or inferred from the effective flag.
				bool measured=false; row["gpuTimings"]=Json::array(); row["observedTimingSamples"]=Json::array();
				const auto observedTimings=context->GetCompletedPostProcessTimings();
				// Preserve every observed owner before selecting an exact join. Older
				// ring results or unavailable queries cannot masquerade as this frame.
				for(const auto& timing:observedTimings) row["observedTimingSamples"].push_back({{"effectId",timing.effect.stableId},
					{"availability",static_cast<unsigned>(timing.availability)},{"reason",static_cast<unsigned>(timing.reason)},
					{"frameId",std::to_string(timing.owner.frameId)},{"rendererSession",std::to_string(timing.owner.lifecycle.rendererSessionGeneration)},
					{"backendResourceGeneration",std::to_string(timing.owner.lifecycle.backendResourceGeneration)},
					{"viewType",static_cast<unsigned>(timing.owner.view.viewType)},{"viewId",timing.owner.view.outputId},
					{"viewLifetime",std::to_string(timing.owner.view.lifetimeGeneration)},{"targetGeneration",std::to_string(timing.owner.targetGeneration)},
					{"settingsRevision",std::to_string(timing.owner.settingsRevision)},{"frameSlot",timing.owner.frameSlot},{"snapshotSlot",timing.owner.snapshotSlot},
					{"queryPoolGeneration",std::to_string(timing.owner.queryPoolGeneration)},{"firstQuery",timing.queries.firstQuery},
					{"submitSerial",std::to_string(timing.submitSerial)},{"completionSerial",std::to_string(timing.completionSerial)},
					{"beginAvailable",timing.beginAvailable},{"endAvailable",timing.endAvailable},{"nonemptyWork",timing.nonemptyWork},
					{"durationMs",timing.durationMilliseconds ? Json(*timing.durationMilliseconds) : Json(nullptr)}});
				for(const auto& timing:observedTimings) if(timing.effect.stableId=="depth-of-field" && timing.owner.frameId==frame) {
					Check(timing.owner.settingsRevision==raw.graphicsSettingsRevision && timing.owner.lifecycle.rendererSessionGeneration==raw.backendTarget->rendererSessionGeneration
						&& timing.owner.view.viewType==RenderViewType::Scene && std::to_string(timing.owner.view.outputId)==proof.at("nativeViewId")
						&& std::to_string(timing.owner.view.lifetimeGeneration)==proof.at("nativeViewGeneration"),"DOF timing does not own the captured image.");
					row["gpuTimings"].push_back({{"effectId",timing.effect.stableId},{"availability",static_cast<unsigned>(timing.availability)},
						{"frameId",std::to_string(timing.owner.frameId)},{"targetGeneration",std::to_string(timing.owner.targetGeneration)},
						{"settingsRevision",std::to_string(timing.owner.settingsRevision)},{"submitSerial",std::to_string(timing.submitSerial)},
						{"completionSerial",std::to_string(timing.completionSerial)},{"durationMs",timing.durationMilliseconds ? Json(*timing.durationMilliseconds) : Json(nullptr)}});
					measured=measured || HasMeasuredPostProcessGpuDuration(timing);
				}
				evidence["depthOfFieldNative"]["pendingCase"]=row;
				// Each immutable diagnostic file belongs to one captured case.
				WriteJson(output/(std::string(lens.name)+"-timing-evidence.json"),evidence);
				Check(measured==(lens.enabled && lens.strength>0.0f),"Enabled DOF needs actual completed GPU timing; Off/zero must not record active DOF work. See pendingCase.observedTimingSamples for exact observed query owners.");
				evidence["depthOfFieldNative"].erase("pendingCase");
				evidence["depthOfFieldNative"]["cases"].push_back(std::move(row));
			}
			Check(!nearImage.empty() && !farImage.empty() && nearImage!=farImage,"Near/far focus produced identical native outputs.");
			std::uint64_t nearBandDifference=0u;
			for(std::uint32_t y=155u;y<190u;++y) for(std::uint32_t x=105u;x<515u;++x) for(std::uint32_t c=0u;c<3u;++c) {
				const auto pixel=(static_cast<std::size_t>(y)*640u+x)*4u+c; nearBandDifference+=static_cast<std::uint64_t>(std::abs(static_cast<int>(nearImage[pixel])-static_cast<int>(farImage[pixel])));
			}
			Check(nearBandDifference!=0u,"Frozen near-ribbon band contains no near/far focus response.");
			evidence["depthOfFieldNative"]["nearRibbonBand"]={{"crop",{105,155,515,190}},{"nearFarAbsoluteRgbDifference",nearBandDifference}};
			Check(Hash(Bytes(options.project/options.scene))==evidence["scene"]["sha256"],"Scene changed during DOF collection.");
			for(const auto& asset:evidence["cookedAssets"]) Check(Hash(Bytes(Path(asset.at("file").get<std::string>())))==asset.at("sha256"),"Cooked payload changed during DOF collection.");
			evidence["status"]="CapturedNativeDepthOfFieldCases"; evidence["evidenceKind"]="NativeBackendReadback";
			evidence["controlsVerified"]=true; evidence["qualificationComplete"]=false; WriteJson(output/"collection.json",evidence);
			std::cout<<"Collected actual native DOF cases and timing joins; focus quality/HUD/picking acceptance remains separate.\n"; return 0;
		}
		std::string stableSession,stableView,stableLifetime;
		std::uint64_t lastResetSequenceFrame=0u;
		for(std::uint32_t phase=0u;phase!=(separate ? (freshReference ? 4u : 68u) : 184u);++phase)
		{
			const bool resetRows=separate && (freshReference || phase>=64u);
			const auto rowPhase=resetRows ? (freshReference ? phase : phase-64u) : phase;
			const auto& frozen=resetRows ? separateInput->at("frames").at(rowPhase) : fixture.at("poses").at(phase);
			Check(frozen.at("sequence")== (resetRows ? options.temporalSequence : "continuous") && frozen.at("phase")==rowPhase,"Frozen phase order differs.");
			const auto oldExtent=options.extent;
			if(resetRows) { const auto& e=frozen.at("pose").at("outputExtent"); options.extent={e.at(0).get<std::uint32_t>(),e.at(1).get<std::uint32_t>()}; }
			// Fresh-reference sample index zero is the settings activation frame itself. An
			// extra update here would consume sample index zero history and invalidate proof.
			const bool useActivation=freshReference && rowPhase==0u;
			auto pose=useActivation ? activationPose : ApplyPose(inputs,frozen.at("pose"),sequence+1u);
			const bool resizeBoundary=resetRows && rowPhase==0u && oldExtent!=options.extent && !useActivation;
			auto frame=useActivation ? activationFrame : update(pose.view,pose.projection,pose.camera,1.0f/60.0f,!resizeBoundary);
			if(resizeBoundary) {
				// Admission may skip frames while the new exact extent is prepared.
				// Record every skip; the first accepted target itself must prove reset.
				const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
				for(;;) {
					const auto candidate=Accepted(*context,frame); bool exact=false;
					if(candidate) for(const auto& input:candidate->presentationInputs)
						if(input.output.kind==CaptureOutput(options) && input.poseSequence==(options.gameView ? 0u : sequence) && input.output.targetSize==options.extent) exact=true;
					if(exact) break;
					if(options.gameView)
					{
						// Retain only the first eight actual admission witnesses. These
						// are copied planned/native facts, never a replacement frame or a
						// request to reset history by temporarily disabling reconstruction.
						auto& witnesses=evidence["temporalResetSequence"]["rejectedResizeViewWitnesses"];
						if(witnesses.is_null())witnesses=Json::array();
						if(witnesses.size()<8u)
						{
							const auto native=context->ReadRecordedGraphicsViews(frame);
							Json witness{{"requestedFrameId",std::to_string(frame)},{"hasAcceptedPublication",bool(candidate)},
								{"expectedOutputExtent",{options.extent.x,options.extent.y}},
								{"expectedInternalExtent",{InternalExtent(options).x,InternalExtent(options).y}},
								{"recordedNativeViewCount",native.size()},{"views",Json::array()}};
							if(candidate)
							{
								witness["acceptedPublicationFrameId"]=std::to_string(candidate->frameId);
								for(std::size_t index=0;index<candidate->presentationInputs.size() && index<8u;++index)
								{
									const auto& view=candidate->presentationInputs[index];
									Json actual{{"publicationIndex",index},{"outputKind",static_cast<unsigned>(view.output.kind)},
										{"outputExtent",{view.output.targetSize.x,view.output.targetSize.y}},
										{"nativePoseSequence",std::to_string(view.poseSequence)},
										{"hasGraphicsCamera",view.graphicsCamera.has_value()},{"hasGraphicsView",view.graphicsView.has_value()}};
									if(view.graphicsCamera)actual["cameraId"]=TerraEngine::ToHexString(view.graphicsCamera->cameraId);
									if(view.graphicsView)actual["viewLifetimeGeneration"]=std::to_string(view.graphicsView->lifetimeGeneration);
									if(view.graphicsSettings)
									{
										actual["settingsRevision"]=std::to_string(view.graphicsSettings->revision);
										actual["settingsOutputExtent"]={view.graphicsSettings->outputWidth,view.graphicsSettings->outputHeight};
										actual["settingsInternalExtent"]={view.graphicsSettings->internalWidth,view.graphicsSettings->internalHeight};
									}
									actual["nativeCommandRecorded"]=std::any_of(native.begin(),native.end(),[&](const auto& item){return item.publicationIndex==index;});
									witness["views"].push_back(std::move(actual));
								}
							}
							witnesses.push_back(std::move(witness));
						}
					}
					evidence["temporalResetSequence"]["skippedBoundaryFrames"].push_back({{"poseSequence",std::to_string(sequence)},{"latestNativeFrame",std::to_string(frame)}});
					Check(std::chrono::steady_clock::now()<deadline,"Resize boundary never admitted its exact native target.");
					pose=ApplyPose(inputs,frozen.at("pose"),sequence+1u); frame=update(pose.view,pose.projection,pose.camera,1.0f/60.0f,false);
				}
			}
			const auto accepted=Accepted(*context,frame); Check(accepted && accepted->frameId==frame,"Exact accepted native publication is unavailable.");
			const auto proof=VerifyPrepared(renderer,*accepted,inputs,frozen.at("pose"),pose,options,useActivation ? activationSequence : sequence);
			Json row{{"sequence",resetRows ? options.temporalSequence : "continuous"},{"phase",rowPhase},{"viewGeneration",frozen.at("viewGeneration")},{"poseSha256",frozen.at("poseSha256")},
				{"nativeRendererSession",proof.at("nativeRendererSession")},{"nativeViewId",proof.at("nativeViewId")},
				{"nativeViewGeneration",proof.at("nativeViewGeneration")},{"backendFrameId",std::to_string(frame)},
				{"effective",evidence.at("effective")},
				{"preparedState",proof},{"nativeRenderAccepted",true}};
			if(resetRows) { row["simulationStateSha256"]=proof.at("simulationStateSha256"); row["effectiveSettingsSha256"]=proof.at("effectiveSettingsSha256"); row["effective"]["internalExtent"]={InternalExtent(options).x,InternalExtent(options).y}; row["effective"]["outputExtent"]={options.extent.x,options.extent.y}; }
			if(phase==0u) { stableSession=row.at("nativeRendererSession"); stableView=row.at("nativeViewId"); stableLifetime=row.at("nativeViewGeneration"); }
			if(!resetRows) Check(row.at("nativeRendererSession")==stableSession && row.at("nativeViewId")==stableView && row.at("nativeViewGeneration")==stableLifetime,"Continuous run changed native view lifetime.");
			if(resetRows && rowPhase==0u) {
				Check(proof.at("nativeTemporalPreviousFrameId")=="0" && proof.at("nativeTemporalJitterPhase")==0u,"First reset/fresh native frame did not prove predecessor0 and jitter0.");
				row["nativeHistoryResetProven"]=true; row["usesFirstSettingsActivationFrame"]=useActivation;
				if(options.temporalSequence=="resize" && !freshReference) Check(oldExtent!=options.extent && row.at("nativeViewGeneration")!=stableLifetime,"Resize did not create an exact new native view lifetime.");
			}

			if(resetRows) {
				if(rowPhase!=0u) Check(proof.at("nativeTemporalPreviousFrameId")==std::to_string(lastResetSequenceFrame) && proof.at("nativeTemporalJitterPhase")==rowPhase,"Separate sequence did not reuse its exact immediately accepted predecessor/jitter phase.");
				lastResetSequenceFrame=frame;
			}
			if(Temporal(options))
			{
				const auto previous=std::stoull(proof.at("nativeTemporalPreviousFrameId").get<std::string>());
				if(previous!=0u)
				{
					Check(acceptedSceneFrames.contains(previous),"Temporal predecessor was not an exact native Scene frame accepted by this collector.");
					evidence["temporalContinuousEvidence"][phase<64u ? "warmupPhasesWithAcceptedPredecessor" : "measuredPhasesWithAcceptedPredecessor"].push_back(phase);
				}
				else evidence["temporalContinuousEvidence"]["zeroPreviousFramePhases"].push_back(phase);
				row["temporalPredecessorAcceptedByCollector"]=previous!=0u;
			}
			if(phase<64u && !freshReference) evidence["warmupRenderReceipts"].push_back(std::move(row));
			else
			{
				const auto raw=Capture(*context,frame,InternalExtent(options),RenderCaptureImageStage::Raw,"raw-"+std::to_string(phase),CaptureView(options));
				const auto processed=Capture(*context,frame,options.extent,RenderCaptureImageStage::Processed,"processed-"+std::to_string(phase),CaptureView(options));
				Check(raw.backendTarget->rendererSessionGeneration==accepted->rendererSessionGeneration
					&& processed.backendTarget->rendererSessionGeneration==accepted->rendererSessionGeneration
					&& raw.backendTarget->targetGeneration==processed.backendTarget->targetGeneration
					&& std::to_string(raw.graphicsSettingsRevision)==proof.at("settingsRevision")
					&& std::to_string(processed.graphicsSettingsRevision)==proof.at("settingsRevision"),
					"Raw/Processed readbacks do not belong to the exact accepted source lifetime.");
				if(Temporal(options))
				{
					Check(std::to_string(raw.frameId)==proof.at("nativeTemporalRecordingId")
						&& processed.frameId==raw.frameId,"Native temporal command receipt did not join both completed readbacks.");
					row["nativeTemporalRecordingJoinedToCompletedReadbacks"]=true;
				}
				for(const auto* capture:{&raw,&processed})
				{
					const auto decoded=TerraEngine::TestSupport::DecodeCaptureBase64(capture->base64Data);
					const auto captureExtent=capture==&raw ? InternalExtent(options) : options.extent;
					Check(decoded.width==captureExtent.x && decoded.height==captureExtent.y,"Decoded native stage image extent differs.");
					for(std::size_t offset=3u;offset<decoded.rgba.size();offset+=4u) Check(decoded.rgba[offset]==255u,"Native fixture alpha is not opaque.");
					// These fixed interior witnesses come from the authored geometry,
					// not from searching candidate images. They detect material/transfer
					// mistakes before those mistakes could look like an AA improvement.
					for(const auto& witness:std::array{std::array<float,3>{8.0f,8.0f,.04f},std::array<float,3>{600.0f,50.0f,.85f}})
					{
						const auto x=static_cast<std::uint32_t>(witness[0]*captureExtent.x/640.0f);
						const auto y=static_cast<std::uint32_t>(witness[1]*captureExtent.y/360.0f);
						const float linear=witness[2]; const auto encoded=static_cast<int>(std::lround((linear<=.0031308f ? linear*12.92f : 1.055f*std::pow(linear,1.0f/2.4f)-.055f)*255.0f));
						const auto offset=(static_cast<std::size_t>(y)*captureExtent.x+x)*4u;
						for(unsigned channel=0;channel!=3u;++channel) Check(std::abs(static_cast<int>(decoded.rgba[offset+channel])-encoded)<=2,"Native material emission or single-transfer interior witness differs from its authored radiance.");
					}
					const auto png=DecodeBase64(capture->base64Data); const auto name=(capture==&raw ? "raw-" : "processed-")+std::to_string(phase)+".png";
					Write(output/Path(name),png);
					row[capture==&raw ? "raw" : "processed"]={{"file",name},{"sha256",Hash(png)},{"nativeBackendReadback",true},
						{"deterministicFallback",false},{"extent",{captureExtent.x,captureExtent.y}},{"format","PNG8"},{"encoding","srgb"},
						{"colorSpace","BT709"},{"signalDomain","display-referred"},{"backendFrameId",std::to_string(capture->frameId)},
						{"settingsRevision",std::to_string(capture->graphicsSettingsRevision)},{"sourceTargetGeneration",std::to_string(capture->backendTarget->targetGeneration)}};
					row[capture==&raw ? "raw" : "processed"]["knownMaterialRadianceWitnessesPassed"]=true;
				}
				evidence["frames"].push_back(std::move(row));
			}
		}
		Check(Hash(Bytes(options.project/options.scene))==evidence["scene"]["sha256"],"Imported scene changed during capture.");
		for(const auto& asset:evidence["cookedAssets"])
			Check(Hash(Bytes(Path(asset.at("file").get<std::string>())))==asset.at("sha256"),"Cooked source payload changed during capture.");
		if(Temporal(options) && !separate)
			Check(!evidence["temporalContinuousEvidence"]["measuredPhasesWithAcceptedPredecessor"].empty(),
				"No measured continuous temporal phase reused an earlier exact accepted frame.");
		if(separate) Check(evidence["frames"].size()==4u,"Separate reset sequence is incomplete.");
		evidence["status"]=separate ? "CapturedNativeTemporalResetSequence" : Temporal(options) ? "CapturedNativeMeshTemporalContinuousRun"
			: "CapturedExploratoryOnly";
		evidence["scorerManifestEmitted"]=false;
		WriteJson(output/"collection.json",evidence);
		std::cout<<(Temporal(options) ? "Collected native temporal continuous command/capture joins; temporal pixel/reset/disocclusion qualification remains separate.\n"
			: "Collected 64 warmup and 120 native measured frames; frozen indexed-route qualification remains unavailable.\n"); return 0;
	}
	catch(const std::exception& error)
	{
		evidence["status"]="Failed"; evidence["failure"]=error.what();
		if(!output.empty() && std::filesystem::is_directory(output) && !std::filesystem::exists(output/"collection.json"))
			try { WriteJson(output/"collection.json",evidence); } catch(...) {}
		std::cerr<<error.what()<<'\n'; return 1;
	}
}
