// TerrainAutomationProvider.cpp -- Canonical terrain automation descriptors.
#include "TerrainAutomationProvider.h"
#include "../TerrainAutomationSchema.h"
#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using Json = nlohmann::json;

		Json NoArgumentsSchema()
		{
			return {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
		}

		Json TerrainCreateSchema(bool importing)
		{
			Json properties = {
				{"destination", {{"type", "string"}, {"description", "Destination .teterrain path inside the active project's Assets directory."}}},
				{"sampleSpacingMetres", {{"type", "number"}, {"exclusiveMinimum", 0.0}}},
				{"heightScaleMetres", {{"type", "number"}}},
				{"heightOffsetMetres", {{"type", "number"}}}
			};
			Json required = Json::array({"destination", "sampleSpacingMetres"});
			if(importing)
			{
				properties["source"] = {{"type", "string"}, {"description", "Source PNG16 or RAW16 little-endian heightmap path."}};
				properties["format"] = {{"type", "string"}, {"enum", Json::array({"png16", "raw16-le"})}, {"default", "png16"}};
				properties["width"] = {{"type", "integer"}, {"minimum", 1}};
				properties["height"] = {{"type", "integer"}, {"minimum", 1}};
				required.push_back("source");
			}
			else
			{
				properties["widthSamples"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 2048}};
				properties["depthSamples"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 2048}};
				required.push_back("widthSamples");
				required.push_back("depthSamples");
			}
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
		}

		Json TerrainEditSchema(std::initializer_list<std::string_view> operations)
		{
			Json operationKinds = Json::array();
			for(const std::string_view operation : operations) operationKinds.push_back(operation);
			const Json decimalString = {{"type", "string"}, {"pattern", "^-?[0-9]+$"}};
			const Json unsignedString = {{"type", "string"}, {"pattern", "^[0-9]+$"}};
			const Json hash = {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}};
			const Json uuid = {{"type", "string"}, {"pattern", "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"}};
			const Json worldPosition = {
				{"type", "object"},
				{"properties", {
					{"cellX", decimalString}, {"cellY", decimalString}, {"cellZ", decimalString},
					{"localX", {{"type", "number"}}}, {"localY", {{"type", "number"}}}, {"localZ", {{"type", "number"}}}
				}},
				{"required", Json::array({"cellX", "cellY", "cellZ", "localX", "localY", "localZ"})},
				{"additionalProperties", false}
			};
			const Json observation = {
				{"type", "object"},
				{"properties", {{"areaX", decimalString}, {"areaZ", decimalString}, {"editGeneration", unsignedString}, {"authoredContentHash", hash}}},
				{"required", Json::array({"areaX", "areaZ", "editGeneration", "authoredContentHash"})},
				{"additionalProperties", false}
			};
			// The application parser performs the final strict typed lowering. This
			// discovery schema makes every required canonical field constructible by
			// MCP clients instead of advertising an impossible empty object.
			return {
				{"type", "object"},
				{"properties", {
					{"schemaVersion", {{"type", "integer"}, {"const", 1}}},
					{"transactionId", uuid},
					{"correlationId", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
					{"idempotencyKey", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
					{"target", {{"type", "object"}, {"properties", {{"assetUuid", uuid}, {"instanceUuid", uuid}}}, {"required", Json::array({"assetUuid", "instanceUuid"})}, {"additionalProperties", false}}},
					{"operation", {{"type", "object"}, {"properties", {{"kind", {{"type", "string"}, {"enum", std::move(operationKinds)}}}, {"settings", {{"type", "object"}, {"additionalProperties", true}}}, {"seed", {{"type", "integer"}, {"minimum", 0}}}}}, {"required", Json::array({"kind", "settings"})}, {"additionalProperties", false}}},
					{"worldBounds", {{"type", "object"}, {"properties", {{"min", worldPosition}, {"max", worldPosition}}}, {"required", Json::array({"min", "max"})}, {"additionalProperties", false}}},
					{"expectedRootHash", hash},
					{"expectedLayerStackGeneration", unsignedString},
					{"observations", {{"type", "array"}, {"minItems", 1}, {"maxItems", 4096}, {"items", observation}}},
					{"budgets", {{"type", "object"}, {"properties", {{"maxAreas", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}}, {"maxSamples", {{"type", "integer"}, {"minimum", 1}}}, {"maxPreparedBytes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 536870912}}}, {"workloadClass", {{"type", "string"}, {"enum", Json::array({"interactive", "bounded", "large"})}}}}}, {"required", Json::array({"maxAreas", "maxSamples", "maxPreparedBytes", "workloadClass"})}, {"additionalProperties", false}}},
					{"previewOnly", {{"type", "boolean"}}},
					{"approvalFingerprint", {{"type", Json::array({"string", "null"})}, {"pattern", "^[0-9a-f]{32}$"}}}
				}},
				{"required", Json::array({"schemaVersion", "transactionId", "correlationId", "idempotencyKey", "target", "operation", "worldBounds", "expectedRootHash", "observations", "budgets", "previewOnly"})},
				{"additionalProperties", false}
			};
		}

		Json TerrainInputSchema(std::string_view name)
		{
			if(name == "terrain.create") return TerrainCreateSchema(false);
			if(name == "terrain.import_heightmap") return TerrainCreateSchema(true);
			if(name == "terrain.stroke.apply") return TerrainEditSchema({"raise", "lower", "smooth", "flatten", "ramp", "stamp", "height-remap"});
			if(name == "terrain.layers.set") return TerrainEditSchema({"paint", "erase-layer", "fill-layer", "normalize-layers"});
			if(name == "terrain.holes.set") return TerrainEditSchema({"paint-hole", "erase-hole"});
			if(name == "terrain.operation.start") return TerrainEditSchema({"terrace", "noise", "hydraulic-erosion", "thermal-erosion", "spline-grade"});
			if(name == "terrain.resize.preview")
				return {{"type", "object"}, {"properties", {
					{"kind", {{"type", "string"}, {"enum", Json::array({"expand", "crop"})}}},
					{"minX", {{"type", "integer"}}}, {"minZ", {{"type", "integer"}}},
					{"maxXExclusive", {{"type", "integer"}}}, {"maxZExclusive", {{"type", "integer"}}},
					{"minimumXAreas", {{"type", "integer"}, {"minimum", 0}, {"maximum", 32}}},
					{"maximumXAreas", {{"type", "integer"}, {"minimum", 0}, {"maximum", 32}}},
					{"minimumZAreas", {{"type", "integer"}, {"minimum", 0}, {"maximum", 32}}},
					{"maximumZAreas", {{"type", "integer"}, {"minimum", 0}, {"maximum", 32}}},
					{"policy", {{"type", "string"}, {"enum", Json::array({"edge-blend", "extend-edge", "flat-default"})}, {"default", "edge-blend"}}},
					{"transitionSamples", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1024}}},
					{"holePolicy", {{"type", "string"}, {"enum", Json::array({"close-at-former-edge", "extend-connected-boundary-holes"})}}}
				}}, {"required", Json::array({"kind"})}, {"additionalProperties", false}};
			if(name == "terrain.expand.start" || name == "terrain.crop.start")
			{
				Json required = Json::array({"proposalFingerprint"});
				if(name == "terrain.crop.start") required.push_back("acknowledgeDataLoss");
				return {{"type", "object"}, {"properties", {
					{"proposalFingerprint", {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}}},
					{"acknowledgeDataLoss", {{"type", "boolean"}}}
				}}, {"required", std::move(required)}, {"additionalProperties", false}};
			}
			if(name == "terrain.job.get" || name == "terrain.job.cancel")
				return {{"type", "object"}, {"properties", {{"jobId", {{"type", "string"}, {"minLength", 1}}}}}, {"required", Json::array({"jobId"})}, {"additionalProperties", false}};
			if(name == "terrain.rebuild")
				return {{"type", "object"}, {"properties", {{"target", {{"type", "string"}, {"enum", Json::array({"all", "collision", "persistence"})}, {"default", "all"}}}}}, {"additionalProperties", false}};
			if(name == "terrain.export")
				return {{"type", "object"}, {"properties", {{"profile", {{"type", "string"}, {"minLength", 1}}}, {"correlationId", {{"type", "string"}}}, {"idempotencyKey", {{"type", "string"}}}}}, {"required", Json::array({"profile"})}, {"additionalProperties", false}};
			return NoArgumentsSchema();
		}

		Json TerrainResizeResultSchema(std::string_view name)
		{
			const Json bounds = {{"type", "object"}, {"properties", {
				{"minX", {{"type", "integer"}}}, {"minZ", {{"type", "integer"}}},
				{"maxXExclusive", {{"type", "integer"}}},
				{"maxZExclusive", {{"type", "integer"}}}}}, {"additionalProperties", false}};
			Json properties = {
				{"jobId", {{"type", "string"}}}, {"transactionId", {{"type", "string"}}},
				{"stage", {{"type", "integer"}}}, {"progress", {{"type", "number"}}},
				{"completedAreas", {{"type", "integer"}}}, {"totalAreas", {{"type", "integer"}}},
				{"cancellationAvailable", {{"type", "boolean"}}},
				{"proposalFingerprint", {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}}},
				{"kind", {{"type", "string"}, {"enum", Json::array({"expand", "crop"})}}},
				{"previousBounds", bounds}, {"proposedBounds", bounds},
				{"policy", {{"type", "string"}}}, {"holePolicy", {{"type", "string"}}},
				{"transitionSamples", {{"type", "integer"}}},
				{"readyGeneration", {{"type", "integer"}}},
				{"previousAreaCount", {{"type", "integer"}}},
				{"proposedAreaCount", {{"type", "integer"}}},
				{"physicalWidthMetres", {{"type", "number"}}},
				{"physicalDepthMetres", {{"type", "number"}}},
				{"requiresDataLossConfirmation", {{"type", "boolean"}}},
				{"cropLoss", {{"type", "object"}}},
				{"warnings", {{"type", "array"}, {"items", {{"type", "string"}}}}},
				{"diagnostic", {{"type", "string"}}},
				{"visibilityBoundary", {{"type", "string"}}}
			};
			Json required = Json::array();
			if(name == "terrain.resize.preview") required = Json::array({"jobId", "stage", "visibilityBoundary"});
			else if(name == "terrain.job.get") required = Json::array({"jobId", "stage", "progress"});
			else required = Json::array({"transactionId", "proposalFingerprint", "visibilityBoundary"});
			return {{"type", "object"}, {"properties", std::move(properties)},
				{"required", std::move(required)}, {"additionalProperties", true}};
		}
	}

	TerrainAutomationProvider::TerrainAutomationProvider(AutomationHandlerResolver resolver) : resolveHandler(std::move(resolver)) {}
	std::string_view TerrainAutomationProvider::GetProviderId() const { return "terrain"; }
	std::vector<AutomationCapabilityRegistration> TerrainAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> result;
		// Each route resolves through the same typed adapter/window boundary. The
		// provider names a capability only when the dispatcher can return a truthful
		// receipt or an explicit prerequisite failure; discovery never invents work.
		for(const std::string_view name : {"terrain.asset.inspect", "terrain.create", "terrain.import_heightmap",
			"terrain.session.begin", "terrain.session.get", "terrain.session.cancel", "terrain.session.commit",
			"terrain.stroke.apply", "terrain.layers.set", "terrain.holes.set", "terrain.operation.start",
			"terrain.resize.preview", "terrain.expand.start", "terrain.crop.start",
			"terrain.rebuild", "terrain.job.get", "terrain.job.cancel", "terrain.diagnostics.get",
			"terrain.validate", "terrain.export", "terrain.save"})
		{
			AutomationCapabilityDescriptor descriptor; descriptor.name=std::string(name); descriptor.category="terrain"; descriptor.description="Canonical, generation-checked terrain operation.";
			descriptor.inputSchema=TerrainInputSchema(name);
			// Inspection and diagnostics publish the same immutable terrain snapshot
			// shape. Other commands retain their existing receipt object contract.
			if(name == "terrain.asset.inspect" || name == "terrain.diagnostics.get")
				descriptor.resultSchema = TerrainAutomationSnapshotResultSchema();
			else if(name == "terrain.resize.preview" || name == "terrain.expand.start"
				|| name == "terrain.crop.start" || name == "terrain.job.get")
				descriptor.resultSchema = TerrainResizeResultSchema(name);
			else descriptor.resultSchema = Json{{"type", "object"}};
			const bool read=name=="terrain.asset.inspect" || name=="terrain.session.get"
				|| name=="terrain.job.get" || name=="terrain.diagnostics.get";
			descriptor.permission=read?AutomationPermission::Read:AutomationPermission::ProjectWrite;
			descriptor.approvalPolicy=(name=="terrain.import_heightmap"||name=="terrain.export"||name=="terrain.rebuild"||name=="terrain.crop.start")?AutomationApprovalPolicy::HighImpact:AutomationApprovalPolicy::None;
			descriptor.mutationMode=read?MutationMode::ReadOnly:MutationMode::AssetMutation; descriptor.availability.requiresProject=true;
			descriptor.execution.workerPreparation=!read; descriptor.execution.protectedCommit=!read; descriptor.execution.visibilityBoundary=read?"immutable_terrain_snapshot":"terrain_authoring_publish";
			descriptor.execution.preparationReads={"TerrainAssetRoots"}; descriptor.execution.commitWrites=read?std::vector<std::string>{}:std::vector<std::string>{"TerrainPreparedTransactions","TerrainPublishedTransactions"};
			result.push_back({std::move(descriptor), resolveHandler(name), {}});
		}
		return result;
	}
}
