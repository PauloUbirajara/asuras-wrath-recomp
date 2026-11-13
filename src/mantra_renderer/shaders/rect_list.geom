#version 450
// Xenos rectangle lists: each 3-vertex primitive is a parallelogram whose
// fourth corner is implied. The vertex opposite the longest edge (the
// diagonal) is the corner shared by the two given edges; the missing vertex is
// the sum of the diagonal's ends minus it, for every output (attributes are
// interpolated linearly across a rectangle anyway).
//
// The interpolators are the translated vertex shaders' fixed outputs
// (XenosRecomp's INTERPOLATORS: TEXCOORD0..15, COLOR0, COLOR1 at locations
// 0..17).

layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;

#define INTERPOLATOR_COUNT 18
layout(location = 0) in vec4 in_interpolators[][INTERPOLATOR_COUNT];
layout(location = 0) out vec4 out_interpolators[INTERPOLATOR_COUNT];

void EmitInput(int v) {
  gl_Position = gl_in[v].gl_Position;
  for (int i = 0; i < INTERPOLATOR_COUNT; ++i) {
    out_interpolators[i] = in_interpolators[v][i];
  }
  EmitVertex();
}

void main() {
  vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;
  vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;
  vec2 p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;
  float d01 = dot(p1 - p0, p1 - p0);
  float d12 = dot(p2 - p1, p2 - p1);
  float d20 = dot(p0 - p2, p0 - p2);
  int c;
  if (d01 >= d12 && d01 >= d20) {
    c = 2;
  } else if (d12 >= d20) {
    c = 0;
  } else {
    c = 1;
  }
  int a = (c + 1) % 3;
  int b = (c + 2) % 3;
  // Strip c, a, b, d: triangles (c, a, b) and (b, a, d) both keep the
  // winding of the input triangle.
  EmitInput(c);
  EmitInput(a);
  EmitInput(b);
  gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;
  for (int i = 0; i < INTERPOLATOR_COUNT; ++i) {
    out_interpolators[i] =
        in_interpolators[a][i] + in_interpolators[b][i] - in_interpolators[c][i];
  }
  EmitVertex();
  EndPrimitive();
}
