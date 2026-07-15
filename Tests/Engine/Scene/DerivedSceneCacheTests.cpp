// DerivedSceneCacheTests.cpp - bounded, source-authoritative envelope tests.

#include "../TestHarness.h"

#include "Scene/DerivedSceneCache.h"

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::Scene::DerivedSceneCompatibilityIdentity MakeIdentity() {
			return {"Scenes/Test.tescene", "source-hash", "dependency-hash", "components-hash", "reflection-hash", "engine-v1", "Windows-x64", TerraEngine::Scene::DerivedSceneByteOrder::LittleEndian};
		}

		TerraEngine::Scene::DerivedSceneEnvelope MakeEnvelope() {
			using namespace TerraEngine::Scene;
			DerivedSceneEnvelope envelope;
			envelope.manifest.identity = MakeIdentity();
			envelope.manifest.objectCount = 1U;
			envelope.manifest.componentCount = 1U;
			envelope.manifest.hierarchyCount = 0U;
			DerivedSceneSection section;
			section.descriptor.type = DerivedSceneSectionType::Objects;
			section.descriptor.offset = 0U;
			section.descriptor.itemCount = 1U;
			section.payload = {std::byte{0x1U}, std::byte{0x2U}, std::byte{0x3U}};
			section.descriptor.storedBytes = section.payload.size();
			section.descriptor.decodedBytes = section.payload.size();
			section.descriptor.checksum = CalculateDerivedSceneChecksum(section.payload);
			envelope.sections.push_back(section);
			envelope.manifest.sections.push_back(section.descriptor);
			envelope.manifest.payloadChecksum = CalculateDerivedSceneChecksum(section.payload);
			return envelope;
		}
	}

	void RegisterDerivedSceneCacheTests(TestRegistry& registry) {
		registry.Add("Engine.DerivedSceneCache.AcceptsVersionedChecksummedCompatibleEnvelope", [](TestContext& context) {
			const auto envelope = MakeEnvelope();
			const auto validation = TerraEngine::Scene::ValidateDerivedSceneEnvelope(envelope, MakeIdentity());
			context.Expect(validation.accepted, "A bounded envelope with matching identities and checksums should be reusable.");
			const auto encoded = TerraEngine::Scene::EncodeDerivedSceneEnvelope(envelope);
			std::string decodeError;
			const auto decoded = TerraEngine::Scene::DecodeDerivedSceneEnvelope(encoded, &decodeError);
			context.Expect(decoded.has_value(), "Portable field-by-field envelope encoding should round-trip without native struct casts.");
			context.Expect(decoded && TerraEngine::Scene::ValidateDerivedSceneEnvelope(*decoded, MakeIdentity()).accepted, "Decoded data must revalidate before it is eligible for worker preparation.");
		});

		registry.Add("Engine.DerivedSceneCache.RejectsIdentityChecksumAndOffsetFailuresWithSourceFallback", [](TestContext& context) {
			auto stale = MakeEnvelope();
			stale.manifest.identity.sourceHash = "changed-source";
			const auto staleResult = TerraEngine::Scene::PrepareDerivedSceneSections(stale, MakeIdentity());
			context.Expect(!staleResult.accepted && staleResult.usedAuthoritativeSource, "A stale source identity must select the authoritative source fallback.");

			auto corrupt = MakeEnvelope();
			corrupt.sections.front().payload.front() = std::byte{0xFFU};
			const auto corruptResult = TerraEngine::Scene::PrepareDerivedSceneSections(corrupt, MakeIdentity());
			context.Expect(!corruptResult.accepted && corruptResult.usedAuthoritativeSource, "A corrupt section checksum must reject derived data before allocation/publication.");

			auto overlapping = MakeEnvelope();
			overlapping.manifest.headerBytes = 4U;
			overlapping.sections.front().descriptor.offset = 2U;
			overlapping.manifest.sections.front().offset = 2U;
			const auto overlapValidation = TerraEngine::Scene::ValidateDerivedSceneEnvelope(overlapping, MakeIdentity());
			context.Expect(!overlapValidation.accepted, "A section offset inside the bounded header must be rejected.");
		});

		registry.Add("Engine.DerivedSceneCache.CancellationLeavesNoPreparedSections", [](TestContext& context) {
			const auto result = TerraEngine::Scene::PrepareDerivedSceneSections(MakeEnvelope(), MakeIdentity(), [] { return true; });
			context.Expect(result.cancelled && !result.accepted && result.sections.empty(), "Cancellation must discard worker-local section records rather than exposing a partial packet.");
		});
	}
}
