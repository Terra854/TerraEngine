#pragma once

// Shared implementation-only dependencies and helpers for Vulkan resource source files.
// Resource ownership stays in VulkanContext::Impl; these helpers only translate and upload
// data for the focused animation, geometry, pipeline, lighting, and diagnostics modules.

#include "VulkanContextImpl.h"
#include "VulkanRendererWorkCache.h"
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
#include <memory>
#include <new>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

		void* mapped = buffer.mappedData;
		const bool temporaryMapping = mapped == nullptr;
		if (temporaryMapping
			&& vmaMapMemory(allocator, buffer.allocation, &mapped) != VK_SUCCESS)
		{
			return false;
		}

		std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, source, static_cast<std::size_t>(size));
		vmaFlushAllocation(allocator, buffer.allocation, offset, size);
		if (temporaryMapping)
		{
			vmaUnmapMemory(allocator, buffer.allocation);
		}
		return true;
	}

	// ClearMappedBufferRange establishes deterministic safe contents only for the
	// bytes a shader may read before overwriting. Large deformation outputs are
	// intentionally excluded: their dispatch writes every active element, and
	// clearing their full retained capacity on the CPU dominated animated frames.
	bool ClearMappedBufferRange(
		VmaAllocator allocator,
		const VulkanBuffer& buffer,
		VkDeviceSize size,
		VkDeviceSize offset = 0u)
	{
		if(allocator == VK_NULL_HANDLE || buffer.allocation == VK_NULL_HANDLE
			|| size == 0u || offset > buffer.size || size > buffer.size - offset) return false;
		void* mapped = buffer.mappedData;
		const bool temporaryMapping = mapped == nullptr;
		if(temporaryMapping
			&& vmaMapMemory(allocator, buffer.allocation, &mapped) != VK_SUCCESS) return false;
		std::memset(static_cast<std::uint8_t*>(mapped) + offset, 0,
			static_cast<std::size_t>(size));
		vmaFlushAllocation(allocator, buffer.allocation, offset, size);
		if(temporaryMapping) vmaUnmapMemory(allocator, buffer.allocation);
		return true;
	}

	bool ClearMappedBuffer(VmaAllocator allocator, const VulkanBuffer& buffer)
	{
		return ClearMappedBufferRange(allocator, buffer, buffer.size);
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

	// One record per selected packet replaces the former 320-byte upload for
	// every triangle. The GPU setup stage combines this immutable state with the
	// page-local TEMF payload and optional deformation bridge.
	struct alignas(16) VirtualGeometryRasterPacketSetupUpload
	{
		glm::mat4 viewProjection = glm::mat4(1.0f);
		glm::mat4 model = glm::mat4(1.0f);
		glm::mat4 normalTransform = glm::mat4(1.0f);
		glm::vec4 editorTint = glm::vec4(0.0f);
		glm::uvec4 payloadState = glm::uvec4(0u);
		glm::uvec4 identity = glm::uvec4(0u);
		glm::uvec4 shadingOffsets = glm::uvec4(0u);
		glm::uvec4 shadingState = glm::uvec4(0u);
		glm::uvec4 debugState = glm::uvec4(0u);
		glm::uvec4 deformationState = glm::uvec4(0u);
	};
	static_assert(
		sizeof(VirtualGeometryRasterPacketSetupUpload) == 304u,
		"Compute-raster packet setup uploads must match the shader ABI.");

	struct VirtualGeometryRasterMeshletSetupUpload
	{
		std::uint32_t packetIndex = 0u;
		std::uint32_t localMeshletIndex = 0u;
		std::uint32_t primitiveBase = 0u;
		std::uint32_t triangleCount = 0u;
	};
	static_assert(sizeof(VirtualGeometryRasterMeshletSetupUpload) == 16u,
		"Compute-raster meshlet work must remain one uint4.");

	struct VirtualGeometryMeshPageKey
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0u;

		bool operator==(const VirtualGeometryMeshPageKey&) const = default;
	};

	struct VirtualGeometryMeshPageKeyHash
	{
		std::size_t operator()(const VirtualGeometryMeshPageKey& key) const noexcept
		{
			const std::size_t meshHash = std::hash<TerraEngine::UUID>{}(key.meshId);
			return meshHash ^ (static_cast<std::size_t>(key.pageId)
				+ 0x9e3779b9u + (meshHash << 6u) + (meshHash >> 2u));
		}
	};

	std::uint64_t BuildVirtualGeometryPagePayloadResourceSignature(
		const std::vector<GpuVirtualGeometryPacketRecord>& packets)
	{
		std::uint64_t signature = TerraEngine::Renderer::Vulkan::BuildVirtualGeometryPagePayloadReuseSignature(packets);
		auto hashValue = [&signature](std::uint64_t value) {
			// Match the packet-signature mixer while adding the loaded resource
			// identity that the pure upload planner cannot see.
			signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6u) + (signature >> 2u);
		};
		// A crowd can contain thousands of packets that reference the same mesh
		// page. Resource identity is page-owned, so resolve and hash each distinct
		// mesh/page pair once while the packet signature above still covers every
		// per-instance meshlet range and binding.
		thread_local std::unordered_set<VirtualGeometryMeshPageKey,
			VirtualGeometryMeshPageKeyHash> visitedPages;
		visitedPages.clear();
		visitedPages.reserve(std::min<std::size_t>(packets.size(), 256u));
		for (const GpuVirtualGeometryPacketRecord& packet : packets)
		{
			if(!visitedPages.emplace(
				VirtualGeometryMeshPageKey{ packet.meshId, packet.pageId }).second)
			{
				continue;
			}
			const auto meshOwner = ResourceManager::Instance().AcquireMeshSnapshot(packet.meshId);
			const Mesh* mesh = meshOwner.get(); // Keep metadata alive across a concurrent reload.
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

		const auto meshOwner = ResourceManager::Instance().AcquireMeshSnapshot(packet.meshId);
		const Mesh* cpuMesh = meshOwner.get(); // Packing consumes this retained immutable publication.
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
