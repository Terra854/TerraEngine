// Renderer efficiency ingress is JSON only at automation boundaries. Native
// frame evidence remains typed and is copied before serialization here.
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include "Utilities/UUID.h"
#include "Scene/SceneMutationQueue.h"
#include "Core/Object/ObjectManager.h"
#include "Renderer/RenderSystem.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include "ThreadPool/ThreadPool.h"
#include "Animation/AnimationSystem.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Components/Transform.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Resource/ResourceManager.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <tuple>
#include <array>
#include <string_view>
#if defined(_WIN32)
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace TerraEngine::Editor::Automation
{
	std::int64_t RendererEfficiencyStaticSampleTick(std::uint64_t frame)
	{
		if(frame>=360)throw std::invalid_argument("P5 sample is outside its frozen60/300 window.");
		return 31+static_cast<std::int64_t>(frame);
	}
	bool RendererEfficiencyStaticOwnerHasOutstandingUse(const Renderer::RendererEfficiencyStaticShadowExecution& current,
		const Renderer::RendererEfficiencyStaticShadowAllocation& expected)
	{
		if(!current.available || !current.backendGeneration || !expected.sessionGeneration || !expected.backendGeneration
			|| !expected.allocationIdentity || !expected.allocationGeneration || current.backendGeneration!=expected.backendGeneration) return false;
		for(const auto& owner:current.allocations) if(!owner.retiring && owner.sessionGeneration==expected.sessionGeneration
			&& owner.backendGeneration==expected.backendGeneration && owner.allocationIdentity==expected.allocationIdentity
			&& owner.allocationGeneration==expected.allocationGeneration) for(const auto& use:owner.uses) {
			if(!use.commandSerial || std::find(current.discardedCommands.begin(),current.discardedCommands.end(),use.commandSerial)!=current.discardedCommands.end()) continue;
			const auto accepted=std::find_if(current.acceptedSubmissions.begin(),current.acceptedSubmissions.end(),[&](const auto& value){return value.commandSerial==use.commandSerial && value.frameSlot==use.frameSlot && value.submitSerial;});
			// Recorded commands without accepted submits are still physical uses.
			// Submitted uses require the native accepted mapping and no fence receipt.
			if(!use.submitSerial && accepted==current.acceptedSubmissions.end()) return true;
			if(accepted!=current.acceptedSubmissions.end() && (!use.submitSerial || use.submitSerial==accepted->submitSerial)
				&& std::find(current.completedSubmissions.begin(),current.completedSubmissions.end(),accepted->submitSerial)==current.completedSubmissions.end()) return true;
		}
		return false;
	}
	bool PrepareRendererEfficiencyStaticShadowTransaction(const nlohmann::json& requests,
		const std::map<std::string,UUID>& bindings,Scene::Transaction& output,std::string& error)
	{
		try {
			if(!requests.is_array() || requests.empty() || requests.size()>4096)throw std::runtime_error("P5 typed request count exceeds its authored pool.");
			Scene::TransactionalMutationBuilder builder("",Scene::TransactionOrigin::Automation,"RendererEfficiencyStaticShadow");
			for(const auto& request:requests) {
				const auto id=ParseUUID(request.at("objectId").get<std::string>());if(!id || *id==EMPTY_UUID)throw std::runtime_error("P5 requires actual full target UUIDs.");
				if(request.at("operation")=="SetStatic" && request.at("isStatic").is_boolean())builder.SetStatic(*id,request.at("isStatic").get<bool>());
				else if(request.at("operation")=="SetWorldTransform")
				{
					// Canonical authored values are copied, not read from live objects.
					Scene::TransactionWriteOperation write;write.kind=Scene::TransactionWriteKind::SetWorldTransform;write.target.objectId=*id;
					const auto& pose=request.at("worldPose");const auto& position=pose.at("position");
					for(int axis=0;axis<3;++axis){const char* key=axis==0?"x":axis==1?"y":"z";const auto cell=position.at("cell").at(key).get<std::string>();std::size_t consumed=0;const auto coordinate=std::stoll(cell,&consumed);if(axis==0)write.worldPose.position.cell.x=coordinate;else if(axis==1)write.worldPose.position.cell.y=coordinate;else write.worldPose.position.cell.z=coordinate;if(consumed!=cell.size())throw std::runtime_error("Invalid P5 canonical cell.");write.worldPose.position.offsetMetres[axis]=position.at("offsetMetres").at(axis).get<double>();write.worldPose.scale[axis]=pose.at("scale").at(axis).get<float>();}
					const auto& rotation=pose.at("rotation");write.worldPose.rotation=glm::quat(rotation.at(0).get<float>(),rotation.at(1).get<float>(),rotation.at(2).get<float>(),rotation.at(3).get<float>());
					// Builder owns deduplication for ordinary category/binding writes;
					// append this exact typed pose after decoding the other requests below.
					Scene::Transaction values=std::move(builder).Build();values.writeSet.operations.push_back(std::move(write));
					// The P5 pose request is deliberately last in its bounded baseline or
					// the sole changed request, avoiding worker-side live observations.
					if(&request!=&requests.back())throw std::runtime_error("P5 canonical pose request must finish its authored batch.");
					output=std::move(values);error.clear();return true;
				}
				else {
					Scene::Transaction decoded;std::string reason;
					if(!PrepareRendererEfficiencyShadowBatchTransaction(nlohmann::json::array({request}),bindings,decoded,reason))throw std::runtime_error(reason);
					// Append copied native writes below; no live component access exists
					// in this worker decoder, including resource/category edits.
					if(request.at("operation")=="SetActive")builder.SetActive(*id,request.at("active").get<bool>());
					else builder.UpdateComponentProperty(*id,"MeshRenderComponent",request.at("propertyName").get<std::string>(),Reflection::Variant(bindings.at(request.at("bindingKey").get<std::string>())));
				}
			}
			output=std::move(builder).Build();error.clear();return true;
		}catch(const std::exception& exception){error=exception.what();return false;}
	}
	bool ParseRendererEfficiencyStaticShadowControls(const nlohmann::json& step,
		Renderer::RendererEfficiencyFixtureIntent& output,std::string& error)
	{
		try {
			// Reuse exact native view/selection admission, without requesting a P4
			// route or grouping override. Static capability stays native-owned.
			auto next=output;auto selection=step;selection["nativePayload"]["shadowBatchControls"]={{"maxInstancesPerGroup",0},{"maxTaskGroupsX",0},{"failInstanceUpload",false}};
			if(!ParseRendererEfficiencyShadowBatchControls(selection,next,error,false))return false;
			next.shadowBatchControls.reset();next.viewOrder.clear();for(const auto& selected:next.casterSelections)next.viewOrder.push_back({selected.viewType,selected.outputId});
			const auto& raw=step.at("nativePayload").at("staticShadowControls");
			if(!raw.at("forceTransferUnavailable").is_boolean() || !raw.at("failAllocation").is_boolean())throw std::runtime_error("P5 seams require boolean authored controls.");
			next.staticShadowControls=Renderer::RendererEfficiencyFixtureStaticShadowControls{raw.at("forceTransferUnavailable").get<bool>(),raw.at("failAllocation").get<bool>()};
			output=std::move(next);error.clear();return true;
		}catch(const std::exception& exception){error=exception.what();return false;}
	}
	bool PrepareRendererEfficiencyShadowBatchTransaction(const nlohmann::json& requests,
		const std::map<std::string, UUID>& bindings, Scene::Transaction& output, std::string& error)
	{
		try {
			if(!requests.is_array() || requests.empty() || requests.size()>4096) throw std::runtime_error("P4 typed request count exceeds its bounded authored pool.");
			Scene::TransactionalMutationBuilder builder("",Scene::TransactionOrigin::Automation,"RendererEfficiencyShadowBatch");
			for(const auto& request:requests) {
				const auto id=ParseUUID(request.at("objectId").get<std::string>());if(!id || *id==EMPTY_UUID) throw std::runtime_error("P4 transaction target identity is invalid.");
				if(request.at("operation")=="SetActive" && request.at("active").is_boolean()) builder.SetActive(*id,request.at("active").get<bool>());
				else if(request.at("operation")=="BindPublishedResource") {
					const auto property=request.at("propertyName").get<std::string>();const auto found=bindings.find(request.at("bindingKey").get<std::string>());
					if((property!="meshId" && property!="materialId") || found==bindings.end() || found->second==EMPTY_UUID) throw std::runtime_error("P4 resource binding lacks its actual named published owner.");
					builder.UpdateComponentProperty(*id,"MeshRenderComponent",property,Reflection::Variant(found->second));
				} else throw std::runtime_error("Unsupported P4 typed mutation.");
			}
			output=std::move(builder).Build();error.clear();return true;
		} catch(const std::exception& exception) {error=exception.what();return false;}
	}
	bool ParseRendererEfficiencyShadowBatchControls(const nlohmann::json& step,
		Renderer::RendererEfficiencyFixtureIntent& output, std::string& error, bool dedicated)
	{
		try {
			// Dedicated recipes require1000 casters. Shared recipes may retain native
			// selections or explicitly select bounded real owners, without rewriting view order.
			auto next=output;if(dedicated || step.at("nativePayload").contains("casterObjectIds")) {next.casterSelections.clear();if(dedicated) next.viewOrder.clear();}const auto& payload=step.at("nativePayload");const auto& raw=payload.at("shadowBatchControls");
			Renderer::RendererEfficiencyFixtureShadowBatchControls controls;
			const auto integer=[](const nlohmann::json& value,std::uint64_t limit) {if(!value.is_number_integer() || value.get<std::int64_t>()<0 || value.get<std::uint64_t>()>limit) throw std::runtime_error("P4 control limit is outside the native admission range.");return value.get<std::uint32_t>();};
			controls.maxInstancesPerGroup=integer(raw.at("maxInstancesPerGroup"),65536);controls.maxTaskGroupsX=integer(raw.at("maxTaskGroupsX"),1048576);
			if(!raw.at("failInstanceUpload").is_boolean()) throw std::runtime_error("P4 upload seam requires a boolean.");controls.failInstanceUpload=raw.at("failInstanceUpload").get<bool>();
			std::vector<UUID> ids;std::set<UUID> unique;
			if(payload.contains("casterObjectIds")) for(const auto& value:payload.at("casterObjectIds")) {const auto id=ParseUUID(value.get<std::string>());if(!id || *id==EMPTY_UUID || !unique.insert(*id).second) throw std::runtime_error("P4 selected casters need unique full UUIDs.");ids.push_back(*id);}
			if((dedicated && ids.size()!=1000) || (!dedicated && payload.contains("casterObjectIds") && (ids.empty() || ids.size()>4096))) throw std::runtime_error("P4 selected caster count is outside its owned recipe bounds.");
			std::set<std::pair<RenderViewType,std::uint32_t>> views;
			for(const auto& view:step.at("requiredBundle").at("views")) {
				if(view.at("viewType")!="Scene" && view.at("viewType")!="Game") throw std::runtime_error("P4 requires actual Scene/Game outputs.");
				const auto type=view.at("viewType")=="Scene"?RenderViewType::Scene:RenderViewType::Game;const auto outputId=view.at("viewId").get<std::uint32_t>();
				if(outputId!=(type==RenderViewType::Scene?1U:2U)) throw std::runtime_error("P4 output ID is not an actual sealed view.");
				if(views.insert({type,outputId}).second) {if(!ids.empty()) next.casterSelections.push_back({type,outputId,ids});if(dedicated) next.viewOrder.push_back({type,outputId});}
			}
			if(views.empty() || views.size()>2) throw std::runtime_error("P4 required view set is invalid.");
			next.shadowBatchControls=controls;output=std::move(next);error.clear();return true;
		} catch(const std::exception& exception) {error=exception.what();return false;}
	}
	bool ParseRendererEfficiencyShadowBatchCameras(const nlohmann::json& script,const nlohmann::json& step,
		std::span<const Renderer::RendererEfficiencyFixtureCamera> primary,
		std::vector<Renderer::RendererEfficiencyFixtureCamera>& output,std::string& error)
	{
		// Per-step cameras are sealed. The off-camera action aims across the
		// receiver; actual frustum membership remains a native source receipt.
		try {
			nlohmann::json expected=nlohmann::json::array();std::vector<Renderer::RendererEfficiencyFixtureCamera> next;const bool off=step.at("action")=="off-camera";
			for(auto recipe:script.at("cameras")) {
				if(recipe.at("viewType")=="Game" && !off) continue;
				const auto type=recipe.at("viewType")=="Scene"?RenderViewType::Scene:RenderViewType::Game;
				const auto found=std::find_if(primary.begin(),primary.end(),[&](const auto& camera){return camera.viewType==type && camera.outputId==recipe.at("viewId").get<std::uint32_t>();});
				if(found==primary.end()) throw std::runtime_error("P4 step camera lacks its actual sealed primary view.");
				auto camera=*found;
				if(off && type==RenderViewType::Scene) {
					if(camera.cameraPosition.cell!=camera.lookAtTarget.cell) throw std::runtime_error("P4 off-camera recipe requires one canonical cell.");
					camera.lookAtTarget.offsetMetres.x+=12;camera.lookAtTarget.offsetMetres.y=-0.1;
					recipe["target"][0]=recipe.at("target")[0].get<double>()+12;recipe["target"][1]=-0.1;
				}
				expected.push_back(std::move(recipe));next.push_back(camera);
			}
			if(next.size()!=(off?2U:1U) || step.at("cameraTransforms").at("cameras")!=expected || step.at("cameraTransforms").at("nativeExtent")!=script.at("nativeExtent")) throw std::runtime_error("P4 step camera bytes differ from the exact selected recipe.");
			output=std::move(next);error.clear();return true;
		} catch(const std::exception& exception) {error=exception.what();return false;}
	}
	Renderer::RendererEfficiencyFixtureIntent RendererEfficiencyShadowBatchMeasurementIntent(
		const Renderer::RendererEfficiencyFixtureIntent& completedTransition)
	{
		// Retain original typed provenance/control values in the receipt. Only the
		// next measurement request disarms the completed one-shot upload seam.
		auto next=completedTransition;if(next.shadowBatchControls) next.shadowBatchControls->failInstanceUpload=false;return next;
	}
	// Shared recipes repeat a view for each shadow light. Lighting has one pair
	// of purposes per output, so normalize only those repeated shadow view keys.
	static nlohmann::json LightingPurposeBundle(const nlohmann::json& bundle)
	{
		nlohmann::json result = {{"views", nlohmann::json::array()}};
		for(const auto& view : bundle.at("views"))
		{
			const auto found = std::find_if(result["views"].begin(), result["views"].end(), [&](const auto& prior) { return prior.at("viewType") == view.at("viewType") && prior.at("viewId") == view.at("viewId"); });
			if(found != result["views"].end())
			{
				if(!view.contains("faceIndices") || found->at("lightingPurposes") != view.at("lightingPurposes")) throw std::runtime_error("Lighting view is duplicated or has conflicting purposes.");
				continue;
			}
			result["views"].push_back({{"viewType", view.at("viewType")}, {"viewId", view.at("viewId")}, {"lightingPurposes", view.at("lightingPurposes")}});
		}
		return result;
	}
	bool RendererEfficiencyLightingBaselineReady(std::uint64_t completed,
		const std::array<std::set<std::pair<std::uint32_t, std::uint32_t>>, 2>& slots)
	{
		// Counts alone cannot prove coverage: every purpose must contain each real
		// frame owner/snapshot pair, after all 60 native baseline exports settled.
		if(completed != 60) return false;
		for(const auto& purpose : slots)
		{
			if(purpose.size() != 24) return false;
			for(std::uint32_t frame = 0; frame < 3; ++frame) for(std::uint32_t snapshot = 0; snapshot < 8; ++snapshot)
				if(!purpose.contains({frame, snapshot})) return false;
		}
		return true;
	}
	bool PrepareRendererEfficiencyLightingTransaction(const nlohmann::json& requests, Scene::Transaction& output, std::string& error)
	{
		try
		{
			if(!requests.is_array() || requests.size() > 2048) throw std::runtime_error("Lighting transaction exceeds bounded recipe writes.");
			Scene::TransactionalMutationBuilder builder("", Scene::TransactionOrigin::Automation, "RendererEfficiencyLighting");
			for(const auto& request : requests)
			{
				const auto id = ParseUUID(request.at("objectId").get<std::string>());
				if(!id || *id == EMPTY_UUID) throw std::runtime_error("Lighting target UUID is invalid.");
				const auto operation = request.at("operation").get<std::string>();
				if(operation == "SetActive" && request.at("active").is_boolean()) builder.SetActive(*id, request.at("active").get<bool>());
				else if(operation == "UpdateComponentProperty" && request.at("componentName") == "LightComponent" && request.at("propertyName") == "intensity")
				{
					const auto value = request.at("value").get<float>();
					if(!std::isfinite(value) || value < 0) throw std::runtime_error("Lighting intensity must be finite and nonnegative.");
					builder.UpdateComponentProperty(*id, "LightComponent", "intensity", Reflection::Variant(value));
				}
				else throw std::runtime_error("Unsupported typed lighting mutation.");
			}
			output = std::move(builder).Build(); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
	bool ParseRendererEfficiencyLightingControls(const nlohmann::json& payload, bool baseline,
		Renderer::RendererEfficiencyFixtureLightingControls& output, std::string& error)
	{
		try
		{
			Renderer::RendererEfficiencyFixtureLightingControls next;
			const auto& budget = payload.at("lightingBudgets");
			for(const auto* key : {"maxUploadedLights", "maxDynamicLights", "maxShadowLights"})
				if(!budget.at(key).is_number_integer() || budget.at(key).get<std::int64_t>() < 0 || budget.at(key).get<std::uint64_t>() > 1024) throw std::runtime_error("Illegal lighting budget.");
			next.maxUploadedLights = budget.at("maxUploadedLights").get<std::uint32_t>(); next.maxDynamicLights = budget.at("maxDynamicLights").get<std::uint32_t>(); next.maxShadowLights = budget.at("maxShadowLights").get<std::uint32_t>();
			if(next.maxDynamicLights > next.maxUploadedLights || next.maxShadowLights > next.maxDynamicLights) throw std::runtime_error("P2 lighting budgets require legal retention and shadow subsets.");
			const auto& order = payload.at(baseline ? "baselineCandidateOrder" : "candidateOrder");
			if(!order.is_array() || order.size() > 4096) throw std::runtime_error("Lighting candidate order is unbounded.");
			std::set<UUID> unique;
			for(const auto& item : order) { const auto id = ParseUUID(item.get<std::string>()); if(!id || *id == EMPTY_UUID || !unique.insert(*id).second) throw std::runtime_error("Lighting order requires unique actual UUIDs."); next.candidateOrder.push_back(*id); }
			output = std::move(next); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
	bool SelectRendererEfficiencyLightingBundle(const nlohmann::json& bundle,
		const Renderer::RendererEfficiencyFixtureIntent& applied, std::span<const Renderer::RendererEfficiencyEvidence> rows,
		std::vector<Renderer::RendererEfficiencyEvidence>& output, std::string& error)
	{
		try
		{
			if(!applied.controlsApplied || !applied.controlGeneration || !applied.lightingControls) throw std::runtime_error("Lighting controls have not been applied.");
			const auto normalized = LightingPurposeBundle(bundle);
			std::vector<Renderer::RendererEfficiencyEvidence> selected;
			std::set<std::pair<std::string, std::uint32_t>> views;
			if(!normalized.at("views").is_array() || normalized.at("views").empty() || normalized.at("views").size() > 2) throw std::runtime_error("Lighting views are empty or unbounded.");
			for(const auto& view : normalized.at("views"))
			{
				const auto type = view.at("viewType").get<std::string>(); const auto id = view.at("viewId").get<std::uint32_t>();
				if((type != "Scene" && type != "Game") || id != (type == "Scene" ? 1U : 2U) || !views.insert({type, id}).second) throw std::runtime_error("Lighting views require unique actual Scene1/Game2 outputs.");
				const auto& purposes = view.at("lightingPurposes");
				if(!purposes.is_array() || purposes.size() != 2 || !(purposes == nlohmann::json::array({"FinalLights", "Analysis"}) || purposes == nlohmann::json::array({"Analysis", "FinalLights"}))) throw std::runtime_error("Every lighting view requires both unique purposes.");
			}
			for(const auto& view : normalized.at("views")) for(const auto& purpose : view.at("lightingPurposes"))
			{
				if(purpose != "FinalLights" && purpose != "Analysis") throw std::runtime_error("Unknown lighting purpose.");
				const bool analysis = purpose == "Analysis";
				const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
					return row.kind == Renderer::RendererEfficiencyEvidenceKind::Lighting && row.analysisPurpose == analysis
						&& row.viewType == (view.at("viewType") == "Scene" ? RenderViewType::Scene : RenderViewType::Game)
						&& row.viewId == view.at("viewId").get<std::uint32_t>() && RendererEfficiencyCompletedControlMatches(row, applied)
						&& row.status == Renderer::RendererEfficiencyEvidenceStatus::Valid && row.frameSerial && row.commandSerial && row.submitSerial && row.completionSerial >= row.submitSerial;
				});
				if(found == rows.end()) throw std::runtime_error("Awaiting current completed lighting purpose bundle.");
				const auto& upload = found->lightingUploads[analysis ? 1 : 0];
				if(!upload.available || !upload.succeeded || upload.analysisPurpose != analysis || !upload.allocationId || !upload.allocationGeneration) throw std::runtime_error("Lighting purpose has no actual successful physical owner.");
				const auto uploaded = (std::min)(static_cast<std::uint32_t>(applied.lightingControls->candidateOrder.size()), applied.lightingControls->maxUploadedLights);
				if(upload.activeRecordCount != (analysis ? uploaded : (std::min)(uploaded, applied.lightingControls->maxDynamicLights))
					|| found->effectiveOptions.settingsGeneration != applied.correlation.settingsGeneration || found->effectiveOptions.rendererSessionGeneration != applied.correlation.rendererSessionGeneration)
					throw std::runtime_error("Lighting count or current settings/session proof differs from applied controls.");
				std::set<UUID> identities;
				if(upload.payloadLightIds.size() != upload.activeRecordCount) throw std::runtime_error("Lighting records lack exact selected identities.");
				for(const auto& id : upload.payloadLightIds) if(id == EMPTY_UUID || !identities.insert(id).second || std::find(applied.lightingControls->candidateOrder.begin(), applied.lightingControls->candidateOrder.end(), id) == applied.lightingControls->candidateOrder.end()) throw std::runtime_error("Lighting payload identities are not a unique subset of actual candidates.");
				const bool selectedOutput = found->viewType == applied.lightingControls->snapshotViewType && found->viewId == applied.lightingControls->snapshotOutputId;
				if(!applied.lightingControls->snapshotSlot || (selectedOutput && upload.snapshotSlot != *applied.lightingControls->snapshotSlot) || upload.frameSlot >= 3 || upload.snapshotSlot >= 8) throw std::runtime_error("Lighting physical slot differs from the actual requested slice.");
				if(applied.lightingControls->frameSlot && upload.frameSlot != *applied.lightingControls->frameSlot) throw std::runtime_error("Lighting frame owner differs from the actual capture target.");
				if(!selected.empty() && (found->frameSerial != selected.front().frameSerial || found->submitSerial != selected.front().submitSerial || found->completionSerial != selected.front().completionSerial)) throw std::runtime_error("Lighting purposes belong to different native completions.");
				if(!found->effectiveOptions.optionsSessionGeneration || (!selected.empty() && found->effectiveOptions.optionsSessionGeneration != selected.front().effectiveOptions.optionsSessionGeneration)) throw std::runtime_error("Lighting purposes have no common actual options epoch.");
				for(const auto& prior : selected) if(prior.viewType == found->viewType && prior.viewId == found->viewId && prior.commandSerial != found->commandSerial) throw std::runtime_error("Lighting purposes have different commands for one qualified view.");
				selected.push_back(*found);
			}
			if(selected.empty() || selected.size() > 4) throw std::runtime_error("Lighting required bundle is empty or unbounded.");
			if(static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.kind == Renderer::RendererEfficiencyEvidenceKind::Lighting; })) != selected.size()) throw std::runtime_error("Lighting cohort contains duplicate or unexpected purpose owners.");
			output = std::move(selected); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
	std::string_view RendererEfficiencyCompletedExportPhase(std::uint64_t warmupCompleted, std::uint64_t warmupFrames)
	{
		// Classify the already completed native frame before dispatch. The host
		// advances counts only after this immutable export successfully settles.
		return warmupCompleted < warmupFrames ? "Warmup" : "Measured";
	}
	bool RendererEfficiencyFixtureControlsMatch(const Renderer::RendererEfficiencyFixtureIntent& expected,
		const Renderer::RendererEfficiencyFixtureIntent& applied)
	{
		// An accepted queue request is not a protected application receipt. Every
		// identity lane and control must belong to the same frozen measurement.
		return applied.controlsApplied && applied.controlGeneration != 0 && applied.correlation == expected.correlation
			&& applied.cameras == expected.cameras && applied.referenceOriginDelta == expected.referenceOriginDelta
			&& applied.shadowSelection == expected.shadowSelection && applied.viewOrigins == expected.viewOrigins
			&& applied.viewOrder == expected.viewOrder && applied.casterSelections == expected.casterSelections && applied.shadowBatchControls == expected.shadowBatchControls && applied.staticShadowControls == expected.staticShadowControls
			&& applied.injection == expected.injection && applied.sampleTick == expected.sampleTick && applied.lightingControls == expected.lightingControls
			&& applied.depthControls == expected.depthControls;
	}
	std::string RendererEfficiencyPendingObservationReason(const Renderer::RendererEfficiencyFixtureIntent& expected,
		const Renderer::RendererEfficiencyFixtureIntent& applied, std::span<const Renderer::RendererEfficiencyEvidence> rows)
	{
		if(applied.correlation == expected.correlation && !RendererEfficiencyFixtureControlsMatch(expected, applied)
			&& !applied.controlReason.empty()) return applied.controlReason;
		for(const auto& row : rows)
		{
			if(row.fixtureCorrelation != expected.correlation || row.effectiveOptions.settingsGeneration != expected.correlation.settingsGeneration
				|| row.effectiveOptions.rendererSessionGeneration != expected.correlation.rendererSessionGeneration) continue;
			if(row.status == Renderer::RendererEfficiencyEvidenceStatus::Valid && row.completionSerial != 0) continue;
			// Preserve native explanations verbatim. An unavailable/incomplete row
			// diagnoses Pending only; it never enters the qualified valid bundle.
			std::string reason = row.fallbackReason;
			if(!row.unavailableReason.empty()) { if(!reason.empty()) reason += "; "; reason += row.unavailableReason; }
			if(!reason.empty()) return reason;
		}
		return RendererEfficiencyFixtureControlsMatch(expected, applied)
			? std::string{} : "Awaiting actual protected fixture control application.";
	}
	namespace
	{
		// This is published input provenance, not proof that GPU work completed.
		// Resolve dynamic lanes because warmed packets retain their original tick.
		bool CopyGpuAnimationInput(const Animation::AnimationPresentationFrame& frame, const UUID& id,
			Animation::GpuAnimationWorkPacket& packet, Animation::GpuAnimationGenerationRecord& generation,
			Animation::GpuAnimationRenderIdentity& identity)
		{
			if(!frame.IsValid() || frame.gpuPlan->safeFallback
				|| frame.gpuPlan->loweringStatus != Animation::GpuAnimationLoweringStatus::Ok
				|| frame.gpuPlan->planStatus != Animation::GpuAnimationPlanStatus::Ok) return false;
			const auto* row = frame.FindAuthorityRowForOwner(id);
			const auto& mappings = frame.gpuPlan->OwnerObjectMappings();
			const auto* mapping = frame.gpuPlan->FindFirstOwnerObjectMapping(id);
			if(!row || !row->IsValid() || row->ownerObjectId != id || !row->active || row->safeFallback || row->snapshot
				|| !mapping || !mapping->HasPublishedPacketIndex()
				|| std::count_if(mappings.begin(), mappings.end(), [&](const auto& value){return value.ownerObjectId == id;}) != 1
				|| mapping->instanceSlot != row->key.slot || mapping->instanceGeneration != row->key.lifetimeGeneration
				|| mapping->componentGeneration != row->componentGeneration) return false;
			const auto& plan = frame.gpuPlan->plan;
			const auto index = mapping->workPacketIndex;
			const auto count = plan.WorkPackets().size();
			if(index >= count || index >= plan.RenderIdentityCount()
				|| (plan.retainedDynamicLanes && !plan.retainedDynamicLanes->HasParallelLengths(count))
				|| (!plan.dynamicSampleTicks.empty() && plan.dynamicSampleTicks.size() != count)) return false;
			packet = plan.ResolvedWorkPacket(index);
			constexpr auto forbidden = static_cast<std::uint32_t>(Animation::GpuAnimationWorkFlag::UseBindPoseFallback)
				| static_cast<std::uint32_t>(Animation::GpuAnimationWorkFlag::UseHoldLastFallback)
				| static_cast<std::uint32_t>(Animation::GpuAnimationWorkFlag::HybridCpuAuthority)
				| static_cast<std::uint32_t>(Animation::GpuAnimationWorkFlag::DeterministicReference)
				| static_cast<std::uint32_t>(Animation::GpuAnimationWorkFlag::SuspendVisualEvaluation);
			if(packet.flags & forbidden) return false;
			if(packet.generationRecordIndex >= plan.GenerationRecordCount()) return false;
			generation = plan.GenerationRecord(packet.generationRecordIndex); identity = plan.RenderIdentity(index);
			if(!Animation::IsGpuAnimationGenerationRecordValid(generation) || !identity.IsComplete()
				|| packet.stableInstanceId != mapping->stableInstanceId
				|| generation.instanceSlot != row->key.slot || generation.instanceGeneration != row->key.lifetimeGeneration
				|| generation.componentGeneration != row->componentGeneration || generation.workGeneration != row->poseGeneration
				|| (generation.flags & Animation::kGpuAnimationGenerationFlagSuspendVisualEvaluation)) return false;
			const auto& candidates = frame.authority->sidecars->gpuCandidates;
			return std::count_if(candidates.begin(), candidates.end(), [&](const auto& value) {
				return value.key.slot == row->key.slot && value.key.lifetimeGeneration == row->key.lifetimeGeneration
					&& value.key.sessionGeneration == row->key.sessionGeneration && value.kind == 1U
					&& value.status == Animation::AnimationStatus::Ok && value.IsValidFor(frame.authority->authorityGeneration);
			}) == 1;
		}
	}
	bool RendererEfficiencyGpuAnimationAuthorityMatches(const Animation::AnimationPresentationFrame& frame,
		const Animation::AnimationPresentationFrame& prior, const Animation::AnimationCommandReceipt& receipt,
		const UUID& id, bool baseline, Animation::AnimationFixedTick requestedTick)
	{
		Animation::GpuAnimationWorkPacket packet, oldPacket;
		Animation::GpuAnimationGenerationRecord generation, oldGeneration;
		Animation::GpuAnimationRenderIdentity identity, oldIdentity;
		if(receipt.status != Animation::AnimationStatus::Ok || receipt.publicationGeneration == 0
			|| requestedTick < 0 || requestedTick > UINT32_MAX
			|| !CopyGpuAnimationInput(frame, id, packet, generation, identity)
			|| !CopyGpuAnimationInput(prior, id, oldPacket, oldGeneration, oldIdentity)) return false;
		const auto* row = frame.FindAuthorityRowForOwner(id); const auto* oldRow = prior.FindAuthorityRowForOwner(id);
		if(row->key.Handle() != receipt.target || row->key != oldRow->key
			|| frame.authority->sessionGeneration != prior.authority->sessionGeneration
			|| frame.authority->lifecycleGeneration != prior.authority->lifecycleGeneration
			|| frame.authority->authorityGeneration < receipt.publicationGeneration
			|| identity != oldIdentity || packet.sampleTick != static_cast<std::uint32_t>(requestedTick)) return false;
		// Resource bindings must stay exact across a seek. Only work/frame/tick lanes
		// may change; command authority and pose-content generations are different.
		if(Animation::MakeGpuAnimationBatchGenerationKey(generation) != Animation::MakeGpuAnimationBatchGenerationKey(oldGeneration)
			|| generation.componentGeneration != oldGeneration.componentGeneration
			|| packet.stableProgramId != oldPacket.stableProgramId || packet.clipResourceIndex != oldPacket.clipResourceIndex
			|| packet.programResourceIndex != oldPacket.programResourceIndex || packet.skeletonResourceIndex != oldPacket.skeletonResourceIndex
			|| packet.maskResourceIndex != oldPacket.maskResourceIndex || packet.morphResourceIndex != oldPacket.morphResourceIndex
			|| packet.stageMask != oldPacket.stageMask || packet.flags != oldPacket.flags
			|| packet.jointCount != oldPacket.jointCount || packet.morphCount != oldPacket.morphCount) return false;
		// A newer work generation establishes freshness only. The actual input
		// change is the resolved tick 0 -> 30 with identical native source bindings.
		return baseline ? requestedTick == 0 && (oldPacket.sampleTick == 0 ? generation.workGeneration >= oldGeneration.workGeneration
			: generation.workGeneration > oldGeneration.workGeneration && frame.authority->authorityGeneration > prior.authority->authorityGeneration)
			: requestedTick == 30 && oldPacket.sampleTick == 0 && generation.workGeneration > oldGeneration.workGeneration
				&& frame.authority->authorityGeneration > prior.authority->authorityGeneration;
	}
	// A command receipt counts authority publications; the retained snapshot
	// counts pose evaluations. Match the authority row and its exact identity,
	// then consume that same immutable row's snapshot without mixing clocks.
	bool RendererEfficiencyAnimationAuthorityMatches(const Animation::AnimationAuthorityRow& row,
		const Animation::AnimationAuthorityFrame& frame, const Animation::AnimationCommandReceipt& receipt,
		const UUID& objectId, bool baseline, Animation::AnimationFixedTick playbackTick,
		const std::shared_ptr<const Animation::AnimationSnapshot>& prior)
	{
		// Baseline Seek(0) can legitimately retain an unchanged pose row. Its
		// accepted command still belongs to a newer completed authority root;
		// changed steps instead require a newly selected row and content proof.
		const auto atZero = [](const auto& snapshot) { return snapshot && snapshot->playback.normalizedTime == 0.0F && snapshot->playback.loopCount == 0; };
		bool retainedBaseline = baseline && playbackTick == 0 && atZero(row.snapshot) && atZero(prior)
			&& prior->animator == receipt.target && row.snapshot->publicationGeneration == prior->publicationGeneration;
		if(retainedBaseline && prior->pose && row.snapshot->pose)
		{
			retainedBaseline = std::ranges::equal(prior->pose->LocalTransforms(), row.snapshot->pose->LocalTransforms())
				&& std::ranges::equal(prior->pose->ModelTransforms(), row.snapshot->pose->ModelTransforms())
				&& std::ranges::equal(prior->pose->MorphValues(), row.snapshot->pose->MorphValues())
				&& std::ranges::equal(prior->pose->CurveValues(), row.snapshot->pose->CurveValues());
		}
		else if(retainedBaseline) retainedBaseline = row.snapshot == prior;
		return receipt.status == Animation::AnimationStatus::Ok && receipt.publicationGeneration != 0
			&& frame.receipt.Published() && frame.receipt.authorityGeneration == frame.authorityGeneration
			&& frame.authorityGeneration >= receipt.publicationGeneration && frame.lifecycleGeneration != 0
			&& frame.sessionGeneration != 0 && row.key.IsValid() && row.key.sessionGeneration == frame.sessionGeneration
			&& row.key.Handle() == receipt.target && objectId != EMPTY_UUID && row.ownerObjectId == objectId
			&& row.objectGeneration != 0 && row.componentGeneration != 0
			&& row.status == Animation::AnimationStatus::Ok && !row.safeFallback && (!baseline || (playbackTick == 0 && atZero(row.snapshot)))
			&& (row.authorityPublicationGeneration >= receipt.publicationGeneration || retainedBaseline)
			&& row.snapshot && row.snapshot->animator == receipt.target;
	}
	std::vector<Animation::AnimationCommandKind> RendererEfficiencySeekCommandKinds(Animation::AnimatorPlaybackRunState state)
	{
		if(state == Animation::AnimatorPlaybackRunState::Playing) return {Animation::AnimationCommandKind::Pause, Animation::AnimationCommandKind::Seek};
		return {Animation::AnimationCommandKind::Seek};
	}
	bool EditorRendererEfficiencyValidation::ValidateSession(const std::filesystem::path& root,
		std::uint64_t generation, std::string& error, bool requireActive) const
	{
		if((requireActive && !active) || generation != projectGeneration)
		{ error = "Renderer fixture operation or project generation is stale."; return false; }
		if(!ValidateRendererEfficiencyOwnedProject(root, fixtureId, fixtureHash, error)) return false;
		try
		{
			if(std::filesystem::canonical(root) != projectRoot)
			{ error = "Renderer fixture admission belongs to a different project root."; return false; }
			std::ifstream input(projectRoot / "renderer-efficiency-project.json", std::ios::binary);
			if(nlohmann::json::parse(input).dump() != markerIdentity)
			{ error = "Renderer fixture ownership marker changed after session start."; return false; }
		}
		catch(const std::exception&) { error = "Renderer fixture ownership marker cannot be read."; return false; }
		error.clear(); return true;
	}
	// The dedicated delete/write fixture must prove its actual atomic decision.
	bool RendererEfficiencyAtomicConflictReceiptMatches(const Scene::AutomationTransactionReceipt& receipt, std::size_t expectedWrites)
	{
		return receipt.status == Scene::TransactionStatus::Conflicted && receipt.appliedWriteCount == 0
			&& expectedWrites == 2 && receipt.rejectedWriteCount == expectedWrites;
	}
	bool EditorRendererEfficiencyValidation::QueueFixtureTransaction(
		const std::filesystem::path& currentRoot, std::uint64_t currentProjectGeneration,
		std::string_view requestedOperation, Scene::Transaction transaction,
		Scene::SceneMutationQueue& queue, std::string& error)
	{
		// Recheck ownership at every protected admission, including after a worker
		// finishes. The complete typed batch is enqueued atomically without a flush.
		if(!HasOperation(requestedOperation)) { error = "Renderer fixture operation is stale."; return false; }
		if(!ValidateSession(currentRoot, currentProjectGeneration, error)) return false;
		if(transaction.id.empty() || transaction.origin != Scene::TransactionOrigin::Automation
			|| transaction.mode != Scene::TransactionMode::Commit || transaction.injectedFailureOperationIndex
			|| !transaction.undoGroupName.empty() || transaction.writeSet.operations.empty())
		{ error = "Renderer fixtures require a complete native automation commit with no undo or test failpoint."; return false; }
		if(!pendingTransactionId.empty())
		{ error = "A renderer fixture transaction is already awaiting its protected completion."; return false; }
		pendingTransactionId = transaction.id;
		// Queue ingress retains only worker-safe typed expectations and a shared
		// immutable packet. The protected receipt pump later captures live proof.
		pendingScenePublication = {};
		pendingScenePublication.transaction = transaction;
		pendingScenePublication.priorPacket = ObjectManager::instance().LoadPublishedSceneChangePacketShared();
		queue.EnqueueTransaction(std::move(transaction));
		state = "AwaitingMutation"; error.clear(); return true;
	}

	namespace
	{
		using namespace Renderer;
		// Hash the exact installed bytes using the operating system provider. This
		// adds no third-party dependency or process/tool invocation at runtime.
		std::string InstalledStepsHash(std::string_view bytes)
		{
#if defined(_WIN32)
			const auto library = LoadLibraryW(L"bcrypt.dll");
			if(!library) throw std::runtime_error("SHA256 provider unavailable");
			const auto closeLibrary = std::shared_ptr<void>(library, [](void* value) { FreeLibrary(static_cast<HMODULE>(value)); });
			const auto open = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(GetProcAddress(library, "BCryptOpenAlgorithmProvider"));
			const auto close = reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(GetProcAddress(library, "BCryptCloseAlgorithmProvider"));
			const auto create = reinterpret_cast<decltype(&BCryptCreateHash)>(GetProcAddress(library, "BCryptCreateHash"));
			const auto hash = reinterpret_cast<decltype(&BCryptHashData)>(GetProcAddress(library, "BCryptHashData"));
			const auto finish = reinterpret_cast<decltype(&BCryptFinishHash)>(GetProcAddress(library, "BCryptFinishHash"));
			const auto destroy = reinterpret_cast<decltype(&BCryptDestroyHash)>(GetProcAddress(library, "BCryptDestroyHash"));
			if(!open || !close || !create || !hash || !finish || !destroy) throw std::runtime_error("SHA256 provider incomplete");
			BCRYPT_ALG_HANDLE algorithm{};
			if(open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("SHA256 initialization failed");
			const auto algorithmOwner = std::shared_ptr<void>(algorithm, [close](void* value) { close(value, 0); });
			BCRYPT_HASH_HANDLE digest{};
			if(create(algorithm, &digest, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("SHA256 hash allocation failed");
			const auto digestOwner = std::shared_ptr<void>(digest, [destroy](void* value) { destroy(value); });
			std::array<unsigned char, 32> output{};
			if(bytes.size() > UINT32_MAX || hash(digest, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0
				|| finish(digest, output.data(), static_cast<ULONG>(output.size()), 0) < 0) throw std::runtime_error("SHA256 calculation failed");
			constexpr char hex[] = "0123456789abcdef";
			std::string result; result.reserve(64);
			for(const auto value : output) { result.push_back(hex[value >> 4]); result.push_back(hex[value & 15]); }
			return result;
#else
			(void)bytes;
			throw std::runtime_error("Installed steps SHA256 provider unavailable on this platform");
#endif
		}
		std::filesystem::path OwnedRegularFile(const std::filesystem::path& root, const std::filesystem::path& relative)
		{
			if(relative.empty() || relative.is_absolute()) throw std::runtime_error("Owned fixture input must be relative");
			const auto canonicalRoot = std::filesystem::canonical(root);
			auto path = canonicalRoot;
			for(const auto& part : relative)
			{
				if(part == "." || part == "..") throw std::runtime_error("Owned fixture input cannot escape its root");
				path /= part;
				if(std::filesystem::is_symlink(path)) throw std::runtime_error("Owned fixture input cannot use a symlink");
#if defined(_WIN32)
				const auto attributes = GetFileAttributesW(path.c_str());
				if(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Owned fixture input cannot use a reparse point");
#endif
			}
			if(!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 8U * 1024U * 1024U)
				throw std::runtime_error("Owned fixture input is absent or exceeds the bounded input size");
			return path;
		}
		std::string ReadOwnedBytes(const std::filesystem::path& root, const std::filesystem::path& relative)
		{
			std::ifstream input(OwnedRegularFile(root, relative), std::ios::binary);
			if(!input) throw std::runtime_error("Owned fixture input cannot be read");
			return std::string((std::istreambuf_iterator<char>(input)), {});
		}
		std::string ReadSealedBytes(const std::filesystem::path& root, const std::filesystem::path& relative)
		{
			const auto marker = nlohmann::json::parse(ReadOwnedBytes(root, "renderer-efficiency-project.json"));
			const auto& files = marker.at("generatedFiles");
			const auto found = std::find_if(files.begin(), files.end(), [&](const auto& file) { return file.at("projectPath") == relative.generic_string(); });
			if(found == files.end()) throw std::runtime_error("Fixture input is not listed in the owned receipt");
			auto bytes = ReadOwnedBytes(root, relative);
			if(InstalledStepsHash(bytes) != found->at("sha256").template get<std::string>()) throw std::runtime_error("Fixture input SHA256 differs from its owned receipt");
			return bytes;
		}
		bool Unsigned(const nlohmann::json& value, std::uint64_t& output)
		{
			if(value.is_number_unsigned()) { output = value.get<std::uint64_t>(); return true; }
			if(value.is_number_integer() && value.get<std::int64_t>() >= 0)
			{ output = static_cast<std::uint64_t>(value.get<std::int64_t>()); return true; }
			return false;
		}
		const char* StatusName(RendererEfficiencyEvidenceStatus status)
		{
			return status == RendererEfficiencyEvidenceStatus::Valid ? "Valid"
				: status == RendererEfficiencyEvidenceStatus::Pending ? "Pending" : "Unavailable";
		}
		template<class Enum> bool Mode(const nlohmann::json& value, const char* first, const char* second, Enum& output)
		{
			if(!value.is_string()) return false;
			const auto text = value.get<std::string>();
			if(text == first) { output = static_cast<Enum>(0); return true; }
			if(text == second) { output = static_cast<Enum>(1); return true; }
			return false;
		}
	}
	bool AdmitRendererEfficiencyResidencyReupload(Renderer::RendererEfficiencyFixtureIntent& activeIntent,
		std::uint64_t baselineStepFrame, std::uint64_t& stepFrame, bool& intentQueued,
		const std::function<Renderer::RendererEfficiencyFixtureIntentReceipt(const Renderer::RendererEfficiencyFixtureIntent&)>& queueIntent,
		const std::function<bool(std::string&)>& invalidate, std::string& error)
	{
		const auto reject = [&](std::string reason) { error = std::move(reason); return false; };
		if(!baselineStepFrame || baselineStepFrame >= 9007199254740991ULL
			|| activeIntent.correlation.operationId.empty() || activeIntent.correlation.fixtureStepId.empty()
			|| !activeIntent.correlation.mutationGeneration || !activeIntent.correlation.settingsGeneration
			|| !activeIntent.correlation.rendererSessionGeneration || !activeIntent.controlGeneration)
			return reject("Residency admission lacks its exact protected baseline owner.");
		// A successfully admitted request remains armed across later pumps. This
		// idempotent boundary must never issue a second eviction for the same token.
		const auto nextFrame = baselineStepFrame + 1U;
		if(intentQueued && stepFrame == nextFrame && activeIntent.correlation.stepFrame == nextFrame) { error.clear(); return true; }
		if(stepFrame != baselineStepFrame || activeIntent.correlation.stepFrame != baselineStepFrame)
			return reject("Residency admission no longer belongs to the completed baseline.");
		auto next = activeIntent;
		next.correlation.stepFrame = nextFrame; next.controlsApplied = false;
		// The next native capture must see the reupload token before resource
		// invalidation can refresh shadows. Waiting until the following Host pump
		// would render an unobserved refresh under the consumed baseline token.
		const auto receipt = queueIntent(next);
		if(!receipt.accepted || !receipt.controlGeneration || receipt.correlation != next.correlation
			|| receipt.controlGeneration != activeIntent.controlGeneration)
			return reject(receipt.reason.empty() ? "Native reupload intent did not accept the exact current control/correlation owner." : receipt.reason);
		next.controlGeneration = receipt.controlGeneration;
		if(!invalidate(error)) return false;
		// No observation count advances here. Only the real queued intent owns the
		// next token; the caller clears it on terminal failure or session stop.
		activeIntent = std::move(next); stepFrame = nextFrame; intentQueued = true;
		error.clear(); return true;
	}
	bool CompleteRendererEfficiencyResidencyPublicationReceipt(const nlohmann::json& scalarReceipt,
		const nlohmann::json& baselineObservation, const nlohmann::json& reuploadObservation,
		nlohmann::json& output, std::string& error)
	{
		try
		{
			// This is receipt validation, not native evidence creation. The writer
			// has already sealed pixels; retain its complete original frame cohort.
			const auto require = [](bool valid) { if(!valid) throw std::runtime_error("Residency receipt lacks both exact original completed observations."); };
			const auto positive = [&](const nlohmann::json& value) {
				require(value.is_string()); const auto text = value.get<std::string>(); std::size_t consumed = 0;
				const auto number = std::stoull(text, &consumed);
				require(number && consumed == text.size() && text == std::to_string(number)); return number;
			};
			const auto validate = [&](const nlohmann::json& observation) {
				require(observation.is_object() && !observation.contains("kind") && !observation.contains("action"));
				for(const auto* field : {"controlGeneration", "frameSerial", "commandSerial", "submitSerial", "completionSerial"}) (void)positive(observation.at(field));
				require(positive(observation.at("completionSerial")) >= positive(observation.at("submitSerial")));
				const auto& token = observation.at("originalCorrelation");
				require(token.size() == 6 && !token.at("operationId").get<std::string>().empty()
					&& token.at("fixtureStepId") == scalarReceipt.at("fixtureStepId") && token.at("stepFrame").get<std::uint64_t>() > 0);
				require((observation.at("frames").size() == 9 || observation.at("frames").size() == 18) && observation.at("artifacts").is_array() && !observation.at("artifacts").empty());
				std::set<std::tuple<std::string, std::uint32_t, std::string, std::uint32_t>> regions;
				std::map<std::pair<std::string, std::uint32_t>, std::uint64_t> commands;
				std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> viewCounts;
				for(const auto& frame : observation.at("frames"))
				{
					require(frame.at("measurementPhase") == "Transition" && frame.at("fixtureStepId") == token.at("fixtureStepId") && frame.at("stepFrame") == token.at("stepFrame"));
					for(const auto* field : {"frameSerial", "submitSerial", "completionSerial"})
						require(frame.at(field).at("status") == "Valid" && frame.at(field).at("value").get<std::uint64_t>() == positive(observation.at(field)));
					for(const auto* field : {"mutationGeneration", "settingsGeneration", "rendererSessionGeneration"})
						require(frame.at(field).at("status") == "Valid" && frame.at(field).at("value") == token.at(field));
					require(frame.at("commandSerial").at("status") == "Valid" && frame.at("lightId").at("status") == "Valid" && frame.at("faceIndex").at("status") == "Valid");
					const auto view = std::pair{frame.at("viewType").get<std::string>(), frame.at("viewId").get<std::uint32_t>()};
					const auto command = frame.at("commandSerial").at("value").get<std::uint64_t>(); require(command != 0);
					const auto [prior, inserted] = commands.emplace(view, command); require(inserted || prior->second == command);
					++viewCounts[view];
					require(regions.emplace(view.first, view.second, frame.at("lightId").at("value").get<std::string>(), frame.at("faceIndex").at("value").get<std::uint32_t>()).second);
				}
				for(const auto& [view, count] : viewCounts) { (void)view; require(count == 9); }
				require(observation.at("frames").front().at("commandSerial").at("value").get<std::uint64_t>() == positive(observation.at("commandSerial")));
				return regions;
			};
			require(scalarReceipt.at("kind") == "ResidencyPublication");
			const auto baselineRegions = validate(baselineObservation), reuploadRegions = validate(reuploadObservation);
			require(baselineRegions == reuploadRegions);
			for(const auto* field : {"operationId", "fixtureStepId", "mutationGeneration", "settingsGeneration", "rendererSessionGeneration"})
				require(baselineObservation.at("originalCorrelation").at(field) == reuploadObservation.at("originalCorrelation").at(field));
			require(baselineObservation.at("controlGeneration") == reuploadObservation.at("controlGeneration")
				&& baselineObservation.at("originalCorrelation").at("stepFrame").get<std::uint64_t>() < reuploadObservation.at("originalCorrelation").at("stepFrame").get<std::uint64_t>()
				&& positive(baselineObservation.at("frameSerial")) < positive(reuploadObservation.at("frameSerial"))
				&& scalarReceipt.at("previousActualFrameSerial").get<std::uint64_t>() == positive(baselineObservation.at("frameSerial"))
				&& scalarReceipt.at("newActualFrameSerial").get<std::uint64_t>() == positive(reuploadObservation.at("frameSerial")));
			// The legacy scalar command names the actual target proof row, which
			// can be Game's command. The nested command represents its first frame.
			// Both are real, but only the nested representative has first-row meaning.
			const auto targetCommand = scalarReceipt.at("commandSerial").get<std::uint64_t>();
			require(targetCommand && std::any_of(reuploadObservation.at("frames").begin(), reuploadObservation.at("frames").end(),
				[&](const auto& frame) { return frame.at("commandSerial").at("value").template get<std::uint64_t>() == targetCommand; }));
			for(const auto* field : {"submitSerial", "completionSerial"})
				require(scalarReceipt.at(field).get<std::uint64_t>() == positive(reuploadObservation.at(field)));
			auto completed = scalarReceipt; completed["baselineObservation"] = baselineObservation; completed["reuploadObservation"] = reuploadObservation;
			output = std::move(completed); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
	bool ParseRendererEfficiencyDepthControls(const std::filesystem::path& root, const nlohmann::json& script,
		const nlohmann::json& step, Renderer::RendererEfficiencyFixtureDepthControls& output, std::string& error)
	{
		// Decode sealed authoring only at ingress. These bits request an isolated
		// oracle image; they never stand in for the backend's actual GPU readback.
		try
		{
			Renderer::RendererEfficiencyFixtureDepthControls next;
			const auto& payload = step.at("nativePayload");
			if(payload.contains("depthInputRecipe"))
			{
				const auto expectedPath = std::string("renderer-efficiency-depth/") + step.at("id").get<std::string>() + ".json";
				if(payload.at("depthRecipePath") != expectedPath) throw std::runtime_error("Depth input path differs from its exact owned step.");
				const auto bytes = ReadSealedBytes(root, expectedPath); const auto sealed = nlohmann::json::parse(bytes);
				if(InstalledStepsHash(bytes) != payload.at("depthRecipeSha256").get<std::string>()
					|| sealed.at("schemaVersion") != 1 || sealed.at("kind") != "LightingDepthInputRecipe"
					|| sealed.at("fixtureId") != script.at("fixtureId") || sealed.at("fixtureHash") != script.at("fixtureHash")
					|| sealed.at("seed") != script.at("seed") || sealed.at("fixtureStepId") != step.at("id")
					|| sealed.at("viewType") != "Scene" || sealed.at("viewId") != 1 || sealed.at("grid") != nlohmann::json::array({16,9,24})
					|| sealed.at("recipe") != payload.at("depthInputRecipe")) throw std::runtime_error("Depth input bytes, hash or owner differ from their sealed authority.");
				const auto& recipe = sealed.at("recipe"); Renderer::LightingDepthReduction::DepthRecipe typed;
				auto integer = [](const nlohmann::json& value) { std::uint64_t number; if(!Unsigned(value, number) || number > UINT32_MAX) throw std::runtime_error("Depth recipes require exact uint32 values."); return static_cast<std::uint32_t>(number); };
				typed.extent = {integer(recipe.at("extent").at("width")), integer(recipe.at("extent").at("height"))};
				if(!typed.extent.width || !typed.extent.height || typed.extent.width > 1920 || typed.extent.height > 1080
					|| recipe.at("extent") != step.at("nativeExtent")) throw std::runtime_error("Depth recipe extent differs from its bounded native camera.");
				typed.fillBits = integer(recipe.at("fillBits")); std::set<std::pair<std::uint32_t,std::uint32_t>> pixels;
				const auto& overrides = recipe.at("overrides"); if(!overrides.is_array() || overrides.size() > 1920U * 1080U) throw std::runtime_error("Depth overrides exceed bounded input.");
				for(const auto& value : overrides) { Renderer::LightingDepthReduction::PixelOverride pixel{integer(value.at("x")), integer(value.at("y")), integer(value.at("bits"))}; if(pixel.x >= typed.extent.width || pixel.y >= typed.extent.height || !pixels.insert({pixel.x,pixel.y}).second) throw std::runtime_error("Depth override is outside its image or duplicated."); typed.overrides.push_back(pixel); }
				next.oracleRecipe = std::move(typed);
			}
			output = std::move(next); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
	bool ParseRendererEfficiencyDepthCameras(const nlohmann::json& script, const nlohmann::json& step,
		std::span<const Renderer::RendererEfficiencyFixtureCamera> primary,
		std::vector<Renderer::RendererEfficiencyFixtureCamera>& output, std::string& error)
	{
		// Preserve every sealed pose/projection field, changing only the declared
		// native image extent. Panel visibility and author UI cannot resize proof.
		try
		{
			std::vector<Renderer::RendererEfficiencyFixtureCamera> next(primary.begin(), primary.end());
			if(next.empty() || next.size() > 2) throw std::runtime_error("Depth fixture requires actual sealed views.");
			if(step.contains("nativeExtent"))
			{
				if(step.at("cameraTransforms").at("cameras") != script.at("cameras") || step.at("cameraTransforms").at("nativeExtent") != step.at("nativeExtent")) throw std::runtime_error("Depth step changed its sealed camera authority.");
				const auto& extent = step.at("nativeExtent"); std::uint64_t width,height;
				if(!Unsigned(extent.at("width"),width) || !Unsigned(extent.at("height"),height)
					|| !((width==1920 && height==1080) || (width==1919 && height==1079) || (width==7 && height==5))) throw std::runtime_error("Depth fixture extent is not a frozen native resolution.");
				for(auto& camera : next) camera.nativeExtent = {static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)};
			}
			output = std::move(next); error.clear(); return true;
		}
		catch(const std::exception& exception) { error=exception.what(); return false; }
	}
	bool HashRendererEfficiencyOwnedInput(const std::filesystem::path& root,
		const std::filesystem::path& relative, std::string& hash, std::string& error)
	{
		try { auto prepared = InstalledStepsHash(ReadOwnedBytes(root, relative)); hash = std::move(prepared); error.clear(); return true; }
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
	bool PrepareRendererEfficiencySourcePatch(const std::filesystem::path& root,
		const std::filesystem::path& source, const std::filesystem::path& replacement,
		std::string_view retainedBaseline, std::string_view expectedPriorHash,
		RendererEfficiencySourcePatch& output, std::string& error)
	{
		try
		{
			RendererEfficiencySourcePatch prepared; prepared.sourcePath = source;
			if(source.extension() != ".gltf" || source.begin() == source.end() || *source.begin() != "Assets")
				throw std::runtime_error("Fixture resource patches require a contained authored glTF source");
			const auto current = ReadOwnedBytes(root, source); prepared.priorHash = InstalledStepsHash(current);
			if(!expectedPriorHash.empty() && prepared.priorHash != expectedPriorHash) throw std::runtime_error("Authored source changed outside the owned fixture sequence");
			prepared.baselineBytes = retainedBaseline.empty() ? ReadSealedBytes(root, source) : std::string(retainedBaseline);
			prepared.baselineHash = InstalledStepsHash(prepared.baselineBytes);
			prepared.replacementBytes = replacement.empty() ? prepared.baselineBytes : ReadSealedBytes(root, replacement);
			const auto document = nlohmann::json::parse(prepared.replacementBytes);
			if(document.at("asset").at("version") != "2.0") throw std::runtime_error("Fixture replacement is not a glTF2 document");
			prepared.replacementHash = InstalledStepsHash(prepared.replacementBytes);
			output = std::move(prepared); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
	bool PrepareRendererEfficiencyPageVariantPatch(const std::filesystem::path& root,
		const std::filesystem::path& source, const std::filesystem::path& replacement,
		std::string_view originalBytes, std::string_view expectedPriorHash,
		std::string_view denseBaselineBytes, RendererEfficiencySourcePatch& output, std::string& error)
	{
		RendererEfficiencySourcePatch prepared;
		if(!PrepareRendererEfficiencySourcePatch(root, source, replacement, originalBytes, expectedPriorHash, prepared, error)) return false;
		// A retained cleanup source is not the content baseline for this edit.
		// Verify the actual current source equals the dense bytes observed before
		// allowing a displacement replacement, then export that semantic baseline.
		if(denseBaselineBytes.empty() || InstalledStepsHash(denseBaselineBytes) != prepared.priorHash)
		{ error = "Page edit requires its exact current dense baseline source bytes."; return false; }
		prepared.baselineBytes = std::string(denseBaselineBytes); prepared.baselineHash = prepared.priorHash;
		if(prepared.replacementHash == prepared.baselineHash) { error = "Page edit did not change dense source content."; return false; }
		output = std::move(prepared); error.clear(); return true;
	}
	bool RendererEfficiencyDepthObservationUnavailable(const Renderer::RendererEfficiencyEvidence& row,
		const Renderer::RendererEfficiencyFixtureIntent& applied, const Renderer::RendererEfficiencyOptionsSnapshot& options,
		std::uint64_t lastAcceptedFrame)
	{
		// A terminal depth readback belongs only to this protected owner. Neither
		// a stale failure nor an intentionally unavailable shadow fault can end it.
		return row.kind==Renderer::RendererEfficiencyEvidenceKind::LightingDepth && applied.depthControls
			&& std::any_of(applied.cameras.begin(),applied.cameras.end(),[&](const auto& camera){return camera.viewType==row.viewType && camera.outputId==row.viewId;})
			&& RendererEfficiencyCompletedControlMatches(row,applied)
			&& row.frameSerial>lastAcceptedFrame && row.status==Renderer::RendererEfficiencyEvidenceStatus::Unavailable
			&& row.effectiveOptions.settingsGeneration==options.settingsGeneration && row.effectiveOptions.rendererSessionGeneration==options.rendererSessionGeneration
			&& row.effectiveOptions.optionsSessionGeneration==options.optionsSessionGeneration && row.effectiveOptions.options==options.options;
	}
	bool SelectRendererEfficiencyDepthBundle(const nlohmann::json& bundle,
		const Renderer::RendererEfficiencyFixtureIntent& applied, const Renderer::RendererEfficiencyOptionsSnapshot& options,
		std::span<const Renderer::RendererEfficiencyEvidence> rows, std::vector<Renderer::RendererEfficiencyEvidence>& output, std::string& error)
	{
		// Join immutable completed owners, including delayed query availability.
		// Numerical oracle comparison belongs to the off-frame exporter/grader.
		try
		{
			using namespace Renderer; std::vector<RendererEfficiencyEvidence> next; std::set<std::pair<RenderViewType,std::uint32_t>> views;
			if(!applied.depthControls && std::any_of(bundle.at("views").begin(),bundle.at("views").end(),[](const auto& view){return view.contains("depthPurposes");})) throw std::runtime_error("Depth purpose has no actual protected typed controls.");
			for(const auto& view : bundle.at("views"))
			{
				if(!view.contains("depthPurposes")) continue;
				const auto type=view.at("viewType")=="Scene" ? RenderViewType::Scene : RenderViewType::Game; const auto id=view.at("viewId").get<std::uint32_t>();
				if(!views.insert({type,id}).second) {const auto prior=std::find_if(bundle.at("views").begin(),bundle.at("views").end(),[&](const auto& value){return value.at("viewType")==view.at("viewType") && value.at("viewId")==view.at("viewId");}); if(prior->at("depthPurposes")!=view.at("depthPurposes")) throw std::runtime_error("Repeated depth view has conflicting purposes."); continue;}
				const bool oracle=applied.depthControls && applied.depthControls->oracleRecipe.has_value();
				if(view.at("depthPurposes") != (oracle ? nlohmann::json::array({"Production","Oracle"}) : nlohmann::json::array({"Production"}))) throw std::runtime_error("Depth purposes differ from actual isolated controls.");
				const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.kind==RendererEfficiencyEvidenceKind::LightingDepth && row.viewType==type && row.viewId==id && RendererEfficiencyCompletedControlMatches(row,applied);});
				if(found==rows.end() || found->status!=RendererEfficiencyEvidenceStatus::Valid || !found->artifacts || !found->artifacts->lightingDepth || !found->artifacts->frameColor || !found->frameSerial || !found->commandSerial || !found->submitSerial || found->completionSerial<found->submitSerial
					|| found->effectiveOptions.options!=options.options || found->effectiveOptions.optionsSessionGeneration!=options.optionsSessionGeneration || !options.optionsSessionGeneration || found->effectiveOptions.settingsGeneration!=options.settingsGeneration || found->effectiveOptions.rendererSessionGeneration!=options.rendererSessionGeneration) throw std::runtime_error("Awaiting exact current completed lighting-depth owner.");
				const auto camera=std::find_if(applied.cameras.begin(),applied.cameras.end(),[&](const auto& value){return value.viewType==type && value.outputId==id;}); const auto& bytes=*found->artifacts->lightingDepth;
				if(camera==applied.cameras.end() || bytes.productionWidth!=camera->nativeExtent.x || bytes.productionHeight!=camera->nativeExtent.y || bytes.productionBounds.size()!=144U*8U) throw std::runtime_error("Depth bounds do not cover the sealed native camera extent.");
				auto queryMatches=[&](const RendererEfficiencyDepthQueryEvidence& query,VulkanLightingDepthTiming::Purpose purpose) {
					const auto& result=query.result; const auto& owner=result.owner;
					return result.status==VulkanLightingDepthTiming::Status::Valid && owner.purpose==purpose && owner.scope==VulkanLightingDepthTiming::Scope::IsolatedDepth && owner.rendererSessionGeneration==found->effectiveOptions.rendererSessionGeneration && owner.backendResourceGeneration && owner.queryPoolGeneration
						&& owner.frameSerial==found->frameSerial && owner.commandSerial==found->commandSerial && owner.outputId==id && owner.view==(type==RenderViewType::Scene ? VulkanLightingDepthTiming::View::Scene : VulkanLightingDepthTiming::View::Game) && owner.frameSlot<3 && owner.snapshotSlot<8
						&& result.submitSerial==found->submitSerial && result.completionSerial==found->completionSerial && query.computeStageMarkers && query.noWaitRead; };
				if(!queryMatches(bytes.productionQuery,VulkanLightingDepthTiming::Purpose::Production)) throw std::runtime_error("Production depth lacks its real isolated completed query.");
				if(oracle) { if(!bytes.oracleRecipe || bytes.oracleRecipe!=applied.depthControls->oracleRecipe || !bytes.oracleQuery || !queryMatches(*bytes.oracleQuery,VulkanLightingDepthTiming::Purpose::Oracle) || bytes.oracleInput.size()!=std::uint64_t(bytes.productionWidth)*bytes.productionHeight*4U || bytes.oracleBounds.size()!=144U*8U || bytes.oracleCoverage.size()!=144U*8U) throw std::runtime_error("Oracle depth lacks complete actual input/bounds/coverage owners."); }
				else if(bytes.oracleRecipe || bytes.oracleQuery || !bytes.oracleInput.empty() || !bytes.oracleBounds.empty() || !bytes.oracleCoverage.empty()) throw std::runtime_error("Production-only view contains unexpected oracle proof.");
				next.push_back(*found);
			}
			if(next.size()!=static_cast<std::size_t>(std::count_if(rows.begin(),rows.end(),[](const auto& row){return row.kind==RendererEfficiencyEvidenceKind::LightingDepth;}))) throw std::runtime_error("Depth cohort has duplicate or unexpected views.");
			output=std::move(next); error.clear(); return true;
		}
		catch(const std::exception& exception) {error=exception.what();return false;}
	}
	bool RendererEfficiencyTransitionNeedsSamplingRearm(const Renderer::RendererEfficiencyFixtureIntent& consumed,
		const Renderer::RendererEfficiencyFixtureCorrelation& measurement)
	{
		// Control and authored-mutation clocks have no shared numeric domain.
		// An accidental equality leaves the RHI's original frame pinned until a
		// real queue receipt supplies a new control owner for the same token.
		return consumed.controlsApplied && consumed.controlGeneration != 0 && consumed.correlation == measurement;
	}
	bool SelectRendererEfficiencyControlTransition(const nlohmann::json& requiredBundle,
		const Renderer::RendererEfficiencyFixtureIntent& applied,
		const Renderer::RendererEfficiencyOptionsSnapshot& options,
		std::span<const Renderer::RendererEfficiencyEvidence> rows,
		std::vector<Renderer::RendererEfficiencyEvidence>& output, std::string& error)
	{
		// Rows are copied immutable CPU owners. The recipe names every required
		// region; this selector never constructs a missing face or modifies output
		// on rejection. The exporter subsequently validates complete pixel bytes.
		if(!applied.controlsApplied || !applied.controlGeneration || !options.optionsSessionGeneration || rows.empty() || rows.size() > 64) { error = "Awaiting completed original control cohort."; return false; }
		if(requiredBundle.at("views").front().contains("lightingPurposes"))
		{
			std::vector<Renderer::RendererEfficiencyEvidence> prepared;
			if(!SelectRendererEfficiencyLightingBundle(requiredBundle, applied, rows, prepared, error)) return false;
			for(const auto& row : prepared) if(row.effectiveOptions.optionsSessionGeneration != options.optionsSessionGeneration || row.effectiveOptions.options != options.options || !row.artifacts || !row.artifacts->frameColor)
			{ error = "Lighting transition lacks current options or complete native artifact owners."; return false; }
			if(requiredBundle.at("views").front().contains("faceIndices"))
			{
				// Preserve all original shadow regions; lighting cannot stand in for
				// a missing cube face in a shared validation workload.
				auto shadowBundle = requiredBundle;
				for(auto& view : shadowBundle["views"]) { view.erase("lightingPurposes"); view.erase("depthPurposes"); }
				std::vector<Renderer::RendererEfficiencyEvidence> shadowRows, selectedShadow;
				for(const auto& row : rows) if(row.kind == Renderer::RendererEfficiencyEvidenceKind::Shadow) shadowRows.push_back(row);
				if(!SelectRendererEfficiencyControlTransition(shadowBundle, applied, options, shadowRows, selectedShadow, error)) return false;
				for(const auto& row : selectedShadow)
				{
					if(row.frameSerial != prepared.front().frameSerial || row.submitSerial != prepared.front().submitSerial || row.completionSerial != prepared.front().completionSerial) { error = "Shared lighting and shadow owners belong to different completed cohorts."; return false; }
					for(const auto& light : prepared) if(light.viewType == row.viewType && light.viewId == row.viewId && light.commandSerial != row.commandSerial) { error = "Shared purposes disagree on their actual view command."; return false; }
				}
				// Keep the original shadow representative for existing preparation and
				// fault receipts; purpose rows supplement rather than replace it.
				prepared.insert(prepared.begin(), selectedShadow.begin(), selectedShadow.end());
			}
			std::vector<Renderer::RendererEfficiencyEvidence> depth;
			if(!SelectRendererEfficiencyDepthBundle(requiredBundle, applied, options, rows, depth, error)) return false;
			for(const auto& row : depth) {
				if(row.frameSerial != prepared.front().frameSerial || row.submitSerial != prepared.front().submitSerial || row.completionSerial != prepared.front().completionSerial) { error="Depth and lighting/shadow owners belong to different native completions."; return false; }
				for(const auto& prior : prepared) if(prior.viewType==row.viewType && prior.viewId==row.viewId && prior.commandSerial!=row.commandSerial) {error="Depth and view purposes disagree on actual commands."; return false;}
			}
			prepared.insert(prepared.end(),depth.begin(),depth.end());
			if(prepared.size() != rows.size()) { error = "Shared transition contains duplicate or unexpected regions."; return false; }
			output = std::move(prepared); return true;
		}
		std::vector<Renderer::RendererEfficiencyEvidence> selected;
		for(const auto& view : requiredBundle.at("views")) for(const auto& face : view.at("faceIndices"))
		{
			const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
				return RendererEfficiencyCompletedControlMatches(row, applied)
					&& row.status == Renderer::RendererEfficiencyEvidenceStatus::Valid && row.artifacts && row.artifacts->frameColor
					&& row.frameSerial && row.commandSerial && row.submitSerial && row.completionSerial >= row.submitSerial
					&& row.effectiveOptions.settingsGeneration == applied.correlation.settingsGeneration
					&& row.effectiveOptions.rendererSessionGeneration == applied.correlation.rendererSessionGeneration
					&& row.effectiveOptions.optionsSessionGeneration == options.optionsSessionGeneration
					&& row.effectiveOptions.settingsGeneration == options.settingsGeneration && row.effectiveOptions.rendererSessionGeneration == options.rendererSessionGeneration
					&& row.effectiveOptions.options == options.options
					&& row.viewType == (view.at("viewType") == "Scene" ? RenderViewType::Scene : RenderViewType::Game)
					&& row.viewId == view.at("viewId").get<std::uint32_t>() && row.lightId == ParseUUID(view.at("lightId").get<std::string>()).value()
					&& row.faceIndex == face.get<std::uint32_t>();
			});
			if(found == rows.end()) { error = "Awaiting every original completed control region."; return false; }
			if(!selected.empty() && (found->frameSerial != selected.front().frameSerial
				|| found->submitSerial != selected.front().submitSerial || found->completionSerial != selected.front().completionSerial
				|| found->effectiveOptions.options != selected.front().effectiveOptions.options
				|| found->effectiveOptions.optionsSessionGeneration != selected.front().effectiveOptions.optionsSessionGeneration)) { error = "Control transition combines different native owners."; return false; }
			selected.push_back(*found);
			// Each output records its own native primary command buffer. Require
			// one command per output, rather than inventing a common Scene/Game one.
			for(const auto& prior : selected) if(prior.viewType == found->viewType && prior.viewId == found->viewId && prior.commandSerial != found->commandSerial)
			{ error = "Control transition combines different commands for one view."; return false; }
		}
		if(selected.empty() || selected.size() != rows.size()) { error = "Control transition has missing or duplicate regions."; return false; }
		output = std::move(selected); error.clear(); return true;
	}
	bool RendererEfficiencyCompletedControlMatches(const Renderer::RendererEfficiencyEvidence& row,
		const Renderer::RendererEfficiencyFixtureIntent& applied)
	{
		// A baseline and its later edit can share frame-zero correlation. Only
		// the actual protected application owner qualifies the completed row.
		return applied.controlsApplied && applied.controlGeneration != 0 && row.fixtureControlGeneration == applied.controlGeneration
			&& row.fixtureCorrelation == applied.correlation;
	}
	bool RendererEfficiencyLightingExportOwnerMatches(const Renderer::RendererEfficiencyFixtureIntent& captured,
		std::uint64_t optionsEpoch, const Renderer::RendererEfficiencyOptions& expectedOptions,
		const Renderer::RendererEfficiencyFixtureIntent& current,
		const Renderer::RendererEfficiencyOptionsSnapshot& options)
	{
		// Project ownership cannot detect a same-project control/settings change.
		// Bind worker output to the protected owner that supplied its real bytes.
		return captured.controlsApplied && captured.controlGeneration && optionsEpoch
			&& RendererEfficiencyFixtureControlsMatch(captured, current) && current.controlGeneration == captured.controlGeneration
			&& options.optionsSessionGeneration == optionsEpoch && options.options == expectedOptions
			&& options.settingsGeneration == captured.correlation.settingsGeneration
			&& options.rendererSessionGeneration == captured.correlation.rendererSessionGeneration;
	}
	bool RendererEfficiencyDensePageBaselineRetryable(std::span<const Renderer::RendererEfficiencyEvidence> rows,
		const Renderer::RendererEfficiencyFixtureIntent& applied, const UUID& target, const UUID& mesh, std::uint64_t publication)
	{
		if(rows.size() != 9 || !publication || mesh == EMPTY_UUID || target == EMPTY_UUID) return false;
		bool notReady = false;
		for(const auto& row : rows)
		{
			if(row.status != Renderer::RendererEfficiencyEvidenceStatus::Valid || !row.artifacts || !row.frameSerial || !row.commandSerial || !row.submitSerial || !row.completionSerial
				|| !RendererEfficiencyCompletedControlMatches(row, applied)
				|| row.effectiveOptions.settingsGeneration != applied.correlation.settingsGeneration || row.effectiveOptions.rendererSessionGeneration != applied.correlation.rendererSessionGeneration
				|| row.frameSerial != rows.front().frameSerial || row.commandSerial != rows.front().commandSerial || row.submitSerial != rows.front().submitSerial || row.completionSerial != rows.front().completionSerial
				|| std::count_if(rows.begin(), rows.end(), [&](const auto& other) { return other.viewType == row.viewType && other.viewId == row.viewId && other.lightId == row.lightId && other.faceIndex == row.faceIndex; }) != 1) return false;
			bool found = false;
			for(const auto& stamp : row.artifacts->eligibleMembership.rows) if(stamp.objectId == target)
			{
				// Retry only real complete owner draws from the retained dense mesh.
				// A wrong publication or stale control is an identity failure, not
				// evidence that another capture might make that same row acceptable.
				if(stamp.meshId != mesh || stamp.meshRevision != publication || !stamp.lifetimeGeneration || !stamp.residencyGeneration) return false;
				if(stamp.payloadSource == 0) notReady = true;
				else if(stamp.payloadSource == 0x100U && stamp.pageContentRevision == publication && stamp.payloadContentRevision == publication) notReady |= stamp.payloadByteSize == 0;
				else return false;
				found = true;
			}
			if(!found) return false;
		}
		return notReady;
	}
	bool RendererEfficiencyDensePageBaselineMatches(std::span<const Renderer::RendererEfficiencyEvidence> rows,
		const Renderer::RendererEfficiencyFixtureIntent& applied, const UUID& target, const UUID& mesh,
		std::uint64_t publication, std::string& error)
	{
		if(!applied.controlsApplied || !applied.controlGeneration || rows.size() != 9 || !publication || target == EMPTY_UUID || mesh == EMPTY_UUID)
		{ error = "Dense page baseline lacks actual protected controls or owner publication."; return false; }
		for(const auto& row : rows)
		{
			if(std::count_if(rows.begin(), rows.end(), [&](const auto& other) { return other.viewType == row.viewType && other.viewId == row.viewId && other.lightId == row.lightId && other.faceIndex == row.faceIndex; }) != 1)
			{ error = "Dense page baseline has duplicate required native region identities."; return false; }
			if(row.status != Renderer::RendererEfficiencyEvidenceStatus::Valid || !row.artifacts || !row.frameSerial || !row.commandSerial || !row.submitSerial || !row.completionSerial
				|| !RendererEfficiencyCompletedControlMatches(row, applied)
				|| row.effectiveOptions.settingsGeneration != applied.correlation.settingsGeneration || row.effectiveOptions.rendererSessionGeneration != applied.correlation.rendererSessionGeneration
				|| row.frameSerial != rows.front().frameSerial || row.commandSerial != rows.front().commandSerial || row.submitSerial != rows.front().submitSerial || row.completionSerial != rows.front().completionSerial)
			{ error = "Dense page baseline lacks exact current completed native evidence."; return false; }
			bool owner = false;
			for(const auto& stamp : row.artifacts->eligibleMembership.rows) if(stamp.objectId == target)
			{
				if(stamp.meshId != mesh || stamp.meshRevision != publication || stamp.pageContentRevision != publication || stamp.payloadContentRevision != publication
					|| stamp.payloadSource != 0x100U || !stamp.payloadByteSize || !stamp.residencyGeneration || !stamp.lifetimeGeneration)
				{ error = "Dense page baseline lacks exact published native page bytes: target=" + ToHexString(target)
					+ " mesh=" + ToHexString(stamp.meshId) + " expectedMeshPublication=" + std::to_string(publication)
					+ " actualMeshPublication=" + std::to_string(stamp.meshRevision) + " route=" + std::to_string(stamp.payloadSource)
					+ " payloadBytes=" + std::to_string(stamp.payloadByteSize); return false; }
				owner = true;
			}
			if(!owner) { error = "Dense page baseline target is absent from a required completed region."; return false; }
		}
		error.clear(); return true;
	}
	bool ApplyRendererEfficiencySourcePatch(const std::filesystem::path& root,
		const RendererEfficiencySourcePatch& patch, std::string& error)
	{
		std::filesystem::path temporary;
		try
		{
			const auto destination = OwnedRegularFile(root, patch.sourcePath);
			if(InstalledStepsHash(ReadOwnedBytes(root, patch.sourcePath)) != patch.priorHash
				|| InstalledStepsHash(patch.replacementBytes) != patch.replacementHash) throw std::runtime_error("Prepared fixture source patch became stale");
			temporary = destination.parent_path() / (".renderer-efficiency-" + ToHexString(GenerateUUID()) + ".tmp");
			{ std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output.write(patch.replacementBytes.data(), static_cast<std::streamsize>(patch.replacementBytes.size())); output.close(); if(!output) throw std::runtime_error("Fixture replacement cannot be written"); }
			// Watchers and compilers see only a complete authored source replacement.
#if defined(_WIN32)
			if(!ReplaceFileW(destination.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) throw std::runtime_error("Atomic fixture replacement failed");
#else
			std::filesystem::rename(temporary, destination);
#endif
			error.clear(); return true;
		}
		catch(const std::exception& exception)
		{
			if(!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
			error = exception.what(); return false;
		}
	}
	bool ParseRendererEfficiencyOptions(const nlohmann::json& request, const Renderer::RendererEfficiencyOptions& base,
		Renderer::RendererEfficiencyOptions& output, std::string& error)
	{
		error.clear();
		if(!request.is_object()) { error = "Renderer efficiency options must be an object."; return false; }
		auto next = base;
		for(const auto& [key, value] : request.items())
		{
			bool valid = false;
			if(key == "shadowReuse") valid = Mode(value, "Enabled", "AlwaysRefresh", next.shadowReuse);
			else if(key == "lightingUploads") valid = Mode(value, "Dirty", "Full", next.lightingUploads);
			else if(key == "lightingDepth") valid = Mode(value, "Cooperative", "CompleteScan", next.lightingDepth);
			else if(key == "shadowSubmission") valid = Mode(value, "Grouped", "Singleton", next.shadowSubmission);
			else if(key == "validationPreparation") valid = Mode(value, "Concurrent", "Serial", next.validationPreparation);
			else if(key == "staticShadowCacheBudgetBytes") valid = Unsigned(value, next.staticShadowCacheBudgetBytes)
				&& next.staticShadowCacheBudgetBytes <= Renderer::kRendererEfficiencyMaximumStaticBudgetBytes;
			if(!valid) { error = "Invalid or unknown renderer efficiency option: " + key; return false; }
		}
		if(!Renderer::ValidateRendererEfficiencyOptions(next, &error)) return false;
		output = next;
		return true;
	}
	bool ValidateRendererEfficiencyOwnedProject(const std::filesystem::path& currentRoot,
		std::string_view fixtureId, std::string_view fixtureHash, std::string& error)
	{
		error.clear();
		try
		{
			const auto root = std::filesystem::canonical(currentRoot);
			const auto markerPath = root / "renderer-efficiency-project.json";
			// A redirected receipt cannot authorize mutation of another project.
			if(std::filesystem::is_symlink(markerPath) || std::filesystem::canonical(markerPath).parent_path() != root)
			{ error = "Ownership marker must be a regular file in the current project."; return false; }
			std::ifstream input(markerPath, std::ios::binary);
			const auto marker = nlohmann::json::parse(input);
			if(!marker.is_object() || marker.value("schemaVersion", 0) != 1
				|| marker.value("kind", std::string{}) != "RendererEfficiencyValidationProject"
				|| fixtureId.empty() || fixtureHash.empty()
				|| marker.value("fixtureId", std::string{}) != fixtureId
				|| marker.value("fixtureHash", std::string{}) != fixtureHash
				|| std::filesystem::canonical(marker.at("projectRoot").get<std::string>()) != root)
			{ error = "Current project, fixture identity or ownership marker does not match."; return false; }
			return true;
		}
		catch(const std::exception&) { error = "A valid owned renderer-efficiency-project.json is required."; return false; }
	}
	nlohmann::json SerializeRendererEfficiencyOptions(const Renderer::RendererEfficiencyOptions& value)
	{
		using namespace Renderer;
		return {{"shadowReuse", value.shadowReuse == RendererEfficiencyShadowReuse::Enabled ? "Enabled" : "AlwaysRefresh"},
			{"lightingUploads", value.lightingUploads == RendererEfficiencyLightingUploads::Dirty ? "Dirty" : "Full"},
			{"lightingDepth", value.lightingDepth == RendererEfficiencyLightingDepth::Cooperative ? "Cooperative" : "CompleteScan"},
			{"shadowSubmission", value.shadowSubmission == RendererEfficiencyShadowSubmission::Grouped ? "Grouped" : "Singleton"},
			{"validationPreparation", value.validationPreparation == RendererEfficiencyPreparation::Concurrent ? "Concurrent" : "Serial"},
			{"staticShadowCacheBudgetBytes", value.staticShadowCacheBudgetBytes}};
	}
	nlohmann::json SerializeRendererEfficiencyToken(const Renderer::RendererEfficiencyOptionsToken& token)
	{
		return {{"optionsSessionGeneration", token.optionsSessionGeneration}, {"options", SerializeRendererEfficiencyOptions(token.options)}, {"valid", token.valid}};
	}
	bool ParseRendererEfficiencyToken(const nlohmann::json& value, Renderer::RendererEfficiencyOptionsToken& token, std::string& error)
	{
		Renderer::RendererEfficiencyOptionsToken next;
		if(!value.is_object() || !value.contains("optionsSessionGeneration") || !Unsigned(value["optionsSessionGeneration"], next.optionsSessionGeneration)
			|| !value.contains("options") || !value.contains("valid") || !value["valid"].is_boolean() || !value["valid"].get<bool>())
		{ error = "priorToken must contain a valid renderer session and options."; return false; }
		if(!ParseRendererEfficiencyOptions(value["options"], {}, next.options, error)) return false;
		next.valid = true; token = next; return true;
	}
	nlohmann::json SerializeRendererEfficiencyEvidence(const Renderer::RendererEfficiencyEvidence& value)
	{
		const auto counter = [](const Renderer::RendererEfficiencyCounter& field) {
			return nlohmann::json{{"status", StatusName(field.status)}, {"value", field.status == Renderer::RendererEfficiencyEvidenceStatus::Valid && field.value ? nlohmann::json(*field.value) : nlohmann::json(nullptr)}, {"unavailableReason", field.reason}};
		};
		// This public automation snapshot serializes copied receipts only. Native
		// uploads remain typed; malformed oversized lists are explicitly unavailable.
		nlohmann::json uploads = nlohmann::json::array();
		for(const auto& upload : value.lightingUploads)
		{
			const bool bounded = upload.payloadLightIds.size() <= 1024 && upload.logicalWriteRanges.size() <= 1024 && upload.physicalFlushRanges.size() <= 1024;
			nlohmann::json ids = nlohmann::json::array(); if(bounded) for(const auto& id : upload.payloadLightIds) ids.push_back(ToHexString(id));
			uploads.push_back({{"available", upload.available && bounded}, {"succeeded", upload.succeeded && bounded}, {"analysisPurpose", upload.analysisPurpose},
				{"allocationId", upload.allocationId}, {"allocationGeneration", upload.allocationGeneration}, {"allocationSizeBytes", upload.allocationSizeBytes}, {"regionOffsetBytes", upload.regionOffsetBytes}, {"regionSizeBytes", upload.regionSizeBytes}, {"nonCoherentAtomSize", upload.nonCoherentAtomSize},
				{"coherent", upload.coherent}, {"frameSlot", upload.frameSlot}, {"snapshotSlot", upload.snapshotSlot}, {"activeRecordCount", upload.activeRecordCount},
				{"logicalWriteBytes", upload.logicalWriteBytes}, {"logicalWriteRecords", upload.logicalWriteRecords}, {"physicalFlushBytes", upload.physicalFlushBytes},
				{"mirrorCpuDurationNanoseconds", upload.mirrorCpuDurationNanoseconds}, {"payloadLightIds", std::move(ids)},
				{"logicalWriteRanges", bounded ? nlohmann::json(upload.logicalWriteRanges) : nlohmann::json::array()}, {"physicalFlushRanges", bounded ? nlohmann::json(upload.physicalFlushRanges) : nlohmann::json::array()},
				{"reason", bounded ? upload.reason : "Native lighting receipt exceeds bounded public diagnostics."}});
		}
		return {{"schemaVersion", value.schemaVersion}, {"status", StatusName(value.status)}, {"unavailableReason", value.unavailableReason},
			{"evidenceKind", value.kind == Renderer::RendererEfficiencyEvidenceKind::Lighting ? "Lighting" : value.kind == Renderer::RendererEfficiencyEvidenceKind::LightingDepth ? "LightingDepth" : "Shadow"}, {"analysisPurpose", value.analysisPurpose}, {"lightingUploads", std::move(uploads)},
			{"viewType", value.viewType == RenderViewType::Game ? "Game" : "Scene"}, {"viewId", value.viewId}, {"lightId", TerraEngine::ToHexString(value.lightId)}, {"faceIndex", value.faceIndex},
			{"resolution", {value.resolution.x, value.resolution.y}}, {"frameSerial", value.frameSerial}, {"commandSerial", value.commandSerial}, {"submitSerial", value.submitSerial}, {"completionSerial", value.completionSerial},
			{"settingsGeneration", value.effectiveOptions.settingsGeneration}, {"rendererSessionGeneration", value.effectiveOptions.rendererSessionGeneration}, {"effectiveOptions", SerializeRendererEfficiencyOptions(value.effectiveOptions.options)},
			{"lightingLogicalWriteBytes", counter(value.lightingLogicalWriteBytes)}, {"lightingWriteRecords", counter(value.lightingWriteRecords)}, {"lightingWriteRanges", counter(value.lightingWriteRanges)},
			{"lightingPhysicalFlushBytes", counter(value.lightingPhysicalFlushBytes)}, {"lightingFlushRanges", counter(value.lightingFlushRanges)}, {"eligibleCasterSegments", counter(value.eligibleCasterSegments)},
			{"submittedCasterSegments", counter(value.submittedCasterSegments)}, {"actualDrawSubmissions", counter(value.actualDrawSubmissions)}, {"groups", counter(value.groups)}, {"singletons", counter(value.singletons)},
			{"staticRefreshes", counter(value.staticRefreshes)}, {"dynamicRefreshes", counter(value.dynamicRefreshes)}, {"shadowReuses", counter(value.shadowReuses)},
			{"staticReservedBytes", counter(value.staticReservedBytes)}, {"staticActiveBytes", counter(value.staticActiveBytes)}, {"staticRetiringBytes", counter(value.staticRetiringBytes)},
			{"staticCacheBudgetRequestedBytes", value.staticCacheBudgetRequestedBytes}, {"staticCacheBudgetEffectiveBytes", value.staticCacheBudgetEffectiveBytes}, {"staticCacheDraining", value.staticCacheDraining},
			{"lightingDepthTiming", {{"status", StatusName(value.lightingDepthTiming.status)}, {"durationMilliseconds", value.lightingDepthTiming.status == Renderer::RendererEfficiencyEvidenceStatus::Valid && value.lightingDepthTiming.durationMilliseconds ? nlohmann::json(*value.lightingDepthTiming.durationMilliseconds) : nlohmann::json(nullptr)}, {"unavailableReason", value.lightingDepthTiming.unavailableReason}}},
			{"refreshReason", value.refreshReason}, {"fallbackReason", value.fallbackReason}, {"contentOwner", value.contentOwner}};
	}
	bool EditorRendererEfficiencyValidation::Start(const std::filesystem::path& root, std::uint64_t generation,
		const nlohmann::json& request, const Renderer::RendererEfficiencyOptionsSnapshot& options, std::string& error)
	{
		if(active) { error = "A renderer efficiency validation session is already active."; return false; }
		try
		{
			const std::string id = request.at("fixtureId").get<std::string>(), hash = request.at("fixtureHash").get<std::string>();
			if(!ValidateRendererEfficiencyOwnedProject(root, id, hash, error)) return false;
			const auto requestedStage = request.value("stage", std::string("All"));
			const auto mode = request.value("mode", std::string("Candidate"));
			if(mode != "Reference" && mode != "Candidate") { error = "mode must be Reference or Candidate."; return false; }
			if(requestedStage != "All" && requestedStage != "P1" && requestedStage != "P2" && requestedStage != "P3" && requestedStage != "P4" && requestedStage != "P5")
			{ error = "stage must be P1 through P5 or All."; return false; }
			std::uint64_t nextSeed = 0, nextRun = 0, nextWarmup = 60, nextMeasured = 300, runs = 3;
			for(const auto& [name, maximum, target] : {std::tuple{"seed", UINT64_MAX, &nextSeed}, {"runIndex", UINT64_MAX, &nextRun}, {"warmupFrames", std::uint64_t{100000}, &nextWarmup}, {"measuredFrames", std::uint64_t{100000}, &nextMeasured}, {"runs", std::uint64_t{100}, &runs}})
				if(request.contains(name) && (!Unsigned(request[name], *target) || *target > maximum)) { error = std::string(name) + " is outside supported integer bounds."; return false; }
			if(nextMeasured == 0 || runs == 0) { error = "measuredFrames and runs must be positive."; return false; }
			if(request.contains("faultInjection")) { error = "Fault injection is unavailable until the selected stage installs its owned-session fault provider."; return false; }
			if(request.contains("ownershipMarkerPath") && std::filesystem::canonical(request["ownershipMarkerPath"].get<std::string>()) != std::filesystem::canonical(root / "renderer-efficiency-project.json"))
			{ error = "ownershipMarkerPath must name the current project's fixed marker."; return false; }
			std::ifstream markerInput(root / "renderer-efficiency-project.json", std::ios::binary);
			const auto marker = nlohmann::json::parse(markerInput);
			if(request.contains("fixtureManifestSha256") && (!request["fixtureManifestSha256"].is_string()
				|| marker.value("fixtureManifestSha256", std::string{}) != request["fixtureManifestSha256"].get<std::string>()))
			{ error = "The installed fixture manifest identity does not match the owned receipt."; return false; }
			const auto nextRunId = request.value("runId", std::string{});
			const auto nextOwnerId = marker.value("ownerId", std::string{});
			nlohmann::json nextSteps;
			std::vector<Renderer::RendererEfficiencyFixtureCamera> nextCameras;
			if(marker.contains("stepsHash"))
			{
				const auto path = root / "renderer-efficiency-steps.json";
				if(marker.value("stepsPath", std::string{}) != path.filename().string()
					|| std::filesystem::is_symlink(path) || std::filesystem::canonical(path).parent_path() != std::filesystem::canonical(root))
				{ error = "Installed fixture steps must be the owned project's fixed regular file."; return false; }
				if(std::filesystem::file_size(path) > 8U * 1024U * 1024U)
				{ error = "Installed fixture steps exceed the bounded input size."; return false; }
				std::ifstream stepsInput(path, std::ios::binary);
				const std::string bytes((std::istreambuf_iterator<char>(stepsInput)), {});
				if(InstalledStepsHash(bytes) != marker.at("stepsHash").get<std::string>())
				{ error = "Installed fixture steps do not match their owned SHA256 receipt."; return false; }
				nextSteps = nlohmann::json::parse(bytes);
				if(nextSteps.at("schemaVersion") != 1 || nextSteps.at("fixtureId") != id || nextSteps.at("fixtureHash") != hash
					|| nextSteps.at("seed") != nextSeed || !nextSteps.at("steps").is_array() || nextSteps.at("steps").empty())
				{ error = "Installed fixture steps identity or ordered steps are invalid."; return false; }
				const auto cameraReceipt = nlohmann::json::parse(ReadOwnedBytes(root, "renderer-efficiency-camera.json"));
				if(InstalledStepsHash(ReadOwnedBytes(root, "renderer-efficiency-camera.json")) != marker.at("cameraHash").get<std::string>()
					|| cameraReceipt.at("schemaVersion") != 1 || cameraReceipt.at("cameras") != nextSteps.at("cameras")
					|| cameraReceipt.at("nativeExtent") != nextSteps.at("nativeExtent") || nextSteps.at("cameras").empty() || nextSteps.at("cameras").size() > 2)
				{ error = "Every installed camera and native extent must match the complete sealed camera receipt."; return false; }
				const auto position = [](const nlohmann::json& values) {
					if(!values.is_array() || values.size() != 3) throw std::runtime_error("Camera world coordinate requires three authored metre values.");
					Scene::WorldPosition value; value.offsetMetres = {values[0].get<double>(), values[1].get<double>(), values[2].get<double>()};
					const auto normalized = Scene::NormalizeWorldPosition(value); if(!normalized.Succeeded()) throw std::runtime_error("Camera authored coordinate is outside canonical world bounds."); return normalized.position;
				};
				std::set<std::pair<RenderViewType, std::uint32_t>> cameraKeys;
				for(const auto& recipe : nextSteps.at("cameras"))
				{
					Renderer::RendererEfficiencyFixtureCamera camera;
					if(recipe.at("viewType") != "Scene" && recipe.at("viewType") != "Game") throw std::runtime_error("Camera view type is not an actual Scene/Game output.");
					camera.viewType = recipe.at("viewType") == "Scene" ? RenderViewType::Scene : RenderViewType::Game;
					camera.outputId = recipe.at("viewId").get<std::uint32_t>();
					if(camera.outputId != (camera.viewType == RenderViewType::Scene ? 1U : 2U) || !cameraKeys.insert({camera.viewType, camera.outputId}).second) throw std::runtime_error("Camera receipt must identify actual Scene1 and Game2 exactly once.");
					camera.cameraPosition = position(recipe.at("position")); camera.lookAtTarget = position(recipe.at("target")); camera.referenceOrigin = position(recipe.at("referenceOrigin"));
					const auto& up = recipe.at("up"); if(up.size() != 3) throw std::runtime_error("Camera up direction requires three values."); camera.up = {up[0].get<double>(), up[1].get<double>(), up[2].get<double>()};
					camera.verticalFovDegrees = recipe.at("verticalFovDegrees").get<double>(); camera.nearPlane = recipe.at("nearPlane").get<double>(); camera.farPlane = recipe.at("farPlane").get<double>();
					camera.nativeExtent = {nextSteps.at("nativeExtent").at("width").get<std::uint32_t>(), nextSteps.at("nativeExtent").at("height").get<std::uint32_t>()};
					if(!camera.nativeExtent.x || !camera.nativeExtent.y || camera.nearPlane <= 0 || camera.farPlane <= camera.nearPlane || camera.verticalFovDegrees <= 0 || camera.verticalFovDegrees >= 180) throw std::runtime_error("Camera projection or native extent is invalid.");
					nextCameras.push_back(camera);
				}
				if(nextSteps.at("stage") != requestedStage)
				{ error = "Installed fixture stage must exactly match the requested stage."; return false; }
				std::set<std::string> stepIds; nlohmann::json selectedIds = nlohmann::json::array();
				std::uint64_t lastOrdinal = 0; bool firstStep = true;
				for(const auto& step : nextSteps.at("steps"))
				{
					std::uint64_t ordinal = 0;
					const auto stepId = step.at("id").get<std::string>();
					if(stepId.empty() || !stepIds.insert(stepId).second || !Unsigned(step.at("ordinal"), ordinal)
						|| (!firstStep && ordinal <= lastOrdinal) || step.at("warmupFrames") != 60 || step.at("measuredFrames") != 300)
					{ error = "Installed fixture steps must be unique, ordered, and use the declared measurement policy."; return false; }
					firstStep = false; lastOrdinal = ordinal; selectedIds.push_back(stepId);
				}
				if(nextSteps.contains("selectedManifestStepIds"))
				{
					if(nextSteps.at("selectedStage") != requestedStage || nextSteps.at("selectedManifestStepIds") != selectedIds)
					{ error = "Installed stage selection does not match its exact ordered manifest subset."; return false; }
					const auto& excluded = nextSteps.at("excludedManifestStepIds");
					if(!excluded.is_array() || selectedIds.size() + excluded.size() != nextSteps.at("fullManifestStepCount").get<std::size_t>())
					{ error = "Installed stage selection does not account for every manifest step."; return false; }
					for(const auto& excludedId : excluded) if(!stepIds.insert(excludedId.get<std::string>()).second)
					{ error = "Installed stage selection contains duplicate or overlapping manifest steps."; return false; }
				}
				if(nextWarmup != 60 || nextMeasured != 300 || runs < 3 || nextRunId.empty() || nextSeed > 9007199254740991ULL)
				{ error = "Installed validation requires a run identity, an exact JSON seed, three runs, and 60 warmup/300 measured frames per step."; return false; }
			}
			projectRoot = std::filesystem::canonical(root); projectGeneration = generation;
			markerIdentity = marker.dump(); pendingTransactionId.clear(); pendingScenePublication = {}; mutationGeneration = 0;
			installedSteps = std::move(nextSteps); fixtureCameras = std::move(nextCameras); nativeProviderFailure = false; fixtureReceipts = nlohmann::json::array(); rejectedReceipt = nullptr;
			depthStepCameras.clear(); depthStepControls.clear(); fixtureRoleTargets=nlohmann::json::object();
			if(!installedSteps.is_null()) {
				for(const auto& [role,target] : installedSteps.at("targets").items()) {
					if(target.is_array()) continue;
					if(role=="depthPanels") {for(const auto& [panel,value] : target.items()) fixtureRoleTargets["depthPanel-"+panel]=value;}
					else fixtureRoleTargets[role]=target;
				}
				if(requestedStage=="P4" && installedSteps.value("shadowBatchFixture",false)) for(const auto& step:installedSteps.at("steps")) { std::vector<Renderer::RendererEfficiencyFixtureCamera> cameras; Renderer::RendererEfficiencyFixtureIntent controls; if(!ParseRendererEfficiencyShadowBatchCameras(installedSteps,step,fixtureCameras,cameras,error) || !ParseRendererEfficiencyShadowBatchControls(step,controls,error)) return false; depthStepCameras.push_back(std::move(cameras)); }
				if(requestedStage=="P3") for(const auto& step : installedSteps.at("steps")) {
					std::vector<Renderer::RendererEfficiencyFixtureCamera> cameras; Renderer::RendererEfficiencyFixtureDepthControls controls;
					if(!ParseRendererEfficiencyDepthCameras(installedSteps,step,fixtureCameras,cameras,error) || !ParseRendererEfficiencyDepthControls(projectRoot,installedSteps,step,controls,error)) return false;
					depthStepCameras.push_back(std::move(cameras));depthStepControls.push_back(std::move(controls));
				}
			}
			animationReadyStepFrame.reset(); animationPerSample=false; staticResourceApplied=false; staticBudgetRequestedGeneration=0; staticLowerBudgetRequestedGeneration=0; pendingStaticLifetimeOwner.reset(); completedStaticLifetimeOwner.reset(); pendingStaticBudgetReceipt=nullptr; publishedBindings.clear(); baselineInputs = {}; pendingAnimationCommands.clear(); pendingAnimationObjects.clear(); animationBefore.clear(); animationGpuBefore.reset(); preparedTransaction = {};
			stepIndex = 0; stepFrame = warmupCompleted = measuredCompleted = batchCursor = lastAcceptedFrame = 0;
				exportJob = {}; pendingExportFrame = 0; pendingExportPurpose = EvidenceExportPurpose::Sample; pendingTransitionReceipt = nullptr; pendingTransitionOptionsEpoch = 0;
				residencyBaselineObservation = nullptr; pendingResidencyReceipt = nullptr;
				pendingEvidenceBundles.clear(); exportedFrames = nlohmann::json::array(); exportedArtifacts = nlohmann::json::array();
			bindingsAwaitingReceipt = baselineAwaitingReceipt = fixtureIntentQueued = stepMutationReady = false; expectingRejectedTransaction = false;
			lightingBaselineObservation = lightingTransitionPending = false; sharedLightingBaselineDone = sharedLightingEditsDone = false; lightingBaselineCompleted = 0; lightingBaselineReceipt = nullptr;
			for(auto& slots : lightingObservedSlots) slots.clear();
			rejectedPriorPublication = {}; rejectedPriorSpatialGeneration = 0;
			faultPhase = 0;
			fixtureId = id; fixtureHash = hash; stage = requestedStage; seed = nextSeed; runIndex = nextRun;
			warmupFrames = nextWarmup; measuredFrames = nextMeasured; runId = nextRunId; ownerId = nextOwnerId;
			runMode = mode; resourceBindingAwaitingReceipt = resourceBaselineBinding = false;
			denseBaselineBinding = denseResourcePrepared = denseBaselineObserved = denseAwaitingObservation = false;
			denseBaselineMeshId = EMPTY_UUID; denseBaselineMeshPublication = 0;
			denseBaselineAttempts = 0;
			// The operation travels into strict native artifacts and their verifier.
			// Use the actual full UUID owner format shared by both boundaries.
			operationId = ToHexString(GenerateUUID());
			priorToken = {options.optionsSessionGeneration, options.options, true}; observedOptions = options;
			submittedFrames = completedFrames = lastSubmit = lastComplete = 0; active = true; state = "Queued";
			unavailableReason = "The selected renderer efficiency fixture driver and GPU evidence are not implemented yet.";
			if(!installedSteps.is_null())
			{
				state = "AwaitingAssets";
				unavailableReason = "Awaiting native named subasset publication, verified cooked geometry, and target binding transactions before fixture measurement.";
			}
			requestedRuns = runs; preparedIdentity.reset(); identityError.clear(); documentJob = {}; completedEvidencePath.clear(); exportedFrameCount = exportedArtifactCount = 0;
			adapterName.clear(); driverVersion.clear(); colorWidth = colorHeight = 0;
			if(stage=="P3") { colorWidth=fixtureCameras.front().nativeExtent.x; colorHeight=fixtureCameras.front().nativeExtent.y; }
			// Authored recipes are immutable input closure, separate from GPU outputs.
			if(stage=="P3") for(const auto& step : installedSteps.at("steps")) if(step.at("nativePayload").contains("depthRecipePath")) {
				const auto& payload=step.at("nativePayload"); const auto path=payload.at("depthRecipePath").get<std::string>(); const auto bytes=ReadSealedBytes(projectRoot,path);
				fixtureReceipts.push_back({{"kind","DepthInputRecipe"},{"fixtureStepId",step.at("id")},{"recipePath",path},{"recipeSha256",payload.at("depthRecipeSha256")},{"nativeExtent",step.at("nativeExtent")},{"recipe",payload.at("depthInputRecipe")}});
				exportedArtifacts.push_back({{"kind","Counters"},{"status","Valid"},{"path",path},{"sha256",InstalledStepsHash(bytes)},{"reason",""}}); ++exportedArtifactCount;
			}
			identityJob = {};
			if(!installedSteps.is_null())
			{
				const auto copiedRoot = projectRoot; const auto copiedSteps = installedSteps;
				auto task = ThreadPool::instance().createTask([copiedRoot, copiedSteps]() {
					PreparedEvidenceIdentity prepared; std::string error;
					try
					{
						if(!PrepareRendererEfficiencyInstallIdentity(prepared.install, error)) return std::make_pair(std::move(prepared), std::move(error));
						const auto camera = ReadOwnedBytes(copiedRoot, "renderer-efficiency-camera.json");
						const auto marker = nlohmann::json::parse(ReadOwnedBytes(copiedRoot, "renderer-efficiency-project.json"));
						prepared.cameraHash = InstalledStepsHash(camera);
						if(prepared.cameraHash != marker.at("cameraHash").get<std::string>()) throw std::runtime_error("Owned camera projection receipt changed.");
						std::map<std::string, std::string> cohort;
						for(const auto& asset : copiedSteps.at("assets"))
						{
							const auto source = std::filesystem::path("Assets") / asset.at("sourcePath").get<std::string>();
							cohort.emplace(source.generic_string(), InstalledStepsHash(ReadSealedBytes(copiedRoot, source)));
						}
						// This stable root identifies actual sealed initial authoring bytes.
						// Bootstrap independently proves their named cooked publications;
						// each frame retains the subsequently changed native revisions.
						prepared.geometryMaterialRevisionHash = InstalledStepsHash(nlohmann::json(cohort).dump());
					}
					catch(const std::exception& exception) { error = exception.what(); }
					return std::make_pair(std::move(prepared), std::move(error));
				});
				identityJob = task->getFuture(); ThreadPool::instance().enqueue(task);
			}
			error.clear(); return true;
		}
		catch(const std::exception& exception)
		{
			// Keep malformed recipes rejected, and identify the underlying field
			// or session failure so an owned acceptance run can repair its inputs.
			error = "Validation start requires valid fixtureId, fixtureHash and bounded session parameters: " + std::string(exception.what());
			return false;
		}
	}
	bool EditorRendererEfficiencyValidation::Stop(std::string_view id, std::string& error)
	{
		if(!HasOperation(id)) { error = "Renderer efficiency operation was not found."; return false; }
		active = false; state = "Stopped"; error.clear(); return true;
	}
	void EditorRendererEfficiencyValidation::CancelPendingFixtureWorkAtProtectedFrame(Scene::SceneMutationQueue& queue)
	{
		// Mark exact native requests canceled; the existing protected producers
		// retain terminal decisions. Detached workers may finish but cannot admit.
		if(!pendingTransactionId.empty()) (void)queue.CancelTransaction(pendingTransactionId);
		pendingScenePublication = {};
		for(const auto& command : pendingAnimationCommands)
			(void)Animation::AnimationSystem::Instance().CancelCommand(command.cancellationToken, command.correlationId);
		pendingAnimationCommands.clear();
	}
	bool EditorRendererEfficiencyValidation::AdmitPublishedFixtureBindings(const std::filesystem::path& root,
		std::uint64_t generation, std::string_view operation,
		const std::vector<RendererEfficiencyPublishedBinding>& bindings, ObjectManager& objects,
		Scene::SceneMutationQueue& queue, std::string& error)
	{
		if(!HasOperation(operation) || !ValidateSession(root, generation, error)) return false;
		const bool initial = state == "AwaitingAssets";
		const bool resourceReset = state == "AwaitingResourceBaseline";
		if(installedSteps.is_null() || (!initial && !resourceReset && state != "AwaitingNativeCapability"))
		{ error = "Fixture bootstrap is not awaiting installed asset publications."; return false; }
		try
		{
			std::map<std::string, UUID> resolved;
			if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && installedSteps.contains("lightingPoolSize"))
			{
				// P2 retains installed foundation geometry, not P1 glTF role imports.
				// Every live renderable must have actual nonloading cooked owners.
				bool ready = true; std::size_t geometry = 0;
				objects.forEachObjectWithComponents<Transform, MeshRenderComponent>([&](Object, Transform&, MeshRenderComponent& mesh) {
					++geometry;
					for(const auto& entry : {std::pair{mesh.meshId, AssetType::Mesh}, std::pair{mesh.materialId, AssetType::Material}})
					{
						const auto publication = ResourceManager::Instance().TryCopyRendererEfficiencyResourcePublication(entry.first, entry.second);
						ready &= publication && publication->present && publication->resourceGeneration && publication->assetId == entry.first;
						if(entry.second == AssetType::Mesh) ready &= publication && publication->cookedLods && publication->cookedMeshlets;
					}
				});
				if(!ready || !geometry || !bindings.empty()) { error = "Awaiting actual P2 foundation mesh/material publications."; return false; }
				Scene::Transaction values;
				if(!PrepareRendererEfficiencyLightingTransaction(installedSteps.at("steps").front().at("nativePayload").at("baselineTransactionRequests"), values, error)) return false;
				Scene::TransactionalMutationBuilder observed(operationId + "-lighting-bindings", Scene::TransactionOrigin::Automation, "RendererEfficiencyLightingBindings"); observed.ObserveScene(objects);
				std::set<UUID> unique;
				for(const auto& value : installedSteps.at("targets").at("lightingPool"))
				{
					const auto id = ParseUUID(value.get<std::string>()).value(); const auto object = objects.getObject(id);
					if(!unique.insert(id).second || !object || !objects.getObjectLifetimeGeneration(*object) || !objects.GetComponent<LightComponent>(*object)) { error = "P2 lighting pool lacks unique actual component lifetimes."; return false; }
					observed.ObserveObject(objects, id);
				}
				if(unique.size() != 1025) { error = "P2 pool must contain exactly 1025 observed lights."; return false; }
				auto transaction = std::move(observed).Build(); transaction.writeSet = std::move(values.writeSet);
				if(!QueueFixtureTransaction(root, generation, operation, std::move(transaction), queue, error)) return false;
				bindingsAwaitingReceipt = true; return true;
			}
			for(const auto& binding : bindings)
			{
				if(binding.assetId == EMPTY_UUID || binding.resourceGeneration == 0 || !resolved.emplace(binding.bindingKey, binding.assetId).second)
				{ error = "Fixture bindings require unique names and actual native resource publications."; return false; }
			}
			std::size_t requiredCount = 0;
			for(const auto& asset : installedSteps.at("assets"))
				for(const auto& required : asset.at("requiredSubAssets"))
				{
					++requiredCount;
					const auto key = required.at("bindingKey").get<std::string>();
					const auto binding = std::find_if(bindings.begin(), bindings.end(), [&key](const auto& row) { return row.bindingKey == key; });
					if(binding == bindings.end() || binding->kind != required.at("kind").get<std::string>()
						|| binding->packageGuid != ParseUUID(asset.at("packageGuid").get<std::string>()).value())
					{ error = "A required named package subasset has not been published."; return false; }
					if((binding->kind == "Mesh" || binding->kind == "DeformableMesh") && (binding->cookedLods < asset.at("requiredCook").at("minimumLods").get<std::uint32_t>()
						|| binding->cookedMeshlets < asset.at("requiredCook").at("minimumMeshlets").get<std::uint32_t>()))
					{ error = "Fixture geometry does not satisfy its actual cooked LOD and meshlet requirements."; return false; }
				}
			if(requiredCount != bindings.size()) { error = "Fixture binding publications include unexpected entries."; return false; }
			if(stage=="P3" && installedSteps.value("depthFixture",false)) {
				// All pool membership and foundation resource reads occur at this
				// protected admission, before detached transaction preparation.
				std::set<UUID> pool;
				for(const auto& value : installedSteps.at("targets").at("lightingPool")) {
					const auto id=ParseUUID(value.get<std::string>()); const auto object=id ? objects.getObject(*id) : std::nullopt;
					if(!id || *id==EMPTY_UUID || !pool.insert(*id).second || !object || !objects.GetComponent<LightComponent>(*object) || !objects.getObjectLifetimeGeneration(*object)) {error="Depth lighting pool lacks exact actual light lifetimes.";return false;}
				}
				const auto expected=installedSteps.at("steps").front().at("nativePayload").at("candidateOrder").size();
				if(pool.size()!=expected || (expected!=17 && expected!=1000)) {error="Depth lighting pool differs from its frozen count.";return false;}
				bool foundationReady=true; std::size_t foundationGeometry=0;
				objects.forEachObjectWithComponents<Transform,MeshRenderComponent>([&](Object,Transform&,MeshRenderComponent& mesh) {
					// Generated panels are bound by this transaction; every retained
					// foundation renderable must already own its real cooked payload.
					++foundationGeometry;
					for(const auto& entry : {std::pair{mesh.meshId,AssetType::Mesh},std::pair{mesh.materialId,AssetType::Material}}) {const auto publication=ResourceManager::Instance().TryCopyRendererEfficiencyResourcePublication(entry.first,entry.second);foundationReady &= publication && publication->present && publication->resourceGeneration;}
				});
				if(!foundationReady || !foundationGeometry) {error="Awaiting actual depth foundation mesh/material publications.";return false;}
			}
			Scene::TransactionalMutationBuilder builder(operationId + "-bindings-" + std::to_string(stepIndex) + (initial ? "-initial" : resourceReset ? "-baseline" : "-variant"), Scene::TransactionOrigin::Automation, "RendererEfficiencyBindings");
			builder.ObserveScene(objects);
			if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && installedSteps.value("sharedLighting", false))
			{
				// The pool is authored once but inactive until its real typed overflow
				// action. Do not treat its UUID array as an imported mesh role.
				std::set<UUID> pool;
				for(const auto& value : installedSteps.at("targets").at("sharedLightingPool"))
				{
					const auto id = ParseUUID(value.get<std::string>()); const auto object = id ? objects.getObject(*id) : std::nullopt;
					if(!id || *id == EMPTY_UUID || !pool.insert(*id).second || !object || !objects.GetComponent<LightComponent>(*object) || !objects.getObjectLifetimeGeneration(*object)) { error = "Shared lighting pool lacks its exact actual native target lifetimes."; return false; }
				}
			}
			// Named role observations cover imported panels and retained foundation;
			// array light pools are verified independently and never parsed as roles.
			for(const auto& [role, target] : fixtureRoleTargets.items())
			{
				if(role == "sharedLightingPool") continue;
				const auto id = ParseUUID(target.at("objectId").get<std::string>()).value();
				if(!objects.getObject(id)) { error = "A generated fixture target is absent from the active scene."; return false; }
				builder.ObserveObject(objects, id);
				// Keep the copied JSON alive while its items proxy supplies references.
				// Iterating items() on value() directly would destroy that temporary
				// before the loop and read dangling binding names and UUID keys.
				const auto assetBindings = target.value("assetBindings", nlohmann::json::object());
				for(const auto& [property, key] : assetBindings.items())
					builder.UpdateComponentProperty(id, target.at("componentName").get<std::string>(), property,
						Reflection::Variant(resolved.at(key.get<std::string>())));
			}
			auto transaction = std::move(builder).Build();
			// Capture authored baseline while protected. Future reset workers receive
			// only copied values; no worker reads ObjectManager or components.
			Scene::TransactionalMutationBuilder baseline("", Scene::TransactionOrigin::Automation, "RendererEfficiencyBaseline");
			std::set<UUID> baselineTargets;
			for(const auto& [role, target] : fixtureRoleTargets.items()) if(role != "sharedLightingPool") baselineTargets.insert(ParseUUID(target.at("objectId").get<std::string>()).value());
			if(stage=="P3" && installedSteps.value("depthFixture",false)) for(const auto& value : installedSteps.at("targets").at("lightingPool")) baselineTargets.insert(ParseUUID(value.get<std::string>()).value());
			for(const auto& step : installedSteps.at("steps"))
				for(const auto& view : step.at("requiredBundle").at("views")) if(view.contains("lightId")) baselineTargets.insert(ParseUUID(view.at("lightId").get<std::string>()).value());
			for(const auto& id : baselineTargets)
			{
				const auto object = objects.getObject(id);
				if(!object) { error = "An authored fixture baseline target is absent."; return false; }
				if(const auto* transform = objects.GetComponent<Transform>(*object)) baseline.SetWorldTransform(objects, id, transform->worldPose);
				baseline.SetActive(id, objects.isObjectActive(*object));
				if(const auto* light = objects.GetComponent<LightComponent>(*object))
					baseline.UpdateComponentProperty(id, "LightComponent", "castsShadows", Reflection::Variant(light->castsShadows));
			}
			auto nextBaseline = std::move(baseline).Build();
			nextBaseline.writeSet.operations.insert(nextBaseline.writeSet.operations.end(), transaction.writeSet.operations.begin(), transaction.writeSet.operations.end());
			if(!QueueFixtureTransaction(root, generation, operation, std::move(transaction), queue, error)) return false;
			publishedBindings = std::move(resolved);
			if(initial) { baselineInputs = std::move(nextBaseline); bindingsAwaitingReceipt = true; }
			else { resourceBindingAwaitingReceipt = true; resourceBaselineBinding = resourceReset; }
			return true;
		}
		catch(const std::exception& exception)
		{
			// Preserve the original rejection while exposing the exact malformed
			// field to the owned validation session, without admitting partial work.
			error = "Installed fixture binding publication is malformed or incomplete: " + std::string(exception.what());
			return false;
		}
	}
	void EditorRendererEfficiencyValidation::PumpFixtureReceiptsAtProtectedFrame(
		const std::filesystem::path& root, std::uint64_t generation, ObjectManager& objects, Scene::SceneMutationQueue& queue)
	{
		if(!active || pendingTransactionId.empty()) return;
		std::string error;
		if(!ValidateSession(root, generation, error))
		{
			// Cancellation uses the same queue decision path. Never consume editor
			// undo receipts or flush the scene from this automation boundary.
			(void)queue.CancelTransaction(pendingTransactionId);
			active = false; state = "Failed"; unavailableReason = "Owned project changed before fixture completion.";
			return;
		}
		if(!pendingScenePublication.committedCaptured)
		{
			const auto receipt = queue.TakeCompletedAutomationTransactionReceipt(pendingTransactionId);
			if(!receipt) return;
			if(expectingRejectedTransaction)
			{
				// Preserve the native conflict classification and require accounting
				// for both frozen operations before checking unchanged protected state.
				const auto expectedWrites = pendingScenePublication.transaction.writeSet.operations.size();
				pendingTransactionId.clear(); pendingScenePublication = {};
				if(!RendererEfficiencyAtomicConflictReceiptMatches(*receipt, expectedWrites))
				{
					active = false; state = "Failed";
					unavailableReason = "Owned rollback fixture lacks its complete atomic conflict decision: status="
						+ std::string(Scene::ToString(receipt->status)) + ", applied=" + std::to_string(receipt->appliedWriteCount)
						+ ", rejected=" + std::to_string(receipt->rejectedWriteCount) + ", expected=" + std::to_string(expectedWrites)
						+ ", nativeReason=" + receipt->message;
					return;
				}
				rejectedReceipt = {{"kind", "TransactionRollback"}, {"fixtureStepId", installedSteps.at("steps").at(stepIndex).at("id")},
					{"transactionId", receipt->transactionId}, {"status", std::string(Scene::ToString(receipt->status))}, {"appliedWriteCount", receipt->appliedWriteCount}, {"rejectedWriteCount", receipt->rejectedWriteCount},
					{"spatialPublicationGeneration", receipt->spatialPublicationGeneration}, {"message", receipt->message}};
				// Conflict publishes no new visibility. Keep the raw reserved receipt
				// generation unchanged and correlate to the captured renderer clock.
				mutationGeneration = rejectedPriorSpatialGeneration;
				state = "CheckingRejectedRollback"; return;
			}
			if(receipt->status != Scene::TransactionStatus::Committed || receipt->rejectedWriteCount != 0)
			{
				active = false; state = "Failed"; unavailableReason = "Fixture transaction rejected: " + receipt->message;
				return;
			}
			// The transaction result reserves a spatial-publication field but generic
			// commits do not publish the scheduler's scene packet. Never admit zero or
			// recycle an old mutation token while that actual boundary is still pending.
			if(!CaptureRendererEfficiencyCommittedScenePublication(objects, pendingScenePublication, *receipt, error))
			{ active = false; state = "Failed"; unavailableReason = error; return; }
			mutationGeneration = 0; state = "AwaitingPublication";
		}
		std::uint64_t publishedSpatialRevision = 0;
		if(!ResolveRendererEfficiencyScenePublication(objects, pendingScenePublication, publishedSpatialRevision, error))
		{ if(pendingScenePublication.invalid) { active = false; state = "Failed"; } else state = "AwaitingPublication"; unavailableReason = error; return; }
		mutationGeneration = publishedSpatialRevision;
		pendingTransactionId.clear(); pendingScenePublication = {};
		if(resourceBindingAwaitingReceipt)
		{
			resourceBindingAwaitingReceipt = false;
			if(denseBaselineBinding) { denseResourcePrepared = true; denseBaselineBinding = false; }
			if(resourceBaselineBinding) state = "PreparingBaseline";
			else { stepMutationReady = true; if(stage=="P5" && installedSteps.value("staticShadowFixture",false)){staticResourceApplied=true;state="PreparingStep";}else state="AwaitingFirstObservation"; }
			return;
		}
		if(bindingsAwaitingReceipt)
		{
			bindingsAwaitingReceipt = false; state = "PreparingBaseline"; unavailableReason.clear(); return;
		}
		if(baselineAwaitingReceipt)
		{
			if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && ((installedSteps.contains("lightingPoolSize") || installedSteps.value("depthFixture", false)) || preparedAction == "lighting-baseline"))
			{
				baselineAwaitingReceipt = false; lightingBaselineObservation = true; stepMutationReady = true;
				state = "AwaitingLightingBaseline"; unavailableReason.clear(); return;
			}
			baselineAwaitingReceipt = false; state = "ResettingAnimationBaseline"; unavailableReason.clear(); return;
		}
		stepMutationReady = true;
		if(stage=="P5" && preparedAction=="static-shadow-step")
		{
			const auto& current=installedSteps.at("steps").at(stepIndex);
			if(current.at("action")=="cutout-revision" && !staticResourceApplied) {state="AwaitingNativeCapability";unavailableReason="Awaiting actual same-source cutout publication.";return;}
			if(!current.at("nativePayload").value("animatePerSample",false)) {lightingTransitionPending=true;state="AwaitingNativeCapability";return;}
			state="SeekingBatchDeformation";unavailableReason.clear();return;
		}
		if((stage=="P4" || stage=="P5") && installedSteps.value("sharedLighting",false) && preparedAction=="lighting-step" && installedSteps.at("steps").at(stepIndex).at("action")=="unsupported-grouped-route") {state="SeekingBatchDeformation";unavailableReason.clear();return;}
		if(stage=="P4" && preparedAction=="shadow-batch-step") {
			if(installedSteps.at("steps").at(stepIndex).at("action")=="deformed") {state="SeekingBatchDeformation";unavailableReason.clear();return;}
			lightingTransitionPending=true;state="AwaitingNativeCapability";unavailableReason.clear();return;
		}
		if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && ((installedSteps.contains("lightingPoolSize") || installedSteps.value("depthFixture", false)) || preparedAction == "lighting-step")) { lightingTransitionPending = true; state = "AwaitingNativeCapability"; unavailableReason.clear(); return; }
		state = "AwaitingFirstObservation";
		unavailableReason = "Awaiting an actual completed GPU bundle correlated with the committed fixture step.";
	}
	void EditorRendererEfficiencyValidation::PumpFixtureAtProtectedFrame(const std::filesystem::path& root,
		std::uint64_t generation, ObjectManager& objects, Scene::SceneMutationQueue& queue, RenderSystem& renderer)
	{
		if(!active || installedSteps.is_null()) return;
		std::string error;
		const auto fail = [&](std::string reason) {
			active = false; state = "Failed"; unavailableReason = std::move(reason);
			if(!pendingTransactionId.empty()) (void)queue.CancelTransaction(pendingTransactionId);
			renderer.ClearRendererEfficiencyFixtureIntent(operationId);
		};
		if(!ValidateSession(root, generation, error)) { fail(error); return; }
		PumpFixtureReceiptsAtProtectedFrame(root, generation, objects, queue);
		if(!active || state == "AwaitingAssets" || !pendingTransactionId.empty()) return;
		try
		{
			if(identityJob.valid() && identityJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			{
				auto [identity, reason] = identityJob.get();
				if(reason.empty()) preparedIdentity = std::move(identity); else identityError = std::move(reason);
			}
			const auto recovery = renderer.GetLatestRendererRecoverySnapshot();
			adapterName = recovery.hardwareName; driverVersion = recovery.driverVersion;
			const auto& steps = installedSteps.at("steps");
			if(stepIndex < steps.size())
			{
				const auto controls = renderer.GetRendererEfficiencyFixtureControlSnapshot();
				if(controls.correlation.operationId == operationId && controls.correlation.fixtureStepId == steps.at(stepIndex).at("id").get<std::string>()
					&& !controls.controlsApplied && controls.controlReason.find("dedicated capture") != std::string::npos)
				{ fail(controls.controlReason); return; }
			}
			if(stepIndex >= steps.size())
			{
				if(!preparedIdentity) { if(!identityError.empty()) fail(identityError); else unavailableReason = "Awaiting verified installed closure identity."; return; }
				if(documentJob.valid())
				{
					if(documentJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
					auto [path, reason] = documentJob.get(); if(!reason.empty()) { fail(reason); return; }
					completedEvidencePath = path.filename().generic_string();
					active = false; state = "Completed"; unavailableReason.clear(); renderer.ClearRendererEfficiencyFixtureIntent(operationId); return;
				}
				// Tens of thousands of native face rows exceed a loopback response.
				// Move their owned arrays into one detached serializer and return only
				// a small project-relative path after fresh protected ownership checks.
				auto task = ThreadPool::instance().createTask([root = projectRoot, marker = markerIdentity, operationId = operationId,
					identity = *preparedIdentity, runId = runId, runIndex = runIndex, stage = stage, runMode = runMode, fixtureId = fixtureId,
					fixtureHash = fixtureHash, seed = seed, adapterName = adapterName, driverVersion = driverVersion, colorWidth = colorWidth, colorHeight = colorHeight,
					options = observedOptions.options, warmupFrames = warmupFrames, measuredFrames = measuredFrames, requestedRuns = requestedRuns,
					frames = std::move(exportedFrames), artifacts = std::move(exportedArtifacts), receipts = std::move(fixtureReceipts)]() mutable {
					std::filesystem::path temporary;
					try
					{
						(void)OwnedRegularFile(root, "renderer-efficiency-project.json");
						if(nlohmann::json::parse(ReadOwnedBytes(root, "renderer-efficiency-project.json")).dump() != marker) throw std::runtime_error("Owned report marker changed before document serialization.");
						// Actual low-frequency native actions accompany the raster artifacts,
						// so the independent comparer can inspect their real decisions.
						const nlohmann::json receiptsDocument = {{"schemaVersion", 1}, {"operationId", operationId}, {"fixtureId", fixtureId}, {"fixtureHash", fixtureHash},
							{"stage", stage}, {"runId", runId}, {"runIndex", runIndex}, {"mode", runMode}, {"seed", seed}, {"receipts", std::move(receipts)}};
						const auto receiptBytes = receiptsDocument.dump(), receiptHash = InstalledStepsHash(receiptBytes);
						const auto relativeReceipt = std::filesystem::path("renderer-efficiency-evidence") / operationId / (receiptHash + ".receipts.json");
						const auto receiptPath = root / relativeReceipt;
						std::filesystem::path checked;
						for(const auto& part : receiptPath.parent_path())
						{
							checked /= part; if(std::filesystem::is_symlink(checked)) throw std::runtime_error("Receipt folder redirects outside its owned project.");
#if defined(_WIN32)
							const auto attributes = GetFileAttributesW(checked.c_str()); if(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Receipt folder traverses a junction.");
#endif
						}
						std::filesystem::create_directories(receiptPath.parent_path());
						temporary = root / (".renderer-efficiency-" + ToHexString(GenerateUUID()) + ".tmp");
						{ std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output.write(receiptBytes.data(), static_cast<std::streamsize>(receiptBytes.size())); output.close(); if(!output) throw std::runtime_error("Native receipt artifact cannot be written."); }
#if defined(_WIN32)
						if(!MoveFileExW(temporary.c_str(), receiptPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Native receipt artifact cannot be published atomically.");
#else
						std::filesystem::rename(temporary, receiptPath);
#endif
						artifacts.push_back({{"kind", "Counters"}, {"status", "Valid"}, {"path", relativeReceipt.generic_string()}, {"sha256", receiptHash}, {"reason", ""}});
						const auto valid = [](const auto& value) { return nlohmann::json{{"status", "Valid"}, {"value", value}, {"reason", ""}}; };
						const auto unavailable = [](std::string reason) { return nlohmann::json{{"status", "Unavailable"}, {"value", nullptr}, {"reason", std::move(reason)}}; };
						nlohmann::json criteria = nlohmann::json::object();
						// These fixed labels are version-one evidence protocol keys.
						// Keep their serialized identity stable for preserved receipts.
						static constexpr std::array<std::string_view, 9> criterionIds{
							"SC-001", "SC-002", "SC-003", "SC-004", "SC-005",
							"SC-006", "SC-007", "SC-008", "SC-009"};
						for(const auto criterionId : criterionIds)
						{
							nlohmann::json comparison = {{"status", "Unverified"}, {"reason", "Requires matched reference/candidate raw artifact comparison."},
								{"validMeasuredFramesPerRun", nlohmann::json::array()}, {"runCount", 0}, {"excludedWarmupSamples", 0}, {"excludedPendingSamples", 0}, {"excludedInvalidSamples", 0}, {"metricScope", "Unavailable"}};
							for(const auto* metric : {"coverageMismatchCount", "casterIdentityMismatchCount", "maxDepthAbsoluteError", "maxImageNormalizedChannelError", "referenceMedianNanoseconds", "candidateMedianNanoseconds", "referenceP95Nanoseconds", "candidateP95Nanoseconds"}) comparison[metric] = unavailable("Comparison is performed by the independent paired-run comparer.");
							criteria[std::string(criterionId)] = std::move(comparison);
						}
						nlohmann::json document = {{"schemaVersion", 1}, {"identity", {{"runId", runId}, {"runIndex", runIndex}, {"stage", stage}, {"mode", runMode}, {"fixtureId", fixtureId},
							{"fixtureHash", valid(fixtureHash)}, {"seed", seed}, {"cameraHash", valid(identity.cameraHash)}, {"geometryMaterialRevisionHash", valid(identity.geometryMaterialRevisionHash)},
							{"referenceRevision", valid(identity.install.revision)}, {"candidateRevision", valid(identity.install.revision)}, {"installedBinaryHash", valid(identity.install.installedBinaryHash)},
							{"installedShaderClosureHash", valid(identity.install.installedShaderClosureHash)}, {"installedAssetClosureHash", valid(identity.install.installedAssetClosureHash)},
							{"postP1BaselineClosureHash", unavailable("Post-P1 baseline archive is not yet captured.")},
							{"adapter", adapterName.empty() ? unavailable("Native selected physical device name is absent.") : valid(adapterName)},
							{"driver", driverVersion.empty() ? unavailable("Native selected driver version is absent.") : valid(driverVersion)},
							{"resolution", nlohmann::json{{"width", colorWidth}, {"height", colorHeight}}}}},
							{"requestedOptions", SerializeRendererEfficiencyOptions(options)},
							{"measurementPolicy", {{"warmupFrames", warmupFrames}, {"measuredFrames", measuredFrames}, {"runs", requestedRuns}, {"depthAbsoluteTolerance", 0.000001},
								{"imageNormalizedChannelTolerance", 2.0 / 255.0}, {"coverageMismatchTolerance", 0}, {"casterIdentityMismatchTolerance", 0}, {"allowExcludedImageRegions", false}}},
							{"frames", std::move(frames)}, {"successCriteria", std::move(criteria)}, {"artifacts", std::move(artifacts)}};
						const auto destination = root / (operationId + "-evidence.json");
						temporary = root / (".renderer-efficiency-" + ToHexString(GenerateUUID()) + ".tmp");
						{ std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output << document; output.close(); if(!output) throw std::runtime_error("Completed evidence document cannot be written."); }
#if defined(_WIN32)
						if(!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Completed evidence document cannot be published atomically.");
#else
						std::filesystem::rename(temporary, destination);
#endif
						return std::make_pair(destination, std::string{});
					}
					catch(const std::exception& exception)
					{
						if(!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
						return std::make_pair(std::filesystem::path{}, std::string(exception.what()));
					}
				});
				documentJob = task->getFuture(); state = "WritingEvidenceDocument";
				renderer.ClearRendererEfficiencyFixtureIntent(operationId); ThreadPool::instance().enqueue(task); return;
			}
			const auto& step = steps.at(stepIndex);
			if(state == "PreparingBaseline" || state == "PreparingLightingBaseline")
			{
				if(stage=="P5" && step.at("nativePayload").contains("staticShadowCacheBudgetBytes"))
				{
					const auto current=renderer.GetRendererEfficiencyOptionsSnapshot();
					const auto budget=runMode=="Reference"?std::uint64_t{0}:step.at("nativePayload").at("staticShadowCacheBudgetBytes").get<std::uint64_t>();
					if(!staticBudgetRequestedGeneration) {auto desired=current.options;desired.staticShadowCacheBudgetBytes=budget;const auto receipt=renderer.QueueRendererEfficiencyOptions(desired);if(!receipt.accepted){fail(receipt.error);return;}staticBudgetRequestedGeneration=receipt.requestedGeneration;pendingStaticBudgetReceipt={{"kind","StaticBudgetPublication"},{"fixtureStepId",step.at("id")},{"operationId",operationId},{"requestedGeneration",receipt.requestedGeneration},{"priorOptions",SerializeRendererEfficiencyOptions(current.options)},{"requestedOptions",SerializeRendererEfficiencyOptions(desired)}};return;}
					if(current.settingsGeneration<staticBudgetRequestedGeneration || current.options.staticShadowCacheBudgetBytes!=budget) {unavailableReason="Awaiting actual protected static budget option publication.";return;}
					if(!pendingStaticBudgetReceipt.is_null()){pendingStaticBudgetReceipt["settingsGeneration"]=current.settingsGeneration;pendingStaticBudgetReceipt["rendererSessionGeneration"]=current.rendererSessionGeneration;pendingStaticBudgetReceipt["optionsSessionGeneration"]=current.optionsSessionGeneration;pendingStaticBudgetReceipt["effectiveOptions"]=SerializeRendererEfficiencyOptions(current.options);if(!RecordFixtureReceipt(root,generation,operationId,std::move(pendingStaticBudgetReceipt),error)){fail(error);return;}pendingStaticBudgetReceipt=nullptr;}
					observedOptions=current;animationReadyStepFrame.reset();
				}
				if(!PendingDenseResourceBaseline().is_null()) { unavailableReason = "Awaiting actual dense page baseline import and native owner publication."; return; }
				if(stage=="P3" || (stage=="P4" && installedSteps.value("shadowBatchFixture",false))) fixtureCameras=depthStepCameras.at(stepIndex);
				renderer.ClearRendererEfficiencyFixtureIntent(operationId); activeFixtureIntent = {}; activeFixtureIntent.cameras = fixtureCameras; if(stage=="P3") activeFixtureIntent.depthControls=depthStepControls.at(stepIndex);
				Scene::TransactionalMutationBuilder captured(operationId + "-baseline-" + std::to_string(stepIndex), Scene::TransactionOrigin::Automation, "RendererEfficiencyBaseline");
				captured.ObserveScene(objects);
				if(stage=="P5" && installedSteps.value("staticShadowFixture",false))
				{
					// Baseline values are copied only after protected live observations.
					// Workers decode writes; the ordinary scheduler owns their visibility.
					const auto requests=step.at("nativePayload").at("baselineTransactionRequests");
					std::set<UUID> ids;for(const auto& request:requests)ids.insert(ParseUUID(request.at("objectId").get<std::string>()).value());for(const auto& id:ids)captured.ObserveObject(objects,id);
					auto transaction=std::move(captured).Build();const auto bindings=publishedBindings;
					auto task=ThreadPool::instance().createTask([transaction=std::move(transaction),requests,bindings]() mutable {Scene::Transaction values;std::string reason;if(!PrepareRendererEfficiencyStaticShadowTransaction(requests,bindings,values,reason))throw std::runtime_error(reason);transaction.writeSet=std::move(values.writeSet);return std::move(transaction);});
					preparedTransaction=task->getFuture();preparedAction="baseline";state="PreparingWorker";ThreadPool::instance().enqueue(task);return;
				}
				if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && ((installedSteps.contains("lightingPoolSize") || installedSteps.value("depthFixture", false)) || state == "PreparingLightingBaseline"))
				{
					const auto requests = step.at("nativePayload").at("baselineTransactionRequests");
					Renderer::RendererEfficiencyFixtureLightingControls controls;
					if(!ParseRendererEfficiencyLightingControls(step.at("nativePayload"), true, controls, error)) { fail(error); return; }
					activeFixtureIntent.lightingControls = std::move(controls);
					lightingBaselineCompleted = 0; lightingBaselineObservation = false; lightingTransitionPending = false;
					lightingBaselineReceipt = {{"kind", "LightingBaseline"}, {"fixtureStepId", step.at("id")}, {"frames", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}};
					for(auto& slots : lightingObservedSlots) slots.clear();
					std::set<UUID> targets; for(const auto& request : requests) targets.insert(ParseUUID(request.at("objectId").get<std::string>()).value());
					for(const auto& id : targets) captured.ObserveObject(objects, id);
					auto transaction = std::move(captured).Build();
					// JSON automation recipes are decoded off the frame hot path. The
					// worker receives immutable bytes and protected copied observations.
					const bool staticRequests=stage=="P5";
						auto task = ThreadPool::instance().createTask([transaction = std::move(transaction), requests, staticRequests]() mutable {
						Scene::Transaction values; std::string error;
						if(!(requests.empty() || (staticRequests ? PrepareRendererEfficiencyStaticShadowTransaction(requests, {}, values, error) : PrepareRendererEfficiencyLightingTransaction(requests, values, error)))) throw std::runtime_error(error);
						transaction.writeSet = std::move(values.writeSet); return std::move(transaction);
					});
					preparedTransaction = task->getFuture(); preparedAction = installedSteps.value("sharedLighting", false) ? "lighting-baseline" : "baseline"; state = "PreparingWorker";
					ThreadPool::instance().enqueue(task); return;
				}
				std::set<UUID> targets;
				for(const auto& operation : baselineInputs.writeSet.operations) targets.insert(operation.target.objectId);
				for(const auto& id : targets) captured.ObserveObject(objects, id);
				auto detached = std::move(captured).Build();
				const auto writes = baselineInputs.writeSet;
				auto task = ThreadPool::instance().createTask([detached = std::move(detached), writes]() mutable {
					// Preparation handles detached value arrays only. Fresh protected
					// observations prevent baseline replay from becoming stale/wildcard.
					detached.writeSet = writes; return detached;
				});
				preparedTransaction = task->getFuture(); ThreadPool::instance().enqueue(task, writes.operations.size());
				preparedAction = "baseline"; state = "PreparingWorker"; return;
			}
			if(state == "PreparingWorker")
			{
				if(!preparedTransaction.valid() || preparedTransaction.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
				auto transaction = preparedTransaction.get();
				if(!QueueFixtureTransaction(root, generation, operationId, std::move(transaction), queue, error)) { fail(error); return; }
				baselineAwaitingReceipt = preparedAction == "baseline" || preparedAction == "lighting-baseline"; return;
			}
			if(state == "ResettingAnimationBaseline" || state == "SeekingBatchDeformation" || state == "SeekingStaticSample" || (state == "PreparingStep" && (step.at("action") == "pose" || step.at("action") == "morph")))
			{
				animationBaseline = state == "ResettingAnimationBaseline"; animationPerSample = state == "SeekingStaticSample";
				std::vector<Animation::AnimationCommand> commands;
				pendingAnimationObjects.clear(); animationBefore.clear();
				animationGpuBefore = Animation::AnimationPresentationSystem::Instance().LoadPublishedAnimationPresentationFrame();
				for(const auto& [role, target] : fixtureRoleTargets.items())
				{
					if(role == "sharedLightingPool" || target.at("componentName") != "AnimatorComponent") continue;
					if(!animationBaseline && stage=="P5" && installedSteps.value("staticShadowFixture",false) && target.at("objectId") != step.at("nativePayload").at("animationTargetObjectId")) continue;
					if(!animationBaseline && stage!="P5" && !(stage=="P4" && (installedSteps.value("shadowBatchFixture",false) || step.at("action")=="unsupported-grouped-route")) && target.at("objectId") != step.at("target").at("objectId")) continue;
					const auto id = ParseUUID(target.at("objectId").get<std::string>()).value();
					const auto handle = Animation::AnimationSystem::Instance().ResolveAnimatorForObject(id);
					if(!handle.IsValid()) { unavailableReason = "Awaiting published native animator instance for " + role; return; }
					const auto playback = Animation::AnimationSystem::Instance().LoadAnimatorPlaybackState(handle);
					if(playback.latestStatus == Animation::AnimationStatus::NotReady) { unavailableReason = "Awaiting configured native animator playback for " + role; return; }
					animationBefore[id] = Animation::AnimationSystem::Instance().LoadAnimatorSnapshot(handle);
					for(const auto kind : RendererEfficiencySeekCommandKinds(playback.runState))
					{
						Animation::AnimationCommand command; command.target = handle; command.kind = kind;
						command.playbackTick = animationBaseline ? 0 : animationPerSample ? RendererEfficiencyStaticSampleTick(stepFrame) : step.at("nativePayload").at("seekTick").get<std::int64_t>();
						command.producerId = 0x6503; command.producerSequence = nextAnimationSequence++;
						command.correlationId = command.producerSequence; command.idempotencyKey = command.producerSequence;
						command.cancellationToken = command.producerSequence;
						pendingAnimationObjects[command.producerSequence] = id;
						commands.push_back(command);
					}
				}
				if(commands.empty()) { if(animationBaseline && installedSteps.value("depthFixture",false)) {state="PreparingStep";unavailableReason.clear();return;} fail("Native seek target is not an installed animator role."); return; }
				// Handle liveness must be checked at this protected admission. The
				// native atomic queue, rather than reflected playback writes, owns seek.
				const auto receipt = Animation::AnimationSystem::Instance().SubmitCommandBatch(commands);
				if(receipt.status != Animation::AnimationStatus::Queued)
				{
					const auto reason = "Native animation batch status=" + std::string(Animation::ToString(receipt.status));
					if(receipt.status == Animation::AnimationStatus::NotReady) { unavailableReason = reason; return; }
					fail(reason); return;
				}
				pendingAnimationCommands = std::move(commands); state = "AwaitingAnimation"; return;
			}
			if(state == "AwaitingAnimation")
			{
				std::uint64_t publication = 0;
				nlohmann::json animationReceipts = nlohmann::json::array();
				for(const auto& command : pendingAnimationCommands)
				{
					const auto receipt = Animation::AnimationSystem::Instance().ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
					const auto waitIdentity = " object=" + ToHexString(pendingAnimationObjects.at(command.producerSequence))
						+ " command=" + (command.kind == Animation::AnimationCommandKind::Pause ? std::string("Pause") : std::string("Seek"))
						+ " sequence=" + std::to_string(command.producerSequence);
					if(receipt.status == Animation::AnimationStatus::Queued || receipt.status == Animation::AnimationStatus::NotReady)
					{ unavailableReason = "Awaiting native animation command receipt status=" + std::string(Animation::ToString(receipt.status)) + waitIdentity; return; }
					if(receipt.status != Animation::AnimationStatus::Ok)
					{
						const auto kind = command.kind == Animation::AnimationCommandKind::Pause ? "Pause" : "Seek";
						fail("Native animation command status=" + std::string(Animation::ToString(receipt.status)) + " object=" + ToHexString(pendingAnimationObjects.at(command.producerSequence))
							+ " command=" + kind + " producer=" + std::to_string(command.producerId) + " sequence=" + std::to_string(command.producerSequence)
							+ " handle=" + std::to_string(command.target.slotIndex) + ":" + std::to_string(command.target.generation)
							+ " publication=" + std::to_string(receipt.publicationGeneration) + " observableTick=" + std::to_string(receipt.observableTick)); return;
					}
					if(receipt.publicationGeneration == 0)
					{ unavailableReason = "Awaiting nonzero native command authority publication." + waitIdentity; return; }
					if(command.kind == Animation::AnimationCommandKind::Seek
						&& Animation::AnimationSystem::Instance().LoadAnimatorPlaybackState(command.target).absoluteTick != command.playbackTick)
					{ unavailableReason = "Awaiting native seek playback tick=" + std::to_string(command.playbackTick) + waitIdentity; return; }
					if(command.kind == Animation::AnimationCommandKind::Seek)
					{
						const auto id = pendingAnimationObjects.at(command.producerSequence); const auto prior = animationBefore.at(id);
						// One retained post-physics envelope binds GPU input and authority.
						// CPU snapshots keep their existing pre-physics validation path.
						const auto presentation = Animation::AnimationPresentationSystem::Instance().LoadPublishedAnimationPresentationFrame();
						const bool gpuOnly = presentation && presentation->FindAuthorityRowForOwner(id)
							&& !presentation->FindAuthorityRowForOwner(id)->snapshot;
						if(stage=="P5" && gpuOnly) {fail("P5 deterministic progression requires actual CPU pose authority; GPU-only output cannot substitute.");return;}
						const auto authority = gpuOnly ? presentation->authority : Animation::AnimationSystem::Instance().LoadPublishedAuthorityFrame();
						const auto* row = authority ? authority->Find(Animation::AnimationSlotGenerationKey{
							command.target.slotIndex, command.target.generation, authority->sessionGeneration}) : nullptr;
						const auto playback = Animation::AnimationSystem::Instance().LoadAnimatorPlaybackState(command.target);
						if(!row || Animation::AnimationSystem::Instance().ResolveAnimatorForObject(id) != command.target
							|| (gpuOnly ? !animationGpuBefore || !RendererEfficiencyGpuAnimationAuthorityMatches(*presentation, *animationGpuBefore,
								receipt, id, animationBaseline, command.playbackTick)
								: !RendererEfficiencyAnimationAuthorityMatches(*row, *authority, receipt, id, animationBaseline, playback.absoluteTick, prior)))
						{
							unavailableReason = "Awaiting matching native authority row/snapshot receiptGeneration=" + std::to_string(receipt.publicationGeneration)
								+ " rowAuthorityGeneration=" + std::to_string(row ? row->authorityPublicationGeneration : 0)
								+ " frameAuthorityGeneration=" + std::to_string(authority ? authority->authorityGeneration : 0)
								+ " frameSession=" + std::to_string(authority ? authority->sessionGeneration : 0)
								+ " rowStatus=" + (row ? std::string(Animation::ToString(row->status)) : std::string("Missing"))
								+ " safeFallback=" + (row && row->safeFallback ? std::string("true") : std::string("false")) + waitIdentity; return;
						}
						const auto snapshot = row->snapshot;
						bool cpuContentChanged = false;
						if(!gpuOnly && !animationBaseline && prior && prior->pose && snapshot->pose)
						{
							if(snapshot->publicationGeneration <= prior->publicationGeneration)
							{ unavailableReason = "Awaiting newer native pose publication after seek priorPoseGeneration=" + std::to_string(prior->publicationGeneration)
								+ " currentPoseGeneration=" + std::to_string(snapshot->publicationGeneration) + waitIdentity; return; }
							const bool morphMutation=step.at("action")=="morph" || (stage=="P5" && fixtureRoleTargets.at("morph").at("objectId")==ToHexString(id)) || (stage=="P4" && (installedSteps.value("shadowBatchFixture",false) || step.at("action")=="unsupported-grouped-route") && fixtureRoleTargets.at("morph").at("objectId")==ToHexString(id));
							const auto oldValues = morphMutation ? prior->pose->MorphValues().size() : prior->pose->LocalTransforms().size();
							if(!oldValues) { fail("Native animation seek lacks retained baseline pose/morph values for " + ToHexString(id)); return; }
							const bool unchanged = morphMutation
								? std::ranges::equal(prior->pose->MorphValues(), snapshot->pose->MorphValues())
								: std::ranges::equal(prior->pose->LocalTransforms(), snapshot->pose->LocalTransforms());
							if(unchanged) { unavailableReason = "Awaiting changed native published pose/morph content after the accepted seek."; return; }
							cpuContentChanged = true;
						}
						// Command acceptance and the immutable animation output are separate
						// receipts. GPU-only output still needs the renderer's completed
						// artifact proof; a missing CPU pose never counts as changed content.
						animationReceipts.push_back({{"kind", "AnimationPublication"}, {"fixtureStepId", step.at("id")}, {"objectId", ToHexString(id)},
							{"command", "Seek"}, {"status", std::string(Animation::ToString(receipt.status))}, {"producerId", receipt.producerId}, {"producerSequence", receipt.producerSequence},
							{"correlationId", receipt.correlationId}, {"handleSlot", receipt.target.slotIndex}, {"handleGeneration", receipt.target.generation},
							{"requestedPlaybackTick", command.playbackTick}, {"observableTick", receipt.observableTick}, {"commandPublicationGeneration", receipt.publicationGeneration},
							{"authorityPublicationGeneration", row->authorityPublicationGeneration}, {"authoritySessionGeneration", row->key.sessionGeneration},
							{"frameAuthorityGeneration", authority->authorityGeneration}, {"authorityLifecycleGeneration", authority->lifecycleGeneration},
							{"retainedBaselineNoop", !gpuOnly && animationBaseline && row->authorityPublicationGeneration < receipt.publicationGeneration},
							{"normalizedTime", snapshot ? snapshot->playback.normalizedTime : playback.normalizedTime}, {"loopCount", snapshot ? snapshot->playback.loopCount : playback.loopCount},
							{"snapshotPublicationGeneration", snapshot ? snapshot->publicationGeneration : 0}, {"previousSnapshotPublicationGeneration", prior ? prior->publicationGeneration : 0},
							{"cpuPosePresent", snapshot && static_cast<bool>(snapshot->pose)}, {"cpuPoseGeneration", snapshot && snapshot->pose ? snapshot->pose->Generation().poseGeneration : 0}, {"cpuContentChanged", cpuContentChanged}, {"baseline", animationBaseline}});
						if(stage=="P5") {auto& saved=animationReceipts.back();saved["perSample"]=animationPerSample;saved["stepFrame"]=stepFrame;saved["sampleTick"]=command.playbackTick;saved["measurementPhase"]=animationBaseline?"Baseline":animationPerSample?(stepFrame<warmupFrames?"Warmup":"Measured"):"Transition";}
						if(gpuOnly)
						{
							Animation::GpuAnimationWorkPacket input; Animation::GpuAnimationGenerationRecord inputGeneration; Animation::GpuAnimationRenderIdentity inputIdentity;
							CopyGpuAnimationInput(*presentation, id, input, inputGeneration, inputIdentity);
							Animation::GpuAnimationWorkPacket oldInput; Animation::GpuAnimationGenerationRecord oldInputGeneration; Animation::GpuAnimationRenderIdentity oldInputIdentity;
							CopyGpuAnimationInput(*animationGpuBefore, id, oldInput, oldInputGeneration, oldInputIdentity);
							auto& recorded = animationReceipts.back();
							recorded["gpuInputPresent"] = true; recorded["gpuSampleTick"] = input.sampleTick;
							recorded["gpuWorkGeneration"] = inputGeneration.workGeneration; recorded["gpuPresentationGeneration"] = presentation->presentationGeneration;
							recorded["gpuCandidateAuthorityGeneration"] = authority->authorityGeneration;
							recorded["gpuMeshId"] = ToHexString(inputIdentity.meshId); recorded["gpuDeformablePayloadGeneration"] = inputIdentity.deformablePayloadGeneration;
							recorded["previousGpuSampleTick"] = oldInput.sampleTick; recorded["previousGpuWorkGeneration"] = oldInputGeneration.workGeneration;
							recorded["gpuSourceBindings"] = {{"stableProgramId", input.stableProgramId}, {"programResourceIndex", input.programResourceIndex},
								{"clipResourceIndex", input.clipResourceIndex}, {"skeletonResourceIndex", input.skeletonResourceIndex}, {"maskResourceIndex", input.maskResourceIndex}, {"morphResourceIndex", input.morphResourceIndex},
								{"programGeneration", inputGeneration.programGeneration}, {"controllerGeneration", inputGeneration.controllerGeneration}, {"clipGeneration", inputGeneration.clipGeneration},
								{"skeletonGeneration", inputGeneration.skeletonGeneration}, {"meshGeneration", inputGeneration.meshGeneration}, {"resourceGeneration", inputGeneration.resourceGeneration},
								{"rendererGeneration", inputIdentity.rendererGeneration}, {"sceneGeneration", inputIdentity.sceneGeneration}, {"meshHeaderVersion", inputIdentity.meshHeaderVersion},
								{"skeletonCompatibilityFingerprint", inputIdentity.skeletonCompatibilityFingerprint}, {"stageMask", input.stageMask}, {"flags", input.flags}};
							// Completion and changed GPU bytes belong to renderer artifacts,
							// not this input receipt. Zero CPU pose fields stay truthful.
						}
					}
					publication = (std::max)(publication, receipt.publicationGeneration);
				}
				for(auto& receipt : animationReceipts) if(!RecordFixtureReceipt(root, generation, operationId, std::move(receipt), error)) { fail(error); return; }
				pendingAnimationCommands.clear(); pendingAnimationObjects.clear(); animationBefore.clear(); animationGpuBefore.reset(); mutationGeneration = publication;
				state = animationBaseline ? ((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && installedSteps.value("sharedLighting", false) ? "PreparingLightingBaseline" : "PreparingStep") : "AwaitingFirstObservation";
				if(animationPerSample) {animationReadyStepFrame=stepFrame;state="AwaitingFirstObservation";}
				if(!animationBaseline && !animationPerSample && stage=="P5") {lightingTransitionPending=true;state="AwaitingNativeCapability";}
				if(!animationBaseline && stage=="P4" && (installedSteps.value("shadowBatchFixture",false) || step.at("action")=="unsupported-grouped-route")) {lightingTransitionPending=true;state="AwaitingNativeCapability";}
				unavailableReason = animationBaseline ? "" : "Awaiting actual completed native GPU observation after animation publication.";
				stepMutationReady = !animationBaseline; return;
			}
			if(state == "CheckingRejectedRollback")
			{
				const auto object = objects.getObject(rejectedTargetObservation.objectId);
				const auto* transform = object ? objects.GetComponent<Transform>(*object) : nullptr;
				if(!object || !transform || objects.getObjectLifetimeGeneration(*object) != rejectedTargetObservation.lifetimeGeneration
					|| objects.getObjectMetadataGeneration(*object) != rejectedTargetObservation.metadataGeneration
					|| objects.getObjectComponentGeneration(*object) != rejectedTargetObservation.componentGeneration
					|| objects.isObjectStatic(*object) != rejectedTargetStatic
					|| objects.isObjectActive(*object) != rejectedTargetActive || transform->worldPose.position != rejectedTargetPose.position
					|| transform->worldPose.rotation != rejectedTargetPose.rotation || transform->worldPose.scale != rejectedTargetPose.scale)
				{ fail("Rejected fixture transaction changed the actual target lifetime, components, metadata, activity or canonical pose."); return; }
				if(!mutationGeneration || !rejectedPriorPublication.packet
					|| rejectedPriorPublication.sceneGeneration != objects.GetSceneGeneration()
					|| !rejectedPriorPublication.packet->Validate(objects.GetSceneGeneration()).valid
					|| !rejectedPriorPublication.packet->PublicationRevision()
					|| rejectedPriorPublication.targetId != rejectedTargetObservation.objectId
					|| rejectedPriorPublication.targetLifetime != rejectedTargetObservation.lifetimeGeneration
					|| rejectedPriorPublication.targetSpatialRevision != rejectedTargetSpatialRevision
					|| mutationGeneration != rejectedPriorPublication.rendererSpatialRevision)
				{ fail("Rejected fixture transaction has no retained same-scene protected renderer revision proof."); return; }
				Scene::TransactionalMutationBuilder after("", Scene::TransactionOrigin::Automation, "RendererEfficiencyRollbackProof");
				after.ObserveObject(objects, rejectedTargetObservation.objectId); const auto observed = std::move(after).Build().readSet.objects.front();
				if(observed.hierarchyRevision != rejectedTargetObservation.hierarchyRevision || observed.transformRevision != rejectedTargetObservation.transformRevision
					|| observed.coordinateFrameRevision != rejectedTargetObservation.coordinateFrameRevision || observed.parentObjectId != rejectedTargetObservation.parentObjectId
					|| observed.parentLifetimeGeneration != rejectedTargetObservation.parentLifetimeGeneration || observed.parentTransformRevision != rejectedTargetObservation.parentTransformRevision)
				{ fail("Rejected fixture transaction changed the actual target hierarchy or transform revision."); return; }
				const auto targetSpatialRevision = objects.GetObjectSpatialRevision(*object);
				if(!rejectedTargetSpatialRevision || targetSpatialRevision != rejectedTargetSpatialRevision)
				{ fail("Rejected fixture transaction changed the actual target spatial revision."); return; }
				const auto proof = [](const Scene::TransactionObjectObservation& observation, const Scene::WorldPose& pose, bool active, std::uint64_t spatialRevision) {
					return nlohmann::json{{"objectId", ToHexString(observation.objectId)}, {"lifetimeGeneration", observation.lifetimeGeneration}, {"metadataGeneration", observation.metadataGeneration},
						{"componentGeneration", observation.componentGeneration}, {"hierarchyRevision", observation.hierarchyRevision}, {"transformRevision", observation.transformRevision}, {"spatialRevision", spatialRevision},
						{"coordinateFrameRevision", observation.coordinateFrameRevision}, {"parentObjectId", ToHexString(observation.parentObjectId)}, {"active", active},
						{"canonicalWorldPose", {{"position", {{"cell", {std::to_string(pose.position.cell.x), std::to_string(pose.position.cell.y), std::to_string(pose.position.cell.z)}},
							{"offsetMetres", {pose.position.offsetMetres.x, pose.position.offsetMetres.y, pose.position.offsetMetres.z}}}},
							{"rotation", {pose.rotation.w, pose.rotation.x, pose.rotation.y, pose.rotation.z}}, {"scale", {pose.scale.x, pose.scale.y, pose.scale.z}}}}};
				};
				rejectedReceipt["before"] = proof(rejectedTargetObservation, rejectedTargetPose, rejectedTargetActive, rejectedTargetSpatialRevision);
				rejectedReceipt["after"] = proof(observed, transform->worldPose, objects.isObjectActive(*object), targetSpatialRevision);
				rejectedReceipt["before"]["isStatic"] = rejectedTargetStatic; rejectedReceipt["after"]["isStatic"] = objects.isObjectStatic(*object);
				if(!RecordFixtureReceipt(root, generation, operationId, std::move(rejectedReceipt), error)) { fail(error); return; }
				expectingRejectedTransaction = false; stepMutationReady = true; state = "AwaitingFirstObservation";
			}
			if(state == "PreparingStep" && step.at("action") == "page-revision" && denseResourcePrepared && !denseBaselineObserved)
			{
				// This actual baseline uses the normal frame-zero correlation, but
				// consumes no export or measurement counts before the page edit.
				denseAwaitingObservation = true; stepMutationReady = true; state = "AwaitingDenseBaselineObservation";
			}
			if(state == "PreparingStep" || state == "PreparingLightingChange")
			{
				const auto action = step.at("action").get<std::string>();
				if(stage=="P5" && installedSteps.value("staticShadowFixture",false))
				{
					if(!ParseRendererEfficiencyStaticShadowControls(step,activeFixtureIntent,error)){fail(error);return;}
					// Preserve only required sealed cameras; no panel visibility or authored
					// camera writes are used to manufacture an extra output owner.
					activeFixtureIntent.cameras.clear();for(const auto& camera:fixtureCameras)if(std::any_of(activeFixtureIntent.viewOrder.begin(),activeFixtureIntent.viewOrder.end(),[&](const auto& view){return view.viewType==camera.viewType && view.outputId==camera.outputId;}))activeFixtureIntent.cameras.push_back(camera);
					const auto requests=step.at("nativePayload").at("transactionRequests");
					if(requests.empty()){state="SeekingBatchDeformation";return;}
					Scene::TransactionalMutationBuilder observed(operationId+"-static-step-"+std::to_string(stepIndex),Scene::TransactionOrigin::Automation,"RendererEfficiencyStaticShadow");observed.ObserveScene(objects);
					std::set<UUID> ids;for(const auto& request:requests)ids.insert(ParseUUID(request.at("objectId").get<std::string>()).value());for(const auto& id:ids)observed.ObserveObject(objects,id);
					auto transaction=std::move(observed).Build();const auto bindings=publishedBindings;
					auto task=ThreadPool::instance().createTask([transaction=std::move(transaction),requests,bindings]() mutable {Scene::Transaction values;std::string reason;if(!PrepareRendererEfficiencyStaticShadowTransaction(requests,bindings,values,reason))throw std::runtime_error(reason);transaction.writeSet=std::move(values.writeSet);return std::move(transaction);});
					preparedTransaction=task->getFuture();preparedAction="static-shadow-step";state="PreparingWorker";ThreadPool::instance().enqueue(task);return;
				}
				if(stage=="P4" && installedSteps.value("shadowBatchFixture",false))
				{
					// Observe live targets only here, then prepare copied UUID/binding
					// values on workers and use the existing protected receipt barrier.
					if(!ParseRendererEfficiencyShadowBatchControls(step,activeFixtureIntent,error)) {fail(error);return;}
					const auto requests=step.at("nativePayload").at("transactionRequests");
					Scene::TransactionalMutationBuilder observed(operationId+"-shadow-batch-step-"+std::to_string(stepIndex),Scene::TransactionOrigin::Automation,"RendererEfficiencyShadowBatchStep");observed.ObserveScene(objects);
					std::set<UUID> ids;for(const auto& request:requests) ids.insert(ParseUUID(request.at("objectId").get<std::string>()).value());
					for(const auto& id:ids) {if(!objects.getObject(id)) {fail("P4 typed target is absent from its protected scene.");return;} observed.ObserveObject(objects,id);}
					auto transaction=std::move(observed).Build();const auto bindings=publishedBindings;
					auto task=ThreadPool::instance().createTask([transaction=std::move(transaction),requests,bindings]() mutable {
						Scene::Transaction values;std::string reason;if(!PrepareRendererEfficiencyShadowBatchTransaction(requests,bindings,values,reason)) throw std::runtime_error(reason);
						transaction.writeSet=std::move(values.writeSet);return std::move(transaction);
					});preparedTransaction=task->getFuture();preparedAction="shadow-batch-step";state="PreparingWorker";ThreadPool::instance().enqueue(task);return;
				}
				if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && ((installedSteps.contains("lightingPoolSize") || installedSteps.value("depthFixture", false)) || state == "PreparingLightingChange"))
				{
					sharedLightingEditsDone = installedSteps.value("sharedLighting", false);
					const auto requests = step.at("nativePayload").at("transactionRequests");
					Renderer::RendererEfficiencyFixtureLightingControls controls;
					if(!ParseRendererEfficiencyLightingControls(step.at("nativePayload"), false, controls, error)) { fail(error); return; }
					activeFixtureIntent.lightingControls = std::move(controls); fixtureIntentQueued = false;
					if((stage=="P4" || stage=="P5") && installedSteps.value("sharedLighting",false) && !ParseRendererEfficiencyShadowBatchControls(step,activeFixtureIntent,error,false)) {fail(error);return;}
					// The unchanged recipe still crosses the already observed protected
					// baseline; only nonempty authored edits need another transaction.
					if(requests.empty()) { lightingTransitionPending = true; state = (stage=="P4" || stage=="P5") && action=="unsupported-grouped-route" ? "SeekingBatchDeformation" : "AwaitingNativeCapability"; }
					else
					{
						Scene::TransactionalMutationBuilder observed(operationId + "-lighting-step-" + std::to_string(stepIndex), Scene::TransactionOrigin::Automation, "RendererEfficiencyLightingStep"); observed.ObserveScene(objects);
						std::set<UUID> ids; for(const auto& request : requests) ids.insert(ParseUUID(request.at("objectId").get<std::string>()).value()); for(const auto& id : ids) observed.ObserveObject(objects, id);
						auto transaction = std::move(observed).Build();
						const bool staticRequests=stage=="P5";
						auto task = ThreadPool::instance().createTask([transaction = std::move(transaction), requests, staticRequests]() mutable {
							Scene::Transaction values; std::string error;
							if(!(requests.empty() || (staticRequests ? PrepareRendererEfficiencyStaticShadowTransaction(requests, {}, values, error) : PrepareRendererEfficiencyLightingTransaction(requests, values, error)))) throw std::runtime_error(error);
							transaction.writeSet = std::move(values.writeSet); return std::move(transaction);
						});
						preparedTransaction = task->getFuture(); preparedAction = "lighting-step"; state = "PreparingWorker"; ThreadPool::instance().enqueue(task);
					}
					return;
				}
				if(stage=="P5" && installedSteps.value("sharedStaticShadow",false)) {
					const auto& raw=step.at("nativePayload").at("staticShadowControls");
					if(!raw.at("forceTransferUnavailable").is_boolean() || !raw.at("failAllocation").is_boolean())throw std::runtime_error("Shared P5 controls must remain typed booleans.");
					activeFixtureIntent.staticShadowControls=Renderer::RendererEfficiencyFixtureStaticShadowControls{raw.at("forceTransferUnavailable").get<bool>(),raw.at("failAllocation").get<bool>()};
					if(action=="unsupported-depth-transfer" || action=="zero-budget" || action=="too-small-budget" || action=="whole-cube-admission" || action=="lower-budget-in-flight" || action=="retirement-view-churn") {state="PreparingLightingChange";return;}
				}
				if((action == "buffer-overflow" || ((stage=="P4" || stage=="P5") && (action=="group-overflow" || action=="unsupported-grouped-route"))) && installedSteps.value("sharedLighting", false)) { state = "PreparingLightingChange"; return; }
				if(action == "stable") { stepMutationReady = true; state = "AwaitingFirstObservation"; unavailableReason = "Awaiting actual completed native GPU bundle for the stable fixture step."; }
				else if(action == "recorded-submit-abort" || action == "incomplete-point-faces" || action == "resource-recreation")
				{
					// A completed owner must exist before testing its invalidation.
					faultPhase = 1; stepFrame = warmupFrames + measuredFrames;
					stepMutationReady = true; state = "AwaitingFirstObservation";
				}
				else if(action == "residency")
				{
					// As with fault testing, establish a fresh completed baseline after
					// source restoration before evicting the native resource owner.
					faultPhase = 4; stepFrame = warmupFrames + measuredFrames;
					stepMutationReady = true; state = "AwaitingFirstObservation";
				}
				else if(action == "transform" || action == "membership-removal" || action == "light-assignment" || action == "lifetime-replacement" || action == "transaction-conflict-rollback")
				{
					const auto id = ParseUUID(step.at("target").at("objectId").get<std::string>()).value();
					Scene::TransactionalMutationBuilder captured(operationId + "-step-" + std::to_string(stepIndex), Scene::TransactionOrigin::Automation, "RendererEfficiencyStep");
					captured.ObserveScene(objects); captured.ObserveObject(objects, id);
					if(action == "light-assignment") captured.ObserveObject(objects, ParseUUID(step.at("nativePayload").at("pairedDisableTargetId").get<std::string>()).value());
					auto detached = std::move(captured).Build(); const auto payload = step.at("nativePayload");
					if(action == "transaction-conflict-rollback")
					{
						const auto object = objects.getObject(id); const auto* transform = object ? objects.GetComponent<Transform>(*object) : nullptr;
						if(!transform || detached.readSet.objects.size() != 1) throw std::runtime_error("Owned rejected-batch target has no exact protected transform observation.");
						expectingRejectedTransaction = true; rejectedTargetObservation = detached.readSet.objects.front();
						rejectedTargetPose = transform->worldPose; rejectedTargetActive = objects.isObjectActive(*object);
						rejectedTargetStatic = objects.isObjectStatic(*object);
						rejectedTargetSpatialRevision = objects.GetObjectSpatialRevision(*object);
						std::string publicationReason;
						if(!CaptureRendererEfficiencyRollbackPublication(objects, detached.readSet, rejectedPriorPublication, publicationReason))
							throw std::runtime_error(publicationReason);
						rejectedPriorSpatialGeneration = rejectedPriorPublication.rendererSpatialRevision;
					}
					const auto bindings = publishedBindings;
					auto task = ThreadPool::instance().createTask([detached = std::move(detached), action, payload, id, bindings]() mutable {
						Scene::TransactionWriteOperation write; write.target.objectId = id;
						if(action == "transform" || action == "lifetime-replacement" || action == "transaction-conflict-rollback")
						{
							if(action == "transaction-conflict-rollback")
							{
								// Native validation rejects writing the deleted target. This is
								// an actual invalid typed batch, with no synthetic failpoint.
								Scene::TransactionWriteOperation remove; remove.kind = Scene::TransactionWriteKind::DeleteObject; remove.target.objectId = id;
								detached.writeSet.operations.push_back(std::move(remove));
							}
							write.kind = Scene::TransactionWriteKind::SetWorldTransform;
							const auto& pose = payload.at("worldPose"); const auto& position = pose.at("position");
							const auto integer = [](const nlohmann::json& value) {
								const auto text = value.get<std::string>(); std::size_t end = 0;
								const auto number = std::stoll(text, &end); if(end != text.size()) throw std::runtime_error("Invalid canonical cell"); return number;
							};
							write.worldPose.position.cell = {integer(position.at("cell").at("x")), integer(position.at("cell").at("y")), integer(position.at("cell").at("z"))};
							for(int axis = 0; axis < 3; ++axis) { write.worldPose.position.offsetMetres[axis] = position.at("offsetMetres").at(axis).get<double>(); write.worldPose.scale[axis] = pose.at("scale").at(axis).get<float>(); }
							const auto& q = pose.at("rotation"); write.worldPose.rotation = glm::quat(q.at(0).get<float>(), q.at(1).get<float>(), q.at(2).get<float>(), q.at(3).get<float>());
							if(action == "lifetime-replacement")
							{
								// Delete and recreate the same logical UUID atomically. The scene
								// queue allocates its real new lifetime; no fixture counter does.
								Scene::TransactionWriteOperation remove; remove.kind = Scene::TransactionWriteKind::DeleteObject; remove.target.objectId = id;
								detached.writeSet.operations.push_back(std::move(remove));
								Scene::TransactionWriteOperation create; create.kind = Scene::TransactionWriteKind::CreateObject; create.createdLocalObjectId = 1; create.createdStableObjectId = id; create.displayName = payload.at("name").get<std::string>();
								detached.writeSet.operations.push_back(std::move(create));
								Scene::TransactionWriteOperation component; component.kind = Scene::TransactionWriteKind::AddComponent; component.target.localObjectId = 1; component.componentName = "MeshRenderComponent";
								detached.writeSet.operations.push_back(std::move(component));
								const auto role = payload.at("replacementRole").get<std::string>();
								for(const auto& property : {std::string("meshId"), std::string("materialId")})
								{
									Scene::TransactionWriteOperation binding; binding.kind = Scene::TransactionWriteKind::UpdateComponentProperty; binding.target.localObjectId = 1; binding.componentName = "MeshRenderComponent"; binding.propertyName = property;
									binding.propertyValue = Reflection::Variant(bindings.at(role + (property == "meshId" ? ".mesh" : ".material")));
									detached.writeSet.operations.push_back(std::move(binding));
								}
								write.target = {}; write.target.localObjectId = 1;
							}
						}
						else if(action == "membership-removal") { write.kind = Scene::TransactionWriteKind::SetActive; write.boolValue = payload.at("active").get<bool>(); }
						else { write.kind = Scene::TransactionWriteKind::UpdateComponentProperty; write.componentName = "LightComponent"; write.propertyName = "castsShadows"; write.propertyValue = Reflection::Variant(true); }
						detached.writeSet.operations.push_back(std::move(write));
						if(action == "light-assignment") { Scene::TransactionWriteOperation paired; paired.kind = Scene::TransactionWriteKind::UpdateComponentProperty; paired.target.objectId = ParseUUID(payload.at("pairedDisableTargetId").get<std::string>()).value(); paired.componentName = "LightComponent"; paired.propertyName = "castsShadows"; paired.propertyValue = Reflection::Variant(false); detached.writeSet.operations.push_back(std::move(paired)); }
						return detached;
					});
					preparedTransaction = task->getFuture(); ThreadPool::instance().enqueue(task);
					preparedAction = action; state = "PreparingWorker"; return;
				}
				else { state = "AwaitingNativeCapability"; unavailableReason = "Unavailable native fixture capability: " + action + "; no step was substituted or measured."; return; }
			}
			if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && installedSteps.value("sharedLighting", false) && sharedLightingBaselineDone && !sharedLightingEditsDone
				&& stepMutationReady && !lightingBaselineObservation && faultPhase != 1 && faultPhase != 2 && faultPhase != 4 && faultPhase != 5)
			{
				// Original transactions, recovery and fault observations settle first.
				// Their retained controls are not replaced by the lighting membership edit.
				state = "PreparingLightingChange"; stepMutationReady = false; return;
			}
			if(state == "AwaitingNativeCapability" && (step.at("action") == "origin" || step.at("action") == "selected-lod" || step.at("action") == "selected-meshlet"
				|| step.at("action") == "distinct-origins" || step.at("action") == "alternate-view-order"
				|| step.at("action") == "different-caster-selections" || step.at("action") == "alternate-snapshot-slot"
				|| step.at("action") == "B-record-overwrite-before-A-old-owner-hit" || lightingTransitionPending))
			{
				const auto options = renderer.GetRendererEfficiencyOptionsSnapshot();
				const auto action = step.at("action").get<std::string>();
				if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && installedSteps.value("sharedLighting", false) && !sharedLightingEditsDone)
				{
					// These actions change only native view controls; their shared recipe
					// has no additional membership writes. Keep both kinds in one transition.
					if(!step.at("nativePayload").at("transactionRequests").empty()) { fail("Shared native control action requires a separate typed lighting edit."); return; }
					sharedLightingEditsDone = true;
					Renderer::RendererEfficiencyFixtureLightingControls lighting;
					if(!ParseRendererEfficiencyLightingControls(step.at("nativePayload"), false, lighting, error)) { fail(error); return; }
					lighting.snapshotSlot = 0; activeFixtureIntent.lightingControls = std::move(lighting);
					if((stage=="P4" || stage=="P5") && !ParseRendererEfficiencyShadowBatchControls(step,activeFixtureIntent,error,false)) {fail(error);return;}
				}
				if(activeFixtureIntent.correlation.fixtureStepId != step.at("id").get<std::string>())
				{
					const auto lighting = activeFixtureIntent.lightingControls;const auto batching=activeFixtureIntent.shadowBatchControls;const auto statics=activeFixtureIntent.staticShadowControls;const auto selected=activeFixtureIntent.casterSelections;const auto staticCameras=activeFixtureIntent.cameras;
					activeFixtureIntent = {}; activeFixtureIntent.shadowBatchControls=batching;activeFixtureIntent.staticShadowControls=statics;activeFixtureIntent.casterSelections=selected; activeFixtureIntent.cameras = stage=="P5" ? staticCameras : fixtureCameras; activeFixtureIntent.lightingControls = lighting; if(stage=="P3") activeFixtureIntent.depthControls=depthStepControls.at(stepIndex);
					activeFixtureIntent.correlation = {operationId, step.at("id").get<std::string>(), stepFrame, mutationGeneration, options.settingsGeneration, options.rendererSessionGeneration};
					const auto queued = renderer.QueueRendererEfficiencyFixtureIntent(activeFixtureIntent);
					if(!queued.accepted) { unavailableReason = queued.reason; return; }
					return; // One real capture first establishes the owned target proof.
				}
				if(lightingTransitionPending && !fixtureIntentQueued)
				{
					activeFixtureIntent.correlation = {operationId, step.at("id").get<std::string>(), 0, mutationGeneration, options.settingsGeneration, options.rendererSessionGeneration};
					if(activeFixtureIntent.lightingControls) {activeFixtureIntent.lightingControls->snapshotSlot = 0;activeFixtureIntent.lightingControls->frameSlot.reset();}
					const auto queued = renderer.QueueRendererEfficiencyFixtureIntent(activeFixtureIntent);
					if(!queued.accepted) { unavailableReason = queued.reason; return; }
					fixtureIntentQueued = true; return;
				}
				if(action == "origin" && !activeFixtureIntent.referenceOriginDelta)
				{
					Scene::WorldDelta delta; delta.offsetMetres.x = 1.0;
					activeFixtureIntent.referenceOriginDelta = delta;
					const auto queued = renderer.QueueRendererEfficiencyFixtureIntent(activeFixtureIntent);
					if(!queued.accepted) { activeFixtureIntent.referenceOriginDelta.reset(); unavailableReason = queued.reason; }
					return;
				}
				if((action == "selected-lod" || action == "selected-meshlet") && !activeFixtureIntent.shadowSelection)
				{
					const auto target = renderer.GetRendererEfficiencyFixtureTarget(ParseUUID(step.at("target").at("objectId").get<std::string>()).value());
					if(!target || !target->ownerGeometrySupported || target->detailLevels.empty()) { unavailableReason = "Awaiting exact immutable owner geometry proof."; return; }
					Renderer::RendererEfficiencyFixtureShadowSelection selection;
					selection.objectId = target->objectId; selection.meshId = target->meshId;
					selection.lifetimeGeneration = target->lifetimeGeneration; selection.resourceGeneration = target->resourceGeneration;
					if(action == "selected-lod")
					{
						const auto found = std::find_if(target->detailLevels.begin(), target->detailLevels.end(), [](const auto& detail) { return detail.lodIndex == 1; });
						if(found == target->detailLevels.end()) { unavailableReason = "Actual cooked LOD 1 is unavailable."; return; }
						selection.kind = Renderer::RendererEfficiencyFixtureSelectionKind::DetailLevel;
						selection.lodIndex = found->lodIndex; selection.meshletOffset = found->meshletOffset; selection.meshletCount = found->meshletCount;
					}
					else
					{
						const auto found = std::find_if(target->detailLevels.begin(), target->detailLevels.end(), [](const auto& detail) { return detail.meshletCount > 1; });
						if(found == target->detailLevels.end()) { unavailableReason = "No actual cooked detail has two meshlets for a distinct valid subset."; return; }
						selection.kind = Renderer::RendererEfficiencyFixtureSelectionKind::MeshletRange;
						selection.lodIndex = found->lodIndex; selection.meshletOffset = found->meshletOffset + 1; selection.meshletCount = found->meshletCount - 1;
					}
					activeFixtureIntent.shadowSelection = selection;
					const auto queued = renderer.QueueRendererEfficiencyFixtureIntent(activeFixtureIntent);
					if(!queued.accepted) { activeFixtureIntent.shadowSelection.reset(); unavailableReason = queued.reason; }
					return;
				}
				if((action == "distinct-origins" || action == "alternate-view-order" || action == "alternate-snapshot-slot" || action == "B-record-overwrite-before-A-old-owner-hit")
					&& activeFixtureIntent.viewOrigins.empty() && activeFixtureIntent.viewOrder.empty())
				{
					// These copied native controls affect real output capture/order,
					// without moving canonical objects or creating duplicate view draws.
					const auto& payload = step.at("nativePayload");
					if(payload.contains("views")) for(const auto& view : payload.at("views"))
					{
						Renderer::RendererEfficiencyFixtureViewOrigin origin;
						origin.viewType = view.at("viewType") == "Scene" ? RenderViewType::Scene : RenderViewType::Game;
						origin.outputId = view.at("viewId").get<std::uint32_t>();
						const auto& cells = view.at("origin").at("cell"); const auto& offsets = view.at("origin").at("offsetMetres");
						if(cells.size() != 3 || offsets.size() != 3) throw std::runtime_error("Fixture origin requires three exact cell/offset coordinates.");
						for(const auto& cell : cells) if(!cell.is_number_integer()) throw std::runtime_error("Fixture origin cell must be an integer.");
						origin.delta.cellDelta = {cells[0].get<std::int64_t>(), cells[1].get<std::int64_t>(), cells[2].get<std::int64_t>()};
						origin.delta.offsetMetres = {offsets[0].get<double>(), offsets[1].get<double>(), offsets[2].get<double>()};
						activeFixtureIntent.viewOrigins.push_back(origin);
					}
					if(payload.contains("viewOrder")) for(const auto& view : payload.at("viewOrder"))
						activeFixtureIntent.viewOrder.push_back({view.at("viewType") == "Scene" ? RenderViewType::Scene : RenderViewType::Game, view.at("viewId").get<std::uint32_t>()});
					const auto queued = renderer.QueueRendererEfficiencyFixtureIntent(activeFixtureIntent);
					if(!queued.accepted) { activeFixtureIntent.viewOrigins.clear(); activeFixtureIntent.viewOrder.clear(); unavailableReason = queued.reason; }
					return;
				}
				if(action == "different-caster-selections" && activeFixtureIntent.casterSelections.empty())
				{
					// Sealed logical targets become controls only after their actual
					// component and accepted named mesh publication agree while protected.
					for(const auto& view : step.at("nativePayload").at("views"))
					{
						Renderer::RendererEfficiencyFixtureCasterSelection selection;
						selection.viewType = view.at("viewType") == "Scene" ? RenderViewType::Scene : RenderViewType::Game;
						selection.outputId = view.at("viewId").get<std::uint32_t>();
						for(const auto& value : view.at("casterObjectIds"))
						{
							const auto id = ParseUUID(value.get<std::string>()).value();
							const auto object = objects.getObject(id); const auto* mesh = object ? objects.GetComponent<MeshRenderComponent>(*object) : nullptr;
							const nlohmann::json* target = nullptr;
							for(const auto& [role, row] : fixtureRoleTargets.items()) if(role != "sharedLightingPool" && row.at("objectId") == value && row.at("componentName") == "MeshRenderComponent") { target = &row; break; }
							if(!target || !mesh || mesh->meshId != publishedBindings.at(target->at("assetBindings").at("meshId").get<std::string>())) throw std::runtime_error("Per-view caster target lacks an exact live named mesh binding.");
							selection.objectIds.push_back(id);
						}
						activeFixtureIntent.casterSelections.push_back(std::move(selection));
					}
					const auto queued = renderer.QueueRendererEfficiencyFixtureIntent(activeFixtureIntent);
					if(!queued.accepted) { activeFixtureIntent.casterSelections.clear(); unavailableReason = queued.reason; }
					return;
				}
				if(exportJob.valid() && pendingExportPurpose == EvidenceExportPurpose::ControlTransition)
				{
					if(exportJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
					// The pump's fresh session/marker validation precedes consumption.
					// A late job from a stopped/replaced operation never reaches here.
					auto [result, exportError] = exportJob.get();
					if(!exportError.empty()) { fail("Control transition export failed: " + exportError); return; }
					const auto currentControls = renderer.GetRendererEfficiencyFixtureControlSnapshot();
					if(!RendererEfficiencyFixtureControlsMatch(activeFixtureIntent, currentControls)
						|| options.optionsSessionGeneration != pendingTransitionOptionsEpoch
						|| currentControls.controlGeneration != activeFixtureIntent.controlGeneration
						|| currentControls.correlation.settingsGeneration != options.settingsGeneration || currentControls.correlation.rendererSessionGeneration != options.rendererSessionGeneration)
					{ fail("Original control owner changed before transition export acceptance."); return; }
					pendingTransitionReceipt["frames"] = std::move(result.frames);
					pendingTransitionReceipt["artifacts"] = result.artifacts;
					exportedArtifactCount += result.artifacts.size();
					for(auto& artifact : result.artifacts) exportedArtifacts.push_back(std::move(artifact));
					const auto control = activeFixtureIntent.controlGeneration;
					if(!RecordFixtureReceipt(root, generation, operationId, std::move(pendingTransitionReceipt), error)) { fail(error); return; }
					pendingTransitionReceipt = nullptr; pendingExportPurpose = EvidenceExportPurpose::Sample; pendingTransitionOptionsEpoch = 0;
					lastAcceptedFrame = pendingExportFrame; pendingExportFrame = 0; pendingEvidenceBundles.clear();
					if(lightingTransitionPending)
					{
						// P4 preserves the first actual fallback receipt, then disarms
						// its one-shot seam instead of injecting a new failure per sample.
						if(stage=="P4") activeFixtureIntent=RendererEfficiencyShadowBatchMeasurementIntent(activeFixtureIntent);
						lightingTransitionPending = false; stepMutationReady = true; state = "AwaitingFirstObservation";
						renderer.ClearRendererEfficiencyFixtureIntent(operationId); fixtureIntentQueued = false; return;
					}
					if(!AdmitNativeStepPublication(root, generation, operationId, step.at("id").get<std::string>(), control, error)) { fail(error); return; }
					auto measurement = activeFixtureIntent;
					measurement.correlation = {operationId, step.at("id").get<std::string>(), stepFrame, mutationGeneration, options.settingsGeneration, options.rendererSessionGeneration};
					if(RendererEfficiencyTransitionNeedsSamplingRearm(activeFixtureIntent, measurement.correlation))
					{
						// Clear removes only this operation's fixture capture/intent
						// metadata. Physical shadow owners and shader leases survive.
						// Requeue at this same protected boundary: no uncontrolled
						// blank frame, fabricated generation, or forced shadow refresh.
						renderer.ClearRendererEfficiencyFixtureIntent(operationId);
						const auto queued = renderer.QueueRendererEfficiencyFixtureIntent(measurement);
						if(!queued.accepted || !queued.controlGeneration) { fail("Colliding transition token could not rearm native capture: " + queued.reason); return; }
						measurement.controlGeneration = queued.controlGeneration;
						measurement.controlsApplied = false; measurement.controlReason.clear();
						activeFixtureIntent = std::move(measurement); fixtureIntentQueued = true;
					}
					return; // Transition is not one of the frozen warmup/measured samples.
				}
				const auto applied = renderer.GetRendererEfficiencyFixtureControlSnapshot();
				if(applied.correlation != activeFixtureIntent.correlation || applied.correlation.operationId != operationId || applied.correlation.fixtureStepId != step.at("id").get<std::string>()
					|| applied.correlation.settingsGeneration != options.settingsGeneration || applied.correlation.rendererSessionGeneration != options.rendererSessionGeneration
					|| !applied.controlsApplied || applied.referenceOriginDelta != activeFixtureIntent.referenceOriginDelta || applied.shadowSelection != activeFixtureIntent.shadowSelection
					|| applied.viewOrigins != activeFixtureIntent.viewOrigins || applied.viewOrder != activeFixtureIntent.viewOrder || applied.cameras != activeFixtureIntent.cameras || applied.casterSelections != activeFixtureIntent.casterSelections || applied.shadowBatchControls != activeFixtureIntent.shadowBatchControls || applied.staticShadowControls != activeFixtureIntent.staticShadowControls || applied.lightingControls != activeFixtureIntent.lightingControls)
				{
					// A protected snapshot may still belong to the preceding command.
					// Forward its native reason only through exact current correlation.
					const auto reason = RendererEfficiencyPendingObservationReason(activeFixtureIntent, applied, {});
					unavailableReason = reason.empty() ? "Awaiting actual protected fixture control application." : reason;
					return;
				}
				activeFixtureIntent = applied;
				const auto batch = renderer.GetRendererEfficiencyEvidenceBatch(batchCursor);
				if(batch.batchSequence > batchCursor)
				{
					batchCursor = batch.batchSequence;
					for(const auto& row : batch.rows)
					{
						if(!RendererEfficiencyCompletedControlMatches(row, applied)) continue;
						// Unavailable is a terminal native decision even when submission
						// or completion failed; never replace it with a later reused frame.
						if(row.status == Renderer::RendererEfficiencyEvidenceStatus::Unavailable) { fail("First control transition native observation is unavailable: " + row.unavailableReason); return; }
						if(row.status != Renderer::RendererEfficiencyEvidenceStatus::Valid || !row.completionSerial) continue;
						auto& rows = pendingEvidenceBundles[{row.frameSerial, row.submitSerial}];
						const auto duplicate = std::find_if(rows.begin(), rows.end(), [&](const auto& prior) { return prior.viewType == row.viewType && prior.viewId == row.viewId && prior.kind == row.kind && prior.analysisPurpose == row.analysisPurpose && prior.lightId == row.lightId && prior.faceIndex == row.faceIndex; });
						if(duplicate == rows.end()) rows.push_back(row);
						if(rows.size() > 64) { fail("Original control transition exceeds the bounded region count."); return; }
					}
					if(pendingEvidenceBundles.size() > 4) { fail("Original control transition cohort exceeded its bounded pending window."); return; }
				}
				if(pendingEvidenceBundles.empty()) { unavailableReason = "Awaiting original completed control transition cohort."; return; }
				// Only the earliest native sampled cohort can prove first visibility.
				// A later reused image cannot substitute for its missing first faces.
				std::vector<Renderer::RendererEfficiencyEvidence> rows;
				if(!SelectRendererEfficiencyControlTransition(step.at("requiredBundle"), applied, options, pendingEvidenceBundles.begin()->second, rows, error)) { unavailableReason = error; return; }
				if(stage=="P4" && installedSteps.value("shadowBatchFixture",false) && step.at("action")=="mixed-route") {
					bool direct=false,virtualGeometry=false;
					for(const auto& row:rows) if(row.artifacts) for(const auto& caster:row.artifacts->eligibleMembership.rows) {
						direct |= caster.geometryRoute==static_cast<std::uint32_t>(Renderer::GeometryRoute::DirectMeshlet);
						virtualGeometry |= caster.geometryRoute==static_cast<std::uint32_t>(Renderer::GeometryRoute::VirtualGeometryHardware) || caster.geometryRoute==static_cast<std::uint32_t>(Renderer::GeometryRoute::VirtualGeometryCompute);
					}
					if(!direct || !virtualGeometry) {active=false;state="Unavailable";nativeProviderFailure=true;unavailableReason="Actual mixed-route fixture did not publish both direct and virtual-geometry owners.";renderer.ClearRendererEfficiencyFixtureIntent(operationId);return;}
				}
				const auto& first = rows.front(); const auto& token = first.fixtureCorrelation;
				RendererEfficiencyEvidenceExportContext context;
				context.projectRoot = projectRoot; context.outputDirectory = projectRoot / "renderer-efficiency-evidence" / operationId / ("step" + std::to_string(stepIndex)) / "transition";
				context.fixtureId = fixtureId; context.fixtureHash = fixtureHash; context.operationId = operationId; context.fixtureStepId = token.fixtureStepId; context.stage = stage;
				context.runId = runId; context.runIndex = runIndex; context.seed = seed; context.mode = runMode; context.measurementPhase = "Transition";
				context.settingsGeneration = token.settingsGeneration; context.mutationGeneration = token.mutationGeneration; context.rendererSessionGeneration = token.rendererSessionGeneration;
				pendingTransitionReceipt = {{"kind", "ControlTransition"}, {"fixtureStepId", token.fixtureStepId}, {"action", action}, {"controlGeneration", std::to_string(applied.controlGeneration)},
					{"originalCorrelation", {{"operationId", token.operationId}, {"fixtureStepId", token.fixtureStepId}, {"stepFrame", token.stepFrame}, {"mutationGeneration", token.mutationGeneration}, {"settingsGeneration", token.settingsGeneration}, {"rendererSessionGeneration", token.rendererSessionGeneration}}},
					{"frameSerial", std::to_string(first.frameSerial)}, {"commandSerial", std::to_string(first.commandSerial)}, {"submitSerial", std::to_string(first.submitSerial)}, {"completionSerial", std::to_string(first.completionSerial)}};
				// The receipt command is the first region's real representative;
				// nested rows retain each output's own primary command identity.
				pendingExportFrame = first.frameSerial; pendingExportPurpose = EvidenceExportPurpose::ControlTransition;
				pendingTransitionOptionsEpoch = first.effectiveOptions.optionsSessionGeneration;
				auto task = ThreadPool::instance().createTask([context, rows = std::move(rows)]() {
					RendererEfficiencyEvidenceExportResult result; std::string exportError;
					ExportRendererEfficiencyEvidence(context, rows, result, exportError); return std::make_pair(std::move(result), std::move(exportError));
				});
				exportJob = task->getFuture(); unavailableReason = "Exporting original completed control transition."; ThreadPool::instance().enqueue(task); return;
			}
			if(!stepMutationReady) return;
			const auto acceptCompletion = [&](std::uint64_t frame) {
				lastAcceptedFrame = frame; ++stepFrame; fixtureIntentQueued = false; pendingEvidenceBundles.clear();
				if(warmupCompleted < warmupFrames) { ++warmupCompleted; state = "Warming"; }
				else { ++measuredCompleted; state = "Measuring"; }
				unavailableReason.clear();
				if(measuredCompleted == measuredFrames)
				{
					if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && ((installedSteps.contains("lightingPoolSize") || installedSteps.value("depthFixture", false)) || installedSteps.value("sharedLighting", false)))
						for(const auto& slots : lightingObservedSlots) if(slots.size() != 24) throw std::runtime_error("P2 measurement did not observe every actual frame/snapshot/purpose owner.");
					++stepIndex; stepFrame = warmupCompleted = measuredCompleted = 0; sharedLightingBaselineDone = sharedLightingEditsDone = false;
					denseBaselineBinding = denseResourcePrepared = denseBaselineObserved = denseAwaitingObservation = false;
					denseBaselineMeshId = EMPTY_UUID; denseBaselineMeshPublication = 0;
					denseBaselineAttempts = 0; animationReadyStepFrame.reset(); staticResourceApplied=false; staticBudgetRequestedGeneration=0; staticLowerBudgetRequestedGeneration=0; pendingStaticLifetimeOwner.reset(); completedStaticLifetimeOwner.reset(); pendingStaticBudgetReceipt=nullptr;
					stepMutationReady = false; state = "PreparingBaseline";
					faultPhase = 0;
				}
			};
			if(exportJob.valid() && (pendingExportPurpose == EvidenceExportPurpose::ResidencyBaseline || pendingExportPurpose == EvidenceExportPurpose::ResidencyReupload))
			{
				if(exportJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
				// Operation/marker ownership was freshly checked above. Recheck the
				// protected control and options owner before accepting detached bytes.
				const auto currentControls = renderer.GetRendererEfficiencyFixtureControlSnapshot();
				const auto currentOptions = renderer.GetRendererEfficiencyOptionsSnapshot();
				if(!RendererEfficiencyFixtureControlsMatch(activeFixtureIntent, currentControls)
					|| currentControls.controlGeneration != activeFixtureIntent.controlGeneration
					|| currentOptions.optionsSessionGeneration != pendingTransitionOptionsEpoch
					|| currentOptions.settingsGeneration != activeFixtureIntent.correlation.settingsGeneration
					|| currentOptions.rendererSessionGeneration != activeFixtureIntent.correlation.rendererSessionGeneration
					|| currentOptions.options != observedOptions.options)
				{ fail("Residency observation owner changed before export acceptance."); return; }
				auto [result, exportError] = exportJob.get();
				if(!exportError.empty() || (result.frames.size() != 9 && result.frames.size() != 18) || result.artifacts.empty()) { fail("Residency cohort export failed: " + exportError); return; }
				pendingTransitionReceipt["frames"] = std::move(result.frames); pendingTransitionReceipt["artifacts"] = result.artifacts;
				if(pendingExportPurpose == EvidenceExportPurpose::ResidencyBaseline)
				{
					residencyBaselineObservation = std::move(pendingTransitionReceipt);
					exportedArtifactCount += result.artifacts.size(); for(auto& artifact : result.artifacts) exportedArtifacts.push_back(std::move(artifact));
					// The one actual invalidation starts only after complete baseline
					// bytes are sealed; it is never repeated by export polling.
					if(!AdmitRendererEfficiencyResidencyReupload(activeFixtureIntent, warmupFrames + measuredFrames, stepFrame, fixtureIntentQueued,
						[&](const auto& intent) { return renderer.QueueRendererEfficiencyFixtureIntent(intent); },
						[&](std::string& reason) { return renderer.QueueRendererResourcePublicationInvalidation(residencyMeshId, AssetType::Mesh, residencyPriorPublication, &reason); }, error)) { fail(error); return; }
					faultPhase = 5;
				}
				else
				{
					nlohmann::json receipt;
					if(!CompleteRendererEfficiencyResidencyPublicationReceipt(pendingResidencyReceipt, residencyBaselineObservation, pendingTransitionReceipt, receipt, error)
						|| !RecordFixtureReceipt(root, generation, operationId, std::move(receipt), error)) { fail(error); return; }
					exportedArtifactCount += result.artifacts.size(); for(auto& artifact : result.artifacts) exportedArtifacts.push_back(std::move(artifact));
					faultPhase = 3; stepFrame = 0;
					residencyBaselineObservation = nullptr; pendingResidencyReceipt = nullptr;
				}
				// These two original observations consume neither warmup nor measured
				// counts. Sampling resumes only after the actual retained result wins.
				lastAcceptedFrame = pendingExportFrame; pendingExportFrame = 0; pendingEvidenceBundles.clear();
				// Baseline acceptance already armed the real reupload before eviction.
				// Preserve it; only successful reupload export resets sampling to zero.
				if(pendingExportPurpose == EvidenceExportPurpose::ResidencyReupload) fixtureIntentQueued = false;
				pendingTransitionReceipt = nullptr; pendingTransitionOptionsEpoch = 0; pendingExportPurpose = EvidenceExportPurpose::Sample; return;
			}
			if(exportJob.valid())
			{
				if(exportJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
				if(pendingExportPurpose == EvidenceExportPurpose::LightingBaseline
					&& !RendererEfficiencyLightingExportOwnerMatches(activeFixtureIntent, pendingTransitionOptionsEpoch, observedOptions.options,
						renderer.GetRendererEfficiencyFixtureControlSnapshot(), renderer.GetRendererEfficiencyOptionsSnapshot()))
				{ fail("Lighting baseline owner changed before detached export acceptance."); return; }
				auto [result, exportError] = exportJob.get();
				// ValidateSession already checked the exact operation and marker this
				// pump. A failed export cannot release the frozen measurement token.
				if(!exportError.empty()) { fail("Completed evidence export failed: " + exportError); return; }
				if(pendingExportPurpose == EvidenceExportPurpose::LightingBaseline)
				{
					// Persist original baseline samples before authoring an edit. Their
					// counters and bytes remain separate from measured root frames.
					for(auto& row : result.frames) lightingBaselineReceipt["frames"].push_back(std::move(row));
					exportedArtifactCount += result.artifacts.size();
					for(auto& artifact : result.artifacts) { lightingBaselineReceipt["artifacts"].push_back(artifact); exportedArtifacts.push_back(std::move(artifact)); }
					++lightingBaselineCompleted; ++stepFrame; lastAcceptedFrame = pendingExportFrame; pendingExportFrame = 0;
					fixtureIntentQueued = false; pendingEvidenceBundles.clear(); pendingExportPurpose = EvidenceExportPurpose::Sample; pendingTransitionOptionsEpoch = 0;
					if(lightingBaselineCompleted == 60)
					{
						if(!RendererEfficiencyLightingBaselineReady(lightingBaselineCompleted, lightingObservedSlots)) { fail("Lighting baseline did not warm all 24 actual physical owners per purpose."); return; }
						if(!RecordFixtureReceipt(root, generation, operationId, std::move(lightingBaselineReceipt), error)) { fail(error); return; }
						renderer.ClearRendererEfficiencyFixtureIntent(operationId); stepFrame = 0;
						lightingBaselineObservation = false; sharedLightingBaselineDone = installedSteps.value("sharedLighting", false); stepMutationReady = false; state = "PreparingStep";
						for(auto& slots : lightingObservedSlots) slots.clear();
					}
					return;
				}
				exportedFrameCount += result.frames.size(); exportedArtifactCount += result.artifacts.size();
				for(auto& row : result.frames) exportedFrames.push_back(std::move(row));
				for(auto& artifact : result.artifacts) exportedArtifacts.push_back(std::move(artifact));
				if(pendingStaticLifetimeOwner) completedStaticLifetimeOwner=std::move(pendingStaticLifetimeOwner);
				acceptCompletion(pendingExportFrame); pendingExportFrame = 0; return;
			}
			if(stage=="P5" && runMode=="Candidate" && step.at("nativePayload").contains("lowerBudgetWhilePendingBytes") && stepFrame>0)
			{
				const auto options=renderer.GetRendererEfficiencyOptionsSnapshot();
				const auto lowered=step.at("nativePayload").at("lowerBudgetWhilePendingBytes").get<std::uint64_t>();
				if(!staticLowerBudgetRequestedGeneration) {
					// Join a previously accepted qualified row to CURRENT physical uses.
					// The prior completed row alone never proves an in-flight reference.
					const auto* context=renderer.GetContext();const auto lifetime=context?context->GetRendererEfficiencyStaticLifetimeSnapshot():std::nullopt;
					if(!lifetime || !completedStaticLifetimeOwner || !RendererEfficiencyStaticOwnerHasOutstandingUse(*lifetime,*completedStaticLifetimeOwner)) {
						active=false;state="Unavailable";nativeProviderFailure=true;unavailableReason="No exact current outstanding static owner use before owned budget lowering.";renderer.ClearRendererEfficiencyFixtureIntent(operationId);return;
					}
					auto desired=options.options;desired.staticShadowCacheBudgetBytes=lowered;
					const auto receipt=renderer.QueueRendererEfficiencyOptions(desired);if(!receipt.accepted){fail(receipt.error);return;}
					staticLowerBudgetRequestedGeneration=receipt.requestedGeneration;pendingStaticBudgetReceipt={{"kind","StaticBudgetPublication"},{"fixtureStepId",step.at("id")},{"operationId",operationId},{"requestedGeneration",receipt.requestedGeneration},{"priorOptions",SerializeRendererEfficiencyOptions(options.options)},{"requestedOptions",SerializeRendererEfficiencyOptions(desired)}};
					nlohmann::json owners=nlohmann::json::array(),accepted=nlohmann::json::array();
					for(const auto& owner:lifetime->allocations){nlohmann::json uses=nlohmann::json::array();for(const auto& use:owner.uses)uses.push_back({{"commandSerial",std::to_string(use.commandSerial)},{"submitSerial",std::to_string(use.submitSerial)},{"frameSlot",use.frameSlot}});owners.push_back({{"sessionGeneration",std::to_string(owner.sessionGeneration)},{"backendGeneration",std::to_string(owner.backendGeneration)},{"allocationIdentity",std::to_string(owner.allocationIdentity)},{"allocationGeneration",std::to_string(owner.allocationGeneration)},{"actualBytes",std::to_string(owner.actualBytes)},{"retiring",owner.retiring},{"uses",std::move(uses)}});}
					for(const auto& use:lifetime->acceptedSubmissions)accepted.push_back({{"commandSerial",std::to_string(use.commandSerial)},{"submitSerial",std::to_string(use.submitSerial)},{"frameSlot",use.frameSlot}});
					nlohmann::json completed=nlohmann::json::array(),discarded=nlohmann::json::array();for(auto value:lifetime->completedSubmissions)completed.push_back(std::to_string(value));for(auto value:lifetime->discardedCommands)discarded.push_back(std::to_string(value));
					nlohmann::json proof={{"kind","StaticBudgetLowering"},{"fixtureStepId",step.at("id")},{"stepFrame",stepFrame},{"requestedGeneration",receipt.requestedGeneration},{"priorOptions",SerializeRendererEfficiencyOptions(options.options)},{"requestedOptions",SerializeRendererEfficiencyOptions(desired)},{"rendererSessionGeneration",options.rendererSessionGeneration},{"optionsSessionGeneration",options.optionsSessionGeneration},{"allocationIdentity",std::to_string(completedStaticLifetimeOwner->allocationIdentity)},{"allocationGeneration",std::to_string(completedStaticLifetimeOwner->allocationGeneration)},{"backendGeneration",std::to_string(lifetime->backendGeneration)},{"allocations",std::move(owners)},{"acceptedSubmissions",std::move(accepted)},{"completedSubmissions",std::move(completed)},{"discardedCommands",std::move(discarded)}};
					if(!RecordFixtureReceipt(root,generation,operationId,std::move(proof),error)){fail(error);return;}return;
				}
				if(options.settingsGeneration<staticLowerBudgetRequestedGeneration || options.options.staticShadowCacheBudgetBytes!=lowered){unavailableReason="Awaiting actual protected in-flight budget publication.";return;}
				observedOptions=options;
			}
			if(stage=="P5" && installedSteps.value("staticShadowFixture",false) && !lightingTransitionPending
				&& step.at("nativePayload").value("animatePerSample",false) && animationReadyStepFrame != stepFrame)
			{
				// Each native sample owns a completed actual Seek publication. Polling,
				// GPU completion and export delay never advance the requested tick.
				renderer.ClearRendererEfficiencyFixtureIntent(operationId);fixtureIntentQueued=false;
				state="SeekingStaticSample";return;
			}
			if(!fixtureIntentQueued)
			{
				const auto options = renderer.GetRendererEfficiencyOptionsSnapshot();
				Renderer::RendererEfficiencyFixtureIntent intent = activeFixtureIntent;
				intent.correlation = {operationId, step.at("id").get<std::string>(), stepFrame, mutationGeneration, options.settingsGeneration, options.rendererSessionGeneration};
				intent.sampleTick = stage=="P5" && step.at("nativePayload").value("animatePerSample",false) ? RendererEfficiencyStaticSampleTick(stepFrame) : step.at("nativePayload").value("seekTick", std::int64_t{0});
				if(intent.lightingControls)
				{
					// Capture waits for the real native frame owner instead of assuming
					// collection cadence naturally visits all three frames in flight.
					intent.lightingControls->snapshotSlot = static_cast<std::uint32_t>(stepFrame % 8);
					intent.lightingControls->frameSlot = static_cast<std::uint32_t>((stepFrame % 24) / 8);
				}
				// View churn changes real qualified origins, not output IDs or receipts.
				// The same accepted frame index drives both independent run modes.
				if(stage=="P5" && step.at("action")=="retirement-view-churn") {
					intent.viewOrigins.clear();for(const auto& camera:intent.cameras){Renderer::RendererEfficiencyFixtureViewOrigin origin;origin.viewType=camera.viewType;origin.outputId=camera.outputId;origin.delta.offsetMetres.x=100.0*static_cast<double>(stepFrame%2);intent.viewOrigins.push_back(origin);}
				}
				const auto receipt = renderer.QueueRendererEfficiencyFixtureIntent(intent);
				if(!receipt.accepted) { unavailableReason = receipt.reason; return; }
				activeFixtureIntent = intent;
				unavailableReason = "Awaiting actual protected fixture control application.";
				fixtureIntentQueued = true;
			}
			const auto appliedControls = renderer.GetRendererEfficiencyFixtureControlSnapshot();
			const auto batch = renderer.GetRendererEfficiencyEvidenceBatch(batchCursor);
			const auto pendingReason = RendererEfficiencyPendingObservationReason(activeFixtureIntent, appliedControls,
				batch.batchSequence > batchCursor ? std::span<const Renderer::RendererEfficiencyEvidence>(batch.rows) : std::span<const Renderer::RendererEfficiencyEvidence>{});
			if(!pendingReason.empty()) unavailableReason = pendingReason;
			if(batch.batchSequence > batchCursor)
			{
				batchCursor = batch.batchSequence;
				for(const auto& row : batch.rows)
				{
					const auto& token = row.fixtureCorrelation;
					// Terminal fault rows carry real native receipts even when their
					// pixels are unavailable. Inspect them before the valid-pixel filter.
					if(faultPhase == 2) for(const auto& action : row.fixtureActions)
					{
						const auto& correlation = action.correlation;
						if(correlation.operationId != operationId || correlation.fixtureStepId != step.at("id").get<std::string>()
							|| correlation.stepFrame != stepFrame || correlation.mutationGeneration != mutationGeneration
							|| correlation.settingsGeneration != observedOptions.settingsGeneration || correlation.rendererSessionGeneration != observedOptions.rendererSessionGeneration) continue;
						const auto injection = activeFixtureIntent.injection;
						const bool proved = action.injection == injection && action.recorded && action.frameSerial && action.commandSerial
							&& ((injection == Renderer::RendererEfficiencyFixtureInjection::RecordedSubmitAbort && !action.submitted && !action.completed && !action.submitSerial && !action.completionSerial)
								|| (injection == Renderer::RendererEfficiencyFixtureInjection::IncompletePointFaces && action.submitted && action.completed && action.submitSerial && action.completionSerial && action.requiredFaceMask == 63 && action.recordedFaceMask == 31 && action.priorOwnerRejected)
								|| (injection == Renderer::RendererEfficiencyFixtureInjection::ShadowResourceRecreation && action.submitted && action.completed && action.submitSerial && action.completionSerial && action.oldAllocationGeneration && action.newAllocationGeneration > action.oldAllocationGeneration && action.priorOwnerRejected));
						if(!proved) continue;
						const char* name = injection == Renderer::RendererEfficiencyFixtureInjection::RecordedSubmitAbort ? "RecordedSubmitAbort" : injection == Renderer::RendererEfficiencyFixtureInjection::IncompletePointFaces ? "IncompletePointFaces" : "ShadowResourceRecreation";
						nlohmann::json receipt{{"kind", "FixtureAction"}, {"fixtureStepId", correlation.fixtureStepId}, {"injection", name},
							{"correlation", {{"operationId", correlation.operationId}, {"fixtureStepId", correlation.fixtureStepId}, {"stepFrame", correlation.stepFrame}, {"mutationGeneration", correlation.mutationGeneration}, {"settingsGeneration", correlation.settingsGeneration}, {"rendererSessionGeneration", correlation.rendererSessionGeneration}}},
							{"frameSerial", action.frameSerial}, {"commandSerial", action.commandSerial}, {"submitSerial", action.submitSerial}, {"completionSerial", action.completionSerial},
							{"oldAllocationGeneration", action.oldAllocationGeneration}, {"newAllocationGeneration", action.newAllocationGeneration}, {"requiredFaceMask", action.requiredFaceMask}, {"recordedFaceMask", action.recordedFaceMask},
							{"recorded", action.recorded}, {"submitted", action.submitted}, {"completed", action.completed}, {"priorOwnerRejected", action.priorOwnerRejected}, {"reason", action.reason}};
						if(!RecordFixtureReceipt(root, generation, operationId, std::move(receipt), error)) { fail(error); return; }
						faultPhase = 3; activeFixtureIntent.injection = Renderer::RendererEfficiencyFixtureInjection::None;
						stepFrame = 0; fixtureIntentQueued = false; pendingEvidenceBundles.clear(); lastAcceptedFrame = action.frameSerial;
						return; // A new token requests a distinct healthy completion.
					}
					// Pin the first matching current owner, not a stale batch from a
					// preceding application that happens to share its numeric step tick.
					if((faultPhase == 4 || faultPhase == 5) && !RendererEfficiencyCompletedControlMatches(row, appliedControls)) continue;
					if((faultPhase == 4 || faultPhase == 5) && RendererEfficiencyCompletedControlMatches(row, appliedControls)
						&& row.frameSerial > lastAcceptedFrame && row.status == Renderer::RendererEfficiencyEvidenceStatus::Unavailable)
					{ fail("First residency native observation is unavailable: " + row.unavailableReason); return; }
					// The depth collector pins terminal failures for this exact token.
					// Ignoring one would wait forever for a later substitute that cannot
					// legitimately replace the first completed native observation.
					if(stage=="P3" && RendererEfficiencyFixtureControlsMatch(activeFixtureIntent,appliedControls)
						&& RendererEfficiencyDepthObservationUnavailable(row,appliedControls,renderer.GetRendererEfficiencyOptionsSnapshot(),lastAcceptedFrame))
					{active=false;state="Unavailable";nativeProviderFailure=true;unavailableReason="Current P3 depth observation is unavailable: "+row.fallbackReason+"; "+row.unavailableReason;renderer.ClearRendererEfficiencyFixtureIntent(operationId);return;}
					if(row.status != Renderer::RendererEfficiencyEvidenceStatus::Valid || !row.completionSerial || row.frameSerial <= lastAcceptedFrame
						|| token.operationId != operationId || token.fixtureStepId != step.at("id").get<std::string>() || token.stepFrame != stepFrame) continue;
					auto& rows = pendingEvidenceBundles[{row.frameSerial, row.submitSerial}];
					const auto duplicate = std::find_if(rows.begin(), rows.end(), [&](const auto& prior) {
						return prior.viewType == row.viewType && prior.viewId == row.viewId && prior.kind == row.kind && prior.analysisPurpose == row.analysisPurpose && prior.lightId == row.lightId && prior.faceIndex == row.faceIndex;
					});
					if(duplicate == rows.end()) rows.push_back(row);
					if(rows.size() > 64) throw std::runtime_error("Completed fixture frame exceeds bounded region count.");
				}
				if(pendingEvidenceBundles.size() > 4) throw std::runtime_error("Completed fixture bundles exceed the bounded in-flight frame window.");
			}
			const auto matching = [&](const auto& row, const auto& view, std::uint32_t face) {
				const auto& correlation = row.fixtureCorrelation;
				return row.status == Renderer::RendererEfficiencyEvidenceStatus::Valid && row.completionSerial != 0
					&& RendererEfficiencyCompletedControlMatches(row, appliedControls)
					&& row.frameSerial > lastAcceptedFrame && correlation.operationId == operationId
					&& correlation.fixtureStepId == step.at("id").get<std::string>() && correlation.stepFrame == stepFrame
					&& correlation.mutationGeneration == mutationGeneration && correlation.settingsGeneration == observedOptions.settingsGeneration
					&& correlation.rendererSessionGeneration == observedOptions.rendererSessionGeneration
					&& row.effectiveOptions.settingsGeneration == correlation.settingsGeneration
					&& row.effectiveOptions.rendererSessionGeneration == correlation.rendererSessionGeneration
					&& row.viewType == (view.at("viewType") == "Scene" ? RenderViewType::Scene : RenderViewType::Game)
					&& row.viewId == view.at("viewId").template get<std::uint32_t>() && row.lightId == ParseUUID(view.at("lightId").template get<std::string>()).value() && row.faceIndex == face;
			};
			std::uint64_t completedFrame = 0, completedSubmission = 0;
			std::vector<Renderer::RendererEfficiencyEvidence> completedRows;
			for(const auto& [frameKey, rows] : pendingEvidenceBundles)
			{
				if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && ((installedSteps.contains("lightingPoolSize") || installedSteps.value("depthFixture", false)) || installedSteps.value("sharedLighting", false)))
				{
					std::vector<Renderer::RendererEfficiencyEvidence> required;
					const auto options = renderer.GetRendererEfficiencyOptionsSnapshot();
					const bool currentOptions = std::all_of(rows.begin(), rows.end(), [&](const auto& row) { return row.effectiveOptions.optionsSessionGeneration == options.optionsSessionGeneration && row.effectiveOptions.options == options.options; });
					// Baselines export only purpose rows; normal shared frames must also
					// retain every original completed shadow light/face requirement.
					const bool selected = lightingBaselineObservation || (stage=="P2" && installedSteps.contains("lightingPoolSize"))
						? SelectRendererEfficiencyLightingBundle(step.at("requiredBundle"), appliedControls, rows, required, error)
						: SelectRendererEfficiencyControlTransition(step.at("requiredBundle"), appliedControls, options, rows, required, error);
					if(currentOptions && frameKey.first > lastAcceptedFrame && selected)
					{ completedFrame = frameKey.first; completedSubmission = frameKey.second; completedRows = std::move(required); break; }
					unavailableReason = error; continue;
				}
				// A later reused cohort cannot replace incomplete first residency
				// faces. Only the earliest retained native frame is eligible here.
				if((faultPhase == 4 || faultPhase == 5) && frameKey != pendingEvidenceBundles.begin()->first) break;
				std::vector<Renderer::RendererEfficiencyEvidence> required; bool complete = true;
				for(const auto& view : step.at("requiredBundle").at("views"))
					for(const auto& face : view.at("faceIndices"))
					{
						const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return matching(row, view, face.get<std::uint32_t>()); });
						if(found == rows.end()) { complete = false; continue; }
						required.push_back(*found);
					}
				if(complete) { completedFrame = frameKey.first; completedSubmission = frameKey.second; completedRows = std::move(required); break; }
			}
			if(!RendererEfficiencyFixtureControlsMatch(activeFixtureIntent, appliedControls) || !completedFrame || !completedSubmission) return;
			if((stage == "P2" || stage == "P3" || ((stage == "P4" || stage == "P5") && installedSteps.value("sharedLighting",false))) && ((installedSteps.contains("lightingPoolSize") || installedSteps.value("depthFixture", false)) || installedSteps.value("sharedLighting", false)))
			{
				for(const auto& row : completedRows)
				{
					if(row.kind != Renderer::RendererEfficiencyEvidenceKind::Lighting) continue;
					const auto purpose = row.analysisPurpose ? 1U : 0U; const auto& upload = row.lightingUploads[purpose];
					lightingObservedSlots[purpose].insert({upload.frameSlot, upload.snapshotSlot});
				}
			}
			if(denseAwaitingObservation)
			{
				const auto target = ParseUUID(step.at("target").at("objectId").get<std::string>()).value();
				if(!RendererEfficiencyDensePageBaselineMatches(completedRows, appliedControls, target, denseBaselineMeshId, denseBaselineMeshPublication, error))
				{
					if(!RendererEfficiencyDensePageBaselineRetryable(completedRows, appliedControls, target, denseBaselineMeshId, denseBaselineMeshPublication)) { unavailableReason = error; return; }
					++denseBaselineAttempts;
					if(denseBaselineAttempts >= 8) { fail("Dense native page baseline remains unavailable after eight actual completed cohorts: " + error); return; }
					// The current full cohort is consumed, never exported or counted.
					// A fresh owned application gets its generation from the renderer.
					renderer.ClearRendererEfficiencyFixtureIntent(operationId); lastAcceptedFrame = completedFrame; fixtureIntentQueued = false; pendingEvidenceBundles.clear();
					unavailableReason = "Awaiting native page payload after actual completed baseline attempt " + std::to_string(denseBaselineAttempts) + ": " + error; return;
				}
				++denseBaselineAttempts;
				// Clearing the owned baseline control lets the next ordinary intent
				// receive its real new generation after the changed native publication.
				renderer.ClearRendererEfficiencyFixtureIntent(operationId);
				denseAwaitingObservation = false; denseBaselineObserved = true; stepMutationReady = false; state = "PreparingStep";
				activeFixtureIntent = {}; activeFixtureIntent.cameras = fixtureCameras; if(stage=="P3" && stepIndex<depthStepControls.size()) activeFixtureIntent.depthControls=depthStepControls.at(stepIndex); fixtureIntentQueued = false;
				pendingEvidenceBundles.clear(); lastAcceptedFrame = completedFrame; unavailableReason.clear(); return;
			}
			if(faultPhase == 4 || faultPhase == 5)
			{
				const auto currentOptions = renderer.GetRendererEfficiencyOptionsSnapshot();
				std::vector<Renderer::RendererEfficiencyEvidence> exactRows;
				if(!SelectRendererEfficiencyControlTransition(step.at("requiredBundle"), appliedControls, currentOptions, completedRows, exactRows, error)) { unavailableReason = error; return; }
				completedRows = std::move(exactRows); activeFixtureIntent = appliedControls;
				const auto target = ParseUUID(step.at("target").at("objectId").get<std::string>()).value();
				const Renderer::ShadowCasterStamp* stamp = nullptr;
				const Renderer::RendererEfficiencyEvidence* proofRow = nullptr;
				for(const auto& row : completedRows) if(row.kind==Renderer::RendererEfficiencyEvidenceKind::Shadow && row.artifacts) for(const auto& candidate : row.artifacts->eligibleMembership.rows)
					if(candidate.objectId == target && candidate.meshRevision && candidate.residencyGeneration) { stamp = &candidate; proofRow = &row; break; }
				if(!stamp || !proofRow) { unavailableReason = "Awaiting completed caster membership with an actual native residency allocation."; return; }
				for(const auto& row : completedRows)
				{
					if(row.kind != Renderer::RendererEfficiencyEvidenceKind::Shadow) continue;
					const auto& membership = row.artifacts->eligibleMembership;
					const auto found = std::find_if(membership.rows.begin(), membership.rows.end(), [&](const auto& value) { return value.objectId == target; });
					if(!membership.reusable || found == membership.rows.end() || !Renderer::IsShadowCasterStampReusable(*found)
						|| found->meshId != stamp->meshId || found->meshRevision != stamp->meshRevision || found->lifetimeGeneration != stamp->lifetimeGeneration || found->residencyGeneration != stamp->residencyGeneration)
					{ fail("Residency target lacks one exact native owner across all first completed regions."); return; }
				}
				if(faultPhase == 4)
				{
					const auto object = objects.getObject(target); const auto* mesh = object ? objects.GetComponent<MeshRenderComponent>(*object) : nullptr;
					if(!mesh || mesh->meshId == EMPTY_UUID || mesh->meshId != stamp->meshId) throw std::runtime_error("Residency target has no matching protected mesh binding.");
					residencyMeshId = mesh->meshId; residencyPriorAllocation = stamp->residencyGeneration;
					residencyPriorPublication = stamp->meshRevision; residencyPriorFrame = completedFrame; residencyLifetime = stamp->lifetimeGeneration;
				}
				else if(stamp->meshId != residencyMeshId || stamp->meshRevision != residencyPriorPublication || stamp->lifetimeGeneration != residencyLifetime)
				{ fail("Residency target publication or lifetime changed during owned native eviction."); return; }
				if(faultPhase == 5 && stamp->residencyGeneration == residencyPriorAllocation)
				{
					// Retirement can defer the real eviction. Ask for another actual
					// completion instead of getting stuck on the collector's frozen token.
					++stepFrame; fixtureIntentQueued = false; pendingEvidenceBundles.clear(); lastAcceptedFrame = completedFrame;
					unavailableReason = "Awaiting actual completed reupload with a new residency allocation."; return;
				}
				if(faultPhase == 5) pendingResidencyReceipt = {{"kind", "ResidencyPublication"}, {"fixtureStepId", step.at("id")}, {"targetObjectId", ToHexString(target)}, {"meshId", ToHexString(residencyMeshId)},
					{"previousMeshPublicationGeneration", residencyPriorPublication}, {"newMeshPublicationGeneration", stamp->meshRevision},
					{"previousAllocationGeneration", residencyPriorAllocation}, {"newAllocationGeneration", stamp->residencyGeneration},
					{"previousActualFrameSerial", residencyPriorFrame}, {"newActualFrameSerial", proofRow->frameSerial}, {"commandSerial", proofRow->commandSerial}, {"submitSerial", proofRow->submitSerial}, {"completionSerial", proofRow->completionSerial}};
				const auto& first = completedRows.front(); const auto& token = first.fixtureCorrelation;
				pendingTransitionReceipt = {{"controlGeneration", std::to_string(first.fixtureControlGeneration)},
					{"frameSerial", std::to_string(first.frameSerial)}, {"commandSerial", std::to_string(first.commandSerial)}, {"submitSerial", std::to_string(first.submitSerial)}, {"completionSerial", std::to_string(first.completionSerial)},
					{"originalCorrelation", {{"operationId", token.operationId}, {"fixtureStepId", token.fixtureStepId}, {"stepFrame", token.stepFrame}, {"mutationGeneration", token.mutationGeneration}, {"settingsGeneration", token.settingsGeneration}, {"rendererSessionGeneration", token.rendererSessionGeneration}}}};
				RendererEfficiencyEvidenceExportContext context;
				context.projectRoot = projectRoot; context.outputDirectory = projectRoot / "renderer-efficiency-evidence" / operationId / ("step" + std::to_string(stepIndex)) / (faultPhase == 4 ? "residency-baseline" : "residency-transition");
				context.fixtureId = fixtureId; context.fixtureHash = fixtureHash; context.operationId = operationId; context.fixtureStepId = token.fixtureStepId; context.stage = stage;
				context.runId = runId; context.runIndex = runIndex; context.seed = seed; context.mode = runMode; context.measurementPhase = "Transition";
				context.transitionPurpose = faultPhase == 4 ? RendererEfficiencyTransitionPurpose::ResidencyBaseline : RendererEfficiencyTransitionPurpose::ResidencyReupload;
				context.residencyBaselineStepFrame = warmupFrames + measuredFrames;
				context.settingsGeneration = token.settingsGeneration; context.mutationGeneration = token.mutationGeneration; context.rendererSessionGeneration = token.rendererSessionGeneration;
				pendingExportPurpose = faultPhase == 4 ? EvidenceExportPurpose::ResidencyBaseline : EvidenceExportPurpose::ResidencyReupload;
				pendingExportFrame = first.frameSerial; pendingTransitionOptionsEpoch = first.effectiveOptions.optionsSessionGeneration;
				// Capture immutable CPU artifacts only. A stopped operation may leave
				// harmless orphan files, but cannot publish this result into a receipt.
				auto task = ThreadPool::instance().createTask([context, rows = std::move(completedRows)]() {
					RendererEfficiencyEvidenceExportResult result; std::string reason;
					if(!ExportRendererEfficiencyEvidence(context, rows, result, reason) && reason.empty()) reason = "Native residency pixels unavailable.";
					return std::make_pair(std::move(result), std::move(reason));
				});
				exportJob = task->getFuture(); unavailableReason = "Exporting first original completed residency cohort."; ThreadPool::instance().enqueue(task); return;
			}
			if(faultPhase == 1)
			{
				const auto action = step.at("action").get<std::string>();
				activeFixtureIntent.injection = action == "recorded-submit-abort" ? Renderer::RendererEfficiencyFixtureInjection::RecordedSubmitAbort
					: action == "incomplete-point-faces" ? Renderer::RendererEfficiencyFixtureInjection::IncompletePointFaces : Renderer::RendererEfficiencyFixtureInjection::ShadowResourceRecreation;
				faultPhase = 2; ++stepFrame; fixtureIntentQueued = false; pendingEvidenceBundles.clear(); lastAcceptedFrame = completedFrame; return;
			}
			if(faultPhase == 2) return; // A healthy row cannot substitute for the requested fault receipt.
			if(stepFrame == 0 && !completedRows.empty())
			{
				// Settlement counts come from the completed renderer preparation,
				// rather than interpreting the requested policy as actual execution.
				const auto& row = completedRows.front();
				const auto& correlation = row.fixtureCorrelation;
				nlohmann::json receipt{{"kind", "Preparation"}, {"fixtureStepId", correlation.fixtureStepId},
					{"operationId", correlation.operationId}, {"frameSerial", row.frameSerial}, {"submitSerial", row.submitSerial}, {"completionSerial", row.completionSerial},
					{"settingsGeneration", correlation.settingsGeneration}, {"rendererSessionGeneration", correlation.rendererSessionGeneration},
					{"mode", row.effectiveOptions.options.validationPreparation == Renderer::RendererEfficiencyPreparation::Serial ? "Serial" : "Concurrent"},
					{"workerTasks", row.preparationWorkerTasks}, {"inlineTasks", row.preparationInlineTasks}, {"settled", row.preparationSettled}};
				if(!RecordFixtureReceipt(root, generation, operationId, std::move(receipt), error)) { fail(error); return; }
			}
			for(const auto& row : completedRows)
			{
				if(!row.artifacts || !row.artifacts->frameColor) throw std::runtime_error("Completed native color artifact is absent.");
				const auto& color = *row.artifacts->frameColor;
				if(stage=="P3") { const auto camera=std::find_if(fixtureCameras.begin(),fixtureCameras.end(),[&](const auto& value){return value.viewType==row.viewType && value.outputId==row.viewId;}); if(camera==fixtureCameras.end() || color.width!=camera->nativeExtent.x || color.height!=camera->nativeExtent.y) throw std::runtime_error("Native completed color differs from sealed per-step camera extent."); }
				else { if(colorWidth && (color.width != colorWidth || color.height != colorHeight)) throw std::runtime_error("Native completed color extent changed during the run."); colorWidth = color.width; colorHeight = color.height; }
			}
			if(stage=="P5") {
				pendingStaticLifetimeOwner.reset();
				for(const auto& row:completedRows) if(row.kind==Renderer::RendererEfficiencyEvidenceKind::Shadow && row.staticShadowExecution.available && row.staticShadowExecution.allocationIdentity) {
					const auto& native=row.staticShadowExecution;Renderer::RendererEfficiencyStaticShadowAllocation owner;
					owner.sessionGeneration=row.fixtureCorrelation.rendererSessionGeneration;owner.backendGeneration=native.backendGeneration;
					owner.allocationIdentity=native.allocationIdentity;owner.allocationGeneration=native.allocationGeneration;
					pendingStaticLifetimeOwner=std::move(owner);break;
				}
			}
			RendererEfficiencyEvidenceExportContext context;
			context.projectRoot = projectRoot;
			context.outputDirectory = projectRoot / "renderer-efficiency-evidence" / operationId / ("step" + std::to_string(stepIndex));
			context.fixtureId = fixtureId; context.fixtureHash = fixtureHash; context.operationId = operationId;
			context.fixtureStepId = step.at("id").get<std::string>(); context.stage = stage;
			context.runId = runId; context.runIndex = runIndex; context.seed = seed; context.mode = runMode;
			// Warmup needs the same genuine native bytes and receipts as measured
			// output. Its phase is captured without subtracting or renumbering ticks.
			context.measurementPhase = RendererEfficiencyCompletedExportPhase(warmupCompleted, warmupFrames);
			if(lightingBaselineObservation)
			{
				context.measurementPhase = "LightingBaseline"; pendingExportPurpose = EvidenceExportPurpose::LightingBaseline;
				activeFixtureIntent = appliedControls;
				pendingTransitionOptionsEpoch = completedRows.front().effectiveOptions.optionsSessionGeneration;
			}
			context.settingsGeneration = observedOptions.settingsGeneration; context.mutationGeneration = mutationGeneration;
			context.rendererSessionGeneration = observedOptions.rendererSessionGeneration;
			// Immutable shared CPU artifact owners are retained by this detached job.
			// The renderer keeps sampling this token frozen until export succeeds.
			auto task = ThreadPool::instance().createTask([context, rows = std::move(completedRows)]() {
				RendererEfficiencyEvidenceExportResult result; std::string error;
				if(!ExportRendererEfficiencyEvidence(context, rows, result, error) && error.empty()) error = "Native completed artifacts are unavailable.";
				return std::make_pair(std::move(result), std::move(error));
			});
			exportJob = task->getFuture(); pendingExportFrame = completedFrame;
			state = "ExportingCompletedEvidence"; ThreadPool::instance().enqueue(task);
		}
		catch(const std::exception& exception) { fail(std::string("Fixture preparation failed atomically: ") + exception.what()); }
	}
	nlohmann::json EditorRendererEfficiencyValidation::PendingNativeStep() const
	{
		if(!active || state != "AwaitingNativeCapability" || installedSteps.is_null()
			|| stepIndex >= installedSteps.at("steps").size()) return nullptr;
		return installedSteps.at("steps").at(stepIndex);
	}
	bool EditorRendererEfficiencyValidation::AdmitNativeStepPublication(const std::filesystem::path& root,
		std::uint64_t generation, std::string_view operation, std::string_view id,
		std::uint64_t actualPublication, std::string& error)
	{
		if(!HasOperation(operation) || !ValidateSession(root, generation, error)) return false;
		const auto step = PendingNativeStep();
		if(step.is_null() || step.at("id").get<std::string>() != id || actualPublication == 0)
		{ error = "Native fixture publication does not match the pending owned step."; return false; }
		nativeProviderFailure = false; mutationGeneration = actualPublication; stepMutationReady = true; state = "AwaitingFirstObservation";
		unavailableReason = "Awaiting actual GPU completion for the published native fixture change.";
		error.clear(); return true;
	}
	bool EditorRendererEfficiencyValidation::RecordFixtureReceipt(const std::filesystem::path& root,
		std::uint64_t generation, std::string_view operation, nlohmann::json receipt, std::string& error)
	{
		if(!HasOperation(operation) || !ValidateSession(root, generation, error)) return false;
		if(!receipt.is_object() || !receipt.contains("kind") || !receipt.at("kind").is_string() || fixtureReceipts.size() >= (stage=="P5" ? 8192U : 128U))
		{ error = "Owned native receipt metadata is invalid or exceeds its bounded step count."; return false; }
		if(receipt.contains("fixtureStepId") && (installedSteps.is_null() || stepIndex >= installedSteps.at("steps").size()
			|| receipt.at("fixtureStepId") != installedSteps.at("steps").at(stepIndex).at("id")))
		{ error = "Native receipt does not match the current owned fixture step."; return false; }
		fixtureReceipts.push_back(std::move(receipt)); error.clear(); return true;
	}
	bool EditorRendererEfficiencyValidation::AdmitCompletedDeviceRecreation(const std::filesystem::path& root,
		std::uint64_t generation, std::string_view operation, std::string_view step, std::uint64_t priorOptionsEpoch,
		const Renderer::RendererEfficiencyOptionsSnapshot& freshOptions, std::uint64_t completedBackendGeneration, std::string& error)
	{
		if(!HasOperation(operation) || !ValidateSession(root, generation, error)) return false;
		const auto pending = PendingNativeStep();
		if(pending.is_null() || pending.at("id").get<std::string>() != step || pending.at("action") != "device-recovery"
			|| priorOptionsEpoch != priorToken.optionsSessionGeneration || freshOptions.optionsSessionGeneration == priorOptionsEpoch
			|| !freshOptions.rendererSessionGeneration || !freshOptions.settingsGeneration || !completedBackendGeneration)
		{ error = "Completed device recreation does not match the exact owned options transition."; return false; }
		// The service has verified the real native recreation receipt and fresh
		// protected settings visibility. Preserve initial prior option values,
		// rebinding their restore capability only into this proven new epoch.
		priorToken.optionsSessionGeneration = freshOptions.optionsSessionGeneration;
		observedOptions = freshOptions; activeFixtureIntent = {}; activeFixtureIntent.cameras = fixtureCameras; if(stage=="P3" && stepIndex<depthStepControls.size()) activeFixtureIntent.depthControls=depthStepControls.at(stepIndex); fixtureIntentQueued = false;
		pendingEvidenceBundles.clear(); batchCursor = lastAcceptedFrame = 0;
		return AdmitNativeStepPublication(root, generation, operation, step, completedBackendGeneration, error);
	}
	nlohmann::json EditorRendererEfficiencyValidation::PendingDenseResourceBaseline() const
	{
		if(!active || state != "PreparingBaseline" || denseResourcePrepared || installedSteps.is_null() || stepIndex >= installedSteps.at("steps").size()) return nullptr;
		const auto& step = installedSteps.at("steps").at(stepIndex);
		return step.at("action") == "page-revision" && step.at("nativePayload").contains("denseBaselineReplacementPath") ? step : nlohmann::json(nullptr);
	}
	bool EditorRendererEfficiencyValidation::BeginResourceBaselineReset(const std::filesystem::path& root,
		std::uint64_t generation, std::string& error, bool densePage)
	{
		if(!ValidateSession(root, generation, error) || state != "PreparingBaseline") return false;
		if(densePage && PendingDenseResourceBaseline().is_null()) { error = "No owned dense page baseline is pending."; return false; }
		denseBaselineBinding = densePage;
		state = "AwaitingResourceBaseline"; return true;
	}
	bool EditorRendererEfficiencyValidation::RetainDenseResourceBaselineOwner(const std::filesystem::path& root,
		std::uint64_t generation, const UUID& meshId, std::uint64_t publication, std::string& error)
	{
		if(!ValidateSession(root, generation, error) || state != "AwaitingResourceBaseline" || !denseBaselineBinding || meshId == EMPTY_UUID || !publication)
		{ error = "Dense baseline owner does not match the active native import admission."; return false; }
		denseBaselineMeshId = meshId; denseBaselineMeshPublication = publication; return true;
	}
	bool EditorRendererEfficiencyValidation::AdmitResourceBaselinePublication(const std::filesystem::path& root,
		std::uint64_t generation, std::uint64_t publication, std::string& error)
	{
		if(!ValidateSession(root, generation, error) || state != "AwaitingResourceBaseline" || !publication) return false;
		mutationGeneration = publication; state = "PreparingBaseline"; error.clear(); return true;
	}
	void EditorRendererEfficiencyValidation::ObserveAtProtectedFrame(const std::filesystem::path& root,
		std::uint64_t generation, const Renderer::RendererEfficiencyEvidence& evidence)
	{
		if(!active) return;
		// Renderer recovery can replace the options owner without switching the
		// editor project. Its old token must never restore into that new session.
		if(evidence.effectiveOptions.optionsSessionGeneration != priorToken.optionsSessionGeneration)
		{
			active = false; state = "Failed";
			unavailableReason = "Renderer options session changed; prior options restoration is unavailable.";
			priorToken.valid = false;
			observedOptions = evidence.effectiveOptions;
			return;
		}
		std::string error;
		if(!ValidateSession(root, generation, error))
		{ active = false; state = "Failed"; unavailableReason = "Owned validation project changed before the protected frame."; return; }
		observedOptions = evidence.effectiveOptions;
		// Stage drivers added by later tasks must correlate fixture steps before
		// enabling acceptance. These are observation counts, never synthetic steps.
		if(evidence.status != Renderer::RendererEfficiencyEvidenceStatus::Valid) return;
		if(evidence.submitSerial > lastSubmit) { lastSubmit = evidence.submitSerial; ++submittedFrames; }
		if(evidence.completionSerial > lastComplete) { lastComplete = evidence.completionSerial; ++completedFrames; }
	}
	nlohmann::json EditorRendererEfficiencyValidation::Status() const
	{
		return {{"operationId", operationId}, {"active", active}, {"state", state}, {"status", state == "Completed" && !completedEvidencePath.empty() ? "Valid" : !installedSteps.is_null() && active && !nativeProviderFailure ? "Pending" : "Unavailable"}, {"unavailableReason", unavailableReason},
			{"projectRoot", projectRoot.generic_string()}, {"ownerId", ownerId}, {"fixtureId", fixtureId}, {"fixtureHash", fixtureHash}, {"stage", stage}, {"seed", seed}, {"runId", runId}, {"runIndex", runIndex},
			{"submittedFrames", submittedFrames}, {"completedFrames", completedFrames}, {"warmupCompletedFrames", warmupCompleted}, {"measuredCompletedFrames", measuredCompleted},
			{"completedSteps", stepIndex}, {"fixtureStepId", !installedSteps.is_null() && stepIndex < installedSteps.at("steps").size() ? installedSteps.at("steps").at(stepIndex).at("id") : nlohmann::json(nullptr)}, {"stepFrame", stepFrame},
			{"mutationGeneration", mutationGeneration}, {"pendingTransactionId", pendingTransactionId},
			{"denseBaselineAttempts", denseBaselineAttempts}, {"denseBaselineObserved", denseBaselineObserved},
			{"lightingBaselineCompleted", lightingBaselineCompleted}, {"lightingBaselineObservation", lightingBaselineObservation}, {"lightingFinalSlotsObserved", lightingObservedSlots[0].size()}, {"lightingAnalysisSlotsObserved", lightingObservedSlots[1].size()},
			{"exportedFrames", exportedFrameCount}, {"exportedArtifacts", exportedArtifactCount}, {"evidenceArtifactPath", completedEvidencePath},
			{"warmupFrames", warmupFrames}, {"measuredFrames", measuredFrames}, {"settingsGeneration", observedOptions.settingsGeneration}, {"queuedSettingsGeneration", observedOptions.settingsGeneration},
			{"priorToken", SerializeRendererEfficiencyToken(priorToken)}, {"visibilityBoundary", "protected_scene_mutation_flush"}};
	}
}
