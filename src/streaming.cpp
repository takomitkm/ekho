/***************************************************************************
 * ekho-linux -- streaming session. See streaming.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include "streaming.h"

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "text_normalize.h"

namespace ekho {

long long nowUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<long long>(tv.tv_sec) * 1000000LL + tv.tv_usec;
}

// ---------------------------------------------------------------------------
// PcmRing
// ---------------------------------------------------------------------------

PcmRing::PcmRing(size_t capacityFrames)
    : buf_(new short[capacityFrames]),
      capacity_(capacityFrames),
      readPos_(0),
      writePos_(0),
      count_(0),
      dropped_(0) {
  pthread_mutex_init(&mutex_, nullptr);
}

PcmRing::~PcmRing() {
  pthread_mutex_destroy(&mutex_);
  delete[] buf_;
}

size_t PcmRing::write(const short* pcm, size_t frames) {
  if (frames == 0) return 0;
  pthread_mutex_lock(&mutex_);

  size_t space = capacity_ - count_;
  if (frames > space) {
    // Drop-oldest. A consumer that cannot keep up should miss audio, not
    // accumulate unbounded lag behind live speech.
    size_t need = frames - space;
    if (need > count_) need = count_;
    readPos_ = (readPos_ + need) % capacity_;
    count_ -= need;
    dropped_ += need;
    space = capacity_ - count_;
  }
  size_t n = std::min(frames, space);
  for (size_t i = 0; i < n; i++) {
    buf_[writePos_] = pcm[i];
    writePos_ = (writePos_ + 1) % capacity_;
  }
  count_ += n;
  pthread_mutex_unlock(&mutex_);
  return n;
}

size_t PcmRing::read(short* out, size_t frames) {
  if (frames == 0) return 0;
  pthread_mutex_lock(&mutex_);
  size_t n = std::min(frames, count_);
  for (size_t i = 0; i < n; i++) {
    out[i] = buf_[readPos_];
    readPos_ = (readPos_ + 1) % capacity_;
  }
  count_ -= n;
  pthread_mutex_unlock(&mutex_);
  return n;
}

void PcmRing::clear() {
  pthread_mutex_lock(&mutex_);
  readPos_ = writePos_ = count_ = 0;
  pthread_mutex_unlock(&mutex_);
}

size_t PcmRing::available() const {
  pthread_mutex_lock(&mutex_);
  size_t n = count_;
  pthread_mutex_unlock(&mutex_);
  return n;
}

size_t PcmRing::droppedFrames() const {
  pthread_mutex_lock(&mutex_);
  size_t n = dropped_;
  pthread_mutex_unlock(&mutex_);
  return n;
}

// ---------------------------------------------------------------------------
// StreamingSession
// ---------------------------------------------------------------------------

StreamingSession::StreamingSession(Ekho* engine, size_t ringFrames)
    : engine_(engine),
      ring_(ringFrames),
      segmented_(true),
      normalize_(true),
      ended_(false),
      stopped_(false),
      minTailChars_(6),
      firstAppendUs_(0) {
  pthread_mutex_init(&stateMutex_, nullptr);
  pthread_cond_init(&dataCond_, nullptr);
}

StreamingSession::~StreamingSession() {
  pthread_cond_destroy(&dataCond_);
  pthread_mutex_destroy(&stateMutex_);
}

int StreamingSession::synthTrampoline(short* pcm, int frames, void* arg,
                                      OverlapType type) {
  StreamingSession* self = static_cast<StreamingSession*>(arg);
  return self->ring_.write(pcm, static_cast<size_t>(frames)) == 0 ? 0 : frames;
}

int StreamingSession::synthSegment(const std::string& segment) {
  if (segment.empty()) return 0;

  long long t0 = nowUs();
  engine_->synth4(segment, &StreamingSession::synthTrampoline, this);
  long long t1 = nowUs();

  pthread_mutex_lock(&stateMutex_);
  stats_.genUs += (t1 - t0);
  stats_.segments++;
  stats_.lastChunkUs = t1 - firstAppendUs_;
  if (stats_.firstChunkUs < 0 && ring_.available() > 0) {
    stats_.firstChunkUs = t1 - firstAppendUs_;
  }
  pthread_mutex_unlock(&stateMutex_);

  pthread_cond_broadcast(&dataCond_);
  return 1;
}

int StreamingSession::append(const std::string& text, bool normalize) {
  long long t0 = nowUs();

  pthread_mutex_lock(&stateMutex_);
  if (stopped_ || ended_) {
    pthread_mutex_unlock(&stateMutex_);
    return 0;
  }
  if (firstAppendUs_ == 0) firstAppendUs_ = t0;
  stats_.lastChunkUs = t0 - firstAppendUs_;
  pthread_mutex_unlock(&stateMutex_);

  if (normalize_ && normalize) {
    buffer_ += normalizeText(text, NormalizeOptions());
  } else {
    buffer_ += text;
  }

  int made = 0;

  if (!segmented_) {
    if (!buffer_.empty()) {
      made += synthSegment(buffer_);
      buffer_.clear();
    }
  } else {
    for (;;) {
      CutPoint cut = lastSafeCut(buffer_, 0, buffer_.size());
      if (cut.offset == 0) break;
      // Do not emit a segment so small it is just one or two syllables.
      if (charCount(buffer_, 0, cut.offset) < 2) break;

      std::string segment = buffer_.substr(0, cut.offset);
      buffer_.erase(0, cut.offset);
      made += synthSegment(segment);
    }
  }

  long long t1 = nowUs();
  pthread_mutex_lock(&stateMutex_);
  stats_.appendUs += (t1 - t0);
  pthread_mutex_unlock(&stateMutex_);
  pthread_cond_broadcast(&dataCond_);

  return made;
}

int StreamingSession::flush() {
  pthread_mutex_lock(&stateMutex_);
  if (ended_ || stopped_) {
    pthread_mutex_unlock(&stateMutex_);
    return 0;
  }
  ended_ = true;
  pthread_mutex_unlock(&stateMutex_);

  int made = 0;
  if (!buffer_.empty()) {
    made += synthSegment(buffer_);
    buffer_.clear();
  }
  pthread_cond_broadcast(&dataCond_);
  return made;
}

int StreamingSession::read(short* out, int frames, int timeoutMs) {
  if (frames <= 0) return 0;
  size_t got = ring_.read(out, static_cast<size_t>(frames));
  if (got > 0) return static_cast<int>(got);
  if (timeoutMs == 0) return 0;

  pthread_mutex_lock(&stateMutex_);
  while (!stopped_ && ring_.available() == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeoutMs / 1000;
    ts.tv_nsec += static_cast<long>(timeoutMs % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000L;
    }
    if (pthread_cond_timedwait(&dataCond_, &stateMutex_, &ts) == ETIMEDOUT) break;
  }
  bool done = stopped_ || (ended_ && ring_.available() == 0);
  pthread_mutex_unlock(&stateMutex_);

  if (done && ring_.available() == 0) return 0;
  return static_cast<int>(ring_.read(out, static_cast<size_t>(frames)));
}

void StreamingSession::stop() {
  pthread_mutex_lock(&stateMutex_);
  stopped_ = true;
  ended_ = true;
  buffer_.clear();
  pthread_mutex_unlock(&stateMutex_);
  pthread_cond_broadcast(&dataCond_);
}

bool StreamingSession::finished() const {
  pthread_mutex_lock(&stateMutex_);
  bool f = (ended_ || stopped_) && ring_.available() == 0;
  pthread_mutex_unlock(&stateMutex_);
  return f;
}

StreamStats StreamingSession::stats() const {
  pthread_mutex_lock(&stateMutex_);
  StreamStats s = stats_;
  pthread_mutex_unlock(&stateMutex_);
  s.droppedFrames = ring_.droppedFrames();
  return s;
}

void StreamingSession::resetStats() {
  pthread_mutex_lock(&stateMutex_);
  stats_ = StreamStats();
  firstAppendUs_ = 0;
  pthread_mutex_unlock(&stateMutex_);
}

size_t StreamingSession::backlogChars() const {
  pthread_mutex_lock(&stateMutex_);
  size_t n = buffer_.size();
  pthread_mutex_unlock(&stateMutex_);
  return n;
}

}  // namespace ekho