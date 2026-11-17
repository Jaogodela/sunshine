/**
 * @file src/sp_metrics.cpp
 * @brief Gaspar optional local Prometheus metrics and session event endpoint.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "input.h"
#include "logging.h"
#include "platform/common.h"
#include "sp_metrics.h"

using namespace std::literals;

namespace {
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;
  using resp_http_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  constexpr std::size_t max_events = 256;  ///< Maximum retained session events.
  constexpr auto events_timeout = 25s;  ///< Long-poll timeout for `/sp/events`.
  constexpr std::size_t http_thread_count = 4;  ///< Workers, so a long poll does not block metrics scrapes.

  std::atomic<double> capture_last_ms {0.0};  ///< Last capture backend call duration.
  std::atomic<double> capture_sum_ms {0.0};  ///< Total measured capture time.
  std::atomic<std::uint64_t> capture_count {0};  ///< Number of timed capture calls.
  std::atomic<double> encode_last_ms {0.0};  ///< Last encoder call duration.
  std::atomic<double> encode_sum_ms {0.0};  ///< Total measured encode time.
  std::atomic<std::uint64_t> encode_count {0};  ///< Number of timed encoder calls.
  std::atomic<double> fps {0.0};  ///< Exported frame rate gauge.
  std::atomic<double> bitrate_kbps {0.0};  ///< Exported bitrate gauge.
  std::atomic<std::uint64_t> frames_dropped {0};  ///< Frames encoded without a fresh capture.
  std::atomic<std::uint64_t> idr_total {0};  ///< IDR frames requested.
  std::atomic<std::uint64_t> bytes_sent {0};  ///< Total video payload bytes sent.

  std::mutex rate_mutex;  ///< Protects the frame-rate and bitrate windows.
  std::chrono::steady_clock::time_point fps_window_start = std::chrono::steady_clock::now();  ///< Current FPS window start.
  std::uint64_t fps_window_frames = 0;  ///< Encoded frames in the current FPS window.
  std::chrono::steady_clock::time_point bitrate_window_start = std::chrono::steady_clock::now();  ///< Current bitrate window start.
  std::uint64_t bitrate_window_bytes = 0;  ///< Bytes in the current bitrate window.

  std::mutex events_mutex;  ///< Protects the event ring.
  std::condition_variable events_cv;  ///< Wakes long-poll requests when an event is recorded or the server stops.
  std::deque<sp_metrics::event_t> events;  ///< Bounded ring of session events.
  std::uint64_t next_event_id = 1;  ///< Identifier assigned to the next event.
  std::atomic<std::uint64_t> shutdown_epoch {0};  ///< Incremented on stop() to wake existing long polls.

  std::mutex server_mutex;  ///< Protects the server instance pointer.
  std::shared_ptr<http_server_t> server_instance;  ///< Active endpoint server, or null when stopped.

  /**
   * @brief Escape a string for inclusion in a JSON string literal.
   *
   * @param value Raw string value.
   * @return Escaped JSON string contents.
   */
  std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());

    for (const char character : value) {
      switch (character) {
        case '"':
          escaped += "\\\"";
          break;
        case '\\':
          escaped += "\\\\";
          break;
        case '\n':
          escaped += "\\n";
          break;
        case '\r':
          escaped += "\\r";
          break;
        case '\t':
          escaped += "\\t";
          break;
        default:
          if (static_cast<unsigned char>(character) < 0x20) {
            escaped += std::format("\\u{:04x}", static_cast<unsigned int>(character));
          } else {
            escaped += character;
          }
          break;
      }
    }

    return escaped;
  }

  /**
   * @brief Write an HTTP 200 response without caching.
   *
   * @param response HTTP response to write.
   * @param body Response body.
   * @param content_type Response content type.
   */
  void write_response(const resp_http_t &response, const std::string &body, const std::string &content_type) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", content_type);
    headers.emplace("Cache-Control", "no-store");
    response->write(SimpleWeb::StatusCode::success_ok, body, headers);
  }

  /**
   * @brief Serve `GET /metrics`.
   *
   * @param response HTTP response to write.
   * @param request Incoming request, unused.
   */
  void metrics_handler(const resp_http_t &response, const req_http_t &request) {
    (void) request;
    write_response(response, sp_metrics::render_metrics_text(), "text/plain; version=0.0.4; charset=utf-8");
  }

  /**
   * @brief Serve `GET /sp/events`.
   *
   * @param response HTTP response to write.
   * @param request Incoming request carrying the optional `since` query parameter.
   */
  void events_handler(const resp_http_t &response, const req_http_t &request) {
    std::uint64_t since = 0;
    const auto query = request->parse_query_string();
    if (const auto it = query.find("since"); it != query.end()) {
      try {
        since = std::stoull(it->second);
      } catch (const std::exception &) {
        since = 0;
      }
    }

    write_response(response, sp_metrics::events_json(since, events_timeout), "application/json; charset=utf-8");
  }

  /**
   * @brief Serve an unknown path with HTTP 404.
   *
   * @param response HTTP response to write.
   * @param request Incoming request, unused.
   */
  void not_found_handler(const resp_http_t &response, const req_http_t &request) {
    (void) request;
    response->write(SimpleWeb::StatusCode::client_error_not_found);
  }

  /**
   * @brief Build the host string the endpoint should bind to.
   *
   * @return Configured bind address, or loopback when the user has not set one.
   */
  std::string bind_address() {
    return config::sunshine.bind_address.empty() ? std::string {"127.0.0.1"} : config::sunshine.bind_address;
  }
}  // namespace

