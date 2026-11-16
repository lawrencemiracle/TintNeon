// TintNeon.asi - window tint + neon with an in-game menu, ASI only (no CLEO needed).
// plugin-sdk, Win32/x86.  NOT COMPILED OR TESTED - expect small API-name fixes on first build.
//
// Usage: in a tuning garage press [4]. The game's own menu panel opens (Window Tint / Neon).
//   Arrow keys scroll (vanilla), SHIFT selects/applies, ENTER (exit-vehicle button) goes back/closes.
//   Numpad 8 / Numpad 2 = move, Numpad 5 = apply, Numpad 0 (or same key again) = close.
// Config: TintNeon.ini next to the .asi (keys, prices, presets). Built-in defaults if missing.
// the project plugin-sdk generates may use a precompiled header; it must be the very first include
#define _CRT_SECURE_NO_WARNINGS
#if __has_include("stdafx.h")
#include "stdafx.h"
#endif
// plugin-sdk's injector.hpp forgets these standard headers on newer MSVC -> include them first
#include <type_traits>
#include <functional>
#include <utility>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <windows.h>
#include "plugin.h"
#include "common.h"
#include "CFont.h"
#include "CSprite2d.h"
#include "CVehicle.h"
#include "CVehicleModelInfo.h"
#include "CModelInfo.h"
#include "CShadows.h"
#include "CTimer.h"
#include "CCamera.h"
#include "CWorld.h"
#include "CPad.h"
#include "CText.h"
#include "CMessages.h"
#include "extensions/ScriptCommands.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace plugin;

// ============================ data ============================
struct Preset { std::string name; int r, g, b, a, mode, price; };
struct TintState { int r, g, b, a, slot; };
struct NeonState { int r, g, b, a, mode, slot; };

static std::vector<Preset> g_tintPresets, g_neonPresets;
static std::unordered_map<CVehicle*, TintState> g_tint;
static std::unordered_map<CVehicle*, NeonState> g_neon;

static int  g_keyMenu = '4';
static bool g_pricing = true, g_anywhere = false;

// ============================ config ============================
static std::string IniPath() {
    char p[MAX_PATH]; HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&IniPath, &h);
    GetModuleFileNameA(h, p, MAX_PATH);
    std::string s(p); s = s.substr(0, s.find_last_of("\\/") + 1);
    return s + "TintNeon.ini";
}
static void LoadPresets(const std::string& ini, const char* sec, bool neon, std::vector<Preset>& out) {
    std::vector<Preset> v;
    for (int i = 1; i <= 12; ++i) {
        char key[16], name[64];
        sprintf(key, "%d", i);
        GetPrivateProfileStringA(sec, key, "", name, sizeof name, ini.c_str());
        if (!name[0]) break;
        auto rd = [&](const char* suf, int def) {
            sprintf(key, "%d%s", i, suf);
            return (int)GetPrivateProfileIntA(sec, key, def, ini.c_str());
        };
        v.push_back({ name, rd("R", 0), rd("G", 0), rd("B", 0), rd("A", 0), neon ? rd("Mode", 0) : 0, rd("Price", 0) });
    }
    if (!v.empty()) out = v;
}
static int ParseKey(const std::string& ini, const char* k, int def) {
    char b[16]; GetPrivateProfileStringA("Settings", k, "", b, sizeof b, ini.c_str());
    if (!b[0]) return def;
    if (strlen(b) == 1) return toupper(b[0]);
    return (int)strtol(b, nullptr, 0);   // allow hex virtual-key codes, e.g. 0x68
}
static void LoadConfig() {
    g_tintPresets = {
        {"Stock",0,0,0,0,0,0},{"Light",0,0,0,90,0,200},{"Dark",0,0,0,160,0,400},
        {"Limo",0,0,0,220,0,600},{"Blue",20,40,120,150,0,500},{"Smoke Green",20,70,40,150,0,500} };
    g_neonPresets = {
        {"Stock",0,0,0,0,-2,0},{"Off",0,0,0,0,-1,0},{"White",255,255,255,200,0,800},
        {"Blue",0,80,255,200,0,800},{"Red",255,0,0,200,0,800},{"Green",0,255,60,200,0,800},
        {"Rainbow",255,255,255,200,1,2000},{"Match Body",255,255,255,200,3,1000} };
    std::string ini = IniPath();
    g_pricing  = GetPrivateProfileIntA("Settings", "EnablePricing", 1, ini.c_str()) != 0;
    g_anywhere = GetPrivateProfileIntA("Settings", "AllowAnywhere", 0, ini.c_str()) != 0;
    g_keyMenu  = ParseKey(ini, "KeyMenu", g_keyMenu);
    LoadPresets(ini, "Tint", false, g_tintPresets);
    LoadPresets(ini, "Neon", true,  g_neonPresets);
}

