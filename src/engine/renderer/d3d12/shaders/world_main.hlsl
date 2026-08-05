cbuffer CameraConstants : register(b0) {
    row_major float4x4 viewProjection;
    float3 cameraPosition;
    float _cameraPadding;
    float3 cameraRight;
    float worldTimeSeconds;
    float3 cameraUp;
    float _cameraBasisPadding;
    float3 fogColor;
    float fogEnabled;
    float fogStartDistance;
    float fogEndDistance;
    float2 playableMinimum;
    float3 sceneAmbient;
    float _ambientPadding;
    float4 directionalToLight[3];
    float4 directionalDiffuse[3];
    float4 dynamicPointPositionInner[20];
    float4 dynamicPointColorOuter[20];
    float4 dynamicPointAmbient[20];
    uint dynamicPointLightCount;
    uint3 _dynamicPointLightPadding;
    row_major float4x4 shadowViewProjection;
    float2 shadowTexelSize;
    float shadowDepthBias;
    float shadowStrength;
    float2 visibilityOrigin;
    float visibilityInvCellSize;
    float visibilityEnabled;
    float2 visibilityTextureSize;
    float2 playableMaximum;
    row_major float4x4 waterReflectionViewProjection;
    uint waterReflectionEnabled;
    uint playableBoundsEnabled;
    float playableBorderFadeWidth;
    uint borderShroudEnabled;
};

