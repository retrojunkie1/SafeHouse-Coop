// Cosmetics (cosmetic.h): the mods folder's worn items, their attachment, the equip state and their draw.

#include "cosmetic.h"

#include "activate.h"
#include "alo.h"
#include "bone.h"
#include "glob.h"
#include "disc.h"
#include "gl.h"
#include "lo.h"
#include "paint.h"
#include "sw.h"
#include "so.h"
#include "stepphys.h"
#include "../platform/platform.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;
extern SW* g_psw;
LO* PloFindSwObjectByOid(SW* psw, OID oid);
ALO* PaloCoopRemoteRoot();       // coop.cpp: the other player's drawn copy (null: none shown)
bool FCoopRemoteShown();

namespace
{
// ------------------------------------------------------------ the characters' attachment points
// (their trees' bone records and groups, from their meshes' bindings: Sly 7, Bentley 8, Murray 9)
struct CHARRIG
{
    int iboneHead;               // the head's record (-1: the hat group's owner record)
    int iglobHatOwner;           // (iboneHead -1) the group whose owner record is the head's frame
    int axisFwd, axisUp;         // that record's axes for forward and up: 1 x, 2 y, 3 z, negative: reversed
    float sHeadTop;              // the top of the head above the record (along up)
    float sScale;                // the items' size for this character (authored on Sly)
    int iboneSpine;              // the back's record
    float sBackUp, sBackOut;     // the back's point: up from that record, out behind it
    std::vector<int> aiglobHat;  // the groups of its own hat (hidden by an item that replaces it)
    int oidCane;                 // the cane's object (Sly), -1 none
    int aiboneFist[2];           // the fists' records (left, right), -1 none
    int axisFistFwd, axisFistUp; // their axes for the punch's direction and the back of the hand
    float sFistOut;              // the fist's centre along that direction from the record
    int axisFistFwdL = 0, axisFistUpL = 0;   // the left's, when its record's axes differ (0: the same)
    float sFistScale = 0.0f;     // the fist items' size (0: sScale)
    std::vector<int> aaiRecHand[2];   // the records whose skinned vertices make each hand (their centre: the fist's)
    float sHeadFwd = 0.0f;       // the head's point forward of the record (the head and face items)
};
const CHARRIG s_arig[CPAINTCHAR] = {
    // Sly: the head record's x up, y back, z right; his cap is part of his head (not hidden)
    {73, -1, -2, 1, 30.0f, 1.0f, 39, 45.0f, 6.0f, {}, 316, {-1, -1}, 1, 3, 0.0f, 0, 0, 0.0f, {}, 3.0f},
    // Bentley: his hat (group 24, with its outline) placed by its own record: x forward, z up
    {-1, 24, 1, 3, 4.0f, 1.5f, 27, 12.0f, 30.0f, {23, 24, 25}, -1, {-1, -1}, 1, 3, 0.0f, 0, 0, 0.0f, {}, 20.0f},
    // Murray: the head record's y forward, z down; his hat is part of his body (not hidden: worn over it); his hands'
    // records 75 (left: y along the forearm) and 122 (right: -y), z the back of the hand
    {84, -1, 2, -3, 55.0f, 1.35f, 61, -6.0f, 26.0f, {}, -1, {75, 122}, -2, 3, 0.0f, 2, 3, 2.1f, {{76, 78, 80}, {124, 127, 129}}, 12.0f},
};

// ------------------------------------------------------------ the items
struct VTXCOS
{
    glm::vec3 pos, normal;
    glm::vec4 color;
    glm::vec2 uv;
};
struct ITEM
{
    std::string id, name;
    unsigned grfChar = 7;        // bit per character
    int islot = COSSLOT_Head;
    glm::mat4 matOffset{1.0f};   // in the attachment's frame
    bool fHidesHat = false, fHidesCane = false;
    std::vector<VTXCOS> avtx;    // triangles
    std::string texPath;
    unsigned vao = 0, vbo = 0, tex = 0;
};
std::vector<std::unique_ptr<ITEM>> s_aitem;
bool s_fScanned = false;

std::string Trim(std::string s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// A Wavefront .obj (v / vt / vn / f, usemtl) with its .mtl's diffuse colours (Kd) and the first diffuse texture
bool FLoadObj(const fs::path& path, ITEM& it)
{
    std::ifstream in(path);
    if (!in) return false;
    std::vector<glm::vec3> apos, anorm;
    std::vector<glm::vec2> auv;
    std::map<std::string, glm::vec4> mpmtl;
    glm::vec4 colorCur(0.8f, 0.8f, 0.8f, 1.0f);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream ss(line);
        std::string k;
        ss >> k;
        if (k == "v") { glm::vec3 p; ss >> p.x >> p.y >> p.z; apos.push_back(p); }
        else if (k == "vn") { glm::vec3 n; ss >> n.x >> n.y >> n.z; anorm.push_back(n); }
        else if (k == "vt") { glm::vec2 t; ss >> t.x >> t.y; auv.push_back(t); }
        else if (k == "mtllib")
        {
            std::string name;
            std::getline(ss, name);
            std::ifstream mtl(path.parent_path() / Trim(name));
            std::string cur, l2;
            while (std::getline(mtl, l2))
            {
                std::istringstream s2(l2);
                std::string k2;
                s2 >> k2;
                if (k2 == "newmtl") { s2 >> cur; mpmtl[cur] = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f); }
                else if (k2 == "Kd") { glm::vec3 c; s2 >> c.r >> c.g >> c.b; mpmtl[cur] = glm::vec4(c, 1.0f); }
                else if (k2 == "map_Kd" && it.texPath.empty())
                {
                    std::string t;
                    std::getline(s2, t);
                    it.texPath = (path.parent_path() / Trim(t)).string();
                }
            }
        }
        else if (k == "usemtl")
        {
            std::string name;
            ss >> name;
            auto f = mpmtl.find(name);
            colorCur = f != mpmtl.end() ? f->second : glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
        }
        else if (k == "f")
        {
            std::vector<VTXCOS> apoly;
            std::string tok;
            while (ss >> tok)
            {
                int iv = 0, it2 = 0, in2 = 0;
                const size_t s1 = tok.find('/');
                iv = std::atoi(tok.c_str());
                if (s1 != std::string::npos)
                {
                    const size_t s2 = tok.find('/', s1 + 1);
                    if (s2 != s1 + 1) it2 = std::atoi(tok.c_str() + s1 + 1);
                    if (s2 != std::string::npos) in2 = std::atoi(tok.c_str() + s2 + 1);
                }
                auto ix = [](int i, size_t c) { return i < 0 ? int(c) + i : i - 1; };
                VTXCOS v{};
                const int a = ix(iv, apos.size());
                if (a < 0 || a >= int(apos.size())) continue;
                v.pos = apos[size_t(a)];
                if (it2) { const int t = ix(it2, auv.size()); if (t >= 0 && t < int(auv.size())) v.uv = auv[size_t(t)]; }
                v.normal = glm::vec3(0.0f);
                if (in2) { const int n = ix(in2, anorm.size()); if (n >= 0 && n < int(anorm.size())) v.normal = anorm[size_t(n)]; }
                v.color = colorCur;
                apoly.push_back(v);
            }
            for (size_t i = 1; i + 1 < apoly.size(); ++i)
            {
                VTXCOS tri[3] = {apoly[0], apoly[i], apoly[i + 1]};
                const glm::vec3 n = glm::normalize(glm::cross(tri[1].pos - tri[0].pos, tri[2].pos - tri[0].pos) + glm::vec3(1e-9f));
                for (VTXCOS& v : tri)
                {
                    if (glm::length(v.normal) < 0.5f) v.normal = n;
                    it.avtx.push_back(v);
                }
            }
        }
    }
    return !it.avtx.empty();
}

