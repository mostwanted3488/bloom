# BLOOM

**An ethereal ASCII flower instrument — by BUYORBEALOSER.**

BLOOM is a VST3 / AU instrument (Windows & macOS). A flower made of glowing characters grows in the window and reacts to every note you play — from the piano roll, a MIDI keyboard, or by hovering the flower itself.

## Features

- **19 sounds** in 6 families — KEYS, BELLS, PLUCKS, PADS, VOX, TEXTURES — plus your own **sample** (drag & drop or *+ LOAD SAMPLE…*).
- **LAYER + BLEND** to stack two sounds.
- **24 presets**, each paired with a flower and a color.
- **16 flowers** (9 botanical + 7 invented, animated) and **15 colors**.
- **9 macro knobs**: ATTACK, RELEASE, TONE, SPACE, SHIMMER, WIDTH, LOW CUT, GLIDE, BLEND — plus the **BLOOM** macro.
- **Scales** (10, incl. Phrygian, Harmonic minor, Hirajoshi, In-sen) with optional **SNAP** to scale.
- **Arpeggiator** (UP, DOWN, UP-DOWN, RANDOM, ORDER, CHORD) and **tempo-synced delay**, locked to the host tempo.
- **Hover play** with grid **QUANTIZE**, click = chord, drag = arpeggio.
- MIDI: velocity, sustain pedal, pitch bend (±2 st), mod wheel (vibrato).
- Every control is a host parameter → **automatable** and saved with your project.
- **MIDI out**: what you play with the mouse is sent to the host, so you can record it straight into the piano roll (see `INSTALL.txt`).

## Download

Built plugins are attached to each [release](../../releases) (`BLOOM-Windows.zip`, `BLOOM-macOS.zip`).
They are also available as artifacts of every run in the [Actions](../../actions) tab.
Installation steps: see `INSTALL.txt` inside the zip.

## Build from source

Requirements: CMake ≥ 3.22, a C++17 compiler (Visual Studio 2022 / Xcode 15).
JUCE 8 is fetched automatically.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target BLOOM_VST3
```

On Windows, BLOOM links WebView2 statically. Install the SDK once:

```powershell
nuget install Microsoft.Web.WebView2 -Version 1.0.3485.44 -OutputDirectory nuget
cmake -B build -DJUCE_WEBVIEW2_PACKAGE_LOCATION=%CD%/nuget
```

## Project layout

| Path | Content |
|---|---|
| `Resources/index.html` | The whole UI (also runs on its own in a browser, with a WebAudio preview engine) |
| `Source/Engine.*` | The C++ sound engine (voices, FM, formants, shimmer reverb, delay, arp…) |
| `Source/SoundBank.h` | Sound definitions, generated from the UI by `tools/gen_soundbank.mjs` |
| `Source/PluginProcessor.*` | Parameters, host tempo, state, samples |
| `Source/PluginEditor.*` | WebView host and the UI ↔ engine bridge |

If you change the sounds in `Resources/index.html`, regenerate the C++ bank:

```bash
node tools/gen_soundbank.mjs
```
