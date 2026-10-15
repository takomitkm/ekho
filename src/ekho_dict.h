/***************************************************************************
 * Copyright (C) 2008-2026 by Cameron Wong                                 *
 * name in passport: HUANG GUANNENG                                        *
 * email: hgneng at gmail.com                                              *
 * website: https://eguidedog.net                                          *
 *                                                                         *
 * This program is free software; you can redistribute it and/or           *
 * modify it under the terms of the GNU General Public License             *
 * as published by the Free Software Foundation; either version 2          *
 * of the License, or any later version.                                   *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program; if not, write to the Free Software             *
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,              *
 * MA  02110-1301, USA.                                                    *
 **************************************************************************/
#ifndef EKHO_DICT
#define EKHO_DICT

#include <sndfile.h>
#include <list>
#include <map>
#include "ekho_typedef.h"
#include "character.h"
#include "zh_symbol_map.h"
#include "zhy_symbol_map.h"
#include "word.h"

#include "config.h"


using namespace std;

namespace ekho {
typedef enum {
  CANTONESE = 1,
  MANDARIN = 2,
  ENGLISH = 3,
  KOREAN = 4,
  HAKKA = 5,
  TIBETAN = 6,
  NGANGIEN = 7,
  TOISANESE = 8,
} Language;

struct DictItem {
  DictItem() { wordList = 0; }
  ~DictItem() {
    if (wordList) {
      delete wordList;
      wordList = 0;
    }
  }
  Character character;
  list<list<Character> > *wordList;
};

class SymbolLetter {
 public:
  unsigned short index;
  SymbolLetter *next;
  static const int SIZE = 36;
};

class Dict {
 public:
  Dict(void);
  Dict(Language lang);
  ~Dict(void);

  static Dict* me;
  // Size of the phonetic symbol tables below. `code` (SymbolCode) and
  // SymbolLetter::index are both 16-bit, so this must stay under 65536.
  //
  // 8000 was too small, and not because any one language has that many symbols.
  // mKaSymbolArray holds the union of the additional dictionaries -- Hakka,
  // Korean, Toisanese, Tibetan and Ngangien -- because the symbol trie and its
  // counter are never reset between language loads. Measured on the shipped
  // voice pack, loading those five in turn reaches roughly 9500 entries, so the
  // old limit was crossed and mKaSymbolArray[mKaSymbolIndex] wrote past the end
  // of the array, over mKaSymbolLetter -- the trie being walked at that moment.
  // The result was a segfault inside getSymbolCode(), nowhere near the store
  // that caused it. getSymbolCode() now refuses to go past the end as well, so
  // a bigger pack degrades to a reported error rather than a crash.
  const static int SYMBOL_ARRAY_SIZE = 16384;
  static bool mDebug;
  string mDataPath;
  // SNDFILE *mVoiceFile;
  FILE* mVoiceFile;
  const char* mVoiceFileType;  // "wav" or "gsm"
  map<string, PhoneticSymbol> mWordSymbolMap;
  SF_INFO mSfinfo;
  // No `= {}` here, deliberately. DictItem has a user-provided default
  // constructor, so an empty initialiser list is value-initialisation, which for
  // such a type just calls that constructor -- exactly what plain
  // default-initialisation does. It changes nothing about the object, but it does
  // change what the compiler has to emit: 65536 explicit initialisers instead of
  // 65536 constructor calls, and with DictItem made trivial to check, 40 MB of
  // object code rather than 3.8 MB. On the Arm 10.3 toolchain's 32-bit host
  // cc1plus anything above -O0 then dies silently with no diagnostic at all,
  // which is how this was found. See wiki/md/技术文档/实时伪tts.
  DictItem mDictItemArray[65536];

  // map code to PhoneticSymbol
  // for Mandarin and Cantonese
  static PhoneticSymbol mSymbolArray[SYMBOL_ARRAY_SIZE];

  // for Hakka
  PhoneticSymbol* mKaSymbolArray[SYMBOL_ARRAY_SIZE];
  int mKaSymbolIndex;  // for aditional dictionary

  SymbolLetter mKaSymbolLetter[SymbolLetter::SIZE];
  int getSymbolCode(SymbolLetter* root, const char* symbol);

  int setLanguage(Language lang);
  inline Language getLanguage(void) { return mLanguage; };
  int setVoice(string voice);
  inline string getVoice(void) { return mVoice; };
  int loadMandarin(void);

