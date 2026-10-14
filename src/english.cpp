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

#ifdef ENABLE_ESPEAK
#include "espeak-ng/speak_lib.h"
#endif


#ifdef ENABLE_ESPEAK
// espeak_Initialize() reports the rate it synthesises at (22050 on this
// build); espeak does not pass it per-callback, so record it once.
static int gsEspeakSampleRate = 0;

static int espeakSynthCallback(short* wav, int numsamples,
                               espeak_EVENT* events) {
  return Ekho::synthCallback(wav, numsamples, EkhoImpl::gEkho, OVERLAP_NONE);
}
#endif

namespace ekho {


SynthCallback* Ekho::synthCallback = nullptr;

void EkhoImpl::initEnglish(void) {
  gEkho = this;


#ifdef ENABLE_ESPEAK
  // espeak
  int samplerate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, nullptr, 1);
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

  int baseDelta = 0;
  if (!Ekho::emotiVoiceEnabled && !Ekho::zhttsEnabled && !Ekho::piperEnglishEnabled) { // EmotiVoice是正常速度，不需要调整
    baseDelta = (int)round(mDict.mSfinfo.frames * 2 * 44100 * 100 / mDict.mSfinfo.samplerate / 20362) - 100;

    // Changing tempo will add noise, we'd better don't do it.
    if (baseDelta < 10 && baseDelta > -10) {
      baseDelta = 0;
    }
  }

  if (EkhoImpl::mDebug) {
    cerr << "setEnglishSpeed espeakRATE default: " << espeak_GetParameter(espeakRATE, 0) << endl;
    cerr << "setEnglishSpeed espeakRATE current: " << espeak_GetParameter(espeakRATE, 1) << endl;
    cerr << "setEnglishSpeed baseDelta: " << baseDelta << endl;
    cerr << "zhttsEnabled: " << Ekho::zhttsEnabled << endl;
  }

  if (delta >= -50 && delta <= 150) {
    int ret = espeak_SetParameter(espeakRATE, 175 * (100 + delta - baseDelta) / 100, 0);
    this->englishSpeedDelta = delta;
    if (EkhoImpl::mDebug) {
      cerr << "english speed: " << 175 * (100 + delta) / 100
           << ", result=" << ret << endl;
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
  static std::string current_voice;
  if (current_voice != mEnglishVoice) {
    int ret = espeak_SetVoiceByName(mEnglishVoice);
    if (ret != EE_OK) {
      std::cerr << "Error setting eSpeak voice: " << mEnglishVoice << ". return " << ret << std::endl;
    } else {
      current_voice = mEnglishVoice;
    }
  }

  if (!isStopped) {
    Ekho::synthCallback(0, 0, gEkho, OVERLAP_NONE);  // flush pending pcm

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