// ============================ glass tint ============================
struct Saved { RpMaterial* mat; RwRGBA col; };
static std::vector<Saved> g_saved;

static bool IsEditableKey(const RwRGBA& c) {
    return (c.red == 60  && c.green == 255 && c.blue == 0)   || (c.red == 255 && c.green == 0   && c.blue == 175) ||
           (c.red == 0   && c.green == 255 && c.blue == 255) || (c.red == 255 && c.green == 0   && c.blue == 255);
}
// Heuristic: translucent and not a body-colour key. Verify on your own car models.
static bool IsGlass(RpMaterial* m) { const RwRGBA* c = &m->color; return c->alpha < 255 && !IsEditableKey(*c); }

static RpMaterial* MatCB(RpMaterial* m, void* d) {
    if (!IsGlass(m)) return m;
    const TintState* t = (const TintState*)d;
    g_saved.push_back({ m, m->color });
    RwRGBA c = { (RwUInt8)t->r, (RwUInt8)t->g, (RwUInt8)t->b, (RwUInt8)t->a };
    m->color = c;
    return m;
}
static RpAtomic* AtomicCB(RpAtomic* a, void* d) {
    if (RpGeometry* g = RpAtomicGetGeometry(a)) RpGeometryForAllMaterials(g, MatCB, d);
    return a;
}
static void TintBefore(CVehicle* v) {
    auto it = g_tint.find(v);
    if (it == g_tint.end() || it->second.a <= 0 || !v->m_pRwClump) return;
    RpClumpForAllAtomics(v->m_pRwClump, AtomicCB, &it->second);   // materials are shared per model -> restore after
}
static void TintAfter(CVehicle*) {
    for (auto& s : g_saved) s.mat->color = s.col;
    g_saved.clear();
}

// ============================ neon ============================
static RwTexture* g_neonTex = nullptr;

