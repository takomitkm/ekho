/***************************************************************************
 * ekho-linux -- ALSA output backend.
 *
 * Why dlopen instead of -lasound:
 *
 *   The target board (FMSH, aarch64, Ubuntu 20.04) has libasound.so.2 at
 *   runtime but no libasound2-dev, so it cannot satisfy a link-time -lasound.
 *   Hardcoding the enum values by hand instead would mean trusting numbers
 *   copied out of a header.
 *
 *   So: compile against the vendored alsa-lib headers in third_party/alsa
 *   (constants are checked by the compiler), and resolve the entry points we
 *   actually use at runtime. The resulting binary needs nothing at link time
 *   and binds to whatever libasound.so.2 is present.
 *
 *   Only functions with scalar signatures are used, so no opaque
 *   snd_pcm_hw_params struct has to be reconstructed by hand.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#ifndef EKHO_ALSA_BACKEND_H
#define EKHO_ALSA_BACKEND_H

#include <string>

namespace ekho {

class AlsaOutput {
 public:
  AlsaOutput();
  ~AlsaOutput();

  // True when libasound.so.2 could be loaded and exposes every required
  // symbol. Cheap; safe to call repeatedly.
  static bool available();

  // `device` is an ALSA device string: "default", "hw:0,0", "plughw:1,0".
  // Opens for playback and configures 16-bit signed interleaved at `rate`.
  // Returns 0 on success, negative on failure with lastError() set.
  int openDevice(const std::string& device, int rate, int channels,
                 unsigned int latencyMs);

  void closeDevice();

  // Blocking write. Returns frames accepted, or negative on a fatal error.
  // A recoverable underrun is handled internally.
  int write(const short* pcm, int frames);

  // Block until the hardware has played everything queued.
  int drain();

  // Frames the device will accept right now; 0 when unknown.
  int availableFrames();

  const std::string& lastError() const { return error_; }
  bool isOpen() const { return handle_ != nullptr; }
  const std::string& device() const { return device_; }
  int lastErrno() const { return lastErrno_; }

 private:
  bool bind();
  int describe(int err);

  void* lib_;
  void* handle_;

  int (*fn_open_)(void**, const char*, int, int);
  int (*fn_close_)(void*);
  int (*fn_set_params_)(void*, int, int, unsigned int, unsigned int, int,
                        unsigned int);
  long (*fn_writei_)(void*, const void*, unsigned long);
  int (*fn_drain_)(void*);
  int (*fn_prepare_)(void*);
  int (*fn_recover_)(void*, int, int);
  int (*fn_avail_delay_)(void*, long*, long*);
  const char* (*fn_strerror_)(int);
  const char* (*fn_name_)(void*);

  std::string device_;
  std::string error_;
  int rate_;
  int channels_;
  int lastErrno_;
};

}  // namespace ekho

#endif  // EKHO_ALSA_BACKEND_H