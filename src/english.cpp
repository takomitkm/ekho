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
  this->setEnglishSpeed(this->getEnglishSpeed());
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
#ifdef ENABLE_ESPEAK
  if (mDict.mSfinfo.samplerate == 0) {
    cerr << "Voice sample rate not set. Maybe you have not setVoice first." << endl;
    return;
  }

  // espeak RATE is words per minute and must stay positive.
  //
  // Upstream computed the Chinese tempo normalisation (the same number
  // setSpeed() feeds to setTempo()) and SUBTRACTED it from espeak's rate:
  //
  //     espeakRATE = 175 * (100 + delta - baseDelta) / 100
  //
  // That mixes units. baseDelta is a tempo multiplier applied by the resampler
  // downstream -- setTempo(139) means "play 2.39x faster" -- whereas espeakRATE
  // is words per minute. With the bundled packs baseDelta is 139, so the
  // expression evaluated to 175 * (100 - 139) / 100 = -68 wpm, and espeak
  // received a negative rate.
  //
  // What the original was reaching for is that English should be sped up by the
  // same factor as Chinese, which is a multiplication:
  //
  //     espeakRATE = 175 * (100 + delta) / 100 * (100 + baseDelta) / 100
  //
  // English now runs at its own default rate. EKHO_ESPEAK_TEMPO_MATCH=1 turns
  // the multiplicative match on. Whatever the path, the value handed to espeak
  // is clamped into the range espeak documents.
  const int kRateDefault = 175;
  const int kRateMin = 80;
  const int kRateMax = 450;

  int tempoMatch = 0;
  const char *opt = getenv("EKHO_ESPEAK_TEMPO_MATCH");
  if (opt != nullptr && *opt != '\0' && atoi(opt) != 0 &&
      mDict.getLanguage() == MANDARIN && mDict.mSfinfo.frames > 0) {
    tempoMatch = (int)round(mDict.mSfinfo.frames * 2 * 44100 * 100 /
                            mDict.mSfinfo.samplerate / 20362) - 100;
  }

  if (EkhoImpl::mDebug) {
    cerr << "setEnglishSpeed espeakRATE default: " << espeak_GetParameter(espeakRATE, 0) << endl;
    cerr << "setEnglishSpeed espeakRATE current: " << espeak_GetParameter(espeakRATE, 1) << endl;
    cerr << "setEnglishSpeed tempoMatch: " << tempoMatch << endl;
    cerr << "zhttsEnabled: " << Ekho::zhttsEnabled << endl;
  }

  if (delta >= -50 && delta <= 150) {
    int rate = kRateDefault * (100 + delta) / 100;
    if (tempoMatch != 0) {
      rate = rate * (100 + tempoMatch) / 100;
    }
    if (rate < kRateMin) {
      cerr << "espeak: RATE " << rate << " clamped up to " << kRateMin
           << " (delta=" << delta << " tempoMatch=" << tempoMatch << ")" << endl;
      rate = kRateMin;
    } else if (rate > kRateMax) {
      cerr << "espeak: RATE " << rate << " clamped down to " << kRateMax
           << " (delta=" << delta << " tempoMatch=" << tempoMatch << ")" << endl;
      rate = kRateMax;
    }
    int ret = espeak_SetParameter(espeakRATE, rate, 0);
    this->englishSpeedDelta = delta;
    if (EkhoImpl::mDebug) {
      cerr << "english speed: " << rate << ", result=" << ret << endl;
    }
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