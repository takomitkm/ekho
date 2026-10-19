/***************************************************************************
 * Copyright (C) 2008-2026 by Cameron Wong                                 *
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
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#ifdef ENABLE_CURL
#include <curl/curl.h>
#endif
#include "config.h"
#include "ekho.h"
#include "ekho_dict.h"
#include "ekho_impl.h"
#include "ssml.h"
#include "audio.h"
#include "utf8.h"

#ifdef ENABLE_ESPEAK
#include "espeak-ng/speak_lib.h"
#endif

using namespace ekho;
using namespace std;

bool EkhoImpl::mDebug = false;
SpeechdSynthCallback* EkhoImpl::speechdSynthCallback = 0;
EkhoImpl* EkhoImpl::gEkho = nullptr;

EkhoImpl::EkhoImpl() { this->init(); }

int EkhoImpl::init(void) {
  if (mDebug) {
    cerr << "EkhoImpl::init" << endl;
  }

  isEspeakInited = false;
  mIsMale = false;
  mPendingFrames = 0;
  supportSsml = true;
  mSpeakIsolatedPunctuation = true;
  mSpeechQueueMutex = PTHREAD_MUTEX_INITIALIZER;
  mSpeechQueueCond = PTHREAD_COND_INITIALIZER;
#ifdef ENABLE_ESPEAK
  // eSpeak voice for ASCII text. Picked by ear 2026-10-17 at 100 wpm from a
  // six-voice comparison (desktop samples 06-en-tone): the inherited default
  // 'en+f1' smears badly at slow rates, 'en-us+f3' stays intelligible there.
  // --english-voice overrides this per run; EKHO_ESPEAK_VOICE moves the base
  // without a rebuild.
  mEnglishVoice = "en-us+f3";
  const char *voiceEnv = getenv("EKHO_ESPEAK_VOICE");
  if (voiceEnv != nullptr && *voiceEnv != '\0') {
    mEnglishVoice = voiceEnv;
  }
#endif

  this->audio = new Audio();

  this->mSndFile = nullptr;
  this->isRecording = false;
  this->tempoDelta = 0;
  this->pitchDelta = 0;
  this->rateDelta = 0;
  // Keep in step with the default in ekho.cpp; measured as the sweet spot.
  this->mOverlap = 4096;

  this->isStopped = false;
  this->isPaused = false;
  this->isEnded = false;
  this->isSoundInited = false;
  this->isSpeechThreadInited = false;

  mPcmCache = true;

  memset(mAlphabetPcmCache, 0, 26 * sizeof(const char*));
  memset(mAlphabetPcmSize, 0, 26 * sizeof(int));
  //memset(mPendingPcm, 0, PENDING_PCM_FRAMES * sizeof(short));

  mPuncMode = EKHO_PUNC_SOME;

  return 0;
}

EkhoImpl::EkhoImpl(string voice) {
  this->init();
  this->setVoice(voice);
}

EkhoImpl::~EkhoImpl(void) {
  //pthread_mutex_lock(&mSpeechQueueMutex);
  this->isEnded = true;
  //pthread_mutex_unlock(&mSpeechQueueMutex);
  pthread_cond_signal(&mSpeechQueueCond);
  pthread_cond_destroy(&mSpeechQueueCond);

  if (this->isSpeechThreadInited) {
    void *ret;
    pthread_attr_destroy(&this->speechThreadAttr);
    pthread_join(this->speechThread, &ret);
  }
  closeStream();

  if (mPendingPcm != nullptr) {
    delete[] mPendingPcm;
    mPendingPcm = nullptr;
    mPendingPcmFrames = 0;
  }

  delete this->audio;
  this->audio = nullptr;


#ifdef ENABLE_ESPEAK
  if (isEspeakInited) {
    espeak_Terminate();
    isEspeakInited = false;
  }
#endif

  if (Ekho::piperEnabled || Ekho::piperEnglishEnabled) {
    #ifdef ENABLE_CURL
    curl_global_cleanup();
    #endif
  }
  // TODO: free mAlphabetPcmCache
}

int EkhoImpl::initSound(void) {
  if (!this->isSoundInited) {
    // launch speechDaemon
    pthread_attr_init(&this->speechThreadAttr);
    pthread_attr_setdetachstate(&this->speechThreadAttr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&this->speechThread, nullptr, speechDaemon, (void *)this);
    this->isSpeechThreadInited = true;
    this->isSoundInited = true;

    // not output sound directly, only return pcm data if mSynthCallback is set.
    if (speechdSynthCallback) {
      return 0;
    }

  }

  return 0;
}

int EkhoImpl::initStream(void) {
  closeStream();
  if (mDict.getLanguage() == ENGLISH) {
    mDict.mSfinfo.samplerate = 16000;
    mDict.mSfinfo.channels = 1;
  } else if (mDict.mSfinfo.samplerate == 0) {
    cerr << "Sample rate not detected: " << mDict.getLanguage() << endl;
    return -1;
  }

  this->audio->setInputSampleRate(mDict.mSfinfo.samplerate);
  // setInputSampleRate() records the pack's rate. The *output* rate is a
  // separate, already-configured value (Ekho::setSampleRate), and
  // initProcessor() re-reads it from audio->outputSampleRate below -- so a
  // caller that asks for 48 kHz gets 48 kHz even when the pack is 44.1 kHz.
  //
  // The pack's channel count is the *input* layout, not the output one. It must
  // not overwrite audio->channels: the pack is mono, while the caller may have
  // asked for stereo (--channels=2, the board's speaker). This used to be a
  // setChannels() call here, which silently threw the caller's choice away and
  // made --channels a no-op.
  this->audio->setInputChannels(mDict.mSfinfo.channels);
  this->audio->initProcessor();

  // Rescale the synthetic pauses before sizing the pending buffer below.
  mDict.setPauseSampleRate(mDict.mSfinfo.samplerate);

  // The pending buffer must hold a full pause plus a whole syllable without
  // overflowing, whatever the voice pack's sample rate is. 16 kHz voices need
  // ~37k frames; 44.1 kHz voices need ~3x that.
  if (mPendingPcm != nullptr) {
    delete[] mPendingPcm;
    mPendingPcm = nullptr;
    mPendingPcmFrames = 0;
  }
  {
    int sr = mDict.mSfinfo.samplerate > 0 ? mDict.mSfinfo.samplerate : 16000;
    // 1.5 s of slack + one full pause + one BUFFER_SIZE, with headroom for a
    // syllable longer than 1.5 s.
    int need = sr * 3 / 2 + mDict.getFullPauseSize() + BUFFER_SIZE;
    if (need < PENDING_PCM_FRAMES) {
      need = PENDING_PCM_FRAMES;
    }
    // One extra frame: the phantom the overlap search peeks at (see mPendingPcm
    // in ekho_impl.h). mPendingPcmFrames stays the usable capacity.
    mPendingPcm = new short[need + 1];
    mPendingPcmFrames = need;
    memset(mPendingPcm, 0, (need + 1) * sizeof(short));
    mPendingFrames = 0;
  }


  return 0;
}  // end of initStream

/* Put the engine back into the state a freshly started process is in.

   Two pieces of state survive a synthesis and otherwise make the next one
   render differently, even for the same text and the same settings:

   1. The join buffer. At the end of every synthesis the pending audio has been
      flushed (synth2() ends with an OVERLAP_NONE write that empties it), so
      mPendingFrames is 0, but the bytes are still there. writeToSonicStream()
      reads mPendingPcm[mPendingFrames] *before* deciding whether to walk
      backwards into the previous audio, and after a flush the frames past the
      valid data hold whatever the previous synthesis left behind. A loud stale
      sample stops the walk before it starts, a quiet one lets it run, purely
      according to what was spoken before. Zeroing the buffer restores the state
      initStream() leaves, which is what the walk has always effectively
      assumed - a new process has a zeroed buffer.

   2. The resampler. sonicFlushStream() empties its buffers but keeps the pitch
      and play-time history (prevPeriod, numPitchSamples, oldRatePosition,
      timeError, ...), so the same audio written to a flushed stream comes back
      with a different phase, and a few frames different in length, than it does
      out of a new stream. Audio::resetProcessor() clears that history.

   Both are no-ops on the first synthesis of a process (fresh buffer, fresh
   stream), so the first render is exactly what it always was; from then on
   every synthesis renders like a first one. That is what makes the second
   request for a text identical to the first in a live server, and it is what
   scripts/server-test.sh sections 3 and 7 check. */
