cbuffer TrailCameraConstants : register(b0) {
    row_major float4x4 viewProjection;
    float2 playableMinimum;
    float2 playableMaximum;
    uint playableBoundsEnabled;
    uint3 playablePadding;
};

Texture2D trailTexture : register(t0);
SamplerState trailSampler : register(s0);

struct VsInput {
    float3 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct PsInput {
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPosition : POSITION0;
};

PsInput VSMain(VsInput input) {
    PsInput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color = input.color;
    output.uv = input.uv;
    output.worldPosition = input.position;
    return output;
}

float4 PSMain(PsInput input) : SV_TARGET {
    if (playableBoundsEnabled != 0u &&
        (any(input.worldPosition.xy < playableMinimum) ||
         any(input.worldPosition.xy > playableMaximum))) {
        clip(-1.0);
    }
    return trailTexture.Sample(trailSampler, input.uv) * input.color;
}
