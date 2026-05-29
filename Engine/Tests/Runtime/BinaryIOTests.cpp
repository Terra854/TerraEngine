#include "TestHarness.h"

#include "Runtime/ContentMount/util/BinaryIO.h"

#include <cstdint>
#include <sstream>
#include <stdexcept>

namespace TerraEngine::Tests {
	namespace {
		struct SamplePod final {
			std::uint32_t id = 0U;
			float weight = 0.0f;
		};
	}

	void RegisterBinaryIOTests(TestRegistry& registry) {
		registry.Add("Engine.BinaryIO.PodRoundTripAndPaddingWritesZeroBytes", [](TestContext& context) {
			std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
			const SamplePod expected{42U, 2.5f};

			tepak::util::WritePod(stream, expected);
			tepak::util::WritePadding(stream, 3U);
			stream.seekg(0);

			const SamplePod actual = tepak::util::ReadPod<SamplePod>(stream);
			char padding[3] = {};
			stream.read(padding, 3);

			context.ExpectEqual(expected.id, actual.id, "Binary POD round trips should preserve integer fields.");
			context.ExpectEqual(expected.weight, actual.weight, "Binary POD round trips should preserve floating-point fields.");
			context.Expect(padding[0] == '\0' && padding[1] == '\0' && padding[2] == '\0', "Binary padding should be written as zero bytes.");
		});

		registry.Add("Engine.BinaryIO.AlignValueHandlesRemaindersAndZeroAlignment", [](TestContext& context) {
			context.ExpectEqual(16ULL, tepak::util::AlignValue(13U, 8U), "AlignValue should advance values with a non-zero remainder.");
			context.ExpectEqual(16ULL, tepak::util::AlignValue(16U, 8U), "AlignValue should leave already aligned values unchanged.");
			context.ExpectEqual(13ULL, tepak::util::AlignValue(13U, 0U), "A zero alignment should leave the input value unchanged.");
		});

		registry.Add("Engine.BinaryIO.ReadPodRejectsTruncatedInput", [](TestContext& context) {
			std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
			const char truncatedBytes[2] = {1, 2};
			stream.write(truncatedBytes, 2);
			stream.seekg(0);

			bool rejected = false;
			try {
				(void)tepak::util::ReadPod<std::uint32_t>(stream);
			} catch(const std::runtime_error&) {
				rejected = true;
			}

			context.Expect(rejected, "ReadPod should reject streams that do not contain a complete POD value.");
		});
	}
}
