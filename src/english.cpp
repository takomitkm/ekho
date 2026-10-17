/***************************************************************************
 * Copyright (C) 2008-2024 by Cameron Wong                                 *
 * name in passport: HUANG GUANNENG                                        *
 * email: hgneng at gmail.com                                              *
 * website: https://eguidedog.net                                          *
 *                                                                         *
 * This program is free software; you can redistribute it and/or           *
 * modify it under the terms of the GNU General Public License             *
 * as published by the Free Software Foundation; either version 2          *
 * of the License, or any later version.                                   *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program; if not, write to the Free Software             *
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,              *
 * MA  02110-1301, USA.                                                    *
 **************************************************************************/
#include "config.h"
#include "ekho.h"
#include "ekho_dict.h"
#include "ekho_impl.h"
#include <math.h>
#include <stdlib.h>

#ifdef ENABLE_ESPEAK
#include "espeak-ng/speak_lib.h"
#endif


#ifdef ENABLE_ESPEAK
// espeak_Initialize() reports the rate it synthesises at (22050 on this
// build); espeak does not pass it per-callback, so record it once.
static int gsEspeakSampleRate = 0;

// espeak's RATE parameter is words per minute; espeak documents 80..450.
static const int kEspeakRateMin = 80;
static const int kEspeakRateMax = 450;

// The rate espeak speaks at when --english-speed asks for no change.
//
// espeak's own default is 175 wpm, which is brisk for English read by an engine
// whose Chinese runs at conversational speed. This tree used to land on 140 by
// accident: initEnglish() applied getEnglishSpeed(), which subtracted 20
// (upstream's "@TODO: slower for bd voice??"), so an explicit --english-speed
// was discarded and an unset one silently became 140. 140 is what the listening
// samples were made with, so it is kept as the deliberate default.
//
// Move it without a rebuild:
//
//     EKHO_ESPEAK_RATE=120      # words per minute, clamped to 80..450
//
// Read once per process; a value that is not a positive integer is reported and
// ignored.
static int espeakRateBase() {
  static int cached = -1;
  if (cached > 0) {
    return cached;
  }
  const int kDefault = 140;
  int wpm = kDefault;
  const char* env = getenv("EKHO_ESPEAK_RATE");
  if (env != nullptr && *env != '\0') {
    int asked = atoi(env);
    if (asked <= 0) {
      cerr << "ekho: EKHO_ESPEAK_RATE='" << env << "' is not a rate; using "
           << kDefault << " wpm" << endl;
    } else {
      wpm = asked;
      if (wpm < kEspeakRateMin) {
        cerr << "ekho: EKHO_ESPEAK_RATE=" << asked << " clamped up to "
             << kEspeakRateMin << " wpm" << endl;
        wpm = kEspeakRateMin;
      } else if (wpm > kEspeakRateMax) {
        cerr << "ekho: EKHO_ESPEAK_RATE=" << asked << " clamped down to "
             << kEspeakRateMax << " wpm" << endl;
        wpm = kEspeakRateMax;
      }
    }
  }
  cached = wpm;
  return cached;
}

// The argument to hand back to Ekho::synthCallback(). espeak invokes its synth
// callback with no userdata, so recover the one synth2() recorded for the call
// in progress; fall back to the impl for paths that never go through synth2().
// Passing EkhoImpl::gEkho unconditionally, as upstream did, meant a caller
// driving Ekho::synth4() received its own callback with the engine as `arg`
// and dereferenced it as its context.
static void* espeakSynthArg() {
  return Ekho::synthUserdata != nullptr
             ? Ekho::synthUserdata
             : static_cast<void*>(EkhoImpl::gEkho);
}

static int espeakSynthCallback(short* wav, int numsamples,
                               espeak_EVENT* events) {
  return Ekho::synthCallback(wav, numsamples, espeakSynthArg(), OVERLAP_NONE);
}
#endif

