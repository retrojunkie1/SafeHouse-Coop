#pragma once

// A standalone scene for the renderer while the world loader is being written: the texture blocks and meshes of
// a world file, found through a parse map of the reference port (build/loadmaps/.../file-N.map: which original
// function read which byte range), with the objects' load-time transforms taken from the reference's RAM after
// loading the same world (final.ram). Replaced by the real world walk (0x13DA18) once it lands.

#include "glob.h"
#include "light.h"
#include "screenobj.h"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

struct SCENEGLOBSET
{
    size_t ib = 0;                          // stream offset of the LoadGlob
    int oid = -1;                           // the object (debugging; set by the placement)
    std::shared_ptr<GLOBSET> pglobset;      // (shared by the instances of an object: one GLOBSET, several placements)
    bool fTransform = false;                // the matrices below come from the reference RAM
    // RenderGlob 0x1686AC..0x1687A8: whole-object sphere and owner +0x38 MRD, before group tests.
    bool fBounds = false;
    glm::vec3 posBounds{0.0f};
    float sBoundsRadius = 0.0f, sMRD = 1e10f;
    bool fMrdOriginal = false;              // (QoL draw distance above 1x: a character's tree, its whole fade the original's)
    uint32_t vaGlob = 0;                    // where the reference keeps this glob (ALO+0x80)
    std::vector<glm::mat4> amatGlob;        // per GLOB: its world matrix
    std::vector<uint8_t> afVisible;         // per GLOB: not hidden in the reference's RAM
    std::vector<std::vector<glm::mat4>> aamatBone;  // per GLOB: skinned groups' bone matrices (bone.h; empty: none)
    int8_t irtgt = -1;                      // drawn into this render target instead of the frame (rtgt.h)
    int8_t irtgtLit = -1;                   // the target whose lights it was last forward-lit with (-1: the frame's)
    std::vector<glm::mat4> amatLit;         // per GLOB: the matrix it was last forward-lit at (RelightForward)
    std::vector<std::array<LIGHTREC, 4>> aalLit;  // per GLOB: the four lights it was last forward-lit with
    std::vector<std::vector<glm::mat4>> aamatBoneLit;  // per GLOB: the bone matrices it was last forward-lit with
    glm::vec4 worldShaderLit{-1.0f};        // and the world shader colour then
    std::vector<const struct XF*> apxfOwner;  // per GLOB: its owner record (scenesw.cpp: looked up once)
    bool fOwners = false;
    uint32_t versionLit = 0;                // and the lights' version then
    // per GLOB: this instance's own copy of the vertices (render.h PglInstanceCopyGpu) when the GLOBSET is shared by
    // several entries: each instance is lit at its own place (0x1CD9E0 / 0x1681F8 bake every object with its own
    // model matrix; the forward-lit ones take their own light context); null: the glob's own arena
    std::vector<struct GLGLOB*> apglInst;
    std::vector<uint64_t> akeyBake;         // per GLOB: what its bake was made from (matrix, lights, world shader)
    // per GLOB: BakeKey's value (without the instance) and the lights' version it was taken at: at the same matrix it
    // stays when the lights changed since did not touch the group (light.h FBakeLightsUnchangedSince)
    std::vector<uint64_t> akeyBakeLights;
    std::vector<uint32_t> aversionKey;
    // The light context's position (0x1F2500: the root object's model position, XF +0x70, shared by its children;
    // 0x1A4478 chooses the lights there). Unset: the group's own centre.
    bool fPosLight = false;
    glm::vec3 posLight{0.0f};
    bool fPosLightMoved = false;
    // The object's draw alphas (RenderGlob 0x168638's gAlpha / gAlpha2: the default ALO draw 0x1F3C10 passes its
    // fade +0x12C twice, an actor's 0x1425B0 the fade times its +0x10D4 / +0x10D8): the meshes' vertex alpha (qw8.x)
    // and the cel borders' (qw8.y).
    float gAlpha = 1.0f, gAlpha2 = 1.0f;
    // The damage flash (ALO +0xD0 = RenderGlob's MODEL +0x50, sent as qw8.z; the actors' UpdateDamage 0x1A1358 pulses it
    // while hurt): P_A draws the cel border toward red by it (N06; measured on the reference).
    float gFlash = 0.0f;
    // (QoL, paint.h) the character repainter's profile (1..6) of the object's tree, 0 none
    uint8_t ipaint = 0;
    // Drawn over the HUD (RenderGlob 0x168638: the glob set's flag bit 34, ALO +0xD0, option 69 SetSoIgnoreLocked,
    // while no screen object of priority >= 16 is shown, G_GAME +0x3BEC == 0): its kinds 3 / 4 -> 22 (23 faded),
    // 5 / 6 -> 23, 9 -> 24, 10 -> 25, drawn after kind 21 (the screen objects) over a cleared Z (group 10's
    // 0x157CF0 sends 0x2DF010). E.g. the title card's close-up Sly.
    bool fForeground = false;
};

struct SCENE
{
    std::vector<SCENEGLOBSET> aglobset;
    int cblockShd = 0, cblockBulk = 0, cfail = 0;
    std::vector<std::shared_ptr<SCREENOBJ>> aso;   // the screen objects (HUD / menus / fonts)
};

// Loads every texture block and glob of the world through its parse map; checks each block ends exactly where
// the map says. Returns false on the first mismatch unless fContinue.
bool FLoadSceneFromMap(const std::vector<uint8_t>& world, const std::string& mapPath, SCENE& scene, bool fVerbose,
                       std::string& error);

// Stand-in for the light bake (P_M + the VIF1 read-back 0x167E58) on lit shaders without baked table colours:
// colour A (the shadow pass) = the world's mean baked ambient x the bake's input colour, colour B (the lit pass)
// = the mean baked light colour with an alpha from a fixed key light. Approximate; the native bake replaces it.
void ApproximateBakeLighting(SCENE& scene);

// The world's active lights (class 4 objects in the SW class bucket 0x3038 + cid * 0xC, linked by +0x20, with
// the LO flag bit 60 of +0x28 and +0x160 == 0, as 0x15E9C8 / 0x167FC0 find them; their 3 qw at +0x120) and the
// world shader colour SW+0x3E28, from the reference's RAM until the world loader's lights exist.
bool FLightsFromRam(const std::string& ramPath, LIGHTSET& lights);

// Lights every vertex the original lights at run time: the bake (P_M) for static groups, forward lighting (P_G)
// for lit-flag groups. Instances of such templates get their own vertex copies.
void ApplyLighting(SCENE& scene, const LIGHTSET& lights);
// Every frame: the forward-lit groups (mesh flag 1: characters, moving objects) again with the lights their light
// context (0x1A4478) chooses at their current position, and their colours to the GPU when fUpload. Returns the
// number of groups relit.
// alightsRtgt (CRTGT entries, or null): the render targets' own lights (0x169198 -> 0x1A4AD8) for the entries drawn
// into them; an entry without lights (actx empty) uses lights.
int RelightForward(SCENE& scene, const LIGHTSET& lights, bool fUpload, const LIGHTSET* alightsRtgt = nullptr);

// Takes the world matrices of the globs from the reference's RAM (final.ram, 32 MB EE memory).
int CApplyRamTransforms(SCENE& scene, const std::string& ramPath);
