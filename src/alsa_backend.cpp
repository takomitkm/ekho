/***************************************************************************
 * ekho-linux -- ALSA output backend. See alsa_backend.h for why this is
 * dlopen-based.
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include "alsa_backend.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <alsa/asoundlib.h>

namespace ekho {

namespace {

// Candidate sonames, newest first. ".2" is what every distribution ships;
// the bare name only resolves when libasound2-dev is installed.
const char* kSonomes[] = {"libasound.so.2", "libasound.so", "libasound.so.0"};

const char* kDeviceFromEnv = "EKHO_ALSA_DEVICE";

}  // namespace

AlsaOutput::AlsaOutput()
    : lib_(nullptr),
      handle_(nullptr),
      fn_open_(nullptr),
      fn_close_(nullptr),
      fn_set_params_(nullptr),
      fn_writei_(nullptr),
      fn_drain_(nullptr),
      fn_prepare_(nullptr),
      fn_recover_(nullptr),
      fn_avail_delay_(nullptr),
      fn_strerror_(nullptr),
      fn_name_(nullptr),
      rate_(0),
      channels_(0),
      lastErrno_(0) {}

AlsaOutput::~AlsaOutput() {
  closeDevice();
  if (lib_) {
    dlclose(lib_);
    lib_ = nullptr;
  }
}

bool AlsaOutput::bind() {
  if (fn_writei_) return true;  // already bound

  for (const char* so : kSonomes) {
    lib_ = dlopen(so, RTLD_LAZY | RTLD_LOCAL);
    if (lib_) break;
  }
  if (!lib_) {
    error_ = std::string("dlopen libasound failed: ") + dlerror();
    return false;
  }

  struct Sym {
    const char* name;
    void** slot;
    bool required;
  };
  // snd_pcm_avail_delay and the two string helpers are optional: without them
  // we fall back to a plain blocking write, which is correct but unpaced.
  Sym syms[] = {
      {"snd_pcm_open", reinterpret_cast<void**>(&fn_open_), true},
      {"snd_pcm_close", reinterpret_cast<void**>(&fn_close_), true},
      {"snd_pcm_set_params", reinterpret_cast<void**>(&fn_set_params_), true},
      {"snd_pcm_writei", reinterpret_cast<void**>(&fn_writei_), true},
      {"snd_pcm_drain", reinterpret_cast<void**>(&fn_drain_), true},
      {"snd_pcm_prepare", reinterpret_cast<void**>(&fn_prepare_), true},
      {"snd_pcm_recover", reinterpret_cast<void**>(&fn_recover_), true},
      {"snd_pcm_avail_delay", reinterpret_cast<void**>(&fn_avail_delay_), false},
      {"snd_strerror", reinterpret_cast<void**>(&fn_strerror_), false},
      {"snd_pcm_name", reinterpret_cast<void**>(&fn_name_), false},
  };

  for (Sym& s : syms) {
    void* sym = dlsym(lib_, s.name);
    if (!sym) {
      if (s.required) {
        error_ = std::string("libasound is missing ") + s.name;
        dlclose(lib_);
        lib_ = nullptr;
        return false;
      }
      continue;
    }
    *s.slot = sym;
  }

  // Sanity check: the vendored header's enum must agree with what the runtime
  // library reports, or we are against a different ABI.
  void* nameSym = dlsym(lib_, "snd_pcm_format_name");
  if (nameSym) {
    typedef const char* (*Fn)(int);
    const char* got = reinterpret_cast<Fn>(nameSym)(SND_PCM_FORMAT_S16_LE);
    if (got && std::strcmp(got, "S16_LE") != 0) {
      char val[32];
      std::snprintf(val, sizeof(val), "%d",
                    static_cast<int>(SND_PCM_FORMAT_S16_LE));
      error_ = std::string("alsa ABI mismatch: SND_PCM_FORMAT_S16_LE=") + val +
               " reports as \"" + got + "\"";
      dlclose(lib_);
      lib_ = nullptr;
      return false;
    }
  }
  return true;
}

bool AlsaOutput::available() {
  AlsaOutput probe;
  return probe.bind();
}

int AlsaOutput::describe(int err) {
  lastErrno_ = err;
  const char* msg = fn_strerror_ ? fn_strerror_(err) : nullptr;
  if (msg) {
    error_ = msg;
  } else {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "alsa error %d", err);
    error_ = buf;
  }
  return err;
}

int AlsaOutput::openDevice(const std::string& device, int rate, int channels,
                           unsigned int latencyMs) {
  closeDevice();
  error_.clear();
  lastErrno_ = 0;

  if (!bind()) return -1;

  device_ = device.empty() ? "default" : device;
  if (device_ == "default" || device_ == "auto") {
    const char* env = getenv(kDeviceFromEnv);
    if (env && *env) device_ = env;
  }
  rate_ = rate;
  channels_ = channels;

  void* h = nullptr;
  int err = fn_open_(&h, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    std::string dev = device_;
    describe(err);
    error_ = "cannot open ALSA device \"" + dev + "\": " + error_;
    dlclose(lib_);
    lib_ = nullptr;
    fn_writei_ = nullptr;
    return err;
  }
  handle_ = h;

  err = fn_set_params_(handle_, SND_PCM_FORMAT_S16_LE,
                       SND_PCM_ACCESS_RW_INTERLEAVED,
                       static_cast<unsigned int>(channels),
                       static_cast<unsigned int>(rate), /*soft_resample=*/1,
                       latencyMs);
  if (err < 0) {
    describe(err);
    error_ = "snd_pcm_set_params failed for \"" + device_ + "\": " + error_;
    closeDevice();
    return err;
  }
  return 0;
}

void AlsaOutput::closeDevice() {
  if (handle_) {
    if (fn_close_) fn_close_(handle_);
    handle_ = nullptr;
  }
}

int AlsaOutput::write(const short* pcm, int frames) {
  if (!handle_ || frames <= 0) return 0;

  long written = fn_writei_(handle_, pcm, static_cast<unsigned long>(frames));
  if (written < 0) {
    int err = static_cast<int>(written);
    // EPIPE is an underrun: the ring ran dry. recover() re-primes it.
    if (err == -EPIPE) {
      if (fn_recover_(handle_, err, /*silent=*/1) < 0) {
        return describe(err);
      }
      written = fn_writei_(handle_, pcm, static_cast<unsigned long>(frames));
      if (written < 0) return describe(static_cast<int>(written));
      lastErrno_ = 0;
    } else {
      return describe(err);
    }
  }
  return static_cast<int>(written);
}

int AlsaOutput::drain() {
  if (!handle_) return 0;
  int err = fn_drain_(handle_);
  if (err < 0) return describe(err);
  return 0;
}

int AlsaOutput::availableFrames() {
  if (!handle_ || !fn_avail_delay_) return 0;
  long avail = 0, delay = 0;
  if (fn_avail_delay_(handle_, &avail, &delay) < 0) return 0;
  return static_cast<int>(avail);
}

}  // namespace ekho