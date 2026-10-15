/***************************************************************************
 * ekho-server -- streaming TTS over HTTP, replacing `ekho --server`.
 *
 * The old server was a private one-shot TCP protocol: the client sent the whole
 * text in one write and got the finished file back, so nothing was playable
 * until the last sample existed, and the reply had no framing (the client
 * guessed the end by "the last read was short"). It could not be driven by text
 * that arrives a token at a time, which is what an LLM or an ASR front end
 * produces.
 *
 * This server keeps the same idea but streams it:
 *
 *   POST /tts
 *       Body is UTF-8 text, Content-Length or chunked. Each block is fed to a
 *       StreamingSession as it arrives; the response is chunked 16-bit PCM
 *       (audio/L16) emitted clause by clause. The synthesised WAV is also
 *       written on the server and its path is returned in X-Ekho-File, which
 *       arrives before the audio, so a client can keep the file rather than the
 *       stream.
 *   POST /tts?wait=1
 *       Same synthesis, buffered and returned as one complete WAV with a
 *       correct Content-Length. This is the drop-in replacement for the old
 *       one-shot behaviour.
 *   GET  /health    liveness, sample rate and current voice.
 *   GET  /voices    voices this build ships data for.
 *
 * Query parameters on /tts: voice=NAME, speed=DELTA, pitch=DELTA, volume=DELTA
 * (the same deltas `ekho -s/-p/-V` take), and fmt=wav as an alias for wait=1.
 *
 * Concurrency
 * -----------
 * One thread per connection, but synthesis is serialised process-wide. That is
 * deliberate rather than lazy: Ekho stores the synthesis callbacks and the
 * engine pointer in statics (Ekho::synthCallback, Ekho::synth4Callback,
 * Ekho::synthUserdata, Ekho::impl) and espeak-ng keeps global voice state, so
 * two syntheses running at once would overwrite each other's callbacks --
 * whichever finished first would deliver its PCM to the other's consumer. The
 * board has four cores and one conversation's worth of work is already far
 * faster than realtime, so serialising costs nothing observable.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

#include <sndfile.h>

#include "ekho.h"
#include "http_server.h"
#include "streaming.h"

namespace {

ekho::HttpServer* gServer = nullptr;

void onSignal(int) {
  if (gServer != nullptr) gServer->stop();
}

void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s [options]\n"
          "\n"
          "  -p, --port=N         listen port (default 2046, the old server's)\n"
          "      --host=ADDR      bind address (default 0.0.0.0)\n"
          "  -v, --voice=VOICE    default voice (default Mandarin)\n"
          "      --ring-ms=N      PCM ring size in ms (default 4000)\n"
          "      --out-dir=DIR    where synthesised WAVs are kept (default /tmp)\n"
          "      --no-save        stream only, keep no server-side copy\n"
          "      --max-segment-chars=N  cap on chars per synth4() burst (0=derive)\n"
          "      --samplerate=N   output sample rate (default: voice native)\n"
          "      --verbose        log each request\n"
          "  -h, --help           this text\n",
          argv0);
}

// The voice names EkhoImpl::setVoice() accepts, and what each selects.
//
// This table exists so an unknown voice is a 400 rather than a silent fallback:
// setVoice() answers an unrecognised name by printing to stderr and speaking
// Mandarin anyway, and a client that asked for Korean and got Mandarin has no
// way to notice. `display` is what /health and the log report.
struct VoiceAlias {
  const char* name;
  const char* canonical;
  const char* display;
};
const VoiceAlias kVoices[] = {
    {"Mandarin", "pinyin", "Mandarin"},   {"zh", "pinyin", "Mandarin"},
    {"cmn", "pinyin", "Mandarin"},        {"pinyin", "pinyin", "Mandarin"},
    {"Cantonese", "jyutping", "Cantonese"}, {"yue", "jyutping", "Cantonese"},
    {"jyutping", "jyutping", "Cantonese"},
    {"Toisanese", "toisanese", "Toisanese"},
    {"Hakka", "hakka", "Hakka"},
    {"Korean", "hangul", "Korean"},       {"ko", "hangul", "Korean"},
    {"hangul", "hangul", "Korean"},
    {"Tibetan", "tibetan", "Tibetan"},    {"bo", "tibetan", "Tibetan"},
    {"tibetan", "tibetan", "Tibetan"},
    {"Ngangien", "ngangien", "Ngangien"},
    {"English", "English", "English"},    {"en", "English", "English"},
    {nullptr, nullptr, nullptr}};

const VoiceAlias* lookupVoice(const std::string& name) {
  for (int i = 0; kVoices[i].name != nullptr; i++) {
    if (name == kVoices[i].name) return &kVoices[i];
  }
  return nullptr;
}

// The display name for a canonical voice, for /voices and the log.
const char* displayName(const std::string& canonical) {
  for (int i = 0; kVoices[i].name != nullptr; i++) {
    if (canonical == kVoices[i].canonical &&
        strcmp(kVoices[i].name, kVoices[i].display) == 0) {
      return kVoices[i].display;
    }
  }
  return "(unknown)";
}

bool voiceInstalled(const std::vector<std::string>& have,
                    const std::string& canonical) {
  for (size_t i = 0; i < have.size(); i++) {
    if (have[i] == canonical) return true;
  }
  return false;
}

struct ServerState {
  Ekho* engine = nullptr;
  pthread_mutex_t synthMutex;
  std::string outDir;
  std::string defaultVoice;   // as the client names it
  std::string currentVoice;   // canonical: "pinyin", "jyutping", ...
  std::string currentDisplay; // "Mandarin", ...
  // Canonical names of the voices this build actually loaded at startup.
  std::vector<std::string> availableVoices;
  int currentSpeed = 0;
  int currentPitch = 0;
  int currentVolume = 0;
  size_t ringFrames = 0;
  int rate = 0;
  int ringMs = 4000;
  int maxSegmentChars = 0;
  bool verbose = false;
  bool saveFiles = true;
};

// Sequential, collision-free names; the process owns the directory prefix.
std::string makeOutPath(ServerState* st, bool* fresh) {
  static pthread_mutex_t nameMutex = PTHREAD_MUTEX_INITIALIZER;
  static unsigned long counter = 0;
  pthread_mutex_lock(&nameMutex);
  unsigned long n = ++counter;
  pthread_mutex_unlock(&nameMutex);

  char name[512];
  snprintf(name, sizeof name, "%s/ekho-%d-%lu-%ld.wav", st->outDir.c_str(),
           (int)getpid(), n, (long)time(nullptr));
  if (fresh != nullptr) *fresh = true;
  return std::string(name);
}

void addCommonHeaders(ekho::HttpConnection& conn, const ServerState* st) {
  conn.addHeader("Server", "ekho-server");
  conn.addHeader("Cache-Control", "no-store");
  conn.addHeader("X-Ekho-Sample-Rate", std::to_string(st->rate));
}

bool sendJson(ekho::HttpConnection& conn, int status, const char* reason,
              const std::string& json) {
  conn.addHeader("Content-Type", "application/json; charset=utf-8");
  if (!conn.beginLengthBody(status, reason, json.size())) return true;
  conn.writeRaw(json.data(), json.size());
  return true;
}

bool readFile(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[65536];
  size_t n;
  out.clear();
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
// POST /tts
// ---------------------------------------------------------------------------

bool handleTts(ekho::HttpConnection& conn, const ekho::HttpRequest& req,
               ekho::HttpBodyReader& body, ServerState* st) {
  std::string fmt = req.param("fmt");
  bool wantCompleteWav = (req.param("wait") == "1") || fmt == "wav";
  std::string requested = req.param("voice", st->defaultVoice);
  std::string speed = req.param("speed");
  std::string pitch = req.param("pitch");
  std::string volume = req.param("volume");

  const VoiceAlias* alias = lookupVoice(requested);
  if (alias == nullptr) {
    return sendJson(conn, 400, "Bad Request",
                    "{\"error\":\"unknown voice\",\"voice\":\"" +
                        ekho::httpJsonEscape(requested) +
                        "\",\"see\":\"/voices\"}\n");
  }
  if (!voiceInstalled(st->availableVoices, alias->canonical)) {
    return sendJson(conn, 400, "Bad Request",
                    "{\"error\":\"voice not installed\",\"voice\":\"" +
                        ekho::httpJsonEscape(alias->display) +
                        "\",\"see\":\"/voices\"}\n");
  }

  // One synthesis at a time: see the note at the top of this file.
  pthread_mutex_lock(&st->synthMutex);

  if (st->currentVoice != alias->canonical) {
    // setVoice() re-applies the tempo normalisation once the new voice pack is
    // loaded (see the comment in EkhoImpl::setVoice), so a voice switch is
    // self-contained and no setSpeed() call is needed here.
    if (st->engine->setVoice(alias->canonical) != 0) {
      std::string bad = alias->canonical;
      pthread_mutex_unlock(&st->synthMutex);
      return sendJson(conn, 400, "Bad Request",
                      "{\"error\":\"voice not installed\",\"voice\":\"" +
                          ekho::httpJsonEscape(bad) + "\"}\n");
    }
    st->currentVoice = alias->canonical;
    st->currentDisplay = alias->display;
    // Deltas are absolute, not sticky, and a voice change resets them.
    st->currentSpeed = st->currentPitch = st->currentVolume = 0;
  }

  // Apply this request's deltas. They are absolute: a request that omits one
  // means zero, not "keep whatever the last request set", so two requests with
  // the same parameters always produce the same audio.
  int wantSpeed = speed.empty() ? 0 : atoi(speed.c_str());
  int wantPitch = pitch.empty() ? 0 : atoi(pitch.c_str());
  int wantVolume = volume.empty() ? 0 : atoi(volume.c_str());
  if (wantSpeed != st->currentSpeed) {
    st->engine->setSpeed(wantSpeed);
    st->currentSpeed = wantSpeed;
  }
  if (wantPitch != st->currentPitch) {
    st->engine->setPitch(wantPitch);
    st->currentPitch = wantPitch;
  }
  if (wantVolume != st->currentVolume) {
    st->engine->setVolume(wantVolume);
    st->currentVolume = wantVolume;
  }

  // Read the first block of the request body before announcing the response.
  //
  // A client that sent Expect: 100-continue is still waiting to upload, and a
  // final status line arriving at that moment makes it abandon the body -- curl
  // then reports the upload as complete having sent nothing. Holding the status
  // line until the body has actually started also makes the header order mean
  // something: by the time a client sees 200 the request has been accepted.
  // Nothing about the streaming property is lost, because the wait is only
  // until the *first* byte of text, not until the last.
  char raw[8192];
  ssize_t firstLen = body.read(raw, sizeof raw);
  if (firstLen > 0) {
    // Tolerate a UTF-8 BOM; some clients send one with a text body.
    if (firstLen >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
      memmove(raw, raw + 3, (size_t)firstLen - 3);
      firstLen -= 3;
    }
  }
  if (firstLen == 0) {
    pthread_mutex_unlock(&st->synthMutex);
    return sendJson(conn, 400, "Bad Request",
                    "{\"error\":\"empty body: nothing to synthesise\"}\n");
  }
  if (firstLen < 0) {
    pthread_mutex_unlock(&st->synthMutex);
    return sendJson(conn, 400, "Bad Request",
                    "{\"error\":\"malformed request body\"}\n");
  }

  // The server-side copy. The brief is that the audio files live on the server,
  // so this is written even when the client is streaming; ?wait=1 additionally
  // reads it back to answer with a Content-Length.
  std::string outPath;
  SNDFILE* snd = nullptr;
  if (st->saveFiles || wantCompleteWav) {
    outPath = makeOutPath(st, nullptr);
    SF_INFO info;
    memset(&info, 0, sizeof info);
    info.samplerate = st->rate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    snd = sf_open(outPath.c_str(), SFM_WRITE, &info);
    if (snd == nullptr) {
      fprintf(stderr, "ekho-server: cannot write %s: %s\n", outPath.c_str(),
              sf_strerror(nullptr));
      outPath.clear();
      if (wantCompleteWav) {
        pthread_mutex_unlock(&st->synthMutex);
        return sendJson(conn, 500, "Internal Server Error",
                        "{\"error\":\"cannot open output file\"}\n");
      }
    }
  }

  ekho::StreamingSession session(st->engine, st->ringFrames);
  session.setSegmented(true);
  session.setNormalize(true);
  if (st->maxSegmentChars > 0) {
    session.setMaxSegmentChars((size_t)st->maxSegmentChars);
  }

  std::vector<short> buf(8192);
  size_t totalFrames = 0;
  bool sendFailed = false;
  bool streaming = !wantCompleteWav;

  if (streaming) {
    conn.addHeader("Content-Type",
                   "audio/L16;rate=" + std::to_string(st->rate) +
                       ";channels=1;bit=16");
    addCommonHeaders(conn, st);
    if (!outPath.empty()) conn.addHeader("X-Ekho-File", outPath);
    if (!conn.beginBody(200, "OK", true)) {
      if (snd) sf_close(snd);
      pthread_mutex_unlock(&st->synthMutex);
      return true;
    }
  }

  // The consumer must be a real thread: synth4() is synchronous inside
  // append(), so a consumer that only starts after the producer returns would
  // measure the ring's back pressure rather than the stream.
  std::thread consumer([&]() {
    for (;;) {
      int got = session.read(buf.data(), (int)buf.size(), 20);
      if (got > 0) {
        if (snd) sf_writef_short(snd, buf.data(), got);
        if (streaming &&
            !conn.writeChunk(buf.data(), (size_t)got * 2)) {
          sendFailed = true;
          // Unblock the producer, which may be parked on a full ring.
          session.stop();
          break;
        }
        totalFrames += (size_t)got;
        continue;
      }
      if (session.finished()) break;
    }
  });

  // Producer: the block already read, then the rest as it arrives.
  session.append(std::string(raw, (size_t)firstLen), true);
  {
    ssize_t n;
    while (!sendFailed && (n = body.read(raw, sizeof raw)) > 0) {
      session.append(std::string(raw, (size_t)n), true);
    }
    if (n < 0) {
      fprintf(stderr, "ekho-server: truncated request body\n");
    }
  }

  if (!sendFailed) session.flush();
  consumer.join();
  if (snd) sf_close(snd);

  if (st->verbose) {
    ekho::StreamStats stats = session.stats();
    fprintf(stderr,
            "[%s] voice=%s body=%s frames_out=%zu segments=%zu first=%.1fms "
            "gen=%.1fms dropped=%zu%s\n",
            req.peer.c_str(), st->currentDisplay.c_str(),
            body.chunked() ? "chunked" : "length", totalFrames, stats.segments,
            stats.firstChunkUs < 0 ? -1.0 : stats.firstChunkUs / 1000.0,
            stats.genUs / 1000.0, stats.droppedFrames,
            sendFailed ? " (client went away)" : "");
  }

  if (streaming) {
    if (!sendFailed) conn.endChunkedBody();
    if (!st->saveFiles && !outPath.empty()) unlink(outPath.c_str());
    pthread_mutex_unlock(&st->synthMutex);
    return true;
  }

  // ?wait=1: answer with the complete file so the framing is unambiguous.
  std::string wavData;
  if (outPath.empty() || !readFile(outPath, wavData)) {
    pthread_mutex_unlock(&st->synthMutex);
    return sendJson(conn, 500, "Internal Server Error",
                    "{\"error\":\"synthesis produced no file\"}\n");
  }

  conn.addHeader("Content-Type", "audio/wav");
  addCommonHeaders(conn, st);
  if (st->saveFiles) conn.addHeader("X-Ekho-File", outPath);
  conn.beginLengthBody(200, "OK", wavData.size());
  conn.writeRaw(wavData.data(), wavData.size());

  if (!st->saveFiles) unlink(outPath.c_str());
  pthread_mutex_unlock(&st->synthMutex);
  return true;
}

// ---------------------------------------------------------------------------
// routing
// ---------------------------------------------------------------------------

bool handleRequest(ekho::HttpConnection& conn, const ekho::HttpRequest& req,
                   ekho::HttpBodyReader& body, void* user) {
  ServerState* st = static_cast<ServerState*>(user);

  if (req.method == "GET" && req.path == "/health") {
    std::string json =
        "{\"status\":\"ok\",\"voice\":\"" +
            ekho::httpJsonEscape(st->currentDisplay) +
        "\",\"sample_rate\":" + std::to_string(st->rate) +
        ",\"ring_ms\":" + std::to_string(st->ringMs) + "}\n";
    return sendJson(conn, 200, "OK", json);
  }

  if (req.method == "GET" && req.path == "/voices") {
    // Only what this build could actually load at startup, so a client can tell
    // "this server does not do Korean" from "this server does Korean and failed".
    std::string json = "{\"default\":\"" +
                       ekho::httpJsonEscape(st->currentDisplay) +
                       "\",\"voices\":[";
    for (size_t i = 0; i < st->availableVoices.size(); i++) {
      if (i > 0) json += ",";
      json += "\"" + ekho::httpJsonEscape(displayName(st->availableVoices[i])) +
              "\"";
    }
    json += "]}\n";
    return sendJson(conn, 200, "OK", json);
  }

  if (req.method == "POST" && (req.path == "/tts" || req.path == "/tts.wav")) {
    return handleTts(conn, req, body, st);
  }

  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  int port = 2046;
  const char* host = "0.0.0.0";
  const char* voice = "Mandarin";
  const char* outDir = "/tmp";
  int ringMs = 4000;
  int maxSegChars = 0;
  int samplerate = 0;
  bool verbose = false;
  bool saveFiles = true;

  static struct option opts[] = {
      {"port", required_argument, nullptr, 'p'},
      {"host", required_argument, nullptr, 1000},
      {"voice", required_argument, nullptr, 'v'},
      {"ring-ms", required_argument, nullptr, 1001},
      {"out-dir", required_argument, nullptr, 1002},
      {"no-save", no_argument, nullptr, 1003},
      {"max-segment-chars", required_argument, nullptr, 1004},
      {"samplerate", required_argument, nullptr, 1005},
      {"verbose", no_argument, nullptr, 1006},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  int c;
  while ((c = getopt_long(argc, argv, "p:v:h", opts, nullptr)) != -1) {
    switch (c) {
      case 'p': port = atoi(optarg); break;
      case 'v': voice = optarg; break;
      case 'h': usage(argv[0]); return 0;
      case 1000: host = optarg; break;
      case 1001: ringMs = atoi(optarg); break;
      case 1002: outDir = optarg; break;
      case 1003: saveFiles = false; break;
      case 1004: maxSegChars = atoi(optarg); break;
      case 1005: samplerate = atoi(optarg); break;
      case 1006: verbose = true; break;
      default: usage(argv[0]); return 2;
    }
  }

  // A client that disconnects mid-stream must not kill the server.
  signal(SIGPIPE, SIG_IGN);

  ServerState st;
  pthread_mutex_init(&st.synthMutex, nullptr);
  st.outDir = outDir;
  st.defaultVoice = voice;
  st.ringMs = ringMs;
  st.verbose = verbose;
  st.saveFiles = saveFiles;

  const VoiceAlias* defaultAlias = lookupVoice(voice);
  if (defaultAlias == nullptr) {
    fprintf(stderr, "ekho-server: unknown voice '%s' (try --help)\n", voice);
    return 3;
  }

  Ekho engine;
  // setVoice() applies the tempo normalisation itself once the pack is loaded,
  // so there is no separate setSpeed() call here.
  if (engine.setVoice(defaultAlias->canonical) != 0) {
    fprintf(stderr, "ekho-server: cannot load voice '%s'\n", voice);
    return 3;
  }

  // Ask the library which of the other voices are installed, rather than
  // assuming. The shipped voice pack is not the same everywhere -- it has full
  // Mandarin and Cantonese packs and no data at all for several of the languages
  // the code knows about -- and a client that requests a voice this build cannot
  // load should get a 400, not a silent fallback or a dead connection. The probe
  // goes through setVoice() on purpose: what counts as installed is the
  // dictionary's business and duplicating its rules here would drift from them.
  int skipped = 0;
  for (int i = 0; kVoices[i].name != nullptr; i++) {
    if (strcmp(kVoices[i].name, kVoices[i].display) != 0) continue;  // canonical only
    std::string canonical = kVoices[i].canonical;
    if (voiceInstalled(st.availableVoices, canonical)) continue;
    if (canonical == defaultAlias->canonical) {
      st.availableVoices.push_back(canonical);
      continue;
    }
    if (engine.setVoice(canonical.c_str()) == 0) {
      st.availableVoices.push_back(canonical);
    } else {
      skipped++;
      if (verbose) {
        fprintf(stderr, "ekho-server: voice %s is not installed here, skipping\n",
                kVoices[i].display);
      }
    }
  }
  // The probe will have landed on the last voice that loaded; go back to the one
  // the operator asked for.
  engine.setVoice(defaultAlias->canonical);
  if (samplerate > 0) engine.setSampleRate(samplerate);

  int rate = samplerate > 0 ? samplerate : engine.getSampleRate();
  if (rate <= 0) rate = 16000;

  st.engine = &engine;
  st.currentVoice = defaultAlias->canonical;
  st.currentDisplay = defaultAlias->display;
  st.rate = rate;
  st.ringFrames = (size_t)rate * (size_t)ringMs / 1000;
  if (st.ringFrames < 4096) st.ringFrames = 4096;
  if (maxSegChars <= 0) {
    maxSegChars = (int)ekho::StreamingSession::deriveMaxSegmentChars(
        st.ringFrames, rate);
  }
  st.maxSegmentChars = maxSegChars;

  ekho::HttpServerOptions serverOpts;
  serverOpts.port = port;
  serverOpts.bindAddr = host;
  ekho::HttpServer server(serverOpts, &handleRequest, &st);
  gServer = &server;

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = onSignal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!server.listen()) return 4;

  printf("ekho-server listening on %s:%d  voice=%s  rate=%d Hz  ring=%d ms  "
         "segment cap=%d chars\n",
         host, port, voice, rate, ringMs, maxSegChars);
  printf("  voices installed:");
  for (size_t i = 0; i < st.availableVoices.size(); i++) {
    printf(" %s", displayName(st.availableVoices[i]));
  }
  if (skipped > 0) printf("  (%d not in this voice pack)", skipped);
  printf("\n");
  printf("  POST /tts        stream 16-bit PCM (audio/L16), chunked\n");
  printf("  POST /tts?wait=1 complete WAV with Content-Length\n");
  printf("  GET  /health     liveness\n");
  printf("  GET  /voices     installed voices\n");
  fflush(stdout);

  int rc = server.serve();

  // Let any in-flight request finish before tearing the engine down.
  for (int i = 0; i < 100; i++) {
    pthread_mutex_lock(&st.synthMutex);
    pthread_mutex_unlock(&st.synthMutex);
    usleep(20000);
    if (i >= 4) break;
  }

  return rc;
}
