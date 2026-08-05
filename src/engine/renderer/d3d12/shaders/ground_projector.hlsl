cbuffer ProjectorCameraConstants : register(b0) {
    row_major float4x4 viewProjection;
    float2 visibilityOrigin;
    float visibilityInverseCellSize;
    float visibilityEnabled;
    float2 visibilityTextureSize;
    float2 playableMinimum;
    float2 playableMaximum;
    float playableBoundsEnabled;
    float3 visibilityPadding;
};

Texture2D<float4> decalTexture : register(t0);
Texture2D<float> localVisibilityMap : register(t1);
SamplerState decalSampler : register(s0);

struct VsInput {
    uint vertexId : SV_VertexID;
    float3 corner0 : PROJECTOR_CORNER0;
    float3 corner1 : PROJECTOR_CORNER1;
    float3 corner2 : PROJECTOR_CORNER2;
    float3 corner3 : PROJECTOR_CORNER3;
    float4 color : PROJECTOR_COLOR0;
    float2 uvScale : PROJECTOR_UV_SCALE0;
    float2 uvOffset : PROJECTOR_UV_OFFSET0;
    float edgeSoftness : PROJECTOR_SOFTNESS0;
    uint radialMask : PROJECTOR_RADIAL0;
    uint blendMode : PROJECTOR_BLEND0;
    uint receivesVisibility : PROJECTOR_VISIBILITY0;
    float2 cornerUv0 : PROJECTOR_EXPLICIT_UV0;
    float2 cornerUv1 : PROJECTOR_EXPLICIT_UV1;
    float2 cornerUv2 : PROJECTOR_EXPLICIT_UV2;
    float2 cornerUv3 : PROJECTOR_EXPLICIT_UV3;
    uint explicitCornerUvs : PROJECTOR_EXPLICIT_UVS0;
    uint triangleFlip : PROJECTOR_TRIANGLE_FLIP0;
};

struct PsInput {
    float4 position : SV_POSITION;
    float3 worldPosition : POSITION0;
    float2 radial : TEXCOORD0;
    float2 uv : TEXCOORD1;
    nointerpolation float4 color : COLOR0;
    nointerpolation float edgeSoftness : TEXCOORD2;
    nointerpolation uint radialMask : TEXCOORD3;
    nointerpolation uint blendMode : TEXCOORD4;
    nointerpolation uint receivesVisibility : TEXCOORD5;
};

PsInput VSMain(VsInput input) {
    const uint regularCornerIndex[6] = {0, 1, 2, 0, 2, 3};
    const uint flippedCornerIndex[6] = {3, 1, 0, 3, 2, 1};
    const uint vertex = min(input.vertexId, 5u);
    const uint selected = input.triangleFlip != 0u
        ? flippedCornerIndex[vertex] : regularCornerIndex[vertex];
    float3 position = input.corner0;
    float2 radial = float2(-1.0, -1.0);
    if (selected == 1u) {
        position = input.corner1;
        radial = float2(-1.0, 1.0);
    } else if (selected == 2u) {
        position = input.corner2;
        radial = float2(1.0, 1.0);
    } else if (selected == 3u) {
        position = input.corner3;
        radial = float2(1.0, -1.0);
    }
    PsInput output;
    output.position = mul(float4(position, 1.0), viewProjection);
    output.worldPosition = position;
    output.radial = radial;
    const float2 canonicalUv = radial * float2(0.5, -0.5) + 0.5;
    float2 explicitUv = input.cornerUv0;
    if (selected == 1u) explicitUv = input.cornerUv1;
    if (selected == 2u) explicitUv = input.cornerUv2;
    if (selected == 3u) explicitUv = input.cornerUv3;
    output.uv = input.explicitCornerUvs != 0u
        ? explicitUv : input.uvOffset + canonicalUv * input.uvScale;
    output.color = input.color;
    output.edgeSoftness = input.edgeSoftness;
    output.radialMask = input.radialMask;
    output.blendMode = input.blendMode;
    output.receivesVisibility = input.receivesVisibility;
    return output;
}

float projectorVisibility(float3 worldPosition) {
    float result = 1.0;
    if (visibilityEnabled > 0.0 && visibilityInverseCellSize > 0.0 &&
        visibilityTextureSize.x > 0.0 && visibilityTextureSize.y > 0.0) {
        const float2 cell = (worldPosition.xy - visibilityOrigin) *
            visibilityInverseCellSize;
        result = 0.0;
        if (cell.x >= 0.0 && cell.y >= 0.0 &&
            cell.x < visibilityTextureSize.x &&
            cell.y < visibilityTextureSize.y) {
            const float2 uv = (cell + 0.5) / visibilityTextureSize;
            result = saturate(localVisibilityMap.SampleLevel(
                decalSampler, uv, 0.0).r);
        }
    }
    return result;
}

float4 PSMain(PsInput input) : SV_TARGET {
    if (playableBoundsEnabled > 0.0 &&
        (any(input.worldPosition.xy < playableMinimum) ||
         any(input.worldPosition.xy > playableMaximum))) {
        clip(-1.0);
    }
    float coverage = 1.0;
    if (input.radialMask != 0u) {
        const float distanceFromCenter = length(input.radial);
        const float softness = clamp(input.edgeSoftness, 0.01, 0.99);
        coverage = 1.0 - smoothstep(1.0 - softness, 1.0,
                                    distanceFromCenter);
    }
    const float4 sampled = decalTexture.Sample(decalSampler, input.uv);
    coverage *= sampled.a;
    clip(coverage - (1.0 / 255.0));
    float visibility = 1.0;
    if (input.receivesVisibility != 0u) {
        visibility = projectorVisibility(input.worldPosition);
    }
    if (input.blendMode == 2u) {
        const float strength = saturate(
            input.color.a * coverage * visibility);
        return float4(lerp(float3(1.0, 1.0, 1.0),
                           sampled.rgb * input.color.rgb, strength), 1.0);
    }
    return float4(sampled.rgb * input.color.rgb,
                  input.color.a * coverage * visibility);
}
