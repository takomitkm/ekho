// ekho-linux: windowed-sinc resampler for the alphabet samples.
//
// ekho-data/alphabet/<letter>.wav are recorded at 16 kHz upstream, while the
// voice pack they get spliced into is 44.1 kHz (upstream) or 48 kHz (this
// fork). PhoneticSymbol::readSndfile() used to copy the frames verbatim, so a
// lone Latin letter -- "A 股", an initial, a letter spelled out on its own --
// played 2.76x too fast and too high on the 44.1 kHz pack and 3x on the 48 kHz
// one. Upstream knew: addSpecialSymbols() carries a "@fixme: disable alphabet
// because the samplerate may not correct".
//
// Converting the files once only fixes the one pack rate they were converted
// to, and the Cantonese pack is still 16 kHz (and the archived Mandarin pack
// 44.1 kHz), so the conversion belongs at load time: readSndfile() asks this
// for the current pack rate and keeps the result.
//
// Kernel: Blackman-windowed sinc, cutoff at the lower of the two Nyquists, 48
// taps each side -- the same kernel (and the same reasoning) as
// scripts/convert-voice.c, which resamples the pack itself. Linear
// interpolation would alias the 4-8 kHz band of a 16 kHz source down onto
// speech; using the output Nyquist when going up would pass the source's
// 8-24 kHz images through. Output length is preserved to one sample, so a
// letter keeps its duration.

#ifndef EKHO_RESAMPLE_H
#define EKHO_RESAMPLE_H

namespace ekho {

// Mono, 16-bit in, 16-bit out. Returns a new[] array of *outFrames samples
// (the caller owns it), or NULL when there is nothing to do (rates equal,
// empty input, bad rates) or on allocation failure. A no-op request is a
// normal return value, not an error: callers check for NULL and keep the
// original buffer.
short *resampleMono16(const short *in, int frames, int rateIn, int rateOut,
                      int *outFrames);

}  // namespace ekho

#endif