cbuffer WorldConstants : register(b1) {
    row_major float4x4 world;
    float4 materialDiffuse;
    float3 materialAmbient;
    float materialShininess;
    float3 materialSpecular;
    float alphaCutoff;
    float3 materialEmissive;
    uint skinBoneCount;
    uint samplerMode;
    uint alphaTestMode;
    uint detailSamplerMode;
    uint hasDetailTexture;
    uint detailColorFunc;
    uint detailAlphaFunc;
    uint fogFunc;
    uint ignoreVertexColor;
    uint lightingEnabled;
    uint waterSurface;
    uint terrainEdgePhase;
    float directionalLightScale;
    float3 scriptFlashTint;
    float _scriptFlashTintPadding;
    float3 scriptIndicatorColor;
    uint houseColorFlags;
    uint receivesShadow;
    uint receivesVisibility;
    uint receivesDynamicPointLights;
    uint texturingEnabled;
    float4 mapperScaleOffset[2];
    float4 mapperMotionCenter[2];
    uint2 mapperTypes;
    uint2 mapperClampFix;
    float mapperTimeSeconds;
    float3 _mapperPadding;
    float3 objectDynamicAmbient;
    uint objectDynamicDiffuseCount;
    float4 objectDynamicPositionInner[4];
    float4 objectDynamicColorOuter[4];
    uint terrainMacroFlags;
    float terrainMacroScale;
    float2 terrainCloudSpeed;
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
Texture2D<float> directionalShadowMap : register(t2);
Texture2D<float> localVisibilityMap : register(t3);
Texture2D<float4> waterReflectionTexture : register(t4);
Texture2D<float4> terrainCloudTexture : register(t5);
Texture2D<float4> terrainMacroTexture : register(t6);
SamplerState linearWrapSampler : register(s0);
SamplerState linearClampUSampler : register(s1);
SamplerState linearClampVSampler : register(s2);
SamplerState linearClampUVSampler : register(s3);
SamplerComparisonState shadowComparisonSampler : register(s4);
SamplerState terrainCloudSampler : register(s5);
SamplerState terrainMacroSampler : register(s6);

float4 sampleDiffuse(float2 texcoord, uint mode) {
    if (mode == 1) return diffuseTexture.Sample(linearClampUSampler, texcoord);
    if (mode == 2) return diffuseTexture.Sample(linearClampVSampler, texcoord);
    if (mode == 3) return diffuseTexture.Sample(linearClampUVSampler, texcoord);
    return diffuseTexture.Sample(linearWrapSampler, texcoord);
}

float4 sampleDetail(float2 texcoord, uint mode) {
    if (mode == 1) return detailTexture.Sample(linearClampUSampler, texcoord);
    if (mode == 2) return detailTexture.Sample(linearClampVSampler, texcoord);
    if (mode == 3) return detailTexture.Sample(linearClampUVSampler, texcoord);
    return detailTexture.Sample(linearWrapSampler, texcoord);
}

float3 combineDetailColor(float4 local, float4 detail) {
    if (detailColorFunc == 1) return detail.rgb;
    if (detailColorFunc == 2) return local.rgb * detail.rgb;
    if (detailColorFunc == 3) return local.rgb + (1.0 - local.rgb) * detail.rgb;
    if (detailColorFunc == 4) return local.rgb + detail.rgb;
    if (detailColorFunc == 5) return local.rgb - detail.rgb;
    if (detailColorFunc == 6) return detail.rgb - local.rgb;
    if (detailColorFunc == 7) return local.a * local.rgb + (1.0 - local.a) * detail.rgb;
    if (detailColorFunc == 8) return detail.a * local.rgb + (1.0 - detail.a) * detail.rgb;
    if (detailColorFunc == 9) return local.rgb + detail.rgb - 0.5;
    if (detailColorFunc == 10) return (local.rgb + detail.rgb - 0.5) * 2.0;
    if (detailColorFunc == 11) return local.rgb * detail.rgb * 2.0;
    if (detailColorFunc == 12) return local.rgb + local.a * detail.rgb;
    return local.rgb;
}

float combineDetailAlpha(float local, float detail) {
    if (detailAlphaFunc == 1) return detail;
    if (detailAlphaFunc == 2) return local * detail;
    if (detailAlphaFunc == 3) return local + (1.0 - local) * detail;
    return local;
}

float3 rgbToHouseHsv(float3 color) {
    // Branch-free RGB/HSV helpers keep the ZHC texture path in the same
    // per-packet shader that already owns material selection. This mirrors
    // the reference alpha-texture recolour's hue replacement and saturation
    // scaling, without allocating a mutable recoloured texture per object.
    const float4 k = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    const float4 p = lerp(float4(color.bg, k.wz), float4(color.gb, k.xy),
                          step(color.b, color.g));
    const float4 q = lerp(float4(p.xyw, color.r), float4(color.r, p.yzx),
                          step(p.x, color.r));
    const float delta = q.x - min(q.w, q.y);
    return float3(abs(q.z + (q.w - q.y) / (6.0 * delta + 1e-10)),
                  delta / (q.x + 1e-10), q.x);
}

float3 houseHsvToRgb(float3 color) {
    const float4 k = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    const float3 p = abs(frac(color.xxx + k.xyz) * 6.0 - k.www);
    return color.z * lerp(k.xxx, saturate(p - k.xxx), color.y);
}

float3 recolorHouseTexture(float3 source, float3 indicator) {
    float3 sourceHsv = rgbToHouseHsv(saturate(source));
    const float3 indicatorHsv = rgbToHouseHsv(saturate(indicator));
    sourceHsv.x = indicatorHsv.x;
    sourceHsv.y *= indicatorHsv.y;
    return houseHsvToRgb(sourceHsv);
}

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texcoord : TEXCOORD0;
    float2 detailTexcoord : TEXCOORD1;
    float4 color    : COLOR0;
    uint boneIndex  : BLENDINDICES0;
    float4 instanceWorld0 : INSTANCEWORLD0;
    float4 instanceWorld1 : INSTANCEWORLD1;
    float4 instanceWorld2 : INSTANCEWORLD2;
    float4 instanceWorld3 : INSTANCEWORLD3;
    float4 instanceLighting : INSTANCELIGHTING0;
    float3 instanceIndicatorColor : INSTANCECOLOR0;
    uint instanceHouseColorFlags : INSTANCEFLAGS0;
    float instanceHeatVisionIntensity : INSTANCEEFFECT0;
    uint instanceHeatVisionMode : INSTANCEFLAGS1;
    float instanceObjectOpacity : INSTANCEEFFECT1;
    float4 instanceTreePush : INSTANCEEFFECT2;
    float instanceTreeDarkening : INSTANCEEFFECT3;
    float4 previousWorld0 : PREVIOUSWORLD0;
    float4 previousWorld1 : PREVIOUSWORLD1;
    float4 previousWorld2 : PREVIOUSWORLD2;
    float4 previousWorld3 : PREVIOUSWORLD3;
    float interpolationAlpha : INTERPOLATION0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float3 worldPosition : TEXCOORD2;
    float2 texcoord : TEXCOORD0;
    float2 detailTexcoord : TEXCOORD1;
    float4 color    : COLOR0;
    nointerpolation float4 instanceLighting : TEXCOORD3;
    nointerpolation float3 instanceIndicatorColor : TEXCOORD4;
    nointerpolation uint instanceHouseColorFlags : TEXCOORD5;
    float4 shadowPosition : TEXCOORD6;
    nointerpolation float instanceHeatVisionIntensity : TEXCOORD7;
    nointerpolation uint instanceHeatVisionMode : TEXCOORD8;
    nointerpolation float instanceObjectOpacity : TEXCOORD9;
};

