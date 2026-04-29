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
    int ShadowIndex; // [복구] Padding0 자리에 ShadowIndex 삽입
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

// [복구] 기존 b6 단일 섀도우 버퍼를 삭제하고 b7 다중 섀도우 배열로 교체
struct FShadowData
{
    // Cascade 최대 개수 3 가정
    row_major float4x4 ShadowLightView[3];
    row_major float4x4 ShadowLightProjection[3];
    
    float2 UVScale;
    float2 UVOffset;
    
    float3 ShadowLightPosition;
    float ShadowFar;
    
    float ShadowBias;
    float ShadowSlopeBias;
    float2 Pad;
    
    uint ShadowMapType;
    uint SliceCount;
    uint ShadowTextureIndex;
    float1 Pad2;
    
    uint isPSM;
    float3 Pad3;
    row_major float4x4 PSM;
    
    float3 CascadeSplits;
    float PointShadowTexelSize;
};

cbuffer ShadowLightViewInfo : register(b7)
{
    FShadowData ShadowDataArray[32]; // 최대 32개의 그림자 정보 전달
}

struct FPointLightData
{
    float3 WorldPos;
    float Radius;
    float3 Color;
    float Intensity;
    int ShadowIndex; // [복구] C++ 쪽 FPointLightData에도 int ShadowIndex; float3 Padding; 추가 필수!
    float3 Padding; // 16바이트 정렬을 위한 패딩
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
    int ShadowIndex; // [복구] Padding float3 자리를 int + float2로 쪼개어 사용
    float2 Padding;
};

StructuredBuffer<FPointLightData> PointLights : register(t8);
StructuredBuffer<FSpotLightData> SpotLights : register(t9);
StructuredBuffer<uint2> TilePointLightGrid : register(t10);
StructuredBuffer<uint> TilePointLightIndices : register(t11);
StructuredBuffer<uint2> TileSpotLightGrid : register(t12);
StructuredBuffer<uint> TileSpotLightIndices : register(t13);

Texture2DArray ShadowMap2D : register(t14);
TextureCubeArray ShadowMapCube : register(t15);
Texture2DArray ShadowMap2DAtlas : register(t16);
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

