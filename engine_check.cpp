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
    puts("engine ok");
}
