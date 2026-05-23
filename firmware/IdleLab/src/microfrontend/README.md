# TF audio_microfrontend lib — reference files

These files are dropped in as **reference / scaffolding for a future session**.
They are NOT currently compiled into the build (nothing in `IdleLab.ino`
`#include`s them yet, so the Arduino build system ignores them).

## What's here

Vendored from upstream:
- **TensorFlow `tensorflow/lite/experimental/microfrontend/lib`** — windowing,
  FFT, mel filter bank, noise reduction, PCAN gain control, log scale.
- **mborgerding/kissfft** — kiss_fft.c / kiss_fft.h / _kiss_fft_guts.h plus
  tools/kiss_fftr.c / .h. Used internally by the microfrontend.

## Why this isn't wired up yet

To make this work on M5StackChan's Arduino build, the next session needs:

1. **`#include` the headers** in `IdleLab.ino` and force the build system
   to compile the .c/.cc files. Arduino IDE 2.x picks up subfolders but
   the include paths need `-I` flags to find nested includes.
2. **Resolve the kissfft namespace**: microfrontend's `kiss_fft_int16.cc`
   does `namespace kissfft_fixed16 { #include "kiss_fft.c" }` to scope the
   FFT symbols. Espressif's bundled tflite-micro library uses a similar
   pattern with namespace `kiss_fft_fixed16` (subtle name difference). The
   two should coexist since they're different symbols, but link-time errors
   are possible if the bundled library's signal kernels somehow get pulled
   in.
3. **Init FrontendState** with the constants from ESPHome's
   `preprocessor_settings.h` (already mirrored in the TFLM scaffolding
   comments in `IdleLab.ino`).
4. **Feed it 20ms audio steps** (320 samples at 16kHz) from the existing
   `audio_buffer` in `ambient_tick`, get 40-dim int8 features back.
5. **Plug features into `tflm_beepoh` interpreter** (currently gated
   behind `TFLM_BEEPOH_ENABLED 0` in `IdleLab.ino`).

## Why I stopped here

Overnight autonomous attempt 2026-05-21. I revised the estimate from
"1-2 days" down to "3-5 hours" once I saw ESPHome's reference and realized
the upstream library could be vendored. After ~1.5 hours of work, Phase A
(porting these files into the tree) was only half-done because of:
- Kissfft files not where I expected (404s on multiple paths before
  finding them on `mborgerding/kissfft` root)
- The bundled tflite-micro library has the kissfft *symbols* referenced
  but undefined — meaning we have to bring in the implementations
  ourselves (which is what these files do)
- Each next step (Arduino compile of subfolder .c/.cc, link, namespace
  resolution, then actual frontend init, then feature feed) has its own
  potential snags

Per the safety-rails plan ("if a phase blows its budget by 2x, stop"),
I rolled back instead of pushing buggy commits at 2 AM. The live build on
BMO is still the working WakeNet9 "Hi, ESP" wake word.