float2 applyVertexMapper(float2 authoredTexcoord,
                         float3 worldPosition,
                         float3 worldNormal,
                         uint stage) {
    const uint mapperType = mapperTypes[stage];
    const float4 scaleOffset = mapperScaleOffset[stage];
    const float4 motionCenter = mapperMotionCenter[stage];
    if (mapperType == 1u || mapperType == 2u) {
        const float3 normal = normalize(worldNormal);
        float3 mappingVector = normal;
        if (mapperType == 1u) {
            const float3 incident = normalize(worldPosition - cameraPosition);
            mappingVector = reflect(incident, normal);
        }
        return float2(dot(mappingVector, cameraRight),
                      dot(mappingVector, cameraUp)) * 0.5 + 0.5;
    }
    if (mapperType == 3u) {
        float2 offset = scaleOffset.zw - motionCenter.xy * mapperTimeSeconds;
        if (mapperClampFix[stage] != 0u) {
            const float2 extent = abs(scaleOffset.xy);
            offset = clamp(offset, -extent, extent);
        } else {
            // RefCode subtracts floor rather than using fmod, preserving the
            // expected positive wrap for authored negative tread speeds.
            offset = frac(offset);
        }
        return authoredTexcoord * scaleOffset.xy + offset;
    }
    if (mapperType == 4u) {
        return authoredTexcoord * scaleOffset.xy;
    }
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
            // RefCode truncates 1000 / abs(FPS) to an integer MSPerFrame,
            // then advances by whole elapsed milliseconds.
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
    return authoredTexcoord;
}

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 currentSkinnedPosition = float4(input.position, 1.0f);
    float4 previousSkinnedPosition = currentSkinnedPosition;
    float3 currentSkinnedNormal = input.normal;
    float3 previousSkinnedNormal = input.normal;
    const float alpha = saturate(input.interpolationAlpha);
    const bool interpolateEndpoints = alpha < 0.999999;
    if (waterSurface != 0) {
        const float waterTime = directionalToLight[0].w;
        const float waveA = sin(input.position.x * 0.070 + input.position.y * 0.035 + waterTime * 1.40);
        const float waveB = sin(input.position.x * -0.028 + input.position.y * 0.082 + waterTime * 0.92);
        // Classic flat water keeps its authored plane height stable. Moving
        // the sparse trigger-polygon vertices vertically made shallow water
        // cross the opaque terrain every frame and appear to flicker. Keep
        // animation in the normal/material response instead.
        currentSkinnedNormal = normalize(
            float3(-waveA * 0.008, -waveB * 0.010, 1.0));
        previousSkinnedNormal = currentSkinnedNormal;
    }
    if (input.boneIndex < skinBoneCount) {
        const SkinJointPair pair = skinBones[input.boneIndex];
        currentSkinnedPosition = mul(currentSkinnedPosition, pair.current);
        currentSkinnedNormal = mul(
            currentSkinnedNormal, (float3x3)pair.current);
        if (interpolateEndpoints) {
            previousSkinnedPosition = mul(
                previousSkinnedPosition, pair.previous);
            previousSkinnedNormal = mul(
                previousSkinnedNormal, (float3x3)pair.previous);
        }
    }
    if (input.instanceTreePush.z > 0.0 &&
        input.instanceTreePush.w > 0.0) {
        currentSkinnedNormal.y = 1.0 -
            input.instanceTreeDarkening * input.instanceTreePush.z;
        previousSkinnedNormal.y = currentSkinnedNormal.y;
    }
    row_major float4x4 currentWorld = float4x4(
        input.instanceWorld0, input.instanceWorld1,
        input.instanceWorld2, input.instanceWorld3);
    row_major float4x4 previousWorld = float4x4(
        input.previousWorld0, input.previousWorld1,
        input.previousWorld2, input.previousWorld3);
    const float4 currentWorldPosition =
        mul(currentSkinnedPosition, currentWorld);
    float4 worldPosition = currentWorldPosition;
    const float3 currentWorldNormal =
        mul(currentSkinnedNormal, (float3x3)currentWorld);
    float3 worldNormal = normalize(currentWorldNormal);
    if (interpolateEndpoints) {
        worldPosition = lerp(
            mul(previousSkinnedPosition, previousWorld),
            currentWorldPosition, alpha);
        worldNormal = normalize(lerp(
            mul(previousSkinnedNormal, (float3x3)previousWorld),
            currentWorldNormal, alpha));
    }
    worldPosition.xy += input.position.z * input.instanceTreePush.z *
        input.instanceTreePush.w * input.instanceTreePush.xy;
    output.position = mul(worldPosition, viewProjection);
    output.normal = worldNormal;
    output.worldPosition = worldPosition.xyz;
    output.texcoord = applyVertexMapper(
        input.texcoord, worldPosition.xyz, worldNormal, 0u);
    output.detailTexcoord = applyVertexMapper(
        input.detailTexcoord, worldPosition.xyz, worldNormal, 1u);
    output.color = input.color;
    output.instanceLighting = input.instanceLighting;
    output.instanceIndicatorColor = input.instanceIndicatorColor;
    output.instanceHouseColorFlags = input.instanceHouseColorFlags;
    output.shadowPosition = mul(worldPosition, shadowViewProjection);
    output.instanceHeatVisionIntensity = input.instanceHeatVisionIntensity;
    output.instanceHeatVisionMode = input.instanceHeatVisionMode;
    output.instanceObjectOpacity = input.instanceObjectOpacity;
    return output;
}

