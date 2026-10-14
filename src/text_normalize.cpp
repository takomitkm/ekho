/***************************************************************************
 * ekho-linux -- text normalisation and segmentation. See text_normalize.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include "text_normalize.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ekho {

namespace {

inline bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// Sentence terminators: always a safe cut and strong enough to flush.
const char* kSentenceEnd = "\xE3\x80\x82"          // 。
                          "\xEF\xBC\x81"          // ！
                          "\xEF\xBC\x9F"          // ？
                          "!" "?" "\xE2\x80\xA6"; // ! ? …

// Clause separators: a legal cut, but not enough on their own to flush.
const char* kClauseSep = "\xEF\xBC\x8C"     // ，
                        "\xE3\x80\x81"     // 、
                        "\xEF\xBC\x9B"     // ；
                        ";" "\xEF\xBC\x9A"; // ; ：

const char* kOpenBracket = "([{（【《";
const char* kCloseBracket = ")]}）】》";

inline size_t utf8Len(unsigned char c) {
  return (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
}

bool matches(const char* set, const std::string& s, size_t at, size_t len) {
  for (size_t i = 0; i < len; i++) {
    for (const char* p = set; *p; p++) {
      if (s.compare(at + i, 1, std::string(1, *p)) == 0) return true;
    }
  }
  return false;
}

// CJK numeral glyphs, kept as explicit UTF-8 escapes so the file stays pure
// ASCII and no compiler input-charset assumption is needed.
const char* const kDigit[10] = {
    "\xE9\x9B\xB6",  // 零
    "\xE4\xB8\x80",  // 一
    "\xE4\xBA\x8C",  // 二
    "\xE4\xB8\x89",  // 三
    "\xE5\x9B\x9B",  // 四
    "\xE4\xBA\x94",  // 五
    "\xE5\x85\xAD",  // 六
    "\xE4\xB8\x83",  // 七
    "\xE5\x85\xAB",  // 八
    "\xE4\xB9\x9D",  // 九
};

const char* const kPlace[9] = {
    "",  "\xE5\x8D\x83",  // 千
    "\xE7\x99\xBE",      // 百
    "\xE5\x8D\x81",      // 十
    "", "", "", "", "",
};

// 1234 -> 一千二百三十四
void appendPlaceValues(std::string& out, const std::string& digits) {
  size_t n = digits.size();
  if (n == 0) return;
  if (n > 9) {
    // Above 999,999,999 just read the digits out; place values do not apply.
    for (char c : digits) out += kDigit[c - '0'];
    return;
  }
  bool pendingZero = false;
  for (size_t i = 0; i < n; i++) {
    int d = digits[i] - '0';
    size_t pos = n - 1 - i;  // 0 = ones place
    if (d == 0) {
      pendingZero = true;
      continue;
    }
    if (pendingZero) {
      out += kDigit[0];
      pendingZero = false;
    }
    // "一十" is read 十二 / 十五, but 一百一十 keeps the leading 一.
    if (!(pos == 1 && d == 1 && n == 2)) out += kDigit[d];
    out += kPlace[pos];
  }
  if (pendingZero) out += kDigit[0];  // 1005 -> 一千零五
}

struct UnitEntry {
  const char* ascii;
  const char* spoken;
};

// Units that follow a bare number. Longest ASCII keys must come first so that
// "kHz" is not matched as "Hz".
const UnitEntry kUnitsTable[] = {
    {"mA",   "\xE6\xAF\xAB\xE5\xAE\x89"},              // 毫安
    {"mV",   "\xE6\xAF\xAB\xE4\xBC\x8F"},              // 毫伏
    {"kV",   "\xE5\x8D\x83\xE4\xBC\x8F"},              // 千伏
    {"GHz",  "\xE5\x85\x8B\xE8\xB5\xB4"},              // 吉赫
    {"MHz",  "\xE5\x85\x8B\xE8\xB5\xB4"},              // 兆赫
    {"kHz",  "\xE5\x8D\x83\xE8\xB5\xB4"},              // 千赫
    {"GB",   "\xE5\x85\x8D\xE5\xAD\x97\xE8\x8A\x82"},  // 吉字节
    {"KB",   "\xE5\x8D\x83\xE5\xAD\x97\xE8\x8A\x82"},  // 千字节
    {"MB",   "\xE5\x85\x8B\xE5\xAD\x97\xE8\x8A\x82"},  // 兆字节
    {"ms",   "\xE6\xAF\xAB\xE7\xA7\x92"},              // 毫秒
    {"Hz",   "\xE8\xB5\xA4\xE5\x85\xB3"},              // 赫兹
    {"km",   "\xE5\x85\xAC\xE9\x87\x8C"},              // 公里
    {"cm",   "\xE5\x8E\x98\xE7\xB1\xB3"},              // 厘米
    {"mm",   "\xE6\xAF\xAB\xE7\xB1\xB3"},              // 毫米
    {"kg",   "\xE5\x85\xAC\xE6\x96\xA4"},              // 公斤
    {"mg",   "\xE6\xAF\xAB\xE5\x85\x8B"},              // 毫克
    {"dB",   "\xE5\x88\x86\xE8\xB4\xA4"},              // 分贝
    {"%",    "\xE7\x99\xBE\xE5\x88\x86\xE4\xB9\x8B"},  // 百分之
    {"V",    "\xE4\xBC\x8F"},                          // 伏
    {"A",    "\xE5\xAE\x89"},                          // 安
    {"W",    "\xE7\x93\xA6"},                          // 瓦
    {"C",    "\xE6\xB0\x8F\xE5\xBA\xA6"},              // 摄氏度
    {"s",    "\xE7\xA7\x92"},                          // 秒
    {"g",    "\xE5\x85\x8B"},                          // 克
    {"m",    "\xE7\xB1\xB3"},                          // 米
    {"L",    "\xE5\x8D\x87"},                          // 升
};

size_t unitLength(const std::string& s, size_t at) {
  for (const UnitEntry& e : kUnitsTable) {
    size_t n = std::strlen(e.ascii);
    if (s.compare(at, n, e.ascii) == 0) return n;
  }
  return 0;
}

std::string lookupUnit(const std::string& s, size_t at) {
  for (const UnitEntry& e : kUnitsTable) {
    size_t n = std::strlen(e.ascii);
    if (s.compare(at, n, e.ascii) == 0) return e.spoken;
  }
  return std::string();
}

}  // namespace

size_t charCount(const std::string& s, size_t from, size_t to) {
  size_t n = 0;
  for (size_t i = from; i < to && i < s.size();) {
    i += utf8Len(static_cast<unsigned char>(s[i]));
    n++;
  }
  return n;
}

std::string normalizeText(const std::string& in, const NormalizeOptions& opt) {
  std::string out;
  out.reserve(in.size() * 2);

  size_t i = 0;
  while (i < in.size()) {
    if (opt.numbers && isAsciiDigit(in[i])) {
      // Gather 123 / 12.5 / 1,234
      std::string digits;
      while (i < in.size() &&
             (isAsciiDigit(in[i]) ||
              (in[i] == '.' && i + 1 < in.size() && isAsciiDigit(in[i + 1])) ||
              (in[i] == ',' && i + 2 < in.size() && isAsciiDigit(in[i + 1]) &&
               isAsciiDigit(in[i + 2])))) {
        if (in[i] == '.') digits += '.';
        else if (in[i] != ',') digits += in[i];
        i++;
      }

      size_t unitLen = (opt.units || opt.percent) ? unitLength(in, i) : 0;
      bool isPercent = opt.percent && in.compare(i, 1, "%") == 0;
      if (isPercent) {
        out += "\xE7\x99\xBE\xE5\x88\x86\xE4\xB9\x8B";  // 百分之
      } else if (unitLen) {
        out += lookupUnit(in, i);
      }

      size_t dot = digits.find('.');
      if (dot == std::string::npos) {
        appendPlaceValues(out, digits);
      } else {
        std::string ip = digits.substr(0, dot);
        std::string fp = digits.substr(dot + 1);
        if (ip.empty()) ip = "0";
        appendPlaceValues(out, ip);
        out += "\xE7\x82\xB9";  // 点
        for (char c : fp) out += kDigit[c - '0'];
      }
      i += unitLen;
      continue;
    }

    if (opt.collapseSpace && in[i] == ' ' && !out.empty() && out.back() == ' ') {
      i++;
      continue;
    }

    out += in[i];
    i++;
  }
  return out;
}

CutPoint lastSafeCut(const std::string& buf, size_t minKeepBytes,
                     size_t maxCutBytes) {
  CutPoint r;
  if (buf.empty()) return r;

  size_t limit = (maxCutBytes == static_cast<size_t>(-1)) ? buf.size() : maxCutBytes;
  if (limit > buf.size()) limit = buf.size();
  if (limit <= minKeepBytes) return r;

  int depth = 0;
  std::vector<size_t> cuts;

  for (size_t i = 0; i < limit; i++) {
    unsigned char c = static_cast<unsigned char>(buf[i]);

    if (matches(kOpenBracket, buf, i, 1)) { depth++; continue; }
    if (matches(kCloseBracket, buf, i, 1)) { if (depth > 0) depth--; continue; }
    if (depth != 0) continue;

    size_t w = utf8Len(c);

    if (isAsciiDigit(c)) {
      // Skip the whole numeric run so a cut cannot land inside "12.5V".
      size_t j = i;
      while (j < limit && (isAsciiDigit(buf[j]) || buf[j] == '.' ||
                           buf[j] == ',')) {
        j++;
      }
      if (j > i) i = j - 1;
      continue;
    }

    if (matches(kSentenceEnd, buf, i, w) || matches(kClauseSep, buf, i, w)) {
      cuts.push_back(i + w);
      i += w - 1;
      continue;
    }
  }

  if (cuts.empty()) return r;
  for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
    if (*it <= minKeepBytes) break;
    r.offset = *it;
    r.tailBytes = buf.size() - *it;
    return r;
  }
  return r;
}

bool isFlushable(const std::string& text, size_t minTailChars) {
  for (size_t i = 0; i < text.size(); i++) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    size_t len = utf8Len(c);
    if (matches(kSentenceEnd, text, i, len)) return true;
    if (matches(kClauseSep, text, i, len)) {
      if (charCount(text, i + len, text.size()) >= minTailChars) return true;
    }
    i += len - 1;
  }
  return false;
}

}  // namespace ekho