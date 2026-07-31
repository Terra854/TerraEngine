// VulkanContextResources.cpp — Vulkan Resource Management Implementation
//
// Implements the management of persistent and transient GPU resources within the Vulkan backend.
// This includes the allocation of buffers, creation of image views, and management of descriptor sets and pipeline states.
// It provides specialized caches for meshes, textures, and materials to optimize GPU resource utilization and performance.

#include "VulkanContextImpl.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Profiling/ProfilerScope.h"
#include "Renderer/VirtualGeometry/Geometry/VirtualGeometryPackedVertex.h"
#include "Resource/ResourceStreaming.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <new>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	constexpr std::uint64_t kClusterRangeHashOffset = 1469598103934665603ull;
	constexpr std::uint64_t kClusterRangeHashPrime = 1099511628211ull;
	constexpr std::uint32_t kFallbackMaterialIndex = 0u;
	constexpr std::uint32_t kFirstRuntimeMaterialIndex = 1u;

	std::array<std::uint32_t, 4> CopyUuidWords(const TerraEngine::UUID& value)
	{
		// UUID is stored as 16 bytes. Copying into fixed words preserves the
		// stable identity bits without depending on alignment or aliasing.
		std::array<std::uint32_t, 4> words{};
		std::memcpy(words.data(), value.data(), value.size());
		return words;
	}

	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord
	BuildVirtualGeometryPacketUploadRecord(const GpuVirtualGeometryPacketRecord& source)
	{
		using UploadRecord =
			TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord;
		UploadRecord record{};
		record.packetId = source.packetId;
		record.viewId = source.viewId;
		record.viewType = static_cast<std::uint32_t>(source.viewType);
		record.frameIdLow = static_cast<std::uint32_t>(source.frameId);
		record.objectIdWords = CopyUuidWords(source.objectId);
		record.meshIdWords = CopyUuidWords(source.meshId);
		record.materialIdWords = CopyUuidWords(source.materialId);
		record.selectedLod = source.selectedLod;
		record.clusterId = source.clusterId;
		record.meshletOffset = source.meshletOffset;
		record.meshletCount = source.meshletCount;
		record.boundsMin = {
			source.boundsMin.x, source.boundsMin.y, source.boundsMin.z, 0.0f };
		record.boundsMax = {
			source.boundsMax.x, source.boundsMax.y, source.boundsMax.z, 0.0f };
		record.projectedBounds = {
			source.projectedBounds.x,
			source.projectedBounds.y,
			source.projectedBounds.z,
			source.projectedBounds.w };
		record.pageId = source.pageId;
		record.fallbackPageId = source.fallbackPageId;
		record.payloadSource = static_cast<std::uint32_t>(source.payloadSource);
		record.emittedSource = static_cast<std::uint32_t>(source.emittedSource);
		record.pagePayloadByteOffset = {
			static_cast<std::uint32_t>(source.pagePayloadByteOffset),
			static_cast<std::uint32_t>(source.pagePayloadByteOffset >> 32u) };
		record.pagePayloadByteSize = {
			static_cast<std::uint32_t>(source.pagePayloadByteSize),
			static_cast<std::uint32_t>(source.pagePayloadByteSize >> 32u) };
		record.payloadBufferIndex = source.payloadBufferIndex;
		record.payloadDescriptorIndex = source.payloadDescriptorIndex;
		record.generatedCommandIndex = source.packetId;
		record.generatedCommandTaskGroupCount =
			(std::max(source.meshletCount, 1u) + 31u) / 32u;
		record.debugIdentity = source.debugIdentity;
		record.flags = source.flags;
		return record;
	}

	constexpr std::uint32_t GetVirtualGeometryTimestampStageIndex(
		TerraEngine::Renderer::VirtualGeometryBackendStage stage)
	{
		return static_cast<std::uint32_t>(stage);
	}

	// CreateFallbackMaterialData defines the renderer-owned material contract
	// for missing, loading, or invalid materials. Release builds cannot rely on
	// GPU memory being zeroed, so every unused material slot is initialized with
	// these safe texture indices and scalar defaults.
	MaterialData CreateFallbackMaterialData()
	{
		MaterialData data{};
		data.BaseColorFactor = glm::vec4(1.0f);
		data.EmissiveFactor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		data.UVTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
		data.MetallicFactor = 0.0f;
		data.RoughnessFactor = 1.0f;
		data.NormalScale = 1.0f;
		data.OcclusionStrength = 1.0f;
		data.AlphaCutoff = 0.5f;
		data.AlphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
		data.ShadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
		data.Flags = 0u;
		data.BaseColorTextureIndex = 0u;
		data.NormalTextureIndex = 1u;
		data.MaskTextureIndex = 2u;
		data.OcclusionTextureIndex = 2u;
		data.EmissiveTextureIndex = 0u;
		data.ShadowBehavior = static_cast<std::uint32_t>(MaterialShadowBehavior::Opaque);
		return data;
	}

	// WriteMappedBuffer copies CPU data into a mapped VMA buffer and flushes the
	// written range before the GPU can copy or read from it. Debug builds often
	// land these uploads in coherent memory, but Release/export runs are free to
	// use non-coherent host-visible memory, so every staging upload must make
	// visibility explicit.
	bool WriteMappedBuffer(VmaAllocator allocator, const VulkanBuffer& buffer, const void* source, VkDeviceSize size, VkDeviceSize offset = 0u)
	{
		if (allocator == VK_NULL_HANDLE
			|| buffer.allocation == VK_NULL_HANDLE
			|| source == nullptr
			|| size == 0u)
		{
			return false;
		}

		void* mapped = nullptr;
		if (vmaMapMemory(allocator, buffer.allocation, &mapped) != VK_SUCCESS)
		{
			return false;
		}

		std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, source, static_cast<std::size_t>(size));
		vmaFlushAllocation(allocator, buffer.allocation, offset, size);
		vmaUnmapMemory(allocator, buffer.allocation);
		return true;
	}

	// ClearMappedBuffer establishes deterministic safe contents for optional
	// animation inputs and compute outputs before shaders can inspect them.
	// This is an upload only; it never asks the GPU to return a pose to the CPU.
	bool ClearMappedBuffer(VmaAllocator allocator, const VulkanBuffer& buffer)
	{
		if(allocator == VK_NULL_HANDLE || buffer.allocation == VK_NULL_HANDLE || buffer.size == 0U) return false;
		void* mapped = nullptr;
		if(vmaMapMemory(allocator, buffer.allocation, &mapped) != VK_SUCCESS) return false;
		std::memset(mapped, 0, static_cast<std::size_t>(buffer.size));
		vmaFlushAllocation(allocator, buffer.allocation, 0U, buffer.size);
		vmaUnmapMemory(allocator, buffer.allocation);
		return true;
	}

	// Builds the conventional-mesh morph stream in deterministic target-major
	// order from the three canonical decoded TEMFPRD1 channels. Graphics always
	// reads the same 48-byte record shape.
	bool BuildMeshMorphDeltaRecords(
		const Mesh& mesh,
		std::vector<TemfDeformablePageMorphVertex>& output)
	{
		output.clear();
		if(!mesh.hasDeformablePayload || mesh.deformableMorphs.empty()
			|| mesh.deformableMetadata.morphTargetCount != mesh.deformableMorphs.size()) {
			output.emplace_back();
			return true;
		}

		std::vector<const TemfDeformableMorphRecord*> targets;
		targets.reserve(mesh.deformableMorphs.size());
		for(const TemfDeformableMorphRecord& target : mesh.deformableMorphs) targets.push_back(&target);
		std::sort(targets.begin(), targets.end(), [](const TemfDeformableMorphRecord* left, const TemfDeformableMorphRecord* right) {
			return left->stableTargetId < right->stableTargetId;
		});

		for(const TemfDeformableMorphRecord* target : targets) {
			if(target == nullptr
				|| target->deltaCount != mesh.deformableMetadata.skinningVertexCount
				|| target->firstDeltaIndex > mesh.deformableMorphPositions.size()
				|| target->deltaCount > mesh.deformableMorphPositions.size() - target->firstDeltaIndex
				|| target->firstDeltaIndex > mesh.deformableMorphNormals.size()
				|| target->deltaCount > mesh.deformableMorphNormals.size() - target->firstDeltaIndex
				|| target->firstDeltaIndex > mesh.deformableMorphTangents.size()
				|| target->deltaCount > mesh.deformableMorphTangents.size() - target->firstDeltaIndex) {
				output.clear();
				output.emplace_back();
				return false;
			}

			for(std::uint32_t vertexIndex = 0U; vertexIndex < target->deltaCount; ++vertexIndex) {
				const std::size_t sourceIndex = target->firstDeltaIndex + vertexIndex;
				TemfDeformablePageMorphVertex record{};
				std::memcpy(record.positionDelta,
					mesh.deformableMorphPositions[sourceIndex].data(), sizeof(float) * 3U);
				std::memcpy(record.normalDelta,
					mesh.deformableMorphNormals[sourceIndex].data(), sizeof(float) * 3U);
				std::memcpy(record.tangentDelta,
					mesh.deformableMorphTangents[sourceIndex].data(), sizeof(float) * 3U);
				output.push_back(record);
			}
		}
		return true;
	}

	VkDeviceSize TotalMeshGpuBytes(const VulkanMesh& mesh)
	{
		return mesh.meshShaderVertexBuffer.size
			+ mesh.meshletVertexRemapBuffer.size
			+ mesh.meshSkinningBuffer.size
			+ mesh.meshMorphDeltaBuffer.size
			+ mesh.meshDecodeInfoBuffer.size
			+ mesh.indexBuffer.size
			+ mesh.meshletBuffer.size
			+ mesh.meshletSectionBuffer.size
			+ mesh.meshletBoundsBuffer.size;
	}

	struct VirtualGeometryPageLocalMeshDecodeInfo
	{
		float positionOffset[4]{};
		float positionScale[4]{};
		float uvOffsetScale[4]{};
		std::uint32_t counts[4]{};
	};
	static_assert(sizeof(VirtualGeometryPageLocalMeshDecodeInfo) == 64u, "Page-local decode info must match the default shader layout.");

	struct VirtualGeometryPageLocalPayloadBuildResult
	{
		std::vector<std::uint32_t> words;
		std::unordered_map<TerraEngine::UUID, std::uint64_t> bytesByMesh;
	};

	// This byte-exact upload record mirrors RasterTriangle in
	// VirtualGeometryComputeRaster.comp.hlsl. CPU preparation resolves selected
	// meshlet primitives to clip space once; the protected compute pass receives
	// only immutable frame-owned triangle values.
	struct alignas(16) VirtualGeometryRasterTriangleUpload
	{
		glm::vec4 clip0 = glm::vec4(0.0f);
		glm::vec4 clip1 = glm::vec4(0.0f);
		glm::vec4 clip2 = glm::vec4(0.0f);
		std::uint32_t stablePrimitiveId = 0xffffffffu;
		std::uint32_t packedOwnerMaterial = 0u;
		std::uint32_t sourceGeneration = 0u;
		std::uint32_t flags = 0u;
	};
	static_assert(
		sizeof(VirtualGeometryRasterTriangleUpload) == 64u,
		"Compute-raster triangle uploads must match the shader ABI.");

	std::uint64_t BuildVirtualGeometryPagePayloadResourceSignature(
		const std::vector<GpuVirtualGeometryPacketRecord>& packets)
	{
		std::uint64_t signature = TerraEngine::Renderer::Vulkan::BuildVirtualGeometryPagePayloadReuseSignature(packets);
		auto hashValue = [&signature](std::uint64_t value) {
			// Match the packet-signature mixer while adding the loaded resource
			// identity that the pure upload planner cannot see.
			signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6u) + (signature >> 2u);
		};
		for (const GpuVirtualGeometryPacketRecord& packet : packets)
		{
			const Mesh* mesh = ResourceManager::Instance().GetMesh(packet.meshId);
			// ResourceManager replaces a loaded Mesh instance on reload. Including
			// both object and backing-storage identity invalidates cached page bytes
			// without hashing the full vertex stream every rendered frame.
			hashValue(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mesh)));
			if (mesh != nullptr)
			{
				hashValue(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mesh->storage.data())));
				hashValue(mesh->storage.size());
				hashValue(mesh->meshletRecords.size());
				hashValue(mesh->meshletBounds.size());
				hashValue(mesh->virtualGeometryPayloadGeneration);
				hashValue(mesh->deformablePayloadGeneration);
				hashValue(
					mesh->deformableDemandedPageDirectory.meshPayloadGeneration);
				const auto page = std::find_if(
					mesh->deformableDemandedPageEntries.begin(),
					mesh->deformableDemandedPageEntries.end(),
					[&packet](
						const TemfDeformableDemandedPageEntry&
							entry)
					{
						return entry.pageId == packet.pageId;
					});
				if(page
					!= mesh->deformableDemandedPageEntries.end())
				{
					hashValue(page->contentGeneration);
					hashValue(page->blobId);
				}
			}
		}
		return signature;
	}

	void AddVirtualGeometryPagePayloadBytes(
		std::unordered_map<TerraEngine::UUID, std::uint64_t>& bytesByMesh,
		const TerraEngine::UUID& meshId,
		std::uint64_t byteCount)
	{
		if (meshId == TerraEngine::EMPTY_UUID || byteCount == 0u)
		{
			return;
		}
		std::uint64_t& total = bytesByMesh[meshId];
		constexpr std::uint64_t maxBytes = std::numeric_limits<std::uint64_t>::max();
		total = total > maxBytes - byteCount ? maxBytes : total + byteCount;
	}

	bool IsVirtualGeometryPageLocalSource(VirtualGeometryPagePayloadSource source)
	{
		return source == VirtualGeometryPagePayloadSource::ResidentPagePayload
			|| source == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
	}

	std::uint32_t AppendVirtualGeometryPayloadBytesAsWords(std::vector<std::uint32_t>& words, const void* data, std::size_t byteSize)
	{
		const std::uint32_t wordOffset = static_cast<std::uint32_t>(words.size());
		if (data == nullptr || byteSize == 0u)
		{
			return wordOffset;
		}

		const auto* bytes = static_cast<const std::uint8_t*>(data);
		const std::size_t wordCount = (byteSize + sizeof(std::uint32_t) - 1u) / sizeof(std::uint32_t);
		for (std::size_t wordIndex = 0u; wordIndex < wordCount; ++wordIndex)
		{
			std::uint32_t packed = 0u;
			const std::size_t byteOffset = wordIndex * sizeof(std::uint32_t);
			const std::size_t copySize = std::min<std::size_t>(sizeof(std::uint32_t), byteSize - byteOffset);
			std::memcpy(&packed, bytes + byteOffset, copySize);
			words.push_back(packed);
		}
		return wordOffset;
	}

	bool TryReadVirtualGeometryRemapIndex(const Mesh& mesh, std::uint32_t remapIndex, std::uint32_t& outGlobalVertexIndex)
	{
		outGlobalVertexIndex = 0u;
		const MeshletRemapEncoding encoding = static_cast<MeshletRemapEncoding>(mesh.runtimeLayout.meshletVertexRemapEncoding);
		if (encoding == MeshletRemapEncoding::Uint16)
		{
			const std::size_t byteOffset = static_cast<std::size_t>(remapIndex) * sizeof(std::uint16_t);
			if (byteOffset + sizeof(std::uint16_t) > mesh.meshletVertexRemapData.size())
			{
				return false;
			}
			std::uint16_t value = 0u;
			std::memcpy(&value, mesh.meshletVertexRemapData.data() + byteOffset, sizeof(value));
			outGlobalVertexIndex = value;
			return outGlobalVertexIndex < mesh.runtimeLayout.meshletVertexCount;
		}
		if (encoding == MeshletRemapEncoding::Uint32)
		{
			const std::size_t byteOffset = static_cast<std::size_t>(remapIndex) * sizeof(std::uint32_t);
			if (byteOffset + sizeof(std::uint32_t) > mesh.meshletVertexRemapData.size())
			{
				return false;
			}
			std::memcpy(&outGlobalVertexIndex, mesh.meshletVertexRemapData.data() + byteOffset, sizeof(outGlobalVertexIndex));
			return outGlobalVertexIndex < mesh.runtimeLayout.meshletVertexCount;
		}
		return false;
	}

	bool AppendVirtualGeometryPageLocalPacketPayload(
		std::vector<std::uint32_t>& words,
		std::uint32_t descriptorIndex,
		const GpuVirtualGeometryPacketRecord& packet,
		std::uint64_t* outAppendedGpuBytes = nullptr)
	{
		if (outAppendedGpuBytes != nullptr)
		{
			*outAppendedGpuBytes = 0u;
		}
		if (!IsVirtualGeometryPageLocalSource(packet.payloadSource))
		{
			return false;
		}
		const std::size_t descriptorWordOffset = static_cast<std::size_t>(descriptorIndex)
			* kVirtualGeometryPagePayloadDescriptorStrideWords;
		if (descriptorWordOffset + kVirtualGeometryPagePayloadDescriptorStrideWords > words.size())
		{
			return false;
		}

		const Mesh* cpuMesh = ResourceManager::Instance().GetMesh(packet.meshId);
		if (cpuMesh == nullptr
			|| cpuMesh->runtimeLayout.layout.strideBytes != kMeshletPackedVertexStrideBytes)
		{
			return false;
		}

		// Canonical demanded-page streams are already page-local and validated by
		// ResourceManager, so this path never walks or repacks the complete mesh.
		if (cpuMesh->hasDeformableDemandedPages)
		{
			const auto entryIt = std::find_if(
				cpuMesh->deformableDemandedPageEntries.begin(),
				cpuMesh->deformableDemandedPageEntries.end(),
				[&packet](const TemfDeformableDemandedPageEntry& entry)
				{
					return entry.pageId == packet.pageId;
				});
			if (entryIt != cpuMesh->deformableDemandedPageEntries.end())
			{
				const std::size_t pageIndex = static_cast<std::size_t>(
					std::distance(
						cpuMesh->deformableDemandedPageEntries.begin(),
						entryIt));
				if (pageIndex
						< cpuMesh->deformableDemandedPagePayloads.size()
					&& pageIndex
						< cpuMesh->deformableDemandedPageSections.size())
				{
					const std::span<const std::uint8_t> blobBytes =
						cpuMesh->deformableDemandedPagePayloads[pageIndex];
					const auto& sections =
						cpuMesh->deformableDemandedPageSections[pageIndex];
					const auto FindSection = [&sections](
						TemfDeformablePageSectionKind kind)
						-> const TemfDeformablePageSection*
					{
						const auto sectionIt = std::find_if(
							sections.begin(),
							sections.end(),
							[kind](
								const TemfDeformablePageSection& section)
							{
								return section.kind == kind;
							});
						return sectionIt == sections.end()
							? nullptr
							: &*sectionIt;
					};
					const TemfDeformablePageSection* vertexSection =
						FindSection(
							TemfDeformablePageSectionKind::PackedVertices);
					const TemfDeformablePageSection* primitiveSection =
						FindSection(
							TemfDeformablePageSectionKind::Topology);
					const TemfDeformablePageSection* remapSection =
						FindSection(
							TemfDeformablePageSectionKind::VertexRemap);
					const TemfDeformablePageSection* meshletSection =
						FindSection(
							TemfDeformablePageSectionKind::Meshlets);
					const TemfDeformablePageSection* boundsSection =
						FindSection(
							TemfDeformablePageSectionKind::MeshletBounds);
					const TemfDeformablePageSection* decodeSection =
						FindSection(
							TemfDeformablePageSectionKind::DecodeConstants);
					if (vertexSection != nullptr
						&& primitiveSection != nullptr
						&& remapSection != nullptr
						&& meshletSection != nullptr
						&& boundsSection != nullptr
						&& decodeSection != nullptr
						&& decodeSection->byteSize
							== sizeof(QuantizationInfo))
					{
						QuantizationInfo quantization{};
						std::memcpy(
							&quantization,
							blobBytes.data()
								+ static_cast<std::size_t>(
									decodeSection->relativeByteOffset),
							sizeof(quantization));
						VirtualGeometryPageLocalMeshDecodeInfo decodeInfo{};
						std::copy_n(
							quantization.positionOffset,
							3u,
							decodeInfo.positionOffset);
						std::copy_n(
							quantization.positionScale,
							3u,
							decodeInfo.positionScale);
						decodeInfo.uvOffsetScale[0] =
							quantization.uvOffset[0];
						decodeInfo.uvOffsetScale[1] =
							quantization.uvOffset[1];
						decodeInfo.uvOffsetScale[2] =
							quantization.uvScale[0];
						decodeInfo.uvOffsetScale[3] =
							quantization.uvScale[1];
						decodeInfo.counts[0] =
							vertexSection->elementCount;
						decodeInfo.counts[1] =
							remapSection->elementCount;
						decodeInfo.counts[2] =
							primitiveSection->elementCount;
						decodeInfo.counts[3] =
							meshletSection->elementCount;

						const auto AppendSection = [
							&words,
							&blobBytes](
							const TemfDeformablePageSection& section)
						{
							return AppendVirtualGeometryPayloadBytesAsWords(
								words,
								blobBytes.data()
									+ static_cast<std::size_t>(
										section.relativeByteOffset),
								static_cast<std::size_t>(section.byteSize));
						};
						const std::size_t payloadStartWords = words.size();
						const std::uint32_t vertexWordOffset =
							AppendSection(*vertexSection);
						const std::uint32_t primitiveWordOffset =
							AppendSection(*primitiveSection);
						const std::uint32_t remapWordOffset =
							AppendSection(*remapSection);
						const std::uint32_t meshletWordOffset =
							AppendSection(*meshletSection);
						const std::uint32_t boundsWordOffset =
							AppendSection(*boundsSection);
						const std::uint32_t decodeWordOffset =
							AppendVirtualGeometryPayloadBytesAsWords(
								words,
								&decodeInfo,
								sizeof(decodeInfo));
						std::uint32_t* descriptor = words.data()
							+ descriptorWordOffset;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorVertexWordOffset]
							= vertexWordOffset;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorVertexWordSize]
							= static_cast<std::uint32_t>(
								(vertexSection->byteSize
									+ sizeof(std::uint32_t) - 1u)
								/ sizeof(std::uint32_t));
						descriptor[
							kVirtualGeometryPagePayloadDescriptorPrimitiveWordOffset]
							= primitiveWordOffset;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorPrimitiveByteSize]
							= static_cast<std::uint32_t>(
								primitiveSection->byteSize);
						descriptor[
							kVirtualGeometryPagePayloadDescriptorRemapWordOffset]
							= remapWordOffset;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorRemapCount]
							= remapSection->elementCount;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorMeshletWordOffset]
							= meshletWordOffset;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorMeshletCount]
							= meshletSection->elementCount;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorBoundsWordOffset]
							= boundsWordOffset;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorBoundsCount]
							= boundsSection->elementCount;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorDecodeWordOffset]
							= decodeWordOffset;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorMaterialWordOffset]
							= 0u;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorMaterialCount]
							= 0u;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorLocalVertexCount]
							= vertexSection->elementCount;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorLocalPrimitiveByteCount]
							= primitiveSection->elementCount;
						descriptor[
							kVirtualGeometryPagePayloadDescriptorLocalRemapEncoding]
							= 0u;
						if (outAppendedGpuBytes != nullptr)
						{
							*outAppendedGpuBytes =
								static_cast<std::uint64_t>(
									words.size() - payloadStartWords)
								* sizeof(std::uint32_t);
						}
						return true;
					}
				}
			}
		}

		if (packet.meshletCount == 0u
			|| packet.meshletOffset >= cpuMesh->meshletRecords.size())
		{
			return false;
		}

		const std::uint32_t availableMeshlets = static_cast<std::uint32_t>(cpuMesh->meshletRecords.size()) - packet.meshletOffset;
		const std::uint32_t selectedMeshlets = std::min(packet.meshletCount, availableMeshlets);
		std::vector<std::uint8_t> localVertexBytes;
		std::vector<std::uint8_t> localPrimitiveBytes;
		std::vector<std::uint32_t> localRemap;
		std::vector<MeshletRecord> localMeshletRecords;
		std::vector<MeshletBounds> localBounds;
		std::unordered_map<std::uint32_t, std::uint32_t> localVertexByOwnerIndex;

		localMeshletRecords.reserve(selectedMeshlets);
		localBounds.reserve(selectedMeshlets);
		for (std::uint32_t localMeshlet = 0u; localMeshlet < selectedMeshlets; ++localMeshlet)
		{
			const std::uint32_t ownerMeshletIndex = packet.meshletOffset + localMeshlet;
			const MeshletRecord& ownerRecord = cpuMesh->meshletRecords[ownerMeshletIndex];
			if (ownerRecord.vertexCount == 0u
				|| ownerRecord.primitiveIndexCount == 0u
				|| ownerRecord.vertexCount > kMeshletMaxVertices
				|| (ownerRecord.primitiveIndexCount % 3u) != 0u)
			{
				return false;
			}

			const std::size_t ownerPrimitiveOffset = static_cast<std::size_t>(ownerRecord.primitiveIndexOffset);
			const std::size_t ownerPrimitiveSize = static_cast<std::size_t>(ownerRecord.primitiveIndexCount);
			if (ownerPrimitiveOffset + ownerPrimitiveSize > cpuMesh->meshletPrimitiveIndexData.size())
			{
				return false;
			}

			MeshletRecord localRecord{};
			localRecord.vertexOffset = static_cast<std::uint32_t>(localRemap.size());
			localRecord.vertexCount = ownerRecord.vertexCount;
			for (std::uint32_t vertexIndex = 0u; vertexIndex < ownerRecord.vertexCount; ++vertexIndex)
			{
				std::uint32_t ownerVertexIndex = 0u;
				if (!TryReadVirtualGeometryRemapIndex(*cpuMesh, ownerRecord.vertexOffset + vertexIndex, ownerVertexIndex))
				{
					return false;
				}

				auto [it, inserted] = localVertexByOwnerIndex.emplace(ownerVertexIndex, static_cast<std::uint32_t>(localVertexBytes.size() / kMeshletPackedVertexStrideBytes));
				if (inserted)
				{
					const std::size_t ownerVertexByteOffset = static_cast<std::size_t>(ownerVertexIndex) * kMeshletPackedVertexStrideBytes;
					if (ownerVertexByteOffset + kMeshletPackedVertexStrideBytes > cpuMesh->meshletVertexData.size())
					{
						return false;
					}
					localVertexBytes.insert(
						localVertexBytes.end(),
						cpuMesh->meshletVertexData.begin() + static_cast<std::ptrdiff_t>(ownerVertexByteOffset),
						cpuMesh->meshletVertexData.begin() + static_cast<std::ptrdiff_t>(ownerVertexByteOffset + kMeshletPackedVertexStrideBytes));
				}
				localRemap.push_back(it->second);
			}

			localRecord.primitiveIndexOffset = static_cast<std::uint32_t>(localPrimitiveBytes.size());
			localRecord.primitiveIndexCount = ownerRecord.primitiveIndexCount;
			localPrimitiveBytes.insert(
				localPrimitiveBytes.end(),
				cpuMesh->meshletPrimitiveIndexData.begin() + static_cast<std::ptrdiff_t>(ownerPrimitiveOffset),
				cpuMesh->meshletPrimitiveIndexData.begin() + static_cast<std::ptrdiff_t>(ownerPrimitiveOffset + ownerPrimitiveSize));
			localMeshletRecords.push_back(localRecord);

			if (ownerMeshletIndex < cpuMesh->meshletBounds.size())
			{
				localBounds.push_back(cpuMesh->meshletBounds[ownerMeshletIndex]);
			}
			else
			{
				MeshletBounds fallbackBounds{};
				fallbackBounds.coneCutoff = 2.0f;
				localBounds.push_back(fallbackBounds);
			}
		}

		if (localVertexBytes.empty() || localPrimitiveBytes.empty() || localRemap.empty() || localMeshletRecords.empty())
		{
			return false;
		}

		VirtualGeometryPageLocalMeshDecodeInfo decodeInfo{};
		decodeInfo.positionOffset[0] = cpuMesh->runtimeLayout.quantization.positionOffset[0];
		decodeInfo.positionOffset[1] = cpuMesh->runtimeLayout.quantization.positionOffset[1];
		decodeInfo.positionOffset[2] = cpuMesh->runtimeLayout.quantization.positionOffset[2];
		decodeInfo.positionScale[0] = cpuMesh->runtimeLayout.quantization.positionScale[0];
		decodeInfo.positionScale[1] = cpuMesh->runtimeLayout.quantization.positionScale[1];
		decodeInfo.positionScale[2] = cpuMesh->runtimeLayout.quantization.positionScale[2];
		decodeInfo.uvOffsetScale[0] = cpuMesh->runtimeLayout.quantization.uvOffset[0];
		decodeInfo.uvOffsetScale[1] = cpuMesh->runtimeLayout.quantization.uvOffset[1];
		decodeInfo.uvOffsetScale[2] = cpuMesh->runtimeLayout.quantization.uvScale[0];
		decodeInfo.uvOffsetScale[3] = cpuMesh->runtimeLayout.quantization.uvScale[1];
		decodeInfo.counts[0] = static_cast<std::uint32_t>(localVertexBytes.size() / kMeshletPackedVertexStrideBytes);
		decodeInfo.counts[1] = static_cast<std::uint32_t>(localRemap.size());
		decodeInfo.counts[2] = static_cast<std::uint32_t>(localPrimitiveBytes.size());
		decodeInfo.counts[3] = static_cast<std::uint32_t>(localMeshletRecords.size());

		const std::size_t payloadStartWords = words.size();
		const std::uint32_t vertexWordOffset = AppendVirtualGeometryPayloadBytesAsWords(words, localVertexBytes.data(), localVertexBytes.size());
		const std::uint32_t primitiveWordOffset = AppendVirtualGeometryPayloadBytesAsWords(words, localPrimitiveBytes.data(), localPrimitiveBytes.size());
		const std::uint32_t remapWordOffset = AppendVirtualGeometryPayloadBytesAsWords(words, localRemap.data(), localRemap.size() * sizeof(std::uint32_t));
		const std::uint32_t meshletWordOffset = AppendVirtualGeometryPayloadBytesAsWords(words, localMeshletRecords.data(), localMeshletRecords.size() * sizeof(MeshletRecord));
		const std::uint32_t boundsWordOffset = AppendVirtualGeometryPayloadBytesAsWords(words, localBounds.data(), localBounds.size() * sizeof(MeshletBounds));
		const std::uint32_t decodeWordOffset = AppendVirtualGeometryPayloadBytesAsWords(words, &decodeInfo, sizeof(decodeInfo));

		// Appending the variable-sized payload above can reallocate the vector.
		// Reacquire the descriptor row by index so the shader-visible table is
		// written into the live page-payload buffer, not a stale data pointer.
		std::uint32_t* descriptor = words.data() + descriptorWordOffset;
		descriptor[kVirtualGeometryPagePayloadDescriptorVertexWordOffset] = vertexWordOffset;
		descriptor[kVirtualGeometryPagePayloadDescriptorVertexWordSize] = static_cast<std::uint32_t>((localVertexBytes.size() + sizeof(std::uint32_t) - 1u) / sizeof(std::uint32_t));
		descriptor[kVirtualGeometryPagePayloadDescriptorPrimitiveWordOffset] = primitiveWordOffset;
		descriptor[kVirtualGeometryPagePayloadDescriptorPrimitiveByteSize] = static_cast<std::uint32_t>(localPrimitiveBytes.size());
		descriptor[kVirtualGeometryPagePayloadDescriptorRemapWordOffset] = remapWordOffset;
		descriptor[kVirtualGeometryPagePayloadDescriptorRemapCount] = static_cast<std::uint32_t>(localRemap.size());
		descriptor[kVirtualGeometryPagePayloadDescriptorMeshletWordOffset] = meshletWordOffset;
		descriptor[kVirtualGeometryPagePayloadDescriptorMeshletCount] = static_cast<std::uint32_t>(localMeshletRecords.size());
		descriptor[kVirtualGeometryPagePayloadDescriptorBoundsWordOffset] = boundsWordOffset;
		descriptor[kVirtualGeometryPagePayloadDescriptorBoundsCount] = static_cast<std::uint32_t>(localBounds.size());
		descriptor[kVirtualGeometryPagePayloadDescriptorDecodeWordOffset] = decodeWordOffset;
		descriptor[kVirtualGeometryPagePayloadDescriptorMaterialWordOffset] = 0u;
		descriptor[kVirtualGeometryPagePayloadDescriptorMaterialCount] = 0u;
		descriptor[kVirtualGeometryPagePayloadDescriptorLocalVertexCount] = decodeInfo.counts[0];
		descriptor[kVirtualGeometryPagePayloadDescriptorLocalPrimitiveByteCount] = decodeInfo.counts[2];
		descriptor[kVirtualGeometryPagePayloadDescriptorLocalRemapEncoding] = 0u;
		if (outAppendedGpuBytes != nullptr)
		{
			*outAppendedGpuBytes = static_cast<std::uint64_t>(words.size() - payloadStartWords)
				* static_cast<std::uint64_t>(sizeof(std::uint32_t));
		}
		return true;
	}

	VirtualGeometryPageLocalPayloadBuildResult BuildVirtualGeometryPageLocalPayloadWords(const std::vector<GpuVirtualGeometryPacketRecord>& packets)
	{
		std::uint32_t descriptorCount = static_cast<std::uint32_t>(packets.size());
		for (const GpuVirtualGeometryPacketRecord& packet : packets)
		{
			descriptorCount = std::max(descriptorCount, packet.packetId + 1u);
		}

		VirtualGeometryPageLocalPayloadBuildResult result;
		result.words.assign(
			static_cast<std::size_t>(descriptorCount) * kVirtualGeometryPagePayloadDescriptorStrideWords,
			0u);
		std::vector<bool> descriptorBytesAttributed(descriptorCount, false);
		for (const GpuVirtualGeometryPacketRecord& packet : packets)
		{
			const std::uint32_t descriptorIndex = std::min(packet.packetId, descriptorCount - 1u);
			std::uint32_t* descriptor = result.words.data()
				+ static_cast<std::size_t>(descriptorIndex) * kVirtualGeometryPagePayloadDescriptorStrideWords;
			descriptor[0] = packet.pageId;
			descriptor[1] = packet.fallbackPageId;
			descriptor[2] = static_cast<std::uint32_t>(packet.payloadSource);
			descriptor[3] = static_cast<std::uint32_t>(packet.emittedSource);
			descriptor[4] = static_cast<std::uint32_t>(packet.pagePayloadByteOffset & 0xffffffffull);
			descriptor[5] = static_cast<std::uint32_t>((packet.pagePayloadByteOffset >> 32u) & 0xffffffffull);
			descriptor[6] = static_cast<std::uint32_t>(packet.pagePayloadByteSize & 0xffffffffull);
			descriptor[7] = static_cast<std::uint32_t>((packet.pagePayloadByteSize >> 32u) & 0xffffffffull);
			descriptor[8] = packet.payloadBufferIndex;
			descriptor[9] = packet.payloadDescriptorIndex != std::numeric_limits<std::uint32_t>::max()
				? packet.payloadDescriptorIndex
				: descriptorIndex;
			descriptor[10] = packet.meshletOffset;
			descriptor[11] = packet.meshletCount;
			descriptor[12] = packet.selectedLod;
			descriptor[13] = packet.clusterId;
			descriptor[14] = packet.packetId;
			descriptor[15] = packet.flags;
			if (!descriptorBytesAttributed[descriptorIndex])
			{
				// The descriptor row is uploaded into the same physical payload
				// buffer as the stream bytes, so attribute that exact GPU cost
				// to the mesh that owns the packet.
				AddVirtualGeometryPagePayloadBytes(
					result.bytesByMesh,
					packet.meshId,
					static_cast<std::uint64_t>(kVirtualGeometryPagePayloadDescriptorStrideWords)
						* static_cast<std::uint64_t>(sizeof(std::uint32_t)));
				descriptorBytesAttributed[descriptorIndex] = true;
			}
			std::uint64_t appendedGpuBytes = 0u;
			if (AppendVirtualGeometryPageLocalPacketPayload(result.words, descriptorIndex, packet, &appendedGpuBytes))
			{
				AddVirtualGeometryPagePayloadBytes(result.bytesByMesh, packet.meshId, appendedGpuBytes);
			}
		}
		return result;
	}

	// HashClusterRangeValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void HashClusterRangeValue(std::uint64_t& hashValue, std::uint64_t input)
	{
		hashValue ^= input;
		hashValue *= kClusterRangeHashPrime;
	}

	// RenderViewTypeToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* RenderViewTypeToString(RenderViewType viewType)
	{
		return ToString(viewType);
	}

	// QualityPresetToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* QualityPresetToString(QualityPreset preset)
	{
		switch (preset)
		{
		case QualityPreset::Low:
			return "Low";
		case QualityPreset::Medium:
			return "Medium";
		case QualityPreset::High:
			return "High";
		case QualityPreset::Ultra:
			return "Ultra";
		case QualityPreset::Custom:
			return "Custom";
		default:
			return "Unknown";
		}
	}

	// LightingReductionCategoryToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* LightingReductionCategoryToString(LightingReductionCategory category)
	{
		switch (category)
		{
		case LightingReductionCategory::None:
			return "None";
		case LightingReductionCategory::Uploaded:
			return "Uploaded";
		case LightingReductionCategory::Special:
			return "Special";
		case LightingReductionCategory::Retained:
			return "Retained";
		case LightingReductionCategory::Shadow:
			return "Shadow";
		default:
			return "Unknown";
		}
	}

	// ShadowResourceKindToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* ShadowResourceKindToString(ShadowResourceKind kind)
	{
		switch (kind)
		{
		case ShadowResourceKind::DirectionalCascade:
			return "DirectionalCascade";
		case ShadowResourceKind::PointCube:
			return "PointCube";
		case ShadowResourceKind::SpotAtlas:
			return "SpotAtlas";
		case ShadowResourceKind::AreaAtlas:
			return "AreaAtlas";
		default:
			return "Unsupported";
		}
	}

	// ShadowQualityTierToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* ShadowQualityTierToString(ShadowQualityTier tier)
	{
		switch (tier)
		{
		case ShadowQualityTier::FullResolution:
			return "FullResolution";
		case ShadowQualityTier::ReducedResolution:
			return "ReducedResolution";
		case ShadowQualityTier::CachedOnly:
			return "CachedOnly";
		case ShadowQualityTier::Disabled:
			return "Disabled";
		default:
			return "Unknown";
		}
	}

	// ShadowDegradationReasonToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* ShadowDegradationReasonToString(ShadowDegradationReason reason)
	{
		switch (reason)
		{
		case ShadowDegradationReason::None:
			return "None";
		case ShadowDegradationReason::LightBudget:
			return "LightBudget";
		case ShadowDegradationReason::StorageBudget:
			return "StorageBudget";
		case ShadowDegradationReason::AllocationFailure:
			return "AllocationFailure";
		case ShadowDegradationReason::UnsupportedLightType:
			return "UnsupportedLightType";
		default:
			return "Unknown";
		}
	}

	// ShadowRefreshReasonToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* ShadowRefreshReasonToString(ShadowRefreshReason reason)
	{
		switch (reason)
		{
		case ShadowRefreshReason::RenderedThisFrame:
			return "RenderedThisFrame";
		case ShadowRefreshReason::CacheHit:
			return "CacheHit";
		case ShadowRefreshReason::LightMoved:
			return "LightMoved";
		case ShadowRefreshReason::ReceiverMoved:
			return "ReceiverMoved";
		case ShadowRefreshReason::CacheInvalidated:
			return "CacheInvalidated";
		case ShadowRefreshReason::Unavailable:
			return "Unavailable";
		default:
			return "Unknown";
		}
	}

	// BuildHashHex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string BuildHashHex(std::uint64_t hashValue)
	{
		std::ostringstream stream;
		stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << hashValue;
		return stream.str();
	}

	// IsTruthyEnvironmentValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsTruthyEnvironmentValue(const char* value)
	{
		if (value == nullptr || value[0] == '\0')
		{
			return false;
		}

		std::string normalizedValue;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const char* cursor = value; *cursor != '\0'; ++cursor)
		{
			normalizedValue.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*cursor))));
		}

		return normalizedValue == "1"
			|| normalizedValue == "true"
			|| normalizedValue == "yes"
			|| normalizedValue == "on";
	}

	// IsClusterValidationCaptureEnabledFromEnvironment is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsClusterValidationCaptureEnabledFromEnvironment()
	{
		return IsTruthyEnvironmentValue(std::getenv("TERRAENGINE_CLUSTER_VALIDATION_CAPTURE"));
	}
}

// VulkanContext::Impl::CreateDefaultTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateDefaultTexture() {
	// 1. Create Default White Texture (Index 0)
	{
		uint32_t whitePixel = 0xFFFFFFFF;
		VkDeviceSize imageSize = 4;
		VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Generic Staging Buffer");
		WriteMappedBuffer(allocator, stagingBuffer, &whitePixel, imageSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = {1, 1, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		vmaCreateImage(allocator, &imageInfo, &allocInfo, &defaultTexture.image, &defaultTexture.allocation, nullptr);
		vmaSetAllocationName(allocator, defaultTexture.allocation, "Default White Texture");

		TransitionImageLayout(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, defaultTexture.image, 1, 1);
		TransitionImageLayout(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		defaultTexture.view = CreateImageView(defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
		
		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		vkCreateSampler(device, &samplerInfo, nullptr, &defaultTexture.sampler);

		uint32_t idx = AllocateTextureIndex(); // Should be 0
		UpdateGlobalTexture(idx, defaultTexture);
	}

	// 2. Create Default Normal Texture (Index 1)
	{
		uint32_t bluePixel = 0xFFFF7F7F; // (127, 127, 255, 255) -> Flat Normal
		VkDeviceSize imageSize = 4;
		VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Default Normal Staging Buffer");
		WriteMappedBuffer(allocator, stagingBuffer, &bluePixel, imageSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = {1, 1, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		vmaCreateImage(allocator, &imageInfo, &allocInfo, &defaultNormalTexture.image, &defaultNormalTexture.allocation, nullptr);

		TransitionImageLayout(defaultNormalTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, defaultNormalTexture.image, 1, 1);
		TransitionImageLayout(defaultNormalTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		defaultNormalTexture.view = CreateImageView(defaultNormalTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
		
		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		vkCreateSampler(device, &samplerInfo, nullptr, &defaultNormalTexture.sampler);

		uint32_t idx = AllocateTextureIndex(); // Should be 1
		UpdateGlobalTexture(idx, defaultNormalTexture);
	}

	// 3. Create Default Material Mask Texture (Index 2)
	{
		// Mask channels follow the fragment shader convention:
		// R = occlusion, G = roughness, B = metallic, A = unused. A missing
		// mask should describe a fully visible, rough, non-metal surface so
		// untextured primitives receive ordinary diffuse lighting.
		uint32_t maskPixel = 0xFF00FFFF; // (255, 255, 0, 255)
		VkDeviceSize imageSize = 4;
		VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Default Material Mask Staging Buffer");
		WriteMappedBuffer(allocator, stagingBuffer, &maskPixel, imageSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = {1, 1, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		vmaCreateImage(allocator, &imageInfo, &allocInfo, &defaultMaterialMaskTexture.image, &defaultMaterialMaskTexture.allocation, nullptr);
		vmaSetAllocationName(allocator, defaultMaterialMaskTexture.allocation, "Default Material Mask Texture");

		TransitionImageLayout(defaultMaterialMaskTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, defaultMaterialMaskTexture.image, 1, 1);
		TransitionImageLayout(defaultMaterialMaskTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		defaultMaterialMaskTexture.view = CreateImageView(defaultMaterialMaskTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		vkCreateSampler(device, &samplerInfo, nullptr, &defaultMaterialMaskTexture.sampler);

		uint32_t idx = AllocateTextureIndex(); // Should be 2
		UpdateGlobalTexture(idx, defaultMaterialMaskTexture);
	}
}

// VulkanContext::Impl::CreateBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VulkanBuffer VulkanContext::Impl::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, const char* name) {
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = memoryUsage;

	VulkanBuffer buffer;
	buffer.size = size;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, nullptr) != VK_SUCCESS) {
		throw std::runtime_error("failed to create buffer!");
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name) {
		vmaSetAllocationName(allocator, buffer.allocation, name);
	}

	return buffer;
}

// VulkanContext::Impl::DestroyBufferImmediate is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyBufferImmediate(VulkanBuffer& buffer)
{
	if (buffer.buffer != VK_NULL_HANDLE && buffer.allocation != VK_NULL_HANDLE)
	{
		vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
	}

	buffer = {};
}

// VulkanContext::Impl::RetireBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RetireBuffer(VulkanBuffer& buffer)
{
	if (buffer.buffer == VK_NULL_HANDLE || buffer.allocation == VK_NULL_HANDLE)
	{
		buffer = {};
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (retiredBuffersByFrame.size() < MAX_FRAMES_IN_FLIGHT)
	{
		retiredBuffersByFrame.resize(MAX_FRAMES_IN_FLIGHT);
	}

	// Shared buffers can still be referenced by other frames already queued on
	// the GPU. Defer destruction until this frame slot comes back around and
	// its fence has been waited, which guarantees the old submissions are done.
	retiredBuffersByFrame[currentFrame].push_back(buffer);
	buffer = {};
}

void VulkanContext::Impl::RetireVulkanAnimationFrameResources(VulkanAnimationFrameResources& resources)
{
	// Frame fences, rather than CPU pose readback, own GPU animation lifetime.
	// RetireBuffer places every old allocation in the existing frame-slot domain
	// so a replacement cannot free memory referenced by an earlier submission.
	for(VulkanBuffer& buffer : resources.buffers) {
		RetireBuffer(buffer);
	}
	RetireBuffer(resources.graphicsBridgeBuffer);
	// Bindless slots are frame-resource identities. Returning them only with the
	// retired buffers prevents a graphics draw from observing a recycled index
	// while an older frame can still sample its original animation allocation.
	const std::array<std::uint32_t, 4U> bindlessIndices{
		resources.generationRecordsBindlessIndex,
		resources.workPacketsBindlessIndex,
		resources.paletteMatricesBindlessIndex,
		resources.morphOutputsBindlessIndex };
	for(const std::uint32_t index : bindlessIndices) {
		if(index != INVALID_BINDLESS_BUFFER_INDEX) freeBufferIndices.push_back(index);
	}
	if(resources.graphicsBridgeBindlessIndex != INVALID_BINDLESS_BUFFER_INDEX)
		freeBufferIndices.push_back(resources.graphicsBridgeBindlessIndex);
	if(activeAnimationGraphicsBufferIndex == resources.graphicsBridgeBindlessIndex)
		activeAnimationGraphicsBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	resources = {};
}

void VulkanContext::Impl::RetireVulkanCpuAnimationFrameResources(
	VulkanCpuAnimationFrameResources& resources)
{
	RetireBuffer(resources.paletteBuffer);
	RetireBuffer(resources.morphWeightBuffer);
	RetireBuffer(resources.graphicsBridgeBuffer);
	const std::array<std::uint32_t, 3U> bindlessIndices{
		resources.paletteBindlessIndex,
		resources.morphWeightBindlessIndex,
		resources.graphicsBridgeBindlessIndex};
	for(const std::uint32_t index : bindlessIndices) {
		if(index != INVALID_BINDLESS_BUFFER_INDEX) {
			freeBufferIndices.push_back(index);
		}
	}
	if(activeCpuAnimationGraphicsBufferIndex
		== resources.graphicsBridgeBindlessIndex) {
		activeCpuAnimationGraphicsBufferIndex =
			INVALID_BINDLESS_BUFFER_INDEX;
	}
	resources = {};
}

bool VulkanContext::Impl::PublishVulkanCpuAnimationResources(
	const TerraEngine::Renderer::CpuAnimationDeformationPlan& plan)
{
	activeCpuAnimationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	if(!plan.IsComplete() || currentFrame >= MAX_FRAMES_IN_FLIGHT
		|| plan.records.size() > std::numeric_limits<std::uint32_t>::max()) {
		return false;
	}
	const bool currentFrameHasRecordedCommands =
		currentFrame < pendingPrimaryCommandBuffers.size()
		&& !pendingPrimaryCommandBuffers[currentFrame].empty();
	const bool currentFrameWasSubmitted =
		currentFrame < pendingPrimaryCommandBuffersSubmitted.size()
		&& pendingPrimaryCommandBuffersSubmitted[currentFrame] != 0U;
	if(currentFrameHasRecordedCommands || currentFrameWasSubmitted) {
		// Global bindless descriptors are mutable host state. CPU deformation is
		// published once before any view records, never between Scene/Game views.
		return false;
	}

	std::size_t paletteCount = 0U;
	std::size_t morphWeightCount = 0U;
	for(const TerraEngine::Renderer::CpuAnimationDeformationRecord& record
		: plan.records) {
		if(!record.IsComplete()
			|| record.rendererGeneration != plan.rendererGeneration
			|| record.data->paletteMatrices.size()
				> std::numeric_limits<std::size_t>::max() - paletteCount
			|| record.data->morphWeights.size()
				> std::numeric_limits<std::size_t>::max() - morphWeightCount) {
			return false;
		}
		paletteCount += record.data->paletteMatrices.size();
		morphWeightCount += record.data->morphWeights.size();
	}
	if(paletteCount == 0U
		|| paletteCount > std::numeric_limits<VkDeviceSize>::max()
			/ sizeof(TerraEngine::Animation::GpuAnimationPaletteRecord)
		|| morphWeightCount > std::numeric_limits<VkDeviceSize>::max()
			/ sizeof(float)
		|| plan.records.size() > std::numeric_limits<VkDeviceSize>::max()
			/ sizeof(VulkanGpuAnimationGraphicsRecord)) {
		return false;
	}
	const VkDeviceSize paletteBytes = static_cast<VkDeviceSize>(
		paletteCount
			* sizeof(TerraEngine::Animation::GpuAnimationPaletteRecord));
	const VkDeviceSize morphWeightBytes = static_cast<VkDeviceSize>(
		morphWeightCount * sizeof(float));
	const VkDeviceSize bridgeBytes = static_cast<VkDeviceSize>(
		plan.records.size() * sizeof(VulkanGpuAnimationGraphicsRecord));
	if(maximumStorageBufferRange == 0U
		|| paletteBytes > maximumStorageBufferRange
		|| morphWeightBytes > maximumStorageBufferRange
		|| bridgeBytes == 0U || bridgeBytes > maximumStorageBufferRange) {
		return false;
	}

	std::vector<TerraEngine::Animation::GpuAnimationPaletteRecord> palettes;
	std::vector<float> morphWeights;
	std::vector<VulkanGpuAnimationGraphicsRecord> bridgeRecords;
	try {
		palettes.reserve(paletteCount);
		morphWeights.reserve(morphWeightCount);
		bridgeRecords.reserve(plan.records.size());
		std::uint64_t paletteOffsetBytes = 0U;
		std::uint64_t morphOffsetBytes = 0U;
		const std::uint32_t planGeneration = static_cast<std::uint32_t>(
			(plan.frameId % std::numeric_limits<std::uint32_t>::max()) + 1U);
		for(std::size_t recordIndex = 0U;
			recordIndex < plan.records.size(); ++recordIndex) {
			const TerraEngine::Renderer::CpuAnimationDeformationRecord& source =
				plan.records[recordIndex];
			palettes.insert(palettes.end(),
				source.data->paletteMatrices.begin(),
				source.data->paletteMatrices.end());
			morphWeights.insert(morphWeights.end(),
				source.data->morphWeights.begin(),
				source.data->morphWeights.end());

			VulkanGpuAnimationGraphicsRecord bridge{};
			bridge.planGeneration = planGeneration;
			bridge.rendererGeneration = static_cast<std::uint32_t>(
				std::min<std::uint64_t>(
					plan.rendererGeneration,
					std::numeric_limits<std::uint32_t>::max()));
			bridge.workPacketIndex =
				static_cast<std::uint32_t>(recordIndex);
			bridge.generationRecordIndex = 0U;
			bridge.paletteOffsetBytes = paletteOffsetBytes;
			bridge.morphOutputOffsetBytes = morphOffsetBytes;
			bridge.jointCount = source.data->jointCount;
			bridge.morphCount = static_cast<std::uint32_t>(
				source.data->morphWeights.size());
			bridge.paletteEntryCount = static_cast<std::uint32_t>(
				source.data->paletteMatrices.size());
			bridge.stageMask = TerraEngine::Animation::GpuAnimationStageMask(
				TerraEngine::Animation::GpuAnimationStage::Palette);
			if(bridge.morphCount != 0U) {
				bridge.stageMask |=
					TerraEngine::Animation::GpuAnimationStageMask(
						TerraEngine::Animation::GpuAnimationStage::Morph);
			}
			bridge.validMarker = 1U;
			bridgeRecords.push_back(bridge);
			paletteOffsetBytes += source.data->paletteMatrices.size()
				* sizeof(TerraEngine::Animation::GpuAnimationPaletteRecord);
			morphOffsetBytes += source.data->morphWeights.size()
				* sizeof(float);
		}
	} catch(const std::bad_alloc&) {
		return false;
	}

	if(cpuAnimationFrameResources.size() < MAX_FRAMES_IN_FLIGHT) {
		cpuAnimationFrameResources.resize(MAX_FRAMES_IN_FLIGHT);
	}
	VulkanCpuAnimationFrameResources& resources =
		cpuAnimationFrameResources[currentFrame];
	if(resources.live
		&& resources.rendererGeneration != plan.rendererGeneration) {
		RetireVulkanCpuAnimationFrameResources(resources);
	}
	const auto ensureBuffer = [this](VulkanBuffer& buffer,
		VkDeviceSize& capacity, VkDeviceSize requiredBytes,
		const char* name) {
		const VkDeviceSize safeBytes = std::max<VkDeviceSize>(
			requiredBytes, 16U);
		if(buffer.buffer == VK_NULL_HANDLE || capacity < safeBytes) {
			RetireBuffer(buffer);
			buffer = CreateBuffer(safeBytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_CPU_TO_GPU, name);
			capacity = safeBytes;
		}
	};
	try {
		ensureBuffer(resources.paletteBuffer,
			resources.paletteCapacityBytes, paletteBytes,
			"CPU Animation Palette Buffer");
		ensureBuffer(resources.morphWeightBuffer,
			resources.morphWeightCapacityBytes,
			std::max<VkDeviceSize>(morphWeightBytes, 16U),
			"CPU Animation Morph Weight Buffer");
		ensureBuffer(resources.graphicsBridgeBuffer,
			resources.graphicsBridgeCapacityBytes, bridgeBytes,
			"CPU Animation Graphics Bridge Buffer");
	} catch(const std::exception&) {
		RetireVulkanCpuAnimationFrameResources(resources);
		return false;
	}
	if(!WriteMappedBuffer(allocator, resources.paletteBuffer,
			palettes.data(), paletteBytes)
		|| (morphWeightBytes != 0U
			&& !WriteMappedBuffer(allocator,
				resources.morphWeightBuffer,
				morphWeights.data(), morphWeightBytes))) {
		RetireVulkanCpuAnimationFrameResources(resources);
		return false;
	}

	if(resources.paletteBindlessIndex == INVALID_BINDLESS_BUFFER_INDEX) {
		resources.paletteBindlessIndex = AllocateBufferIndex();
	}
	if(resources.morphWeightBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX) {
		resources.morphWeightBindlessIndex = AllocateBufferIndex();
	}
	for(VulkanGpuAnimationGraphicsRecord& bridge : bridgeRecords) {
		bridge.paletteMatricesBindlessIndex =
			resources.paletteBindlessIndex;
		bridge.morphOutputsBindlessIndex =
			resources.morphWeightBindlessIndex;
	}
	if(!WriteMappedBuffer(allocator, resources.graphicsBridgeBuffer,
			bridgeRecords.data(), bridgeBytes)) {
		RetireVulkanCpuAnimationFrameResources(resources);
		return false;
	}
	if(resources.graphicsBridgeBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX) {
		resources.graphicsBridgeBindlessIndex = AllocateBufferIndex();
	}
	UpdateGlobalBufferForFrame(currentFrame,
		resources.paletteBindlessIndex, resources.paletteBuffer.buffer,
		0U, paletteBytes);
	UpdateGlobalBufferForFrame(currentFrame,
		resources.morphWeightBindlessIndex,
		resources.morphWeightBuffer.buffer, 0U,
		std::max<VkDeviceSize>(morphWeightBytes, 16U));
	UpdateGlobalBufferForFrame(currentFrame,
		resources.graphicsBridgeBindlessIndex,
		resources.graphicsBridgeBuffer.buffer, 0U, bridgeBytes);
	resources.frameId = plan.frameId;
	resources.rendererGeneration = plan.rendererGeneration;
	resources.live = true;
	activeCpuAnimationGraphicsBufferIndex =
		resources.graphicsBridgeBindlessIndex;
	return true;
}

bool VulkanContext::Impl::EnsureVulkanAnimationDescriptorSets()
{
	if(animationDescriptorSetLayout != VK_NULL_HANDLE
		&& animationDescriptorSets.size() == MAX_FRAMES_IN_FLIGHT) return true;
	if(device == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE) return false;

	std::array<VkDescriptorSetLayoutBinding, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount> bindings{};
	for(std::uint32_t binding = 0U; binding < bindings.size(); ++binding) {
		bindings[binding].binding = binding;
		bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[binding].descriptorCount = 1U;
		bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &animationDescriptorSetLayout) != VK_SUCCESS) {
		animationDescriptorSetLayout = VK_NULL_HANDLE;
		return false;
	}

	std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
	layouts.fill(animationDescriptorSetLayout);
	animationDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
	VkDescriptorSetAllocateInfo allocateInfo{};
	allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocateInfo.descriptorPool = descriptorPool;
	allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
	allocateInfo.pSetLayouts = layouts.data();
	if(vkAllocateDescriptorSets(device, &allocateInfo, animationDescriptorSets.data()) != VK_SUCCESS) {
		vkDestroyDescriptorSetLayout(device, animationDescriptorSetLayout, nullptr);
		animationDescriptorSetLayout = VK_NULL_HANDLE;
		animationDescriptorSets.clear();
		return false;
	}
	return true;
}

bool VulkanContext::Impl::EnsureVulkanDeformationDescriptorSets()
{
	if(deformationDescriptorSetLayout != VK_NULL_HANDLE
		&& deformationDescriptorSets.size()
			== MAX_FRAMES_IN_FLIGHT)
	{
		return true;
	}
	if(device == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE)
	{
		return false;
	}
	std::array<
		VkDescriptorSetLayoutBinding,
		TerraEngine::Renderer::Vulkan::
			kVulkanDeformationDescriptorBindingCount> bindings{};
	for(std::uint32_t binding = 0u;
		binding < bindings.size();
		++binding)
	{
		bindings[binding].binding = binding;
		bindings[binding].descriptorType =
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[binding].descriptorCount = 1u;
		bindings[binding].stageFlags =
			VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType =
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount =
		static_cast<std::uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(
		device,
		&layoutInfo,
		nullptr,
		&deformationDescriptorSetLayout) != VK_SUCCESS)
	{
		deformationDescriptorSetLayout = VK_NULL_HANDLE;
		return false;
	}
	std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT>
		layouts{};
	layouts.fill(deformationDescriptorSetLayout);
	deformationDescriptorSets.assign(
		MAX_FRAMES_IN_FLIGHT,
		VK_NULL_HANDLE);
	VkDescriptorSetAllocateInfo allocateInfo{};
	allocateInfo.sType =
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocateInfo.descriptorPool = descriptorPool;
	allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
	allocateInfo.pSetLayouts = layouts.data();
	if(vkAllocateDescriptorSets(
		device,
		&allocateInfo,
		deformationDescriptorSets.data()) != VK_SUCCESS)
	{
		vkDestroyDescriptorSetLayout(
			device,
			deformationDescriptorSetLayout,
			nullptr);
		deformationDescriptorSetLayout = VK_NULL_HANDLE;
		deformationDescriptorSets.clear();
		return false;
	}
	return true;
}

void VulkanContext::Impl::RetireVulkanDeformationFrameResources(
	VulkanDeformationFrameResources& resources)
{
	for(VulkanBuffer& buffer : resources.buffers)
	{
		RetireBuffer(buffer);
	}
	RetireBuffer(resources.graphicsBridgeBuffer);
	const std::array<std::uint32_t, 4u> bindlessIndices{
		resources.currentVerticesBindlessIndex,
		resources.boundsBindlessIndex,
		resources.refinementBindlessIndex,
		resources.graphicsBridgeBindlessIndex };
	for(const std::uint32_t index : bindlessIndices)
	{
		if(index != INVALID_BINDLESS_BUFFER_INDEX)
		{
			freeBufferIndices.push_back(index);
		}
	}
	if(activeDeformationGraphicsBufferIndex
		== resources.graphicsBridgeBindlessIndex)
	{
		activeDeformationGraphicsBufferIndex =
			INVALID_BINDLESS_BUFFER_INDEX;
	}
	resources = {};
}

bool VulkanContext::Impl::PublishVulkanDeformationResources(
	const TerraEngine::Renderer::GpuDeformationPlan& plan)
{
	using DescriptorBinding =
		TerraEngine::Renderer::Vulkan::
			VulkanDeformationDescriptorBinding;
	if(plan.work.empty())
	{
		return plan.valid;
	}
	if(!plan.IsComplete()
		|| currentFrame >= MAX_FRAMES_IN_FLIGHT
		|| (currentFrame < pendingPrimaryCommandBuffers.size()
			&& !pendingPrimaryCommandBuffers[currentFrame].empty())
		|| (currentFrame
				< pendingPrimaryCommandBuffersSubmitted.size()
			&& pendingPrimaryCommandBuffersSubmitted[currentFrame]
				!= 0u))
	{
		return false;
	}
	const auto descriptorPlan =
		TerraEngine::Renderer::Vulkan::
			BuildVulkanDeformationDescriptorSetPlan(
				plan,
				maximumStorageBufferRange);
	if(!descriptorPlan.valid || !descriptorPlan.hasWork
		|| !EnsureVulkanDeformationDescriptorSets())
	{
		return false;
	}

	enum class PaletteAuthority
	{
		Unknown,
		Gpu,
		Cpu
	};
	PaletteAuthority authority = PaletteAuthority::Unknown;
	VkBuffer paletteBuffer = VK_NULL_HANDLE;
	VkDeviceSize paletteBytes = 0u;
	VkBuffer morphBuffer = VK_NULL_HANDLE;
	VkDeviceSize morphBytes = 0u;
	std::vector<TerraEngine::Renderer::
		GpuDeformationGenerationRecord> generations;
	std::vector<TerraEngine::Renderer::
		GpuDeformationShaderWorkRecord> shaderWork;
	std::vector<std::uint8_t> staticPageBytes;
	std::vector<TerraEngine::Renderer::DeformedPageKey> workKeys;
	struct RefinementRecord
	{
		std::uint32_t parentWorkIndex = 0u;
		std::uint32_t firstChildWorkIndex = 0u;
		std::uint32_t childCount = 0u;
		std::uint32_t selectedWorkIndex = 0u;
		float geometricError = 0.0f;
		float deformationError = 0.0f;
		float projectedScale = 1.0f;
		float thresholdPixels = 1.0f;
		std::uint32_t flags = 0x03u;
		std::uint32_t requestFeedbackIndex = 0u;
		std::array<std::uint32_t, 2u> reserved{};
	};
	static_assert(sizeof(RefinementRecord) == 48u);
	std::vector<RefinementRecord> refinement;
	try
	{
		generations.reserve(plan.work.size());
		shaderWork.reserve(plan.work.size());
		workKeys.reserve(plan.work.size());
		refinement.reserve(plan.totalClusterBoundsCount);
	}
	catch(const std::bad_alloc&)
	{
		return false;
	}

	const auto FindSection = [](
		const std::vector<TemfDeformablePageSection>& sections,
		TemfDeformablePageSectionKind kind)
		-> const TemfDeformablePageSection*
	{
		const auto found = std::find_if(
			sections.begin(),
			sections.end(),
			[kind](
				const TemfDeformablePageSection& section)
			{
				return section.kind == kind;
			});
		return found == sections.end() ? nullptr : &*found;
	};
	for(std::uint32_t workIndex = 0u;
		workIndex < plan.work.size();
		++workIndex)
	{
		const TerraEngine::Renderer::DeformationWorkRecord& source =
			plan.work[workIndex];
		const Mesh* mesh =
			ResourceManager::Instance().GetMesh(source.key.page.meshId);
		if(mesh == nullptr
			|| !mesh->hasDeformableDemandedPages)
		{
			return false;
		}
		const auto entryIt = std::find_if(
			mesh->deformableDemandedPageEntries.begin(),
			mesh->deformableDemandedPageEntries.end(),
			[&source](
				const TemfDeformableDemandedPageEntry& entry)
			{
				return entry.pageId == source.key.page.pageId
					&& entry.contentGeneration
						== source.key.page.pageContentGeneration;
			});
		if(entryIt == mesh->deformableDemandedPageEntries.end())
		{
			return false;
		}
		const std::size_t pageIndex = static_cast<std::size_t>(
			std::distance(mesh->deformableDemandedPageEntries.begin(), entryIt));
		if(pageIndex >= mesh->deformableDemandedPagePayloads.size()
			|| pageIndex >= mesh->deformableDemandedPageSections.size()
			|| staticPageBytes.size()
				> std::numeric_limits<std::uint32_t>::max())
		{
			return false;
		}
		const std::span<const std::uint8_t> blobBytes =
			mesh->deformableDemandedPagePayloads[pageIndex];
		const auto& sections =
			mesh->deformableDemandedPageSections[pageIndex];
		const TemfDeformablePageSection* vertices =
			FindSection(
				sections,
				TemfDeformablePageSectionKind::PackedVertices);
		const TemfDeformablePageSection* skin =
			FindSection(
				sections,
				TemfDeformablePageSectionKind::SkinRecords);
		const TemfDeformablePageSection* joints =
			FindSection(
				sections,
				TemfDeformablePageSectionKind::JointPalette);
		const TemfDeformablePageSection* morphTargets =
			FindSection(
				sections,
				TemfDeformablePageSectionKind::MorphTargets);
		const TemfDeformablePageSection* morphDeltas =
			FindSection(
				sections,
				TemfDeformablePageSectionKind::MorphDeltas);
		const TemfDeformablePageSection* decode =
			FindSection(
				sections,
				TemfDeformablePageSectionKind::DecodeConstants);
		if(vertices == nullptr
			|| skin == nullptr
			|| joints == nullptr
			|| decode == nullptr
			|| decode->byteSize != sizeof(QuantizationInfo)
			|| vertices->elementCount != source.vertexCount
			|| skin->elementCount != source.vertexCount)
		{
			return false;
		}
		const std::size_t alignedBase =
			(staticPageBytes.size() + 15u) & ~std::size_t{15u};
		if(alignedBase
				> std::numeric_limits<std::uint32_t>::max()
			|| blobBytes.size()
				> std::numeric_limits<std::uint32_t>::max()
					- alignedBase)
		{
			return false;
		}
		staticPageBytes.resize(alignedBase, 0u);
		const std::uint32_t blobBase =
			static_cast<std::uint32_t>(alignedBase);
		staticPageBytes.insert(
			staticPageBytes.end(),
			blobBytes.begin(),
			blobBytes.end());

		std::uint64_t paletteOffsetBytes = 0u;
		std::uint64_t morphOffsetBytes = 0u;
		if(pendingAnimationImmutablePlan != nullptr
			&& source.sourceRecordIndex
				< pendingAnimationImmutablePlan->workPackets.size()
			&& currentFrame < animationFrameResources.size())
		{
			if(authority == PaletteAuthority::Cpu)
			{
				return false;
			}
			authority = PaletteAuthority::Gpu;
			const auto& packet =
				pendingAnimationImmutablePlan->workPackets[
					source.sourceRecordIndex];
			paletteOffsetBytes = packet.paletteOffsetBytes;
			morphOffsetBytes = packet.morphOutputOffsetBytes;
			const auto& animationResources =
				animationFrameResources[currentFrame];
			const std::size_t paletteBinding =
				static_cast<std::size_t>(
					TerraEngine::Animation::
						GpuAnimationDescriptorBinding::
							PaletteMatrices);
			const std::size_t morphBinding =
				static_cast<std::size_t>(
					TerraEngine::Animation::
						GpuAnimationDescriptorBinding::
							MorphOutputs);
			paletteBuffer =
				animationResources.buffers[paletteBinding].buffer;
			paletteBytes =
				animationResources.buffers[paletteBinding].size;
			morphBuffer =
				animationResources.buffers[morphBinding].buffer;
			morphBytes =
				animationResources.buffers[morphBinding].size;
		}
		else if(pendingCpuAnimationPlan != nullptr
			&& currentFrame < cpuAnimationFrameResources.size())
		{
			if(authority == PaletteAuthority::Gpu)
			{
				return false;
			}
			authority = PaletteAuthority::Cpu;
			std::size_t recordIndex = 0u;
			for(;
				recordIndex
					< pendingCpuAnimationPlan->records.size();
				++recordIndex)
			{
				const auto& record =
					pendingCpuAnimationPlan->records[recordIndex];
				if(record.stableInstanceId
					== source.key.deformation.stableInstanceId)
				{
					break;
				}
				paletteOffsetBytes +=
					record.data->paletteMatrices.size()
					* sizeof(
						TerraEngine::Animation::
							GpuAnimationPaletteRecord);
				morphOffsetBytes +=
					record.data->morphWeights.size()
					* sizeof(float);
			}
			if(recordIndex
				== pendingCpuAnimationPlan->records.size())
			{
				return false;
			}
			const auto& cpuResources =
				cpuAnimationFrameResources[currentFrame];
			paletteBuffer = cpuResources.paletteBuffer.buffer;
			paletteBytes = cpuResources.paletteBuffer.size;
			morphBuffer = cpuResources.morphWeightBuffer.buffer;
			morphBytes = cpuResources.morphWeightBuffer.size;
		}
		else
		{
			return false;
		}
		if(paletteBuffer == VK_NULL_HANDLE
			|| paletteOffsetBytes % 16u != 0u
			|| morphOffsetBytes % sizeof(float) != 0u)
		{
			return false;
		}

		QuantizationInfo quantization{};
		std::memcpy(
			&quantization,
			blobBytes.data()
				+ static_cast<std::size_t>(decode->relativeByteOffset),
			sizeof(quantization));
		TerraEngine::Renderer::GpuDeformationGenerationRecord
			generation{};
		generation.stableInstanceId =
			source.key.deformation.stableInstanceId;
		generation.animationPublicationGeneration =
			source.key.deformation.animationPublicationGeneration;
		generation.pageContentGeneration =
			source.key.page.pageContentGeneration;
		generation.residencyGeneration =
			source.key.page.residencyGeneration;
		generation.rendererGeneration =
			source.key.deformation.rendererGeneration;
		generation.sceneGeneration =
			source.key.deformation.sceneGeneration;
		generation.meshPayloadGeneration =
			source.key.deformation.meshPayloadGeneration;
		generations.push_back(generation);

		TerraEngine::Renderer::GpuDeformationShaderWorkRecord
			work{};
		work.generationRecordIndex = workIndex;
		work.inputVertexByteOffset =
			blobBase
			+ vertices->relativeByteOffset;
		work.skinRecordByteOffset =
			blobBase
			+ skin->relativeByteOffset;
		work.morphDeltaByteOffset = morphDeltas == nullptr
				? 0u
				: blobBase
					+ morphDeltas->relativeByteOffset;
		work.vertexCount = source.vertexCount;
		work.jointCount = joints->elementCount;
		work.morphTargetCount = morphTargets == nullptr
			? 0u
			: morphTargets->elementCount;
		work.paletteRowOffset =
			static_cast<std::uint32_t>(
				paletteOffsetBytes / 16u);
		work.morphWeightOffset =
			static_cast<std::uint32_t>(
				morphOffsetBytes / sizeof(float));
		work.currentOutputOffset = source.vertexOffset;
		work.previousOutputOffset = source.vertexOffset;
		work.jointPaletteByteOffset =
			blobBase
			+ joints->relativeByteOffset;
		std::copy_n(
			quantization.positionOffset,
			3u,
			work.positionOffset.begin());
		std::copy_n(
			quantization.positionScale,
			3u,
			work.positionScale.begin());
		shaderWork.push_back(work);
		workKeys.push_back(source.key);
		for(std::uint32_t cluster = 0u;
			cluster < source.clusterCount;
			++cluster)
		{
			RefinementRecord record{};
			record.parentWorkIndex = workIndex;
			record.selectedWorkIndex = workIndex;
			record.requestFeedbackIndex =
				source.clusterBoundsOffset + cluster;
			record.geometricError = entryIt->geometricError;
			record.deformationError =
				entryIt->deformationError;
			refinement.push_back(record);
		}
	}
	if(staticPageBytes.empty()
		|| generations.size() != plan.work.size()
		|| shaderWork.size() != plan.work.size()
		|| refinement.size()
			!= plan.totalClusterBoundsCount
		|| paletteBytes == 0u)
	{
		return false;
	}

	if(deformationFrameResources.size()
		< MAX_FRAMES_IN_FLIGHT)
	{
		deformationFrameResources.resize(
			MAX_FRAMES_IN_FLIGHT);
	}
	VulkanDeformationFrameResources& resources =
		deformationFrameResources[currentFrame];
	if(resources.live
		&& resources.rendererGeneration
			!= plan.rendererGeneration)
	{
		RetireVulkanDeformationFrameResources(resources);
	}
	const std::uint32_t previousFrame =
		(currentFrame + MAX_FRAMES_IN_FLIGHT - 1u)
		% MAX_FRAMES_IN_FLIGHT;
	const VulkanDeformationFrameResources* previousResources =
		previousFrame < deformationFrameResources.size()
			? &deformationFrameResources[previousFrame]
			: nullptr;
	const bool historyCompatible =
		previousResources != nullptr
		&& previousResources->live
		&& previousResources->rendererGeneration
			== plan.rendererGeneration
		&& previousResources->workKeys == workKeys;
	for(std::size_t index = 0u;
		index < shaderWork.size();
		++index)
	{
		if(historyCompatible && plan.work[index].historyValid)
		{
			shaderWork[index].flags |= 0x01u;
		}
	}
	const auto EnsureBuffer = [this, &resources](
		std::size_t binding,
		VkDeviceSize requiredBytes,
		const char* name)
	{
		const VkDeviceSize safeBytes =
			std::max<VkDeviceSize>(requiredBytes, 16u);
		VulkanBuffer& buffer = resources.buffers[binding];
		if(buffer.buffer == VK_NULL_HANDLE
			|| resources.capacities[binding] < safeBytes)
		{
			RetireBuffer(buffer);
			buffer = CreateBuffer(
				safeBytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_CPU_TO_GPU,
				name);
			resources.capacities[binding] = safeBytes;
		}
		return buffer.buffer != VK_NULL_HANDLE;
	};
	const auto BindingIndex = [](DescriptorBinding binding)
	{
		return static_cast<std::size_t>(binding);
	};
	for(std::size_t binding = 0u;
		binding
			< TerraEngine::Renderer::Vulkan::
				kVulkanDeformationDescriptorBindingCount;
		++binding)
	{
		if(binding == BindingIndex(DescriptorBinding::Palettes)
			|| binding
				== BindingIndex(
					DescriptorBinding::MorphWeights)
			|| binding
				== BindingIndex(
					DescriptorBinding::PreviousVertices))
		{
			continue;
		}
		VkDeviceSize requiredBytes =
			descriptorPlan.bindings[binding].minimumByteCount;
		if(binding
			== BindingIndex(
				DescriptorBinding::StaticPageBytes))
		{
			requiredBytes = staticPageBytes.size();
		}
		else if(binding
			== BindingIndex(
				DescriptorBinding::Refinement))
		{
			requiredBytes =
				refinement.size() * sizeof(RefinementRecord);
		}
		if(!EnsureBuffer(
			binding,
			requiredBytes,
			"Animated Virtual Geometry Frame Buffer"))
		{
			RetireVulkanDeformationFrameResources(resources);
			return false;
		}
		if(!ClearMappedBuffer(
			allocator,
			resources.buffers[binding]))
		{
			RetireVulkanDeformationFrameResources(resources);
			return false;
		}
	}
	const auto Upload = [this, &resources](
		DescriptorBinding binding,
		const void* bytes,
		VkDeviceSize byteCount)
	{
		return byteCount == 0u
			|| WriteMappedBuffer(
				allocator,
				resources.buffers[
					static_cast<std::size_t>(binding)],
				bytes,
				byteCount);
	};
	if(!Upload(
			DescriptorBinding::Generations,
			generations.data(),
			generations.size() * sizeof(generations.front()))
		|| !Upload(
			DescriptorBinding::Work,
			shaderWork.data(),
			shaderWork.size() * sizeof(shaderWork.front()))
		|| !Upload(
			DescriptorBinding::StaticPageBytes,
			staticPageBytes.data(),
			staticPageBytes.size())
		|| !Upload(
			DescriptorBinding::Refinement,
			refinement.data(),
			refinement.size() * sizeof(refinement.front())))
	{
		RetireVulkanDeformationFrameResources(resources);
		return false;
	}

	std::array<
		VkDescriptorBufferInfo,
		TerraEngine::Renderer::Vulkan::
			kVulkanDeformationDescriptorBindingCount> infos{};
	std::array<
		VkWriteDescriptorSet,
		TerraEngine::Renderer::Vulkan::
			kVulkanDeformationDescriptorBindingCount> writes{};
	for(std::uint32_t binding = 0u;
		binding < infos.size();
		++binding)
	{
		infos[binding].buffer =
			resources.buffers[binding].buffer;
		infos[binding].offset = 0u;
		infos[binding].range =
			std::max<VkDeviceSize>(
				16u,
				resources.buffers[binding].size);
		if(binding == BindingIndex(DescriptorBinding::Palettes))
		{
			infos[binding].buffer = paletteBuffer;
			infos[binding].range = paletteBytes;
		}
		else if(binding
			== BindingIndex(DescriptorBinding::MorphWeights))
		{
			infos[binding].buffer =
				morphBuffer != VK_NULL_HANDLE
					? morphBuffer
					: paletteBuffer;
			infos[binding].range =
				morphBuffer != VK_NULL_HANDLE
					? std::max<VkDeviceSize>(16u, morphBytes)
					: 16u;
		}
		else if(binding
			== BindingIndex(
				DescriptorBinding::PreviousVertices))
		{
			const VulkanBuffer& previousBuffer =
				historyCompatible
					? previousResources->buffers[
						BindingIndex(
							DescriptorBinding::
								CurrentVertices)]
					: resources.buffers[
						BindingIndex(
							DescriptorBinding::
								CurrentVertices)];
			infos[binding].buffer = previousBuffer.buffer;
			infos[binding].range = previousBuffer.size;
		}
		if(infos[binding].buffer == VK_NULL_HANDLE
			|| infos[binding].range == 0u
			|| infos[binding].range > maximumStorageBufferRange)
		{
			RetireVulkanDeformationFrameResources(resources);
			return false;
		}
		writes[binding].sType =
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstSet =
			deformationDescriptorSets[currentFrame];
		writes[binding].dstBinding = binding;
		writes[binding].descriptorType =
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[binding].descriptorCount = 1u;
		writes[binding].pBufferInfo = &infos[binding];
	}
	vkUpdateDescriptorSets(
		device,
		static_cast<std::uint32_t>(writes.size()),
		writes.data(),
		0u,
		nullptr);

	// Publish only the canonical current/history/bounds/refinement products to
	// graphics. The bridge deliberately omits palettes, skin records, and morph
	// weights, keeping custom surface stages animation-agnostic.
	const std::size_t currentBinding =
		BindingIndex(DescriptorBinding::CurrentVertices);
	const std::size_t boundsBinding =
		BindingIndex(DescriptorBinding::ClusterBounds);
	const std::size_t refinementBinding =
		BindingIndex(DescriptorBinding::Refinement);
	if(resources.currentVerticesBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		resources.currentVerticesBindlessIndex =
			AllocateBufferIndex();
	}
	if(resources.boundsBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		resources.boundsBindlessIndex = AllocateBufferIndex();
	}
	if(resources.refinementBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		resources.refinementBindlessIndex =
			AllocateBufferIndex();
	}
	UpdateGlobalBufferForFrame(
		currentFrame,
		resources.currentVerticesBindlessIndex,
		resources.buffers[currentBinding].buffer,
		0u,
		resources.buffers[currentBinding].size);
	UpdateGlobalBufferForFrame(
		currentFrame,
		resources.boundsBindlessIndex,
		resources.buffers[boundsBinding].buffer,
		0u,
		resources.buffers[boundsBinding].size);
	UpdateGlobalBufferForFrame(
		currentFrame,
		resources.refinementBindlessIndex,
		resources.buffers[refinementBinding].buffer,
		0u,
		resources.buffers[refinementBinding].size);
	const std::uint32_t previousVerticesBindlessIndex =
		historyCompatible
			? previousResources->currentVerticesBindlessIndex
			: resources.currentVerticesBindlessIndex;
	if(previousVerticesBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		RetireVulkanDeformationFrameResources(resources);
		return false;
	}

	std::vector<std::uint32_t> orderedWork(plan.work.size());
	std::iota(orderedWork.begin(), orderedWork.end(), 0u);
	std::sort(
		orderedWork.begin(),
		orderedWork.end(),
		[&plan](std::uint32_t left, std::uint32_t right)
		{
			const auto& leftWork = plan.work[left];
			const auto& rightWork = plan.work[right];
			if(leftWork.key.deformation.stableInstanceId
				!= rightWork.key.deformation.stableInstanceId)
			{
				return leftWork.key.deformation.stableInstanceId
					< rightWork.key.deformation.stableInstanceId;
			}
			if(leftWork.key.page.meshId
				!= rightWork.key.page.meshId)
			{
				return leftWork.key.page.meshId
					< rightWork.key.page.meshId;
			}
			return leftWork.key.page.pageId
				< rightWork.key.page.pageId;
		});
	struct GroupRange
	{
		std::uint64_t stableInstanceId = 0u;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::size_t begin = 0u;
		std::size_t count = 0u;
	};
	std::vector<GroupRange> groups;
	for(std::size_t orderedIndex = 0u;
		orderedIndex < orderedWork.size();
		++orderedIndex)
	{
		const auto& work = plan.work[orderedWork[orderedIndex]];
		if(groups.empty()
			|| groups.back().stableInstanceId
				!= work.key.deformation.stableInstanceId
			|| groups.back().meshId != work.key.page.meshId)
		{
			groups.push_back({
				work.key.deformation.stableInstanceId,
				work.key.page.meshId,
				orderedIndex,
				0u });
		}
		++groups.back().count;
	}
	constexpr std::uint32_t kBridgeHeaderWords = 16u;
	constexpr std::uint32_t kBridgeRecordWords = 4u;
	constexpr std::uint32_t kBridgeMarker = 0x4445464du;
	std::vector<std::uint32_t> bridgeWords;
	bridgeWords.reserve(
		groups.size() * kBridgeHeaderWords
			+ orderedWork.size() * kBridgeRecordWords);
	resources.graphicsGroups.clear();
	resources.graphicsGroups.reserve(groups.size());
	for(std::uint32_t groupIndex = 0u;
		groupIndex < groups.size();
		++groupIndex)
	{
		const GroupRange& group = groups[groupIndex];
		const std::size_t headerOffset =
			bridgeWords.size();
		if(headerOffset
				> std::numeric_limits<std::uint32_t>::max()
					- kBridgeHeaderWords
			|| group.count
				> (std::numeric_limits<std::uint32_t>::max()
					- (headerOffset + kBridgeHeaderWords))
					/ kBridgeRecordWords)
		{
			RetireVulkanDeformationFrameResources(resources);
			return false;
		}
		const std::uint32_t recordWordOffset =
			static_cast<std::uint32_t>(
				headerOffset + kBridgeHeaderWords);
		bridgeWords.resize(
			headerOffset + kBridgeHeaderWords
				+ group.count * kBridgeRecordWords,
			0u);
		const auto& firstWork =
			plan.work[orderedWork[group.begin]];
		bridgeWords[headerOffset + 0u] =
			TerraEngine::Renderer::kAnimatedGeometryAbiVersion;
		bridgeWords[headerOffset + 1u] =
			static_cast<std::uint32_t>(
				plan.rendererGeneration);
		bridgeWords[headerOffset + 2u] = recordWordOffset;
		bridgeWords[headerOffset + 3u] =
			static_cast<std::uint32_t>(group.count);
		bridgeWords[headerOffset + 4u] =
			resources.currentVerticesBindlessIndex;
		bridgeWords[headerOffset + 5u] =
			previousVerticesBindlessIndex;
		bridgeWords[headerOffset + 6u] =
			resources.boundsBindlessIndex;
		bridgeWords[headerOffset + 7u] =
			resources.refinementBindlessIndex;
		bridgeWords[headerOffset + 8u] =
			static_cast<std::uint32_t>(
				group.stableInstanceId);
		bridgeWords[headerOffset + 9u] =
			static_cast<std::uint32_t>(
				group.stableInstanceId >> 32u);
		bridgeWords[headerOffset + 10u] =
			static_cast<std::uint32_t>(
				firstWork.key.page.meshPayloadGeneration);
		bridgeWords[headerOffset + 11u] =
			static_cast<std::uint32_t>(
				firstWork.key.page.meshPayloadGeneration
					>> 32u);
		bridgeWords[headerOffset + 12u] = 0x05u
			| (historyCompatible ? 0x02u : 0u);
		bridgeWords[headerOffset + 13u] =
			plan.totalVertexCount;
		bridgeWords[headerOffset + 14u] = groupIndex;
		bridgeWords[headerOffset + 15u] = kBridgeMarker;
		resources.graphicsGroups.push_back({
			group.stableInstanceId,
			group.meshId,
			static_cast<std::uint32_t>(headerOffset) });
		for(std::size_t localRecord = 0u;
			localRecord < group.count;
			++localRecord)
		{
			const std::uint32_t workIndex =
				orderedWork[group.begin + localRecord];
			const auto& work = plan.work[workIndex];
			const std::size_t outputOffset =
				static_cast<std::size_t>(recordWordOffset)
					+ localRecord * kBridgeRecordWords;
			bridgeWords[outputOffset + 0u] =
				work.key.page.pageId;
			bridgeWords[outputOffset + 1u] =
				work.vertexOffset;
			bridgeWords[outputOffset + 2u] =
				workIndex;
			bridgeWords[outputOffset + 3u] = 0x05u
				| (shaderWork[workIndex].flags & 0x01u
					? 0x02u
					: 0u);
		}
	}
	const VkDeviceSize bridgeBytes =
		bridgeWords.size() * sizeof(std::uint32_t);
	if(resources.graphicsBridgeBuffer.buffer == VK_NULL_HANDLE
		|| resources.graphicsBridgeCapacityBytes < bridgeBytes)
	{
		RetireBuffer(resources.graphicsBridgeBuffer);
		resources.graphicsBridgeBuffer = CreateBuffer(
			bridgeBytes,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU,
			"Animated Virtual Geometry Graphics Bridge");
		resources.graphicsBridgeCapacityBytes = bridgeBytes;
	}
	if(resources.graphicsBridgeBuffer.buffer == VK_NULL_HANDLE
		|| !WriteMappedBuffer(
			allocator,
			resources.graphicsBridgeBuffer,
			bridgeWords.data(),
			bridgeBytes))
	{
		RetireVulkanDeformationFrameResources(resources);
		return false;
	}
	if(resources.graphicsBridgeBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		resources.graphicsBridgeBindlessIndex =
			AllocateBufferIndex();
	}
	UpdateGlobalBufferForFrame(
		currentFrame,
		resources.graphicsBridgeBindlessIndex,
		resources.graphicsBridgeBuffer.buffer,
		0u,
		bridgeBytes);
	resources.descriptorPlan = descriptorPlan;
	resources.workKeys = std::move(workKeys);
	resources.frameId = plan.frameId;
	resources.rendererGeneration = plan.rendererGeneration;
	resources.live = true;
	activeDeformationGraphicsBufferIndex =
		resources.graphicsBridgeBindlessIndex;
	return true;
}

bool VulkanContext::Impl::PublishVulkanAnimationResources(
	const TerraEngine::Animation::GpuAnimationPlan& plan,
	std::uint32_t rendererGeneration)
{
	activeAnimationGraphicsBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	if(plan.dispatchCommands.empty()) {
		// The dormant/static invariant is strict: zero animation work creates no
		// buffers, descriptor writes, uploads, barriers, or queue submissions.
		return true;
	}

	if(rendererGeneration == 0U || currentFrame >= MAX_FRAMES_IN_FLIGHT || plan.workPackets.empty()) return false;
	const bool currentFrameHasRecordedCommands = currentFrame < pendingPrimaryCommandBuffers.size()
		&& !pendingPrimaryCommandBuffers[currentFrame].empty();
	const bool currentFrameWasSubmitted = currentFrame < pendingPrimaryCommandBuffersSubmitted.size()
		&& pendingPrimaryCommandBuffersSubmitted[currentFrame] != 0U;
	if(currentFrameHasRecordedCommands || currentFrameWasSubmitted) {
		// Bindless set-0 descriptors are mutable host state. Refuse a late
		// animation publication instead of stalling or rewriting a descriptor a
		// submitted Scene/Game command buffer may still reference.
		return false;
	}

	const TerraEngine::Renderer::Vulkan::VulkanAnimationDescriptorSetPlan descriptorPlan =
		TerraEngine::Renderer::Vulkan::BuildVulkanAnimationDescriptorSetPlan(plan);
	if(!descriptorPlan.valid || !descriptorPlan.hasWork) return false;
	// Validate every copied index before creating or updating any Vulkan object.
	// The renderer treats this immutable plan as one transaction: malformed
	// producer bytes must not expose a partially updated descriptor set.
	const auto& payloads = plan.residentPayloads;
	if(plan.generation == 0U || plan.generationRecords.empty()
		|| !payloads.HasRequiredSampleAndHierarchyInputs()) return false;
	for(const TerraEngine::Animation::GpuAnimationWorkPacket& packet : plan.workPackets) {
		if(packet.generationRecordIndex >= plan.generationRecords.size()
			|| packet.clipResourceIndex == TerraEngine::Animation::kGpuAnimationInvalidResourceIndex
			|| packet.programResourceIndex == TerraEngine::Animation::kGpuAnimationInvalidResourceIndex
			|| packet.skeletonResourceIndex == TerraEngine::Animation::kGpuAnimationInvalidResourceIndex
			|| packet.stageMask == 0U
			|| (packet.stageMask & ~TerraEngine::Animation::kGpuAnimationAllStageMask) != 0U
			|| (packet.maskResourceIndex != TerraEngine::Animation::kGpuAnimationInvalidResourceIndex
				&& payloads.maskTables.empty())
			|| (packet.morphCount != 0U && (packet.morphResourceIndex
				== TerraEngine::Animation::kGpuAnimationInvalidResourceIndex || payloads.morphInputs.empty()))) {
			return false;
		}
		const TerraEngine::Animation::GpuAnimationGenerationRecord& generation =
			plan.generationRecords[packet.generationRecordIndex];
		const std::uint64_t stableInstanceId =
			(static_cast<std::uint64_t>(generation.instanceSlot) << 32U)
			| static_cast<std::uint64_t>(generation.instanceGeneration);
		if(!TerraEngine::Animation::IsGpuAnimationGenerationRecordValid(generation)
			|| generation.rendererGeneration != rendererGeneration
			|| packet.stableInstanceId != stableInstanceId) {
			// Never relabel an immutable packet from an older renderer session with
			// the current recovery generation in the graphics bridge.
			return false;
		}
	}
	for(const TerraEngine::Animation::GpuAnimationDispatchRecord& dispatch : plan.dispatchRecords) {
		if(dispatch.workPacketIndex >= plan.workPackets.size()
			|| dispatch.generationRecordIndex >= plan.generationRecords.size()
			|| !TerraEngine::Animation::IsGpuAnimationStageValueValid(dispatch.stage)
			|| dispatch.elementCount == 0U) return false;
		const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
			plan.workPackets[dispatch.workPacketIndex];
		if(dispatch.stableInstanceId != packet.stableInstanceId
			|| dispatch.generationRecordIndex != packet.generationRecordIndex) return false;
	}
	for(const TerraEngine::Animation::GpuAnimationDispatchCommand& command : plan.dispatchCommands) {
		const std::size_t first = command.pushConstants.firstDispatchRecord;
		const std::size_t count = command.pushConstants.dispatchRecordCount;
		if(command.pushConstants.planGeneration != plan.generation
			|| !TerraEngine::Animation::IsGpuAnimationStageValueValid(command.pushConstants.stage)
			|| count == 0U
			|| count > TerraEngine::Animation::kGpuAnimationMaximumDispatchRecordsPerCommand
			|| first > plan.dispatchRecords.size()
			|| count > plan.dispatchRecords.size() - first
			|| command.groupCountX == 0U || command.groupCountY == 0U || command.groupCountZ == 0U) {
			return false;
		}
	}
	if(maximumStorageBufferRange == 0U
		|| plan.workPackets.size() > std::numeric_limits<VkDeviceSize>::max()
			/ sizeof(VulkanGpuAnimationGraphicsRecord)) return false;
	const VkDeviceSize bridgeBytes = static_cast<VkDeviceSize>(
		plan.workPackets.size() * sizeof(VulkanGpuAnimationGraphicsRecord));
	if(bridgeBytes == 0U || bridgeBytes > maximumStorageBufferRange) return false;
	std::vector<VulkanAnimationPoseHistoryIdentity> currentPoseHistoryIdentities;
	bool requestsHoldLast = false;
	try {
		currentPoseHistoryIdentities.reserve(plan.workPackets.size());
		for(const TerraEngine::Animation::GpuAnimationWorkPacket& packet : plan.workPackets) {
			const TerraEngine::Animation::GpuAnimationGenerationRecord& generation =
				plan.generationRecords[packet.generationRecordIndex];
			currentPoseHistoryIdentities.push_back({
				packet.stableInstanceId,
				packet.localPoseOffsetBytes,
				packet.jointCount,
				generation.skeletonGeneration,
				generation.programGeneration});
			requestsHoldLast = requestsHoldLast || TerraEngine::Animation::HasGpuAnimationFlag(
				packet.flags, TerraEngine::Animation::GpuAnimationWorkFlag::UseHoldLastFallback);
		}
	} catch(const std::bad_alloc&) {
		return false;
	}
	if(animationFrameResources.size() < MAX_FRAMES_IN_FLIGHT) {
		animationFrameResources.resize(MAX_FRAMES_IN_FLIGHT);
	}
	if(!EnsureVulkanAnimationDescriptorSets()) return false;
	VulkanAnimationFrameResources& resources = animationFrameResources[currentFrame];
	if(resources.live && resources.rendererGeneration != rendererGeneration) {
		// A completed frame slot may reuse and overwrite its buffers for a newer
		// plan generation. Only renderer-session replacement invalidates the native
		// allocations and bindless identities themselves.
		RetireVulkanAnimationFrameResources(resources);
	}
	bool holdLastHistoryCompatible = requestsHoldLast && resources.live
		&& resources.poseHistoryIdentities == currentPoseHistoryIdentities;

	bool descriptorBindingsChanged = false;
	auto ensureBuffer = [this, &resources, &descriptorBindingsChanged](
		std::size_t bindingIndex,
		VkDeviceSize requiredBytes,
		const char* name) {
		// Vulkan disallows zero-sized buffers. A sixteen-byte safe sentinel lets a
		// missing optional cooked input fail in shader diagnostics rather than
		// binding VK_NULL_HANDLE or reading arbitrary old memory.
		const VkDeviceSize safeBytes = std::max<VkDeviceSize>(requiredBytes, 16U);
		VulkanBuffer& buffer = resources.buffers[bindingIndex];
		if(buffer.buffer == VK_NULL_HANDLE || resources.capacities[bindingIndex] < safeBytes) {
			RetireBuffer(buffer);
			buffer = CreateBuffer(safeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_CPU_TO_GPU, name);
			resources.capacities[bindingIndex] = safeBytes;
			descriptorBindingsChanged = true;
		}
	};

	const auto payloadByteCount = [&payloads](TerraEngine::Animation::GpuAnimationDescriptorBinding binding) {
		using Binding = TerraEngine::Animation::GpuAnimationDescriptorBinding;
		switch(binding) {
		case Binding::ProgramWords: return static_cast<VkDeviceSize>(payloads.programWords.size() * sizeof(std::uint32_t));
		case Binding::PackedClipBytes: return static_cast<VkDeviceSize>(payloads.packedClipBytes.size());
		case Binding::SkeletonTables: return static_cast<VkDeviceSize>(payloads.skeletonTables.size());
		case Binding::MaskTables: return static_cast<VkDeviceSize>(payloads.maskTables.size());
		case Binding::MorphInputs: return static_cast<VkDeviceSize>(payloads.morphInputs.size());
		default: return VkDeviceSize{0U};
		}
	};
	std::array<VkDeviceSize, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount>
		descriptorByteCounts{};
	for(std::size_t index = 0U; index < descriptorPlan.bindings.size(); ++index) {
		const auto binding = static_cast<TerraEngine::Animation::GpuAnimationDescriptorBinding>(index);
		descriptorByteCounts[index] = std::max<VkDeviceSize>(16U,
			std::max<VkDeviceSize>(descriptorPlan.bindings[index].minimumByteCount,
				payloadByteCount(binding)));
		if(descriptorByteCounts[index] > maximumStorageBufferRange) {
			// VkDescriptorBufferInfo::range may not exceed the selected device's
			// storage-buffer limit. Fail open before allocating or updating a set.
			return false;
		}
	}
	const std::size_t localPoseBindingIndex = static_cast<std::size_t>(
		TerraEngine::Animation::GpuAnimationDescriptorBinding::LocalPoses);
	holdLastHistoryCompatible = holdLastHistoryCompatible
		&& resources.buffers[localPoseBindingIndex].buffer != VK_NULL_HANDLE
		&& resources.capacities[localPoseBindingIndex] >= descriptorByteCounts[localPoseBindingIndex];
	try {
		for(std::size_t index = 0U; index < descriptorPlan.bindings.size(); ++index) {
			ensureBuffer(index, descriptorByteCounts[index], "GPU Animation Frame Buffer");
			const bool preserveLocalPoseHistory = holdLastHistoryCompatible
				&& index == localPoseBindingIndex;
			if(!preserveLocalPoseHistory && !ClearMappedBuffer(allocator, resources.buffers[index])) {
				RetireVulkanAnimationFrameResources(resources);
				return false;
			}
		}
	} catch(const std::exception&) {
		RetireVulkanAnimationFrameResources(resources);
		return false;
	}

	using Binding = TerraEngine::Animation::GpuAnimationDescriptorBinding;
	const auto upload = [this, &resources](Binding binding, const void* bytes, VkDeviceSize size) {
		if(size == 0U) return true;
		return WriteMappedBuffer(allocator, resources.buffers[static_cast<std::size_t>(binding)], bytes, size);
	};
	if(!upload(Binding::GenerationRecords, plan.generationRecords.data(),
		static_cast<VkDeviceSize>(plan.generationRecords.size() * sizeof(TerraEngine::Animation::GpuAnimationGenerationRecord)))
		|| !upload(Binding::WorkPackets, plan.workPackets.data(),
			static_cast<VkDeviceSize>(plan.workPackets.size() * sizeof(TerraEngine::Animation::GpuAnimationWorkPacket)))
		|| !upload(Binding::DispatchRecords, plan.dispatchRecords.data(),
			static_cast<VkDeviceSize>(plan.dispatchRecords.size() * sizeof(TerraEngine::Animation::GpuAnimationDispatchRecord)))
		|| !upload(Binding::ProgramWords, payloads.programWords.data(),
			static_cast<VkDeviceSize>(payloads.programWords.size() * sizeof(std::uint32_t)))
		|| !upload(Binding::PackedClipBytes, payloads.packedClipBytes.data(),
			static_cast<VkDeviceSize>(payloads.packedClipBytes.size()))
		|| !upload(Binding::SkeletonTables, payloads.skeletonTables.data(),
			static_cast<VkDeviceSize>(payloads.skeletonTables.size()))
		|| !upload(Binding::MaskTables, payloads.maskTables.data(),
			static_cast<VkDeviceSize>(payloads.maskTables.size()))
		|| !upload(Binding::MorphInputs, payloads.morphInputs.data(),
			static_cast<VkDeviceSize>(payloads.morphInputs.size()))) {
		RetireVulkanAnimationFrameResources(resources);
		return false;
	}

	resources.descriptorPlan = descriptorPlan;
	resources.planGeneration = plan.generation;
	resources.rendererGeneration = rendererGeneration;
	std::array<VkDescriptorBufferInfo, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount> infos{};
	std::array<VkWriteDescriptorSet, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount> writes{};
	for(std::uint32_t binding = 0U; binding < infos.size(); ++binding) {
		infos[binding].buffer = resources.buffers[binding].buffer;
		infos[binding].offset = 0U;
		infos[binding].range = resources.capacities[binding];
		writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstSet = animationDescriptorSets[currentFrame];
		writes[binding].dstBinding = binding;
		writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[binding].descriptorCount = 1U;
		writes[binding].pBufferInfo = &infos[binding];
	}
	// Descriptor handles and ranges remain stable while a frame-slot allocation
	// has enough capacity. Avoid rewriting an already-bound set when the editor
	// asks for a dedicated capture later in the same frame.
	if(descriptorBindingsChanged)
	{
		vkUpdateDescriptorSets(
			device,
			static_cast<std::uint32_t>(writes.size()),
			writes.data(),
			0U,
			nullptr);
	}
	const auto publishInternalAnimationBuffer = [this, &resources, &descriptorByteCounts](
		std::uint32_t& bindlessIndex,
		TerraEngine::Animation::GpuAnimationDescriptorBinding binding) {
		const std::size_t bindingIndex = static_cast<std::size_t>(binding);
		const VulkanBuffer& buffer = resources.buffers[bindingIndex];
		if(buffer.buffer == VK_NULL_HANDLE || buffer.size == 0U) return INVALID_BINDLESS_BUFFER_INDEX;
		if(bindlessIndex == INVALID_BINDLESS_BUFFER_INDEX) bindlessIndex = AllocateBufferIndex();
		// Global set 0, binding 9 is updated only before this frame records. These
		// indices stay internal; the graphics bridge below is the sole exposed one.
		UpdateGlobalBufferForFrame(currentFrame, bindlessIndex, buffer.buffer, 0U,
			descriptorByteCounts[bindingIndex]);
		return bindlessIndex;
	};
	try {
		(void)publishInternalAnimationBuffer(
			resources.generationRecordsBindlessIndex, Binding::GenerationRecords);
		(void)publishInternalAnimationBuffer(
			resources.workPacketsBindlessIndex, Binding::WorkPackets);
		(void)publishInternalAnimationBuffer(
			resources.paletteMatricesBindlessIndex, Binding::PaletteMatrices);
		(void)publishInternalAnimationBuffer(
			resources.morphOutputsBindlessIndex, Binding::MorphOutputs);

		if(resources.graphicsBridgeBuffer.buffer == VK_NULL_HANDLE
			|| resources.graphicsBridgeCapacityBytes < bridgeBytes) {
			RetireBuffer(resources.graphicsBridgeBuffer);
			resources.graphicsBridgeBuffer = CreateBuffer(bridgeBytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
				"GPU Animation Graphics Bridge Buffer");
			resources.graphicsBridgeCapacityBytes = bridgeBytes;
		}
		std::vector<VulkanGpuAnimationGraphicsRecord> bridgeRecords;
		bridgeRecords.reserve(plan.workPackets.size());
		for(std::size_t packetIndex = 0U; packetIndex < plan.workPackets.size(); ++packetIndex) {
			const TerraEngine::Animation::GpuAnimationWorkPacket& packet = plan.workPackets[packetIndex];
			VulkanGpuAnimationGraphicsRecord record{};
			record.planGeneration = plan.generation;
			record.rendererGeneration = rendererGeneration;
			record.workPacketIndex = static_cast<std::uint32_t>(packetIndex);
			record.generationRecordIndex = packet.generationRecordIndex;
			record.paletteMatricesBindlessIndex = resources.paletteMatricesBindlessIndex;
			record.morphOutputsBindlessIndex = resources.morphOutputsBindlessIndex;
			record.paletteOffsetBytes = packet.paletteOffsetBytes;
			record.morphOutputOffsetBytes = packet.morphOutputOffsetBytes;
			record.jointCount = packet.jointCount;
			record.morphCount = packet.morphCount;
			record.paletteEntryCount = packet.paletteEntryCount;
			record.flags = packet.flags;
			record.stageMask = packet.stageMask;
			record.validMarker = 1U;
			bridgeRecords.push_back(record);
		}
		if(!WriteMappedBuffer(allocator, resources.graphicsBridgeBuffer,
			bridgeRecords.data(), bridgeBytes)) {
			RetireVulkanAnimationFrameResources(resources);
			return false;
		}
		if(resources.graphicsBridgeBindlessIndex == INVALID_BINDLESS_BUFFER_INDEX)
			resources.graphicsBridgeBindlessIndex = AllocateBufferIndex();
		UpdateGlobalBufferForFrame(currentFrame, resources.graphicsBridgeBindlessIndex,
			resources.graphicsBridgeBuffer.buffer, 0U, bridgeBytes);
		activeAnimationGraphicsBufferIndex = resources.graphicsBridgeBindlessIndex;
	} catch(const std::exception&) {
		RetireVulkanAnimationFrameResources(resources);
		return false;
	}
	resources.live = true;
	resources.poseHistoryIdentities = std::move(currentPoseHistoryIdentities);
	resources.completionPending = false;
	return true;
}

void VulkanContext::Impl::MarkVulkanAnimationGenerationSubmitted(std::uint32_t rendererGeneration)
{
	if(currentFrame >= animationFrameResources.size()) return;
	VulkanAnimationFrameResources& resources = animationFrameResources[currentFrame];
	if(!resources.live || resources.rendererGeneration != rendererGeneration
		|| resources.planGeneration == 0U || pendingAnimationFrameId == 0U
		|| !animationComputeRecordedForCurrentFrame
		|| recordedAnimationDispatchCountForCurrentFrame == 0U) {
		return;
	}
	resources.frameId = pendingAnimationFrameId;
	resources.submissionSerial = nextAnimationSubmissionSerial++;
	resources.recordedDispatchCount =
		recordedAnimationDispatchCountForCurrentFrame;
	resources.completionPending = true;
	const TerraEngine::Renderer::AnimationGpuBackendExecutionRecord submitted{
		resources.frameId,
		resources.submissionSerial,
		resources.planGeneration,
		resources.rendererGeneration,
		resources.recordedDispatchCount};
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestAnimationGpuBackendExecution.latestSubmitted = submitted;
	}
}

void VulkanContext::Impl::CompleteVulkanAnimationFrameResources(std::uint32_t frameIndex)
{
	if(frameIndex >= animationFrameResources.size()) return;
	VulkanAnimationFrameResources& resources = animationFrameResources[frameIndex];
	if(!resources.live || !resources.completionPending) return;
	// This function is called only after the owning frame fence has completed.
	// It publishes a generation-stamped receipt and deliberately performs no
	// pose readback or wait for current-frame compute work.
	while(completedAnimationGenerations.size() >= MAX_FRAMES_IN_FLIGHT) {
		// Completion is an observation channel, not a pose cache. Retain a bounded
		// recent history so a stalled diagnostic consumer cannot grow renderer RAM.
		completedAnimationGenerations.pop_front();
	}
	completedAnimationGenerations.push_back({
		resources.frameId,
		resources.planGeneration,
		resources.rendererGeneration,
		resources.submissionSerial,
		resources.recordedDispatchCount});
	const TerraEngine::Renderer::AnimationGpuBackendExecutionRecord completed{
		resources.frameId,
		resources.submissionSerial,
		resources.planGeneration,
		resources.rendererGeneration,
		resources.recordedDispatchCount};
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestAnimationGpuBackendExecution.latestCompleted = completed;
	}
	resources.completionPending = false;
}

void VulkanContext::Impl::DestroyVirtualGeometryGpuPacketResources(VirtualGeometryGpuPacketFrameResources& resources)
{
	// The descriptor pool permits individual frees. Releasing this per-view set
	// before its buffers avoids accumulating stale compaction bindings across
	// editor view creation and retirement.
	if(device != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE
		&& resources.executorDescriptorSet != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(device, descriptorPool, 1u, &resources.executorDescriptorSet);
	}
	const std::array<VkDescriptorSet, 3> visibilityDescriptorSets{
		resources.visibilityComputeRasterDescriptorSet,
		resources.visibilityResolveDescriptorSet,
		resources.visibilityShadingBinsDescriptorSet };
	if(device != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE) {
		for(const VkDescriptorSet descriptorSet : visibilityDescriptorSets) {
			if(descriptorSet != VK_NULL_HANDLE) vkFreeDescriptorSets(device, descriptorPool, 1u, &descriptorSet);
		}
	}
	DestroyBufferImmediate(resources.packetBuffer);
	DestroyBufferImmediate(resources.traversalInstanceBuffer);
	DestroyBufferImmediate(resources.traversalRequestBuffer);
	DestroyBufferImmediate(resources.traversalCounterBuffer);
	DestroyBufferImmediate(resources.batchBuffer);
	DestroyBufferImmediate(resources.compactedPacketBuffer);
	DestroyBufferImmediate(resources.generatedSubmissionBuffer);
	DestroyBufferImmediate(resources.counterReadbackBuffer);
	DestroyBufferImmediate(resources.zeroWorkIndirectBuffer);
	DestroyBufferImmediate(resources.visibilityTriangleBuffer);
	DestroyBufferImmediate(resources.visibilityDepthBuffer);
	DestroyBufferImmediate(resources.visibilityRecordBuffer);
	DestroyBufferImmediate(resources.resolvedVisibilityBuffer);
	DestroyBufferImmediate(resources.shadingBinBuffer);
	DestroyBufferImmediate(resources.pagePayloadBuffer);
	if (resources.packetBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.packetBufferIndex);
	}
	if (resources.batchBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.batchBufferIndex);
	}
	if (resources.compactedPacketBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.compactedPacketBufferIndex);
	}
	if (resources.generatedSubmissionBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.generatedSubmissionBufferIndex);
	}
	if (resources.counterReadbackBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.counterReadbackBufferIndex);
	}
	if (resources.pagePayloadBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.pagePayloadBufferIndex);
	}
	resources = {};
}

void VulkanContext::Impl::PublishVirtualGeometryDecodedPageResources(const FrameRenderData& frameData)
{
	if (currentFrame >= pendingVirtualGeometryPublicationsByFrame.size())
	{
		return;
	}

	auto& pending = pendingVirtualGeometryPublicationsByFrame[currentFrame];
	for (const VirtualGeometryDecodedPageUploadRecord& upload : frameData.virtualGeometryDecodedPageUploads)
	{
		if (upload.assetId == TerraEngine::EMPTY_UUID
			|| upload.pageId == 0xffffffffu
			|| upload.clusterId == 0xffffffffu
			|| upload.publicationGeneration == 0u
			|| upload.integrityValue == 0u
			|| upload.decodedBytes.empty())
		{
			continue;
		}

		auto resource = std::find_if(
			persistentVirtualGeometryPages.begin(),
			persistentVirtualGeometryPages.end(),
			[&upload](const PersistentVirtualGeometryPageResource& candidate)
			{
				return candidate.assetId == upload.assetId
					&& candidate.pageId == upload.pageId;
			});
		if (resource != persistentVirtualGeometryPages.end()
			&& resource->publicationGeneration >= upload.publicationGeneration)
		{
			continue;
		}

		std::uint64_t retiredAllocationId = 0u;
		if (resource == persistentVirtualGeometryPages.end())
		{
			persistentVirtualGeometryPages.push_back({});
			resource = std::prev(persistentVirtualGeometryPages.end());
			resource->assetId = upload.assetId;
			resource->pageId = upload.pageId;
			resource->bindlessIndex = AllocateBufferIndex();
		}
		else
		{
			retiredAllocationId = resource->allocationId;
			RetireBuffer(resource->buffer);
		}

		resource->buffer = CreateBuffer(
			static_cast<VkDeviceSize>(upload.decodedBytes.size()),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU,
			"Persistent Virtual Geometry Decoded Page");
		if (resource->buffer.buffer == VK_NULL_HANDLE
			|| !WriteMappedBuffer(
				allocator,
				resource->buffer,
				upload.decodedBytes.data(),
				static_cast<VkDeviceSize>(upload.decodedBytes.size())))
		{
			RetireBuffer(resource->buffer);
			continue;
		}

		resource->publicationGeneration = upload.publicationGeneration;
		resource->allocationId = nextVirtualGeometryPersistentPageAllocationId++;
		UpdateGlobalBuffer(
			resource->bindlessIndex,
			resource->buffer.buffer,
			0u,
			resource->buffer.size);

		TerraEngine::Resource::VirtualGeometryPagePublicationCandidate candidate{};
		candidate.candidateId = resource->allocationId;
		candidate.publicationGeneration = upload.publicationGeneration;
		candidate.key.asset.assetId = upload.assetId;
		candidate.key.asset.sourceHash = upload.integrityValue;
		candidate.key.asset.buildRecipeHash =
			upload.integrityValue ^ 0x9e3779b97f4a7c15ull;
		if (candidate.key.asset.buildRecipeHash == 0u)
		{
			candidate.key.asset.buildRecipeHash = 1u;
		}
		candidate.key.generations = {
			upload.publicationGeneration,
			upload.publicationGeneration,
			upload.publicationGeneration,
			upload.publicationGeneration,
			upload.publicationGeneration };
		candidate.key.pageId = upload.pageId;
		candidate.key.pageContentGeneration = upload.integrityValue;
		candidate.payload = {
			upload.storedByteCount,
			upload.decodedByteCount,
			upload.integrityValue,
			true,
			true };
		candidate.hierarchy.clusterIds = { upload.clusterId };
		candidate.hierarchy.fallbackPageId =
			upload.fallbackPageId == 0xffffffffu
				? upload.pageId
				: upload.fallbackPageId;
		candidate.hierarchy.fallbackCoverageVerified = true;
		candidate.hierarchy.hierarchyAcyclic = true;
		candidate.descriptor.allocationId = resource->allocationId;
		candidate.descriptor.bufferIndex = resource->bindlessIndex;
		candidate.descriptor.descriptorIndex = resource->bindlessIndex;
		// BeginFrame changes this to true only after the owning slot fence.
		candidate.descriptor.uploadCompleted = false;
		for (const std::uint32_t dependency : upload.dependencyPageIds)
		{
			if (dependency != 0xffffffffu
				&& dependency != upload.pageId
				&& std::find(
					candidate.dependencyPageIds.begin(),
					candidate.dependencyPageIds.end(),
					dependency) == candidate.dependencyPageIds.end())
			{
				candidate.dependencyPageIds.push_back(dependency);
			}
		}
		candidate.dependenciesVerified = true;
		candidate.residencyTransitionVerified = true;
		if (retiredAllocationId != 0u)
		{
			candidate.retirements.push_back({
				retiredAllocationId,
				upload.publicationGeneration });
		}

		const auto duplicate = std::find_if(
			pending.begin(),
			pending.end(),
			[&candidate](const auto& existing)
			{
				return existing.key.asset.assetId == candidate.key.asset.assetId
					&& existing.key.pageId == candidate.key.pageId
					&& existing.publicationGeneration
						== candidate.publicationGeneration;
			});
		if (duplicate == pending.end())
		{
			pending.push_back(std::move(candidate));
		}
	}
}

// Prepare one bounded ExpandedProduction parity request for this frame slot.
// The upload record already contains immutable CPU-decoded canonical bytes, so
// the simple ExpandedProduction kernel can copy those exact bytes and prove
// that its ABI, descriptor ranges, and host readback contract are sound.
void VulkanContext::Impl::PrepareVulkanVirtualGeometryProductionDecode(const FrameRenderData& frameData)
{
	if(currentFrame >= virtualGeometryProductionDecodeResourcesByFrame.size()) {
		return;
	}
	auto& resources = virtualGeometryProductionDecodeResourcesByFrame[currentFrame];
	if(resources.preparedFrameId == backendFrameSerial) {
		return;
	}
	resources.preparedFrameId = backendFrameSerial;
	// A slot may have completed an earlier request. Start this new frame with no
	// recordable plan so an absent/invalid candidate cannot replay stale bytes.
	resources.plan = {};
	resources.completionPending = false;

	constexpr std::size_t kMaximumDecodeBytes = 262144u;
	const auto candidate = std::find_if(frameData.virtualGeometryDecodedPageUploads.begin(),
		frameData.virtualGeometryDecodedPageUploads.end(), [kMaximumDecodeBytes](const VirtualGeometryDecodedPageUploadRecord& upload) {
		return upload.publicationGeneration != 0u
			&& !upload.decodedBytes.empty()
			&& upload.decodedBytes.size() <= kMaximumDecodeBytes
			&& upload.decodedBytes.size() % sizeof(std::uint32_t) == 0u;
		});
	if(candidate == frameData.virtualGeometryDecodedPageUploads.end()) {
		return;
	}
	if(!EnsureVulkanVirtualGeometryProductionDecodePipeline()) {
		return;
	}

	const auto ensureCapacity = [this](VulkanBuffer& buffer, VkDeviceSize requiredBytes, const char* name) {
		if(buffer.buffer != VK_NULL_HANDLE && buffer.size >= requiredBytes) {
			return true;
		}
		RetireBuffer(buffer);
		try {
			buffer = CreateBuffer(requiredBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_CPU_TO_GPU, name);
		}
		catch(const std::exception&) {
			return false;
		}
		return buffer.buffer != VK_NULL_HANDLE;
	};
	const VkDeviceSize decodedBytes = static_cast<VkDeviceSize>(candidate->decodedBytes.size());
	if(!ensureCapacity(resources.requestBuffer,
		sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeRequest),
		"Virtual Geometry Production Decode Request")
		|| !ensureCapacity(resources.encodedBuffer, decodedBytes,
			"Virtual Geometry Production Decode Encoded")
		|| !ensureCapacity(resources.decodedBuffer, decodedBytes,
			"Virtual Geometry Production Decode Decoded")
		|| !ensureCapacity(resources.resultReadbackBuffer,
		sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResult),
			"Virtual Geometry Production Decode Result")) {
		virtualGeometryProductionDecodeUnavailableReason = "Could not allocate bounded ExpandedProduction decode buffers.";
		return;
	}

	const std::uint32_t boundedBytes = static_cast<std::uint32_t>(decodedBytes);
	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeRequest request{};
	request.profileId = 1u;
	request.codecVersion = 1u;
	request.encodedByteCount = boundedBytes;
	request.decodedByteCount = boundedBytes;
	request.outputCapacityBytes = boundedBytes;
	request.requestIdentity = static_cast<std::uint32_t>(candidate->publicationGeneration);
	if(request.requestIdentity == 0u) {
		request.requestIdentity = 1u;
	}
	const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResult clearedResult{};
	if(!WriteMappedBuffer(allocator, resources.requestBuffer, &request, sizeof(request))
		|| !WriteMappedBuffer(allocator, resources.encodedBuffer, candidate->decodedBytes.data(), decodedBytes)
		|| !ClearMappedBuffer(allocator, resources.decodedBuffer)
		|| !WriteMappedBuffer(allocator, resources.resultReadbackBuffer, &clearedResult, sizeof(clearedResult))) {
		virtualGeometryProductionDecodeUnavailableReason = "Could not upload bounded ExpandedProduction decode inputs.";
		return;
	}
	if(resources.descriptorSet == VK_NULL_HANDLE) {
		VkDescriptorSetAllocateInfo allocationInfo{};
		allocationInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocationInfo.descriptorPool = descriptorPool;
		allocationInfo.descriptorSetCount = 1u;
		allocationInfo.pSetLayouts = &virtualGeometryProductionDecodeDescriptorSetLayout;
		if(vkAllocateDescriptorSets(device, &allocationInfo, &resources.descriptorSet) != VK_SUCCESS) {
			virtualGeometryProductionDecodeUnavailableReason = "Could not allocate the ExpandedProduction decode descriptor set.";
			return;
		}
	}
	const std::array<VkDescriptorBufferInfo, 4> bufferInfos{
		VkDescriptorBufferInfo{ resources.requestBuffer.buffer, 0u, resources.requestBuffer.size },
		VkDescriptorBufferInfo{ resources.encodedBuffer.buffer, 0u, resources.encodedBuffer.size },
		VkDescriptorBufferInfo{ resources.decodedBuffer.buffer, 0u, resources.decodedBuffer.size },
		VkDescriptorBufferInfo{ resources.resultReadbackBuffer.buffer, 0u, resources.resultReadbackBuffer.size } };
	std::array<VkWriteDescriptorSet, 4> writes{};
	for(std::uint32_t binding = 0u; binding < writes.size(); ++binding) {
		writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstSet = resources.descriptorSet;
		writes[binding].dstBinding = binding;
		writes[binding].descriptorCount = 1u;
		writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[binding].pBufferInfo = &bufferInfos[binding];
	}
	vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);

	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResources decodeResources{};
	decodeResources.requestBuffer = resources.requestBuffer.buffer;
	decodeResources.encodedBuffer = resources.encodedBuffer.buffer;
	decodeResources.decodedBuffer = resources.decodedBuffer.buffer;
	decodeResources.resultReadbackBuffer = resources.resultReadbackBuffer.buffer;
	decodeResources.descriptorSet = resources.descriptorSet;
	decodeResources.encodedCapacityBytes = static_cast<std::uint32_t>(resources.encodedBuffer.size);
	decodeResources.decodedCapacityBytes = static_cast<std::uint32_t>(resources.decodedBuffer.size);
	decodeResources.resourcesOwnedByFrame = true;
	resources.plan = TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryProductionDecodePlan(
		request, candidate->publicationGeneration,
		CalculateVirtualGeometryCompactHash(candidate->decodedBytes), decodeResources,
		virtualGeometryProductionDecodePipelines);
	if(!resources.plan.valid) {
		virtualGeometryProductionDecodeUnavailableReason = resources.plan.unavailableReason;
	}
}

// BeginFrame has already waited for this slot's fence before calling here.
// Mapping therefore consumes completed device writes without adding a CPU/GPU
// wait; failures leave the most recently verified device generation intact.
void VulkanContext::Impl::CompleteVulkanVirtualGeometryProductionDecode(std::uint32_t frameIndex)
{
	if(frameIndex >= virtualGeometryProductionDecodeResourcesByFrame.size()) {
		return;
	}
	auto& resources = virtualGeometryProductionDecodeResourcesByFrame[frameIndex];
	if(!resources.completionPending) {
		return;
	}
	resources.completionPending = false;
	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResult result{};
	std::vector<std::uint8_t> decodedBytes(resources.plan.request.decodedByteCount);
	void* mappedResult = nullptr;
	void* mappedDecoded = nullptr;
	const bool mapped = resources.resultReadbackBuffer.allocation != VK_NULL_HANDLE
		&& resources.decodedBuffer.allocation != VK_NULL_HANDLE
		&& vmaInvalidateAllocation(allocator, resources.resultReadbackBuffer.allocation, 0u,
			resources.resultReadbackBuffer.size) == VK_SUCCESS
		&& vmaInvalidateAllocation(allocator, resources.decodedBuffer.allocation, 0u,
			resources.decodedBuffer.size) == VK_SUCCESS
		&& vmaMapMemory(allocator, resources.resultReadbackBuffer.allocation, &mappedResult) == VK_SUCCESS
		&& vmaMapMemory(allocator, resources.decodedBuffer.allocation, &mappedDecoded) == VK_SUCCESS;
	if(mapped) {
		std::memcpy(&result, mappedResult, sizeof(result));
		std::memcpy(decodedBytes.data(), mappedDecoded, decodedBytes.size());
	}
	if(mappedDecoded != nullptr) vmaUnmapMemory(allocator, resources.decodedBuffer.allocation);
	if(mappedResult != nullptr) vmaUnmapMemory(allocator, resources.resultReadbackBuffer.allocation);
	const auto receipt = mapped
		? TerraEngine::Renderer::Vulkan::VerifyVulkanVirtualGeometryProductionDecodeParity(
			resources.plan, result, decodedBytes)
		: TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeParityReceipt{
			resources.plan.generation, false, false,
			"ExpandedProduction decode readback could not be mapped after its completed fence." };
	if(receipt.acceptedForPublication) {
		completedVirtualGeometryProductionDecodeEvidence = true;
		publishedVirtualGeometryProductionDecodeGeneration = receipt.generation;
		virtualGeometryProductionDecodeUnavailableReason.clear();
	}
	else {
		virtualGeometryProductionDecodeUnavailableReason = receipt.reason;
	}
}

void VulkanContext::Impl::PublishVirtualGeometryGpuPacketResources(
	const FrameRenderData& frameData,
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint64_t frameId)
{
	activeVirtualGeometryPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryBatchBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryCompactedPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryGeneratedSubmissionBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryCounterReadbackBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryPagePayloadBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	// This reason is scoped to one immutable view publication. Retaining it
	// would make a repaired Scene or capture generation look unavailable because
	// an earlier view rejected stale packet input.
	virtualGeometryExecutorUnavailableReason.clear();
	virtualGeometryVisibilityUnavailableReason.clear();
	// Do not retain a previous view's success or failure if this frame has no
	// packet publication. Capability diagnostics are scoped to the current
	// command buffer, not the last frame that happened to have geometry.
	latestVirtualGeometryExecutorPlan = {};
	latestVirtualGeometryExecutorFailure = {};
	latestVirtualGeometryExecutorResources = {};
	latestVirtualGeometryVisibilityPlan = {};
	latestVirtualGeometryVisibilityResources = {};
	if (currentFrame >= virtualGeometryGpuPacketResourcesByFrame.size())
	{
		return;
	}

	std::vector<VirtualGeometryGpuPacketFrameResources>& frameResources = virtualGeometryGpuPacketResourcesByFrame[currentFrame];
	auto resourceIt = std::find_if(
		frameResources.begin(),
		frameResources.end(),
		[viewType, viewId](const VirtualGeometryGpuPacketFrameResources& resources) {
			return resources.viewType == viewType && resources.viewId == viewId;
		});
	if (resourceIt == frameResources.end())
	{
		VirtualGeometryGpuPacketFrameResources resources;
		resources.resourceSerial = nextVirtualGeometryGpuPacketResourceSerial++;
		resources.viewType = viewType;
		resources.viewId = viewId;
		frameResources.push_back(std::move(resources));
		resourceIt = frameResources.end();
		--resourceIt;
	}

	VirtualGeometryGpuPacketFrameResources& resources = *resourceIt;
	resources.frameId = frameId;
	resources.viewType = viewType;
	resources.viewId = viewId;
	resources.live = !frameData.virtualGeometryGpuPackets.empty() || !frameData.virtualGeometryGpuBatches.empty();
	resources.retireAfterFrameId = frameId + static_cast<std::uint64_t>(MAX_FRAMES_IN_FLIGHT);
	// Capture, reflection, Scene, and Game command buffers may share a frame
	// slot, but never a packet generation. Validate every immutable producer
	// record against this output before allocating or binding Vulkan buffers so a
	// stale editor/capture mirror safely falls back instead of crossing views.
	const auto belongsToCurrentPublication = [viewType, viewId](const auto& record) {
		return record.frameId != 0u && record.viewType == viewType
			&& record.viewId == viewId;
	};
	const bool hasCanonicalFrameSet = std::any_of(
		frameData.virtualGeometryGpuFrameSets.begin(),
		frameData.virtualGeometryGpuFrameSets.end(),
		[&belongsToCurrentPublication](const GpuVirtualGeometryPacketFrameSet& frameSet) {
			return belongsToCurrentPublication(frameSet);
		});
	const bool canonicalPacketPublication = hasCanonicalFrameSet
		&& std::all_of(frameData.virtualGeometryGpuPackets.begin(),
			frameData.virtualGeometryGpuPackets.end(), belongsToCurrentPublication)
		&& std::all_of(frameData.virtualGeometryGpuBatches.begin(),
			frameData.virtualGeometryGpuBatches.end(), belongsToCurrentPublication)
		&& std::all_of(frameData.virtualGeometryGpuCompactionDispatches.begin(),
			frameData.virtualGeometryGpuCompactionDispatches.end(), belongsToCurrentPublication)
		&& std::all_of(frameData.virtualGeometryGeneratedSubmissions.begin(),
			frameData.virtualGeometryGeneratedSubmissions.end(), belongsToCurrentPublication);
	if(resources.live && !canonicalPacketPublication)
	{
		resources.live = false;
		virtualGeometryExecutorUnavailableReason =
			"Virtual-geometry packet publication does not match the current frame-view identity.";
	}
	if (currentFrame < pendingVirtualGeometryPageRetirementsByFrame.size())
	{
		std::vector<TerraEngine::Renderer::Vulkan::VirtualGeometryPageRetirementPlan>& pendingPageRetirements =
			pendingVirtualGeometryPageRetirementsByFrame[currentFrame];
		for (const VirtualGeometryPageRetirementRecord& retirement : frameData.virtualGeometryPageRetirements)
		{
			if (retirement.protectedFallback || retirement.retired)
			{
				continue;
			}
			// Page retirement is represented as a value plan first. Actual
			// Vulkan buffer releases still go through the existing deferred
			// RetireBuffer path after all frame fences that could sample the
			// page payload have completed.
			TerraEngine::Renderer::Vulkan::VirtualGeometryPageRetirementPlan plan;
			plan.pageId = retirement.pageId;
			plan.canRetire = true;
			plan.retireAfterFrameId = std::max(retirement.frameId, frameId) + static_cast<std::uint64_t>(MAX_FRAMES_IN_FLIGHT);
			plan.reason = retirement.reason.empty()
				? "virtual geometry page queued from frame retirement record"
				: retirement.reason;
			pendingPageRetirements.push_back(std::move(plan));
		}
	}
	if (!resources.live)
	{
		latestVirtualGeometryExecutorPlan.frameId = frameId;
		latestVirtualGeometryExecutorPlan.evidenceId =
			"vulkan-vg-executor.rejected-publication."
				+ std::to_string(frameId) + "." + std::to_string(viewId);
		latestVirtualGeometryExecutorPlan.unavailableReason =
			virtualGeometryExecutorUnavailableReason;
		return;
	}

	auto ensureAndWriteBuffer = [&](VulkanBuffer& buffer, std::size_t& capacity, std::size_t elementCount, std::size_t elementSize, const void* source, const char* name,
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
		if (elementCount == 0u || source == nullptr)
		{
			return;
		}

		const VkDeviceSize requiredBytes = static_cast<VkDeviceSize>(elementCount * elementSize);
		if (buffer.buffer == VK_NULL_HANDLE || capacity < elementCount || buffer.size < requiredBytes)
		{
			RetireBuffer(buffer);
			buffer = CreateBuffer(requiredBytes, usage, VMA_MEMORY_USAGE_CPU_TO_GPU, name);
			capacity = elementCount;
		}
		WriteMappedBuffer(allocator, buffer, source, requiredBytes);
	};

	std::vector<TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord>
		packetUploadRecords;
	packetUploadRecords.reserve(frameData.virtualGeometryGpuPackets.size());
	for (const GpuVirtualGeometryPacketRecord& packet : frameData.virtualGeometryGpuPackets)
	{
		packetUploadRecords.push_back(BuildVirtualGeometryPacketUploadRecord(packet));
	}
	for (const GpuVirtualGeometryBatchDescriptor& batch : frameData.virtualGeometryGpuBatches)
	{
		const std::size_t batchBegin = batch.packetOffset;
		const std::size_t batchEnd = batchBegin + batch.packetCount;
		if (batch.packetCount == 0u
			|| batchBegin >= packetUploadRecords.size()
			|| batchEnd > packetUploadRecords.size())
		{
			continue;
		}
		// Graphics records the compatible batch at its first packet descriptor.
		// Carry that same stable slot through compaction so the device can
		// aggregate every surviving packet without relying on compacted order.
		const std::uint32_t generatedCommandIndex =
			packetUploadRecords[batchBegin].packetId;
		const std::uint32_t generatedCommandTaskGroupCount =
			(std::max(batch.submittedMeshlets, 1u) + 31u) / 32u;
		for (std::size_t packetIndex = batchBegin; packetIndex < batchEnd; ++packetIndex)
		{
			packetUploadRecords[packetIndex].generatedCommandIndex =
				generatedCommandIndex;
			packetUploadRecords[packetIndex].generatedCommandTaskGroupCount =
				generatedCommandTaskGroupCount;
		}
	}
	ensureAndWriteBuffer(
		resources.packetBuffer,
		resources.packetCapacity,
		packetUploadRecords.size(),
		sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord),
		packetUploadRecords.data(),
		"Virtual Geometry GPU Packet Buffer");
	// Traversal consumes a compact, fixed-stride instance table rather than the
	// larger packet record. Every entry carries this frame resource generation so
	// a stale table fail-opens to a bounded fallback request in the shader.
	struct TraversalInstanceWords { std::uint32_t root, generation, flags, reserved; };
	std::vector<TraversalInstanceWords> traversalInstances;
	traversalInstances.reserve(frameData.virtualGeometryGpuPackets.size());
	for(const GpuVirtualGeometryPacketRecord& packet : frameData.virtualGeometryGpuPackets) {
		traversalInstances.push_back({ packet.clusterId, static_cast<std::uint32_t>(resources.resourceSerial), packet.flags, 0u });
	}
	std::vector<std::uint32_t> traversalRequests(std::max<std::size_t>(3u, traversalInstances.size() * 3u), 0u);
	std::array<std::uint32_t, 4> traversalCounters{};
	ensureAndWriteBuffer(resources.traversalInstanceBuffer, resources.traversalInstanceCapacity,
		traversalInstances.size(), sizeof(TraversalInstanceWords), traversalInstances.data(), "Virtual Geometry Traversal Instance Buffer");
	ensureAndWriteBuffer(resources.traversalRequestBuffer, resources.traversalRequestCapacity,
		traversalRequests.size(), sizeof(std::uint32_t), traversalRequests.data(), "Virtual Geometry Traversal Request Buffer");
	std::size_t traversalCounterCapacity = 4u;
	ensureAndWriteBuffer(resources.traversalCounterBuffer, traversalCounterCapacity,
		traversalCounters.size(), sizeof(std::uint32_t), traversalCounters.data(), "Virtual Geometry Traversal Counter Buffer");
	ensureAndWriteBuffer(
		resources.batchBuffer,
		resources.batchCapacity,
		frameData.virtualGeometryGpuBatches.size(),
		sizeof(GpuVirtualGeometryBatchDescriptor),
		frameData.virtualGeometryGpuBatches.data(),
		"Virtual Geometry GPU Batch Descriptor Buffer");
	// Compaction is the only producer of this output. Starting from zero makes
	// every rejected or unused slot explicit zero work for indirect generation.
	std::vector<TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord>
		compactedPacketUploadRecords(packetUploadRecords.size());
	ensureAndWriteBuffer(
		resources.compactedPacketBuffer,
		resources.compactedPacketCapacity,
		compactedPacketUploadRecords.size(),
		sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord),
		compactedPacketUploadRecords.data(),
		"Virtual Geometry GPU Compacted Packet Buffer");
	// The compute shader owns these output ranges. Clear their CPU-mapped memory
	// before recording so counters cannot inherit a prior frame's values; the
	// existing CPU draw records remain the independent fail-open authority.
	std::vector<std::uint32_t> generatedSubmissionWords(
		std::max<std::size_t>(4u, frameData.virtualGeometryGpuPackets.size() * 4u), 0u);
	std::vector<std::uint32_t> counterReadbackWords(4u, 0u);
	ensureAndWriteBuffer(
		resources.generatedSubmissionBuffer,
		resources.generatedSubmissionCapacity,
		generatedSubmissionWords.size(),
		sizeof(std::uint32_t),
		generatedSubmissionWords.data(),
		"Virtual Geometry Generated Submission Buffer",
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
	// Older frame-slot resources predate indirect usage. Recreate before any
	// generated command can be consumed so Vulkan validation never sees a
	// storage-only buffer at an indirect boundary.
	if(!resources.generatedSubmissionSupportsIndirect && resources.generatedSubmissionBuffer.buffer != VK_NULL_HANDLE) {
		RetireBuffer(resources.generatedSubmissionBuffer);
		resources.generatedSubmissionBuffer = CreateBuffer(
			static_cast<VkDeviceSize>(generatedSubmissionWords.size() * sizeof(std::uint32_t)),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU, "Virtual Geometry Generated Submission Buffer");
		resources.generatedSubmissionCapacity = generatedSubmissionWords.size();
		WriteMappedBuffer(allocator, resources.generatedSubmissionBuffer, generatedSubmissionWords.data(),
			static_cast<VkDeviceSize>(generatedSubmissionWords.size() * sizeof(std::uint32_t)));
		resources.generatedSubmissionSupportsIndirect = true;
	}
	ensureAndWriteBuffer(
		resources.counterReadbackBuffer,
		resources.counterReadbackCapacity,
		counterReadbackWords.size(),
		sizeof(std::uint32_t),
		counterReadbackWords.data(),
		"Virtual Geometry Counter Readback Buffer");
	std::array<std::uint32_t, 3> zeroWorkIndirect{};
	std::size_t zeroWorkIndirectCapacity = 3u;
	if(resources.zeroWorkIndirectBuffer.buffer == VK_NULL_HANDLE) {
		resources.zeroWorkIndirectBuffer = CreateBuffer(sizeof(zeroWorkIndirect),
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU, "Virtual Geometry Zero Work Indirect Buffer");
	}
	WriteMappedBuffer(allocator, resources.zeroWorkIndirectBuffer, zeroWorkIndirect.data(), sizeof(zeroWorkIndirect));

	// The page-local virtual geometry payload appends a shader-decodable stream
	// after the packet descriptor table. Building that stream performs remapping
	// and temporary allocations, so keep the result in this frame slot when
	// the packet cut and loaded mesh instances are unchanged.
	const std::uint64_t pagePayloadSignature =
		BuildVirtualGeometryPagePayloadResourceSignature(frameData.virtualGeometryGpuPackets);
	const bool reusePagePayload = TerraEngine::Renderer::Vulkan::CanReuseVirtualGeometryPagePayload(
		resources.pagePayloadReuseSignature,
		pagePayloadSignature,
		resources.pagePayloadCacheValid,
		resources.pagePayloadBuffer.buffer != VK_NULL_HANDLE);
	if (!reusePagePayload)
	{
		VirtualGeometryPageLocalPayloadBuildResult pagePayloadBuild;
		{
			TERRAENGINE_PROFILE_SCOPE("Vulkan: Build Virtual Geometry Page Payload", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			pagePayloadBuild = BuildVirtualGeometryPageLocalPayloadWords(frameData.virtualGeometryGpuPackets);
		}
		resources.pagePayloadBytesByMesh = std::move(pagePayloadBuild.bytesByMesh);
		{
			TERRAENGINE_PROFILE_SCOPE("Vulkan: Upload Virtual Geometry Page Payload", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			ensureAndWriteBuffer(
				resources.pagePayloadBuffer,
				resources.pagePayloadCapacity,
				pagePayloadBuild.words.size(),
				sizeof(std::uint32_t),
				pagePayloadBuild.words.data(),
				"Virtual Geometry Physical Page Payload Descriptor Buffer");
		}
		resources.pagePayloadReuseSignature = pagePayloadSignature;
		resources.pagePayloadCacheValid = resources.pagePayloadBuffer.buffer != VK_NULL_HANDLE
			&& !pagePayloadBuild.words.empty();
	}

	auto publishBindlessBufferForCurrentFrame = [&](
		std::uint32_t& bindlessIndex,
		VirtualGeometryGpuPacketFrameResources::BindlessBufferPublication& publication,
		const VulkanBuffer& buffer) -> std::uint32_t {
		if (buffer.buffer == VK_NULL_HANDLE || buffer.size == 0u)
		{
			return INVALID_BINDLESS_BUFFER_INDEX;
		}
		if (bindlessIndex == INVALID_BINDLESS_BUFFER_INDEX)
		{
			bindlessIndex = AllocateBufferIndex();
			publication = {};
		}
		const bool alreadyPublished =
			publication.buffer == buffer.buffer
			&& publication.range == buffer.size;
		if (!alreadyPublished)
		{
			// The global bindless descriptor set is shared by every editor view
			// in this frame slot. Only publish when the handle or range changes;
			// otherwise an unnecessary descriptor update can invalidate a Scene
			// View command buffer that was recorded before Game View submission.
			UpdateGlobalBufferForFrame(currentFrame, bindlessIndex, buffer.buffer, 0u, buffer.size);
			publication.buffer = buffer.buffer;
			publication.range = buffer.size;
		}
		return bindlessIndex;
	};
	activeVirtualGeometryPacketBufferIndex = publishBindlessBufferForCurrentFrame(resources.packetBufferIndex, resources.packetPublication, resources.packetBuffer);
	activeVirtualGeometryBatchBufferIndex = publishBindlessBufferForCurrentFrame(resources.batchBufferIndex, resources.batchPublication, resources.batchBuffer);
	activeVirtualGeometryCompactedPacketBufferIndex = publishBindlessBufferForCurrentFrame(resources.compactedPacketBufferIndex, resources.compactedPacketPublication, resources.compactedPacketBuffer);
	activeVirtualGeometryGeneratedSubmissionBufferIndex = publishBindlessBufferForCurrentFrame(resources.generatedSubmissionBufferIndex, resources.generatedSubmissionPublication, resources.generatedSubmissionBuffer);
	activeVirtualGeometryCounterReadbackBufferIndex = publishBindlessBufferForCurrentFrame(resources.counterReadbackBufferIndex, resources.counterReadbackPublication, resources.counterReadbackBuffer);
	// These page payload source buffers are owned by the current frame slot.
	// They stay alive until the frame is safely retired, which prevents Scene
	// View/Game View from sampling a buffer that another view already reused.
	activeVirtualGeometryPagePayloadBufferIndex = publishBindlessBufferForCurrentFrame(resources.pagePayloadBufferIndex, resources.pagePayloadPublication, resources.pagePayloadBuffer);

	// Build a private descriptor set rather than borrowing the graphics bindless
	// set: the compaction shader has a fixed four-storage-buffer ABI at set 0.
	// Allocation or shader setup failures remain a diagnosed CPU fallback.
	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorResources executorResources;
	executorResources.packetBuffer = resources.packetBuffer.buffer;
	executorResources.traversalInstanceBuffer = resources.traversalInstanceBuffer.buffer;
	executorResources.traversalRequestBuffer = resources.traversalRequestBuffer.buffer;
	executorResources.traversalCounterBuffer = resources.traversalCounterBuffer.buffer;
	executorResources.compactedPacketBuffer = resources.compactedPacketBuffer.buffer;
	executorResources.generatedSubmissionBuffer = resources.generatedSubmissionBuffer.buffer;
	executorResources.counterReadbackBuffer = resources.counterReadbackBuffer.buffer;
	executorResources.zeroWorkIndirectBuffer = resources.zeroWorkIndirectBuffer.buffer;
	executorResources.resourcesOwnedByFrame = resources.live;
	const bool executorBuffersValid = executorResources.packetBuffer != VK_NULL_HANDLE
		&& executorResources.compactedPacketBuffer != VK_NULL_HANDLE
		&& executorResources.generatedSubmissionBuffer != VK_NULL_HANDLE
		&& executorResources.counterReadbackBuffer != VK_NULL_HANDLE
		&& executorResources.traversalInstanceBuffer != VK_NULL_HANDLE
		&& executorResources.traversalRequestBuffer != VK_NULL_HANDLE
		&& executorResources.traversalCounterBuffer != VK_NULL_HANDLE
		&& executorResources.zeroWorkIndirectBuffer != VK_NULL_HANDLE;
	if(!executorBuffersValid) {
		virtualGeometryExecutorUnavailableReason = "Virtual-geometry compaction storage buffers are unavailable for this view.";
	}
	else if(EnsureVulkanVirtualGeometryExecutorPipeline()) {
		if(resources.executorDescriptorSet == VK_NULL_HANDLE) {
			VkDescriptorSetAllocateInfo allocationInfo{};
			allocationInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocationInfo.descriptorPool = descriptorPool;
			allocationInfo.descriptorSetCount = 1u;
			allocationInfo.pSetLayouts = &virtualGeometryExecutorDescriptorSetLayout;
			if(vkAllocateDescriptorSets(device, &allocationInfo, &resources.executorDescriptorSet) != VK_SUCCESS) {
				virtualGeometryExecutorUnavailableReason = "Could not allocate the virtual-geometry compaction descriptor set.";
			}
		}
		if(resources.executorDescriptorSet != VK_NULL_HANDLE) {
			const std::array<VkDescriptorBufferInfo, 7> bufferInfos{
				VkDescriptorBufferInfo{ resources.traversalInstanceBuffer.buffer, 0u, resources.traversalInstanceBuffer.size },
				VkDescriptorBufferInfo{ resources.traversalRequestBuffer.buffer, 0u, resources.traversalRequestBuffer.size },
				VkDescriptorBufferInfo{ resources.traversalCounterBuffer.buffer, 0u, resources.traversalCounterBuffer.size },
				VkDescriptorBufferInfo{ resources.packetBuffer.buffer, 0u, resources.packetBuffer.size },
				VkDescriptorBufferInfo{ resources.compactedPacketBuffer.buffer, 0u, resources.compactedPacketBuffer.size },
				VkDescriptorBufferInfo{ resources.generatedSubmissionBuffer.buffer, 0u, resources.generatedSubmissionBuffer.size },
				VkDescriptorBufferInfo{ resources.counterReadbackBuffer.buffer, 0u, resources.counterReadbackBuffer.size } };
			std::array<VkWriteDescriptorSet, 7> writes{};
			for(std::uint32_t binding = 0u; binding < writes.size(); ++binding) {
				writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[binding].dstSet = resources.executorDescriptorSet;
				writes[binding].dstBinding = binding;
				writes[binding].descriptorCount = 1u;
				writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				writes[binding].pBufferInfo = &bufferInfos[binding];
			}
			vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
			executorResources.descriptorSet = resources.executorDescriptorSet;
		}
	}
	const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorPipelines executorPipelines =
		virtualGeometryExecutorPipelines;
	const std::uint64_t executorGeneration = resources.resourceSerial;
	const std::uint32_t boundedCapacity = static_cast<std::uint32_t>(
		std::min<std::size_t>(resources.packetCapacity, std::numeric_limits<std::uint32_t>::max()));
	latestVirtualGeometryExecutorPlan =
		TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryExecutorPlan(
			static_cast<std::uint32_t>(std::min<std::size_t>(frameData.virtualGeometryGpuPackets.size(), std::numeric_limits<std::uint32_t>::max())),
			boundedCapacity,
			false,
			executorResources,
			executorPipelines,
			frameId,
			executorGeneration,
			executorGeneration,
			executorGeneration);
	latestVirtualGeometryExecutorFailure =
		TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryFailureReceipt(
			latestVirtualGeometryExecutorPlan,
			TerraEngine::Renderer::VirtualGeometryBackendStage::Compaction);
	latestVirtualGeometryExecutorResources = executorResources;
	if(!virtualGeometryExecutorUnavailableReason.empty()
		&& latestVirtualGeometryExecutorPlan.unavailableReason.empty()) {
		latestVirtualGeometryExecutorPlan.unavailableReason = virtualGeometryExecutorUnavailableReason;
		latestVirtualGeometryExecutorFailure =
			TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryFailureReceipt(
				latestVirtualGeometryExecutorPlan,
				TerraEngine::Renderer::VirtualGeometryBackendStage::Compaction);
	}

	// Decode the selected owner meshlets into the exact RasterTriangle ABI.
	// Compute never reinterprets a large C++ packet structure as triangle input,
	// and every primitive carries the same material/route identity selected by
	// RenderSystem.
	constexpr std::size_t kMaximumVisibilityPrimitiveCount = 1u << 20u;
	constexpr std::uint64_t kMaximumVisibilityPixelCount = 1u << 24u;
	std::vector<VirtualGeometryRasterTriangleUpload> visibilityTriangles;
	visibilityTriangles.reserve(std::min<std::size_t>(
		frameData.virtualGeometryGpuPackets.size() * 64u,
		kMaximumVisibilityPrimitiveCount));
	for(const GpuVirtualGeometryPacketRecord& packet :
		frameData.virtualGeometryGpuPackets)
	{
		if(visibilityTriangles.size() >= kMaximumVisibilityPrimitiveCount)
		{
			break;
		}
		const Mesh* mesh = ResourceManager::Instance().GetMesh(packet.meshId);
		const auto commandIt = std::find_if(
			frameData.commands.begin(),
			frameData.commands.end(),
			[&packet](const DrawCommand& command) {
				const std::uint64_t commandEnd =
					static_cast<std::uint64_t>(command.virtualMeshletOffset)
					+ command.virtualMeshletCount;
				const std::uint64_t packetEnd =
					static_cast<std::uint64_t>(packet.meshletOffset)
					+ packet.meshletCount;
				return command.objectId == packet.objectId
					&& command.meshId == packet.meshId
					&& command.materialId == packet.materialId
					&& command.virtualMeshletOffset <= packet.meshletOffset
					&& commandEnd >= packetEnd;
			});
		if(mesh == nullptr || commandIt == frameData.commands.end())
		{
			continue;
		}
		const glm::mat4 clipTransform =
			frameData.projectionMatrix * frameData.viewMatrix
			* commandIt->modelMatrix;
		const std::uint32_t endMeshlet = std::min<std::uint32_t>(
			packet.meshletOffset + packet.meshletCount,
			static_cast<std::uint32_t>(mesh->meshletRecords.size()));
		for(std::uint32_t meshletIndex = packet.meshletOffset;
			meshletIndex < endMeshlet
				&& visibilityTriangles.size()
					< kMaximumVisibilityPrimitiveCount;
			++meshletIndex)
		{
			const MeshletRecord& meshlet = mesh->meshletRecords[meshletIndex];
			if(meshlet.vertexCount == 0u
				|| (meshlet.primitiveIndexCount % 3u) != 0u
				|| static_cast<std::size_t>(meshlet.primitiveIndexOffset)
					+ meshlet.primitiveIndexCount
					> mesh->meshletPrimitiveIndexData.size())
			{
				continue;
			}
			for(std::uint32_t primitiveOffset = 0u;
				primitiveOffset < meshlet.primitiveIndexCount
					&& visibilityTriangles.size()
						< kMaximumVisibilityPrimitiveCount;
				primitiveOffset += 3u)
			{
				glm::vec4 clipPositions[3]{};
				bool validTriangle = true;
				for(std::uint32_t corner = 0u; corner < 3u; ++corner)
				{
					const std::uint8_t localVertex =
						mesh->meshletPrimitiveIndexData[
							static_cast<std::size_t>(
								meshlet.primitiveIndexOffset)
							+ primitiveOffset + corner];
					std::uint32_t globalVertex = 0u;
					if(localVertex >= meshlet.vertexCount
						|| !TryReadVirtualGeometryRemapIndex(
							*mesh,
							meshlet.vertexOffset + localVertex,
							globalVertex))
					{
						validTriangle = false;
						break;
					}
					const std::size_t vertexByteOffset =
						static_cast<std::size_t>(globalVertex)
						* kMeshletPackedVertexStrideBytes;
					if(vertexByteOffset
						+ sizeof(TerraEngine::VirtualGeometryPackedVertex)
						> mesh->meshletVertexData.size())
					{
						validTriangle = false;
						break;
					}
					TerraEngine::VirtualGeometryPackedVertex packedVertex{};
					std::memcpy(
						&packedVertex,
						mesh->meshletVertexData.data()
							+ vertexByteOffset,
						sizeof(packedVertex));
					const std::array<float, 3u> decodedPosition =
						TerraEngine::DecodeVirtualGeometryPackedPosition(
							packedVertex);
					const glm::vec3 normalizedPosition(
						decodedPosition[0],
						decodedPosition[1],
						decodedPosition[2]);
					const glm::vec3 localPosition(
						mesh->runtimeLayout.quantization.positionOffset[0]
							+ normalizedPosition.x
								* mesh->runtimeLayout.quantization.positionScale[0],
						mesh->runtimeLayout.quantization.positionOffset[1]
							+ normalizedPosition.y
								* mesh->runtimeLayout.quantization.positionScale[1],
						mesh->runtimeLayout.quantization.positionOffset[2]
							+ normalizedPosition.z
								* mesh->runtimeLayout.quantization.positionScale[2]);
					clipPositions[corner] =
						clipTransform * glm::vec4(localPosition, 1.0f);
				}
				if(!validTriangle)
				{
					continue;
				}
				VirtualGeometryRasterTriangleUpload triangle;
				triangle.clip0 = clipPositions[0];
				triangle.clip1 = clipPositions[1];
				triangle.clip2 = clipPositions[2];
				triangle.stablePrimitiveId =
					static_cast<std::uint32_t>(
						visibilityTriangles.size());
				const std::uint32_t materialBin =
					(packet.flags >> kVirtualGeometryMaterialBinShift)
					& kVirtualGeometryMaterialBinMask;
				const std::uint32_t rasterRoute =
					(packet.flags >> kVirtualGeometryRasterRouteShift)
					& kVirtualGeometryRasterRouteMask;
				triangle.packedOwnerMaterial =
					materialBin | (rasterRoute << 8u);
				triangle.sourceGeneration =
					static_cast<std::uint32_t>(executorGeneration);
				triangle.flags = packet.flags;
				visibilityTriangles.push_back(triangle);
			}
		}
	}
	ensureAndWriteBuffer(
		resources.visibilityTriangleBuffer,
		resources.visibilityTriangleCapacity,
		visibilityTriangles.size(),
		sizeof(VirtualGeometryRasterTriangleUpload),
		visibilityTriangles.data(),
		"Virtual Geometry Compute Raster Triangle Buffer");

	// Visibility records are pixel-addressed, not primitive-addressed. Preserve
	// the view's real aspect ratio and fail closed instead of allocating an
	// unbounded target supplied by malformed automation input.
	glm::uvec2 visibilityTargetSize(0u);
	for(const GpuVirtualGeometryPacketFrameSet& frameSet :
		frameData.virtualGeometryGpuFrameSets)
	{
		if(frameSet.viewType == viewType && frameSet.viewId == viewId)
		{
			visibilityTargetSize = frameSet.targetSize;
			break;
		}
	}
	const std::uint64_t requestedPixelCount =
		static_cast<std::uint64_t>(visibilityTargetSize.x)
		* static_cast<std::uint64_t>(visibilityTargetSize.y);
	const std::size_t visibilityPixelCount =
		requestedPixelCount != 0u
			&& requestedPixelCount <= kMaximumVisibilityPixelCount
		? static_cast<std::size_t>(requestedPixelCount)
		: 0u;
	if(visibilityPixelCount == 0u)
	{
		virtualGeometryVisibilityUnavailableReason =
			"Virtual-geometry visibility target is empty or exceeds the bounded pixel capacity.";
	}
	std::vector<std::uint32_t> visibilityDepthWords(
		visibilityPixelCount,
		std::numeric_limits<std::uint32_t>::max());
	std::vector<std::uint32_t> visibilityRecordWords(
		visibilityPixelCount * 4u,
		0u);
	for(std::size_t pixel = 0u; pixel < visibilityPixelCount; ++pixel)
	{
		visibilityRecordWords[pixel * 4u] =
			std::numeric_limits<std::uint32_t>::max();
	}
	constexpr std::size_t kVisibilityMaterialBinCount = 64u;
	std::array<std::uint32_t, kVisibilityMaterialBinCount>
		shadingBinWords{};
	ensureAndWriteBuffer(resources.visibilityDepthBuffer, resources.visibilityDepthCapacity,
		visibilityDepthWords.size(), sizeof(std::uint32_t), visibilityDepthWords.data(), "Virtual Geometry Visibility Depth Buffer");
	ensureAndWriteBuffer(resources.visibilityRecordBuffer, resources.visibilityRecordCapacity,
		visibilityRecordWords.size(), sizeof(std::uint32_t), visibilityRecordWords.data(), "Virtual Geometry Visibility Record Buffer");
	ensureAndWriteBuffer(resources.resolvedVisibilityBuffer, resources.resolvedVisibilityCapacity,
		visibilityRecordWords.size(), sizeof(std::uint32_t), visibilityRecordWords.data(), "Virtual Geometry Resolved Visibility Buffer");
	ensureAndWriteBuffer(resources.shadingBinBuffer, resources.shadingBinCapacity,
		shadingBinWords.size(), sizeof(std::uint32_t), shadingBinWords.data(), "Virtual Geometry Shading Bin Buffer");

	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityResources visibilityResources;
	visibilityResources.inputTriangleBuffer =
		resources.visibilityTriangleBuffer.buffer;
	visibilityResources.depthBuffer = resources.visibilityDepthBuffer.buffer;
	visibilityResources.visibilityRecordBuffer = resources.visibilityRecordBuffer.buffer;
	visibilityResources.resolvedVisibilityBuffer = resources.resolvedVisibilityBuffer.buffer;
	visibilityResources.shadingBinBuffer = resources.shadingBinBuffer.buffer;
	visibilityResources.diagnosticBuffer = resources.counterReadbackBuffer.buffer;
	visibilityResources.resourcesOwnedByFrame = resources.live;
	if(visibilityResources.inputTriangleBuffer == VK_NULL_HANDLE
		|| visibilityResources.depthBuffer == VK_NULL_HANDLE
		|| visibilityResources.visibilityRecordBuffer == VK_NULL_HANDLE
		|| visibilityResources.resolvedVisibilityBuffer == VK_NULL_HANDLE
		|| visibilityResources.shadingBinBuffer == VK_NULL_HANDLE
		|| visibilityResources.diagnosticBuffer == VK_NULL_HANDLE) {
		virtualGeometryVisibilityUnavailableReason = "Virtual-geometry visibility frame storage buffers are unavailable for this view.";
	}
	else if(EnsureVulkanVirtualGeometryVisibilityPipelines()) {
		if(resources.visibilityComputeRasterDescriptorSet == VK_NULL_HANDLE
			|| resources.visibilityResolveDescriptorSet == VK_NULL_HANDLE
			|| resources.visibilityShadingBinsDescriptorSet == VK_NULL_HANDLE) {
			const std::array<VkDescriptorSetLayout, 3> layouts{
				virtualGeometryVisibilityDescriptorSetLayout,
				virtualGeometryVisibilityDescriptorSetLayout,
				virtualGeometryVisibilityDescriptorSetLayout };
			std::array<VkDescriptorSet, 3> descriptorSets{};
			VkDescriptorSetAllocateInfo allocationInfo{};
			allocationInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocationInfo.descriptorPool = descriptorPool;
			allocationInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
			allocationInfo.pSetLayouts = layouts.data();
			if(vkAllocateDescriptorSets(device, &allocationInfo, descriptorSets.data()) != VK_SUCCESS) {
				virtualGeometryVisibilityUnavailableReason = "Could not allocate virtual-geometry visibility descriptor sets.";
			}
			else {
				resources.visibilityComputeRasterDescriptorSet = descriptorSets[0];
				resources.visibilityResolveDescriptorSet = descriptorSets[1];
				resources.visibilityShadingBinsDescriptorSet = descriptorSets[2];
			}
		}
		if(resources.visibilityComputeRasterDescriptorSet != VK_NULL_HANDLE
			&& resources.visibilityResolveDescriptorSet != VK_NULL_HANDLE
			&& resources.visibilityShadingBinsDescriptorSet != VK_NULL_HANDLE) {
			const std::array<std::array<VkDescriptorBufferInfo, 4>, 3> bufferInfos{{
				{{ { resources.visibilityTriangleBuffer.buffer, 0u, resources.visibilityTriangleBuffer.size }, { resources.visibilityDepthBuffer.buffer, 0u, resources.visibilityDepthBuffer.size }, { resources.visibilityRecordBuffer.buffer, 0u, resources.visibilityRecordBuffer.size }, { resources.counterReadbackBuffer.buffer, 0u, resources.counterReadbackBuffer.size } }},
				{{ { resources.visibilityRecordBuffer.buffer, 0u, resources.visibilityRecordBuffer.size }, { resources.resolvedVisibilityBuffer.buffer, 0u, resources.resolvedVisibilityBuffer.size }, { resources.counterReadbackBuffer.buffer, 0u, resources.counterReadbackBuffer.size }, { resources.visibilityTriangleBuffer.buffer, 0u, resources.visibilityTriangleBuffer.size } }},
				{{ { resources.resolvedVisibilityBuffer.buffer, 0u, resources.resolvedVisibilityBuffer.size }, { resources.shadingBinBuffer.buffer, 0u, resources.shadingBinBuffer.size }, { resources.counterReadbackBuffer.buffer, 0u, resources.counterReadbackBuffer.size }, { resources.counterReadbackBuffer.buffer, 0u, resources.counterReadbackBuffer.size } }} }};
			const std::array<VkDescriptorSet, 3> descriptorSets{
				resources.visibilityComputeRasterDescriptorSet,
				resources.visibilityResolveDescriptorSet,
				resources.visibilityShadingBinsDescriptorSet };
			std::array<VkWriteDescriptorSet, 12> writes{};
			for(std::uint32_t passIndex = 0u; passIndex < descriptorSets.size(); ++passIndex) {
				for(std::uint32_t binding = 0u; binding < bufferInfos[passIndex].size(); ++binding) {
					VkWriteDescriptorSet& write = writes[passIndex * bufferInfos[passIndex].size() + binding];
					write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					write.dstSet = descriptorSets[passIndex];
					write.dstBinding = binding;
					write.descriptorCount = 1u;
					write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					write.pBufferInfo = &bufferInfos[passIndex][binding];
				}
			}
			vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
			visibilityResources.computeRasterDescriptorSet = resources.visibilityComputeRasterDescriptorSet;
			visibilityResources.visibilityResolveDescriptorSet = resources.visibilityResolveDescriptorSet;
			visibilityResources.shadingBinsDescriptorSet = resources.visibilityShadingBinsDescriptorSet;
		}
	}
	const std::uint32_t boundedVisibilityCapacity =
		static_cast<std::uint32_t>(std::min<std::size_t>(
			resources.visibilityTriangleCapacity,
			std::numeric_limits<std::uint32_t>::max()));
	const glm::uvec2 boundedVisibilityTargetSize =
		visibilityPixelCount != 0u
			? visibilityTargetSize
			: glm::uvec2(0u);
	latestVirtualGeometryVisibilityPlan = TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryVisibilityPlan(
		static_cast<std::uint32_t>(std::min<std::size_t>(
			visibilityTriangles.size(),
			std::numeric_limits<std::uint32_t>::max())),
		boundedVisibilityCapacity,
		boundedVisibilityTargetSize.x,
		boundedVisibilityTargetSize.y,
		static_cast<std::uint32_t>(kVisibilityMaterialBinCount),
		visibilityResources,
		virtualGeometryVisibilityPipelines,
		frameId,
		executorGeneration);
	latestVirtualGeometryVisibilityResources = visibilityResources;
	if(!virtualGeometryVisibilityUnavailableReason.empty()
		&& latestVirtualGeometryVisibilityPlan.unavailableReason.empty()) {
		latestVirtualGeometryVisibilityPlan.unavailableReason = virtualGeometryVisibilityUnavailableReason;
	}

	// CPU-expanded commands remain the diagnosed fail-open fallback. When all
	// executor stages record successfully, the main mesh-task pass consumes the
	// GPU-authored 16-byte indirect records from this frame-owned buffer.
}

// VulkanContext::Impl::CopyBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return;
	}

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	(void)EndSingleTimeCommands(commandBuffer, "buffer copy");
}

// VulkanContext::Impl::BeginSingleTimeCommands is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkCommandBuffer VulkanContext::Impl::BeginSingleTimeCommands() {
	std::lock_guard<std::mutex> lock(immediateCommandMutex);
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = immediateCommandPool != VK_NULL_HANDLE ? immediateCommandPool : commandPools[currentFrame];
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to allocate immediate command buffer");
		return VK_NULL_HANDLE;
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to begin immediate command buffer");
		vkFreeCommandBuffers(device, allocInfo.commandPool, 1, &commandBuffer);
		return VK_NULL_HANDLE;
	}
	RecordDiagnosticCheckpoint(commandBuffer, "immediate.begin");

	return commandBuffer;
}

// VulkanContext::Impl::EndSingleTimeCommands is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::EndSingleTimeCommands(VkCommandBuffer commandBuffer, std::string_view operation) {
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(immediateCommandMutex);
	RecordDiagnosticCheckpoint(commandBuffer, "immediate.end");
	const VkCommandPool owningPool = immediateCommandPool != VK_NULL_HANDLE ? immediateCommandPool : commandPools[currentFrame];
	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to end immediate command buffer");
		vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
		return false;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence immediateFence = VK_NULL_HANDLE;
	if (vkCreateFence(device, &fenceInfo, nullptr, &immediateFence) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to create immediate command fence");
		vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
		return false;
	}

	const VkResult submitResult = SubmitGraphicsQueue(1, &submitInfo, immediateFence);
	if (submitResult != VK_SUCCESS)
	{
		const std::string submitOperation = std::string(operation) + " submit";
		if (MarkDeviceLost(submitResult, submitOperation))
		{
			vkDestroyFence(device, immediateFence, nullptr);
			return false;
		}
		TERRAENGINE_LOG(LogLevel::Error, "failed to submit immediate command buffer: " << submitResult);
		// A failed submit can leave ownership ambiguous on validation/device-loss
		// paths. Do not free the command buffer unless the queue proves idle;
		// freeing a possibly-pending upload buffer creates the secondary
		// vkFreeCommandBuffers validation error seen after editor asset removal.
		const VkResult idleResult = WaitGraphicsQueueIdle();
		vkDestroyFence(device, immediateFence, nullptr);
		if (idleResult == VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
		}
		if (MarkDeviceLost(idleResult, "immediate command failed-submit queue idle"))
		{
			return false;
		}
		return false;
	}

	const VkResult waitResult = vkWaitForFences(device, 1, &immediateFence, VK_TRUE, UINT64_MAX);
	if (waitResult != VK_SUCCESS)
	{
		// Preserve the caller's purpose in terminal diagnostics. A device loss is
		// often reported by this fence after an earlier queue command caused the
		// fault, so a generic "immediate command" label hides the useful boundary.
		const std::string waitOperation = std::string(operation) + " fence wait";
		if (MarkDeviceLost(waitResult, waitOperation))
		{
			vkDestroyFence(device, immediateFence, nullptr);
			return false;
		}
		TERRAENGINE_LOG(LogLevel::Error, "failed to wait for immediate command fence: " << waitResult);
		// The fence is a temporary ownership marker for this helper. Destroy it
		// even on a wait failure so shutdown does not leak child objects; command
		// buffer freeing still stays gated on the queue proving idle/completed.
		const VkResult idleResult = WaitGraphicsQueueIdle();
		vkDestroyFence(device, immediateFence, nullptr);
		if (idleResult == VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
		}
		if (MarkDeviceLost(idleResult, "immediate command failed-wait queue idle"))
		{
			return false;
		}
		return false;
	}

	vkDestroyFence(device, immediateFence, nullptr);

	vkFreeCommandBuffers(device, owningPool, 1, &commandBuffer);
	return true;
}

// VulkanContext::Impl::TransitionImageLayout is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels, uint32_t arrayLayers) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return false;
	}

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;

	// Determine aspect mask based on format
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || 
		format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || 
		format == VK_FORMAT_D24_UNORM_S8_UINT) {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT) {
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	} else {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}

	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = mipLevels == 0u ? VK_REMAINING_MIP_LEVELS : mipLevels;
	barrier.subresourceRange.baseArrayLayer = 0;
	// Cubemap resources contain six array layers. Let callers transition the
	// complete resource so every framebuffer face starts from the declared layout.
	barrier.subresourceRange.layerCount = std::max(arrayLayers, 1u);

	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	} else {
		throw std::invalid_argument("unsupported layout transition!");
	}

	vkCmdPipelineBarrier(
		commandBuffer,
		sourceStage, destinationStage,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier
	);

	const std::string transitionOperation = "image layout transition "
		+ std::to_string(static_cast<int>(oldLayout))
		+ "->"
		+ std::to_string(static_cast<int>(newLayout))
		+ " format="
		+ std::to_string(static_cast<int>(format))
		+ " layers="
		+ std::to_string(arrayLayers);
	return EndSingleTimeCommands(commandBuffer, transitionOperation);
}

// VulkanContext::Impl::CopyBufferToImage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return false;
	}

	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = {
		width,
		height,
		1
	};

	vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	return EndSingleTimeCommands(commandBuffer, "buffer-to-image copy");
}

// VulkanContext::Impl::CreateImageView is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkImageView VulkanContext::Impl::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels) {
	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = std::max(1u, mipLevels);
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	VkImageView imageView;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
		throw std::runtime_error("failed to create texture image view!");
	}

	return imageView;
}

// VulkanContext::Impl::StageVisibilityToVulkan is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkShaderStageFlags VulkanContext::Impl::StageVisibilityToVulkan(StageVisibilityMask visibility) {
	VkShaderStageFlags flags = 0;
	for (std::uint32_t bit = 0; bit < 32; ++bit) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((visibility & (1u << bit)) == 0) continue;
		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (static_cast<ShaderStage>(bit)) {
			case ShaderStage::Vertex: flags |= VK_SHADER_STAGE_VERTEX_BIT; break;
			case ShaderStage::Fragment: flags |= VK_SHADER_STAGE_FRAGMENT_BIT; break;
			case ShaderStage::Geometry: flags |= VK_SHADER_STAGE_GEOMETRY_BIT; break;
			case ShaderStage::TessellationControl: flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; break;
			case ShaderStage::TessellationEvaluation: flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; break;
			case ShaderStage::Compute: flags |= VK_SHADER_STAGE_COMPUTE_BIT; break;
			case ShaderStage::Mesh: flags |= VK_SHADER_STAGE_MESH_BIT_EXT; break;
			case ShaderStage::Task: flags |= VK_SHADER_STAGE_TASK_BIT_EXT; break;
			default: break;
		}
	}
	return flags;
}

// VulkanContext::Impl::MapDescriptorType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkDescriptorType VulkanContext::Impl::MapDescriptorType(DescriptorType type) {
	switch (type) {
		case DescriptorType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		case DescriptorType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		case DescriptorType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
		case DescriptorType::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		case DescriptorType::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		case DescriptorType::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		case DescriptorType::InputAttachment: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		case DescriptorType::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		default: return VK_DESCRIPTOR_TYPE_SAMPLER;
	}
}

// VulkanContext::Impl::DestroyPipelines is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyPipelines() {
	// Mesh descriptor sets are allocated from per-pipeline set layouts, so they
	// must be freed before the corresponding layouts are destroyed.
	std::vector<VkDescriptorSet> meshDescriptorSets;
	for (auto& [meshId, mesh] : meshCache) {
		for (const auto& [layout, descriptorSet] : mesh.descriptorSetsByLayout) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (descriptorSet != VK_NULL_HANDLE) {
				meshDescriptorSets.push_back(descriptorSet);
			}
		}
		mesh.descriptorSetsByLayout.clear();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!meshDescriptorSets.empty() && device != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(meshDescriptorSets.size()), meshDescriptorSets.data());
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& outerEntry : pipelineCache) {
		auto& map = outerEntry.second;
		for (auto& innerEntry : map) {
			VulkanPipeline& pipe = innerEntry.second;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (device) {
				vkDestroyPipeline(device, pipe.pipeline, nullptr);
				vkDestroyPipelineLayout(device, pipe.layout, nullptr);
				// Start from 1 to skip the global descriptor set layout (index 0) which is shared and destroyed in Cleanup
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t i = 1; i < pipe.setLayouts.size(); ++i) {
					vkDestroyDescriptorSetLayout(device, pipe.setLayouts[i], nullptr);
				}
			}
		}
	}
	pipelineCache.clear();
}

// VulkanContext::Impl::DestroyWorkerPools is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyWorkerPools() {
	if (!device) return;
	for (size_t frameIndex = 0; frameIndex < workerCommandPools.size(); ++frameIndex) {
		auto& framePools = workerCommandPools[frameIndex];
		std::vector<std::vector<VkCommandBuffer>> emptyBuffers;
		auto& frameBuffers = (frameIndex < workerCommandBuffers.size()) ? workerCommandBuffers[frameIndex] : emptyBuffers;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t poolIndex = 0; poolIndex < framePools.size(); ++poolIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!framePools[poolIndex]) continue;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (poolIndex < frameBuffers.size() && !frameBuffers[poolIndex].empty()) {
				vkFreeCommandBuffers(device, framePools[poolIndex], static_cast<uint32_t>(frameBuffers[poolIndex].size()), frameBuffers[poolIndex].data());
				frameBuffers[poolIndex].clear();
			}
			vkDestroyCommandPool(device, framePools[poolIndex], nullptr);
		}
	}
	workerCommandPools.clear();
	workerCommandBuffers.clear();
}

// VulkanContext::Impl::EnsureWorkerPools is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsureWorkerPools(uint32_t frameIndex, size_t workerCount) {
	if (workerCommandPools.size() <= frameIndex) {
		workerCommandPools.resize(frameIndex + 1);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (workerCommandBuffers.size() <= frameIndex) {
		workerCommandBuffers.resize(frameIndex + 1);
	}

	auto& framePools = workerCommandPools[frameIndex];
	auto& frameBuffers = workerCommandBuffers[frameIndex];
	QueueFamilyIndices indices = FindQueueFamilies(physicalDevice);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (framePools.size() < workerCount) {
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = indices.graphicsFamily.value();

		VkCommandPool pool = VK_NULL_HANDLE;
		vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
		framePools.push_back(pool);
		frameBuffers.emplace_back();
	}
}

// VulkanContext::Impl::AcquirePrimaryCommandBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkCommandBuffer VulkanContext::Impl::AcquirePrimaryCommandBuffer(uint32_t frameIndex) {
	if (frameIndex >= commandBuffers.size()) return VK_NULL_HANDLE;
	return commandBuffers[frameIndex];
}

// VulkanContext::Impl::AcquireWorkerCommandBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkCommandBuffer VulkanContext::Impl::AcquireWorkerCommandBuffer(uint32_t frameIndex, size_t workerIndex) {
	auto& framePools = workerCommandPools[frameIndex];
	auto& frameBuffers = workerCommandBuffers[frameIndex];
	
	// Ensure tracking structures are sized correctly
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (workerBufferUsage.size() <= frameIndex) workerBufferUsage.resize(frameIndex + 1);
	if (workerBufferUsage[frameIndex].size() <= workerIndex) workerBufferUsage[frameIndex].resize(workerIndex + 1, 0);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameBuffers.size() <= workerIndex) frameBuffers.resize(workerIndex + 1);

	size_t& usageIndex = workerBufferUsage[frameIndex][workerIndex];
	std::vector<VkCommandBuffer>& buffers = frameBuffers[workerIndex];

	// reuse existing buffer if available at this usage index
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (usageIndex < buffers.size()) {
		return buffers[usageIndex++];
	}

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = framePools[workerIndex];
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer secondary = VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkAllocateCommandBuffers(device, &allocInfo, &secondary) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	buffers.push_back(secondary);
	usageIndex++;
	return secondary;
}

// VulkanContext::Impl::MapTextureFormat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkFormat VulkanContext::Impl::MapTextureFormat(uint32_t vkFormat) {
	return static_cast<VkFormat>(vkFormat);
}

// VulkanContext::Impl::EnsureTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsureTexture(TerraEngine::UUID textureId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	if (textureCache.contains(textureId)) {
		const VulkanImage& cachedTexture = textureCache[textureId];
		const TerraEngine::Resource::TextureMipResidency desiredMipResidency =
			ResourceManager::Instance().ResolveTextureMipResidencyForTexture(textureId, cachedTexture.sourceMipCount);
		if (desiredMipResidency.residentMipCount != cachedTexture.residentMipCount
			|| desiredMipResidency.sourceMipOffset != cachedTexture.residentMipOffset)
		{
			// Texture streaming policy and view distance can change while a
			// texture is cached. Recreate the image when the desired mip tail no
			// longer matches the cached VkImage dimensions, then fall through to
			// the normal upload path below.
			InvalidateTexture(textureId, false);
		}
		else
		{
		ResourceManager::Instance().MarkResourceUploadedToGpu(
			textureId,
			AssetType::Texture,
			static_cast<std::uint64_t>(currentFrame),
			static_cast<std::uint64_t>(cachedTexture.size),
			"vulkan texture cache hit",
			cachedTexture.residentMipCount,
			cachedTexture.sourceMipCount,
			cachedTexture.residentMipOffset);
		if (!textureIndexMap.contains(textureId)) {
			uint32_t index = AllocateTextureIndex();
			textureIndexMap[textureId] = index;
			UpdateGlobalTexture(index, cachedTexture);
		}
		return;
		}
	}

	Texture* cpuTexture = ResourceManager::Instance().GetTexture(textureId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!cpuTexture) return;
	
	// Ensure we have transcoded UASTC data
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cpuTexture->transcodedData.empty())
	{
		TERRAENGINE_LOG(LogLevel::Error, "Texture " << ToHexString(textureId) << " has no transcoded payload data.");
		return;
	}

	const std::uint32_t sourceMipCount = std::max(1u, static_cast<std::uint32_t>(cpuTexture->descriptor.mipCount));
	const TerraEngine::Resource::TextureMipResidency mipResidency =
		ResourceManager::Instance().ResolveTextureMipResidencyForTexture(textureId, sourceMipCount);
	const std::uint32_t residentMipOffset = std::min(mipResidency.sourceMipOffset, sourceMipCount - 1u);
	const std::uint32_t residentMipCount = std::max(1u, std::min(mipResidency.residentMipCount, sourceMipCount - residentMipOffset));
	VkDeviceSize residentPayloadSize = 0u;
	for (std::uint32_t mipIndex = 0u; mipIndex < residentMipCount; ++mipIndex)
	{
		const std::uint32_t sourceMip = residentMipOffset + mipIndex;
		if (sourceMip >= cpuTexture->levelTable.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Texture " << ToHexString(textureId) << " has missing mip metadata.");
			return;
		}
		residentPayloadSize += static_cast<VkDeviceSize>(cpuTexture->levelTable[sourceMip].size);
	}
	if (residentPayloadSize == 0u)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Texture " << ToHexString(textureId) << " selected no resident mip payload.");
		return;
	}

	std::vector<std::uint8_t> residentUploadData(static_cast<std::size_t>(residentPayloadSize));
	VkDeviceSize uploadOffset = 0u;
	std::vector<VkBufferImageCopy> regions;
	regions.reserve(residentMipCount);
	for (std::uint32_t mipIndex = 0u; mipIndex < residentMipCount; ++mipIndex)
	{
		const std::uint32_t sourceMip = residentMipOffset + mipIndex;
		const auto& level = cpuTexture->levelTable[sourceMip];
		if (level.offset + level.size > cpuTexture->transcodedData.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Texture " << ToHexString(textureId) << " mip payload overruns transcoded data.");
			return;
		}
		std::memcpy(
			residentUploadData.data() + static_cast<std::size_t>(uploadOffset),
			cpuTexture->transcodedData.data() + static_cast<std::size_t>(level.offset),
			static_cast<std::size_t>(level.size));

		VkBufferImageCopy region{};
		region.bufferOffset = uploadOffset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mipIndex;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = std::max(1u, static_cast<uint32_t>(cpuTexture->descriptor.width) >> sourceMip);
		region.imageExtent.height = std::max(1u, static_cast<uint32_t>(cpuTexture->descriptor.height) >> sourceMip);
		region.imageExtent.depth = 1;
		regions.push_back(region);
		uploadOffset += static_cast<VkDeviceSize>(level.size);
	}

	VulkanBuffer stagingBuffer = CreateBuffer(residentPayloadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Texture Mip Staging Buffer");
	WriteMappedBuffer(allocator, stagingBuffer, residentUploadData.data(), residentPayloadSize);

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = std::max(1u, static_cast<uint32_t>(cpuTexture->descriptor.width) >> residentMipOffset);
	imageInfo.extent.height = std::max(1u, static_cast<uint32_t>(cpuTexture->descriptor.height) >> residentMipOffset);
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = residentMipCount;
	imageInfo.arrayLayers = 1;
	imageInfo.format = MapTextureFormat(cpuTexture->descriptor.vkFormat);
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VulkanImage vImage;
	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &vImage.image, &vImage.allocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create image for texture");
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		return;
	}

	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
		vmaDestroyImage(allocator, vImage.image, vImage.allocation);
		return;
	}
	VkImageMemoryBarrier uploadBarrier{};
	uploadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	uploadBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	uploadBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	uploadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	uploadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	uploadBarrier.image = vImage.image;
	uploadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	uploadBarrier.subresourceRange.baseMipLevel = 0u;
	uploadBarrier.subresourceRange.levelCount = residentMipCount;
	uploadBarrier.subresourceRange.baseArrayLayer = 0u;
	uploadBarrier.subresourceRange.layerCount = 1u;
	uploadBarrier.srcAccessMask = 0u;
	uploadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0u,
		0u, nullptr,
		0u, nullptr,
		1u, &uploadBarrier);

	vkCmdCopyBufferToImage(commandBuffer, stagingBuffer.buffer, vImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());

	VkImageMemoryBarrier shaderReadBarrier = uploadBarrier;
	shaderReadBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	shaderReadBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	shaderReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	shaderReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT,
		0u,
		0u, nullptr,
		0u, nullptr,
		1u, &shaderReadBarrier);

	if (!EndSingleTimeCommands(commandBuffer, "texture upload"))
	{
		// If the queue did not prove completion, leave the upload allocations
		// alone instead of destroying resources a possibly-pending command
		// buffer may still reference. The texture is not cached or published.
		return;
	}

	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	vImage.view = CreateImageView(vImage.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT, residentMipCount);
	vImage.size = residentPayloadSize;
	vImage.sourceMipCount = sourceMipCount;
	vImage.residentMipCount = residentMipCount;
	vImage.residentMipOffset = residentMipOffset;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vImage.view == VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, vImage.image, vImage.allocation);
		return;
	}
	
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = static_cast<float>(residentMipCount - 1u);
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateSampler(device, &samplerInfo, nullptr, &vImage.sampler) != VK_SUCCESS) {
		vkDestroyImageView(device, vImage.view, nullptr);
		vmaDestroyImage(allocator, vImage.image, vImage.allocation);
		return;
	}

	textureCache[textureId] = vImage;
	ResourceManager::Instance().MarkResourceUploadedToGpu(
		textureId,
		AssetType::Texture,
		static_cast<std::uint64_t>(currentFrame),
		static_cast<std::uint64_t>(residentPayloadSize),
		mipResidency.partial ? "vulkan texture partial mip upload" : "vulkan texture upload",
		residentMipCount,
		sourceMipCount,
		residentMipOffset);

	// Assign bindless index
	uint32_t index = AllocateTextureIndex();
	textureIndexMap[textureId] = index;
	UpdateGlobalTexture(index, vImage);
}

// VulkanContext::Impl::EnsureMaterialDescriptorSets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<VkDescriptorSet> VulkanContext::Impl::EnsureMaterialDescriptorSets(TerraEngine::UUID shaderCollectionId, TerraEngine::UUID materialId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	// No longer returns descriptor sets, but acts as an upload mechanism for material data.
	// The return value is kept for now to minimize API breakage, but it will be empty besides the global set.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (materialIndexMap.contains(materialId)) {
		return { globalDescriptorSets[currentFrame] };
	}

	Material* material = ResourceManager::Instance().GetMaterial(materialId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!material) return { globalDescriptorSets[currentFrame] };
	
	MaterialData data = CreateFallbackMaterialData();
	data.BaseColorFactor = glm::vec4(
		material->header.baseColorFactor[0],
		material->header.baseColorFactor[1],
		material->header.baseColorFactor[2],
		material->header.baseColorFactor[3]
	);
	data.EmissiveFactor = glm::vec4(
		material->header.emissiveFactor[0],
		material->header.emissiveFactor[1],
		material->header.emissiveFactor[2],
		1.0f
	);
	data.MetallicFactor = material->header.metallicFactor;
	data.RoughnessFactor = material->header.roughnessFactor;
	data.NormalScale = material->header.normalScale;
	data.OcclusionStrength = material->header.occlusionStrength;
	data.AlphaCutoff = material->header.alphaCutoff;
	data.AlphaMode = material->header.alphaMode;
	data.ShadingModel = material->header.shadingModel;
	data.Flags = material->header.flags;
	// Shadow behavior is packed beside the ordinary material parameters so
	// both the default shading pass and shadow-map pass can make the same
	// opacity decision without guessing from alpha mode alone.
	data.ShadowBehavior = static_cast<std::uint32_t>(material->shadowBehavior);
	data.UVTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); // Default: Scale 1, Offset 0

	// Initialize texture slots to renderer-owned fallbacks:
	// 0 = white base color, 1 = flat normal, 2 = non-metal rough material mask.
	data.BaseColorTextureIndex = 0u;
	data.NormalTextureIndex = 1u;
	data.MaskTextureIndex = 2u;
	data.OcclusionTextureIndex = 2u;
	data.EmissiveTextureIndex = 0u;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t i = 0; i < material->header.textureCount; ++i) {
		const auto& binding = material->textures[i];
		if (binding.textureUuid == TerraEngine::EMPTY_UUID) continue;

		EnsureTexture(binding.textureUuid);
		uint32_t gpuIndex = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (textureIndexMap.contains(binding.textureUuid)) {
			gpuIndex = textureIndexMap[binding.textureUuid];
		}

		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (binding.semantic) {
			case TextureSemantic::Base:         
				data.BaseColorTextureIndex = gpuIndex; 
				// Use Base's UV transform as the material-wide transform
				data.UVTransform = glm::vec4(binding.uvScale[0], binding.uvScale[1], binding.uvOffset[0], binding.uvOffset[1]);
				break;
			case TextureSemantic::Normal:            data.NormalTextureIndex = gpuIndex; break;
			case TextureSemantic::MetallicRoughness: data.MaskTextureIndex = gpuIndex; break;
			case TextureSemantic::Occlusion:         data.OcclusionTextureIndex = gpuIndex; break;
			case TextureSemantic::Emissive:          data.EmissiveTextureIndex = gpuIndex; break;
			default: break;
		}
	}
	
	uint32_t materialIndex = AllocateMaterialIndex();
	materialIndexMap[materialId] = materialIndex;
	
	VkDeviceSize bufferSize = sizeof(MaterialData);
	VulkanBuffer stagingBuffer = CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Material Data Staging Buffer");
	WriteMappedBuffer(allocator, stagingBuffer, &data, bufferSize);
	
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	if (commandBuffer != VK_NULL_HANDLE)
	{
		VkBufferCopy copyRegion{};
		copyRegion.srcOffset = 0;
		copyRegion.dstOffset = materialIndex * sizeof(MaterialData);
		copyRegion.size = bufferSize;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			vkCmdCopyBuffer(commandBuffer, stagingBuffer.buffer, globalMaterialSSBOs[i].buffer, 1, &copyRegion);
		}
		EndSingleTimeCommands(commandBuffer, "material data upload");
	}
	
	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	return { globalDescriptorSets[currentFrame] };
}

// VulkanContext::Impl::InvalidateMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateMaterial(TerraEngine::UUID materialId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	// Material edits can change alpha clipping or shadow behavior, so cached
	// shadow results are conservatively invalidated before the material is
	// rebound.
	shadowCacheEntries.clear();
	auto it = materialIndexMap.find(materialId);
	if (it != materialIndexMap.end()) {
		if (it->second != kFallbackMaterialIndex) {
			freeMaterialIndices.push_back(it->second);
		}
		materialIndexMap.erase(it);
	}
}

// VulkanContext::Impl::InvalidateMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateMesh(TerraEngine::UUID meshId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	// Mesh edits change caster geometry. Clear every cache entry rather than
	// trying to prove which light was affected.
	shadowCacheEntries.clear();
	auto it = meshCache.find(meshId);
	if (it != meshCache.end()) {
		// Pending command buffers can still reference the mesh-local descriptor
		// sets and bindless buffer slot while a project reload is invalidating
		// resources. Wait once before mutating or freeing any descriptor-backed
		// state owned by this mesh.
		WaitForDescriptorMutationIdle();
		if (it->second.bindlessIndex != INVALID_BINDLESS_BUFFER_INDEX) {
			// The descriptor still contains the old buffer handle until the slot
			// is reused, but no live draw should index this mesh after the cache
			// entry is erased. Writing VK_NULL_HANDLE would require the Vulkan
			// nullDescriptor feature, so recycle only the CPU-side slot owner.
			freeBufferIndices.push_back(it->second.bindlessIndex);
			it->second.bindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!it->second.descriptorSetsByLayout.empty() && descriptorPool != VK_NULL_HANDLE) {
			std::vector<VkDescriptorSet> descriptorSets;
			descriptorSets.reserve(it->second.descriptorSetsByLayout.size());
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& [layout, set] : it->second.descriptorSetsByLayout) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (set != VK_NULL_HANDLE) {
					descriptorSets.push_back(set);
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!descriptorSets.empty()) {
				vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data());
			}
			it->second.descriptorSetsByLayout.clear();
		}
		vmaDestroyBuffer(allocator, it->second.meshShaderVertexBuffer.buffer, it->second.meshShaderVertexBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshletVertexRemapBuffer.buffer, it->second.meshletVertexRemapBuffer.allocation);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (it->second.meshSkinningBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, it->second.meshSkinningBuffer.buffer, it->second.meshSkinningBuffer.allocation);
		}
		if (it->second.meshMorphDeltaBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, it->second.meshMorphDeltaBuffer.buffer, it->second.meshMorphDeltaBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, it->second.meshDecodeInfoBuffer.buffer, it->second.meshDecodeInfoBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.indexBuffer.buffer, it->second.indexBuffer.allocation);
		vmaDestroyBuffer(allocator, it->second.meshletBuffer.buffer, it->second.meshletBuffer.allocation);
		if (it->second.meshletSectionBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, it->second.meshletSectionBuffer.buffer, it->second.meshletSectionBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, it->second.meshletBoundsBuffer.buffer, it->second.meshletBoundsBuffer.allocation);
		meshCache.erase(it);
	}
}

// VulkanContext::Impl::InvalidateTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateTexture(TerraEngine::UUID textureId, bool reloadAfterInvalidate) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	// Texture edits may affect alpha-tested caster silhouettes, so shadow cache
	// reuse falls back to refresh after the texture changes.
	shadowCacheEntries.clear();
	bool textureWasKnown = false;
	auto it = textureCache.find(textureId);
	if (it != textureCache.end()) {
		// Don't invalidate default texture
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (it->second.image == defaultTexture.image) return;

		// Texture reloads update bindless descriptors and destroy old image
		// views. Wait for submitted command buffers before those descriptors can
		// point at replacement resources.
		WaitForDescriptorMutationIdle();
		vkDestroySampler(device, it->second.sampler, nullptr);
		vkDestroyImageView(device, it->second.view, nullptr);
		vmaDestroyImage(allocator, it->second.image, it->second.allocation);
		textureCache.erase(it);
		textureWasKnown = true;
	}

	if (auto indexIt = textureIndexMap.find(textureId); indexIt != textureIndexMap.end())
	{
		const uint32_t releasedIndex = indexIt->second;
		UpdateGlobalTexture(releasedIndex, defaultTexture);
		textureIndexMap.erase(indexIt);
		if (releasedIndex > 2u)
		{
			freeTextureIndices.push_back(releasedIndex);
		}
		textureWasKnown = true;
	}

	if (!reloadAfterInvalidate && textureWasKnown && !materialIndexMap.empty())
	{
		// Materials store bindless texture indices in the material SSBO. When
		// residency evicts a texture, any cached material that referenced that
		// index must be rebuilt the next time it is drawn so EnsureTexture can
		// reload visible texture dependencies.
		for (const auto& [materialId, materialIndex] : materialIndexMap)
		{
			(void)materialId;
			if (materialIndex != kFallbackMaterialIndex)
			{
				freeMaterialIndices.push_back(materialIndex);
			}
		}
		materialIndexMap.clear();
	}

	if (reloadAfterInvalidate)
	{
		// Reload paths wait for CPU transcode first, so they can re-upload here.
		// Residency retirement passes false so an evicted texture stays unloaded
		// until visibility streaming demands it again.
		EnsureTexture(textureId);
	}
}

// VulkanContext::Impl::InvalidateResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::InvalidateResource(TerraEngine::UUID id, bool reloadTexturesAfterInvalidate) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	InvalidateMesh(id);
	InvalidateTexture(id, reloadTexturesAfterInvalidate);
	InvalidateMaterial(id);
}

// VulkanContext::Impl::EnsureMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsureMesh(TerraEngine::UUID meshId) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	auto existingMeshIt = meshCache.find(meshId);
	if (existingMeshIt != meshCache.end())
	{
		const VulkanMesh& mesh = existingMeshIt->second;
		const VkDeviceSize totalGpuBytes = TotalMeshGpuBytes(mesh);
		const Mesh* cpuMesh = ResourceManager::Instance().GetMesh(meshId);
		std::string uploadDetail = "vulkan mesh cache hit";
		if (cpuMesh != nullptr && cpuMesh->hasVirtualGeometry && cpuMesh->virtualGeometryPageStreamingReady)
		{
			uploadDetail = cpuMesh->virtualGeometryOwnerFallbackReason.empty()
				? "vulkan mesh cache hit with page-local virtual geometry owner placeholders"
				: "vulkan mesh cache hit with owning-mesh virtual geometry fallback: " + cpuMesh->virtualGeometryOwnerFallbackReason;
		}
		ResourceManager::Instance().MarkResourceUploadedToGpu(
			meshId,
			AssetType::Mesh,
			static_cast<std::uint64_t>(currentFrame),
			static_cast<std::uint64_t>(totalGpuBytes),
			uploadDetail);
		return;
	}
	
	Mesh* cpuMesh = ResourceManager::Instance().GetMesh(meshId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!cpuMesh) return;

	const VkDeviceSize packedVertexSize = static_cast<VkDeviceSize>(cpuMesh->meshletVertexData.size());
	const VkDeviceSize vertexRemapSize = static_cast<VkDeviceSize>(cpuMesh->meshletVertexRemapData.size());
	const VkDeviceSize skinningDataSize = static_cast<VkDeviceSize>(cpuMesh->meshletSkinningData.size());
	const VkDeviceSize primitiveIndexSize = static_cast<VkDeviceSize>(cpuMesh->meshletPrimitiveIndexData.size());
	// ByteAddressBuffer.Load reads one complete 32-bit word even when the
	// logical payload stores 16-bit remaps or 8-bit primitive indices. Pad the
	// Vulkan allocation and descriptor range to that access granularity while
	// retaining the original logical counts in MeshDecodeInfo.
	const auto AlignByteAddressRange = [](VkDeviceSize byteCount)
	{
		return (byteCount + 3u) & ~VkDeviceSize{3u};
	};
	const VkDeviceSize vertexRemapUploadSize =
		AlignByteAddressRange(vertexRemapSize);
	const VkDeviceSize primitiveIndexUploadSize =
		AlignByteAddressRange(primitiveIndexSize);
	const VkDeviceSize meshletTableSize = static_cast<VkDeviceSize>(cpuMesh->meshletRecords.size() * sizeof(MeshletRecord));
	const VkDeviceSize meshletSectionSize = static_cast<VkDeviceSize>(cpuMesh->meshletSections.size() * sizeof(MeshletSection));
	const VkDeviceSize meshletBoundsSize = static_cast<VkDeviceSize>(cpuMesh->meshletBounds.size() * sizeof(MeshletBounds));
	const bool hasBoundsPayload = meshletBoundsSize > 0;
	const VkDeviceSize boundsUploadSize = hasBoundsPayload ? meshletBoundsSize : static_cast<VkDeviceSize>(sizeof(MeshletBounds));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (packedVertexSize == 0
		|| vertexRemapSize == 0
		|| primitiveIndexSize == 0
		|| meshletTableSize == 0
		|| meshletSectionSize == 0
		|| cpuMesh->runtimeLayout.meshletCount == 0)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload is missing packed vertex/remap/primitive/table data for UUID " << ToHexString(meshId));
		return;
	}

	const uint32_t packedStride = cpuMesh->runtimeLayout.layout.strideBytes;
	const uint32_t meshletVertexCount = cpuMesh->runtimeLayout.meshletVertexCount;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (packedStride != kMeshletPackedVertexStrideBytes || meshletVertexCount == 0) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload has invalid packed vertex stride/count for UUID " << ToHexString(meshId));
		return;
	}

	const VkDeviceSize expectedVertexBytes = static_cast<VkDeviceSize>(packedStride) * static_cast<VkDeviceSize>(meshletVertexCount);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (expectedVertexBytes != packedVertexSize) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload packed vertex range is truncated for UUID " << ToHexString(meshId));
		return;
	}

	// struct MeshDecodeInfo groups related state and operations so callers have one documented contract.
	struct MeshDecodeInfo
	{
		float positionOffset[4];
		float positionScale[4];
		float uvOffsetScale[4];
		std::uint32_t counts[4];
	};
	static_assert(sizeof(MeshDecodeInfo) == 64, "Mesh decode info buffer must keep a stable 64-byte layout.");

	MeshDecodeInfo decodeInfo{};
	decodeInfo.positionOffset[0] = cpuMesh->runtimeLayout.quantization.positionOffset[0];
	decodeInfo.positionOffset[1] = cpuMesh->runtimeLayout.quantization.positionOffset[1];
	decodeInfo.positionOffset[2] = cpuMesh->runtimeLayout.quantization.positionOffset[2];
	decodeInfo.positionScale[0] = cpuMesh->runtimeLayout.quantization.positionScale[0];
	decodeInfo.positionScale[1] = cpuMesh->runtimeLayout.quantization.positionScale[1];
	decodeInfo.positionScale[2] = cpuMesh->runtimeLayout.quantization.positionScale[2];
	decodeInfo.uvOffsetScale[0] = cpuMesh->runtimeLayout.quantization.uvOffset[0];
	decodeInfo.uvOffsetScale[1] = cpuMesh->runtimeLayout.quantization.uvOffset[1];
	decodeInfo.uvOffsetScale[2] = cpuMesh->runtimeLayout.quantization.uvScale[0];
	decodeInfo.uvOffsetScale[3] = cpuMesh->runtimeLayout.quantization.uvScale[1];
	decodeInfo.counts[0] = cpuMesh->runtimeLayout.meshletVertexCount;
	decodeInfo.counts[1] = cpuMesh->runtimeLayout.meshletVertexRemapCount;
	decodeInfo.counts[2] = cpuMesh->runtimeLayout.meshletPrimitiveIndexCount;
	// counts.w: bits [0:30] = meshletCount, bit 31 = remapEncoding (0=u32, 1=u16)
	decodeInfo.counts[3] = (cpuMesh->runtimeLayout.meshletCount & 0x7FFFFFFFu) | (cpuMesh->runtimeLayout.meshletVertexRemapEncoding << 31u);

	std::vector<TemfDeformablePageMorphVertex> morphDeltaRecords;
	const bool hasValidatedMorphDeltas = BuildMeshMorphDeltaRecords(*cpuMesh, morphDeltaRecords);
	const VkDeviceSize morphDeltaSize = static_cast<VkDeviceSize>(
		morphDeltaRecords.size() * sizeof(TemfDeformablePageMorphVertex));
	if(morphDeltaRecords.empty() || morphDeltaSize == 0U) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh morph delta preparation produced no safe fallback record for UUID " << ToHexString(meshId));
		return;
	}
	if(!hasValidatedMorphDeltas) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh morph runtime data was inconsistent; uploading a zero morph delta fallback for UUID " << ToHexString(meshId));
	}

	VulkanMesh vMesh{};
	vMesh.indexCount = cpuMesh->runtimeLayout.meshletPrimitiveIndexCount;
	vMesh.indexType = VK_INDEX_TYPE_UINT16;
	vMesh.meshletCount = cpuMesh->runtimeLayout.meshletCount;
	auto createPlaceholderStorageBuffer = [&](VkDeviceSize byteSize, const char* name) -> VulkanBuffer {
		// Reflected storage-buffer descriptors may be statically used even when
		// this mesh has no corresponding payload. Give those bindings a small,
		// zero-filled owner instead of leaving an invalid descriptor unwritten.
		const VkDeviceSize safeSize = std::max<VkDeviceSize>(byteSize, 4u);
		VulkanBuffer buffer = CreateBuffer(safeSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, name);
		std::vector<std::uint8_t> zeros(static_cast<std::size_t>(safeSize), 0u);
		WriteMappedBuffer(allocator, buffer, zeros.data(), safeSize);
		return buffer;
	};

	const bool usePageLocalVirtualGeometry =
		cpuMesh->hasVirtualGeometry
		&& cpuMesh->virtualGeometryPageStreamingReady
		&& cpuMesh->virtualGeometryOwnerFallbackReason.empty()
		// Deformable draws currently consume owner skinning and morph streams
		// through the graphics bridge. Retain those full streams until the
		// page-local path can provide equivalent deformable payloads.
		&& !cpuMesh->hasDeformablePayload;
	if (usePageLocalVirtualGeometry)
	{
		// Page-streamed virtual geometry uses the per-frame page-local payload
		// buffer for real vertices, remaps, primitive indices, records, bounds,
		// and decode constants. These tiny owner buffers keep the existing mesh
		// descriptor set valid for legacy shader layouts without uploading the
		// full high-detail owning mesh as a hidden residency cost.
		vMesh.meshShaderVertexBuffer = createPlaceholderStorageBuffer(kMeshletPackedVertexStrideBytes, "Virtual Geometry Owner Placeholder Vertex Buffer");
		vMesh.meshletVertexRemapBuffer = createPlaceholderStorageBuffer(sizeof(std::uint32_t), "Virtual Geometry Owner Placeholder Remap Buffer");
		vMesh.meshSkinningBuffer = createPlaceholderStorageBuffer(kTemfSkinningStrideBytes, "Virtual Geometry Owner Placeholder Skinning Buffer");
		vMesh.meshDecodeInfoBuffer = createPlaceholderStorageBuffer(sizeof(MeshDecodeInfo), "Virtual Geometry Owner Placeholder Decode Buffer");
		vMesh.indexBuffer = createPlaceholderStorageBuffer(sizeof(std::uint32_t), "Virtual Geometry Owner Placeholder Primitive Buffer");
		vMesh.meshletBuffer = createPlaceholderStorageBuffer(sizeof(MeshletRecord), "Virtual Geometry Owner Placeholder Meshlet Buffer");
		vMesh.meshletBoundsBuffer = createPlaceholderStorageBuffer(sizeof(MeshletBounds), "Virtual Geometry Owner Placeholder Bounds Buffer");
		vMesh.meshMorphDeltaBuffer = createPlaceholderStorageBuffer(sizeof(TemfDeformablePageMorphVertex), "Virtual Geometry Owner Placeholder Morph Delta Buffer");
		vMesh.hasMeshletBounds = true;
	}
	else
	{
		VulkanBuffer stagingPackedVertex = CreateBuffer(packedVertexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingPackedVertex, cpuMesh->meshletVertexData.data(), packedVertexSize);

		VulkanBuffer stagingVertexRemap = CreateBuffer(vertexRemapUploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		// Zero-copy: upload raw on-disk bytes (u16 or u32) without CPU expansion.
		ClearMappedBuffer(allocator, stagingVertexRemap);
		WriteMappedBuffer(allocator, stagingVertexRemap, cpuMesh->meshletVertexRemapData.data(), vertexRemapSize);

		VulkanBuffer stagingSkinning = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0 };
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skinningDataSize > 0)
		{
			stagingSkinning = CreateBuffer(skinningDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
			WriteMappedBuffer(allocator, stagingSkinning, cpuMesh->meshletSkinningData.data(), skinningDataSize);
		}

		VulkanBuffer stagingMorphDeltas = CreateBuffer(morphDeltaSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingMorphDeltas, morphDeltaRecords.data(), morphDeltaSize);

		VulkanBuffer stagingIndex = CreateBuffer(primitiveIndexUploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		ClearMappedBuffer(allocator, stagingIndex);
		WriteMappedBuffer(allocator, stagingIndex, cpuMesh->meshletPrimitiveIndexData.data(), primitiveIndexSize);

		VulkanBuffer stagingMeshletTable = CreateBuffer(meshletTableSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingMeshletTable, cpuMesh->meshletRecords.data(), meshletTableSize);

		VulkanBuffer stagingMeshletSection = CreateBuffer(meshletSectionSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		// Virtual geometry stores one meshlet section range per emitted LOD in the MVP.
		// Upload it with the other resident streams now; later stories bind it when
		// shaders start selecting LOD ranges directly.
		WriteMappedBuffer(allocator, stagingMeshletSection, cpuMesh->meshletSections.data(), meshletSectionSize);

		VulkanBuffer stagingMeshletBounds = CreateBuffer(boundsUploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hasBoundsPayload)
		{
			WriteMappedBuffer(allocator, stagingMeshletBounds, cpuMesh->meshletBounds.data(), meshletBoundsSize);
		}
		else
		{
			MeshletBounds fallbackBounds{};
			fallbackBounds.coneCutoff = 2.0f;
			WriteMappedBuffer(allocator, stagingMeshletBounds, &fallbackBounds, sizeof(fallbackBounds));
		}

		VulkanBuffer stagingDecodeInfo = CreateBuffer(sizeof(MeshDecodeInfo), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingDecodeInfo, &decodeInfo, sizeof(MeshDecodeInfo));

		// No CPU repack path: mesh shader reads the packed payload directly from storage buffers.
		vMesh.meshShaderVertexBuffer = CreateBuffer(packedVertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshletVertexRemapBuffer = CreateBuffer(vertexRemapUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		if (skinningDataSize > 0)
		{
			vMesh.meshSkinningBuffer = CreateBuffer(skinningDataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		}
		else
		{
			// Default mesh shaders statically read binding 6. A zero joint and
			// zero weight record makes that binding valid while preserving the
			// shader's identity-deformation behavior for non-skinned meshes.
			vMesh.meshSkinningBuffer = createPlaceholderStorageBuffer(kTemfSkinningStrideBytes, "Static Mesh Skinning Fallback Buffer");
		}
		vMesh.meshMorphDeltaBuffer = CreateBuffer(morphDeltaSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshDecodeInfoBuffer = CreateBuffer(sizeof(MeshDecodeInfo), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.indexBuffer = CreateBuffer(primitiveIndexUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshletBuffer = CreateBuffer(meshletTableSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshletSectionBuffer = CreateBuffer(meshletSectionSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.meshletBoundsBuffer = CreateBuffer(boundsUploadSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		vMesh.hasMeshletBounds = hasBoundsPayload;

		CopyBuffer(stagingPackedVertex.buffer, vMesh.meshShaderVertexBuffer.buffer, packedVertexSize);
		CopyBuffer(stagingVertexRemap.buffer, vMesh.meshletVertexRemapBuffer.buffer, vertexRemapUploadSize);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skinningDataSize > 0)
		{
			CopyBuffer(stagingSkinning.buffer, vMesh.meshSkinningBuffer.buffer, skinningDataSize);
		}
		CopyBuffer(stagingMorphDeltas.buffer, vMesh.meshMorphDeltaBuffer.buffer, morphDeltaSize);
		CopyBuffer(stagingDecodeInfo.buffer, vMesh.meshDecodeInfoBuffer.buffer, sizeof(MeshDecodeInfo));
		CopyBuffer(stagingIndex.buffer, vMesh.indexBuffer.buffer, primitiveIndexUploadSize);
		CopyBuffer(stagingMeshletTable.buffer, vMesh.meshletBuffer.buffer, meshletTableSize);
		CopyBuffer(stagingMeshletSection.buffer, vMesh.meshletSectionBuffer.buffer, meshletSectionSize);
		CopyBuffer(stagingMeshletBounds.buffer, vMesh.meshletBoundsBuffer.buffer, boundsUploadSize);

		vmaDestroyBuffer(allocator, stagingPackedVertex.buffer, stagingPackedVertex.allocation);
		vmaDestroyBuffer(allocator, stagingVertexRemap.buffer, stagingVertexRemap.allocation);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skinningDataSize > 0)
		{
			vmaDestroyBuffer(allocator, stagingSkinning.buffer, stagingSkinning.allocation);
		}
		vmaDestroyBuffer(allocator, stagingMorphDeltas.buffer, stagingMorphDeltas.allocation);
		vmaDestroyBuffer(allocator, stagingDecodeInfo.buffer, stagingDecodeInfo.allocation);
		vmaDestroyBuffer(allocator, stagingIndex.buffer, stagingIndex.allocation);
		vmaDestroyBuffer(allocator, stagingMeshletTable.buffer, stagingMeshletTable.allocation);
		vmaDestroyBuffer(allocator, stagingMeshletSection.buffer, stagingMeshletSection.allocation);
		vmaDestroyBuffer(allocator, stagingMeshletBounds.buffer, stagingMeshletBounds.allocation);
	}

	vMesh.bindlessIndex = AllocateBufferIndex();
	UpdateGlobalBuffer(vMesh.bindlessIndex, vMesh.meshDecodeInfoBuffer.buffer, 0, vMesh.meshDecodeInfoBuffer.size);

	meshCache[meshId] = vMesh;
	const VkDeviceSize totalGpuBytes = TotalMeshGpuBytes(meshCache[meshId]);
	const bool hasVirtualGeometryPageMetadata = cpuMesh->hasVirtualGeometry && !cpuMesh->virtualGeometryPages.empty();
	std::string uploadDetail = "vulkan mesh buffer upload";
	if (usePageLocalVirtualGeometry)
	{
		uploadDetail = "vulkan page-local virtual geometry placeholder owner descriptors; owner mesh buffers are skipped for page-local virtual geometry";
	}
	else if (cpuMesh->hasDeformablePayload
		&& cpuMesh->virtualGeometryPageStreamingReady
		&& cpuMesh->virtualGeometryOwnerFallbackReason.empty())
	{
		uploadDetail = "vulkan deformable virtual geometry owner upload retained for graphics deformation";
	}
	else if (hasVirtualGeometryPageMetadata)
	{
		// The current shader path still binds owner mesh buffers for decoding.
		// Label that allocation as an owning-mesh fallback so exact residency
		// tools do not mistake it for a page-local physical payload.
		uploadDetail = cpuMesh->virtualGeometryPageStreamingReady
			? "vulkan owning-mesh virtual geometry fallback upload: " + cpuMesh->virtualGeometryOwnerFallbackReason
			: "vulkan mesh buffer upload with virtual geometry metadata that is not page-streaming ready";
	}
	ResourceManager::Instance().MarkResourceUploadedToGpu(
		meshId,
		AssetType::Mesh,
		static_cast<std::uint64_t>(currentFrame),
		static_cast<std::uint64_t>(totalGpuBytes),
		uploadDetail);
}

// VulkanContext::Impl::EnsureMeshDescriptorSet is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkDescriptorSet VulkanContext::Impl::EnsureMeshDescriptorSet(TerraEngine::UUID meshId, const VulkanPipeline& pipeline) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	auto meshIt = meshCache.find(meshId);
	if (meshIt == meshCache.end()) {
		return VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pipeline.setLayouts.size() <= 1 || pipeline.setLayouts[1] == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh shader pipeline is missing descriptor set layout 1 for mesh payload buffers.");
		return VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pipeline.bindingsPerSet.size() <= 1) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh shader pipeline is missing reflected binding metadata for descriptor set 1.");
		return VK_NULL_HANDLE;
	}

	const auto& meshBindings = pipeline.bindingsPerSet[1];
	auto HasStorageBinding = [&](uint32_t bindingIndex) -> bool {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const DescriptorBinding& binding : meshBindings) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (binding.binding == bindingIndex) {
				return binding.type == static_cast<uint32_t>(DescriptorType::StorageBuffer);
			}
		}

		return false;
	};

	// Validate reflected layout before descriptor writes to avoid VK validation
	// crashes when stale shader artifacts expose incomplete bindings.
	if (!HasStorageBinding(0)
		|| !HasStorageBinding(1)
		|| !HasStorageBinding(2)
		|| !HasStorageBinding(3)
		|| !HasStorageBinding(4)) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh shader descriptor set 1 must expose storage-buffer bindings 0 through 4.");
		return VK_NULL_HANDLE;
	}

	VulkanMesh& mesh = meshIt->second;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (mesh.meshShaderVertexBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshletVertexRemapBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshDecodeInfoBuffer.buffer == VK_NULL_HANDLE
		|| mesh.indexBuffer.buffer == VK_NULL_HANDLE
		|| mesh.meshletBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh payload buffers are not initialized for mesh shader descriptor binding.");
		return VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (HasStorageBinding(5) && mesh.meshletBoundsBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh descriptor requires bounds binding 5 but meshlet bounds payload is unavailable.");
		return VK_NULL_HANDLE;
	}
	if (HasStorageBinding(6) && mesh.meshSkinningBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh descriptor requires skinning binding 6 but neither real data nor the zero fallback buffer is available.");
		return VK_NULL_HANDLE;
	}
	if (HasStorageBinding(7) && mesh.meshMorphDeltaBuffer.buffer == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Mesh descriptor requires morph binding 7 but the safe morph delta buffer is unavailable.");
		return VK_NULL_HANDLE;
	}

	const VkDescriptorSetLayout layout = pipeline.setLayouts[1];
	auto cachedSetIt = mesh.descriptorSetsByLayout.find(layout);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cachedSetIt != mesh.descriptorSetsByLayout.end() && cachedSetIt->second != VK_NULL_HANDLE) {
		return cachedSetIt->second;
	}

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &layout;

	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to allocate mesh descriptor set for mesh shader pipeline.");
		return VK_NULL_HANDLE;
	}

	VkDescriptorBufferInfo vertexInfo{};
	vertexInfo.buffer = mesh.meshShaderVertexBuffer.buffer;
	vertexInfo.offset = 0;
	vertexInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo primitiveIndexInfo{};
	primitiveIndexInfo.buffer = mesh.indexBuffer.buffer;
	primitiveIndexInfo.offset = 0;
	primitiveIndexInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo meshletTableInfo{};
	meshletTableInfo.buffer = mesh.meshletBuffer.buffer;
	meshletTableInfo.offset = 0;
	meshletTableInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo vertexRemapInfo{};
	vertexRemapInfo.buffer = mesh.meshletVertexRemapBuffer.buffer;
	vertexRemapInfo.offset = 0;
	vertexRemapInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo decodeInfo{};
	decodeInfo.buffer = mesh.meshDecodeInfoBuffer.buffer;
	decodeInfo.offset = 0;
	decodeInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo boundsInfo{};
	boundsInfo.buffer = mesh.meshletBoundsBuffer.buffer;
	boundsInfo.offset = 0;
	boundsInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo skinningInfo{};
	skinningInfo.buffer = mesh.meshSkinningBuffer.buffer;
	skinningInfo.offset = 0;
	skinningInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo morphDeltaInfo{};
	morphDeltaInfo.buffer = mesh.meshMorphDeltaBuffer.buffer;
	morphDeltaInfo.offset = 0;
	morphDeltaInfo.range = VK_WHOLE_SIZE;

	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve(8);

	VkWriteDescriptorSet w{};
	w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w.dstSet = descriptorSet;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

	w.dstBinding = 0; w.pBufferInfo = &vertexInfo; writes.push_back(w);
	w.dstBinding = 1; w.pBufferInfo = &primitiveIndexInfo; writes.push_back(w);
	w.dstBinding = 2; w.pBufferInfo = &meshletTableInfo; writes.push_back(w);
	w.dstBinding = 3; w.pBufferInfo = &vertexRemapInfo; writes.push_back(w);
	w.dstBinding = 4; w.pBufferInfo = &decodeInfo; writes.push_back(w);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (HasStorageBinding(5))
	{
		w.dstBinding = 5; w.pBufferInfo = &boundsInfo; writes.push_back(w);
	}

	// Binding 6 is always written when declared. Static meshes own an explicit
	// zero skinning record so the statically used shader resource stays valid.
	if (HasStorageBinding(6))
	{
		w.dstBinding = 6; w.pBufferInfo = &skinningInfo; writes.push_back(w);
	}

	// Binding 7 is always written when declared. Static and legacy meshes own a
	// one-record zero fallback, so default mesh shaders never receive an invalid
	// descriptor while their morph weight resolves to zero.
	if (HasStorageBinding(7))
	{
		w.dstBinding = 7; w.pBufferInfo = &morphDeltaInfo; writes.push_back(w);
	}

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	mesh.descriptorSetsByLayout[layout] = descriptorSet;
	return descriptorSet;
}

// VulkanContext::Impl::BuildPipelineLayouts is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants) {
	if (globalDescriptorSetLayout == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Global descriptor set layout is not initialized; cannot build reflected pipeline layouts.");
		return false;
	}

	// struct AggregatedBinding groups related state and operations so callers have one documented contract.
	struct AggregatedBinding {
		DescriptorBinding binding{};
		VkShaderStageFlags stages = 0;
		bool initialized = false;
	};

	std::map<uint32_t, std::map<uint32_t, AggregatedBinding>> setBindings;
	std::map<std::pair<uint32_t, uint32_t>, VkShaderStageFlags> pushConstantMerge;
	supportsDefaultPushConstants = false;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& stage : collection.stages) {
		Shader* shader = ResourceManager::Instance().GetShader(stage.shaderId);
		if (!shader) {
			TERRAENGINE_LOG(LogLevel::Error, "Shader stage missing for collection; cannot build pipeline layouts");
			return false;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const DescriptorSetLayoutDesc& setDesc : shader->descriptorSets) {
			for (uint32_t idx = 0; idx < setDesc.bindingCount; ++idx) {
				const uint32_t bindingIndex = setDesc.firstBinding + idx;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (bindingIndex >= shader->descriptorBindings.size()) continue;
			const DescriptorBinding& srcBinding = shader->descriptorBindings[bindingIndex];
			auto& aggregated = setBindings[setDesc.setIndex][srcBinding.binding];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!aggregated.initialized) {
				aggregated.binding = srcBinding;
				if (aggregated.binding.arrayCount == 0) {
					aggregated.binding.arrayCount = 1; // default to 1 so descriptors get valid writes
				}
				aggregated.initialized = true;
			} else {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (aggregated.binding.type != srcBinding.type || aggregated.binding.arrayCount != srcBinding.arrayCount) {
					TERRAENGINE_LOG(LogLevel::Warning, "Descriptor binding mismatch across stages for set " << setDesc.setIndex << ", binding " << srcBinding.binding);
				}
				}
				StageVisibilityMask visibility = srcBinding.visibility;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (visibility == 0) {
					visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
				}
				aggregated.stages |= StageVisibilityToVulkan(visibility);
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const PushConstantRange& pc : shader->pushConstants) {
			StageVisibilityMask visibility = pc.visibility;
			if (visibility == 0) {
				visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
			}
			const VkShaderStageFlags vkStages = StageVisibilityToVulkan(visibility);
			std::pair<uint32_t, uint32_t> key = {pc.offsetBytes, pc.sizeBytes};
			pushConstantMerge[key] |= vkStages;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (pc.offsetBytes == 0
				&& pc.sizeBytes >= sizeof(PushConstants)
				&& (vkStages & VK_SHADER_STAGE_MESH_BIT_EXT)) {
				supportsDefaultPushConstants = true;
			}
		}
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [key, stages] : pushConstantMerge) {
		VkPushConstantRange range{};
		range.offset = key.first;
		range.size = key.second;
		range.stageFlags = stages;
		pushConstants.push_back(range);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pushConstants.empty()) {
		VkPushConstantRange fallback{};
		fallback.offset = 0;
		fallback.size = sizeof(PushConstants);
		fallback.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_TASK_BIT_EXT;
		pushConstants.push_back(fallback);
		supportsDefaultPushConstants = true;
	}

	// Set 0 is always the Global Bindless Heap
	setLayouts.push_back(globalDescriptorSetLayout);
	bindingsPerSet.push_back({}); 

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [setIndex, bindingMap] : setBindings) {
		if (setIndex == 0) continue; // Skip Set 0, handled globally

		// Handle gaps if sets are not contiguous (e.g. Set 0 and Set 2 but no Set 1)
		// This simple implementation assumes sequential or just appends. 
		// Real robustness would require resizing setLayouts to setIndex+1.
		// For now, we assume materials use Set 1.
		while (setLayouts.size() <= setIndex) {
			setLayouts.push_back(VK_NULL_HANDLE); // Placeholder for gaps?
			bindingsPerSet.push_back({});
		}
		
		// If we filled a gap with NULL, we need to create a real layout now?
		// Actually, standard behavior: just append. 
		// If setIndex is 1, and size is 1 (Set 0), we push back. Correct.
		
		std::vector<VkDescriptorSetLayoutBinding> vkBindings;
		std::vector<DescriptorBinding> storedBindings;
		vkBindings.reserve(bindingMap.size());
		storedBindings.reserve(bindingMap.size());

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& [bindingNumber, aggregated] : bindingMap) {
			VkDescriptorSetLayoutBinding vkBinding{};
			vkBinding.binding = bindingNumber;
			vkBinding.descriptorCount = aggregated.binding.arrayCount;
			vkBinding.descriptorType = MapDescriptorType(static_cast<DescriptorType>(aggregated.binding.type));
			vkBinding.stageFlags = aggregated.stages;
			vkBindings.push_back(vkBinding);
			storedBindings.push_back(aggregated.binding);
		}

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = static_cast<uint32_t>(vkBindings.size());
		layoutInfo.pBindings = vkBindings.data();

		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create descriptor set layout from reflection");
			for (size_t i = 1; i < setLayouts.size(); ++i) { // Start from 1 to avoid destroying global layout
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (setLayouts[i] != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayouts[i], nullptr);
			}
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (setLayouts.size() <= setIndex) {
			setLayouts.resize(setIndex + 1, VK_NULL_HANDLE);
			bindingsPerSet.resize(setIndex + 1);
		}
		setLayouts[setIndex] = layout;
		bindingsPerSet[setIndex] = std::move(storedBindings);
	}

	return true;
}

std::optional<PipelineKey> VulkanContext::Impl::BuildPipelineKey(
	TerraEngine::UUID shaderCollectionId,
	uint32_t materialFlags,
	bool usesConventionalVisibilityIdentity) const
{
	// Pipeline lookup must use every generation and ABI field used by insertion.
	// Otherwise a successfully-created pipeline is unreachable as soon as the
	// cache key grows beyond the collection UUID and material flags.
	ShaderCollection* collection =
		ResourceManager::Instance().GetShaderCollection(shaderCollectionId);
	if(collection == nullptr)
	{
		return std::nullopt;
	}

	std::uint64_t shaderModuleGeneration = 1469598103934665603ull;
	for(const ShaderCollection::StageBinding& binding : collection->stages)
	{
		for(const std::uint8_t byte : binding.shaderId)
		{
			shaderModuleGeneration ^= byte;
			shaderModuleGeneration *= 1099511628211ull;
		}
		const Shader* shader = ResourceManager::Instance().GetShader(binding.shaderId);
		if(shader == nullptr)
		{
			return std::nullopt;
		}
		shaderModuleGeneration ^= shader->bytecode.size();
		shaderModuleGeneration *= 1099511628211ull;
	}

	return PipelineKey{
		shaderCollectionId,
		materialFlags,
		collection->collectionGeneration,
		shaderModuleGeneration,
		collection->geometryAbiVersion,
		static_cast<std::uint32_t>(collection->geometryPolicy),
		usesConventionalVisibilityIdentity
	};
}

// VulkanContext::Impl::EnsurePipeline is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsurePipeline(
	TerraEngine::UUID shaderCollectionId,
	VkRenderPass renderPass,
	uint32_t materialFlags,
	bool usesConventionalVisibilityIdentity) {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (renderPass == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Cannot ensure pipeline: render pass handle is null.");
		return;
	}
	ShaderCollection* collection =
		ResourceManager::Instance().GetShaderCollection(shaderCollectionId);
	if(collection == nullptr || !collection->reflectionComplete
		|| collection->usesReservedBindings
		|| collection->usesAnimationBindings
		|| collection->geometryAbiVersion != 1u
		|| collection->collectionGeneration == 0u)
	{
		// Include every admission field in the diagnostic. A missing collection
		// and an invalid reflected contract require different repairs, and a
		// generic message made live Vulkan failures unnecessarily ambiguous.
		if(collection == nullptr)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Failed to build graphics pipeline: shader collection "
				<< ToHexString(shaderCollectionId) << " is unavailable.");
		}
		else
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Failed to build graphics pipeline: shader geometry contract is incomplete or incompatible"
				<< " (collection=" << ToHexString(shaderCollectionId)
				<< ", reflectionComplete=" << collection->reflectionComplete
				<< ", usesReservedBindings=" << collection->usesReservedBindings
				<< ", usesAnimationBindings=" << collection->usesAnimationBindings
				<< ", geometryAbiVersion=" << collection->geometryAbiVersion
				<< ", collectionGeneration=" << collection->collectionGeneration
				<< ").");
		}
		return;
	}
	const std::optional<PipelineKey> key = BuildPipelineKey(
		shaderCollectionId, materialFlags, usesConventionalVisibilityIdentity);
	if(!key.has_value())
	{
		TERRAENGINE_LOG(LogLevel::Error,
			"Failed to build graphics pipeline: shader module generation is missing.");
		return;
	}

	auto passIt = pipelineCache.find(renderPass);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (passIt != pipelineCache.end()) {
		auto cachedPipelineIt = passIt->second.find(*key);
		if (cachedPipelineIt != passIt->second.end()) {
			VulkanPipeline& cached = cachedPipelineIt->second;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (cached.pipeline != VK_NULL_HANDLE && cached.layout != VK_NULL_HANDLE) {
				return;
			}

			// A stale/invalid cache entry can appear when creation failed previously.
			// Remove it now so the next path can attempt a clean rebuild.
			if (device != VK_NULL_HANDLE) {
				if (cached.pipeline != VK_NULL_HANDLE) {
					vkDestroyPipeline(device, cached.pipeline, nullptr);
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (cached.layout != VK_NULL_HANDLE) {
					vkDestroyPipelineLayout(device, cached.layout, nullptr);
				}
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t layoutIndex = 1; layoutIndex < cached.setLayouts.size(); ++layoutIndex) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (cached.setLayouts[layoutIndex] != VK_NULL_HANDLE) {
						vkDestroyDescriptorSetLayout(device, cached.setLayouts[layoutIndex], nullptr);
					}
				}
			}
			passIt->second.erase(cachedPipelineIt);
		}
	}

	// Cache hits return above. This marker therefore reports only an actual
	// graphics-pipeline build or rebuild, which is the expensive driver path a
	// broad resource-preparation sample cannot distinguish.
	TERRAENGINE_PROFILE_SCOPE("Vulkan: Build Graphics Pipeline", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	
	const bool shadowPipeline = (renderPass == shadowRenderPass);
	const bool conventionalVisibilityPass = UsesConventionalVisibilityAttachments(renderPass);
	const bool defaultConventionalIdentityVariant = usesConventionalVisibilityIdentity
		&& conventionalVisibilityPass
		&& shaderCollectionId == ResourceManager::Instance().GetDefaultShaderCollectionID();
	const bool defaultTaskConventionalIdentityVariant = usesConventionalVisibilityIdentity
		&& conventionalVisibilityPass
		&& shaderCollectionId == ResourceManager::Instance().GetDefaultTaskShaderCollectionID();
	const bool useConventionalIdentityVariant = defaultConventionalIdentityVariant
		|| defaultTaskConventionalIdentityVariant;
	if (usesConventionalVisibilityIdentity && !useConventionalIdentityVariant) {
		// Authored surface shaders retain their target0-only ABI. The second
		// attachment remains clear for those draws instead of loading an arbitrary
		// SPIR-V fragment module that lacks their reflected descriptor contract.
		TERRAENGINE_LOG(LogLevel::Debug,
			"Conventional visibility identity is unavailable for a non-default surface collection; using target0-only shader ABI.");
	}
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
	bool hasMeshStage = false;
	bool hasTaskStage = false;
	bool identityVariantLoadFailed = false;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& stage : collection->stages) {
		Shader* shader = ResourceManager::Instance().GetShader(stage.shaderId);
		if (shader) {
			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			std::vector<std::uint32_t> conventionalIdentityBytecode;
			const bool replaceFragmentWithIdentity = useConventionalIdentityVariant
				&& stage.stage == ShaderStage::Fragment;
			if (replaceFragmentWithIdentity) {
				// The artifact is intentionally raw Vulkan SPIR-V rather than a
				// cross-backend .tspf resource. Resolve it from the installed engine
				// root so projects launched from arbitrary working directories agree.
				const std::filesystem::path variantPath = ResourceManager::Instance().GetEngineRoot()
					/ "Shaders" / "Default.ConventionalVisibilityIdentity.frag.spv";
				std::ifstream variantFile(variantPath, std::ios::ate | std::ios::binary);
				if (!variantFile.is_open()) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Failed to open Vulkan conventional visibility identity shader: " << variantPath);
					identityVariantLoadFailed = true;
					continue;
				}
				const std::streamsize byteCount = variantFile.tellg();
				if (byteCount <= 0 || (byteCount % static_cast<std::streamsize>(sizeof(std::uint32_t))) != 0) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Vulkan conventional visibility identity shader has an invalid SPIR-V byte count: " << variantPath);
					identityVariantLoadFailed = true;
					continue;
				}
				conventionalIdentityBytecode.resize(static_cast<std::size_t>(byteCount) / sizeof(std::uint32_t));
				variantFile.seekg(0);
				variantFile.read(reinterpret_cast<char*>(conventionalIdentityBytecode.data()), byteCount);
				if (!variantFile) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Failed to read Vulkan conventional visibility identity shader: " << variantPath);
					identityVariantLoadFailed = true;
					continue;
				}
				createInfo.codeSize = static_cast<std::size_t>(byteCount);
				createInfo.pCode = conventionalIdentityBytecode.data();
			} else {
				createInfo.codeSize = shader->bytecode.size();
				createInfo.pCode = reinterpret_cast<const uint32_t*>(shader->bytecode.data());
			}

			VkShaderModule shaderModule;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module");
				continue;
			}
			
			VkPipelineShaderStageCreateInfo stageInfo{};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (stage.stage == ShaderStage::Fragment) {
				stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			}
			else if (stage.stage == ShaderStage::Mesh) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!meshShaderSupported) {
					TERRAENGINE_LOG(LogLevel::Error, "Cannot build mesh pipeline because mesh shader support is unavailable on this device.");
					vkDestroyShaderModule(device, shaderModule, nullptr);
					continue;
				}
				stageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
				hasMeshStage = true;
			}
			else if (stage.stage == ShaderStage::Task) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!taskShaderSupported) {
					TERRAENGINE_LOG(LogLevel::Error, "Cannot build task shader stage because task shader support is unavailable on this device.");
					vkDestroyShaderModule(device, shaderModule, nullptr);
					continue;
				}
				stageInfo.stage = VK_SHADER_STAGE_TASK_BIT_EXT;
				hasTaskStage = true;
			}
			else {
				TERRAENGINE_LOG(LogLevel::Warning, "Skipping unsupported shader stage for graphics pipeline build.");
				vkDestroyShaderModule(device, shaderModule, nullptr);
				continue;
			}
			
			stageInfo.module = shaderModule;
			stageInfo.pName = shader->stringTable.data() + shader->binary.entryPointOffset;
			shaderStages.push_back(stageInfo);
		}
	}
	if (identityVariantLoadFailed) {
		// Do not silently build a mesh-only pipeline after a required fragment
		// variant failed to load; the next frame may recover after installation.
		for (const VkPipelineShaderStageCreateInfo& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (shaderStages.empty()) {
		TERRAENGINE_LOG(LogLevel::Error, "No shader stages available to build pipeline");
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!hasMeshStage) {
		TERRAENGINE_LOG(LogLevel::Error, "Graphics pipeline must include a mesh stage in this renderer.");
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasTaskStage && !hasMeshStage) {
		TERRAENGINE_LOG(LogLevel::Error, "Shader collection includes a task stage without a mesh stage.");
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	std::vector<VkDescriptorSetLayout> setLayouts;
	std::vector<std::vector<DescriptorBinding>> bindingsPerSet;
	std::vector<VkPushConstantRange> pushConstants;
	bool supportsDefaultPushConstants = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!BuildPipelineLayouts(*collection, setLayouts, bindingsPerSet, pushConstants, supportsDefaultPushConstants)) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 0;
	vertexInputInfo.pVertexBindingDescriptions = nullptr;
	vertexInputInfo.vertexAttributeDescriptionCount = 0;
	vertexInputInfo.pVertexAttributeDescriptions = nullptr;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	// Viewport and scissor are dynamic so editor window resizing and reduced
	// shadow targets use the current framebuffer dimensions instead of the size
	// that happened to be active when the pipeline was created.
	viewport.width = shadowPipeline ? static_cast<float>(SHADOW_MAP_RESOLUTION) : static_cast<float>(swapchainExtent.width);
	viewport.height = shadowPipeline ? static_cast<float>(SHADOW_MAP_RESOLUTION) : static_cast<float>(swapchainExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = shadowPipeline
		? VkExtent2D{SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION}
		: swapchainExtent;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;

	std::array<VkDynamicState, 2> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	// Shadow maps are visibility tests, not final shaded surfaces. Rendering
	// both sides makes primitive winding and negative scale less likely to make
	// a blocker disappear from the light's point of view.
	rasterizer.cullMode = shadowPipeline
		? VK_CULL_MODE_NONE
		: ((materialFlags & DoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	// Shadow pipelines still keep a tiny fragment stage for driver-compatible
	// depth rendering, but the color attachment is only present to satisfy the
	// shared render-pass shape and does not need stored color output.
	colorBlendAttachment.colorWriteMask = shadowPipeline ? 0u : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	colorBlendAttachment.blendEnable = VK_FALSE;

	// The identity target is integer typed. Required independentBlend permits
	// target0-only sky/UI/authored shaders to disable this slot's blending and
	// writes without changing their target0 blend state.
	VkPipelineColorBlendAttachmentState identityBlendAttachment{};
	identityBlendAttachment.colorWriteMask = useConventionalIdentityVariant
		? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
		: 0u;
	identityBlendAttachment.blendEnable = VK_FALSE;
	const std::array<VkPipelineColorBlendAttachmentState, 2> conventionalBlendAttachments = {
		colorBlendAttachment, identityBlendAttachment };

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.logicOp = VK_LOGIC_OP_COPY;
	colorBlending.attachmentCount = conventionalVisibilityPass ? 2u : 1u;
	colorBlending.pAttachments = conventionalVisibilityPass
		? conventionalBlendAttachments.data() : &colorBlendAttachment;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.minDepthBounds = 0.0f;
	depthStencil.maxDepthBounds = 1.0f;
	depthStencil.stencilTestEnable = VK_FALSE;
	depthStencil.front = {};
	depthStencil.back = {};

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
	pipelineLayoutInfo.pPushConstantRanges = pushConstants.data();

	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline layout.");
		return;
	}

	VkPipelineFragmentShadingRateStateCreateInfoKHR vrsState{};
	vrsState.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
	vrsState.fragmentSize = { 1, 1 };
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vrsFeatures.primitiveFragmentShadingRate) {
		vrsState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
	} else {
		vrsState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (UsesVrsAttachment()) {
		vrsState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
	} else {
		vrsState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.pNext = &vrsState;
	pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
	pipelineInfo.pStages = shaderStages.data();
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = pipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

	VkPipeline graphicsPipeline = VK_NULL_HANDLE;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline.");
		return;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& stage : shaderStages) {
		vkDestroyShaderModule(device, stage.module, nullptr);
	}

	VkShaderStageFlags defaultPushConstantStages = 0;
	bool hasCompatibleDefaultPushRange = false;
	// vkCmdPushConstants validates every byte against every requested stage.
	// Draw submission now pushes each reflected range separately; the full-range
	// mask remains as conservative metadata for pipelines whose stages all share
	// the complete default mesh PushConstants contract.
	for (const VkPushConstantRange& range : pushConstants) {
		if (range.stageFlags != 0u
			&& range.size > 0u
			&& range.offset < sizeof(PushConstants)
			&& range.size <= sizeof(PushConstants) - range.offset) {
			hasCompatibleDefaultPushRange = true;
		}
		if (range.offset == 0u && range.size >= sizeof(PushConstants)) {
			defaultPushConstantStages |= range.stageFlags;
		}
	}
	supportsDefaultPushConstants = hasCompatibleDefaultPushRange;
	if (!supportsDefaultPushConstants) {
		TERRAENGINE_LOG(LogLevel::Warning, "Graphics pipeline push constants do not overlap the default mesh push-constant contract; draw submission will skip default push ranges.");
	}

	VulkanPipeline pipeline{};
	pipeline.pipeline = graphicsPipeline;
	pipeline.layout = pipelineLayout;
	pipeline.setLayouts = std::move(setLayouts);
	pipeline.bindingsPerSet = std::move(bindingsPerSet);
	pipeline.pushConstants = std::move(pushConstants);
	pipeline.supportsDefaultPushConstants = supportsDefaultPushConstants;
	pipeline.defaultPushConstantStages = defaultPushConstantStages;
	pipeline.hasTaskStage = hasTaskStage;

	pipelineCache[renderPass][*key] = std::move(pipeline);
}

// VulkanContext::Impl::CreateGlobalDescriptorResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateGlobalDescriptorResources() {
	std::array<VkDescriptorSetLayoutBinding, 25> bindings{};
	// Textures
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = MAX_BINDLESS_TEXTURES;
	bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[0].pImmutableSamplers = nullptr;
	// Materials
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[1].pImmutableSamplers = nullptr;
	// Frame culling payload (frustum planes + camera data) consumed by task stage.
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[2].pImmutableSamplers = nullptr;
	// Meshlet counters used for stats/debug output.
	bindings[3].binding = 3;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[3].pImmutableSamplers = nullptr;
	// Global light payload array consumed by fragment and compute stages.
	bindings[4].binding = 4;
	bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[4].descriptorCount = 1;
	bindings[4].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[4].pImmutableSamplers = nullptr;
	// Cluster metadata table (offset/count per cluster).
	bindings[5].binding = 5;
	bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[5].descriptorCount = 1;
	bindings[5].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[5].pImmutableSamplers = nullptr;
	// Flat light-index list addressed by the cluster metadata.
	bindings[6].binding = 6;
	bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[6].descriptorCount = 1;
	bindings[6].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[6].pImmutableSamplers = nullptr;
	// Dedicated omnidirectional point-shadow cubemap image used by fragment shading.
	bindings[7].binding = 7;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[7].descriptorCount = 1;
	bindings[7].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[7].pImmutableSamplers = nullptr;
	// Dedicated sampler for the point-shadow cubemap.
	bindings[8].binding = 8;
	bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	bindings[8].descriptorCount = 1;
	bindings[8].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[8].pImmutableSamplers = nullptr;
	// Bindless Buffers (for MDI and mesh data)
	bindings[9].binding = 9;
	bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[9].descriptorCount = MAX_BINDLESS_BUFFERS;
	bindings[9].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[9].pImmutableSamplers = nullptr;
	// Hi-Z Pyramid Texture
	bindings[10].binding = 10;
	bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[10].descriptorCount = kHiZViewCount;
	bindings[10].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[10].pImmutableSamplers = nullptr;
	// Hi-Z Sampler
	bindings[11].binding = 11;
	bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	bindings[11].descriptorCount = 1;
	bindings[11].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[11].pImmutableSamplers = nullptr;
	// Global Depth Buffer for masking and VRS
	bindings[12].binding = 12;
	bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[12].descriptorCount = 1;
	bindings[12].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[12].pImmutableSamplers = nullptr;
	// Cluster Depth Bounds for light culling masking
	bindings[13].binding = 13;
	bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[13].descriptorCount = 1;
	bindings[13].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[13].pImmutableSamplers = nullptr;
	// VRS Shading Rate Image
	bindings[14].binding = 14;
	bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[14].descriptorCount = 1;
	bindings[14].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[14].pImmutableSamplers = nullptr;
	// VRS Color Input
	bindings[15].binding = 15;
	bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[15].descriptorCount = 1;
	bindings[15].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[15].pImmutableSamplers = nullptr;
	// Hi-Z Storage Output (one mip at a time)
	bindings[16].binding = 16;
	bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[16].descriptorCount = 1;
	bindings[16].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[16].pImmutableSamplers = nullptr;
	// Lightweight GPU light-analysis output consumed by the hybrid selection path.
	bindings[17].binding = 17;
	bindings[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[17].descriptorCount = 1;
	bindings[17].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[17].pImmutableSamplers = nullptr;
	// Separate candidate-light input for the GPU analysis pass so the final
	// retained-light upload can use a different buffer in the same frame.
	bindings[18].binding = 18;
	bindings[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[18].descriptorCount = 1;
	bindings[18].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[18].pImmutableSamplers = nullptr;
	// Screen-space ambient occlusion produced by GTAO and sampled by blur and material shading.
	bindings[19].binding = 19;
	bindings[19].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[19].descriptorCount = 1;
	bindings[19].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[19].pImmutableSamplers = nullptr;
	// Point-light shadows are sampled as cubemaps and cannot share the 2D
	// bindless texture heap. Keep a small slot array that mirrors the CPU shadow
	// resource assignment table.
	bindings[20].binding = 20;
	bindings[20].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[20].descriptorCount = MAX_SHADOW_RESOURCE_SLOTS;
	bindings[20].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[20].pImmutableSamplers = nullptr;
	// Previous-frame AO is sampled by the bilateral blur pass for temporal
	// smoothing. It is a separate binding so the raw AO input can keep using 19.
	bindings[21].binding = 21;
	bindings[21].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[21].descriptorCount = 1;
	bindings[21].stageFlags = VK_SHADER_STAGE_ALL;
	bindings[21].pImmutableSamplers = nullptr;
	// Raw AO input is kept separate from the final material AO binding so
	// command recording never depends on rebinding one descriptor to two
	// meanings inside the same command buffer.
	bindings[22].binding = 22;
	bindings[22].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[22].descriptorCount = 1;
	bindings[22].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[22].pImmutableSamplers = nullptr;
	// GTAO raw output uses a dedicated storage binding instead of Hi-Z's
	// storage binding. Sharing binding 16 made optimized frames race against
	// whichever descriptor update happened last during command recording.
	bindings[23].binding = 23;
	bindings[23].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[23].descriptorCount = 1;
	bindings[23].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[23].pImmutableSamplers = nullptr;
	// GTAO blur output is separate from raw output so blur never reads and
	// writes through the same descriptor binding.
	bindings[24].binding = 24;
	bindings[24].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[24].descriptorCount = 1;
	bindings[24].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[24].pImmutableSamplers = nullptr;

	VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{};
	bindingFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	std::array<VkDescriptorBindingFlags, 25> flags = {
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 0: Textures
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 1: Materials (updated mid-frame)
		0, // 2
		0, // 3
		0, // 4
		0, // 5
		0, // 6
		0, // 7
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 8: Point-shadow sampler can change when shadow resources are resized between editor views.
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 9: Bindless Buffers
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 10: Hi-Z Texture
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 11: Hi-Z sampler can be republished while multi-view command buffers are still executable.
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 12: Depth Buffer
		0, // 13: Cluster Depth
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 14: VRS Image
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 15: VRS Color
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 16: Hi-Z Output
		0, // 17: Light analysis output
		0, // 18: Light analysis input
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 19: Ambient occlusion texture
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 20: Point shadow cubemap array
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 21: AO temporal history
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 22: AO raw sampled input
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 23: AO raw storage output
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT // 24: AO blur storage output
	};
	bindingFlags.bindingCount = static_cast<uint32_t>(flags.size());
	bindingFlags.pBindingFlags = flags.data();

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	layoutInfo.pNext = &bindingFlags;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &globalDescriptorSetLayout) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create global descriptor set layout!");
		return false;
	}

	std::array<VkDescriptorPoolSize, 5> poolSizes{};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[0].descriptorCount = MAX_BINDLESS_TEXTURES * MAX_FRAMES_IN_FLIGHT;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	poolSizes[1].descriptorCount = (6 + kHiZViewCount + MAX_SHADOW_RESOURCE_SLOTS) * MAX_FRAMES_IN_FLIGHT; // Point shadows + per-view Hi-Z + Depth + VRS Color + AO + AO history + AO raw
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	poolSizes[2].descriptorCount = 2 * MAX_FRAMES_IN_FLIGHT; // Point shadow + Hi-Z
	poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[3].descriptorCount = (9 + MAX_BINDLESS_BUFFERS) * MAX_FRAMES_IN_FLIGHT;
	poolSizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[4].descriptorCount = 4 * MAX_FRAMES_IN_FLIGHT; // VRS Image + Hi-Z Output + AO raw + AO blur

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &globalDescriptorPool) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create global descriptor pool!");
		return false;
	}

	globalDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = globalDescriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &globalDescriptorSetLayout;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkAllocateDescriptorSets(device, &allocInfo, &globalDescriptorSets[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to allocate global descriptor set for frame " << i);
			return false;
		}
	}

	// Binding 8 is a standalone sampler that the material shader statically
	// declares for point-shadow cubemaps. Initialize it even before any point
	// light requests shadow storage so descriptor validation always sees a
	// valid sampler; real point-shadow allocation overwrites this descriptor
	// with the active cubemap sampler later.
	VkSamplerCreateInfo fallbackPointSamplerInfo{};
	fallbackPointSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	fallbackPointSamplerInfo.magFilter = VK_FILTER_LINEAR;
	fallbackPointSamplerInfo.minFilter = VK_FILTER_LINEAR;
	fallbackPointSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	fallbackPointSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	fallbackPointSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	fallbackPointSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	fallbackPointSamplerInfo.anisotropyEnable = VK_FALSE;
	fallbackPointSamplerInfo.maxAnisotropy = 1.0f;
	fallbackPointSamplerInfo.compareEnable = VK_FALSE;
	fallbackPointSamplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	fallbackPointSamplerInfo.minLod = 0.0f;
	fallbackPointSamplerInfo.maxLod = 1.0f;
	fallbackPointSamplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	fallbackPointSamplerInfo.unnormalizedCoordinates = VK_FALSE;
	if (vkCreateSampler(device, &fallbackPointSamplerInfo, nullptr, &fallbackPointShadowSampler) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create fallback point-shadow sampler.");
		return false;
	}

	VkDescriptorImageInfo fallbackPointSamplerDescriptor{};
	fallbackPointSamplerDescriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	fallbackPointSamplerDescriptor.imageView = VK_NULL_HANDLE;
	fallbackPointSamplerDescriptor.sampler = fallbackPointShadowSampler;

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		VkWriteDescriptorSet samplerWrite{};
		samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		samplerWrite.dstSet = globalDescriptorSets[i];
		samplerWrite.dstBinding = 8;
		samplerWrite.dstArrayElement = 0;
		samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerWrite.descriptorCount = 1;
		samplerWrite.pImageInfo = &fallbackPointSamplerDescriptor;
		vkUpdateDescriptorSets(device, 1, &samplerWrite, 0, nullptr);
	}

	return true;
}

// VulkanContext::Impl::CreateMaterialBufferResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateMaterialBufferResources() {
	VkDeviceSize bufferSize = sizeof(MaterialData) * MAX_BINDLESS_MATERIALS;
	globalMaterialSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		globalMaterialSSBOs[i] = CreateBuffer(bufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = globalMaterialSSBOs[i].buffer;
		bufferInfo.offset = 0;
		bufferInfo.range = bufferSize;

		VkWriteDescriptorSet descriptorWrite{};
		descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite.dstSet = globalDescriptorSets[i];
		descriptorWrite.dstBinding = 1;
		descriptorWrite.dstArrayElement = 0;
		descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptorWrite.descriptorCount = 1;
		descriptorWrite.pBufferInfo = &bufferInfo;

		vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
	}

	const MaterialData fallbackMaterial = CreateFallbackMaterialData();
	std::vector<MaterialData> fallbackMaterials(MAX_BINDLESS_MATERIALS, fallbackMaterial);
	VulkanBuffer stagingBuffer = CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Fallback Material Staging Buffer");
	void* mappedData = nullptr;
	if (vmaMapMemory(allocator, stagingBuffer.allocation, &mappedData) == VK_SUCCESS) {
		std::memcpy(mappedData, fallbackMaterials.data(), static_cast<std::size_t>(bufferSize));
		vmaFlushAllocation(allocator, stagingBuffer.allocation, 0, bufferSize);
		vmaUnmapMemory(allocator, stagingBuffer.allocation);

		VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
		if (commandBuffer != VK_NULL_HANDLE)
		{
			VkBufferCopy copyRegion{};
			copyRegion.srcOffset = 0u;
			copyRegion.dstOffset = 0u;
			copyRegion.size = bufferSize;
			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
				vkCmdCopyBuffer(commandBuffer, stagingBuffer.buffer, globalMaterialSSBOs[i].buffer, 1, &copyRegion);
			}
			EndSingleTimeCommands(commandBuffer, "fallback material upload");
		}
	}
	else {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to map fallback material staging buffer.");
	}
	vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

	// Slot zero is permanently reserved for missing/unloaded materials. Real
	// material assets start at one so draw commands that fall back to zero never
	// accidentally sample an authored material or uninitialized GPU memory.
	freeMaterialIndices.clear();
	materialIndexMap.clear();
	nextMaterialIndex = kFirstRuntimeMaterialIndex;
}

// VulkanContext::Impl::CreateCullingBufferResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateCullingBufferResources() {
	const VkDeviceSize frameDataBufferSize = static_cast<VkDeviceSize>(sizeof(FrameCullingDataGPU) * kStatsSlotCount);
	const VkDeviceSize countersBufferSize = static_cast<VkDeviceSize>(sizeof(MeshletCullingCountersGPU) * kStatsSlotCount);

	globalFrameCullingSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	globalMeshletCountersSSBOs.resize(MAX_FRAMES_IN_FLIGHT);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		globalFrameCullingSSBOs[i] = CreateBuffer(frameDataBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		globalMeshletCountersSSBOs[i] = CreateBuffer(countersBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		FrameCullingDataGPU defaultFrameData{};
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t slotIndex = 0; slotIndex < kStatsSlotCount; ++slotIndex) {
			// Initialize all slots for this frame's buffer
			void* mapped = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vmaMapMemory(allocator, globalFrameCullingSSBOs[i].allocation, &mapped) == VK_SUCCESS) {
				const VkDeviceSize offset = static_cast<VkDeviceSize>(slotIndex) * sizeof(FrameCullingDataGPU);
				std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, &defaultFrameData, sizeof(FrameCullingDataGPU));
				vmaFlushAllocation(allocator, globalFrameCullingSSBOs[i].allocation, offset, sizeof(FrameCullingDataGPU));
				vmaUnmapMemory(allocator, globalFrameCullingSSBOs[i].allocation);
			}
		}

		VkDescriptorBufferInfo frameCullingInfo{};
		frameCullingInfo.buffer = globalFrameCullingSSBOs[i].buffer;
		frameCullingInfo.offset = 0;
		frameCullingInfo.range = frameDataBufferSize;

		VkDescriptorBufferInfo meshletCountersInfo{};
		meshletCountersInfo.buffer = globalMeshletCountersSSBOs[i].buffer;
		meshletCountersInfo.offset = 0;
		meshletCountersInfo.range = countersBufferSize;

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalDescriptorSets[i];
		writes[0].dstBinding = 2;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo = &frameCullingInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = globalDescriptorSets[i];
		writes[1].dstBinding = 3;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].descriptorCount = 1;
		writes[1].pBufferInfo = &meshletCountersInfo;

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t viewIndex = 0; viewIndex < kStatsViewCount; ++viewIndex) {
		latestStatsByView[viewIndex] = {};
		latestStatsByView[viewIndex].viewType = (viewIndex == 0u) ? RenderViewType::Game : RenderViewType::Scene;
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0; slotIndex < kStatsSlotCount; ++slotIndex) {
		pendingStatsBySlot[slotIndex] = {};
		pendingStatsBySlot[slotIndex].viewType = (slotIndex % kStatsViewCount == 0u) ? RenderViewType::Game : RenderViewType::Scene;
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (ClusterValidationSnapshot& snapshot : pendingClusterValidationByFrame) {
		snapshot = {};
		snapshot.viewType = RenderViewType::Game;
	}
	previousClusterValidationSnapshot = {};
	previousClusterValidationSnapshot.viewType = RenderViewType::Game;
	previousClusterValidationSnapshotValid = false;
}

// VulkanContext::Impl::CreateLightingBufferResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateLightingBufferResources() {
	const VkDeviceSize lightBufferSize = static_cast<VkDeviceSize>(sizeof(GPULight) * MAX_GLOBAL_LIGHTS * MAX_RENDER_SNAPSHOTS_PER_FRAME);
	const VkDeviceSize clusterBufferSize = static_cast<VkDeviceSize>(sizeof(ClusterData) * MAX_LIGHT_CLUSTERS * MAX_RENDER_SNAPSHOTS_PER_FRAME);
	const VkDeviceSize lightIndexBufferSize = static_cast<VkDeviceSize>(sizeof(uint32_t) * MAX_LIGHT_INDEX_ENTRIES * MAX_RENDER_SNAPSHOTS_PER_FRAME);
	const VkDeviceSize clusterDepthBufferSize = static_cast<VkDeviceSize>(sizeof(float) * 2 * kLightingClusterDimX * kLightingClusterDimY * MAX_RENDER_SNAPSHOTS_PER_FRAME);
	const VkDeviceSize lightAnalysisBufferSize = static_cast<VkDeviceSize>(sizeof(GPULightAnalysisRecord) * MAX_GLOBAL_LIGHTS * kStatsSlotCount);

	clusterValidationCaptureEnabled = IsClusterValidationCaptureEnabledFromEnvironment();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterValidationCaptureEnabled) {
		TERRAENGINE_LOG(LogLevel::Info, "Cluster validation capture enabled via TERRAENGINE_CLUSTER_VALIDATION_CAPTURE. Lighting buffers will remain CPU-visible for frame-end readback.");
	}

	globalLightSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	lightAnalysisInputSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	clusterGridSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	lightIndexSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	clusterDepthSSBOs.resize(MAX_FRAMES_IN_FLIGHT);
	lightAnalysisSSBOs.resize(MAX_FRAMES_IN_FLIGHT);

	const VmaMemoryUsage clusterBufferMemoryUsage = clusterValidationCaptureEnabled
		? VMA_MEMORY_USAGE_CPU_TO_GPU
		: VMA_MEMORY_USAGE_GPU_ONLY;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		// Light uploads are CPU-driven every frame, so this buffer must remain
		// CPU-visible regardless of whether validation capture is active.
		globalLightSSBOs[i] = CreateBuffer(lightBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		lightAnalysisInputSSBOs[i] = CreateBuffer(lightBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "Light Analysis Input Buffer");
		clusterGridSSBOs[i] = CreateBuffer(clusterBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, clusterBufferMemoryUsage);
		lightIndexSSBOs[i] = CreateBuffer(lightIndexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, clusterBufferMemoryUsage);
		clusterDepthSSBOs[i] = CreateBuffer(clusterDepthBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, clusterBufferMemoryUsage);
		lightAnalysisSSBOs[i] = CreateBuffer(lightAnalysisBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, "Light Analysis Buffer");

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterValidationCaptureEnabled) {
			// Validation mode seeds buffers with deterministic defaults so the
			// frame-end readback code can flag incomplete writes immediately.
			void* mappedClusterGrid = nullptr;
			if (vmaMapMemory(allocator, clusterGridSSBOs[i].allocation, &mappedClusterGrid) == VK_SUCCESS) {
				std::memset(mappedClusterGrid, 0, static_cast<size_t>(clusterBufferSize));
				vmaFlushAllocation(allocator, clusterGridSSBOs[i].allocation, 0, clusterBufferSize);
				vmaUnmapMemory(allocator, clusterGridSSBOs[i].allocation);
			}

			void* mappedLightIndices = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vmaMapMemory(allocator, lightIndexSSBOs[i].allocation, &mappedLightIndices) == VK_SUCCESS) {
				std::fill_n(static_cast<std::uint32_t*>(mappedLightIndices), MAX_LIGHT_INDEX_ENTRIES * MAX_RENDER_SNAPSHOTS_PER_FRAME, 0xFFFFFFFFu);
				vmaFlushAllocation(allocator, lightIndexSSBOs[i].allocation, 0, lightIndexBufferSize);
				vmaUnmapMemory(allocator, lightIndexSSBOs[i].allocation);
			}

			void* mappedClusterDepth = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vmaMapMemory(allocator, clusterDepthSSBOs[i].allocation, &mappedClusterDepth) == VK_SUCCESS) {
				std::fill_n(static_cast<float*>(mappedClusterDepth), static_cast<size_t>(clusterDepthBufferSize / sizeof(float)), 1.0f);
				vmaFlushAllocation(allocator, clusterDepthSSBOs[i].allocation, 0, clusterDepthBufferSize);
				vmaUnmapMemory(allocator, clusterDepthSSBOs[i].allocation);
			}
		}

		void* mappedLightAnalysis = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaMapMemory(allocator, lightAnalysisSSBOs[i].allocation, &mappedLightAnalysis) == VK_SUCCESS) {
			std::memset(mappedLightAnalysis, 0, static_cast<size_t>(lightAnalysisBufferSize));
			vmaFlushAllocation(allocator, lightAnalysisSSBOs[i].allocation, 0, lightAnalysisBufferSize);
			vmaUnmapMemory(allocator, lightAnalysisSSBOs[i].allocation);
		}

		VkDescriptorBufferInfo lightInfo{};
		lightInfo.buffer = globalLightSSBOs[i].buffer;
		lightInfo.offset = 0;
		lightInfo.range = lightBufferSize;

		VkDescriptorBufferInfo clusterInfo{};
		clusterInfo.buffer = clusterGridSSBOs[i].buffer;
		clusterInfo.offset = 0;
		clusterInfo.range = clusterBufferSize;

		VkDescriptorBufferInfo lightIndexInfo{};
		lightIndexInfo.buffer = lightIndexSSBOs[i].buffer;
		lightIndexInfo.offset = 0;
		lightIndexInfo.range = lightIndexBufferSize;

		VkDescriptorImageInfo depthInfo{};
		depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		depthInfo.imageView = depthImageView;
		// Cluster-depth consumers only need a sampler object. Reuse the
		// sampler from the same frame's Hi-Z resource so descriptor set `i`
		// never references a sampler owned by another frame slot.
		depthInfo.sampler = HiZForFrame(kHiZGameViewIndex, i).sampler;

		VkDescriptorBufferInfo clusterDepthInfo{};
		clusterDepthInfo.buffer = clusterDepthSSBOs[i].buffer;
		clusterDepthInfo.offset = 0;
		clusterDepthInfo.range = clusterDepthBufferSize;

		VkDescriptorBufferInfo lightAnalysisInfo{};
		lightAnalysisInfo.buffer = lightAnalysisSSBOs[i].buffer;
		lightAnalysisInfo.offset = 0;
		lightAnalysisInfo.range = lightAnalysisBufferSize;

		VkDescriptorBufferInfo lightAnalysisInputInfo{};
		lightAnalysisInputInfo.buffer = lightAnalysisInputSSBOs[i].buffer;
		lightAnalysisInputInfo.offset = 0;
		lightAnalysisInputInfo.range = lightBufferSize;

		std::array<VkWriteDescriptorSet, 7> writes{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalDescriptorSets[i];
		writes[0].dstBinding = 4;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo = &lightInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = globalDescriptorSets[i];
		writes[1].dstBinding = 5;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].descriptorCount = 1;
		writes[1].pBufferInfo = &clusterInfo;

		writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[2].dstSet = globalDescriptorSets[i];
		writes[2].dstBinding = 6;
		writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[2].descriptorCount = 1;
		writes[2].pBufferInfo = &lightIndexInfo;

		writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[3].dstSet = globalDescriptorSets[i];
		writes[3].dstBinding = 12;
		writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[3].descriptorCount = 1;
		writes[3].pImageInfo = &depthInfo;

		writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[4].dstSet = globalDescriptorSets[i];
		writes[4].dstBinding = 13;
		writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[4].descriptorCount = 1;
		writes[4].pBufferInfo = &clusterDepthInfo;

		writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[5].dstSet = globalDescriptorSets[i];
		writes[5].dstBinding = 17;
		writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[5].descriptorCount = 1;
		writes[5].pBufferInfo = &lightAnalysisInfo;

		writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[6].dstSet = globalDescriptorSets[i];
		writes[6].dstBinding = 18;
		writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[6].descriptorCount = 1;
		writes[6].pBufferInfo = &lightAnalysisInputInfo;

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}

	UploadLightingData({});
}

// VulkanContext::Impl::CreateIndirectDrawResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateIndirectDrawResources() {
	VkDeviceSize indirectBufferSize = MAX_BINDLESS_DRAWS * MAX_RENDER_SNAPSHOTS_PER_FRAME * sizeof(VkDrawMeshTasksIndirectCommandEXT);
	indirectDrawBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		indirectDrawBuffers[i] = CreateBuffer(indirectBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	}
}

// VulkanContext::Impl::DestroyEnvironmentLightingResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyEnvironmentLightingResources()
{
	auto DestroyImage = [&](VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image = {};
	};

	DestroyImage(environmentIrradianceTexture);
	DestroyImage(prefilteredEnvironmentTexture);
	DestroyImage(brdfIntegrationTexture);
	environmentIrradianceTextureIndex = 0u;
	prefilteredEnvironmentTextureIndex = 0u;
	brdfIntegrationTextureIndex = 0u;
}

// VulkanContext::Impl::CreateEnvironmentLightingResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateEnvironmentLightingResources()
{
	if (environmentIrradianceTexture.view != VK_NULL_HANDLE
		&& prefilteredEnvironmentTexture.view != VK_NULL_HANDLE
		&& brdfIntegrationTexture.view != VK_NULL_HANDLE) {
		return;
	}

	DestroyEnvironmentLightingResources();

	auto CreateFloatTexture = [&](const std::vector<glm::vec4>& texels, uint32_t width, uint32_t height, const char* allocationName, VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (texels.empty() || width == 0u || height == 0u) {
			return;
		}

		const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(sizeof(glm::vec4) * texels.size());
		VulkanBuffer stagingBuffer = CreateBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Environment Lighting Staging Buffer");
		WriteMappedBuffer(allocator, stagingBuffer, texels.data(), uploadSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { width, height, 1u };
		imageInfo.mipLevels = 1u;
		imageInfo.arrayLayers = 1u;
		imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
			image = {};
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, allocationName);

		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, image.image, width, height);
		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view == VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image = {};
			return;
		}

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 1.0f;
		vkCreateSampler(device, &samplerInfo, nullptr, &image.sampler);
	};

	// These deterministic fallback textures give the renderer real IBL
	// descriptors before imported sky probes exist. The material shader can
	// continue using its procedural fallback without risking unbound resources.
	CreateFloatTexture({ glm::vec4(0.18f, 0.22f, 0.28f, 1.0f) }, 1u, 1u, "Fallback Environment Irradiance", environmentIrradianceTexture);
	CreateFloatTexture({ glm::vec4(0.45f, 0.55f, 0.72f, 1.0f) }, 1u, 1u, "Fallback Prefiltered Environment", prefilteredEnvironmentTexture);

	const uint32_t brdfSize = 64u;
	std::vector<glm::vec4> brdfTexels(brdfSize * brdfSize);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t y = 0u; y < brdfSize; ++y) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t x = 0u; x < brdfSize; ++x) {
			const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(brdfSize);
			const float nDotV = (static_cast<float>(x) + 0.5f) / static_cast<float>(brdfSize);
			brdfTexels[y * brdfSize + x] = glm::vec4(1.0f - 0.35f * roughness, nDotV * (1.0f - roughness), 0.0f, 1.0f);
		}
	}
	CreateFloatTexture(brdfTexels, brdfSize, brdfSize, "Fallback BRDF Integration LUT", brdfIntegrationTexture);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (environmentIrradianceTexture.view != VK_NULL_HANDLE) {
		environmentIrradianceTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(environmentIrradianceTextureIndex, environmentIrradianceTexture);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (prefilteredEnvironmentTexture.view != VK_NULL_HANDLE) {
		prefilteredEnvironmentTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(prefilteredEnvironmentTextureIndex, prefilteredEnvironmentTexture);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (brdfIntegrationTexture.view != VK_NULL_HANDLE) {
		brdfIntegrationTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(brdfIntegrationTextureIndex, brdfIntegrationTexture);
	}
}

// VulkanContext::Impl::DestroyAmbientOcclusionResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyAmbientOcclusionResources()
{
	auto DestroyImage = [&](VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image = {};
	};

	for (VulkanImage& image : ambientOcclusionRawByFrame) {
		DestroyImage(image);
	}
	for (VulkanImage& image : ambientOcclusionBlurredByFrame) {
		DestroyImage(image);
	}
	for (VulkanImage& image : ambientOcclusionHistoryByFrame) {
		DestroyImage(image);
	}
	for (ViewDepthHistoryState& viewHistory : depthHistoryByView) {
		for (VulkanImage& image : viewHistory.ambientOcclusionDepthByFrame) {
			DestroyImage(image);
		}
		viewHistory.ambientOcclusionDepthValidByFrame.fill(false);
	}
	ambientOcclusionWidth = 0u;
	ambientOcclusionHeight = 0u;
	ambientOcclusionHistoryValidByFrame.fill(false);
}

// VulkanContext::Impl::CreateAmbientOcclusionResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateAmbientOcclusionResources()
{
	const uint32_t width = std::max(swapchainExtent.width, 1u);
	const uint32_t height = std::max(swapchainExtent.height, 1u);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ambientOcclusionRawByFrame.empty()
		&& ambientOcclusionRawByFrame[0].image != VK_NULL_HANDLE
		&& ambientOcclusionBlurredByFrame[0].image != VK_NULL_HANDLE
		&& ambientOcclusionHistoryByFrame[0].image != VK_NULL_HANDLE
		&& depthHistoryByView[0].ambientOcclusionDepthByFrame[0].image != VK_NULL_HANDLE
		&& ambientOcclusionWidth == width
		&& ambientOcclusionHeight == height) {
		return;
	}

	DestroyAmbientOcclusionResources();

	auto CreateAoImage = [&](const char* allocationName, VulkanImage& image) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { width, height, 1u };
		imageInfo.mipLevels = 1u;
		imageInfo.arrayLayers = 1u;
		imageInfo.format = AMBIENT_OCCLUSION_FORMAT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		// AO images are shader-readable/writable, clearable during creation,
		// and the resolved frame image is copied into the history image after
		// blur. Include both transfer directions so the temporal path never has
		// to swap Vulkan handles or rewrite material descriptors mid-frame.
		imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image = {};
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, allocationName);

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view == VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image = {};
			return;
		}

		// Newly created AO targets must contain explicit visibility data before
		// any optimized shader is allowed to sample them. A value of 1.0 means
		// fully open/no contact darkening, which is the safe fallback until the
		// compute pass writes real occlusion.
		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkCommandBuffer clearCommandBuffer = BeginSingleTimeCommands();
		if (clearCommandBuffer != VK_NULL_HANDLE)
		{
			VkImageSubresourceRange clearRange{};
			clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			clearRange.baseMipLevel = 0u;
			clearRange.levelCount = 1u;
			clearRange.baseArrayLayer = 0u;
			clearRange.layerCount = 1u;
			VkClearColorValue clearValue{};
			clearValue.float32[0] = 1.0f;
			clearValue.float32[1] = 1.0f;
			clearValue.float32[2] = 1.0f;
			clearValue.float32[3] = 1.0f;
			vkCmdClearColorImage(clearCommandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1u, &clearRange);
			EndSingleTimeCommands(clearCommandBuffer, "ambient occlusion initialization clear");
		}
		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	};

	auto CreateAoDepthImage = [&](const char* allocationName, VulkanImage& image) {
		const VkFormat historyDepthFormat = (depthFormat != VK_FORMAT_UNDEFINED) ? depthFormat : FindDepthFormat();
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { width, height, 1u };
		imageInfo.mipLevels = 1u;
		imageInfo.arrayLayers = 1u;
		imageInfo.format = historyDepthFormat;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image = {};
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, allocationName);

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_DEPTH_BIT);
		if (image.view == VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image = {};
			return;
		}

		// The history image starts readable so descriptor sets can be populated
		// immediately; it is sampled like a texture, not used as a depth
		// attachment, so keep it in shader-read layout between transfer writes.
		VkCommandBuffer transitionCommandBuffer = BeginSingleTimeCommands();
		if (transitionCommandBuffer == VK_NULL_HANDLE)
		{
			return;
		}
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = 1u;
		barrier.subresourceRange.baseArrayLayer = 0u;
		barrier.subresourceRange.layerCount = 1u;
		barrier.srcAccessMask = 0u;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(
			transitionCommandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier);
		EndSingleTimeCommands(transitionCommandBuffer, "ambient occlusion depth-history transition");
	};

	bool allImagesReady = true;
	for (uint32_t frameIndex = 0u; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
		CreateAoImage("GTAO Raw Occlusion", ambientOcclusionRawByFrame[frameIndex]);
		CreateAoImage("GTAO Blurred Occlusion", ambientOcclusionBlurredByFrame[frameIndex]);
		CreateAoImage("GTAO History Occlusion", ambientOcclusionHistoryByFrame[frameIndex]);
		allImagesReady = allImagesReady
			&& ambientOcclusionRawByFrame[frameIndex].view != VK_NULL_HANDLE
			&& ambientOcclusionBlurredByFrame[frameIndex].view != VK_NULL_HANDLE
			&& ambientOcclusionHistoryByFrame[frameIndex].view != VK_NULL_HANDLE;
	}
	for (ViewDepthHistoryState& viewHistory : depthHistoryByView) {
		for (uint32_t frameIndex = 0u; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
			CreateAoDepthImage("GTAO Depth History", viewHistory.ambientOcclusionDepthByFrame[frameIndex]);
			allImagesReady = allImagesReady
				&& viewHistory.ambientOcclusionDepthByFrame[frameIndex].view != VK_NULL_HANDLE;
		}
		viewHistory.ambientOcclusionDepthValidByFrame.fill(false);
	}
	ambientOcclusionWidth = allImagesReady ? width : 0u;
	ambientOcclusionHeight = allImagesReady ? height : 0u;
	ambientOcclusionHistoryValidByFrame.fill(false);

	if (!allImagesReady) {
		return;
	}

	for (uint32_t frameIndex = 0u; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
		VkDescriptorImageInfo finalAoInfo{};
		finalAoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		finalAoInfo.imageView = ambientOcclusionBlurredByFrame[frameIndex].view;
		finalAoInfo.sampler = VK_NULL_HANDLE;

		// Global binding 19 is the only AO descriptor consumed by material
		// shading. Compute-only AO inputs and outputs are bound through the
		// private per-snapshot AO descriptor set during GenerateAmbientOcclusion.
		std::array<VkWriteDescriptorSet, 1> descriptorWrites{};
		descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[0].dstSet = globalDescriptorSets[frameIndex];
		descriptorWrites[0].dstBinding = 19;
		descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].pImageInfo = &finalAoInfo;
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
	}
}

// VulkanContext::Impl::EnsureShadowRenderResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::EnsureShadowRenderResources(const ShadowResourceRequest& request) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	ShadowResourceRequest targetRequest{};
	targetRequest.directionalCount = std::min(request.directionalCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.spotCount = std::min(request.spotCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.areaCount = std::min(request.areaCount, MAX_SHADOW_RESOURCE_SLOTS);
	targetRequest.pointCount = std::min(request.pointCount, MAX_SHADOW_RESOURCE_SLOTS);
	// Scene and Game views can request different shadow capacities in one CPU
	// frame. Publish and complete every earlier recorded view before replacing
	// framebuffers, image views, or descriptor bindings that those views own.
	WaitForDescriptorMutationIdle();
	if (deviceLost) {
		return false;
	}
	const VkResult idleResult = WaitDeviceIdle();
	if (MarkDeviceLost(idleResult, "shadow render resource rebuild device idle")) {
		return false;
	}
	if (idleResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping shadow render resource rebuild because the device did not become idle: " << idleResult);
		return false;
	}
	// Rebuilding this block (for example after swapchain recreation) must tear
	// down stale views/framebuffers before allocating replacement images.
	auto DestroyFramebuffer = [&](VkFramebuffer& framebuffer) {
		if (framebuffer != VK_NULL_HANDLE) {
			vkDestroyFramebuffer(device, framebuffer, nullptr);
			framebuffer = VK_NULL_HANDLE;
		}
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : directionalShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : spotShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : areaShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkFramebuffer, 6>& faceFramebuffers : pointShadowFaceFramebuffersBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkFramebuffer& framebuffer : faceFramebuffers) {
			DestroyFramebuffer(framebuffer);
		}
	}

	auto DestroyImageView = [&](VkImageView& imageView) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (imageView != VK_NULL_HANDLE) {
			vkDestroyImageView(device, imageView, nullptr);
			imageView = VK_NULL_HANDLE;
		}
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkImageView, 6>& faceViews : pointShadowDepthFaceViewsBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkImageView& faceView : faceViews) {
			DestroyImageView(faceView);
		}
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkImageView, 6>& faceViews : pointShadowColorFaceViewsBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkImageView& faceView : faceViews) {
			DestroyImageView(faceView);
		}
	}

	auto DestroyImage = [&](VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
			image.sampler = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
			image.view = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image.image = VK_NULL_HANDLE;
		image.allocation = VK_NULL_HANDLE;
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : directionalShadowDepths) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : directionalShadowColors) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : spotShadowDepths) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : spotShadowColors) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : areaShadowDepths) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : areaShadowColors) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : pointShadowCubeDepths) {
		DestroyImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : pointShadowCubeColors) {
		DestroyImage(image);
	}
	activeShadowResourceRequest = {};
	shadowStorageRegions.fill({});
	shadowCacheEntries.clear();
	DestroyImage(ltcAmplitudeTexture);
	DestroyImage(ltcInverseTexture);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (targetRequest.TotalCount() == 0u) {
		return true;
	}

	const VkFormat shadowDepthFormat = (depthFormat != VK_FORMAT_UNDEFINED) ? depthFormat : FindDepthFormat();
	const uint32_t pointShadowSize = POINT_SHADOW_MAP_RESOLUTION;
	const auto Shadow2DRegionSizeForSlot = [](std::uint32_t typedSlot) {
		// Lower-priority 2D shadows use physically smaller images. This is the
		// memory-saving step; diagnostics and viewport state mirror this size.
		return typedSlot >= 16u ? SHADOW_MAP_RESOLUTION / 2u : SHADOW_MAP_RESOLUTION;
	};

	auto CreateColorAttachment = [&](uint32_t width, uint32_t height, uint32_t layers, VkImageCreateFlags flags, VulkanImage& image, VkImageViewType viewType) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = layers;
		imageInfo.format = swapchainImageFormat;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.flags = flags;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, "Shadow/Attachment Color Image");
		// Shadow color is reused as an attachment every frame. Initialize it once
		// instead of asking each render pass to discard an unknown prior layout.
		if (!TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1u, layers)) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image.image;
		viewInfo.viewType = viewType;
		viewInfo.format = imageInfo.format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = layers;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateImageView(device, &viewInfo, nullptr, &image.view) != VK_SUCCESS) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			image.view = VK_NULL_HANDLE;
		}
	};

	auto CreateDepthAttachment2D = [&](uint32_t width, uint32_t height, VulkanImage& image) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = shadowDepthFormat;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, "Shadow/Attachment Depth Image");
		// The shadow pass finishes in read-only layout for material sampling. Start
		// the first frame in that same persistent layout so first and later uses
		// follow one render-pass contract.
		if (!TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}
		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_DEPTH_BIT);
	};

	auto CreateDepthAttachmentCube = [&](uint32_t size, VulkanImage& image) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = size;
		imageInfo.extent.height = size;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 6;
		imageInfo.format = shadowDepthFormat;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}
		vmaSetAllocationName(allocator, image.allocation, "Shadow/Attachment Cube Depth Image");
		// Transition every cubemap face. Each face has its own framebuffer, but all
		// six layers must agree with the shadow render pass before any face is used.
		if (!TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 1u, 6u)) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		viewInfo.format = imageInfo.format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 6;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateImageView(device, &viewInfo, nullptr, &image.view) != VK_SUCCESS) {
			vmaDestroyImage(allocator, image.image, image.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			image.view = VK_NULL_HANDLE;
		}
	};

	auto CreateShadowSampler = [&](VulkanImage& image, bool useCompareSampler) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image == VK_NULL_HANDLE) {
			return;
		}

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.maxAnisotropy = 1.0f;
		samplerInfo.compareEnable = useCompareSampler ? VK_TRUE : VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 1.0f;
		samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		vkCreateSampler(device, &samplerInfo, nullptr, &image.sampler);
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.directionalCount; ++slotIndex) {
		const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
		CreateColorAttachment(regionSize, regionSize, 1u, 0u, directionalShadowColors[slotIndex], VK_IMAGE_VIEW_TYPE_2D);
		CreateDepthAttachment2D(regionSize, regionSize, directionalShadowDepths[slotIndex]);
		CreateShadowSampler(directionalShadowDepths[slotIndex], false);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.spotCount; ++slotIndex) {
		const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
		CreateColorAttachment(regionSize, regionSize, 1u, 0u, spotShadowColors[slotIndex], VK_IMAGE_VIEW_TYPE_2D);
		CreateDepthAttachment2D(regionSize, regionSize, spotShadowDepths[slotIndex]);
		CreateShadowSampler(spotShadowDepths[slotIndex], false);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.areaCount; ++slotIndex) {
		const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
		CreateColorAttachment(regionSize, regionSize, 1u, 0u, areaShadowColors[slotIndex], VK_IMAGE_VIEW_TYPE_2D);
		CreateDepthAttachment2D(regionSize, regionSize, areaShadowDepths[slotIndex]);
		CreateShadowSampler(areaShadowDepths[slotIndex], false);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.pointCount; ++slotIndex) {
		CreateColorAttachment(pointShadowSize, pointShadowSize, 6u, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, pointShadowCubeColors[slotIndex], VK_IMAGE_VIEW_TYPE_CUBE);
		CreateDepthAttachmentCube(pointShadowSize, pointShadowCubeDepths[slotIndex]);
		CreateShadowSampler(pointShadowCubeDepths[slotIndex], false);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
			VkImageViewCreateInfo depthFaceViewInfo{};
			depthFaceViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			depthFaceViewInfo.image = pointShadowCubeDepths[slotIndex].image;
			depthFaceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			depthFaceViewInfo.format = shadowDepthFormat;
			depthFaceViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			depthFaceViewInfo.subresourceRange.baseMipLevel = 0;
			depthFaceViewInfo.subresourceRange.levelCount = 1;
			depthFaceViewInfo.subresourceRange.baseArrayLayer = faceIndex;
			depthFaceViewInfo.subresourceRange.layerCount = 1;
			vkCreateImageView(device, &depthFaceViewInfo, nullptr, &pointShadowDepthFaceViewsBySlot[slotIndex][faceIndex]);

			VkImageViewCreateInfo colorFaceViewInfo{};
			colorFaceViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			colorFaceViewInfo.image = pointShadowCubeColors[slotIndex].image;
			colorFaceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			colorFaceViewInfo.format = swapchainImageFormat;
			colorFaceViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			colorFaceViewInfo.subresourceRange.baseMipLevel = 0;
			colorFaceViewInfo.subresourceRange.levelCount = 1;
			colorFaceViewInfo.subresourceRange.baseArrayLayer = faceIndex;
			colorFaceViewInfo.subresourceRange.layerCount = 1;
			vkCreateImageView(device, &colorFaceViewInfo, nullptr, &pointShadowColorFaceViewsBySlot[slotIndex][faceIndex]);
		}
	}

	auto BindDepthTexture = [&](uint32_t index, const VulkanImage& image) {
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		imageInfo.imageView = image.view;
		imageInfo.sampler = image.sampler;

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			VkWriteDescriptorSet descriptorWrite{};
			descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			descriptorWrite.dstSet = globalDescriptorSets[i];
			descriptorWrite.dstBinding = 0;
			descriptorWrite.dstArrayElement = index;
			descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			descriptorWrite.descriptorCount = 1;
			descriptorWrite.pImageInfo = &imageInfo;
			vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
		}
	};

	// Register only the active 2D shadow regions into the bindless texture
	// heap. Each light stores the resolved texture index after CPU assignment.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.directionalCount; ++slotIndex) {
		directionalShadowTextureIndices[slotIndex] = AllocateTextureIndex();
		BindDepthTexture(directionalShadowTextureIndices[slotIndex], directionalShadowDepths[slotIndex]);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.spotCount; ++slotIndex) {
		spotShadowTextureIndices[slotIndex] = AllocateTextureIndex();
		BindDepthTexture(spotShadowTextureIndices[slotIndex], spotShadowDepths[slotIndex]);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t slotIndex = 0u; slotIndex < targetRequest.areaCount; ++slotIndex) {
		areaShadowTextureIndices[slotIndex] = AllocateTextureIndex();
		BindDepthTexture(areaShadowTextureIndices[slotIndex], areaShadowDepths[slotIndex]);
	}

	// Point shadows use a dedicated cubemap binding array because the global
	// bindless heap is declared as Texture2D in shaders.
	if (targetRequest.pointCount > 0u) {
		std::vector<VkDescriptorImageInfo> pointShadowCubeImageInfos(targetRequest.pointCount);
		for (uint32_t slotIndex = 0u; slotIndex < targetRequest.pointCount; ++slotIndex) {
			pointShadowCubeImageInfos[slotIndex].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			pointShadowCubeImageInfos[slotIndex].imageView = pointShadowCubeDepths[slotIndex].view;
			pointShadowCubeImageInfos[slotIndex].sampler = VK_NULL_HANDLE;
		}

		VkDescriptorImageInfo pointShadowCubeSamplerInfo{};
		pointShadowCubeSamplerInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		pointShadowCubeSamplerInfo.imageView = VK_NULL_HANDLE;
		pointShadowCubeSamplerInfo.sampler = pointShadowCubeDepths[0].sampler;

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			std::array<VkWriteDescriptorSet, 2> pointShadowCubeWrites{};
			pointShadowCubeWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			pointShadowCubeWrites[0].dstSet = globalDescriptorSets[i];
			pointShadowCubeWrites[0].dstBinding = 20;
			pointShadowCubeWrites[0].dstArrayElement = 0;
			pointShadowCubeWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			pointShadowCubeWrites[0].descriptorCount = targetRequest.pointCount;
			pointShadowCubeWrites[0].pImageInfo = pointShadowCubeImageInfos.data();

			pointShadowCubeWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			pointShadowCubeWrites[1].dstSet = globalDescriptorSets[i];
			pointShadowCubeWrites[1].dstBinding = 8;
			pointShadowCubeWrites[1].dstArrayElement = 0;
			pointShadowCubeWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			pointShadowCubeWrites[1].descriptorCount = 1;
			pointShadowCubeWrites[1].pImageInfo = &pointShadowCubeSamplerInfo;

			vkUpdateDescriptorSets(device, static_cast<uint32_t>(pointShadowCubeWrites.size()), pointShadowCubeWrites.data(), 0, nullptr);
		}
	}
	activeShadowResourceRequest = targetRequest;
	shadowStorageRegions.fill({});
	const auto estimateRegionBytes = [&](ShadowResourceKind kind) {
		constexpr std::uint64_t kDepthBytesPerPixel = 4u;
		constexpr std::uint64_t kColorBytesPerPixel = 4u;
		const std::uint64_t texelCount = static_cast<std::uint64_t>(SHADOW_MAP_RESOLUTION) * static_cast<std::uint64_t>(SHADOW_MAP_RESOLUTION);
		const std::uint64_t pointTexelCount = static_cast<std::uint64_t>(POINT_SHADOW_MAP_RESOLUTION) * static_cast<std::uint64_t>(POINT_SHADOW_MAP_RESOLUTION) * 6u;
		return (kind == ShadowResourceKind::PointCube ? pointTexelCount : texelCount) * (kDepthBytesPerPixel + kColorBytesPerPixel);
	};
	std::uint32_t regionIndex = 0u;
	auto registerRegion = [&](ShadowResourceKind kind, std::uint32_t typedSlot) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (regionIndex >= shadowStorageRegions.size()) {
			return;
		}
		const bool reduced2DRegion = kind != ShadowResourceKind::PointCube && typedSlot >= 16u;
		ShadowStorageRegion& region = shadowStorageRegions[regionIndex++];
		region.resourceKind = kind;
		region.typedSlot = typedSlot;
		region.atlasId = static_cast<std::uint32_t>(kind);
		region.x = 0u;
		region.y = 0u;
		region.width = reduced2DRegion ? Shadow2DRegionSizeForSlot(typedSlot) : SHADOW_MAP_RESOLUTION;
		region.height = reduced2DRegion ? Shadow2DRegionSizeForSlot(typedSlot) : SHADOW_MAP_RESOLUTION;
		region.qualityTier = reduced2DRegion ? ShadowQualityTier::ReducedResolution : ShadowQualityTier::FullResolution;
		region.estimatedBytes = estimateRegionBytes(kind);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (reduced2DRegion) {
			region.estimatedBytes /= 4u;
		}
		region.live = true;
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t slotIndex = 0u; slotIndex < targetRequest.directionalCount; ++slotIndex) {
		registerRegion(ShadowResourceKind::DirectionalCascade, slotIndex);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t slotIndex = 0u; slotIndex < targetRequest.spotCount; ++slotIndex) {
		registerRegion(ShadowResourceKind::SpotAtlas, slotIndex);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t slotIndex = 0u; slotIndex < targetRequest.areaCount; ++slotIndex) {
		registerRegion(ShadowResourceKind::AreaAtlas, slotIndex);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t slotIndex = 0u; slotIndex < targetRequest.pointCount; ++slotIndex) {
		registerRegion(ShadowResourceKind::PointCube, slotIndex);
	}

	// Build lightweight procedural LTC lookup textures. These are deterministic
	// fallback tables until authored LUT assets are imported through content.
	auto CreateLtcLookupTexture = [&](const std::vector<glm::vec4>& texels, VulkanImage& image) {
		if (texels.empty()) {
			return;
		}

		const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(sizeof(glm::vec4) * texels.size());
		VulkanBuffer stagingBuffer = CreateBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		WriteMappedBuffer(allocator, stagingBuffer, texels.data(), uploadSize);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = LTC_LUT_RESOLUTION;
		imageInfo.extent.height = LTC_LUT_RESOLUTION;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
			vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
			image.image = VK_NULL_HANDLE;
			image.allocation = VK_NULL_HANDLE;
			return;
		}

		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, image.image, LTC_LUT_RESOLUTION, LTC_LUT_RESOLUTION);
		TransitionImageLayout(image.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

		image.view = CreateImageView(image.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.maxAnisotropy = 1.0f;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 1.0f;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		vkCreateSampler(device, &samplerInfo, nullptr, &image.sampler);
	};

	const uint32_t lutSize = LTC_LUT_RESOLUTION;
	std::vector<glm::vec4> amplitudeLut(lutSize * lutSize, glm::vec4(1.0f));
	std::vector<glm::vec4> inverseLut(lutSize * lutSize, glm::vec4(0.0f));
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t y = 0u; y < lutSize; ++y) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t x = 0u; x < lutSize; ++x) {
			const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(lutSize);
			const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(lutSize);
			const float roughness = std::clamp(v, 0.02f, 1.0f);
			const float fresnelScale = std::clamp(1.0f - 0.5f * roughness, 0.05f, 1.0f);
			amplitudeLut[y * lutSize + x] = glm::vec4(fresnelScale, fresnelScale, fresnelScale, 1.0f);
			inverseLut[y * lutSize + x] = glm::vec4(1.0f, 0.0f, 0.0f, u);
		}
	}

	CreateLtcLookupTexture(amplitudeLut, ltcAmplitudeTexture);
	CreateLtcLookupTexture(inverseLut, ltcInverseTexture);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ltcAmplitudeTexture.image != VK_NULL_HANDLE) {
		ltcAmplitudeTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(ltcAmplitudeTextureIndex, ltcAmplitudeTexture);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ltcInverseTexture.image != VK_NULL_HANDLE) {
		ltcInverseTextureIndex = AllocateTextureIndex();
		UpdateGlobalTexture(ltcInverseTextureIndex, ltcInverseTexture);
	}

	return true;
}

// VulkanContext::Impl::UploadLightingData is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UploadLightingData(const std::vector<GPULight>& lights) {
	if (globalLightSSBOs.empty() || globalLightSSBOs[currentFrame].buffer == VK_NULL_HANDLE) {
		return;
	}

	const uint32_t clampedLightCount = static_cast<uint32_t>(std::min<size_t>(lights.size(), MAX_GLOBAL_LIGHTS));
	const uint32_t lightOffset = activeRenderSnapshot.valid ? activeRenderSnapshot.lightDataOffset : 0u;

	void* mappedLights = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, globalLightSSBOs[currentFrame].allocation, &mappedLights) == VK_SUCCESS) {
		auto* gpuLights = static_cast<GPULight*>(mappedLights) + lightOffset;
		std::fill(gpuLights, gpuLights + MAX_GLOBAL_LIGHTS, GPULight{});
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clampedLightCount > 0u) {
			std::memcpy(gpuLights, lights.data(), sizeof(GPULight) * clampedLightCount);
		}
		const VkDeviceSize byteOffset = static_cast<VkDeviceSize>(lightOffset) * sizeof(GPULight);
		const VkDeviceSize byteSize = static_cast<VkDeviceSize>(MAX_GLOBAL_LIGHTS) * sizeof(GPULight);
		vmaFlushAllocation(allocator, globalLightSSBOs[currentFrame].allocation, byteOffset, byteSize);
		vmaUnmapMemory(allocator, globalLightSSBOs[currentFrame].allocation);
	}
}

// VulkanContext::Impl::UploadLightAnalysisInputData is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UploadLightAnalysisInputData(const std::vector<GPULight>& lights) {
	if (lightAnalysisInputSSBOs.empty() || lightAnalysisInputSSBOs[currentFrame].buffer == VK_NULL_HANDLE) {
		return;
	}

	const uint32_t clampedLightCount = static_cast<uint32_t>(std::min<size_t>(lights.size(), MAX_GLOBAL_LIGHTS));
	const uint32_t lightOffset = activeRenderSnapshot.valid ? activeRenderSnapshot.lightAnalysisInputOffset : 0u;

	void* mappedLights = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, lightAnalysisInputSSBOs[currentFrame].allocation, &mappedLights) == VK_SUCCESS) {
		auto* gpuLights = static_cast<GPULight*>(mappedLights) + lightOffset;
		std::fill(gpuLights, gpuLights + MAX_GLOBAL_LIGHTS, GPULight{});
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clampedLightCount > 0u) {
			std::memcpy(gpuLights, lights.data(), sizeof(GPULight) * clampedLightCount);
		}
		const VkDeviceSize byteOffset = static_cast<VkDeviceSize>(lightOffset) * sizeof(GPULight);
		const VkDeviceSize byteSize = static_cast<VkDeviceSize>(MAX_GLOBAL_LIGHTS) * sizeof(GPULight);
		vmaFlushAllocation(allocator, lightAnalysisInputSSBOs[currentFrame].allocation, byteOffset, byteSize);
		vmaUnmapMemory(allocator, lightAnalysisInputSSBOs[currentFrame].allocation);
	}
}

// VulkanContext::Impl::ResetMeshletCounters is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::ResetMeshletCounters(uint32_t statsSlotIndex) {
	if (globalMeshletCountersSSBOs.empty() || globalMeshletCountersSSBOs[currentFrame].buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return;
	}

	MeshletCullingCountersGPU counters{};
	void* mapped = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, globalMeshletCountersSSBOs[currentFrame].allocation, &mapped) != VK_SUCCESS) {
		return;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(MeshletCullingCountersGPU);
	std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, &counters, sizeof(MeshletCullingCountersGPU));
	vmaFlushAllocation(allocator, globalMeshletCountersSSBOs[currentFrame].allocation, offset, sizeof(MeshletCullingCountersGPU));
	vmaUnmapMemory(allocator, globalMeshletCountersSSBOs[currentFrame].allocation);
}

// VulkanContext::Impl::UpdateFrameCullingData is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateFrameCullingData(uint32_t statsSlotIndex, const FrameCullingDataGPU& frameData) {
	if (globalFrameCullingSSBOs.empty() || globalFrameCullingSSBOs[currentFrame].buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return;
	}

	void* mapped = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, globalFrameCullingSSBOs[currentFrame].allocation, &mapped) != VK_SUCCESS) {
		return;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(FrameCullingDataGPU);
	std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, &frameData, sizeof(FrameCullingDataGPU));
	vmaFlushAllocation(allocator, globalFrameCullingSSBOs[currentFrame].allocation, offset, sizeof(FrameCullingDataGPU));
	vmaUnmapMemory(allocator, globalFrameCullingSSBOs[currentFrame].allocation);
}

// VulkanContext::Impl::ReadMeshletCounters is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
MeshletCullingCountersGPU VulkanContext::Impl::ReadMeshletCounters(uint32_t statsSlotIndex) const {
	MeshletCullingCountersGPU counters{};
	if (globalMeshletCountersSSBOs.empty() || globalMeshletCountersSSBOs[currentFrame].buffer == VK_NULL_HANDLE || statsSlotIndex >= kStatsSlotCount) {
		return counters;
	}

	void* mapped = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, globalMeshletCountersSSBOs[currentFrame].allocation, &mapped) != VK_SUCCESS) {
		return counters;
	}
	const VkDeviceSize offset = static_cast<VkDeviceSize>(statsSlotIndex) * sizeof(MeshletCullingCountersGPU);
	vmaInvalidateAllocation(allocator, globalMeshletCountersSSBOs[currentFrame].allocation, offset, sizeof(MeshletCullingCountersGPU));
	std::memcpy(&counters, static_cast<const std::uint8_t*>(mapped) + offset, sizeof(MeshletCullingCountersGPU));
	vmaUnmapMemory(allocator, globalMeshletCountersSSBOs[currentFrame].allocation);
	return counters;
}

// VulkanContext::Impl::GetStatsSlotIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
uint32_t VulkanContext::Impl::GetStatsSlotIndex(uint32_t frameIndex, VulkanContext::ViewType view) const {
	const uint32_t clampedFrameIndex = std::min(frameIndex, kMaxStatsFrameSlots - 1u);
	const uint32_t viewIndex = (view == VulkanContext::ViewType::Game) ? 0u : 1u;
	return clampedFrameIndex * kStatsViewCount + viewIndex;
}

// VulkanContext::Impl::CreateGpuTimestampQueries is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateGpuTimestampQueries() {
	gpuTimestampQueriesSupported = false;
	gpuTimestampPeriodNanoseconds = 0.0f;
	completedVirtualGeometryExecutionReceipts.clear();
	completedVirtualGeometryMeasurements.clear();
	completedVirtualGeometryStageEvidence.fill(false);
	for (auto& frameSamples : completedGpuTimestampSamplesByFrame) {
		for (GpuTimestampViewSample& sample : frameSamples) {
			sample = {};
			sample.reason = "GPU timestamp queries have not produced a completed sample yet.";
		}
	}
	for (auto& frameWrites : pendingGpuTimestampWritesByFrame) {
		frameWrites.fill(false);
	}
	for (auto& frameIds : pendingGpuTimestampFrameIdsByFrame) {
		frameIds.fill(0u);
	}
	for(auto& frameSamples : virtualGeometryGpuTimestampSamplesByFrame) {
		for(auto& viewSamples : frameSamples) {
			for(VirtualGeometryGpuTimestampStageSample& sample : viewSamples) {
				sample = {};
				sample.reason = "No live Vulkan command owns this virtual-geometry stage yet.";
			}
		}
	}

	if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
		return true;
	}

	VkPhysicalDeviceProperties deviceProperties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
	gpuTimestampPeriodNanoseconds = deviceProperties.limits.timestampPeriod;

	const QueueFamilyIndices indices = FindQueueFamilies(physicalDevice);
	std::uint32_t queueFamilyCount = 0u;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	if (queueFamilyCount > 0u) {
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
	}

	const bool graphicsQueueHasTimestampBits = indices.graphicsFamily.has_value()
		&& *indices.graphicsFamily < queueFamilies.size()
		&& queueFamilies[*indices.graphicsFamily].timestampValidBits > 0u;
	if (!deviceProperties.limits.timestampComputeAndGraphics
		|| !graphicsQueueHasTimestampBits
		|| gpuTimestampPeriodNanoseconds <= 0.0f) {
		for (auto& frameSamples : completedGpuTimestampSamplesByFrame) {
			for (GpuTimestampViewSample& sample : frameSamples) {
				sample = {};
				sample.reason = "The selected Vulkan graphics queue does not expose timestamp queries.";
			}
		}
		TERRAENGINE_LOG(LogLevel::Warning, "Vulkan GPU profiler timestamps are unavailable on the selected graphics queue.");
		return true;
	}

	const std::uint32_t queryCount = static_cast<std::uint32_t>(MAX_FRAMES_IN_FLIGHT)
		* kTotalGpuTimestampQueriesPerFrame;
	VkQueryPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
	poolInfo.queryCount = queryCount;
	if (vkCreateQueryPool(device, &poolInfo, nullptr, &gpuTimestampQueryPool) != VK_SUCCESS) {
		for (auto& frameSamples : completedGpuTimestampSamplesByFrame) {
			for (GpuTimestampViewSample& sample : frameSamples) {
				sample = {};
				sample.reason = "Vulkan failed to create the GPU timestamp query pool.";
			}
		}
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to create Vulkan GPU timestamp query pool; profiler GPU rows will stay unavailable.");
		gpuTimestampQueryPool = VK_NULL_HANDLE;
		return true;
	}

	// A new query pool starts with uninitialized queries. Reset every slot once
	// through a command buffer so startup does not require the optional
	// hostQueryReset feature; the per-view reset below still handles reuse.
	VkCommandBuffer resetCommandBuffer = BeginSingleTimeCommands();
	if (resetCommandBuffer == VK_NULL_HANDLE)
	{
		vkDestroyQueryPool(device, gpuTimestampQueryPool, nullptr);
		gpuTimestampQueryPool = VK_NULL_HANDLE;
		return true;
	}
	vkCmdResetQueryPool(resetCommandBuffer, gpuTimestampQueryPool, 0u, queryCount);
	if (!EndSingleTimeCommands(resetCommandBuffer, "GPU timestamp query reset"))
	{
		vkDestroyQueryPool(device, gpuTimestampQueryPool, nullptr);
		gpuTimestampQueryPool = VK_NULL_HANDLE;
		return true;
	}
	gpuTimestampQueriesSupported = true;
	// Query-pool generation changes whenever Vulkan recreates the underlying
	// object. Completed measurements carry this value so consumers can reject
	// stale readbacks instead of joining unrelated device lifetimes.
	++gpuTimestampQueryPoolGeneration;
	if(gpuTimestampQueryPoolGeneration == 0u) {
		gpuTimestampQueryPoolGeneration = 1u;
	}
	return true;
}

// VulkanContext::Impl::DestroyGpuTimestampQueries is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyGpuTimestampQueries() {
	if (device != VK_NULL_HANDLE && gpuTimestampQueryPool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(device, gpuTimestampQueryPool, nullptr);
	}
	gpuTimestampQueryPool = VK_NULL_HANDLE;
	gpuTimestampQueriesSupported = false;
	gpuTimestampPeriodNanoseconds = 0.0f;
	for (auto& frameWrites : pendingGpuTimestampWritesByFrame) {
		frameWrites.fill(false);
	}
	for(auto& frameSamples : virtualGeometryGpuTimestampSamplesByFrame) {
		for(auto& viewSamples : frameSamples) {
			for(VirtualGeometryGpuTimestampStageSample& sample : viewSamples) {
				sample = {};
				sample.reason = "Vulkan timestamp queries are unavailable because the query pool was destroyed.";
			}
		}
	}
}

// VulkanContext::Impl::MarkGpuTimestampBegin is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::MarkGpuTimestampBegin(VkCommandBuffer cmd, uint32_t frameIndex, VulkanContext::ViewType view, std::uint64_t frameId) {
	if (!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| cmd == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	const uint32_t viewIndex = (view == VulkanContext::ViewType::Game) ? 0u : 1u;
	const uint32_t firstQuery = frameIndex * kTotalGpuTimestampQueriesPerFrame + viewIndex * kGpuTimestampQueriesPerView;
	// Reset immediately before writing so reused frame-slot queries never carry
	// an old availability bit into the next profiler sample.
	vkCmdResetQueryPool(cmd, gpuTimestampQueryPool, firstQuery, kGpuTimestampQueriesPerView);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpuTimestampQueryPool, firstQuery);
	pendingGpuTimestampWritesByFrame[frameIndex][viewIndex] = true;
	pendingGpuTimestampFrameIdsByFrame[frameIndex][viewIndex] = frameId;
	completedGpuTimestampSamplesByFrame[frameIndex][viewIndex] = {
		false,
		0.0f,
		frameId,
		0u,
		"GPU timestamp query is pending until this frame slot completes."
	};
}

// VulkanContext::Impl::MarkGpuTimestampEnd is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::MarkGpuTimestampEnd(VkCommandBuffer cmd, uint32_t frameIndex, VulkanContext::ViewType view) {
	if (!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| cmd == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	const uint32_t viewIndex = (view == VulkanContext::ViewType::Game) ? 0u : 1u;
	const uint32_t firstQuery = frameIndex * kTotalGpuTimestampQueriesPerFrame + viewIndex * kGpuTimestampQueriesPerView;
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpuTimestampQueryPool, firstQuery + 1u);
}

// VulkanContext::Impl::BeginVirtualGeometryTimestampFrame establishes explicit
// unavailable rows for every stage. Later executors must replace a row only when
// they own a live Vulkan command; CPU-prepared packets cannot opt in here.
void VulkanContext::Impl::BeginVirtualGeometryTimestampFrame(
	uint32_t frameIndex,
	VulkanContext::ViewType view,
	std::uint64_t frameNumber,
	std::uint32_t viewId,
	std::uint64_t rendererGeneration)
{
	if(frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	const std::uint32_t viewIndex = view == VulkanContext::ViewType::Game ? 0u : 1u;
	const auto markers = TerraEngine::Renderer::Vulkan::CreateVulkanVirtualGeometryStageMarkers();
	for(std::size_t index = 0u; index < markers.size(); ++index) {
		const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryStageMarker& marker = markers[index];
		VirtualGeometryGpuTimestampStageSample& sample =
			virtualGeometryGpuTimestampSamplesByFrame[frameIndex][viewIndex][index];
		sample = {};
		sample.stage = marker.stage;
		sample.route = marker.route;
		sample.availability = marker.availability;
		sample.frameNumber = frameNumber;
		sample.viewId = viewId;
		sample.rendererGeneration = rendererGeneration;
		sample.reason = marker.reason;
	}
}

// VulkanContext::Impl::MarkVirtualGeometryTimestampBegin reserves a dedicated
// pair of queries for a stage only when an executor is ready to record that
// stage's live command. This keeps CPU packet upload and CPU-expanded draws
// truthful while allowing the begin timestamp to surround the dispatch.
void VulkanContext::Impl::MarkVirtualGeometryTimestampBegin(
	VkCommandBuffer cmd,
	uint32_t frameIndex,
	VulkanContext::ViewType view,
	TerraEngine::Renderer::VirtualGeometryBackendStage stage,
	TerraEngine::Renderer::VirtualGeometryBackendRoute route,
	std::uint64_t geometryGeneration,
	std::uint64_t residencyGeneration,
	std::uint64_t commandGeneration)
{
	const std::uint32_t stageIndex = GetVirtualGeometryTimestampStageIndex(stage);
	if(!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| cmd == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT
		|| stageIndex >= TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount) {
		return;
	}

	const std::uint32_t viewIndex = view == VulkanContext::ViewType::Game ? 0u : 1u;
	VirtualGeometryGpuTimestampStageSample& sample =
		virtualGeometryGpuTimestampSamplesByFrame[frameIndex][viewIndex][stageIndex];
	if(sample.frameNumber == 0u
		|| sample.rendererGeneration == 0u
		|| geometryGeneration == 0u
		|| commandGeneration == 0u) {
		return;
	}

	const std::uint32_t firstQuery =
		frameIndex * kTotalGpuTimestampQueriesPerFrame
		+ kGpuTimestampQueriesPerFrame
		+ viewIndex * static_cast<std::uint32_t>(TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount)
			* kVirtualGeometryTimestampQueriesPerStage
		+ stageIndex * kVirtualGeometryTimestampQueriesPerStage;
	// Reset on reuse so a completed value from an earlier frame slot cannot be
	// mistaken for this command's delayed result.
	vkCmdResetQueryPool(cmd, gpuTimestampQueryPool, firstQuery, kVirtualGeometryTimestampQueriesPerStage);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpuTimestampQueryPool, firstQuery);
	sample.route = route;
	sample.geometryGeneration = geometryGeneration;
	sample.residencyGeneration = residencyGeneration;
	sample.commandGeneration = commandGeneration;
	sample.availability = TerraEngine::Renderer::BackendMeasurementAvailability::Pending;
	sample.firstQuery = firstQuery;
	sample.timestampWritesRecorded = false;
	sample.reason = "The live Vulkan virtual-geometry command is waiting for its end timestamp and delayed readback.";
}

// VulkanContext::Impl::MarkVirtualGeometryTimestampEnd closes the dedicated
// stage interval. Query results are read only after the owning frame-slot fence.
void VulkanContext::Impl::MarkVirtualGeometryTimestampEnd(
	VkCommandBuffer cmd,
	uint32_t frameIndex,
	VulkanContext::ViewType view,
	TerraEngine::Renderer::VirtualGeometryBackendStage stage)
{
	const std::uint32_t stageIndex = GetVirtualGeometryTimestampStageIndex(stage);
	if(!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| cmd == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT
		|| stageIndex >= TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount) {
		return;
	}

	const std::uint32_t viewIndex = view == VulkanContext::ViewType::Game ? 0u : 1u;
	VirtualGeometryGpuTimestampStageSample& sample =
		virtualGeometryGpuTimestampSamplesByFrame[frameIndex][viewIndex][stageIndex];
	if(sample.availability != TerraEngine::Renderer::BackendMeasurementAvailability::Pending) {
		return;
	}
	vkCmdWriteTimestamp(
		cmd,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		gpuTimestampQueryPool,
		sample.firstQuery + 1u);
	sample.timestampWritesRecorded = true;
	sample.reason = "The live Vulkan virtual-geometry timestamp interval is pending delayed readback.";
}

// VulkanContext::Impl::ResolveCompletedGpuTimestamps is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::ResolveCompletedGpuTimestamps(uint32_t frameIndex) {
	if (!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	struct TimestampQueryResult
	{
		std::uint64_t value = 0u;
		std::uint64_t available = 0u;
	};

	for (uint32_t viewIndex = 0u; viewIndex < kStatsViewCount; ++viewIndex) {
		GpuTimestampViewSample& sample = completedGpuTimestampSamplesByFrame[frameIndex][viewIndex];
		if (!pendingGpuTimestampWritesByFrame[frameIndex][viewIndex]) {
			sample = {};
			sample.reason = "No GPU timestamp query was written for this view in the completed frame slot.";
			continue;
		}

		std::array<TimestampQueryResult, kGpuTimestampQueriesPerView> queryResults{};
		const uint32_t firstQuery = frameIndex * kTotalGpuTimestampQueriesPerFrame + viewIndex * kGpuTimestampQueriesPerView;
		const VkResult result = vkGetQueryPoolResults(
			device,
			gpuTimestampQueryPool,
			firstQuery,
			kGpuTimestampQueriesPerView,
			sizeof(TimestampQueryResult) * queryResults.size(),
			queryResults.data(),
			sizeof(TimestampQueryResult),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

		pendingGpuTimestampWritesByFrame[frameIndex][viewIndex] = false;
		if (result != VK_SUCCESS
			|| queryResults[0].available == 0u
			|| queryResults[1].available == 0u
			|| queryResults[1].value < queryResults[0].value) {
			sample = {};
			sample.frameId = pendingGpuTimestampFrameIdsByFrame[frameIndex][viewIndex];
			sample.reason = "GPU timestamp query result was not available after the frame-slot fence.";
			continue;
		}

		const std::uint64_t tickDelta = queryResults[1].value - queryResults[0].value;
		const double milliseconds = static_cast<double>(tickDelta) * static_cast<double>(gpuTimestampPeriodNanoseconds) / 1000000.0;
		sample.valid = true;
		sample.milliseconds = static_cast<float>(std::max(0.0, milliseconds));
		sample.frameId = pendingGpuTimestampFrameIdsByFrame[frameIndex][viewIndex];
		sample.latencyFrames = 1u;
		sample.reason.clear();
	}
}

// VulkanContext::Impl::ResolveCompletedVirtualGeometryTimestamps publishes only
// after the frame-slot fence. Unavailable rows keep no numeric duration, while
// measured rows retain the pool generation and raw ticks needed for validation.
void VulkanContext::Impl::ResolveCompletedVirtualGeometryTimestamps(uint32_t frameIndex)
{
	if(frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	struct TimestampQueryResult
	{
		std::uint64_t value = 0u;
		std::uint64_t available = 0u;
	};

	for(uint32_t viewIndex = 0u; viewIndex < kStatsViewCount; ++viewIndex) {
		for(VirtualGeometryGpuTimestampStageSample& sample :
			virtualGeometryGpuTimestampSamplesByFrame[frameIndex][viewIndex]) {
			if(sample.published || sample.frameNumber == 0u) {
				continue;
			}

			TerraEngine::Renderer::VirtualGeometryBackendMeasurement measurement{};
			measurement.stage = sample.stage;
			measurement.frameNumber = sample.frameNumber;
			measurement.viewId = sample.viewId;
			measurement.rendererGeneration = sample.rendererGeneration;
			measurement.firstQuery = sample.firstQuery;
			measurement.queryCount = sample.timestampWritesRecorded
				? kVirtualGeometryTimestampQueriesPerStage
				: 0u;
			measurement.availability = sample.availability;
			measurement.reason = sample.reason;

			if(sample.availability == TerraEngine::Renderer::BackendMeasurementAvailability::Pending) {
				std::array<TimestampQueryResult, kVirtualGeometryTimestampQueriesPerStage> queryResults{};
				const VkResult result = vkGetQueryPoolResults(
					device,
					gpuTimestampQueryPool,
					sample.firstQuery,
					kVirtualGeometryTimestampQueriesPerStage,
					sizeof(TimestampQueryResult) * queryResults.size(),
					queryResults.data(),
					sizeof(TimestampQueryResult),
					VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

				if(!sample.timestampWritesRecorded
					|| result != VK_SUCCESS
					|| queryResults[0].available == 0u
					|| queryResults[1].available == 0u
					|| queryResults[1].value < queryResults[0].value) {
					measurement.availability = TerraEngine::Renderer::BackendMeasurementAvailability::Invalid;
					measurement.queryCount = 0u;
					measurement.reason = "The completed Vulkan virtual-geometry timestamp interval was unavailable or invalid.";
				}
				else {
					const std::uint64_t rawTicks = queryResults[1].value - queryResults[0].value;
					measurement.queryPoolGeneration = gpuTimestampQueryPoolGeneration;
					measurement.rawTicks = rawTicks;
					measurement.durationMilliseconds =
						static_cast<double>(rawTicks) * static_cast<double>(gpuTimestampPeriodNanoseconds) / 1000000.0;
					measurement.availability = TerraEngine::Renderer::BackendMeasurementAvailability::Measured;
					measurement.reason.clear();

					// A receipt is stronger than timing alone: the exact live command
					// recorded both marker boundaries, submitted with this frame slot,
					// and became visible only after the slot fence completed. The
					// command serial acts as the fence identity across handle reuse.
					TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt receipt{};
					receipt.stage = sample.stage;
					receipt.route = sample.route;
					receipt.frameNumber = sample.frameNumber;
					receipt.viewId = sample.viewId;
					receipt.rendererGeneration = sample.rendererGeneration;
					receipt.geometryGeneration = sample.geometryGeneration;
					receipt.residencyGeneration = sample.residencyGeneration;
					receipt.commandGeneration = sample.commandGeneration;
					receipt.completionFence = sample.frameNumber;
					receipt.commandRecorded = true;
					receipt.commandSubmitted = true;
					receipt.completionObserved = true;
					receipt.generationMatched = true;
					receipt.evidenceId = "vulkan.timestamp."
						+ std::to_string(gpuTimestampQueryPoolGeneration)
						+ "."
						+ std::to_string(sample.firstQuery)
						+ "."
						+ std::to_string(sample.frameNumber);
					if(receipt.IsExecuted()) {
						const std::uint32_t stageIndex = GetVirtualGeometryTimestampStageIndex(receipt.stage);
						if(stageIndex < completedVirtualGeometryStageEvidence.size()) {
							completedVirtualGeometryStageEvidence[stageIndex] = true;
						}
						if(receipt.stage == TerraEngine::Renderer::VirtualGeometryBackendStage::IndirectGeneration
							&& receipt.commandGeneration == latestVirtualGeometryExecutorPlan.commandGeneration
							&& latestVirtualGeometryExecutorPlan.boundedPacketCount != 0u) {
							// A non-empty indirect receipt is the earliest point at which
							// this frame's GPU-authored commands may be advertised.
							latestVirtualGeometryExecutorPlan.generatedSubmissionExecutionAvailable = true;
						}
						completedVirtualGeometryExecutionReceipts.push_back(std::move(receipt));
					}
				}
			}

			completedVirtualGeometryMeasurements.push_back(std::move(measurement));
			sample.published = true;
		}
	}
}

// VulkanContext::Impl::RecordPendingStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingStats(uint32_t statsSlotIndex, const RenderStatsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingStatsBySlot[statsSlotIndex] = snapshot;
}

// VulkanContext::Impl::RecordPendingBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingBudgetDiagnostics(uint32_t statsSlotIndex, const FrameBudgetDiagnosticsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingBudgetDiagnosticsBySlot[statsSlotIndex] = snapshot;
}

// VulkanContext::Impl::RecordPendingLightAnalysisCandidates is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingLightAnalysisCandidates(uint32_t statsSlotIndex, const std::vector<FrameLightCandidate>& candidates) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingLightAnalysisCandidatesBySlot[statsSlotIndex] = candidates;
}

// VulkanContext::Impl::PublishCompletedFrameStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::PublishCompletedFrameStats(uint32_t frameIndex) {
	ResolveCompletedGpuTimestamps(frameIndex);
	ResolveCompletedVirtualGeometryTimestamps(frameIndex);

	TerraEngine::Renderer::BackendResourceResidencySnapshot backendResidency{};
	backendResidency.frameId = frameIndex;
	backendResidency.records.reserve(meshCache.size() + textureCache.size());
	for (const auto& [meshId, mesh] : meshCache)
	{
		const VkDeviceSize gpuBytes = TotalMeshGpuBytes(mesh);
		if (gpuBytes == 0u)
		{
			continue;
		}

		TerraEngine::Renderer::BackendResourceResidencyRecord record{};
		record.assetId = meshId;
		record.kind = TerraEngine::Renderer::BackendResourceResidencyKind::Mesh;
		record.gpuBytes = static_cast<std::uint64_t>(gpuBytes);
		record.exactGpuBytes = static_cast<std::uint64_t>(gpuBytes);
		record.allocationId = static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId));
		record.byteStatus = "exact";
		record.retirementState = "published";
		record.subresourceCount = 8u;
		record.detail = "vulkan mesh cache";
		backendResidency.records.push_back(std::move(record));
	}
	if (frameIndex < virtualGeometryGpuPacketResourcesByFrame.size())
	{
		for (const VirtualGeometryGpuPacketFrameResources& resources : virtualGeometryGpuPacketResourcesByFrame[frameIndex])
		{
			if (!resources.live || resources.pagePayloadBytesByMesh.empty())
			{
				continue;
			}
			for (const auto& [meshId, gpuBytes] : resources.pagePayloadBytesByMesh)
			{
				if (meshId == TerraEngine::EMPTY_UUID || gpuBytes == 0u)
				{
					continue;
				}

				TerraEngine::Renderer::BackendResourceResidencyRecord record{};
				record.assetId = meshId;
				record.kind = TerraEngine::Renderer::BackendResourceResidencyKind::Mesh;
				record.gpuBytes = gpuBytes;
				record.exactGpuBytes = gpuBytes;
				record.allocationId = static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId))
					^ (resources.resourceSerial + 0x9e3779b97f4a7c15ull);
				record.byteStatus = "exact";
				record.retirementState = "published";
				record.subresourceCount = 1u;
				// Page-local VG geometry is packed into a per-view physical
				// payload buffer instead of the owner mesh cache. Publishing a
				// second asset-owned record keeps Resource Residency exact while
				// still allowing the renderer to batch those page bytes together.
				record.detail = "vulkan virtual geometry page-local payload";
				backendResidency.records.push_back(std::move(record));
			}
		}
	}
	for (const auto& [textureId, texture] : textureCache)
	{
		if (texture.size == 0u)
		{
			continue;
		}

		TerraEngine::Renderer::BackendResourceResidencyRecord record{};
		record.assetId = textureId;
		record.kind = TerraEngine::Renderer::BackendResourceResidencyKind::Texture;
		record.gpuBytes = static_cast<std::uint64_t>(texture.size);
		record.exactGpuBytes = static_cast<std::uint64_t>(texture.size);
		record.allocationId = static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(textureId));
		record.byteStatus = "exact";
		record.retirementState = "published";
		record.subresourceCount = texture.residentMipCount;
		record.detail = "vulkan texture cache";
		backendResidency.records.push_back(std::move(record));
	}
	TerraEngine::Renderer::PublishBackendResourceResidencySnapshot(std::move(backendResidency));

	const uint32_t gameSlot = GetStatsSlotIndex(frameIndex, VulkanContext::ViewType::Game);
	const uint32_t sceneSlot = GetStatsSlotIndex(frameIndex, VulkanContext::ViewType::Scene);
	const auto hasReadableCompletedHiZ = [](const VulkanHiZ& hiZ, const RenderStatsSnapshot& stats) {
		const bool sizeMatchesStats =
			stats.renderSize.x == 0u
			|| stats.renderSize.y == 0u
			|| (hiZ.width == stats.renderSize.x && hiZ.height == stats.renderSize.y);
		return hiZ.image != VK_NULL_HANDLE
			&& hiZ.fullView != VK_NULL_HANDLE
			&& hiZ.sampler != VK_NULL_HANDLE
			&& hiZ.mipLevels > 0u
			&& hiZ.layoutInitialized
			&& hiZ.history.valid
			&& sizeMatchesStats;
	};

	RenderStatsSnapshot gameSnapshot = pendingStatsBySlot[gameSlot];
	const GpuTimestampViewSample gameGpuTimestamp = completedGpuTimestampSamplesByFrame[std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1))][0u];
	const MeshletCullingCountersGPU gameCounters = ReadMeshletCounters(gameSlot);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (gameSnapshot.valid) {
		gameSnapshot.gpuTimestampValid = gameGpuTimestamp.valid;
		gameSnapshot.gpuTimestampMs = gameGpuTimestamp.milliseconds;
		gameSnapshot.gpuTimestampFrameId = gameGpuTimestamp.frameId;
		gameSnapshot.gpuTimestampLatencyFrames = gameGpuTimestamp.latencyFrames;
		gameSnapshot.gpuTimestampReason = gameGpuTimestamp.reason;
		const bool useGpuCounters = (gameCounters.meshletsTotal > 0u) || (gameSnapshot.meshletsTotal == 0u);
		if (useGpuCounters) {
			gameSnapshot.meshletsTotal = gameCounters.meshletsTotal;
			gameSnapshot.meshletsDispatched = gameCounters.meshletsDispatched;
			gameSnapshot.meshletsCulled = gameCounters.meshletsCulled;
			gameSnapshot.meshletsCulledHiZ = gameCounters.meshletsCulledHiZ;
			gameSnapshot.hiZTests = gameCounters.hiZTests;
			gameSnapshot.hiZProjectionFailed = gameCounters.hiZProjectionFailed;
			gameSnapshot.hiZFullRectCulled = gameCounters.hiZFullRectCulled;
			gameSnapshot.hiZRectCoverageCulled = gameCounters.hiZRectCoverageCulled;
			gameSnapshot.hiZCenterCulled = gameCounters.hiZCenterCulled;
			gameSnapshot.hiZVertexCoverageCulled = gameCounters.hiZVertexCoverageCulled;
			gameSnapshot.hiZNoOcclusionProof = gameCounters.hiZNoOcclusionProof;
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::GPUReadback;
			gameSnapshot.meshletCounterFrameId = frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = 0u;
			gameSnapshot.meshletCounterReason.clear();
		} else if (gameSnapshot.taskCullingActive) {
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::DelayedGPUReadback;
			gameSnapshot.meshletCounterFrameId = frameIndex > 0u ? frameIndex - 1u : frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = frameIndex > 0u ? 1u : 0u;
			gameSnapshot.meshletCounterReason = "GPU meshlet counter readback is delayed; showing prepared counts.";
		} else {
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::Prepared;
			gameSnapshot.meshletCounterFrameId = frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = 0u;
			gameSnapshot.meshletCounterReason = "Task culling is inactive; counts are prepared CPU estimates.";
		}
		gameSnapshot.hiZActive = gameSnapshot.hiZActive && hasReadableCompletedHiZ(HiZForFrame(kHiZGameViewIndex, frameIndex), gameSnapshot);
		gameSnapshot.vrsActive = gameSnapshot.vrsActive && UsesVrsAttachment();
	}

	RenderStatsSnapshot sceneSnapshot = pendingStatsBySlot[sceneSlot];
	const GpuTimestampViewSample sceneGpuTimestamp = completedGpuTimestampSamplesByFrame[std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1))][1u];
	const MeshletCullingCountersGPU sceneCounters = ReadMeshletCounters(sceneSlot);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (sceneSnapshot.valid) {
		sceneSnapshot.gpuTimestampValid = sceneGpuTimestamp.valid;
		sceneSnapshot.gpuTimestampMs = sceneGpuTimestamp.milliseconds;
		sceneSnapshot.gpuTimestampFrameId = sceneGpuTimestamp.frameId;
		sceneSnapshot.gpuTimestampLatencyFrames = sceneGpuTimestamp.latencyFrames;
		sceneSnapshot.gpuTimestampReason = sceneGpuTimestamp.reason;
		const bool useGpuCounters = (sceneCounters.meshletsTotal > 0u) || (sceneSnapshot.meshletsTotal == 0u);
		if (useGpuCounters) {
			sceneSnapshot.meshletsTotal = sceneCounters.meshletsTotal;
			sceneSnapshot.meshletsDispatched = sceneCounters.meshletsDispatched;
			sceneSnapshot.meshletsCulled = sceneCounters.meshletsCulled;
			sceneSnapshot.meshletsCulledHiZ = sceneCounters.meshletsCulledHiZ;
			sceneSnapshot.hiZTests = sceneCounters.hiZTests;
			sceneSnapshot.hiZProjectionFailed = sceneCounters.hiZProjectionFailed;
			sceneSnapshot.hiZFullRectCulled = sceneCounters.hiZFullRectCulled;
			sceneSnapshot.hiZRectCoverageCulled = sceneCounters.hiZRectCoverageCulled;
			sceneSnapshot.hiZCenterCulled = sceneCounters.hiZCenterCulled;
			sceneSnapshot.hiZVertexCoverageCulled = sceneCounters.hiZVertexCoverageCulled;
			sceneSnapshot.hiZNoOcclusionProof = sceneCounters.hiZNoOcclusionProof;
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::GPUReadback;
			sceneSnapshot.meshletCounterFrameId = frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = 0u;
			sceneSnapshot.meshletCounterReason.clear();
		} else if (sceneSnapshot.taskCullingActive) {
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::DelayedGPUReadback;
			sceneSnapshot.meshletCounterFrameId = frameIndex > 0u ? frameIndex - 1u : frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = frameIndex > 0u ? 1u : 0u;
			sceneSnapshot.meshletCounterReason = "GPU meshlet counter readback is delayed; showing prepared counts.";
		} else {
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::Prepared;
			sceneSnapshot.meshletCounterFrameId = frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = 0u;
			sceneSnapshot.meshletCounterReason = "Task culling is inactive; counts are prepared CPU estimates.";
		}
		sceneSnapshot.hiZActive = sceneSnapshot.hiZActive && hasReadableCompletedHiZ(HiZForFrame(kHiZSceneViewIndex, frameIndex), sceneSnapshot);
		sceneSnapshot.vrsActive = sceneSnapshot.vrsActive && UsesVrsAttachment();
	}

	const FrameBudgetDiagnosticsSnapshot gameBudgetDiagnostics = pendingBudgetDiagnosticsBySlot[gameSlot];
	const FrameBudgetDiagnosticsSnapshot sceneBudgetDiagnostics = pendingBudgetDiagnosticsBySlot[sceneSlot];
	const auto buildLightingDiagnostics = [this](const FrameBudgetDiagnosticsSnapshot& budgetDiagnostics) {
		LightingDiagnosticsSnapshot snapshot{};
		snapshot.viewType = budgetDiagnostics.viewType;
		snapshot.valid = budgetDiagnostics.valid;
		snapshot.indirectLightingActive = true;
		const bool gtaoAvailable = ambientOcclusionBlurredByFrame[0].image != VK_NULL_HANDLE
			&& ambientOcclusionBlurredByFrame[0].view != VK_NULL_HANDLE
			&& gtaoComputePipeline != VK_NULL_HANDLE
			&& gtaoBlurComputePipeline != VK_NULL_HANDLE;
		snapshot.ambientOcclusionActive = gtaoAvailable;

		LightingDiagnosticEntry shadowEntry{};
		shadowEntry.category = LightingDiagnosticCategory::Shadow;
		shadowEntry.status = budgetDiagnostics.shadowReductionOccurred
			? LightingDiagnosticStatus::Degraded
			: LightingDiagnosticStatus::Active;
		shadowEntry.message = budgetDiagnostics.shadowReductionOccurred
			? "Some shadow-capable retained lights did not receive shadow resources this frame."
			: "Shadow-capable retained lights are within the active shadow budget.";
		shadowEntry.recommendedAction = budgetDiagnostics.shadowReductionOccurred
			? "Increase the shadow light budget, lower shadow demand, or raise important lights' priority."
			: "";
		snapshot.entries.push_back(std::move(shadowEntry));

		LightingDiagnosticEntry shadowMemoryEntry{};
		shadowMemoryEntry.category = LightingDiagnosticCategory::Shadow;
		shadowMemoryEntry.status = budgetDiagnostics.allocationFailureCount > 0u
			? LightingDiagnosticStatus::Unavailable
			: LightingDiagnosticStatus::Active;
		shadowMemoryEntry.message =
			"Shadow storage allocated "
			+ std::to_string(budgetDiagnostics.allocatedRegionCount)
			+ " active regions for "
			+ std::to_string(budgetDiagnostics.activeShadowCount)
			+ " shadowed lights.";
		shadowMemoryEntry.recommendedAction = budgetDiagnostics.allocationFailureCount > 0u
			? "Reduce shadow demand or inspect Vulkan allocation failures."
			: "";
		snapshot.entries.push_back(std::move(shadowMemoryEntry));

		const auto shadowKindName = [](ShadowResourceKind kind) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (kind)
			{
			case ShadowResourceKind::DirectionalCascade:
				return "directional cascade";
			case ShadowResourceKind::PointCube:
				return "point cubemap";
			case ShadowResourceKind::SpotAtlas:
				return "spot atlas";
			case ShadowResourceKind::AreaAtlas:
				return "area atlas";
			default:
				return "unsupported shadow resource";
			}
		};

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ShadowResourceAssignmentDiagnostic& assignment : budgetDiagnostics.shadowAssignments)
		{
			LightingDiagnosticEntry assignmentEntry{};
			assignmentEntry.category = LightingDiagnosticCategory::Shadow;
			assignmentEntry.status = assignment.assigned
				? LightingDiagnosticStatus::Active
				: LightingDiagnosticStatus::Degraded;
			assignmentEntry.lightId = assignment.lightId;
			assignmentEntry.message = assignment.assigned
				? std::string("Light received ") + shadowKindName(assignment.resourceKind) + " slot " + std::to_string(assignment.resourceSlot) + "."
				: std::string("Light requested shadows but did not receive a ") + shadowKindName(assignment.resourceKind) + " slot this frame.";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assignment.reusedPreviousOwner)
			{
				assignmentEntry.message += " Reused previous assignment for stability.";
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assignment.replacedPreviousOwner)
			{
				assignmentEntry.message += " Replaced the previous owner of this slot.";
			}
			assignmentEntry.recommendedAction = assignment.assigned
				? ""
				: "Increase maxShadowLights or raise this light's authored priority.";
			snapshot.entries.push_back(std::move(assignmentEntry));
		}

		LightingDiagnosticEntry indirectEntry{};
		indirectEntry.category = LightingDiagnosticCategory::IndirectLighting;
		indirectEntry.status = LightingDiagnosticStatus::Active;
		indirectEntry.message = "Indirect lighting is evaluated separately from hard direct shadow visibility.";
		snapshot.entries.push_back(std::move(indirectEntry));

		LightingDiagnosticEntry aoEntry{};
		aoEntry.category = LightingDiagnosticCategory::AmbientOcclusion;
		aoEntry.status = gtaoAvailable
			? LightingDiagnosticStatus::Active
			: LightingDiagnosticStatus::Fallback;
		aoEntry.message = gtaoAvailable
			? "GTAO contact darkening is generated before material shading and applied to indirect lighting."
			: "Contact darkening uses material occlusion until the GTAO compute pass is available.";
		aoEntry.recommendedAction = gtaoAvailable
			? ""
			: "Check GTAO shader packaging and descriptor creation if screen-space contact darkening should be active.";
		snapshot.entries.push_back(std::move(aoEntry));

		LightingDiagnosticEntry materialEntry{};
		materialEntry.category = LightingDiagnosticCategory::MaterialShadow;
		materialEntry.status = LightingDiagnosticStatus::Fallback;
		materialEntry.message = "Opaque and alpha-cutout materials cast shadows; blended transparent materials currently do not cast shadows.";
		materialEntry.recommendedAction = "Use Mask alpha mode for cutout objects that must cast silhouette shadows.";
		snapshot.entries.push_back(std::move(materialEntry));

		LightingDiagnosticEntry budgetEntry{};
		budgetEntry.category = LightingDiagnosticCategory::Budget;
		budgetEntry.status = budgetDiagnostics.budgetReductionOccurred
			? LightingDiagnosticStatus::Degraded
			: LightingDiagnosticStatus::Active;
		budgetEntry.message = budgetDiagnostics.budgetReductionOccurred
			? "The scene exceeded one or more active lighting budgets."
			: "The scene is within active lighting budgets.";
		budgetEntry.recommendedAction = budgetDiagnostics.budgetReductionOccurred
			? "Use quality settings or light priorities to control which lights are retained."
			: "";
		snapshot.entries.push_back(std::move(budgetEntry));
		return snapshot;
	};
	const LightingDiagnosticsSnapshot gameLightingDiagnostics = buildLightingDiagnostics(gameBudgetDiagnostics);
	const LightingDiagnosticsSnapshot sceneLightingDiagnostics = buildLightingDiagnostics(sceneBudgetDiagnostics);

	GpuLightAnalysisViewSnapshot gameLightAnalysis{};
	GpuLightAnalysisViewSnapshot sceneLightAnalysis{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex < lightAnalysisSSBOs.size() && lightAnalysisSSBOs[frameIndex].buffer != VK_NULL_HANDLE) {
		const VulkanBuffer& analysisBuffer = lightAnalysisSSBOs[frameIndex];
		void* mappedAnalysis = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaMapMemory(allocator, analysisBuffer.allocation, &mappedAnalysis) == VK_SUCCESS) {
			vmaInvalidateAllocation(allocator, analysisBuffer.allocation, 0, analysisBuffer.size);
			const auto* analysisRecords = static_cast<const GPULightAnalysisRecord*>(mappedAnalysis);
			const auto readSnapshot = [&](uint32_t statsSlotIndex) {
				GpuLightAnalysisViewSnapshot snapshot{};
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (statsSlotIndex >= pendingLightAnalysisCandidatesBySlot.size()) {
					return snapshot;
				}

				const std::vector<FrameLightCandidate>& candidates = pendingLightAnalysisCandidatesBySlot[statsSlotIndex];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (candidates.empty()) {
					snapshot.valid = true;
					return snapshot;
				}

				const std::size_t clampedCount = std::min<std::size_t>(candidates.size(), MAX_GLOBAL_LIGHTS);
				const std::size_t baseIndex = static_cast<std::size_t>(statsSlotIndex) * static_cast<std::size_t>(MAX_GLOBAL_LIGHTS);
				snapshot.recordsByLightId.reserve(clampedCount);
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (std::size_t lightIndex = 0; lightIndex < clampedCount; ++lightIndex) {
					const GPULightAnalysisRecord& record = analysisRecords[baseIndex + lightIndex];
					snapshot.recordsByLightId.emplace(candidates[lightIndex].lightId, record);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if ((record.flags & kGpuLightAnalysisFlagVisible) != 0u) {
						snapshot.visibleCandidateCount += 1u;
					}
				}
				snapshot.valid = true;
				return snapshot;
			};

			gameLightAnalysis = readSnapshot(gameSlot);
			sceneLightAnalysis = readSnapshot(sceneSlot);
			vmaUnmapMemory(allocator, analysisBuffer.allocation);
		}
	}

	ClusterValidationSnapshot clusterSnapshot{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterValidationCaptureEnabled) {
		clusterSnapshot = ReadCompletedClusterValidationSnapshot(frameIndex);
	}

	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestStatsByView[0] = gameSnapshot;
		latestStatsByView[1] = sceneSnapshot;
		latestBudgetDiagnosticsByView[0] = gameBudgetDiagnostics;
		latestBudgetDiagnosticsByView[1] = sceneBudgetDiagnostics;
		latestLightingDiagnosticsByView[0] = gameLightingDiagnostics;
		latestLightingDiagnosticsByView[1] = sceneLightingDiagnostics;
		latestGpuLightAnalysisByView[0] = std::move(gameLightAnalysis);
		latestGpuLightAnalysisByView[1] = std::move(sceneLightAnalysis);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterSnapshot.valid) {
			previousClusterValidationSnapshot = clusterSnapshot;
			previousClusterValidationSnapshotValid = true;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterSnapshot.valid) {
		const FrameBudgetDiagnosticsSnapshot& captureDiagnostics = (clusterSnapshot.viewType == RenderViewType::Scene)
			? sceneBudgetDiagnostics
			: gameBudgetDiagnostics;
		WriteClusterValidationCapture(clusterSnapshot, captureDiagnostics);
	}
}

// VulkanContext::Impl::GetLatestStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderStatsSnapshot VulkanContext::Impl::GetLatestStats(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestStatsByView[index];
}

// VulkanContext::Impl::GetLatestBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
FrameBudgetDiagnosticsSnapshot VulkanContext::Impl::GetLatestBudgetDiagnostics(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestBudgetDiagnosticsByView[index];
}

// VulkanContext::Impl::GetLatestLightingDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
LightingDiagnosticsSnapshot VulkanContext::Impl::GetLatestLightingDiagnostics(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestLightingDiagnosticsByView[index];
}

// VulkanContext::Impl::GetLatestGpuLightAnalysis is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VulkanContext::Impl::GpuLightAnalysisViewSnapshot VulkanContext::Impl::GetLatestGpuLightAnalysis(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestGpuLightAnalysisByView[index];
}

// VulkanContext::Impl::RecordPendingClusterValidationSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingClusterValidationSnapshot(uint32_t frameIndex, const ClusterValidationSnapshot& snapshot) {
	if (frameIndex >= pendingClusterValidationByFrame.size()) {
		return;
	}
	pendingClusterValidationByFrame[frameIndex] = snapshot;
}

// VulkanContext::Impl::ReadCompletedClusterValidationSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ClusterValidationSnapshot VulkanContext::Impl::ReadCompletedClusterValidationSnapshot(uint32_t frameIndex) const {
	if (!clusterValidationCaptureEnabled) {
		return {};
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex >= pendingClusterValidationByFrame.size()) {
		return {};
	}

	ClusterValidationSnapshot snapshot = pendingClusterValidationByFrame[frameIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!snapshot.valid || !snapshot.buildMetadata.clusteredLightingAvailable) {
		return snapshot;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex >= clusterGridSSBOs.size() || frameIndex >= lightIndexSSBOs.size()) {
		snapshot.valid = false;
		return snapshot;
	}

	const VulkanBuffer& clusterBuffer = clusterGridSSBOs[frameIndex];
	const VulkanBuffer& lightIndexBuffer = lightIndexSSBOs[frameIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterBuffer.buffer == VK_NULL_HANDLE || lightIndexBuffer.buffer == VK_NULL_HANDLE) {
		snapshot.valid = false;
		return snapshot;
	}

	const std::uint32_t clusterCount = snapshot.buildMetadata.clusterCount;
	snapshot.ranges.clear();
	snapshot.ranges.reserve(clusterCount);

	void* mappedClusters = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaMapMemory(allocator, clusterBuffer.allocation, &mappedClusters) != VK_SUCCESS) {
		snapshot.valid = false;
		return snapshot;
	}

	const VkDeviceSize clusterReadOffset = static_cast<VkDeviceSize>(snapshot.buildMetadata.clusterDataOffset) * sizeof(ClusterData);
	const VkDeviceSize clusterReadSize = static_cast<VkDeviceSize>(sizeof(ClusterData) * clusterCount);
	vmaInvalidateAllocation(allocator, clusterBuffer.allocation, clusterReadOffset, clusterReadSize);

	const ClusterData* clusterGridData = static_cast<const ClusterData*>(mappedClusters) + snapshot.buildMetadata.clusterDataOffset;
	std::uint64_t rangeHash = kClusterRangeHashOffset;
	std::uint32_t usedIndexCount = 0u;
	std::uint32_t actualAssignedLightCount = 0u;
	std::uint32_t emptyClusterCount = 0u;
	std::uint32_t maxClusterLightCount = 0u;
	std::uint32_t outOfBoundsRangeCount = 0u;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t clusterIndex = 0u; clusterIndex < clusterCount; ++clusterIndex) {
		const ClusterData& cluster = clusterGridData[clusterIndex];
		const std::uint64_t rangeEnd = static_cast<std::uint64_t>(cluster.offset) + static_cast<std::uint64_t>(cluster.count);
		const bool outOfBounds = rangeEnd > snapshot.buildMetadata.lightIndexCapacity
			|| rangeEnd > static_cast<std::uint64_t>(MAX_LIGHT_INDEX_ENTRIES);

		ClusterLightRangeSnapshot rangeSnapshot{};
		rangeSnapshot.clusterIndex = clusterIndex;
		rangeSnapshot.offset = cluster.offset;
		rangeSnapshot.count = cluster.count;
		rangeSnapshot.outOfBounds = outOfBounds;
		snapshot.ranges.push_back(rangeSnapshot);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (cluster.count == 0u) {
			emptyClusterCount += 1u;
		}
		maxClusterLightCount = std::max(maxClusterLightCount, cluster.count);
		actualAssignedLightCount += cluster.count;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (rangeEnd > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
			usedIndexCount = std::numeric_limits<std::uint32_t>::max();
		}
		else {
			usedIndexCount = std::max(usedIndexCount, static_cast<std::uint32_t>(rangeEnd));
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outOfBounds) {
			outOfBoundsRangeCount += 1u;
		}

		HashClusterRangeValue(rangeHash, clusterIndex);
		HashClusterRangeValue(rangeHash, cluster.offset);
		HashClusterRangeValue(rangeHash, cluster.count);
		HashClusterRangeValue(rangeHash, outOfBounds ? 1u : 0u);
	}

	vmaUnmapMemory(allocator, clusterBuffer.allocation);

	std::uint32_t invalidLightIndexCount = 0u;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (usedIndexCount > 0u) {
		void* mappedLightIndices = nullptr;
		if (vmaMapMemory(allocator, lightIndexBuffer.allocation, &mappedLightIndices) == VK_SUCCESS) {
			const VkDeviceSize lightIndexReadOffset = static_cast<VkDeviceSize>(snapshot.buildMetadata.lightIndexOffset) * sizeof(std::uint32_t);
			const VkDeviceSize lightIndexReadSize = static_cast<VkDeviceSize>(sizeof(std::uint32_t) * usedIndexCount);
			vmaInvalidateAllocation(allocator, lightIndexBuffer.allocation, lightIndexReadOffset, lightIndexReadSize);

			const std::uint32_t* lightIndexData = static_cast<const std::uint32_t*>(mappedLightIndices) + snapshot.buildMetadata.lightIndexOffset;
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::uint32_t entryIndex = 0u; entryIndex < usedIndexCount; ++entryIndex) {
				const std::uint32_t lightIndex = lightIndexData[entryIndex];
				if (lightIndex == 0xFFFFFFFFu || lightIndex >= snapshot.buildMetadata.activeLightCount) {
					invalidLightIndexCount += 1u;
				}
			}

			vmaUnmapMemory(allocator, lightIndexBuffer.allocation);
		}
	}

	std::uint32_t determinismMismatchCount = 0u;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (previousClusterValidationSnapshotValid
		&& previousClusterValidationSnapshot.viewType == snapshot.viewType
		&& previousClusterValidationSnapshot.ranges.size() == snapshot.ranges.size()
		&& previousClusterValidationSnapshot.buildMetadata.activeLightCount == snapshot.buildMetadata.activeLightCount) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t rangeIndex = 0; rangeIndex < snapshot.ranges.size(); ++rangeIndex) {
			const ClusterLightRangeSnapshot& currentRange = snapshot.ranges[rangeIndex];
			const ClusterLightRangeSnapshot& previousRange = previousClusterValidationSnapshot.ranges[rangeIndex];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (currentRange.offset != previousRange.offset
				|| currentRange.count != previousRange.count
				|| currentRange.outOfBounds != previousRange.outOfBounds) {
				determinismMismatchCount += 1u;
			}
		}
	}

	snapshot.buildMetadata.usedIndexCount = usedIndexCount;
	snapshot.buildMetadata.actualAssignedLightCount = actualAssignedLightCount;
	snapshot.buildMetadata.emptyClusterCount = emptyClusterCount;
	snapshot.buildMetadata.maxClusterLightCount = maxClusterLightCount;
	snapshot.buildMetadata.outOfBoundsRangeCount = outOfBoundsRangeCount;
	snapshot.buildMetadata.invalidLightIndexCount = invalidLightIndexCount;
	snapshot.buildMetadata.overflowClusterCount = outOfBoundsRangeCount;
	snapshot.buildMetadata.determinismMismatchCount = determinismMismatchCount;
	snapshot.buildMetadata.rangeHash = rangeHash;
	snapshot.buildMetadata.overflowDetected = (outOfBoundsRangeCount > 0u) || (invalidLightIndexCount > 0u);
	return snapshot;
}

// VulkanContext::Impl::WriteClusterValidationCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::WriteClusterValidationCapture(const ClusterValidationSnapshot& snapshot, const FrameBudgetDiagnosticsSnapshot& diagnostics) const {
	if (!clusterValidationCaptureEnabled || !snapshot.valid) {
		return;
	}

	try {
		namespace fs = std::filesystem;
		const fs::path outputPath = clusterValidationCapturePath;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outputPath.has_parent_path()) {
			fs::create_directories(outputPath.parent_path());
		}

		const double emptyClusterPercent = snapshot.buildMetadata.clusterCount > 0u
			? (static_cast<double>(snapshot.buildMetadata.emptyClusterCount) * 100.0 / static_cast<double>(snapshot.buildMetadata.clusterCount))
			: 0.0;
		const double logicalIndexOverheadPercent = snapshot.buildMetadata.actualAssignedLightCount > 0u
			? (static_cast<double>(snapshot.buildMetadata.usedIndexCount) * 100.0 / static_cast<double>(snapshot.buildMetadata.actualAssignedLightCount))
			: 0.0;

		nlohmann::json capture;
		capture["view_type"] = RenderViewTypeToString(snapshot.viewType);
		capture["valid"] = snapshot.valid;
		capture["clustered_lighting_available"] = snapshot.buildMetadata.clusteredLightingAvailable;
		capture["overflow_detected"] = snapshot.buildMetadata.overflowDetected;
		capture["active_preset_name"] = QualityPresetToString(diagnostics.activePreset);
		capture["reduction_category"] = LightingReductionCategoryToString(diagnostics.reductionCategory);
		capture["gpu_assist_active"] = diagnostics.gpuAssistActive;
		capture["custom_override_active"] = diagnostics.customOverrideActive;
		capture["uploaded_light_budget"] = diagnostics.uploadedLightBudget;
		capture["retained_light_budget"] = diagnostics.retainedLightBudget;
		capture["shadow_light_budget"] = diagnostics.shadowLightBudget;
		capture["special_light_budget"] = diagnostics.specialLightBudget;
		capture["candidate_light_count"] = diagnostics.candidateLightCount;
		capture["uploaded_light_count"] = diagnostics.uploadedLightCount;
		capture["retained_light_count"] = diagnostics.retainedLightCount;
		capture["special_light_count"] = diagnostics.specialLightCount;
		capture["shadow_eligible_light_count"] = diagnostics.shadowEligibleLightCount;
		capture["shadowed_light_count"] = diagnostics.shadowedLightCount;
		capture["gpu_visible_candidate_count"] = diagnostics.gpuVisibleCandidateCount;
		capture["retained_directional_light_count"] = diagnostics.retainedDirectionalLightCount;
		capture["retained_authored_priority_light_count"] = diagnostics.retainedAuthoredPriorityLightCount;
		capture["budget_reduction_occurred"] = diagnostics.budgetReductionOccurred;
		capture["uploaded_reduction_occurred"] = diagnostics.uploadedReductionOccurred;
		capture["special_reduction_occurred"] = diagnostics.specialReductionOccurred;
		capture["shadow_reduction_occurred"] = diagnostics.shadowReductionOccurred;
		capture["retained_light_churn_count"] = diagnostics.retainedLightChurnCount;
		capture["retained_light_replacement_count"] = diagnostics.retainedLightReplacementCount;
		capture["stability_bias_applied_count"] = diagnostics.stabilityBiasAppliedCount;
		capture["shadow_reuse_count"] = diagnostics.shadowReuseCount;
		capture["shadow_replacement_count"] = diagnostics.shadowReplacementCount;
		capture["configured_shadow_capacity"] = diagnostics.configuredShadowCapacity;
		capture["active_shadow_count"] = diagnostics.activeShadowCount;
		capture["allocated_shadow_region_count"] = diagnostics.allocatedRegionCount;
		capture["allocated_shadow_2d_region_count"] = diagnostics.allocated2DRegionCount;
		capture["allocated_shadow_point_region_count"] = diagnostics.allocatedPointRegionCount;
		capture["allocated_shadow_directional_region_count"] = diagnostics.allocatedDirectionalRegionCount;
		capture["allocated_shadow_spot_region_count"] = diagnostics.allocatedSpotRegionCount;
		capture["allocated_shadow_area_region_count"] = diagnostics.allocatedAreaRegionCount;
		capture["estimated_shadow_memory_bytes"] = diagnostics.estimatedShadowMemoryBytes;
		capture["shadow_cache_reuse_count"] = diagnostics.cacheReuseCount;
		capture["shadow_cache_refresh_count"] = diagnostics.cacheRefreshCount;
		capture["shadow_cache_invalidation_count"] = diagnostics.cacheInvalidationCount;
		capture["degraded_shadow_count"] = diagnostics.degradedShadowCount;
		capture["shadow_allocation_failure_count"] = diagnostics.allocationFailureCount;
		capture["camera_visible_shadow_caster_count"] = diagnostics.cameraVisibleShadowCasterCount;
		capture["light_visible_shadow_caster_count"] = diagnostics.lightVisibleShadowCasterCount;
		capture["shadow_draw_submission_count"] = diagnostics.shadowDrawSubmissionCount;
		capture["shadow_pass_recorded"] = diagnostics.shadowPassRecorded;
		capture["shadow_assignments"] = nlohmann::json::array();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ShadowResourceAssignmentDiagnostic& assignment : diagnostics.shadowAssignments) {
			nlohmann::json assignmentJson;
			assignmentJson["light_id"] = TerraEngine::ToHexString(assignment.lightId);
			assignmentJson["resource_kind"] = ShadowResourceKindToString(assignment.resourceKind);
			assignmentJson["resource_slot"] = assignment.resourceSlot;
			assignmentJson["storage_region_id"] = assignment.storageRegionId;
			assignmentJson["atlas_id"] = assignment.atlasId;
			assignmentJson["region_x"] = assignment.regionX;
			assignmentJson["region_y"] = assignment.regionY;
			assignmentJson["region_width"] = assignment.regionWidth;
			assignmentJson["region_height"] = assignment.regionHeight;
			assignmentJson["estimated_bytes"] = assignment.estimatedBytes;
			assignmentJson["frame_serial"] = assignment.frameSerial;
			assignmentJson["frames_since_last_seen"] = assignment.framesSinceLastSeen;
			assignmentJson["retention_score"] = assignment.retentionScore;
			assignmentJson["quality_tier"] = ShadowQualityTierToString(assignment.qualityTier);
			assignmentJson["degradation_reason"] = ShadowDegradationReasonToString(assignment.degradationReason);
			assignmentJson["refresh_reason"] = ShadowRefreshReasonToString(assignment.refreshReason);
			assignmentJson["assigned"] = assignment.assigned;
			assignmentJson["reused_previous_owner"] = assignment.reusedPreviousOwner;
			assignmentJson["replaced_previous_owner"] = assignment.replacedPreviousOwner;
			assignmentJson["shadowed_previous_frame"] = assignment.shadowedPreviousFrame;
			assignmentJson["reused_cached_result"] = assignment.reusedCachedResult;
			assignmentJson["refreshed_this_frame"] = assignment.refreshedThisFrame;
			assignmentJson["invalidated_this_frame"] = assignment.invalidatedThisFrame;
			capture["shadow_assignments"].push_back(std::move(assignmentJson));
		}
		capture["cluster_count"] = snapshot.buildMetadata.clusterCount;
		capture["active_light_count"] = snapshot.buildMetadata.activeLightCount;
		capture["light_index_capacity"] = snapshot.buildMetadata.lightIndexCapacity;
		capture["used_index_count"] = snapshot.buildMetadata.usedIndexCount;
		capture["actual_assigned_light_count"] = snapshot.buildMetadata.actualAssignedLightCount;
		capture["logical_index_overhead_percent"] = logicalIndexOverheadPercent;
		capture["empty_cluster_count"] = snapshot.buildMetadata.emptyClusterCount;
		capture["empty_cluster_percent"] = emptyClusterPercent;
		capture["max_cluster_light_count"] = snapshot.buildMetadata.maxClusterLightCount;
		capture["retained_eligible_light_count"] = snapshot.buildMetadata.maxClusterLightCount;
		capture["out_of_bounds_range_count"] = snapshot.buildMetadata.outOfBoundsRangeCount;
		capture["invalid_light_index_count"] = snapshot.buildMetadata.invalidLightIndexCount;
		capture["overflow_cluster_count"] = snapshot.buildMetadata.overflowClusterCount;
		capture["determinism_mismatch_count"] = snapshot.buildMetadata.determinismMismatchCount;
		capture["range_hash"] = snapshot.buildMetadata.rangeHash;
		capture["range_hash_hex"] = BuildHashHex(snapshot.buildMetadata.rangeHash);
		capture["ranges"] = nlohmann::json::array();

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ClusterLightRangeSnapshot& range : snapshot.ranges) {
			nlohmann::json rangeJson;
			rangeJson["cluster_index"] = range.clusterIndex;
			rangeJson["offset"] = range.offset;
			rangeJson["count"] = range.count;
			rangeJson["out_of_bounds"] = range.outOfBounds;
			capture["ranges"].push_back(std::move(rangeJson));
		}

		std::ofstream outputFile(outputPath);
		outputFile << capture.dump(2);
	}
	catch (const std::exception& exception) {
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to write cluster validation capture: " << exception.what());
	}
}

// VulkanContext::Impl::AllocateTextureIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
uint32_t VulkanContext::Impl::AllocateTextureIndex() {
	if (!freeTextureIndices.empty()) {
		uint32_t index = freeTextureIndices.back();
		freeTextureIndices.pop_back();
		return index;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (nextTextureIndex >= MAX_BINDLESS_TEXTURES) {
		TERRAENGINE_LOG(LogLevel::Error, "Max bindless textures exceeded!");
		return 0; // Return default texture
	}
	return nextTextureIndex++;
}

// VulkanContext::Impl::AllocateMaterialIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
uint32_t VulkanContext::Impl::AllocateMaterialIndex() {
	while (!freeMaterialIndices.empty()) {
		uint32_t index = freeMaterialIndices.back();
		freeMaterialIndices.pop_back();
		if (index != kFallbackMaterialIndex) {
			return index;
		}
	}
	if (nextMaterialIndex <= kFallbackMaterialIndex) {
		nextMaterialIndex = kFirstRuntimeMaterialIndex;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (nextMaterialIndex >= MAX_BINDLESS_MATERIALS) {
		TERRAENGINE_LOG(LogLevel::Error, "Max bindless materials exceeded!");
		return 0; 
	}
	return nextMaterialIndex++;
}

// VulkanContext::Impl::UpdateGlobalTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateGlobalTexture(uint32_t index, const VulkanImage& image) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	WaitForDescriptorMutationIdle();

	VkDescriptorImageInfo imageInfo{};
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imageInfo.imageView = image.view;
	imageInfo.sampler = image.sampler;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		VkWriteDescriptorSet descriptorWrite{};
		descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite.dstSet = globalDescriptorSets[i];
		descriptorWrite.dstBinding = 0;
		descriptorWrite.dstArrayElement = index;
		descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrite.descriptorCount = 1;
		descriptorWrite.pImageInfo = &imageInfo;

		vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
	}
}

// VulkanContext::Impl::AllocateBufferIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
uint32_t VulkanContext::Impl::AllocateBufferIndex() {
	if (!freeBufferIndices.empty()) {
		uint32_t index = freeBufferIndices.back();
		freeBufferIndices.pop_back();
		return index;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (nextBufferIndex >= MAX_BINDLESS_BUFFERS) {
		throw std::runtime_error("Exceeded MAX_BINDLESS_BUFFERS!");
	}
	return nextBufferIndex++;
}

// VulkanContext::Impl::UpdateGlobalBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateGlobalBuffer(uint32_t index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (index >= MAX_BINDLESS_BUFFERS
		|| buffer == VK_NULL_HANDLE
		|| range == 0u)
	{
		return;
	}

	WaitForDescriptorMutationIdle();

	VkDescriptorBufferInfo bufferInfo{};
	bufferInfo.buffer = buffer;
	bufferInfo.offset = offset;
	bufferInfo.range = range;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		VkWriteDescriptorSet descriptorWrite{};
		descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite.dstSet = globalDescriptorSets[i];
		descriptorWrite.dstBinding = 9;
		descriptorWrite.dstArrayElement = index;
		descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptorWrite.descriptorCount = 1;
		descriptorWrite.pBufferInfo = &bufferInfo;

		vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
	}
}

void VulkanContext::Impl::UpdateGlobalBufferForFrame(uint32_t frameIndex, uint32_t index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range)
{
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (frameIndex >= globalDescriptorSets.size()
		|| index >= MAX_BINDLESS_BUFFERS
		|| globalDescriptorSets[frameIndex] == VK_NULL_HANDLE
		|| buffer == VK_NULL_HANDLE
		|| range == 0u)
	{
		return;
	}

	if (frameIndex == currentFrame)
	{
		// Updating a bindless buffer slot mutates the shared global descriptor
		// set. Reuse the central descriptor-idle path so already-submitted editor
		// view command buffers are waited on instead of accidentally submitted a
		// second time.
		WaitForDescriptorMutationIdle();
	}

	VkDescriptorBufferInfo bufferInfo{};
	bufferInfo.buffer = buffer;
	bufferInfo.offset = offset;
	bufferInfo.range = range;

	VkWriteDescriptorSet descriptorWrite{};
	descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet = globalDescriptorSets[frameIndex];
	descriptorWrite.dstBinding = 9;
	descriptorWrite.dstArrayElement = index;
	descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.pBufferInfo = &bufferInfo;

	vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
}

// VulkanContext::Impl::UpdateGlobalHiZ is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::UpdateGlobalHiZ(uint32_t frameIndex) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (frameIndex >= globalDescriptorSets.size()) return;

	std::array<VkDescriptorImageInfo, kHiZViewCount> imageInfos{};
	const auto isReadableHiZ = [](const VulkanHiZ& hiZ) {
		return hiZ.fullView != VK_NULL_HANDLE
			&& hiZ.sampler != VK_NULL_HANDLE
			&& hiZ.layoutInitialized
			&& hiZ.history.valid;
	};
	const VulkanHiZ* samplerOwner = nullptr;
	const VulkanHiZ* fallbackHiZ = nullptr;
	for (uint32_t hiZViewIndex = 0u; hiZViewIndex < kHiZViewCount; ++hiZViewIndex) {
		const VulkanHiZ& hiZ = HiZForFrame(hiZViewIndex, frameIndex);
		if (samplerOwner == nullptr && hiZ.sampler != VK_NULL_HANDLE) {
			samplerOwner = &hiZ;
		}
		if (isReadableHiZ(hiZ)) {
			fallbackHiZ = &hiZ;
			break;
		}
	}
	if (samplerOwner == nullptr) {
		return;
	}

	WaitForDescriptorMutationIdle();
	VkDescriptorImageInfo samplerInfo{};
	samplerInfo.sampler = samplerOwner->sampler;

	// The sampler is safe before the image has a layout. Binding 10 is partially
	// bound and frame data keeps sampling disabled until a completed write is
	// promoted, so do not publish an UNDEFINED image merely to fill the slot.
	if (fallbackHiZ == nullptr) {
		VkWriteDescriptorSet samplerWrite{};
		samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		samplerWrite.dstSet = globalDescriptorSets[frameIndex];
		samplerWrite.dstBinding = 11;
		samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerWrite.descriptorCount = 1;
		samplerWrite.pImageInfo = &samplerInfo;
		vkUpdateDescriptorSets(device, 1, &samplerWrite, 0, nullptr);
		return;
	}

	for (uint32_t hiZViewIndex = 0u; hiZViewIndex < kHiZViewCount; ++hiZViewIndex) {
		const VulkanHiZ& hiZ = HiZForFrame(hiZViewIndex, frameIndex);
		const VulkanHiZ& readableHiZ = isReadableHiZ(hiZ) ? hiZ : *fallbackHiZ;
		// Binding 10 is an array so editor Scene/Game command buffers can be
		// recorded once and submitted together without racing a descriptor
		// mutation between views. If one editor view has not produced a pyramid
		// yet, keep its descriptor valid with the other view's image while that
		// missing view's frame-data gate keeps Hi-Z sampling fail-open.
		imageInfos[hiZViewIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfos[hiZViewIndex].imageView = readableHiZ.fullView;
		imageInfos[hiZViewIndex].sampler = VK_NULL_HANDLE;
	}

	samplerInfo.sampler = fallbackHiZ->sampler;

	VkWriteDescriptorSet writes[2]{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = globalDescriptorSets[frameIndex];
	writes[0].dstBinding = 10;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	writes[0].descriptorCount = kHiZViewCount;
	writes[0].pImageInfo = imageInfos.data();

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = globalDescriptorSets[frameIndex];
	writes[1].dstBinding = 11;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	writes[1].descriptorCount = 1;
	writes[1].pImageInfo = &samplerInfo;

	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

void VulkanContext::Impl::WaitForDescriptorMutationIdle() {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	if (!initialized || device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE) {
		return;
	}

	// Global bindless sets and mesh-local descriptor sets are persistent across
	// frame slots. Asset reloads and export-runtime streaming can update or free
	// them after a view command buffer has recorded/bound descriptors. Queue idle
	// covers already-submitted work. Recorded view prefixes are submitted first,
	// then proven idle, so descriptor safety never silently drops Scene/Game work.
	if (currentFrame < pendingPrimaryCommandBuffers.size())
	{
		std::vector<VkCommandBuffer>& pendingCommandBuffers = pendingPrimaryCommandBuffers[currentFrame];
		if (!pendingCommandBuffers.empty())
		{
			const bool alreadySubmitted = currentFrame < pendingPrimaryCommandBuffersSubmitted.size()
				&& pendingPrimaryCommandBuffersSubmitted[currentFrame] != 0u;
			if (!alreadySubmitted)
			{
				VkSubmitInfo prefixSubmit{};
				prefixSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				prefixSubmit.commandBufferCount = static_cast<std::uint32_t>(pendingCommandBuffers.size());
				prefixSubmit.pCommandBuffers = pendingCommandBuffers.data();

				VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
				VkPipelineStageFlags acquireStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				const bool consumesRuntimeAcquire = !editorMode
					&& swapchainImageAcquired
					&& !swapchainAcquireWaitConsumed;
				if (consumesRuntimeAcquire)
				{
					acquireSemaphore = imageAvailableSemaphores[currentFrame];
					prefixSubmit.waitSemaphoreCount = 1u;
					prefixSubmit.pWaitSemaphores = &acquireSemaphore;
					prefixSubmit.pWaitDstStageMask = &acquireStage;
				}

				const VkResult submitResult = SubmitGraphicsQueue(1u, &prefixSubmit, VK_NULL_HANDLE);
				if (MarkDeviceLost(submitResult, "recorded view prefix submit before descriptor mutation"))
				{
					return;
				}
				if (submitResult != VK_SUCCESS)
				{
					TERRAENGINE_LOG(LogLevel::Error, "failed to submit recorded views before descriptor mutation: " << submitResult);
					return;
				}
				if (currentFrame < pendingPrimaryCommandBuffersSubmitted.size())
				{
					// Descriptor publication can re-enter this synchronization
					// path while asset finalization is still unwinding. Mark the
					// one-time primaries immediately after a successful submit so
					// a nested mutation waits for them instead of submitting the
					// same buffers again.
					pendingPrimaryCommandBuffersSubmitted[currentFrame] = 1u;
				}
				if (consumesRuntimeAcquire) swapchainAcquireWaitConsumed = true;
				ConfirmHiZWritesSubmitted(currentFrame);
				for (RenderSnapshotOwnerSummary& owner : pendingRenderSnapshotDiagnostics.ownerSummaries)
				{
					if (owner.state == RenderSnapshotState::Recorded)
					{
						owner.state = RenderSnapshotState::Submitted;
					}
				}
			}

			const VkResult prefixIdleResult = WaitGraphicsQueueIdle();
			if (MarkDeviceLost(prefixIdleResult, "render command buffer idle before descriptor mutation"))
			{
				return;
			}
			if (prefixIdleResult != VK_SUCCESS)
			{
				TERRAENGINE_LOG(LogLevel::Error, "failed to wait for render command buffers before descriptor mutation: " << prefixIdleResult);
				return;
			}
			if (editorMode && editorRenderSubmitted)
			{
				// EndFrame signaled this slot's binary render-finished semaphore,
				// but an asset/descriptor mutation reached the protected boundary
				// before PresentEditor consumed it. Queue idle proves the signal
				// operation completed; an explicit empty wait consumes that signal
				// before this function clears the one-submit marker and permits a
				// later render prefix to signal the same binary semaphore again.
				VkSemaphore renderSemaphore =
					renderFinishedSemaphores[currentFrame];
				VkPipelineStageFlags renderStage =
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				VkSubmitInfo drainSubmit{};
				drainSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				drainSubmit.waitSemaphoreCount = 1u;
				drainSubmit.pWaitSemaphores = &renderSemaphore;
				drainSubmit.pWaitDstStageMask = &renderStage;
				const VkResult drainResult =
					SubmitGraphicsQueue(1u, &drainSubmit, VK_NULL_HANDLE);
				if (MarkDeviceLost(
						drainResult,
						"descriptor mutation editor render semaphore drain"))
				{
					return;
				}
				if (drainResult != VK_SUCCESS)
				{
					TERRAENGINE_LOG(
						LogLevel::Error,
						"failed to consume editor render semaphore before descriptor mutation: "
							<< drainResult);
					return;
				}
				const VkResult drainIdleResult = WaitGraphicsQueueIdle();
				if (MarkDeviceLost(
						drainIdleResult,
						"descriptor mutation editor render semaphore drain idle"))
				{
					return;
				}
				if (drainIdleResult != VK_SUCCESS)
				{
					TERRAENGINE_LOG(
						LogLevel::Error,
						"failed to finish editor render semaphore drain before descriptor mutation: "
							<< drainIdleResult);
					return;
				}
				editorRenderSubmitted = false;
			}
			pendingCommandBuffers.clear();
			if (currentFrame < pendingPrimaryCommandBuffersSubmitted.size())
			{
				pendingPrimaryCommandBuffersSubmitted[currentFrame] = 0u;
			}
		}
	}

	const VkResult idleResult = WaitGraphicsQueueIdle();
	if (MarkDeviceLost(idleResult, "descriptor mutation queue idle"))
	{
		return;
	}
	if (idleResult != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to wait for descriptor mutation queue idle: " << idleResult);
	}
}
