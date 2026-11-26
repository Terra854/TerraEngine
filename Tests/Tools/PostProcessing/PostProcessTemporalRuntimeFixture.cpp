// Packaged temporal diagnostics: actual native output never implies quality.
#include "PostProcessTemporalRuntimeFixture.h"
#include "Core/MainEngine.h"
#include "Components/CameraComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "DecodedCaptureAssertions.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace TerraEngine::Tests
{
	namespace
	{
		using Json=nlohmann::json;
		using namespace Renderer;
		using namespace Renderer::GraphicsSettings;
		void Require(bool valid, const std::string& detail) { if (!valid) throw std::runtime_error(detail); }
		std::string Environment(const char* name)
		{
			const char* text = std::getenv(name);
			Require(text && *text, std::string("Missing temporal fixture environment: ") + name); return text;
		}
		bool SafeToken(const std::string& text)
		{
			return !text.empty() && text.size() <= 100 && text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string::npos;
		}
		std::uint64_t Number(const Json& object, const char* name)
		{
			const auto& value = object.at(name);
			if (value.is_string()) {
				const auto text = value.get<std::string>();
				Require(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos, std::string("Invalid decimal native token: ") + name);
				return std::stoull(text);
			}
			Require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0), "Native token must be unsigned.");
			return value.get<std::uint64_t>();
		}
		void AtomicJson(const std::filesystem::path& path, const Json& document)
		{
			const auto pending = std::filesystem::path(path.native() + std::filesystem::path(".pending").native());
			{ std::ofstream output(pending, std::ios::binary | std::ios::trunc); output << document.dump(2) << '\n'; output.flush(); Require(output.good(), "Cannot flush temporal evidence."); }
#ifdef _WIN32
			Require(MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "Cannot publish temporal evidence atomically.");
#else
			std::filesystem::rename(pending, path);
#endif
		}
		std::vector<std::uint8_t> PngBytes(const std::string& encoded)
		{
			Require(encoded.size() % 4 == 0, "Native capture Base64 length is invalid.");
			constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::vector<std::uint8_t> bytes; unsigned bits = 0, count = 0; bool padding = false;
			for (const auto character : encoded) {
				if (character == '=') { padding = true; continue; }
				const auto digit = alphabet.find(character); Require(!padding && digit != std::string_view::npos, "Native capture Base64 alphabet is invalid.");
				bits = (bits << 6u) | static_cast<unsigned>(digit); count += 6;
				if (count >= 8) { count -= 8; bytes.push_back(static_cast<std::uint8_t>((bits >> count) & 255u)); }
			}
			return bytes;
		}
		std::string Hash(std::span<const std::uint8_t> bytes)
		{
#ifdef _WIN32
			// Use the platform provider already shipped with Windows. RAII retains
			// and releases both native handles on every failure path.
			struct Algorithm { BCRYPT_ALG_HANDLE value{}; ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); } } algorithm;
			struct Digest { BCRYPT_HASH_HANDLE value{}; ~Digest() { if (value) BCryptDestroyHash(value); } } digest;
			Require(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Cannot open native SHA256 provider.");
			Require(BCryptCreateHash(algorithm.value, &digest.value, nullptr, 0, nullptr, 0, 0) >= 0, "Cannot create native image hash.");
			for (std::size_t offset = 0; offset < bytes.size();) {
				const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 65536));
				Require(BCryptHashData(digest.value, const_cast<PUCHAR>(bytes.data() + offset), count, 0) >= 0, "Cannot hash native PNG bytes."); offset += count;
			}
			std::array<std::uint8_t, 32> result{}; Require(BCryptFinishHash(digest.value, result.data(), static_cast<ULONG>(result.size()), 0) >= 0, "Cannot finish native image hash.");
			constexpr char digits[] = "0123456789abcdef"; std::string text;
			for (const auto byte : result) { text += digits[byte >> 4u]; text += digits[byte & 15u]; } return text;
#else
			(void)bytes; throw std::runtime_error("The installed authoring fixture requires the Windows native SHA256 provider.");