namespace sp_metrics {
  void start() {
    const auto configured_port = config::sunshine.sp_metrics_port;
    if (configured_port <= 0) {
      return;
    }

    auto server = std::make_shared<http_server_t>();
    server->config.address = bind_address();
    server->config.port = static_cast<unsigned short>(configured_port);
    server->config.reuse_address = true;
    server->config.thread_pool_size = http_thread_count;

    server->resource["^/metrics$"]["GET"] = [](const resp_http_t &response, const req_http_t &request) {
      metrics_handler(response, request);
    };
    server->resource["^/sp/events$"]["GET"] = [](const resp_http_t &response, const req_http_t &request) {
      events_handler(response, request);
    };
    server->default_resource["GET"] = not_found_handler;

    {
      std::lock_guard lock {server_mutex};
      server_instance = server;
    }

    platf::set_thread_name("sp_metrics");
    BOOST_LOG(info) << "Starting metrics/events endpoint on "sv << server->config.address << ':' << configured_port;

    try {
      server->start();
    } catch (const std::exception &err) {
      BOOST_LOG(error) << "Couldn't start metrics/events endpoint on "sv << server->config.address << ':' << configured_port << ": "sv << err.what();
    }

    {
      std::lock_guard lock {server_mutex};
      if (server_instance == server) {
        server_instance.reset();
      }
    }

    BOOST_LOG(info) << "Metrics/events endpoint stopped"sv;
  }

  void stop() {
    std::shared_ptr<http_server_t> server;
    {
      std::lock_guard lock {server_mutex};
      server = server_instance;
    }

    shutdown_epoch.fetch_add(1, std::memory_order_relaxed);
    events_cv.notify_all();

    if (server) {
      server->stop();
    }
  }

  bool is_running() {
    std::lock_guard lock {server_mutex};
    return server_instance != nullptr;
  }

  void observe_capture_ms(double milliseconds) {
    capture_last_ms.store(milliseconds, std::memory_order_relaxed);
    capture_sum_ms.fetch_add(milliseconds, std::memory_order_relaxed);
    capture_count.fetch_add(1, std::memory_order_relaxed);
  }

  void observe_encode_ms(double milliseconds) {
    encode_last_ms.store(milliseconds, std::memory_order_relaxed);
    encode_sum_ms.fetch_add(milliseconds, std::memory_order_relaxed);
    encode_count.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard lock {rate_mutex};

    const auto now = std::chrono::steady_clock::now();
    if (fps_window_frames == 0) {
      fps_window_start = now;
    }
    ++fps_window_frames;

    const auto elapsed = now - fps_window_start;
    if (elapsed >= 1s) {
      fps.store(static_cast<double>(fps_window_frames) / std::chrono::duration<double>(elapsed).count(), std::memory_order_relaxed);
      fps_window_start = now;
      fps_window_frames = 0;
    }
  }

  void set_fps(double value) {
    fps.store(value, std::memory_order_relaxed);
  }

  void set_bitrate_kbps(double value) {
    bitrate_kbps.store(value, std::memory_order_relaxed);
  }

  void add_frames_dropped(std::uint64_t count) {
    frames_dropped.fetch_add(count, std::memory_order_relaxed);
  }

  void add_idr(std::uint64_t count) {
    idr_total.fetch_add(count, std::memory_order_relaxed);
  }

  void add_bytes_sent(std::uint64_t bytes) {
    bytes_sent.fetch_add(bytes, std::memory_order_relaxed);

    std::lock_guard lock {rate_mutex};

    const auto now = std::chrono::steady_clock::now();
    if (bitrate_window_bytes == 0) {
      bitrate_window_start = now;
    }
    bitrate_window_bytes += bytes;

    const auto elapsed = now - bitrate_window_start;
    if (elapsed >= 1s) {
      const auto elapsed_seconds = std::chrono::duration<double>(elapsed).count();
      bitrate_kbps.store(static_cast<double>(bitrate_window_bytes) * 8.0 / 1000.0 / elapsed_seconds, std::memory_order_relaxed);
      bitrate_window_start = now;
      bitrate_window_bytes = 0;
    }
  }

  void record_event(std::string_view type, std::string_view detail) {
    {
      std::lock_guard lock {events_mutex};

      event_t event {
        .id = next_event_id++,
        .type = std::string {type},
        .detail = std::string {detail},
        .ts = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
      };

      if (events.size() >= max_events) {
        events.pop_front();
      }

      events.emplace_back(std::move(event));
    }

    events_cv.notify_all();
  }