float SamplePointShadowPCF(float3 SampleDir, float CurrentDepth, FShadowData SData)
{
    const float FilterRadius = max(SData.PointShadowTexelSize, 1.0e-4f);
    const float3 UpRef = (abs(SampleDir.z) < 0.99f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 Tangent = normalize(cross(UpRef, SampleDir));
    const float3 Bitangent = cross(SampleDir, Tangent);

    float Shadow = 0.0f;

    [unroll]
    for (int Y = -1; Y <= 1; ++Y)
    {
        [unroll]
        for (int X = -1; X <= 1; ++X)
        {
            const float2 Offset = float2(X, Y) * FilterRadius;
            const float3 OffsetDir = normalize(SampleDir + Tangent * Offset.x + Bitangent * Offset.y);
            const float StoredDepth = ShadowMapCube.Sample(ShadowSampler, float4(OffsetDir, SData.ShadowTextureIndex)).r;
            Shadow += (StoredDepth + SData.ShadowBias >= CurrentDepth) ? 1.0f : 0.0f;
        }
    }

    return Shadow / 9.0f;
}

float CalculateShadowFactor(float3 WorldPos, float3 N, float3 L, int ShadowIndex)
{
    if (ShadowIndex < 0)
        return 1.0f;

    FShadowData SData = ShadowDataArray[ShadowIndex];
    
    float CosTheta = saturate(dot(N, L));
    float Slope = sqrt(1 - CosTheta * CosTheta) / max(CosTheta, 1e-4);
    float FinalBias = SData.ShadowBias + Slope * SData.ShadowSlopeBias;
        
    if (SData.ShadowMapType == SHADOW_MAP_TYPE_DEPTH2D)
    {
        float ViewDepth = mul(float4(WorldPos, 1), View).x;
        int SliceIndex = 0;

        if (SliceIndex + 1 < SData.SliceCount && ViewDepth > SData.CascadeSplits.x)
            SliceIndex = 1;

        if (SliceIndex + 1 < SData.SliceCount && ViewDepth > SData.CascadeSplits.y)
            SliceIndex = 2;

        float4 ShadowLightPos;
        
        if (SData.isPSM)
        {
            ShadowLightPos = mul(float4(WorldPos, 1.0f), SData.PSM);
        }
        else
        {
            ShadowLightPos = mul(mul(float4(WorldPos, 1), SData.ShadowLightView[SliceIndex]), SData.ShadowLightProjection[SliceIndex]);
        }
        float3 NDC = ShadowLightPos.xyz / ShadowLightPos.w;
        float2 ShadowUV = NDC.xy * float2(0.5, -0.5) + 0.5;
        float CurrentDepth = NDC.z;

        if (ShadowUV.x < 0.0 || ShadowUV.x > 1.0 || ShadowUV.y < 0.0 || ShadowUV.y > 1.0 || CurrentDepth < 0.0 || CurrentDepth > 1.0)
            return 1.0f; // 빛의 범위를 벗어나면 그림자 없음

        ShadowUV = (ShadowUV * SData.UVScale) + SData.UVOffset;

        uint ShadowMapWidth = 0;
        uint ShadowMapHeight = 0;
        uint ShadowMapLayers = 0;
        const bool bUseAtlasShadowMap = (SData.ShadowTextureIndex == 1u);
        if (bUseAtlasShadowMap)
        {
            ShadowMap2DAtlas.GetDimensions(ShadowMapWidth, ShadowMapHeight, ShadowMapLayers);
        }
        else
        {
            ShadowMap2D.GetDimensions(ShadowMapWidth, ShadowMapHeight, ShadowMapLayers);
        }

        float2 TexelSize = 1.0f / max(float2(ShadowMapWidth, ShadowMapHeight), float2(1.0f, 1.0f));
        float Shadow = 0.0f;

        [unroll]
        for (int X = -1; X <= 1; ++X)
        {
            [unroll]
            for (int Y = -1; Y <= 1; ++Y)
            {
                float2 Offset = float2(X, Y) * TexelSize;
                float SampleDepth = bUseAtlasShadowMap
                    ? ShadowMap2DAtlas.Sample(ShadowSampler, float3(ShadowUV + Offset, SliceIndex)).r
                    : ShadowMap2D.Sample(ShadowSampler, float3(ShadowUV + Offset, SliceIndex)).r;
                Shadow += (SampleDepth + FinalBias >= CurrentDepth) ? 1.0f : 0.0f;
            }
        }

        return Shadow / 9.0f;
    }
    else if (SData.ShadowMapType == SHADOW_MAP_TYPE_DEPTHCUBE)
    {
        if (SData.ShadowFar <= 1.0e-4f)
        {
            return 1.0f;
        }

        const float3 ToPixel = WorldPos - SData.ShadowLightPosition;
        const float Distance = length(ToPixel);
        if (Distance <= 1.0e-4f || Distance >= SData.ShadowFar)
        {
            return 1.0f;
        }

        const float3 SampleDir = ToPixel / Distance;
        const float ViewDepth = max(abs(ToPixel.x), max(abs(ToPixel.y), abs(ToPixel.z)));
        const float PointShadowNear = 0.1f;
        if (SData.ShadowFar <= PointShadowNear + 1.0e-4f || ViewDepth <= PointShadowNear + 1.0e-4f)
        {
            return 1.0f;
        }

        const float DepthA = SData.ShadowFar / (SData.ShadowFar - PointShadowNear);
        const float DepthB = (PointShadowNear * SData.ShadowFar) / (SData.ShadowFar - PointShadowNear);
        const float CurrentDepth = DepthA - (DepthB / ViewDepth);
        if (CurrentDepth < 0.0f || CurrentDepth > 1.0f)
        {
            return 1.0f;
        }

        return SamplePointShadowPCF(SampleDir, CurrentDepth, SData);
    }

    return 1.0f;
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

// [복구] ShadowIndex 파라미터 추가 및 ShadowMask 계산
void AccumulateDirectLight(float3 WorldPos, float3 N, float3 V, float3 L, float3 LightContribution, int ShadowIndex, inout FLightingResult Result)
{
    float ShadowMask = CalculateShadowFactor(WorldPos, N, L, ShadowIndex);
#if defined(LIGHTING_MODEL_TOON)
    const float HalfLambert = dot(N, L) * 0.5f + 0.5f;

    float ToonDiffuse;
    if (HalfLambert > 0.75f)
        ToonDiffuse = 1.0f;
    else if (HalfLambert > 0.4f)
        ToonDiffuse = 0.6f;
    else
        ToonDiffuse = 0.15f;

    // 그림자 적용
    Result.Diffuse += LightContribution * ToonDiffuse * ShadowMask;
#else
        const float NdotL = saturate(dot(N, L));
    // 그림자 적용
        Result.Diffuse += LightContribution * NdotL * ShadowMask;

#if defined(LIGHTING_MODEL_GOURAUD) || defined(LIGHTING_MODEL_PHONG)
        const float3 H = normalize(L + V);
        const float SpecularPower = pow(saturate(dot(N, H)), max(Shininess, 1.0e-4f));
    // 그림자 적용
        Result.Specular += SpecularColor * LightContribution * SpecularPower * ShadowMask;
#endif
#endif
}

void AccumulateVisibleLights(float3 WorldPos, float3 N, float3 V, float2 ScreenPos, inout FLightingResult Result)
{
    if (TileCountX == 0u || TileCountY == 0u || TileSize == 0u)
    {
        return;
    }

    const uint TileX = min((uint) ScreenPos.x / TileSize, TileCountX - 1u);
    const uint TileY = min((uint) ScreenPos.y / TileSize, TileCountY - 1u);
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
        if (Distance >= Light.Radius)
            continue;

        const float3 L = ToLight / max(Distance, 1.0e-4f);
        float Att = ComputeDistanceAttenuation(Distance, Light.Radius);

        // [복구] Light.ShadowIndex 전달
        AccumulateDirectLight(WorldPos, N, V, L, Light.Color * Light.Intensity * Att, Light.ShadowIndex, Result);
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
        if (Distance >= Light.Radius)
            continue;

        const float3 L = ToLight / max(Distance, 1.0e-4f);
        float Att = ComputeDistanceAttenuation(Distance, Light.Radius);

        const float3 SpotDir = normalize(Light.Direction);
        const float CosAngle = dot(SpotDir, -L);
        float spotFactor = smoothstep(Light.OuterConeCos, Light.InnerConeCos, CosAngle);
        spotFactor *= spotFactor;
        Att *= spotFactor;

        if (Att > 0.0f)
        {
            // [복구] Light.ShadowIndex 전달
            AccumulateDirectLight(WorldPos, N, V, L, Light.Color * Light.Intensity * Att, Light.ShadowIndex, Result);
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
            AccumulateDirectLight(WorldPos, N, V, normalize(Light.Direction), LightColor, Light.ShadowIndex, Result);
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
            AccumulateDirectLight(WorldPos, N, V, normalize(Light.Direction), LightColor, Light.ShadowIndex, Result);
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
            AccumulateDirectLight(WorldPos, N, V, ToLight / max(Dist, 1.0e-4f), Light.Color * Light.Intensity * Att, Light.ShadowIndex, Result);
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
                AccumulateDirectLight(WorldPos, N, V, L, Light.Color * Light.Intensity * Att, Light.ShadowIndex, Result);
            }
        }
    }

    return Result;
}

float3 ApplyLighting(FUberSurfaceData Surface, FLightingResult Lighting)
{
    return Surface.Albedo * Lighting.Diffuse + Lighting.Specular;
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

    return ComposeOutput(Surface, ApplyLighting(Surface, Lighting));
}

#endif
