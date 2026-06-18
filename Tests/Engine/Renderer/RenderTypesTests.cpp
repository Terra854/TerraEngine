#include "TestHarness.h"

#include "Renderer/RenderTypes.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		bool NearlyEqual(float left, float right, float epsilon = 0.0001f) {
			return std::fabs(left - right) <= epsilon;
		}

		bool NearlyEqualVec2(const glm::vec2& left, const glm::vec2& right) {
			return NearlyEqual(left.x, right.x) && NearlyEqual(left.y, right.y);
		}

		bool NearlyEqualVec3(const glm::vec3& left, const glm::vec3& right) {
			return NearlyEqual(left.x, right.x) && NearlyEqual(left.y, right.y) && NearlyEqual(left.z, right.z);
		}

		TerraEngine::UUID MakeUuid(std::uint8_t value) {
			TerraEngine::UUID id{};
			id.fill(value);
			return id;
		}
	}

	// These compile-time checks catch accidental GPU-layout drift before any
	// renderer backend needs to allocate or upload these mirrored structures.
	static_assert(sizeof(GPULight) == 176, "GPULight layout should continue to match the shader mirror.");
	static_assert(sizeof(GPULightAnalysisRecord) == 16, "GPU light analysis records should stay compact for readback buffers.");
	static_assert(sizeof(ClusterData) == 48, "Cluster data should stay aligned with clustered-lighting shader reads.");
	static_assert(sizeof(FrameCullingDataGPU) == 336, "Frame culling data should stay tightly packed for GPU uploads.");
	static_assert(sizeof(MeshletCullingCountersGPU) == 16, "Meshlet culling counters should preserve their 16-byte GPU layout.");

	void RegisterRenderTypesTests(TestRegistry& registry) {
		registry.Add("Engine.RenderTypes.ClampSkyTintClampsEveryChannel", [](TestContext& context) {
			const glm::vec3 clamped = ClampSkyTint(glm::vec3(-1.0f, 2.5f, 8.0f));

			context.Expect(NearlyEqualVec3(glm::vec3(0.0f, 2.5f, 4.0f), clamped), "Sky tint clamping should clamp each color channel into the authored range.");
		});

		registry.Add("Engine.RenderTypes.DefaultSceneSkySettingsMatchRuntimeBaseline", [](TestContext& context) {
			const SceneSkySettings settings = MakeDefaultSceneSkySettings();

			context.Expect(settings.enabled, "Default scene sky settings should render the sky background.");
			context.Expect(settings.sourceAsset.empty(), "Default scene sky settings should use the built-in procedural fallback.");
			context.Expect(NearlyEqual(1.0f, settings.intensity), "Default scene sky intensity should be neutral.");
			context.Expect(NearlyEqual(1.0f, settings.exposure), "Default scene sky exposure should be neutral.");
			context.Expect(NearlyEqual(0.0f, settings.rotationOffsetDegrees), "Default scene sky rotation should start at zero degrees.");
			context.Expect(NearlyEqualVec3(glm::vec3(0.92f, 0.96f, 1.0f), settings.horizonTint), "Default horizon tint should match the runtime baseline.");
			context.Expect(NearlyEqualVec3(glm::vec3(0.20f, 0.38f, 0.80f), settings.zenithTint), "Default zenith tint should match the runtime baseline.");
			context.Expect(NearlyEqualVec3(glm::vec3(0.10f, 0.12f, 0.18f), settings.groundTint), "Default ground tint should match the runtime baseline.");
			context.Expect(settings.directionalLightObjectId == TerraEngine::EMPTY_UUID, "Default sky settings should let the renderer resolve the scene directional light.");
		});

		registry.Add("Engine.RenderTypes.SanitizeSceneSkySettingsClampsUnsafeAuthoringValues", [](TestContext& context) {
			SceneSkySettings unsafeSettings;
			unsafeSettings.enabled = false;
			unsafeSettings.sourceAsset = "Assets/Skies/Test.hdr";
			unsafeSettings.intensity = 25.0f;
			unsafeSettings.exposure = -3.0f;
			unsafeSettings.rotationOffsetDegrees = std::numeric_limits<float>::infinity();
			unsafeSettings.horizonTint = glm::vec3(-2.0f, 1.0f, 8.0f);
			unsafeSettings.zenithTint = glm::vec3(6.0f, -1.0f, 0.5f);
			unsafeSettings.groundTint = glm::vec3(0.25f, 9.0f, -0.5f);
			unsafeSettings.directionalLightObjectId = MakeUuid(9);

			const SceneSkySettings sanitized = SanitizeSceneSkySettings(unsafeSettings);

			context.Expect(!sanitized.enabled, "Sky sanitization should preserve the authored enable flag.");
			context.ExpectEqual(std::string("Assets/Skies/Test.hdr"), sanitized.sourceAsset, "Sky sanitization should preserve the source asset path.");
			context.Expect(NearlyEqual(16.0f, sanitized.intensity), "Sky intensity should clamp to the supported maximum.");
			context.Expect(NearlyEqual(0.0f, sanitized.exposure), "Sky exposure should clamp to the supported minimum.");
			context.Expect(NearlyEqual(0.0f, sanitized.rotationOffsetDegrees), "Non-finite sky rotation should be reset to a safe default.");
			context.Expect(NearlyEqualVec3(glm::vec3(0.0f, 1.0f, 4.0f), sanitized.horizonTint), "Horizon tint should clamp each channel.");
			context.Expect(NearlyEqualVec3(glm::vec3(4.0f, 0.0f, 0.5f), sanitized.zenithTint), "Zenith tint should clamp each channel.");
			context.Expect(NearlyEqualVec3(glm::vec3(0.25f, 4.0f, 0.0f), sanitized.groundTint), "Ground tint should clamp each channel.");
			context.Expect(sanitized.directionalLightObjectId == MakeUuid(9), "Sky sanitization should preserve explicit directional light overrides.");

			unsafeSettings.rotationOffsetDegrees = 720.0f;
			context.Expect(NearlyEqual(360.0f, SanitizeSceneSkySettings(unsafeSettings).rotationOffsetDegrees), "Positive finite sky rotation should clamp to one full turn.");
			unsafeSettings.rotationOffsetDegrees = -720.0f;
			context.Expect(NearlyEqual(-360.0f, SanitizeSceneSkySettings(unsafeSettings).rotationOffsetDegrees), "Negative finite sky rotation should clamp to one full turn.");
		});

		registry.Add("Engine.RenderTypes.SanitizeSceneRenderSettingsClampsNestedSkyAndLighting", [](TestContext& context) {
			const SceneRenderSettings defaultSettings = MakeDefaultSceneRenderSettings();
			context.Expect(NearlyEqual(1.0f, defaultSettings.indirectStrength), "Default render settings should leave indirect lighting at neutral strength.");
			context.Expect(NearlyEqual(1.0f, defaultSettings.ambientOcclusionStrength), "Default render settings should leave ambient occlusion at neutral strength.");
			context.ExpectEqual(2u, defaultSettings.ambientOcclusionQuality, "Default render settings should use the medium ambient-occlusion quality tier.");

			SceneRenderSettings unsafeSettings;
			unsafeSettings.sky.intensity = -4.0f;
			unsafeSettings.sky.rotationOffsetDegrees = 720.0f;
			unsafeSettings.indirectStrength = 11.0f;
			unsafeSettings.ambientOcclusionStrength = -2.0f;
			unsafeSettings.ambientOcclusionQuality = 7u;

			const SceneRenderSettings sanitized = SanitizeSceneRenderSettings(unsafeSettings);

			context.Expect(NearlyEqual(0.0f, sanitized.sky.intensity), "Scene render sanitization should clamp nested sky intensity.");
			context.Expect(NearlyEqual(360.0f, sanitized.sky.rotationOffsetDegrees), "Scene render sanitization should clamp nested sky rotation.");
			context.Expect(NearlyEqual(8.0f, sanitized.indirectStrength), "Indirect lighting strength should clamp to the supported maximum.");
			context.Expect(NearlyEqual(0.0f, sanitized.ambientOcclusionStrength), "Ambient occlusion strength should clamp to the supported minimum.");
			context.ExpectEqual(3u, sanitized.ambientOcclusionQuality, "Ambient occlusion quality should clamp to the supported maximum tier.");
		});

		registry.Add("Engine.RenderTypes.UIRenderBatchAppendPreservesViewportAndConcatenatesRuns", [](TestContext& context) {
			UIRenderBatch source;
			// Populate every UI run list so Append is checked as one complete
			// render-batch operation instead of a single-vector helper.
			source.viewportSize = glm::vec2(1920.0f, 1080.0f);
			source.staticQuads.push_back(UIStaticQuad{glm::vec2(1.0f, 2.0f), glm::vec2(3.0f, 4.0f), glm::vec4(1.0f), 2.0f});
			source.immediateQuads.push_back(UIImmediateQuad{glm::vec2(5.0f, 6.0f), glm::vec2(7.0f, 8.0f), glm::vec4(0.5f), glm::vec4(0.0f, 0.0f, 0.5f, 0.5f)});
			source.textRuns.push_back(UITextRun{"Health", glm::vec2(9.0f, 10.0f), glm::vec2(100.0f, 24.0f), glm::vec4(1.0f), 18.0f});
			source.imageRuns.push_back(UIImageRun{"Textures/Icon.png", glm::vec2(11.0f, 12.0f), glm::vec2(32.0f, 32.0f), glm::vec4(1.0f), 4.0f});

			UIRenderBatch emptyTarget;
			emptyTarget.Append(source);

			context.Expect(NearlyEqualVec2(source.viewportSize, emptyTarget.viewportSize), "Appending into an empty UI batch should adopt the source viewport.");
			context.ExpectEqual(1u, static_cast<unsigned int>(emptyTarget.staticQuads.size()), "Appending should copy static quads.");
			context.ExpectEqual(1u, static_cast<unsigned int>(emptyTarget.immediateQuads.size()), "Appending should copy immediate quads.");
			context.ExpectEqual(1u, static_cast<unsigned int>(emptyTarget.textRuns.size()), "Appending should copy text runs.");
			context.ExpectEqual(1u, static_cast<unsigned int>(emptyTarget.imageRuns.size()), "Appending should copy image runs.");
			context.ExpectEqual(std::string("Health"), emptyTarget.textRuns[0].text, "Appending should preserve text run content.");
			context.ExpectEqual(std::string("Textures/Icon.png"), emptyTarget.imageRuns[0].texturePath, "Appending should preserve image texture paths.");

			UIRenderBatch existingTarget;
			existingTarget.viewportSize = glm::vec2(1280.0f, 720.0f);
			existingTarget.staticQuads.push_back(UIStaticQuad{glm::vec2(0.0f), glm::vec2(10.0f), glm::vec4(0.25f), 0.0f});
			existingTarget.Append(source);

			context.Expect(NearlyEqualVec2(glm::vec2(1280.0f, 720.0f), existingTarget.viewportSize), "Appending into an initialized UI batch should keep the existing viewport.");
			context.ExpectEqual(2u, static_cast<unsigned int>(existingTarget.staticQuads.size()), "Appending should concatenate static quads onto existing contents.");
			context.ExpectEqual(1u, static_cast<unsigned int>(existingTarget.immediateQuads.size()), "Appending should concatenate immediate quads onto existing contents.");
			context.ExpectEqual(1u, static_cast<unsigned int>(existingTarget.textRuns.size()), "Appending should concatenate text runs onto existing contents.");
			context.ExpectEqual(1u, static_cast<unsigned int>(existingTarget.imageRuns.size()), "Appending should concatenate image runs onto existing contents.");
		});
	}
}
