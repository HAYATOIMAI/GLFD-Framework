
// 定数バッファ
cbuffer GlobalConstants : register(b0)
{
    float AspectRatio;
    float Time;
    float2 Padding;
};

// テクスチャは使わない (ECS 2-3)。PS が四角の中に丸を描く

// 頂点の Pos.z は丸の半径 (NDC の縦)。深さではない (ECS 2-3 (3))
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

    // 大きさは頂点が持つ (Pos.z)。ここに既定値を置かない
    float size = input[0].Pos.z;
    float sizeX = size / AspectRatio;
    float sizeY = size;
    // z は深さとしては使わない。0 に戻す (深さの切り取りに掛からないように)
    float4 center = float4(input[0].Pos.xy, 0.0, input[0].Pos.w);

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

// [PS] 四角の中に丸を描く (ECS 2-3)
// UV の中心からの距離が 1 を超えるところは捨てる。縁は 1 画素ぶんだけαでぼかす
float4 PS(PS_INPUT input) : SV_TARGET
{
    float2 d     = input.UV * 2.0 - 1.0;
    float  r     = length(d);
    float  edge  = max(fwidth(r), 1e-4);
    float  alpha = 1.0 - smoothstep(1.0 - edge, 1.0, r);
    clip(alpha - 0.001);
    return float4(input.Color.rgb, input.Color.a * alpha);
}