void EkhoImpl::resetSynthesisState(void) {
  if (mPendingPcm != nullptr) {
    memset(mPendingPcm, 0, (mPendingPcmFrames + 1) * sizeof(short));
  }
  mPendingFrames = 0;

  this->audio->resetProcessor();
}

void EkhoImpl::closeStream(void) {
  this->audio->destroyProcessor();
}

int EkhoImpl::writePcm(short *pcm, int frames, void *arg, OverlapType type) {
  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::writePcm(frames=" << frames << ", type=" << type << ")" << endl;
  }
  EkhoImpl *pEkho = (EkhoImpl *)arg;
/*
  if (frames == 0) {
    // send finish event to speech-dispatcher
    EkhoImpl::speechdSynthCallback(nullptr, 0, 16,
      pEkho->audio->channels, pEkho->audio->sampleRate, 1);
  }*/

  short *buffer = new short[BUFFER_SIZE];
  pthread_mutex_lock(&(pEkho->mSpeechQueueMutex));

  if (!pEkho->isStopped) {        
    int flush_frames = pEkho->writeToSonicStream(pcm, frames, type);
    if (EkhoImpl::mDebug) {
      cerr << "EkhoImpl::writePcm: flush_frames=" << flush_frames << endl;
    }

    if (flush_frames) {
      do {
        if (pEkho->isStopped) {
          break;
        }

        while (pEkho->isPaused) {
          sleep(1);
        }

        frames = pEkho->audio->readShortFrames(buffer, BUFFER_SIZE);
        if (EkhoImpl::mDebug) {
          cerr << "EkhoImpl::writePcm: read frames=" << frames << endl;
        }

        if (frames > 0) {
          if (pEkho->mSndFile) {
            /*int writtenFrames = */sf_writef_short(pEkho->mSndFile, buffer, frames / pEkho->audio->channels);
            /*
            if (frames / pEkho->audio->channels != writtenFrames) {
              cerr << "Fail to write WAV file " << writtenFrames << " out of "
                   << frames << " written" << endl;
              return -1;
            }*/
          } else {
            if (EkhoImpl::speechdSynthCallback) {
              if (EkhoImpl::mDebug) {
                cerr << "EkhoImpl::speechdSynthCallback: " << frames << endl;
              }
              EkhoImpl::speechdSynthCallback(buffer, frames, 16,
                  pEkho->audio->channels, pEkho->audio->sampleRate, 0);
            } else {
            }
          }
        }
      } while (frames > 0);
    }
  }
  pthread_mutex_unlock(&(pEkho->mSpeechQueueMutex));

  delete[] buffer;
  buffer = nullptr;
  return 0;
}