// procedural glow texture: bright rounded-rectangle outline fading outwards/inwards (no TXD needed)
static RwTexture* MakeNeonTexture() {
    const int N = 128;
    RwRaster* r = RwRasterCreate(N, N, 32, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
    if (!r) return nullptr;
    RwUInt8* px = RwRasterLock(r, 0, rwRASTERLOCKWRITE);
    if (!px) { RwRasterDestroy(r); return nullptr; }
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
        float u = (x + 0.5f) / N * 2.f - 1.f, v = (y + 0.5f) / N * 2.f - 1.f;
        float hx = 0.62f, hy = 0.70f, rad = 0.22f;                      // box half-size + corner radius
        float qx = std::fabs(u) - hx + rad, qy = std::fabs(v) - hy + rad;
        float d = std::sqrt(std::max(qx,0.f)*std::max(qx,0.f) + std::max(qy,0.f)*std::max(qy,0.f))
                  + std::min(std::max(qx,qy),0.f) - rad;              // signed distance to rounded box
        float a = std::exp(-std::fabs(d) * 9.f);
        RwUInt8* p = px + (y * N + x) * 4;                             // BGRA
        p[0] = p[1] = p[2] = 255; p[3] = (RwUInt8)(std::clamp(a, 0.f, 1.f) * 255.f);
    }
    RwRasterUnlock(r);
    RwTexture* t = RwTextureCreate(r);
    if (t) { RwTextureSetFilterMode(t, rwFILTERLINEAR); RwTextureSetAddressing(t, rwTEXTUREADDRESSCLAMP); }
    return t;
}
static void Rainbow(float h, int& r, int& g, int& b) {
    r = (int)(255 * std::clamp(std::fabs(h*6.f-3.f)-1.f, 0.f, 1.f));
    g = (int)(255 * std::clamp(2.f-std::fabs(h*6.f-2.f), 0.f, 1.f));
    b = (int)(255 * std::clamp(2.f-std::fabs(h*6.f-4.f), 0.f, 1.f));
}
static void DrawNeon(CVehicle* v) {
    auto it = g_neon.find(v);
    if (it == g_neon.end() || it->second.mode < 0 || it->second.a <= 0 || !g_neonTex) return;
    if (v->m_fHealth <= 0.f || v->IsUpsideDown() || v->bSubmergedInWater) return;
    const NeonState& n = it->second;
    int r = n.r, g = n.g, b = n.b;
    float t = CTimer::m_snTimeInMilliseconds * 0.001f;
    if (n.mode == 1) Rainbow(std::fmod(t * 0.25f, 1.f), r, g, b);
    else if (n.mode == 3 || n.mode == 4) {                              // body colours
        int idx = (n.mode == 3) ? v->m_nPrimaryColor : v->m_nSecondaryColor;
        CRGBA c = CVehicleModelInfo::ms_vehicleColourTable[idx];
        r = c.r; g = c.g; b = c.b;
    }
    CVector d3 = v->GetPosition() - TheCamera.GetPosition();
    float dist = d3.Magnitude();
    if (dist > 60.f) return;
    float fade = dist > 42.f ? (60.f - dist) / 18.f : 1.f;

    CColModel* col = CModelInfo::GetModelInfo(v->m_nModelIndex)->m_pColModel;
    if (!col) return;
    CVector mn = col->m_boundBox.m_vecMin, mx = col->m_boundBox.m_vecMax;
    CMatrix& m = *(CMatrix*)v->m_matrix;
    CVector up = m.up; up.z = 0; up.Normalize();
    CVector rt = m.right; rt.z = 0; rt.Normalize();
    up = up * ((mx.y - mn.y) * 0.5f);  rt = rt * ((mx.x - mn.x) * 0.5f);
    CVector c = v->TransformFromObjectSpace(CVector((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, 0.f));
    CVector pos(c.x, c.y, v->GetPosition().z + 1.5f);
    CShadows::StoreShadowToBeRendered(2, g_neonTex, &pos, up.x, up.y, rt.x, rt.y,
        (short)(n.a * fade), (uint8_t)r, (uint8_t)g, (uint8_t)b, 6.0f, false, 1.0f, nullptr, true);
}

// ============================ apply ============================
static void ApplyTint(CVehicle* v, int row) {
    const Preset& p = g_tintPresets[row];
    g_tint[v] = { p.r, p.g, p.b, p.a, row };
}
static void ApplyNeon(CVehicle* v, int row) {
    const Preset& p = g_neonPresets[row];
    if (p.mode == -2) { g_neon.erase(v); return; }
    g_neon[v] = { p.r, p.g, p.b, p.a, p.mode, row };
}

// ============================ vanilla menu ============================
// Uses the game's own menu panels (same opcodes your SB handling script uses: create menu, set column,
// get selected, activate item).  The menu texts come from the GXT, so while the menu is open we temporarily
// point 28 unused frontend GXT entries (FEM_*) at our own strings and restore them on close.
static int  Money()         { return CWorld::Players[0].m_nMoney; }
static void SetMoney(int m) { CWorld::Players[0].m_nMoney = m; }

static const int NSLOT = 28;           // 0 title, 1 header "Variant", 2 header "Price", 3..14 rows, 15..26 prices
struct GxtSlot { char key[9]; uint32_t* offPtr; uint32_t origOff; char text[48]; };
static GxtSlot  g_slot[NSLOT];
static uint8_t* g_dataBase = nullptr;
static bool     g_gxtReady = false, g_gxtTried = false;

static bool InitGxt() {
    g_gxtTried = true;
    uint8_t* base = (uint8_t*)&TheText;
    if (IsBadReadPtr(base, 32)) return false;
    uint8_t* entries = *(uint8_t**)(base + 0);
    uint32_t count   = *(uint32_t*)(base + 4);
    if (!entries || IsBadReadPtr(entries, 12) || count < 300 || count > 1000000) return false;
    struct Lay { int off, key; } lays[] = { {0, 4}, {8, 0} };
    int dataOffs[] = { 8, 12, 16, 20, 24 };
    for (auto& lay : lays) for (int dOff : dataOffs) {
        uint8_t* data = *(uint8_t**)(base + dOff);
        if (!data || IsBadReadPtr(data, 16)) continue;
        int tested = 0, ok = 0;
        for (uint32_t i = count / 5; i < count && tested < 6; i += count / 9 + 1) {
            uint8_t* e = entries + (size_t)i * 12;
            char k[9]; memcpy(k, e + lay.key, 8); k[8] = 0;
            if (!k[0]) continue;
            uint32_t off = *(uint32_t*)(e + lay.off);
            const uint8_t* p = (const uint8_t*)TheText.Get(k);
            ++tested;
            if (p == data + off) ++ok;
        }
        if (tested < 4 || ok != tested) continue;
        // layout verified -> collect unused frontend keys
        int n = 0;
        for (uint32_t i = 0; i < count && n < NSLOT; ++i) {
            uint8_t* e = entries + (size_t)i * 12;
            if (memcmp(e + lay.key, "FEM_", 4) != 0) continue;
            GxtSlot& sl = g_slot[n];
            memcpy(sl.key, e + lay.key, 8); sl.key[8] = 0;
            sl.offPtr = (uint32_t*)(e + lay.off);
            sl.origOff = *sl.offPtr;
            sl.text[0] = 0;
            ++n;
        }
        if (n < NSLOT) return false;
        g_dataBase = data;
        return true;
    }
    return false;
}
static void SetText(int i, const char* t) {
    strncpy(g_slot[i].text, t, 47); g_slot[i].text[47] = 0;
    *g_slot[i].offPtr = (uint32_t)((uint8_t*)g_slot[i].text - g_dataBase);
}
static void RestoreTexts() { for (int i = 0; i < NSLOT; ++i) if (g_slot[i].offPtr) *g_slot[i].offPtr = g_slot[i].origOff; }

// ---- garage menu plumbing (same memory writes as the SB handling script) ----
static void WriteU8(uintptr_t a, uint8_t v)   { DWORD o; VirtualProtect((void*)a, 1, PAGE_EXECUTE_READWRITE, &o); *(uint8_t*)a = v;  VirtualProtect((void*)a, 1, o, &o); }
static void WriteU16(uintptr_t a, uint16_t v) { DWORD o; VirtualProtect((void*)a, 2, PAGE_EXECUTE_READWRITE, &o); *(uint16_t*)a = v; VirtualProtect((void*)a, 2, o, &o); }
static bool GarageMenuActive(CVehicle* v) { return v && (g_anywhere || *(uint8_t*)0xBA82E3 > 0); }

enum { ST_CLOSED, ST_MAIN, ST_TINT, ST_NEON };
static int  g_state = ST_CLOSED, g_panel = -1, g_rows = 0;
static bool g_leftHidden = false, g_enterPending = false, g_prevEnter = false, g_prevShift = false, g_prevOpen = false;

static bool EnterDown() {
    CPad* pad = CPad::GetPad(0);
    return (pad && pad->NewState.ButtonTriangle) || (GetAsyncKeyState(VK_RETURN) & 0x8000);
}
static int InstalledSlot(CVehicle* v, int state) {
    if (state == ST_TINT) { auto it = g_tint.find(v); return it == g_tint.end() ? 0 : it->second.slot; }
    auto it = g_neon.find(v); return it == g_neon.end() ? 0 : it->second.slot;
}
static void Notify(const char* t) { CMessages::AddMessageJumpQ((char*)t, 2000, 0, false); }

static void RefreshRows(CVehicle* v) {
    for (int r = 0; r < g_rows; ++r) Command<COMMAND_ACTIVATE_MENU_ITEM>(g_panel, r, 1);
    if (g_state == ST_TINT || g_state == ST_NEON) {
        int in = InstalledSlot(v, g_state);
        if (in >= 0 && in < g_rows) Command<COMMAND_ACTIVATE_MENU_ITEM>(g_panel, in, 0);
    }
}

static void DestroyPanel() {
    if (g_panel >= 0) { Command<COMMAND_DELETE_MENU>(g_panel); g_panel = -1; }
}
static void BuildPanel(int state, CVehicle* v) {
    DestroyPanel();
    for (int i = 0; i < NSLOT; ++i) SetText(i, "");
    const std::vector<Preset>* list = nullptr;
    if (state == ST_TINT) list = &g_tintPresets; else if (state == ST_NEON) list = &g_neonPresets;
    bool priceCol = (list != nullptr) && g_pricing;

    SetText(0, state == ST_MAIN ? "Tint & Neon" : state == ST_TINT ? "Window Tint" : "Neon");
    SetText(1, state == ST_MAIN ? "Menu" : "Variant");
    SetText(2, "Price");
    if (!list) { SetText(3, "Window Tint"); SetText(4, "Neon"); g_rows = 2; }
    else {
        g_rows = std::min<int>((int)list->size(), 12);
        for (int i = 0; i < g_rows; ++i) {
            SetText(3 + i, (*list)[i].name.c_str());
            char pr[16]; if ((*list)[i].price) sprintf(pr, "$%d", (*list)[i].price); else strcpy(pr, "Free");
            SetText(15 + i, pr);
        }
    }
    g_state = state;
    int cols = priceCol ? 2 : 1;
    Command<COMMAND_CREATE_MENU>((char*)g_slot[0].key, 25.0f, 145.0f, 156.0f, cols, 1, 1, 1, &g_panel);
    Command<COMMAND_SET_MENU_COLUMN>(g_panel, 0, (char*)g_slot[1].key,
        (char*)g_slot[3].key, (char*)g_slot[4].key, (char*)g_slot[5].key, (char*)g_slot[6].key, (char*)g_slot[7].key, (char*)g_slot[8].key,
        (char*)g_slot[9].key, (char*)g_slot[10].key, (char*)g_slot[11].key, (char*)g_slot[12].key, (char*)g_slot[13].key, (char*)g_slot[14].key);
    if (priceCol)
        Command<COMMAND_SET_MENU_COLUMN>(g_panel, 1, (char*)g_slot[2].key,
            (char*)g_slot[15].key, (char*)g_slot[16].key, (char*)g_slot[17].key, (char*)g_slot[18].key, (char*)g_slot[19].key, (char*)g_slot[20].key,
            (char*)g_slot[21].key, (char*)g_slot[22].key, (char*)g_slot[23].key, (char*)g_slot[24].key, (char*)g_slot[25].key, (char*)g_slot[26].key);

    WriteU8(0xBA82E2, 1);                                   // make our panel the active menu
    Command<COMMAND_SET_ACTIVE_MENU_ITEM>(g_panel, 0);
    uint32_t cur = *(uint32_t*)0xBA82DC;                    // tell the menu system how many rows it has
    if (cur) *(uint8_t*)(cur + 0x3F6) = (uint8_t)(g_rows - 1);
    if (!g_leftHidden && *(uint16_t*)0x582645 == 0xDB32) { WriteU16(0x582645, 0x01B3); g_leftHidden = true; }   // hide left (stock tuning) menu
    RefreshRows(v);
}
static void ClosePanel() {
    DestroyPanel();
    RestoreTexts();
    if (g_leftHidden) { WriteU16(0x582645, 0xDB32); g_leftHidden = false; }
    WriteU8(0xBA82E2, 0);                                   // back to the stock menu
    g_state = ST_CLOSED; g_enterPending = false;
}

static void Update() {
    CVehicle* v = FindPlayerVehicle(-1, false);
    if (GetForegroundWindow() != RsGlobal.ps->window) return;
    bool garage = GarageMenuActive(v);
    bool enter = EnterDown(), shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool open4 = (GetAsyncKeyState(g_keyMenu) & 0x8000) != 0;

    if (g_state == ST_CLOSED) {
        if (garage && open4 && !g_prevOpen) {
            if (!g_gxtTried) g_gxtReady = InitGxt();
            if (!g_gxtReady) Notify("~r~TintNeon: game text table not supported");
            else BuildPanel(ST_MAIN, v);
        }
    } else {
        if (!garage) { ClosePanel(); }
        else if (g_enterPending) {                           // act when ENTER is released (like the SB script)
            if (!enter) {
                g_enterPending = false;
                if (g_state == ST_MAIN) ClosePanel(); else BuildPanel(ST_MAIN, v);
            }
        }
        else if (enter && !g_prevEnter) g_enterPending = true;
        else if (shift && !g_prevShift) {
            int row = 0; Command<COMMAND_GET_MENU_ITEM_SELECTED>(g_panel, &row);
            if (row < 0) row = 0;
            if (g_state == ST_MAIN) BuildPanel(row == 0 ? ST_TINT : ST_NEON, v);
            else if (row < g_rows) {
                auto& list = (g_state == ST_TINT) ? g_tintPresets : g_neonPresets;
                if (row != InstalledSlot(v, g_state)) {
                    int price = g_pricing ? list[row].price : 0;
                    if (price > 0 && Money() < price) Notify("~r~Not enough money !~s~");
                    else {
                        if (price > 0) SetMoney(Money() - price);
                        if (g_state == ST_TINT) ApplyTint(v, row); else ApplyNeon(v, row);
                        RefreshRows(v);
                    }
                }
            }
        }
    }
    g_prevEnter = enter; g_prevShift = shift; g_prevOpen = open4;
}

// ============================ plugin ============================
class TintNeon {
public:
    TintNeon() {
        Events::initRwEvent += [] { LoadConfig(); g_neonTex = MakeNeonTexture(); };
        Events::gameProcessEvent += [] { Update(); };
        Events::vehicleRenderEvent.before += TintBefore;
        Events::vehicleRenderEvent.after  += [](CVehicle* v) { TintAfter(v); DrawNeon(v); };
        Events::vehicleDtorEvent += [](CVehicle* v) { g_tint.erase(v); g_neon.erase(v); };
    }
} g_tintNeon;
