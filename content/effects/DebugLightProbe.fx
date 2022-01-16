cbuffer CBufferPerObject
{
    float4x4 ViewProjection;
    float4x4 World;
    float4 CameraPosition;
}
TextureCube CubemapTexture;

SamplerState AnisotropicSampler
{
    Filter = ANISOTROPIC;
    MaxAnisotropy = 16;
    AddressU = Wrap;
    AddressV = Wrap;
    AddressW = Wrap;
};


struct VS_INPUT
{
    float4 Position : POSITION;
    float2 Texcoord0 : TEXCOORD;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;  
};

struct VS_INPUT_INSTANCING
{
    float4 Position : POSITION;
    float2 Texcoord0 : TEXCOORD;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    
    //instancing
    row_major float4x4 World : WORLD; 
};

struct VS_OUTPUT
{
    float4 Position : SV_Position;
    float3 WorldPos : WorldPos;
    float2 UV : TexCoord0;
    float3 Normal : Normal;
    float3 Tangent : Tangent;
};


VS_OUTPUT mainVS(VS_INPUT IN)
{
    VS_OUTPUT OUT = (VS_OUTPUT) 0;

    OUT.Position = mul(IN.Position, mul(World, ViewProjection));
    OUT.WorldPos = mul(IN.Position, World).xyz;
    OUT.UV = IN.Texcoord0;
    OUT.Normal = normalize(mul(float4(IN.Normal, 0), World).xyz);
    OUT.Tangent = IN.Tangent;

    return OUT;
}

VS_OUTPUT mainVS_Instancing(VS_INPUT_INSTANCING IN)
{
    VS_OUTPUT OUT = (VS_OUTPUT) 0;

    OUT.WorldPos = mul(IN.Position, IN.World).xyz;
    OUT.Position = mul(float4(OUT.WorldPos, 1.0f), ViewProjection);
    OUT.UV = IN.Texcoord0;
    OUT.Normal = normalize(mul(float4(IN.Normal, 0), IN.World).xyz);
    OUT.Tangent = IN.Tangent;

    return OUT;
}

float3 mainPS(VS_OUTPUT vsOutput) : SV_Target0
{
    float3 viewDir = normalize(CameraPosition.xyz - vsOutput.WorldPos);
    float3 reflectDir = normalize(reflect(-viewDir, vsOutput.Normal));
    
    return CubemapTexture.SampleLevel(AnisotropicSampler, reflectDir, 0).rgb;
}

float3 recomputePS(VS_OUTPUT vsOutput) : SV_Target0
{
    return float3(1.0f, 0.0f, 0.0f);
}

/************* Techniques *************/

technique11 main
{
    pass p0
    {
        SetVertexShader(CompileShader(vs_5_0, mainVS()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, mainPS()));
    }
}

technique11 main_instancing
{
    pass p0
    {
        SetVertexShader(CompileShader(vs_5_0, mainVS_Instancing()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, mainPS()));
    }
}

technique11 recompute
{
    pass p0
    {
        SetVertexShader(CompileShader(vs_5_0, mainVS()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, recomputePS()));
    }
}

technique11 recompute_instancing
{
    pass p0
    {
        SetVertexShader(CompileShader(vs_5_0, mainVS_Instancing()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_5_0, recomputePS()));
    }
}