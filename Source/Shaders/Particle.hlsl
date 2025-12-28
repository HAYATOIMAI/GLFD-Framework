
// 定数バッファ
cbuffer GlobalConstants : register(b0)
{
    float AspectRatio;
    float Time;
    float2 Padding;
};

// テクスチャとサンプラー
Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

struct VS_INPUT
{
    float4 Pos : POSITION;
    float4 Color : COLOR;
};
struct GS_INPUT
{
    float4 Pos : SV_POSITION;
    float4 Color : COLOR;
};
struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float4 Color : COLOR;
    float2 UV : TEXCOORD0;
};

// [VS]
GS_INPUT VS(VS_INPUT input)
{
    GS_INPUT output;
    output.Pos = input.Pos;
    output.Color = input.Color;
    return output;
}

// [GS]
[maxvertexcount(4)]
void GS(point GS_INPUT input[1], inout TriangleStream<PS_INPUT> outputStream)
{
    PS_INPUT outVert;
    
    // 時間変化
    float timeFactor = (sin(Time * 3.0) + 1.0) * 0.5;
    outVert.Color = input[0].Color;

    float size = 0.05;
    float sizeX = size / AspectRatio;
    float sizeY = size;
    float4 center = input[0].Pos;

    // 左下
    outVert.Pos = center + float4(-sizeX, -sizeY, 0, 0);
    outVert.UV = float2(0, 1);
    outputStream.Append(outVert);

    // 左上
    outVert.Pos = center + float4(-sizeX, sizeY, 0, 0);
    outVert.UV = float2(0, 0);
    outputStream.Append(outVert);

    // 右下
    outVert.Pos = center + float4(sizeX, -sizeY, 0, 0);
    outVert.UV = float2(1, 1);
    outputStream.Append(outVert);

    // 右上
    outVert.Pos = center + float4(sizeX, sizeY, 0, 0);
    outVert.UV = float2(1, 0);
    outputStream.Append(outVert);
    
    outputStream.RestartStrip();
}

float4 PS(PS_INPUT input) : SV_TARGET
{
    float4 texColor = g_Texture.Sample(g_Sampler, input.UV);
    return input.Color * texColor;
}