# E.T. the Extra-Terrestrial — Atari 2600 Native C++ Port

[中文说明](README.zh-CN.md)

A source-level C++ rewrite of the Atari 2600 game **E.T. the Extra-Terrestrial**
(Atari, 1982), made portable by replacing the 6502 assembly with statically
translated C++ and replacing the console hardware with a small native hardware
model.

This is **not** a from-scratch remake, redesign, or hand-written reimplementation.
The original game logic was preserved by translating the original 6502 assembly
into C++. The result is the same game running natively, outside an emulator, on
modern platforms.

## What this is

- A **static translation** of the original E.T. 6502 assembly into C++
  (`game/game.cpp`).
- A **hardware shim** that models only what the game needs: the TIA
  (video, audio, collisions), the RIOT (RAM, input, interval timer), and the
  F8 cartridge bank switch (`src/hardware.cpp`).
- A small **frontend** for video, audio, and keyboard input (`src/main.cpp`).

## What this is not

- It is **not** a 6502 emulator. There is no instruction fetch/decode loop and no
  emulated CPU executing a ROM image; the 6502 instructions are C++ statements.
- It is **not** a new game design. Gameplay, timing, data tables, and behavior
  come from the original assembly.
- It is **not** a general-purpose Atari 2600 emulator and cannot run other ROMs.

## Features

- Original game logic and timing, running natively at 60 FPS.
- Native video output, keyboard controls, and PulseAudio sound.
- Selectable NTSC palette (RGB565).
- Color/B&W and both difficulty switches emulated.
- Self-contained build: no external emulator, no runtime dependencies beyond the
  window/audio libraries.

## Build and run

Tested on Ubuntu 20.04 with the commands below.

```sh
sudo apt update
sudo apt install make g++ libpulse-dev
```

```sh
make          # builds ./et
make run      # builds and starts the game
```

## Controls

| Atari 2600 | Keyboard |
| --- | --- |
| Joystick direction | Arrow keys or `W` `A` `S` `D` |
| Fire button | `Space`, `Z`, or `Ctrl` |
| Select | `F1` or `Tab` |
| Reset | `F2` or `Enter` |
| Color / B&W switch | `F3` (toggles) |
| Left difficulty switch | `F4` (toggles A/B) |
| Right difficulty switch | `F5` (toggles A/B) |
| Quit | `Esc` |

## Porting to another platform

The game core is platform-independent. Only `src/main.cpp` is Linux-specific.
A port needs to provide three things:

1. **Video** — call `et::start_game()` once, then `et::wait_frame(et::Frame&)` at
   60 Hz. `Frame::pixels` holds 160 × 210 TIA colour indices; convert them with
   `et::ntsc_palette[256]` (RGB565).
2. **Audio** — play `Frame::audio` (`audio_samples` interleaved stereo `s16`
   frames at 31,400 Hz).
3. **Input** — set the fields of the global `et::input` (`up`, `down`, `left`,
   `right`, `fire`, `reset`, `select`, `color`, `left_diff_a`, `right_diff_a`).
   Call `et::stop_game()` on exit.

That is enough to build an SDL2, Win32, macOS, web, or console frontend without
touching the translated game or the hardware model.

## Accuracy and limitations

- This is a functional translation, not a cycle-exact silicon simulation. The
  TIA is modelled closely enough for this game's display kernel and audio, but it
  is not a general TIA implementation.
- F8 banking is supported; other cartridge schemes are not.
- Only the NTSC palette and timing are implemented.
- The game is designed for a 60 Hz display; output is vsync-independent and
  paced by a timer.

## Credits

- Original game: **E.T. the Extra-Terrestrial** (Atari, 1982), designed by
  Howard Scott Warshaw.
- 6502 disassembly used as the translation source: the
  [E.T. disassembly project at Retro-Replay](https://retro-replay.com/romhacking/documents/e-t-the-extra-terrestrial-disassembly/#credits).
  Thanks to its authors and contributors for documenting the original code.

## Legal

- The translated source code and hardware model in this repository are released
  under the **GNU General Public License v3.0** (see `LICENSE`).
- **E.T. the Extra-Terrestrial** and the original ROM are copyrighted by Atari.
  The embedded ROM data is included solely to preserve and study the original
  game. This project is not affiliated with or endorsed by Atari.
- No original game data was created by this project; all game content comes from
  the original cartridge and the community disassembly.
