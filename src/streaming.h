/***************************************************************************
 * ekho-linux -- streaming session.
 *
 * Upstream ekho has exactly two shapes of use: blockSpeak() (queue the whole
 * string, sleep until the queue drains) and speak() (same, but asynchronous).
 * Neither can be driven by text that arrives a token at a time, which is how
 * an LLM or a speech recogniser actually feeds a TTS engine, and neither
 * exposes when the first playable audio existed -- only when the last sample
 * did.
 *
 * StreamingSession sits on top of Ekho::synth4(), which already hands finished
 * PCM to a callback, and adds three things:
 *
 *   * incremental append() -- text is buffered, and only the prefix that ends
 *     on a safe clause boundary is synthesised, so audio starts while the
 *     caller is still feeding and syllables are never cut in half
 *   * a bounded PCM ring with a pull-side reader, so the producer cannot grow
 *     without limit if the consumer stalls
 *   * timing instrumentation matching the board benchmark's definition of
 *     first_ms: time from append() to the first PCM sample a reader can take
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#ifndef EKHO_STREAMING_H
#define EKHO_STREAMING_H

#include <pthread.h>

#include <cstddef>
#include <string>

#include "ekho.h"

namespace ekho {

long long nowUs();

// Single producer / single consumer ring of 16-bit PCM frames.
//
// Overflow policy is drop-oldest in write(), for callers that would rather
// lose stale audio than accrue latency. The streaming session does not use
// that path: silent sample loss in synthesised speech is an audible artefact,
// so it uses writeBlocking() and lets the generator run slower than real time
// instead. Backpressure, not dropping, is the right failure mode for TTS.
class PcmRing {
 public:
  explicit PcmRing(size_t capacityFrames);
  ~PcmRing();

  // Copies as much as fits, discarding the oldest frames on overflow.
  size_t write(const short* pcm, size_t frames);

  // Blocks until every frame is stored, so nothing is ever dropped. Returns
  // the number of frames stored, or 0 if the ring was aborted while waiting.
  //
  // Note this hands back pressure to the producer: a synthesised clause whose
  // audio exceeds the ring makes synth4() thread block instead of overflowing.
  size_t writeBlocking(const short* pcm, size_t frames);

  size_t read(short* out, size_t frames);
  void clear();

  // Releases every blocked writer and makes later writeBlocking() calls return
  // immediately. Called when the session is stopped, so a producer parked on a
  // full ring cannot outlive the consumer.
  void abort();

  size_t available() const;
  size_t capacityFrames() const { return capacity_; }
  size_t droppedFrames() const;

 private:
  mutable pthread_mutex_t mutex_;
  pthread_cond_t space_;
  bool aborted_;
  short* buf_;
  size_t capacity_;
  size_t readPos_;
  size_t writePos_;
  size_t count_;
  size_t dropped_;
};

// Statistics matching the benchmark harness on the board.
struct StreamStats {
  long long firstChunkUs = -1;  // append() -> first readable PCM sample
  long long lastChunkUs = -1;   // append()/flush() -> most recent PCM
  long long genUs = 0;          // total time inside synth4()
  long long appendUs = 0;       // total time servicing append()
  size_t segments = 0;          // safe segments synthesised
  size_t framesOut = 0;         // PCM frames handed to the ring
  size_t droppedFrames = 0;     // ring overflow
};

class StreamingSession {
 public:
  // `engine` must outlive the session. `ringFrames` caps buffered audio;
  // one second at 48 kHz is 48000.
  StreamingSession(Ekho* engine, size_t ringFrames = 48000);
  ~StreamingSession();

  // Feed a chunk of text. Synthesises every complete segment that can be cut
  // from the accumulated buffer. Returns the number of segments synthesised.
  int append(const std::string& text, bool normalize = true);

  // Declares the text stream finished and synthesises the remaining tail.
  int flush();

  // Drain PCM. timeoutMs < 0 waits for data or for the session to end.
  int read(short* out, int frames, int timeoutMs = 0);

  void stop();
  bool finished() const;

  StreamStats stats() const;
  void resetStats();

  // Text still buffered but not yet synthesised.
  size_t backlogChars() const;

  // --- knobs ---------------------------------------------------------
  // false: synthesise the whole buffer every time instead of waiting for a
  // boundary. Lower latency, worse prosody, more rework.
  void setSegmented(bool on) { segmented_ = on; }
  void setMinTailChars(size_t n) { minTailChars_ = n; }
  void setNormalize(bool on) { normalize_ = on; }

  // Hard cap on how much text one append() may synthesise, in characters.
  // 0 disables the cap. synth4() is synchronous: it pushes a whole segment's
  // audio into the ring in one burst, so an uncapped clause is hundreds of
  // thousands of frames and overflows any bounded ring.
  //
  // This cannot be a constant. Mandarin runs about 0.2 s per character, so a
  // segment must stay well under the ring's duration:
  //     maxSegmentChars ~= (ringFrames / sampleRate) / 0.2 * safety
  // The CLI does that arithmetic; see deriveMaxSegmentChars().
  void setMaxSegmentChars(size_t n) { maxSegmentChars_ = n; }
  size_t maxSegmentChars() const { return maxSegmentChars_; }

  // Characters per segment that keep one burst inside a ring of ringFrames at
  // the given sample rate. Mandarin is ~0.2 s/char; the 0.6 factor leaves room
  // for the clause pause the overlap logic inserts at the cut.
  static size_t deriveMaxSegmentChars(size_t ringFrames, int sampleRate,
                                      double secondsPerChar = 0.2,
                                      double safety = 0.6);

 private:
  static int synthTrampoline(short* pcm, int frames, void* arg, OverlapType type);
  int synthSegment(const std::string& segment);

  Ekho* engine_;
  PcmRing ring_;
  mutable pthread_mutex_t stateMutex_;
  pthread_cond_t dataCond_;

  std::string buffer_;
  // Bytes of an incomplete UTF-8 sequence left over at the end of buffer_.
  // A caller chunking at byte boundaries will split a CJK character across two
  // append() calls; feeding the half to the front end is what produced
  // "translatePunctuations: Invalid UTF8 encoding" and an occasional abort.
  std::string pendingBytes_;
  // Declaration order is initialisation order; keep the constructor
  // initialiser list matching this or -Wreorder fires.
  bool segmented_;
  bool normalize_;
  bool ended_;
  bool stopped_;
  size_t minTailChars_;
  size_t maxSegmentChars_;

  long long firstAppendUs_;
  StreamStats stats_;
};

}  // namespace ekho

#endif  // EKHO_STREAMING_H