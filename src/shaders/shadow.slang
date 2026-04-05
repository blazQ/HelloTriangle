struct ShadowPush { float4x4 model; };
[[vk::push_constant]] ShadowPush push;

struct ShadowUBO { float4x4 view; float4x4 proj; float4x4 lightSpaceMatrix; };
ConstantBuffer<ShadowUBO> ubo;

[shader("vertex")]
float4 vertMain(float3 pos : POSITION) : SV_Position {
    return mul(ubo.lightSpaceMatrix, mul(push.model, float4(pos, 1.0)));
}