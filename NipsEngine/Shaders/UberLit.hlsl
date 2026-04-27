#include "UberSurface.hlsli"

#if !defined(MATERIAL_DOMAIN_DECAL) && !defined(LIGHTING_MODEL_GOURAUD) && !defined(LIGHTING_MODEL_LAMBERT) && !defined(LIGHTING_MODEL_PHONG) && !defined(LIGHTING_MODEL_TOON)
#define LIGHTING_MODEL_PHONG 1
#endif

cbuffer UberLighting : register(b3)
{
    uint SceneGlobalLightCount;
    float3 _UberLightingPad0;
}

struct FGPULight
{
    uint Type;
    float Intensity;
    float Radius;
    float FalloffExponent;

    float3 Color;
    float SpotInnerCos;

    float3 Position;
    float SpotOuterCos;

    float3 Direction;
    float Padding0;
};

StructuredBuffer<FGPULight> GlobalLights : register(t3);

cbuffer VisibleLightInfo : register(b4)
{
    uint TileCountX;
    uint TileCountY;
    uint TileSize;
    uint MaxPointLightsPerTile;
    uint MaxSpotLightsPerTile;
    uint PointLightCount;
    uint SpotLightCount;
    float _VisibleLightInfoPad;
}

cbuffer ShadowLightViewInfo : register(b6)
{
    row_major float4x4 ShadowLightView;
    row_major float4x4 ShadowLightProjection;
    row_major float4x4 ShadowCubeViewProjection[6];
    float3 ShadowLightPosition;
    float ShadowFar;
    float ShadowBias;
    uint ShadowMapType;
    uint ShadowedVisibleLightIndex;
    uint bShadowEnabled;
    uint _ShadowPad0;
}

struct FPointLightData
{
    float3 WorldPos;
    float Radius;
    float3 Color;
    float Intensity;
};

struct FSpotLightData
{
    float3 WorldPos;
    float Radius;
    float3 Color;
    float Intensity;
    float3 Direction;
    float InnerConeCos;
    float OuterConeCos;
    float3 Padding;
};

// 2.5D Light Culling Buffers (Aligned with SceneLightBinding.h: PSSetShaderResources(8, 6, SRVs))
// t8: PointLightBuffer
// t9: SpotLightBuffer
// t10: TilePointLightGrid
// t11: TilePointLightIndices
// t12: TileSpotLightGrid
// t13: TileSpotLightIndices
StructuredBuffer<FPointLightData> PointLights : register(t8);
StructuredBuffer<FSpotLightData> SpotLights : register(t9);
StructuredBuffer<uint2> TilePointLightGrid : register(t10);
StructuredBuffer<uint> TilePointLightIndices : register(t11);
StructuredBuffer<uint2> TileSpotLightGrid : register(t12);
StructuredBuffer<uint> TileSpotLightIndices : register(t13);

// Shadow resources are separated from the 2.5D culling SRV range.
Texture2D ShadowMap2D : register(t14);
TextureCube ShadowMapCube : register(t15);
SamplerState ShadowSampler : register(s1);

static const uint LIGHT_TYPE_DIRECTIONAL = 0u;
static const uint LIGHT_TYPE_POINT = 1u;
static const uint LIGHT_TYPE_SPOT = 2u;
static const uint LIGHT_TYPE_AMBIENT = 3u;
static const uint SHADOW_MAP_TYPE_NONE = 0u;
static const uint SHADOW_MAP_TYPE_DEPTH2D = 1u;
static const uint SHADOW_MAP_TYPE_DEPTHCUBE = 2u;
static const float3 DEFAULT_AMBIENT_COLOR = float3(0.02f, 0.02f, 0.02f);

struct FLightingResult
{
    float3 Diffuse;
    float3 Specular;
};

