// Firmware: turns an ATmega32U4 board (Arduino Leonardo / Pro Micro) into a
// real USB mouse driven over serial. The PC side (MouseDown) sends 3-byte
// packets: 0x80, dx, dy  (dx/dy are int8, clamped to -127..127).
// 0x80 can never appear as a data byte, so it re-syncs the stream after any
// disconnect. Flash with the Arduino IDE, board = Leonardo or Pro Micro.
//
// Pico/RP2040: swap Mouse.h for the arduino-pico core's Mouse (same API),
// everything else is identical.
#include <Mouse.h>

void setup() {
    Serial.begin(115200); // CDC ignores the rate; any value works
    Mouse.begin();
}

void loop() {
    static int8_t buf[2];
    static uint8_t n = 0;
    static bool sync = false;
    while (Serial.available()) {
        uint8_t b = Serial.read();
        if (!sync) {              // wait for the 0x80 frame marker
            if (b == 0x80) { sync = true; n = 0; }
            continue;
        }
        buf[n++] = (int8_t)b;
        if (n == 2) { Mouse.move(buf[0], buf[1]); sync = false; }
    }
}