void ScanItems()
{
    if (s_fScanned) return;
    s_fScanned = true;
    // the release's starter items (share/safehouse/sly2/Cosmetics), then the player's Mods/Cosmetics (an item of the
    // same folder name there replaces the bundled one)
    std::vector<fs::path> adir;
    std::error_code ec;
    auto scan = [&](const fs::path& dir) {
        if (!fs::is_directory(dir, ec)) return;
        std::vector<fs::path> a;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory()) a.push_back(e.path());
        std::sort(a.begin(), a.end());
        for (const fs::path& d : a)
        {
            const auto itOld = std::find_if(adir.begin(), adir.end(), [&](const fs::path& o) { return o.filename() == d.filename(); });
            if (itOld != adir.end()) *itOld = d;
            else adir.push_back(d);
        }
    };
    scan(cane::shareDirectory() / "safehouse" / "sly2" / "Cosmetics");
    if (const std::string dirMods = g_disc.DirMods(); !dirMods.empty()) scan(cane::path(dirMods) / "Cosmetics");
    for (const fs::path& d : adir)
    {
        std::ifstream ini(d / "item.ini");
        if (!ini) continue;
        auto pit = std::make_unique<ITEM>();
        ITEM& it = *pit;
        it.id = cane::utf8(d.filename());
        it.name = it.id;
        std::string model;
        glm::vec3 off(0.0f), rot(0.0f);
        float scale = 1.0f;
        std::string line;
        while (std::getline(ini, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '#' || line[0] == ';') continue;
            const std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
            std::istringstream ss(v);
            if (k == "name") it.name = v;
            else if (k == "model") model = v;
            else if (k == "slot") it.islot = v == "back" ? COSSLOT_Back : v == "hand" || v == "cane" ? COSSLOT_Hand : v == "fists" ? COSSLOT_Fists : v == "face" ? COSSLOT_Face : COSSLOT_Head;
            else if (k == "characters")
            {
                it.grfChar = 0;
                if (v.find("sly") != std::string::npos) it.grfChar |= 1;
                if (v.find("bentley") != std::string::npos) it.grfChar |= 2;
                if (v.find("murray") != std::string::npos) it.grfChar |= 4;
                if (v.find("all") != std::string::npos) it.grfChar = 7;
            }
            else if (k == "offset") ss >> off.x >> off.y >> off.z;
            else if (k == "rotate") ss >> rot.x >> rot.y >> rot.z;
            else if (k == "scale") ss >> scale;
            else if (k == "hides")
            {
                it.fHidesHat = v.find("hat") != std::string::npos;
                it.fHidesCane = v.find("cane") != std::string::npos;
            }
        }
        if (it.islot == COSSLOT_Hand) it.grfChar &= 1;   // (the hand: Sly's cane)
        if (it.islot == COSSLOT_Fists)                    // (the fists: the characters with them placed)
            for (int c = 0; c < CPAINTCHAR; ++c)
                if (s_arig[c].aiboneFist[1] < 0) it.grfChar &= ~(1 << c);
        if (model.empty() && (it.fHidesHat || it.fHidesCane)) {}   // (an item that only hides: "No hat")
        else if (model.empty() || !FLoadObj(d / model, it))
        {
            std::fprintf(stderr, "cosmetics: %s: no model loaded\n", it.id.c_str());
            continue;
        }
        it.matOffset = glm::translate(glm::mat4(1.0f), off) *
                       glm::rotate(glm::mat4(1.0f), glm::radians(rot.z), glm::vec3(0, 0, 1)) *
                       glm::rotate(glm::mat4(1.0f), glm::radians(rot.y), glm::vec3(0, 1, 0)) *
                       glm::rotate(glm::mat4(1.0f), glm::radians(rot.x), glm::vec3(1, 0, 0)) *
                       glm::scale(glm::mat4(1.0f), glm::vec3(scale));
        std::printf("cosmetics: %s (%s): %zu triangles\n", it.id.c_str(), SzCosSlot(it.islot), it.avtx.size() / 3);
        s_aitem.push_back(std::move(pit));
    }
}

