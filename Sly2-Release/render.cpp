// See render.h.

#include "texmod.h"
#include "paint.h"
#include "cosmetic.h"
#include <set>
#include "render.h"
#include "simdmat.h"
#include "drawdist.h"

#include "display.h"
#include "cm.h"
#include "emitter.h"
#include "gl.h"
#include "light.h"
#include "laserrender.h"
#include "pocket.h"
void CollectHackSwarmDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra);  // (hack.cpp: the swarms' particles)
#include "rtgt.h"
#include "wr.h"

#include <stb/stb_image_write.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <deque>
#include <map>
#include <unordered_map>
#include <string>

RENDERSTATS g_renderstats;
constinit PFNEXTRADRAWS g_pfnExtraDraws = nullptr;

struct GLGLOB
{
    GLuint vao = 0;             // the geometry arena's (one per BuildSceneGpu: all its globs' vertices and indices)
    GLuint vbo = 0;             // the arena's vertex buffer (moving geometry: UpdateGlobVerticesGpu)
    GLuint vboColor = 0;        // the arena's lit colours (rgbaA, rgbaB per vertex: attributes 2 / 3; UpdateGlobColorsGpu)
    int32_t baseVertex = 0;     // where this glob's vertices / indices start in the arena
    uint32_t firstIndex = 0;
    GLuint ssboEdges = 0;
    GLuint ssboEdgeSkin = 0;   // per edge vertex: bone weights and indices (skinned cel borders)
    glm::vec3 posCenter{0.0f};  // the geometry's bounding sphere (for globs without a draw record)
    float sRadius = -1.0f;
    GLsizei cedge = 0;
    GLsizei cindex = 0;
};

// A SUBGLOB's commands, cached: per pass sub-draw its state and order.
struct PASSCMD { uint32_t istate; uint32_t order; };
struct SUBGLOBGPU
{
    const SUBGLOB* powner = nullptr;
    const SHD* pshd = nullptr;
    int iframe = -1;
    GLuint vao = 0;
    uint8_t fCull = 0;
    std::vector<PASSCMD> apasscmd;
    SUBGLOBGPU* pnextVao = nullptr;   // the same SUBGLOB drawn from another arena (an instance's own vertices)
    uint32_t genTex = 0;              // the texture cache's generation its states were made in (s_genTexCache)
};

namespace
{
void InitCommonOnce();
struct GPUVTX
{
    float pos[3];
    float uv[2];
    uint8_t rgbaA[4];
    uint8_t rgbaB[4];
    float normal[3];
    uint8_t abone[4];   // skinning (animation code): the group's bone indices (VTXGLOB::abone)
    uint8_t aw[4];      // and the raw weights (/255, P_G 726)
};
static_assert(sizeof(GPUVTX) == 48);
// (the colours' own buffer: GPUVTX's rgbaA / rgbaB are not read from the vertex buffer)
struct GPUCOL
{
    uint8_t rgbaA[4];
    uint8_t rgbaB[4];
};
static_assert(sizeof(GPUCOL) == 8);
inline GPUCOL ColOfGpuVtx(const GPUVTX& v)
{
    GPUCOL c;
    std::memcpy(c.rgbaA, v.rgbaA, 4);
    std::memcpy(c.rgbaB, v.rgbaB, 4);
    return c;
}

std::vector<std::unique_ptr<GLGLOB>> s_aglglob;
std::map<std::pair<const BMP*, const CLUT*>, GLuint> s_mptex;
// (bumped when the texture cache is reset: a SUBGLOB's cached states name its GL textures, which are deleted then. Only
// the PC guests' kept-world globs (pcloader.h) outlive a world load with their cache: they are drawn again in the next
// world and must not keep the deleted names, which the new world's textures reuse: R5x, the guests' wrong textures
// after a death's reload)
uint32_t s_genTexCache = 1;
GLuint s_texWhite = 0;
GLuint s_asampler[16] = {};  // bit 0 clamp S, 1 clamp T, 2 nearest, 3 mipmapped (a texture mod's own mip chain: texmod.h)
// The mipmapped samplers' anisotropy (texture mods only; SLY2N_MODANISO=N, 1: off): clamped to the GPU's maximum
const float s_rModAniso = std::getenv("SLY2N_MODANISO") ? float(std::atof(std::getenv("SLY2N_MODANISO"))) : 8.0f;
// The sampler for a texture: the mipmapped one for a texture mod
inline uint8_t SamplerFor(GLuint tex, uint8_t sampler) { return uint8_t(sampler | (FTextureIsMod(tex) ? 8 : 0)); }
GLuint s_vaoEmpty = 0;
// Geometry arenas: the globs built by one BuildSceneGpu share one VBO / EBO / VAO, so a run of draws with one GL
// state is one glMultiDrawElementsIndirect. Attribute 6 is the draw index (an iota buffer, divisor 1, selected by
// the command's baseInstance) into the per-frame draw data (glob.vert DRAWS / RECS).
std::vector<struct GPUVTX> s_avtxArena;
std::vector<uint32_t> s_aindexArena;
std::vector<GLGLOB*> s_apglArena;
GLuint s_vboDrawIndex = 0;
constexpr uint32_t CDRAW_MAX = 1u << 18;

// (the old world's textures: deleted at the next lookup, where GL is current)
std::vector<GLuint> s_atexFreed;

// (perf-3, g_fGlobBindless) the resident handles of (texture, sampler) pairs the pass draws use; a deleted texture's are
// made non-resident first and forgotten (its name may come back), and the per-state cache is invalidated
std::unordered_map<uint64_t, GLuint64> s_mphandle;
uint32_t s_genHandle = 1;
GLuint64 TexHandle(GLuint tex, uint8_t sampler)
{
    const uint64_t key = (uint64_t(tex) << 8) | (sampler & 15u);
    auto [it, fNew] = s_mphandle.try_emplace(key, 0);
    if (fNew)
    {
        it->second = glGetTextureSamplerHandleARB(tex, s_asampler[sampler & 15u]);
        if (it->second) glMakeTextureHandleResidentARB(it->second);
    }
    return it->second;
}
void ForgetTexHandles(const std::vector<GLuint>& atex)
{
    if (s_mphandle.empty()) return;
    for (GLuint tex : atex)
        for (uint64_t i = 0; i < 16; ++i)
        {
            const auto it = s_mphandle.find((uint64_t(tex) << 8) | i);
            if (it == s_mphandle.end()) continue;
            if (it->second) glMakeTextureHandleNonResidentARB(it->second);
            s_mphandle.erase(it);
        }
    ++s_genHandle;
}

std::unordered_map<GLuint, std::string> s_mptexName;   // (debug SLY2N_PAINTTEX: the characters' textures by name)
GLuint TexFromSel(const TEXSEL& sel)
{
    InitCommonOnce();
    if (!s_atexFreed.empty())
    {
        ForgetTexHandles(s_atexFreed);
        glDeleteTextures(GLsizei(s_atexFreed.size()), s_atexFreed.data());
        s_atexFreed.clear();
    }
    if (!sel.pbmp) return s_texWhite;
    const std::pair<const BMP*, const CLUT*> key(sel.pbmp, sel.pclut);
    auto it = s_mptex.find(key);
    if (it != s_mptex.end()) return it->second;
    std::vector<uint8_t> rgba;
    std::string error;
    GLuint tex = s_texWhite;
    if (FDecodeTexture(sel.pbmp, sel.pclut, rgba, error))
    {
        // (a texture mod: Mods/Textures/<W>x<H>_<hash>.png)
        if (const GLuint texMod = GlTexFromMod(rgba, sel.pbmp->dx, sel.pbmp->dy))
        {
            s_mptex[key] = texMod;
            ++g_renderstats.ctexture;
            return texMod;
        }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sel.pbmp->dx, sel.pbmp->dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        ++g_renderstats.ctexture;
        if (static const bool s_f = std::getenv("SLY2N_PAINTTEX") != nullptr; s_f)
            s_mptexName[tex] = SzTextureModName(rgba, sel.pbmp->dx, sel.pbmp->dy);
    }
    else
    {
        ++g_renderstats.cfailTexture;
        static int s_cwarn = 0;
        if (s_cwarn++ < 10) std::fprintf(stderr, "texture 0x%X: %s\n", sel.pbmp->id, error.c_str());
    }
    s_mptex[key] = tex;
    return tex;
}

void InitCommon();
void InitCommonOnce() { InitCommon(); }
void InitCommon()
{
    if (s_texWhite) return;
    const uint32_t white = 0x80FFFFFFu;  // RGB 255, alpha 0x80 (1.0)
    glGenTextures(1, &s_texWhite);
    glBindTexture(GL_TEXTURE_2D, s_texWhite);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glGenSamplers(16, s_asampler);
    constexpr GLenum kMaxAniso = 0x84FF, kAniso = 0x84FE;  // GL_(MAX_)TEXTURE_MAX_ANISOTROPY (4.6, EXT before)
    GLfloat rAnisoMax = 0.0f;
    glGetFloatv(kMaxAniso, &rAnisoMax);
    while (glGetError() != GL_NO_ERROR) {}
    const float rAniso = std::min(s_rModAniso, rAnisoMax);
    for (int i = 0; i < 16; ++i)
    {
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_WRAP_S, (i & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_WRAP_T, (i & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        const GLint filter = (i & 4) ? GL_NEAREST : GL_LINEAR;
        // (mipmapped, for a texture mod: the original's filter between texels, trilinear / nearest-mip between sizes;
        // the anisotropy only where the original is bilinear, so point-sampled pixel art stays crisp)
        const GLint filterMin = !(i & 8) ? filter : (i & 4) ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_MIN_FILTER, filterMin);
        glSamplerParameteri(s_asampler[i], GL_TEXTURE_MAG_FILTER, filter);
        if ((i & 8) && !(i & 4) && rAniso > 1.0f) glSamplerParameterf(s_asampler[i], kAniso, rAniso);
    }
    glGenVertexArrays(1, &s_vaoEmpty);
    std::vector<uint32_t> aiota(CDRAW_MAX);
    for (uint32_t i = 0; i < CDRAW_MAX; ++i) aiota[i] = i;
    glGenBuffers(1, &s_vboDrawIndex);
    glBindBuffer(GL_ARRAY_BUFFER, s_vboDrawIndex);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(aiota.size() * 4), aiota.data(), GL_STATIC_DRAW);
}

template <class F> void SetBounds(GLGLOB* pgl, size_t c, F posOf)
{
    if (c == 0) return;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (size_t i = 0; i < c; ++i) { const glm::vec3 p = posOf(i); lo = glm::min(lo, p); hi = glm::max(hi, p); }
    pgl->posCenter = (lo + hi) * 0.5f;
    float r2 = 0;
    for (size_t i = 0; i < c; ++i) { const glm::vec3 d = posOf(i) - pgl->posCenter; r2 = std::max(r2, glm::dot(d, d)); }
    pgl->sRadius = std::sqrt(r2);
}

void AppendArena(GLGLOB* pgl, const std::vector<GPUVTX>& av, const std::vector<uint32_t>& ai)
{
    SetBounds(pgl, av.size(), [&](size_t i) { return glm::vec3(av[i].pos[0], av[i].pos[1], av[i].pos[2]); });
    pgl->baseVertex = int32_t(s_avtxArena.size());
    pgl->firstIndex = uint32_t(s_aindexArena.size());
    s_avtxArena.insert(s_avtxArena.end(), av.begin(), av.end());
    s_aindexArena.insert(s_aindexArena.end(), ai.begin(), ai.end());
    s_apglArena.push_back(pgl);
}

void FlushArena()
{
    if (s_apglArena.empty()) return;
    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(s_avtxArena.size() * sizeof(GPUVTX)), s_avtxArena.data(), GL_STATIC_DRAW);
    // (perf-3) the colours in a buffer of their own: the run-time lighting rewrites them (8 bytes a vertex, not the
    // whole vertex), a dynamic buffer (Mesa warns on, and may stall for, updates of a static one)
    GLuint vboColor = 0;
    {
        std::vector<GPUCOL> acol(s_avtxArena.size());
        for (size_t i = 0; i < acol.size(); ++i) acol[i] = ColOfGpuVtx(s_avtxArena[i]);
        glGenBuffers(1, &vboColor);
        glBindBuffer(GL_ARRAY_BUFFER, vboColor);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(acol.size() * sizeof(GPUCOL)), acol.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
    }
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(s_aindexArena.size() * 4), s_aindexArena.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, uv)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, normal)));
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, abone)));
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GPUVTX), reinterpret_cast<void*>(offsetof(GPUVTX, aw)));
    glBindBuffer(GL_ARRAY_BUFFER, vboColor);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GPUCOL), reinterpret_cast<void*>(offsetof(GPUCOL, rgbaA)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GPUCOL), reinterpret_cast<void*>(offsetof(GPUCOL, rgbaB)));
    glBindBuffer(GL_ARRAY_BUFFER, s_vboDrawIndex);
    glEnableVertexAttribArray(6);
    glVertexAttribIPointer(6, 1, GL_UNSIGNED_INT, 4, nullptr);
    glVertexAttribDivisor(6, 1);
    glBindVertexArray(0);
    for (GLGLOB* pgl : s_apglArena) { pgl->vao = vao; pgl->vbo = vbo; pgl->vboColor = vboColor; }
    s_apglArena.clear();
    std::vector<GPUVTX>().swap(s_avtxArena);
    std::vector<uint32_t>().swap(s_aindexArena);
}

void GpuVerticesOfGlob(const GLOB& glob, std::vector<GPUVTX>& av)
{
    av.resize(glob.avtx.size());
    for (size_t i = 0; i < av.size(); ++i)
    {
        const VTXGLOB& v = glob.avtx[i];
        std::memcpy(av[i].pos, &v.pos, 12);
        std::memcpy(av[i].uv, &v.uv, 8);
        std::memcpy(av[i].rgbaA, &v.rgbaA, 4);
        std::memcpy(av[i].rgbaB, &v.rgbaB, 4);
        std::memcpy(av[i].normal, &v.normal, 12);
        std::memcpy(av[i].abone, v.abone, 4);  // (skinning, animation code)
        std::memcpy(av[i].aw, v.aw, 4);
    }
}

void BuildGlobGpu(GLOB& glob)
{
    if (glob.pgl) return;  // (a GLOBSET shared by several scene entries)
    if (glob.meshk == MESHK_Strips && !glob.avtx.empty() && !glob.aindex.empty())
    {
        auto pgl = std::make_unique<GLGLOB>();
        std::vector<GPUVTX> av;
        GpuVerticesOfGlob(glob, av);
        AppendArena(pgl.get(), av, glob.aindex);
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
    else if (glob.meshk == MESHK_Indexed && !glob.asubfx.empty())
    {
        // Type 3 (shadow hulls of rp 7 / 8): per strip the position dictionary indexed per strip vertex, triangles
        // where the strip mask's (ADC) bit is clear, winding alternating with the vertex index as in type 0.
        std::vector<GPUVTX> av;
        std::vector<uint32_t> ai;
        for (const SUBFX& fx : glob.asubfx)
        {
            const uint32_t base = uint32_t(av.size());
            for (size_t k = 0; k < fx.aipos.size(); ++k)
            {
                GPUVTX v{};
                v.rgbaA[3] = 128;
                const glm::vec3 p = fx.aipos[k] < fx.apos.size() ? fx.apos[fx.aipos[k]] : glm::vec3(0.0f);
                std::memcpy(v.pos, &p, 12);
                av.push_back(v);
                const bool fAdc = (fx.amask[k / 32] >> (k % 32)) & 1;
                if (k < 2 || fAdc) continue;
                const uint32_t i = base + uint32_t(k);
                if (k & 1) { ai.push_back(i - 1); ai.push_back(i - 2); }
                else { ai.push_back(i - 2); ai.push_back(i - 1); }
                ai.push_back(i);
            }
        }
        if (ai.empty()) return;
        auto pgl = std::make_unique<GLGLOB>();
        AppendArena(pgl.get(), av, ai);
        pgl->cindex = GLsizei(ai.size());
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
    else if (glob.meshk == MESHK_CelBorder && !glob.asubcel.empty())
    {
        std::vector<glm::vec4> aedge;
        struct EDGESKIN { glm::vec4 w; glm::uvec4 ab; };   // per edge vertex: weights and the group's bone indices
        std::vector<EDGESKIN> askin;
        for (const SUBCEL& cel : glob.asubcel)
            for (size_t i = 0; i + 3 < cel.aedge.size(); i += 4)
                for (int j = 0; j < 4; ++j)
                {
                    // The stream's records are {OA, E0, E1, OB} (Sly 1's TWEF order 1, 2, 0, 3): checked on Paris, the
                    // triangles (OA, E0, E1) and (E1, E0, OB) of every record share their winding.
                    static const int s_aj[4] = {1, 2, 0, 3};
                    const int jSrc = s_aj[j];
                    const uint8_t ipos = cel.aedge[i + size_t(jSrc)];
                    aedge.emplace_back(ipos < cel.apos.size() ? cel.apos[ipos] : glm::vec3(0.0f), 1.0f);
                    EDGESKIN sk{glm::vec4(0.0f), glm::uvec4(cel.aiboneSlot[0], cel.aiboneSlot[1], cel.aiboneSlot[2], cel.aiboneSlot[3])};
                    for (int k = 0; k < cel.cw && k < 4; ++k)
                        if (size_t(ipos) * size_t(cel.cw) + size_t(k) < cel.aw.size())
                            sk.w[k] = float(cel.aw[size_t(ipos) * size_t(cel.cw) + size_t(k)]) / 255.0f;
                    askin.push_back(sk);
                }
        if (aedge.empty()) return;
        auto pgl = std::make_unique<GLGLOB>();
        glGenBuffers(1, &pgl->ssboEdges);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pgl->ssboEdges);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(aedge.size() * sizeof(glm::vec4)), aedge.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &pgl->ssboEdgeSkin);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pgl->ssboEdgeSkin);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(askin.size() * sizeof(EDGESKIN)), askin.data(), GL_STATIC_DRAW);
        pgl->cedge = GLsizei(aedge.size() / 4);
        SetBounds(pgl.get(), aedge.size(), [&](size_t i) { return glm::vec3(aedge[i]); });
        glob.pgl = pgl.get();
        s_aglglob.push_back(std::move(pgl));
    }
}

