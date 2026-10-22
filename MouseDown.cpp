// Hold a key to move the mouse. Build from an x64 Native Tools prompt:
//   cl /std:c++17 /utf-8 /O2 /MT /EHsc MouseDown.cpp /link /MANIFEST:EMBED
#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "winmm")
#pragma comment(lib, "winhttp")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

// ---------- settings ----------

const int MAX_PROFILES = 100, NAME_LEN = 20, OFF_BITS = 10, ACCENTS = 5; // ponytail: fixed array keeps the per-tick config copy allocation-free

struct Profile {
    wchar_t name[NAME_LEN + 4] = L"Default";
    int holdKey = VK_XBUTTON1, requireKey = 0, hotkey = 0, mode = 0; // mode 0 = hold, 1 = toggle, 2 = tap
    float vy = 300, vx = 0, jitter = 0, randMin = 15, randMax = 30, startDelay = 0, ramp = 0, smoothing = 0, duration = 0, tickHz = 500;
    float vy2 = 300, vx2 = 0, at2 = 0, vy3 = 300, vx3 = 0, at3 = 0; // stages 2/3: pull from atN ms on (0 = off)
    float growEvery = 0, growY = 0, growX = 0; // every growEvery ms held, pull gains growY/growX px/s (0 = off)
    float sway = 0, swayMs = 500; // horizontal sway: ±sway px/s, one full cycle every swayMs (0 = off)
    // auto spam: hold spamTrigger (+ spamRequire if set) to repeatedly tap spamKey
    bool spamOn = false, spamDelayOff = false;
    bool spamPull = false; // pull runs the whole time auto spam is clicking, in any activation mode
    int spamTrigger = 0, spamRequire = 0, spamKey = 0;
    float spamRate = 10, spamDelay = 0, spamJitter = 0; // spamDelay: ms held before clicking; spamJitter: ±% per interval
    bool off[OFF_BITS] = {}; // per-feature switch (FieldDef::bit); off acts like the value is 0, keeping the value
    // group ("parent profile"): pulls with the profile in its active slot; each slot has a key that picks it
    bool group = false;
    int child[2] = {-1, -1}, childKey[2] = {}, childSel = 0;
};
struct Config {
    Profile p[MAX_PROFILES];
    int n = 1, cur = 0, toggleKey = VK_F8, nextKey = 0, prevKey = 0;
    int pauseKey = 0; // held: pull and spam are suspended
    int lastKey = 0;  // jump back to the profile used before this one
    int quickKey = 0; // held: 1-9 picks that profile, the mouse wheel steps through them
    bool hud = false; // small always-on-top readout of the active profile
    int swapKey = 0;          // flips the active group between its two slots
    bool scrollSwap = false;  // each mouse wheel notch flips the active group's slot (the game still gets the scroll)
    bool slotReset = false;   // switching to a group starts it in slot 1, not where it was left
    int listShow = 0;         // profile list: 0 everything, 1 only normal profiles, 2 only groups
    bool switchGroups = false; // wheel, quick switch and next/prev move between groups only
    bool sound = false; // beep on on/off and profile switches
    int accent = 0; // UI colour, index into accents[]
    int wheelKey = 0; // hold to show the profile wheel
    float sens = 100;
    bool enabled = true, onTop = false, notify = true;
    wchar_t port[16] = L""; // serial COM port of the HID board; empty = inject with SendInput
};

struct FieldDef { const wchar_t *key, *label, *fmt, *zero; float Profile::*f; float lo, hi, step; int bit = -1; float Config::*g = nullptr; };
static const FieldDef fields[] = {
    {L"vy", L"Vertical pull", L"%.0f px/s", nullptr, &Profile::vy, -1000, 1000, 1},
    {L"vx", L"Horizontal pull", L"%.0f px/s", nullptr, &Profile::vx, -500, 500, 1},
    {L"vy2", L"Vertical pull", L"%.0f px/s", nullptr, &Profile::vy2, -1000, 1000, 1},
    {L"vx2", L"Horizontal pull", L"%.0f px/s", nullptr, &Profile::vx2, -500, 500, 1},
    {L"at2", L"Starts after", L"%.0f ms", L"Off", &Profile::at2, 0, 3000, 10, 6},
    {L"vy3", L"Vertical pull", L"%.0f px/s", nullptr, &Profile::vy3, -1000, 1000, 1},
    {L"vx3", L"Horizontal pull", L"%.0f px/s", nullptr, &Profile::vx3, -500, 500, 1},
    {L"at3", L"Starts after", L"%.0f ms", L"Off", &Profile::at3, 0, 3000, 10, 7},
    {L"jitter", L"Jitter", L"±%.0f%%", L"Off", &Profile::jitter, 0, 50, 1, 0},
    // random offset per axis, re-rolled per activation: size between min and max, random direction
    {L"randMin", L"Offset min", L"±%.0f", L"Off", &Profile::randMin, 0, 200, 1, 1},
    {L"randMax", L"Offset max", L"±%.0f", L"Off", &Profile::randMax, 0, 200, 1, 1},
    {L"startDelay", L"Start delay", L"%.0f ms", L"None", &Profile::startDelay, 0, 1000, 5, 2},
    {L"ramp", L"Ramp-up", L"%.0f ms", L"Instant", &Profile::ramp, 0, 2000, 10, 3},
    {L"smoothing", L"Smoothing", L"%.0f%%", L"Off", &Profile::smoothing, 0, 100, 1, 4},
    {L"duration", L"Pull duration", L"%.0f ms", L"Unlimited", &Profile::duration, 0, 5000, 50, 5},
    {L"tickHz", L"Update rate", L"%.0f Hz", nullptr, &Profile::tickHz, 100, 1000, 10},
};
static const FieldDef growDefs[] = { // own tab in the Pull card, so kept out of fields[]'s layout
    {L"growEvery", L"Grow every", L"%.0f ms", L"Off", &Profile::growEvery, 0, 2000, 10, 8},
    {L"growY", L"Vertical per step", L"%+.0f px/s", nullptr, &Profile::growY, -200, 200, 1},
    {L"growX", L"Horizontal per step", L"%+.0f px/s", nullptr, &Profile::growX, -200, 200, 1},
};
static const FieldDef swayDefs[] = { // own pair, kept out of fields[] so its indices don't shift
    {L"sway", L"Sway", L"±%.0f px/s", L"Off", &Profile::sway, 0, 400, 1, 9},
    {L"swayMs", L"Sway period", L"%.0f ms", nullptr, &Profile::swayMs, 100, 3000, 10},
};
static const FieldDef sensDef = {L"sens", L"Sensitivity", L"%.0f%%", nullptr, nullptr, 10, 300, 5, -1, &Config::sens};
static const FieldDef spamDefs[] = { // auto spam's sliders; saved with their own code, so not in ForFields
    {L"spamRate", L"Rate", L"%.0f /s", nullptr, &Profile::spamRate, 1, 50, 1},
    {L"spamDelay", L"Delay", L"%.0f ms", L"Instant", &Profile::spamDelay, 0, 2000, 10},
    {L"spamJitter", L"Rate jitter", L"±%.0f%%", L"Off", &Profile::spamJitter, 0, 50, 1},
};
const int STAGE_FIELDS = 8, PULL_FIELDS = 11; // fields[] layout: 8 stage fields, jitter, offset min/max, then Timing
const double SPAM_HOLD_MS = 15; // how long each spam click stays pressed (shorter at high rates)
const double TAP_MS = 150;    // tap-mode pull length when Pull duration is Unlimited
const double SWAY_TAU = 6.283185307179586; // 2π, for the horizontal sway oscillation

// Visit every profile slider field.
template <class F> static void ForFields(F fn) {
    for (auto& d : fields) fn(d);
    for (auto& d : growDefs) fn(d);
    for (auto& d : swayDefs) fn(d);
}

// Visit every profile field that has an on/off switch.
template <class F> static void ForSwitches(F fn) {
    ForFields([&](const FieldDef& d) { if (d.bit >= 0) fn(d); });
}

static float Default(const FieldDef& d) {
    static const Profile dp{};
    static const Config dc{};
    return d.f ? dp.*d.f : d.g ? dc.*d.g : d.lo;
}

// The normal profile a group's slot runs, or -1 if the slot is empty, out of range or (no nesting) a group.
static int SlotProfile(const Config& c, const Profile& g, int slot) {
    int ch = g.child[slot & 1];
    return ch >= 0 && ch < c.n && !c.p[ch].group ? ch : -1;
}

// What the wheel, quick switch and next/prev move between, in list order: the groups when switching is set
// to groups only (and there are some), otherwise every profile. Returns how many.
static int SwitchTargets(const Config& c, int* out) {
    int m = 0;
    if (c.switchGroups)
        for (int i = 0; i < c.n; i++)
            if (c.p[i].group) out[m++] = i;
    if (!m)
        for (int i = 0; i < c.n; i++) out[m++] = i;
    return m;
}

// The profile list's rows, in list order: everything, only normal profiles or only groups. The active
// profile always shows, so the one being edited never disappears. Returns how many.
static int VisibleProfiles(const Config& c, int* out) {
    int m = 0;
    for (int i = 0; i < c.n; i++)
        if (c.listShow == 0 || (c.listShow == 1) != c.p[i].group || i == c.cur) out[m++] = i;
    return m;
}

// The target `step` places on from profile `from`, wrapping. From one that isn't a target, +1 is the first.
static int StepTarget(const Config& c, int from, int step) {
    int t[MAX_PROFILES], m = SwitchTargets(c, t), pos = -1;
    for (int i = 0; i < m; i++)
        if (t[i] == from) pos = i;
    if (pos < 0) pos = step > 0 ? -1 : 0;
    return t[((pos + step) % m + m) % m];
}

// Keep group slots pointing at the same profiles when the list changes. map: old index -> new (-1 = gone).
template <class F> static void RemapSlots(Config& c, F map) {
    for (int i = 0; i < c.n; i++)
        for (int& ch : c.p[i].child)
            if (ch >= 0) ch = map(ch);
}

static void InsertProfile(Config& c, int at, Profile p) { // p by value: it may be a copy of one that moves
    std::copy_backward(c.p + at, c.p + c.n, c.p + c.n + 1);
    c.p[at] = p;
    c.n++;
    RemapSlots(c, [at](int i) { return i >= at ? i + 1 : i; });
}

static void DeleteProfile(Config& c, int at) {
    std::copy(c.p + at + 1, c.p + c.n, c.p + at);
    c.n--;
    RemapSlots(c, [at](int i) { return i == at ? -1 : i > at ? i - 1 : i; });
}

static void SwapProfiles(Config& c, int a, int b) {
    std::swap(c.p[a], c.p[b]);
    RemapSlots(c, [a, b](int i) { return i == a ? b : i == b ? a : i; });
}

// The profile as the engine sees it: switched-off features read as 0.
static Profile Effective(const Profile& p) {
    Profile e = p;
    ForSwitches([&](const FieldDef& d) { if (p.off[d.bit]) e.*d.f = 0; });
    return e;
}

static std::mutex mu; // guards cfg between the UI thread and the engine thread
static Config cfg;
static std::atomic<bool> capturing{false}, pulling{false}, pausedNow{false}; // capturing: UI is grabbing keys, engine ignores them
static wchar_t ini[MAX_PATH];

static void LoadSettings() {
    auto get = [](const wchar_t* sec, const wchar_t* k, double def, double lo, double hi) {
        wchar_t buf[32];
        GetPrivateProfileStringW(sec, k, L"", buf, 32, ini);
        double v = *buf ? _wtof(buf) : def;
        return std::clamp(v, lo, hi);
    };
    cfg.n = (int)get(L"general", L"profiles", 1, 1, MAX_PROFILES);
    cfg.cur = std::min((int)get(L"general", L"profile", 0, 0, MAX_PROFILES - 1), cfg.n - 1);
    cfg.toggleKey = (int)get(L"general", L"toggleKey", cfg.toggleKey, 0, 254);
    cfg.nextKey = (int)get(L"general", L"nextKey", cfg.nextKey, 0, 254);
    cfg.prevKey = (int)get(L"general", L"prevKey", cfg.prevKey, 0, 254);
    cfg.sens = (float)get(L"general", L"sens", cfg.sens, sensDef.lo, sensDef.hi);
    cfg.enabled = get(L"general", L"enabled", 1, 0, 1) != 0;
    cfg.onTop = get(L"general", L"onTop", 0, 0, 1) != 0;
    cfg.notify = get(L"general", L"notify", 1, 0, 1) != 0;
    cfg.wheelKey = (int)get(L"general", L"wheelKey", cfg.wheelKey, 0, 254);
    cfg.pauseKey = (int)get(L"general", L"pauseKey", cfg.pauseKey, 0, 254);
    cfg.sound = get(L"general", L"sound", 0, 0, 1) != 0;
    cfg.accent = (int)get(L"general", L"accent", 0, 0, ACCENTS - 1);
    cfg.lastKey = (int)get(L"general", L"lastKey", cfg.lastKey, 0, 254);
    cfg.quickKey = (int)get(L"general", L"quickKey", cfg.quickKey, 0, 254);
    cfg.hud = get(L"general", L"hud", 0, 0, 1) != 0;
    cfg.swapKey = (int)get(L"general", L"swapKey", cfg.swapKey, 0, 254);
    cfg.scrollSwap = get(L"general", L"scrollSwap", 0, 0, 1) != 0;
    cfg.slotReset = get(L"general", L"slotReset", 0, 0, 1) != 0;
    cfg.listShow = (int)get(L"general", L"listShow", get(L"general", L"listGroups", 0, 0, 1) ? 2 : 0, 0, 2);
    cfg.switchGroups = get(L"general", L"switchGroups", 0, 0, 1) != 0;
    GetPrivateProfileStringW(L"general", L"port", L"", cfg.port, 16, ini);
    for (int i = 0; i < cfg.n; i++) {
        wchar_t sec[16], def[16];
        swprintf_s(sec, L"profile%d", i + 1);
        swprintf_s(def, i ? L"Profile %d" : L"Default", i + 1);
        Profile& p = cfg.p[i];
        GetPrivateProfileStringW(sec, L"name", def, p.name, NAME_LEN + 1, ini);
        p.holdKey = (int)get(sec, L"holdKey", p.holdKey, 0, 254);
        p.requireKey = (int)get(sec, L"requireKey", p.requireKey, 0, 254);
        p.hotkey = (int)get(sec, L"hotkey", p.hotkey, 0, 254);
        p.mode = (int)get(sec, L"mode", p.mode, 0, 2);
        p.group = get(sec, L"group", 0, 0, 1) != 0;
        p.child[0] = (int)get(sec, L"slot1", -1, -1, MAX_PROFILES - 1);
        p.child[1] = (int)get(sec, L"slot2", -1, -1, MAX_PROFILES - 1);
        p.childKey[0] = (int)get(sec, L"slot1Key", 0, 0, 254);
        p.childKey[1] = (int)get(sec, L"slot2Key", 0, 0, 254);
        p.childSel = (int)get(sec, L"slot", 0, 0, 1);
        // auto spam used to be one setting for all profiles: a profile without its own starts from that
        auto spam = [&](const wchar_t* k, double def, double lo, double hi) { return get(sec, k, get(L"general", k, def, lo, hi), lo, hi); };
        p.spamOn = get(sec, L"spamOn", 0, 0, 1) != 0; // off unless this profile turned it on
        p.spamPull = spam(L"spamPull", 0, 0, 1) != 0;
        p.spamDelayOff = spam(L"spamDelayOff", 0, 0, 1) != 0;
        p.spamTrigger = (int)spam(L"spamTrigger", 0, 0, 254);
        p.spamRequire = (int)spam(L"spamRequire", 0, 0, 254);
        p.spamKey = (int)spam(L"spamKey", 0, 0, 254);
        for (auto& d : spamDefs) p.*d.f = (float)spam(d.key, p.*d.f, d.lo, d.hi);
        for (auto& d : fields) p.*d.f = (float)get(sec, d.key, p.*d.f, d.lo, d.hi);
        for (auto& d : growDefs) p.*d.f = (float)get(sec, d.key, p.*d.f, d.lo, d.hi);
        for (auto& d : swayDefs) p.*d.f = (float)get(sec, d.key, p.*d.f, d.lo, d.hi);
        ForSwitches([&](const FieldDef& d) {
            p.off[d.bit] = get(sec, (std::wstring(d.key) + L"Off").c_str(), 0, 0, 1) != 0;
        });
    }
}

