#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"

#include <cstddef>
#include <cstdint>

namespace TerraEngine::Tests
{
	// These checks pin the canonical production TEMF ABI at the test boundary.
	// The records are packed on disk, so a compiler-inserted padding byte would
	// change every table offset and make independently cooked files unreadable.
	void RegisterVirtualGeometryProductionFormatTests(TestRegistry &registry)
	{
		registry.Add("Resource.VirtualGeometryProductionFormat.CanonicalRecordSizes",
			[](TestContext &context)
			{
				context.ExpectEqual(static_cast<std::size_t>(192u), sizeof(MeshHeader),
					"The production TEMF root header must remain exactly 192 bytes.");
				context.ExpectEqual(static_cast<std::size_t>(64u), sizeof(TemfPageDescriptor),
					"Page descriptors must remain independently addressable at 64 bytes.");
				context.ExpectEqual(static_cast<std::size_t>(112u), sizeof(TemfBlockDescriptor),
					"Block descriptors must remain independently addressable at 112 bytes.");
				context.ExpectEqual(static_cast<std::size_t>(128u), sizeof(TemfQuantizationDomain),
					"Quantization domains must remain independently addressable at 128 bytes.");
				context.ExpectEqual(static_cast<std::size_t>(16u), sizeof(TemfBlockDependency),
					"Dependency records must remain independently addressable at 16 bytes.");
			});

		registry.Add("Resource.VirtualGeometryProductionFormat.CanonicalFieldOffsets",
			[](TestContext &context)
			{
				context.ExpectEqual(static_cast<std::size_t>(0u), offsetof(MeshHeader, magic), "TEMF magic starts the root header.");
				context.ExpectEqual(static_cast<std::size_t>(8u), offsetof(MeshHeader, headerBytes), "Header size remains at byte eight.");
				context.ExpectEqual(static_cast<std::size_t>(12u), offsetof(MeshHeader, representationFlags), "Representation flags remain adjacent to the size.");
				context.ExpectEqual(static_cast<std::size_t>(168u), offsetof(MeshHeader, formatContractId), "The mandatory contract identity remains at byte 168.");
				context.ExpectEqual(static_cast<std::size_t>(0u), offsetof(TemfPageDescriptor, logicalPageId), "Page identity starts each page record.");
				context.ExpectEqual(static_cast<std::size_t>(40u), offsetof(TemfPageDescriptor, pageIntegrity), "Page integrity remains after byte counters.");
				context.ExpectEqual(static_cast<std::size_t>(0u), offsetof(TemfBlockDescriptor, blockId), "Block identity starts each block record.");
				context.ExpectEqual(static_cast<std::size_t>(40u), offsetof(TemfBlockDescriptor, encodedPayloadOffset), "Block payload offsets remain 64-bit aligned.");
				context.ExpectEqual(static_cast<std::size_t>(24u), offsetof(TemfQuantizationDomain, origin), "Quantization origin remains after scalar metadata.");
				context.ExpectEqual(static_cast<std::size_t>(0u), offsetof(TemfBlockDependency, targetBlockId), "Dependency target identity starts each record.");
			});

		registry.Add("Resource.VirtualGeometryProductionFormat.FlagsAndLimits",
			[](TestContext &context)
			{
				constexpr std::uint64_t kContractIdentity = 0x31445250464D4554ull; // TEMFPRD1 in little-endian byte order.
				context.ExpectEqual(kContractIdentity, static_cast<std::uint64_t>(MeshHeader{}.formatContractId),
					"The production contract identity must be represented by TEMFPRD1.");
				context.ExpectEqual(static_cast<std::uint32_t>(192u), MeshHeader{}.headerBytes,
					"A default production header advertises its exact byte size.");
				context.Expect((MeshHeader{}.representationFlags & 0x7u) == 0u,
					"Reserved representation bits must remain clear in a default header.");
				context.Expect(TemfPageDescriptor{}.reserved[0] == 0u && TemfPageDescriptor{}.reserved[1] == 0u,
					"Reserved page descriptor words must remain zero.");
				context.Expect(TemfBlockDescriptor{}.reserved == 0u,
					"Reserved block descriptor words must remain zero.");
				context.Expect(TemfQuantizationDomain{}.componentCount <= 4u,
					"Quantization domains are limited to four components.");
			});
	}
}
