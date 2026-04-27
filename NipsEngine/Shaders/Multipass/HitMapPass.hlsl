cbuffer HitMapInfo : register(b4)
{
    uint TileCountX;
    uint TileCountY;
    uint TileSize;
    uint MaxPointLightsPerTile;
    uint MaxSpotLightsPerTile;
    uint VisiblePointLightCount;
    uint VisibleSpotLightCount;
    uint Padding0;
}

StructuredBuffer<uint2> TilePointLightGrid : register(t10);
StructuredBuffer<uint2> TileSpotLightGrid : register(t12);

struct VS_OUTPUT
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
};

VS_OUTPUT VSMain(uint vI : SV_VertexID)
{
    VS_OUTPUT vout;
    vout.UV = float2((vI << 1) & 2, vI & 2);
    vout.Pos = float4(vout.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

float3 HeatGradient(float t)
{
    const float3 Cold = float3(0.0f, 0.25f, 1.0f);
    const float3 Warm = float3(1.0f, 0.9f, 0.1f);
    const float3 Hot = float3(1.0f, 0.1f, 0.0f);

    if (t < 0.5f)
    {
        return lerp(Cold, Warm, t * 2.0f);
    }

    return lerp(Warm, Hot, (t - 0.5f) * 2.0f);
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    if (TileCountX == 0u || TileCountY == 0u || TileSize == 0u)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const uint2 Pixel = uint2(input.Pos.xy);
    const uint TileX = min(Pixel.x / TileSize, TileCountX - 1u);
    const uint TileY = min(Pixel.y / TileSize, TileCountY - 1u);
    const uint TileIndex = TileY * TileCountX + TileX;

    const uint PointCount = TilePointLightGrid[TileIndex].y;
    const uint SpotCount = TileSpotLightGrid[TileIndex].y;
    const uint TotalCount = PointCount + SpotCount;
    if (TotalCount == 0u)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const uint MaxTotal = max(MaxPointLightsPerTile + MaxSpotLightsPerTile, 1u);
    const float Normalized = saturate((float)TotalCount / (float)MaxTotal);
    const float Heat = pow(Normalized, 0.65f);

    const float3 Color = HeatGradient(Heat);
    const float Alpha = saturate(0.12f + Heat * 0.58f);
    return float4(Color, Alpha);
}