  std::string render_metrics_text() {
    const auto capture_last = capture_last_ms.load(std::memory_order_relaxed);
    const auto capture_sum = capture_sum_ms.load(std::memory_order_relaxed);
    const auto capture_samples = capture_count.load(std::memory_order_relaxed);
    const auto encode_last = encode_last_ms.load(std::memory_order_relaxed);
    const auto encode_sum = encode_sum_ms.load(std::memory_order_relaxed);
    const auto encode_samples = encode_count.load(std::memory_order_relaxed);

    std::string text;
    text.reserve(2048);

    text += "# HELP sunshine_capture_ms Last observed capture backend call duration in milliseconds.\n";
    text += "# TYPE sunshine_capture_ms gauge\n";
    text += std::format("sunshine_capture_ms {:.3f}\n", capture_last);
    text += "# HELP sunshine_capture_ms_sum Total time spent in capture backend calls, in milliseconds.\n";
    text += "# TYPE sunshine_capture_ms_sum counter\n";
    text += std::format("sunshine_capture_ms_sum {:.3f}\n", capture_sum);
    text += "# HELP sunshine_capture_ms_count Number of measured capture backend calls.\n";
    text += "# TYPE sunshine_capture_ms_count counter\n";
    text += std::format("sunshine_capture_ms_count {}\n", capture_samples);

    text += "# HELP sunshine_encode_ms Last observed encoder call duration in milliseconds.\n";
    text += "# TYPE sunshine_encode_ms gauge\n";
    text += std::format("sunshine_encode_ms {:.3f}\n", encode_last);
    text += "# HELP sunshine_encode_ms_sum Total time spent in encoder calls, in milliseconds.\n";
    text += "# TYPE sunshine_encode_ms_sum counter\n";
    text += std::format("sunshine_encode_ms_sum {:.3f}\n", encode_sum);
    text += "# HELP sunshine_encode_ms_count Number of measured encoder calls.\n";
    text += "# TYPE sunshine_encode_ms_count counter\n";
    text += std::format("sunshine_encode_ms_count {}\n", encode_samples);

    text += "# HELP sunshine_fps Encoded frames per second over the last rate window.\n";
    text += "# TYPE sunshine_fps gauge\n";
    text += std::format("sunshine_fps {:.3f}\n", fps.load(std::memory_order_relaxed));

    text += "# HELP sunshine_bitrate_kbps Video bitrate in kilobits per second over the last rate window.\n";
    text += "# TYPE sunshine_bitrate_kbps gauge\n";
    text += std::format("sunshine_bitrate_kbps {:.3f}\n", bitrate_kbps.load(std::memory_order_relaxed));

    text += "# HELP sunshine_frames_dropped_total Frames encoded without a fresh capture (duplicated or skipped).\n";
    text += "# TYPE sunshine_frames_dropped_total counter\n";
    text += std::format("sunshine_frames_dropped_total {}\n", frames_dropped.load(std::memory_order_relaxed));

    text += "# HELP sunshine_idr_total IDR frames requested from the encoder.\n";
    text += "# TYPE sunshine_idr_total counter\n";
    text += std::format("sunshine_idr_total {}\n", idr_total.load(std::memory_order_relaxed));

    text += "# HELP sunshine_bytes_sent_total Video payload bytes handed to the network sender.\n";
    text += "# TYPE sunshine_bytes_sent_total counter\n";
    text += std::format("sunshine_bytes_sent_total {}\n", bytes_sent.load(std::memory_order_relaxed));

    text += "# HELP sunshine_input_blocks_total Client keyboard events swallowed by the input blocklist.\n";
    text += "# TYPE sunshine_input_blocks_total counter\n";
    text += std::format("sunshine_input_blocks_total {}\n", input::sp_input_blocks_total());

    return text;
  }

  std::vector<event_t> wait_events(std::uint64_t since, std::chrono::milliseconds timeout) {
    std::unique_lock lock {events_mutex};

    const auto epoch = shutdown_epoch.load(std::memory_order_relaxed);
    const auto has_new_event = [&] {
      return shutdown_epoch.load(std::memory_order_relaxed) != epoch ||
             (!events.empty() && events.back().id > since);
    };

    if (!has_new_event()) {
      events_cv.wait_for(lock, timeout, has_new_event);
    }

    std::vector<event_t> result;
    for (const auto &event : events) {
      if (event.id > since) {
        result.emplace_back(event);
      }
    }

    return result;
  }

  std::string events_json(std::uint64_t since, std::chrono::milliseconds timeout) {
    const auto new_events = wait_events(since, timeout);

    std::string json {"{\"events\":["};
    for (std::size_t index = 0; index < new_events.size(); ++index) {
      if (index > 0) {
        json += ',';
      }

      const auto &event = new_events[index];
      json += std::format(
        R"({{"id":{},"type":"{}","ts":{},"detail":"{}"}})",
        event.id,
        json_escape(event.type),
        event.ts,
        json_escape(event.detail)
      );
    }
    json += "]}";

    return json;
  }

  std::uint64_t latest_event_id() {
    std::lock_guard lock {events_mutex};
    return next_event_id - 1;
  }
}  // namespace sp_metrics
