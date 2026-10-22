// Self-check for the pull / auto spam / growth logic. Build and run from an x64 Native Tools prompt:
//   cl /std:c++17 /utf-8 /EHsc engine_check.cpp /link /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup && .\engine_check
#define wWinMain MouseDownMain
#include "MouseDown.cpp"
#include <cassert>

int main() {
    bool r;
    // Tap: pressing the combo starts a fresh pull; releasing does not stop it
    Trigger t;
    assert(Step(t, 2, true, true, false, true, false, r) && r);
    assert(Step(t, 2, false, false, false, true, true, r) && !r);
    // Tap + linked spam already pulling: a new combo edge must not restart (growth keeps counting)
    Trigger s;
    assert(Step(s, 2, false, false, true, true, true, r) && !r);
    assert(Step(s, 2, true, true, true, true, true, r) && !r);
    // Hold: linked spam keeps the pull on without the profile keys
    Trigger h;
    assert(Step(h, 0, false, true, true, true, false, r));
    assert(!Step(h, 0, false, true, false, true, true, r));
    // Toggle: first press latches, letting go of the required key drops it, spam overrides
    Trigger g;
    assert(Step(g, 1, true, true, false, true, false, r));
    assert(!Step(g, 1, false, false, false, true, true, r));
    assert(Step(g, 1, false, false, true, true, false, r));
    // Disabled / capturing blocks everything, spam included
    Trigger d;
    assert(!Step(d, 0, true, true, true, false, false, r));

    // Growth stacks on top of stages and steps every growEvery ms after the start delay
    Profile p;
    p.vy = 100, p.vx = 0, p.at2 = 500, p.vy2 = 200, p.vx2 = 10;
    p.growEvery = 100, p.growY = 5, p.growX = -1, p.startDelay = 50;
    double tx, ty;
    Target(p, 1, 40, tx, ty);
    assert(tx == 0 && ty == 0);                          // still in start delay
    Target(p, 1, 50 + 250, tx, ty);
    assert(ty == 100 + 2 * 5 && tx == 2 * -1);           // stage 1, 2 growth steps
    Target(p, 1, 50 + 600, tx, ty);
    assert(ty == 200 + 6 * 5 && tx == 10 + 6 * -1);      // stage 2, 6 growth steps
    // Duration cuts the pull; with duration 0 (what spam-linked Tap uses) it keeps going
    p.duration = 300;
    Target(p, 1, 50 + 600, tx, ty);
    assert(tx == 0 && ty == 0);
    p.duration = 0;
    Target(p, 1, 50 + 5000, tx, ty);
    assert(ty == 200 + 50 * 5);
    // Switched-off features read as 0 for the engine, but keep their value
    p.growEvery = 100, p.off[8] = true;
    Profile e = Effective(p);
    assert(e.growEvery == 0 && p.growEvery == 100);
    Target(e, 1, 50 + 600, tx, ty);
    assert(ty == 200);

    // Horizontal sway: ±sway px/s on x, one full cycle per swayMs
    auto approx = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    Profile w;
    w.vy = 0, w.vx = 0, w.sway = 50, w.swayMs = 400;
    Target(w, 1, 100, tx, ty);
    assert(approx(tx, 50) && ty == 0);      // quarter cycle: peak +
    Target(w, 1, 300, tx, ty);
    assert(approx(tx, -50));                // three-quarter cycle: peak -
    Target(w, 1, 200, tx, ty);
    assert(approx(tx, 0));                  // half cycle: zero crossing
    w.off[9] = true;                        // the switch zeroes sway, keeping its value
    Profile we = Effective(w);
    Target(we, 1, 100, tx, ty);
    assert(tx == 0 && we.sway == 0 && w.sway == 50);

    // Clipboard text round-trips a profile; the target keeps its own hotkey, junk is rejected
    Profile a, b;
    a.vy = 123, a.sway = 40, a.off[9] = true, a.mode = 2, a.holdKey = VK_LBUTTON, a.hotkey = 5;
    wcscpy_s(a.name, L"A;b=c");
    b.hotkey = 7;
    assert(ImportProfile(ExportProfile(a).c_str(), b));
    assert(b.vy == 123 && b.sway == 40 && b.off[9] && b.mode == 2 && b.holdKey == VK_LBUTTON);
    assert(!wcscmp(b.name, L"A;b=c") && b.hotkey == 7);
    assert(!ImportProfile(L"garbage", b) && b.vy == 123);

    // Groups: slots follow their profiles through inserts, moves and deletes; empty, out-of-range and
    // nested (group) slots run nothing
    Config gc;
    gc.n = 3;
    gc.p[2].group = true, gc.p[2].child[0] = 0, gc.p[2].child[1] = 1;
    InsertProfile(gc, 1, Profile{});
    assert(gc.n == 4 && gc.p[3].group && gc.p[3].child[0] == 0 && gc.p[3].child[1] == 2);
    SwapProfiles(gc, 0, 3);
    assert(gc.p[0].group && gc.p[0].child[0] == 3 && gc.p[0].child[1] == 2);
    DeleteProfile(gc, 2);
    assert(gc.n == 3 && gc.p[0].child[0] == 2 && gc.p[0].child[1] == -1);
    assert(SlotProfile(gc, gc.p[0], 0) == 2 && SlotProfile(gc, gc.p[0], 1) == -1);
    gc.p[0].child[1] = 7;
    assert(SlotProfile(gc, gc.p[0], 1) == -1);
    gc.p[2].group = true;
    assert(SlotProfile(gc, gc.p[0], 0) == -1);

    // Switching targets: everything, or only the groups when set to groups (unless there are none)
    int tg[MAX_PROFILES];
    assert(SwitchTargets(gc, tg) == 3 && StepTarget(gc, 2, 1) == 0 && StepTarget(gc, 0, -1) == 2);
    gc.switchGroups = true; // groups are 0 and 2
    assert(SwitchTargets(gc, tg) == 2 && tg[0] == 0 && tg[1] == 2);
    assert(StepTarget(gc, 0, 1) == 2 && StepTarget(gc, 2, 1) == 0 && StepTarget(gc, 0, -1) == 2);
    assert(StepTarget(gc, 1, 1) == 0 && StepTarget(gc, 1, -1) == 2); // from a normal profile: first / last group
    gc.p[0].group = gc.p[2].group = false;
    assert(SwitchTargets(gc, tg) == 3);

    // The list shows everything, only profiles or only groups; the active one always shows
    gc.p[1].group = true, gc.cur = 0;
    int vg[MAX_PROFILES];
    gc.listShow = 0;
    assert(VisibleProfiles(gc, vg) == 3);
    gc.listShow = 1;
    assert(VisibleProfiles(gc, vg) == 2 && vg[0] == 0 && vg[1] == 2);
    gc.listShow = 2;
    assert(VisibleProfiles(gc, vg) == 2 && vg[0] == 0 && vg[1] == 1); // 0 is a profile, but it's the active one

    // New profiles hold right mouse and also require left mouse, for the pull and for auto spam (which starts off)
    assert(Profile{}.holdKey == VK_RBUTTON && Profile{}.requireKey == VK_LBUTTON &&
           Profile{}.spamTrigger == VK_RBUTTON && Profile{}.spamRequire == VK_LBUTTON && !Profile{}.spamOn);

    // Settings survive a save and load, non-ASCII names and deleted profiles included
    GetTempPathW(MAX_PATH, ini);
    wcscat_s(ini, L"engine_check.ini");
    cfg.n = 3, cfg.cur = 2, cfg.prevKey = VK_F6, cfg.accent = 3;
    cfg.p[1] = a;
    cfg.p[1].spamJitter = 12, cfg.p[1].spamKey = VK_LBUTTON;
    cfg.p[0].group = true, cfg.p[0].child[0] = 1, cfg.p[0].childKey[1] = '2', cfg.p[0].childSel = 1;
    wcscpy_s(cfg.p[0].name, L"Spray ü");
    wcscpy_s(cfg.port, L"COM7");
    Save();
    cfg.n = 2;
    Save();
    wchar_t gone[16];
    GetPrivateProfileStringW(L"profile3", L"name", L"none", gone, 16, ini);
    assert(!wcscmp(gone, L"none")); // the deleted profile's section is gone
    cfg = Config{};
    LoadSettings();
    assert(cfg.n == 2 && cfg.cur == 1 && cfg.prevKey == VK_F6 && cfg.accent == 3);
    assert(cfg.p[1].vy == 123 && cfg.p[1].off[9] && !wcscmp(cfg.p[1].name, L"A;b=c") && !wcscmp(cfg.port, L"COM7"));
    assert(cfg.p[1].spamJitter == 12 && cfg.p[1].spamKey == VK_LBUTTON && cfg.p[0].spamKey == 0); // spam is per profile
    assert(!wcscmp(cfg.p[0].name, L"Spray ü"));
    assert(cfg.p[0].group && cfg.p[0].child[0] == 1 && cfg.p[0].child[1] == -1 && cfg.p[0].childKey[1] == '2' &&
           cfg.p[0].childSel == 1 && !cfg.p[1].group);
    // Auto spam saved before it was per profile: a profile without its own starts from the old shared setting
    WritePrivateProfileStringW(L"general", L"spamKey", L"1", ini);
    WritePrivateProfileStringW(L"general", L"spamRate", L"25", ini);
    WritePrivateProfileStringW(L"general", L"spamOn", L"1", ini);
    WritePrivateProfileStringW(L"profile1", L"spamOn", nullptr, ini);
    WritePrivateProfileStringW(L"profile1", L"spamKey", nullptr, ini);
    WritePrivateProfileStringW(L"profile1", L"spamRate", nullptr, ini);
    cfg = Config{};
    LoadSettings();
    assert(cfg.p[0].spamKey == VK_LBUTTON && cfg.p[0].spamRate == 25 && cfg.p[1].spamRate == 10);
    assert(!cfg.p[0].spamOn); // but spam itself starts off
    DeleteFileW(ini);
    puts("engine ok");
}
