#version 430 core
// Sly 2 glob triangles clipped against the near plane the way the original's VU1 program does it, before the GS draws
// them (R51). The GS interpolates RGBA and F affinely in screen space (glob.vert: noperspective), but only between the
// vertices it is sent: P_G clips a triangle that crosses the near plane in 3D, so a new vertex on the plane carries the
// colour linear in the clip-space segment (the reference's GS log at Trojan Tank: alphas 104..117 on the clip vertices
// of the ramp under the camera, between its vertices' 70..127). GL's own clipper gives a noperspective output at a
// clip vertex the value that varies linearly in screen space over the unclipped triangle instead (GL 4.6 13.7), which
// for a vertex far behind the camera extrapolates: the floor under a low camera went dark and its frame alpha (the
// shadow combine's darkness) to 0. Here the near plane is w - z >= 0 (render.cpp: reversed-Z, z = near); a triangle
// wholly in front is passed through unchanged.

layout (triangles) in;
layout (triangle_strip, max_vertices = 4) out;

noperspective in vec4 gsColor[];
in vec2 gsUv[];
noperspective in float gsFog[];
flat in uint gsPaint[];
flat out uint vPaint;
#ifdef BINDLESS
flat in uvec2 gsTexh[];
flat out uvec2 vTexh;
#endif

noperspective out vec4 vColor;
out vec2 vUv;
noperspective out float vFog;

void EmitAt(int i, int j, float t)
{
    gl_Position = mix(gl_in[i].gl_Position, gl_in[j].gl_Position, t);
    vColor = mix(gsColor[i], gsColor[j], t);
    vUv = mix(gsUv[i], gsUv[j], t);
    vFog = mix(gsFog[i], gsFog[j], t);
    vPaint = gsPaint[0];
#ifdef BINDLESS
    vTexh = gsTexh[0];
#endif
    EmitVertex();
}

void main()
{
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = gl_in[i].gl_Position.w - gl_in[i].gl_Position.z;
    if (d[0] >= 0.0 && d[1] >= 0.0 && d[2] >= 0.0)
    {
        for (int i = 0; i < 3; ++i) EmitAt(i, i, 0.0);
        EndPrimitive();
        return;
    }
    // Sutherland-Hodgman against the one plane: up to four vertices, each (i, j, t), in the triangle's order
    ivec2 aij[4];
    float at[4];
    int c = 0;
    for (int i = 0; i < 3; ++i)
    {
        const int j = (i + 1) % 3;
        if (d[i] >= 0.0) { aij[c] = ivec2(i, i); at[c] = 0.0; ++c; }
        if ((d[i] >= 0.0) != (d[j] >= 0.0)) { aij[c] = ivec2(i, j); at[c] = d[i] / (d[i] - d[j]); ++c; }
    }
    if (c < 3) return;
    // (a strip of the polygon a b c [d]: a b d c)
    EmitAt(aij[0].x, aij[0].y, at[0]);
    EmitAt(aij[1].x, aij[1].y, at[1]);
    if (c == 4) EmitAt(aij[3].x, aij[3].y, at[3]);
    EmitAt(aij[2].x, aij[2].y, at[2]);
    EndPrimitive();
}
