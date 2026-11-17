/**
 * @file src/sp_metrics.h
 * @brief Gaspar optional local Prometheus metrics and session event endpoint.
 *
 * The endpoint is disabled by default (`config::sunshine.sp_metrics_port == 0`) and, when enabled, only binds to
 * `config::sunshine.bind_address` or to loopback when no bind address is configured.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Gaspar optional observability endpoint and hook recorders.
 */
namespace sp_metrics {
  /**
   * @brief One observable Sunshine session event.
   */
  struct event_t {
    std::uint64_t id;  ///< Monotonic event identifier used by the `since` query parameter.
    std::string type;  ///< Event type: `client_connected`, `client_disconnected`, or `app_exited`.
    std::string detail;  ///< Optional event detail, such as the app name for `app_exited`.
    std::int64_t ts;  ///< Unix timestamp in seconds.
  };

  /**
   * @brief Serve the metrics and event endpoints until stop() is called.
   *
   * Does nothing when `config::sunshine.sp_metrics_port` is zero. A bind failure is logged and does not stop
   * Sunshine.
   */
  void start();

  /**
   * @brief Stop the endpoint and wake any request blocked in a long poll.
   */
  void stop();

  /**
   * @brief Return whether the endpoint server currently owns a listening socket.
   *
   * @return True while the server is running.
   */
  bool is_running();

  /**
   * @brief Record one capture backend call duration.
   *
   * @param milliseconds Capture call duration in milliseconds.
   */
  void observe_capture_ms(double milliseconds);

  /**
   * @brief Record one encoder call duration and advance the derived frame-rate window.
   *
   * @param milliseconds Encoder call duration in milliseconds.
   */
  void observe_encode_ms(double milliseconds);

  /**
   * @brief Set the exported `sunshine_fps` gauge.
   *
   * @param fps Frames per second.
   */
  void set_fps(double fps);

  /**
   * @brief Set the exported `sunshine_bitrate_kbps` gauge.
   *
   * @param kbps Bitrate in kilobits per second.
   */
  void set_bitrate_kbps(double kbps);

  /**
   * @brief Add to the exported `sunshine_frames_dropped_total` counter.
   *
   * @param count Number of frames to add.
   */
  void add_frames_dropped(std::uint64_t count);

  /**
   * @brief Add to the exported `sunshine_idr_total` counter.
   *
   * @param count Number of IDR frames to add.
   */
  void add_idr(std::uint64_t count);

  /**
   * @brief Add transmitted video bytes and advance the derived bitrate window.
   *
   * @param bytes Number of bytes written to the video socket.
   */
  void add_bytes_sent(std::uint64_t bytes);

  /**
   * @brief Append one session event to the bounded in-memory ring.
   *
   * @param type Event type.
   * @param detail Optional detail string.
   */
  void record_event(std::string_view type, std::string_view detail = {});

  /**
   * @brief Render the Prometheus 0.0.4 text payload.
   *
   * @return Metrics text including HELP and TYPE metadata for every series.
   */
  std::string render_metrics_text();

  /**
   * @brief Wait for events newer than an identifier.
   *
   * @param since Return only events with an identifier greater than this value.
   * @param timeout Maximum time to wait when no newer event exists yet.
   * @return New events, which may be empty when the timeout expires.
   */
  std::vector<event_t> wait_events(std::uint64_t since, std::chrono::milliseconds timeout);

  /**
   * @brief Wait for events and serialize them as the `/sp/events` JSON payload.
   *
   * @param since Return only events with an identifier greater than this value.
   * @param timeout Maximum time to wait when no newer event exists yet.
   * @return JSON object with an `events` array.
   */
  std::string events_json(std::uint64_t since, std::chrono::milliseconds timeout);

  /**
   * @brief Return the identifier of the most recently recorded event.
   *
   * @return Latest event identifier, or zero when no event has been recorded.
   */
  std::uint64_t latest_event_id();
}  // namespace sp_metrics
