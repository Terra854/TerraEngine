// Fixture bootstrap has separate worker IO preparation and protected cache
// observation. Neither phase starts an import/load or mutates the live scene.
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyFixtureBindings.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using Json = nlohmann::json;
		constexpr std::uintmax_t kMaximumInputBytes = 4 * 1024 * 1024;
		void Require(bool condition, std::string_view reason)
		{
			if(!condition) throw std::runtime_error(std::string(reason));
		}
		std::filesystem::path ContainedFile(const std::filesystem::path& root, const std::filesystem::path& relative)
		{
			Require(!relative.empty() && !relative.is_absolute() && !relative.has_root_name(), "Fixture binding path must be relative.");
			auto cursor = root;
			for(const auto& part : relative)
			{
				Require(part != ".." && part != ".", "Fixture binding path contains traversal.");
				cursor /= part;
				Require(!std::filesystem::is_symlink(std::filesystem::symlink_status(cursor)), "Fixture binding path redirects through a symlink: " + cursor.generic_string());
#ifdef _WIN32
				const auto attributes = GetFileAttributesW(cursor.c_str());
				Require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
					"Fixture binding path is missing or redirects through a junction/reparse point: " + cursor.generic_string());
#endif
			}
			const auto path = std::filesystem::canonical(cursor), canonicalRoot = std::filesystem::canonical(root);
			auto pathPart = path.begin();
			for(const auto& rootPart : canonicalRoot)
			{
				Require(pathPart != path.end(), "Fixture binding path escaped the project.");
				auto left = rootPart.native(), right = pathPart->native();
				std::transform(left.begin(), left.end(), left.begin(), [](auto value) { return std::towlower(value); });
				std::transform(right.begin(), right.end(), right.begin(), [](auto value) { return std::towlower(value); });
				Require(left == right, "Fixture binding path escaped the project."); ++pathPart;
			}
			Require(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) > 0
				&& std::filesystem::file_size(path) <= kMaximumInputBytes, "Fixture binding file is missing, empty, or oversized.");
			return path;
		}
		Json ReadJson(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary); Require(input.good(), "Fixture binding JSON cannot be read.");
			return Json::parse(input);
		}
		std::string SourceHash(const std::filesystem::path& path)
		{
			// Match the importer sourceHash contract exactly (FNV-1a, lowercase
			// unpadded hex); the separate ownership seal remains SHA-256.
			std::ifstream input(path, std::ios::binary); Require(input.good(), "Fixture source cannot be read.");
			std::uint64_t hash = 1469598103934665603ULL; std::array<char, 4096> buffer{};
			while(input)
			{
				input.read(buffer.data(), buffer.size());
				for(std::streamsize index = 0; index < input.gcount(); ++index) { hash ^= static_cast<unsigned char>(buffer[index]); hash *= 1099511628211ULL; }
			}
			Require(input.eof(), "Fixture source read was incomplete.");
			std::ostringstream text; text << std::hex << hash; return text.str();
		}
		UUID ReadId(const Json& value)
		{
			const auto id = ParseUUID(value.get<std::string>()); Require(id && *id != EMPTY_UUID, "Fixture binding GUID is invalid or empty."); return *id;
		}
		AssetType Type(std::string_view kind)
		{
			if(kind == "Mesh" || kind == "DeformableMesh") return AssetType::Mesh;
			if(kind == "Material") return AssetType::Material;
			if(kind == "Texture") return AssetType::Texture;
			if(kind == "Skeleton") return AssetType::Skeleton;
			if(kind == "AnimationClip") return AssetType::AnimationClip;
			throw std::runtime_error("Fixture binding kind is unsupported.");
		}
	}

	RendererEfficiencyImportObservation ClassifyRendererEfficiencyImportReceipt(const Json& receipt,
		const UUID& expectedPackage, std::string_view actualSourceHash)
	{
		RendererEfficiencyImportObservation result;
		try {
			Require(ReadId(receipt.at("packageGuid")) == expectedPackage, "Owned import package identity changed.");
			if(receipt.at("package").at("sourceHash").get<std::string>() != actualSourceHash) { result.reason = "Awaiting import receipt for current authored bytes."; return result; }
			const auto& generation = receipt.at("animationManifest").at("candidateGeneration");
			Require(generation.is_number_integer() && (generation.is_number_unsigned() || generation.get<std::int64_t>() > 0),
				"Owned import candidate generation is invalid.");
			result.generation = generation.get<std::uint64_t>();
			Require(result.generation != 0, "Owned import has no actual candidate generation.");
			const auto status = receipt.at("status").get<std::string>();
			bool textureFailure = false, otherFailure = false;
			for(const auto& diagnostic : receipt.at("diagnostics")) {
				const auto severity = diagnostic.at("severity").get<std::string>();
				if(diagnostic.at("code") == "TextureCompileFailed") textureFailure = true;
				else if(severity == "Error") otherFailure = true;
			}
			if(status == "Succeeded") {
				Require(!textureFailure && !otherFailure, "Owned import success contradicts its terminal cook diagnostics.");
				result.disposition = RendererEfficiencyImportDisposition::Ready; return result;
			}
			result.disposition = textureFailure && !otherFailure && (status == "SucceededWithWarnings" || status == "Failed")
				? RendererEfficiencyImportDisposition::RetryableCookFailure : RendererEfficiencyImportDisposition::Invalid;
			result.reason = textureFailure ? "Current owned texture cook failed (TextureCompileFailed)." : "Current owned import did not succeed.";
		} catch(const std::exception& exception) { result.disposition = RendererEfficiencyImportDisposition::Invalid; result.reason = exception.what(); }
		return result;
	}
	RendererEfficiencyImportDecision DecideRendererEfficiencyImportAdmission(const RendererEfficiencyImportObservation& observation,
		std::uint32_t admittedImports, std::uint64_t lastFailedGeneration)
	{
		// A physical retry can leave the preceding sidecar visible until its
		// new candidate settles. Never spend another attempt on those same bytes.
		if(observation.disposition == RendererEfficiencyImportDisposition::Invalid) return RendererEfficiencyImportDecision::Fail;
		if(observation.disposition == RendererEfficiencyImportDisposition::Pending || observation.generation <= lastFailedGeneration)
			return RendererEfficiencyImportDecision::Wait;
		if(observation.disposition == RendererEfficiencyImportDisposition::Ready) return RendererEfficiencyImportDecision::PrepareBindings;
		return admittedImports < 3 ? RendererEfficiencyImportDecision::Reimport : RendererEfficiencyImportDecision::Fail;
	}
	RendererEfficiencyImportObservation InspectRendererEfficiencyOwnedImport(const std::filesystem::path& root,
		const std::filesystem::path& source, const UUID& package, std::string_view expectedSha256)
	{
		RendererEfficiencyImportObservation result; std::string hash, error;
		if(!HashRendererEfficiencyOwnedInput(root, source, hash, error) || hash != expectedSha256)
		{ result.disposition = RendererEfficiencyImportDisposition::Invalid; result.reason = error.empty() ? "Owned authored source SHA256 changed." : error; return result; }
		try {
			const auto metadata = ReadJson(ContainedFile(root, source.generic_string() + ".meta"));
			Require(ReadId(metadata.at("guid")) == package, "Owned source metadata package identity changed.");
			const auto receiptPath = root / (source.generic_string() + ".import.json");
			if(!std::filesystem::exists(receiptPath)) { result.reason = "Awaiting owned import receipt publication."; return result; }
			return ClassifyRendererEfficiencyImportReceipt(ReadJson(ContainedFile(root, source.generic_string() + ".import.json")), package, SourceHash(ContainedFile(root, source)));
		} catch(const std::exception& exception) { result.disposition = RendererEfficiencyImportDisposition::Invalid; result.reason = exception.what(); return result; }
	}
	bool PrepareRendererEfficiencyFixtureBindings(const std::filesystem::path& projectRoot,
		const Json& sealedSteps, RendererEfficiencyFixtureBindingPreparation& output, std::string& error)
	{
		try
		{
			Require(sealedSteps.at("schemaVersion") == 1, "Fixture binding schema is unsupported.");
			const auto root = std::filesystem::canonical(projectRoot);
			RendererEfficiencyFixtureBindingPreparation prepared; prepared.projectRoot = root;
			std::set<std::string> keys;
			Require(sealedSteps.at("assets").size() <= 64, "Fixture binding asset count exceeds the bounded recipe.");
			for(const auto& asset : sealedSteps.at("assets"))
			{
				const auto sourcePath = std::filesystem::path(asset.at("sourcePath").get<std::string>());
				const auto source = ContainedFile(root, std::filesystem::path("Assets") / sourcePath);
				const auto metadata = ReadJson(ContainedFile(root, std::filesystem::path("Assets") / (sourcePath.generic_string() + ".meta")));
				const auto receipt = ReadJson(ContainedFile(root, std::filesystem::path("Assets") / (sourcePath.generic_string() + ".import.json")));
				const UUID package = ReadId(asset.at("packageGuid"));
				Require(ReadId(metadata.at("guid")) == package && ReadId(receipt.at("packageGuid")) == package, "Imported package identity differs from the sealed fixture.");
				Require(receipt.at("status") == "Succeeded", "Fixture import has not succeeded.");
				Require(receipt.at("package").at("sourceHash") == SourceHash(source), "Fixture import receipt describes stale source bytes.");
				// Static import results also serialize an empty animation manifest.
				// Require atomic animation publication only when this recipe actually
				// asks for a skeleton, clip, or deformable child.
				const auto& requiredChildren = asset.at("requiredSubAssets");
				const bool animated = std::any_of(requiredChildren.begin(), requiredChildren.end(), [](const Json& child) {
					const auto kind = child.at("kind").get<std::string>(); return kind == "Skeleton" || kind == "AnimationClip" || kind == "DeformableMesh";
				});
				if(animated)
				{
					const auto& manifest = receipt.at("animationManifest");
					Require(manifest.at("publishedAtomically") == true && manifest.value("preservedLastKnownGood", false) == false
						&& manifest.value("cancelled", false) == false && manifest.at("publishedGeneration").get<std::uint64_t>() > 0
						&& manifest.at("candidateGeneration") == manifest.at("publishedGeneration") && ReadId(manifest.at("packageGuid")) == package,
						"Animated fixture package lacks its complete current atomic publication.");
				}
				for(const auto& required : asset.at("requiredSubAssets"))
				{
					RendererEfficiencyFixtureBindingCandidate candidate;
					auto& binding = candidate.binding;
					binding.bindingKey = required.at("bindingKey"); binding.kind = required.at("kind"); binding.packageGuid = package; binding.sourcePath = source;
					Require(keys.insert(binding.bindingKey).second, "Fixture binding keys are not unique.");
					const auto type = Type(binding.kind);
					const Json* found = nullptr;
					const auto consider = [&](const Json& record) {
						if(record.value("name", "") == required.at("name") && (record.value("kind", binding.kind) == binding.kind))
						{
							// Native import results can list the same texture once for
							// each material dependency. Only an exactly identical record
							// is one publication; differing identities or metadata remain
							// ambiguous and cannot be resolved by picking a convenient row.
							Require(found == nullptr || *found == record, "Imported named fixture binding is ambiguous.");
							if(found == nullptr) found = &record;
						}
					};
					if(IsAnimationAssetType(type) || binding.kind == "DeformableMesh") for(const auto& record : receipt.at("animationManifest").at("subAssets")) consider(record);
					else
					{
						const auto collection = type == AssetType::Mesh ? "generatedMeshes" : type == AssetType::Texture ? "generatedTextures" : "generatedMaterials";
						Require(receipt.contains(collection), "Imported fixture child collection is missing.");
						for(const auto& record : receipt.at(collection)) consider(record);
					}
					Require(found != nullptr, "Required named fixture child is not published by the importer.");
					binding.assetId = ReadId(found->at("guid"));
					binding.cookedPath = ContainedFile(root, std::filesystem::path("Build/Assets") / found->at("compiledPath").get<std::string>());
					if(type == AssetType::Mesh)
					{
						candidate.minimumLods = asset.at("requiredCook").at("minimumLods"); candidate.minimumMeshlets = asset.at("requiredCook").at("minimumMeshlets");
					}
					prepared.candidates.push_back(std::move(candidate));
				}
			}
			Require(!prepared.candidates.empty() && prepared.candidates.size() <= 256, "Fixture has no bindings or exceeds the bounded child count.");
			output = std::move(prepared); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}

	bool ResolveRendererEfficiencyFixtureBindings(const RendererEfficiencyFixtureBindingPreparation& preparation,
		ResourceManager& resources, std::vector<RendererEfficiencyPublishedBinding>& output, std::string& error)
	{
		try
		{
			std::vector<RendererEfficiencyPublishedBinding> resolved;
			Require(!preparation.candidates.empty(), "Fixture bindings have not been prepared.");
			const auto rendererGeneration = resources.GetRendererResourcePublicationGeneration();
			const auto animationGeneration = resources.GetAnimationBindingResourceRevision();
			for(const auto& candidate : preparation.candidates)
			{
				auto binding = candidate.binding; const auto type = Type(binding.kind);
				const auto snapshot = resources.TryCopyRendererEfficiencyResourcePublication(binding.assetId, type);
				// A completed UUID future is not current payload ownership. Keep the
				// readiness predicate strict while identifying the exact child and
				// separating a busy supported cache from an absent publication.
				if (!snapshot || !snapshot->present || snapshot->resourceGeneration == 0)
				{
					std::ostringstream reason;
					reason << "Fixture child publication unavailable: bindingKey=" << binding.bindingKey
						<< " assetId=" << ToHexString(binding.assetId) << " kind=" << binding.kind;
					if (!snapshot) reason << " state=WriterBusy";
					else reason << " state=Unpublished present=" << (snapshot->present ? "true" : "false")
						<< " generation=" << snapshot->resourceGeneration
						<< " registeredPath=" << snapshot->registeredPath.generic_string()
						<< " deformablePayloadGeneration=" << snapshot->deformablePayloadGeneration;
					throw std::runtime_error(reason.str());
				}
				Require(snapshot->assetId == binding.assetId && snapshot->type == type, "Fixture publication identity or type differs from its imported child.");
				if(binding.kind == "DeformableMesh") Require(snapshot->deformablePayloadGeneration > 0, "Animated fixture mesh has no published deformable payload.");
				const auto& registered = snapshot->registeredPath;
				// ResourceManager normalizes Windows registry keys to lowercase;
				// preserve containment validation while comparing that spelling fairly.
				const auto pathKey = [](const std::filesystem::path& path) {
					auto key = path.lexically_normal().generic_wstring();
					std::transform(key.begin(), key.end(), key.begin(), [](auto value) { return std::towlower(value); }); return key;
				};
				Require(!registered.empty() && pathKey(registered) == pathKey(binding.cookedPath), "Resource registration does not match the actual contained cooked child.");
				binding.resourceGeneration = snapshot->resourceGeneration; binding.cookedLods = snapshot->cookedLods; binding.cookedMeshlets = snapshot->cookedMeshlets;
				Require(binding.cookedLods >= candidate.minimumLods && binding.cookedMeshlets >= candidate.minimumMeshlets,
					"Actual published fixture geometry lacks the required cooked LODs or meshlets.");
				resolved.push_back(std::move(binding));
			}
			// A concurrent package publication must not combine earlier old children
			// with later new children into one accepted bootstrap cohort.
			Require(rendererGeneration == resources.GetRendererResourcePublicationGeneration()
				&& animationGeneration == resources.GetAnimationBindingResourceRevision(), "Fixture resource cohort changed during publication observation.");
			output = std::move(resolved); error.clear(); return true;
		}
		catch(const std::exception& exception) { error = exception.what(); return false; }
	}
}