int EkhoImpl::writeToSonicStream(short* pcm, int frames, OverlapType type) {

  if (mDebug) {
    cerr << "EkhoImpl::writeToSonicStream(frames=" << frames <<
      ", type=" << type << ")" << endl;
  }
  int i = 0;
  /*
  if (type != OVERLAP_NONE) {
    // 把前后音量为0的部分去掉
    int minLevel = 512;
    while (i < frames && abs(*pcm) < minLevel) {
      i++;
      pcm++;
    }
    if (i > 0) {
      // cerr << "trim left: " << i << endl;
      frames -= i;
    }

    i = 0;
    while (frames > 0 && abs(*(pcm + frames - 1)) < minLevel) {
      i++;
      frames--;
    }
    if (i > 0) {
      // cerr << "trim right: " << i << endl;
    }
  }*/

  // 如果将要播放的帧太多了，先播放掉缓冲区，并把多余缓冲区的部分不经交叠处理播放
  int remainBufferFrames = mPendingPcmFrames - mPendingFrames;
  while (frames > remainBufferFrames) {
    if (mDebug) {
      cerr << "flush frames: " << remainBufferFrames << endl;
    }
    memcpy(mPendingPcm + mPendingFrames, pcm, remainBufferFrames * 2);
    this->audio->writeShortFrames(mPendingPcm, mPendingPcmFrames);
    pcm += remainBufferFrames;
    frames -= remainBufferFrames;
    mPendingFrames = 0;
    remainBufferFrames = mPendingPcmFrames;
  }

  // 音量低于(quiet_level / 32767)的部分重叠
  //
  // Rate-gated overlap threshold for converted voice packs. The 44.1 kHz
  // packs were tuned with quiet_level = mOverlap: the engine walks each
  // chunk's quiet edges while samples stay below this threshold, and at
  // 44.1 kHz the walk stops on the onset/offset transients (bursts,
  // fricative tails) that still poke above it. An 8 kHz resample lowpasses
  // exactly those away, so the same threshold walks further per join and the
  // pack drifts from the 44.1 kHz timing. Scaling the threshold as
  // (rate/44100)^p puts the stop points back.
  //
  // p was first fitted indirectly: solve-threshold.py put the mean-zero
  // threshold at Q~1950 for the first-stage walks alone, and the engine was
  // then measured at Q=2048 (net overlap +995 ms over 58 joins), Q=1878
  // (-605 ms) and Q=1744 (-779 ms), which interpolated to Q~1925 -> p=0.442.
  // Measuring the finished engine on a wider corpus is better than fitting
  // parts of it: with 11 ordinary sentences plus the 7 listening texts
  // rendered at both rates and only the threshold changed, Q=1925 left the
  // ordinary sentences +0.28% long on average with a +6.46% worst case,
  // while Q=2050 centres them (mean +0.06%, median +0.20%) and halves the
  // worst case to 3.47%; texts within +/-1.5% go from 5/18 to 10/18. Q=2200
  // (-0.93% mean, 5.10% worst) and above start to overshoot. Hence p=0.405,
  // which is Q=2050 at 8 kHz. The residual spread is text-dependent: the
  // tongue-twister in the listening set (四是四，十是十...) stays ~-12% at
  // every threshold, because the walk reacts to the resampled waveform
  // rather than to a level, so one global threshold cannot do better. The
  // engine's second-stage q_level walks scale with quiet_level too, so the
  // first-stage solver alone under-corrects. 44.1 kHz and English keep
  // mOverlap exactly (byte-identical behaviour).
  int quiet_level = mOverlap;
  int voice_rate = mDict.mSfinfo.samplerate;
  if (mDict.getLanguage() != ENGLISH && voice_rate > 0 && voice_rate < 44100) {
    quiet_level = (int)(mOverlap * pow(voice_rate / 44100.0, 0.405) + 0.5);
  }
  int flushframes = 0;  // mPendingFrames里应该输出的frames

  // 下一段音频里，0到cpframe - 1是已经被合并到mPendingFrames里的，
  // 剩下的直接复制到mPendingFrames尾部
  int cpframe = 0;

  int startframe = 0; // 下一段音频的开始重叠位置
  int endframe = mPendingFrames; // 上一段音频的结束重叠位置
  int q_level = 0;

  // 保证每个拼音的单独长度，不被交叠太多。
  //
  // 上游用 de5 的长度 * 0.8 作为所有音节的下限，但 de5 是最长的音节之一：
  // 44100 Hz 语音包下 minFrames = 24336*0.8 = 19468 帧 = 0.44 秒，比绝大多数
  // 中文字（0.2~0.3 秒）还长。minFrames >= frames 时下面两个 while 循环
  // 条件恒为假，startframe/endframe 都不移动，重叠机制被整个短路，
  // 表现为「一个字一个字」、字与字之间留有明显空白。
  //
  // 改为相对当前音节自身长度，并留一个下限防止极短音节被吃光。
  // EKHO_MINRATIO 可调（百分比），默认 50。
  int minRatio = 50;
  const char* envRatio = getenv("EKHO_MINRATIO");
  if (envRatio) {
    minRatio = atoi(envRatio);
    if (minRatio < 0 || minRatio > 100) {
      minRatio = 50;
    }
  }
  int minFrames = (int)(frames * minRatio / 100);
  int minFramesFloor = (int)(mDict.mSfinfo.samplerate * 60 / 1000);  // 60 ms
  if (minFrames < minFramesFloor) {
    minFrames = minFramesFloor;
  }
  if (minFrames > frames) {
    minFrames = frames;
  }

  // 上一段（左边）音频剩下的帧数
  // mPendingFrames - endframe - 1 + startframe;
  int maxLeftFrames;
  // cerr << "frames:" << frames << ",minFrames:" << minFrames << ",type:" << type << endl;

  switch (type) {
    case OVERLAP_NONE:
      if (mDebug) {
        cerr << "writeToSonicStream: OVERLAP_NONE, mPendingFrames=" << mPendingFrames << endl;
      }
      memcpy(mPendingPcm + mPendingFrames, pcm, frames * 2);
      mPendingFrames += frames;
      flushframes = mPendingFrames;
      cpframe = frames;
      break;

    case OVERLAP_DEFAULT:
    case OVERLAP_QUIET_PART:
      // don't overlap more than 0.3(endframe)+0.3(startframe) of the syllable frames

      // 找出上一段音频尾部音量小的帧
      while (endframe > 0 &&
        mPendingFrames - endframe < frames * 0.3 &&
        mPendingFrames - endframe < (frames - minFrames) * 0.5 &&
        abs(mPendingPcm[endframe]) < quiet_level) {
        endframe--;
      }

      // 找出下一段音频头部音量小的帧
      while (startframe < frames * 0.3 &&
          startframe < (frames - minFrames) * 0.5 &&
          abs(pcm[startframe]) < quiet_level) {
        startframe++;
      }

      // 在上一段音频里再往前找一些帧，
      // 确保下一段音频头重叠起来不能超过最大音量
      i = endframe;
      q_level = 32767 - quiet_level;
      while (i > 0 && endframe - i < startframe &&
          abs(mPendingPcm[i]) < q_level) {
        i--;
      }

      // 下一段音频的帧并非都达到32767 - quiet_level水平，可以再尽可能多重叠一些
      q_level = 32767 - abs(mPendingPcm[i]);
      while (i > 0 && endframe - i < startframe &&
          abs(pcm[endframe - i]) < q_level) {
        i--;
      }

      // 有超过最大音量的部分，减少重叠的帧数
      if (startframe > endframe - i) {
        startframe = endframe - i;
      }

      // 上一段（左边）音频剩下的帧数
      maxLeftFrames = mPendingFrames - endframe - 1 + startframe;

      // 下一段音频的开始位置再往后找一些帧，
      // 确保和上一段音频重叠起来不能超过最大音量
      i = startframe;
      q_level = 32767 - quiet_level;
      while (i < frames && i < maxLeftFrames &&
          abs(pcm[i]) < q_level) {
        i++;
      }

      // 上一段音频的帧并非都达到32767 - quiet_level水平，可以再尽可能多重叠一些
      if (i < frames) {
        q_level = 32767 - abs(pcm[i]);
        while (i < frames && i < maxLeftFrames &&
            abs(mPendingPcm[mPendingFrames + startframe - i - 1]) < q_level) {
          i++;
        }
      }

      // 有超过最大音量的部分，减少重叠的帧数
      if (i < maxLeftFrames && i < frames) {
        //cerr << "endframe: " << endframe << " to " << mPendingFrames + startframe - i - 1<< endl;
        endframe = mPendingFrames + startframe - i - 1;
      }

      // 拼接上下两段音频里音量小的帧
      for (i = max(0, endframe - startframe);
          i < mPendingFrames && cpframe < frames; i++) {
        mPendingPcm[i] += pcm[cpframe];
        /*
        if (mPendingPcm[i] > 32767) {
          cerr << "overflow: " << mPendingPcm[i] << endl;
        }*/
        cpframe++;
      }

      if (mDebug) {
        cerr << "frames:" << frames << ", startframe: " << startframe <<
          ", endframe:" << endframe << ", overlap: " <<
          i - max(0, endframe - startframe) << endl;
      }

      if (frames == 0) {
        // frames=0 means flush all pending frames
        flushframes = mPendingFrames;
      } else if (mPendingFrames + mPendingFrames > frames) {
        // guaranteer pending frames no more than haft frames
        flushframes = mPendingFrames - frames * 0.5;
      }

      if (mDebug) {
        if (endframe < mPendingFrames - 1) {
          cerr << "clip endframe: " << mPendingFrames - endframe + 1 << endl;
        }

        if (startframe > 0) {
          cerr << "clip startframe: " << startframe << endl;
        }

        cerr << "cpframes: " << cpframe << ", flushframes: " << flushframes
          << ", mPendingFrames: " << mPendingFrames << endl;
      }

      break;

    case OVERLAP_HALF_PART:
      if (mDebug) {
        cerr << "OVERLAP_HALF_PART" << endl;
      }

      // find quiet frames of first char
      while (endframe > 0 && abs(mPendingPcm[endframe]) < 32767) {
        endframe--;
      }

      // find half but not too lound part of second char
      while (startframe < frames * 0.5 && abs(pcm[startframe]) < 32767) {
        startframe++;
      }

      for (i = max(endframe, mPendingFrames - startframe);
           i < mPendingFrames && cpframe < frames; i++) {
        mPendingPcm[i] += pcm[cpframe];
        cpframe++;
      }
      flushframes = i;

      // make a liner joining. fade out + fade in
      // Reference: splice.c of sox
      /*
      for (int i = 1; i < mPendingFrames && i <= frames; i++) {
        double p = i / mPendingFrames;
        mPendingPcm[i] = mPendingPcm[i] * (1 - p) + pcm[i] * p;
      }*/
      break;
  }

  this->audio->writeShortFrames(mPendingPcm, flushframes);
  mPendingFrames -= flushframes;
  if (mPendingFrames > 0 && flushframes > 0) {
    memmove(mPendingPcm, mPendingPcm + flushframes, mPendingFrames * 2);
  }

  if (frames > cpframe) {
    memcpy(mPendingPcm + mPendingFrames, pcm + cpframe,
     (frames - cpframe) * 2);
    mPendingFrames += frames - cpframe;
  }

  return flushframes;
}