// GS ALPHA -> GL blend: Cv = (A - B) * C + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}. Each colour's coefficient
// is k1 + kC * C; GL factors cover 0, 1, C and 1 - C (and -C via reverse subtract).
bool FSetBlend(uint64_t alpha)
{
    const int a = int(alpha & 3), b = int((alpha >> 2) & 3), c = int((alpha >> 4) & 3), d = int((alpha >> 6) & 3);
    const float fix = float((alpha >> 32) & 0xFF) / 128.0f;
    auto coef = [&](int sel, int& k1, int& kc) {
        k1 = (d == sel) ? 1 : 0;
        kc = (a == sel ? 1 : 0) - (b == sel ? 1 : 0);
    };
    int s1, sc, d1, dc;
    coef(0, s1, sc);
    coef(1, d1, dc);
    const GLenum cf = c == 0 ? GL_SRC_ALPHA : c == 1 ? GL_DST_ALPHA : GL_CONSTANT_ALPHA;
    const GLenum cfInv = c == 0 ? GL_ONE_MINUS_SRC_ALPHA : c == 1 ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_CONSTANT_ALPHA;
    if (c == 2) glBlendColor(0, 0, 0, fix);
    auto factor = [&](int k1, int kc, GLenum& f) {
        if (k1 == 0 && kc == 0) f = GL_ZERO;
        else if (k1 == 1 && kc == 0) f = GL_ONE;
        else if (k1 == 0 && kc == 1) f = cf;
        else if (k1 == 1 && kc == -1) f = cfInv;
        else return false;
        return true;
    };
    // The GS blends only the colour: the frame's alpha is the source alpha (the shadow pass, 0x16DA48 / 0x16DD58,
    // reads it: render.md "shadow frame-channel pass").
    GLenum fs, fd;
    if (sc == -1 && s1 == 0 && factor(d1, dc, fd))
    {
        glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
        glBlendFuncSeparate(cf, fd, GL_ONE, GL_ZERO);
        return true;
    }
    glBlendEquation(GL_FUNC_ADD);
    if (!factor(s1, sc, fs) || !factor(d1, dc, fd))
    {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        return false;
    }
    glBlendFuncSeparate(fs, fd, GL_ONE, GL_ZERO);
    return true;
}

struct DRAWREC
{
    int kind;
    float z;
    const GLOB* pglob;      // the GLOB (for its shaders / visibility)
    const GLOB* pglobGeom;  // where its geometry lives
    const glm::mat4* pmat;
    const std::vector<glm::mat4>* pamatBone = nullptr;  // skinned: the bone matrices (they replace pmat)
    float alpha = 1.0f;                                 // the object alpha (qw8.x; RIPs' fades)
    float alpha2 = 1.0f;                                // the cel borders' (qw8.y)
    const SCENEGLOBSET* psgs = nullptr;                 // the entry and the group (cast shadows)
    size_t iglob = 0;
    uint32_t iboneFirst = 0;                            // their place in the frame's bone SSBO
};

struct UNIFORMS
{
    GLint matWorldToClip, icolor, rgbaConst, posCamera, dirCamera;
    GLint tex, fTextured, ate, atst, aref, afailMode, fogColor, fFogBlack, fDate, texDate, paint;
} s_u;
struct CELUNIFORMS { GLint matWorldToClip, matModel, halfWidthNdc, edgeBase, celRgba, cbone, iboneFirst; } s_uc;

glm::vec4 Vec4FromRgba(uint32_t rgba, float scale)
{
    return glm::vec4(float(rgba & 0xFF), float((rgba >> 8) & 0xFF), float((rgba >> 16) & 0xFF), float(rgba >> 24)) * scale;
}

// Per-frame draw data (glob.vert): per record its matrix and fog, per draw (record x entry) the SAA's ST offset,
// the object alpha and the record.
// bone: first matrix, count (skinned), the vertices' base in the arena; spec0 / spec1 / specPos: GLOB::iSpec's view-angle
// alpha (P_G 638): {g0, g1, g2, kind}, {clamp lo, hi, on, 0}, {posSpec, 0}; warp: the state vectors' first + 1 (0: no
// warp), the glob's vertex count, the first matrix, the matrix count | 0x100 positions | 0x200 coordinates (wr.h)
struct RECGPU { glm::mat4 model; glm::vec4 fog; glm::uvec4 bone{0u}; glm::vec4 spec0{0.0f}, spec1{0.0f}, specPos{0.0f}; glm::uvec4 warp{0u}; };
struct DRAWGPU { float du, dv, uAlpha; uint32_t irec; };

// The GL state a run of draws shares (one glMultiDrawElementsIndirect): everything DrawPass used to set per pass.
struct DSTATE
{
    GLuint vao = 0, tex = 0;
    uint64_t alpha = 0;         // GS ALPHA (fBlend)
    uint8_t sampler = 0, fTextured = 0, icolor = 0, fBlend = 0;
    uint8_t fCull = 0, depthFunc = 0, depthMask = 0, colorMask = 0;   // depthFunc 0 always 1 gequal 2 greater; colorMask 1 rgb 2 a
    uint8_t ate = 0, atst = 0, aref = 0, afailMode = 0;
    uint8_t fFogBlack = 0;      // FOGCOL 0 (0x1EB960: the additive passes of kinds 1 / 7)
    uint8_t fFba = 0;           // FBA 1 (kinds 4 / 6: 0x16E3C8's GS block 0x2DF700): the alpha's bit 7 (stencil STENCIL_FBA)
    uint8_t pad[2] = {};
};
static_assert(sizeof(DSTATE) == 32);
bool operator==(const DSTATE& a, const DSTATE& b) { return std::memcmp(&a, &b, sizeof(DSTATE)) == 0; }

enum CMDK : uint8_t { CMDK_Pass, CMDK_Volume, CMDK_Shadow, CMDK_Cel, CMDK_Particles, CMDK_Shadow2, CMDK_ShadowBegin, CMDK_ShadowKind8, CMDK_ShadowCombine, CMDK_FbaClear };
// Interned states: a command refers to its state by index.
struct DSTATEHASH
{
    size_t operator()(const DSTATE& st) const
    {
        uint64_t h = 1469598103934665603ull;
        const uint8_t* pb = reinterpret_cast<const uint8_t*>(&st);
        for (size_t i = 0; i < sizeof(DSTATE); ++i) h = (h ^ pb[i]) * 1099511628211ull;
        return size_t(h);
    }
};
std::vector<DSTATE> s_astate;
std::unordered_map<DSTATE, uint32_t, DSTATEHASH> s_mpstate;
uint32_t IstateOf(const DSTATE& st)
{
    auto it = s_mpstate.find(st);
    if (it != s_mpstate.end()) return it->second;
    const uint32_t i = uint32_t(s_astate.size());
    s_astate.push_back(st);
    s_mpstate.emplace(st, i);
    return i;
}

// The state with FBA set (render kinds 4 / 6).
std::vector<uint32_t> s_aistateFba;
uint32_t IstateFba(uint32_t istate)
{
    if (istate >= s_aistateFba.size()) s_aistateFba.resize(istate + 1, UINT32_MAX);
    if (s_aistateFba[istate] == UINT32_MAX)
    {
        DSTATE st = s_astate[istate];
        st.fFba = 1;
        const uint32_t i = IstateOf(st);
        if (istate >= s_aistateFba.size()) s_aistateFba.resize(istate + 1, UINT32_MAX);
        s_aistateFba[istate] = i;
    }
    return s_aistateFba[istate];
}

std::deque<SUBGLOBGPU> s_asubgpu;

struct CMD
{
    uint32_t istate = 0;
    GLuint vao = 0;             // (specials)
    uint32_t order = 0;         // pass x 4 + its alpha-test sub-draw (the order within a record)
    uint32_t count = 0, firstIndex = 0;
    int32_t baseVertex = 0;
    uint32_t idraw = 0;
    CMDK cmdk = CMDK_Pass;
    const DRAWREC* prec = nullptr;
    const SUBGLOB* psub = nullptr;
};
struct DEICMD { uint32_t count, instanceCount, firstIndex; int32_t baseVertex; uint32_t baseInstance; };

GLuint s_ssboRec = 0, s_ssboDraw = 0, s_bufIndirect = 0, s_ssboBone = 0, s_ssboWarp = 0, s_ssboWarpMat = 0, s_ssboTexh = 0;

// (perf-3) The passes' per-frame draw data (records, draw entries, bones, warps, the indirect commands, the texture
// handles) written in place into one persistently mapped buffer (GL 4.4 glBufferStorage, coherent): no copy through the
// driver's staging memory and its thread. Three segments, one a frame (RenderScene), each reused after its fence (three
// frames on). A pass that does not fit the frame's segment uploads as before (orphan + sub-data), and the ring doubles
// at the next frame. Opt-in, SLY2N_PERSIST=1: on Mesa (radeonsi, threaded) it measured the same as the orphaned
// buffers, and some drivers place persistent coherent memory in system memory, which the vertex stage reads per vertex.
struct STREAMRING
{
    static constexpr int CSEG = 3;
    GLuint buf = 0;
    uint8_t* pb = nullptr;
    size_t cbSeg = 0, off = 0, offLim = 0, align = 256;
    int iseg = 0;
    GLsync async[CSEG] = {};
    bool fGrow = false, fDisabled = false;
    typedef void (*PFNBUFFERSTORAGE)(GLenum, GLsizeiptr, const void*, GLbitfield);
    PFNBUFFERSTORAGE pfnBufferStorage = nullptr;

    void Create(size_t cb)
    {
        constexpr GLbitfield grf = 0x0002 /*MAP_WRITE*/ | 0x0040 /*MAP_PERSISTENT*/ | 0x0080 /*MAP_COHERENT*/;
        glGenBuffers(1, &buf);
        glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
        pfnBufferStorage(GL_COPY_WRITE_BUFFER, GLsizeiptr(cb * CSEG), nullptr, grf);
        pb = static_cast<uint8_t*>(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, GLsizeiptr(cb * CSEG), grf));
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        if (!pb)
        {
            glDeleteBuffers(1, &buf);
            buf = 0;
            fDisabled = true;
            return;
        }
        cbSeg = cb;
    }
    void Destroy()
    {
        for (GLsync& sync : async)
            if (sync) { glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull); glDeleteSync(sync); sync = nullptr; }
        if (buf)
        {
            glBindBuffer(GL_COPY_WRITE_BUFFER, buf);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            glDeleteBuffers(1, &buf);
        }
        buf = 0;
        pb = nullptr;
    }
    // The start of a frame: the last segment fenced, the next one waited for.
    void BeginFrame()
    {
        if (fDisabled) return;
        if (!pfnBufferStorage)
        {
            static const bool s_fOff = std::getenv("SLY2N_PERSIST") == nullptr;
            pfnBufferStorage = reinterpret_cast<PFNBUFFERSTORAGE>(cane::gl::ProcAddress("glBufferStorage"));
#if defined(__linux__)
            // (a 4.3 context, Zink on ARM GPUs: Mesa returns an entry point even when the driver lacks the feature)
            if ((GLVersion.major < 4 || (GLVersion.major == 4 && GLVersion.minor < 4)) && !glfwExtensionSupported("GL_ARB_buffer_storage")) pfnBufferStorage = nullptr;
#endif
            if (s_fOff || !pfnBufferStorage) { fDisabled = true; return; }
            GLint a = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &a);
            align = std::max<size_t>(256, size_t(std::max(a, 1)));
        }
        if (buf)
        {
            if (async[iseg]) glDeleteSync(async[iseg]);
            async[iseg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
        if (fGrow || !buf)
        {
            const size_t cb = buf ? cbSeg * 2 : size_t(4) << 20;
            Destroy();
            fGrow = false;
            Create(cb);
            if (!buf) return;
        }
        iseg = (iseg + 1) % CSEG;
        if (async[iseg])
        {
            // (polled first: a waiting call with the flush makes a threaded driver catch up first)
            if (glClientWaitSync(async[iseg], 0, 0) == GL_TIMEOUT_EXPIRED)
                glClientWaitSync(async[iseg], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync(async[iseg]);
            async[iseg] = nullptr;
        }
        off = cbSeg * size_t(iseg);
        offLim = off + cbSeg;
    }
    // cb bytes (aligned) of this frame's segment: their offset, or SIZE_MAX when they do not fit
    size_t Alloc(size_t cb)
    {
        if (!pb) return SIZE_MAX;
        const size_t o = (off + align - 1) / align * align;
        if (o + cb > offLim) { fGrow = true; return SIZE_MAX; }
        off = o + cb;
        return o;
    }
};
STREAMRING s_ring;

// (g_fGlobBindless) a state's texture handle (0: untextured, or none made: then drawn with the texture bound) and its
// merge class (the state without its texture and sampler, interned: equal classes differ only in those)
struct HANDLEOFSTATE { uint32_t gen = 0; uint32_t iclass = UINT32_MAX; GLuint64 handle = 0; };
std::vector<HANDLEOFSTATE> s_ahandleIstate;
std::unordered_map<DSTATE, uint32_t, DSTATEHASH> s_mpclass;
const HANDLEOFSTATE& HandleEntryOfIstate(uint32_t istate)
{
    if (istate >= s_ahandleIstate.size()) s_ahandleIstate.resize(istate + 1);
    HANDLEOFSTATE& e = s_ahandleIstate[istate];
    if (e.gen != s_genHandle)
    {
        DSTATE st = s_astate[istate];
        e.gen = s_genHandle;
        e.handle = st.fTextured ? TexHandle(st.tex, st.sampler) : 0;
        st.tex = 0;
        st.sampler = 0;
        e.iclass = s_mpclass.try_emplace(st, uint32_t(s_mpclass.size())).first->second;
    }
    return e;
}
GLuint64 HandleOfIstate(uint32_t istate) { return HandleEntryOfIstate(istate).handle; }
// Two pass states one run can draw with handles: the same but for the texture and sampler (both textured, with handles)
bool FSameButTexture(uint32_t istateA, uint32_t istateB)
{
    if (istateA == istateB) return true;
    HandleEntryOfIstate(istateA);
    HandleEntryOfIstate(istateB);   // (both made first: a lookup may grow the vector)
    const HANDLEOFSTATE& a = s_ahandleIstate[istateA];
    const HANDLEOFSTATE& b = s_ahandleIstate[istateB];
    return a.iclass == b.iclass && a.handle && b.handle;
}

// (SLY2N_GLSTATS=N: the GL state calls per frame, averaged over N frames: the draw path's costs on the driver)
struct GLSTATS { long capply, cfull, ctex, csampler, cuniform, cblend, crun, cpass; };
GLSTATS s_glstats{};
const int s_cframeGlStats = std::getenv("SLY2N_GLSTATS") ? std::atoi(std::getenv("SLY2N_GLSTATS")) : 0;

// Applies a run's state, setting only what changed since the last run (fForce: everything).
struct STATECACHE
{
    DSTATE cur;
    bool fValid = false;
    // fBindTexture false: the run's draws take their textures from handles (g_fGlobBindless): the unit's binding is left
    // as it is (and forgotten: the next run that binds one binds it)
    void Apply(const DSTATE& st, bool fBindTexture = true)
    {
        const bool f = !fValid;
        if (s_cframeGlStats)
        {
            ++s_glstats.capply; s_glstats.cfull += f;
            
            s_glstats.ctex += fBindTexture && st.fTextured && (f || !cur.fTextured || st.tex != cur.tex || !fTexBound);
            s_glstats.csampler += fBindTexture && st.fTextured && (f || !cur.fTextured || st.sampler != cur.sampler || !fTexBound);
            s_glstats.cuniform += (f || st.fTextured != cur.fTextured) + (f || st.icolor != cur.icolor) + (f || st.ate != cur.ate) + (f || st.atst != cur.atst) +
                                  (f || st.aref != cur.aref) + (f || st.afailMode != cur.afailMode) + (f || st.fFogBlack != cur.fFogBlack);
            s_glstats.cblend += st.fBlend && (f || !cur.fBlend || st.alpha != cur.alpha);
        }
        if (f || st.vao != cur.vao) glBindVertexArray(st.vao);
        if (fBindTexture && st.fTextured && (f || !cur.fTextured || st.tex != cur.tex || !fTexBound)) glBindTexture(GL_TEXTURE_2D, st.tex);
        if (fBindTexture && st.fTextured && (f || !cur.fTextured || st.sampler != cur.sampler || !fTexBound)) glBindSampler(0, s_asampler[st.sampler]);
        if (f || st.fTextured != cur.fTextured) glUniform1i(s_u.fTextured, st.fTextured);
        if (f || st.icolor != cur.icolor) glUniform1i(s_u.icolor, st.icolor);
        if (f || st.fBlend != cur.fBlend) { if (st.fBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
        if (st.fBlend && (f || !cur.fBlend || st.alpha != cur.alpha)) FSetBlend(st.alpha);
        if (f || st.fCull != cur.fCull)
        {
            if (st.fCull) { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); }
            else glDisable(GL_CULL_FACE);
        }
        if (f) glEnable(GL_DEPTH_TEST);
        if (f || st.depthFunc != cur.depthFunc) glDepthFunc(st.depthFunc == 0 ? GL_ALWAYS : st.depthFunc == 1 ? GL_GEQUAL : GL_GREATER);
        if (f || st.depthMask != cur.depthMask) glDepthMask(st.depthMask ? GL_TRUE : GL_FALSE);
        if (f || st.colorMask != cur.colorMask)
        {
            const GLboolean c = (st.colorMask & 1) ? GL_TRUE : GL_FALSE;
            glColorMask(c, c, c, (st.colorMask & 2) ? GL_TRUE : GL_FALSE);
        }
        if (f || st.ate != cur.ate) glUniform1i(s_u.ate, st.ate);
        if (f || st.atst != cur.atst) glUniform1i(s_u.atst, st.atst);
        if (f || st.aref != cur.aref) glUniform1f(s_u.aref, float(st.aref));
        if (f || st.afailMode != cur.afailMode) glUniform1i(s_u.afailMode, st.afailMode);
        if (f || st.fFogBlack != cur.fFogBlack) glUniform1i(s_u.fFogBlack, st.fFogBlack);
        // the frame alpha's bit 7 (FBA) in the stencil: set by the FBA kinds' alpha writes, cleared by the others'
        // (the native frame keeps alpha / 128, clamped: the shadow pass's DATE needs the bit, its CLUT the bits below)
        if (f || st.fFba != cur.fFba || st.colorMask != cur.colorMask)
        {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, st.fFba ? 0x80 : 0, 0x80);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glStencilMask((st.colorMask & 2) ? 0x80 : 0);
        }
        cur = st;
        fValid = true;
        fTexBound = fBindTexture;
    }
    bool fTexBound = true;
};

// One shader pass of an entry (formerly drawn directly): the GL state its GS registers ask for, as 1..2 commands
// (the alpha test's pass / fail sub-draws).
void EmitPass(std::vector<PASSCMD>& apasscmd, DSTATE st, const SUBGLOB& sub, const SHDPASS& pass, int ipass)
{
    // Texture
    if (pass.fTextured)
    {
        const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, pass.itexPass);
        st.tex = TexFromSel(sel);
        st.sampler = SamplerFor(st.tex, uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0) | (pass.fNearest ? 4 : 0)));
    }
    st.fTextured = pass.fTextured ? 1 : 0;
    st.icolor = uint8_t(pass.icolor);
    st.fFogBlack = pass.fFogBlack ? 1 : 0;
    // Blend (PRIM ABE)
    st.fBlend = pass.fBlend ? 1 : 0;
    st.alpha = pass.fBlend ? pass.alpha : 0;
    const bool fWritesColor = !((pass.alpha & 0xFF) == 0x40 || ((pass.alpha & 0xFF) == 0x50));  // Cd: (X - X) * C + Cd
    // Depth test (TEST ZTE / ZTST; reversed Z)
    const uint64_t test = pass.test;
    const bool zte = (test >> 16) & 1;
    const int ztst = int((test >> 17) & 3);
    if (zte && ztst == 0) return;
    st.depthFunc = uint8_t(!zte || ztst == 1 ? 0 : ztst == 2 ? 1 : 2);
    // Alpha test (TEST ATE / ATST / AREF / AFAIL)
    const bool ate = test & 1;
    const int atst = int((test >> 1) & 7);
    const int aref = int((test >> 4) & 0xFF);
    const int afail = int((test >> 12) & 3);
    st.ate = ate ? 1 : 0;
    st.atst = uint8_t(atst);
    st.aref = uint8_t(aref);
    auto draw = [&](int iSub, int afailMode, bool fDepth, bool fColor, bool fAlpha) {
        DSTATE stDraw = st;
        stDraw.afailMode = uint8_t(afailMode);
        stDraw.depthMask = fDepth ? 1 : 0;
        stDraw.colorMask = uint8_t((fColor ? 1 : 0) | (fColor && fAlpha ? 2 : 0));
        apasscmd.push_back({IstateOf(stDraw), uint32_t(ipass * 4 + iSub)});
    };
    if (!ate || atst == 1)
        draw(0, 0, true, fWritesColor, true);
    else
    {
        if (atst != 0) draw(0, 1, true, fWritesColor, true);
        switch (afail)
        {
        case 1: draw(1, 2, false, fWritesColor, true); break;    // FB_ONLY
        case 2: draw(1, 2, true, false, false); break;           // ZB_ONLY
        case 3: draw(1, 2, false, fWritesColor, false); break;   // RGB_ONLY
        default: break;                                          // KEEP
        }
    }
}

