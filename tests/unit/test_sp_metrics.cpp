/**
 * @file tests/unit/test_sp_metrics.cpp
 * @brief Tests for the Gaspar optional metrics and session event endpoint.
 *
 * The endpoint is disabled by default. These tests never open a listening socket: they exercise configuration
 * parsing, the Prometheus renderer, and the event ring/long-poll waiter directly.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cstdint>
#include <format>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// local includes
#include <src/config.h>
#include <src/sp_metrics.h>

using namespace std::chrono_literals;

namespace {
  /**
   * @brief Fixture that restores Sunshine configuration after each test.
   */
  class SpMetricsTest: public testing::Test {
  protected:
    /**
     * @brief Save the active configuration and disable the endpoint.
     */
    void SetUp() override {
      original_sunshine_ = config::sunshine;
      config::sunshine.sp_metrics_port = 0;
    }

    /**
     * @brief Restore the active configuration.
     */
    void TearDown() override {
      config::sunshine = std::move(original_sunshine_);
    }

  private:
    config::sunshine_t original_sunshine_ {};  ///< Configuration restored after each test.
  };

  /**
   * @brief Read a single Prometheus sample value from rendered text.
   *
   * @param text Rendered metrics payload.
   * @param name Exact series name.
   * @return Sample value, or an empty string when the series is absent.
   */
  std::string metric_value(std::string_view text, std::string_view name) {
    const auto needle = std::string {name} + ' ';
    const auto line_start = text.find(needle);
    if (line_start == std::string_view::npos) {
      return {};
    }

    const auto value_start = line_start + needle.size();
    const auto value_end = text.find('\n', value_start);
    return std::string {text.substr(value_start, value_end - value_start)};
  }

  /**
   * @brief Assert that a metric family has HELP and TYPE metadata.
   *
   * @param text Rendered metrics payload.
   * @param name Metric family name.
   */
  void expect_metric_metadata(std::string_view text, std::string_view name) {
    EXPECT_NE(std::string_view::npos, text.find(std::format("# HELP {} ", name))) << name;
    EXPECT_NE(std::string_view::npos, text.find(std::format("# TYPE {} ", name))) << name;
  }
}  // namespace

TEST_F(SpMetricsTest, DefaultPortIsZeroAndStartIsNoOp) {
  EXPECT_EQ(0, config::sunshine_t {}.sp_metrics_port);

  sp_metrics::start();

  EXPECT_FALSE(sp_metrics::is_running());
}

TEST_F(SpMetricsTest, ConfigAcceptsPositivePort) {
  config::apply_config_for_test("sp_metrics_port = 9587\n");

  EXPECT_EQ(9587, config::sunshine.sp_metrics_port);
}

TEST_F(SpMetricsTest, ConfigRejectsOutOfRangePort) {
  config::sunshine.sp_metrics_port = 0;

  config::apply_config_for_test("sp_metrics_port = -1\n");
  EXPECT_EQ(0, config::sunshine.sp_metrics_port);

  config::apply_config_for_test("sp_metrics_port = 70000\n");
  EXPECT_EQ(0, config::sunshine.sp_metrics_port);
}

