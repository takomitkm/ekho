// convert-voice: re-encode an ekho .voice pack at another sample rate.
//
// Two uses so far. (1) The shipped pack: the upstream Mandarin/Cantonese packs
// are 44.1 kHz while the engine's default output is 48 kHz, so the engine runs
// a sonic resample on every sample of every syllable. Converting the pack once,
// offline, to 48 kHz makes pack rate == output rate and removes that work.
// (2) The rejected low-RSS experiment: the bundled 44.1 kHz Mandarin pack
// decodes to ~120 MB of cached PCM; the same syllables at 8 kHz decode to
// ~22 MB. (Rejected 2026-10: the 4 kHz lowpass dulls the sibilants.)
//
// Nothing else in the pipeline cares about the rate: the .index header carries
// it, setSpeed()'s tempo constant is duration-invariant, pause buffers and the
// overlap floor rescale from mSfinfo.samplerate, and espeak's 22050 Hz goes
// through the same sonic stream as everything else.
//
// Each chunk of a .voice file is a complete WAV file with its own header (see
// Dict::makeVoiceFile and the note in PhoneticSymbol::getPcm), and the runtime
// opens chunks independently -- so this tool decodes each chunk with
// libsndfile (virtual IO, exactly like the runtime), resamples it, and writes
// a fresh minimal WAV chunk plus a rebuilt .index with new offsets. Record
// order and codes are preserved verbatim; empty records (bytes == 0, used by
// the tone-5 -> tone-1 fallback) stay empty.
//
// Resampling is a windowed-sinc fractional resampler (Blackman window, TAPS
// each side, cutoff at the lower of the two Nyquists): linear interpolation
// would alias the 4-22 kHz band of the source down onto speech, and using the
// *output* Nyquist when going up would let the 22.05-24 kHz images of a 44.1k
// source through instead of removing them. Chunk durations are preserved to
// one output sample, so speaking rate does not move.
//
// Since 2026-10 the shipped Mandarin/Cantonese packs ARE 48 kHz, produced by
// this tool from the upstream 44.1 kHz ones:
//
//   convert-voice pinyin.voice.44k pinyin.index.44k 48000 pinyin.voice pinyin.index
//
// That is what lets the engine run with pack rate == output rate: no sonic
// resample per sample on the target, and no linear-interpolation imaging.
// The rate is carried by the .index header, so this is invisible to the code
// path. Output Nyquist > input Nyquist here, i.e. no bandwidth is lost and
// nothing gets "darker" -- the 22.05 kHz band of the source survives intact.
//
// NOTE on join duration: the engine joins syllables by walking each chunk's
// edges while samples sit below quiet_level (mOverlap, 4096), and at 8 kHz
// the 4 kHz lowpass removes the HF transients that used to cross that
// threshold, so the walks run further and the pack speaks ~11% fast. An
// earlier version of this tool tried to fix that here by re-trimming the
// sub-threshold edges to the source's wall-clock positions. It failed and
// was removed: output duration = sum(chunk frames) - sum(overlap frames),
// and trimming a quiet run of length H shortens both terms by ~H, so the
// net is zero (measured: overlaps matched to -0.11 s, chunk sum shrank
// -4.58 s, duration got slightly worse). The fix belongs in the engine,
// which now scales quiet_level by rate (see EkhoImpl::writeToSonicStream).
//
// usage: convert-voice <in.voice> <in.index> <out.rate> <out.voice> <out.index>
// build: cc -O2 -o convert-voice convert-voice.c -lsndfile -lm
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sndfile.h>

#define TAPS 48 // window half-width in input samples

#define MAX_WORD_SYMS 32

typedef struct {
  int code_count; // 1 for a syllable, > 1 for a word record
  uint16_t codes[MAX_WORD_SYMS];
  uint32_t offset;
  uint32_t bytes;
} Record;

typedef struct {
  const char *data;
  sf_count_t size;
  sf_count_t pos;
} MemSource;