// The entry's cached commands (rebuilt when its shader or SAA frame changes).
const SUBGLOBGPU& SubgpuOf(const SUBGLOB& sub, GLuint vao, uint8_t fCull)
{
    SUBGLOBGPU* p = sub.pgpu;
    if (!p || p->powner != &sub)
    {
        p = &s_asubgpu.emplace_back();
        p->powner = &sub;
        sub.pgpu = p;
    }
    // (one cache per arena: the instances' own vertex copies live in other arenas)
    while (p->vao != vao && p->vao != 0)
    {
        if (!p->pnextVao)
        {
            SUBGLOBGPU* pnew = &s_asubgpu.emplace_back();
            pnew->powner = &sub;
            p->pnextVao = pnew;
        }
        p = p->pnextVao;
    }
    const int iframe = sub.psaa ? sub.psaa->iframe : 0;
    if (p->pshd != sub.pshd || p->iframe != iframe || p->vao != vao || p->fCull != fCull || p->genTex != s_genTexCache)
    {
        p->genTex = s_genTexCache;
        p->pshd = sub.pshd;
        p->iframe = iframe;
        p->vao = vao;
        p->fCull = fCull;
        p->apasscmd.clear();
        DSTATE st;
        st.vao = vao;
        st.fCull = fCull;
        for (size_t ipass = 0; ipass < sub.pshd->apass.size(); ++ipass)
            EmitPass(p->apasscmd, st, sub, sub.pshd->apass[ipass], int(ipass));
    }
    return *p;
}

// ------------------------------------------------------------ cast shadows (mesh type 2)
// What its VU1 program draws, reconstructed from the data and 0x1A4478's shadow quadwords (ShadowOfLightContext):
// the mesh's records are edges {OA, E0, E1, OB} (vertex index x 2: each vertex twice in VU memory, as itself and
// projected), so the program extrudes the silhouette (the edges between a face toward the shadow's centre and one
// away from it) from the vertices to their projections from the centre onto the far plane (centre + axis, normal
// along the axis), with the faces toward the centre as the near cap and their projections as the far cap: a
// shadow volume. The pixels inside are darkened like the type-3 hulls (z-fail stencil, the shader's alpha).
std::vector<glm::vec3> s_avtxShadow2;
GLSHADER s_shShadowVol;
GLuint s_vaoShadowVol = 0, s_vboShadowVol = 0;

bool FBuildShadow2(const SCENEGLOBSET& sgs, size_t iglob, const GLOB& glob, const glm::vec3& posCamera, float sNear, CMD& cmd)
{
    static const bool s_fDbg = std::getenv("SLY2_SH2DBG") != nullptr;
    static const char* const s_szOnly = std::getenv("SLY2_SH2ONLY");
    if (s_fDbg)
        std::printf("shadow2: oid %d sgs %p glob %zu sMRD %g grfGroup 0x%X n0 %d cbone %d fx %zu oidOwner %d gZ %g\n", sgs.oid, (const void*)&sgs, iglob, glob.sMRD,
                    glob.grfGroup, glob.n0, glob.cbone, glob.asubfx.size(), glob.oidOwner, glob.gZOrder);
    if (s_szOnly && int(iglob) != std::atoi(s_szOnly)) return false;
    const glm::mat4& mat = sgs.amatGlob[iglob];
    const std::vector<glm::mat4>* pamatBone = iglob < sgs.aamatBone.size() && !sgs.aamatBone[iglob].empty() ? &sgs.aamatBone[iglob] : nullptr;
    // the world positions (skinned by the weights' bones)
    static std::vector<std::vector<glm::vec3>> s_aav;
    s_aav.resize(glob.asubfx.size());
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (size_t k = 0; k < glob.asubfx.size(); ++k)
    {
        const SUBFX& fx = glob.asubfx[k];
        auto& av = s_aav[k];
        av.resize(fx.apos.size());
        for (size_t v = 0; v < fx.apos.size(); ++v)
        {
            const glm::vec4 p(fx.apos[v], 1.0f);
            glm::vec3 w(0.0f);
            if (pamatBone && fx.cw > 0)
            {
                for (int j = 0; j < fx.cw && j < 4; ++j)
                {
                    const size_t ib = std::min<size_t>(fx.aiboneSlot[j], pamatBone->size() - 1);
                    w += float(fx.aw[v * size_t(fx.cw) + size_t(j)]) / 255.0f * glm::vec3(MulMat4Vec4((*pamatBone)[ib], p));
                }
            }
            else w = glm::vec3(MulMat4Vec4(mat, p));
            av[v] = w;
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    }
    const glm::vec3 posCtx = sgs.fPosLight ? sgs.posLight : (lo + hi) * 0.5f;
    float sRadius = 0;   // (pl +0x1C: the shadow-casting hierarchy's radius about the context's position)
    for (const auto& av : s_aav)
        for (const glm::vec3& v : av) sRadius = std::max(sRadius, glm::length(v - posCtx));
    const SHADOWCTX sh = ShadowOfLightContext(WorldLights(), posCtx, sRadius, 400.0f, posCamera, sNear);   // (+0x13C 0x2C3B90)
    if (!sh.fValid) return false;
    if (s_fDbg)
        std::printf("shadow2: lo %.0f %.0f %.0f hi %.0f %.0f %.0f bones %zu\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, pamatBone ? pamatBone->size() : 0),
        std::printf("shadow2: ctx %.0f %.0f %.0f r %.0f centre %.0f %.0f %.0f axis %.0f %.0f %.0f\n", posCtx.x, posCtx.y, posCtx.z, sRadius,
                    sh.center.x, sh.center.y, sh.center.z, sh.axis.x, sh.axis.y, sh.axis.z);
    const glm::vec3 dir = glm::normalize(sh.axis);
    const glm::vec3 posFar = sh.center + sh.axis;
    const float dFar = glm::dot(posFar - sh.center, dir);
    // PC QoL "Long shadows" (display.h DISPLAY::fLongShadows, default on; NOT the original, render.md "Long shadows"):
    // the original ends every prism at the plane 400 past the caster along the axis (+0x13C), so a low light (the
    // title's window) cuts the shadow off at a straight edge on the floor (R39). Here a ray from the centre through a
    // vertex that descends is lengthened, never shortened, until it reaches the horizontal plane kLongShadowDepth below
    // the caster's lowest vertex (its feet: the floor it stands on), at most kLongShadowMaxExtra past the vertex. So the
    // far end is under the floor the shadow falls on (out of view) instead of on it, and the extension cannot reach
    // more than kLongShadowDepth below the caster (no shadows through floors onto the level below, no streaks across
    // the level). Rays that do not descend (a light below the vertex) keep the original's length.
    constexpr float kLongShadowDepth = 50.0f, kLongShadowMaxExtra = 2000.0f;
    static const bool s_fLongOff = [] { const char* sz = std::getenv("SLY2N_LONGSHADOWS"); return sz && sz[0] == '0'; }();
    const bool fLong = g_display.fLongShadows && !s_fLongOff;
    const float zLongFloor = lo.z - kLongShadowDepth;
    auto project = [&](const glm::vec3& v) {
        const glm::vec3 dvc = v - sh.center;
        const float dv = glm::dot(dvc, dir);
        if (dv <= 1.0f) return v + dir * dFar;
        float s = dFar / dv;
        const float dzDown = -dvc.z;   // (how far the vertex is below the centre)
        if (fLong && dzDown > 1e-3f)
        {
            const float sFloor = (sh.center.z - zLongFloor) / dzDown;
            if (sFloor > s) s = std::min(sFloor, std::max(s, 1.0f + kLongShadowMaxExtra / glm::length(dvc)));
        }
        return sh.center + dvc * s;
    };
    const uint32_t ivtxFirst = uint32_t(s_avtxShadow2.size());
    // (perf-3: a prism's 24 vertices written through one resize, not 24 push_backs)
    glm::vec3* pout = nullptr;
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, const glm::vec3& outward) {
        if (glm::dot(glm::cross(b - a, c - a), outward) < 0.0f) std::swap(b, c);
        pout[0] = a;
        pout[1] = b;
        pout[2] = c;
        pout += 3;
    };
    for (size_t k = 0; k < glob.asubfx.size(); ++k)
    {
        const SUBFX& fx = glob.asubfx[k];
        const auto& av = s_aav[k];
        auto P = [&](uint8_t u) { const size_t i = u / 2; return i < av.size() ? av[i] : glm::vec3(0.0f); };
        auto lit = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
            return glm::dot(glm::cross(b - a, c - a), dir) < 0.0f;
        };
        // each face once (it is in three edge records), as its own closed prism from the face to its projection:
        // the union of the prisms is the shadow whatever the mesh's closure and winding (the records' edges
        // between faces toward and away from the centre are the silhouette of a closed mesh; the hulls are not
        // all closed: a silhouette-only volume leaks there)
        // (each face once: a bit per sorted vertex triple; the records index at most 128 vertices x 2)
        static std::vector<uint64_t> s_abDone((size_t(1) << 21) / 64, 0);
        static std::vector<uint32_t> s_akDone;
        for (uint32_t k : s_akDone) s_abDone[k >> 6] = 0;
        s_akDone.clear();
        auto prism = [&](uint8_t ia, uint8_t ib, uint8_t ic) {
            std::array<uint8_t, 3> key{ia, ib, ic};
            // (sorted: a three-element network, the same order as std::sort)
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[2] < key[1]) std::swap(key[1], key[2]);
            if (key[1] < key[0]) std::swap(key[0], key[1]);
            if (key[0] == key[1] || key[1] == key[2]) return;
            const uint32_t k = (uint32_t(key[0] >> 1) << 14) | (uint32_t(key[1] >> 1) << 7) | uint32_t(key[2] >> 1);
            if (s_abDone[k >> 6] & (uint64_t(1) << (k & 63))) return;
            s_abDone[k >> 6] |= uint64_t(1) << (k & 63);
            s_akDone.push_back(k);
            const glm::vec3 a = P(ia), b = P(ib), c = P(ic);
            if (!lit(a, b, c) && !lit(a, c, b)) return;   // (edge-on)
            const glm::vec3 af = project(a), bf = project(b), cf = project(c);
            const glm::vec3 m = (a + b + c + af + bf + cf) / 6.0f;
            const size_t ivtx = s_avtxShadow2.size();
            s_avtxShadow2.resize(ivtx + 24);
            pout = s_avtxShadow2.data() + ivtx;
            auto side = [&](const glm::vec3& p, const glm::vec3& q, const glm::vec3& r) { tri(p, q, r, (p + q + r) / 3.0f - m); };
            side(a, b, c);
            side(af, bf, cf);
            side(a, b, bf); side(a, bf, af);
            side(b, c, cf); side(b, cf, bf);
            side(c, a, af); side(c, af, cf);
        };
        for (size_t r = 0; r + 3 < fx.arec.size(); r += 4)
        {
            const uint8_t oa = fx.arec[r], e0 = fx.arec[r + 1], e1 = fx.arec[r + 2], ob = fx.arec[r + 3];
            prism(oa, e0, e1);
            prism(e1, e0, ob);
        }
    }
    cmd.firstIndex = ivtxFirst;
    cmd.count = uint32_t(s_avtxShadow2.size()) - ivtxFirst;
    cmd.idraw = uint32_t(glob.asubfx[0].pshd->argba[0] >> 24);   // (the darkening alpha)
    return cmd.count > 0;
}

// The stencil while a shadow phase is open (0x16DA48 .. 0x16DD58), as the original's frame green: kind 7 counts in
// bits 0..6 from 0x3F (0x16DA48's fill 0x7E, the hulls' +-2 per face: G bit 7), so bit 6 is set where the count is
// 1..64; kind 8 counts in bits 0..5 from 0x1F (fill 0x3E keeping bit 7: G bit 6), bit 5 set at 1..32, bit 6 kept.
// STENCIL_FBA: the frame alpha's bit 7 the FBA kinds wrote (STATECACHE).
constexpr GLuint STENCIL_FBA = 0x80, STENCIL_SH7 = 0x40, STENCIL_SH8 = 0x20, STENCIL_COUNT7 = 0x7F, STENCIL_COUNT8 = 0x3F;

void DrawShadow2(const CMD& cmd, const glm::mat4& matWorldToClip)
{
    if (!s_shShadowVol.ID)
    {
        s_shShadowVol.Init("shadowvol.vert", "shadowvol.frag");
        glGenVertexArrays(1, &s_vaoShadowVol);
        glGenBuffers(1, &s_vboShadowVol);
        glBindVertexArray(s_vaoShadowVol);
        glBindBuffer(GL_ARRAY_BUFFER, s_vboShadowVol);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    }
    s_shShadowVol.Use();
    glUniformMatrix4fv(s_shShadowVol.Loc("matWorldToClip"), 1, GL_FALSE, glm::value_ptr(matWorldToClip));
    glUniform1i(s_shShadowVol.Loc("texDate"), 1);
    glUniform1i(s_shShadowVol.Loc("fDate"), (cmd.order & 8) ? 0 : 1);   // (the shader's flag 8: no DATE)
    glUniform4f(s_shShadowVol.Loc("rgba"), 0, 0, 0, 0);
    glBindVertexArray(s_vaoShadowVol);
    glBindBuffer(GL_ARRAY_BUFFER, s_vboShadowVol);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(size_t(cmd.count) * sizeof(glm::vec3)), s_avtxShadow2.data() + cmd.firstIndex, GL_STREAM_DRAW);
    // the prisms' z-fail count into kind 7's (closed volumes: the original's +-2 per face in front of the scene)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(STENCIL_COUNT7);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_EQUAL, 0, (cmd.order & 8) ? 0 : STENCIL_FBA);   // (DATE: the alpha's bit 7 clear)
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(cmd.count));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

void DrawOne(const CMD& cmd)
{
    glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, GLsizei(cmd.count), GL_UNSIGNED_INT,
                                                  reinterpret_cast<void*>(size_t(cmd.firstIndex) * 4), 1, cmd.baseVertex, cmd.idraw);
    ++g_renderstats.cpass;
}

// Shader kind 3 (rp 11 / 13): a projected volume. 0x1EB960's four lists use the destination alpha's bit 7 as a
// mask (two untextured passes over the volume, a DATE-tested fill with (rgba1 + 1) / 2, a last pass with rgba2);
// as in Sly1-Release's DrawProjVolume the mask is the stencil's bit 7: set where the scene is in front of the
// volume's back faces, cleared where it is in front of its front faces, then filled where it stays set.
void DrawProjVolume(const CMD& cmd)
{
    const SUBGLOB& sub = *cmd.psub;
    const SHD* pshd = sub.pshd;
    const uint8_t fl = pshd->grfshd;
    // (debug: SLY2N_VOLPRINT=1: each volume drawn, its object, shader and place)
    if (static const bool s_fDbg = std::getenv("SLY2N_VOLPRINT") != nullptr; s_fDbg)
        std::printf("vol: oid %d glob %zu kind %d shd %d grfshd 0x%X tris %u rgba %08X %08X %08X at %.0f %.0f %.0f\n",
                    cmd.prec->psgs ? cmd.prec->psgs->oid : -1, cmd.prec->iglob, cmd.prec->kind, pshd->id, unsigned(fl), cmd.count / 3,
                    pshd->argba[0], pshd->argba[1], pshd->argba[2], (*cmd.prec->pmat)[3].x, (*cmd.prec->pmat)[3].y, (*cmd.prec->pmat)[3].z);
    glBindVertexArray(cmd.vao);
    glUniform1i(s_u.fTextured, 0);
    glUniform1i(s_u.ate, 0);
    glUniform1i(s_u.afailMode, 0);
    glm::vec4 c;
    for (int i = 0; i < 4; ++i) c[i] = float(((pshd->argba[1] >> (8 * i)) & 0xFF) + 1) / 2.0f;
    if (!(fl & 2))
    {
        // An untextured volume (the murk of rp 13, e.g. the intro's rooftop 150055): the mask as the original's lists
        // make it, so only the scene inside the volume is filled (the gang's silhouettes in the dark), not the
        // volume's faces in front of everything: bit 7 set where the scene is in front of a back face (z-fail),
        // cleared where it is in front of a front face, filled (and cleared) where it stays set.
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0x80);
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_GREATER);
        glDisable(GL_BLEND);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_ALWAYS, 0x80, 0x80);
        glCullFace(GL_FRONT);
        glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
        DrawOne(cmd);
        glCullFace(GL_BACK);
        glStencilOp(GL_KEEP, GL_ZERO, GL_KEEP);
        DrawOne(cmd);
        // the fill through the back faces, without the depth test: the camera may be inside the volume (no front face
        // in view), and a back face covers every pixel whose scene point can be inside
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_DEPTH_TEST);
        glCullFace(GL_FRONT);
        glStencilFunc(GL_EQUAL, 0x80, 0x80);
        glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
        glEnable(GL_BLEND);
        FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
        // (debug: SLY2N_VOLDBG=1: the filled pixels in a colour per volume, red, green, blue, ... in draw order)
        static const bool s_fVolDbg = std::getenv("SLY2N_VOLDBG") != nullptr;
        static int s_ivolDbg = 0;
        static const float s_argbDbg[8][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}, {255, 128, 0}};
        if (s_fVolDbg) { const float* prgb = s_argbDbg[s_ivolDbg++ % 8]; glUniform4f(s_u.rgbaConst, prgb[0], prgb[1], prgb[2], 64.0f); }
        else glUniform4f(s_u.rgbaConst, c.r, c.g, c.b, c.a);
        // (the lists' DATE fill is a flat sprite of (rgba1 + 1) / 2, not faded by the hull's vertex alpha: SLY2N_VOLVA=1
        // fades it for comparison)
        static const bool s_fVa = std::getenv("SLY2N_VOLVA") != nullptr;
        glUniform1i(s_u.icolor, s_fVa ? 3 : 2);
        DrawOne(cmd);
        glCullFace(GL_BACK);
        glEnable(GL_DEPTH_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        return;
    }
    // A textured volume (light beams): the volume's front faces in front of the scene, filled once per pixel
    // (stencil bit 7), with rgba1 / 2 faded by the vertex alpha; then the bit is cleared again. (The exact P_G
    // 0x3628 / 0x37A8 passes are not reconstructed; this matches the beams PCSX2 shows.)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x80);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_NOTEQUAL, 0x80, 0x80);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glEnable(GL_BLEND);
    FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
    glUniform4f(s_u.rgbaConst, c.r, c.g, c.b, c.a);
    glUniform1i(s_u.icolor, 3);
    DrawOne(cmd);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_ALWAYS, 0, 0x80);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
    DrawOne(cmd);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    if (fl & 2)
    {
        // The textured surface of the volume (list 0 with flag 2): blended, no Z write.
        glFrontFace(GL_CCW);
        glDepthFunc(GL_GEQUAL);
        const TEXSEL sel = TexselFromShd(pshd, sub.psaa ? sub.psaa->iframe : 0, 0);
        const GLuint tex = TexFromSel(sel);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(0, s_asampler[SamplerFor(tex, uint8_t((sel.fClampS ? 1 : 0) | (sel.fClampT ? 2 : 0)))]);
        glUniform1i(s_u.fTextured, 1);
        glUniform1i(s_u.icolor, 0);
        FSetBlend((uint64_t((fl & 1) ? 2 : 1) << 2) | 0x40u);
        DrawOne(cmd);
    }
}

