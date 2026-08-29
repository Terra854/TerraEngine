#pragma pack_matrix(column_major)

struct PushConstants
{
    column_major float4x4 view;
    column_major float4x4 proj;
    column_major float4x4 viewProj; // Not strictly needed if we do inv here, but let's see.
    // We need inverse view and inverse proj to unproject.
    // Or we can just reconstruct the ray.
    float3 cameraPos;
    float nearPlane;
    float farPlane;
};

// We will likely need a specific push constant struct for the grid pass if we want to pass specific data.
// Or we reuse the scene uniforms but interpret them differently? No, better to have a dedicated struct or pass extras.
// For now, let's assume we can pass a struct via push constants.

// [[vk::push_constant]] ConstantBuffer<PushConstants> GridUniforms; // Removed duplicate

struct VSInput {
    uint VertexIndex : SV_VertexID;
};

struct VSOutput {
    float4 Pos : SV_POSITION;
    float3 NearPoint : TEXCOORD0;
    float3 FarPoint : TEXCOORD1;
};

// Helper to inverse a 4x4 matrix (if needed, but better passed from CPU)
// Since we don't have it, let's try to pass inverse matrices in the push constants.

struct GridPushConstants {
    column_major float4x4 view;
    column_major float4x4 proj;
    column_major float4x4 inverseView;
    column_major float4x4 inverseProj;
};

[[vk::push_constant]] ConstantBuffer<GridPushConstants> PC;

// A world unit is the finest useful construction interval. Smaller
// subdivisions add shimmer and clutter without helping normal scene placement.
static const float kMinimumGridSpacing = 1;

float3 UnprojectPoint(float x, float y, float z, float4x4 viewInv, float4x4 projInv) {
    float4 unprojectedPoint = mul(viewInv, mul(projInv, float4(x, y, z, 1.0)));
    return unprojectedPoint.xyz / unprojectedPoint.w;
}

VSOutput VSMain(VSInput input) {
    VSOutput output;
    
    // Grid covers screen
    float2 gridPlane[6] = {
        float2(1, 1), float2(-1, -1), float2(-1, 1),
        float2(-1, -1), float2(1, 1), float2(1, -1)
    };
    
    float2 p = gridPlane[input.VertexIndex];
    output.Pos = float4(p, 0.0, 1.0); 

    // Unproject to find world space near and far points on the view frustum corners
    output.NearPoint = UnprojectPoint(p.x, p.y, 0.0, PC.inverseView, PC.inverseProj).xyz;
    output.FarPoint = UnprojectPoint(p.x, p.y, 1.0, PC.inverseView, PC.inverseProj).xyz;
    
    // Pass view and proj for fragment usage if needed (e.g. for fading)
    return output;
}

bool IsOrthographicGrid()
{
	// The Scene View passes its final projection matrix to this shader. In the
	// standard perspective matrix _44 is zero, while the orthographic matrix
	// keeps it at one, so this is a cheap per-pixel branch selector.
	return abs(PC.proj._44) > 0.5;
}

bool TryResolveGridWorldPosition(VSOutput input, out float3 worldPos)
{
	const float denominator = input.FarPoint.y - input.NearPoint.y;
	if (abs(denominator) <= 0.00001)
	{
		worldPos = float3(0.0, 0.0, 0.0);
		return false;
	}

	// Keep the editor construction plane just below authored Y=0 geometry. A
	// mathematically coplanar grid z-fights through flat terrain and makes both
	// surfaces look damaged even when the perspective lines are continuous.
	const float kGridPlaneY = -0.02;
	const float t = (kGridPlaneY - input.NearPoint.y) / denominator;
	if (!IsOrthographicGrid() && t < 0.0)
	{
		worldPos = float3(0.0, 0.0, 0.0);
		return false;
	}

	// Orthographic zoom can place part of the near plane below the ground grid.
	// Keep treating the grid as an infinite editor construction plane instead
	// of clipping it at that arbitrary near-plane slice.
	worldPos = input.NearPoint + t * (input.FarPoint - input.NearPoint);
	return true;
}

