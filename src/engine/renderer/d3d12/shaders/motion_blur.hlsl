cbuffer MotionBlurConstants : register(b0) {
    // xy: recurrence centre in full-display UV; z: opaque base scale;
    // w: tactical-view height / full display height.
    float4 blurData0;
    // xy: per-tap scale; z: source alpha; w: additive branch.
    float4 blurData1;
    // x: authored recurrence count, capped to the source MAX_LIMIT (30).
    float4 blurData2;
};

Texture2D<float4> sceneTexture : register(t0);
SamplerState sceneSampler : register(s0);

struct MotionBlurVsOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

MotionBlurVsOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0),
    };
    const float2 texcoords[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0),
    };
    MotionBlurVsOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.texcoord = texcoords[vertexId];
    return output;
}

float4 PSMain(MotionBlurVsOutput input) : SV_TARGET {
    float2 uv = float2(input.texcoord.x, input.texcoord.y * blurData0.w);
    uv = (uv - blurData0.xy) * blurData0.z + blurData0.xy;
    const float4 source = sceneTexture.Sample(sceneSampler, uv);
    float3 result = source.rgb;
    // This is the DX8 quad recurrence expressed inside one shader: an opaque
    // transformed source followed by at most thirty progressively scaled
    // samples of the same captured tactical view.
    [loop]
    for (int index = 0; index < 30; ++index) {
        if (index >= int(blurData2.x + 0.5)) break;
        uv = (uv - blurData0.xy) * blurData1.xy + blurData0.xy;
        const float3 sampleColor = sceneTexture.Sample(sceneSampler, uv).rgb;
        if (blurData1.w > 0.5) {
            result += sampleColor * blurData1.z;
        } else {
            result = lerp(result, sampleColor, blurData1.z);
        }
    }
    return float4(saturate(result), source.a);
}

