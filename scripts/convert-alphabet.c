// convert-alphabet: resample ekho-data/alphabet/*.wav to a new rate.
//
// The letter samples are 16 kHz upstream while the voice pack is 44.1 kHz
// (upstream) or 48 kHz (this fork), and the engine splices them into a stream
// that is running at the pack's rate -- so a lone Latin letter used to play
// 2.76x or 3x too fast and too high. The engine now resamples them at load time
// (src/resample.h), which fixes every pack rate including the 16 kHz Cantonese
// one; this tool additionally ships the files in the rate the default pack
// uses, so the data is self-describing and the loader has nothing to do.
//
// The two big a-z_us.wav / a-z_uk.wav files are whole-alphabet recordings that
// no code path reads (Dict::addSpecialSymbols only takes a.wav..z.wav), so they
// are left alone: converting them would add ~3.5 MB of dead weight to the kit.
//
// The originals are kept next to the new files, suffixed with their rate
// (a.wav.16k), the same way convert-voice leaves pinyin.voice.44k. Re-running
// on already-converted files does nothing.
//
// Kernel: Blackman-windowed sinc, cutoff at the lower of the two Nyquists, 48
// taps each side. Same kernel and same reasoning as scripts/convert-voice.c
// and src/resample.cpp.
//
// usage: convert-alphabet <alphabet-dir> <out.rate>
// build: cc -O2 -o convert-alphabet convert-alphabet.c -lsndfile -lm
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sndfile.h>

#define TAPS 48

static double sinc(double x) {
  if (fabs(x) < 1e-12)
    return 1.0;
  double pix = M_PI * x;
  return sin(pix) / pix;
}

