cbuffer CB : register(b0) {
    float2 resolution;
    float2 _pad;
};

struct VSInput {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.pos = float4(
        (input.pos.x / resolution.x) * 2.0 - 1.0,
        1.0 - (input.pos.y / resolution.y) * 2.0,
        0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSSolid(PSInput input) : SV_Target {
    return input.color;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSTextured(PSInput input) : SV_Target {
    float4 color = tex0.Sample(samp0, input.uv) * input.color;
    clip(color.a - (1.0 / 255.0));
    return color;
}
