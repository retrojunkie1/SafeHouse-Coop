#version 430 core
// Cosmetics (cosmetic.h): an item's mesh at its attachment's matrix, lit by one key light over an ambient.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec4 aColor;
layout (location = 3) in vec2 aUv;

uniform mat4 matWorldToClip;
uniform mat4 matModel;
uniform vec3 posCamera;
uniform vec4 fogParams;   // near, far, max, on (the scene's)

out vec3 vNormal;
out vec4 vColor;
out vec2 vUv;
out float vFog;

void main()
{
    vec4 posWorld = matModel * vec4(aPos, 1.0);
    gl_Position = matWorldToClip * posWorld;
    vNormal = normalize(mat3(matModel) * aNormal);
    vColor = aColor;
    vUv = aUv;
    float d = distance(posWorld.xyz, posCamera);
    vFog = fogParams.w > 0.0 ? clamp((d - fogParams.x) / max(fogParams.y - fogParams.x, 1.0), 0.0, 1.0) * fogParams.z : 0.0;
}