  bool isNumber(int code);

  PhoneticSymbol* lookup(const Character& c);
  PhoneticSymbol* lookup(unsigned int code) {
    Character c(code);
    return lookup(c);
  }
  list<PhoneticSymbol*> lookup(const string& text) {
    list<Character> charList = Character::split(text);
    return lookup(charList);
  }
  list<PhoneticSymbol*> lookup(list<Character>& charList,
      bool firstWord = false);
  list<PhoneticSymbol*> lookup(const char* text) {
    string str(text);
    return lookup(str);
  }

  list<OverlapType> lookupOverlap(list<Character>& charList);
  list<OverlapType> lookupOverlap(string& text) {
    list<Character> charList = Character::split(text);
    return lookupOverlap(charList);
  }

  list<PhoneticSymbol*> lookupFirstWord(const char* text) {
    string str = text;
    list<Character> char_list = Character::split(str, 10);
    return lookup(char_list, true);
  }

  void getWordPcm(list<PhoneticSymbol*>& word_phon, unsigned int& offset,
      unsigned short& bytes);

  int loadEspeakDict(const char* path);
  inline int loadEspeakDict(const string& path) {
    return loadEspeakDict(path.c_str());
  };
  int saveEkhoDict(const char* path);
  inline int saveEkhoDict(const string& path) {
    return saveEkhoDict(path.c_str());
  };
  int loadEkhoDict(const char* path);
  inline int loadEkhoDict(const string& path) {
    return loadEkhoDict(path.c_str());
  };
  inline PhoneticSymbol* getFullPause(void) { return mFullPause; };
  inline PhoneticSymbol* getHalfPause(void) { return mHalfPause; };
  inline PhoneticSymbol* getQuaterPause(void) { return mQuaterPause; };
  inline int getFullPauseSize(void) { return mFullPausePcmSize; };
  // Rescale the synthetic pause buffers to the voice pack's sample rate.
  // They are allocated for 16 kHz (25000 frames = 1.56 s); at 44100 Hz that
  // would be only 0.57 s, so every punctuation pause came out ~2.8x too short.
  void setPauseSampleRate(int samplerate);

  bool isPunctuationChar(int code, EkhoPuncType mode); 
  inline string getPunctuationName(int code) {
    map<int, const char *>::iterator it = mPunctuationNameMap.find(code);
    if (it != mPunctuationNameMap.end())
      return it->second;
    else
      return 0;
  }

  /**
   * Return whether character is within word context.
   * Last character will be marked 0. Other characters will be marked 1
   * All characters will be marked 0 if len not match phons_len
   * it's caller's responsibility to free in_word_context
   */
  void getWordContext(const char* text, char* in_word_context, int phons_len);

  // for style of [[pin1 yin1]]
  PhoneticSymbol* getPhoneticSymbol(string symbol);

  list<Character> replaceNumbers(list<Character> charList);

 private:
  /**
   * Map character code to DictItem
   */
  map<int, DictItem> mExtraDictItemMap;
  map<int, const char*> mPunctuationNameMap;
  list<PhoneticSymbol*> mSpecialSymbols; // for delete when destroy
  list<char *> mSpecialChars; // for delete when destroy

  string mVoice;
  int mFullPausePcmSize;
  Language mLanguage;
  PhoneticSymbol* mFullPause;
  PhoneticSymbol* mHalfPause;
  PhoneticSymbol* mQuaterPause;


  void init(void);
  string getDefaultDataPath(void);
  void addSpecialSymbols(void);

  inline PhoneticSymbol* getZhyPhon(const char* sym) {
    return &mSymbolArray[ZHY_PHash::in_word_set(sym, strlen(sym))->code];
  }

  inline PhoneticSymbol* getZhPhon(const char* sym) {
    return &mSymbolArray[ZH_PHash::in_word_set(sym, strlen(sym))->code];
  }

  inline void addDictItem(unsigned short code, PhoneticSymbol* phonSymbol) {
    mDictItemArray[code].character.code = code;
    mDictItemArray[code].character.phonSymbol = phonSymbol;
  }

  bool hasNumbers(list<Character>& charList);

  unsigned short getCodeOfSymbol(string &symbol);

  void saveEkhoVoiceFile();
  void loadEkhoVoiceFile(string path);
};
}

#endif