float ComputePointShadowFactor(float3 WorldPos)
{
    if (bShadowEnabled == 0u || ShadowMapType != SHADOW_MAP_TYPE_DEPTHCUBE || ShadowFar <= 1.0e-4f)
    {
        return 1.0f;
    }

    const float3 ToPixel = WorldPos - ShadowLightPosition;
    const float Distance = length(ToPixel);
    if (Distance <= 1.0e-4f || Distance >= ShadowFar)
    {
        return 1.0f;
    }

    const float3 SampleDir = ToPixel / Distance;

    // Reconstruct depth using the major axis chosen by cube-map sampling.
    const float ViewDepth = max(abs(ToPixel.x), max(abs(ToPixel.y), abs(ToPixel.z)));
    const float PointShadowNear = 0.1f;
    if (ShadowFar <= PointShadowNear + 1.0e-4f)
    {
        return 1.0f;
    }

    if (ViewDepth <= PointShadowNear + 1.0e-4f)
    {
        return 1.0f;
    }

    const float DepthA = ShadowFar / (ShadowFar - PointShadowNear);
    const float DepthB = (PointShadowNear * ShadowFar) / (ShadowFar - PointShadowNear);
    const float CurrentDepth = DepthA - (DepthB / ViewDepth);
    if (CurrentDepth < 0.0f || CurrentDepth > 1.0f)
    {
        return 1.0f;
    }

    const float StoredDepth = ShadowMapCube.Sample(ShadowSampler, SampleDir).r;
    return (StoredDepth + ShadowBias >= CurrentDepth) ? 1.0f : 0.0f;
}

float ComputeDistanceAttenuation(float Distance, float Radius)
{
    if (Radius <= 0.0f)
    {
        return 0.0f;
    }

    const float T = saturate(1.0f - (Distance / max(Radius, 1.0e-4f)));
    return T * T; // Quadratic falloff
}

void AccumulateDirectLight(float3 WorldPos, float3 N, float3 V, float3 L, float3 LightContribution, inout FLightingResult Result)
{
#if defined(LIGHTING_MODEL_TOON)
    const float HalfLambert = dot(N, L) * 0.5f + 0.5f;

    float ToonDiffuse;
    if (HalfLambert > 0.75f)
        ToonDiffuse = 1.0f;
    else if (HalfLambert > 0.4f)
        ToonDiffuse = 0.6f;
    else
        ToonDiffuse = 0.15f;

    Result.Diffuse += LightContribution * ToonDiffuse;
#else
    const float NdotL = saturate(dot(N, L));
    Result.Diffuse += LightContribution * NdotL;

#if defined(LIGHTING_MODEL_GOURAUD) || defined(LIGHTING_MODEL_PHONG)
    const float3 H = normalize(L + V);
    const float SpecularPower = pow(saturate(dot(N, H)), max(Shininess, 1.0e-4f));
    Result.Specular += SpecularColor * LightContribution * SpecularPower;
#endif
#endif
}

