// Native X11/ALSA frontend for the statically translated E.T. game.
#include "hardware.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

using namespace et;

static const int SCALE = 4;
static const int WIN_W = Frame::W * SCALE;
static const int WIN_H = Frame::H * SCALE;

static Display* dpy = nullptr;
static Window win = 0;
static GC gc = nullptr;
static XImage* img = nullptr;
static Visual* vis = nullptr;
static Atom wm_delete = 0;
static bool quit = false;
static bool key_down[256] = {false};
static uint32_t palette32[256];
static Frame last_frame;
static bool have_last = false;

static unsigned long scale_component(unsigned v, unsigned long mask) {
  if (!mask) return 0;
  int shift = 0;
  unsigned long m = mask;
  while (!(m & 1UL)) { m >>= 1; shift++; }
  return ((unsigned long)v * m / 255UL) << shift;
}

static unsigned long pack_rgb(unsigned r, unsigned g, unsigned b) {
  return scale_component(r, vis->red_mask) |
         scale_component(g, vis->green_mask) |
         scale_component(b, vis->blue_mask);
}

static void init_palette() {
  for (int i = 0; i < 256; ++i) {
    u16 v = ntsc_palette[i];
    unsigned r5 = (v >> 11) & 31;
    unsigned g6 = (v >> 5) & 63;
    unsigned b5 = v & 31;
    unsigned r = (r5 << 3) | (r5 >> 2);
    unsigned g = (g6 << 2) | (g6 >> 4);
    unsigned b = (b5 << 3) | (b5 >> 2);
    palette32[i] = (uint32_t)pack_rgb(r, g, b);
  }
}

static void draw_frame(const Frame& f) {
  if (!img) return;
  int stride = img->bytes_per_line / 4;
  uint32_t* base = (uint32_t*)img->data;
  for (int dy = 0; dy < WIN_H; ++dy) {
    int sy = dy / SCALE;
    const u8* row = f.pixels + sy * Frame::W;
    uint32_t* out = base + dy * stride;
    for (int dx = 0; dx < WIN_W; ++dx)
      out[dx] = palette32[row[dx / SCALE]];
  }
  XPutImage(dpy, win, gc, img, 0, 0, 0, 0, WIN_W, WIN_H);
}

static void clear_input() {
  input.up = input.down = input.left = input.right = false;
  input.fire = false;
  input.reset = input.select = false;
}

static void handle_key(KeySym ks, bool down, bool repeated) {
  switch (ks) {
    case XK_Up: case XK_w: case XK_W: input.up = down; break;
    case XK_Down: case XK_s: case XK_S: input.down = down; break;
    case XK_Left: case XK_a: case XK_A: input.left = down; break;
    case XK_Right: case XK_d: case XK_D: input.right = down; break;
    case XK_space: case XK_z: case XK_Z: case XK_Control_L: input.fire = down; break;
    case XK_Return: case XK_KP_Enter: case XK_F2: input.reset = down; break;
    case XK_Tab: case XK_F1: input.select = down; break;
    case XK_F3:
      if (down && !repeated) input.color = !input.color;
      break;
    case XK_F4:
      if (down && !repeated) input.left_diff_a = !input.left_diff_a;
      break;
    case XK_F5:
      if (down && !repeated) input.right_diff_a = !input.right_diff_a;
      break;
    case XK_Escape:
      if (down) quit = true;
      break;
    default: break;
  }
}

