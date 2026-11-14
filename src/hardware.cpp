#include "hardware.h"
#include "rom_data.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace et {

u8 A = 0, X = 0, Y = 0, S = 0xFF;
bool C = false, Z = false, I = true, D = false, V = false, N = false;
u8 ram[128];
int bank = 1;
InputState input;

namespace {

struct GameExit {};

// ---------------------------------------------------------------------------
// TIA state
// ---------------------------------------------------------------------------
struct Tia {
  u8 colup0 = 0, colup1 = 0, colupf = 0, colubk = 0;
  u8 nusiz0 = 0, nusiz1 = 0, ctrlpf = 0;
  u8 refp0 = 0, refp1 = 0;
  u8 vdelp0 = 0, vdelp1 = 0, vdelbl = 0;
  u8 grp0 = 0, grp1 = 0, dgrp0 = 0, dgrp1 = 0, grp0_disp = 0, grp1_disp = 0;
  u8 pf0 = 0, pf1 = 0, pf2 = 0;
  u8 enam0 = 0, enam1 = 0, enabl = 0;
  u8 hmp0 = 0, hmp1 = 0, hmm0 = 0, hmm1 = 0, hmbl = 0;
  u8 pos_p0 = 0, pos_p1 = 0, pos_m0 = 0, pos_m1 = 0, pos_bl = 0;
  u8 vblank = 0, vsync = 0;
  u8 cx[8] = {0,0,0,0,0,0,0,0};
  u8 audc[2] = {0,0}, audf[2] = {0,0}, audv[2] = {0,0};

  int clock = 0;                 // colour clock within scanline, 0..227
  int line = 0;                  // scanline, 0..261
  bool in_vsync = false;
  bool have_frame = false;

  u8 fb[Frame::W * Frame::H];

