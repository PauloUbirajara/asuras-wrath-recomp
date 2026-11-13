#ifndef MANTRA_SHADER_COMMON_HLSL
#define MANTRA_SHADER_COMMON_HLSL
// mantra's replacement for XenosRecomp's shader_common.h (prepended to every
// generated shader). XenosRecomp's own header needs bufferDeviceAddress and
// runtime descriptor arrays, which the rexglue Vulkan device does not enable.
// Here everything goes through plain descriptors instead:
//
//   push constants (12 bytes, VS+PS): byte offsets of this draw's VS float
//     constants, PS float constants and shared constants in g_Constants.
//   set 0 binding 0: g_Constants, the per-frame constant upload ring.
//   set 1 bindings 0..3: Texture2D/Texture3D/TextureCube/SamplerState
//     arrays indexed by Xenos fetch constant slot (0..31). Only constant
//     indices are used: translator.cpp rewrites XenosRecomp's descriptor index
//     loads into the sampler's register (= fetch slot).
//
// translator.cpp rewrites vk::RawBufferLoad<T>(addr[, align]) into
// MantraLoad_T(addr[, align]). The shared constants layout is XenosRecomp's:
// 256 g_Booleans, 260 g_SwappedTexcoords, 264 g_HalfPixelOffset,
// 272 g_AlphaThreshold (the descriptor index area 0..255 is unused).
//
// The helper functions below are XenosRecomp's (MIT), unchanged except where
// noted.

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#define MANTRA_FETCH_SLOTS 32

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

struct PushConstants
{
    uint VertexShaderConstants;
    uint PixelShaderConstants;
    uint SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

[[vk::binding(0, 0)]] ByteAddressBuffer g_Constants;

float4 MantraLoad_float4(uint address) { return g_Constants.Load<float4>(address); }
float4 MantraLoad_float4(uint address, uint alignment) { return g_Constants.Load<float4>(address); }
float2 MantraLoad_float2(uint address) { return g_Constants.Load<float2>(address); }
float2 MantraLoad_float2(uint address, uint alignment) { return g_Constants.Load<float2>(address); }
float MantraLoad_float(uint address) { return g_Constants.Load<float>(address); }
float MantraLoad_float(uint address, uint alignment) { return g_Constants.Load<float>(address); }
uint MantraLoad_uint(uint address) { return g_Constants.Load(address); }
uint MantraLoad_uint(uint address, uint alignment) { return g_Constants.Load(address); }

#define g_Booleans                 MantraLoad_uint(g_PushConstants.SharedConstants + 256)
#define g_SwappedTexcoords         MantraLoad_uint(g_PushConstants.SharedConstants + 260)
#define g_HalfPixelOffset          MantraLoad_float2(g_PushConstants.SharedConstants + 264)
#define g_AlphaThreshold           MantraLoad_float(g_PushConstants.SharedConstants + 272)

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

[[vk::binding(0, 1)]] Texture2D<float4> g_Texture2DDescriptorHeap[MANTRA_FETCH_SLOTS];
[[vk::binding(1, 1)]] Texture3D<float4> g_Texture3DDescriptorHeap[MANTRA_FETCH_SLOTS];
[[vk::binding(2, 1)]] TextureCube<float4> g_TextureCubeDescriptorHeap[MANTRA_FETCH_SLOTS];
[[vk::binding(3, 1)]] SamplerState g_SamplerDescriptorHeap[MANTRA_FETCH_SLOTS];

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float w0(float a) { return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f); }
float w1(float a) { return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f); }
float w2(float a) { return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f); }
float w3(float a) { return (1.0f / 6.0f) * (a * a * a); }
float g0(float a) { return w0(a) + w1(a); }
float g1(float a) { return w2(a) + w3(a); }
float h0(float a) { return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f; }
float h1(float a) { return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f; }

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);

    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

// mantra: 1u instead of 1ull (no Int64 capability; semanticIndex < 32).
float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1u << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;

    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif
