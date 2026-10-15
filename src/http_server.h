/***************************************************************************
 * ekho-linux -- minimal HTTP/1.1 server for the streaming TTS endpoint.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Why this exists
 * ---------------
 * Upstream `ekho --server` speaks a private one-shot protocol on a bare TCP
 * socket: the client sends [cmd][speed][pitch][volume][text...] in one write,
 * the server synthesises the entire utterance, then dumps the finished file
 * back and closes. Nothing is playable until the last sample has been
 * generated, and because the reply is not length-framed the client guesses the
 * end of the body by "the last read was short", which breaks for any payload
 * whose size is an exact multiple of the read size.
 *
 * That protocol cannot carry text that arrives a token at a time, which is what
 * an LLM or an ASR front end produces, and it cannot deliver audio before the
 * text is complete. This server is the replacement:
 *
 *   POST /tts        body is UTF-8 text, either Content-Length or chunked.
 *                    The body is fed to a StreamingSession as it arrives and
 *                    the response body is length-framed 16-bit PCM, chunked,
 *                    emitted as each clause finishes. The synthesised WAV is
 *                    also written on the server; its path is returned in the
 *                    X-Ekho-File response header, which arrives before any
 *                    audio, so a client can decide to fetch the file later
 *                    rather than keep the stream.
 *   POST /tts?wait=1 same work, but buffered and returned as one complete WAV
 *                    with a correct Content-Length. This is the drop-in
 *                    replacement for the old one-shot behaviour.
 *   GET  /health     liveness, sample rate and voice, for supervisors.
 *   GET  /voices     installed voices.
 *
 * A small HTTP implementation is included rather than pulled in, because the
 * target board has no package manager and the fork is meant to cross-compile
 * against nothing but libc, pthread and whatever voice backends are enabled.
 * Only what the endpoints above need is implemented: request line, headers,
 * chunked request bodies, chunked responses, keep-alive for the small GETs
 * and Content-Length for the buffered POST.
 *
 * Concurrency: one detached thread per connection, but synthesis itself is
 * serialised process-wide by the caller. That is not a shortcut -- Ekho keeps
 * the synthesis callbacks and the engine pointer in static members
 * (Ekho::synthCallback, Ekho::synth4Callback, Ekho::synthUserdata, Ekho::impl)
 * and espeak-ng keeps global voice state, so two concurrent syntheses would
 * overwrite each other's callbacks. See the note in ekho_server_main.cpp.
 **************************************************************************/
#ifndef EKHO_HTTP_SERVER_H
#define EKHO_HTTP_SERVER_H

#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

namespace ekho {

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

struct HttpRequest {
  std::string method;
  std::string target;  // the raw request target, query included
  std::string path;    // target up to '?'
  std::string query;   // everything after '?', empty when absent
  std::string version;
  // Header names are lower-cased. Repeated headers keep the last value.
  std::map<std::string, std::string> headers;
  std::string peer;  // "ip:port", for logging

  std::string header(const std::string& name) const {
    std::map<std::string, std::string>::const_iterator it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
  }

  // Percent-decoded value of a query parameter, or `fallback` when absent.
  std::string param(const std::string& name,
                    const std::string& fallback = std::string()) const;
};

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

// Owns one socket. Header writes and body writes may come from different
// threads -- the streaming endpoint drains its PCM ring in a consumer thread
// while the request thread reads the body -- so every write takes a mutex.
class HttpConnection {
 public:
  explicit HttpConnection(int fd);
  ~HttpConnection();

  int fd() const { return fd_; }

  // Queue a header. Flushed by beginBody(). Safe to call only before then.
  void addHeader(const std::string& name, const std::string& value);

  // Sends the status line and queued headers. `chunked` selects
  // Transfer-Encoding: chunked and implies no Content-Length.
  bool beginBody(int status, const char* reason, bool chunked);

  // Length-framed response: sends the headers with Content-Length.
  bool beginLengthBody(int status, const char* reason, size_t length);

  // Writes bytes verbatim. Used for length-framed bodies.
  bool writeRaw(const void* data, size_t len);

  // Writes one chunk, framed. Used when beginBody(chunked=true) was used.
  bool writeChunk(const void* data, size_t len);

  // Terminates a chunked body with the zero-length chunk.
  bool endChunkedBody();

 private:
  bool sendAll(const void* data, size_t len);  // no locking
  void fail();

  int fd_;
  pthread_mutex_t writeMutex_;
  std::vector<std::string> headers_;
  bool began_;
  bool chunked_;
  bool broken_;
};

// ---------------------------------------------------------------------------
// Request body
// ---------------------------------------------------------------------------

// Pulls the request body out incrementally, decoding either Content-Length or
// Transfer-Encoding: chunked. The point is that the caller can start producing
// a response after the first block rather than after the whole body.
class HttpBodyReader {
 public:
  // `preloaded` / `preloadedLen` carry body bytes that arrived in the same read
  // as the request head. They must be served before anything is taken off the
  // socket, or a client that pipelines the two would lose its first bytes.
  HttpBodyReader(int fd, bool chunked, long long contentLength,
                 const char* preloaded = nullptr, size_t preloadedLen = 0);

  // Returns the number of bytes stored in `out` (>0), 0 at end of body, or -1
  // on a malformed body or a read error.
  ssize_t read(char* out, size_t maxLen);

  bool chunked() const { return chunked_; }
  long long declaredLength() const { return declared_; }

 private:
  int fill();  // returns a byte, or -1 on error/EOF
  bool readLine(std::string& out);
  // Moves up to `n` bytes into `out`, from the preload, the receive buffer, or
  // the socket, in that order. Sets eof_ when the peer is done.
  size_t pull(char* out, size_t n);

  int fd_;
  bool chunked_;
  long long remaining_;   // bytes left under Content-Length
  long long declared_;    // the original Content-Length
  bool done_;
  size_t chunkRemaining_;
  std::string preload_;
  size_t preloadPos_;
  char buf_[8192];
  size_t pos_;
  size_t len_;
  bool eof_;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

// Handles one request. It may write its own response. Return false to let the
// server emit a generic 404/500 for it.
typedef bool (*HttpHandler)(HttpConnection& conn, const HttpRequest& req,
                            HttpBodyReader& body, void* user);

struct HttpServerOptions {
  int port;
  std::string bindAddr;
  int backlog;
  std::string name;  // appears in the Server: header
  HttpServerOptions()
      : port(2046), bindAddr("0.0.0.0"), backlog(32), name("ekho-server") {}
};

class HttpServer {
 public:
  HttpServer(const HttpServerOptions& options, HttpHandler handler,
             void* user);
  ~HttpServer();

  // Binds and listens. Returns false (with a message on stderr) on failure.
  bool listen();

  // Accepts until stop() is called. Returns 0 normally.
  int serve();

  // Async-signal-safe: may be called from a SIGINT/SIGTERM handler.
  void stop();

 private:
  static void* connectionEntry(void* arg);
  void handleConnection(int fd, const std::string& peer);

  HttpServerOptions options_;
  HttpHandler handler_;
  void* user_;
  int listenFd_;
  volatile int stopping_;

  struct ConnArg {
    HttpServer* server;
    int fd;
    std::string peer;
  };
};

// Small helpers shared with the driver binaries.
std::string httpUrlDecode(const std::string& in);
std::string httpUrlEncode(const std::string& in);
std::string httpJsonEscape(const std::string& in);

}  // namespace ekho

#endif  // EKHO_HTTP_SERVER_H