// Writes the whole ini in one go. WritePrivateProfileString rewrites the file on every call, which
// with ~50 keys per profile stalled the UI on each click. Temp file + rename, so a crash can't truncate it.
static void Save() {
    Config c;
    { std::lock_guard<std::mutex> l(mu); c = cfg; }
    std::wstring out = L"\xFEFF"; // UTF-16 BOM makes the ini APIs keep non-ASCII profile names
    auto sec = [&](const wchar_t* name) { out += L"["; out += name; out += L"]\r\n"; };
    auto str = [&](const wchar_t* k, const wchar_t* v) { out += k; out += L"="; out += v; out += L"\r\n"; };
    auto put = [&](const wchar_t* k, double v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%g", v);
        str(k, buf);
    };
    sec(L"general");
    put(L"profiles", c.n);
    put(L"profile", c.cur);
    put(L"toggleKey", c.toggleKey);
    put(L"nextKey", c.nextKey);
    put(L"prevKey", c.prevKey);
    put(L"sens", c.sens);
    put(L"enabled", c.enabled);
    put(L"onTop", c.onTop);
    put(L"notify", c.notify);
    put(L"wheelKey", c.wheelKey);
    put(L"pauseKey", c.pauseKey);
    put(L"sound", c.sound);
    put(L"accent", c.accent);
    put(L"lastKey", c.lastKey);
    put(L"quickKey", c.quickKey);
    put(L"hud", c.hud);
    put(L"swapKey", c.swapKey);
    put(L"scrollSwap", c.scrollSwap);
    put(L"slotReset", c.slotReset);
    put(L"listShow", c.listShow);
    put(L"switchGroups", c.switchGroups);
    str(L"port", c.port);
    for (int i = 0; i < c.n; i++) {
        wchar_t name[16];
        swprintf_s(name, L"profile%d", i + 1);
        sec(name);
        const Profile& p = c.p[i];
        str(L"name", p.name);
        put(L"holdKey", p.holdKey);
        put(L"requireKey", p.requireKey);
        put(L"hotkey", p.hotkey);
        put(L"mode", p.mode);
        if (p.group) {
            put(L"group", 1);
            put(L"slot1", p.child[0]);
            put(L"slot2", p.child[1]);
            put(L"slot1Key", p.childKey[0]);
            put(L"slot2Key", p.childKey[1]);
            put(L"slot", p.childSel);
        }
        ForFields([&](const FieldDef& d) { put(d.key, p.*d.f); });
        ForSwitches([&](const FieldDef& d) { put((std::wstring(d.key) + L"Off").c_str(), p.off[d.bit]); });
        put(L"spamOn", p.spamOn);
        put(L"spamPull", p.spamPull);
        put(L"spamDelayOff", p.spamDelayOff);
        put(L"spamTrigger", p.spamTrigger);
        put(L"spamRequire", p.spamRequire);
        put(L"spamKey", p.spamKey);
        for (auto& d : spamDefs) put(d.key, p.*d.f);
    }
    std::wstring tmp = std::wstring(ini) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    bool ok = WriteFile(h, out.data(), (DWORD)(out.size() * sizeof(wchar_t)), &wr, nullptr);
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), ini, MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

// One-line text form of a profile, for sharing through the clipboard. The name goes last so it may
// contain any character; the profile hotkey stays out, since it belongs to this machine's setup.
static std::wstring ExportProfile(const Profile& p) {
    std::wstring s = L"MouseDown";
    auto add = [&](const std::wstring& k, double v) {
        wchar_t b[32];
        swprintf_s(b, L"=%g", v);
        s += L";" + k + b;
    };
    add(L"holdKey", p.holdKey);
    add(L"requireKey", p.requireKey);
    add(L"mode", p.mode);
    if (p.group) { // slots are list positions, so they only mean the same thing pasted on this PC
        add(L"group", 1);
        add(L"slot1", p.child[0]);
        add(L"slot2", p.child[1]);
        add(L"slot1Key", p.childKey[0]);
        add(L"slot2Key", p.childKey[1]);
    }
    ForFields([&](const FieldDef& d) { add(d.key, p.*d.f); });
    ForSwitches([&](const FieldDef& d) { add(std::wstring(d.key) + L"Off", p.off[d.bit]); });
    add(L"spamOn", p.spamOn);
    add(L"spamPull", p.spamPull);
    add(L"spamDelayOff", p.spamDelayOff);
    add(L"spamTrigger", p.spamTrigger);
    add(L"spamRequire", p.spamRequire);
    add(L"spamKey", p.spamKey);
    for (auto& d : spamDefs) add(d.key, p.*d.f);
    return s + L";name=" + p.name;
}

// Overwrites p from ExportProfile text (keeping p's hotkey); false if it isn't one.
static bool ImportProfile(const wchar_t* s, Profile& p) {
    const wchar_t tag[] = L"MouseDown;";
    if (wcsncmp(s, tag, std::size(tag) - 1)) return false;
    Profile q = p;
    for (const wchar_t* t = s + std::size(tag) - 1; *t;) {
        if (!wcsncmp(t, L"name=", 5)) {
            wcsncpy_s(q.name, t + 5, NAME_LEN);
            break;
        }
        const wchar_t* end = wcschr(t, L';');
        if (!end) end = t + wcslen(t);
        std::wstring kv(t, end);
        size_t eq = kv.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring k = kv.substr(0, eq);
            double v = _wtof(kv.c_str() + eq + 1);
            if (k == L"holdKey") q.holdKey = std::clamp((int)v, 0, 254);
            if (k == L"requireKey") q.requireKey = std::clamp((int)v, 0, 254);
            if (k == L"mode") q.mode = std::clamp((int)v, 0, 2);
            if (k == L"group") q.group = v != 0;
            if (k == L"slot1") q.child[0] = std::clamp((int)v, -1, MAX_PROFILES - 1);
            if (k == L"slot2") q.child[1] = std::clamp((int)v, -1, MAX_PROFILES - 1);
            if (k == L"slot1Key") q.childKey[0] = std::clamp((int)v, 0, 254);
            if (k == L"slot2Key") q.childKey[1] = std::clamp((int)v, 0, 254);
            if (k == L"spamOn") q.spamOn = v != 0;
            if (k == L"spamPull") q.spamPull = v != 0;
            if (k == L"spamDelayOff") q.spamDelayOff = v != 0;
            if (k == L"spamTrigger") q.spamTrigger = std::clamp((int)v, 0, 254);
            if (k == L"spamRequire") q.spamRequire = std::clamp((int)v, 0, 254);
            if (k == L"spamKey") q.spamKey = std::clamp((int)v, 0, 254);
            for (auto& d : spamDefs)
                if (k == d.key) q.*d.f = (float)std::clamp(v, (double)d.lo, (double)d.hi);
            ForFields([&](const FieldDef& d) {
                if (k == d.key) q.*d.f = (float)std::clamp(v, (double)d.lo, (double)d.hi);
                if (d.bit >= 0 && k == std::wstring(d.key) + L"Off") q.off[d.bit] = v != 0;
            });
        }
        t = *end ? end + 1 : end;
    }
    p = q;
    return true;
}

// ---------- engine ----------

enum { EV_ON = 1, EV_OFF, EV_PROFILE, EV_SLOT }; // wParam of WM_APP + 1

// Physical key/button state from low-level hooks, which skip injected input. Our own spam clicks
// would otherwise read as the user letting go (e.g. spamming Left Mouse while Left Mouse is held).
static std::atomic<bool> phys[256];
static std::atomic<bool> hooked;

