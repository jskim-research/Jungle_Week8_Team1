#include "UberSurface.hlsli"

#if !defined(LIGHTING_MODEL_GOURAUD) && !defined(LIGHTING_MODEL_LAMBERT) && !defined(LIGHTING_MODEL_PHONG)
#define LIGHTING_MODEL_PHONG 1
#endif

// ─────────────────────────────────────────────
// 기존 전체-라이트 cbuffer / 구조체 (Directional, Ambient용)
// ─────────────────────────────────────────────
cbuffer UberLighting : register(b3)
{
    uint SceneLightCount;
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

StructuredBuffer<FGPULight> SceneLights : register(t3);

cbuffer VisibleLightInfo : register(b4)
{
    uint TileCountX;
    uint TileCountY;
    uint TileSize;
    uint MaxLightsPerTile;
    uint VisibleLightCount;
    float3 _VisibleLightInfoPad0;
}

struct FVisibleLightData
{
    float3 WorldPos;
    float Radius;
    float3 Color;
    float Intensity;
    float RadiusFalloff;
    float3 Padding;
};

StructuredBuffer<FVisibleLightData> VisibleLights : register(t8);
StructuredBuffer<uint> TileVisibleLightCount : register(t9);
StructuredBuffer<uint> TileVisibleLightIndices : register(t10);

// ─────────────────────────────────────────────
// 공용 상수
// ─────────────────────────────────────────────
static const uint LIGHT_TYPE_DIRECTIONAL = 0u;
static const uint LIGHT_TYPE_POINT = 1u;
static const uint LIGHT_TYPE_SPOT = 2u;
static const uint LIGHT_TYPE_AMBIENT = 3u;
static const float3 DEFAULT_AMBIENT_COLOR = float3(0.02f, 0.02f, 0.02f);

struct FLightingResult
{
    float3 Diffuse;
    float3 Specular;
};

// ─────────────────────────────────────────────
// 거리 감쇠
// ─────────────────────────────────────────────
float ComputeDistanceAttenuation(float Distance, float Radius, float FalloffExponent)
{
    if (Radius <= 0.0f)
        return 0.0f;

    const float RadiusFactor = saturate(1.0f - (Distance / max(Radius, 1.0e-4f)));
    return pow(RadiusFactor, max(FalloffExponent, 1.0e-4f));
}

// ─────────────────────────────────────────────
// 직접광 누적 (Directional / 기존 Point·Spot 공용)
// ─────────────────────────────────────────────
void AccumulateDirectLight(float3 WorldPos, float3 N, float3 V,
                           float3 L, float3 LightContribution,
                           inout FLightingResult Result)
{
    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f)
        return;

    Result.Diffuse += LightContribution * NdotL;

#if LIGHTING_MODEL_GOURAUD || LIGHTING_MODEL_PHONG
    const float3 H = normalize(L + V);
    const float SpecularPower = pow(saturate(dot(N, H)), max(Shininess, 1.0e-4f));
    Result.Specular += SpecularColor * LightContribution * SpecularPower;
#endif
}

// ─────────────────────────────────────────────
// [NEW] Tile-Culled Point/Spot 라이트 누적
// ScreenPos : SV_Position.xy (픽셀 좌표)
// ─────────────────────────────────────────────
void AccumulateVisiblePointLights(float3 WorldPos, float3 N, float3 V,
                                  float2 ScreenPos,
                                  inout FLightingResult Result)
{
    if (VisibleLightCount == 0u || TileCountX == 0u ||
        TileCountY == 0u || TileSize == 0u || MaxLightsPerTile == 0u)
    {
        return;
    }

    const uint TileX = min((uint) ScreenPos.x / TileSize, TileCountX - 1u);
    const uint TileY = min((uint) ScreenPos.y / TileSize, TileCountY - 1u);
    const uint TileIndex = TileY * TileCountX + TileX;

    const uint LocalCount = min(TileVisibleLightCount[TileIndex], MaxLightsPerTile);
    const uint TileOffset = TileIndex * MaxLightsPerTile;

    [loop]
    for (uint VisIdx = 0u; VisIdx < LocalCount; ++VisIdx)
    {
        const uint LightIndex = TileVisibleLightIndices[TileOffset + VisIdx];
        if (LightIndex >= VisibleLightCount)
            continue;

        const FVisibleLightData Light = VisibleLights[LightIndex];

        const float3 ToLight = Light.WorldPos - WorldPos;
        const float Distance = length(ToLight);

        if (Distance <= 1.0e-4f || Distance >= Light.Radius)
            continue;

        const float3 L = ToLight / Distance;
        const float Att = ComputeDistanceAttenuation(Distance, Light.Radius, Light.RadiusFalloff);
        if (Att <= 0.0f)
            continue;

        const float3 LightContrib = Light.Color * Light.Intensity * Att;
        AccumulateDirectLight(WorldPos, N, V, L, LightContrib, Result);
    }
}