static double tap(double d, double fc) {
  if (d <= -(double)TAPS || d >= (double)TAPS)
    return 0.0;
  double u = d / (double)TAPS + 1.0;
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

static long resample(const double *in, long nIn, int rateIn, double *out,
                     long nOutMax, int rateOut) {
  double fc = 0.5;
  if (rateOut < rateIn)
    fc = 0.5 * ((double)rateOut / (double)rateIn);

  long nOut = (long)llround((double)nIn * (double)rateOut / (double)rateIn);
  if (nOut > nOutMax)
    nOut = nOutMax;

  long g = gcdLong(rateIn, rateOut);
  long L = rateOut / g;
  long M = rateIn / g;
  int rowLen = 2 * TAPS + 1;
  double *tab = (double *)malloc((size_t)L * rowLen * sizeof(double));
  if (!tab) {
    perror("malloc");
    return 0;
  }
  for (long p = 0; p < L; p++) {
    double frac = (double)p / (double)L;
    for (int m = -TAPS; m <= TAPS; m++)
      tab[p * rowLen + (m + TAPS)] = tap(frac + (double)m, fc);
  }

  for (long j = 0; j < nOut; j++) {
    long long num = (long long)j * M;
    long i0 = (long)(num / L);
    long p = (long)(num - (long long)i0 * L);
    const double *row = tab + p * rowLen;
    double acc = 0.0;
    for (int m = -TAPS; m <= TAPS; m++) {
      long i = i0 - m;
      long idx = i < 0 ? 0 : (i >= nIn ? nIn - 1 : i);
      acc += in[idx] * row[m + TAPS];
    }
    out[j] = acc * fc;
  }

  free(tab);
  return nOut;
}

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

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <alphabet-dir> <out.rate>\n", argv[0]);
    return 2;
  }
  const char *dir = argv[1];
  int outRate = atoi(argv[2]);
  if (outRate <= 0) {
    fprintf(stderr, "bad out.rate %s\n", argv[2]);
    return 2;
  }

  int nDone = 0, nSkip = 0, nFail = 0;
  for (char c = 'a'; c <= 'z'; c++) {
    char name[32], path[4096], tmp[4096], bak[4096];
    snprintf(name, sizeof(name), "%c.wav", c);
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    struct stat st;
    if (stat(path, &st) != 0) {
      fprintf(stderr, "  %s: missing\n", name);
      nFail++;
      continue;
    }

    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE *sf = sf_open(path, SFM_READ, &info);
    if (!sf) {
      fprintf(stderr, "  %s: %s\n", name, sf_strerror(NULL));
      nFail++;
      continue;
    }
    if (info.channels != 1) {
      fprintf(stderr, "  %s: %d channels, expected mono\n", name,
              info.channels);
      sf_close(sf);
      nFail++;
      continue;
    }
    if (info.samplerate == outRate) {
      printf("  %-8s already %d Hz, skipped\n", name, outRate);
      sf_close(sf);
      nSkip++;
      continue;
    }

    double *pcm = (double *)malloc((size_t)info.frames * sizeof(double));
    if (!pcm) {
      perror("malloc");
      sf_close(sf);
      return 1;
    }
    sf_count_t got = sf_readf_double(sf, pcm, info.frames);
    int inRate = info.samplerate;
    sf_close(sf);
    if (got != info.frames) {
      fprintf(stderr, "  %s: read %lld of %lld frames\n", name,
              (long long)got, (long long)info.frames);
      free(pcm);
      nFail++;
      continue;
    }

    long want = (long)llround((double)info.frames * (double)outRate /
                              (double)inRate);
    double *out = (double *)malloc((size_t)(want + 64) * sizeof(double));
    if (!out) {
      perror("malloc");
      free(pcm);
      return 1;
    }
    long nOut = resample(pcm, (long)info.frames, inRate, out, want + 64, outRate);
    free(pcm);
    if (nOut <= 0) {
      free(out);
      nFail++;
      continue;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.%dk", path, inRate / 1000);
    FILE *o = fopen(tmp, "wb");
    if (!o) {
      perror(tmp);
      free(out);
      return 1;
    }
    unsigned dataBytes = (unsigned)(nOut * 2);
    unsigned byteRate = (unsigned)outRate * 2;
    fwrite("RIFF", 1, 4, o);
    putU32(o, 36 + dataBytes);
    fwrite("WAVE", 1, 4, o);
    fwrite("fmt ", 1, 4, o);
    putU32(o, 16);
    putU16(o, 1);           // PCM
    putU16(o, 1);           // mono
    putU32(o, (unsigned)outRate);
    putU32(o, byteRate);
    putU16(o, 2);           // block align
    putU16(o, 16);          // bits
    fwrite("data", 1, 4, o);
    putU32(o, dataBytes);
    for (long s = 0; s < nOut; s++) {
      double v = out[s] * 32768.0;
      if (v > 32767.0)
        v = 32767.0;
      else if (v < -32768.0)
        v = -32768.0;
      int16_t pcm16 = (int16_t)v;
      fwrite(&pcm16, 2, 1, o);
    }
    fclose(o);
    free(out);

    // Keep the original: moving it aside first means a crash between the two
    // renames leaves the data recoverable, and re-running is idempotent.
    if (access(bak, F_OK) == 0) {
      unlink(tmp);  // a stale backup: this file was converted before
      printf("  %-8s backup %s exists, left alone\n", name, bak);
      nSkip++;
      continue;
    }
    if (rename(path, bak) != 0) {
      perror("rename to backup");
      unlink(tmp);
      nFail++;
      continue;
    }
    if (rename(tmp, path) != 0) {
      perror("rename into place");
      rename(bak, path);
      nFail++;
      continue;
    }

    double durIn = (double)info.frames / inRate;
    double durOut = (double)nOut / outRate;
    printf("  %-8s %5d -> %5d Hz  %7.0f + %7.0f ms\n", name, inRate, outRate,
           durIn * 1000.0, durOut * 1000.0);
    nDone++;
  }

  printf("converted %d, skipped %d, failed %d (out.rate %d)\n", nDone, nSkip,
         nFail, outRate);
  return nFail ? 1 : 0;
}