float directionalShadowVisibility(float4 shadowPosition) {
    if (shadowStrength <= 0.0 || receivesShadow == 0u || shadowPosition.w <= 0.0) {
        return 1.0;
    }
    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0 ||
        any(uv < 0.0) || any(uv > 1.0)) {
        return 1.0;
    }
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float2 offset = float2(x, y) * shadowTexelSize;
            visibility += directionalShadowMap.SampleCmpLevelZero(
                shadowComparisonSampler, uv + offset,
                projected.z - shadowDepthBias);
        }
    }
    visibility *= 1.0 / 9.0;
    return lerp(1.0, visibility, saturate(shadowStrength));
}

float localVisibilityMultiplier(float3 worldPosition) {
    if (receivesVisibility == 0u) return 1.0;
    const bool terrainBorderMode = receivesVisibility >= 3u;
    float playableBorderMultiplier = 1.0;
    if (playableBoundsEnabled != 0u) {
        const float2 exteriorByAxis = max(
            playableMinimum - worldPosition.xy,
            worldPosition.xy - playableMaximum);
        const float exteriorDistance = max(
            exteriorByAxis.x, exteriorByAxis.y);
        if (exteriorDistance > 0.0) {
            if (!terrainBorderMode) {
                // Object/effect content remains a strict gameplay partition:
                // exterior fragments must not write depth through blackness.
                clip(-1.0);
                return 0.0;
            }
            // Exterior terrain remains physically resident for camera pitch
            // and high zoom, but the active map rectangle owns no visible
            // colour outside its edge. The soft transition belongs on the
            // playable side of the edge, not as a strip of normally rendered
            // map leaking into the black exterior.
            if (borderShroudEnabled == 0u) return 1.0;
            return 0.0;
        }
        if (terrainBorderMode && borderShroudEnabled != 0u &&
            playableBorderFadeWidth > 0.0) {
            const float2 interiorByAxis = min(
                worldPosition.xy - playableMinimum,
                playableMaximum - worldPosition.xy);
            playableBorderMultiplier = saturate(
                min(interiorByAxis.x, interiorByAxis.y) /
                playableBorderFadeWidth);
        }
    }
    // Modes 2/4 apply only their hard/soft map boundary respectively.
    if (receivesVisibility == 2u || receivesVisibility == 4u)
        return playableBorderMultiplier;
    if (visibilityEnabled <= 0.0 ||
        visibilityInvCellSize <= 0.0 || any(visibilityTextureSize <= 0.0)) {
        return playableBorderMultiplier;
    }
    const float2 cell = (worldPosition.xy - visibilityOrigin) * visibilityInvCellSize;
    if (any(cell < 0.0) || any(cell >= visibilityTextureSize)) {
        return 0.0;
    }
    // RefCode enables the default texture filter and separately eases each
    // texel toward its target over roughly one second. The authority already
    // supplies that 0..255 temporal luminance; normalized UV sampling adds
    // the original spatial filtering without re-quantizing it to three
    // instantaneous states here.
    const float2 uv = (cell + 0.5) / visibilityTextureSize;
    return playableBorderMultiplier * saturate(localVisibilityMap.SampleLevel(
        linearClampUVSampler, uv, 0.0).r);
}

float pointLightLinearAttenuation(float distanceToLight,
                                  float innerRadius,
                                  float outerRadius) {
    const float outer = max(outerRadius, 0.0);
    const float inner = clamp(innerRadius, 0.0, outer);
    if (distanceToLight >= outer) return 0.0;
    if (distanceToLight <= inner) return 1.0;
    return saturate((outer - distanceToLight) / max(outer - inner, 0.00001));
}