static sf_count_t memGetFilelen(void *user) {
  return ((MemSource *)user)->size;
}
static sf_count_t memSeek(sf_count_t offset, int whence, void *user) {
  MemSource *m = (MemSource *)user;
  sf_count_t np = 0;
  if (whence == SEEK_SET)
    np = offset;
  else if (whence == SEEK_CUR)
    np = m->pos + offset;
  else if (whence == SEEK_END)
    np = m->size + offset;
  if (np < 0 || np > m->size)
    return -1;
  m->pos = np;
  return np;
}
static sf_count_t memRead(void *ptr, sf_count_t count, void *user) {
  MemSource *m = (MemSource *)user;
  sf_count_t avail = m->size - m->pos;
  if (count > avail)
    count = avail;
  memcpy(ptr, m->data + m->pos, (size_t)count);
  m->pos += count;
  return count;
}
static sf_count_t memWrite(const void *ptr, sf_count_t count, void *user) {
  (void)ptr;
  (void)user;
  return count; // read-only
}
static sf_count_t memTell(void *user) { return ((MemSource *)user)->pos; }

static double *decodeChunk(const char *chunk, size_t bytes, int *rate,
                           long *framesOut) {
  MemSource src;
  src.data = chunk;
  src.size = (sf_count_t)bytes;
  src.pos = 0;
  SF_VIRTUAL_IO vio;
  vio.get_filelen = memGetFilelen;
  vio.seek = memSeek;
  vio.read = memRead;
  vio.write = memWrite;
  vio.tell = memTell;

  SF_INFO info;
  memset(&info, 0, sizeof(info));
  SNDFILE *sf = sf_open_virtual(&vio, SFM_READ, &info, &src);
  if (!sf) {
    fprintf(stderr, "chunk at %zu B: sf_open_virtual: %s\n", bytes,
            sf_strerror(NULL));
    return NULL;
  }
  if (info.channels != 1) {
    fprintf(stderr, "chunk: expected mono, got %d channels\n", info.channels);
    sf_close(sf);
    return NULL;
  }
  if (info.frames <= 0) {
    fprintf(stderr, "chunk: no frames\n");
    sf_close(sf);
    return NULL;
  }

  double *pcm = (double *)malloc((size_t)info.frames * sizeof(double));
  if (!pcm) {
    sf_close(sf);
    return NULL;
  }
  sf_count_t got = sf_readf_double(sf, pcm, info.frames);
  sf_close(sf);
  if (got != info.frames) {
    fprintf(stderr, "chunk: read %lld of %lld frames\n", (long long)got,
            (long long)info.frames);
    free(pcm);
    return NULL;
  }

  *rate = info.samplerate;
  *framesOut = (long)info.frames;
  return pcm;
}

static double sinc(double x) {
  if (fabs(x) < 1e-12)
    return 1.0;
  double pix = M_PI * x;
  return sin(pix) / pix;
}

// One tap: Blackman-windowed sinc at offset d input samples, fc cycles per
// input sample. Zero outside the window, exactly as the direct loop skipped.
static double tap(double d, double fc) {
  if (d <= -(double)TAPS || d >= (double)TAPS)
    return 0.0;
  double u = d / (double)TAPS + 1.0; // 0..2
  double w = 0.42 - 0.5 * cos(M_PI * u) + 0.08 * cos(2.0 * M_PI * u);
  return w * sinc(fc * d);
}

static long gcdLong(long a, long b) {
  while (b) {
    long t = a % b;
    a = b;
    b = t;
  }
  return a;
}

// Phases beyond this fall back to evaluating the kernel per sample.
#define MAX_PHASES 4096