static void process_events() {
  while (XPending(dpy) > 0) {
    XEvent ev;
    XNextEvent(dpy, &ev);
    switch (ev.type) {
      case Expose:
        if (have_last && ev.xexpose.count == 0) draw_frame(last_frame);
        break;
      case KeyPress: {
        KeySym ks = XLookupKeysym(&ev.xkey, 0);
        bool rep = ev.xkey.keycode < 256 && key_down[ev.xkey.keycode];
        if (ev.xkey.keycode < 256) key_down[ev.xkey.keycode] = true;
        handle_key(ks, true, rep);
        break;
      }
      case KeyRelease: {
        KeySym ks = XLookupKeysym(&ev.xkey, 0);
        if (ev.xkey.keycode < 256) key_down[ev.xkey.keycode] = false;
        handle_key(ks, false, false);
        break;
      }
      case FocusOut:
        memset(key_down, 0, sizeof(key_down));
        clear_input();
        break;
      case ClientMessage:
        if ((Atom)ev.xclient.data.l[0] == wm_delete) quit = true;
        break;
      case DestroyNotify:
        quit = true;
        break;
    }
  }
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  std::printf("E.T. the Extra-Terrestrial - native C++ port\n");
  std::printf("Controls: arrows/WASD move, Space/Z fire, F1/Tab Select, F2/Enter Reset,\n");
  std::printf("          F3 Color/BW, F4 left difficulty, F5 right difficulty, Esc quit\n");

  dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    std::fprintf(stderr, "Cannot open X display. Set DISPLAY (e.g. export DISPLAY=:0).\n");
    return 1;
  }
  int scr = DefaultScreen(dpy);
  vis = DefaultVisual(dpy, scr);
  int depth = DefaultDepth(dpy, scr);
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, WIN_W, WIN_H, 0,
                            BlackPixel(dpy, scr), BlackPixel(dpy, scr));
  XStoreName(dpy, win, "E.T. the Extra-Terrestrial (Atari 2600) - native C++");
  XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask |
                         StructureNotifyMask | FocusChangeMask);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);
  XSizeHints hints;
  memset(&hints, 0, sizeof(hints));
  hints.flags = PMinSize | PMaxSize;
  hints.min_width = hints.max_width = WIN_W;
  hints.min_height = hints.max_height = WIN_H;
  XSetWMNormalHints(dpy, win, &hints);
  gc = XCreateGC(dpy, win, 0, nullptr);

  img = XCreateImage(dpy, vis, depth, ZPixmap, 0, nullptr, WIN_W, WIN_H, 32, 0);
  if (!img) { std::fprintf(stderr, "XCreateImage failed\n"); return 1; }
  img->data = (char*)calloc((size_t)img->bytes_per_line * WIN_H, 1);
  if (!img->data) { std::fprintf(stderr, "out of memory\n"); return 1; }
  init_palette();
  XMapWindow(dpy, win);
  XFlush(dpy);

  Bool detectable = False;
  XkbSetDetectableAutoRepeat(dpy, True, &detectable);

  // Audio (optional: the game still runs without a sound device).
  pa_simple* pa = nullptr;
  bool audio_ok = false;
  {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 31400;
    ss.channels = 2;
    int err = 0;
    pa = pa_simple_new(nullptr, "ET-native", PA_STREAM_PLAYBACK, nullptr,
                       "game", &ss, nullptr, nullptr, &err);
    if (pa) audio_ok = true;
    else std::fprintf(stderr, "PulseAudio unavailable (%s); continuing without sound.\n",
                      pa_strerror(err));
  }

  start_game();

  const double fps = 60.0;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  const long frame_ns = (long)(1000000000.0 / fps + 0.5);

  while (!quit) {
    Frame f;
    wait_frame(f);
    last_frame = f;
    have_last = true;
    process_events();
    if (quit) break;
    draw_frame(f);
    if (audio_ok && f.audio_samples > 0) {
      int err = 0;
      if (pa_simple_write(pa, f.audio, (size_t)f.audio_samples * 2 * sizeof(s16), &err) < 0)
        std::fprintf(stderr, "PulseAudio write failed: %s\n", pa_strerror(err));
    }
    XFlush(dpy);

    next.tv_nsec += frame_ns;
    while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > next.tv_sec || (now.tv_sec == next.tv_sec && now.tv_nsec > next.tv_nsec + 100000000L)) {
      next = now; // resync after a stall
    } else {
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
  }

  stop_game();
  if (audio_ok) { int err = 0; pa_simple_drain(pa, &err); pa_simple_free(pa); }
  if (img) { XDestroyImage(img); img = nullptr; }
  if (gc) XFreeGC(dpy, gc);
  if (win) XDestroyWindow(dpy, win);
  if (dpy) XCloseDisplay(dpy);
  return 0;
}