float ComputeGridDepth(float3 worldPos)
{
	float4 clipPos = mul(PC.proj, mul(PC.view, float4(worldPos, 1.0)));
	float depth = clipPos.z / clipPos.w;
	return IsOrthographicGrid() ? saturate(depth) : depth;
}

float GridLineAlpha(float2 worldCoord, float spacing, float pixelThickness)
{
	const float safeSpacing = max(spacing, 0.0001);
	float2 scaledCoord = worldCoord / safeSpacing;
	float2 derivative = max(fwidth(scaledCoord), float2(0.0001, 0.0001));
	float2 grid = abs(frac(scaledCoord - 0.5) - 0.5) / derivative;
	float lineVal = min(grid.x, grid.y);
	return saturate(pixelThickness - lineVal);
}

float ChooseOrthographicGridSpacing(float2 worldCoord)
{
	float2 dx = ddx(worldCoord);
	float2 dy = ddy(worldCoord);
	float worldUnitsPerPixel = max(length(dx), length(dy));
	// Return the continuous target spacing instead of snapping to 1/2/5 steps.
	// BuildAdaptiveGridLines uses its fractional logarithm to cross-fade nested
	// decades, which makes wheel zoom feel stable rather than popping abruptly.
	return max(worldUnitsPerPixel * 28.0, kMinimumGridSpacing);
}

float ChoosePerspectiveGridSpacing()
{
	// A perspective plane must use one spacing for the whole draw. Choosing the
	// spacing from per-pixel derivatives makes neighbouring fragments select
	// different decades, which breaks continuous lines into the chaotic wedges
	// seen when the camera looks across a large terrain.
	float3 cameraPosition = mul(PC.inverseView, float4(0, 0, 0, 1)).xyz;
	// Keep the one-unit construction floor when the camera approaches the plane.
	// Above that floor, the logarithmic blend reveals progressively coarser detail
	// without a discontinuous spacing choice.
	return max(abs(cameraPosition.y) * 0.08, kMinimumGridSpacing);
}

void BuildAdaptiveGridLines(
	float2 worldCoord,
	float desiredSpacing,
	out float opacity,
	out float majorInfluence)
{
	// Each level is an exact ten-times subdivision of the next, matching the
	// predictable hierarchy used by common scene editors. Three simultaneous
	// levels make the end of one decade identical to the start of the next:
	// fine lines disappear, major lines become the new fine lines, and a new
	// major anchor arrives. Smoothstep softens both ends of the transition.
	float logarithmicSpacing = log(max(desiredSpacing, kMinimumGridSpacing)) / 2.302585093;
	float baseSpacing = pow(10.0, floor(logarithmicSpacing));
	float lodBlend = smoothstep(0.0, 1.0, frac(logarithmicSpacing));

	float fineCoverage = GridLineAlpha(worldCoord, baseSpacing, 0.92);
	float middleThickness = lerp(1.25, 0.92, lodBlend);
	float middleCoverage = GridLineAlpha(worldCoord, baseSpacing * 10.0, middleThickness);
	float coarseCoverage = GridLineAlpha(worldCoord, baseSpacing * 100.0, 1.25);

	float fineOpacity = fineCoverage * (1.0 - lodBlend) * 0.52;
	float middleOpacity = middleCoverage * lerp(0.82, 0.52, lodBlend);
	float coarseOpacity = coarseCoverage * lodBlend * 0.82;
	opacity = max(fineOpacity, max(middleOpacity, coarseOpacity));

	// Dark emphasis also migrates between nested levels instead of changing on
	// a single frame, so line weight and colour tell the same scale story.
	majorInfluence = max(
		middleCoverage * (1.0 - lodBlend),
		coarseCoverage * lodBlend);
}