// Whether a point (the hull's space) is inside a type-3 hull: a ray's crossings of its triangles (as BuildGlobGpu
// makes them), odd inside. (The list-1 caps the original adds where the near plane is inside the hull.)
// A type-3 hull's triangles in its own space (as BuildGlobGpu makes them), kept per glob.
const std::vector<glm::vec3>& HullTriangles(const GLOB& glob)
{
    static std::unordered_map<const GLOB*, std::vector<glm::vec3>> s_mpatri;
    auto [it, fNew] = s_mpatri.try_emplace(&glob);
    std::vector<glm::vec3>& atri = it->second;
    if (fNew)
        for (const SUBFX& fx : glob.asubfx)
            for (size_t k = 2; k < fx.aipos.size(); ++k)
            {
                if ((fx.amask[k / 32] >> (k % 32)) & 1) continue;
                for (size_t j = k - 2; j <= k; ++j) atri.push_back(fx.aipos[j] < fx.apos.size() ? fx.apos[fx.aipos[j]] : glm::vec3(0.0f));
            }
    return atri;
}

bool FPointInHull(const GLOB& glob, const glm::vec3& pos)
{
    const std::vector<glm::vec3>& atri = HullTriangles(glob);
    const glm::vec3 dir = glm::normalize(glm::vec3(0.1234f, 0.4567f, 0.8812f));
    int c = 0;
    for (size_t i = 0; i + 2 < atri.size(); i += 3)
    {
        const glm::vec3 e1 = atri[i + 1] - atri[i], e2 = atri[i + 2] - atri[i];
        const glm::vec3 p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-12f) continue;
        const glm::vec3 t = pos - atri[i];
        const float u = glm::dot(t, p) / det;
        if (u < 0.0f || u > 1.0f) continue;
        const glm::vec3 q = glm::cross(t, e1);
        const float v = glm::dot(dir, q) / det;
        if (v < 0.0f || u + v > 1.0f) continue;
        if (glm::dot(e2, q) / det > 0.0f) ++c;
    }
    return c & 1;
}

// Whether the segment a..b crosses the triangle (p0, p1, p2).
bool FSegmentHitsTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2)
{
    const glm::vec3 d = b - a, e1 = p1 - p0, e2 = p2 - p0;
    const glm::vec3 p = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const glm::vec3 t = a - p0;
    const float u = glm::dot(t, p) / det;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(t, e1);
    const float v = glm::dot(d, q) / det;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float s = glm::dot(e2, q) / det;
    return s >= 0.0f && s <= 1.0f;
}

// Whether the drawn near plane (its four corners, the hull's space) is inside the hull anywhere: a hull triangle cuts
// the near rectangle (a triangle's edge through it or the rectangle's edge through the triangle), or the rectangle lies
// inside (its centre, ray parity).
bool FNearPlaneInHull(const GLOB& glob, const std::array<glm::vec3, 4>& apos)
{
    const std::vector<glm::vec3>& atri = HullTriangles(glob);
    for (size_t i = 0; i + 2 < atri.size(); i += 3)
    {
        const glm::vec3* pt = &atri[i];
        for (int k = 0; k < 3; ++k)
            if (FSegmentHitsTriangle(pt[k], pt[(k + 1) % 3], apos[0], apos[1], apos[2]) ||
                FSegmentHitsTriangle(pt[k], pt[(k + 1) % 3], apos[0], apos[2], apos[3]))
                return true;
        for (int k = 0; k < 4; ++k)
            if (FSegmentHitsTriangle(apos[size_t(k)], apos[size_t((k + 1) % 4)], pt[0], pt[1], pt[2])) return true;
    }
    return FPointInHull(glob, (apos[0] + apos[1] + apos[2] + apos[3]) * 0.25f);
}

// Shader kind 2 on rp 7 / 8: shadow hulls. The original marks the scene inside them in the frame's green channel
// (0x16DA48 fills it with 0x7E / 0x3E; the hull's faces in front of the scene (list 0, TEST GREATER) subtract their
// colour, +2 or -2 (254) by their facing: the count goes up by one per front face and down by one per back face in
// front of the scene; DATE without the shader's flag 8; the faces the camera's near plane cuts add theirs through list 1,
// ZTST ALWAYS; reference GS log, render.md "shadow frame-channel pass"), and 0x16DD58 darkens the marked pixels by the
// scene's own alpha (ShadowCombine). Natively the same count in the stencil: z-pass (front +1, back -1 where they pass
// the depth test), or z-fail when the drawn near plane is inside the hull anywhere (list 1's near-plane caps: the same
// count for a closed hull). Only the hulls the near plane reaches: a z-fail count for every hull near the camera marked
// open hulls' insides the original leaves alone; a camera-inside test missed the hulls the near plane cuts with the
// camera just outside (R47: a screen-tall unmarked band, the hull's part behind the near plane uncounted).
void DrawShadow(const CMD& cmd, const glm::vec3& posCamera, float sNearClip, const std::array<glm::vec3, 4>& aposNear)
{
    const GLOB& geom = *cmd.prec->pglobGeom;
    const GLuint maskCount = cmd.prec->kind == 8 ? STENCIL_COUNT8 : STENCIL_COUNT7;
    bool fNear = false;
    if (geom.pgl && geom.pgl->sRadius > 0.0f)
    {
        const glm::mat4& mat = *cmd.prec->pmat;
        const float scale = std::max({glm::length(glm::vec3(mat[0])), glm::length(glm::vec3(mat[1])), glm::length(glm::vec3(mat[2]))});
        const glm::vec3 c = glm::vec3(mat * glm::vec4(geom.pgl->posCenter, 1.0f));
        float sNearExtent = sNearClip;
        for (const glm::vec3& pos : aposNear) sNearExtent = std::max(sNearExtent, glm::distance(posCamera, pos));
        if (glm::distance(posCamera, c) < geom.pgl->sRadius * scale + sNearExtent)
        {
            const glm::mat4 matInv = glm::inverse(mat);
            std::array<glm::vec3, 4> apos;
            for (size_t i = 0; i < 4; ++i) apos[i] = glm::vec3(matInv * glm::vec4(aposNear[i], 1.0f));
            static const bool s_fCamOnly = std::getenv("SLY2N_SHNEARCAM") != nullptr;   // (debug: the camera-inside test only)
            fNear = s_fCamOnly ? glm::distance(posCamera, c) < geom.pgl->sRadius * scale + sNearClip &&
                                     FPointInHull(geom, glm::vec3(matInv * glm::vec4(posCamera, 1.0f)))
                               : FNearPlaneInHull(geom, apos);
        }
    }
    static const int s_iShMode = std::getenv("SLY2N_SHCOUNT") ? std::atoi(std::getenv("SLY2N_SHCOUNT")) : 0;   // (debug: 1 z-pass, 2 z-fail)
    if (s_iShMode) fNear = s_iShMode == 2;
    const bool fDate = !(geom.asubfx[0].pshd->grfshd & 8);
    if (static const bool s_fDbg = std::getenv("SLY2N_SHHULLDBG") != nullptr; s_fDbg)
        std::printf("shhull: oid %d glob %zu kind %d grfshd 0x%X near %d tris %u r %.0f\n", cmd.prec->psgs ? cmd.prec->psgs->oid : -1, cmd.prec->iglob,
                    cmd.prec->kind, unsigned(geom.asubfx[0].pshd->grfshd), int(fNear), cmd.count / 3, geom.pgl ? geom.pgl->sRadius : -1.0f);
    glBindVertexArray(cmd.vao);
    glUniform1i(s_u.fTextured, 0);
    glUniform1i(s_u.ate, 0);
    glUniform1i(s_u.afailMode, 0);
    glUniform4f(s_u.rgbaConst, 0, 0, 0, 0);
    glUniform1i(s_u.icolor, 2);
    glUniform1i(s_u.fDate, fDate ? 1 : 0);   // (DATE: the alpha's bit 7 clear: the FBA bit, or alpha >= 0x80)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(maskCount);
    glStencilFunc(GL_EQUAL, 0, fDate ? STENCIL_FBA : 0);
    if (fNear)
    {
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    }
    else
    {
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GREATER);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    DrawOne(cmd);
    glUniform1i(s_u.fDate, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
}

// 0x16DA48 (AddScreenEffect) at a shadow phase's start: the frame as the scene left it (the original copies its green
// plane away, 0x16D6C8 0x299, and fills it with the marks' base); natively its alpha into a texture the hulls' DATE
// and the combine read (resolved when multisampled).
GLuint s_fboShAlpha = 0, s_texShAlpha = 0;
int s_dxShAlpha = 0, s_dyShAlpha = 0;
GLSHADER s_shShadowCombine;
void ShadowBegin(GLuint fbo, int dx, int dy, int kind)
{
    if (!s_fboShAlpha || dx != s_dxShAlpha || dy != s_dyShAlpha)
    {
        if (!s_fboShAlpha) { glGenFramebuffers(1, &s_fboShAlpha); glGenTextures(1, &s_texShAlpha); }
        glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShAlpha);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_texShAlpha, 0);
        s_dxShAlpha = dx;
        s_dyShAlpha = dy;
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fboShAlpha);
    glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
    glActiveTexture(GL_TEXTURE0);
    // the count's base (the green fill)
    glStencilMask(STENCIL_COUNT7);
    glClearStencil(kind == 8 ? 0x1F : 0x3F);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    // (debug: SLY2N_SHALPHA=dir writes each phase's frame alpha x 2 as a grey PNG)
    if (static const char* s_szDir = std::getenv("SLY2N_SHALPHA"); s_szDir)
    {
        static int s_c = 0;
        std::vector<uint8_t> ab(size_t(dx) * size_t(dy) * 4), ag(size_t(dx) * size_t(dy));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fboShAlpha);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dx, dy, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        for (int y = 0; y < dy; ++y)
            for (int x = 0; x < dx; ++x) ag[size_t(dy - 1 - y) * size_t(dx) + size_t(x)] = ab[(size_t(y) * size_t(dx) + size_t(x)) * 4 + 3];
        stbi_write_png((std::string(s_szDir) + "/shalpha_" + std::to_string(s_c++) + ".png").c_str(), dx, dy, 1, ag.data(), dx);
    }
}

// 0x16DA48 for kind 8 after kind 7: the kind-8 count's base, kind 7's mark (bit 6) kept (FBMSK 0xFFFF80FF).
void ShadowKind8()
{
    glStencilMask(STENCIL_COUNT8);
    glClearStencil(0x1F);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
}

// Past the FBA kinds without a shadow phase: the stencil's FBA bits cleared (the volumes' mask, kinds 11 / 13, is bit 7).
void ClearFbaBits()
{
    glStencilMask(STENCIL_FBA);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
}

// 0x16DD58 (AddScreenCombine) at the phase's end: the CLUT pass (shadowcombine.frag), one draw per ramp the marks make
// (their pixels are disjoint: one pass over the frame in all), then the unmarked pixels' alpha (entry 0); the marks
// are cleared. f7 / f8: the phase drew kind 7 / kind 8.
void ShadowCombine(bool f7, bool f8)
{
    // (debug: SLY2N_SHMARKS=dir writes each combine's marks: red kind 7, green kind 8, blue the FBA bit; no MSAA)
    if (static const char* s_szDir = std::getenv("SLY2N_SHMARKS"); s_szDir && s_dxShAlpha > 0)
    {
        static int s_c = 0;
        const int dx = s_dxShAlpha, dy = s_dyShAlpha;
        std::vector<uint8_t> as(size_t(dx) * size_t(dy)), argb(size_t(dx) * size_t(dy) * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dx, dy, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, as.data());
        for (int y = 0; y < dy; ++y)
            for (int x = 0; x < dx; ++x)
            {
                const uint8_t v = as[size_t(y) * size_t(dx) + size_t(x)];
                uint8_t* p = &argb[(size_t(dy - 1 - y) * size_t(dx) + size_t(x)) * 3];
                p[0] = (v & STENCIL_SH7) ? 255 : 0;
                p[1] = f8 && (v & STENCIL_SH8) ? 255 : 0;
                p[2] = (v & STENCIL_FBA) ? 128 : 0;
            }
        stbi_write_png((std::string(s_szDir) + "/shmarks_" + std::to_string(s_c++) + ".png").c_str(), dx, dy, 3, argb.data(), dx * 3);
    }
    if (!s_shShadowCombine.ID) s_shShadowCombine.Init("shadowcombine.vert", "shadowcombine.frag");
    s_shShadowCombine.Use();
    glUniform1i(s_shShadowCombine.Loc("texAlpha"), 1);
    const GLint locGain = s_shShadowCombine.Loc("gain"), locM8 = s_shShadowCombine.Loc("m8"), locReset = s_shShadowCombine.Loc("fReset");
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_texShAlpha);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vaoEmpty);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);   // (0 - Cd) x As / 128 + Cd; A = As
    glUniform1i(locReset, 0);
    // (GAME +0x3E64 / +0x3E70, 0x1EA9F0: 128 / 96 / 160 and black in every world's RAM seen)
    struct RAMP { GLuint stencil; float gain; int m8; bool f; };
    const RAMP aramp[] = {{STENCIL_SH7, 128.0f, 0, f7}, {STENCIL_SH8, 96.0f, 1, f8}, {STENCIL_SH7 | STENCIL_SH8, 160.0f, 1, f7 && f8}};
    const GLuint maskMarks = f8 ? STENCIL_SH7 | STENCIL_SH8 : STENCIL_SH7;   // (without kind 8, bit 5 is kind 7's count)
    for (const RAMP& r : aramp)
    {
        if (!r.f) continue;
        glStencilFunc(GL_EQUAL, GLint(r.stencil), maskMarks);
        glUniform1f(locGain, r.gain / 255.0f);
        glUniform1i(locM8, r.m8);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glStencilFunc(GL_EQUAL, 0, maskMarks);
    glUniform1i(locReset, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
}
}  // namespace

void UpdateGlobColorsGpu(const GLOB& glob) { UpdateGlobColorsGpu(glob, glob.pgl); }