void EkhoImpl::finishWritePcm(void) {
  writePcm(0, 0, this, OVERLAP_NONE);
}

int EkhoImpl::drainAudio(SynthCallback* callback, void* userdata) {
  if (callback == nullptr) {
    return 0;
  }

  // Publish whatever the overlap buffer still holds. writeToSonicStream() is
  // the only code that knows how much of mPendingPcm has not been resampled
  // yet, and OVERLAP_NONE with zero new frames means "emit all of it".
  if (mPendingFrames > 0) {
    short pad = 0;
    writeToSonicStream(&pad, 0, OVERLAP_NONE);
  }

  // Then make the resampler emit the input it has not converted. sonic's flush
  // pads with silence, converts, and then trims the padding off again, so this
  // yields exactly the missing tail and no extra silence. The flush is not
  // sticky: the stream keeps working normally afterwards.
  this->audio->flushFrames();

  short* buffer = new short[BUFFER_SIZE];
  int frames;
  do {
    frames = this->audio->readShortFrames(buffer, BUFFER_SIZE);
    if (frames > 0) {
      callback(buffer, frames, userdata, OVERLAP_NONE);
    }
  } while (frames > 0);
  delete[] buffer;

  return 0;
}

void* EkhoImpl::speechDaemon(void *args) {
  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::speechDaemon begin" << endl;
  }
  EkhoImpl *pEkho = (EkhoImpl *)args;

  pEkho->initEnglish();

  while (1) {
    pthread_mutex_lock(&pEkho->mSpeechQueueMutex);
    if (pEkho->mSpeechQueue.empty() && !pEkho->isEnded) {
      if (EkhoImpl::mDebug) {
        cerr << "EkhoImpl::speechDaemon waiting speech queue" << endl;
      }
      pthread_cond_wait(&pEkho->mSpeechQueueCond, &pEkho->mSpeechQueueMutex);
    }

    if (pEkho->isEnded) {
      pthread_mutex_unlock(&pEkho->mSpeechQueueMutex);
      break;
    }

    if (pEkho->mSpeechQueue.empty()) {
      pthread_mutex_unlock(&pEkho->mSpeechQueueMutex);
      continue;
    }

    SpeechOrder order = pEkho->mSpeechQueue.front();
    pEkho->isStopped = false;
    pthread_mutex_unlock(&pEkho->mSpeechQueueMutex);

    // It seems that Sonic doesn't work on threads. Set arguments again. It's
    // fixed before. But it doesn't work again...
    /* @fixme: reset tempoDelta will cause double speed from 139 to 278
    // too quick for pinyin-huang will break with 天才是怎样练成的 赣F54562
    pEkho->setSpeed(pEkho->tempoDelta);
    pEkho->setPitch(pEkho->pitchDelta);
    pEkho->setVolume(pEkho->volumeDelta);
    pEkho->setRate(pEkho->rateDelta);
    */

    if (EkhoImpl::mDebug) {
      cerr << "EkhoImpl::speechDaemon synth2 " << order.text << endl;
    }
    pEkho->synth2(order.text, writePcm);

    if (!pEkho->isStopped) {
      // try to add pause to flush short pcm char
      int size = 0;
      const char* pcm = pEkho->mDict.getQuaterPause()->getPcm(size);
      if (EkhoImpl::mDebug) {
        cerr << "add quaterpause " << size << endl;
      }
      pEkho->writePcm((short *)pcm, size / 2, pEkho, OVERLAP_NONE);
      // pEkho->speakPcm(0, 0, pEkho, OVERLAP_NONE);
      if (EkhoImpl::speechdSynthCallback) {
        // @fixme: following two statements seems not flush rest PCM
        // It seems that it has not use at all.
        // should work without it.
        EkhoImpl::speechdSynthCallback(0, 0, 0, 0, 0, 1);
      }

      if (EkhoImpl::mDebug) {
        cerr << "EkhoImpl::speechDaemon synth2 end" << endl;
      }

    }

    if (!pEkho->isStopped) {
      pthread_mutex_lock(&pEkho->mSpeechQueueMutex);
      if (order.pCallback) {
        order.pCallback(order.pCallbackArgs);
      }

      if (!pEkho->mSpeechQueue.empty()) {
        pEkho->mSpeechQueue.pop();
      }
      pthread_mutex_unlock(&pEkho->mSpeechQueueMutex);
    }

    if (pEkho->isStopped) {
      while (not pEkho->mSpeechQueue.empty()) {
        pEkho->mSpeechQueue.pop();
      }


      #ifdef ENABLE_ESPEAK
        espeak_Cancel();
#endif
    }
  }

  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::speechDaemon end" << endl;
  }

  return 0;
}  // end of speechDaemon

