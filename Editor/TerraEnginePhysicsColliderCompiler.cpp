// TerraEnginePhysicsColliderCompiler.cpp - Non-interactive .tecollider cooker.
//
// The executable owns command-line diagnostics only. The typed recipe and
// envelope callbacks are supplied by the physics asset integration layer so
// this boundary never serializes provider classes or Jolt implementation data.

#include "Editor/AssetCompilers/PhysicsColliderCompiler.h"
#include "Physics/Assets/ColliderRecipe.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::filesystem::path settings;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		bool showHelp = false;
	};

	void PrintUsage()
	{
		std::cout << "Usage: TerraEnginePhysicsColliderCompiler --input <recipe.tecollider> --output <collider.tepcoll> --asset-id <guid> [--settings <physics-settings.json>]\n";
	}

	bool ParseCommandLine(int argc, char** argv, CommandLine& commandLine)
	{
		for(int index = 1; index < argc; ++index)
		{
			const std::string_view flag = argv[index];
			if(flag == "--help" || flag == "-h")
			{
				commandLine.showHelp = true;
				return true;
			}
			if((flag == "--input" || flag == "-i") && index + 1 < argc)
				commandLine.input = argv[++index];
			else if((flag == "--output" || flag == "-o") && index + 1 < argc)
				commandLine.output = argv[++index];
			else if(flag == "--settings" && index + 1 < argc)
				commandLine.settings = argv[++index];
			else if(flag == "--asset-id" && index + 1 < argc)
			{
				const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(argv[++index]);
				if(!parsed)
				{
					std::cerr << "--asset-id must be a 32-character hexadecimal GUID.\n";
					return false;
				}
				commandLine.assetId = *parsed;
			}
			else
			{
				std::cerr << "Unrecognized or incomplete argument: " << flag << '\n';
				return false;
			}
		}
		return commandLine.showHelp || (!commandLine.input.empty() && !commandLine.output.empty() && commandLine.assetId != TerraEngine::EMPTY_UUID);
	}

	std::string EscapeJson(std::string_view text)
	{
		std::string escaped;
		for(const char value : text)
		{
			if(value == '\\' || value == '"')
				escaped.push_back('\\');
			if(value == '\n')
				escaped += "\\n";
			else
				escaped.push_back(value);
		}
		return escaped;
	}

	void EmitReport(const TerraEngine::AssetPipeline::PhysicsColliderCompileReport& report)
	{
		std::cout << "{\"type\":\"physicsColliderCompile\",\"stage\":\""
			<< TerraEngine::AssetPipeline::ToString(report.stage)
			<< "\",\"published\":" << (report.published ? "true" : "false")
			<< ",\"preservedLastKnownGood\":" << (report.preservedLastKnownGood ? "true" : "false")
			<< ",\"providerRevision\":\"" << TerraEngine::AssetPipeline::kPhysicsColliderProviderRevision
			<< "\",\"diagnostics\":[";
		for(std::size_t index = 0U; index < report.diagnostics.size(); ++index)
		{
			const auto& diagnostic = report.diagnostics[index];
			if(index != 0U)
				std::cout << ',';
			std::cout << "{\"stage\":\"" << TerraEngine::AssetPipeline::ToString(diagnostic.stage)
				<< "\",\"code\":\"" << EscapeJson(diagnostic.code)
				<< "\",\"message\":\"" << EscapeJson(diagnostic.message) << "\"}";
		}
		std::cout << "]}" << std::endl;
	}

	struct CookContext
	{
		TerraEngine::Physics::ColliderRecipe normalizedRecipe;
		std::uint64_t recipeDocumentFingerprint = 0U;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	};

	std::uint64_t Fingerprint(const std::vector<std::byte>& bytes)
	{
		std::uint64_t value = 14695981039346656037ULL;
		for(const std::byte byte : bytes)
		{
			value ^= static_cast<std::uint8_t>(byte);
			value *= 1099511628211ULL;
		}
		return value;
	}

	bool ReadBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if(!stream)
			return false;
		const std::streamsize size = stream.tellg();
		if(size < 0)
			return false;
		bytes.resize(static_cast<std::size_t>(size));
		stream.seekg(0, std::ios::beg);
		return size == 0 || static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()), size));
	}

	void AddDiagnostic(TerraEngine::AssetPipeline::PhysicsColliderCompileReport& report, std::string code, std::string message)
	{
		if(report.diagnostics.size() < 32U)
			report.diagnostics.push_back({report.stage, std::move(code), std::move(message)});
	}

	bool MapRecipe(
		const TerraEngine::Physics::ColliderRecipe& source,
		TerraEngine::Physics::Backend::PhysicsShapeRecipe& destination)
	{
		using namespace TerraEngine::Physics;
		destination.sourceAssetId = source.provenance.sourceAssetId;
		destination.stableRecipeHash = source.StableHash();
		if(destination.stableRecipeHash == 0U)
			return false;
		switch(source.kind)
		{
		case ColliderRecipeKind::ConvexHull:
			destination.kind = PhysicsShapeKind::ConvexHull;
			destination.points = source.vertices;
			return true;
		case ColliderRecipeKind::TriangleMesh:
			destination.kind = PhysicsShapeKind::Mesh;
			destination.points = source.vertices;
			destination.indices = source.indices;
			return true;
		case ColliderRecipeKind::Compound:
			destination.kind = PhysicsShapeKind::Compound;
			for(const ColliderRecipeCompoundChild& child : source.compoundChildren)
			{
				if(!child.recipe)
					return false;
				Backend::PhysicsShapeRecipe childRecipe;
				childRecipe.localCenter = child.localPose.position;
				childRecipe.localRotation = child.localPose.rotation;
				if(!MapRecipe(*child.recipe, childRecipe))
					return false;
				destination.children.push_back(std::move(childRecipe));
			}
			return !destination.children.empty();
		}
		return false;
	}

	TerraEngine::Physics::PhysicsQuaternion Normalize(
		TerraEngine::Physics::PhysicsQuaternion rotation)
	{
		const float length = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y
			+ rotation.z * rotation.z + rotation.w * rotation.w);
		rotation.x /= length;
		rotation.y /= length;
		rotation.z /= length;
		rotation.w /= length;
		return rotation;
	}

	TerraEngine::Physics::PhysicsVector3 Rotate(
		const TerraEngine::Physics::PhysicsQuaternion& rotation,
		const TerraEngine::Physics::PhysicsVector3& point)
	{
		const TerraEngine::Physics::PhysicsVector3 axis{rotation.x, rotation.y, rotation.z};
		const auto cross = [](const auto& left, const auto& right) {
			return TerraEngine::Physics::PhysicsVector3{
				left.y * right.z - left.z * right.y,
				left.z * right.x - left.x * right.z,
				left.x * right.y - left.y * right.x};
		};
		const TerraEngine::Physics::PhysicsVector3 first = cross(axis, point);
		const TerraEngine::Physics::PhysicsVector3 doubled{2.0F * first.x, 2.0F * first.y, 2.0F * first.z};
		const TerraEngine::Physics::PhysicsVector3 second = cross(axis, doubled);
		return {
			point.x + rotation.w * doubled.x + second.x,
			point.y + rotation.w * doubled.y + second.y,
			point.z + rotation.w * doubled.z + second.z};
	}

	TerraEngine::Physics::PhysicsQuaternion Multiply(
		const TerraEngine::Physics::PhysicsQuaternion& left,
		const TerraEngine::Physics::PhysicsQuaternion& right)
	{
		return Normalize({
			left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
			left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
			left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
			left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z});
	}

	TerraEngine::Physics::PhysicsPose Compose(
		const TerraEngine::Physics::PhysicsPose& parent,
		const TerraEngine::Physics::PhysicsPose& child)
	{
		const TerraEngine::Physics::PhysicsVector3 offset = Rotate(parent.rotation, child.position);
		return {
			{parent.position.x + offset.x, parent.position.y + offset.y, parent.position.z + offset.z},
			Multiply(parent.rotation, child.rotation)};
	}

	void AccumulateBounds(
		const TerraEngine::Physics::ColliderRecipe& recipe,
		const TerraEngine::Physics::PhysicsPose& transform,
		TerraEngine::Physics::CookedColliderBounds& bounds,
		bool& hasBounds)
	{
		using namespace TerraEngine::Physics;
		for(const PhysicsVector3& vertex : recipe.vertices)
		{
			const PhysicsVector3 rotated = Rotate(transform.rotation, vertex);
			const PhysicsVector3 point{
				rotated.x + transform.position.x,
				rotated.y + transform.position.y,
				rotated.z + transform.position.z};
			if(!hasBounds)
			{
				bounds.minimum = point;
				bounds.maximum = point;
				hasBounds = true;
				continue;
			}
			bounds.minimum.x = std::min(bounds.minimum.x, point.x);
			bounds.minimum.y = std::min(bounds.minimum.y, point.y);
			bounds.minimum.z = std::min(bounds.minimum.z, point.z);
			bounds.maximum.x = std::max(bounds.maximum.x, point.x);
			bounds.maximum.y = std::max(bounds.maximum.y, point.y);
			bounds.maximum.z = std::max(bounds.maximum.z, point.z);
		}
		for(const ColliderRecipeCompoundChild& child : recipe.compoundChildren)
		{
			if(child.recipe)
			{
				AccumulateBounds(*child.recipe, Compose(transform, child.localPose), bounds, hasBounds);
			}
		}
	}

	std::uint64_t CountVerticesWide(const TerraEngine::Physics::ColliderRecipe& recipe)
	{
		std::uint64_t count = recipe.vertices.size();
		for(const TerraEngine::Physics::ColliderRecipeCompoundChild& child : recipe.compoundChildren)
			if(child.recipe)
				count += CountVerticesWide(*child.recipe);
		return count;
	}

	std::uint64_t CountTrianglesWide(const TerraEngine::Physics::ColliderRecipe& recipe)
	{
		std::uint64_t count = recipe.indices.size() / 3U;
		for(const TerraEngine::Physics::ColliderRecipeCompoundChild& child : recipe.compoundChildren)
			if(child.recipe)
				count += CountTrianglesWide(*child.recipe);
		return count;
	}

	std::uint64_t CountPartsWide(const TerraEngine::Physics::ColliderRecipe& recipe)
	{
		if(recipe.kind != TerraEngine::Physics::ColliderRecipeKind::Compound) return 1U;
		std::uint64_t count = 0U;
		for(const TerraEngine::Physics::ColliderRecipeCompoundChild& child : recipe.compoundChildren)
			if(child.recipe)
				count += CountPartsWide(*child.recipe);
		return count;
	}

	std::uint64_t CountMaterialSlotsWide(const TerraEngine::Physics::ColliderRecipe& recipe)
	{
		std::uint64_t count = recipe.materialSlots.size();
		for(const TerraEngine::Physics::ColliderRecipeCompoundChild& child : recipe.compoundChildren)
			if(child.recipe)
				count += CountMaterialSlotsWide(*child.recipe);
		return count;
	}
}