#endif
		}

		std::uint32_t UInt(const GraphicsSettingsSnapshot& snapshot,GraphicsSettingId id)
		{
			for(const auto& field:snapshot.values) if(field.id==id) {
				Require(std::holds_alternative<std::uint32_t>(field.value),"Expected actual UInt32 graphics value."); return std::get<std::uint32_t>(field.value);
			}
			throw std::runtime_error("Actual native settings omitted a required temporal control.");
		}
		Json NativeView(VulkanContext& native,const RenderFramePublication& frame,std::size_t index)
		{
			const auto& input=frame.presentationInputs.at(index); Require(input.graphicsSettings && input.graphicsView,"Native view lacks exact immutable settings/view ownership.");
			const auto& settings=*input.graphicsSettings;
			const auto recorded=native.ReadRecordedGraphicsViews(frame.frameId);
			const auto actual=std::find_if(recorded.begin(),recorded.end(),[&](const auto& item){return item.publicationIndex==index;});
			Require(actual!=recorded.end(),"Exact accepted view lacks command-side native observations.");
			const auto plan=BuildPostProcessGraphSettings(settings);
			return {{"frameId",std::to_string(frame.frameId)},{"renderer",std::to_string(frame.rendererSessionGeneration)},
				{"backend",std::to_string(frame.backendResourceGeneration)},{"graphicsSettingsRevision",std::to_string(settings.revision)},
				{"viewId",input.graphicsView->outputId},{"viewLifetime",std::to_string(input.graphicsView->lifetimeGeneration)},
				{"outputKind",static_cast<unsigned>(input.output.kind)},{"publicationIndex",index},
				{"cameraId",input.graphicsCamera ? ToHexString(input.graphicsCamera->cameraId) : ""},
				{"aaMode",UInt(settings,GraphicsSettingId::AntiAliasingMode)},{"upscalingMode",UInt(settings,GraphicsSettingId::UpscalingMode)},
				{"sampleCount",actual->rasterSampleCount},{"indexedDraws",actual->indexedDrawCount},
				{"meshTaskDraws",nullptr},{"meshTaskDrawCountAvailable",false},{"recordedMotionDraws",actual->recordedMotionDrawCount},
				{"temporalRecordingId",std::to_string(actual->temporalRecordingId)},{"temporalPreviousFrameId",std::to_string(actual->temporalPreviousFrameId)},
				{"temporalJitterPhase",actual->temporalJitterPhase},{"outputExtent",{actual->width,actual->height}},
				{"internalExtent",{actual->internalWidth,actual->internalHeight}},{"plannedInternalExtent",{plan.internalSize.x,plan.internalSize.y}},
				{"temporalResolvePlanned",plan.temporalResolve},{"vrsEnabled",input.settings.variableRateShading!=0u}};
		}
		void ObserveCapture(RenderSystem& renderer,VulkanContext& native,const std::filesystem::path& root,
			const Json& observation,const RenderViewPresentationInput& normal,Json& report)
		{
			Require(normal.graphicsCamera.has_value(),"A dedicated diagnostic capture requires the actual accepted authored camera.");
			auto& objects=ObjectManager::instance(); const auto object=objects.getObject(normal.graphicsCamera->cameraId);
			Require(object.has_value(),"Accepted native camera no longer exists.");
			const auto* camera=objects.GetComponent<CameraComponent>(*object); const auto pose=objects.GetResolvedWorldPose(*object);
			const auto resolved=renderer.ResolveGraphicsSettingsCameraTarget(object->entityValue);
			Require(camera && pose && resolved && resolved->camera==normal.graphicsCamera,"Dedicated capture camera lifetime changed.");
			const auto& effective=observation.at("effective"); const auto& output=effective.at("outputExtent");
			RenderCaptureRequest request; request.sourceKind=RenderCaptureSourceKind::CameraObject; request.viewType=RenderViewType::Game;
			request.sourceObjectId=normal.graphicsCamera->cameraId; request.sourceName=objects.getObjectName(*object); request.graphicsCamera=normal.graphicsCamera;
			request.targetSize={output.at(0).get<std::uint32_t>(),output.at(1).get<std::uint32_t>()};
			const auto rotation=glm::normalize(pose->rotation); const auto up=glm::normalize(rotation*camera->up);
			request.cameraCanonicalPosition=pose->position; request.viewMatrix=glm::lookAt(glm::vec3(0),rotation*glm::vec3(0,0,-1),up);
			request.projectionMatrix=glm::perspectiveZO(glm::radians(camera->verticalFovDegrees),camera->aspectRatio,camera->nearPlane,camera->farPlane); request.projectionMatrix[1][1]*=-1;
			request.freshness="nextFrame"; request.allowBackendFallback=false; request.includeOverlays=false; request.timeoutMs=30000;
			const auto caseId=observation.at("captureCaseId").get<std::string>(); Require(SafeToken(caseId),"Invalid temporal case token.");
			request.requestId="temporal-warm-"+caseId; const auto warm=renderer.CaptureImage(request);
			Require(warm.ok && warm.backendReadback && !warm.deterministicFallback && warm.backendTarget,"Diagnostic camera target initialization failed: "+warm.message);
			report["dedicatedInitialization"]={{"frameId",std::to_string(warm.frameId)},{"targetGeneration",std::to_string(warm.backendTarget->targetGeneration)}};
			for(const auto stage:{RenderCaptureImageStage::Raw,RenderCaptureImageStage::Processed}) {
				const std::string name=caseId+(stage==RenderCaptureImageStage::Raw ? "-raw" : "-processed"); request.requestId="temporal-"+name; request.imageStage=stage;
				const auto capture=renderer.CaptureImage(request);
				Require(capture.ok && capture.backendReadback && !capture.deterministicFallback && !capture.pendingAsync && capture.backendTarget
					&& capture.imageStage==stage && capture.size==request.targetSize && capture.frameId>=Number(effective,"frameId"),"Exact dedicated temporal diagnostic capture failed: "+capture.message);
				const auto accepted=native.ReadAcceptedRenderFramePublication(capture.frameId); Require(accepted && accepted->rendererSessionGeneration==capture.backendTarget->rendererSessionGeneration,"Diagnostic capture has no exact native frame owner.");
				std::optional<std::size_t> index; for(std::size_t i=0;i<accepted->presentationInputs.size();++i) {
					const auto& view=accepted->presentationInputs[i]; if(view.output.kind==RenderOutputKind::OffscreenTexture && view.graphicsCamera==normal.graphicsCamera) { Require(!index,"Ambiguous dedicated native view."); index=i; }
				}
				Require(index.has_value(),"Dedicated capture omitted its exact accepted camera/view.");
				auto row=NativeView(native,*accepted,*index); Require(Number(row,"graphicsSettingsRevision")==capture.graphicsSettingsRevision
					&& Number(row,"renderer")==Number(effective,"renderer") && Number(row,"backend")==Number(effective,"backend")
					&& row.at("aaMode")==effective.at("aaMode") && row.at("upscalingMode")==effective.at("upscalingMode")
					&& row.at("sampleCount")==effective.at("sampleCount"),"Dedicated diagnostic mode/settings/lifecycle differ from managed case.");
				const auto decoded=TerraEngine::TestSupport::DecodeCaptureBase64(capture.base64Data); Require(decoded.width==request.targetSize.x && decoded.height==request.targetSize.y,"Decoded diagnostic extent differs.");
				const auto bytes=PngBytes(capture.base64Data); const auto relative=name+".png"; Require(!std::filesystem::exists(root/relative),"Diagnostic capture path already exists.");
				{std::ofstream file(root/relative,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));Require(file.good(),"Cannot save diagnostic PNG.");}
				row["capturePath"]=relative;row["captureSha256"]=Hash(bytes);row["stage"]=stage==RenderCaptureImageStage::Raw ? "raw" : "processed";
				row["sourceExtent"]={capture.backendTarget->sourceWidth,capture.backendTarget->sourceHeight};row["sourceEncoding"]=static_cast<unsigned>(capture.sourceColorEncoding);row["deliveredEncoding"]=static_cast<unsigned>(capture.deliveredColorEncoding);
				row["sourceTargetGeneration"]=std::to_string(capture.backendTarget->targetGeneration);row["nativeBackendReadback"]=true;row["deterministicFallback"]=false;
				row["dedicatedCaptureIsIndependentFromNormalHistory"]=true;report["captures"].push_back(std::move(row));
			}
		}
	}
	int RunPostProcessTemporalRuntimeFixture(const std::filesystem::path& evidenceDirectory)
	{
		Engine engine; Json report{{"schemaVersion",1},{"status","pending"},{"qualificationComplete",false},{"ackWritten",false},{"captures",Json::array()}};
		bool ready=false;
		try {
			const auto root=std::filesystem::absolute(evidenceDirectory).lexically_normal(); Require(std::filesystem::is_directory(root),"Temporal evidence root must already exist.");
			Require(root==std::filesystem::absolute(Environment("TERRAENGINE_TEMPORAL_AA_FIXTURE_ROOT")).lexically_normal(),"Managed/native evidence directories differ.");
			Require(!std::filesystem::exists(root/"temporal-runtime-native-observation.json"),"Native temporal observation path must be fresh.");ready=true;
			const auto run=Environment("TERRAENGINE_TEMPORAL_AA_FIXTURE_RUN_ID");Require(SafeToken(run),"Invalid temporal run token.");report["runId"]=run;
			engine.Initialize();auto* renderer=engine.GetSystem<RenderSystem>();Require(renderer,"Exported temporal fixture has no RenderSystem.");
			auto* native=dynamic_cast<VulkanContext*>(renderer->GetContext());Require(native,"Exported temporal fixture requires actual Vulkan.");
			const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(3);
			for(;;) {
				Require(std::chrono::steady_clock::now()<deadline,"Managed temporal checkpoint timed out.");Require(engine.Update(1.0f/60.0f),"Exported engine stopped before temporal observation.");
				const auto path=root/"temporal-aa-observation.json";if(!std::filesystem::is_regular_file(path))continue;
				Json observation;{std::ifstream file(path);file>>observation;}Require(observation.at("schemaVersion")==1 && observation.at("runId")==run,"Managed temporal observation identity differs.");
				Require(observation.at("status")!="failed",observation.value("failure",std::string{}));
				if(observation.at("phase")!="Capture" && observation.at("phase")!="RollbackCapture")continue;
				const auto& effective=observation.at("effective");const auto game=renderer->GetGraphicsSettingsSnapshot();
				Require(game && game->revision==Number(effective,"revision") && game->lifecycle.rendererSessionGeneration==Number(effective,"renderer")
					&& game->lifecycle.backendResourceGeneration==Number(effective,"backend"),"Managed Game checkpoint differs from actual native snapshot.");
				const auto requestId=Number(observation,"requestId");const auto receipt=renderer->GetGraphicsSettingsReceipt(requestId);
				Require(requestId && receipt && receipt->requestId==requestId,"Managed checkpoint has no actual native receipt.");
				const bool rollback=observation.at("phase")=="RollbackCapture";
				Require(rollback ? (receipt->state==GraphicsRequestState::Rejected && receipt->reason==GraphicsRequestReason::Unsupported)
					: (receipt->state==GraphicsRequestState::Applied && receipt->effectiveRevision==game->revision && receipt->appliedFrameId && game->publishedFrameId>=receipt->appliedFrameId),"Managed checkpoint native terminal receipt differs.");
				report["nativeReceipt"]={{"requestId",std::to_string(requestId)},{"state",static_cast<unsigned>(receipt->state)},
					{"reason",static_cast<unsigned>(receipt->reason)},{"effectiveRevision",std::to_string(receipt->effectiveRevision)},{"appliedFrameId",std::to_string(receipt->appliedFrameId)}};
				report["validationOverrideActive"]=renderer->GetVirtualGeometryValidationRouteOverride().has_value();
				const auto reference=renderer->GetLatestRenderReferenceFrame(RenderViewType::Game);Require(reference.has_value(),"Normal runtime camera did not render.");
				const auto accepted=native->ReadAcceptedRenderFramePublication(reference->frameId);Require(accepted && accepted->frameId>=Number(effective,"frameId"),"Normal runtime publication is unavailable or stale.");
				report["managedCheckpoint"]=observation;report["normalViews"]=Json::array();const RenderViewPresentationInput* camera=nullptr;
				for(std::size_t i=0;i<accepted->presentationInputs.size();++i)if(accepted->presentationInputs[i].output.kind==RenderOutputKind::MainRuntimeView) {
					auto row=NativeView(*native,*accepted,i);Require(row.at("aaMode")==effective.at("aaMode") && row.at("upscalingMode")==effective.at("upscalingMode") && row.at("sampleCount")==effective.at("sampleCount"),"Normal native mode/sample tuple differs.");
					Require(row.at("outputExtent")==effective.at("outputExtent"),"Actual native runtime output extent differs from managed case.");
					report["normalViews"].push_back(std::move(row));if(!camera && accepted->presentationInputs[i].graphicsCamera)camera=&accepted->presentationInputs[i];
				}
				Require(camera,"No actual authored MainRuntime camera is available for diagnostic capture.");
				ObserveCapture(*renderer,*native,root,observation,*camera,report);
				report["missingAcceptanceProof"]={"scored-edge-quality","scored-shimmer","moving-camera-and-rigid-and-skinned-motion-pixels","cut-resize-history-reset","view-isolation","actual-mesh-task-command-count","atomic-rollback-images"};
				report["detail"]="Native diagnostics are exact; the strict managed ACK remains unwritten. Dedicated images have separate histories. Frozen moving-edge scoring/reset proof must be integrated before acceptance; arbitrary external booleans are not accepted.";
				engine.Shutdown();report["orderlyShutdown"]=true;AtomicJson(root/"temporal-runtime-native-observation.json",report);return 2;
			}
		} catch(const std::exception& error) {
			try{engine.Shutdown();report["orderlyShutdown"]=true;}catch(...){report["orderlyShutdown"]=false;}
			report["status"]="failed";report["failure"]=error.what();if(ready)try{AtomicJson(evidenceDirectory/"temporal-runtime-native-observation.json",report);}catch(...){}return 1;
		}
	}
}