int EkhoImpl::play(string file) {
  int ret = system((this->player + " " + file + " 2>/dev/null").c_str());

  return ret;
}

int EkhoImpl::setVoice(string voice) {
  if (mDebug) {
    cerr << "[" << pthread_self() << "] EkhoImpl::setVoice" << endl;
  }

  if (voice.compare(mDict.getVoice()) == 0) return 0;

  if (voice.compare("Cantonese") == 0 || voice.compare("yue") == 0) {
    voice = "jyutping";
    mIsMale = true;
  } else if (voice.compare("Mandarin") == 0 || voice.compare("zh") == 0 ||
             voice.compare("cmn") == 0) {
    voice = "pinyin";
    mIsMale = false;
    // setEnglishVoice("voice_cmu_us_slt_arctic_hts");
  } else if (voice.compare("Korean") == 0 || voice.compare("ko") == 0) {
    voice = "hangul";
    mIsMale = false;
    // setEnglishVoice("voice_cmu_us_slt_arctic_hts");
  } else if (voice.compare("Toisanese") == 0) {
    voice = "toisanese";
    mIsMale = true;
  } else if (voice.compare("Hakka") == 0) {
    voice = "hakka";
    mIsMale = true;
  } else if (voice.compare("Tibetan") == 0 || voice.compare("bo") == 0) {
    voice = "tibetan";
    mIsMale = false;
    // setEnglishVoice("voice_cmu_us_slt_arctic_hts");
  } else if (voice.compare("Ngangien") == 0) {
    voice = "ngangien";
    mIsMale = true;
  } else if (voice.compare("en") == 0) {
    voice = "English";
    mIsMale = true;
  }

  // Fail here rather than a few lines down. mDict.setVoice() assumes
  // setLanguage() has populated the dictionary with the new language's
  // characters, and on a dictionary that failed to load (Tibetan, for instance,
  // has no data directory in the shipped voice pack) it walks a structure that
  // was never filled in and segfaults. Ignoring this return code therefore meant
  // any caller could select an uninstalled voice and take the process down; in a
  // server that is one HTTP request from any client on the network.
  int langRet = 0;
  if (voice.find("jyutping") == 0) {
    langRet = mDict.setLanguage(CANTONESE);
  } else if (voice.find("pinyin") == 0) {
    langRet = mDict.setLanguage(MANDARIN);
  } else if (voice.find("hangul") == 0) {
    langRet = mDict.setLanguage(KOREAN);
  } else if (voice.find("hakka") == 0) {
    langRet = mDict.setLanguage(HAKKA);
  } else if (voice.find("toisanese") == 0) {
    langRet = mDict.setLanguage(TOISANESE);
  } else if (voice.find("tibetan") == 0) {
    langRet = mDict.setLanguage(TIBETAN);
  } else if (voice.find("ngangien") == 0) {
    langRet = mDict.setLanguage(NGANGIEN);
  } else if (voice.find("English") == 0) {
    langRet = mDict.setLanguage(ENGLISH);
  } else {
    cerr << "Invalid voice: " << voice << ". Fallback to Mandarin." << endl;
    langRet = mDict.setLanguage(MANDARIN);
    voice = "pinyin";
  }

  if (langRet != 0) {
    cerr << "ekho: voice '" << voice
         << "' is not installed: its dictionary data is missing from "
         << mDict.mDataPath << endl;
    return -2;
  }

  if (mDict.setVoice(voice.c_str()) != 0) {
    cerr << "Fail to setVoice of dictionary" << endl;
    return -2;
  }

  this->initStream();

  // Apply the tempo normalisation for the voice that initStream() just loaded.
  //
  // This has to happen here, after initStream(), and not in the Mandarin branch
  // above: setSpeed() derives baseDelta from mDict.mSfinfo.frames, and until
  // setLanguage()/setVoice()/initStream() have run, mSfinfo still describes the
  // *previous* voice -- usually nothing at all, so frames is 0, baseDelta comes
  // out 0 and setTempo() is never called. Selecting Mandarin therefore used to
  // leave the tempo at 1.0, and the engine spoke 2.4x slower than intended.
  //
  // The CLI hid this by calling setSpeed() again after setVoice() (ekho.cpp),
  // so only callers that drove the library directly -- Ekho::synth4(), which is
  // the whole streaming path -- noticed. Same bug shape as the "espeak was never
  // initialised outside speechDaemon()" one: the CLI did the extra step by hand
  // and the library never learned to do it itself.
  setSpeed(0);

  return 0;
}

