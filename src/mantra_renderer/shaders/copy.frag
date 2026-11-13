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
} pc;

layout(location = 0) out vec4 out_color;

void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy) + pc.src_offset;
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
  value *= pc.scale;
  float comps[6] = float[6](value.r, value.g, value.b, value.a, 0.0, 1.0);
  out_color = vec4(comps[pc.swizzle.x], comps[pc.swizzle.y], comps[pc.swizzle.z],
                   comps[pc.swizzle.w]);
}
