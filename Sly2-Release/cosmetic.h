#pragma once

// Cosmetics (QoL, SafeHouse-Coop): items worn by Sly, Bentley and Murray (hats, backpacks, a cane in Sly's hand),
// original models loaded from the mods folder (Mods/Cosmetics/<item>/item.ini + a Wavefront .obj with its .mtl colours
// / texture). Each item names the characters that may wear it, where it is pinned (the head, the back or the hand)
// with an offset, and what of the character it hides (Bentley's hat, Sly's cane). The wardrobe (po.cpp) equips them;
// cosmetics.ini in the user folder keeps the choice, and co-op sends it (the other player's copy wears theirs: the
// same item installed on both sides).

#include <string>
#include <vector>

struct ALO;
struct SCENE;
struct CAMERA;
struct RENDEROPTS;

enum COSSLOT { COSSLOT_Head = 0, COSSLOT_Back = 1, COSSLOT_Hand = 2, COSSLOT_Max = 3 };

// The items installed for a character (0..2) in a slot, by id (the folder's name), in order.
std::vector<std::string> AstrCosmetics(int ichar, int islot);
// An item's display name (its id when it names none).
std::string StrCosmeticName(const std::string& id);
const char* SzCosSlot(int islot);

// The local player's equipped items (empty: none), changed by the wardrobe; saved in cosmetics.ini.
std::string StrEquipped(int ichar, int islot);
void SetEquipped(int ichar, int islot, const std::string& id);
void SaveCosmetics();
// The other player's (co-op), and forgetting them.
void SetRemoteEquipped(int ichar, int islot, const std::string& id);
void ClearRemoteCosmetics();
std::string StrLocalEquipped(int ichar, int islot);   // (what co-op sends)

// The scene's placement (scenesw.cpp): this group of the object hidden by an equipped item.
bool FCosmeticHidesGlob(const ALO* palo, size_t iglob);
// The renderer (render.cpp): the frame's items drawn (after its passes, the frame's own view).
void DrawCosmetics(const float* pmatWorldToClip, const float* pposCamera, const float* prgbaFog);
