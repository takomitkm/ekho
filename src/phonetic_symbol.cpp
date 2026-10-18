/***************************************************************************
 * Copyright (C) 2008-2024 by Cameron Wong                                 *
 * name in passport: HUANG GUANNENG                                        *
 * email: hgneng at gmail.com                                              *
 * website: https://eguidedog.net                                       *
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
#include <iostream>
#include <sndfile.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include "audio.h"
#include "ekho_dict.h"

using namespace std;
namespace ekho {
  bool PhoneticSymbol::debug = false;

  // Memory-backed source for libsndfile. libsndfile has no "open from
  // memory" call -- sf_open_virtual() is the documented way to decode a
  // buffer in RAM, and it exists in every version we link (1.0.28 in the
  // cross kit, 1.2.2 in the simulation environment).
  namespace {
    struct MemSource {
      const char *data;
      sf_count_t size;
      sf_count_t pos;
    };

    sf_count_t memGetFilelen(void *user) {
      return static_cast<MemSource *>(user)->size;
    }

    sf_count_t memSeek(sf_count_t offset, int whence, void *user) {
      MemSource *m = static_cast<MemSource *>(user);
      sf_count_t np;
      switch (whence) {
        case SEEK_SET: np = offset; break;
        case SEEK_CUR: np = m->pos + offset; break;
        case SEEK_END: np = m->size + offset; break;
        default: return -1;
      }
      if (np < 0 || np > m->size) return -1;
      m->pos = np;
      return np;
    }

    sf_count_t memRead(void *ptr, sf_count_t count, void *user) {
      MemSource *m = static_cast<MemSource *>(user);
      sf_count_t avail = m->size - m->pos;
      if (count > avail) count = avail;
      if (count <= 0) return 0;
      memcpy(ptr, m->data + m->pos, (size_t)count);
      m->pos += count;
      return count;
    }

    // Read-only source: libsndfile checks that all five callbacks are
    // non-NULL even in SFM_READ mode, so write must exist -- it just never
    // gets called, and "wrote nothing" is the honest answer anyway.
    sf_count_t memWrite(const void *, sf_count_t, void *) { return 0; }

    sf_count_t memTell(void *user) {
      return static_cast<MemSource *>(user)->pos;
    }
  }  // namespace

  const char* PhoneticSymbol::getPcm(FILE *file, int &size) {
    if (PhoneticSymbol::debug) {
      cerr << "PhoneticSymbol::getPcm:" << "symbol=" <<
        this->symbol << ", offset=" << offset <<
        ", bytes=" << bytes << endl;
    }


    // 如果该拼音没有找到5声调音频，尝试用1升调代替
    int symbolLen = strlen(symbol);
    if (bytes == 0 && symbolLen > 0 && symbol[symbolLen - 1] == '5') {
      char s[16] = {0};
      strncat(s, symbol, 15);
      s[symbolLen - 1] = '1';
      if (PhoneticSymbol::debug) {
        cerr << symbol << " not found. try " << s << endl;
      }
      PhoneticSymbol *phon = Dict::me->getPhoneticSymbol(s);
      if (phon->bytes > 0) {
        offset = phon->offset;
        bytes = phon->bytes;
      }
    }

    // The .voice file is a plain concatenation of per-symbol sound files
    // (see Dict::makeVoiceFile): each chunk at [offset, offset+bytes) is a
    // complete WAV/GSM file with its own header, and the .index carries the
    // offsets. Upstream decoded such a chunk by copying it to a temp file
    // under /tmp and reopening that with libsndfile. The copy bought
    // nothing -- libsndfile parses the very same bytes straight out of
    // memory -- and cost four things on the target:
    //   * a hidden "must have a writable /tmp" runtime dependency; on a
    //     read-only rootfs every syllable decoded to silence;
    //   * a predictable temp name (/tmp/ekho<N>, a per-process counter),
    //     so two processes clobbered each other's half-written chunks;
    //   * two leaked handles per symbol: neither the FILE* nor the SNDFILE
    //     was ever closed, so a long session crawled toward RLIMIT_NOFILE;
    //   * write+unlink on flash plus page-cache churn on first use of
    //     every symbol.
    // Read the chunk once into a heap buffer and decode it from memory.
    if (!mPcm && file && bytes > 0 && fseek(file, offset, SEEK_SET) == 0) {
      vector<char> chunk((size_t)bytes);
      if (fread(chunk.data(), 1, (size_t)bytes, file) != (size_t)bytes) {
        cerr << "PhoneticSymbol::getPcm: fail to read " << bytes
             << " bytes at offset " << offset << " for symbol "
             << this->symbol << endl;
        size = 0;
        return 0;
      }

      SF_INFO sfinfo;
      memset(&sfinfo, 0, sizeof(SF_INFO));
      MemSource src;
      src.data = chunk.data();
      src.size = (sf_count_t)bytes;
      src.pos = 0;
      SF_VIRTUAL_IO vio;
      vio.get_filelen = memGetFilelen;
      vio.seek = memSeek;
      vio.read = memRead;
      vio.write = memWrite;
      vio.tell = memTell;
      SNDFILE *sndfile = sf_open_virtual(&vio, SFM_READ, &sfinfo, &src);
      readSndfile(sndfile, sfinfo);
      if (sndfile) {
        sf_close(sndfile);
      }

    }

    size = mSize;


    return mPcm;
  }

  void PhoneticSymbol::readSndfile(SNDFILE *sndfile, SF_INFO sfinfo) {
      if (!sndfile) {
//            cerr << "Fail to open file " << wav_file << " at " << __LINE__ <<
//              " of " << __FILE__ << endl;                
      } else {
//            sfinfo.channels = 1; // this->sfinfo.channels is corrupted
        if (sfinfo.channels > 2) {
          static bool show_channel_error = true;
          if (show_channel_error) {
            cerr << "Invalid channels: " << sfinfo.channels << endl;
            show_channel_error = false;
          }

          sfinfo.channels = 1;
        }
        int samples = 0;

        /* sfinfo.channels has not been taken into account .... */
        switch (sfinfo.format & SF_FORMAT_SUBMASK) {
          case SF_FORMAT_VORBIS:
          case SF_FORMAT_GSM610:
          case SF_FORMAT_PCM_16:
            mSize = (int)sfinfo.frames * 2 * sfinfo.channels;
            if (mPcm) {
              delete[] mPcm;
              mPcm = nullptr;
            }
            mPcm = new char[mSize];
            samples = (int)sf_readf_short(sndfile, (short int*)mPcm, sfinfo.frames);
            break;
          case SF_FORMAT_PCM_S8:
          case SF_FORMAT_PCM_U8:
          default:
            cerr << "Unknown soundfile format: " << (sfinfo.format & SF_FORMAT_SUBMASK) << endl;
        }

        if (samples != sfinfo.frames) {
          cerr << "Fail to read : " << samples <<
            " frames out of " << sfinfo.frames << " have been read." << endl;
        }
      }
  };

  const char* PhoneticSymbol::getPcm(const char *wavDir, const char *postfix, int &size, SF_INFO &sfinfo) {
    if (PhoneticSymbol::debug) {
      cerr << "PhoneticSymbol::getPcm:"  << "symbol=" <<
        this->symbol << ", wavDir=" << wavDir <<
        ", postfix=" << postfix << endl;
    }

    if (!mPcm) {
      memset(&sfinfo, 0, sizeof(SF_INFO));

      string wav_file = wavDir;
      // char | 32 means get lower case
      if (this->symbol[0] == '\\')
      {
        char c = this->symbol[1] | 32;
        if (c >= 'a' && c <= 'z') {
          wav_file += "/../alphabet/";
          wav_file += c;
          wav_file += ".wav";
        } else {
          return 0;
        }
      } else {
        wav_file += "/";
        wav_file += this->symbol;
        wav_file += ".";
        wav_file += postfix;
      }

      SNDFILE *sndfile = sf_open(wav_file.c_str(), SFM_READ, &sfinfo);
      readSndfile(sndfile, sfinfo);
      sf_close(sndfile);
    }

    size = mSize;
    return mPcm;
  };
}
