cbuffer ShadowCameraConstants : register(b0) {
    row_major float4x4 lightViewProjection;
};

cbuffer ShadowDrawConstants : register(b1) {
    uint skinBoneCount;
    uint samplerMode;
    uint alphaTestMode;
    float alphaCutoff;
    uint detailSamplerMode;
    uint hasDetailTexture;
    uint detailAlphaFunc;
    float materialDiffuseAlpha;
    float4 mapperScaleOffset[2];
    float4 mapperMotionCenter[2];
    uint2 mapperTypes;
    uint2 mapperClampFix;
    float mapperTimeSeconds;
    float3 _mapperPadding;
};

struct SkinJointPair {
    row_major float4x4 previous;
    row_major float4x4 current;
};

cbuffer SkinPalette : register(b2) {
    SkinJointPair skinBones[256];
};

Texture2D<float4> diffuseTexture : register(t0);
Texture2D<float4> detailTexture : register(t1);
SamplerState linearWrapSampler : register(s0);
SamplerState linearClampUSampler : register(s1);
SamplerState linearClampVSampler : register(s2);
SamplerState linearClampUVSampler : register(s3);

struct VsInput {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
    float2 detailTexcoord : TEXCOORD1;
    float4 color : COLOR0;
    uint boneIndex : BLENDINDICES0;
    float4 instanceWorld0 : INSTANCEWORLD0;
    float4 instanceWorld1 : INSTANCEWORLD1;
    float4 instanceWorld2 : INSTANCEWORLD2;
    float4 instanceWorld3 : INSTANCEWORLD3;
    float4 instanceTreePush : INSTANCEEFFECT2;
    float4 previousWorld0 : PREVIOUSWORLD0;
    float4 previousWorld1 : PREVIOUSWORLD1;
    float4 previousWorld2 : PREVIOUSWORLD2;
    float4 previousWorld3 : PREVIOUSWORLD3;
    float interpolationAlpha : INTERPOLATION0;
};

struct PsInput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float2 detailTexcoord : TEXCOORD1;
    float4 color : COLOR0;
};

float2 applyShadowMapper(float2 authoredTexcoord, uint stage) {
    const uint mapperType = mapperTypes[stage];
    const float4 scaleOffset = mapperScaleOffset[stage];
    const float4 motionCenter = mapperMotionCenter[stage];
    if (mapperType == 3u) {
        float2 offset = scaleOffset.zw - motionCenter.xy * mapperTimeSeconds;
        if (mapperClampFix[stage] != 0u) {
            const float2 extent = abs(scaleOffset.xy);
            offset = clamp(offset, -extent, extent);
        } else {
            offset = frac(offset);
        }
        return authoredTexcoord * scaleOffset.xy + offset;
    }
    if (mapperType == 4u) return authoredTexcoord * scaleOffset.xy;
    if (mapperType == 5u) {
        const uint gridWidthLog2 = min(asuint(motionCenter.y), 15u);
        const uint gridWidth = 1u << gridWidthLog2;
        const uint defaultLast = gridWidth * gridWidth;
        const uint authoredLast = asuint(motionCenter.z);
        const uint lastFrame = max(
            1u, authoredLast == 0u ? defaultLast : authoredLast);
        const uint authoredOffset = asuint(motionCenter.w) % lastFrame;
        const float fps = motionCenter.x;
        uint elapsedFrames = 0u;
        if (fps != 0.0) {
            const uint millisecondsPerFrame = max(
                1u, (uint)(1000.0 / abs(fps)));
            const uint elapsedMilliseconds = (uint)floor(
                max(0.0, mapperTimeSeconds) * 1000.0);
            elapsedFrames = elapsedMilliseconds / millisecondsPerFrame;
        }
        const uint initialFrame = fps < 0.0
            ? (lastFrame - 1u - authoredOffset) % lastFrame
            : authoredOffset;
        const uint currentFrame = fps < 0.0
            ? (initialFrame + lastFrame - elapsedFrames % lastFrame) % lastFrame
            : (initialFrame + elapsedFrames) % lastFrame;
        const uint gridMask = gridWidth - 1u;
        const uint gridX = currentFrame & gridMask;
        const uint gridY = (currentFrame >> gridWidthLog2) & gridMask;
        return authoredTexcoord + float2(gridX, gridY) / (float)gridWidth;
    }
    if (mapperType == 6u) {
        const float angle = motionCenter.x * mapperTimeSeconds * 6.28318530718;
        float sineValue;
        float cosineValue;
        sincos(angle, sineValue, cosineValue);
        const float2 centered = authoredTexcoord - motionCenter.zw;
        const float2 rotated = float2(
            cosineValue * centered.x - sineValue * centered.y,
            sineValue * centered.x + cosineValue * centered.y);
        return (rotated + motionCenter.zw) * scaleOffset.xy;
    }
    // Environment mappers depend on the presentation camera and world
    // normal. Shadow alpha falls back to authored UVs rather than inventing
    // light-camera-dependent holes in an otherwise valid caster.
    return authoredTexcoord;
}