// Quick switching. The hooks only flag what was asked for (they must return fast); the engine applies it.
static std::atomic<int> quickKeyNow;          // engine keeps this at cfg.quickKey (0 while capturing keys)
static std::atomic<int> quickJump{-1};        // profile index picked with quick key + 1-9
static std::atomic<int> quickStep, wheelStep; // mouse wheel steps: quick key held / profile wheel open
static std::atomic<bool> wheelShown, wheelClick; // profile wheel is open / it got a left click
static std::atomic<int> gameScroll;              // whole wheel notches that went on to the game (slot swapping)
static bool Down(int vk);

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    static bool eatLUp; // the left press went to the wheel, so its release must too
    auto* m = (MSLLHOOKSTRUCT*)lp;
    if (code == HC_ACTION && !(m->flags & LLMHF_INJECTED)) {
        if (wp == WM_LBUTTONDOWN && wheelShown) { // picks on the wheel, never fires in the game
            wheelClick = eatLUp = true;
            return 1;
        }
        if (wp == WM_LBUTTONUP && eatLUp) {
            eatLUp = false;
            return 1;
        }
        if (wp == WM_MOUSEWHEEL) {
            int dir = (short)HIWORD(m->mouseData) > 0 ? -1 : 1; // scroll down = next profile
            if (wheelShown) {
                wheelStep += dir;
                return 1;
            }
            int q = quickKeyNow;
            if (q && Down(q)) {
                quickStep += dir;
                return 1;
            }
            static int part; // fine-step wheels send fractions of a notch
            for (part += std::abs((short)HIWORD(m->mouseData)); part >= WHEEL_DELTA; part -= WHEEL_DELTA) gameScroll++;
        }
        switch (wp) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: phys[VK_LBUTTON] = wp == WM_LBUTTONDOWN; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: phys[VK_RBUTTON] = wp == WM_RBUTTONDOWN; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: phys[VK_MBUTTON] = wp == WM_MBUTTONDOWN; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            phys[HIWORD(m->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2] = wp == WM_XBUTTONDOWN; break;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    static bool eaten[256]; // digits that went to quick switch: their release is eaten too
    auto* k = (KBDLLHOOKSTRUCT*)lp;
    if (code == HC_ACTION && !(k->flags & LLKHF_INJECTED)) {
        int vk = k->vkCode & 255, q = quickKeyNow;
        bool down = !(k->flags & LLKHF_UP);
        int digit = vk >= '1' && vk <= '9' ? vk - '1' : vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9 ? vk - VK_NUMPAD1 : -1;
        if (down && digit >= 0 && q && q != vk && Down(q)) {
            quickJump = digit;
            eaten[vk] = true;
            return 1;
        }
        if (!down && eaten[vk]) {
            eaten[vk] = false;
            return 1;
        }
        phys[vk] = down;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Own thread so a busy UI thread can never stall system-wide input.
static void HookThread() {
    hooked = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0) &&
             SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
}

static bool Down(int vk) {
    if (!vk) return false;
    if (!hooked) return GetAsyncKeyState(vk) & 0x8000;
    switch (vk) { // LL hooks report the left/right variants only
    case VK_SHIFT: return phys[VK_LSHIFT] || phys[VK_RSHIFT];
    case VK_CONTROL: return phys[VK_LCONTROL] || phys[VK_RCONTROL];
    case VK_MENU: return phys[VK_LMENU] || phys[VK_RMENU];
    }
    return phys[vk];
}

static bool Extended(int vk) { // keys whose scan code needs the E0 prefix
    return (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_RCONTROL ||
           vk == VK_RMENU || vk == VK_DIVIDE;
}

// Press or release vk (mouse button or key) via SendInput.
static void SendKey(int vk, bool down) {
    INPUT ev{};
    DWORD dn = 0, up = 0;
    switch (vk) {
    case VK_LBUTTON: dn = MOUSEEVENTF_LEFTDOWN; up = MOUSEEVENTF_LEFTUP; break;
    case VK_RBUTTON: dn = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; break;
    case VK_MBUTTON: dn = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; break;
    case VK_XBUTTON1: case VK_XBUTTON2:
        dn = MOUSEEVENTF_XDOWN; up = MOUSEEVENTF_XUP; ev.mi.mouseData = vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2; break;
    }
    if (dn) {
        ev.type = INPUT_MOUSE;
        ev.mi.dwFlags = down ? dn : up;
    } else { // scan codes, since many games read those rather than virtual keys
        ev.type = INPUT_KEYBOARD;
        ev.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        ev.ki.dwFlags = KEYEVENTF_SCANCODE | (Extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    }
    SendInput(1, &ev, sizeof ev);
}

// Open the HID board's serial port. \\.\ prefix is needed for COM10+.
static HANDLE OpenSerial(const wchar_t* port) {
    if (!*port) return INVALID_HANDLE_VALUE;
    wchar_t path[32];
    swprintf_s(path, L"\\\\.\\%s", port);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    DCB dcb{sizeof dcb};
    GetCommState(h, &dcb);
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(h, &dcb);
    COMMTIMEOUTS to{}; // all zero: writes return at once, never block the engine tick
    SetCommTimeouts(h, &to);
    return h;
}

// Target speed (px/s) t ms after activation; scale is the global sensitivity.
static void Target(const Profile& p, double scale, double t, double& tx, double& ty) {
    tx = ty = 0;
    if (t < p.startDelay) return;
    double te = t - p.startDelay;
    if (p.duration > 0 && te > p.duration) return;
    double f = p.ramp > 0 ? std::min(1.0, te / p.ramp) : 1.0;
    f = f * f * (3 - 2 * f) * scale; // smoothstep: eases in and out of the ramp
    float vx = p.vx, vy = p.vy, from = 0; // the enabled stage that started last wins
    if (p.at2 > 0 && te >= p.at2) vx = p.vx2, vy = p.vy2, from = p.at2;
    if (p.at3 > 0 && te >= p.at3 && p.at3 >= from) vx = p.vx3, vy = p.vy3;
    if (p.growEvery > 0) { // stepped growth for as long as the pull stays active
        double k = std::floor(te / p.growEvery);
        vx += (float)(k * p.growX);
        vy += (float)(k * p.growY);
    }
    double sway = p.swayMs > 0 && p.sway != 0 ? p.sway * std::sin(SWAY_TAU * te / p.swayMs) : 0;
    tx = (vx + sway) * f;
    ty = vy * f;
}

// Smoothing = exponential follow of the target speed; 100% is a 300 ms time constant.
static double Follow(double v, double target, const Profile& p, double dtMs) {
    if (p.smoothing <= 0) return target;
    return v + (target - v) * (1 - std::exp(-dtMs / (p.smoothing * 3.0)));
}

// Activation state machine, one call per engine tick. combo = hold key + required key both down.
// Returns whether the pull is active; restart = start the pull over (new t0, offsets, growth).
struct Trigger { bool comboWas = false, latched = false; };
static bool Step(Trigger& t, int mode, bool combo, bool req, bool spamPulls, bool ok, bool running, bool& restart) {
    restart = false;
    if (mode == 1 && !req) t.latched = false; // a toggle only lives while the required key is held
    if (combo && !t.comboWas && ok) {         // combo completes, whichever key went down last
        if (mode == 1) t.latched = !t.latched;
        // each tap restarts the pull, unless spam is already driving one (keeps growth/stages going)
        if (mode == 2) t.latched = true, restart = !(running && spamPulls);
    }
    t.comboWas = combo;
    // tap runs its full length even if the keys are let go; linked spam pulls for as long as it clicks
    return ok && (spamPulls || (mode == 0 ? combo : mode == 1 ? t.latched && req : t.latched));
}

static void Engine(HWND hwnd) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> uni(-1, 1);
    LARGE_INTEGER freq, t0{}, last{}, now;
    QueryPerformanceFrequency(&freq);
    auto ms = [&](LONGLONG ticks) { return ticks * 1000.0 / freq.QuadPart; };

    HANDLE serial = INVALID_HANDLE_VALUE;
    bool wasActive = false, toggleWas = false, nextWas = false, prevWas = false, backWas = false;
    int seenCur = -1, backCur = -1; // profile in use before the current one, for the last-profile key
    Trigger trig;
    bool hotWas[MAX_PROFILES] = {};
    int lastCur = -1, lastRun = -1;
    bool slotWas[2] = {}, swapWas = false;
    double vx = 0, vy = 0, ax = 0, ay = 0, jx = 1, jy = 1, nextJitter = 0, ox = 0, oy = 0, spamAt = 0, spamUpAt = 0;
    int spamVk = 0;         // key currently pressed by spam (0 = none)
    int spamLast = 0;       // the key spam was clicking last tick, to hand back to the game when it stops
    bool spamWas = false;
    double spamSince = -1; // when the spam keys went down (-1 = not held)
    Config c;
    { std::lock_guard<std::mutex> l(mu); c = cfg; }
    for (;;) {
        { // never wait on the UI while it draws a frame: a missed tick stutters the pull; last tick's copy will do
            std::unique_lock<std::mutex> l(mu, std::try_to_lock);
            if (l) c = cfg;
        }
        bool cap = capturing;
        if (c.cur != seenCur) { // however it changed (hotkey, wheel, a click in the UI)
            if (seenCur >= 0) backCur = seenCur;
            seenCur = c.cur;
        }
        quickKeyNow = cap ? 0 : c.quickKey;

        // global hotkeys: on/off, next / previous / last profile, quick switch, per-profile jump
        auto edge = [&](int vk, bool& was) { bool d = Down(vk), e = d && !was && !cap; was = d; return e; };
        int event = 0;
        if (edge(c.toggleKey, toggleWas)) {
            c.enabled = !c.enabled;
            event = c.enabled ? EV_ON : EV_OFF;
        }
        int newCur = c.cur;
        if (edge(c.nextKey, nextWas)) newCur = StepTarget(c, c.cur, 1);
        if (edge(c.prevKey, prevWas)) newCur = StepTarget(c, c.cur, -1);
        if (edge(c.lastKey, backWas) && backCur >= 0 && backCur < c.n) newCur = backCur;
        int jump = quickJump.exchange(-1), step = quickStep.exchange(0);
        if (jump >= 0) {
            int t[MAX_PROFILES], m = SwitchTargets(c, t);
            if (jump < m) newCur = t[jump];
        }
        if (step) newCur = StepTarget(c, newCur, step);
        for (int i = 0; i < c.n; i++)
            if (edge(c.p[i].hotkey, hotWas[i])) newCur = i;
        if (newCur != c.cur) {
            c.cur = newCur;
            event = EV_PROFILE;
        }
        if (event) {
            std::lock_guard<std::mutex> l(mu);
            cfg.enabled = c.enabled;
            cfg.cur = c.cur;
            PostMessageW(hwnd, WM_APP + 1, event, 0);
        }

        int run = c.cur; // the profile that pulls: a group hands this to the one in its active slot
        bool swapped = edge(c.swapKey, swapWas), scrolled = gameScroll.exchange(0) % 2 && c.scrollSwap && !cap;
        if (c.p[c.cur].group) {
            Profile& g = c.p[c.cur];
            int sel = c.cur != lastCur && c.slotReset ? 0 : g.childSel; // just switched to it: maybe back to slot 1
            bool keyed = false; // a slot change the player asked for, worth a toast
            for (int s = 0; s < 2; s++)
                if (edge(g.childKey[s], slotWas[s])) sel = s, keyed = true;
            if (swapped != scrolled) sel ^= 1, keyed = true; // both in one tick cancel out
            if (sel != g.childSel) {
                g.childSel = sel;
                std::lock_guard<std::mutex> l(mu);
                if (cfg.cur == c.cur && cfg.p[c.cur].group) cfg.p[c.cur].childSel = sel;
                if (keyed) PostMessageW(hwnd, WM_APP + 1, EV_SLOT, 0);
                else PostMessageW(hwnd, WM_APP, 0, 0);
            }
            run = SlotProfile(c, g, sel);
        }
        const Profile p = Effective(c.p[run >= 0 ? run : c.cur]);
        if (c.cur != lastCur || run != lastRun || !c.enabled) trig.latched = false, wasActive = false; // a switch starts fresh
        lastCur = c.cur;
        lastRun = run;
        QueryPerformanceCounter(&now);
        double nowMs = ms(now.QuadPart);

        // spam first, so a linked pull starts on the same tick as the first click
        bool paused = Down(c.pauseKey) && !cap;
        if (paused != pausedNow) {
            pausedNow = paused;
            PostMessageW(hwnd, WM_APP, 0, 0);
        }
        bool spamKeys = c.enabled && p.spamOn && run >= 0 && !cap && !paused && p.spamKey && Down(p.spamTrigger) &&
                        (!p.spamRequire || Down(p.spamRequire));
        if (!spamKeys) spamSince = -1;
        else if (spamSince < 0) spamSince = nowMs;
        bool spam = spamKeys && nowMs - spamSince >= (p.spamDelayOff ? 0 : p.spamDelay); // keys must be held spamDelay ms first
        bool spamPulls = spam && p.spamPull;

        bool req = !p.requireKey || Down(p.requireKey), restart;
        bool active = Step(trig, p.mode, Down(p.holdKey) && req, req, spamPulls, c.enabled && !cap && !paused && run >= 0, wasActive,
                           restart);
        if (restart) wasActive = false;
        if (active != pulling) {
            pulling = active;
            PostMessageW(hwnd, WM_APP, 0, 0);
        }

        if (!active) {
            wasActive = false;
        } else {
            if (!wasActive) {
                wasActive = true;
                t0 = last = now;
                vx = vy = ax = ay = 0;
                nextJitter = 0;
                auto offset = [&] {
                    double lo = std::min(p.randMin, p.randMax), hi = std::max(p.randMin, p.randMax);
                    return (uni(rng) < 0 ? -1 : 1) * (lo + (hi - lo) * (uni(rng) + 1) / 2);
                };
                ox = offset();
                oy = offset();
                if (serial == INVALID_HANDLE_VALUE) serial = OpenSerial(c.port); // (re)connect per activation
            }
            double tm = ms(now.QuadPart - t0.QuadPart), dt = ms(now.QuadPart - last.QuadPart);
            last = now;
            if (p.mode == 2 && tm >= p.startDelay + (p.duration > 0 ? p.duration : TAP_MS)) trig.latched = false; // tap done
            if (tm >= nextJitter) { // new random strength every 50 ms; per-tick noise would just average out
                jx = 1 + p.jitter / 100 * uni(rng);
                jy = 1 + p.jitter / 100 * uni(rng);
                nextJitter = tm + 50;
            }
            double tx, ty;
            Profile q = p;
            q.vx += (float)ox;
            q.vy += (float)oy;
            q.vx2 += (float)ox;
            q.vy2 += (float)oy;
            q.vx3 += (float)ox;
            q.vy3 += (float)oy;
            if (p.mode == 2 && spamPulls) q.duration = 0; // Tap: duration is the tap length, spam keeps it going
            Target(q, c.sens / 100, tm, tx, ty);
            vx = Follow(vx, tx * jx, p, dt);
            vy = Follow(vy, ty * jy, p, dt);
            ax += vx * dt / 1000; // keep the sub-pixel remainder so slow speeds still move
            ay += vy * dt / 1000;
            int ix = std::clamp((int)ax, -127, 127), iy = std::clamp((int)ay, -127, 127); // one HID report
            if (ix || iy) {
                ax -= ix;
                ay -= iy;
                if (serial != INVALID_HANDLE_VALUE) { // real USB mouse: the game can't tell it from hardware
                    unsigned char pkt[3] = {0x80, (unsigned char)(char)ix, (unsigned char)(char)iy};
                    DWORD wr;
                    if (!WriteFile(serial, pkt, 3, &wr, nullptr)) { CloseHandle(serial); serial = INVALID_HANDLE_VALUE; }
                } else {
                    INPUT in{}; // no board: fall back to injection (desktop + games that don't filter it)
                    in.type = INPUT_MOUSE;
                    in.mi.dx = ix;
                    in.mi.dy = iy;
                    in.mi.dwFlags = MOUSEEVENTF_MOVE; // relative, works with games that lock the cursor
                    SendInput(1, &in, sizeof in);
                }
            }
        }

        // spam: click spamKey at spamRate while spamTrigger is held. Each click is held briefly,
        // since games that poll button state can miss a press and release sent together.
        if (spamVk && (nowMs >= spamUpAt || !spam)) {
            SendKey(spamVk, false);
            spamVk = 0;
        }
        if (spam && !spamVk && nowMs >= spamAt) {
            double period = 1000 / p.spamRate * (1 + p.spamJitter / 100 * uni(rng));
            spamVk = p.spamKey;
            SendKey(spamVk, true);
            spamUpAt = nowMs + std::min(SPAM_HOLD_MS, period / 2);
            spamAt = nowMs + period;
        }
        if (!spam) spamAt = 0; // first click fires right on press
        if (spamWas && !spam && spamLast && Down(spamLast))
            SendKey(spamLast, true); // still physically held: hand the button back down to the game
        spamWas = spam;
        if (spam) spamLast = p.spamKey;

        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(10'000'000 / p.tickHz); // 100 ns units, negative = relative
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }
}

// ---------- updates ----------

// The app updates itself from the repo's main branch. Files it downloads and writes itself carry no
// "downloaded from the internet" mark, so new versions don't need unblocking the way a browser download does.
#define UPDATE_REPO L"pokeneed3/Testing"
static const wchar_t* const updateFiles[] = {L"MouseDown.cpp", L"engine_check.cpp", L"mouse_hid.ino"}; // besides the exe
enum { UPD_IDLE, UPD_CHECKING, UPD_LATEST, UPD_READY, UPD_INSTALLING, UPD_FAILED };
static std::atomic<int> updState;
static std::string updExe;  // the newer exe, once updState is UPD_READY
static std::wstring updSha; // the commit it came from

static bool HttpGetVia(DWORD access, const wchar_t* host, const std::wstring& path, std::string& out) {
    out.clear();
    HINTERNET s = WinHttpOpen(L"MouseDown-updater", access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : nullptr;
    DWORD status = 0, sz = sizeof status, got;
    bool ok = r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr) &&
              WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                  &status, &sz, WINHTTP_NO_HEADER_INDEX) && status == 200;
    char buf[16384];
    while (ok && (ok = WinHttpReadData(r, buf, sizeof buf, &got) != FALSE) && got) out.append(buf, got); // 0 bytes = done
    ok = ok && !out.empty();
    for (HINTERNET h : {r, c, s}) if (h) WinHttpCloseHandle(h);
    return ok;
}

// HTTPS GET into out: the system's automatic proxy setup first, then the plain WinHTTP proxy setting.
static bool HttpGet(const wchar_t* host, const std::wstring& path, std::string& out) {
    return HttpGetVia(4 /*WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY*/, host, path, out) ||
           HttpGetVia(WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, host, path, out);
}

static std::wstring SelfPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

static bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER n;
    DWORD got = 0;
    bool ok = GetFileSizeEx(h, &n) && n.QuadPart < (1 << 30);
    if (ok) {
        out.resize((size_t)n.QuadPart);
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

static bool WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr) && wr == data.size();
    CloseHandle(h);
    return ok;
}

// Worker thread: find main's latest commit and fetch the exe from exactly that commit (raw.githubusercontent
// caches a branch's files for minutes, a commit's never go stale). Newer = differs from the running exe.
static bool FindUpdate() {
    updState = UPD_CHECKING;
    std::string api, exe, self;
    size_t at;
    if (!HttpGet(L"api.github.com", L"/repos/" UPDATE_REPO L"/commits/main", api) ||
        (at = api.find("\"sha\":\"")) == std::string::npos || api.size() < at + 47) {
        updState = UPD_FAILED;
        return false;
    }
    std::wstring sha(api.begin() + at + 7, api.begin() + at + 47);
    if (!HttpGet(L"raw.githubusercontent.com", L"/" UPDATE_REPO L"/" + sha + L"/MouseDown.exe", exe) || exe.size() < 4096 ||
        exe[0] != 'M' || exe[1] != 'Z') {
        updState = UPD_FAILED;
        return false;
    }
    if (ReadAll(SelfPath(), self) && self == exe) {
        updState = UPD_LATEST;
        return false;
    }
    updExe = std::move(exe);
    updSha = sha;
    updState = UPD_READY;
    return true;
}

// Worker thread: put the new files in place, then ask the UI to save and restart. A running exe can't be
// overwritten but can be renamed, so it moves aside to .old (deleted by the next start). settings.ini is never touched.
static void InstallUpdate(HWND w) {
    updState = UPD_INSTALLING;
    std::wstring self = SelfPath(), dir = self.substr(0, self.rfind(L'\\') + 1);
    std::string data;
    for (const wchar_t* f : updateFiles) // sources are a convenience: a missing one doesn't stop the update
        if (HttpGet(L"raw.githubusercontent.com", L"/" UPDATE_REPO L"/" + updSha + L"/" + f, data)) WriteAll(dir + f, data);
    std::wstring fresh = self + L".new", old = self + L".old";
    bool ok = WriteAll(fresh, updExe) && MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (ok && !MoveFileExW(fresh.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING); // put the running one back
        ok = false;
    }
    DeleteFileW(fresh.c_str());
    if (!ok) {
        updState = UPD_FAILED;
        return;
    }
    PostMessageW(w, WM_APP + 2, 0, 0);
}

// Update button: install what a check already found, otherwise check and install if there is something newer.
static void StartUpdate(HWND w) {
    int st = updState;
    if (st == UPD_CHECKING || st == UPD_INSTALLING) return;
    updState = UPD_CHECKING; // shows at once, and a second click can't start another
    std::thread([w, st] {
        if (st == UPD_READY || FindUpdate()) InstallUpdate(w);
        PostMessageW(w, WM_APP, 0, 0);
    }).detach();
}

// ---------- UI ----------

enum : UINT32 {
    BG = 0x0E1014, CARD = 0x161920, BORDER = 0x252A35, TEXT = 0xE8EAF0, MUTED = 0x868D9C, DISABLED = 0x4A5060,
    GREEN = 0x3DDC97, TEAL = 0x38BDF8, TRACK = 0x2A2F3B, ORANGE = 0xF59E0B,
    BTN = 0x1D212A, BTN_HOVER = 0x272C38, RED = 0xE5484D, RED_SOFT = 0x3A1F24, WHITE = 0xFFFFFF,
};
static const UINT32 accents[ACCENTS][2] = { // accent, soft tint for text on it
    {0x7C6CFF, 0xD8D4FF}, {0x3B82F6, 0xBFDBFE}, {0xDB2777, 0xFBCFE8}, {0xEA580C, 0xFED7AA}, {0x059669, 0xA7F3D0},
};
static UINT32 ACCENT = accents[0][0], ACCENT_SOFT = accents[0][1]; // set from cfg.accent each frame
const float WIN_W = 1260, WIN_H = 830, HEADER = 56;
const D2D1_RECT_F PILL = {132, 17, 216, 39}, UPDATE_BTN = {WIN_W - 236, 13, WIN_W - 108, 43}, TABS = {236, 13, 530, 43};

static HWND hwnd;
static ID2D1Factory* d2d;
static IDWriteFactory* dw;
static ID2D1HwndRenderTarget* hrt;   // main window
static ID2D1DCRenderTarget* dcrt;     // overlays (wheel, toast)
static ID2D1RenderTarget* rt;         // whichever one is drawing now
static ID2D1SolidColorBrush *br, *hbr, *obr;
static HWND wheelWnd, toastWnd, hudWnd;
static IDWriteTextFormat *fTitle, *fSection, *fLabel, *fValue, *fSmall, *fPill;

static struct { float mx = -1, my = -1, px = -1, wheel = 0; bool down = false, pressed = false, rpressed = false, dbl = false; } in; // px: x at the last press
static void* active;        // slider being dragged
static int* bindTarget;     // key slot waiting for a key press
static wchar_t* editTarget; // text field being typed into (profile name or COM port)
static int editMax;         // its capacity, minus the null
static wchar_t numBuf[16];  // slider value being typed (editTarget == numBuf)
static float* numTarget;    // the slider value it will be written to
static bool numSeen, numCancel;
static bool bindArmed, saveReq, onTopApplied;
static LPCWSTR cursor = IDC_ARROW;
static int headerCmd, profileCmd; // run after the frame so nothing shifts mid-draw

static void SyncCapture() { capturing = editTarget || bindTarget; }

static std::wstring KeyName(int vk) {
    static const wchar_t* mouse[] = {L"None", L"Left Mouse", L"Right Mouse", L"Cancel", L"Middle Mouse", L"Mouse 4", L"Mouse 5"};
    if (vk >= 0 && vk <= 6) return mouse[vk];
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1); // F13+ have no key-name text
    LONG sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    if (Extended(vk)) sc |= 1 << 24; // otherwise arrows etc. come back as numpad names
    wchar_t buf[64] = {};
    if (!GetKeyNameTextW(sc, buf, 64)) swprintf_s(buf, L"VK 0x%02X", vk);
    return buf;
}

