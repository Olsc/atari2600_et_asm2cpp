#pragma once
// -----------------------------------------------------------------------------
// Native Atari 2600 hardware support for the statically translated E.T. game.
//
// This is NOT a 6502 emulator.  The game logic is compiled to C++ by
// tools/transpile.py; this file only models the TIA (video/audio/collisions),
// the RIOT (RAM/input/interval timer) and the F8 cartridge bank switch.
// -----------------------------------------------------------------------------
#include <stdint.h>
#include <stddef.h>

namespace et {
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s8  = int8_t;
using s16 = int16_t;

// 6502 architectural state.  The generated native code uses these directly.
extern u8 A, X, Y, S;
extern bool C, Z, I, D, V, N;
extern u8 ram[128];
extern int bank;              // 0 = bank 0 ($B000), 1 = bank 1 ($F000)

void reset_cpu();
void run_game();
[[noreturn]] void fatal(const char* msg);

u8   read8(u16 addr);
void write8(u16 addr, u8 value);
void store8(u16 addr, u8 value);   // a write on the final cycle of an instruction
void tick(int cpu_cycles);         // advance TIA + RIOT by whole CPU cycles
void last_cycle();                 // advance the final CPU cycle of an instruction
void push8(u8 value);
u8   pull8();

// ---- 6502 flag / ALU helpers ------------------------------------------------
inline void set_nz(u8 v) { N = (v & 0x80) != 0; Z = (v == 0); }

inline void cmp(u8 reg, u8 v) {
  u16 d = (u16)reg - v;
  C = reg >= v;
  set_nz((u8)d);
}

inline u8 asl_(u8 v) { C = (v & 0x80) != 0; v = (u8)(v << 1); set_nz(v); return v; }
inline u8 lsr_(u8 v) { C = (v & 0x01) != 0; v = (u8)(v >> 1); set_nz(v); return v; }
inline u8 rol_(u8 v) { bool c = C; C = (v & 0x80) != 0; v = (u8)((v << 1) | (c ? 1 : 0)); set_nz(v); return v; }
inline u8 ror_(u8 v) { bool c = C; C = (v & 0x01) != 0; v = (u8)((v >> 1) | (c ? 0x80 : 0)); set_nz(v); return v; }

inline void adc(u8 v) {
  if (D) {
    u16 al = (u16)(A & 0x0F) + (v & 0x0F) + (C ? 1 : 0);
    u16 ah = (u16)(A >> 4) + (v >> 4);
    if (al > 9) { al += 6; ah++; }
    // The low-nibble carry into the high nibble
    u16 binary = (u16)A + v + (C ? 1 : 0);
    C = binary > 0xFF;
    V = ((~(A ^ v) & (A ^ (u8)binary)) & 0x80) != 0;
    if (ah > 9) ah += 6;
    A = (u8)(((ah & 0x0F) << 4) | (al & 0x0F));
    set_nz(A);
  } else {
    u16 sum = (u16)A + v + (C ? 1 : 0);
    C = sum > 0xFF;
    V = ((~(A ^ v) & (A ^ (u8)sum)) & 0x80) != 0;
    A = (u8)sum;
    set_nz(A);
  }
}

inline void sbc(u8 v) {
  if (D) {
    u16 diff = (u16)A - v - (C ? 0 : 1);
    int al = (A & 0x0F) - (v & 0x0F) - (C ? 0 : 1);
    int ah = (A >> 4) - (v >> 4);
    if (al < 0) { al -= 6; ah--; }
    if (ah < 0) ah -= 6;
    C = diff < 0x100;
    V = (((A ^ v) & (A ^ (u8)diff)) & 0x80) != 0;
    A = (u8)(((ah & 0x0F) << 4) | (al & 0x0F));
    set_nz(A);
  } else {
    u16 diff = (u16)A - v - (C ? 0 : 1);
    C = diff < 0x100;
    V = (((A ^ v) & (A ^ (u8)diff)) & 0x80) != 0;
    A = (u8)diff;
    set_nz(A);
  }
}

inline u8 pack_flags(bool break_flag) {
  return (u8)((C ? 0x01 : 0) | (Z ? 0x02 : 0) | (I ? 0x04 : 0) |
              (D ? 0x08 : 0) | 0x20 | (break_flag ? 0x10 : 0) |
              (V ? 0x40 : 0) | (N ? 0x80 : 0));
}
inline void unpack_flags(u8 p) {
  C = (p & 0x01) != 0; Z = (p & 0x02) != 0; I = (p & 0x04) != 0;
  D = (p & 0x08) != 0; V = (p & 0x40) != 0; N = (p & 0x80) != 0;
}

// ---- Input -----------------------------------------------------------------
struct InputState {
  // Player 0 joystick (active high in this struct; converted to active-low
  // SWCHA bits on read).
  volatile bool up = false, down = false, left = false, right = false;
  volatile bool fire = false;
  volatile bool reset = false, select = false;
  volatile bool left_diff_a = false;   // false = B, true = A
  volatile bool right_diff_a = false;  // false = B, true = A
  volatile bool color = true;          // true = COLOR, false = B/W
};
extern InputState input;

// ---- Video / audio output ---------------------------------------------------
struct Frame {
  static const int W = 160;
  static const int H = 210;
  u8 pixels[W * H];                   // TIA colour indices
  s16 audio[2048];                    // interleaved stereo, 31400 Hz
  int audio_samples = 0;              // stereo sample frames
};

// Frame producer/consumer.  The native game runs in its own thread and blocks
// at each vertical sync until the frontend has consumed the previous frame.
void start_game();
void wait_frame(Frame& out);
void stop_game();
bool game_finished();

// Run one frame synchronously (used by tests; not by the frontend).
void run_one_frame(Frame& out);

// TIA colour index -> RGB565 palette (NTSC).
extern const u16 ntsc_palette[256];

} // namespace et
