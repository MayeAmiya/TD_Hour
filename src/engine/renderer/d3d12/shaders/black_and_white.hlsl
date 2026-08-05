cbuffer BlackAndWhiteConstants : register(b0) {
    float blackAndWhiteMix;
};

Texture2D<float4> sceneTexture : register(t0);
SamplerState sceneSampler : register(s0);

struct BwVsOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

BwVsOutput VSMain(uint vertexId : SV_VertexID) {
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
    BwVsOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.texcoord = texcoords[vertexId];
    return output;
}

float4 PSMain(BwVsOutput input) : SV_TARGET {
    const float4 source = sceneTexture.Sample(sceneSampler, input.texcoord);
    const float gray = dot(source.rgb, float3(0.3, 0.59, 0.11));
    return float4(lerp(source.rgb, gray.xxx, blackAndWhiteMix), source.a);
}

