#version 430 core
// Sly 2 glob fragments: the GS's texture function (MODULATE, TCC 1) and alpha test, for one pass of a shader.
// The pass's ALPHA blend and depth test are GL state (render.cpp).

uniform sampler2D tex;
uniform int  fTextured;
uniform int  ate;            // TEST: alpha test enable
uniform int  atst;           //       0 never 1 always 2 less 3 lequal 4 equal 5 gequal 6 greater 7 notequal
uniform float aref;          //       reference (GS units)
uniform int  afailMode;      // 0: no test, 1: keep fragments that pass, 2: keep fragments that fail
uniform vec4 fogColor;       // 0..1
uniform int  fFogBlack;      // the pass's FOGCOL is 0 (0x1EB960: kinds 1 / 7's additive passes), not fogColor
uniform int  fDate;          // TEST DATE, DATM 0 (the shadow hulls): only where the frame's alpha before the marking
uniform sampler2D texDate;   //   (texDate, 0x16DA48's copy) is below 0x80

#ifdef BINDLESS
flat in uvec2 vTexh;
#endif
noperspective in vec4 vColor;
in vec2 vUv;
noperspective in float vFog;
flat in uint vPaint;
// (paint.h) the character repainter: per profile (vPaint - 1) and slot two vec4s: {band hue, half width, least
// saturation, on}, {new hue, saturation scale, brightness scale, 0}
uniform vec4 paint[48];

out vec4 FragColor;

vec3 RgbToHsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 HsvToRgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
// the slots' bands recoloured: the hue moved by (new - band centre), the saturation and brightness scaled
vec3 Repaint(vec3 rgb, uint ip)
{
    vec3 hsv = RgbToHsv(rgb);
    vec3 outc = rgb;
    for (int s = 0; s < 4; ++s)
    {
        vec4 b = paint[int(ip - 1u) * 8 + s * 2];
        if (b.w == 0.0) continue;
        vec4 t = paint[int(ip - 1u) * 8 + s * 2 + 1];
        float dh = abs(hsv.x - b.x);
        dh = min(dh, 1.0 - dh);
        float w = (1.0 - smoothstep(b.y * 0.7, b.y, dh)) * smoothstep(b.z * 0.5, b.z, hsv.y);
        if (w <= 0.0) continue;
        vec3 n = vec3(fract(hsv.x + t.x - b.x + 1.0), clamp(hsv.y * t.y, 0.0, 1.0), clamp(hsv.z * t.z, 0.0, 1.0));
        outc = mix(outc, HsvToRgb(n), w);
    }
    return outc;
}

bool AlphaPasses(float a)
{
    switch (atst)
    {
    case 0: return false;
    case 1: return true;
    case 2: return a < aref;
    case 3: return a <= aref;
    case 4: return a == aref;
    case 5: return a >= aref;
    case 6: return a > aref;
    default: return a != aref;
    }
}

void main()
{
    if (fDate != 0 && texelFetch(texDate, ivec2(gl_FragCoord.xy), 0).a > 254.0 / 255.0) discard;
    vec4 c = vColor;
    if (fTextured != 0)
    {
#ifdef BINDLESS
        // (the same texture and sampler as a bound one: handle 0 is the special draws', which bind tex)
        vec4 t = (vTexh != uvec2(0u) ? texture(sampler2D(vTexh), vUv) : texture(tex, vUv)) * 255.0;
#else
        vec4 t = texture(tex, vUv) * 255.0;
#endif
        // (the repainter recolours the texel before the lighting: the band is the texture's own colour)
        if (vPaint != 0u) t.rgb = Repaint(t.rgb / 255.0, vPaint) * 255.0;
        c = floor(t * vColor / 128.0);
    }
    c = clamp(c, 0.0, 255.0);
    if (afailMode != 0 && ate != 0)
    {
        bool fPass = AlphaPasses(c.a);
        if ((afailMode == 1) != fPass) discard;
    }
    vec3 rgb = c.rgb / 255.0;
    rgb = mix(rgb, fFogBlack != 0 ? vec3(0.0) : fogColor.rgb, vFog);
    // The GS blends with As / 128 (0x80 = 1.0).
    FragColor = vec4(rgb, min(c.a / 128.0, 1.0));
}