float4 BuildPerspectiveGridColor(float3 worldPos)
{
	float2 coord = worldPos.xz;
	// The camera-height LOD is constant across the draw, keeping every grid line
	// continuous while still reducing density when a large object is framed.
	float desiredSpacing = ChoosePerspectiveGridSpacing();
	float gridOpacity = 0.0;
	float majorInfluence = 0.0;
	BuildAdaptiveGridLines(coord, desiredSpacing, gridOpacity, majorInfluence);
	float4 color = float4(0.20, 0.22, 0.24, gridOpacity);
	color.rgb = lerp(color.rgb, float3(0.13, 0.15, 0.17), majorInfluence);
	float minimumz = max(fwidth(worldPos.z), 0.0001);
	float minimumx = max(fwidth(worldPos.x), 0.0001);

	// Z Axis (Blue)
	if(abs(worldPos.x) < minimumx)
		color.xyz = float3(0.0, 0.0, 1.0);

	// X Axis (Red)
	if(abs(worldPos.z) < minimumz)
		color.xyz = float3(1.0, 0.0, 0.0);

	float dist = distance(mul(PC.inverseView, float4(0,0,0,1)).xyz, worldPos);
	// Preserve roughly two hundred visible cells at every adaptive spacing. A
	// large framed terrain therefore keeps contextual grid lines without making
	// a close-up view extend all the way to the projection far plane.
	float fadeDistance = max(100.0, desiredSpacing * 100.0);
	float fade = max(0.0, 1.0 - (dist / fadeDistance));
	color.a *= fade;
	return color;
}

float4 BuildOrthographicGridColor(float3 worldPos)
{
	float2 coord = worldPos.xz;
	float desiredSpacing = ChooseOrthographicGridSpacing(coord);
	float gridOpacity = 0.0;
	float majorInfluence = 0.0;
	BuildAdaptiveGridLines(coord, desiredSpacing, gridOpacity, majorInfluence);

	// Orthographic mode uses a pale drafting background, so the grid needs more
	// contrast than the perspective grid while still staying behind authored
	// geometry. Keep minor lines readable and let every tenth line anchor scale.
	float4 color = float4(0.20, 0.24, 0.27, gridOpacity);
	color.rgb = lerp(color.rgb, float3(0.13, 0.16, 0.18), majorInfluence);

	// Draw world axes at a stable screen thickness. They remain useful in ortho
	// even when the regular grid steps up to larger spacing.
	float zAxisAlpha = saturate(1.4 - abs(worldPos.x) / max(fwidth(worldPos.x), 0.0001));
	float xAxisAlpha = saturate(1.4 - abs(worldPos.z) / max(fwidth(worldPos.z), 0.0001));
	color.rgb = lerp(color.rgb, float3(0.25, 0.35, 0.95), zAxisAlpha);
	color.rgb = lerp(color.rgb, float3(0.95, 0.24, 0.24), xAxisAlpha);
	color.a = max(color.a, max(zAxisAlpha, xAxisAlpha) * 0.9);

	// Orthographic zoom is a projection scale, not a movement through space, so
	// avoid camera-distance or screen-edge fades that make the grid look cut off.
	return color;
}

float4 PSMain(VSOutput input) : SV_TARGET {
    float3 worldPos;
    if (!TryResolveGridWorldPosition(input, worldPos)) discard;
    
    // Compute depth
    float depth = ComputeGridDepth(worldPos);
    
    float4 color = IsOrthographicGrid()
		? BuildOrthographicGridColor(worldPos)
		: BuildPerspectiveGridColor(worldPos);
    
    if (color.a <= 0.0) discard;
    
    // Write Depth
    // output depth? We need to output depth so it interacts with other objects?
    // If we write depth, we need to output SV_Depth.
    
    return color;
}

struct PSOutput {
    float4 Color : SV_Target;
    float Depth : SV_Depth;
};

PSOutput PSMainWithDepth(VSOutput input) {
    PSOutput output;
    
    float3 worldPos;
    if (!TryResolveGridWorldPosition(input, worldPos)) discard;
    
    float depth = ComputeGridDepth(worldPos);
    
    float4 color = IsOrthographicGrid()
		? BuildOrthographicGridColor(worldPos)
		: BuildPerspectiveGridColor(worldPos);
    
    if (color.a <= 0.0) discard;
    
    output.Color = color;
    output.Depth = depth;
    return output;
}
