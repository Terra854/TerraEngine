// AnimatedDeformedGeometry.hlsli - canonical graphics-only deformation fetch.
//
// Include this after declaring the engine globalBuffers[] bindless array.
// Geometry stages see only current/history vertices and dynamic proof records;
// skin, morph, palette, and animation-program resources remain compute-private.

#ifndef TERRAENGINE_ANIMATED_DEFORMED_GEOMETRY_HLSLI
#define TERRAENGINE_ANIMATED_DEFORMED_GEOMETRY_HLSLI

#include "AnimatedVirtualGeometryAbi.hlsli"

#define TE_ANIMATED_GRAPHICS_BRIDGE_HEADER_WORDS 16u
#define TE_ANIMATED_GRAPHICS_BRIDGE_RECORD_WORDS 4u
#define TE_ANIMATED_GRAPHICS_BRIDGE_MARKER 0x4445464du

struct TeAnimatedGraphicsBridge
{
	uint currentBufferIndex;
	uint previousBufferIndex;
	uint boundsBufferIndex;
	uint refinementBufferIndex;
	uint recordWordOffset;
	uint recordCount;
	uint flags;
};

struct TeAnimatedGraphicsPage
{
	uint pageId;
	uint currentVertexOffset;
	uint boundsIndex;
	uint flags;
};

bool TeTryLoadAnimatedGraphicsBridge(
	uint bridgeBufferIndex,
	uint headerWordOffset,
	out TeAnimatedGraphicsBridge bridge)
{
	bridge = (TeAnimatedGraphicsBridge)0;
	if(bridgeBufferIndex == TE_ANIMATED_GEOMETRY_INVALID_INDEX
		|| headerWordOffset > 0xffffffffu
			- TE_ANIMATED_GRAPHICS_BRIDGE_HEADER_WORDS)
	{
		return false;
	}
	uint elementCount;
	uint stride;
	globalBuffers[NonUniformResourceIndex(bridgeBufferIndex)]
		.GetDimensions(elementCount, stride);
	const uint header = headerWordOffset;
	if(header > elementCount
		|| TE_ANIMATED_GRAPHICS_BRIDGE_HEADER_WORDS
			> elementCount - header
		|| globalBuffers[
			NonUniformResourceIndex(bridgeBufferIndex)][header + 15u]
			!= TE_ANIMATED_GRAPHICS_BRIDGE_MARKER
		|| globalBuffers[
			NonUniformResourceIndex(bridgeBufferIndex)][header + 0u]
			!= TE_ANIMATED_GEOMETRY_ABI_VERSION)
	{
		return false;
	}
	bridge.recordWordOffset = globalBuffers[
		NonUniformResourceIndex(bridgeBufferIndex)][header + 2u];
	bridge.recordCount = globalBuffers[
		NonUniformResourceIndex(bridgeBufferIndex)][header + 3u];
	bridge.currentBufferIndex = globalBuffers[
		NonUniformResourceIndex(bridgeBufferIndex)][header + 4u];
	bridge.previousBufferIndex = globalBuffers[
		NonUniformResourceIndex(bridgeBufferIndex)][header + 5u];
	bridge.boundsBufferIndex = globalBuffers[
		NonUniformResourceIndex(bridgeBufferIndex)][header + 6u];
	bridge.refinementBufferIndex = globalBuffers[
		NonUniformResourceIndex(bridgeBufferIndex)][header + 7u];
	bridge.flags = globalBuffers[
		NonUniformResourceIndex(bridgeBufferIndex)][header + 12u];
	return bridge.currentBufferIndex
			!= TE_ANIMATED_GEOMETRY_INVALID_INDEX
		&& bridge.boundsBufferIndex
			!= TE_ANIMATED_GEOMETRY_INVALID_INDEX
		&& bridge.recordCount != 0u
		&& bridge.recordWordOffset <= elementCount
		&& bridge.recordCount
			<= (elementCount - bridge.recordWordOffset)
				/ TE_ANIMATED_GRAPHICS_BRIDGE_RECORD_WORDS;
}

