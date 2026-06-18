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
    float4 GridView : TEXCOORD2; // view matrix
    float4 GridProj : TEXCOORD3; // proj matrix
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

float4 PSMain(VSOutput input) : SV_TARGET {
    float t = -input.NearPoint.y / (input.FarPoint.y - input.NearPoint.y);
    
    if (t < 0.0) discard; // Sky
    
    float3 worldPos = input.NearPoint + t * (input.FarPoint - input.NearPoint);
    
    // Compute depth
    float4 clipPos = mul(PC.proj, mul(PC.view, float4(worldPos, 1.0)));
    float depth = clipPos.z / clipPos.w;
    
    // Draw grid
    float2 coord = worldPos.xz;
    float2 derivative = fwidth(coord);
    float2 grid = abs(frac(coord - 0.5) - 0.5) / derivative;
    float lineVal = min(grid.x, grid.y);
    float minimumz = min(derivative.y, 1.0);
    float minimumx = min(derivative.x, 1.0);
    
    float4 color = float4(0.2, 0.2, 0.2, 1.0 - min(lineVal, 1.0));
    
    // Z Axis (Blue)
    if(abs(worldPos.x) < minimumx)
        color = float4(0.0, 0.0, 1.0, 1.0);
        
    // X Axis (Red)
    if(abs(worldPos.z) < minimumz)
        color = float4(1.0, 0.0, 0.0, 1.0);

    // Fade out
    float dist = distance(mul(PC.inverseView, float4(0,0,0,1)).xyz, worldPos);
    float fade = max(0.0, 1.0 - (dist / 100.0)); // 100m grid radius
    
    color.a *= fade;
    
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
    
    float t = -input.NearPoint.y / (input.FarPoint.y - input.NearPoint.y);
    
    if (t < 0.0) discard;
    
    float3 worldPos = input.NearPoint + t * (input.FarPoint - input.NearPoint);
    
    float4 clipPos = mul(PC.proj, mul(PC.view, float4(worldPos, 1.0)));
    float depth = clipPos.z / clipPos.w;
    
    float2 coord = worldPos.xz;
    float2 derivative = fwidth(coord);
    float2 grid = abs(frac(coord - 0.5) - 0.5) / derivative;
    float lineVal = min(grid.x, grid.y);
    float minimumz = min(derivative.y, 1.0);
    float minimumx = min(derivative.x, 1.0);
    
    float4 color = float4(0.2, 0.2, 0.2, 1.0 - min(lineVal, 1.0));
    
    // Z Axis (Blue)
    if(abs(worldPos.x) < minimumx)
        color.xyz = float3(0.0, 0.0, 1.0);
        
    // X Axis (Red)
    if(abs(worldPos.z) < minimumz)
        color.xyz = float3(1.0, 0.0, 0.0);

    float dist = distance(mul(PC.inverseView, float4(0,0,0,1)).xyz, worldPos);
    float fade = max(0.0, 1.0 - (dist / 100.0));
    
    color.a *= fade;
    
    if (color.a <= 0.0) discard;
    
    output.Color = color;
    output.Depth = depth;
    return output;
}
