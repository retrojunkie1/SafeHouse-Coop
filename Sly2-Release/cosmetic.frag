#version 430 core
uniform sampler2D tex;
uniform int fTextured;
uniform vec3 dirLight;      // toward the light (world)
uniform vec3 rgbAmbient;    // the scene's ambient level
uniform vec3 rgbLight;      // the key light's
uniform vec4 fogColor;
uniform vec4 tint;          // the wardrobe's colour (rgb, w: amount)

in vec3 vNormal;
in vec4 vColor;
in vec2 vUv;
in float vFog;
out vec4 FragColor;

void main()
{
    vec4 c = vColor;
    if (fTextured != 0) c *= texture(tex, vUv);
    if (c.a < 0.3) discard;
    // (the wardrobe's colour: the material's brightness kept, its hue and saturation the tint's)
    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    c.rgb = mix(c.rgb, tint.rgb * (l * 1.6), tint.w * c.a);
    float diff = max(dot(normalize(vNormal), normalize(dirLight)), 0.0);
    vec3 rgb = c.rgb * (rgbAmbient + rgbLight * diff);
    rgb = mix(rgb, fogColor.rgb, vFog);
    FragColor = vec4(rgb, 1.0);
}