int main(int argc, char** argv)
{
	CommandLine commandLine;
	if(!ParseCommandLine(argc, argv, commandLine))
	{
		PrintUsage();
		return 2;
	}
	if(commandLine.showHelp)
	{
		PrintUsage();
		return 0;
	}

	// The callbacks capture a single worker-local context. No callback mutates
	// AssetDatabase state or a published output; only CompilePhysicsCollider's
	// final protected rename makes this request visible to other editor systems.
	auto context = std::make_shared<CookContext>();
	context->assetId = commandLine.assetId;
	TerraEngine::AssetPipeline::PhysicsColliderCompileRequest request;
	request.recipePath = commandLine.input;
	request.outputPath = commandLine.output;
	if(!commandLine.settings.empty() && !ReadBytes(commandLine.settings, request.settingsBytes))
	{
		std::cerr << "Physics settings file could not be read: " << commandLine.settings << '\n';
		return 2;
	}
	request.callbacks.canReusePublishedOutput = [context](
		const std::filesystem::path& outputPath,
		const std::vector<std::byte>& recipeBytes,
		std::uint64_t sourceFingerprint,
		std::uint64_t settingsFingerprint,
		TerraEngine::AssetPipeline::PhysicsColliderCompileReport&) {
		(void)sourceFingerprint;
		(void)settingsFingerprint;
		try
		{
			std::vector<std::byte> outputBytes;
			if(!ReadBytes(outputPath, outputBytes)) return false;
			TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
			const auto existing = TerraEngine::Physics::CookedColliderAsset::Deserialize(outputBytes, diagnostics);
			if(!existing || existing->Envelope().assetId != context->assetId
				|| existing->Envelope().status != TerraEngine::Physics::CookedColliderStatus::Valid)
				return false;
			const auto compatibility = existing->CheckCompatibility(
				TerraEngine::Physics::CookedColliderAsset::HostPlatform(),
				TerraEngine::Physics::CookedColliderProviderFamily::Jolt,
				TerraEngine::Physics::kJoltCookProviderVersion,
				TerraEngine::Physics::kJoltCookRevision);
			if(!compatibility.compatible) return false;

			TerraEngine::Physics::ColliderRecipe parsed;
			TerraEngine::Physics::ColliderRecipe normalized;
			TerraEngine::Physics::ColliderRecipeValidationResult validation;
			const nlohmann::json document = nlohmann::json::parse(recipeBytes.begin(), recipeBytes.end());
			if(!TerraEngine::Physics::ColliderRecipe::LoadJson(document, parsed, validation)
				|| !parsed.TryNormalize(normalized, validation)) return false;
			const auto& envelope = existing->Envelope();
			return envelope.recipeHash == normalized.StableHash()
				&& envelope.sourceAssetId == normalized.provenance.sourceAssetId
				&& envelope.sourceContentHash == normalized.provenance.sourceContentHash
				&& envelope.settingsHash == normalized.provenance.settingsHash;
		}
		catch(const nlohmann::json::exception&)
		{
			return false;
		}
	};
	request.callbacks.validate = [context](const std::filesystem::path&, const std::vector<std::byte>& recipeBytes, std::vector<std::byte>& preparedRecipe, TerraEngine::AssetPipeline::PhysicsColliderCompileReport& report) {
		try
		{
			const nlohmann::json document = nlohmann::json::parse(recipeBytes.begin(), recipeBytes.end());
			TerraEngine::Physics::ColliderRecipe parsed;
			TerraEngine::Physics::ColliderRecipeValidationResult validation;
			if(!TerraEngine::Physics::ColliderRecipe::LoadJson(document, parsed, validation) || !parsed.TryNormalize(context->normalizedRecipe, validation))
			{
				AddDiagnostic(report, std::string(TerraEngine::Physics::ToString(validation.code)), validation.message);
				return false;
			}
			context->recipeDocumentFingerprint = Fingerprint(recipeBytes);
			preparedRecipe = recipeBytes;
			return true;
		}
		catch(const nlohmann::json::exception& exception)
		{
			AddDiagnostic(report, "invalid_recipe_json", exception.what());
			return false;
		}
	};
	request.callbacks.cook = [context](const std::vector<std::byte>&, const std::filesystem::path& stagedOutput, const std::atomic_bool& cancelled, TerraEngine::AssetPipeline::PhysicsColliderCompileReport& report) {
		if(cancelled.load(std::memory_order_acquire))
			return false;
		TerraEngine::Physics::Backend::PhysicsShapeRecipe shapeRecipe;
		if(!MapRecipe(context->normalizedRecipe, shapeRecipe))
		{
			AddDiagnostic(report, "recipe_mapping_failed", "The normalized collider recipe could not be mapped to a provider-neutral shape request.");
			return false;
		}

		ThreadPool& pool = ThreadPool::instance();
		const bool ownsPoolLifetime = pool.getThreadCount() == 0U;
		pool.initialize();
		TerraEngine::Physics::Backend::JoltPhysicsBackend backend(pool);
		const TerraEngine::Physics::Backend::PhysicsBackendResult initialized = backend.Initialize();
		if(!initialized.Succeeded())
		{
			AddDiagnostic(report, "backend_initialize_failed", initialized.message);
			if(ownsPoolLifetime) pool.shutdown();
			return false;
		}
		TerraEngine::Physics::Backend::PhysicsCookRequest cookRequest;
		cookRequest.recipe = std::move(shapeRecipe);
		cookRequest.correlationId = TerraEngine::ToHexString(context->assetId);
		const TerraEngine::Physics::Backend::PhysicsCookResult cooked = backend.CookShape(cookRequest);
		const TerraEngine::Physics::Backend::PhysicsBackendResult shutdown = backend.Shutdown();
		if(ownsPoolLifetime) pool.shutdown();
		if(cancelled.load(std::memory_order_acquire) || !cooked.result.Succeeded() || !shutdown.Succeeded())
		{
			AddDiagnostic(report, "jolt_cook_failed", cooked.result.message.empty() ? shutdown.message : cooked.result.message);
			return false;
		}
		if(cooked.formatFamily != TerraEngine::Physics::kJoltCookPayloadFormatFamily
			|| cooked.formatVersion != TerraEngine::Physics::kJoltCookPayloadFormatVersion
			|| cooked.provider != "Jolt"
			|| cooked.providerVersion != TerraEngine::Physics::kJoltCookProviderVersion
			|| cooked.providerRevision != TerraEngine::Physics::kJoltCookRevision)
		{
			AddDiagnostic(report, "provider_contract_mismatch", "The Jolt cook result did not match the approved 5.6.0 payload contract.");
			return false;
		}

		using namespace TerraEngine::Physics;
		CookedColliderBounds bounds;
		bool hasBounds = false;
		PhysicsPose identity;
		const std::uint64_t vertexCountWide = CountVerticesWide(context->normalizedRecipe);
		const std::uint64_t triangleCountWide = CountTrianglesWide(context->normalizedRecipe);
		const std::uint64_t partCountWide = CountPartsWide(context->normalizedRecipe);
		const std::uint64_t materialSlotCountWide = CountMaterialSlotsWide(context->normalizedRecipe);
		AccumulateBounds(context->normalizedRecipe, identity, bounds, hasBounds);
		if(!hasBounds || vertexCountWide == 0U
			|| vertexCountWide > std::numeric_limits<std::uint32_t>::max()
			|| triangleCountWide > std::numeric_limits<std::uint32_t>::max()
			|| partCountWide == 0U || partCountWide > std::numeric_limits<std::uint32_t>::max()
			|| materialSlotCountWide > std::numeric_limits<std::uint32_t>::max())
		{
			AddDiagnostic(report, "invalid_cooked_bounds", "The normalized recipe did not produce finite collider bounds.");
			return false;
		}
		CookedColliderEnvelope envelope;
		envelope.platform = CookedColliderAsset::HostPlatform();
		envelope.assetId = context->assetId;
		envelope.sourceAssetId = context->normalizedRecipe.provenance.sourceAssetId;
		envelope.sourceContentHash = context->normalizedRecipe.provenance.sourceContentHash;
		envelope.recipeHash = context->normalizedRecipe.StableHash();
		envelope.settingsHash = context->normalizedRecipe.provenance.settingsHash;
		envelope.vertexCount = static_cast<std::uint32_t>(vertexCountWide);
		envelope.triangleCount = static_cast<std::uint32_t>(triangleCountWide);
		envelope.partCount = static_cast<std::uint32_t>(partCountWide);
		envelope.materialSlotCount = static_cast<std::uint32_t>(materialSlotCountWide);
		envelope.bounds = bounds;
		envelope.provenanceSummary = "Jolt Physics 5.6.0 / e77f175 collider cook";
		CookedColliderDiagnostics diagnostics;
		const std::optional<CookedColliderAsset> asset = CookedColliderAsset::Create(envelope, cooked.payload, diagnostics);
		if(!asset)
		{
			for(const CookedColliderDiagnostic& diagnostic : diagnostics.entries)
				AddDiagnostic(report, diagnostic.code, diagnostic.message);
			return false;
		}
		const std::vector<std::byte> serialized = asset->Serialize();
		if(serialized.empty())
		{
			AddDiagnostic(report, "serialize_failed", "The validated cooked collider envelope did not serialize.");
			return false;
		}
		std::ofstream stream(stagedOutput, std::ios::binary | std::ios::trunc);
		if(!stream || !(stream.write(reinterpret_cast<const char*>(serialized.data()), static_cast<std::streamsize>(serialized.size()))))
		{
			AddDiagnostic(report, "write_stage_failed", "The cooked collider staging file could not be written.");
			return false;
		}
		return true;
	};
	request.callbacks.validateStagedOutput = [](const std::filesystem::path& stagedOutput, TerraEngine::AssetPipeline::PhysicsColliderCompileReport& report) {
		std::vector<std::byte> bytes;
		if(!ReadBytes(stagedOutput, bytes))
		{
			AddDiagnostic(report, "stage_read_failed", "The cooked collider staging file could not be read back.");
			return false;
		}
		TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
		const std::optional<TerraEngine::Physics::CookedColliderAsset> asset = TerraEngine::Physics::CookedColliderAsset::Deserialize(bytes, diagnostics);
		if(!asset)
		{
			for(const TerraEngine::Physics::CookedColliderDiagnostic& diagnostic : diagnostics.entries)
				AddDiagnostic(report, diagnostic.code, diagnostic.message);
			return false;
		}
		const auto compatibility = asset->CheckCompatibility(
			TerraEngine::Physics::CookedColliderAsset::HostPlatform(),
			TerraEngine::Physics::CookedColliderProviderFamily::Jolt,
			TerraEngine::Physics::kJoltCookProviderVersion,
			TerraEngine::Physics::kJoltCookRevision);
		if(!compatibility.compatible)
		{
			AddDiagnostic(report, std::string(TerraEngine::Physics::ToString(compatibility.code)), compatibility.message);
			return false;
		}
		return true;
	};
	request.callbacks.verifyPublicationInputs = [context, recipePath = commandLine.input, settingsPath = commandLine.settings](std::uint64_t sourceFingerprint, std::uint64_t settingsFingerprint, TerraEngine::AssetPipeline::PhysicsColliderCompileReport& report) {
		std::vector<std::byte> latestBytes;
		if(!ReadBytes(recipePath, latestBytes)
			|| Fingerprint(latestBytes) != context->recipeDocumentFingerprint
			|| Fingerprint(latestBytes) != sourceFingerprint)
		{
			AddDiagnostic(report, "recipe_changed", "The collider recipe changed while the worker was cooking.");
			return false;
		}
		std::vector<std::byte> latestSettings;
		if((!settingsPath.empty() && !ReadBytes(settingsPath, latestSettings))
			|| Fingerprint(latestSettings) != settingsFingerprint)
		{
			AddDiagnostic(report, "settings_changed", "Physics settings changed while the worker was cooking.");
			return false;
		}
		try
		{
			TerraEngine::Physics::ColliderRecipe latest;
			TerraEngine::Physics::ColliderRecipe normalized;
			TerraEngine::Physics::ColliderRecipeValidationResult validation;
			const nlohmann::json document = nlohmann::json::parse(latestBytes.begin(), latestBytes.end());
			return TerraEngine::Physics::ColliderRecipe::LoadJson(document, latest, validation)
				&& latest.TryNormalize(normalized, validation)
				&& normalized.provenance.sourceContentHash == context->normalizedRecipe.provenance.sourceContentHash
				&& normalized.provenance.settingsHash == context->normalizedRecipe.provenance.settingsHash;
		}
		catch(const nlohmann::json::exception& exception)
		{
			AddDiagnostic(report, "recipe_revalidation_failed", exception.what());
			return false;
		}
	};

	const TerraEngine::AssetPipeline::PhysicsColliderCompileReport report = TerraEngine::AssetPipeline::CompilePhysicsCollider(request);
	EmitReport(report);
	return report.published ? 0 : 1;
}