void UpdateGlobVerticesGpu(const GLOB& glob)
{
    const GLGLOB* pgl = glob.pgl;
    if (!pgl || !pgl->vbo || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    static std::vector<GPUVTX> s_av;
    GpuVerticesOfGlob(glob, s_av);
    glBindBuffer(GL_ARRAY_BUFFER, pgl->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(size_t(pgl->baseVertex) * sizeof(GPUVTX)), GLsizeiptr(s_av.size() * sizeof(GPUVTX)), s_av.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    UpdateGlobColorsGpu(glob, pgl);
}

void SetGlobBoundsGpu(const GLOB& glob, const glm::vec3& posCenter, float sRadius)
{
    if (!glob.pgl) return;
    glob.pgl->posCenter = posCenter;
    glob.pgl->sRadius = sRadius;
}

void UpdateGlobColorsGpu(const GLOB& glob, const GLGLOB* pgl)
{
    if (!pgl || !pgl->vboColor || glob.meshk != MESHK_Strips || glob.avtx.empty()) return;
    static std::vector<GPUCOL> s_acol;
    s_acol.resize(glob.avtx.size());
    for (size_t i = 0; i < s_acol.size(); ++i)
    {
        std::memcpy(s_acol[i].rgbaA, &glob.avtx[i].rgbaA, 4);
        std::memcpy(s_acol[i].rgbaB, &glob.avtx[i].rgbaB, 4);
    }
    glBindBuffer(GL_ARRAY_BUFFER, pgl->vboColor);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(size_t(pgl->baseVertex) * sizeof(GPUCOL)), GLsizeiptr(s_acol.size() * sizeof(GPUCOL)), s_acol.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

GLGLOB* PglInstanceCopyGpu(const GLOB& glob)
{
    if (glob.meshk != MESHK_Strips || glob.avtx.empty() || glob.aindex.empty()) return nullptr;
    auto pgl = std::make_unique<GLGLOB>();
    std::vector<GPUVTX> av;
    GpuVerticesOfGlob(glob, av);
    AppendArena(pgl.get(), av, glob.aindex);
    GLGLOB* p = pgl.get();
    s_aglglob.push_back(std::move(pgl));
    return p;
}

void FlushInstanceGlobsGpu() { FlushArena(); }

void ResetTextureCache()
{
    // The cache is keyed by the old world's BMP / CLUT records, which ResetShaders frees: a new world's record may get
    // a freed one's address (the wrong texture), and the old textures would stay in GPU memory for the whole session
    // (with texture mods, ~400 MiB per world at 4x: MODDING.md "Limits").
    for (const auto& [key, tex] : s_mptex)
        if (tex && tex != s_texWhite)
        {
            s_atexFreed.push_back(tex);
            ForgetModTexture(tex);
        }
    s_mptex.clear();
    ++s_genTexCache;
}

unsigned GlTextureOfShd(const SHD* pshd, int iframe)
{
    InitCommon();
    return TexFromSel(TexselFromShd(pshd, iframe, 0));
}

glm::vec3 CAMERA::Dir() const
{
    if (fMat) return mat[0];
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return glm::vec3(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p));
}

glm::mat4 CAMERA::MatWorldToClip(float aspect) const
{
    // Reversed-Z infinite-far perspective (clip control 0..1): depth = near / w, greater is nearer.
    const float f = 1.0f / std::tan(glm::radians(fovY) * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][3] = -1.0f;
    proj[3][2] = sNear;
    if (fMat)
    {
        // GL eye axes from the camera's rows: right = -y, up = z, back = -x
        glm::mat4 eye(1.0f);
        eye[0] = glm::vec4(-mat[1], 0.0f);
        eye[1] = glm::vec4(mat[2], 0.0f);
        eye[2] = glm::vec4(-mat[0], 0.0f);
        eye[3] = glm::vec4(pos, 1.0f);
        return proj * glm::inverse(eye);
    }
    const glm::mat4 view = glm::lookAt(pos, pos + Dir(), glm::vec3(0, 0, 1));
    return proj * view;
}

void BuildSceneGpu(SCENE& scene)
{
    InitCommon();
    for (SCENEGLOBSET& sgs : scene.aglobset)
        for (GLOB& glob : sgs.pglobset->aglob)
            BuildGlobGpu(glob);
    FlushArena();
    s_u.matWorldToClip = glGlobShader.Loc("matWorldToClip");
    s_u.icolor = glGlobShader.Loc("icolor");
    s_u.rgbaConst = glGlobShader.Loc("rgbaConst");
    s_u.posCamera = glGlobShader.Loc("posCamera");
    s_u.dirCamera = glGlobShader.Loc("dirCamera");
    s_u.tex = glGlobShader.Loc("tex");
    s_u.fTextured = glGlobShader.Loc("fTextured");
    s_u.ate = glGlobShader.Loc("ate");
    s_u.atst = glGlobShader.Loc("atst");
    s_u.aref = glGlobShader.Loc("aref");
    s_u.afailMode = glGlobShader.Loc("afailMode");
    s_u.fogColor = glGlobShader.Loc("fogColor");
    s_u.paint = glGlobShader.Loc("paint");
    s_u.fFogBlack = glGlobShader.Loc("fFogBlack");
    s_u.fDate = glGlobShader.Loc("fDate");
    s_u.texDate = glGlobShader.Loc("texDate");
    s_uc.matWorldToClip = glCelBorderShader.Loc("matWorldToClip");
    s_uc.matModel = glCelBorderShader.Loc("matModel");
    s_uc.halfWidthNdc = glCelBorderShader.Loc("halfWidthNdc");
    s_uc.edgeBase = glCelBorderShader.Loc("edgeBase");
    s_uc.celRgba = glCelBorderShader.Loc("celRgba");
    s_uc.cbone = glCelBorderShader.Loc("cbone");
    s_uc.iboneFirst = glCelBorderShader.Loc("iboneFirst");
    if (!s_ssboRec)
    {
        glGenBuffers(1, &s_ssboBone);
        glGenBuffers(1, &s_ssboRec);
        glGenBuffers(1, &s_ssboDraw);
        glGenBuffers(1, &s_bufIndirect);
        glGenBuffers(1, &s_ssboWarp);
        glGenBuffers(1, &s_ssboWarpMat);
        glGenBuffers(1, &s_ssboTexh);
    }
}

namespace
{
// One view of the scene into one target: the frame (irtgt -1: the globsets not drawn into a render target) or a
// render target (irtgt 0 / 1: only its globsets, their kinds remapped as RenderGlobCtx does through 0x1BEE10).
struct PASSVIEW
{
    glm::mat4 matWorldToClip{1.0f};
    glm::vec3 pos{0.0f}, dir{1, 0, 0};
    GLuint fbo = 0;
    int dx = 1, dy = 1;
    float aspect = 1.0f;        // the target's pixel aspect (cel-border widths)
    float rMRDAdjust = 1.0f;
    uint32_t grfChannels = 0xFF;   // CM +0x14 (the volume tree's channel test)
    int irtgt = -1;
    bool fForeground = false;   // the foreground kinds 22..25 only (after the HUD, over a cleared depth)
    uint32_t rgbaClear = 0;
    uint8_t alphaClear = 0;
};

// orig: 0x002C0B28 / 0x002C0B90 (D_002C0BF8's per-target tables: a group kind -> its kind in the target, -1 not
// drawn): 3 / 4 -> 15 (18), 5 / 6 -> 16 (19), 9 -> 17 (20), the fog kinds and the target kinds likewise.
const int8_t s_amapkindRtgt[2][26] = {
    {-1, -1, -1, 15, 15, 16, 16, -1, -1, 17, -1, -1, -1, -1, -1, 15, 16, 17, 15, 16, 17, -1, 15, 16, 17, -1},
    {-1, -1, -1, 18, 18, 19, 19, -1, -1, 20, -1, -1, -1, -1, -1, 18, 19, 20, 18, 19, 20, -1, 18, 19, 20, -1},
};

// The regroup keys (order << 52 | state << 28 | index) ascending. The keys are distinct (the index), so any
// sort gives the same order: a stable LSD radix sort on bits 28..63 (the index part is already ascending)
// for the long segments, std::sort for the short ones.
void SortKeysUnique(std::vector<uint64_t>& akey)
{
    if (akey.size() < 512)
    {
        std::sort(akey.begin(), akey.end());
        return;
    }
    static std::vector<uint64_t> s_atmp;
    static uint32_t s_ac[4096];
    s_atmp.resize(akey.size());
    std::vector<uint64_t>* psrc = &akey;
    std::vector<uint64_t>* pdst = &s_atmp;
    for (int shift = 28; shift < 64; shift += 12)
    {
        std::fill(std::begin(s_ac), std::end(s_ac), 0u);
        for (uint64_t k : *psrc) ++s_ac[(k >> shift) & 0xFFF];
        uint32_t sum = 0;
        for (uint32_t& c : s_ac) { const uint32_t n = c; c = sum; sum += n; }
        for (uint64_t k : *psrc) (*pdst)[s_ac[(k >> shift) & 0xFFF]++] = k;
        std::swap(psrc, pdst);
    }
    if (psrc != &akey) akey.swap(*psrc);   // (three passes: the result is in the other buffer)
}

// orig: VU1 P_G 14-82 (the billboard modes: LoadGlob 0x167320 passes the group's S8 rtck to the type-0 loader, which
// makes rtck 0 / 1 / 2 the param flags 0x100 / 0x200 / 0x400; P_G then builds the model's basis from the camera
// position): Sly 1's RTCK with the values one lower (-1 none). 0: the model's x axis toward the camera, z the
// camera's up made square to it (Sly 1 RTCK_All, AdjustAloRtckMat); 1: x toward the camera about the world's z
// (RTCK_WorldZ). The axes keep the matrix's scales; the model turns about its origin. 2 (only on effect meshes in
// Paris): not changed.
// TODO: check the pivot and mode 2 against P_G (not disassembled here).
static bool FBillboardMatrix(const glm::mat4& m, int rtck, const glm::vec3& posCam, const glm::vec3& vecUpCam, glm::mat4& mOut)
{
    if (rtck != 0 && rtck != 1) return false;
    static const bool s_fOff = std::getenv("SLY2_NOBILLBOARD") != nullptr;   // debugging: the loaded rotation
    if (s_fOff) return false;
    const glm::vec3 pos(m[3]);
    glm::vec3 x = posCam - pos;
    if (rtck == 1) x.z = 0.0f;
    if (glm::dot(x, x) < 1e-6f) return false;
    x = glm::normalize(x);
    glm::vec3 z = rtck == 1 ? glm::vec3(0.0f, 0.0f, 1.0f) : vecUpCam - x * glm::dot(vecUpCam, x);
    if (glm::dot(z, z) < 1e-6f) return false;
    z = glm::normalize(z);
    const glm::vec3 y = glm::cross(z, x);
    mOut = glm::mat4(glm::vec4(x * glm::length(glm::vec3(m[0])), 0.0f), glm::vec4(y * glm::length(glm::vec3(m[1])), 0.0f),
                     glm::vec4(z * glm::length(glm::vec3(m[2])), 0.0f), m[3]);
    return true;
}

void RenderPass(const SCENE& scene, const PASSVIEW& pv, const RENDEROPTS& opts)
{
    const float aspect = pv.aspect;
    const glm::mat4& matWorldToClip = pv.matWorldToClip;
    struct { glm::vec3 pos, dir; glm::vec3 Dir() const { return dir; } } cam{pv.pos, pv.dir};
    const float sNearClipSh = g_cm.sNearClip;   // (CM +0x4C: the hulls' near test, DrawShadow)
    // the drawn near plane's corners (NDC depth 1 in the reversed-Z projection): the hulls it reaches count z-fail
    std::array<glm::vec3, 4> aposNear;
    {
        const glm::mat4 m = glm::inverse(matWorldToClip);
        const float ax[4] = {-1.0f, 1.0f, 1.0f, -1.0f}, ay[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        for (size_t i = 0; i < 4; ++i)
        {
            const glm::vec4 v = m * glm::vec4(ax[i], ay[i], 1.0f, 1.0f);
            aposNear[i] = glm::vec3(v) / v.w;
        }
    }
    // The view frustum's planes (Gribb / Hartmann on the clip matrix; reversed Z, infinite far: left, right, bottom,
    // top), normalized so a sphere is outside when its centre is at least its radius behind one.
    glm::vec4 aplane[4];
    {
        const glm::mat4 m = glm::transpose(matWorldToClip);   // rows of the clip matrix
        aplane[0] = m[3] + m[0];
        aplane[1] = m[3] - m[0];
        aplane[2] = m[3] + m[1];
        aplane[3] = m[3] - m[1];
        for (glm::vec4& p : aplane) p /= glm::length(glm::vec3(p));
    }
    CM cmFade;
    cmFade.rMRDAdjust = pv.rMRDAdjust * g_display.drawDistance;
    // (QoL draw distance: the cel borders (default MRD 2000) follow it only below 1x; above, the original's limit
    // stays, or the far characters' fixed-width outlines cover them)
    CM cmFadeCel;
    cmFadeCel.rMRDAdjust = pv.rMRDAdjust * std::min(g_display.drawDistance, 1.0f);
    static const int s_oidFade = std::getenv("SLY2_FADEOID") ? std::atoi(std::getenv("SLY2_FADEOID")) : -2;
    auto maxScale = [](const glm::mat4& m) {
        return std::sqrt(std::max({glm::dot(glm::vec3(m[0]), glm::vec3(m[0])),
                                   glm::dot(glm::vec3(m[1]), glm::vec3(m[1])),
                                   glm::dot(glm::vec3(m[2]), glm::vec3(m[2]))}));
    };
    auto outside = [&](const glm::vec3& pos, float radius) {
        for (int i = 0; i < 4; ++i)
            if (glm::dot(glm::vec3(aplane[i]), pos) + aplane[i].w <= -radius) return true;
        return false;
    };
    static const bool s_fNoFrustum = std::getenv("SLY2_NOFRUSTUM") != nullptr;
    static const bool s_fNoFade = std::getenv("SLY2_NOFADE") != nullptr;   // debugging: no distance fade
    static const bool s_fNoTree = std::getenv("SLY2_NOTREE") != nullptr;   // debugging: the volume tree's culling off

    // Draw records (RenderSw 0x1CDA80 / RenderGlob 0x168638): kind = the group's rp, z = distance squared.
    // (one buffer per target kind: the frame's pass and the render targets' passes keep their records apart,
    // reused across frames without reallocating)
    static std::vector<DRAWREC> s_aarec[1 + CRTGT];
    std::vector<DRAWREC>& arec = s_aarec[size_t(pv.irtgt + 1)];
    arec.clear();
    // the billboarded groups' matrices this pass (a deque: the records point into it)
    static std::deque<glm::mat4> s_aamatBillboard[1 + CRTGT];
    std::deque<glm::mat4>& amatBillboard = s_aamatBillboard[size_t(pv.irtgt + 1)];
    amatBillboard.clear();
    const glm::vec3 vecUpCam = [&] {
        const glm::vec3 r1(matWorldToClip[0][1], matWorldToClip[1][1], matWorldToClip[2][1]);   // the clip matrix's y row
        return glm::dot(r1, r1) > 0.0f ? glm::normalize(r1) : glm::vec3(0.0f, 0.0f, 1.0f);
    }();
    auto pmatBillboard = [&](const glm::mat4& m, int rtck) -> const glm::mat4* {
        glm::mat4 mb;
        if (!FBillboardMatrix(m, rtck, cam.pos, vecUpCam, mb)) return &m;
        amatBillboard.push_back(mb);
        return &amatBillboard.back();
    };
    static const char* s_szVolDbg = std::getenv("SLY2_VOLDBG");   // debugging: the volumes (shader kind 3) within this distance
    if (s_szVolDbg && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        if (++s_cframe == 1)
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const GLOB& gg = !g.pgl && g.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(g.iglobTemplate)] : g;
                    int shdk = -1;
                    for (const SUBGLOB& sub : gg.asubglob) if (sub.pshd) shdk = std::max(shdk, int(sub.pshd->shdk));
                    static const bool s_fFx = std::getenv("SLY2_VOLDBGFX") != nullptr;   // (the type-3 effect meshes instead)
                    if (s_fFx)
                    {
                        if (gg.meshk != MESHK_Indexed || gg.asubfx.empty() || !gg.asubfx[0].pshd) continue;
                        const SHD* ps = gg.asubfx[0].pshd;
                        const glm::vec3 c = i < sgs.amatGlob.size() ? glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f)) : glm::vec3(0);
                        std::printf("fx3: oid %d #%zu rp %d shd %d k%d grfshd 0x%X rgba %08X %08X r %.0f at %.0f %.0f %.0f vis %d\n", sgs.oid, i, g.rp, ps->id, ps->shdk, ps->grfshd,
                                    ps->argba[0], ps->argba[1], g.sRadius, c.x, c.y, c.z, i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1);
                        continue;
                    }
                    if (shdk != 3 && (g.rp & 0x1F) != 11 && (g.rp & 0x1F) != 13) continue;
                    const glm::vec3 c = i < sgs.amatGlob.size() ? glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f)) : glm::vec3(0);
                    const float d = glm::distance(c, cam.pos);
                    if (d > std::atof(s_szVolDbg)) continue;
                    std::printf("vol: oid %d glob@%zu #%zu rp %d shdk %d grfshd 0x%X r %.0f at %.0f %.0f %.0f d %.0f xf %d vis %d hidden %d irtgt %d A %08X\n", sgs.oid, sgs.ib, i,
                                g.rp, shdk, gg.asubglob.empty() || !gg.asubglob[0].pshd ? 0 : gg.asubglob[0].pshd->grfshd, g.sRadius, c.x, c.y, c.z, d,
                                int(sgs.fTransform), i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, int(g.fHidden), sgs.irtgt,
                                gg.avtx.empty() ? 0 : gg.avtx[0].rgbaA);
                    for (const SUBGLOB& sub : gg.asubglob)
                        if (sub.pshd && sub.pshd->shdk == 3)
                            std::printf("    vshd %d grfshd 0x%X argba %08X %08X %08X tex %zu\n", sub.pshd->id, sub.pshd->grfshd, sub.pshd->argba[0], sub.pshd->argba[1], sub.pshd->argba[2], sub.pshd->atex.size());
                }
    }
    if (static const bool s_fRtckDbg = std::getenv("SLY2_RTCKDBG") != nullptr; s_fRtckDbg && pv.irtgt < 0 && !pv.fForeground)   // debugging: the groups with a billboard mode
    {
        static bool s_f = false;
        if (!s_f)
        {
            s_f = true;
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size() && i < sgs.amatGlob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    if (g.rtck == -1) continue;
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f));
                    std::printf("rtck %d: oid %d #%zu rp %d meshk %d r %.0f at %.0f %.0f %.0f vis %d inst %d\n", g.rtck, sgs.oid, i, g.rp, int(g.meshk), g.sRadius, c.x, c.y, c.z,
                                i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, g.iglobTemplate);
                }
        }
    }
    static const char* s_szPtDbg = std::getenv("SLY2_PTDBG");   // debugging: "x y z": the groups whose sphere holds the point
    if (s_szPtDbg && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        glm::vec3 pt(0.0f);
        std::sscanf(s_szPtDbg, "%f %f %f", &pt.x, &pt.y, &pt.z);
        if (++s_cframe == 1)
            for (const SCENEGLOBSET& sgs : scene.aglobset)
                for (size_t i = 0; i < sgs.pglobset->aglob.size() && i < sgs.amatGlob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const GLOB& gg = !g.pgl && g.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(g.iglobTemplate)] : g;
                    const float r = std::max(g.sRadius, gg.pgl ? gg.pgl->sRadius : 0.0f);
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(gg.pgl && !g.fDraw ? gg.pgl->posCenter : g.posCenter, 1.0f));
                    static const float s_rPt = std::getenv("SLY2_PTDBGR") ? float(std::atof(std::getenv("SLY2_PTDBGR"))) : -1.0f;
                    if (glm::distance(c, pt) > (s_rPt > 0.0f ? s_rPt : r)) continue;
                    std::printf("pt: oid %d #%zu n0 %d rp %d meshk %d r %.0f at %.0f %.0f %.0f vis %d xf %d:", sgs.oid, i, g.n0, g.rp, int(gg.meshk), r, c.x, c.y, c.z,
                                i < sgs.afVisible.size() ? int(sgs.afVisible[i]) : -1, int(sgs.fTransform));
                    for (const SUBGLOB& sub : gg.asubglob) if (sub.pshd) std::printf(" <shd %d k%d grf 0x%X %08X>", sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0]);
                    for (const auto& sub : gg.asubfx) if (sub.pshd) std::printf(" <fx shd %d k%d grf 0x%X %08X>", sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0]);
                    std::printf("\n");
                    // (SLY2_PTDBGV=N: also the group's N vertices nearest the point, their colours A / B)
                    if (const char* szV = std::getenv("SLY2_PTDBGV"))
                    {
                        std::vector<std::pair<float, size_t>> ad;
                        for (size_t k = 0; k < gg.avtx.size(); ++k)
                            ad.push_back({glm::distance(glm::vec3(sgs.amatGlob[i] * glm::vec4(gg.avtx[k].pos, 1.0f)), pt), k});
                        std::sort(ad.begin(), ad.end());
                        for (size_t k = 0; k < ad.size() && k < size_t(std::max(1, std::atoi(szV))); ++k)
                        {
                            const VTXGLOB& v = gg.avtx[ad[k].second];
                            const glm::vec3 w = glm::vec3(sgs.amatGlob[i] * glm::vec4(v.pos, 1.0f));
                            std::printf("    v%zu at %.0f %.0f %.0f d %.0f A %08X B %08X unbaked %d\n", ad[k].second, w.x, w.y, w.z, ad[k].first,
                                        v.rgbaA, v.rgbaB, int(v.fUnbaked));
                        }
                    }
                }
    }
    for (const SCENEGLOBSET& sgs : scene.aglobset)
    {
        if (!sgs.fTransform || sgs.irtgt != pv.irtgt) continue;
        static const char* s_range = std::getenv("SLY2_IBRANGE");   // debugging: only globsets in [lo, hi)
        if (s_range)
        {
            size_t lo = 0, hi = 0;
            std::sscanf(s_range, "%zu,%zu", &lo, &hi);
            if (sgs.ib < lo || sgs.ib >= hi) continue;
        }
        const GLOBSET& gs = *sgs.pglobset;
        // orig: 0x1B4478: a glob set with sphere sets (+0x48) is drawn through its volume tree (0x1D5808), not
        // RenderGlob: from set 0, each record (kind -1 skipped) whose channel mask meets CM +0x14 is tested by VU0
        // micro 0x1C0 (the four side planes by its radius, the MRD fade by its centre, x CM +0x1C); 0 culls it (a
        // kind-1 record: its whole set), a group's record submits the fade as its alpha (qw8 = gVis, 1, scale).
        // Neither the whole-object sphere nor the object's fade apply.
        const bool fTree = !s_fNoTree && pv.irtgt < 0 && !gs.asphereset.empty();
        static std::vector<float> s_agVisTree;
        if (fTree)
        {
            s_agVisTree.assign(gs.aglob.size(), 0.0f);
            int aiset[256];
            int ciset = 0;
            aiset[ciset++] = 0;
            while (ciset > 0)
            {
                const int iset = aiset[--ciset];
                if (iset < 0 || size_t(iset) >= gs.asphereset.size()) continue;
                for (const GLOBSPHERE& sphere : gs.asphereset[size_t(iset)])
                {
                    if (sphere.kind == -1) continue;
                    if (!(sphere.mask & pv.grfChannels)) continue;
                    if (!s_fNoFrustum && outside(sphere.pos, sphere.sRadius)) continue;
                    const bool fCel = sphere.kind == 0 && sphere.iglob >= 0 && size_t(sphere.iglob) < gs.aglob.size() &&
                                      gs.aglob[size_t(sphere.iglob)].meshk == MESHK_CelBorder;
                    const float gVis = UCmSphereDistanceFade(fCel ? &cmFadeCel : &cmFade, sphere.pos - cam.pos, sphere.sMRD);
                    if (gVis == 0.0f) { ++g_renderstats.cmrd; continue; }
                    if (sphere.kind == 1) { if (ciset < 256) aiset[ciset++] = sphere.iset; }
                    else if (sphere.kind == 0 && sphere.iglob >= 0 && size_t(sphere.iglob) < gs.aglob.size())
                        s_agVisTree[size_t(sphere.iglob)] = gVis;
                }
            }
        }
        // orig: 0x1686AC..0x1687A8: the whole sphere precedes every group and supplies f20.
        float wholeFade = 1.0f;
        if (pv.irtgt < 0 && sgs.fBounds && !fTree)
        {
            wholeFade = UCmSphereDistanceFade(sgs.fMrdOriginal ? &cmFadeCel : &cmFade, sgs.posBounds - cam.pos, sgs.sMRD);
            if (sgs.oid == s_oidFade)
                std::printf("fadeobj oid %d center %.9g %.9g %.9g radius %.9g mrd %.9g adjust %.9g fade %.9g\n",
                            sgs.oid, sgs.posBounds.x, sgs.posBounds.y, sgs.posBounds.z, sgs.sBoundsRadius, sgs.sMRD, cmFade.rMRDAdjust, wholeFade);
            if (wholeFade == 0.0f) { ++g_renderstats.cmrd; continue; }
            if (!s_fNoFrustum && outside(sgs.posBounds, sgs.sBoundsRadius)) { ++g_renderstats.cculled; continue; }
        }
        for (size_t i = 0; i < gs.aglob.size(); ++i)
        {
            const GLOB& glob = gs.aglob[i];
            // (afVisible holds the loaded flag (0x800) as changed at run time by ShowAloGlobs: a group loaded hidden
            // and shown by a script draws; the AI code's fix for Paris' guards)
            if (i < sgs.afVisible.size() ? !sgs.afVisible[i] : glob.fHidden) continue;
            int kind = glob.rp & 0x1F;
            if (pv.irtgt >= 0)
            {
                kind = kind < 26 ? s_amapkindRtgt[pv.irtgt][kind] : -1;
                if (kind < 0) continue;
            }
            const GLOB* pgeom = &glob;
            if (!glob.pgl && glob.iglobTemplate >= 0) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
            if (!pgeom->pgl && !(pgeom->meshk == MESHK_Effect && opts.fShadows && pv.irtgt < 0)) continue;
            const glm::mat4& mat = sgs.amatGlob[i];
            const glm::vec3 posWorld = glm::vec3(MulMat4Vec4(mat, glm::vec4(glob.posCenter, 1.0f)));
            const float d = glm::distance(posWorld, cam.pos);
            static const int s_oidRec = std::getenv("SLY2_RECOID") ? std::atoi(std::getenv("SLY2_RECOID")) : -2;   // debugging
            static const int s_oidOnly = std::getenv("SLY2_ONLYOID") ? std::atoi(std::getenv("SLY2_ONLYOID")) : -2;   // debugging: only this object
            if (s_oidOnly != -2 && sgs.oid != s_oidOnly) continue;
            static const int s_oidSkip = std::getenv("SLY2_SKIPOID") ? std::atoi(std::getenv("SLY2_SKIPOID")) : -2;   // debugging: all but this object
            static const char* s_szGlobSkip = std::getenv("SLY2_SKIPGLOB");   // (with SLY2_SKIPOID: only these groups, "a,b,c")
            if (sgs.oid == s_oidSkip &&
                (!s_szGlobSkip || std::strstr((std::string(",") + s_szGlobSkip + ",").c_str(), ("," + std::to_string(i) + ",").c_str())))
                continue;
            static const int s_iglobOnly = std::getenv("SLY2_ONLYGLOB") ? std::atoi(std::getenv("SLY2_ONLYGLOB")) : -1;   // debugging: only this group (with SLY2_ONLYOID)
            if (s_iglobOnly >= 0 && s_oidOnly != -2 && int(i) != s_iglobOnly) continue;
            if (sgs.oid == s_oidRec)
                std::printf("rec oid %d #%zu rp %d d %.0f r %.1f mrd %.0f fDraw %d alpha %.2f at %.0f %.0f %.0f\n", sgs.oid, i, glob.rp, d, glob.sRadius, glob.sMRD,
                            int(glob.fDraw), sgs.gAlpha, posWorld.x, posWorld.y, posWorld.z);
            if (sgs.oid == s_oidRec && std::getenv("SLY2_RECVTX"))
            {
                std::printf("   vtx A alpha:");
                for (const VTXGLOB& v : pgeom->avtx) std::printf(" %u", v.rgbaA >> 24);
                std::printf("\n   vtx src alpha:");
                for (const VTXGLOB& v : pgeom->avtx) std::printf(" %u", v.rgbaSrc >> 24);
                std::printf("\n");
                if (glob.rp == 10)
                    for (const VTXGLOB& v : pgeom->avtx)
                    {
                        const glm::vec3 pw = glm::vec3(mat * glm::vec4(v.pos, 1.0f));
                        const glm::vec3 nw = glm::normalize(glm::mat3(mat) * v.normal);
                        const glm::vec4 c = pv.matWorldToClip * glm::vec4(pw, 1.0f);
                        const glm::vec3 tc = glm::normalize(cam.pos - pw);
                        float aSpec = float(v.rgbaA >> 24);
                        if (pgeom->iSpec == 1)
                        {
                            const glm::mat3 mInv = glm::inverse(glm::mat3(mat));
                            const glm::vec3 vv = glm::normalize(mInv * (cam.pos - glm::vec3(mat[3])) - pgeom->posSpec);
                            const float x = glm::dot(vv, glm::trunc(v.normal * 127.0f));
                            const float g = std::clamp(pgeom->agSpec[0] + pgeom->agSpec[1] * x + pgeom->agSpec[2] * x * x, pgeom->agClamp[0], pgeom->agClamp[1]);
                            aSpec = std::floor(aSpec * g + 0.5f);
                        }
                        std::printf("   gv %.1f %.1f a %.0f ndv %.4f d %.0f w %.1f %.1f %.1f n %.3f %.3f %.3f nlen %.4f\n", (c.x / c.w * 0.5f + 0.5f) * 512.0f,
                                    (0.5f - c.y / c.w * 0.5f) * 448.0f, aSpec, glm::dot(nw, tc), glm::distance(pw, cam.pos), pw.x, pw.y, pw.z, nw.x, nw.y, nw.z, glm::length(v.normal));
                    }
            }
            if (sgs.oid == s_oidRec)
                for (const SUBGLOB& sub : pgeom->asubglob)
                {
                    if (!sub.pshd) continue;
                    std::printf("   sub grf 0x%X idx %u shd %d k%d grfshd 0x%X rgba %08X saa %d:", sub.grf, sub.indexCount, sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd,
                                sub.pshd->argba[0], sub.psaa ? sub.psaa->cid : -1);
                    for (const SHDPASS& ps : sub.pshd->apass)
                        std::printf(" [tex %d itex %d col %d test 0x%llX alpha 0x%llX abe %d]", int(ps.fTextured), ps.itexPass, ps.icolor,
                                    (unsigned long long)ps.test, (unsigned long long)ps.alpha, int(ps.fBlend));
                    const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, 0);
                    if (sel.pbmp)
                    {
                        std::vector<uint8_t> rgba; std::string err;
                        if (FDecodeTexture(sel.pbmp, sel.pclut, rgba, err))
                        {
                            int ca[4] = {}; uint64_t sr = 0, sg = 0, sb = 0;
                            for (size_t k = 0; k + 3 < rgba.size(); k += 4) { ++ca[std::min(3, rgba[k + 3] / 0x40)]; sr += rgba[k]; sg += rgba[k + 1]; sb += rgba[k + 2]; }
                            const size_t n = rgba.size() / 4;
                            std::printf(" bmp %dx%d alpha q %d %d %d %d mean rgb %d %d %d", sel.pbmp->dx, sel.pbmp->dy, ca[0], ca[1], ca[2], ca[3], int(sr / n), int(sg / n), int(sb / n));
                        }
                    }
                    std::printf("\n");
                }
            if (sgs.oid == s_oidRec && !pgeom->avtx.empty())
            {
                const VTXGLOB& v = pgeom->avtx[0];
                std::printf("   vtx0 A %08X B %08X pos %.1f %.1f %.1f n %zu mat %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f bones %zu\n", v.rgbaA, v.rgbaB, v.pos.x, v.pos.y, v.pos.z, pgeom->avtx.size(), mat[0][0], mat[0][1], mat[0][2], mat[1][0], mat[1][1], mat[1][2], mat[2][0], mat[2][1], mat[2][2], i < sgs.aamatBone.size() ? sgs.aamatBone[i].size() : 0);
            }
            // orig: 0x1689CC..0x168AC4: scale the group radius; unskinned groups use the VU sphere
            // test, skinned groups use 0x14F898 without a group plane test. Neither subtracts the radius.
            const bool fSkinned = !glob.abone.empty() || !pgeom->abone.empty();
            const float radius = glob.sRadius * maxScale(mat);
            float groupFade = 1.0f;
            bool fDistance = true;
            if (fTree)
            {
                groupFade = s_agVisTree[i];
                fDistance = groupFade != 0.0f;
            }
            else if (pv.irtgt < 0 && glob.fDraw)
            {
                const CM* pcmG = pgeom->meshk == MESHK_CelBorder ? &cmFadeCel : &cmFade;
                if (fSkinned) fDistance = FInsideCmMrd(pcmG, posWorld - cam.pos, glob.sMRD, &groupFade);
                else { groupFade = UCmSphereDistanceFade(pcmG, posWorld - cam.pos, glob.sMRD); fDistance = groupFade != 0.0f; }
            }
            const float fade = s_fNoFade ? 1.0f : std::min(wholeFade, groupFade);
            if (sgs.oid == s_oidFade)
                std::printf("fadegroup oid %d group %zu center %.9g %.9g %.9g radius %.9g mrd %.9g skinned %d visible %d whole %.9g groupfade %.9g alpha %.9g alpha2 %.9g\n",
                            sgs.oid, i, posWorld.x, posWorld.y, posWorld.z, radius, glob.sMRD, int(fSkinned), int(fDistance), wholeFade,
                            groupFade, sgs.gAlpha * fade, sgs.gAlpha2 * fade);
            if (!fDistance) { ++g_renderstats.cmrd; continue; }
            const bool fGeomBounds = !fTree && !glob.fDraw && pgeom->pgl && pgeom->pgl->sRadius >= 0 && !fSkinned;
            if (pv.irtgt < 0 && !fTree && !s_fNoFrustum && pgeom->meshk != MESHK_Effect && ((glob.fDraw && !fSkinned) || fGeomBounds))
            {
                const glm::vec3 posC = fGeomBounds ? glm::vec3(MulMat4Vec4(mat, glm::vec4(pgeom->pgl->posCenter, 1.0f))) : posWorld;
                const float r = fGeomBounds ? pgeom->pgl->sRadius * maxScale(mat) : radius;
                if (outside(posC, r)) { ++g_renderstats.cculled; continue; }
            }
            DRAWREC rec;
            rec.kind = kind;
            // orig: 0x168D64..0x168D94: both input alphas receive min(wholeFade, groupFade).
            // orig: 0x169198 RenderGlobCtx: render-target draws use unit alphas without the main-view tests.
            rec.alpha = pv.irtgt >= 0 ? 1.0f : std::clamp((fTree ? 1.0f : sgs.gAlpha) * fade, 0.0f, 1.0f);
            rec.alpha2 = pv.irtgt >= 0 || fTree ? 1.0f : std::clamp(sgs.gAlpha2 * fade, 0.0f, 1.0f);
            // the foreground kinds (0x169054..0x1690E8's table by rp - 3)
            if (sgs.fForeground && pv.irtgt < 0)
                switch (kind)
                {
                case 3: case 4: kind = rec.alpha == 1.0f ? 22 : 23; break;
                case 5: case 6: kind = 23; break;
                case 9: kind = 24; break;
                case 10: kind = 25; break;
                default: break;
                }
            if (pv.irtgt < 0 && (kind >= 22 && kind <= 25) != pv.fForeground) continue;
            rec.kind = kind;
            // RenderGlob: a faded group of the opaque kinds 3 / 4 is recorded as the sorted (blended) 5 / 6
            if (rec.alpha != 1.0f && pv.irtgt < 0 && (kind == 3 || kind == 4)) rec.kind = kind + 2;
            static const char* s_kinds = std::getenv("SLY2_KINDS");   // debugging: only these kinds (hex digits list)
            if (s_kinds)
            {
                char sz[8];
                std::snprintf(sz, sizeof(sz), ",%d,", rec.kind);
                if (!std::strstr((std::string(",") + s_kinds + ",").c_str(), sz)) continue;
            }
            // orig: 0x00169100 (RenderGlob): the record's sort key is the group's z order (its header +4) when its
            // flags have 0x400 (e.g. the sky's layers, world 2 / 8), else the squared distance
            // (a group without a draw record: the volume tree's record, keyed by its sphere in the sphere sets, 0x1D5808)
            float dSort = d;
            if (!glob.fDraw && glob.fSphere) dSort = glm::distance(glm::vec3(MulMat4Vec4(mat, glm::vec4(glob.posSphere, 1.0f))), cam.pos);
            rec.z = (glob.grfGroup & 0x400) ? glob.gZOrder : dSort * dSort;
            rec.pglob = &glob;
            rec.pglobGeom = pgeom;
            rec.pmat = pmatBillboard(sgs.amatGlob[i], glob.rtck);
            rec.psgs = &sgs;
            rec.iglob = i;
            if (i < sgs.aamatBone.size() && !sgs.aamatBone[i].empty()) rec.pamatBone = &sgs.aamatBone[i];
            arec.push_back(rec);
            if (sgs.oid == s_oidFade)
                std::printf("fadesubmit oid %d group %zu kind %d alpha %.9g alpha2 %.9g\n", sgs.oid, i, rec.kind, rec.alpha, rec.alpha2);
        }
    }
    if (static const bool s_fRecList = std::getenv("SLY2_RECLIST") != nullptr; s_fRecList && pv.irtgt < 0 && !pv.fForeground)   // debugging: the objects with records this frame
    {
        std::map<int, int> mp;
        const SCENEGLOBSET* psgsPrev = nullptr;
        for (const DRAWREC& r : arec) if (r.psgs != psgsPrev) { psgsPrev = r.psgs; ++mp[r.psgs->oid]; }
        for (const auto& [o, n] : mp) std::printf("reclist %d %d\n", o, n);
    }
    // extra draws (the frame only): RIPs' models at their own matrices (emitter.cpp)
    static std::vector<EXTRADRAW> s_aextra;
    s_aextra.clear();
    if (pv.irtgt < 0 && !pv.fForeground)
    {
        if (g_pfnExtraDraws) g_pfnExtraDraws(scene, s_aextra);
        CollectDrawDistanceGhosts(scene, s_aextra);   // (QoL draw distance above 1x: drawdist.h)
        CollectLaserDraws(scene, cam.pos, s_aextra);
        CollectPocketDraws(scene, s_aextra);
        CollectHackSwarmDraws(scene, s_aextra);
        if (static const bool s_fList = std::getenv("SLY2N_EXTRALIST") != nullptr; s_fList)
            for (const EXTRADRAW& x : s_aextra)
                std::fprintf(stderr, "extradraw sgs %d glob %d at %.0f %.0f %.0f\n", x.psgs ? x.psgs->oid : -1, x.iglob, x.mat[3].x, x.mat[3].y, x.mat[3].z);
    }
    for (const EXTRADRAW& x : s_aextra)
    {
        const GLOBSET& gs = *x.psgs->pglobset;
        if (x.iglob >= gs.aglob.size()) continue;
        const GLOB& glob = gs.aglob[x.iglob];
        const GLOB* pgeom = &glob;
        if (!glob.pgl && glob.iglobTemplate >= 0) pgeom = &gs.aglob[size_t(glob.iglobTemplate)];
        if (!pgeom->pgl) continue;
        const glm::vec3 posWorld = glm::vec3(x.mat * glm::vec4(glob.posCenter, 1.0f));
        float wholeFade = 1.0f, groupFade = 1.0f;
        if (x.fBounds)
        {
            wholeFade = UCmSphereDistanceFade(&cmFade, x.posBounds - cam.pos, x.sMRD);
            if (wholeFade == 0.0f) { ++g_renderstats.cmrd; continue; }
            if (!s_fNoFrustum && outside(x.posBounds, x.sBoundsRadius)) { ++g_renderstats.cculled; continue; }
        }
        if (glob.fDraw)
        {
            const bool fSkinned = !glob.abone.empty() || !pgeom->abone.empty();
            const CM* pcmG = pgeom->meshk == MESHK_CelBorder ? &cmFadeCel : &cmFade;
            if (fSkinned)
            {
                if (!FInsideCmMrd(pcmG, posWorld - cam.pos, glob.sMRD, &groupFade)) { ++g_renderstats.cmrd; continue; }
            }
            else
            {
                groupFade = UCmSphereDistanceFade(pcmG, posWorld - cam.pos, glob.sMRD);
                if (groupFade == 0.0f) { ++g_renderstats.cmrd; continue; }
                if (!s_fNoFrustum && outside(posWorld, glob.sRadius * maxScale(x.mat))) { ++g_renderstats.cculled; continue; }
            }
        }
        const float alpha = x.alpha * (s_fNoFade ? 1.0f : std::min(wholeFade, groupFade));
        DRAWREC rec;
        rec.kind = glob.rp & 0x1F;
        const float d = glm::distance(posWorld, cam.pos);
        rec.z = (glob.grfGroup & 0x400) ? glob.gZOrder : d * d;  // 0x169100 (as above)
        rec.pglob = &glob;
        rec.pglobGeom = pgeom;
        rec.pmat = pmatBillboard(x.mat, glob.rtck);
        rec.alpha = rec.alpha2 = std::clamp(alpha, 0.0f, 1.0f);
        if (rec.alpha != 1.0f && (rec.kind == 3 || rec.kind == 4)) rec.kind += 2;
        arec.push_back(rec);
        if (x.psgs->oid == s_oidFade)
            std::printf("fadesubmitextra oid %d group %zu kind %d alpha %.9g alpha2 %.9g\n", x.psgs->oid, x.iglob, rec.kind, rec.alpha, rec.alpha2);
    }
    if (pv.irtgt < 0 && [] { static const bool f = std::getenv("SLY2_PICK") != nullptr; return f; }())
    {
        glm::vec3 dir = cam.Dir();
        // SLY2_PICK="sx sy" (-1..1, y up): the ray through that screen point instead of the centre
        float sx = 0.0f, sy = 0.0f;
        if (std::sscanf(std::getenv("SLY2_PICK"), "%f %f", &sx, &sy) == 2)
        {
            const glm::mat4 inv = glm::inverse(matWorldToClip);
            const glm::vec4 a = inv * glm::vec4(sx, sy, 0.5f, 1.0f);
            dir = glm::normalize(glm::vec3(a) / a.w - cam.pos);
            std::printf("pick ray %.3f %.3f %.3f from %.0f %.0f %.0f\n", dir.x, dir.y, dir.z, cam.pos.x, cam.pos.y, cam.pos.z);
        }
        for (const SCENEGLOBSET& sgs : scene.aglobset)
        {
            if (!sgs.fTransform) continue;
            if (const char* r = std::getenv("SLY2_IBRANGE"))
            {
                size_t lo = 0, hi = 0;
                std::sscanf(r, "%zu,%zu", &lo, &hi);
                if (sgs.ib < lo || sgs.ib >= hi) continue;
                std::printf("in range: glob@%zu va 0x%X groups %zu\n", sgs.ib, sgs.vaGlob, sgs.pglobset->aglob.size());
                for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
                {
                    const GLOB& g = sgs.pglobset->aglob[i];
                    const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(g.posCenter, 1.0f));
                    std::printf("  grfMesh 0x%X cbone %d gZ %g\n", g.grfMesh, g.cbone, g.gZOrder);
                    for (const SUBGLOB& sub : g.asubglob)
                        if (sub.pshd)
                            for (int tp = 0; tp < 2; ++tp)
                            {
                                const TEXSEL sel = TexselFromShd(sub.pshd, 0, tp);
                                if (!sel.pbmp) continue;
                                std::vector<uint8_t> rgba; std::string err;
                                if (!FDecodeTexture(sel.pbmp, sel.pclut, rgba, err)) continue;
                                int ca[4] = {};
                                for (size_t k = 3; k < rgba.size(); k += 4) ++ca[std::min(3, rgba[k] / 0x40)];
                                std::printf("    shd 0x%X pass %d bmp 0x%X %dx%d alpha quartiles %d %d %d %d\n", sub.pshd->id, tp, sel.pbmp->id, sel.pbmp->dx, sel.pbmp->dy, ca[0], ca[1], ca[2], ca[3]);
                            }
                    std::printf("  #%zu rp %d meshk %d r %.0f at %.0f %.0f %.0f hidden %d shdk %d grfshd 0x%X A %08X\n", i, g.rp, g.meshk, g.sRadius, c.x, c.y, c.z,
                                int(g.fHidden || !sgs.afVisible[i]), g.asubglob.empty() || !g.asubglob[0].pshd ? -1 : g.asubglob[0].pshd->shdk,
                                g.asubglob.empty() || !g.asubglob[0].pshd ? 0 : g.asubglob[0].pshd->grfshd, g.avtx.empty() ? 0 : g.avtx[0].rgbaA);
                }
            }
            for (size_t i = 0; i < sgs.pglobset->aglob.size(); ++i)
            {
                const GLOB& glob = sgs.pglobset->aglob[i];
                const glm::vec3 c = glm::vec3(sgs.amatGlob[i] * glm::vec4(glob.posCenter, 1.0f));
                const float t = glm::dot(c - cam.pos, dir);
                const float dperp = glm::length(c - cam.pos - dir * t);
                const GLOB& gr = !glob.pgl && glob.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(glob.iglobTemplate)] : glob;
                const float sRPick = std::max(glob.sRadius, gr.pgl ? gr.pgl->sRadius : 0.0f);
                if (t > 0 && dperp < sRPick)
                    std::printf("pick: oid %d #%zu rp %d meshk %d r %.0f t %.0f hidden %d shd0 0x%X c %.0f %.0f %.0f fog %g z %g grf 0x%X A %08X\n", sgs.oid, i,
                                glob.rp, glob.meshk, sRPick, t, int(glob.fHidden || !sgs.afVisible[i]),
                                glob.asubglob.empty() || !glob.asubglob[0].pshd ? 0 : glob.asubglob[0].pshd->id, c.x, c.y, c.z, glob.uFog, glob.gZOrder,
                                glob.grfGroup, gr.avtx.empty() ? 0 : gr.avtx[0].rgbaA);
                if (glob.asubglob.size() && glob.asubglob[0].pshd && t > 0 && dperp < sRPick)
                {
                    const GLOB& g = glob.avtx.empty() && glob.iglobTemplate >= 0 ? sgs.pglobset->aglob[size_t(glob.iglobTemplate)] : glob;
                    if (!g.avtx.empty())
                        std::printf("      shdk %d grfshd 0x%X grf 0x%X si %d A %08X B %08X unbaked %d inst %d\n", glob.asubglob[0].pshd->shdk,
                                    glob.asubglob[0].pshd->grfshd, glob.asubglob[0].grf, glob.asubglob[0].pshd->unSelfIllum,
                                    g.avtx[0].rgbaA, g.avtx[0].rgbaB, g.avtx[0].fUnbaked, glob.iglobTemplate);
                }
            }
        }
    }
    // DrawRpl 0x157FA0 / SortRpl 0x157AE0: by kind; kinds 1, 2, 5, 6, 10, 25 far to near, 21 near to far.
    std::stable_sort(arec.begin(), arec.end(), [](const DRAWREC& a, const DRAWREC& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        switch (a.kind)
        {
        case 1: case 2: case 5: case 6: case 10: case 25: return a.z > b.z;
        case 21: return a.z < b.z;
        default: return false;
        }
    });

    // The frame's draw stream: per record its matrix / fog (RECGPU), per entry its draw data (DRAWGPU), per pass
    // one or two commands with their GL state; volumes, shadows and cel borders are special commands.
    static std::vector<RECGPU> s_arecgpu;
    static std::vector<DRAWGPU> s_adrawgpu;
    static std::vector<CMD> s_acmd;
    static std::vector<glm::mat4> s_amatBone;
    s_amatBone.clear();
    for (DRAWREC& rec : arec)
        if (rec.pamatBone)
        {
            rec.iboneFirst = uint32_t(s_amatBone.size());
            s_amatBone.insert(s_amatBone.end(), rec.pamatBone->begin(), rec.pamatBone->end());
        }
    s_arecgpu.clear();
    s_adrawgpu.clear();
    s_acmd.clear();
    // the warped globs' state vectors and their WRs' matrices (wr.h; glob.vert adds sum(matrix x state))
    static std::vector<glm::vec4> s_awarpst;
    static std::vector<glm::mat4> s_amatWarp;
    static std::unordered_map<const WRBG*, uint32_t> s_mpwrbgist;
    static std::unordered_map<const WR*, uint32_t> s_mpwrimat;
    static const bool s_fNoWarp = std::getenv("SLY2_NOWARP") != nullptr;   // debugging: no space warps
    s_awarpst.clear();
    s_amatWarp.clear();
    s_mpwrbgist.clear();
    s_mpwrimat.clear();
    auto warpOf = [&](const GLOB& geom) {
        const WRBG* pw = geom.pwrbg.get();
        const WR* pwr = PwrOfWrbg(pw);
        if (!pwr || s_fNoWarp || !(pw->fDpos || pw->fDuv)) return glm::uvec4(0u);
        const size_t cvtx = geom.avtx.size();
        const uint32_t cmat = uint32_t(std::clamp(pw->cmat, 0, 4));
        if (cmat == 0 || pw->astate.size() < cmat * cvtx) return glm::uvec4(0u);
        auto [itst, fNewSt] = s_mpwrbgist.try_emplace(pw, uint32_t(s_awarpst.size()) + 1u);
        if (fNewSt) s_awarpst.insert(s_awarpst.end(), pw->astate.begin(), pw->astate.begin() + std::ptrdiff_t(cmat * cvtx));
        auto [itmat, fNewMat] = s_mpwrimat.try_emplace(pwr, uint32_t(s_amatWarp.size()));
        if (fNewMat)
        {
            s_amatWarp.insert(s_amatWarp.end(), std::begin(pwr->amatDpos), std::end(pwr->amatDpos));
            s_amatWarp.insert(s_amatWarp.end(), std::begin(pwr->amatDuv), std::end(pwr->amatDuv));
        }
        return glm::uvec4(itst->second, uint32_t(cvtx), itmat->second, cmat | (pw->fDpos ? 0x100u : 0u) | (pw->fDuv ? 0x200u : 0u));
    };
    s_avtxShadow2.clear();
    // Kinds drawn in record order with no sort (DrawRpl): their commands are regrouped by pass and state, so
    // the runs are long; the sorted (blended) kinds keep their order, merging only neighbours.
    static const bool s_fNoRegroup = std::getenv("SLY2_NOREGROUP") != nullptr;
    auto fSortedKind = [](int kind) {
        switch (kind)
        {
        case 1: case 2: case 5: case 6: case 10: case 25: case 21: return true;
        default: return false;
        }
    };
    struct SEGMENT { size_t icmdFirst, icmdLim; bool fRegroup; };
    std::vector<SEGMENT> aseg;
    size_t icmdSeg = 0;
    int kindSeg = -1;
    auto endSegment = [&](bool fRegroup) {
        if (s_acmd.size() > icmdSeg) aseg.push_back({icmdSeg, s_acmd.size(), fRegroup});
        icmdSeg = s_acmd.size();
    };
    static const char* s_cull = std::getenv("SLY2_CULL");   // "none": no back-face culling
    const bool fCullNone = s_cull && !std::strcmp(s_cull, "none");
    // the particles (render kind 12, emitter.cpp) where the frame draws that kind
    bool fParticles = pv.irtgt >= 0 || pv.fForeground;
    auto addParticles = [&]() {
        if (fParticles) return;
        fParticles = true;
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        CMD cmd;
        cmd.cmdk = CMDK_Particles;
        s_acmd.push_back(cmd);
        endSegment(false);
    };
    // The type-2 shadows (render kind 7: the characters' cast shadows) mark with the kind-7 hulls; the frame's alpha
    // keeps the characters' own pixels out (DATE), as on the PS2.
    bool fShadow2 = false;
    auto addShadow2 = [&]() {
        if (fShadow2 || !opts.fShadows || pv.fForeground) return;
        fShadow2 = true;
        // all the volumes in one count, marked once (their union: a character's overlapping pieces, as the PS2's
        // green count, darken the floor once)
        CMD cmdAll;
        cmdAll.firstIndex = uint32_t(s_avtxShadow2.size());
        cmdAll.count = 0;
        cmdAll.idraw = 0;
        cmdAll.order = 8;
        for (const DRAWREC& rec : arec)
        {
            const GLOB& geom = *rec.pglobGeom;
            if (geom.meshk != MESHK_Effect || !rec.psgs || geom.asubfx.empty() || !geom.asubfx[0].pshd) continue;
            CMD cmd;
            if (!FBuildShadow2(*rec.psgs, rec.iglob, geom, cam.pos, 52.0f, cmd)) continue;
            cmdAll.count += cmd.count;
            cmdAll.idraw = std::max(cmdAll.idraw, cmd.idraw);
            if (!(geom.asubfx[0].pshd->grfshd & 8)) cmdAll.order = 0;   // (DATE unless every shader has flag 8)
            cmdAll.prec = &rec;
        }
        if (cmdAll.count == 0) return;
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        cmdAll.cmdk = CMDK_Shadow2;
        s_acmd.push_back(cmdAll);
        endSegment(false);
    };
    // A shadow phase: kinds 7 / 8 between 0x16DA48 (begin: the frame's alpha kept) and 0x16DD58 (combine: the marked
    // pixels darkened). count: bit 0 kind 7 drawn, bit 1 kind 8.
    const bool fFbaPass = pv.irtgt < 0 && !pv.fForeground;
    int icmdShPhase = -1;
    bool fFbaOpen = true;   // (the FBA kinds' stencil bits not cleared yet)
    auto shadowCmd = [&](CMDK cmdk) {
        endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
        CMD cmd;
        cmd.cmdk = cmdk;
        s_acmd.push_back(cmd);
        endSegment(false);
    };
    auto shadowPhase = [&](int kind) {
        const bool fShKind = opts.fShadows && !pv.fForeground && (kind == 7 || kind == 8);
        if (fFbaOpen && (kind > 6 || kind < 0) && !fShKind)
        {
            shadowCmd(CMDK_FbaClear);
            fFbaOpen = false;
        }
        if (fShKind) fFbaOpen = false;   // (the combine clears them)
        if (icmdShPhase >= 0 && !fShKind)
        {
            shadowCmd(CMDK_ShadowCombine);
            s_acmd.back().count = s_acmd[size_t(icmdShPhase)].count;
            icmdShPhase = -1;
        }
        else if (fShKind && icmdShPhase < 0)
        {
            shadowCmd(CMDK_ShadowBegin);
            icmdShPhase = int(s_acmd.size()) - 1;
            s_acmd.back().order = uint32_t(kind);
        }
        else if (fShKind && kind == 8) shadowCmd(CMDK_ShadowKind8);
    };
    for (const DRAWREC& rec : arec)
    {
        const GLOB& glob = *rec.pglob;
        const GLOB& geom = *rec.pglobGeom;
        if (rec.kind != kindSeg) shadowPhase(rec.kind);
        if (rec.kind == 7 && icmdShPhase >= 0)
        {
            const size_t ccmd = s_acmd.size();
            addShadow2();
            if (s_acmd.size() != ccmd) s_acmd[size_t(icmdShPhase)].count |= 1;
        }
        if (rec.kind > 12) addParticles();
        if (rec.kind != kindSeg) { endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg)); kindSeg = rec.kind; }
        if (s_adrawgpu.size() + geom.asubglob.size() * (g_fGlobBindless ? 16 : 1) + 1 > CDRAW_MAX) break;
        const uint32_t irec = uint32_t(s_arecgpu.size());
        if (geom.meshk == MESHK_Strips && opts.fMeshes)
        {
            // an instance lit at its own place draws its own copy of the vertices (SCENEGLOBSET::apglInst)
            const GLGLOB* pglDraw = rec.psgs && rec.iglob < rec.psgs->apglInst.size() && rec.psgs->apglInst[rec.iglob] &&
                                            rec.psgs->apglInst[rec.iglob]->vao
                                        ? rec.psgs->apglInst[rec.iglob]
                                        : geom.pgl;
            // CM_BuildFogRecip(g_cm, qw 9, uFog): uFog unset (FLT_MAX) counts as 1.
            const float uFog = glob.uFog == std::numeric_limits<float>::max() ? 1.0f : glob.uFog;
            s_arecgpu.push_back({*rec.pmat, glm::vec4(opts.sFogNear, opts.sFogFar / std::max(opts.sFogNear, 1.0f),
                                                      opts.uFogMax * uFog, opts.fFog ? 1.0f : 0.0f),
                                 glm::uvec4(rec.iboneFirst, rec.pamatBone ? uint32_t(rec.pamatBone->size()) : 0u, uint32_t(pglDraw->baseVertex),
                                            rec.psgs ? uint32_t(rec.psgs->ipaint) : 0u)});
            s_arecgpu.back().warp = warpOf(geom);
            // orig: P_G 638 (0x16A000 SPECPARAMS): the view-angle alpha. Unlit groups only: a lit group's gain scales its
            // normals' brightness (w) for the lighting (522) instead (TODO(render): not modelled)
            if (geom.iSpec != -1 && !(geom.grfMesh & 1))
            {
                RECGPU& r = s_arecgpu.back();
                r.spec0 = glm::vec4(geom.agSpec[0], geom.agSpec[1], geom.agSpec[2], float(geom.iSpec));
                r.spec1 = glm::vec4(geom.agClamp[0], geom.agClamp[1], 1.0f, 0.0f);
                r.specPos = glm::vec4(geom.posSpec, 0.0f);
            }
            CMD base;
            base.vao = pglDraw->vao;
            // P_G culls back faces unless the mesh is double-sided (flag 0x10 -> param 0x80).
            const uint8_t fCull = !(geom.grfMesh & 0x10) && !fCullNone;
            base.baseVertex = pglDraw->baseVertex;
            base.prec = &rec;
            for (const SUBGLOB& sub : geom.asubglob)
            {
                if (!sub.pshd || sub.indexCount == 0) continue;
                // (debug: SLY2N_SKIPSHD=id: that shader's entries not drawn)
                if (static const int s_shdSkip = std::getenv("SLY2N_SKIPSHD") ? std::atoi(std::getenv("SLY2N_SKIPSHD")) : -2; sub.pshd->id == s_shdSkip) continue;
                ++g_renderstats.cdraw;
                base.idraw = uint32_t(s_adrawgpu.size());
                base.count = sub.indexCount;
                base.firstIndex = pglDraw->firstIndex + sub.firstIndex;
                base.psub = &sub;
                if (sub.pshd->shdk == 3)
                {
                    // orig: 0x0016A000 (one projected-volume batch can have several entries of the same material).
                    // Keep that hull together through all mask/fill/clear passes. The entries can be open pieces
                    // of one closed hull: clearing the mask after each piece lights surfaces outside the volume.
                    if (!s_acmd.empty())
                    {
                        CMD& prev = s_acmd.back();
                        if (prev.cmdk == CMDK_Volume && prev.prec == &rec && prev.psub->pshd == sub.pshd &&
                            prev.psub->psaa == sub.psaa && prev.psub->ibatch == sub.ibatch &&
                            prev.vao == base.vao && prev.baseVertex == base.baseVertex &&
                            prev.firstIndex + prev.count == base.firstIndex)
                        {
                            prev.count += base.count;
                            continue;
                        }
                    }
                    s_adrawgpu.push_back({0.0f, 0.0f, 1.0f, irec});
                    endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
                    CMD cmd = base;
                    cmd.cmdk = CMDK_Volume;
                    s_acmd.push_back(cmd);
                    endSegment(false);
                    continue;
                }
                const DRAWGPU drawgpu{sub.psaa ? sub.psaa->du / 4096.0f : 0.0f, sub.psaa ? sub.psaa->dv / 4096.0f : 0.0f, rec.alpha, irec};
                if (!g_fGlobBindless) s_adrawgpu.push_back(drawgpu);
                for (const PASSCMD& pc : SubgpuOf(sub, pglDraw->vao, fCull).apasscmd)
                {
                    base.istate = fFbaPass && (rec.kind == 4 || rec.kind == 6) ? IstateFba(pc.istate) : pc.istate;
                    if (static const bool s_f = std::getenv("SLY2N_PAINTTEX") != nullptr; s_f && rec.psgs && rec.psgs->ipaint)
                    {
                        static std::set<std::pair<int, GLuint>> s_seen;
                        const GLuint tex = s_astate[base.istate].tex;
                        if (s_seen.emplace(rec.psgs->ipaint, tex).second)
                            std::printf("painttex profile %d oid %d tex %s\n", rec.psgs->ipaint, rec.psgs->oid,
                                        s_mptexName.count(tex) ? s_mptexName[tex].c_str() : "?");
                    }
                    base.order = pc.order;
                    // (bindless: each pass draw its own draw entry, for its texture handle: s_atexh)
                    if (g_fGlobBindless)
                    {
                        base.idraw = uint32_t(s_adrawgpu.size());
                        s_adrawgpu.push_back(drawgpu);
                    }
                    s_acmd.push_back(base);
                }
            }
        }
        else if (geom.meshk == MESHK_Indexed && opts.fShadows && geom.pgl->cindex > 0 && !geom.asubfx.empty() && geom.asubfx[0].pshd)
        {
            s_arecgpu.push_back({*rec.pmat, glm::vec4(1, 1, 0, 0)});
            endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
            CMD cmd;
            cmd.cmdk = CMDK_Shadow;
            if (icmdShPhase >= 0) s_acmd[size_t(icmdShPhase)].count |= rec.kind == 8 ? 2u : 1u;
            cmd.vao = geom.pgl->vao;
            cmd.count = uint32_t(geom.pgl->cindex);
            cmd.firstIndex = geom.pgl->firstIndex;
            cmd.baseVertex = geom.pgl->baseVertex;
            cmd.idraw = uint32_t(s_adrawgpu.size());
            cmd.prec = &rec;
            s_adrawgpu.push_back({0.0f, 0.0f, 1.0f, irec});
            s_acmd.push_back(cmd);
            endSegment(false);
        }
        else if (geom.meshk == MESHK_CelBorder && opts.fCelBorders && geom.pgl && geom.pgl->cedge > 0)
        {
            endSegment(!s_fNoRegroup && !fSortedKind(rec.kind));
            CMD cmd;
            cmd.cmdk = CMDK_Cel;
            cmd.prec = &rec;
            s_acmd.push_back(cmd);
            endSegment(false);
        }
    }
    shadowPhase(-1);
    addParticles();
    endSegment(!s_fNoRegroup && kindSeg >= 0 && !fSortedKind(kindSeg));
    // Regroup: within a segment of an unsorted kind, by (pass, sub-draw) then state (stable: record order inside).
    static std::vector<uint64_t> s_akey;
    static std::vector<CMD> s_acmdTmp;
    for (const SEGMENT& seg : aseg)
    {
        if (!seg.fRegroup || seg.icmdLim - seg.icmdFirst < 2) continue;
        s_akey.clear();
        for (size_t i = seg.icmdFirst; i < seg.icmdLim; ++i)
        {
            const CMD& c = s_acmd[i];
            s_akey.push_back((uint64_t(std::min(c.order, 0xFFFu)) << 52) | (uint64_t(c.istate & 0xFFFFFFu) << 28) | uint64_t(i - seg.icmdFirst));
        }
        SortKeysUnique(s_akey);
        s_acmdTmp.assign(s_acmd.begin() + std::ptrdiff_t(seg.icmdFirst), s_acmd.begin() + std::ptrdiff_t(seg.icmdLim));
        for (size_t i = 0; i < s_akey.size(); ++i) s_acmd[seg.icmdFirst + i] = s_acmdTmp[size_t(s_akey[i] & 0xFFFFFFFu)];
    }
    // Runs of equal state -> indirect commands.
    struct RUN { size_t icmd; uint32_t ideiFirst, cdei; };
    static std::vector<DEICMD> s_adei;
    static std::vector<RUN> s_arun;
    s_adei.clear();
    s_arun.clear();
    for (size_t i = 0; i < s_acmd.size();)
    {
        const CMD& cmd = s_acmd[i];
        if (cmd.cmdk != CMDK_Pass) { s_arun.push_back({i, 0, 0}); ++i; continue; }
        RUN run{i, uint32_t(s_adei.size()), 0};
        size_t j = i;
        // (bindless: the neighbours that differ only in their textures join the run, in the same order)
        for (; j < s_acmd.size() && s_acmd[j].cmdk == CMDK_Pass && (s_acmd[j].istate == cmd.istate || (g_fGlobBindless && FSameButTexture(cmd.istate, s_acmd[j].istate))); ++j)
        {
            const CMD& c = s_acmd[j];
            s_adei.push_back({c.count, 1, c.firstIndex, c.baseVertex, c.idraw});
            ++run.cdei;
        }
        s_arun.push_back(run);
        i = j;
    }
    static const bool s_fPerfDbg = std::getenv("SLY2_PERFDBG") != nullptr;
    if (s_fPerfDbg)
    {
        // records / commands / runs per kind, and records without a draw record (never culled)
        int acrec[32] = {}, acmd[32] = {}, acrun[32] = {}, acnodraw[32] = {};
        for (const DRAWREC& rec : arec) { ++acrec[rec.kind]; if (!rec.pglob->fDraw) ++acnodraw[rec.kind]; }
        for (const CMD& c : s_acmd) if (c.prec) ++acmd[c.prec->kind];
        for (const RUN& r : s_arun) if (s_acmd[r.icmd].prec) ++acrun[s_acmd[r.icmd].prec->kind];
        for (int k = 0; k < 32; ++k)
            if (acrec[k]) std::printf("kind %2d: %5d records (%d without bounds), %5d commands, %5d runs\n", k, acrec[k], acnodraw[k], acmd[k], acrun[k]);
    }
    // (each array's place: in the frame's ring segment when the pass fits (all or none), else its own orphaned buffer)
    struct BUFRANGE { GLuint buf; size_t off, cb; };
    const size_t cbAll = s_arecgpu.size() * sizeof(RECGPU) + s_amatBone.size() * sizeof(glm::mat4) + s_adrawgpu.size() * sizeof(DRAWGPU) +
                         s_awarpst.size() * sizeof(glm::vec4) + s_amatWarp.size() * sizeof(glm::mat4) + s_adei.size() * sizeof(DEICMD) +
                         (g_fGlobBindless ? s_adrawgpu.size() * sizeof(GLuint64) : 0) + 8 * (s_ring.align + 16);
    size_t offRing = s_ring.Alloc(cbAll);
    auto upload = [&](GLenum target, GLuint buf, const void* pv, size_t cb) -> BUFRANGE {
        const size_t cbBind = std::max<size_t>(cb, 16);
        if (offRing != SIZE_MAX)
        {
            const size_t o = (offRing + s_ring.align - 1) / s_ring.align * s_ring.align;
            if (cb) std::memcpy(s_ring.pb + o, pv, cb);
            offRing = o + cbBind;
            return {s_ring.buf, o, cbBind};
        }
        glBindBuffer(target, buf);
        glBufferData(target, GLsizeiptr(cbBind), nullptr, GL_STREAM_DRAW);   // orphan
        if (cb) glBufferSubData(target, 0, GLsizeiptr(cb), pv);
        return {buf, 0, cbBind};
    };
    const BUFRANGE brRec = upload(GL_SHADER_STORAGE_BUFFER, s_ssboRec, s_arecgpu.data(), s_arecgpu.size() * sizeof(RECGPU));
    const BUFRANGE brBone = upload(GL_SHADER_STORAGE_BUFFER, s_ssboBone, s_amatBone.data(), s_amatBone.size() * sizeof(glm::mat4));
    const BUFRANGE brDraw = upload(GL_SHADER_STORAGE_BUFFER, s_ssboDraw, s_adrawgpu.data(), s_adrawgpu.size() * sizeof(DRAWGPU));
    const BUFRANGE brWarp = upload(GL_SHADER_STORAGE_BUFFER, s_ssboWarp, s_awarpst.data(), s_awarpst.size() * sizeof(glm::vec4));
    const BUFRANGE brWarpMat = upload(GL_SHADER_STORAGE_BUFFER, s_ssboWarpMat, s_amatWarp.data(), s_amatWarp.size() * sizeof(glm::mat4));
    const BUFRANGE brIndirect = upload(GL_DRAW_INDIRECT_BUFFER, s_bufIndirect, s_adei.data(), s_adei.size() * sizeof(DEICMD));
    BUFRANGE brTexh{0, 0, 0};
    if (g_fGlobBindless)
    {
        // each pass draw's texture handle by its draw entry (0: the other entries, and a state without one)
        static std::vector<GLuint64> s_atexh;
        s_atexh.assign(s_adrawgpu.size(), 0);
        for (const CMD& c : s_acmd)
            if (c.cmdk == CMDK_Pass) s_atexh[c.idraw] = HandleOfIstate(c.istate);
        brTexh = upload(GL_SHADER_STORAGE_BUFFER, s_ssboTexh, s_atexh.data(), s_atexh.size() * sizeof(GLuint64));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, pv.fbo);
    glViewport(0, 0, pv.dx, pv.dy);
    const glm::vec4 clear = Vec4FromRgba(pv.rgbaClear, 1.0f / 255.0f);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(clear.r, clear.g, clear.b, float(pv.alphaClear) / 128.0f);
    glClearDepth(0.0);
    glStencilMask(0xFF);
    glClearStencil(0);
    // (the foreground pass: only the depth (0x2DF010's Z 0 over the frame) and the stencil, over the frame and HUD)
    glClear(pv.fForeground ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_CULL_FACE);

    auto bindRange = [](GLuint ibinding, const BUFRANGE& br) {
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ibinding, br.buf, GLintptr(br.off), GLsizeiptr(br.cb));
    };
    bindRange(0, brRec);
    bindRange(1, brDraw);
    bindRange(2, brBone);
    bindRange(3, brWarp);
    bindRange(4, brWarpMat);
    if (g_fGlobBindless) bindRange(5, brTexh);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, brIndirect.buf);
    const size_t offIndirect = brIndirect.off;
    glActiveTexture(GL_TEXTURE0);
    STATECACHE sc;
    bool fGlobProgram = false;
    auto useGlob = [&]() {
        if (fGlobProgram) return;
        glGlobShader.Use();
        glUniformMatrix4fv(s_u.matWorldToClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
        glUniform3fv(s_u.posCamera, 1, glm::value_ptr(cam.pos));
        glUniform3fv(s_u.dirCamera, 1, glm::value_ptr(cam.Dir()));
        glUniform1i(s_u.tex, 0);
        glUniform1i(s_u.texDate, 1);
        glUniform1i(s_u.fDate, 0);
        glUniform4fv(s_u.fogColor, 1, glm::value_ptr(Vec4FromRgba(opts.rgbaFog, 1.0f / 255.0f)));
        if (s_u.paint >= 0)
        {
            static float s_apaint[CPAINTVEC * 4];
            FillPaintUniforms(s_apaint);
            glUniform4fv(s_u.paint, CPAINTVEC, s_apaint);
        }
        fGlobProgram = true;
        sc.fValid = false;
    };
    static const bool s_fCelAlways = std::getenv("SLY2_CELALWAYS") != nullptr;
    for (const RUN& run : s_arun)
    {
        const CMD& cmd = s_acmd[run.icmd];
        switch (cmd.cmdk)
        {
        case CMDK_Pass:
            useGlob();
            // (bindless: a run with handles leaves the texture unit alone; one without, a single state, binds it)
            sc.Apply(s_astate[cmd.istate], !g_fGlobBindless || !HandleOfIstate(cmd.istate));
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<void*>(offIndirect + size_t(run.ideiFirst) * sizeof(DEICMD)),
                                        GLsizei(run.cdei), 0);
            s_glstats.crun += 1; s_glstats.cpass += run.cdei;
            g_renderstats.cpass += int(run.cdei);
            ++g_renderstats.cbatch;
            break;
        case CMDK_Volume:
            useGlob();
            DrawProjVolume(cmd);
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_Shadow:
            useGlob();
            DrawShadow(cmd, cam.pos, sNearClipSh, aposNear);
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_ShadowBegin:
            ShadowBegin(pv.fbo, pv.dx, pv.dy, int(cmd.order));
            ++g_renderstats.cbatch;
            break;
        case CMDK_ShadowKind8:
            ShadowKind8();
            ++g_renderstats.cbatch;
            break;
        case CMDK_FbaClear:
            ClearFbaBits();
            sc.fValid = false;
            break;
        case CMDK_ShadowCombine:
            ShadowCombine(cmd.count & 1, cmd.count & 2);
            fGlobProgram = false;
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_Shadow2:
            DrawShadow2(cmd, matWorldToClip);
            fGlobProgram = false;
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_Particles:
            glDisable(GL_STENCIL_TEST);
            if (opts.fMeshes) DrawParticles(matWorldToClip, cam.pos, aspect);
            fGlobProgram = false;
            sc.fValid = false;
            ++g_renderstats.cbatch;
            break;
        case CMDK_Cel:
        {
            const GLOB& geom = *cmd.prec->pglobGeom;
            glDisable(GL_CULL_FACE);
            glDisable(GL_STENCIL_TEST);
            glCelBorderShader.Use();
            fGlobProgram = false;
            sc.fValid = false;
            glUniformMatrix4fv(s_uc.matWorldToClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
            glUniformMatrix4fv(s_uc.matModel, 1, GL_FALSE, glm::value_ptr(*cmd.prec->pmat));
            // Half a PS2 pixel (448 lines) in NDC, square pixels; the line is one PS2 pixel wide (AA1 line + plain).
            const float hy = 1.0f / 448.0f;
            glUniform2f(s_uc.halfWidthNdc, hy / aspect, hy);
            glUniform1i(s_uc.edgeBase, 0);
            glm::vec4 rgba = Vec4FromRgba(opts.rgbaCel, 1.0f / 255.0f);
            // the damage flash (qw8.z): the border toward red (the reference's border at flash 0.5 / 0.75 / 1: red 87 /
            // 123 / 159 of 255, linear in the flash)
            if (const float gFlash = cmd.prec->psgs ? cmd.prec->psgs->gFlash : 0.0f; 0.0f < gFlash)
                rgba = glm::vec4(glm::mix(glm::vec3(rgba), glm::vec3(159.0f / 255.0f, 0.0f, 0.0f), std::min(gFlash, 1.0f)), rgba.a);
            // P_A: the alpha times 0.6 times qw8.y while the object fades (runtime/native_cel_geometry.cpp)
            const float a2 = cmd.prec->alpha2;
            glUniform4f(s_uc.celRgba, rgba.r, rgba.g, rgba.b, std::min(rgba.a * 255.0f / 128.0f, 1.0f) * (a2 < 1.0f ? a2 * 0.6f : 1.0f));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, geom.pgl->ssboEdges);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, geom.pgl->ssboEdgeSkin);
            glUniform1i(s_uc.cbone, cmd.prec->pamatBone ? GLint(cmd.prec->pamatBone->size()) : 0);
            glUniform1i(s_uc.iboneFirst, GLint(cmd.prec->iboneFirst));
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(s_fCelAlways ? GL_ALWAYS : GL_GEQUAL);
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBindVertexArray(s_vaoEmpty);
            glDrawArrays(GL_TRIANGLES, 0, geom.pgl->cedge * 6);
            ++g_renderstats.ccel;
            ++g_renderstats.cbatch;
            break;
        }
        }
    }
    // (QoL, cosmetic.h) the characters' worn items: the frame's own view only
    if (pv.irtgt < 0 && !pv.fForeground)
    {
        const glm::vec4 fogColor = Vec4FromRgba(opts.rgbaFog, 1.0f / 255.0f);
        const float afog[8] = {fogColor.r, fogColor.g, fogColor.b, 1.0f, opts.sFogNear, opts.sFogFar, opts.uFogMax, opts.fFog ? 1.0f : 0.0f};
        DrawCosmetics(glm::value_ptr(matWorldToClip), glm::value_ptr(cam.pos), afog);
        fGlobProgram = false;
        sc.fValid = false;
    }
    if (s_cframeGlStats && pv.irtgt < 0 && !pv.fForeground)
    {
        static int s_cframe = 0;
        if (++s_cframe % s_cframeGlStats == 0)
        {
            const double n = s_cframeGlStats;
            std::printf("glstats: per frame %.0f runs (%.0f draws), %.0f applies (%.0f full), %.0f texture binds, %.0f sampler binds, %.0f uniforms, %.0f blend%s%s\n",
                        s_glstats.crun / n, s_glstats.cpass / n, s_glstats.capply / n, s_glstats.cfull / n, s_glstats.ctex / n, s_glstats.csampler / n,
                        s_glstats.cuniform / n, s_glstats.cblend / n, g_fGlobBindless ? " (bindless)" : "", s_ring.buf ? " (persistent ring)" : "");
            s_glstats = {};
        }
    }
    glBindVertexArray(0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
}
}  // namespace