TEST_F(SpMetricsTest, RenderIncludesRequiredSeriesAndSamples) {
  const auto before = sp_metrics::render_metrics_text();
  const auto before_idr = metric_value(before, "sunshine_idr_total");
  const auto before_dropped = metric_value(before, "sunshine_frames_dropped_total");
  const auto before_capture_count = metric_value(before, "sunshine_capture_ms_count");
  const auto before_encode_count = metric_value(before, "sunshine_encode_ms_count");

  ASSERT_FALSE(before_idr.empty());
  ASSERT_FALSE(before_dropped.empty());
  ASSERT_FALSE(before_capture_count.empty());
  ASSERT_FALSE(before_encode_count.empty());

  sp_metrics::add_idr(3);
  sp_metrics::add_frames_dropped(2);
  sp_metrics::observe_capture_ms(4.5);
  sp_metrics::observe_encode_ms(6.5);
  sp_metrics::set_fps(59.940);
  sp_metrics::set_bitrate_kbps(12500.0);

  const auto after = sp_metrics::render_metrics_text();

  for (const auto *name : {
         "sunshine_capture_ms",
         "sunshine_capture_ms_sum",
         "sunshine_capture_ms_count",
         "sunshine_encode_ms",
         "sunshine_encode_ms_sum",
         "sunshine_encode_ms_count",
         "sunshine_fps",
         "sunshine_bitrate_kbps",
         "sunshine_frames_dropped_total",
         "sunshine_idr_total",
         "sunshine_bytes_sent_total",
         "sunshine_input_blocks_total",
       }) {
    expect_metric_metadata(after, name);
  }

  EXPECT_EQ("4.500", metric_value(after, "sunshine_capture_ms"));
  EXPECT_EQ("6.500", metric_value(after, "sunshine_encode_ms"));
  EXPECT_EQ("59.940", metric_value(after, "sunshine_fps"));
  EXPECT_EQ("12500.000", metric_value(after, "sunshine_bitrate_kbps"));

  EXPECT_EQ(std::stoull(before_idr) + 3, std::stoull(metric_value(after, "sunshine_idr_total")));
  EXPECT_EQ(std::stoull(before_dropped) + 2, std::stoull(metric_value(after, "sunshine_frames_dropped_total")));
  EXPECT_EQ(std::stoull(before_capture_count) + 1, std::stoull(metric_value(after, "sunshine_capture_ms_count")));
  EXPECT_EQ(std::stoull(before_encode_count) + 1, std::stoull(metric_value(after, "sunshine_encode_ms_count")));

  EXPECT_FALSE(metric_value(after, "sunshine_input_blocks_total").empty());
}

TEST_F(SpMetricsTest, EventsSinceFiltersOlderEvents) {
  const auto baseline = sp_metrics::latest_event_id();

  sp_metrics::record_event("client_connected", "first-test-event");
  const auto first_id = sp_metrics::latest_event_id();
  ASSERT_GT(first_id, baseline);

  sp_metrics::record_event("app_exited", "second-test-event");
  const auto second_id = sp_metrics::latest_event_id();
  ASSERT_EQ(first_id + 1, second_id);

  const auto all = sp_metrics::events_json(baseline, 0ms);
  EXPECT_NE(std::string::npos, all.find("first-test-event"));
  EXPECT_NE(std::string::npos, all.find("second-test-event"));

  const auto newer = sp_metrics::events_json(first_id, 0ms);
  EXPECT_EQ(std::string::npos, newer.find("first-test-event"));
  EXPECT_NE(std::string::npos, newer.find("second-test-event"));
  EXPECT_NE(std::string::npos, newer.find(std::format("\"id\":{}", second_id)));
  EXPECT_EQ(std::string::npos, newer.find(std::format("\"id\":{}", first_id)));
}

TEST_F(SpMetricsTest, LongPollWakesWhenEventRecorded) {
  const auto since = sp_metrics::latest_event_id();

  auto waiter = std::async(std::launch::async, [since] {
    return sp_metrics::events_json(since, 5s);
  });

  std::this_thread::sleep_for(50ms);
  sp_metrics::record_event("client_disconnected", "long-poll-test-event");

  ASSERT_EQ(std::future_status::ready, waiter.wait_for(2s));
  EXPECT_NE(std::string::npos, waiter.get().find("long-poll-test-event"));
}

TEST_F(SpMetricsTest, LongPollReturnsEmptyListOnTimeout) {
  const auto since = sp_metrics::latest_event_id();
  const auto start = std::chrono::steady_clock::now();

  const auto payload = sp_metrics::events_json(since, 50ms);

  EXPECT_EQ(R"({"events":[]})", payload);
  EXPECT_TRUE(std::chrono::steady_clock::now() - start >= 40ms);
}
