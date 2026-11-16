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
// 272 g_AlphaThreshold (mantra: bytes 0..127 hold each fetch slot's exp_adjust), plus
// mantra's 288 g_NdcScale, 300 g_MantraInputRemapMask and 320 the per-input
// remaps (VS inputs whose runtime fetch swizzle differs from the container's,
// rebuilt at the top of main by MantraRemapInput), 304 g_NdcOffset:
// translator.cpp replaces the VS's
// final half-pixel offset with oPos.xyz = oPos.xyz * g_NdcScale +
// g_NdcOffset * oPos.w (the host viewport transform; Y is pre-negated because
// DXC's -fvk-invert-y negates oPos.y afterwards).
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
float3 MantraLoad_float3(uint address) { return g_Constants.Load<float3>(address); }
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
#define g_MantraVertexIndexBase    MantraLoad_uint(g_PushConstants.SharedConstants + 276)
#define g_MantraFixed16OutputMask  MantraLoad_uint(g_PushConstants.SharedConstants + 280)
#define g_MantraFloat7e3OutputMask MantraLoad_uint(g_PushConstants.SharedConstants + 284)
#define g_NdcScale                 MantraLoad_float3(g_PushConstants.SharedConstants + 288)
#define g_NdcOffset                MantraLoad_float3(g_PushConstants.SharedConstants + 304)
#define g_MantraInputRemapMask     MantraLoad_uint(g_PushConstants.SharedConstants + 300)

// mantra: SharedConstants::input_remap for input `location`.
uint MantraInputRemap(uint location)
{
    return (MantraLoad_uint(g_PushConstants.SharedConstants + 320 + (location >> 1) * 4) >>
            ((location & 1) * 16)) & 0xFFF;
}

float4 MantraRemapInput(float4 v, uint location)
{
    uint m = MantraInputRemap(location);
    float s[6] = { v.x, v.y, v.z, v.w, 0.0, 1.0 };
    return float4(s[m & 7], s[(m >> 3) & 7], s[(m >> 6) & 7], s[(m >> 9) & 7]);
}

// uint4 inputs hold float bits (asfloat()ed by the translation).
uint4 MantraRemapInput(uint4 v, uint location)
{
    uint m = MantraInputRemap(location);
    uint s[6] = { v.x, v.y, v.z, v.w, 0u, 0x3F800000u };
    return uint4(s[m & 7], s[(m >> 3) & 7], s[(m >> 6) & 7], s[(m >> 9) & 7]);
}

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

[[vk::binding(0, 1)]] Texture2D<float4> g_Texture2DDescriptorHeap[MANTRA_FETCH_SLOTS];
[[vk::binding(1, 1)]] Texture3D<float4> g_Texture3DDescriptorHeap[MANTRA_FETCH_SLOTS];
[[vk::binding(2, 1)]] TextureCube<float4> g_TextureCubeDescriptorHeap[MANTRA_FETCH_SLOTS];
[[vk::binding(3, 1)]] SamplerState g_SamplerDescriptorHeap[MANTRA_FETCH_SLOTS];

// Vertex shaders have no implicit derivatives: they fetch from mip 0. The
// translator defines MANTRA_VERTEX_SHADER for them.
#ifdef MANTRA_VERTEX_SHADER
#define MANTRA_SAMPLE(samplerState, texCoord) SampleLevel(samplerState, texCoord, 0)
#else
#define MANTRA_SAMPLE(samplerState, texCoord) Sample(samplerState, texCoord)
#endif

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

// mantra: the fetch constant's exp_adjust (results * 2^exp_adjust, signed),
// stored per fetch slot in the shared constants' descriptor index area.
// Games use it to read 16-bit fixed-point (-32...32) render targets back
// in range.
float4 mantraExpAdjust(uint slot, float4 value)
{
    int e = asint(MantraLoad_uint(g_PushConstants.SharedConstants + slot * 4));
    return e != 0 ? value * exp2(float(e)) : value;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return mantraExpAdjust(resourceDescriptorIndex, texture.MANTRA_SAMPLE(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture)));
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
        g0(fy) * (g0x * texture.MANTRA_SAMPLE(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.MANTRA_SAMPLE(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.MANTRA_SAMPLE(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.MANTRA_SAMPLE(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return mantraExpAdjust(resourceDescriptorIndex, r);
}

// 1D textures are bound as 2D views of height 1 (the translator points the
// Texture1D index at the fetch slot, like the others).
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord)
{
    return mantraExpAdjust(resourceDescriptorIndex, g_Texture2DDescriptorHeap[resourceDescriptorIndex].MANTRA_SAMPLE(
        g_SamplerDescriptorHeap[samplerDescriptorIndex], float2(texCoord, 0.5)));
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return mantraExpAdjust(resourceDescriptorIndex, g_Texture3DDescriptorHeap[resourceDescriptorIndex].MANTRA_SAMPLE(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord));
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return mantraExpAdjust(resourceDescriptorIndex, g_TextureCubeDescriptorHeap[resourceDescriptorIndex].MANTRA_SAMPLE(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]));
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