void AccumulateVisibleLights(float3 WorldPos, float3 N, float3 V, float2 ScreenPos, inout FLightingResult Result)
{
    if (TileCountX == 0u || TileCountY == 0u || TileSize == 0u)
    {
        return;
    }

    const uint TileX = min((uint)ScreenPos.x / TileSize, TileCountX - 1u);
    const uint TileY = min((uint)ScreenPos.y / TileSize, TileCountY - 1u);
    const uint TileIndex = TileY * TileCountX + TileX;

    // --- Point Lights ---
    const uint2 PointGrid = TilePointLightGrid[TileIndex];
    const uint PointOffset = PointGrid.x;
    const uint LocalPointCount = PointGrid.y;

    [loop]
    for (uint pIdx = 0u; pIdx < LocalPointCount; ++pIdx)
    {
        const uint LightIndex = TilePointLightIndices[PointOffset + pIdx];
        const FPointLightData Light = PointLights[LightIndex];

        const float3 ToLight = Light.WorldPos - WorldPos;
        const float Distance = length(ToLight);
        if (Distance >= Light.Radius) continue;

        const float3 L = ToLight / max(Distance, 1.0e-4f);
        float Att = ComputeDistanceAttenuation(Distance, Light.Radius);

        if (bShadowEnabled != 0u && ShadowMapType == SHADOW_MAP_TYPE_DEPTHCUBE)
        {
            const bool bMatchesShadowLight =
                all(abs(Light.WorldPos - ShadowLightPosition) < float3(0.01f, 0.01f, 0.01f)) &&
                abs(Light.Radius - ShadowFar) < 0.01f;

            if (bMatchesShadowLight)
            {
                Att *= ComputePointShadowFactor(WorldPos);
            }

            if (Att <= 0.0f)
            {
                continue;
            }
        }

        AccumulateDirectLight(WorldPos, N, V, L, Light.Color * Light.Intensity * Att, Result);
    }

    // --- Spot Lights ---
    const uint2 SpotGrid = TileSpotLightGrid[TileIndex];
    const uint SpotOffset = SpotGrid.x;
    const uint LocalSpotCount = SpotGrid.y;

    [loop]
    for (uint sIdx = 0u; sIdx < LocalSpotCount; ++sIdx)
    {
        const uint LightIndex = TileSpotLightIndices[SpotOffset + sIdx];
        const FSpotLightData Light = SpotLights[LightIndex];

        const float3 ToLight = Light.WorldPos - WorldPos;
        const float Distance = length(ToLight);
        if (Distance >= Light.Radius) continue;

        const float3 L = ToLight / max(Distance, 1.0e-4f);
        float Att = ComputeDistanceAttenuation(Distance, Light.Radius);

        const float3 SpotDir = normalize(Light.Direction);
        const float CosAngle = dot(SpotDir, -L);
        float spotFactor = smoothstep(Light.OuterConeCos, Light.InnerConeCos, CosAngle);
        spotFactor *= spotFactor;
        Att *= spotFactor;

        if (Att > 0.0f)
        {
            AccumulateDirectLight(WorldPos, N, V, L, Light.Color * Light.Intensity * Att, Result);
        }
    }
}

FLightingResult EvaluateLightingFromWorld(float3 WorldPos, float3 WorldNormal, float2 ScreenPos)
{
    FLightingResult Result;
    Result.Diffuse = 0.0f.xxx;
    Result.Specular = 0.0f.xxx;

    const float3 N = normalize(WorldNormal);
    const float3 V = normalize(CameraPosition - WorldPos);

    float3 AmbientContribution = DEFAULT_AMBIENT_COLOR;
    uint bHasAmbientLight = 0u;

    [loop]
    for (uint LightIndex = 0u; LightIndex < SceneGlobalLightCount; ++LightIndex)
    {
        const FGPULight Light = GlobalLights[LightIndex];
        const float3 LightColor = Light.Color * Light.Intensity;

        if (Light.Type == LIGHT_TYPE_AMBIENT)
        {
            if (bHasAmbientLight == 0u)
            {
                AmbientContribution = 0.0f.xxx;
                bHasAmbientLight = 1u;
            }

            AmbientContribution += LightColor;
            continue;
        }

        if (Light.Type == LIGHT_TYPE_DIRECTIONAL)
        {
            AccumulateDirectLight(WorldPos, N, V, normalize(Light.Direction), LightColor, Result);
        }
    }

    Result.Diffuse += AmbientContribution;
    AccumulateVisibleLights(WorldPos, N, V, ScreenPos, Result);

    return Result;
}

FLightingResult EvaluateLightingFromWorldVertex(float3 WorldPos, float3 WorldNormal)
{
    FLightingResult Result;
    Result.Diffuse = DEFAULT_AMBIENT_COLOR;
    Result.Specular = 0.0f.xxx;

    const float3 N = normalize(WorldNormal);
    const float3 V = normalize(CameraPosition - WorldPos);

    float3 AmbientAccum = 0.0f.xxx;
    uint HasAmbient = 0u;

    [loop]
    for (uint i = 0u; i < SceneGlobalLightCount; ++i)
    {
        const FGPULight Light = GlobalLights[i];
        const float3 LightColor = Light.Color * Light.Intensity;

        if (Light.Type == LIGHT_TYPE_AMBIENT)
        {
            if (HasAmbient == 0u)
            {
                AmbientAccum = 0.0f.xxx;
                HasAmbient = 1u;
            }
            AmbientAccum += LightColor;
            continue;
        }

        if (Light.Type == LIGHT_TYPE_DIRECTIONAL)
        {
            AccumulateDirectLight(WorldPos, N, V, normalize(Light.Direction), LightColor, Result);
        }
    }

    Result.Diffuse += AmbientAccum;

    [loop]
    for (uint p = 0u; p < PointLightCount; ++p)
    {
        const FPointLightData Light = PointLights[p];
        const float3 ToLight = Light.WorldPos - WorldPos;
        const float Dist = length(ToLight);
        if (Dist < Light.Radius)
        {
            float Att = ComputeDistanceAttenuation(Dist, Light.Radius);
            AccumulateDirectLight(WorldPos, N, V, ToLight / max(Dist, 1.0e-4f), Light.Color * Light.Intensity * Att, Result);
        }
    }

    [loop]
    for (uint s = 0u; s < SpotLightCount; ++s)
    {
        const FSpotLightData Light = SpotLights[s];
        const float3 ToLight = Light.WorldPos - WorldPos;
        const float Dist = length(ToLight);
        if (Dist < Light.Radius)
        {
            float Att = ComputeDistanceAttenuation(Dist, Light.Radius);
            const float3 L = ToLight / max(Dist, 1.0e-4f);
            const float3 SpotDir = normalize(Light.Direction);
            const float CosAngle = dot(SpotDir, -L);
            float spotFactor = smoothstep(Light.OuterConeCos, Light.InnerConeCos, CosAngle);
            spotFactor *= spotFactor;
            Att *= spotFactor;

            if (Att > 0.0f)
            {
                AccumulateDirectLight(WorldPos, N, V, L, Light.Color * Light.Intensity * Att, Result);
            }
        }
    }

    return Result;
}

float3 ApplyLighting(FUberSurfaceData Surface, FLightingResult Lighting)
{
    return Surface.Albedo * Lighting.Diffuse + Lighting.Specular;
}

float3 ApplyShadow(FUberSurfaceData Surface, float3 ColorAfterLighting)
{
    if (bShadowEnabled == 0u || ShadowMapType != SHADOW_MAP_TYPE_DEPTH2D)
    {
        return ColorAfterLighting;
    }

    float4 ShadowLightPos = mul(mul(float4(Surface.WorldPos, 1), ShadowLightView), ShadowLightProjection);

    float3 NDC = ShadowLightPos.xyz / ShadowLightPos.w;
    float2 ShadowUV = NDC.xy * float2(0.5, -0.5) + 0.5;
    float CurrentDepth = NDC.z;

    if (ShadowUV.x < 0.0 || ShadowUV.x > 1.0 ||
        ShadowUV.y < 0.0 || ShadowUV.y > 1.0 ||
        CurrentDepth < 0.0 || CurrentDepth > 1.0)
        return ColorAfterLighting;

    float ShadowLightDepth = ShadowMap2D.Sample(ShadowSampler, ShadowUV);
    float ShadowFactor = (ShadowLightDepth + ShadowBias >= CurrentDepth) ? 1.0f : 0.0f;

    return ColorAfterLighting * ShadowFactor;
}

#if defined(MATERIAL_DOMAIN_DECAL)

cbuffer UberDecal : register(b5)
{
    row_major float4x4 InvDecalWorld;
}