// ─────────────────────────────────────────────
// 픽셀 셰이더용 — ScreenPos 있음 (tile culling 사용)
// Directional / Ambient → SceneLights 루프
// Point / Spot          → Tile-culled VisibleLights
// ─────────────────────────────────────────────
FLightingResult EvaluateLightingFromWorld(float3 WorldPos, float3 WorldNormal, float2 ScreenPos)
{
    FLightingResult Result;
    
    Result.Diffuse = 0.0f.xxx;
    Result.Specular = 0.0f.xxx;

    const float3 N = normalize(WorldNormal);
    const float3 V = normalize(CameraPosition - WorldPos);

    float3 AmbientContribution = DEFAULT_AMBIENT_COLOR;
    uint bHasAmbientLight = 0u;

    // Directional / Ambient 만 순회 (Point·Spot 은 아래 tile path 처리)
    [loop]
    for (uint LightIndex = 0u; LightIndex < SceneLightCount; ++LightIndex)
    {
        const FGPULight Light = SceneLights[LightIndex];
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
            const float3 L = normalize(Light.Direction);
            AccumulateDirectLight(WorldPos, N, V, L, LightColor, Result);
            continue;
        }

        // Point / Spot 은 tile path에서 처리하므로 skip
    }

    Result.Diffuse += AmbientContribution;

    // Tile-culled Point / Spot
    AccumulateVisiblePointLights(WorldPos, N, V, ScreenPos, Result);

    return Result;
}

// ─────────────────────────────────────────────
// 버텍스 셰이더용 (Gouraud) — ScreenPos 없으므로
// 기존 SceneLights 전체 순회 유지
// ─────────────────────────────────────────────
FLightingResult EvaluateLightingFromWorldVertex(float3 WorldPos, float3 WorldNormal)
{
    FLightingResult Result;
    Result.Diffuse = DEFAULT_AMBIENT_COLOR;
    Result.Specular = 0.0f.xxx;

    const float3 N = normalize(WorldNormal);
    const float3 V = normalize(CameraPosition - WorldPos);

    [loop]
    for (uint LightIndex = 0u; LightIndex < SceneLightCount; ++LightIndex)
    {
        const FGPULight Light = SceneLights[LightIndex];
        const float3 LightColor = Light.Color * Light.Intensity;

        if (Light.Type == LIGHT_TYPE_AMBIENT)
            continue; // Diffuse 초기값에 ambient 포함됨

        if (Light.Type == LIGHT_TYPE_DIRECTIONAL)
        {
            const float3 L = normalize(Light.Direction);
            AccumulateDirectLight(WorldPos, N, V, L, LightColor, Result);
            continue;
        }

        if (Light.Type == LIGHT_TYPE_POINT || Light.Type == LIGHT_TYPE_SPOT)
        {
            const float3 ToLight = Light.Position - WorldPos;
            const float Distance = length(ToLight);
            if (Distance <= 1.0e-4f)
                continue;

            const float3 L = ToLight / Distance;
            float Attenuation = ComputeDistanceAttenuation(Distance, Light.Radius, Light.FalloffExponent);
            if (Attenuation <= 0.0f)
                continue;

            if (Light.Type == LIGHT_TYPE_SPOT)
            {
                const float3 LightToSurface = -L;
                const float ConeFactor = dot(normalize(Light.Direction), LightToSurface);
                const float ConeRange = max(Light.SpotInnerCos - Light.SpotOuterCos, 1.0e-4f);
                const float ConeAtt = saturate((ConeFactor - Light.SpotOuterCos) / ConeRange);
                Attenuation *= ConeAtt;
                if (Attenuation <= 0.0f)
                    continue;
            }

            AccumulateDirectLight(WorldPos, N, V, L, LightColor * Attenuation, Result);
        }
    }

    return Result;
}