// Windowed-sinc fractional resample. t = j * rateIn / rateOut is rational
// (rateIn/g and rateOut/g integers), so its fractional part takes only
// rateOut/g distinct values: tabulate the kernel per phase instead of calling
// sin() and cos() 2*TAPS+1 times per output sample. 125 MB of 44.1 kHz pack
// is ~68 M output samples here, which is minutes that way and seconds this
// way. The kernel is the same function of the same offsets; only how the
// phase is derived (exact integers instead of a rounded double) differs, by
// ~1e-16, which cannot move a PCM16 sample by more than a bit of dither.
static long resample(const double *in, long nIn, int rateIn, double *out,
                     long nOutMax, int rateOut) {
  // Cutoff at the lower of the two Nyquists, in cycles per input sample.
  // Going down (44.1k -> 8k) that is the output Nyquist, which keeps the
  // discarded band from folding back into speech. Going up (44.1k -> 48k)
  // the output Nyquist would sit *above* the source band's 0.5 and let its
  // 22.05-24 kHz images through; the input Nyquist is the ideal
  // reconstruction and leaves the source band completely intact.
  double fc = 0.5;
  if (rateOut < rateIn)
    fc = 0.5 * ((double)rateOut / (double)rateIn);

  long nOut = (long)llround((double)nIn * (double)rateOut / (double)rateIn);
  if (nOut > nOutMax)
    nOut = nOutMax;

  long g = gcdLong(rateIn, rateOut);
  long L = rateOut / g; // distinct fractional positions
  long M = rateIn / g;
  double *tab = NULL;
  if (L <= MAX_PHASES) {
    tab = (double *)malloc((size_t)L * (2 * TAPS + 1) * sizeof(double));
    if (!tab) {
      perror("malloc");
      return 0;
    }
    for (long p = 0; p < L; p++) {
      double frac = (double)p / (double)L;
      for (long m = -TAPS; m <= TAPS; m++)
        tab[p * (2 * TAPS + 1) + (m + TAPS)] = tap(frac + (double)m, fc);
    }
  }

  for (long j = 0; j < nOut; j++) {
    double acc = 0.0;
    if (tab) {
      long long num = (long long)j * M; // position in input samples = num / L
      long i0 = (long)(num / L);
      long p = (long)(num - (long long)i0 * L);
      const double *row = tab + p * (2 * TAPS + 1);
      for (long m = -TAPS; m <= TAPS; m++) {
        long i = i0 - m;
        long idx = i < 0 ? 0 : (i >= nIn ? nIn - 1 : i);
        acc += in[idx] * row[m + TAPS];
      }
    } else {
      double t = (double)j * (double)rateIn / (double)rateOut;
      long i0 = (long)floor(t);
      for (long i = i0 - TAPS; i <= i0 + TAPS; i++) {
        long idx = i < 0 ? 0 : (i >= nIn ? nIn - 1 : i);
        acc += in[idx] * tap(t - (double)i, fc);
      }
    }
    out[j] = acc * fc;
  }

  free(tab);
  return nOut;
}

// --- output helpers -------------------------------------------------------
static void putU16(FILE *f, unsigned v) {
  fputc(v & 0xFF, f);
  fputc((v >> 8) & 0xFF, f);
}
static void putU32(FILE *f, unsigned v) {
  fputc(v & 0xFF, f);
  fputc((v >> 8) & 0xFF, f);
  fputc((v >> 16) & 0xFF, f);
  fputc((v >> 24) & 0xFF, f);
}
static void putU24(FILE *f, unsigned v) {
  fputc(v & 0xFF, f);
  fputc((v >> 8) & 0xFF, f);
  fputc((v >> 16) & 0xFF, f);
}

static void writeWavHeader(FILE *f, unsigned dataBytes, unsigned rate) {
  unsigned byteRate = rate * 2; // mono, 16-bit
  fwrite("RIFF", 1, 4, f);
  putU32(f, 36 + dataBytes);
  fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f);
  putU32(f, 16);
  putU16(f, 1); // PCM
  putU16(f, 1); // mono
  putU32(f, rate);
  putU32(f, byteRate);
  putU16(f, 2);  // block align
  putU16(f, 16); // bits
  fwrite("data", 1, 4, f);
  putU32(f, dataBytes);
}