float3 dynamicPointLambert(float3 worldPosition, float3 worldNormal) {
    if (receivesDynamicPointLights == 0u) {
        return 0.0;
    }
    if (receivesDynamicPointLights == 1u) {
        float3 objectLighting = objectDynamicAmbient;
        [unroll]
        for (uint lightIndex = 0u; lightIndex < 4u; ++lightIndex) {
            if (lightIndex < objectDynamicDiffuseCount) {
                const float4 positionInner =
                    objectDynamicPositionInner[lightIndex];
                const float4 colorOuter =
                    objectDynamicColorOuter[lightIndex];
                const float3 offsetToLight =
                    positionInner.xyz - worldPosition;
                const float distanceSquared = dot(offsetToLight, offsetToLight);
                if (distanceSquared < colorOuter.w * colorOuter.w) {
                    const float distanceToLight = sqrt(max(distanceSquared, 0.0));
                    const float attenuation = pointLightLinearAttenuation(
                        distanceToLight, positionInner.w, colorOuter.w);
                    const float lambert = distanceSquared <= 0.000001
                        ? 1.0
                        : saturate(dot(
                              worldNormal,
                              offsetToLight * rsqrt(distanceSquared)));
                    objectLighting += colorOuter.rgb * attenuation * lambert;
                }
            }
        }
        return objectLighting;
    }
    if (dynamicPointLightCount == 0u) return 0.0;
    float3 lighting = 0.0;
    [unroll]
    for (uint lightIndex = 0u; lightIndex < 20u; ++lightIndex) {
        if (lightIndex < dynamicPointLightCount) {
            const float4 positionInner = dynamicPointPositionInner[lightIndex];
            const float4 colorOuter = dynamicPointColorOuter[lightIndex];
            const float3 ambientColor = dynamicPointAmbient[lightIndex].rgb;
            const float3 offsetToLight = positionInner.xyz - worldPosition;
            const float distanceSquared = dot(offsetToLight, offsetToLight);
            if (distanceSquared < colorOuter.w * colorOuter.w) {
                const float distanceToLight = sqrt(max(distanceSquared, 0.0));
                const float attenuation = pointLightLinearAttenuation(
                    distanceToLight, positionInner.w, colorOuter.w);
                const float lambert = distanceSquared <= 0.000001
                    ? 1.0
                    : saturate(dot(worldNormal, offsetToLight * rsqrt(distanceSquared)));
                // Most LightPulse sources use equal ambient/diffuse. Police
                // search lights author half-strength ambient, which must stay
                // visible on uneven terrain without flattening the Lambert
                // contribution into the same colour term.
                lighting += attenuation *
                    (ambientColor + colorOuter.rgb * lambert);
            }
        }
    }
    return lighting;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float4 sampledDiffuse =
        (texturingEnabled != 0u || ignoreVertexColor == 2u)
        ? sampleDiffuse(input.texcoord, samplerMode)
        : float4(1.0, 1.0, 1.0, 1.0);
    // Texture-only diagnostic deliberately exits before every material
    // operation. It is encoded as value 2 so the existing value 1 keeps its
    // original skeleton meaning (ignore only vertex colour).
    if (ignoreVertexColor == 2) return sampledDiffuse;
    const bool heatVisionOnly = input.instanceHeatVisionMode == 2u;
    // RefCode uses _PresetAdditiveSolidShader for the replacement pass. It
    // does not sample the object's base texture or inherit its alpha holes.
    if (heatVisionOnly) sampledDiffuse = float4(1.0, 1.0, 1.0, 1.0);
    // Modern reconstruction of the dormant W3DCustomEdging capability.  Its
    // edge mask is tested before vertex diffuse alpha: 0x00 white shows the
    // blend-source pass (<= 0x7B), 0x80 black remains a base-terrain gap,
    // and coloured texels show the edge RGB pass (>= 0x84).  This gives the
    // dormant asset format a deterministic DX12 contract without reviving
    // DX8 state caching or exposing a live height map to rendering.
    if (terrainEdgePhase == 1) {
        const float edgeAlpha = sampleDetail(input.detailTexcoord, detailSamplerMode).a;
        clip((0x7B / 255.0) - edgeAlpha);
    }
    if (terrainEdgePhase == 2) {
        clip(sampledDiffuse.a - (0x84 / 255.0));
    }
    const float4 vertexColor = ignoreVertexColor != 0 || heatVisionOnly
        ? float4(1.0, 1.0, 1.0, 1.0) : input.color;
    const bool primaryGradientDisabled = ignoreVertexColor == 3;
    // Texture stages operate before the material/light response. This keeps
    // an explicitly lit W3D material from applying its diffuse value both to
    // the sampled texel and to the directional term.
    float4 surface = sampledDiffuse;
    if (waterSurface != 0u) {
        // W3DWater overrides stage-0 alpha with an ADD operation. Authored
        // standing-water opacity therefore comes from vertex/material alpha
        // (and the soft-edge depth response), not by multiplying the colour
        // texture's incidental alpha channel into it. Keep texture RGB while
        // allowing the packet's explicit opacity contract to control alpha.
        surface.a = 1.0;
    }
    if (!heatVisionOnly && (input.instanceHouseColorFlags & 2u) != 0u) {
        const bool inverseAlphaMask =
            (input.instanceHouseColorFlags & 4u) != 0u;
        const float3 recolored = recolorHouseTexture(
            surface.rgb, input.instanceIndicatorColor);
        if (inverseAlphaMask) {
            // RefCode treats each ZHCA source texel whose inverse alpha is
            // non-zero as house-coloured, then forces the generated texture
            // opaque before filtering. Sampling the source texture directly
            // exposes the filtered inverse mask, so blending by 1-alpha is
            // the shader-equivalent edge response instead of turning any
            // fractional footprint into a fully recoloured block.
            surface.rgb = lerp(
                surface.rgb, recolored, saturate(1.0 - sampledDiffuse.a));
            surface.a = 1.0;
        } else {
            surface.rgb = recolored;
        }
    }
    // TerrainTextureClass copies source RGB into its atlas with an opaque
    // alpha channel.  The custom-edge source phase therefore must not inherit
    // an authored 32-bit TGA alpha; only its explicit 0x80 vertex alpha
    // controls the blend response after the edge-mask clip above.
    if (terrainEdgePhase == 1) surface.a = 1.0;
    if (!heatVisionOnly && texturingEnabled != 0u && hasDetailTexture != 0) {
        const float4 detail = sampleDetail(input.detailTexcoord, detailSamplerMode);
        surface.rgb = combineDetailColor(surface, detail);
        surface.a = combineDetailAlpha(surface.a, detail.a);
    }
    if (!heatVisionOnly && terrainMacroFlags != 0u) {
        const float2 macroUv = input.worldPosition.xy * terrainMacroScale;
        if ((terrainMacroFlags & 1u) != 0u) {
            // RefCode's TerrainShader2Stage advances TSCloudMed at
            // (-0.02, -0.03) UV/second and wraps it over a 63/2-cell span.
            // The packet clock is simulation-derived, so pause/replay/epoch
            // do not consult wall time while recording commands.
            surface.rgb *= terrainCloudTexture.Sample(
                terrainCloudSampler,
                macroUv + terrainCloudSpeed * mapperTimeSeconds).rgb;
        }
        if ((terrainMacroFlags & 2u) != 0u) {
            surface.rgb *= terrainMacroTexture.Sample(
                terrainMacroSampler, macroUv).rgb;
        }
    }
    // Preserve the texture-stage albedo before prelit terrain/road vertex RGB
    // is applied.  Dynamic light must not be multiplied by baked lighting a
    // second time.
    const float3 textureStageRgb = surface.rgb;
    // DX8 ShaderClass::GRADIENT_DISABLE selects the texture for both colour
    // and alpha.  Keep object-opacity as the later Drawable override, but do
    // not let mesh vertex diffuse leak into this texture-only stage.
    if (!primaryGradientDisabled) surface *= vertexColor;
    if (!primaryGradientDisabled) surface.a *= materialDiffuse.a;
    surface.a *= saturate(input.instanceObjectOpacity);
    if (!heatVisionOnly && alphaTestMode == 1) clip(surface.a - alphaCutoff);
    if (!heatVisionOnly && alphaTestMode == 2) clip(alphaCutoff - surface.a);
    float3 effectiveMaterialDiffuse = materialDiffuse.rgb;
    float3 effectiveMaterialAmbient = materialAmbient;
    if ((input.instanceHouseColorFlags & 1u) != 0u) {
        // Recolor_Vertex_Material replaces both ambient and diffuse with the
        // object indicator colour for HOUSECOLOR meshes. This is a value
        // override in the transient constant buffer, not a material edit.
        effectiveMaterialDiffuse = input.instanceIndicatorColor;
        effectiveMaterialAmbient = input.instanceIndicatorColor;
    }
    float3 color = primaryGradientDisabled
        ? surface.rgb : surface.rgb * effectiveMaterialDiffuse;
    const float shadowVisibility = directionalShadowVisibility(input.shadowPosition);
    if (lightingEnabled != 0 && !primaryGradientDisabled) {
        const float3 normal = normalize(input.normal);
        // W3DScene adds Drawable tint to every global directional light and
        // once more to the equivalent ambient before building the object's
        // LightEnvironment.  This matters for FRENZY's signed red bias: a
        // single post-light albedo add cannot reproduce the original.
        const float3 drawableTint = input.instanceLighting.yzw;
        // LightEnvironment clamps the completed equivalent ambient before it
        // reaches fixed-function material lighting.
        float3 lighting = effectiveMaterialAmbient *
            saturate(sceneAmbient + drawableTint);
        float primarySpecular = 0.0;
        [unroll]
        for (uint lightIndex = 0; lightIndex < 3; ++lightIndex) {
            const float3 direction = directionalToLight[lightIndex].xyz;
            const float directionLengthSquared = dot(direction, direction);
            if (directionLengthSquared <= 0.000001f) continue;
            const float diffuse = saturate(dot(normal, direction * rsqrt(directionLengthSquared)));
            // W3DScene copies each global directional light for infantry,
            // multiplies its diffuse/ambient colour by the script scale, and
            // clamps every component to one.  The reconstructed world pass
            // has no separate per-light ambient lane, so preserve the visible
            // directional part exactly while keeping sceneAmbient unchanged.
            // Keep the signed tint on each directional light.  In particular,
            // infantry FRENZY subtracts green/blue; DX8 carries those signed
            // diffuse terms into the final vertex-light sum instead of
            // clamping every light independently.
            const float3 lightColor =
                directionalDiffuse[lightIndex].rgb *
                    input.instanceLighting.x + drawableTint;
            const float lightVisibility = lightIndex == 0u ? shadowVisibility : 1.0;
            lighting += effectiveMaterialDiffuse * lightColor * diffuse * lightVisibility;
            // DX8Wrapper assigns white specular only to light slot 0 when it
            // installs a LightEnvironment. Preserve that convention without
            // inventing specular for the two fill lights.
            if (lightIndex == 0 && materialShininess > 0.0) {
                const float3 toView = normalize(cameraPosition - input.worldPosition);
                const float3 halfVector = normalize(direction * rsqrt(directionLengthSquared) + toView);
                primarySpecular = pow(saturate(dot(normal, halfVector)), materialShininess) *
                    shadowVisibility;
            }
        }
        // DX8 fixed-function lighting folds emissive into the material/vertex
        // lighting colour before texture stage 0 modulates it. Adding it after
        // the texture turns additive muzzle flashes and building light cards
        // into opaque white quads regardless of their authored texture.
        color = surface.rgb * (lighting + materialEmissive) +
            materialSpecular * primarySpecular;
    } else if (receivesShadow == 2u && !primaryGradientDisabled) {
        // Terrain and prelit W3D already contain the authored light response
        // in vertex/material colour, so their primary directional term cannot
        // be isolated exactly. Estimate its share from the sealed global
        // environment and attenuate only that share, preserving ambient/fill
        // light instead of multiplying the whole surface toward black.
        const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
        const float primaryEnergy = dot(
            saturate(directionalDiffuse[0].rgb * input.instanceLighting.x),
            lumaWeights);
        float totalEnergy = dot(saturate(sceneAmbient), lumaWeights);
        [unroll]
        for (uint lightIndex = 0; lightIndex < 3; ++lightIndex) {
            totalEnergy += dot(
                saturate(directionalDiffuse[lightIndex].rgb * input.instanceLighting.x),
                lumaWeights);
        }
        const float primaryShare = saturate(primaryEnergy / max(totalEnergy, 0.0001));
        color *= lerp(1.0, shadowVisibility, primaryShare);
    }
    if (receivesDynamicPointLights != 0u && !primaryGradientDisabled) {
        const float3 pointLighting = dynamicPointLambert(
            input.worldPosition, normalize(input.normal));
        const float3 dynamicAlbedo = lightingEnabled != 0u
            ? surface.rgb * effectiveMaterialDiffuse
            : textureStageRgb * effectiveMaterialDiffuse;
        color += dynamicAlbedo * pointLighting;
    }
    if (waterSurface != 0) {
        const float3 normal = normalize(input.normal);
        const float3 viewDirection = normalize(cameraPosition - input.worldPosition);
        const float fresnel = pow(1.0 - saturate(dot(normal, viewDirection)), 3.0);
        const float ripple = 0.5 + 0.5 * sin(input.worldPosition.x * 0.070 +
                                               input.worldPosition.y * 0.040 +
                                               worldTimeSeconds * 1.40);
        // WaterSet supplies the sampled texture, time-of-day vertex gradient
        // and diffuse colour through this immutable packet. Preserve those
        // authored values as the base response and use Fresnel/ripple only
        // as a bounded reflection lift; fixed blue constants made every map
        // and time of day converge to the same material.
        const float3 authoredWater = max(color, 0.0);
        float3 reflectionSource = sampledDiffuse.rgb;
        if (waterReflectionEnabled != 0u) {
            const float4 reflectedClip = mul(
                float4(input.worldPosition, 1.0),
                waterReflectionViewProjection);
            const float reciprocalW = rcp(max(abs(reflectedClip.w), 0.00001));
            float2 reflectionUv = reflectedClip.xy * reciprocalW;
            reflectionUv = reflectionUv * float2(0.5, -0.5) + 0.5;
            const float2 rippleOffset = float2(
                normal.x + sin(worldTimeSeconds * 0.73 + input.worldPosition.y * 0.025),
                normal.y + cos(worldTimeSeconds * 0.61 + input.worldPosition.x * 0.021)) * 0.012;
            reflectionSource = waterReflectionTexture.Sample(
                linearClampUVSampler, reflectionUv + rippleOffset).rgb;
        }
        const float3 reflectionLift = saturate(
            authoredWater + reflectionSource * (0.16 + ripple * 0.10));
        color = lerp(authoredWater * (0.86 + ripple * 0.08),
                     reflectionLift,
                     saturate(fresnel * 0.72 + ripple * 0.08));
    }
    const float heatVisionIntensity = saturate(input.instanceHeatVisionIntensity);
    if (heatVisionIntensity > 0.0) {
        // RefCode's pass uses ambient black, a very small warm diffuse term
        // and orange emissive (0.5, 0.2, 0). Reconstruct that response in a
        // packet-level additive pass so it remains compatible with rigid/skin
        // instancing and needs no mutable DX8-style material stack.
        // materialPassEmissiveOverride scales only emissive in the original;
        // the small lit diffuse response remains present throughout the
        // detector fade.
        // The custom pass is an untextured, lighting-enabled material.  Its
        // small diffuse term must follow the mesh normal and the same light
        // environment; multiplying texture albedo by a constant makes even
        // back-facing surfaces glow and is not the fixed-function result.
        const float3 heatMaterialDiffuse = float3(0.02, 0.01, 0.0);
        const float3 heatNormal = normalize(input.normal);
        float3 heatDiffuseLighting = 0.0;
        [unroll]
        for (uint lightIndex = 0; lightIndex < 3; ++lightIndex) {
            const float3 direction = directionalToLight[lightIndex].xyz;
            const float directionLengthSquared = dot(direction, direction);
            if (directionLengthSquared <= 0.000001f) continue;
            const float diffuse = saturate(dot(
                heatNormal, direction * rsqrt(directionLengthSquared)));
            const float3 lightColor =
                directionalDiffuse[lightIndex].rgb *
                    input.instanceLighting.x + input.instanceLighting.yzw;
            const float lightVisibility =
                lightIndex == 0u ? shadowVisibility : 1.0;
            heatDiffuseLighting +=
                heatMaterialDiffuse * lightColor * diffuse * lightVisibility;
        }
        if (receivesDynamicPointLights != 0u) {
            heatDiffuseLighting += heatMaterialDiffuse * dynamicPointLambert(
                input.worldPosition, heatNormal);
        }
        const float3 heatVisionColor = heatDiffuseLighting +
            float3(0.5, 0.2, 0.0) * heatVisionIntensity;
        if (input.instanceHeatVisionMode == 2u) {
            // The base pass is explicitly skipped for enemy-detected stealth.
            // Fade only the additive emissive response; never blend the
            // suppressed ordinary material back into view.
            color = heatVisionColor;
        } else {
            color += heatVisionColor;
        }
    }
    if (fogEnabled != 0.0 && fogFunc != 0) {
        const float range = max(fogEndDistance - fogStartDistance, 0.0001);
        const float amount = saturate((distance(cameraPosition, input.worldPosition) - fogStartDistance) / range);
        if (fogFunc == 1) color = lerp(color, fogColor, amount);
        if (fogFunc == 2) color *= 1.0 - amount;
        if (fogFunc == 3) color = lerp(color, float3(1.0, 1.0, 1.0), amount);
    }
    color *= localVisibilityMultiplier(input.worldPosition);
    return float4(color, surface.a);
}
