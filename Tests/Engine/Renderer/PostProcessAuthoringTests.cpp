// Pure authoring regressions: no scene mutation, renderer, or GPU execution.
#include "TestHarness.h"
#include "Renderer/GraphicsSettings/PostProcessProfileResolver.h"
#include "Resource/ResourceManager.h"
#include "Resource/PostProcessEffectAsset.h"
#include "Runtime/CorrectnessTestSupport.h"
#include "ThreadPool/ThreadPool.h"
#include <algorithm>
#include <future>
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <limits>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::GraphicsSettings;
		UUID Id(std::uint8_t byte) { UUID id{}; id[0] = byte; return id; }
		GraphicsSettingDescriptor Scalar(GraphicsSettingId id, float value)
		{
			GraphicsSettingDescriptor descriptor;
			descriptor.id = id; descriptor.valueKind = GraphicsValueKind::Scalar;
			descriptor.defaultValue = value; descriptor.minimum = -100.0f; descriptor.maximum = 100.0f;
			descriptor.cameraScope = true; descriptor.available = true;
			return descriptor;
		}
		// Match existing resource-lifecycle fixtures: no renderer/editor starts.
		class ProfileResourceSession final
		{
		public:
			ProfileResourceSession()
			{
				ThreadPool::instance().shutdown();
				ResourceManager::Instance().BeginRequestSession();
				ResourceManager::Instance().ClearCaches();
				ThreadPool::instance().initialize(1u);
			}
			~ProfileResourceSession()
			{
				ResourceManager::Instance().SetRequestTestHooks({});
				ThreadPool::instance().shutdown();
				ResourceManager::Instance().ClearCaches();
				ResourceManager::Instance().BeginRequestSession();
			}
		};
		void WriteProfile(TestContext& context, const std::filesystem::path& path, const UUID& id, float marker)
		{
			Resource::PostProcessProfile profile; profile.assetId = id; profile.revision = 1u;
			profile.values = {{GraphicsSettingId::FixedExposureCompensation, marker}};
			profile.overrideMask = {GraphicsSettingId::FixedExposureCompensation};
			std::vector<std::byte> bytes; std::string detail;
			context.Expect(Resource::SerializePostProcessProfile(profile, bytes, detail), "Resource fixture uses the actual profile encoder.");
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			context.Expect(bool(file), "Profile fixture bytes reach their requested path.");
		}
		void WriteStageContainer(const std::filesystem::path& path, ShaderStage stage, std::uint32_t marker)
		{
			// Match the existing shader-publication fixtures: parser-valid TSPF
			// metadata and synthetic payload test CPU ownership only, never GPU use.
			const std::string source = "Engine/Shaders/PostProcess/Validation/Publication.frag.hlsl";
			ShaderHeader header{}; std::memcpy(header.magic, &kShaderMagicValue, sizeof(kShaderMagicValue));
			header.version = kShaderCurrentVersion; header.headerSize = static_cast<std::uint16_t>(sizeof(header));
			header.reflectionOffset = sizeof(header); header.compiledBinaryOffset = header.reflectionOffset + sizeof(ShaderReflection);
			header.stringTableOffset = header.compiledBinaryOffset + sizeof(CompiledStageBinary);
			header.stringTableSize = static_cast<std::uint32_t>(source.size() + 1u); header.compiledBinarySize = 5u * sizeof(std::uint32_t);
			ShaderReflection reflection{}; reflection.source.stage = static_cast<std::uint32_t>(stage);
			reflection.source.entryPointOffset = kInvalidStringOffset; reflection.source.targetProfileOffset = kInvalidStringOffset;
			CompiledStageBinary binary{}; binary.format = static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV);
			binary.entryPointOffset = kInvalidStringOffset; binary.targetProfileOffset = kInvalidStringOffset;
			binary.bytecodeOffset = header.stringTableOffset + header.stringTableSize; binary.bytecodeSize = header.compiledBinarySize;
			const std::array<std::uint32_t, 5> code{0x07230203u, 0x00010000u, marker, 1u, 0u};
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(&header), sizeof(header)); file.write(reinterpret_cast<const char*>(&reflection), sizeof(reflection));
			file.write(reinterpret_cast<const char*>(&binary), sizeof(binary)); file.write(source.c_str(), static_cast<std::streamsize>(source.size() + 1u));
			file.write(reinterpret_cast<const char*>(code.data()), sizeof(code));
			if(!file) throw std::runtime_error("Could not write stage ownership fixture.");
		}
		float ProfileMarker(const Resource::PostProcessProfile& profile)
		{
			return std::get<float>(profile.values.front().value);
		}
		PostProcessProfileResolutionInput Input()
		{
			PostProcessProfileResolutionInput input;
			input.target.scope = GraphicsScope::Camera;
			input.target.camera = GraphicsCameraTarget{Id(1), 1, 1, 1, 1};
			input.target.view = GraphicsViewIdentity{static_cast<RenderViewType>(1), 1, 1, static_cast<RenderOutputKind>(1)};
			input.lifecycle = {1, 1, 1, 1}; input.baseRevision = 9;
			input.descriptors.push_back(Scalar(GraphicsSettingId::FixedExposureCompensation, 0.0f));
			GraphicsSettingDescriptor bloom;
			bloom.id = GraphicsSettingId::BloomEnabled; bloom.valueKind = GraphicsValueKind::Boolean;
			bloom.defaultValue = false; bloom.cameraScope = true; bloom.available = true;
			input.descriptors.push_back(bloom);
			return input;
		}
		std::shared_ptr<Resource::PostProcessProfile> Profile(std::vector<GraphicsSettingValue> values)
		{
			auto profile = std::make_shared<Resource::PostProcessProfile>();
			profile->assetId = Id(2); profile->revision = 1; profile->values = std::move(values);
			for (const auto& field : profile->values) profile->overrideMask.push_back(field.id);
			return profile;
		}
		const GraphicsValue& Value(const PostProcessResolvedProfile& output, GraphicsSettingId id)
		{
			return std::find_if(output.settings.values.begin(), output.settings.values.end(), [id](const auto& field) { return field.id == id; })->value;
		}
		PostProcessVolumeInput Volume(std::uint8_t key, std::int32_t priority, float weight, float exposure, bool bloom)
		{
			PostProcessVolumeInput volume;
			volume.stableId = Id(key); volume.priority = priority; volume.weight = weight; volume.global = true;
			volume.profile = Profile({{GraphicsSettingId::FixedExposureCompensation, exposure}, {GraphicsSettingId::BloomEnabled, bloom}});
			return volume;
		}
	}

	void RegisterPostProcessAuthoringTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.Authoring.Resource.EffectIdentityAndFailedReloadKeepOwner", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); Resource::PostProcessEffectAsset effect;
			effect.descriptor.assetId = GenerateUUID(); effect.descriptor.shaderAssetId = GenerateUUID();
			effect.descriptor.revision = 1u; effect.descriptor.stableId = "fixture-effect";
			std::vector<std::byte> bytes; std::string detail;
			context.Expect(Resource::SerializePostProcessEffectAsset(effect, bytes, detail), "Actual versioned effect envelope encodes.");
			const auto path = fixture.Child("owned.teposteffect");
			{ std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }
			const auto id = effect.descriptor.assetId;
			context.Expect(resources.LoadPostProcessEffect(path) == id, "Effect identity derives from its checked descriptor envelope.");
			const auto owner = resources.AcquirePostProcessEffectSnapshot(id);
			context.Expect(owner && owner->descriptor.assetId == id && resources.LoadPostProcessEffectAsync(id).get() == id, "Typed UUID async requests retain the accepted effect root.");
			{ std::ofstream file(path, std::ios::trunc); file << "broken effect"; }
			context.Expect(resources.ReloadPostProcessEffectAsync(path).get() == EMPTY_UUID && resources.AcquirePostProcessEffectSnapshot(id) == owner,
				"Failed effect reload preserves the complete prior root.");
			resources.DeleteResource(id, AssetType::PostProcessEffect);
			context.Expect(!resources.AcquirePostProcessEffectSnapshot(id) && owner && owner->descriptor.assetId == id, "Deleting an effect withdraws lookup while an immutable lease remains alive.");
		});
		registry.Add("Renderer.PostProcess.Authoring.Resource.StageRequiresTypedIdentityAndGenuinePublication", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); const auto id = GenerateUUID(); const auto path = fixture.Child("fullscreen.tspf");
			WriteStageContainer(path, ShaderStage::Fragment, 1u);
			context.Expect(resources.LoadPostProcessShaderStageAsync(id).get() == EMPTY_UUID, "Unknown stage IDs cannot generate filename-derived identities.");
			resources.RegisterAssetPath(id, path, AssetType::ShaderCollection);
			context.Expect(resources.LoadPostProcessShaderStageAsync(id).get() == EMPTY_UUID, "A collection registration cannot masquerade as an individual stage.");
			resources.RegisterAssetPath(id, path, AssetType::PostProcessShaderStage);
			context.Expect(resources.LoadPostProcessShaderStage(id) == id, "Explicit typed stage metadata reaches ordinary shader-cache publication.");
			const auto owner = resources.AcquirePostProcessShaderStageSnapshot(id);
			context.Expect(owner && owner->publicationGeneration > 0u && resources.AcquireShaderSnapshot(id) == owner,
				"Stage readiness retains the actual shader-cache owner with a genuine accepted generation.");
			WriteStageContainer(path, ShaderStage::Vertex, 2u);
			context.Expect(resources.ReloadPostProcessShaderStageAsync(id).get() == EMPTY_UUID && resources.AcquirePostProcessShaderStageSnapshot(id) == owner,
				"Wrong-stage reload rejects without replacing the prior fragment owner.");
			resources.ClearCaches(); context.Expect(!resources.AcquirePostProcessShaderStageSnapshot(id) && owner && owner->publicationGeneration > 0u,
				"Cache clear withdraws typed registration without freeing a retained shader lease.");
		});
		registry.Add("Renderer.PostProcess.Authoring.Resource.LooseIdentityAndRegisteredAsyncOwner", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); const auto id = GenerateUUID(); const auto path = fixture.Child("owned.tepostprofile");
			WriteProfile(context, path, id, 2.0f);
			context.Expect(resources.LoadPostProcessProfile(path) == id, "Unregistered profile paths derive the checked embedded UUID.");
			const auto owner = resources.AcquirePostProcessProfileSnapshot(id);
			context.Expect(owner && ProfileMarker(*owner) == 2.0f, "A no-IO immutable lease exposes accepted bytes.");
			context.Expect(resources.LoadPostProcessProfileAsync(id).get() == id, "Registered UUID requests use the real asynchronous cache route.");
			context.Expect(resources.AcquirePostProcessProfileSnapshot(id) == owner, "Warm requests retain the accepted immutable owner.");
		});
		registry.Add("Renderer.PostProcess.Authoring.Resource.InvalidReloadRetainsLastUsableOwner", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); const auto id = GenerateUUID(); const auto path = fixture.Child("reload.tepostprofile");
			WriteProfile(context, path, id, 3.0f); context.Expect(resources.LoadPostProcessProfile(path) == id, "Initial profile publication succeeds.");
			const auto retained = resources.AcquirePostProcessProfileSnapshot(id);
			{ std::ofstream file(path, std::ios::trunc); file << "invalid profile bytes"; }
			context.Expect(resources.ReloadPostProcessProfileAsync(path).get() == EMPTY_UUID, "Malformed reload reports failure.");
			context.Expect(retained && resources.AcquirePostProcessProfileSnapshot(id) == retained && ProfileMarker(*retained) == 3.0f, "Failure leaves the prior owner and retained bytes intact.");
		});
		registry.Add("Renderer.PostProcess.Authoring.Resource.PathRemapCannotBorrowSameRevisionBytes", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); const auto id = GenerateUUID();
			const auto first = fixture.Child("first.tepostprofile"), second = fixture.Child("second.tepostprofile");
			WriteProfile(context, first, id, 1.0f); WriteProfile(context, second, id, 2.0f);
			context.Expect(resources.LoadPostProcessProfile(first) == id, "First path publishes.");
			const auto firstOwner = resources.AcquirePostProcessProfileSnapshot(id);
			resources.RegisterAssetPath(id, second, AssetType::PostProcessProfile);
			context.Expect(!resources.AcquirePostProcessProfileSnapshot(id), "New registration cannot lend the old path's bytes.");
			context.Expect(resources.LoadPostProcessProfileAsync(id).get() == id, "Second path publishes its actual bytes.");
			const auto secondOwner = resources.AcquirePostProcessProfileSnapshot(id);
			context.Expect(firstOwner && secondOwner && secondOwner != firstOwner && ProfileMarker(*secondOwner) == 2.0f && ProfileMarker(*firstOwner) == 1.0f,
				"Same authored revision still retains two distinct immutable payload owners.");
			resources.RegisterAssetPath(id, first, AssetType::PostProcessProfile);
			context.Expect(!resources.AcquirePostProcessProfileSnapshot(id), "Returning to a prior path cannot borrow a different owner's equal revision.");
			context.Expect(resources.LoadPostProcessProfileAsync(id).get() == id, "The remapped path is accepted again.");
			const auto reread = resources.AcquirePostProcessProfileSnapshot(id);
			context.Expect(reread && ProfileMarker(*reread) == 1.0f, "The remapped path must expose its own bytes.");
		});
		registry.Add("Renderer.PostProcess.Authoring.Resource.DeleteAndClearKeepRetainedLeases", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); const auto id = GenerateUUID(); const auto path = fixture.Child("retained.tepostprofile");
			WriteProfile(context, path, id, 4.0f); context.Expect(resources.LoadPostProcessProfile(path) == id, "Initial profile publishes.");
			const auto deletedLease = resources.AcquirePostProcessProfileSnapshot(id);
			resources.DeleteResource(id, AssetType::PostProcessProfile);
			context.Expect(!resources.AcquirePostProcessProfileSnapshot(id) && deletedLease && ProfileMarker(*deletedLease) == 4.0f, "Delete withdraws lookup but preserves an acquired lease.");
			context.Expect(resources.LoadPostProcessProfile(path) == id, "Deleted paths can be admitted fresh.");
			const auto clearedLease = resources.AcquirePostProcessProfileSnapshot(id); resources.ClearCaches();
			context.Expect(!resources.AcquirePostProcessProfileSnapshot(id) && clearedLease && ProfileMarker(*clearedLease) == 4.0f, "Clear invalidates current cache ownership without freeing retained input roots.");
		});
		registry.Add("Renderer.PostProcess.Authoring.Resource.EpochRejectsLatePhysicalPublication", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); const auto id = GenerateUUID(); const auto path = fixture.Child("epoch.tepostprofile");
			WriteProfile(context, path, id, 5.0f); Correctness::ControlledGate gate;
			ResourceManager::RequestTestHooks hooks; const auto waiter = gate.MakeWaiter();
			hooks.beforePublication = [waiter](const std::string&) { (void)waiter(); }; resources.SetRequestTestHooks(std::move(hooks));
			auto pending = resources.LoadPostProcessProfileAsync(path);
			context.Expect(gate.WaitForArrivals(1u), "A real decoded profile stops before protected publication.");
			resources.ClearCaches(); resources.SetRequestTestHooks({}); gate.Release();
			context.Expect(pending.get() == EMPTY_UUID && !resources.AcquirePostProcessProfileSnapshot(id), "An old physical completion cannot publish after cache epoch replacement.");
			context.Expect(resources.LoadPostProcessProfile(path) == id, "Fresh current-epoch work remains usable.");
		});
		registry.Add("Renderer.PostProcess.Authoring.Resource.NewAliasCannotBorrowAnExistingEmbeddedIdentity", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); const auto embedded = GenerateUUID(), alias = GenerateUUID(); const auto path = fixture.Child("alias.tepostprofile");
			WriteProfile(context, path, embedded, 7.0f); context.Expect(resources.LoadPostProcessProfile(path) == embedded, "Original embedded identity publishes.");
			const auto retained = resources.AcquirePostProcessProfileSnapshot(embedded);
			resources.RegisterAssetPath(alias, path, AssetType::PostProcessProfile);
			context.Expect(!resources.AcquirePostProcessProfileSnapshot(alias) && !resources.AcquirePostProcessProfileSnapshot(embedded),
				"Changing the path identity cannot expose previously accepted bytes as either current registration.");
			context.Expect(resources.LoadPostProcessProfileAsync(alias).get() == EMPTY_UUID && retained && ProfileMarker(*retained) == 7.0f,
				"Wrong alias load rejects without damaging the explicit old immutable lease.");
		});
		registry.Add("Renderer.PostProcess.Authoring.Resource.RegisteredIdentityMismatchRejects", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; ProfileResourceSession session;
			auto& resources = ResourceManager::Instance(); const auto actual = GenerateUUID(), registered = GenerateUUID(); const auto path = fixture.Child("wrong-id.tepostprofile");
			WriteProfile(context, path, actual, 6.0f); resources.RegisterAssetPath(registered, path, AssetType::PostProcessProfile);
			context.Expect(resources.LoadPostProcessProfileAsync(registered).get() == EMPTY_UUID, "A registered UUID cannot relabel another embedded profile ID.");
			context.Expect(!resources.AcquirePostProcessProfileSnapshot(registered) && !resources.AcquirePostProcessProfileSnapshot(actual), "Rejected identity input has no partial cache publication.");
		});
		registry.Add("Renderer.PostProcess.Authoring.ProfileBytesRoundTripAllTypedValues", [](TestContext& context) {
			auto profile = Profile({{GraphicsSettingId::BloomEnabled, true}, {GraphicsSettingId::AntiAliasingMode, std::uint32_t{3}},
				{GraphicsSettingId::VirtualGeometryBudgetBytes, std::numeric_limits<std::uint64_t>::max()},
				{GraphicsSettingId::FixedExposureCompensation, -1.375f},
				{GraphicsSettingId::ColorCurve, Id(7)}, {static_cast<GraphicsSettingId>(1001), std::string("copied authored string")}});
			profile->name = "Roundtrip"; profile->revision = std::numeric_limits<std::uint64_t>::max();
			profile->customEffects = {Id(9), Id(8)}; profile->overrideCustomEffects = true;
			std::vector<std::byte> bytes; std::string detail;
			context.Expect(Resource::SerializePostProcessProfile(*profile, bytes, detail), "All owned value kinds encode.");
			const auto decoded = Resource::DeserializePostProcessProfile(bytes, profile->assetId);
			context.Expect(decoded.profile && decoded.reason == GraphicsRequestReason::None, "Project/package byte decoding returns one immutable root.");
			context.Expect(decoded.profile->revision == profile->revision && decoded.profile->customEffects == profile->customEffects
				&& decoded.profile->overrideCustomEffects, "Full-width revisions and ordered UUID references survive.");
			for (const auto& field : profile->values) {
				const auto found = std::find_if(decoded.profile->values.begin(), decoded.profile->values.end(), [&](const auto& item) { return item.id == field.id; });
				context.Expect(found != decoded.profile->values.end() && found->value == field.value, "Typed values preserve all bits and variant tags.");
			}
			std::vector<std::byte> again;
			context.Expect(Resource::SerializePostProcessProfile(*decoded.profile, again, detail) && again == bytes, "Canonical serialization is byte-stable.");
			std::reverse(profile->values.begin(), profile->values.end()); std::reverse(profile->overrideMask.begin(), profile->overrideMask.end());
			context.Expect(Resource::SerializePostProcessProfile(*profile, again, detail) && again == bytes, "Field and mask iteration order cannot change package bytes.");
			context.Expect(!Resource::DeserializePostProcessProfile(bytes, Id(99)).profile, "Package identity mismatch rejects instead of relabeling bytes.");
		});
		registry.Add("Renderer.PostProcess.Authoring.ProfileBytesMalformedEnvelopesRejectAtomically", [](TestContext& context) {
			auto profile = Profile({{GraphicsSettingId::BloomEnabled, true}}); std::vector<std::byte> encoded; std::string detail;
			context.Expect(Resource::SerializePostProcessProfile(*profile, encoded, detail), "Valid fixture encodes.");
			const std::string text(reinterpret_cast<const char*>(encoded.data()), encoded.size());
			const auto original = nlohmann::json::parse(text);
			for (int failure = 0; failure < 13; ++failure) {
				auto json = original;
				if (failure == 0) json["schemaVersion"] = 2u;
				if (failure == 1) json["assetType"] = "Wrong.Asset";
				if (failure == 2) json["assetId"] = "not-a-guid";
				if (failure == 3) json["revision"] = "18446744073709551616";
				if (failure == 4) json["revision"] = 1u;
				if (failure == 5) json["values"][0]["kind"] = "unknown-kind";
				if (failure == 6) json["values"][0]["value"] = 1u;
				if (failure == 7) json["overrideMask"].push_back(static_cast<std::uint32_t>(GraphicsSettingId::BloomEnabled));
				if (failure == 8) json["values"] = nlohmann::json::array();
				if (failure == 9) json["customEffects"] = {ToHexString(Id(4)), ToHexString(Id(4))};
				if (failure == 10) json["unknownMember"] = true;
				if (failure == 11) json.erase("overrideCustomEffects");
				if (failure == 12) json["values"][0]["id"] = -1;
				const auto malformed = json.dump(); std::vector<std::byte> bytes(malformed.size()); std::memcpy(bytes.data(), malformed.data(), malformed.size());
				context.Expect(!Resource::DeserializePostProcessProfile(bytes).profile, "Malformed/version/type/mask/reference inputs return no asset root.");
			}
			for (const std::string malformed : {text + " trailing", text.substr(0, text.size() - 1), std::string("{\"assetType\":\"a\",\"assetType\":\"b\"}")}) {
				std::vector<std::byte> bytes(malformed.size()); std::memcpy(bytes.data(), malformed.data(), malformed.size());
				context.Expect(!Resource::DeserializePostProcessProfile(bytes).profile, "Trailing/truncated/duplicate-key JSON rejects.");
			}
			std::vector<std::byte> oversized(Resource::kMaximumPostProcessProfileBytes + 1u);
			context.Expect(!Resource::DeserializePostProcessProfile(oversized).profile, "Oversized inputs reject before JSON allocation.");
			const auto before = encoded; profile->values[0].value = std::numeric_limits<float>::infinity();
			context.Expect(!Resource::SerializePostProcessProfile(*profile, encoded, detail) && encoded == before, "Failed serialization preserves all previous bytes.");
		});
		registry.Add("Renderer.PostProcess.Authoring.ProfileBytesSparseLegacyAndBounds", [](TestContext& context) {
			nlohmann::json json = {{"assetType", "TerraEngine.PostProcessProfile"}, {"schemaVersion", 0u}, {"assetId", ToHexString(Id(2))}, {"revision", "1"}};
			auto text = json.dump(); std::vector<std::byte> bytes(text.size()); std::memcpy(bytes.data(), text.data(), text.size());
			auto result = Resource::DeserializePostProcessProfile(bytes);
			context.Expect(result.profile && result.profile->values.empty() && result.profile->overrideMask.empty()
				&& !result.profile->overrideCustomEffects, "Sparse version0 assets retain inherited legacy defaults.");
			json["values"] = {{{"id", 31u}, {"kind", "unsigned32"}, {"value", std::uint64_t{4294967296ull}}}};
			text = json.dump(); bytes.resize(text.size()); std::memcpy(bytes.data(), text.data(), text.size());
			context.Expect(!Resource::DeserializePostProcessProfile(bytes).profile, "Unsigned32 overflow is not truncated.");
			json["values"][0] = {{"id", 65u}, {"kind", "scalar"}, {"value", 1.0e200}};
			text = json.dump(); bytes.resize(text.size()); std::memcpy(bytes.data(), text.data(), text.size());
			context.Expect(!Resource::DeserializePostProcessProfile(bytes).profile, "Float32 overflow is not converted to infinity.");
			json["values"][0] = {{"id", 24u}, {"kind", "unsigned64"}, {"value", "01"}};
			text = json.dump(); bytes.resize(text.size()); std::memcpy(bytes.data(), text.data(), text.size());
			context.Expect(!Resource::DeserializePostProcessProfile(bytes).profile, "Ambiguous noncanonical unsigned strings reject.");
		});
		registry.Add("Renderer.PostProcess.Authoring.LegacyDefaultsAndExplicitMasks", [](TestContext& context) {
			auto input = Input(); auto profile = Profile({{GraphicsSettingId::FixedExposureCompensation, 7.0f}});
			profile->schemaVersion = 0; profile->overrideMask.clear(); input.sceneProfile = profile;
			auto result = ResolvePostProcessProfile(input);
			context.Expect(result.candidate.has_value(), "Old sparse profiles resolve through current defaults.");
			context.Expect(std::get<float>(Value(*result.candidate, GraphicsSettingId::FixedExposureCompensation)) == 0.0f, "Unmasked stored values inherit.");
			context.Expect(!std::get<bool>(Value(*result.candidate, GraphicsSettingId::BloomEnabled)), "Absent effects stay disabled.");
			context.Expect(result.candidate->settings.publishedFrameId == 0 && result.candidate->settings.postProcessResourceGeneration == 0, "Prepared authored values have no execution proof.");
		});
		registry.Add("Renderer.PostProcess.Authoring.FullLayerPrecedenceAndPlayerDisable", [](TestContext& context) {
			auto input = Input();
			input.engineDefaults = {{GraphicsSettingId::FixedExposureCompensation, 1.0f}};
			input.projectDefaults = {{GraphicsSettingId::FixedExposureCompensation, 2.0f}};
			input.qualityPreset = {{GraphicsSettingId::FixedExposureCompensation, 3.0f}};
			input.sceneProfile = Profile({{GraphicsSettingId::FixedExposureCompensation, 4.0f}});
			input.volumes = {Volume(3, 0, 1.0f, 5.0f, true)};
			input.cameraProfile = Profile({{GraphicsSettingId::FixedExposureCompensation, 6.0f}});
			input.cameraOverrides = {{GraphicsSettingId::FixedExposureCompensation, 7.0f}};
			input.runtimeCameraOverrides = {{GraphicsSettingId::FixedExposureCompensation, 8.0f}};
			input.playerAllowedSettings = {GraphicsSettingId::FixedExposureCompensation, GraphicsSettingId::BloomEnabled};
			input.playerOverrides = {{GraphicsSettingId::FixedExposureCompensation, 9.0f}, {GraphicsSettingId::BloomEnabled, true}};
			input.playerDisabledSettings = {GraphicsSettingId::BloomEnabled};
			auto result = ResolvePostProcessProfile(input);
			context.Expect(result.candidate.has_value(), "All permitted layers resolve.");
			context.Expect(std::get<float>(Value(*result.candidate, GraphicsSettingId::FixedExposureCompensation)) == 9.0f, "Player choices have final precedence.");
			context.Expect(!std::get<bool>(Value(*result.candidate, GraphicsSettingId::BloomEnabled)), "Player disable wins over camera and volume enables.");
		});
		registry.Add("Renderer.PostProcess.Authoring.EveryPrecedenceLayerRemainsObservable", [](TestContext& context) {
			auto input = Input();
			input.engineDefaults = {{GraphicsSettingId::FixedExposureCompensation, 1.0f}};
			input.projectDefaults = {{GraphicsSettingId::FixedExposureCompensation, 2.0f}};
			input.qualityPreset = {{GraphicsSettingId::FixedExposureCompensation, 3.0f}};
			input.sceneProfile = Profile({{GraphicsSettingId::FixedExposureCompensation, 4.0f}});
			input.volumes = {Volume(3, 0, 1.0f, 5.0f, true)};
			input.cameraProfile = Profile({{GraphicsSettingId::FixedExposureCompensation, 6.0f}});
			input.cameraOverrides = {{GraphicsSettingId::FixedExposureCompensation, 7.0f}};
			input.runtimeCameraOverrides = {{GraphicsSettingId::FixedExposureCompensation, 8.0f}};
			input.playerAllowedSettings = {GraphicsSettingId::FixedExposureCompensation};
			input.playerOverrides = {{GraphicsSettingId::FixedExposureCompensation, 9.0f}};
			auto check = [&](float expected) {
				const auto result = ResolvePostProcessProfile(input);
				context.Expect(result.candidate && std::get<float>(Value(*result.candidate, GraphicsSettingId::FixedExposureCompensation)) == expected,
					"Removing one override reveals exactly the preceding inherited layer.");
			};
			check(9.0f); input.playerOverrides.clear(); check(8.0f);
			input.runtimeCameraOverrides.clear(); check(7.0f); input.cameraOverrides.clear(); check(6.0f);
			input.cameraProfile.reset(); check(5.0f); input.volumes.clear(); check(4.0f);
			input.sceneProfile.reset(); check(3.0f); input.qualityPreset.clear(); check(2.0f);
			input.projectDefaults.clear(); check(1.0f); input.engineDefaults.clear(); check(0.0f);
		});
		registry.Add("Renderer.PostProcess.Authoring.PlayerDisableRequiresDeclaredTypedOff", [](TestContext& context) {
			auto input = Input(); GraphicsSettingDescriptor mode;
			mode.id = GraphicsSettingId::AntiAliasingMode; mode.valueKind = GraphicsValueKind::Unsigned32;
			mode.defaultValue = std::uint32_t{0}; mode.options = {std::uint32_t{0}, std::uint32_t{1}};
			mode.cameraScope = true; mode.available = true; input.descriptors.push_back(mode);
			input.cameraOverrides = {{GraphicsSettingId::AntiAliasingMode, std::uint32_t{1}}};
			input.playerAllowedSettings = {GraphicsSettingId::AntiAliasingMode};
			input.playerDisabledSettings = {GraphicsSettingId::AntiAliasingMode};
			auto result = ResolvePostProcessProfile(input);
			context.Expect(result.candidate && std::get<std::uint32_t>(Value(*result.candidate, GraphicsSettingId::AntiAliasingMode)) == 0u,
				"A catalog-declared unsigned Off option can be disabled by player policy.");
			input.playerDisabledSettings.clear(); input.cameraOverrides = {{GraphicsSettingId::AntiAliasingMode, std::uint32_t{3}}};
			const auto unsupported = ResolvePostProcessProfile(input);
			context.Expect(!unsupported.candidate && unsupported.reason == GraphicsRequestReason::Unsupported,
				"A known AA mode absent from captured options reports unavailable rather than changing modes.");
			input.cameraOverrides.clear();
			input.playerAllowedSettings = {GraphicsSettingId::FixedExposureCompensation};
			input.playerDisabledSettings = {GraphicsSettingId::FixedExposureCompensation};
			context.Expect(!ResolvePostProcessProfile(input).candidate, "Scalar fields are not silently reinterpreted as Boolean effect enables.");
		});
		registry.Add("Renderer.PostProcess.Authoring.FinalCrossFieldConstraintsReject", [](TestContext& context) {
			auto input = Input(); input.descriptors.push_back(Scalar(GraphicsSettingId::AutoExposureMinEV, -8.0f));
			input.descriptors.push_back(Scalar(GraphicsSettingId::AutoExposureMaxEV, 8.0f));
			input.cameraOverrides = {{GraphicsSettingId::AutoExposureMinEV, 9.0f}};
			context.Expect(!ResolvePostProcessProfile(input).candidate, "Individually valid exposure values cannot produce an inverted range.");
			input = Input(); GraphicsSettingDescriptor aa;
			aa.id = GraphicsSettingId::AntiAliasingMode; aa.valueKind = GraphicsValueKind::Unsigned32;
			aa.defaultValue = std::uint32_t{0}; aa.options = {std::uint32_t{0}, std::uint32_t{5}};
			aa.cameraScope = true; aa.available = true; input.descriptors.push_back(aa);
			// TAA without the independently required indexed raster tuple is invalid.
			input.cameraOverrides = {{GraphicsSettingId::AntiAliasingMode, std::uint32_t{5}}};
			const auto result = ResolvePostProcessProfile(input);
			context.Expect(!result.candidate && result.reason == GraphicsRequestReason::Unsupported, "Real canonical AA tuple validation governs authored candidates.");
		});
		registry.Add("Renderer.PostProcess.Authoring.DeterministicPriorityUuidAndDiscreteDominance", [](TestContext& context) {
			auto input = Input(); input.volumes = {Volume(4, 1, 0.5f, 8.0f, true), Volume(3, 1, 0.5f, 4.0f, false), Volume(8, 0, 1.0f, 2.0f, false)};
			auto first = ResolvePostProcessProfile(input); std::reverse(input.volumes.begin(), input.volumes.end()); auto second = ResolvePostProcessProfile(input);
			context.Expect(first.candidate && second.candidate, "Copied volume orders resolve.");
			context.Expect(first.candidate->settings.values == second.candidate->settings.values, "Input iteration order never changes resolution.");
			context.Expect(std::get<float>(Value(*first.candidate, GraphicsSettingId::FixedExposureCompensation)) == 5.5f, "Continuous values blend in priority then UUID order.");
			context.Expect(std::get<bool>(Value(*first.candidate, GraphicsSettingId::BloomEnabled)), "At half weight the later UUID dominates discrete values.");
		});
		registry.Add("Renderer.PostProcess.Authoring.LayerFilteringAndDistanceFalloff", [](TestContext& context) {
			auto input = Input(); input.cameraPosition = {2.0, 0.0, 0.0}; input.cameraLayerMask = 1;
			auto local = Volume(3, 0, 1.0f, 8.0f, true); local.global = false; local.boundsMinimum = {-1.0, -1.0, -1.0}; local.boundsMaximum = {1.0, 1.0, 1.0}; local.blendDistance = 2.0f;
			auto otherLayer = Volume(4, 9, 1.0f, 99.0f, false); otherLayer.layerMask = 2;
			input.volumes = {local, otherLayer}; auto result = ResolvePostProcessProfile(input);
			context.Expect(result.candidate.has_value(), "A matching local volume resolves.");
			context.Expect(std::get<float>(Value(*result.candidate, GraphicsSettingId::FixedExposureCompensation)) == 4.0f, "One metre into a two-metre blend has half influence.");
		});
		registry.Add("Renderer.PostProcess.Authoring.RejectedLateLayerLeavesNoPartialCandidate", [](TestContext& context) {
			auto input = Input(); input.sceneProfile = Profile({{GraphicsSettingId::FixedExposureCompensation, 3.0f}});
			input.runtimeCameraOverrides = {{GraphicsSettingId::BloomEnabled, true}, {GraphicsSettingId::FixedExposureCompensation, std::numeric_limits<float>::quiet_NaN()}};
			const auto before = input.sceneProfile->values; auto result = ResolvePostProcessProfile(input);
			context.Expect(!result.candidate && result.reason == GraphicsRequestReason::InvalidValue, "Late invalid values reject the complete candidate.");
			context.Expect(before == input.sceneProfile->values, "Copied authored values are never mutated.");
		});
		registry.Add("Renderer.PostProcess.Authoring.ProfileSchemaIdentityMasksAndReferencesReject", [](TestContext& context) {
			for (int failure = 0; failure < 7; ++failure) {
				auto input = Input(); auto profile = Profile({{GraphicsSettingId::BloomEnabled, true}});
				if (failure == 0) profile->schemaVersion = 2;
				if (failure == 1) profile->revision = 0;
				if (failure == 2) profile->assetId = EMPTY_UUID;
				if (failure == 3) profile->overrideMask.push_back(GraphicsSettingId::BloomEnabled);
				if (failure == 4) profile->values.clear();
				if (failure == 5) profile->customEffects = {EMPTY_UUID};
				if (failure == 6) profile->customEffects = {Id(7), Id(7)};
				input.sceneProfile = profile; context.Expect(!ResolvePostProcessProfile(input).candidate, "Invalid profiles never expose partial candidates.");
			}
		});
		registry.Add("Renderer.PostProcess.Authoring.CustomReferenceMaskCanClearInheritance", [](TestContext& context) {
			auto input = Input(); auto scene = Profile({}); scene->customEffects = {Id(8)}; scene->overrideCustomEffects = true;
			auto camera = Profile({}); input.sceneProfile = scene; input.cameraProfile = camera;
			auto inherited = ResolvePostProcessProfile(input); context.Expect(inherited.candidate && inherited.candidate->customEffects == scene->customEffects, "Absent override inherits references.");
			camera->overrideCustomEffects = true; auto cleared = ResolvePostProcessProfile(input);
			context.Expect(cleared.candidate && cleared.candidate->customEffects.empty(), "Explicit empty override clears references.");
		});
		registry.Add("Renderer.PostProcess.Authoring.UnknownUnavailableScopeAndPolicyReject", [](TestContext& context) {
			for (int failure = 0; failure < 5; ++failure) {
				auto input = Input(); input.cameraOverrides = {{GraphicsSettingId::BloomEnabled, true}};
				if (failure == 0) input.cameraOverrides[0].id = GraphicsSettingId::Invalid;
				if (failure == 1) input.descriptors[1].available = false;
				if (failure == 2) input.descriptors[1].cameraScope = false;
				if (failure == 3) input.descriptors[1].activation = GraphicsActivation::NextLaunch;
				if (failure == 4) { input.playerOverrides = input.cameraOverrides; input.cameraOverrides.clear(); }
				context.Expect(!ResolvePostProcessProfile(input).candidate, "Unknown/unavailable/scope/policy writes reject completely.");
			}
		});
		registry.Add("Renderer.PostProcess.Authoring.VolumeIdentityAndBoundsReject", [](TestContext& context) {
			for (int failure = 0; failure < 5; ++failure) {
				auto input = Input(); auto volume = Volume(3, 0, 1.0f, 1.0f, true);
				if (failure == 0) volume.weight = std::numeric_limits<float>::infinity();
				if (failure == 1) volume.blendDistance = -1.0f;
				if (failure == 2) { volume.global = false; volume.boundsMinimum[0] = 1.0; }
				if (failure == 3) volume.profile.reset();
				input.volumes.push_back(volume); if (failure == 4) input.volumes.push_back(volume);
				context.Expect(!ResolvePostProcessProfile(input).candidate, "Invalid contributing volume DTOs reject completely.");
			}
		});
		registry.Add("Renderer.PostProcess.Authoring.ExactPublicationGuardAndCameraIsolation", [](TestContext& context) {
			auto input = Input(); auto output = ResolvePostProcessProfile(input); context.Expect(output.candidate.has_value(), "Baseline candidate resolves.");
			context.Expect(IsPostProcessResolutionCurrent(*output.candidate, input.target, input.lifecycle, input.baseRevision), "Matching protected owner may prepare publication.");
			auto target = input.target; target.camera->objectLifetimeGeneration++;
			context.Expect(!IsPostProcessResolutionCurrent(*output.candidate, target, input.lifecycle, input.baseRevision), "Recycled camera rejects.");
			target = input.target; target.view->lifetimeGeneration++;
			context.Expect(!IsPostProcessResolutionCurrent(*output.candidate, target, input.lifecycle, input.baseRevision), "Recreated view rejects.");
			auto lifecycle = input.lifecycle; lifecycle.backendResourceGeneration++;
			context.Expect(!IsPostProcessResolutionCurrent(*output.candidate, input.target, lifecycle, input.baseRevision), "Backend replacement rejects.");
			context.Expect(!IsPostProcessResolutionCurrent(*output.candidate, input.target, input.lifecycle, input.baseRevision + 1), "Later setting revision rejects.");
		});
		registry.Add("Renderer.PostProcess.Authoring.ConcurrentCopiedResolutionIsDeterministic", [](TestContext& context) {
			auto input = Input(); input.volumes = {Volume(3, 1, 0.5f, 8.0f, true)};
			const auto baseline = ResolvePostProcessProfile(input); std::vector<std::future<PostProcessProfileResolution>> jobs;
			for (int index = 0; index < 16; ++index) jobs.push_back(std::async(std::launch::async, [input] { return ResolvePostProcessProfile(input); }));
			for (auto& job : jobs) { const auto result = job.get(); context.Expect(result.candidate && result.candidate->settings.values == baseline.candidate->settings.values, "Parallel worker preparations have identical isolated outputs."); }
		});
	}
}
