/***************************************************************************
 * ekho-linux -- text normalisation and segmentation helpers.
 *
 * Two jobs, both driven by what the field benchmarks on this class of engine
 * kept hitting:
 *
 *  1. Normalisation.  Digits, units and symbols reach the front end as ASCII
 *     and get read letter by letter or as garbage.  "12.5V" has to become
 *     "十二点五伏" and "93.7%" has to become "百分之九十三点七" before any
 *     of it is speakable Chinese.
 *
 *  2. Segmentation.  A streaming front end has to decide when a growing text
 *     buffer can be cut.  Cutting on every punctuation mark would let the
 *     overlap logic insert a clause pause after every comma; never cutting
 *     means the caller waits for the whole paragraph before any audio exists.
 *     lastSafeCut() is the policy: cut at a clause boundary, but not inside a
 *     number, a Latin word, or a bracket pair.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#ifndef EKHO_TEXT_NORMALIZE_H
#define EKHO_TEXT_NORMALIZE_H

#include <cstddef>
#include <string>

namespace ekho {

// Normalisation policy, so callers can opt out of individual rewrites.
struct NormalizeOptions {
  bool numbers = true;       // ASCII digits -> Chinese numerals
  bool units = true;         // "V" after a number -> "伏"
  bool percent = true;       // "93.7%" -> "百分之九十三点七"
  bool collapseSpace = true;
};

// Rewrites raw text into something the pinyin front end can pronounce.
std::string normalizeText(const std::string& in,
                          const NormalizeOptions& opt = NormalizeOptions());

// Where it is safe to cut a partially received buffer.
struct CutPoint {
  size_t offset = 0;     // byte offset of the cut; 0 means "no safe cut yet"
  size_t tailBytes = 0;  // bytes left behind after the cut
};

// Finds the last clause boundary at or before (size - minKeepBytes).
//
// A boundary is a sentence terminator or a comma-like separator that is not
// nested inside brackets.  Numbers are skipped wholesale so a cut can never
// land inside "12.5V".  Returns offset == 0 when there is no usable boundary.
//
// forceAtMax: when the buffer is longer than maxCutBytes and still has no
// boundary, cut at maxCutBytes anyway (backed off to the nearest safe spot
// inside the last 25% of that window).  A streaming front end that will not cut
// produces one enormous burst of audio, which overflows any bounded ring and
// makes playback stutter; capping the segment is what keeps the consumer ahead
// of the producer.
CutPoint lastSafeCut(const std::string& buf, size_t minKeepBytes = 0,
                     size_t maxCutBytes = static_cast<size_t>(-1),
                     bool forceAtMax = false);

// True when `text` ends on a boundary strong enough to flush without waiting
// for more input.
bool isFlushable(const std::string& text, size_t minTailChars = 6);

// Character (not byte) count of [from, to).
size_t charCount(const std::string& s, size_t from, size_t to);

}  // namespace ekho

#endif  // EKHO_TEXT_NORMALIZE_H