namespace ekho {


SynthCallback* Ekho::synthCallback = nullptr;

void EkhoImpl::initEnglish(void) {
  gEkho = this;


#ifdef ENABLE_ESPEAK
  // Idempotent. speechDaemon() calls this once per speech session and synth2()
  // now calls it on the callback path, which upstream never initialised at all:
  // espeak_Initialize() only ever ran inside speechDaemon(), so a program
  // driving Ekho::synth4() directly left espeak uninitialised and the first
  // espeak_SetVoiceByName() returned EE_NOT_FOUND (2).
  if (isEspeakInited) {
    return;
  }

  // espeak
  int samplerate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, nullptr, 1);
  if (samplerate <= 0) {
    cerr << "ekho: espeak_Initialize failed (returned " << samplerate
         << "); English synthesis is unavailable" << endl;
    return;
  }
  gsEspeakSampleRate = samplerate;
  isEspeakInited = true;
  // Re-apply whatever setEnglishSpeed() was told before espeak existed. A rate
  // change does not stick until espeak_Initialize() has run, so the CLI's
  // --english-speed used to be silently lost right here, and the value applied
  // instead was getEnglishSpeed() -- the delta minus 20.
  this->applyEnglishSpeed();
  if (mDebug) {
    cerr << "espeak init samplerate: " << samplerate << endl;
  }
  espeak_SetSynthCallback(espeakSynthCallback);
  /* 女声好像并不好听，还是用原声吧
  if (!mIsMale) {
    espeak_SetVoiceByName("en+f4");
  } else {
    espeak_SetVoiceByName("en");
  }*/
#endif

}

void EkhoImpl::setEnglishSpeed(int delta) {
  // Remember the request first. It has to survive until espeak is initialised
  // (initEnglish() calls applyEnglishSpeed()), and getEnglishSpeed() reports it
  // -- it is the delta the caller asked for, not a derived rate.
  this->englishSpeedDelta = delta;
  this->applyEnglishSpeed();
}

// Hand the current delta to espeak. Split out of setEnglishSpeed() because the
// lazy init has to apply a delta that was set before espeak existed: a rate set
// before espeak_Initialize() does not stick.
void EkhoImpl::applyEnglishSpeed(void) {
#ifdef ENABLE_ESPEAK
  if (!isEspeakInited) {
    if (EkhoImpl::mDebug) {
      cerr << "english speed: delta " << this->englishSpeedDelta
           << "% held until espeak init" << endl;
    }
    return;
  }

  // espeak RATE is words per minute and must stay positive, so the delta is a
  // percentage on a rate, never a rate. Upstream fed the Chinese tempo
  // normalisation into it instead:
  //
  //     espeakRATE = 175 * (100 + delta - baseDelta) / 100
  //
  // which mixes units: baseDelta is the tempo multiplier setSpeed() hands to
  // setTempo() downstream (with the bundled Mandarin pack it is 139, so that
  // expression asked espeak for -68 wpm). Its intent -- English sped up by the
  // same factor as Chinese -- is a multiplication, and that is what
  // EKHO_ESPEAK_TEMPO_MATCH=1 turns on. Off by default: the two quantities are
  // unrelated, and matching them makes English fast.
  int tempoMatch = 0;
  const char *opt = getenv("EKHO_ESPEAK_TEMPO_MATCH");
  if (opt != nullptr && *opt != '\0' && atoi(opt) != 0 &&
      mDict.getLanguage() == MANDARIN && mDict.mSfinfo.frames > 0) {
    tempoMatch = (int)round(mDict.mSfinfo.frames * 2 * 44100 * 100 /
                            mDict.mSfinfo.samplerate / 20362) - 100;
  }

  const int base = espeakRateBase();
  int rate = (int)((long)base * (100 + this->englishSpeedDelta) / 100);
  if (tempoMatch != 0) {
    rate = rate * (100 + tempoMatch) / 100;
  }

  // Out-of-range deltas are clamped, not discarded: silently ignoring what the
  // caller asked for is how this knob came to lie about the rate in use.
  if (rate < kEspeakRateMin) {
    cerr << "espeak: RATE " << rate << " clamped up to " << kEspeakRateMin
         << " (base=" << base << " wpm, delta=" << this->englishSpeedDelta
         << "%, tempoMatch=" << tempoMatch << ")" << endl;
    rate = kEspeakRateMin;
  } else if (rate > kEspeakRateMax) {
    cerr << "espeak: RATE " << rate << " clamped down to " << kEspeakRateMax
         << " (base=" << base << " wpm, delta=" << this->englishSpeedDelta
         << "%, tempoMatch=" << tempoMatch << ")" << endl;
    rate = kEspeakRateMax;
  }

  int ret = espeak_SetParameter(espeakRATE, rate, 0);
  if (EkhoImpl::mDebug) {
    cerr << "english speed: " << rate << " wpm (base " << base << " wpm, delta "
         << this->englishSpeedDelta << "%, tempoMatch " << tempoMatch
         << "), result=" << ret << ", espeak says "
         << espeak_GetParameter(espeakRATE, 1) << endl;
  }
#endif
}

