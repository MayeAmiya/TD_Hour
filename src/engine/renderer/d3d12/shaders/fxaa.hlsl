cbuffer FxaaConstants : register(b0) {
    float2 inverseTextureSize;
    float tacticalHeightScale;
    float subpixelAmount;
    float edgeThreshold;
    float edgeThresholdMin;
    float2 fxaaPadding;
};

Texture2D<float4> sceneTexture : register(t0);
SamplerState sceneSampler : register(s0);

struct FxaaVsOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

FxaaVsOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0),
    };
    const float2 texcoords[3] = {
        float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0),
    };
    FxaaVsOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.texcoord = texcoords[vertexId];
    return output;
}

float fxaaLuma(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

float4 sampleClamped(float2 uv, float2 uvMin, float2 uvMax) {
    return sceneTexture.SampleLevel(sceneSampler, clamp(uv, uvMin, uvMax), 0.0);
}

float4 PSMain(FxaaVsOutput input) : SV_TARGET {
    const float2 uvMin = inverseTextureSize * 0.5;
    const float2 uvMax = float2(1.0, tacticalHeightScale) - uvMin;
    const float2 uv = clamp(
        float2(input.texcoord.x, input.texcoord.y * tacticalHeightScale),
        uvMin, uvMax);
    const float2 dx = float2(inverseTextureSize.x, 0.0);
    const float2 dy = float2(0.0, inverseTextureSize.y);

    const float4 center = sampleClamped(uv, uvMin, uvMax);
    const float4 northColor = sampleClamped(uv - dy, uvMin, uvMax);
    const float4 southColor = sampleClamped(uv + dy, uvMin, uvMax);
    const float4 westColor = sampleClamped(uv - dx, uvMin, uvMax);
    const float4 eastColor = sampleClamped(uv + dx, uvMin, uvMax);
    const float lumaM = fxaaLuma(center.rgb);
    const float lumaN = fxaaLuma(northColor.rgb);
    const float lumaS = fxaaLuma(southColor.rgb);
    const float lumaW = fxaaLuma(westColor.rgb);
    const float lumaE = fxaaLuma(eastColor.rgb);
    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float lumaRange = lumaMax - lumaMin;
    if (lumaRange < max(edgeThresholdMin, lumaMax * edgeThreshold)) {
        return center;
    }

    const float lumaNW = fxaaLuma(sampleClamped(uv - dx - dy, uvMin, uvMax).rgb);
    const float lumaNE = fxaaLuma(sampleClamped(uv + dx - dy, uvMin, uvMax).rgb);
    const float lumaSW = fxaaLuma(sampleClamped(uv - dx + dy, uvMin, uvMax).rgb);
    const float lumaSE = fxaaLuma(sampleClamped(uv + dx + dy, uvMin, uvMax).rgb);
    const float horizontal = abs(lumaN + lumaS - 2.0 * lumaM) * 2.0 +
        abs(lumaNW + lumaSW - 2.0 * lumaW) +
        abs(lumaNE + lumaSE - 2.0 * lumaE);
    const float vertical = abs(lumaW + lumaE - 2.0 * lumaM) * 2.0 +
        abs(lumaNW + lumaNE - 2.0 * lumaN) +
        abs(lumaSW + lumaSE - 2.0 * lumaS);
    const float2 tangent = horizontal >= vertical ? dx : dy;
    const float3 nearEdge = 0.5 * (
        sampleClamped(uv - tangent * 0.5, uvMin, uvMax).rgb +
        sampleClamped(uv + tangent * 0.5, uvMin, uvMax).rgb);
    const float3 farEdge = 0.5 * (
        sampleClamped(uv - tangent * 1.5, uvMin, uvMax).rgb +
        sampleClamped(uv + tangent * 1.5, uvMin, uvMax).rgb);
    const float3 edgeColor = lerp(farEdge, nearEdge, 0.75);
    const float neighborhood = (lumaN + lumaS + lumaW + lumaE) * 0.25;
    float subpixel = saturate(abs(neighborhood - lumaM) / max(lumaRange, 1e-5));
    subpixel = subpixel * subpixel * (3.0 - 2.0 * subpixel);
    const float3 result = lerp(edgeColor, 0.5 * (edgeColor + center.rgb),
                               subpixel * subpixelAmount);
    return float4(result, center.a);
}
