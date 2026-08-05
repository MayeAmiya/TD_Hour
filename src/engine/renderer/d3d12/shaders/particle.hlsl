cbuffer CameraConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    float2 playableMinimum;
    float2 playableMaximum;
    uint playableBoundsEnabled;
    uint3 playablePadding;
};

Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

struct ParticleVertexInput
{
    float2 corner : POSITION;
    float2 uv : TEXCOORD0;
    float3 center : INSTANCE_POSITION;
    float size : INSTANCE_SIZE;
    float3 endCenter : INSTANCE_END_POSITION;
    float endSize : INSTANCE_END_SIZE;
    float4 color : INSTANCE_COLOR;
    float4 endColor : INSTANCE_END_COLOR;
    float angle : INSTANCE_ANGLE;
    uint flags : INSTANCE_FLAGS;
};

struct ParticleVertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 worldPosition : POSITION0;
};

ParticleVertexOutput ParticleVSMain(ParticleVertexInput input)
{
    const bool streak = (input.flags & 2u) != 0u;
    if (streak)
    {
        const float along = input.corner.y + 0.5f;
        const float3 center = lerp(input.center, input.endCenter, along);
        const float3 segment = input.endCenter - input.center;
        const float2 projected = float2(dot(segment, cameraRight.xyz),
                                        dot(segment, cameraUp.xyz));
        const float projectedLengthSquared = dot(projected, projected);
        const float3 ribbonRight = projectedLengthSquared > 1.0e-8f
            ? normalize(cameraRight.xyz * -projected.y + cameraUp.xyz * projected.x)
            : cameraRight.xyz;

        ParticleVertexOutput output;
        const float3 worldPosition = center + ribbonRight * input.corner.x *
            lerp(input.size, input.endSize, along);
        output.position = mul(float4(worldPosition, 1.0f), viewProjection);
        output.uv = float2(input.uv.x, 1.0f - along);
        output.color = lerp(input.color, input.endColor, along);
        output.worldPosition = worldPosition;
        return output;
    }

    const float cosineAngle = cos(input.angle);
    const float sineAngle = sin(input.angle);
    float2 rotated = float2(
        input.corner.x * cosineAngle - input.corner.y * sineAngle,
        input.corner.x * sineAngle + input.corner.y * cosineAngle) * input.size;
    if ((input.flags & 8u) != 0u)
    {
        const float diamondScale = 0.70710678f;
        rotated = float2(rotated.x - rotated.y, rotated.x + rotated.y) * diamondScale;
    }
    const bool groundAligned = (input.flags & 1u) != 0u;
    const float3 worldOffset = groundAligned
        ? float3(rotated.x, rotated.y, 0.0f)
        : cameraRight.xyz * rotated.x + cameraUp.xyz * rotated.y;

    ParticleVertexOutput output;
    const float3 worldPosition = input.center + worldOffset;
    output.position = mul(float4(worldPosition, 1.0f), viewProjection);
    output.uv = input.uv;
    output.color = input.color;
    output.worldPosition = worldPosition;
    return output;
}

void clipToPlayableBounds(float3 worldPosition)
{
    if (playableBoundsEnabled != 0u &&
        (any(worldPosition.xy < playableMinimum) ||
         any(worldPosition.xy > playableMaximum)))
    {
        clip(-1.0);
    }
}

float4 ParticlePSMain(ParticleVertexOutput input) : SV_TARGET
{
    clipToPlayableBounds(input.worldPosition);
    return sourceTexture.Sample(sourceSampler, input.uv) * input.color;
}

float4 ParticlePSAlphaTest(ParticleVertexOutput input) : SV_TARGET
{
    clipToPlayableBounds(input.worldPosition);
    const float4 sampleColor = sourceTexture.Sample(sourceSampler, input.uv);
    const float4 modulatedColor = sampleColor * input.color;
    clip(modulatedColor.a - (96.0f / 255.0f));
    return modulatedColor;
}

struct SmudgeVertexInput
{
    float3 position : POSITION;
    float2 uvOffset : TEXCOORD0;
    float alpha : ALPHA0;
};

struct SmudgeVertexOutput
{
    float4 position : SV_POSITION;
    float2 sceneUv : TEXCOORD0;
    float alpha : ALPHA0;
    float3 worldPosition : POSITION0;
};

SmudgeVertexOutput SmudgeVSMain(SmudgeVertexInput input)
{
    SmudgeVertexOutput output;
    output.position = mul(float4(input.position, 1.0f), viewProjection);
    const float inverseW = abs(output.position.w) > 1.0e-6f
        ? rcp(output.position.w) : 0.0f;
    const float2 ndc = output.position.xy * inverseW;
    output.sceneUv = ndc * float2(0.5f, -0.5f) + 0.5f + input.uvOffset;
    output.alpha = input.alpha;
    output.worldPosition = input.position;
    return output;
}

float4 SmudgePSMain(SmudgeVertexOutput input) : SV_TARGET
{
    clipToPlayableBounds(input.worldPosition);
    const float2 uv = clamp(input.sceneUv, 0.0f, 1.0f);
    return float4(sourceTexture.Sample(sourceSampler, uv).rgb,
                  saturate(input.alpha));
}