PsInput ShadowVS(VsInput input) {
    float4 previousPosition = float4(input.position, 1.0);
    float4 currentPosition = previousPosition;
    const float alpha = saturate(input.interpolationAlpha);
    const bool interpolateEndpoints = alpha < 0.999999;
    if (input.boneIndex < skinBoneCount) {
        const SkinJointPair pair = skinBones[input.boneIndex];
        currentPosition = mul(currentPosition, pair.current);
        if (interpolateEndpoints) {
            previousPosition = mul(previousPosition, pair.previous);
        }
    }
    row_major float4x4 currentWorld = float4x4(
        input.instanceWorld0, input.instanceWorld1,
        input.instanceWorld2, input.instanceWorld3);
    row_major float4x4 previousWorld = float4x4(
        input.previousWorld0, input.previousWorld1,
        input.previousWorld2, input.previousWorld3);
    float4 worldPosition = mul(currentPosition, currentWorld);
    if (interpolateEndpoints) {
        worldPosition = lerp(
            mul(previousPosition, previousWorld),
            worldPosition, alpha);
    }
    worldPosition.xy += input.position.z * input.instanceTreePush.z *
        input.instanceTreePush.w * input.instanceTreePush.xy;
    PsInput output;
    output.position = mul(worldPosition, lightViewProjection);
    output.texcoord = applyShadowMapper(input.texcoord, 0u);
    output.detailTexcoord = applyShadowMapper(input.detailTexcoord, 1u);
    output.color = input.color;
    return output;
}

float sampleAlpha(float2 texcoord) {
    if (samplerMode == 1u) return diffuseTexture.Sample(linearClampUSampler, texcoord).a;
    if (samplerMode == 2u) return diffuseTexture.Sample(linearClampVSampler, texcoord).a;
    if (samplerMode == 3u) return diffuseTexture.Sample(linearClampUVSampler, texcoord).a;
    return diffuseTexture.Sample(linearWrapSampler, texcoord).a;
}

float sampleDetailAlpha(float2 texcoord) {
    if (detailSamplerMode == 1u) return detailTexture.Sample(linearClampUSampler, texcoord).a;
    if (detailSamplerMode == 2u) return detailTexture.Sample(linearClampVSampler, texcoord).a;
    if (detailSamplerMode == 3u) return detailTexture.Sample(linearClampUVSampler, texcoord).a;
    return detailTexture.Sample(linearWrapSampler, texcoord).a;
}

float combineShadowAlpha(float local, float detail) {
    if (detailAlphaFunc == 1u) return detail;
    if (detailAlphaFunc == 2u) return local * detail;
    if (detailAlphaFunc == 3u) return local + (1.0 - local) * detail;
    return local;
}

void ShadowAlphaPS(PsInput input) {
    float alpha = sampleAlpha(input.texcoord);
    if (hasDetailTexture != 0u) {
        alpha = combineShadowAlpha(alpha, sampleDetailAlpha(input.detailTexcoord));
    }
    alpha *= input.color.a * materialDiffuseAlpha;
    if (alphaTestMode == 1u) clip(alpha - alphaCutoff);
    if (alphaTestMode == 2u) clip(alphaCutoff - alpha);
}
