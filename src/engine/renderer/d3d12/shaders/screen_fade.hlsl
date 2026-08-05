cbuffer ScreenFadeConstants : register(b0) {
    float screenFadeIntensity;
};

struct FadeVsOutput {
    float4 position : SV_POSITION;
};

FadeVsOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0),
    };
    FadeVsOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

float4 PSMain(FadeVsOutput input) : SV_TARGET {
    return float4(screenFadeIntensity, screenFadeIntensity, screenFadeIntensity, 1.0);
}