string EkhoImpl::getVoice(void) { return this->mDict.getVoice(); }

int EkhoImpl::speak(string text, void (*pCallback)(void *),
                    void *pCallbackArgs) {
  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::speak(" << text << ") begin" << endl;
  }

  this->initSound();
  this->isPaused = false;
  // this->isStopped = false;
  SpeechOrder order;
  order.text = text;
  order.pCallback = pCallback;
  order.pCallbackArgs = pCallbackArgs;
  pthread_mutex_lock(&mSpeechQueueMutex);
  mSpeechQueue.push(order);
  pthread_mutex_unlock(&mSpeechQueueMutex);
  pthread_cond_signal(&mSpeechQueueCond);

  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::speak end" << endl;
  }

  return 0;
}

int EkhoImpl::stopAndSpeak(string text, void (*pCallback)(void *),
                           void *pCallbackArgs) {
  this->stop();
  this->speak(text, pCallback, pCallbackArgs);
  return 0;
}

int EkhoImpl::blockSpeak(string text) {
  return 0;
}

int EkhoImpl::pause(void) {
  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::pause" << endl;
  }

  if (!this->isPaused) {
    this->isPaused = true;
    return 0;
  } else {
    return 1;
  }
}

int EkhoImpl::resume(void) {
  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::resume" << endl;
  }

  if (this->isPaused) {
    this->isPaused = false;
    return 0;
  } else {
    return 1;
  }
}

