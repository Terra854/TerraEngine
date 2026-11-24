#include "SMAACommon.hlsli"
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float2> edgeTexture;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState edgeSampler;
float2 Edges(int2 p)
{
	return Inside(p) ? step(0.5f.xx, edgeTexture.SampleLevel(edgeSampler, PixelUv(p), 0)) : 0.0f.xx;
}
// Local coordinates (t,n) follow an edge tangent and its positive normal.
// Rotating local coordinates lets the same classifier process north and west.
bool LocalInside(int2 q, bool north) { return Inside(north ? q : q.yx); }
float Primary(int2 q, bool north) { return north ? Edges(q).y : Edges(q.yx).x; }
float Crossing(int2 q, bool north) { return north ? Edges(q).x : Edges(q.yx).y; }
float2 CrossingSides(int2 endpoint, bool north)
{
	return float2(Crossing(endpoint + int2(0, -1), north), Crossing(endpoint, north));
}
float CrossingSign(int2 endpoint, bool north)
{
	const float2 crossing = CrossingSides(endpoint, north);
	return crossing.y - crossing.x; // Positive bends into the current pixel row.
}
uint SearchCardinal(int2 q, int direction, bool north)
{
	uint distance = 0;
	[loop] for(uint stepIndex = 0; stepIndex < min(smaa.maxSearchSteps, 64u); ++stepIndex)
	{
		const int2 boundary = q + int2(direction > 0 ? int(distance) + 1 : -int(distance), 0);
		if(any(CrossingSides(boundary, north))) break;
		const int2 next = q + int2(direction * (int(distance) + 1), 0);
		if(Primary(next, north) < 0.5f) break;
		++distance;
	}
	return distance;
}
bool DiagonalLink(int2 q, int2 direction, bool north)
{
	// The connector is the perpendicular unit segment of a true staircase.
	// Requiring it and the next parallel segment rejects nearby unrelated edges.
	const int2 connector = q + int2(direction.x > 0 ? 1 : 0, direction.y < 0 ? -1 : 0);
	return Primary(q + direction, north) > 0.5f && Crossing(connector, north) > 0.5f;
}
uint SearchDiagonal(int2 q, int2 direction, bool north)
{
	uint distance = 0;
	[loop] for(uint stepIndex = 0; stepIndex < min(smaa.maxDiagonalSteps, 32u); ++stepIndex)
	{
		const int2 current = q + direction * int(distance);
		if(!DiagonalLink(current, direction, north)) break;
		++distance;
	}
	return distance;
}

