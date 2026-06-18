#include "TestHarness.h"

#include "archive/ArchiveWorkflow.h"

#include <string>

namespace TerraEngine::EditorTests {
	void RegisterArchiveSerializationTests(TestRegistry& registry) {
		registry.Add("Editor.ArchiveSerialization.AssetClassCodesRoundTripKnownValues", [](TestContext& context) {
			context.ExpectEqual(1U, tepak::archive::EncodeAssetClass("mesh"), "Mesh assets should use their stable archive code.");
			context.ExpectEqual(2U, tepak::archive::EncodeAssetClass("texture"), "Texture assets should use their stable archive code.");
			context.ExpectEqual(3U, tepak::archive::EncodeAssetClass("shader"), "Shader assets should use their stable archive code.");
			context.ExpectEqual(std::string("audio"), tepak::archive::DecodeAssetClass(4U), "Audio archive codes should decode to their stable asset class.");
			context.ExpectEqual(std::string("video"), tepak::archive::DecodeAssetClass(5U), "Video archive codes should decode to their stable asset class.");
			context.ExpectEqual(std::string("data"), tepak::archive::DecodeAssetClass(6U), "Data archive codes should decode to their stable asset class.");
		});

		registry.Add("Editor.ArchiveSerialization.UnknownAssetClassesFallbackToOther", [](TestContext& context) {
			context.ExpectEqual(7U, tepak::archive::EncodeAssetClass("font"), "Unknown authored asset classes should encode as other.");
			context.ExpectEqual(7U, tepak::archive::EncodeAssetClass(""), "Empty authored asset classes should encode as other.");
			context.ExpectEqual(std::string("other"), tepak::archive::DecodeAssetClass(0U), "Unknown low archive codes should decode as other.");
			context.ExpectEqual(std::string("other"), tepak::archive::DecodeAssetClass(99U), "Unknown high archive codes should decode as other.");
		});

		registry.Add("Editor.ArchiveSerialization.DictionaryFlagUsesLowBitOnly", [](TestContext& context) {
			context.ExpectEqual(0U, tepak::archive::EncodePackFlags(false), "Packs without dictionaries should encode no flags.");
			context.ExpectEqual(1U, tepak::archive::EncodePackFlags(true), "Packs with dictionaries should encode the low dictionary flag.");
			context.Expect(!tepak::archive::HasDictionaryFlag(0U), "A zero flag field should not report a dictionary.");
			context.Expect(tepak::archive::HasDictionaryFlag(1U), "The low bit should report a dictionary.");
			context.Expect(tepak::archive::HasDictionaryFlag(3U), "Additional future bits should not hide the dictionary flag.");
		});
	}
}