// ─────────────────────────────────────────────
// ApplyLighting
// ─────────────────────────────────────────────
float3 ApplyLighting(FUberSurfaceData Surface, FLightingResult Lighting)
{
    return Surface.Albedo * Lighting.Diffuse + Lighting.Specular;
}

// ─────────────────────────────────────────────
// [DEBUG] 타일별 라이트 개수 시각화
// ─────────────────────────────────────────────
float3 GetTileDebugColor(float2 ScreenPos)
{
    if (TileCountX == 0u || TileCountY == 0u || TileSize == 0u)
        return float3(0, 0, 0);

    // 1. 현재 픽셀의 타일 인덱스 계산
    const uint TileX = min((uint) ScreenPos.x / TileSize, TileCountX - 1u);
    const uint TileY = min((uint) ScreenPos.y / TileSize, TileCountY - 1u);
    const uint TileIndex = TileY * TileCountX + TileX;

    // 2. 해당 타일의 라이트 개수 가져오기
    const uint LocalCount = TileVisibleLightCount[TileIndex];
    
    // 3. 시각화 비율 계산 (MaxLightsPerTile 대비 현재 몇 개인지)
    // 예: 라이트가 0개면 검정, Max에 가까울수록 빨강
    float Ratio = (float) LocalCount / (float) max(MaxLightsPerTile, 1u);

    // Heatmap 색상 계산 (간단한 선형 보간)
    float3 Color = float3(0, 0, 0);
    if (LocalCount > 0)
    {
        Color = lerp(float3(0, 0, 1), float3(0, 1, 0), saturate(Ratio * 2.0f)); // Blue to Green
        Color = lerp(Color, float3(1, 0, 0), saturate((Ratio - 0.5f) * 2.0f)); // Green to Red
    }

    // 타일 경계선 추가 (격자 보기)
    uint2 PixelInTile = (uint2) ScreenPos % TileSize;
    if (PixelInTile.x == 0 || PixelInTile.y == 0)
    {
        Color += float3(0.2f, 0.2f, 0.2f);
    }

    return Color;
}

// ─────────────────────────────────────────────
// Vertex Shader
// ─────────────────────────────────────────────
FUberPSInput mainVS(FUberVSInput Input)
{
    FUberPSInput Output = BuildSurfaceVertex(Input);

#if LIGHTING_MODEL_GOURAUD
    // Vertex 단계엔 tile 정보 없으므로 전체 순회 버전 사용
    const FLightingResult Lighting = EvaluateLightingFromWorldVertex(Output.WorldPos, Output.WorldNormal);
    Output.VertexDiffuseLighting  = Lighting.Diffuse;
    Output.VertexSpecularLighting = Lighting.Specular;
#endif

    return Output;
}

// ─────────────────────────────────────────────
// Pixel Shader
// ─────────────────────────────────────────────
FUberPSOutput mainPS(FUberPSInput Input)
{
    FUberSurfaceData Surface = EvaluateSurface(Input);
    FLightingResult Lighting;

#if LIGHTING_MODEL_GOURAUD
    Lighting.Diffuse  = Input.VertexDiffuseLighting;
    Lighting.Specular = Input.VertexSpecularLighting;
#elif LIGHTING_MODEL_LAMBERT
    Lighting          = EvaluateLightingFromWorld(Surface.WorldPos, Surface.WorldNormal, Input.ClipPos.xy);
    Lighting.Specular = 0.0f.xxx;
#else // PHONG
    Lighting = EvaluateLightingFromWorld(Surface.WorldPos, Surface.WorldNormal, Input.ClipPos.xy);
#endif

    // return ComposeOutput(Surface, ApplyLighting(Surface, Lighting));
    
    return ComposeOutput(Surface, GetTileDebugColor(Input.ClipPos.xy));
}

