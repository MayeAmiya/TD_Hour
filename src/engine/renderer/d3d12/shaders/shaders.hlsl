// 2D rendering shaders for GeneralsTD
// Transforms from 800×600 virtual coords to NDC

cbuffer CB : register(b0) {
    float2 resolution;  // (800, 600)
    float2 padding;
};

struct VSInput {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 color : COLOR0;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 color : COLOR0;
};

// Vertex shader — orthographic projection from 800×600 to NDC [-1,1]
PSInput VSMain(VSInput input) {
    PSInput output;

    // Map x: [0, 800] → [-1, 1]
    // Map y: [0, 600] → [1, -1] (flip Y so 0=top)
    float ndcX = (input.pos.x / resolution.x) * 2.0 - 1.0;
    float ndcY = 1.0 - (input.pos.y / resolution.y) * 2.0;

    output.pos = float4(ndcX, ndcY, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;

    return output;
}

// Solid color pixel shader
float4 PSSolid(PSInput input) : SV_Target {
    return input.color;
}

// Textured pixel shader
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSTextured(PSInput input) : SV_Target {
    float4 color = tex0.Sample(samp0, input.uv) * input.color;
    clip(color.a - (1.0 / 255.0));
    return color;
}