ITEM* PitemOf(const std::string& id)
{
    ScanItems();
    for (auto& p : s_aitem)
        if (p->id == id) return p.get();
    return nullptr;
}

// ------------------------------------------------------------ equipped
std::string s_aequipLocal[CPAINTCHAR][COSSLOT_Max];
std::string s_aequipRemote[CPAINTCHAR][COSSLOT_Max];
bool s_fLoaded = false;
fs::path PathIni() { return cane::userDirectory() / "cosmetics.ini"; }
void LoadEquipped()
{
    if (s_fLoaded) return;
    s_fLoaded = true;
    std::ifstream in(PathIni());
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream ss(line);
        int c = -1, s = -1;
        std::string id;
        if (!(ss >> c >> s >> id) || c < 0 || c >= CPAINTCHAR || s < 0 || s >= COSSLOT_Max) continue;
        s_aequipLocal[c][s] = id;
    }
}

// the character of a tree's root, and whether it is the other player's copy
bool FCharOfRoot(const ALO* proot, int* pichar, bool* pfRemote)
{
    if (!proot) return false;
    const ALO* premote = PaloCoopRemoteRoot();
    *pfRemote = premote && proot == premote;
    *pichar = IPaintCharOfOid(proot->oid);
    return *pichar >= 0;
}
const std::string& StrEquippedFor(int ichar, bool fRemote, int islot)
{
    LoadEquipped();
    return fRemote ? s_aequipRemote[ichar][islot] : s_aequipLocal[ichar][islot];
}