int main(int argc, char **argv) {
  if (argc != 6) {
    fprintf(stderr,
            "usage: %s <in.voice> <in.index> <out.rate> <out.voice> "
            "<out.index>\n",
            argv[0]);
    return 2;
  }
  const char *inVoicePath = argv[1];
  const char *inIndexPath = argv[2];
  int outRate = atoi(argv[3]);
  const char *outVoicePath = argv[4];
  const char *outIndexPath = argv[5];
  if (outRate <= 0) {
    fprintf(stderr, "bad out.rate %s\n", argv[3]);
    return 2;
  }

  // ---- parse the input index (same layout Dict::loadEkhoVoiceFile reads) ----
  FILE *is = fopen(inIndexPath, "rb");
  if (!is) {
    perror(inIndexPath);
    return 1;
  }
  int b0 = fgetc(is), b1 = fgetc(is), b2 = fgetc(is);
  fgetc(is), fgetc(is); // reserved (symbol count)
  if (b0 == EOF || b1 == EOF || b2 == EOF) {
    fprintf(stderr, "truncated index header\n");
    return 1;
  }
  int inRate = b0 | (b1 << 8);
  int inType = b2;

  Record *recs = NULL;
  size_t nRecs = 0, capRecs = 0;
  int codeCount;
  while ((codeCount = fgetc(is)) != EOF) {
    Record r;
    memset(&r, 0, sizeof(r));
    r.code_count = codeCount;
    if (codeCount <= 0 || codeCount > MAX_WORD_SYMS) {
      fprintf(stderr, "record %zu: implausible code_count %d\n", nRecs,
              codeCount);
      return 1;
    }
    for (int i = 0; i < codeCount; i++) {
      int lo = fgetc(is), hi = fgetc(is);
      if (lo == EOF || hi == EOF) {
        fprintf(stderr, "truncated record %zu\n", nRecs);
        return 1;
      }
      r.codes[i] = (uint16_t)(lo | (hi << 8));
    }
    unsigned o0 = fgetc(is), o1 = fgetc(is), o2 = fgetc(is), o3 = fgetc(is);
    unsigned s0 = fgetc(is), s1 = fgetc(is), s2 = fgetc(is);
    if (o3 == EOF || s2 == EOF) {
      fprintf(stderr, "truncated record %zu offsets\n", nRecs);
      return 1;
    }
    r.offset = o0 | (o1 << 8) | (o2 << 16) | ((uint32_t)o3 << 24);
    r.bytes = s0 | (s1 << 8) | (s2 << 16);
    if (nRecs == capRecs) {
      capRecs = capRecs ? capRecs * 2 : 4096;
      recs = (Record *)realloc(recs, capRecs * sizeof(Record));
      if (!recs) {
        perror("realloc");
        return 1;
      }
    }
    recs[nRecs++] = r;
  }
  fclose(is);

  fprintf(stderr, "index: rate=%d type=%d records=%zu\n", inRate, inType,
          nRecs);
  if (nRecs == 0) {
    fprintf(stderr, "no records; nothing to do\n");
    return 1;
  }

  // ---- read the whole input blob ----
  FILE *iv = fopen(inVoicePath, "rb");
  if (!iv) {
    perror(inVoicePath);
    return 1;
  }
  fseek(iv, 0, SEEK_END);
  long voiceSize = ftell(iv);
  fseek(iv, 0, SEEK_SET);
  char *voice = (char *)malloc((size_t)voiceSize);
  if (!voice || fread(voice, 1, (size_t)voiceSize, iv) != (size_t)voiceSize) {
    fprintf(stderr, "short read of %s\n", inVoicePath);
    return 1;
  }
  fclose(iv);

  // ---- convert every record ----
  FILE *ov = fopen(outVoicePath, "wb");
  FILE *oi = fopen(outIndexPath, "wb");
  if (!ov || !oi) {
    perror("output");
    return 1;
  }
  putU16(oi, (unsigned)outRate);
  fputc(1, oi);    // type: wav
  fputc(0, oi);    // reserved
  fputc(0, oi);

  uint32_t outOffset = 0;
  long worstDrift = 0;
  double totalDrift = 0.0;
  long nConverted = 0, nEmpty = 0;
  double *outBuf = NULL;
  long outCap = 0;

  for (size_t i = 0; i < nRecs; i++) {
    Record *r = &recs[i];
    fputc(r->code_count, oi);
    for (int c = 0; c < r->code_count; c++) {
      putU16(oi, r->codes[c]);
    }
    putU32(oi, outOffset);

    if (r->bytes == 0) {
      // tone-5 fallback record: keep it empty
      putU24(oi, 0);
      nEmpty++;
      continue;
    }
    if ((long)r->offset + (long)r->bytes > voiceSize) {
      fprintf(stderr, "record %zu: chunk [%u,+%u) past end of %ld B blob\n", i,
              r->offset, r->bytes, voiceSize);
      return 1;
    }

    int chunkRate = 0;
    long nIn = 0;
    double *pcmIn =
        decodeChunk(voice + r->offset, r->bytes, &chunkRate, &nIn);
    if (!pcmIn) {
      fprintf(stderr, "record %zu (code %u): decode failed\n", i, r->codes[0]);
      return 1;
    }
    if (chunkRate != inRate) {
      fprintf(stderr, "record %zu: chunk rate %d != index rate %d\n", i,
              chunkRate, inRate);
      return 1;
    }

    long want = (long)llround((double)nIn * (double)outRate /
                              (double)chunkRate);
    if (want + 4096 > outCap) {
      outCap = want + 4096;
      outBuf = (double *)realloc(outBuf, (size_t)outCap * sizeof(double));
      if (!outBuf) {
        perror("realloc");
        return 1;
      }
    }
    long nOut = resample(pcmIn, nIn, chunkRate, outBuf, outCap, outRate);

    // duration check: out frames / outRate vs in frames / inRate, in output
    // samples. One sample of drift per chunk is inaudible; anything larger
    // here would move the speaking rate of the finished pack.
    long drift = nOut - want;
    if (labs(drift) > labs(worstDrift))
      worstDrift = drift;
    totalDrift += (double)drift / outRate;

    free(pcmIn);

    unsigned dataBytes = (unsigned)(nOut * 2);
    writeWavHeader(ov, dataBytes, (unsigned)outRate);
    for (long s = 0; s < nOut; s++) {
      // sf_readf_double is normalized to [-1, 1]; scale to PCM16 full range.
      double v = outBuf[s] * 32768.0;
      if (v > 32767.0)
        v = 32767.0;
      else if (v < -32768.0)
        v = -32768.0;
      int16_t pcm16 = (int16_t)v;
      fwrite(&pcm16, 2, 1, ov);
    }
    // The record length is the WHOLE chunk, header included: the runtime
    // freads `bytes` bytes from `offset` and hands exactly those to
    // libsndfile, so an entry that counts only the PCM loses the last 22
    // frames of every syllable (measured: every chunk came back 22 frames
    // short, which is 44 B of header, and the pack ran 0.1% fast). The
    // upstream .voice files count the header; match them.
    putU24(oi, 44 + dataBytes);
    outOffset += 44 + dataBytes;
    nConverted++;
  }

  fclose(ov);
  fclose(oi);
  free(outBuf);
  free(voice);
  free(recs);

  fprintf(stderr,
          "converted %ld chunks (%ld empty): %ld B -> %u B, worst per-chunk "
          "drift %ld sample(s), total drift %.3f ms\n",
          nConverted, nEmpty, voiceSize, outOffset, worstDrift,
          totalDrift * 1000.0);
  return 0;
}
