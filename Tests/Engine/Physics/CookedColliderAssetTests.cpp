// T050 tests cover the immutable cooked envelope and its corruption guards.
#include "../TestHarness.h"
#include "Physics/Assets/CookedColliderAsset.h"

#include <cstddef>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Physics;

	namespace
	{
		TerraEngine::UUID Id(const char* text) { return *TerraEngine::ParseUUID(text); }
		CookedColliderEnvelope ValidEnvelope()
		{
			CookedColliderEnvelope e;
			e.assetId = Id("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); e.sourceAssetId = Id("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
			e.sourceContentHash = 11U; e.recipeHash = 22U; e.vertexCount = 4U; e.partCount = 1U;
			e.bounds.minimum = {-1, -1, -1}; e.bounds.maximum = {1, 1, 1}; e.provenanceSummary = "fixture";
			return e;
		}
		std::optional<CookedColliderAsset> Make(CookedColliderEnvelope envelope, std::vector<std::byte> payload = {std::byte{1}, std::byte{2}, std::byte{3}})
		{
			CookedColliderDiagnostics diagnostics; return CookedColliderAsset::Create(std::move(envelope), std::move(payload), diagnostics);
		}
		void ExpectCode(TestContext& context, CookedColliderEnvelope e, CookedColliderValidationCode code)
		{
			CookedColliderDiagnostics diagnostics;
			auto asset = CookedColliderAsset::Create(std::move(e), {std::byte{1}, std::byte{2}, std::byte{3}}, diagnostics);
			context.Expect(!asset.has_value(), "Invalid envelope must not be published.");
			context.Expect(!diagnostics.entries.empty(), "Rejected publication must provide diagnostics.");
			context.ExpectEqual(std::string(ToString(code)), diagnostics.entries.front().code, "Envelope rejection code must identify the cause.");
		}
	}

	void RegisterCookedColliderAssetTests(TestRegistry& registry)
	{
		registry.Add("Physics.CookedCollider.EnvelopeRoundTripAndCompatibility", [](TestContext& context) {
			auto asset = Make(ValidEnvelope()); context.Expect(asset.has_value(), "Valid envelope must publish.");
			context.Expect(asset->Validate().accepted, "Published envelope must validate.");
			const auto bytes = asset->Serialize(); context.Expect(!bytes.empty(), "Valid envelope serialization must be nonempty.");
			CookedColliderDiagnostics diagnostics; auto restored = CookedColliderAsset::Deserialize(bytes, diagnostics);
			context.Expect(restored.has_value(), "Serialized envelope must deserialize.");
			const auto compatibility = restored->CheckCompatibility(CookedColliderAsset::HostPlatform(), CookedColliderProviderFamily::Jolt, kJoltCookProviderVersion, kJoltCookRevision);
			context.Expect(compatibility.compatible, "Pinned provider/version/revision must be compatible.");
		});

		registry.Add("Physics.CookedCollider.MetadataAndLastKnownGood", [](TestContext& context) {
			for(auto mutation : {0, 1, 2, 3, 4, 5, 6, 7})
			{
				auto e = ValidEnvelope();
				if(mutation == 0) e.magic = 0U;
				if(mutation == 1) e.formatVersion++;
				if(mutation == 2) e.providerVersion = "other";
				if(mutation == 3) e.cookRevision = "other";
				if(mutation == 4) e.payloadFormatFamily = "other";
				if(mutation == 5) e.bounds.maximum.x = -2.0F;
				if(mutation == 6) e.status = CookedColliderStatus::InvalidUnpublished;
				if(mutation == 7) { e.status = CookedColliderStatus::StaleLastKnownGood; e.lastKnownGood = CookedColliderLastKnownGood{33U, 44U, "failed recook"}; }
				if(mutation == 7) { auto asset = Make(e); context.Expect(asset.has_value(), "Bounded last-known-good metadata must be accepted."); }
				else
				{
					const CookedColliderValidationCode expected[] = {
						CookedColliderValidationCode::InvalidMagic,
						CookedColliderValidationCode::UnsupportedVersion,
						CookedColliderValidationCode::ProviderVersionMismatch,
						CookedColliderValidationCode::CookRevisionMismatch,
						CookedColliderValidationCode::PayloadFormatFamilyMismatch,
						CookedColliderValidationCode::InvalidBounds,
						CookedColliderValidationCode::InvalidStatus};
					ExpectCode(context, std::move(e), expected[mutation]);
				}
			}
		});

		registry.Add("Physics.CookedCollider.CorruptionTruncationAndTrailingData", [](TestContext& context) {
			auto asset = Make(ValidEnvelope()); const auto bytes = asset->Serialize();
			CookedColliderDiagnostics diagnostics;
			context.Expect(!CookedColliderAsset::Deserialize(std::span<const std::byte>(bytes.data(), bytes.size() - 1U), diagnostics), "Truncated payload must be rejected.");
			auto trailing = bytes; trailing.push_back(std::byte{0x7f}); diagnostics = {};
			context.Expect(!CookedColliderAsset::Deserialize(trailing, diagnostics), "Trailing envelope data must be rejected.");
			auto tampered = bytes; tampered.back() ^= std::byte{1}; diagnostics = {};
			context.Expect(!CookedColliderAsset::Deserialize(tampered, diagnostics), "Checksum corruption must be rejected.");
		});
	}
}
