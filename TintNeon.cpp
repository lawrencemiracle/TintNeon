// TintNeon.asi - window tint + neon with an in-game menu, ASI only (no CLEO needed).
// plugin-sdk, Win32/x86.  NOT COMPILED OR TESTED - expect small API-name fixes on first build.
//
// Usage: in a tuning garage (or anywhere if AllowAnywhere=1) press [4] tint menu, [5] neon menu.
//   Numpad 8 / Numpad 2 = move, Numpad 5 = apply, Numpad 0 (or same key again) = close.
// Config: TintNeon.ini next to the .asi (keys, prices, presets). Built-in defaults if missing.
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

static int  g_keyTint = '4', g_keyNeon = '5';
static int  g_keyUp = VK_NUMPAD8, g_keyDown = VK_NUMPAD2, g_keyApply = VK_NUMPAD5, g_keyClose = VK_NUMPAD0;
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
    g_keyTint  = ParseKey(ini, "KeyTint", g_keyTint);   g_keyNeon  = ParseKey(ini, "KeyNeon", g_keyNeon);
    g_keyUp    = ParseKey(ini, "KeyUp", g_keyUp);       g_keyDown  = ParseKey(ini, "KeyDown", g_keyDown);
    g_keyApply = ParseKey(ini, "KeyApply", g_keyApply); g_keyClose = ParseKey(ini, "KeyClose", g_keyClose);
    LoadPresets(ini, "Tint", false, g_tintPresets);
    LoadPresets(ini, "Neon", true,  g_neonPresets);
}

// ============================ glass tint ============================
struct Saved { RwMaterial* mat; RwRGBA col; };
static std::vector<Saved> g_saved;

static bool IsEditableKey(const RwRGBA& c) {
    return (c.red == 60  && c.green == 255 && c.blue == 0)   || (c.red == 255 && c.green == 0   && c.blue == 175) ||
           (c.red == 0   && c.green == 255 && c.blue == 255) || (c.red == 255 && c.green == 0   && c.blue == 255);
}
// Heuristic: translucent and not a body-colour key. Verify on your own car models.
static bool IsGlass(RwMaterial* m) { const RwRGBA* c = RwMaterialGetColor(m); return c->alpha < 255 && !IsEditableKey(*c); }