// ------------------------------------------------------------ drawing
GLSHADER s_sh;
bool s_fShader = false;
void UploadItem(ITEM& it)
{
    if (it.vao) return;
    glGenVertexArrays(1, &it.vao);
    glGenBuffers(1, &it.vbo);
    glBindVertexArray(it.vao);
    glBindBuffer(GL_ARRAY_BUFFER, it.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(it.avtx.size() * sizeof(VTXCOS)), it.avtx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VTXCOS), reinterpret_cast<void*>(offsetof(VTXCOS, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VTXCOS), reinterpret_cast<void*>(offsetof(VTXCOS, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(VTXCOS), reinterpret_cast<void*>(offsetof(VTXCOS, color)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(VTXCOS), reinterpret_cast<void*>(offsetof(VTXCOS, uv)));
    glBindVertexArray(0);
    if (!it.texPath.empty())
    {
        int w = 0, h = 0;
        stbi_set_flip_vertically_on_load(1);
        if (stbi_uc* pb = stbi_load(it.texPath.c_str(), &w, &h, nullptr, 4))
        {
            glGenTextures(1, &it.tex);
            glBindTexture(GL_TEXTURE_2D, it.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pb);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            stbi_image_free(pb);
        }
        stbi_set_flip_vertically_on_load(0);
    }
}

// The skinned vertices' centroids by their main record (index in proot->axf; debug and the fists' places): the
// record -> {count, world centroid}
std::map<int, std::pair<int, glm::vec3>> MpRecordCentroid(const ALO* proot, std::vector<glm::vec3>* pavAll = nullptr)
{
    std::map<int, std::pair<int, glm::vec3>> mp;
    const auto* paabind = PaabindOfAlo(proot);
    if (!paabind || !proot->pglobset) return mp;
    std::vector<glm::mat4> amat;
    std::map<const XF*, int> mpxfirec;
    for (size_t k = 0; k < proot->axf.size(); ++k) mpxfirec[proot->axf[k].get()] = int(k);
    for (size_t ig = 0; ig < proot->pglobset->aglob.size() && ig < paabind->size(); ++ig)
    {
        const GLOB& g = proot->pglobset->aglob[ig];
        const auto& abind = (*paabind)[ig];
        if (abind.empty() || !FGlobBoneMatrices(proot, int(ig), &amat)) continue;
        for (const VTXGLOB& v : g.avtx)
        {
            int jBest = 0;
            glm::vec3 w(0.0f);
            float sw = 0.0f;
            for (int j = 0; j < 4; ++j)
            {
                if (!v.aw[j]) continue;
                const size_t ib = std::min<size_t>(v.abone[j], amat.size() - 1);
                w += float(v.aw[j]) * glm::vec3(amat[ib] * glm::vec4(v.pos, 1.0f));
                sw += float(v.aw[j]);
                if (v.aw[j] > v.aw[jBest]) jBest = j;
            }
            if (sw <= 0.0f) continue;
            const XF* pxf = abind[std::min<size_t>(v.abone[jBest], abind.size() - 1)].pxfBone;
            const auto itr = mpxfirec.find(pxf);
            auto& e = mp[itr == mpxfirec.end() ? -1 : itr->second];
            e.first++;
            e.second += w / sw;
            if (pavAll) pavAll->push_back(w / sw);
        }
    }
    for (auto& [k, e] : mp) e.second /= float(e.first);
    return mp;
}

// The centre of a hand's skinned vertices (the records listed), refreshed once per drawn frame
int s_cDrawCosmetics = 0;
bool FHandCentre(const ALO* proot, const std::vector<int>& airec, glm::vec3* ppos)
{
    static std::map<const ALO*, std::pair<int, std::map<int, std::pair<int, glm::vec3>>>> s_mp;
    auto& e = s_mp[proot];
    if (e.first != s_cDrawCosmetics || e.second.empty())
    {
        e.first = s_cDrawCosmetics;
        e.second = MpRecordCentroid(proot);
    }
    int c = 0;
    glm::vec3 sum(0.0f);
    for (int k : airec)
        if (auto it = e.second.find(k); it != e.second.end()) c += it->second.first, sum += it->second.second * float(it->second.first);
    if (!c) return false;
    *ppos = sum / float(c);
    return true;
}

// The attachment's world matrix: X forward (the character's heading), Z up (along its spine to its head), at the top of
// its head / the middle of its back, scaled for the character; the hand: the cane's own placement
bool FAttachMatrix(const ALO* proot, int ichar, int islot, glm::mat4* pmat, int iside = 1)
{
    const CHARRIG& rig = s_arig[ichar];
    if (islot == COSSLOT_Fists)
    {
        // a fist (iside 0 left, 1 right): X the punch's direction, Z the back of the hand, at the fist's centre; the
        // left one mirrored (its Y reversed) so one model fits both
        const int ib = rig.aiboneFist[iside];
        if (ib < 0 || ib >= int(proot->axf.size()) || !proot->axf[size_t(ib)]) return false;
        const glm::mat4& mb = proot->axf[size_t(ib)]->matWorld;
        if (std::getenv("SLY2N_RAWHAND"))   // (debug: the record's own frame)
        {
            *pmat = mb;
            return true;
        }
        auto axisb = [&](int a) {
            const glm::vec3 v = glm::normalize(glm::vec3(mb[std::abs(a) - 1]));
            return a < 0 ? -v : v;
        };
        const glm::vec3 fwd = axisb(iside == 0 && rig.axisFistFwdL ? rig.axisFistFwdL : rig.axisFistFwd);
        glm::vec3 up = axisb(iside == 0 && rig.axisFistUpL ? rig.axisFistUpL : rig.axisFistUp);
        up = glm::normalize(up - fwd * glm::dot(up, fwd));
        glm::vec3 side = glm::cross(up, fwd);
        if (iside == 0) side = -side;
        // (SLY2N_FISTTUNE="out,scale": overrides for tuning)
        float sOut = rig.sFistOut, sScale = rig.sFistScale > 0.0f ? rig.sFistScale : rig.sScale;
        if (const char* szt = std::getenv("SLY2N_FISTTUNE")) std::sscanf(szt, "%f,%f", &sOut, &sScale);
        glm::mat4 m(1.0f);
        m[0] = glm::vec4(fwd * sScale, 0.0f);
        m[1] = glm::vec4(side * sScale, 0.0f);
        m[2] = glm::vec4(up * sScale, 0.0f);
        glm::vec3 pos = glm::vec3(mb[3]);
        if (!rig.aaiRecHand[iside].empty()) FHandCentre(proot, rig.aaiRecHand[iside], &pos);
        m[3] = glm::vec4(pos + fwd * sOut, 1.0f);
        *pmat = m;
        return true;
    }
    if (islot == COSSLOT_Hand)
    {
        if (rig.oidCane < 0) return false;
        for (const LO* pc : proot->aploChild)
            if (pc && pc->oid == rig.oidCane)
            {
                const ALO* pcane = static_cast<const ALO*>(pc);
                if (pcane->axf.empty()) return false;
                *pmat = pcane->axf[0]->matWorld;
                return true;
            }
        return false;
    }
    // the head's record and its frame
    const XF* pxfHead = nullptr;
    if (rig.iboneHead >= 0) pxfHead = rig.iboneHead < int(proot->axf.size()) ? proot->axf[size_t(rig.iboneHead)].get() : nullptr;
    else if (proot->pglobset && int(proot->pglobset->aglob.size()) > rig.iglobHatOwner &&
             proot->pglobset->aglob[size_t(rig.iglobHatOwner)].oidOwner >= 0)
        pxfHead = PxfFindAlo(const_cast<ALO*>(proot), proot->pglobset->aglob[size_t(rig.iglobHatOwner)].oidOwner);
    if (!pxfHead) return false;
    const glm::mat4& mh = pxfHead->matWorld;
    if (std::getenv("SLY2N_RAWHEAD") && islot == COSSLOT_Head)   // (debug: the head record's own frame)
    {
        *pmat = mh;
        return true;
    }
    auto axis = [&](int a) {
        const glm::vec3 v = glm::normalize(glm::vec3(mh[std::abs(a) - 1]));
        return a < 0 ? -v : v;
    };
    const glm::vec3 up = axis(rig.axisUp);
    glm::vec3 fwd = axis(rig.axisFwd);
    fwd = glm::normalize(fwd - up * glm::dot(fwd, up));
    const glm::vec3 side = glm::cross(up, fwd);
    glm::vec3 pos;
    if (islot == COSSLOT_Head || islot == COSSLOT_Face) pos = glm::vec3(mh[3]) + up * rig.sHeadTop + fwd * rig.sHeadFwd;
    else
    {
        if (rig.iboneSpine >= int(proot->axf.size()) || !proot->axf[size_t(rig.iboneSpine)]) return false;
        pos = glm::vec3(proot->axf[size_t(rig.iboneSpine)]->matWorld[3]) + up * rig.sBackUp - fwd * rig.sBackOut;
    }
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(fwd * rig.sScale, 0.0f);
    m[1] = glm::vec4(side * rig.sScale, 0.0f);
    m[2] = glm::vec4(up * rig.sScale, 0.0f);
    m[3] = glm::vec4(pos, 1.0f);
    *pmat = m;
    return true;
}
}

const char* SzCosSlot(int islot) { return islot == COSSLOT_Head ? "head" : islot == COSSLOT_Back ? "back" : islot == COSSLOT_Fists ? "fists" : islot == COSSLOT_Face ? "face" : "hand"; }

std::vector<std::string> AstrCosmetics(int ichar, int islot)
{
    ScanItems();
    std::vector<std::string> a;
    for (const auto& p : s_aitem)
        if (p->islot == islot && (p->grfChar >> ichar & 1)) a.push_back(p->id);
    return a;
}
std::string StrCosmeticName(const std::string& id)
{
    const ITEM* p = PitemOf(id);
    return p ? p->name : id;
}
std::string StrEquipped(int ichar, int islot)
{
    LoadEquipped();
    return s_aequipLocal[ichar][islot];
}
std::string StrLocalEquipped(int ichar, int islot) { return StrEquipped(ichar, islot); }
void SetEquipped(int ichar, int islot, const std::string& id)
{
    LoadEquipped();
    s_aequipLocal[ichar][islot] = id;
}
void SaveCosmetics()
{
    LoadEquipped();
    std::ofstream out(PathIni());
    out << "# character slot item (the safe house wardrobe: Mods/Cosmetics/<item>)\n";
    for (int c = 0; c < CPAINTCHAR; ++c)
        for (int s = 0; s < COSSLOT_Max; ++s)
            if (!s_aequipLocal[c][s].empty()) out << c << ' ' << s << ' ' << s_aequipLocal[c][s] << '\n';
}
void SetRemoteEquipped(int ichar, int islot, const std::string& id)
{
    if (ichar >= 0 && ichar < CPAINTCHAR && islot >= 0 && islot < COSSLOT_Max) s_aequipRemote[ichar][islot] = id;
}
void ClearRemoteCosmetics()
{
    for (auto& a : s_aequipRemote)
        for (auto& s : a) s.clear();
}

bool FCosmeticHidesGlob(const ALO* palo, size_t iglob)
{
    if (!palo) return false;
    static const char* s_sz = std::getenv("SLY2N_HIDEGROUPS");   // (debug: "oid:i,oid:i" those groups hidden)
    if (s_sz)
    {
        const std::string key = std::to_string(palo->oid) + ":" + std::to_string(iglob);
        const std::string all = std::string(",") + s_sz + ",";
        if (all.find("," + key + ",") != std::string::npos) return true;
        if (all.find("," + std::to_string(palo->oid) + ":*,") != std::string::npos) return true;
    }
    // (the cane's own object: hidden by a cane worn in its place)
    const ALO* proot = palo;
    while (proot->paloParent) proot = proot->paloParent;
    int ichar = -1;
    bool fRemote = false;
    if (!FCharOfRoot(proot, &ichar, &fRemote)) return false;
    const CHARRIG& rig = s_arig[ichar];
    if (palo != proot)
    {
        // (the cane: its shaft 316, its head 317 under it, and its other copy 318 with a head of its own)
        const bool fCane = rig.oidCane >= 0 && (palo->oid == rig.oidCane || palo->oid == rig.oidCane + 1 || palo->oid == rig.oidCane + 2 ||
                                                (palo->paloParent && palo->paloParent->oid == rig.oidCane));
        if (!fCane) return false;
        const ITEM* p = PitemOf(StrEquippedFor(ichar, fRemote, COSSLOT_Hand));
        return p && p->fHidesCane;
    }
    if (std::find(rig.aiglobHat.begin(), rig.aiglobHat.end(), int(iglob)) == rig.aiglobHat.end()) return false;
    for (int s : {int(COSSLOT_Head), int(COSSLOT_Face)})
        if (const ITEM* p = PitemOf(StrEquippedFor(ichar, fRemote, s)); p && p->fHidesHat) return true;
    return false;
}

void DrawCosmetics(const float* pmatWorldToClip, const float* pposCamera, const float* pfog)
{
    if (!g_psw) return;
    ++s_cDrawCosmetics;
    ScanItems();
    if (s_aitem.empty()) return;
    // the characters drawn: the gang's in this world, and the other player's copy
    std::vector<std::pair<const ALO*, bool>> aroot;
    for (int oid : {7, 8, 9})
        if (LO* plo = PloFindSwObjectByOid(g_psw, oid); plo && plo->fInWorld && FCidDerivedFrom(plo->Cid(), 0x30))
            aroot.emplace_back(static_cast<const ALO*>(plo), false);
    if (ALO* pr = PaloCoopRemoteRoot(); pr && FCoopRemoteShown()) aroot.emplace_back(pr, true);
    if (!s_fShader)
    {
        s_fShader = true;
        s_sh.Init("cosmetic.vert", "cosmetic.frag");
    }
    if (!s_sh.ID) return;
    s_sh.Use();
    glUniformMatrix4fv(s_sh.Loc("matWorldToClip"), 1, GL_FALSE, pmatWorldToClip);
    glUniform3fv(s_sh.Loc("posCamera"), 1, pposCamera);
    glUniform4fv(s_sh.Loc("fogParams"), 1, pfog + 4);
    glUniform4fv(s_sh.Loc("fogColor"), 1, pfog);
    glUniform3f(s_sh.Loc("dirLight"), 0.3f, -0.4f, 0.85f);
    glUniform3f(s_sh.Loc("rgbAmbient"), 0.42f, 0.44f, 0.52f);
    glUniform3f(s_sh.Loc("rgbLight"), 0.55f, 0.53f, 0.5f);
    glUniform1i(s_sh.Loc("tex"), 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(std::getenv("SLY2N_RAWHEAD") ? GL_ALWAYS : GL_GEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (static const bool s_fDbg = std::getenv("SLY2N_COSDBG") != nullptr; s_fDbg)
    {
        static int s_c = 0;
        if (++s_c % 120 == 1)
            for (const auto& [proot, fRemote] : aroot)
            {
                const int ichar = IPaintCharOfOid(proot->oid);
                auto pr = [&](const char* sz, const glm::mat4& m) {
                    glm::mat4 f = proot->apxfRef[1] ? proot->apxfRef[1]->matWorld : glm::mat4(1.0f);
                    f[3] = glm::vec4(PosLoWorld(proot), 1.0f);
                    const glm::mat4 r = glm::inverse(f) * m;
                    std::printf("cosdbg %d %s: rel pos %.1f %.1f %.1f  x %.2f %.2f %.2f  y %.2f %.2f %.2f  z %.2f %.2f %.2f\n", proot->oid, sz,
                                r[3].x, r[3].y, r[3].z, r[0].x, r[0].y, r[0].z, r[1].x, r[1].y, r[1].z, r[2].x, r[2].y, r[2].z);
                };
                glm::mat4 m;
                for (int s = 0; s < COSSLOT_Max; ++s)
                    if (FAttachMatrix(proot, ichar, s, &m)) pr(SzCosSlot(s), m);
                if (FAttachMatrix(proot, ichar, COSSLOT_Fists, &m, 0)) pr("fist L", m);
                if (std::getenv("SLY2N_COSFIT"))   // (debug: the head's and the back's surfaces about their places)
                {
                    std::vector<glm::vec3> av;
                    MpRecordCentroid(proot, &av);
                    for (int s : {int(COSSLOT_Head), int(COSSLOT_Back)})
                    {
                        if (!FAttachMatrix(proot, ichar, s, &m)) continue;
                        glm::mat4 u = m;
                        for (int c = 0; c < 3; ++c) u[c] = glm::vec4(glm::normalize(glm::vec3(m[c])), 0.0f);
                        const glm::mat4 ui = glm::inverse(u);
                        int c = 0;
                        glm::vec3 sum(0.0f), lo(1e9f), hi(-1e9f);
                        for (const glm::vec3& w : av)
                        {
                            const glm::vec3 l = glm::vec3(ui * glm::vec4(w, 1.0f));
                            const bool fIn = s == COSSLOT_Head ? (l.x * l.x + l.y * l.y < 40.0f * 40.0f && l.z > -45.0f && l.z < 20.0f)
                                                               : (std::abs(l.z) < 10.0f && std::abs(l.y) < 12.0f && std::abs(l.x) < 70.0f);
                            if (!fIn) continue;
                            ++c, sum += l, lo = glm::min(lo, l), hi = glm::max(hi, l);
                        }
                        if (c) sum /= float(c);
                        std::printf("cosfit %d %s: %d verts centre %.1f %.1f %.1f  x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n", proot->oid,
                                    SzCosSlot(s), c, sum.x, sum.y, sum.z, lo.x, hi.x, lo.y, hi.y, lo.z, hi.z);
                    }
                }
                if (s_c == 1 || std::getenv("SLY2N_COSBONES"))
                {
                    const glm::vec3 p0 = PosLoWorld(proot);
                    std::printf("cosbones %d:", proot->oid);
                    for (size_t i = 0; i < proot->axf.size(); ++i)
                        if (proot->axf[i])
                        {
                            const glm::vec3 d = glm::vec3(proot->axf[i]->matWorld[3]) - p0;
                            std::printf(" %zu:%.0f,%.0f,%.0f", i, d.x, d.y, d.z);
                        }
                    std::printf("\n");
                    if (std::getenv("SLY2N_COSVERTS"))   // (debug: the skinned vertices by their main record)
                    {
                        glm::mat4 f = proot->apxfRef[1] ? proot->apxfRef[1]->matWorld : glm::mat4(1.0f);
                        f[3] = glm::vec4(PosLoWorld(proot), 1.0f);
                        const glm::mat4 fi = glm::inverse(f);
                        for (const auto& [k, e] : MpRecordCentroid(proot))
                        {
                            const glm::vec3 r = glm::vec3(fi * glm::vec4(e.second, 1.0f));
                            std::printf("cosverts %d rec %d: %d at %.1f %.1f %.1f\n", proot->oid, k, e.first, r.x, r.y, r.z);
                        }
                    }
                    if (const char* szb = std::getenv("SLY2N_COSFRAMES"))   // (debug: "i,j,..": those records' frames)
                    {
                        glm::mat4 f = proot->apxfRef[1] ? proot->apxfRef[1]->matWorld : glm::mat4(1.0f);
                        f[3] = glm::vec4(PosLoWorld(proot), 1.0f);
                        std::stringstream ss(szb);
                        for (std::string t; std::getline(ss, t, ',');)
                            if (const size_t i = size_t(std::atoi(t.c_str())); i < proot->axf.size() && proot->axf[i])
                            {
                                const glm::mat4 r = glm::inverse(f) * proot->axf[i]->matWorld;
                                std::printf("cosframe %d %zu: pos %.1f %.1f %.1f  x %.2f %.2f %.2f  y %.2f %.2f %.2f  z %.2f %.2f %.2f\n", proot->oid, i,
                                            r[3].x, r[3].y, r[3].z, r[0].x, r[0].y, r[0].z, r[1].x, r[1].y, r[1].z, r[2].x, r[2].y, r[2].z);
                            }
                    }
                }
            }
    }
    for (const auto& [proot, fRemote] : aroot)
    {
        const int ichar = IPaintCharOfOid(proot->oid);
        if (ichar < 0 || proot->gFade < 0.5f) continue;
        for (int s = 0; s < COSSLOT_Max; ++s)
            for (int iside = 1; iside >= (s == COSSLOT_Fists ? 0 : 1); --iside)   // (the fists: both hands)
        {
            ITEM* p = PitemOf(StrEquippedFor(ichar, fRemote, s));
            glm::mat4 mat;
            if (!p || p->avtx.empty() || !(p->grfChar >> ichar & 1) || !FAttachMatrix(proot, ichar, s, &mat, iside)) continue;
            UploadItem(*p);
            const glm::mat4 m = mat * p->matOffset;
            glUniformMatrix4fv(s_sh.Loc("matModel"), 1, GL_FALSE, glm::value_ptr(m));
            glUniform1i(s_sh.Loc("fTextured"), p->tex ? 1 : 0);
            glUniform4f(s_sh.Loc("tint"), 0, 0, 0, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, p->tex);
            glBindVertexArray(p->vao);
            glDrawArrays(GL_TRIANGLES, 0, GLsizei(p->avtx.size()));
        }
    }
    glBindVertexArray(0);
}