  // Audio
  int audio_accum = 0;
  int div_cnt[2] = {0,0};
  int lfsr[2] = {1,1};
  int phase[2] = {0,0};
  s16 audio[2048];
  int audio_count = 0;
};

Tia tia;

// ---------------------------------------------------------------------------
// RIOT (6532) state
// ---------------------------------------------------------------------------
struct Riot {
  u8 timer_value = 0;
  int timer_period = 1;
  int timer_count = 1;
};
Riot riot;

// ---------------------------------------------------------------------------
// Thread/frame hand-off
// ---------------------------------------------------------------------------
Frame g_shared;
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_frame_ready = false;
bool g_quit = false;
bool g_finished = false;
bool g_started = false;
std::thread g_thread;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline int wrap160(int v) {
  v %= 160;
  if (v < 0) v += 160;
  return v;
}

inline bool is_tia_addr(u16 a) {
  return (a & 0x1080) == 0x0000; // A12=0, A7=0: TIA mirrors at $0000-$007F, $0100-$017F
}

inline bool is_ram_addr(u16 a) {
  return (a & 0x1080) == 0x0080 && (a & 0x0200) == 0;
}

inline bool is_riot_addr(u16 a) {
  return (a & 0x1080) == 0x0080 && (a & 0x0200) != 0;
}

inline void riot_tick(int cycles) {
  while (cycles-- > 0) {
    if (--riot.timer_count <= 0) {
      riot.timer_count = riot.timer_period;
      if (riot.timer_value > 0) riot.timer_value--;
    }
  }
}

inline void audio_sample() {
  int mix = 0;
  for (int ch = 0; ch < 2; ++ch) {
    u8 audv = tia.audv[ch];
    if (audv == 0) continue;
    u8 audc = tia.audc[ch];
    u8 audf = tia.audf[ch];
    if (--tia.div_cnt[ch] <= 0) {
      tia.div_cnt[ch] = audf + 1;
      if ((audc & 0x0C) == 0x0C) tia.div_cnt[ch] *= 3;
      int l = tia.lfsr[ch];
      int bit = ((l >> 0) ^ (l >> 2)) & 1;
      tia.lfsr[ch] = (l >> 1) | (bit << 3);
      tia.phase[ch] ^= 1;
    }
    bool out;
    switch (audc & 0x0F) {
      case 0:  out = true; break;                         // volume only
      case 8: case 15: out = (tia.lfsr[ch] & 1) != 0; break; // noise
      case 1: case 3:  out = (tia.lfsr[ch] & 2) != 0; break; // saw / engine
      default: out = tia.phase[ch] != 0; break;           // square-ish
    }
    mix += (out ? 1 : -1) * (int)audv * 700;
  }
  if (mix > 32767) mix = 32767;
  if (mix < -32768) mix = -32768;
  if (tia.audio_count < 1024) {
    tia.audio[tia.audio_count * 2]     = (s16)mix;
    tia.audio[tia.audio_count * 2 + 1] = (s16)mix;
    tia.audio_count++;
  }
}

inline bool pf_bit_at(int x) {
  int idx = (x % 80) / 4;
  if (x >= 80 && (tia.ctrlpf & 0x01)) idx = 19 - idx;
  int bit;
  if (idx < 4)       bit = (tia.pf0 >> (4 + idx)) & 1;
  else if (idx < 12) bit = (tia.pf1 >> (7 - (idx - 4))) & 1;
  else               bit = (tia.pf2 >> (idx - 12)) & 1;
  return bit != 0;
}

struct Copy { int off; int size; };

inline int copy_list(int mode, Copy out[3]) {
  switch (mode & 7) {
    case 0: out[0] = {0, 1}; return 1;
    case 1: out[0] = {0, 1}; out[1] = {16, 1}; return 2;
    case 2: out[0] = {0, 1}; out[1] = {32, 1}; return 2;
    case 3: out[0] = {0, 1}; out[1] = {16, 1}; out[2] = {32, 1}; return 3;
    case 4: out[0] = {0, 1}; out[1] = {64, 1}; return 2;
    case 5: out[0] = {0, 2}; return 1;
    case 6: out[0] = {0, 1}; out[1] = {32, 1}; out[2] = {64, 1}; return 3;
    default: out[0] = {0, 4}; return 1;
  }
}

inline bool player_bit(int idx, int x) {
  u8 nusiz = idx ? tia.nusiz1 : tia.nusiz0;
  u8 grp   = idx ? tia.grp1_disp : tia.grp0_disp;
  int pos  = idx ? tia.pos_p1 : tia.pos_p0;
  if (grp == 0) return false;
  Copy c[3];
  int n = copy_list(nusiz, c);
  bool refl = (idx ? tia.refp1 : tia.refp0) & 0x08;
  for (int k = 0; k < n; ++k) {
    int start = wrap160(pos + c[k].off);
    int width = 8 * c[k].size;
    if (x >= start && x < start + width && x < Frame::W) {
      int bi = (x - start) / c[k].size;
      int bit = refl ? bi : (7 - bi);
      if ((grp >> bit) & 1) return true;
    }
  }
  return false;
}

inline bool missile_bit(int idx, int x) {
  u8 enam = idx ? tia.enam1 : tia.enam0;
  if (!(enam & 0x02)) return false;
  u8 nusiz = idx ? tia.nusiz1 : tia.nusiz0;
  int pos  = idx ? tia.pos_m1 : tia.pos_m0;
  int width = 1 << ((nusiz >> 4) & 3);
  Copy c[3];
  int n = copy_list(nusiz, c);
  for (int k = 0; k < n; ++k) {
    int start = wrap160(pos + c[k].off);
    if (x >= start && x < start + width && x < Frame::W) return true;
  }
  return false;
}

inline bool ball_bit(int x) {
  if (!(tia.enabl & 0x02)) return false;
  int width = 1 << ((tia.ctrlpf >> 4) & 3);
  int start = tia.pos_bl;
  return x >= start && x < start + width && x < Frame::W;
}

inline u8 compute_pixel(int x, bool& pf, bool& p0, bool& p1, bool& m0, bool& m1, bool& bl) {
  pf = pf_bit_at(x);
  p0 = player_bit(0, x);
  p1 = player_bit(1, x);
  m0 = missile_bit(0, x);
  m1 = missile_bit(1, x);
  bl = ball_bit(x);

  // Collision latches (only the bits this game reads are essential, but all
  // standard TIA collision bits are maintained).
  if (p0 && p1) tia.cx[7] |= 0x80;
  if (m0 && m1) tia.cx[7] |= 0x40;
  if (m0 && p1) tia.cx[0] |= 0x80;
  if (m0 && p0) tia.cx[0] |= 0x40;
  if (m1 && p0) tia.cx[1] |= 0x80;
  if (m1 && p1) tia.cx[1] |= 0x40;
  if (p0 && pf) tia.cx[2] |= 0x80;
  if (p0 && bl) tia.cx[2] |= 0x40;
  if (p1 && pf) tia.cx[3] |= 0x80;
  if (p1 && bl) tia.cx[3] |= 0x40;
  if (m0 && pf) tia.cx[4] |= 0x80;
  if (m0 && bl) tia.cx[4] |= 0x40;
  if (m1 && pf) tia.cx[5] |= 0x80;
  if (m1 && bl) tia.cx[5] |= 0x40;
  if (bl && pf) tia.cx[6] |= 0x80;

  u8 pf_color = tia.colupf;
  if (tia.ctrlpf & 0x02) pf_color = (x < 80) ? tia.colup0 : tia.colup1;

  bool pf_priority = (tia.ctrlpf & 0x04) != 0;
  if (pf && pf_priority) return pf_color;
  if (p0) return tia.colup0;
  if (p1) return tia.colup1;
  if (m0) return tia.colup0;
  if (m1) return tia.colup1;
  if (bl) return tia.colupf;
  if (pf) return pf_color;
  return tia.colubk;
}

inline void render_current_clock() {
  int x = tia.clock - 68;
  if (x < 0 || x >= Frame::W) return;
  int y = tia.line - 34;
  if (y < 0 || y >= Frame::H) return;
  u8 color = 0;
  if (!(tia.vblank & 0x02) && !(tia.vsync & 0x02)) {
    bool pf, p0, p1, m0, m1, bl;
    color = compute_pixel(x, pf, p0, p1, m0, m1, bl);
  }
  tia.fb[y * Frame::W + x] = color;
}

inline void on_new_line() {
  if (tia.line >= 262) tia.line = 0;
}

inline void tia_tick_colors(int colors) {
  while (colors-- > 0) {
    render_current_clock();
    if (++tia.audio_accum >= 114) {
      tia.audio_accum -= 114;
      audio_sample();
    }
    if (++tia.clock >= 228) {
      tia.clock = 0;
      tia.line++;
      on_new_line();
    }
  }
}

inline void submit_frame_from_game() {
  std::unique_lock<std::mutex> lock(g_mutex);
  if (g_quit) throw GameExit{};
  memcpy(g_shared.pixels, tia.fb, sizeof(tia.fb));
  g_shared.audio_samples = tia.audio_count;
  if (tia.audio_count > 0)
    memcpy(g_shared.audio, tia.audio, sizeof(s16) * 2 * tia.audio_count);
  tia.audio_count = 0;
  g_frame_ready = true;
  g_cv.notify_all();
  g_cv.wait(lock, [] { return !g_frame_ready || g_quit; });
  if (g_quit) throw GameExit{};
}

// ---------------------------------------------------------------------------
// TIA register read/write
// ---------------------------------------------------------------------------
inline void reset_pos_player(int idx) {
  int hpos = tia.clock - 68;
  int nx = hpos < -2 ? 3 : wrap160(hpos + 5);
  if (idx == 0) tia.pos_p0 = (u8)nx; else tia.pos_p1 = (u8)nx;
}

inline void reset_pos_missile(int idx) {
  int hpos = tia.clock - 68;
  int nx = hpos < -1 ? 2 : wrap160(hpos + 4);
  if (idx == 0) tia.pos_m0 = (u8)nx; else tia.pos_m1 = (u8)nx;
}

inline void reset_pos_ball() {
  int hpos = tia.clock - 68;
  tia.pos_bl = (u8)(hpos < 0 ? 2 : wrap160(hpos + 4));
}

inline int hmove_delta(u8 h) {
  return 8 - (((h ^ 0x80) >> 4) & 0x0F);
}

u8 tia_read(u8 reg) {
  reg &= 0x0F;
  if (reg <= 0x07) return tia.cx[reg];
  switch (reg) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: return 0x80; // paddles
    case 0x0C: return input.fire ? 0x00 : 0x80;             // INPT4
    case 0x0D: return 0x80;                                 // INPT5
    default: return 0;
  }
}

