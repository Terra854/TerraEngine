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

		registry.Add("Editor.ArchiveSerialization.AnimationAssetClassCodesRemainAppendOnly", [](TestContext& context) {
			context.ExpectEqual(8U, tepak::archive::EncodeAssetClass("skeleton"), "Skeletons must retain appended TEPAK code eight.");
			context.ExpectEqual(9U, tepak::archive::EncodeAssetClass("animation-clip"), "Animation clips must retain appended TEPAK code nine.");
			context.ExpectEqual(10U, tepak::archive::EncodeAssetClass("animation-controller"), "Controllers must retain appended TEPAK code ten.");
			context.ExpectEqual(std::string("animation-mask"), tepak::archive::DecodeAssetClass(11U), "Animation mask code eleven must decode in the writer.");
			context.ExpectEqual(std::string("retarget-profile"), tepak::archive::DecodeAssetClass(12U), "Retarget code twelve must decode in the writer.");
			context.ExpectEqual(std::string("motion-database"), tepak::archive::DecodeAssetClass(13U), "Motion database code thirteen must decode in the writer.");
			context.ExpectEqual(std::string("ragdoll-profile"), tepak::archive::DecodeAssetClass(14U), "Ragdoll code fourteen must decode in the writer.");
			context.ExpectEqual(15U, tepak::archive::EncodeAssetClass("terrain"), "Terrain assets must retain appended TEPAK code fifteen.");
			context.ExpectEqual(std::string("terrain"), tepak::archive::DecodeAssetClass(15U), "Terrain code fifteen must survive writer serialization.");
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