struct FUberDecalPSOutput
{
    float4 Color : SV_TARGET0;
    float4 Normal : SV_TARGET1;
};

FUberSurfaceData EvaluateProjectedDecal(FUberPSInput Input)
{
    FUberSurfaceData Surface;
    Surface.WorldPos = Input.WorldPos;
    Surface.WorldNormal = normalize(Input.WorldNormal);
    Surface.bIsEmissive = any(EmissiveColor > 0.0f) ? 1u : 0u;

    const float4 localPos = mul(float4(Input.WorldPos, 1.0f), InvDecalWorld);
    clip(0.5f - abs(localPos.xyz));

    Surface.UV = float2(localPos.y + 0.5f, 1.0f - (localPos.z + 0.5f));
    Surface.DiffuseSample = DiffuseMap.Sample(SampleState, Surface.UV);
    Surface.WorldNormal = ResolveSurfaceWorldNormal(Input, Surface.UV, Surface.WorldNormal);
    Surface.Albedo = BaseColor * Surface.DiffuseSample.rgb;

    return Surface;
}

FUberPSInput mainVS(FUberVSInput Input)
{
    FUberPSInput Output = BuildSurfaceVertex(Input);
    Output.ClipPos.z -= 0.0001f;
    return Output;
}

FUberDecalPSOutput mainPS(FUberPSInput Input)
{
    FUberSurfaceData Surface = EvaluateProjectedDecal(Input);
    Surface.Albedo *= PrimitiveColor.rgb;

    const float Alpha = saturate(Opacity * PrimitiveColor.a * Surface.DiffuseSample.a);
    clip(Alpha - 0.001f);

    float3 FinalColor = Surface.Albedo;
    if (bLightingEnabled > 0.5f)
    {
        const FLightingResult Lighting = EvaluateLightingFromWorld(Surface.WorldPos, Surface.WorldNormal, Input.ClipPos.xy);
        FinalColor = ApplyLighting(Surface, Lighting);
    }

    if (Surface.bIsEmissive != 0u)
    {
        FinalColor += EmissiveColor * Surface.DiffuseSample.rgb * PrimitiveColor.rgb;
    }

    if (bIsWireframe > 0.5f)
    {
        FinalColor = WireframeRGB;
    }

    FUberDecalPSOutput Output;
    Output.Color = float4(FinalColor, Alpha);
    Output.Normal = float4(Surface.WorldNormal * 0.5f + 0.5f, Alpha);
    return Output;
}

#else

FUberPSInput mainVS(FUberVSInput Input)
{
    FUberPSInput Output = BuildSurfaceVertex(Input);

#if defined(LIGHTING_MODEL_GOURAUD)
    const FLightingResult Lighting = EvaluateLightingFromWorldVertex(Output.WorldPos, Output.WorldNormal);
    Output.VertexDiffuseLighting = Lighting.Diffuse;
    Output.VertexSpecularLighting = Lighting.Specular;
#endif

    return Output;
}

FUberPSOutput mainPS(FUberPSInput Input)
{
    const FUberSurfaceData Surface = EvaluateSurface(Input);
    FLightingResult Lighting;

#if defined(LIGHTING_MODEL_GOURAUD)
    Lighting.Diffuse = Input.VertexDiffuseLighting;
    Lighting.Specular = Input.VertexSpecularLighting;

#elif defined(LIGHTING_MODEL_LAMBERT)
    Lighting = EvaluateLightingFromWorld(Surface.WorldPos, Surface.WorldNormal, Input.ClipPos.xy);
    Lighting.Specular = 0.0f.xxx;
#else
    Lighting = EvaluateLightingFromWorld(Surface.WorldPos, Surface.WorldNormal, Input.ClipPos.xy);
#endif
    const float3 LitColor = ApplyLighting(Surface, Lighting);
    return ComposeOutput(Surface, ApplyShadow(Surface, LitColor));
}

#endif
