// See resample.h for why this exists and what the kernel is.

#include "resample.h"

#include <math.h>
#include <stdlib.h>

namespace ekho {

namespace {

const int kTaps = 48;  // window half-width, in input samples
const int kMaxPhases = 4096;  // beyond this, evaluate the kernel per sample

double sinc(double x) {
  if (fabs(x) < 1e-12)
    return 1.0;
  double pix = M_PI * x;
  return sin(pix) / pix;
}

// One tap: Blackman-windowed sinc at offset d input samples, fc cycles per
// input sample. Zero outside the window.
double tap(double d, double fc) {
  if (d <= -(double)kTaps || d >= (double)kTaps)
    return 0.0;
  double u = d / (double)kTaps + 1.0;  // 0..2
  double w = 0.42 - 0.5 * cos(M_PI * u) + 0.08 * cos(2.0 * M_PI * u);
  return w * sinc(fc * d);
}

long gcdLong(long a, long b) {
  while (b) {
    long t = a % b;
    a = b;
    b = t;
  }
  return a;
}

}  // namespace

short *resampleMono16(const short *in, int frames, int rateIn, int rateOut,
                      int *outFrames) {
  if (!in || frames <= 0 || rateIn <= 0 || rateOut <= 0 ||
      rateIn == rateOut || !outFrames)
    return 0;

  // Cutoff at the lower of the two Nyquists, in cycles per input sample.
  double fc = 0.5;
  if (rateOut < rateIn)
    fc = 0.5 * ((double)rateOut / (double)rateIn);

  const long nIn = frames;
  const long nOut = (long)llround((double)nIn * (double)rateOut /
                                  (double)rateIn);
  if (nOut <= 0)
    return 0;

  short *out = new short[(size_t)nOut];
  if (!out)
    return 0;

  // t = j * rateIn / rateOut is rational, so its fractional part takes only
  // rateOut/gcd distinct values: tabulate the kernel per phase instead of
  // calling sin()/cos() 2*kTaps+1 times per output sample. 48000 -> 44100 has
  // 147 phases, 48000 -> 16000 has 3.
  const long g = gcdLong(rateIn, rateOut);
  const long L = rateOut / g;
  const long M = rateIn / g;
  const int rowLen = 2 * kTaps + 1;
  double *tab = 0;
  if (L <= kMaxPhases) {
    tab = (double *)malloc((size_t)L * rowLen * sizeof(double));
    if (!tab) {
      delete[] out;
      return 0;
    }
    for (long p = 0; p < L; p++) {
      double frac = (double)p / (double)L;
      for (int m = -kTaps; m <= kTaps; m++)
        tab[p * rowLen + (m + kTaps)] = tap(frac + (double)m, fc);
    }
  }

  for (long j = 0; j < nOut; j++) {
    double acc = 0.0;
    if (tab) {
      long long num = (long long)j * M;  // input position = num / L
      long i0 = (long)(num / L);
      long p = (long)(num - (long long)i0 * L);
      const double *row = tab + p * rowLen;
      for (int m = -kTaps; m <= kTaps; m++) {
        long i = i0 - m;
        long idx = i < 0 ? 0 : (i >= nIn ? nIn - 1 : i);
        acc += (double)in[idx] * row[m + kTaps];
      }
    } else {
      double t = (double)j * (double)rateIn / (double)rateOut;
      long i0 = (long)floor(t);
      for (long i = i0 - kTaps; i <= i0 + kTaps; i++) {
        long idx = i < 0 ? 0 : (i >= nIn ? nIn - 1 : i);
        acc += (double)in[idx] * tap(t - (double)i, fc);
      }
    }
    acc *= fc;  // the windowed sinc's DC gain is 1/fc
    if (acc > 32767.0)
      acc = 32767.0;
    else if (acc < -32768.0)
      acc = -32768.0;
    out[j] = (short)lround(acc);
  }

  free(tab);
  *outFrames = (int)nOut;
  return out;
}

}  // namespace ekho