bool HasDiagonalConnection(int2 q, bool north)
{
	[loop] for(uint orientation = 0; orientation < 4u; ++orientation)
	{
		const int2 direction = int2((orientation & 1u) != 0u ? 1 : -1, (orientation & 2u) != 0u ? 1 : -1);
		if(DiagonalLink(q, direction, north)) return true;
	}
	return false;
}
bool CrossingBelongsToDiagonal(int2 endpoint, bool north)
{
	const int2 previous = endpoint + int2(0, -1);
	return (Crossing(endpoint, north) > 0.5f && HasDiagonalConnection(endpoint.yx, !north))
		|| (Crossing(previous, north) > 0.5f && HasDiagonalConnection(previous.yx, !north));
}
bool ExtendedCorner(int2 endpoint, float side, bool north)
{
	const int offset = side > 0.0f ? 1 : -2;
	return side != 0.0f && Crossing(endpoint + int2(0, offset), north) > 0.5f;
}
float CornerScale(int2 endpoint, float side, bool north)
{
	return ExtendedCorner(endpoint, side, north) ? saturate(smaa.cornerRounding) : 1.0f;
}
float2 IntegrateSegment(float startX, float endX, float startY, float endY, float pixelX)
{
	const float lo = max(pixelX, startX), hi = min(pixelX + 1.0f, endX);
	if(hi <= lo) return 0.0f.xx;
	const float inverseLength = rcp(max(endX - startX, 1.0e-6f));
	const float a = lerp(startY, endY, (lo - startX) * inverseLength);
	const float b = lerp(startY, endY, (hi - startX) * inverseLength);
	return (hi - lo) * PixelCrossingAreas(a, b);
}
float2 CardinalAreas(int2 q, bool north)
{
	const uint left = SearchCardinal(q, -1, north), right = SearchCardinal(q, 1, north);
	const int2 start = q + int2(-int(left), 0), end = q + int2(int(right) + 1, 0);
	const float a = CrossingSign(start, north), b = CrossingSign(end, north);
	const float length = float(left + right + 1u);
	// A straight cardinal side meeting an already detected diagonal at one end
	// and an extended geometric corner at the other is not an aliased U run.
	// Let the diagonal resolve its junction instead of bending the whole side.
	if((ExtendedCorner(start, a, north) && CrossingBelongsToDiagonal(end, north))
		|| (ExtendedCorner(end, b, north) && CrossingBelongsToDiagonal(start, north))) return 0.0f.xx;
	float2 area;
	if(a * b > 0.0f)
	{
		// Same-side endpoint crossings describe a U pattern. Its two segments
		// meet at the original boundary instead of displacing an entire long run.
		area = IntegrateSegment(0, 0.5f * length, 0.5f * a, 0, float(left))
			+ IntegrateSegment(0.5f * length, length, 0, 0.5f * b, float(left));
	}
	else area = IntegrateSegment(0, length, 0.5f * a, 0.5f * b, float(left));
	return area * (left < right ? CornerScale(start, a, north) : CornerScale(end, b, north));
}
float2 DiagonalAreas(int2 q, int slope, uint left, uint right, bool north)
{
	const int2 start = q - int2(1, slope) * int(left);
	const int2 end = q + int2(1, slope) * int(right) + int2(1, 0);
	// Endpoint phase is a bounded re-vectorization model derived from crossing
	// orientation. The reconstruction model is independent of upstream lookup
	// layouts; coverage itself is integrated exactly against each pixel box.
	// A viewport-clipped contour is not an observed object endpoint. In
	// particular, a last in-bounds staircase connector cannot be reused as a
	// terminal crossing when its next primary segment lies outside the image.
	const int2 tangent = int2(1, slope);
	const int2 last = q + tangent * int(right);
	const bool leftKnown = left < min(smaa.maxDiagonalSteps, 32u) && LocalInside(start - tangent, north);
	const bool rightKnown = right < min(smaa.maxDiagonalSteps, 32u) && LocalInside(last + tangent, north);
	// A staircase connector owned by this diagonal is not an independent
	// transverse crossing. Using it twice biases the entire finite segment.
	const float phaseA = leftKnown && !CrossingBelongsToDiagonal(start, north) ? -0.25f * CrossingSign(start, north) : 0.0f;
	const float phaseB = rightKnown && !CrossingBelongsToDiagonal(end, north) ? 0.25f * CrossingSign(end, north) : 0.0f;
	const float span = float(left + right + 1u);
	const float leftInterpolatedPhase = lerp(phaseA, phaseB, float(left) / span);
	const float rightInterpolatedPhase = lerp(phaseA, phaseB, float(left + 1u) / span);
	return PixelCrossingAreas(-0.5f * float(slope) + leftInterpolatedPhase, 0.5f * float(slope) + rightInterpolatedPhase);
}
float2 Areas(int2 q, bool north)
{
	if(Primary(q, north) < 0.5f) return 0.0f.xx;
	const uint al = SearchDiagonal(q, int2(-1, -1), north), ar = SearchDiagonal(q, int2(1, 1), north);
	const uint bl = SearchDiagonal(q, int2(-1, 1), north), br = SearchDiagonal(q, int2(1, -1), north);
	const bool diagonal = max(al + ar, bl + br) >= 2u;
	if(diagonal)
	{
		const float2 area = al + ar >= bl + br ? DiagonalAreas(q, 1, al, ar, north) : DiagonalAreas(q, -1, bl, br, north);
		return area; // Resolve selects one representation of overlapping diagonal coverage.
	}
	return CardinalAreas(q, north);
}
float4 PSMain(float4 position : SV_Position) : SV_Target0
{
	const int2 p = int2(position.xy);
	const float2 west = Areas(p.yx, false), north = Areas(p, true);
	return float4(west, north);
}
