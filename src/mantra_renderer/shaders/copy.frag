#version 450
// mantra texel copy: resolves (render target -> resolved surface) and the
// frontbuffer present (resolved surface -> presenter guest output).
//   dst(x, y) = swizzle(scale * src(x + src_offset.x, y + src_offset.y))
// Multisampled sources average the samples selected by sample_mask.
#extension GL_EXT_samplerless_texture_functions : require

#ifdef MULTISAMPLE
layout(set = 0, binding = 0) uniform texture2DMS src_texture;
#else
layout(set = 0, binding = 0) uniform texture2D src_texture;
#endif

layout(push_constant) uniform PushConstants {
  ivec2 src_offset;
  uint sample_mask;
  float scale;
  // Per output component: 0-3 = source component, 4 = 0.0, 5 = 1.0.
  uvec4 swizzle;
  // 0 = plain copy, 1 = float -> 7e3 bits as 10:10:10:2 unorm,
  // 2 = 10:10:10:2 unorm holding 7e3 bits -> float.
  uint mode;
  // Source pixels per destination pixel; bilinear when linear != 0.
  float src_ratio;
  uint linear;
} pc;

// Xenos 7e3: 3-bit exponent (bias 3), 7-bit mantissa, [0, 31.875].
uvec3 Float32To7e3(vec3 f) {
  uvec3 b = floatBitsToUint(clamp(f, vec3(0.0), vec3(31.875)));
  uvec3 denorm = ((b & 0x7FFFFFu) | 0x800000u) >> min(uvec3(125u) - min(b >> 23u, uvec3(125u)), uvec3(24u));
  uvec3 norm = b + 0xC2000000u;
  uvec3 f10 = mix(norm, denorm, lessThan(b, uvec3(0x3E800000u)));
  return ((f10 + 0x7FFFu + ((f10 >> 16u) & 1u)) >> 16u) & 0x3FFu;
}
vec3 Float7e3To32(uvec3 f10) {
  uvec3 e = f10 >> 7u, m = f10 & 0x7Fu;
  vec3 normal = uintBitsToFloat(((e + 124u) << 23u) | (m << 16u));
  vec3 denormal = vec3(m) * (1.0 / 512.0);
  return mix(normal, denormal, equal(e, uvec3(0u)));
}

layout(location = 0) out vec4 out_color;

void main() {
  // Source position (render scales may differ between the two images).
  vec2 src_pos = gl_FragCoord.xy * pc.src_ratio + vec2(pc.src_offset);
  ivec2 coord = ivec2(floor(src_pos));
#ifndef MULTISAMPLE
  if (pc.linear != 0u) {
    ivec2 last = textureSize(src_texture, 0) - 1;
    vec2 p = src_pos - 0.5;
    ivec2 i0 = ivec2(floor(p));
    vec2 f = p - vec2(i0);
    ivec2 a = clamp(i0, ivec2(0), last), b = clamp(i0 + 1, ivec2(0), last);
    vec4 t00 = texelFetch(src_texture, a, 0), t10 = texelFetch(src_texture, ivec2(b.x, a.y), 0);
    vec4 t01 = texelFetch(src_texture, ivec2(a.x, b.y), 0), t11 = texelFetch(src_texture, b, 0);
    vec4 value = mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y);
    if (pc.mode == 1u) {
      out_color = vec4(vec3(Float32To7e3(value.rgb)) / 1023.0, clamp(value.a, 0.0, 1.0));
      return;
    }
    value *= pc.scale;
    float lc[6] = float[6](value.r, value.g, value.b, value.a, 0.0, 1.0);
    out_color = vec4(lc[pc.swizzle.x], lc[pc.swizzle.y], lc[pc.swizzle.z], lc[pc.swizzle.w]);
    return;
  }
#endif
#ifdef MULTISAMPLE
  vec4 sum = vec4(0.0);
  float count = 0.0;
  for (int i = 0; i < 4; ++i) {
    if (((pc.sample_mask >> uint(i)) & 1u) != 0u) {
      sum += texelFetch(src_texture, coord, i);
      count += 1.0;
    }
  }
  vec4 value = sum / max(count, 1.0);
#else
  vec4 value = texelFetch(src_texture, coord, 0);
#endif
  if (pc.mode == 1u) {
    out_color = vec4(vec3(Float32To7e3(value.rgb)) / 1023.0, clamp(value.a, 0.0, 1.0));
    return;
  }
  if (pc.mode == 2u) {
    out_color = vec4(Float7e3To32(uvec3(round(clamp(value.rgb, 0.0, 1.0) * 1023.0))), value.a);
    return;
  }
  value *= pc.scale;
  float comps[6] = float[6](value.r, value.g, value.b, value.a, 0.0, 1.0);
  out_color = vec4(comps[pc.swizzle.x], comps[pc.swizzle.y], comps[pc.swizzle.z],
                   comps[pc.swizzle.w]);
}
