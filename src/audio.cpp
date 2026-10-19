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

#include <iostream>
#include <string>
#include <stdlib.h>
#include <sndfile.h>
#include <pthread.h>
#include <fstream>
#include <sys/stat.h>
#include "audio.h"
#include "ekho_impl.h"

#ifdef HAVE_MPG123
#include <mpg123.h>
#include <out123.h>
#endif


using namespace std;

namespace ekho {
string Audio::tempDirectory = "";
bool Audio::debug = false;

Audio::Audio(void) {
  this->pitchDelta = 0;
  this->volumeDelta = 0;
  this->rateDelta = 0;
  this->tempoDelta = 0;
  this->sampleRate = 0; // Ekho voice source sample rate
  this->currentSampleRate = 0; // sonic processing sample (change for espeak)
  this->outputSampleRate = 0; // for readShortFrames
  this->channels = 1;
  this->speechdSynthCallback = nullptr;
  this->hasProcessorInited = false;
  this->pcmFrameBuffer = nullptr;
  this->pcmFrameMaxSize = 0;
  this->pcmFrameCurrentSize = 0;

#ifdef HAVE_SONIC
  this->processorStream = nullptr;
#endif

#ifdef HAVE_MPG123
  this->mpg123Handle = nullptr;
#endif

}

Audio::~Audio(void) {
#ifdef HAVE_SONIC
  if (this->processorStream) {
    sonicDestroyStream(this->processorStream);
    this->processorStream = 0;
  }
#endif

#ifdef HAVE_MPG123
  if (this->mpg123Handle) {
    mpg123_delete(this->mpg123Handle);
    mpg123_exit();
    this->mpg123Handle = nullptr;
  }
#endif
}

void Audio::initProcessor() {
  if (this->outputSampleRate == 0) {
    cerr << "Audio::initProcessor: Fail! You should setSampleRate first." << endl;
    return;
  }

  this->initProcessor(this->outputSampleRate, this->channels);
}

void Audio::initProcessor(int samplerate, int channels) {
  if (Audio::debug) {
    cerr << "[" << pthread_self() << "] Audio::initProcessor: " << samplerate << endl;
  }

  this->outputSampleRate = samplerate;
  this->channels = channels;

#ifdef HAVE_SONIC
  this->processorStream = sonicCreateStream(samplerate, 1);
  sonicSetQuality(this->processorStream, 1); // high quality but slower
  this->pcmFrameBuffer = (short*)malloc(20480 * sizeof(short));
  this->pcmFrameMaxSize = 20480;
#endif

  if (this->outputSampleRate != this->sampleRate) {
    this->setPitchFloat(1);
    this->setTempoFloat(1);
  }

  this->hasProcessorInited = true;
}

void Audio::destroyProcessor() {
#ifdef HAVE_SONIC
  if (this->processorStream) {
    sonicDestroyStream(this->processorStream);
    this->processorStream = 0;
  }
  this->pcmFrameMaxSize = 0;
  this->pcmFrameCurrentSize = 0;
  free(this->pcmFrameBuffer);
  this->pcmFrameBuffer = nullptr;
#endif
}

void Audio::resetProcessor() {
#ifdef HAVE_SONIC
  if (this->processorStream) {
    sonicResetStream(this->processorStream);
  }
#endif
}


int Audio::setTempo(int delta) {
#if 1
  if (delta < -50) {
    cerr << "Audio::setTempo out of range: " << delta << endl;
    delta = -50;
  }

  if (delta > 300) {
    cerr << "Audio::setTempo out of range: " << delta << endl;
    delta = 300;
  }

  this->setTempoFloat((float)(100 + delta) / 100);
  this->tempoDelta = delta;
#endif

  return this->tempoDelta;
}

// 1 means no change. 2 means double speed
void Audio::setTempoFloat(float factor) {
  /*
  if (!this->processorStream) {
    cerr << "Audio::processorStream not init" << endl;
  }*/

  float finalFactor = factor * this->sampleRate / this->outputSampleRate;

  if (Audio::debug) {
    cerr << "Audio::setTempoFloat: " << factor <<
      ", finalFactor=" << finalFactor << endl;
  }


#ifdef HAVE_SONIC
  sonicSetSpeed(this->processorStream, finalFactor);
#endif
}

// 1 means no change. 2 means double high pitch
void Audio::setPitchFloat(float factor) {
#ifdef HAVE_SONIC
  if (!this->processorStream) {
    cerr << "Audio::processorStream not init" << endl;
    return;
  }

  float finalFactor = factor * this->sampleRate / this->outputSampleRate;

  if (Audio::debug) {
    cerr << "Audio::setPitchFloat: " << factor <<
        ", finalFactor=" << finalFactor <<
        ", sampleRate=" << this->sampleRate <<
        ", outputSampleRate=" << this->outputSampleRate <<
        endl;
  }

  sonicSetPitch(this->processorStream, finalFactor);
#endif
}

// 设置（英语）输入源的sample rate，sonic需要调整当前PCM流的语速，
// 让后来不同的sample rate PCM流和之前的语速一致
int Audio::setSampleRate(int rate) {
  float r = (float)rate / this->outputSampleRate;
  if (Audio::debug) {
    cerr << "Audio::setSampleRate: " << rate <<
        ", outputSampleRate: " << this->outputSampleRate <<
        ", sampleRate: " << this->sampleRate <<
        ", target rate: " << r << endl;
  }

  //flushFrames();
  //sonicSetRate(this->processorStream, r);
#ifdef HAVE_SONIC
  sonicSetRate(this->processorStream, (float)rate / this->sampleRate);
  this->currentSampleRate = rate;
#endif
  return rate;
}

void Audio::setInputSampleRate(int rate) {
  this->sampleRate = rate;
  if (this->outputSampleRate == 0) {
    this->outputSampleRate = rate;
  }
}

void Audio::setOutputSampleRate(int rate) {
  if (Audio::debug) {
    cerr << "Audio::setOutputSampleRate: " << rate << endl;
  }

  if (rate > 0) {
    this->outputSampleRate = rate;
  }
}

void Audio::setChannels(int channels) {
  if (Audio::debug) {
    cerr << "Audio::setChannels: " << channels << endl;
  }

  this->channels = channels;
}

int Audio::setPitch(int delta) {
  /*
  if (!hasProcessorInited) {
    cerr << "Audio processor has not initialized." << endl;
    return 0;
  }*/

  if (Audio::debug) {
    cerr << "Audio::setPitch: " << delta << endl;
  }

  if (delta < -100) {
    delta = -100;
  }
  if (delta > 100) {
    delta = 100;
  }
  this->pitchDelta = delta;

#if 1
  if (delta < 0) {
    this->setPitchFloat(((float)delta / 2 + 100) / 100);
  } else {
    this->setPitchFloat((float)(100 + delta) / 100);
  }
#endif

  return this->pitchDelta;
}

int Audio::setVolume(int delta) {
  /*
  if (!hasProcessorInited) {
    cerr << "Audio processor has not initialized." << endl;
    return 0;
  }*/

  if (delta >= -100 && delta <= 100) {
#ifdef HAVE_SONIC
    this->volumeDelta = delta;
    // @TODO: Using sonic's setVolume doesn't work. Don't know why...
    if (this->processorStream) {
      sonicSetVolume(this->processorStream, (float)(100 + delta) / 100);
    }
#endif
  }

  return this->volumeDelta;
}

int Audio::setRate(int delta) {
  if (Audio::debug) {
    cerr << "Audio::setRate: " << delta << endl;
  }

#ifdef HAVE_SONIC
  if (this->processorStream) {
    sonicSetRate(this->processorStream, (float)(100 + delta) / 100);
  }
  this->rateDelta = delta;
#endif

  return this->rateDelta;
}

int Audio::readShortFrames(short buffer[], int size) {
#ifdef HAVE_SONIC
  if (!this->processorStream) {
    cerr << "processorStream not initialized" << endl;
    return 0;
  }
  // sonic会自动剪去一些空白的frame?

  if (this->channels == 2) {
    int monoSize = sonicReadShortFromStream(this->processorStream, buffer, size / 2);
    for (int i = monoSize; i >= 0; i--) {
      buffer[i + i + 1] = buffer[i];
      buffer[i + i] = buffer[i];
    }
    return monoSize + monoSize;
  } else {
    // mono
    return sonicReadShortFromStream(this->processorStream, buffer, size);
  }
  if (size > this->pcmFrameCurrentSize) {
    size = this->pcmFrameCurrentSize;
  }
  //cerr << "readShortFrames: " << size << endl;
  memcpy(buffer, this->pcmFrameBuffer, size * sizeof(short));
  this->pcmFrameCurrentSize -= size;
  if (this->pcmFrameCurrentSize > 0) {
    memmove(this->pcmFrameBuffer, this->pcmFrameBuffer + size,
      this->pcmFrameCurrentSize * sizeof(short));
  }
  return size;
#endif

  return 0;
}

int Audio::writeShortFrames(short buffer[], int size) {
#ifdef HAVE_SONIC
  if (!this->processorStream) {
    cerr << "processorStream not initialized" << endl;
    return 0;
  }

  return sonicWriteShortToStream(this->processorStream, buffer, size);
  while (this->pcmFrameCurrentSize + size > this->pcmFrameMaxSize) {
    this->pcmFrameMaxSize *= 2;
    this->pcmFrameBuffer = (short*)realloc(this->pcmFrameBuffer,
      this->pcmFrameMaxSize * sizeof(short));
  }

  memcpy(this->pcmFrameBuffer + this->pcmFrameCurrentSize, buffer,
    size * sizeof(short));
  this->pcmFrameCurrentSize += size;
  return size;
#endif
}

void Audio::flushFrames() {
#ifdef HAVE_SONIC
  if (!this->processorStream) {
    cerr << "processorStream not initialized" << endl;
    return;
  }
  sonicFlushStream(this->processorStream);
#endif
}

void Audio::play(const string& path) {
  //cerr << "Audio::play:" << path << endl;

  SF_INFO sfinfo;
  SNDFILE *sndfile = sf_open(path.c_str(), SFM_READ, &sfinfo);
  if (!sndfile) {
    return;
  }

  if (sfinfo.frames <= 0) {
    sf_close(sndfile);
    return;
  }

  short *pcm = new short[sfinfo.frames];
  sf_readf_short(sndfile, pcm, sfinfo.frames);

  if (this->speechdSynthCallback) {
    // @TODO: read real bits
    speechdSynthCallback(pcm, sfinfo.frames, 16, sfinfo.channels, sfinfo.samplerate, 0);
  }

  delete[] pcm;
  pcm = nullptr;
  sf_close(sndfile);
}

std::string Audio::getTempDir()
{
#if 1
    const char* env_tmpdir = std::getenv("TMPDIR");
    std::string temp_path;
    if (env_tmpdir != nullptr && strlen(env_tmpdir) > 0) {
        temp_path = env_tmpdir;
        if (temp_path.back() != '/') {
            temp_path += '/';
        }
        struct stat stat_buf{};
        if (stat(temp_path.c_str(), &stat_buf) != 0 || !S_ISDIR(stat_buf.st_mode)) {
            temp_path = "/tmp/";
        }
    } else {
        temp_path = "/tmp/";
    }
    return temp_path;
#endif
}

// generate temp filename
// to be improve...
string Audio::genTempFilename() {
  string tempFilePath;
  if (Audio::tempDirectory.empty()) {
    // Upstream hardcoded "\\TEMP\\ekho" for its MSVC build. That is a folder
    // named TEMP in the root of the current drive, which does not exist on a
    // normal Windows install, so every fopen() below failed and each syllable
    // decoded to silence. Use getTempDir() (%TEMP%) on every platform instead.
    tempFilePath = Audio::getTempDir() + "ekho";
  } else {
    tempFilePath = Audio::tempDirectory;
  }

  static int count = 0;
  count++;
  tempFilePath.append(to_string(count));

  return tempFilePath;
}

void Audio::setTempDirectory(string dir) {
  Audio::tempDirectory = dir;
  struct stat st;
  if (stat(dir.c_str(), &st) == 0) {
    if ((st.st_mode & S_IFDIR) != 0) {
    }
  } else {
    const int dir_err = mkdir(dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
#if 1
    if (-1 == dir_err) {
      cerr << "Error creating directory: %s" << dir.c_str() << endl;
    }
#endif
  }
}

// It's caller's responsibility to delete return short space
short* Audio::readPcmFromAudioFile(string filepath, int& size) {
  if (filepath.empty()) {
    cerr << "Audio::readPcmFromAudioFile: empty filepath" << endl;
    size = 0;
    return nullptr;
  }

  if (filepath.back() == '3') {
    // SNDFILE默认配置下不支持MP3: http://www.mega-nerd.com/libsndfile/FAQ.html#Q020
    return this->readPcmFromMp3File(filepath, size);
  }

  SF_INFO sfinfo = {0};
  SNDFILE* sndfile = sf_open(filepath.c_str(), SFM_READ, &sfinfo);
  if (!sndfile) {
    cerr << "Fail to open " << filepath << endl;
    return nullptr;
  }

  if (sfinfo.frames <= 0) {
    cerr << filepath << " is empty." << endl;
    sf_close(sndfile);
    return nullptr;
  }

  size = sfinfo.frames;
  short* pcm = new short[size];

  sf_readf_short(sndfile, pcm, size);
  sf_close(sndfile);

  return pcm;
}

void Audio::initMp3Processor() {
#ifdef HAVE_MPG123
  if (!mpg123Handle) {
    mpg123_init();
    int err;
    this->mpg123Handle = mpg123_new(nullptr, &err);
  }
#endif
}

// It's caller's responsibility to delete return short space
short* Audio::amplifyPcm(short* pcm, int size, float rate) {
  short* amplifiedPcm = new short[size];
  for (int i = 0; i < size; i++) {
    int sample = (int)pcm[i] * rate;

    if (sample > 32767) {
      cerr << "[Warning] Audio::amplifyPcm(" << pcm[i] << "," << rate << ") overflow, try to lower the amplify rate: " << sample << endl;
      sample = 32767;
    }

    if (sample < -32768) {
      cerr << "[Warning] Audio::amplifyPcm(" << pcm[i] << "," << rate << ") overflow, try to lower the amplify rate: " << sample << endl;
      sample = -32768;
    }

    amplifiedPcm[i] = (short)sample;
  }

  return amplifiedPcm;
}

// It's caller's responsibility to delete return short space
short* Audio::readPcmFromMp3File(string filepath, int& size) {
#ifdef HAVE_MPG123
  this->initMp3Processor();

  size_t blockSize = mpg123_outblock(this->mpg123Handle);
  size_t bufferSize = blockSize;
  unsigned char* buffer = (unsigned char*)malloc(bufferSize * sizeof(unsigned char));
  unsigned char* bufferPtr = buffer;
  int channels, encoding;
  long rate;

  mpg123_open(this->mpg123Handle, filepath.c_str());
  mpg123_getformat(this->mpg123Handle, &rate, &channels, &encoding);
  size_t readBytes = 0;
  size_t totalBytes = 0;
  int ret = 0;

  while ((ret = mpg123_read(this->mpg123Handle, bufferPtr, blockSize, &readBytes)) != MPG123_DONE) {
    if (readBytes != blockSize) {
      cerr << "something wrong with mpg123_read: readBytes=" <<
       readBytes << ", blockSize=" << blockSize << ", ret=" << ret <<
       ", MPG123_DONE=" << MPG123_DONE << endl;
    }
    bufferSize += blockSize;
    buffer = (unsigned char*)realloc(buffer, bufferSize * sizeof(unsigned char*));
    bufferPtr = buffer + blockSize;
    totalBytes += readBytes;
  }
  mpg123_close(this->mpg123Handle);
  totalBytes += readBytes;
  size = totalBytes / 2;
  // cerr << "size: " << size << endl;

  return (short*)buffer;
  return 0;
#endif
}

int EkhoImpl::saveWav(string text, string filename) {
  initEnglish();
  if (EkhoImpl::mDebug) {
    cerr << "Writting WAV file " << filename << " ..." << endl;
  }

  // open record file
  SF_INFO sfinfo;
  memcpy(&sfinfo, &mDict.mSfinfo, sizeof(SF_INFO));
  sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  sfinfo.samplerate = this->audio->outputSampleRate;
  sfinfo.channels = this->audio->channels;

  if (EkhoImpl::mDebug) {
    cerr << "sfinfo format: samplerate=" << sfinfo.samplerate
         << ", channel=" << sfinfo.channels;
    cerr.setf(ios::hex);
    cerr << ", format=" << sfinfo.format << endl;
    cerr.unsetf(ios::hex);
  }
  mSndFile = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
  if (!mSndFile) {
    cerr << "Fail to open file " << filename << " at " << __LINE__ << endl;
  }

  synth2(text, writePcm);
  finishWritePcm();

  // close record file
  sf_close(mSndFile);
  mSndFile = nullptr;

  if (EkhoImpl::mDebug) cerr << "Finish writting WAV file " << filename << endl;

  return 0;
}

int EkhoImpl::saveOgg(string text, string filename) {
  initEnglish();
  if (EkhoImpl::mDebug) {
    cerr << "Writting OGG file " << filename << " ..." << endl;
  }

  // open record file
  SF_INFO sfinfo;
  memcpy(&sfinfo, &mDict.mSfinfo, sizeof(SF_INFO));
  sfinfo.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
  if (EkhoImpl::mDebug) {
    cerr << "sfinfo format: samplerate=" << sfinfo.samplerate
         << ", channel=" << sfinfo.channels;
    cerr.setf(ios::hex);
    cerr << ", format=" << sfinfo.format << endl;
    cerr.unsetf(ios::hex);
  }
  mSndFile = sf_open(filename.c_str(), SFM_WRITE, &sfinfo);
  if (!mSndFile) {
    cerr << "Fail to open file " << filename << " at " << __LINE__ << endl;
    cerr << "error code: " << sf_error(0) << endl;
    cerr << "error: " << sf_strerror(0) << endl;
  }

  synth2(text, writePcm);
  finishWritePcm();

  // close record file
  sf_close(mSndFile);
  mSndFile = nullptr;

  if (EkhoImpl::mDebug) {
    cerr << "Finish writting WAV file " << filename << " ..." << endl;
  }
  return 0;
}

}