bool TeTryResolveAnimatedGraphicsPage(
	uint bridgeBufferIndex,
	TeAnimatedGraphicsBridge bridge,
	uint pageId,
	out TeAnimatedGraphicsPage page)
{
	page = (TeAnimatedGraphicsPage)0;
	for(uint index = 0u; index < bridge.recordCount; ++index)
	{
		const uint word = bridge.recordWordOffset
			+ index * TE_ANIMATED_GRAPHICS_BRIDGE_RECORD_WORDS;
		if(globalBuffers[
			NonUniformResourceIndex(bridgeBufferIndex)][word] != pageId)
		{
			continue;
		}
		page.pageId = pageId;
		page.currentVertexOffset = globalBuffers[
			NonUniformResourceIndex(bridgeBufferIndex)][word + 1u];
		page.boundsIndex = globalBuffers[
			NonUniformResourceIndex(bridgeBufferIndex)][word + 2u];
		page.flags = globalBuffers[
			NonUniformResourceIndex(bridgeBufferIndex)][word + 3u];
		return (page.flags & TE_ANIMATED_GEOMETRY_VALID_CURRENT)
			!= 0u;
	}
	return false;
}

bool TeTryLoadAnimatedVertex(
	TeAnimatedGraphicsBridge bridge,
	TeAnimatedGraphicsPage page,
	uint localVertexIndex,
	out TeAnimatedDeformedVertex vertex)
{
	vertex = (TeAnimatedDeformedVertex)0;
	if(localVertexIndex > (0xffffffffu - page.currentVertexOffset)
		|| bridge.currentBufferIndex
			== TE_ANIMATED_GEOMETRY_INVALID_INDEX)
	{
		return false;
	}
	const uint vertexIndex =
		page.currentVertexOffset + localVertexIndex;
	if(vertexIndex > (0xffffffffu - 16u) / 16u)
	{
		return false;
	}
	const uint word = vertexIndex * 16u;
	uint elementCount;
	uint stride;
	globalBuffers[
		NonUniformResourceIndex(bridge.currentBufferIndex)]
			.GetDimensions(elementCount, stride);
	if(word > elementCount || 16u > elementCount - word)
	{
		return false;
	}
	vertex.currentPosition = asfloat(uint4(
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 0u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 1u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 2u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 3u]));
	vertex.previousPosition = asfloat(uint4(
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 4u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 5u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 6u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 7u]));
	vertex.normal = asfloat(uint4(
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 8u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 9u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 10u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 11u]));
	vertex.tangentAndSign = asfloat(uint4(
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 12u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 13u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 14u],
		globalBuffers[
			NonUniformResourceIndex(bridge.currentBufferIndex)][word + 15u]));
	return all(!isnan(vertex.currentPosition.xyz))
		&& all(!isinf(vertex.currentPosition.xyz))
		&& all(!isnan(vertex.normal.xyz))
		&& all(!isinf(vertex.normal.xyz))
		&& all(!isnan(vertex.tangentAndSign.xyz))
		&& all(!isinf(vertex.tangentAndSign.xyz));
}

bool TeTryLoadAnimatedBounds(
	TeAnimatedGraphicsBridge bridge,
	TeAnimatedGraphicsPage page,
	out TeAnimatedDynamicBounds bounds)
{
	bounds = (TeAnimatedDynamicBounds)0;
	if(bridge.boundsBufferIndex
			== TE_ANIMATED_GEOMETRY_INVALID_INDEX
		|| page.boundsIndex > (0xffffffffu - 16u) / 16u)
	{
		return false;
	}
	const uint word = page.boundsIndex * 16u;
	uint elementCount;
	uint stride;
	globalBuffers[
		NonUniformResourceIndex(bridge.boundsBufferIndex)]
			.GetDimensions(elementCount, stride);
	if(word > elementCount || 16u > elementCount - word)
	{
		return false;
	}
	bounds.minimum = asfloat(uint4(
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 0u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 1u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 2u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 3u]));
	bounds.maximum = asfloat(uint4(
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 4u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 5u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 6u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 7u]));
	bounds.sphere = asfloat(uint4(
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 8u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 9u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 10u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 11u]));
	bounds.identity = uint4(
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 12u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 13u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 14u],
		globalBuffers[
			NonUniformResourceIndex(bridge.boundsBufferIndex)][word + 15u]);
	return (bounds.identity.w
			& (TE_ANIMATED_GEOMETRY_VALID_CURRENT
				| TE_ANIMATED_GEOMETRY_VALID_BOUNDS))
		== (TE_ANIMATED_GEOMETRY_VALID_CURRENT
			| TE_ANIMATED_GEOMETRY_VALID_BOUNDS);
}

#endif
