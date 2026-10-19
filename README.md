# ekho

Read <a href='https://github.com/hgneng/ekho/blob/master/INSTALL'>INSTALL</a> file if you have trouble in building.

Chinese text-to-speech engine. It's part of <a href='http://www.eguidedog.net'>eGuideDog project</a>. Here is <a href='http://www.eguidedog.net/ekho.php'>Ekho TTS's home page</a>.

This repository is forked from <a href='https://sourceforge.net/p/e-guidedog/code/HEAD/tree/eGuideDog_TTS/'>SourceForge</a> at version r2418 (and patch to r2478).

Voice files are not included. We can get them from a distribution package or <a href='https://sourceforge.net/projects/e-guidedog/files/Ekho-Voice-Data/0.2/'>Ekho Voice Data's download page</a>. To replace Mandarin voice files, we can replace the whole pinyin folder, then remove pinyin.index and pinyin.voice. Re-running ekho will generate new pinyin.index and pinyin.voice from new voice files.

In order to create your own voice, please refere to <a href='http://eguidedog.net/doc/doc_make_new_voice_cn.php'>如何为Ekho添加新的声音</a>.

## This fork: Linux only

This tree is a Linux-only fork of the above. Windows build products are gone, the
build is autotools only, and a few things were added or fixed. It is published at
<https://github.com/takomitkm/ekho> -- itself a fork of
<https://github.com/hgneng/ekho>, pushed there as a fresh snapshot (the two
histories share no common ancestor; the fork's `master` is this tree). What it
needs and what it gives you:

    scripts/build-native.sh      # configure + make into out/ (ekho, ekho-stream, ekho-server)
    scripts/sim.sh               # correctness and streaming assertions; non-zero exit on failure
    scripts/server-test.sh       # real HTTP server, sections 0-8
    scripts/make-samples.sh      # render listening samples to $HOME/ekho-samples

Added on top of upstream:

* **ALSA output** (`src/alsa_backend.{h,cpp}`), used by `ekho --play`. The library
  is `dlopen`ed, so building needs the vendored headers only -- no `libasound2-dev`,
  and no `libasound.so.2` in DT_NEEDED.
* **Streaming synthesis** (`src/streaming.{h,cpp}`, `Ekho::synth4()`): append text as
  it arrives and get PCM out before the text is finished. Punctuation forces a
  segment break; numbers and units are normalised first.
* **`ekho-server`** (`src/ekho_server_main.cpp`): `POST /tts` chunked in, chunked
  `audio/L16` out, plus `POST /tts?wait=1`, `GET /health`, `GET /voices`.
  Default port 2046. It needs no privileges: bind above 1024, data path from the
  environment, output path from `--out-dir`.
* **English speed is a real knob**, independent of the Chinese tempo: `--english-speed`
  in all three binaries (`ekho-server`: as the default, plus `english_speed=DELTA` per
  request), and `EKHO_ESPEAK_RATE=WPM` to move the base rate without a rebuild. Before
  this it did nothing: the value was set before espeak existed, then overwritten by a
  delta 20 points off, and out-of-range values were dropped without a word. The base
  rate is 100 wpm and the espeak voice is `en-us+f3`, both picked by ear on 2026-10-17
  (see `scripts/make-samples.sh` groups 05 and 06); `--english-voice` overrides the
  voice for one run and `EKHO_ESPEAK_VOICE` moves the default without a rebuild.
* **The Mandarin pack is 48 kHz, and 48 kHz / 2ch is the default output.** The tracked
  `ekho-data/pinyin.index` describes a 48 kHz version of the upstream 44.1 kHz pack,
  and `scripts/convert-voice.c` produces the matching `pinyin.voice` from the upstream
  one. The 44.1 kHz originals stay next to them as `*.44k` -- they are what the
  converter reads, and both pairs are gitignored, so a clone has to supply them before
  the pack can be re-derived or the regression anchor run.
  The rate follows the target board: its only playback device accepts
  48 kHz / S16_LE / 2ch and nothing else, so `ekho`, `ekho-server` and `ekho-stream`
  all default there, and pack rate equals output rate -- no sonic resample on every
  sample of every syllable. `--samplerate=0` asks for the pack's own rate,
  `--channels=1` for mono.
  **This replaced an 8 kHz conversion made the same day.** Ekho decodes each syllable
  chunk on first use and keeps the PCM for the life of the process, so 8 kHz had cut
  peak RSS from 117 MB to 48 MB on a text reaching every syllable in the dictionary
  (small sentences 25.3 -> 24.5 MB). It was dropped because that resampler's 4 kHz
  low-pass dulls the sibilants. The trade is explicit: the 48 kHz pack is
  136,359,454 B against the 44.1 kHz original's 125,287,518 B, so the resident-RAM
  saving is gone -- **RSS at 48 kHz has not been measured, and neither has the CPU the
  missing resample would save.**
  Speaking rate holds: `scripts/reg-44k/expected.md5` records the 44.1 kHz / mono
  bytes of seven reference texts and `scripts/reg-44k.sh` re-checks them, then measures
  the 48 kHz / 2ch renders against them (recorded 2026-10-19: mean -0.17%, worst
  -1.32%; it fails past mean 0.5% or any single text 2%). The syllable-join threshold
  is **not** rate-scaled at 44.1 kHz and above -- scaling it was measured on the
  48 kHz pack and made things worse (worst 3.4%), so `quiet_level` stays equal to
  `mOverlap` there; see the comment in `src/libekho_impl.cpp`. The ~11% and ~12%
  figures that used to sit in this section were artifacts of the rejected 8 kHz pack.
  **Cantonese is not converted**: `jyutping.voice` is still 16 kHz, so that voice does
  get resampled at run time.

### Known issues

* **There is no standalone English voice.** `-v English` and `-v en` reach a stub
  in `src/synthesize.cpp` that returns without synthesising: you get a 44-byte
  silent WAV and exit status 0. This is inherited from upstream (English used to
  go through an older API), and it is deliberately not fixed here. English words
  *do* work through a Chinese voice on ASCII text -- that path is espeak-ng, and
  `scripts/make-samples.sh` group 03 uses it. `GET /voices` lists `English`
  because its dictionary loads; that is all it promises.
* **Voice data must be supplied.** `ekho-data/*.voice` and the pinyin/jyutping
  sample trees are not in the repository (they are large binaries); the `.index`
  files are. A fresh clone builds but cannot synthesise until voice data is
  dropped into `ekho-data/`, or `EKHO_DATA_PATH` points at a directory that has
  it. Ekho writes `.index`/`.voice` into that directory when the index is missing,
  so it must be writable -- that is the historical reason people ran it as root
  once; a relocatable kit with its own data directory has no such problem.
* **ALSA has never been run on the target board.** The `dlopen` path compiles and
  links against the target's symbol versions, but whether the board has a
  usable sound device is untested.