const char* EkhoImpl::getEnglishPcm(string text, int &size) {
  if (Ekho::emotiVoiceEnabled) { 
    //this->audio->setSampleRate(Ekho::COQUI_SAMPLE_RATE);
    const char* pcm = (const char*)this->getPcmFromServer(Ekho::EMOTIVOICE_PORT, text, size, Ekho::EMOTIVOICE_AMPLIFY_RATE);
    size *= 2; // convert frames to chars
    return pcm;
  } else if (Ekho::piperEnglishEnabled) {
    // 这个地方不知道为什么这样设置反而不对，明明是要把22050输入源转换成16000输出采样率的
    //this->audio->setSampleRate(22050);
    const char* pcm = (const char*)this->getPcmFromPiperServer(text, size, Ekho::PIPER_ENGLISH_PORT);
    // @fixme: not sure whether this will crash when delete[]
    size *= 2; // convert frames to chars
    return pcm;
  } else {


#ifdef ENABLE_ESPEAK
  this->synthWithEspeak(text);
  return nullptr;
#endif
  }
}

void EkhoImpl::synthWithEspeak(string text) {
#ifdef ENABLE_ESPEAK
  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::synthWithEspeak: " << text << endl;
  }

  // Track the last voice actually selected. This must be mutable: as a
  // `static const char*` initialised to "en" it could never be updated, so a
  // later --english-voice would compare against the old value and be ignored.
  // It is now a member, so two engines with different voices do not share it.
  //
  // espeak voice names are data-dependent: whether "en+f1" resolves depends on
  // which voice files the installed espeak-ng ships. Failing used to be silent
  // apart from one line on stderr, with no fallback, so English was read in
  // whatever voice espeak happened to have selected.
  if (mAppliedEnglishVoice != mEnglishVoice) {
    static const char *const kFallbacks[] = {"en", "en-us", "english", nullptr};
    int ret = espeak_SetVoiceByName(mEnglishVoice);
    if (ret == EE_OK) {
      mAppliedEnglishVoice = mEnglishVoice;
    } else {
      const char *used = nullptr;
      for (const char *const *f = kFallbacks; *f != nullptr; f++) {
        if (espeak_SetVoiceByName(*f) == EE_OK) {
          used = *f;
          break;
        }
      }
      cerr << "ekho: eSpeak voice '" << mEnglishVoice
           << "' rejected (code " << ret << ")";
      if (used != nullptr) {
        cerr << ", falling back to '" << used << "'";
      } else {
        cerr << " and no fallback voice is installed";
      }
      cerr << endl;
      mAppliedEnglishVoice = (used != nullptr) ? used : "";
    }
  }

  if (!isStopped) {
    Ekho::synthCallback(0, 0, espeakSynthArg(), OVERLAP_NONE);  // flush pending pcm

    // espeak synthesises at 22050 Hz. Tell the shared sonic stream that the
    // samples about to be written are at 22050 so it resamples them up to
    // outputSampleRate on the way out. Using the *persistent* stream matters:
    // sonic keeps filter state across calls, so the whole sentence goes
    // through one resampler instead of restarting it per chunk.
    this->audio->setSampleRate(gsEspeakSampleRate);

    espeak_Synth(text.c_str(), text.length() + 1, 0, POS_CHARACTER, 0,
                 espeakCHARS_UTF8, 0, 0);

    // Drain the espeak PCM *before* restoring the rate, otherwise the tail
    // would be resampled as if it were at the voice rate.
    finishWritePcm();
    this->audio->flushFrames();
    this->audio->setSampleRate(this->audio->sampleRate);
  }
#endif
}

// It's caller's responsibility to delete the returned pointer
const char* EkhoImpl::getPcmFromFlite(string text, int& size) {
  
  return nullptr;
}

// It's caller's responsibility to delete the returned pointer
const char* EkhoImpl::getPcmFromFestival(string text, int& size) {
#if 1
  size = 0;
  return 0;
#endif
}

} // end of namespace ekho