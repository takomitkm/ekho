/***************************************************************************
 * ekho-stream -- streaming driver and latency harness for ekho-linux.
 *
 * Feeds text to StreamingSession the way a real caller would (piece by piece,
 * optionally with a simulated inter-token delay) while a separate consumer
 * thread drains the PCM ring, and reports first-chunk latency, throughput and
 * ring pressure using the same definitions as the board-side TTS benchmark in
 * tts-bench.
 *
 * The consumer has to be a real thread. Serialising "feed everything, then
 * drain" measures the ring's overflow policy, not the engine: synthesis is
 * synchronous inside append(), so the producer runs far ahead of a consumer
 * that only starts afterwards.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <sndfile.h>

#include "ekho.h"
#include "streaming.h"

namespace {

void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s -v VOICE (-f TEXTFILE | --text '...') [options]\n"
          "\n"
          "  -f, --file=FILE      read text from FILE ('-' for stdin)\n"
          "      --text=TEXT      literal text instead of a file\n"
          "  -v, --voice=VOICE    Cantonese | Mandarin | Toisanese | Hakka | ...\n"
          "  -o, --output=FILE    write the synthesised audio here\n"
          "      --chunk=N        feed the text in N-byte pieces (default: whole)\n"
          "      --chunk-delay=N  sleep N ms between pieces (simulates an LLM)\n"
          "      --no-segment     synthesise the whole buffer on every append\n"
          "      --no-normalize   skip text normalisation\n"
          "      --max-segment-chars=N  cap on chars per synth4() burst (default: derived from the ring)\n"
          "      --ring-ms=N      PCM ring size in milliseconds (default 4000)\n"
          "      --consumer-delay=N  consumer polls every N ms (default 1)\n"
          "      --samplerate=N   output sample rate (default: voice native)\n"
          "      --json           emit machine-readable stats on stdout\n",
          argv0);
}

std::string readAll(const char* path) {
  if (strcmp(path, "-") == 0) {
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) out.append(buf, n);
    return out;
  }
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "ekho-stream: cannot open %s\n", path);
    exit(2);
  }
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* voice = nullptr;
  const char* path = nullptr;
  const char* literal = nullptr;
  const char* outPath = nullptr;
  size_t chunk = 0;
  int chunkDelayMs = 0;
  int consumerDelayMs = 1;
  bool segmented = true;
  bool doNormalize = true;
  bool json = false;
  int ringMs = 4000;
  int maxSegChars = 0;   // 0 = derive from ring capacity
  int samplerate = 0;

  static struct option opts[] = {
      {"file", required_argument, nullptr, 'f'},
      {"text", required_argument, nullptr, 1000},
      {"voice", required_argument, nullptr, 'v'},
      {"output", required_argument, nullptr, 'o'},
      {"chunk", required_argument, nullptr, 1001},
      {"chunk-delay", required_argument, nullptr, 1002},
      {"no-segment", no_argument, nullptr, 1003},
      {"no-normalize", no_argument, nullptr, 1004},
      {"max-segment-chars", required_argument, nullptr, 1009},
      {"ring-ms", required_argument, nullptr, 1005},
      {"consumer-delay", required_argument, nullptr, 1008},
      {"samplerate", required_argument, nullptr, 1006},
      {"json", no_argument, nullptr, 1007},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  int c;
  while ((c = getopt_long(argc, argv, "f:v:o:h", opts, nullptr)) != -1) {
    switch (c) {
      case 'f': path = optarg; break;
      case 'v': voice = optarg; break;
      case 'o': outPath = optarg; break;
      case 'h': usage(argv[0]); return 0;
      case 1000: literal = optarg; break;
      case 1001: chunk = (size_t)atoi(optarg); break;
      case 1002: chunkDelayMs = atoi(optarg); break;
      case 1003: segmented = false; break;
      case 1004: doNormalize = false; break;
      case 1005: ringMs = atoi(optarg); break;
      case 1006: samplerate = atoi(optarg); break;
      case 1007: json = true; break;
      case 1008: consumerDelayMs = atoi(optarg); break;
      case 1009: maxSegChars = atoi(optarg); break;
      default: usage(argv[0]); return 2;
    }
  }

  if (!voice || (!path && !literal)) {
    usage(argv[0]);
    return 2;
  }

  std::string text = literal ? std::string(literal) : readAll(path);
  if (text.empty()) {
    fprintf(stderr, "ekho-stream: empty text\n");
    return 2;
  }

  Ekho engine;
  if (engine.setVoice(voice) != 0) {
    fprintf(stderr, "ekho-stream: cannot load voice '%s'\n", voice);
    return 3;
  }
  if (samplerate > 0) engine.setSampleRate(samplerate);

  int rate = samplerate > 0 ? samplerate : engine.getSampleRate();
  if (rate <= 0) rate = 16000;
  size_t ringFrames = (size_t)rate * ringMs / 1000;
  if (ringFrames < 4096) ringFrames = 4096;

  ekho::StreamingSession session(&engine, ringFrames);
  session.setSegmented(segmented);
  session.setNormalize(doNormalize);
  // One synth4() burst must fit inside the ring, so the segment cap follows the
  // ring: Mandarin is ~0.2 s per character.
  if (maxSegChars <= 0) {
    maxSegChars = (int)ekho::StreamingSession::deriveMaxSegmentChars(
        ringFrames, rate);
  }
  session.setMaxSegmentChars((size_t)maxSegChars);

  if (chunk == 0) chunk = text.size();
  if (chunk == 0) chunk = 1;

  SNDFILE* snd = nullptr;
  if (outPath) {
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    info.samplerate = rate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    snd = sf_open(outPath, SFM_WRITE, &info);
    if (!snd) {
      fprintf(stderr, "ekho-stream: cannot write %s: %s\n", outPath,
              sf_strerror(nullptr));
      return 4;
    }
  }

  // Consumer: runs for the whole session, concurrently with the producer.
  std::atomic<size_t> totalFrames(0);
  std::vector<short> buf(8192);
  std::thread consumer([&]() {
    for (;;) {
      int got = session.read(buf.data(), (int)buf.size(), 20);
      if (got > 0) {
        if (snd) sf_writef_short(snd, buf.data(), got);
        totalFrames += (size_t)got;
        continue;
      }
      if (session.finished()) break;
      if (consumerDelayMs > 0) usleep((useconds_t)consumerDelayMs * 1000);
    }
  });

  const long long t0 = ekho::nowUs();

  // Producer: feed text in pieces.
  size_t pos = 0;
  while (pos < text.size()) {
    size_t n = (text.size() - pos < chunk) ? (text.size() - pos) : chunk;
    session.append(text.substr(pos, n), doNormalize);
    pos += n;
    if (chunkDelayMs > 0 && pos < text.size()) {
      usleep((useconds_t)chunkDelayMs * 1000);
    }
  }
  session.flush();

  consumer.join();
  long long elapsed = ekho::nowUs() - t0;

  size_t frames = totalFrames;
  ekho::StreamStats st = session.stats();
  if (snd) sf_close(snd);

  double audioSec = (double)frames / (double)rate;
  double firstMs = (st.firstChunkUs < 0) ? -1.0 : (double)st.firstChunkUs / 1000.0;
  double genMs = (double)st.genUs / 1000.0;
  double rtf = (genMs > 0.0) ? (audioSec * 1000.0 / genMs) : 0.0;

  if (json) {
    printf("{\"voice\":\"%s\",\"chars\":%zu,\"segments\":%zu,"
           "\"frames\":%zu,\"sample_rate\":%d,\"audio_s\":%.3f,"
           "\"first_ms\":%.3f,\"gen_ms\":%.3f,\"total_ms\":%.3f,"
           "\"rtf\":%.3f,\"dropped_frames\":%zu,\"ring_ms\":%d,"
           "\"segmented\":%s,\"normalized\":%s,\"out\":\"%s\"}\n",
           voice, text.size(), st.segments, frames, rate, audioSec, firstMs,
           genMs, (double)elapsed / 1000.0, rtf, st.droppedFrames, ringMs,
           segmented ? "true" : "false", doNormalize ? "true" : "false",
           outPath ? outPath : "");
  } else {
    printf("voice          : %s @ %d Hz\n", voice, rate);
    printf("chars in       : %zu\n", text.size());
    printf("segments       : %zu (%s)\n", st.segments,
           segmented ? "clause-boundary" : "whole-buffer");
    printf("audio out      : %zu frames = %.2f s\n", frames, audioSec);
    printf("first chunk    : %s\n",
           firstMs < 0 ? "n/a" : "see below");
    if (firstMs >= 0) printf("                %.3f ms\n", firstMs);
    printf("synthesis time : %.3f ms  (%.2fx realtime)\n", genMs, rtf);
    printf("wall clock     : %.3f ms\n", (double)elapsed / 1000.0);
    printf("ring           : %d ms capacity, %zu frames dropped\n", ringMs,
           st.droppedFrames);
    if (outPath) printf("wrote          : %s\n", outPath);
  }
  return 0;
}