static RpMaterial* MatCB(RpMaterial* m, void* d) {
    if (!IsGlass(m)) return m;
    const TintState* t = (const TintState*)d;
    g_saved.push_back({ m, *RwMaterialGetColor(m) });
    RwRGBA c = { (RwUInt8)t->r, (RwUInt8)t->g, (RwUInt8)t->b, (RwUInt8)t->a };
    RpMaterialSetColor(m, &c);
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
    for (auto& s : g_saved) RpMaterialSetColor(s.mat, &s.col);
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

// ============================ menu ============================
enum { MENU_NONE, MENU_TINT, MENU_NEON };
static int  g_menu = MENU_NONE, g_row = 0;
static char g_msg[64] = ""; static unsigned g_msgUntil = 0;
static bool g_prev[256];

static bool Pressed(int vk) {                       // edge-triggered key press
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0, was = g_prev[vk & 255];
    g_prev[vk & 255] = now;
    return now && !was;
}
static bool InGarageMenu(CVehicle* v) {
    if (!v) return false;
    if (g_anywhere) return true;
    return *(uint8_t*)0xBA82E3 > 0;                 // game's "menu in use" flag (same one your script polls)
}
static int  Money()            { return CWorld::Players[0].m_nMoney; }
static void SetMoney(int m)    { CWorld::Players[0].m_nMoney = m; }
static void Msg(const char* s) { strncpy(g_msg, s, 63); g_msgUntil = CTimer::m_snTimeInMilliseconds + 2000; }

static void Update() {
    CVehicle* v = FindPlayerVehicle(-1, false);
    if (GetForegroundWindow() != RsGlobal.ps->window) return;
    if (!v || !InGarageMenu(v)) { g_menu = MENU_NONE; for (int k : {g_keyTint,g_keyNeon,g_keyUp,g_keyDown,g_keyApply,g_keyClose}) g_prev[k&255] = (GetAsyncKeyState(k)&0x8000)!=0; return; }

    int kT = Pressed(g_keyTint), kN = Pressed(g_keyNeon);
    if (kT) { g_menu = (g_menu == MENU_TINT) ? MENU_NONE : MENU_TINT; g_row = 0; }
    if (kN) { g_menu = (g_menu == MENU_NEON) ? MENU_NONE : MENU_NEON; g_row = 0; }
    if (g_menu == MENU_NONE) { Pressed(g_keyUp); Pressed(g_keyDown); Pressed(g_keyApply); Pressed(g_keyClose); return; }

    auto& list = (g_menu == MENU_TINT) ? g_tintPresets : g_neonPresets;
    int n = (int)list.size();
    if (Pressed(g_keyUp))   g_row = (g_row + n - 1) % n;
    if (Pressed(g_keyDown)) g_row = (g_row + 1) % n;
    if (Pressed(g_keyClose)) g_menu = MENU_NONE;
    if (Pressed(g_keyApply)) {
        int installed = (g_menu == MENU_TINT) ? (g_tint.count(v) ? g_tint[v].slot : 0)
                                              : (g_neon.count(v) ? g_neon[v].slot : 0);
        if (g_row == installed) { Msg("Already installed"); return; }
        int price = g_pricing ? list[g_row].price : 0;
        if (price > 0) {
            if (Money() < price) { Msg("Not enough money!"); return; }
            SetMoney(Money() - price);
        }
        if (g_menu == MENU_TINT) ApplyTint(v, g_row); else ApplyNeon(v, g_row);
        Msg("Installed");
    }
}

static void DrawMenu() {
    if (g_menu == MENU_NONE) return;
    CVehicle* v = FindPlayerVehicle(-1, false);
    if (!v) return;
    auto& list = (g_menu == MENU_TINT) ? g_tintPresets : g_neonPresets;
    int installed = (g_menu == MENU_TINT) ? (g_tint.count(v) ? g_tint[v].slot : 0)
                                          : (g_neon.count(v) ? g_neon[v].slot : 0);
    float sx = RsGlobal.maximumWidth / 640.f, sy = RsGlobal.maximumHeight / 448.f;
    float x = 25 * sx, y = 145 * sy, w = 200 * sx, rowH = 17 * sy;
    float h = rowH * (list.size() + 2) + 6 * sy;

    CSprite2d::DrawRect(CRect(x, y, x + w, y + h), CRGBA(0, 0, 0, 170));
    CFont::SetBackground(false, false);
    CFont::SetFontStyle(FONT_SUBTITLES);
    CFont::SetProportional(true);
    CFont::SetScale(0.55f * sx, 1.0f * sy);
    CFont::SetColor(CRGBA(255, 220, 90, 255));
    CFont::SetOrientation(ALIGN_LEFT);
    CFont::PrintString(x + 6 * sx, y + 4 * sy, (char*)(g_menu == MENU_TINT ? "WINDOW TINT" : "NEON"));

    for (size_t i = 0; i < list.size(); ++i) {
        float ry = y + 4 * sy + rowH * (i + 1);
        if ((int)i == g_row) CSprite2d::DrawRect(CRect(x + 2 * sx, ry, x + w - 2 * sx, ry + rowH - 1 * sy), CRGBA(200, 40, 40, 150));
        CFont::SetColor((int)i == installed ? CRGBA(130, 130, 130, 255) : CRGBA(255, 255, 255, 255));
        CFont::SetOrientation(ALIGN_LEFT);
        CFont::PrintString(x + 6 * sx, ry + 1 * sy, (char*)list[i].name.c_str());
        if (g_pricing) {
            char pr[16]; if (list[i].price) sprintf(pr, "$%d", list[i].price); else strcpy(pr, "Free");
            CFont::SetOrientation(ALIGN_RIGHT);
            CFont::SetRightJustifyWrap(0.f);
            CFont::PrintString(x + w - 6 * sx, ry + 1 * sy, pr);
        }
    }
    if (g_msgUntil > CTimer::m_snTimeInMilliseconds) {
        CFont::SetOrientation(ALIGN_LEFT);
        CFont::SetColor(CRGBA(255, 160, 160, 255));
        CFont::PrintString(x + 6 * sx, y + h + 3 * sy, g_msg);
    }
}

// ============================ plugin ============================
class TintNeon {
public:
    TintNeon() {
        Events::initRwEvent += [] { LoadConfig(); g_neonTex = MakeNeonTexture(); };
        Events::gameProcessEvent += [] { Update(); };
        Events::drawHudEvent += [] { DrawMenu(); };
        Events::vehicleRenderEvent.before += TintBefore;
        Events::vehicleRenderEvent.after  += [](CVehicle* v) { TintAfter(v); DrawNeon(v); };
        Events::vehicleDtorEvent += [](CVehicle* v) { g_tint.erase(v); g_neon.erase(v); };
    }
} g_tintNeon;