void tia_write(u8 addr, u8 value) {
  switch (addr & 0x3F) {
    case 0x00: // VSYNC
      tia.vsync = value;
      if (value & 0x02) {
        if (!tia.in_vsync) {
          tia.in_vsync = true;
          if (tia.have_frame) submit_frame_from_game();
          memset(tia.fb, 0, sizeof(tia.fb));
          tia.have_frame = true;
          tia.line = 0;
        }
      } else {
        tia.in_vsync = false;
      }
      break;
    case 0x01: // VBLANK
      tia.vblank = value;
      break;
    case 0x02: // WSYNC (wait handled by store8)
      break;
    case 0x03: // RSYNC
      tia.clock = 0;
      break;
    case 0x04: tia.nusiz0 = value; break;
    case 0x05: tia.nusiz1 = value; break;
    case 0x06: tia.colup0 = value; break;
    case 0x07: tia.colup1 = value; break;
    case 0x08: tia.colupf = value; break;
    case 0x09: tia.colubk = value; break;
    case 0x0A: tia.ctrlpf = value; break;
    case 0x0B: tia.refp0 = value; break;
    case 0x0C: tia.refp1 = value; break;
    case 0x0D: tia.pf0 = value; break;
    case 0x0E: tia.pf1 = value; break;
    case 0x0F: tia.pf2 = value; break;
    case 0x10: reset_pos_player(0); break;
    case 0x11: reset_pos_player(1); break;
    case 0x12: reset_pos_missile(0); break;
    case 0x13: reset_pos_missile(1); break;
    case 0x14: reset_pos_ball(); break;
    case 0x15: tia.audc[0] = value & 0x0F; break;
    case 0x16: tia.audc[1] = value & 0x0F; break;
    case 0x17: tia.audf[0] = value & 0x1F; tia.div_cnt[0] = 0; break;
    case 0x18: tia.audf[1] = value & 0x1F; tia.div_cnt[1] = 0; break;
    case 0x19: tia.audv[0] = value & 0x0F; break;
    case 0x1A: tia.audv[1] = value & 0x0F; break;
    case 0x1B: // GRP0
      tia.grp0 = value;
      tia.dgrp1 = tia.grp1;
      tia.grp0_disp = tia.vdelp0 ? tia.dgrp0 : tia.grp0;
      tia.grp1_disp = tia.vdelp1 ? tia.dgrp1 : tia.grp1;
      break;
    case 0x1C: // GRP1
      tia.grp1 = value;
      tia.dgrp0 = tia.grp0;
      tia.grp0_disp = tia.vdelp0 ? tia.dgrp0 : tia.grp0;
      tia.grp1_disp = tia.vdelp1 ? tia.dgrp1 : tia.grp1;
      break;
    case 0x1D: tia.enam0 = value; break;
    case 0x1E: tia.enam1 = value; break;
    case 0x1F: tia.enabl = value; break;
    case 0x20: tia.hmp0 = value; break;
    case 0x21: tia.hmp1 = value; break;
    case 0x22: tia.hmm0 = value; break;
    case 0x23: tia.hmm1 = value; break;
    case 0x24: tia.hmbl = value; break;
    case 0x25:
      tia.vdelp0 = value & 1;
      tia.grp0_disp = tia.vdelp0 ? tia.dgrp0 : tia.grp0;
      break;
    case 0x26:
      tia.vdelp1 = value & 1;
      tia.grp1_disp = tia.vdelp1 ? tia.dgrp1 : tia.grp1;
      break;
    case 0x27: tia.vdelbl = value & 1; break;
    case 0x28: case 0x29: break; // RESMP (unused by E.T.)
    case 0x2A: { // HMOVE
      tia.pos_p0 = (u8)wrap160(tia.pos_p0 + hmove_delta(tia.hmp0));
      tia.pos_p1 = (u8)wrap160(tia.pos_p1 + hmove_delta(tia.hmp1));
      tia.pos_m0 = (u8)wrap160(tia.pos_m0 + hmove_delta(tia.hmm0));
      tia.pos_m1 = (u8)wrap160(tia.pos_m1 + hmove_delta(tia.hmm1));
      tia.pos_bl = (u8)wrap160(tia.pos_bl + hmove_delta(tia.hmbl));
      break;
    }
    case 0x2B: // HMCLR
      tia.hmp0 = tia.hmp1 = tia.hmm0 = tia.hmm1 = tia.hmbl = 0;
      break;
    case 0x2C: // CXCLR
      memset(tia.cx, 0, sizeof(tia.cx));
      break;
    default: break;
  }
}