int EkhoImpl::stop(void) {
  if (EkhoImpl::mDebug) {
    cerr << "EkhoImpl::stop" << endl;
  }
  this->isPaused = false;
  this->isStopped = true;

/*
  pthread_mutex_lock(&mSpeechQueueMutex);
  while (not mSpeechQueue.empty()) {
    mSpeechQueue.pop();
  }

  this->mPendingFrames = 0;
#ifdef ENABLE_FESTIVAL
  festival_eval_command("(audio_mode 'shutup)");
#endif

#ifdef ENABLE_ESPEAK
  espeak_Cancel();
#endif

  pthread_mutex_unlock(&mSpeechQueueMutex);
*/

  return 0;
}

void EkhoImpl::setSpeed(int tempo_delta) {
  int baseDelta = 0;
  // Tempo normalisation for the Chinese voice packs. This is NOT the same
  // quantity as espeak's RATE and must not be fed to it: baseDelta is a tempo
  // multiplier applied by the resampler downstream (setTempo(139) means
  // "play 2.39x faster"), whereas espeakRATE is words per minute.
  //
  // It is load-bearing. The 44.1 kHz Mandarin pack is recorded slowly, and
  // without this offset a sentence of 42 characters takes 20.3 s instead of the
  // intended 8.96 s. Removing it looks like an obvious cleanup -- the constant
  // 20362 has no provenance and `frames` is whatever voice file was loaded last
  // -- but it changes the speaking rate of every Chinese sentence by 2.4x.
  // English used to subtract this same value from espeakRATE, which is the bug
  // that was fixed in setEnglishSpeed().
  if (mDict.getLanguage() == MANDARIN && mDict.mSfinfo.frames > 0) {
    // Floating point on purpose. `frames * 44100 / samplerate` is the de5
    // duration expressed in 44.1 kHz frames, and integer division truncates
    // twice on the way in, which is enough to move baseDelta by one step:
    // a 48 kHz pack whose frames/samplerate does not divide evenly lands on
    // 238.x instead of 239.x and speaks 0.42% slower. Same value as before
    // for the 44.1 kHz pack, where the numbers happen to divide exactly.
    baseDelta = (int)round(mDict.mSfinfo.frames * 2.0 * 44100.0 * 100.0 /
                           mDict.mSfinfo.samplerate / 20362.0) - 100;
    if (this->mDebug) {
      cerr << "setSpeed baseDelta=" << baseDelta << " (frames="
           << mDict.mSfinfo.frames << " samplerate="
           << mDict.mSfinfo.samplerate << ")" << endl;
    }
  }

  if (baseDelta + tempo_delta != 0 || tempo_delta != this->tempoDelta) {
    if (this->mDebug) {
      cerr << "baseDelta=" << baseDelta << ", tempo delta: " << baseDelta + tempo_delta << endl;
    }

    this->tempoDelta = this->audio->setTempo(baseDelta + tempo_delta);
  }
}

void EkhoImpl::translatePunctuations(string &text, EkhoPuncType mode) {
  bool changed = false;

  string text2;
  bool in_chinese_context = true;

  int c;
  string::iterator it = text.begin();
  string::iterator it2 = text.begin();
  string::iterator end = text.end();

  while (it != end) {
    it2 = it;
#if 1
    try {
      c = utf8::next(it, end);
    } catch (utf8::not_enough_room &) {
      text = text2;
      return;
    } catch (utf8::invalid_utf8 &) {
      cerr << "translatePunctuations: Invalid UTF8 encoding" << endl;
      text = text2;
      return;
    }
#endif

    if (in_chinese_context && mDict.isPunctuationChar(c, mode)) {
      text2 += mDict.getPunctuationName(c);
      changed = true;
    } else {
      while (it2 != it) {
        text2.push_back(*it2);
        it2++;
      }
      in_chinese_context = (c > 128 || (c >= '0' && c <= '9'));
    }

    while (it2 != it) it2++;
  }

  if (changed) {
    text = text2;
  }
}

void EkhoImpl::setSpeechdSynthCallback(SpeechdSynthCallback *callback) {
  EkhoImpl::speechdSynthCallback = callback;
  this->audio->speechdSynthCallback = callback;
}