static D2D1_RECT_F R(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
static bool In(D2D1_RECT_F r) { return in.mx >= r.left && in.mx < r.right && in.my >= r.top && in.my < r.bottom; }
static void Color(UINT32 c, float a = 1) { br->SetColor(D2D1::ColorF(c, a)); }
static void Fill(D2D1_RECT_F r, float rad, UINT32 c, float a = 1) { Color(c, a); rt->FillRoundedRectangle({r, rad, rad}, br); }
static void Stroke(D2D1_RECT_F r, float rad, UINT32 c, float w = 1) { // inset half a pixel so 1px lines stay crisp
    Color(c);
    rt->DrawRoundedRectangle({{r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f}, rad, rad}, br, w);
}
static void Circle(float x, float y, float rad, UINT32 c) { Color(c); rt->FillEllipse({{x, y}, rad, rad}, br); }
static void Line(float x1, float y1, float x2, float y2, UINT32 c, float w = 1, float a = 1) { Color(c, a); rt->DrawLine({x1, y1}, {x2, y2}, br, w); }
static void Text(const wchar_t* s, D2D1_RECT_F r, IDWriteTextFormat* f, UINT32 c,
                 DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
    f->SetTextAlignment(al);
    Color(c);
    rt->DrawText(s, (UINT32)wcslen(s), f, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static void CopyText(const std::wstring& s) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        CopyMemory(GlobalLock(g), s.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

static std::wstring PasteText() {
    std::wstring s;
    if (!OpenClipboard(hwnd)) return s;
    if (HANDLE g = GetClipboardData(CF_UNICODETEXT))
        if (auto* t = (const wchar_t*)GlobalLock(g)) {
            s = t;
            GlobalUnlock(g);
        }
    CloseClipboard();
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

// ---------- text editing (name fields and typed slider values) ----------

static int caret, anchor;  // caret and the other end of the selection, as indexes into editTarget
static ULONGLONG caretAt;  // last caret move: the caret shows solid right after one, then blinks
static bool selecting;     // a mouse drag is extending the selection

static bool CaretVisible() { return editTarget && (GetTickCount64() - caretAt) % 1060 < 530; }
static bool WordCh(wchar_t c) { return iswalnum(c) || c == L'_'; }
static int WordLeft(const wchar_t* s, int i) {
    while (i > 0 && !WordCh(s[i - 1])) i--;
    while (i > 0 && WordCh(s[i - 1])) i--;
    return i;
}
static int WordRight(const wchar_t* s, int i, int n) {
    while (i < n && !WordCh(s[i])) i++;
    while (i < n && WordCh(s[i])) i++;
    return i;
}

static void SetSel(int a, int c) { // anchor, caret
    int n = (int)wcslen(editTarget);
    anchor = std::clamp(a, 0, n);
    caret = std::clamp(c, 0, n);
    caretAt = GetTickCount64();
}

// Replace [a, b) of the text being edited with ins (filtered to what the field accepts, cut to fit).
static void Replace(int a, int b, std::wstring ins) {
    std::wstring s = editTarget;
    a = std::clamp(a, 0, (int)s.size());
    b = std::clamp(b, a, (int)s.size());
    std::wstring t;
    for (wchar_t ch : ins)
        if (ch >= 32 && ch != 127 && (editTarget != numBuf || iswdigit(ch) || ch == '-' || ch == '.')) t += ch;
    if (t.empty() && !ins.empty()) return; // nothing usable typed: keep the selection
    t.resize(std::min(t.size(), (size_t)std::max(0, editMax - ((int)s.size() - (b - a)))));
    s.replace(a, b - a, t);
    wcscpy_s(editTarget, editMax + 1, s.c_str());
    SetSel(a + (int)t.size(), a + (int)t.size());
}

static float TextWidth(const wchar_t* s, int n) {
    IDWriteTextLayout* tl;
    DWRITE_TEXT_METRICS m;
    float w = 0;
    if (SUCCEEDED(dw->CreateTextLayout(s, n, fValue, 10000, 100, &tl))) {
        if (SUCCEEDED(tl->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        tl->Release();
    }
    return w;
}

// Screen x of caret position i, for the text being edited drawn in r.
static float CaretX(D2D1_RECT_F r, DWRITE_TEXT_ALIGNMENT al, int i) {
    float x = r.left + TextWidth(editTarget, i);
    if (al == DWRITE_TEXT_ALIGNMENT_TRAILING) x += r.right - r.left - TextWidth(editTarget, (int)wcslen(editTarget));
    return x;
}

static int HitPos(D2D1_RECT_F r, DWRITE_TEXT_ALIGNMENT al, float x) { // nearest caret position to x
    int n = (int)wcslen(editTarget), best = 0;
    float bestD = 1e9f;
    for (int i = 0; i <= n; i++) {
        float d = std::fabs(CaretX(r, al, i) - x);
        if (d < bestD) bestD = d, best = i;
    }
    return best;
}

// Mouse on the field being edited: click places the caret, drag or shift+click selects, double-click
// selects a word. hover = the click landed on this field.
static void EditMouse(D2D1_RECT_F r, DWRITE_TEXT_ALIGNMENT al, bool hover) {
    if (in.pressed && hover) {
        int p = HitPos(r, al, in.px);
        if (in.dbl) {
            int a = p, b = p, n = (int)wcslen(editTarget);
            while (a > 0 && WordCh(editTarget[a - 1])) a--;
            while (b < n && WordCh(editTarget[b])) b++;
            SetSel(a, b);
        } else { // the caret goes to where the mouse is now: a fast drag may be over (or done) by this frame
            SetSel(GetKeyState(VK_SHIFT) < 0 ? anchor : p, HitPos(r, al, in.mx));
            selecting = true;
        }
    } else if (selecting && in.down) {
        SetSel(anchor, HitPos(r, al, in.mx));
    }
    if (!in.down) selecting = false;
}

// The field being edited: text, selection highlight and blinking caret.
static void DrawEdit(D2D1_RECT_F r, DWRITE_TEXT_ALIGNMENT al, UINT32 color) {
    int n = (int)wcslen(editTarget), a = std::clamp(std::min(caret, anchor), 0, n), b = std::clamp(std::max(caret, anchor), 0, n);
    float cy = (r.top + r.bottom) / 2;
    if (a != b) {
        float x1 = CaretX(r, al, a), x2 = CaretX(r, al, b);
        Fill({x1, cy - 9, x2, cy + 9}, 2, ACCENT, .35f);
    }
    Text(editTarget, r, fValue, color, al);
    if (CaretVisible()) {
        float cx = std::min(CaretX(r, al, std::clamp(caret, 0, n)) + (al == DWRITE_TEXT_ALIGNMENT_TRAILING ? 0 : 1), r.right + 2);
        Line(cx, cy - 8, cx, cy + 8, ACCENT, 1.5f);
    }
}

// Keys that aren't characters: caret movement, delete and the clipboard. True if the text changed.
static bool EditKey(int vk) {
    wchar_t* s = editTarget;
    int n = (int)wcslen(s), a = std::min(caret, anchor), b = std::max(caret, anchor);
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    auto move = [&](int to) { SetSel(shift ? anchor : to, to); };
    switch (vk) {
    case VK_LEFT:
        if (a != b && !shift) move(a);
        else move(ctrl ? WordLeft(s, caret) : caret - 1);
        break;
    case VK_RIGHT:
        if (a != b && !shift) move(b);
        else move(ctrl ? WordRight(s, caret, n) : caret + 1);
        break;
    case VK_HOME: move(0); break;
    case VK_END: move(n); break;
    case VK_DELETE:
        if (a == b) b = ctrl ? WordRight(s, caret, n) : std::min(n, caret + 1);
        Replace(a, b, L"");
        return true;
    case 'A':
        if (ctrl) SetSel(0, n);
        break;
    case 'C': case 'X':
        if (!ctrl || a == b) break;
        CopyText(std::wstring(s + a, s + b));
        if (vk == 'X') {
            Replace(a, b, L"");
            return true;
        }
        break;
    case 'V':
        if (!ctrl) break;
        {
            std::wstring t = PasteText();
            Replace(a, b, t.substr(0, t.find_first_of(L"\r\n")));
        }
        return true;
    }
    return false;
}

// Characters from WM_CHAR: typing replaces the selection; Backspace (Ctrl = a word) erases.
static void EditChar(wchar_t ch) {
    int a = std::min(caret, anchor), b = std::max(caret, anchor);
    if (ch == VK_BACK || ch == 127) {
        if (a == b) a = ch == 127 ? WordLeft(editTarget, caret) : std::max(0, caret - 1);
        Replace(a, b, L"");
    } else if (ch >= 32) {
        Replace(a, b, std::wstring(1, ch));
    }
}

static bool Button(D2D1_RECT_F r) {
    bool hover = In(r);
    if (hover) cursor = IDC_HAND;
    return hover && in.pressed;
}

static bool TextButton(const wchar_t* label, D2D1_RECT_F r, bool enabled = true, UINT32 hoverColor = BTN_HOVER) {
    bool hover = enabled && In(r), click = enabled && Button(r);
    Fill(r, 8, hover ? hoverColor : BTN);
    Stroke(r, 8, BORDER);
    Text(label, r, fValue, enabled ? TEXT : DISABLED, DWRITE_TEXT_ALIGNMENT_CENTER);
    return click;
}

static void Card(float x, float y, float w, float h, const wchar_t* title, const wchar_t* hint = nullptr) {
    Fill(R(x, y, w, h), 12, CARD);
    Stroke(R(x, y, w, h), 12, BORDER);
    Text(title, R(x + 16, y + 12, w - 32, 18), fSection, MUTED);
    if (hint) Text(hint, R(x + 16, y + 12, w - 32, 18), fSmall, DISABLED, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

// off: optional on/off switch drawn before the label; changing the value turns it back on.
static void Slider(const FieldDef& d, float& v, float x, float y, float w, bool* off = nullptr) {
    float ty = y + 32, old = v;
    D2D1_RECT_F sw = R(x, y + 2, 30, 16);
    if (off && Button(sw)) {
        if (!*off && v != 0) *off = true;
        else { // turning on from 0 picks a starting value (10% of the range)
            *off = false;
            if (v == 0) v = d.lo + (d.hi - d.lo) * .1f;
        }
        saveReq = true;
    }
    bool hover = In(R(x - 8, ty - 12, w + 16, 24));
    if (hover) cursor = IDC_HAND;
    if (hover && in.pressed) active = &v;
    if (active == &v) {
        v = d.lo + std::clamp((in.mx - x) / w, 0.f, 1.f) * (d.hi - d.lo);
        if (!in.down) active = nullptr;
    }
    if (hover && in.wheel) {
        v += d.step * in.wheel;
        saveReq = true;
    }
    if (in.rpressed && In(R(x - 8, y, w + 16, 44))) { // right-click: back to the default
        v = Default(d);
        if (off) *off = false;
        saveReq = true;
    }

    // click the value to type an exact number; Enter or click away applies, Esc cancels
    D2D1_RECT_F vr = R(x + w - std::min(120.f, w / 2), y, std::min(120.f, w / 2), 20);
    bool vHover = In(vr), mine = numTarget == &v && editTarget == numBuf;
    D2D1_RECT_F er = R(x, y, w, 20); // typed text sits where the value was, right-aligned
    if (vHover) cursor = IDC_IBEAM;
    if (in.pressed && vHover && !mine) { // start typing with the whole value selected
        if (numTarget && !numCancel && *numBuf && editTarget == numBuf) *numTarget = (float)_wtof(numBuf);
        numTarget = &v;
        swprintf_s(numBuf, L"%g", v);
        editTarget = numBuf;
        editMax = 10;
        numCancel = false;
        selecting = false;
        SetSel(0, (int)wcslen(numBuf));
        SyncCapture();
    } else if (mine) {
        if (in.pressed && !vHover) {
            editTarget = nullptr;
            SyncCapture();
        } else {
            EditMouse(er, DWRITE_TEXT_ALIGNMENT_TRAILING, vHover);
        }
    }
    bool typing = numTarget == &v;
    if (typing) {
        numSeen = true;
        if (editTarget != numBuf) {
            if (!numCancel && *numBuf) v = (float)_wtof(numBuf);
            numTarget = nullptr;
            typing = false;
            saveReq = true;
        }
    }
    auto snap = [&](float a) { return std::clamp(std::round(a / d.step) * d.step, d.lo, d.hi); };
    v = snap(v);
    if (off && *off && v != snap(old)) *off = false;
    bool dim = off && (*off || v == 0); // a value of 0 is off too

    bool hot = hover || active == &v || vHover;
    wchar_t buf[32];
    if ((v == 0 || dim) && d.zero) wcscpy_s(buf, d.zero);
    else swprintf_s(buf, d.fmt, v);
    if (off) {
        Fill(sw, 8, dim ? TRACK : ACCENT);
        Circle(dim ? sw.left + 8 : sw.right - 8, sw.top + 8, 5.5f, TEXT);
    }
    float lx = off ? x + 38 : x;
    Text(d.label, R(lx, y, x + w - lx, 20), fLabel, dim ? MUTED : TEXT);
    if (typing) DrawEdit(er, DWRITE_TEXT_ALIGNMENT_TRAILING, ACCENT);
    else Text(buf, er, fValue, hot ? ACCENT : dim ? DISABLED : MUTED, DWRITE_TEXT_ALIGNMENT_TRAILING);

    UINT32 fill = dim ? DISABLED : ACCENT;
    float f = (v - d.lo) / (d.hi - d.lo), z = d.lo < 0 ? -d.lo / (d.hi - d.lo) : 0; // bipolar ranges fill from 0
    Fill(R(x, ty - 2, w, 4), 2, TRACK);
    Fill(R(x + w * std::min(z, f), ty - 2, w * std::fabs(f - z), 4), 2, fill);
    if (z > 0) Line(x + w * z, ty - 5, x + w * z, ty + 5, MUTED, 1, .6f);
    float r = hot ? 8.f : 7.f;
    Circle(x + w * f, ty, r, fill);
    Circle(x + w * f, ty, r - 2.5f, dim ? MUTED : TEXT);
}

static void KeyBind(const wchar_t* label, int& vk, float x, float y, float w, float bw = 136) { // bw: key box width
    D2D1_RECT_F b = R(x + w - bw, y, bw, 32);
    bool waiting = bindTarget == &vk, hover = In(b);
    if (Button(b)) {
        bindTarget = &vk;
        bindArmed = false;
        SyncCapture();
        SetTimer(hwnd, 1, 10, nullptr);
    }
    if (*label) Text(label, R(x, y, w - bw - 4, 32), fLabel, TEXT);
    Fill(b, 8, hover || waiting ? BTN_HOVER : BTN);
    Stroke(b, 8, waiting ? ACCENT : BORDER, waiting ? 1.5f : 1);
    Text(waiting ? L"Press a key…" : KeyName(vk).c_str(), b, fValue, waiting ? ACCENT : vk ? TEXT : MUTED,
         DWRITE_TEXT_ALIGNMENT_CENTER);
}

static void Toggle(const wchar_t* label, bool& v, float x, float y, float w) {
    if (Button(R(x, y, w, 32))) {
        v = !v;
        saveReq = true;
    }
    Text(label, R(x, y, w, 32), fLabel, TEXT);
    D2D1_RECT_F sw = R(x + w - 42, y + 6, 42, 20);
    Fill(sw, 10, v ? ACCENT : TRACK);
    Circle(v ? sw.right - 10 : sw.left + 10, y + 16, 7, TEXT);
}

template <int N>
static void Segment(const wchar_t* label, int& v, const wchar_t* const (&opts)[N], float x, float y, float w, float sw = 58) {
    Text(label, R(x, y, w - N * sw - 10, 32), fLabel, TEXT);
    D2D1_RECT_F r = R(x + w - N * sw - 6, y, N * sw + 6, 32);
    Fill(r, 8, BTN);
    Stroke(r, 8, BORDER);
    for (int k = 0; k < N; k++) {
        D2D1_RECT_F s = R(r.left + 3 + k * sw, y + 3, sw, 26);
        if (Button(s)) {
            v = k;
            saveReq = true;
        }
        if (v == k) Fill(s, 6, ACCENT);
        Text(opts[k], s, fValue, v == k ? WHITE : MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void NameField(wchar_t* val, int max, D2D1_RECT_F r, const wchar_t* placeholder, const wchar_t* hint) {
    bool hover = In(r);
    if (hover) cursor = IDC_IBEAM;
    D2D1_RECT_F tr = R(r.left + 12, r.top, r.right - r.left - 24, r.bottom - r.top);
    if (in.pressed) { // click in → edit this field; click out → stop if it was ours
        if (hover && editTarget != val) {
            editTarget = val;
            editMax = max;
        } else if (!hover && editTarget == val) {
            editTarget = nullptr;
        }
        SyncCapture();
    }
    bool editing = editTarget == val;
    if (editing) EditMouse(tr, DWRITE_TEXT_ALIGNMENT_LEADING, hover);
    Fill(r, 8, editing || hover ? BTN_HOVER : BTN);
    Stroke(r, 8, editing ? ACCENT : BORDER, editing ? 1.5f : 1);
    if (editing) {
        DrawEdit(tr, DWRITE_TEXT_ALIGNMENT_LEADING, TEXT);
    } else {
        Text(*val ? val : placeholder, tr, fValue, *val ? TEXT : MUTED);
        if (hover && hint) Text(hint, tr, fSmall, MUTED, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

// Runs the same model as the engine (minus jitter) and plots speed over time.
static void Preview(const Profile& p, double scale, float x, float y, float w, float h) {
    float T = p.startDelay + (p.duration > 0 ? p.duration + 200 : std::max({p.ramp, p.at2, p.at3, p.growEvery * 4, p.sway ? p.swayMs * 2 : 0}) + 600);
    const int N = 100;
    float sy[N + 1], sx[N + 1], lo = 0, hi = 0;
    double vx = 0, vy = 0, totX = 0, totY = 0;
    for (int ms = 0, i = 0; ms <= (int)std::ceil(T); ms++) {
        double tx, ty;
        Target(p, scale, ms, tx, ty);
        vx = Follow(vx, tx, p, 1);
        vy = Follow(vy, ty, p, 1);
        totX += vx / 1000;
        totY += vy / 1000;
        if (ms >= i * T / N && i <= N) {
            sy[i] = (float)vy;
            sx[i] = (float)vx;
            lo = std::min({lo, sy[i], sx[i]});
            hi = std::max({hi, sy[i], sx[i]});
            i++;
        }
    }
    if (hi - lo < 1) hi = lo + 1;
    auto Y = [&](float v) { return y + h - (v - lo) / (hi - lo) * h; };

    for (int g = 1; g < 4; g++) Line(x, y + h * g / 4, x + w, y + h * g / 4, BORDER, 1);
    Line(x, Y(0), x + w, Y(0), MUTED, 1, .5f);
    for (int i = 0; i < N; i++) {
        float x1 = x + w * i / N, x2 = x + w * (i + 1) / N;
        if (p.vx || p.sway) Line(x1, Y(sx[i]), x2, Y(sx[i + 1]), TEAL, 2);
        Line(x1, Y(sy[i]), x2, Y(sy[i + 1]), ACCENT, 2);
    }

    wchar_t buf[64];
    swprintf_s(buf, L"Δy %+.0f   Δx %+.0f px", totY, totX);
    Text(buf, R(x, y + h + 8, w, 20), fSmall, MUTED);
    swprintf_s(buf, L"over %.2f s", T / 1000);
    Text(buf, R(x, y + h + 28, w, 20), fSmall, MUTED);
}

static wchar_t toastReq[64]; // toast to show once the frame is done (the overlay can't draw mid-frame)
static UINT32 toastReqColor;

// "Group › Active slot" for a group, else the profile's name.
static void ProfileLabel(const Config& c, int i, wchar_t* out, size_t cap) {
    const Profile& g = c.p[i];
    int ch = g.group ? SlotProfile(c, g, g.childSel) : -1;
    const wchar_t* n = *g.name ? g.name : L"Untitled";
    if (ch >= 0) swprintf_s(out, cap, L"%s › %s", n, *c.p[ch].name ? c.p[ch].name : L"Untitled");
    else wcscpy_s(out, cap, n);
}

// Slot picker: ‹ name ›. The left half steps back, the right half (or the mouse wheel) forward, through
// None and the normal profiles.
static void ProfilePick(int& idx, D2D1_RECT_F b, bool active) {
    bool hover = In(b);
    auto step = [&](int dir) {
        int k = idx < cfg.n ? idx : -1;
        do {
            k += dir;
            if (k < -1) k = cfg.n - 1;
            if (k >= cfg.n) k = -1;
        } while (k >= 0 && cfg.p[k].group);
        idx = k;
        saveReq = true;
    };
    if (hover) cursor = IDC_HAND;
    if (hover && in.pressed) step(in.mx < (b.left + b.right) / 2 ? -1 : 1);
    if (hover && in.wheel) step(in.wheel > 0 ? -1 : 1);
    bool ok = idx >= 0 && idx < cfg.n && !cfg.p[idx].group;
    Fill(b, 8, hover ? BTN_HOVER : BTN);
    Stroke(b, 8, active ? ACCENT : BORDER, active ? 1.5f : 1);
    Text(ok ? (*cfg.p[idx].name ? cfg.p[idx].name : L"Untitled") : L"None", R(b.left + 18, b.top, b.right - b.left - 36, 32),
         fValue, ok ? TEXT : MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(L"‹", R(b.left + 7, b.top, 10, 32), fValue, MUTED);
    Text(L"›", R(b.right - 15, b.top, 10, 32), fValue, MUTED);
}

// One line about a profile for the list: its pull and keys, or a group's slots (● = active) and their keys.
static std::wstring Details(const Config& c, int i) {
    const Profile& p = c.p[i];
    std::wstring s;
    if (p.group) {
        for (int k = 0; k < 2; k++) {
            int ch = SlotProfile(c, p, k);
            s += k ? L"        " : L"";
            s += p.childSel == k ? L"●  " : L"○  ";
            s += ch >= 0 ? (*c.p[ch].name ? c.p[ch].name : L"Untitled") : L"empty";
            if (p.childKey[k]) s += L"  [" + KeyName(p.childKey[k]) + L"]";
        }
        return s;
    }
    static const wchar_t* const modes[] = {L"Hold", L"Toggle", L"Tap"};
    wchar_t b[96];
    swprintf_s(b, L"↓%.0f  →%.0f px/s     ·     %s ", p.vy, p.vx, modes[p.mode]);
    s = b + KeyName(p.holdKey);
    if (p.spamOn) s += L"     ·     spam " + KeyName(p.spamKey);
    return s;
}

static void RunProfileCmd() {
    Profile* p = cfg.p;
    int& n = cfg.n;
    int& cur = cfg.cur;
    if (profileCmd == 1 && n < MAX_PROFILES) { // new
        InsertProfile(cfg, n, Profile{});
        swprintf_s(p[n - 1].name, L"Profile %d", n);
        cur = n - 1;
    } else if (profileCmd == 6 && n < MAX_PROFILES) { // new group, its slots on the first two normal profiles
        InsertProfile(cfg, n, Profile{});
        Profile& g = p[n - 1];
        g.group = true;
        swprintf_s(g.name, L"Group %d", n);
        for (int i = 0, s = 0; i < n - 1 && s < 2; i++)
            if (!p[i].group) g.child[s++] = i;
        cur = n - 1;
    } else if (profileCmd == 2 && n < MAX_PROFILES) { // duplicate right after the current one
        InsertProfile(cfg, cur + 1, p[cur]);
        swprintf_s(p[cur + 1].name, L"%.15s copy", p[cur].name);
        p[cur + 1].hotkey = 0; // two profiles on one hotkey would fight
        cur++;
    } else if (profileCmd == 3 && n > 1) { // delete
        DeleteProfile(cfg, cur);
        cur = std::min(cur, n - 1);
    } else if (profileCmd == 4) { // copy
        CopyText(ExportProfile(p[cur]));
        wcscpy_s(toastReq, L"Profile copied");
        toastReqColor = ACCENT;
    } else if (profileCmd == 5) { // paste over the current one
        bool ok = ImportProfile(PasteText().c_str(), p[cur]);
        wcscpy_s(toastReq, ok ? L"Profile pasted" : L"No profile on the clipboard");
        toastReqColor = ok ? GREEN : RED;
    }
    if (profileCmd) {
        saveReq = true;
        InvalidateRect(hwnd, nullptr, FALSE); // this frame was drawn before the change
    }
    profileCmd = 0;
}

static void Frame(float W) {
    cursor = IDC_ARROW;
    numSeen = false;
    if (bindTarget) in.pressed = in.rpressed = false; // binding is modal: the click belongs to the key catcher
    ACCENT = accents[cfg.accent][0];
    ACCENT_SOFT = accents[cfg.accent][1];
    rt->Clear(D2D1::ColorF(BG));
    Profile& g = cfg.p[cfg.cur]; // the selected list entry: a profile or a group
    int runIdx = g.group ? SlotProfile(cfg, g, g.childSel) : cfg.cur; // whose pull settings show (-1: empty slot)
    Profile& p = cfg.p[runIdx >= 0 ? runIdx : cfg.cur];

    // header: title, clickable status pill, window buttons
    Text(L"Mouse Down", R(20, 0, 200, HEADER), fTitle, TEXT);
    bool pull = pulling, paused = pausedNow;
    UINT32 sc = !cfg.enabled ? MUTED : paused ? ORANGE : pull ? GREEN : ACCENT;
    if (Button(PILL)) {
        cfg.enabled = !cfg.enabled;
        saveReq = true;
    }
    Fill(PILL, 11, sc, In(PILL) ? .24f : .14f);
    Circle(PILL.left + 13, PILL.top + 11, 3.5f, sc);
    Text(!cfg.enabled ? L"OFF" : paused ? L"PAUSED" : pull ? L"PULLING" : L"READY", R(PILL.left + 23, PILL.top, 60, 22), fPill, sc);

    int upd = updState;
    static const wchar_t* const updLabels[] = {L"Update", L"Checking…", L"Up to date", L"Install update", L"Updating…", L"Update failed"};
    bool updBusy = upd == UPD_CHECKING || upd == UPD_INSTALLING;
    if (Button(UPDATE_BTN) && !updBusy) StartUpdate(hwnd);
    Fill(UPDATE_BTN, 8, upd == UPD_READY ? ACCENT : In(UPDATE_BTN) && !updBusy ? BTN_HOVER : BTN);
    Stroke(UPDATE_BTN, 8, upd == UPD_READY ? ACCENT : BORDER);
    Text(updLabels[upd], UPDATE_BTN, fValue, upd == UPD_READY ? WHITE : upd == UPD_FAILED ? RED : updBusy ? MUTED : TEXT,
         DWRITE_TEXT_ALIGNMENT_CENTER);

    D2D1_RECT_F minB = R(W - 96, 10, 38, 36), closeB = R(W - 52, 10, 38, 36);
    if (In(minB)) Fill(minB, 8, BTN_HOVER);
    if (In(closeB)) Fill(closeB, 8, RED);
    if (Button(minB)) headerCmd = 1;
    if (Button(closeB)) headerCmd = 2;
    Line(W - 82, 28, W - 72, 28, TEXT, 1.2f);
    Line(W - 38, 23, W - 28, 33, TEXT, 1.2f);
    Line(W - 38, 33, W - 28, 23, TEXT, 1.2f);

    static int tab = 1; // 0 = the profile list, 1 = edit the selected one, 2 = settings
    static const wchar_t* const tabNames[] = {L"Profiles", L"Edit", L"Settings"};
    Fill(TABS, 8, BTN);
    Stroke(TABS, 8, BORDER);
    for (int k = 0; k < 3; k++) {
        D2D1_RECT_F t = R(TABS.left + 3 + k * 96.f, TABS.top + 3, 96, TABS.bottom - TABS.top - 6);
        if (Button(t) && tab != k) {
            tab = k;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (tab == k) Fill(t, 6, ACCENT);
        Text(tabNames[k], t, fValue, tab == k ? WHITE : In(t) ? TEXT : MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // the profile list scrolls inside its card and follows the current profile when it changes elsewhere
    static float scroll;
    static int shownCur = -1, dragRow = -1; // dragRow: profile being dragged to a new place in the list
    static const wchar_t* const scopes[] = {L"All", L"Groups"};
    static const wchar_t* const shows[] = {L"All", L"Profiles", L"Groups"};
    int vis[MAX_PROFILES], nv = VisibleProfiles(cfg, vis), curRow = 0;
    for (int v = 0; v < nv; v++)
        if (vis[v] == cfg.cur) curRow = v;
    if (tab == 0) {
        int tgt[MAX_PROFILES], nt = SwitchTargets(cfg, tgt), num[MAX_PROFILES];
        std::fill(num, num + cfg.n, -1); // quick switch number of each profile, if it has one
        for (int k = 0; k < nt; k++) num[tgt[k]] = k;

        // bar: list commands and what the list shows
        if (TextButton(L"+  Profile", R(20, 64, 110, 34), cfg.n < MAX_PROFILES)) profileCmd = 1;
        if (TextButton(L"+  Group", R(138, 64, 110, 34), cfg.n < MAX_PROFILES)) profileCmd = 6;
        if (TextButton(L"Duplicate", R(256, 64, 100, 34), cfg.n < MAX_PROFILES)) profileCmd = 2;
        if (TextButton(L"Delete", R(364, 64, 84, 34), cfg.n > 1, RED_SOFT)) profileCmd = 3;
        if (TextButton(L"Copy", R(456, 64, 72, 34))) profileCmd = 4;
        if (TextButton(L"Paste", R(536, 64, 72, 34))) profileCmd = 5;
        Segment(L"Show", cfg.listShow, shows, 900, 65, 340, 80);

        Card(20, 116, 1220, 696, L"PROFILES", L"click to select  ·  double-click to edit  ·  drag to reorder");
        static const std::pair<const wchar_t*, float> heads[] = {{L"#", 40}, {L"Name", 68}, {L"Type", 298}, {L"Details", 378}};
        for (auto& [label, x] : heads) Text(label, R(x, 148, 200, 18), fSmall, DISABLED);
        Text(L"Hotkey", R(1082, 148, 140, 18), fSmall, DISABLED, DWRITE_TEXT_ALIGNMENT_TRAILING);
        const D2D1_RECT_F list = R(24, 168, 1212, 640);
        const float listH = list.bottom - list.top - 8, maxScroll = std::max(0.f, nv * 38.f - 4 - listH);
        if (In(list) && in.wheel) scroll -= in.wheel * 38;
        if (cfg.cur != shownCur) {
            shownCur = cfg.cur;
            scroll = std::clamp(scroll, curRow * 38.f + 34 - listH, curRow * 38.f);
        }
        scroll = std::clamp(scroll, 0.f, maxScroll);
        rt->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int v = 0; v < nv; v++) {
            int i = vis[v];
            const Profile& q = cfg.p[i];
            D2D1_RECT_F r = R(28, 172 + v * 38.f - scroll, 1204, 34);
            if (r.bottom < list.top || r.top > list.bottom) continue;
            bool sel = cfg.cur == i, hover = In(r) && In(list);
            if (hover && Button(r)) {
                cfg.cur = i;
                if (in.dbl) tab = 1, dragRow = -1; // double-click: open it
                else if (cfg.listShow == 0) dragRow = i; // reordering works on the full list only
                saveReq = true;
            }
            Fill(r, 8, sel ? ACCENT : hover || dragRow == i ? BTN_HOVER : CARD);
            if (num[i] >= 0 && num[i] < 9)
                Text(std::to_wstring(num[i] + 1).c_str(), R(40, r.top, 20, 34), fSmall, sel ? ACCENT_SOFT : DISABLED);
            Text(*q.name ? q.name : L"Untitled", R(68, r.top, 220, 34), fLabel, sel ? WHITE : TEXT);
            Text(q.group ? L"Group" : L"Profile", R(298, r.top, 70, 34), fSmall, sel ? ACCENT_SOFT : q.group ? ACCENT : MUTED);
            Text(Details(cfg, i).c_str(), R(378, r.top, 690, 34), fSmall, sel ? ACCENT_SOFT : MUTED);
            if (q.hotkey) Text(KeyName(q.hotkey).c_str(), R(r.right - 150, r.top, 138, 34), fSmall, sel ? ACCENT_SOFT : MUTED,
                               DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (!nv) Text(L"Nothing to show here: pick All or make one", R(40, 180, 600, 34), fLabel, MUTED);
        rt->PopAxisAlignedClip();
        if (maxScroll > 0) { // scrollbar
            float th = listH * listH / (listH + maxScroll), ty = list.top + 4 + (listH - th) * scroll / maxScroll;
            Fill(R(1233, ty, 3, th), 1.5f, MUTED, .5f);
        }
    } else if (tab == 1) {
        // bar: step through the list without leaving the page, rename, list commands
        for (int k = 0; k < 2; k++)
            if (TextButton(k ? L"›" : L"‹", R(20 + k * 38.f, 64, 34, 34), nv > 1)) {
                cfg.cur = vis[(curRow + (k ? 1 : nv - 1)) % nv];
                saveReq = true;
            }
        NameField(g.name, NAME_LEN, R(100, 64, 250, 34), L"Untitled", L"Rename");
        if (TextButton(L"Duplicate", R(360, 64, 100, 34), cfg.n < MAX_PROFILES)) profileCmd = 2;
        if (TextButton(L"Delete", R(468, 64, 84, 34), cfg.n > 1, RED_SOFT)) profileCmd = 3;
        if (TextButton(L"Copy", R(560, 64, 72, 34))) profileCmd = 4;
        if (TextButton(L"Paste", R(640, 64, 72, 34))) profileCmd = 5;
        Text(L"Scroll: fine steps · click value: type · right-click: reset", R(720, 64, W - 740, 34), fSmall, MUTED,
             DWRITE_TEXT_ALIGNMENT_TRAILING);

        // left column: keys (or the group's slots), then auto spam
        if (g.group) { // slots: click the label to make it active; ‹ › picks the profile; then the slot's key
            Card(20, 116, 360, 220, L"GROUP", L"Backspace clears");
            for (int k = 0; k < 2; k++) {
                float y = 152 + k * 44.f;
                bool act = g.childSel == k;
                if (Button(R(36, y, 54, 32))) {
                    g.childSel = k;
                    saveReq = true;
                }
                if (act) Circle(40, y + 16, 3.5f, GREEN);
                Text(k ? L"Slot 2" : L"Slot 1", R(48, y, 46, 32), fLabel, act ? TEXT : MUTED);
                ProfilePick(g.child[k], R(94, y, 150, 32), act);
                KeyBind(L"", g.childKey[k], 250, y, 114, 114);
            }
            Text(L"Slot keys switch only while this group is active", R(36, 240, 328, 18), fSmall, MUTED);
            Text(L"and still reach the game (bind your weapon keys)", R(36, 258, 328, 18), fSmall, MUTED);
        } else {
            Card(20, 116, 360, 220, L"KEYS", L"Backspace clears");
            KeyBind(L"Hold to pull", p.holdKey, 36, 152, 328);
            KeyBind(L"Also require", p.requireKey, 36, 196, 328);
            static const wchar_t* const modes[] = {L"Hold", L"Toggle", L"Tap"};
            Segment(L"Activation", p.mode, modes, 36, 240, 328);
        }
        KeyBind(L"Profile hotkey", g.hotkey, 36, 284, 328);

        wchar_t spamTitle[48] = L"AUTO SPAM";
        if (g.group && runIdx >= 0) swprintf_s(spamTitle, L"AUTO SPAM  ·  %s", *p.name ? p.name : L"Untitled");
        Card(20, 348, 360, 464, spamTitle);
        if (runIdx >= 0) {
            Toggle(L"Enabled", p.spamOn, 36, 384, 328);
            KeyBind(L"Hold to spam", p.spamTrigger, 36, 428, 328);
            KeyBind(L"Also require", p.spamRequire, 36, 472, 328);
            KeyBind(L"Key to spam", p.spamKey, 36, 516, 328);
            Slider(spamDefs[0], p.spamRate, 36, 562, 156);
            Slider(spamDefs[1], p.spamDelay, 208, 562, 156, &p.spamDelayOff);
            Toggle(L"Clicks trigger pull", p.spamPull, 36, 612, 328);
            Slider(spamDefs[2], p.spamJitter, 36, 658, 328);
        }

        // pull in the middle, timing + curve on the right
        Card(840, 424, 400, 388, L"CURVE");
        Circle(1190, 445, 3.5f, ACCENT);
        Text(L"y", R(1196, 436, 12, 18), fSmall, MUTED);
        Circle(1210, 445, 3.5f, TEAL);
        Text(L"x", R(1216, 436, 12, 18), fSmall, MUTED);
        if (runIdx >= 0) {
            // stage tabs pick which stage's sliders show; stage 1 runs from activation
            static int stage;
            static const wchar_t* const stages[] = {L"1", L"2", L"3"};
            wchar_t pullTitle[48] = L"PULL";
            if (g.group) swprintf_s(pullTitle, L"PULL  ·  %s", *p.name ? p.name : L"Untitled"); // editing the active slot's profile
            Card(400, 116, 420, 696, pullTitle);
            Segment(L"Spray stage", stage, stages, 416, 152, 388);
            const float segL = 416 + 388 - std::size(stages) * 58.f - 3; // left edge of the first tab
            for (int s = 1; s < 3; s++) { // green dot over stages that are on
                float at = s == 1 ? (p.off[6] ? 0 : p.at2) : (p.off[7] ? 0 : p.at3);
                if (at > 0) Circle(segL + s * 58 + 50, 158, 3, GREEN);
            }
            auto PS = [&](const FieldDef& d, float x, float y, float w) { // profile slider, with its switch if it has one
                Slider(d, p.*d.f, x, y, w, d.bit >= 0 ? &p.off[d.bit] : nullptr);
            };
            int base = stage ? 2 + (stage - 1) * 3 : 0;
            PS(fields[base], 416, 196, 388);
            PS(fields[base + 1], 416, 246, 388);
            if (stage) PS(fields[base + 2], 416, 296, 388);
            else Text(L"Runs from activation until stage 2 / 3 starts", R(416, 296, 388, 40), fSmall, MUTED);
            PS(fields[STAGE_FIELDS], 416, 346, 388); // jitter
            PS(fields[STAGE_FIELDS + 1], 416, 396, 184); // offset min/max: both switches flip the same flag
            PS(fields[STAGE_FIELDS + 2], 620, 396, 184);
            PS(swayDefs[0], 416, 452, 184); // sway amplitude (its switch) + period
            PS(swayDefs[1], 620, 452, 184);
            Line(416, 512, 804, 512, BORDER);
            Text(L"GROWTH", R(416, 524, 200, 18), fSection, MUTED);
            Text(L"pull gained every step while held", R(416, 524, 388, 18), fSmall, DISABLED, DWRITE_TEXT_ALIGNMENT_TRAILING);
            for (int i = 0; i < 3; i++) PS(growDefs[i], 416, 556 + i * 50.f, 388);

            Card(840, 116, 400, 296, L"TIMING & SMOOTHING");
            for (int i = PULL_FIELDS; i < (int)std::size(fields); i++)
                PS(fields[i], 856, 152 + (i - PULL_FIELDS) * 50.f, 368);
            Preview(Effective(p), cfg.sens / 100, 856, 464, 368, 270);
        } else {
            Card(400, 116, 420, 696, L"PULL");
            Text(L"This slot is empty: pick a profile for it in the Group card", R(416, 152, 388, 32), fLabel, MUTED);
            Card(840, 116, 400, 296, L"TIMING & SMOOTHING");
        }
    } else {
        Card(20, 64, 380, 460, L"GENERAL");
        KeyBind(L"On / off", cfg.toggleKey, 36, 100, 348);
        KeyBind(L"Hold to pause", cfg.pauseKey, 36, 144, 348);
        Slider(sensDef, cfg.sens, 36, 190, 348);
        Toggle(L"Always on top", cfg.onTop, 36, 248, 348);
        Text(L"HID port", R(36, 292, 160, 34), fLabel, TEXT);
        NameField(cfg.port, 15, R(248, 292, 136, 34), L"SendInput", L"blank = inject");
        Toggle(L"Sounds", cfg.sound, 36, 336, 348);
        Toggle(L"Notifications", cfg.notify, 36, 380, 348);
        Toggle(L"In-game HUD", cfg.hud, 36, 424, 348);
        Text(L"Accent", R(36, 468, 120, 32), fLabel, TEXT);
        for (int k = 0; k < ACCENTS; k++) {
            float cx = 373 - (ACCENTS - 1 - k) * 28.f, cy = 484;
            if (Button(R(cx - 12, cy - 12, 24, 24))) {
                cfg.accent = k;
                saveReq = true;
            }
            Circle(cx, cy, 9, accents[k][0]);
            if (cfg.accent == k) {
                Color(TEXT);
                rt->DrawEllipse({{cx, cy}, 12.5f, 12.5f}, br, 1.5f);
            }
        }

        Card(420, 64, 380, 460, L"SWITCHING", L"Backspace clears");
        KeyBind(L"Next profile", cfg.nextKey, 436, 100, 348);
        KeyBind(L"Prev profile", cfg.prevKey, 436, 144, 348);
        KeyBind(L"Last profile", cfg.lastKey, 436, 188, 348);
        KeyBind(L"Quick switch", cfg.quickKey, 436, 232, 348);
        KeyBind(L"Hold for wheel", cfg.wheelKey, 436, 276, 348);
        int sg = cfg.switchGroups;
        Segment(L"Wheel, 1–9 and next / prev use", sg, scopes, 436, 322, 348, 66);
        Segment(L"Profile list shows", cfg.listShow, shows, 436, 366, 348, 66);
        cfg.switchGroups = sg;
        Text(L"Quick switch: hold it and press 1–9, or scroll", R(436, 414, 348, 18), fSmall, MUTED);
        Text(L"Wheel: aim, scroll or click to pick", R(436, 432, 348, 18), fSmall, MUTED);
        Text(L"Groups: normal profiles are skipped (the list still", R(436, 450, 348, 18), fSmall, MUTED);
        Text(L"shows the active one); 1–9 count groups only", R(436, 468, 348, 18), fSmall, MUTED);

        Card(820, 64, 420, 460, L"GROUPS");
        KeyBind(L"Swap slot", cfg.swapKey, 836, 100, 388);
        Toggle(L"Mouse wheel swaps slots", cfg.scrollSwap, 836, 144, 388);
        static const wchar_t* const resume[] = {L"Last slot", L"Slot 1"};
        int sr = cfg.slotReset;
        Segment(L"Switching to a group picks", sr, resume, 836, 188, 388, 84);
        cfg.slotReset = sr;
        Text(L"Swap slot flips the active group between its two slots.", R(836, 236, 388, 18), fSmall, MUTED);
        Text(L"Mouse wheel swaps on every notch and still scrolls in the", R(836, 254, 388, 18), fSmall, MUTED);
        Text(L"game, so it follows games that change weapons with it.", R(836, 272, 388, 18), fSmall, MUTED);
        Text(L"Each group's own slot keys also still reach the game.", R(836, 290, 388, 18), fSmall, MUTED);
        Text(L"Make groups with  + Group  on the Profiles tab.", R(836, 326, 388, 18), fSmall, MUTED);
    }

    if (numTarget && !numSeen) { // its slider is gone (profile or stage switched): drop the edit
        numTarget = nullptr;
        if (editTarget == numBuf) editTarget = nullptr;
        SyncCapture();
    }
    RunProfileCmd();

    if (dragRow >= 0) { // after the frame, like RunProfileCmd, so nothing moves mid-draw
        if (!in.down || dragRow >= cfg.n) {
            dragRow = -1;
        } else {
            int to = std::clamp((int)std::floor((in.my - 172 + scroll) / 38), 0, cfg.n - 1);
            if (to != dragRow) cursor = IDC_SIZENS;
            for (; dragRow < to; dragRow++) SwapProfiles(cfg, dragRow, dragRow + 1);
            for (; dragRow > to; dragRow--) SwapProfiles(cfg, dragRow, dragRow - 1);
            if (cfg.cur != dragRow) {
                cfg.cur = dragRow;
                saveReq = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
    }
}

static void Paint() {
    if (!hrt) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        float dpi = (float)GetDpiForWindow(hwnd);
        d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi),
            D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &hrt);
        hrt->CreateSolidColorBrush(D2D1::ColorF(0), &hbr);
    }
    rt = hrt;
    br = hbr;
    bool onTop;
    rt->BeginDraw();
    {
        std::lock_guard<std::mutex> l(mu);
        Frame(rt->GetSize().width);
        onTop = cfg.onTop;
    }
    in.pressed = in.rpressed = in.dbl = false;
    in.wheel = 0;
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        hbr->Release();
        hrt->Release();
        hbr = nullptr;
        hrt = nullptr;
    }
    if (onTop != onTopApplied) {
        onTopApplied = onTop;
        SetWindowPos(hwnd, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// ---------- overlays: profile wheel + toast ----------

static wchar_t toastMsg[64];
static UINT32 toastColor;
static ULONGLONG toastAt; // 0 = no toast showing
static bool wheelOpen;
static int wheelSel = -1;
static float wheelX, wheelY; // raw mouse motion since the wheel opened
const float WHEEL_SIZE = 400, TOAST_W = 280, TOAST_H = 48;

static RECT GameMonitor() { // the monitor the foreground window (usually the game) is on
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

// Render into a per-pixel-alpha layered window at screen pixel (x, y), sized in DIPs.
static void DrawLayered(HWND w, int x, int y, float wDip, float hDip, void (*draw)(float, float), BYTE alpha = 255) {
    float dpi = (float)GetDpiForSystem();
    int pw = (int)std::ceil(wDip * dpi / 96), ph = (int)std::ceil(hDip * dpi / 96);
    if (!dcrt) {
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        d2d->CreateDCRenderTarget(&props, &dcrt);
        dcrt->CreateSolidColorBrush(D2D1::ColorF(0), &obr);
        dcrt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // ClearType needs an opaque background
    }
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), pw, -ph, 1, 32}};
    void* bits;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT rc{0, 0, pw, ph};
    dcrt->BindDC(mem, &rc);
    rt = dcrt;
    br = obr;
    dcrt->BeginDraw();
    dcrt->Clear(D2D1::ColorF(0, 0));
    draw(wDip, hDip);
    if (dcrt->EndDraw() == D2DERR_RECREATE_TARGET) {
        obr->Release();
        dcrt->Release();
        obr = nullptr;
        dcrt = nullptr;
    }
    POINT pos{x, y}, src{};
    SIZE sz{pw, ph};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(w, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    ShowWindow(w, SW_SHOWNOACTIVATE);
}

static void DrawToast(float w, float h) {
    Fill(R(0, 0, w, h), 12, CARD, .95f);
    Stroke(R(0, 0, w, h), 12, BORDER);
    Circle(24, h / 2, 5, toastColor);
    Text(toastMsg, R(40, 0, w - 52, h), fValue, TEXT);
}

static void Notify(const wchar_t* msg, UINT32 color) {
    wcscpy_s(toastMsg, msg);
    toastColor = color;
    toastAt = GetTickCount64();
    RECT m = GameMonitor();
    float s = GetDpiForSystem() / 96.f;
    DrawLayered(toastWnd, (m.left + m.right) / 2 - (int)(TOAST_W * s / 2), m.top + (int)(40 * s), TOAST_W, TOAST_H, DrawToast);
}

static void Chime(int ev) { // Beep blocks for its length, so it gets its own thread
    DWORD freq = ev == EV_ON ? 880 : ev == EV_OFF ? 440 : 660;
    std::thread([freq] { Beep(freq, 70); }).detach();
}

static float wheelHl[MAX_PROFILES]; // per-slice highlight, eased toward 1 for the hovered one
static int wheelIdx[MAX_PROFILES], wheelN = 1; // profile on each slice (SwitchTargets), and how many
static float wheelOpenT;            // 0..1 open animation
const float TAU = 6.2831853f;

// Donut slice between radii r0..r1 and angles a0..a1 (0 = up, clockwise).
static void Slice(float c, float r0, float r1, float a0, float a1, UINT32 col, float alpha, UINT32 edge = 0) {
    auto P = [&](float r, float a) { return D2D1::Point2F(c + r * std::sin(a), c - r * std::cos(a)); };
    auto big = a1 - a0 > TAU / 2 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    ID2D1PathGeometry* g;
    ID2D1GeometrySink* s;
    d2d->CreatePathGeometry(&g);
    g->Open(&s);
    s->BeginFigure(P(r1, a0), D2D1_FIGURE_BEGIN_FILLED);
    s->AddArc({P(r1, a1), {r1, r1}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, big});
    s->AddLine(P(r0, a1));
    s->AddArc({P(r0, a0), {r0, r0}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, big});
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    Color(col, alpha);
    rt->FillGeometry(g, br);
    if (edge) {
        Color(edge);
        rt->DrawGeometry(g, br, 1);
    }
    g->Release();
}

static void DrawWheel(float w, float) {
    std::lock_guard<std::mutex> l(mu);
    float c = w / 2, e = 1 - (1 - wheelOpenT) * (1 - wheelOpenT); // ease-out
    float k = .88f + .12f * e;
    rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, {c, c}));

    const float R0 = 78, R1 = 176, gap = .035f, span = TAU / wheelN;
    for (int i = 0; i < wheelN; i++) {
        float t = wheelHl[i], a0 = i * span - span / 2 + gap, a1 = (i + 1) * span - span / 2 - gap;
        float r1 = R1 + 10 * t; // hovered slice pops outward
        Slice(c, R0, r1, a0, a1, CARD, .94f, BORDER);
        if (t > .01f) Slice(c, R0, r1, a0, a1, ACCENT, .9f * t);
        if (wheelIdx[i] == cfg.cur) Slice(c, r1 - 4, r1, a0, a1, GREEN, 1); // current profile: green rim

        float am = i * span, lr = (R0 + R1) / 2 + 4 * t, px = c + lr * std::sin(am), py = c - lr * std::cos(am);
        const Profile& p = cfg.p[wheelIdx[i]];
        UINT32 tc = t > .5f ? WHITE : TEXT;
        if (wheelN > 8) { // crowded: just the name, as wide as the slice allows (hub shows the rest)
            float lw = std::min(104.f, lr * span - 8);
            Text(*p.name ? p.name : L"Untitled", R(px - lw / 2, py - 10, lw, 20), fSmall, tc, DWRITE_TEXT_ALIGNMENT_CENTER);
            continue;
        }
        Text(*p.name ? p.name : L"Untitled", R(px - 52, py - 18, 104, 20), fValue, tc, DWRITE_TEXT_ALIGNMENT_CENTER);
        wchar_t sub[32];
        swprintf_s(sub, L"%d", i + 1);
        if (p.hotkey) swprintf_s(sub, L"%d · %s", i + 1, KeyName(p.hotkey).c_str());
        else if (int ch = p.group ? SlotProfile(cfg, p, p.childSel) : -1; ch >= 0) swprintf_s(sub, L"%d › %.12s", i + 1, cfg.p[ch].name);
        Text(sub, R(px - 52, py + 2, 104, 18), fSmall, t > .5f ? ACCENT_SOFT : MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // hub: what releasing now would pick, plus its pull
    Color(BG, .96f);
    rt->FillEllipse({{c, c}, R0 - 8, R0 - 8}, br);
    Color(BORDER);
    rt->DrawEllipse({{c, c}, R0 - 8, R0 - 8}, br, 1);
    int show = wheelSel >= 0 ? wheelIdx[wheelSel] : cfg.cur;
    const Profile& p = cfg.p[show];
    Text(*p.name ? p.name : L"Untitled", R(c - 62, c - 26, 124, 22), fValue, TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
    wchar_t stats[48];
    int ch = p.group ? SlotProfile(cfg, p, p.childSel) : -1;
    if (p.group) swprintf_s(stats, L"› %.20s", ch >= 0 ? cfg.p[ch].name : L"empty slot");
    else swprintf_s(stats, L"↓%.0f  →%.0f", p.vy, p.vx);
    Text(stats, R(c - 62, c - 4, 124, 18), fSmall, MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(wheelSel < 0 ? L"aim or scroll" : show == cfg.cur ? L"current" : L"release or click", R(c - 62, c + 14, 124, 18),
         fSmall, wheelSel >= 0 && show != cfg.cur ? ACCENT : DISABLED, DWRITE_TEXT_ALIGNMENT_CENTER);

    // pointer: arc on the hub edge facing the mouse direction
    float len = std::hypot(wheelX, wheelY);
    if (len > 8) {
        float a = std::atan2(wheelX, -wheelY);
        Slice(c, R0 - 8, R0 - 3, a - .35f, a + .35f, ACCENT, std::min(1.f, len / 25));
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

// Raw mouse motion steers the wheel. Only subscribed while it's open: a high-rate gaming mouse
// would otherwise send the UI thread thousands of messages a second for nothing.
static void RawMouse(bool on) {
    RAWINPUTDEVICE rid{1, 2, (DWORD)(on ? RIDEV_INPUTSINK : RIDEV_REMOVE), on ? hwnd : nullptr}; // generic desktop / mouse
    RegisterRawInputDevices(&rid, 1, sizeof rid);
}

static bool caretShown;

// In-game HUD: active profile, its number and status, in the corner of the game's monitor.
// Redrawn only when something on it changes.
static wchar_t hudName[2 * NAME_LEN + 8], hudSub[32];
static UINT32 hudColor;
const float HUD_W = 300, HUD_H = 40;

static void DrawHud(float w, float h) {
    Fill(R(0, 0, w, h), 10, CARD, .9f);
    Stroke(R(0, 0, w, h), 10, BORDER);
    Circle(18, h / 2, 5, hudColor);
    Text(hudName, R(32, 0, w - 140, h), fValue, TEXT);
    Text(hudSub, R(w - 112, 0, 100, h), fSmall, MUTED, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void UpdateHud() {
    static bool shown;
    static LONG atX, atY;
    bool on, hud;
    wchar_t name[2 * NAME_LEN + 8], sub[32];
    int cur, n;
    {
        std::lock_guard<std::mutex> l(mu);
        hud = cfg.hud;
        on = cfg.enabled;
        cur = cfg.cur;
        n = cfg.n;
        ProfileLabel(cfg, cur, name, std::size(name));
    }
    if (!hud) {
        if (shown) ShowWindow(hudWnd, SW_HIDE);
        shown = false;
        return;
    }
    bool paused = pausedNow, pull = pulling;
    UINT32 col = !on ? MUTED : paused ? ORANGE : pull ? GREEN : ACCENT;
    swprintf_s(sub, L"%s  %d/%d", !on ? L"OFF" : paused ? L"PAUSED" : pull ? L"PULLING" : L"READY", cur + 1, n);
    RECT m = GameMonitor();
    if (shown && m.left == atX && m.top == atY && col == hudColor && !wcscmp(name, hudName) && !wcscmp(sub, hudSub)) return;
    shown = true;
    atX = m.left, atY = m.top;
    hudColor = col;
    wcscpy_s(hudName, name);
    wcscpy_s(hudSub, sub);
    float s = GetDpiForSystem() / 96.f;
    DrawLayered(hudWnd, m.left + (int)(16 * s), m.top + (int)(16 * s), HUD_W, HUD_H, DrawHud);
}

// Timer 2 (~60 Hz): open/update/close the wheel, fade out the toast, keep the HUD current, blink the caret.
static void OverlayTick() {
    ULONGLONG now = GetTickCount64();
    if (CaretVisible() != caretShown) {
        caretShown = !caretShown;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (toastAt && now - toastAt > 1800) {
        ShowWindow(toastWnd, SW_HIDE);
        toastAt = 0;
    } else if (toastAt && now - toastAt > 1500) {
        BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(255 * (1800 - (now - toastAt)) / 300), AC_SRC_ALPHA};
        UpdateLayeredWindow(toastWnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
    }

    UpdateHud();

    int key, n;
    {
        std::lock_guard<std::mutex> l(mu);
        key = cfg.wheelKey;
        n = wheelN = SwitchTargets(cfg, wheelIdx);
    }
    static bool wheelLatch; // picked with a click: stays shut until the key is let go
    bool held = Down(key) && !capturing;
    if (!held) wheelLatch = false;
    if (held && !wheelOpen && !wheelLatch) {
        wheelOpen = wheelShown = true;
        wheelStep = 0;
        wheelClick = false;
        RawMouse(true);
        wheelX = wheelY = wheelOpenT = 0;
        wheelSel = -1;
        std::fill(std::begin(wheelHl), std::end(wheelHl), 0.f);
    }
    if (!wheelOpen) return;
    if (int st = wheelStep.exchange(0)) { // scrolling steps the pick, as if the mouse had moved that way
        int k = wheelSel;
        if (k < 0) { // start from the current profile's slice (or just before the first)
            std::lock_guard<std::mutex> l(mu);
            k = st > 0 ? -1 : 0;
            for (int i = 0; i < n; i++)
                if (wheelIdx[i] == cfg.cur) k = i;
        }
        k = ((k + st) % n + n) % n;
        wheelX = 60 * std::sin(k * TAU / n);
        wheelY = -60 * std::cos(k * TAU / n);
    }
    float len = std::hypot(wheelX, wheelY);
    wheelSel = -1;
    if (len > 25) { // slice 0 at the top, clockwise
        float a = std::atan2(wheelX, -wheelY) + TAU + TAU / 2 / n;
        wheelSel = (int)(a / (TAU / n)) % n;
    }
    bool click = wheelClick.exchange(false);
    if (held && !click) {
        wheelOpenT = std::min(1.f, wheelOpenT + .16f); // ~100 ms
        for (int i = 0; i < n; i++) wheelHl[i] += ((i == wheelSel) - wheelHl[i]) * .35f;
        RECT m = GameMonitor();
        float s = GetDpiForSystem() / 96.f;
        int half = (int)(WHEEL_SIZE * s / 2);
        DrawLayered(wheelWnd, (m.left + m.right) / 2 - half, (m.top + m.bottom) / 2 - half, WHEEL_SIZE, WHEEL_SIZE,
                    DrawWheel, (BYTE)(255 * wheelOpenT));
        return;
    }
    wheelLatch = click;
    wheelOpen = wheelShown = false;
    RawMouse(false);
    ShowWindow(wheelWnd, SW_HIDE);
    if (wheelSel < 0) return;
    wchar_t msg[64];
    bool notify, sound;
    {
        std::lock_guard<std::mutex> l(mu);
        cfg.cur = wheelIdx[std::min(wheelSel, wheelN - 1)];
        ProfileLabel(cfg, cfg.cur, msg, std::size(msg));
        notify = cfg.notify;
        sound = cfg.sound;
    }
    Save();
    InvalidateRect(hwnd, nullptr, FALSE);
    if (notify) Notify(msg, ACCENT);
    if (sound) Chime(EV_PROFILE);
}

static float Scale() { return 96.f / GetDpiForWindow(hwnd); } // pixels -> DIPs

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: { // drag the window by the header, except over the pill and buttons
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        float s = Scale(), x = pt.x * s, y = pt.y * s;
        bool overPill = x >= PILL.left && x < PILL.right && y >= PILL.top && y < PILL.bottom;
        bool overUpdate = x >= UPDATE_BTN.left && x < UPDATE_BTN.right && y >= UPDATE_BTN.top && y < UPDATE_BTN.bottom;
        bool overTabs = x >= TABS.left && x < TABS.right && y >= TABS.top && y < TABS.bottom;
        return y < HEADER && x < WIN_W - 100 && !overPill && !overUpdate && !overTabs ? HTCAPTION : HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        float s = Scale();
        in.mx = GET_X_LPARAM(lp) * s;
        in.my = GET_Y_LPARAM(lp) * s;
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h};
        TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!in.down) in.mx = in.my = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: // stands in for the second click's down, so it still presses whatever is under it
        in.down = in.pressed = true;
        in.dbl = msg == WM_LBUTTONDBLCLK;
        in.px = GET_X_LPARAM(lp) * Scale(); // the frame may run after the mouse has moved on (fast drag-select)
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        in.down = false;
        ReleaseCapture();
        saveReq = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_RBUTTONDOWN:
        in.rpressed = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        in.wheel += GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) != HTCLIENT) break;
        SetCursor(LoadCursor(nullptr, cursor));
        return TRUE;

    case WM_CHAR: { // typing into a text field (profile name, COM port or a slider value)
        if (!editTarget) break;
        {
            std::lock_guard<std::mutex> l(mu);
            if (wp == VK_RETURN || wp == VK_ESCAPE) {
                numCancel = wp == VK_ESCAPE;
                editTarget = nullptr;
            } else {
                EditChar((wchar_t)wp);
            }
        }
        SyncCapture();
        saveReq = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: { // caret keys, Delete, Ctrl+A/C/X/V in a text field
        if (!editTarget) break;
        bool changed;
        {
            std::lock_guard<std::mutex> l(mu);
            changed = EditKey((int)wp);
        }
        if (changed) saveReq = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    // Key binding: wait for every key to be released (the click that started it), then take the next one.
    case WM_TIMER: {
        if (wp == 2) {
            OverlayTick();
            return 0;
        }
        if (wp == 3) {
            KillTimer(h, 3);
            Save();
            return 0;
        }
        int pressed = 0;
        for (int vk = 1; vk < 255 && !pressed; vk++)
            if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && Down(vk)) pressed = vk; // keep L/R variants only
        if (!bindArmed) {
            bindArmed = !pressed;
            return 0;
        }
        if (!pressed) return 0;
        KillTimer(h, 1);
        {
            std::lock_guard<std::mutex> l(mu);
            if (pressed == VK_BACK) *bindTarget = 0;
            else if (pressed != VK_ESCAPE) *bindTarget = pressed;
        }
        bindTarget = nullptr;
        SyncCapture();
        Save();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_APP + 1: { // engine hotkey: on/off or profile switch
        bool notify, sound;
        wchar_t msg[64];
        {
            std::lock_guard<std::mutex> l(mu);
            notify = cfg.notify;
            sound = cfg.sound;
            if (wp == EV_PROFILE || wp == EV_SLOT) ProfileLabel(cfg, cfg.cur, msg, std::size(msg));
            else wcscpy_s(msg, wp == EV_ON ? L"Enabled" : L"Disabled");
        }
        Save();
        InvalidateRect(h, nullptr, FALSE);
        if (notify) Notify(msg, wp == EV_ON ? GREEN : wp == EV_OFF ? RED : ACCENT);
        if (sound) Chime((int)wp);
        return 0;
    }
    case WM_INPUT: { // raw mouse motion steers the wheel, even while a game has the cursor locked
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (wheelOpen && GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            wheelX += ri.data.mouse.lLastX;
            wheelY += ri.data.mouse.lLastY;
            float len = std::hypot(wheelX, wheelY);
            if (len > 100) { // cap so changing direction responds right away
                wheelX *= 100 / len;
                wheelY *= 100 / len;
            }
        }
        break;
    }
    case WM_APP + 2: { // update installed: save, start the new exe, quit
        KillTimer(h, 3);
        Save();
        std::wstring self = SelfPath();
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(self.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        DestroyWindow(h);
        return 0;
    }
    case WM_APP + 3: { // startup check found a newer version
        bool notify;
        { std::lock_guard<std::mutex> l(mu); notify = cfg.notify; }
        InvalidateRect(h, nullptr, FALSE);
        if (notify) Notify(L"Update available", ACCENT);
        return 0;
    }
    case WM_APP: // engine started/stopped pulling, or the update state changed
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        Paint();
        EndPaint(h, &ps);
        if (saveReq) { // saved once things settle, not on every click, scroll step or keystroke
            saveReq = false;
            SetTimer(h, 3, 500, nullptr);
            InvalidateRect(h, nullptr, FALSE); // the change happened mid-frame: draw it now, not on the next mouse move
        }
        if (*toastReq) {
            Notify(toastReq, toastReqColor);
            *toastReq = 0;
        }
        if (headerCmd == 1) ShowWindow(h, SW_MINIMIZE);
        if (headerCmd == 2) DestroyWindow(h);
        headerCmd = 0;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (hrt) hrt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        if (hrt) hrt->SetDpi(HIWORD(wp), HIWORD(wp));
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        if (KillTimer(h, 3)) Save(); // a save was still pending
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* f;
    dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wcsrchr(ini, L'\\')[1] = 0;
    wcscat_s(ini, L"settings.ini");
    LoadSettings();

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
    fTitle = Font(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fSection = Font(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fLabel = Font(13, DWRITE_FONT_WEIGHT_NORMAL);
    fValue = Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fSmall = Font(12, DWRITE_FONT_WEIGHT_NORMAL);
    fPill = Font(11, DWRITE_FONT_WEIGHT_BOLD);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.style = CS_DBLCLKS; // double-click selects a word in text fields
    wc.hInstance = inst;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"MouseDown";
    RegisterClassW(&wc);

    float s = GetDpiForSystem() / 96.f;
    int w = (int)(WIN_W * s), hgt = (int)(WIN_H * s);
    hwnd = CreateWindowW(L"MouseDown", L"Mouse Down", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                         (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2, w, hgt,
                         nullptr, nullptr, inst, nullptr);
    DWORD corners = 2; // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corners, sizeof corners);
    COLORREF border = RGB(0x25, 0x2A, 0x35);
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof border);
    ShowWindow(hwnd, show);

    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"MouseDownOverlay";
    RegisterClassW(&wc);
    for (HWND* w : {&wheelWnd, &toastWnd, &hudWnd}) // click-through, never takes focus from the game
        *w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"MouseDownOverlay", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    SetTimer(hwnd, 2, 16, nullptr);

    std::thread(HookThread).detach();
    std::thread(Engine, hwnd).detach();
    std::thread([] { // the exe an update replaced has exited by now; then see if there's a newer one
        Sleep(3000);
        DeleteFileW((SelfPath() + L".old").c_str());
        if (updState != UPD_IDLE) return; // the button got there first
        if (FindUpdate()) PostMessageW(hwnd, WM_APP + 3, 0, 0);
        else if (updState == UPD_FAILED) updState = UPD_IDLE; // offline at startup isn't worth a red button
        PostMessageW(hwnd, WM_APP, 0, 0);
    }).detach();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