u8 riot_read(u8 reg) {
  switch (reg & 0x1F) {
    case 0x00: { // SWCHA
      u8 v = 0xFF;
      if (input.right) v &= ~0x80;
      if (input.left)  v &= ~0x40;
      if (input.down)  v &= ~0x20;
      if (input.up)    v &= ~0x10;
      return v;
    }
    case 0x01: return 0x00; // SWACNT
    case 0x02: { // SWCHB
      u8 v = 0x0B;
      if (input.left_diff_a)  v |= 0x40; else v &= ~0x40;
      if (input.right_diff_a) v |= 0x80; else v &= ~0x80;
      if (input.color) v |= 0x08; else v &= ~0x08;
      if (input.select) v &= ~0x02; else v |= 0x02;
      if (input.reset)  v &= ~0x01; else v |= 0x01;
      return v;
    }
    case 0x03: return 0x00; // SWBCNT
    case 0x04: case 0x06: return riot.timer_value; // INTIM
    case 0x05: case 0x07: return 0x00;             // TIMINT/INSTAT
    default: return 0;
  }
}

void riot_write(u8 reg, u8 value) {
  switch (reg & 0x1F) {
    case 0x00: case 0x01: case 0x02: case 0x03: break; // ports (not used)
    case 0x14: riot.timer_value = value; riot.timer_period = 1;    riot.timer_count = 1;    break;
    case 0x15: riot.timer_value = value; riot.timer_period = 8;    riot.timer_count = 8;    break;
    case 0x16: riot.timer_value = value; riot.timer_period = 64;   riot.timer_count = 64;   break;
    case 0x17: riot.timer_value = value; riot.timer_period = 1024; riot.timer_count = 1024; break;
    default: break;
  }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public hardware API used by the generated game
// ---------------------------------------------------------------------------
void reset_cpu() {
  A = X = Y = 0;
  S = 0xFF;
  C = Z = D = V = N = false;
  I = true;
  memset(ram, 0, sizeof(ram));
  bank = 1;

  // Reset TIA.  Keep input settings (the frontend owns those).
  memset(&tia, 0, sizeof(tia));
  tia.audio_accum = 0;
  for (int i = 0; i < 2; ++i) { tia.div_cnt[i] = 0; tia.lfsr[i] = 1; tia.phase[i] = 0; }
  memset(&riot, 0, sizeof(riot));
  riot.timer_period = 1;
  riot.timer_count = 1;
}

void fatal(const char* msg) {
  std::fprintf(stderr, "E.T. native: %s\n", msg);
  std::fflush(stderr);
  std::abort();
}

u8 read8(u16 addr) {
  // Cartridge hotspots (F8)
  if (addr == 0xFFF8) { bank = 0; return 0; }
  if (addr == 0xFFF9) { bank = 1; return 0; }
  if (addr >= 0xB000 && addr < 0xC000)
    return ROM_DATA[addr & 0x0FFF];
  if (addr >= 0xF000)
    return ROM_DATA[0x1000 + (addr & 0x0FFF)];
  if (is_ram_addr(addr))
    return ram[addr & 0x7F];
  if (is_riot_addr(addr))
    return riot_read((u8)(addr & 0x1F));
  if (is_tia_addr(addr))
    return tia_read((u8)(addr & 0x3F));
  return 0;
}

void write8(u16 addr, u8 value) {
  if (addr == 0xFFF8) { bank = 0; return; }
  if (addr == 0xFFF9) { bank = 1; return; }
  if (is_ram_addr(addr)) {
    ram[addr & 0x7F] = value;
    return;
  }
  if (is_riot_addr(addr)) {
    riot_write((u8)(addr & 0x1F), value);
    return;
  }
  if (is_tia_addr(addr)) {
    tia_write((u8)(addr & 0x3F), value);
    return;
  }
}

void tick(int cpu_cycles) {
  if (cpu_cycles <= 0) return;
  riot_tick(cpu_cycles);
  tia_tick_colors(cpu_cycles * 3);
}

void last_cycle() {
  riot_tick(1);
  tia_tick_colors(3);
}

void store8(u16 addr, u8 value) {
  bool wsync = is_tia_addr(addr) && ((addr & 0x3F) == 0x02);
  write8(addr, value);
  if (wsync) {
    // WSYNC halts the CPU until the next scanline.  The TIA keeps rendering.
    int remaining = (tia.clock == 0) ? 228 : (228 - tia.clock);
    riot_tick(remaining / 3);
    tia_tick_colors(remaining);
  }
}

void push8(u8 value) {
  store8((u16)(0x0100 | S), value);
  S--;
}

u8 pull8() {
  S++;
  return read8((u16)(0x0100 | S));
}

// ---------------------------------------------------------------------------
// Frame/thread API
// ---------------------------------------------------------------------------
void start_game() {
  if (g_started) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_quit = false;
    g_finished = false;
    g_frame_ready = false;
  }
  g_started = true;
  g_thread = std::thread([] {
    try {
      run_game();
    } catch (const GameExit&) {
      // normal shutdown
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_finished = true;
    }
    g_cv.notify_all();
  });
}

void wait_frame(Frame& out) {
  std::unique_lock<std::mutex> lock(g_mutex);
  g_cv.wait(lock, [] { return g_frame_ready || g_finished; });
  if (g_frame_ready) {
    out = g_shared;
    g_frame_ready = false;
    g_cv.notify_all();
  } else {
    memset(out.pixels, 0, sizeof(out.pixels));
    out.audio_samples = 0;
  }
}

void stop_game() {
  if (!g_started) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_quit = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
  g_started = false;
}

bool game_finished() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_finished;
}

void run_one_frame(Frame& out) {
  start_game();
  wait_frame(out);
  stop_game();
}

} // namespace et