namespace
{
// orig: 0x001BE990 / 0x001BE9E8 (each owned target: its camera's view / projection (0x1BE6F0), the clear packet)
// then the target's kinds 15..17 / 18..20 drawn into it.
void RenderRenderTargets(const SCENE& scene, const RENDEROPTS& opts)
{
    for (int i = 0; i < CRTGT; ++i)
    {
        RTGT* p = PrtgtGet(i);
        if (!p || !p->powner) continue;
        GlTextureOfRtgt(i);   // (the FBO)
        PASSVIEW pv;
        pv.irtgt = i;
        pv.fbo = p->fbo;
        pv.dx = p->dx;
        pv.dy = p->dy;
        pv.aspect = float(p->dx) / float(p->dy);
        pv.pos = p->pos;
        pv.dir = glm::normalize(p->dirForward);
        // reversed-Z infinite far, the target's aspect (BuildSimpleProjectionMatrix with dx / (tan x aspect), dy / tan)
        const float f = 1.0f / std::tan(p->radFovY * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = f / pv.aspect;
        proj[1][1] = f;
        proj[2][3] = -1.0f;
        proj[3][2] = p->sNear;
        pv.matWorldToClip = proj * glm::lookAt(p->pos, p->pos + pv.dir, p->dirUp);
        pv.rgbaClear = 0;   // 0x1BE398's clear: RGBAQ 0, Z 0
        pv.alphaClear = 0;
        RenderPass(scene, pv, opts);
    }
}
}  // namespace

void RenderScene(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts)
{
    g_renderstats.cdraw = g_renderstats.cpass = g_renderstats.ccel = 0;
    g_renderstats.cbatch = g_renderstats.cculled = g_renderstats.cmrd = 0;
    s_ring.BeginFrame();
    // The render targets first (their globsets, their cameras), so this frame's screen quads show them.
    RenderRenderTargets(scene, opts);
    PASSVIEW pv;
    pv.aspect = float(g_gl.renderWidth) / float(g_gl.renderHeight);
    // The projection for the picture's aspect mode (display.h: Hor+, the PS2 anamorphic 4:3, Vert- 16:9).
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA camDisplay = cam;
    camDisplay.fovY = glm::degrees(proj.radFovY);
    pv.matWorldToClip = camDisplay.MatWorldToClip(proj.rAspect);
    pv.rMRDAdjust = cam.rMRDAdjust;
    pv.grfChannels = cam.grfChannels;
    pv.pos = cam.pos;
    pv.dir = cam.Dir();
    pv.fbo = g_gl.FboScene();
    pv.dx = g_gl.renderWidth;
    pv.dy = g_gl.renderHeight;
    pv.rgbaClear = opts.rgbaClear;
    RenderPass(scene, pv, opts);
}

void RenderSceneForeground(const SCENE& scene, const CAMERA& cam, const RENDEROPTS& opts)
{
    if (std::none_of(scene.aglobset.begin(), scene.aglobset.end(), [](const SCENEGLOBSET& s) { return s.fForeground && s.irtgt < 0; })) return;
    PASSVIEW pv;
    pv.aspect = float(g_gl.renderWidth) / float(g_gl.renderHeight);
    const DISPPROJ proj = ProjectionForDisplay(glm::radians(cam.fovY));
    CAMERA camDisplay = cam;
    camDisplay.fovY = glm::degrees(proj.radFovY);
    pv.matWorldToClip = camDisplay.MatWorldToClip(proj.rAspect);
    pv.rMRDAdjust = cam.rMRDAdjust;
    pv.grfChannels = cam.grfChannels;
    pv.pos = cam.pos;
    pv.dir = cam.Dir();
    pv.fbo = g_gl.FboScene();
    pv.dx = g_gl.renderWidth;
    pv.dy = g_gl.renderHeight;
    pv.fForeground = true;
    RenderPass(scene, pv, opts);
}
