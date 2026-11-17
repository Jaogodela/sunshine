/**
 * @file tests/unit/test_sp_xtest.cpp
 * @brief Tests for the Gaspar X11 XTest keyboard/mouse backend.
 *
 * The tests install a recording event sink, so no X server, XTest library call, or
 * `/dev/uinput` device is required. The default `uinput` path is checked to prove it
 * never reaches the XTest sink.
 */

// test includes
#include "../tests_common.h"

#if defined(__linux__)

// standard includes
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

// local includes
#include "src/config.h"
#include "src/platform/linux/input/xtest.h"
#include "src/platform/virtualhid_input.h"

namespace {

  /**
   * @brief Recorded key event.
   */
  struct key_event_t {
    std::uint32_t keysym;  ///< X11 keysym.
    bool pressed;  ///< Whether the key was pressed.
  };

  /**
   * @brief Recorded pointer motion.
   */
  struct motion_t {
    int x;  ///< X coordinate or delta.
    int y;  ///< Y coordinate or delta.
  };

  /**
   * @brief Recorded mouse button event.
   */
  struct button_event_t {
    unsigned int button;  ///< X11 button number.
    bool pressed;  ///< Whether the button was pressed.
  };

  /**
   * @brief Events recorded by the fake XTest sink.
   */
  struct recorder_t {
    std::vector<key_event_t> keys;  ///< Key events.
    std::vector<motion_t> relative;  ///< Relative motion events.
    std::vector<motion_t> absolute;  ///< Absolute motion events.
    std::vector<button_event_t> buttons;  ///< Mouse button events.
    int flushes = 0;  ///< Number of flush calls.
  };

  recorder_t g_recorder;  ///< Shared recorder used by the function-pointer sink.

  /**
   * @brief Record a key event.
   *
   * @param keysym X11 keysym.
   * @param pressed Whether the key was pressed.
   */
  void record_key(std::uint32_t keysym, bool pressed) {
    g_recorder.keys.push_back({keysym, pressed});
  }

  /**
   * @brief Record relative motion.
   *
   * @param x Horizontal delta.
   * @param y Vertical delta.
   */
  void record_relative(int x, int y) {
    g_recorder.relative.push_back({x, y});
  }

  /**
   * @brief Record absolute motion.
   *
   * @param x Root X coordinate.
   * @param y Root Y coordinate.
   */
  void record_absolute(int x, int y) {
    g_recorder.absolute.push_back({x, y});
  }

  /**
   * @brief Record a mouse button event.
   *
   * @param button X11 button number.
   * @param pressed Whether the button was pressed.
   */
  void record_button(unsigned int button, bool pressed) {
    g_recorder.buttons.push_back({button, pressed});
  }

  /**
   * @brief Record a flush.
   */
  void record_flush() {
    ++g_recorder.flushes;
  }

  const platf::sp_xtest::ops_t g_recording_ops {
    record_key,
    record_relative,
    record_absolute,
    record_button,
    record_flush,
  };

  /**
   * @brief Fixture that installs the recording sink and restores configuration.
   */
  class SpXtestTest: public ::testing::Test {
  protected:
    /**
     * @brief Save configuration and install the recording sink.
     */
    void SetUp() override {
      original_input_ = config::input;
      original_file_apps_ = config::stream.file_apps;
      g_recorder = {};
      config::input.sp_kbm_backend = "uinput";
      platf::sp_xtest::set_ops_for_testing(&g_recording_ops);
      test_apps_ = std::filesystem::temp_directory_path() / "sp_xtest_apps.json";
      std::ofstream(test_apps_) << "[]\n";
      config::stream.file_apps = test_apps_.string();
    }

    /**
     * @brief Remove the recording sink and restore configuration.
     */
    void TearDown() override {
      platf::sp_xtest::set_ops_for_testing(nullptr);
      config::input = std::move(original_input_);
      config::stream.file_apps = original_file_apps_;
      std::error_code ec;
      std::filesystem::remove(test_apps_, ec);
    }

  private:
    config::input_t original_input_;  ///< Configuration restored after each test.
    std::string original_file_apps_;  ///< Default apps.json path restored after each test.
    std::filesystem::path test_apps_;  ///< Temporary apps.json so apply_config can run in tests.
  };

}  // namespace

TEST_F(SpXtestTest, DefaultBackendIsUinput) {
  EXPECT_EQ(config::input_t {}.sp_kbm_backend, "uinput");
}

TEST_F(SpXtestTest, ConfigParsesAllowedBackends) {
  config::input.sp_kbm_backend = "uinput";

  config::apply_config_for_test("sp_kbm_backend = xtest\n");
  EXPECT_EQ(config::input.sp_kbm_backend, "xtest");

  config::apply_config_for_test("sp_kbm_backend = wlr_virtual\n");
  EXPECT_EQ(config::input.sp_kbm_backend, "wlr_virtual");

  config::apply_config_for_test("sp_kbm_backend = uinput\n");
  EXPECT_EQ(config::input.sp_kbm_backend, "uinput");

  config::apply_config_for_test("sp_kbm_backend = not_a_backend\n");
  EXPECT_EQ(config::input.sp_kbm_backend, "uinput");
}

TEST_F(SpXtestTest, MapsVirtualKeysToUsKeysyms) {
  // VK_A -> XK_a
  EXPECT_EQ(platf::sp_xtest::sp_vk_to_keysym(0x41).value_or(0), 0x0061u);
  // VK_0 -> XK_0
  EXPECT_EQ(platf::sp_xtest::sp_vk_to_keysym(0x30).value_or(0), 0x0030u);
  // VK_RETURN -> XK_Return
  EXPECT_EQ(platf::sp_xtest::sp_vk_to_keysym(0x0D).value_or(0), 0xFF0Du);
  // VK_LWIN -> XK_Super_L
  EXPECT_EQ(platf::sp_xtest::sp_vk_to_keysym(0x5B).value_or(0), 0xFFEBu);
  // VK_F4 -> XK_F4
  EXPECT_EQ(platf::sp_xtest::sp_vk_to_keysym(0x73).value_or(0), 0xFFC1u);
  // VK_OEM_COMMA -> XK_comma
  EXPECT_EQ(platf::sp_xtest::sp_vk_to_keysym(0xBC).value_or(0), 0x002Cu);
  // VK_LSHIFT -> XK_Shift_L
  EXPECT_EQ(platf::sp_xtest::sp_vk_to_keysym(0xA0).value_or(0), 0xFFE1u);

  EXPECT_FALSE(platf::sp_xtest::sp_vk_to_keysym(0xFF).has_value());
}

TEST_F(SpXtestTest, ClampsAbsoluteCoordinatesToTouchPort) {
  const platf::touch_port_t touch_port {0, 0, 1920, 1080, 1920, 1080};

  const auto inside = platf::sp_xtest::sp_abs_coordinates(touch_port, 100.4F, 200.6F);
  EXPECT_EQ(inside.first, 100);
  EXPECT_EQ(inside.second, 201);

  const auto high = platf::sp_xtest::sp_abs_coordinates(touch_port, 5000.0F, 5000.0F);
  EXPECT_EQ(high.first, 1919);
  EXPECT_EQ(high.second, 1079);

  const auto low = platf::sp_xtest::sp_abs_coordinates(touch_port, -10.0F, -10.0F);
  EXPECT_EQ(low.first, 0);
  EXPECT_EQ(low.second, 0);
}

TEST_F(SpXtestTest, UinputBackendDoesNotUseXtestSink) {
  config::input.sp_kbm_backend = "uinput";

  auto input = platf::input();
  ASSERT_TRUE(input);
  auto &context = platf::virtualhid::get_input_context(input);
  context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};

  platf::move_mouse(input, 3, 4);
  platf::abs_mouse(input, platf::touch_port_t {0, 0, 1920, 1080, 1920, 1080}, 10.0F, 20.0F);
  platf::button_mouse(input, BUTTON_LEFT, false);
  platf::scroll(input, 120);
  platf::hscroll(input, 120);
  platf::keyboard_update(input, 0x41, false, 0);
  platf::unicode(input, "A", 1);

  EXPECT_TRUE(g_recorder.keys.empty());
  EXPECT_TRUE(g_recorder.relative.empty());
  EXPECT_TRUE(g_recorder.absolute.empty());
  EXPECT_TRUE(g_recorder.buttons.empty());
}

TEST_F(SpXtestTest, WlrVirtualBackendFailsClosed) {
  config::input.sp_kbm_backend = "wlr_virtual";

  platf::virtualhid::input_context_t context {lvh::BackendKind::fake};
  EXPECT_EQ(context.keyboard, nullptr);
  EXPECT_EQ(context.mouse, nullptr);

  auto input = platf::input();
  ASSERT_TRUE(input);
  platf::keyboard_update(input, 0x41, false, 0);
  platf::move_mouse(input, 1, 1);

  EXPECT_TRUE(g_recorder.keys.empty());
  EXPECT_TRUE(g_recorder.relative.empty());
}

#ifdef SUNSHINE_BUILD_X11

TEST_F(SpXtestTest, XtestBackendRoutesKeyboardAndMouse) {
  config::input.sp_kbm_backend = "xtest";

  auto input = platf::input();
  ASSERT_TRUE(input);

  platf::keyboard_update(input, 0x41, false, 0);
  platf::keyboard_update(input, 0x41, true, 0);
  platf::keyboard_update(input, 0x0D, false, 0);
  platf::keyboard_update(input, 0x5B, false, 0);
  platf::keyboard_update(input, 0x73, false, 0);

  ASSERT_EQ(g_recorder.keys.size(), 5U);
  EXPECT_EQ(g_recorder.keys[0].keysym, 0x0061u);
  EXPECT_TRUE(g_recorder.keys[0].pressed);
  EXPECT_EQ(g_recorder.keys[1].keysym, 0x0061u);
  EXPECT_FALSE(g_recorder.keys[1].pressed);
  EXPECT_EQ(g_recorder.keys[2].keysym, 0xFF0Du);
  EXPECT_EQ(g_recorder.keys[3].keysym, 0xFFEBu);
  EXPECT_EQ(g_recorder.keys[4].keysym, 0xFFC1u);

  platf::move_mouse(input, 7, -3);
  ASSERT_EQ(g_recorder.relative.size(), 1U);
  EXPECT_EQ(g_recorder.relative[0].x, 7);
  EXPECT_EQ(g_recorder.relative[0].y, -3);

  const platf::touch_port_t touch_port {0, 0, 1920, 1080, 1920, 1080};
  platf::abs_mouse(input, touch_port, 100.4F, 200.6F);
  ASSERT_EQ(g_recorder.absolute.size(), 1U);
  EXPECT_EQ(g_recorder.absolute[0].x, 100);
  EXPECT_EQ(g_recorder.absolute[0].y, 201);

  platf::button_mouse(input, BUTTON_LEFT, false);
  platf::button_mouse(input, BUTTON_RIGHT, false);
  platf::button_mouse(input, BUTTON_MIDDLE, true);

  ASSERT_EQ(g_recorder.buttons.size(), 3U);
  EXPECT_EQ(g_recorder.buttons[0].button, 1U);
  EXPECT_TRUE(g_recorder.buttons[0].pressed);
  EXPECT_EQ(g_recorder.buttons[1].button, 3U);
  EXPECT_TRUE(g_recorder.buttons[1].pressed);
  EXPECT_EQ(g_recorder.buttons[2].button, 2U);
  EXPECT_FALSE(g_recorder.buttons[2].pressed);

  EXPECT_GT(g_recorder.flushes, 0);
}

TEST_F(SpXtestTest, XtestBackendEmitsScrollTicks) {
  config::input.sp_kbm_backend = "xtest";

  auto input = platf::input();
  ASSERT_TRUE(input);

  platf::scroll(input, 120);
  platf::scroll(input, 240);
  platf::scroll(input, -120);
  platf::scroll(input, 60);  // Less than one notch is ignored.

  platf::hscroll(input, 120);
  platf::hscroll(input, -240);
  platf::hscroll(input, -60);  // Less than one notch is ignored.

  ASSERT_EQ(g_recorder.buttons.size(), 14U);
  for (std::size_t i = 0; i + 1 < g_recorder.buttons.size(); i += 2) {
    EXPECT_EQ(g_recorder.buttons[i].button, g_recorder.buttons[i + 1].button);
    EXPECT_TRUE(g_recorder.buttons[i].pressed);
    EXPECT_FALSE(g_recorder.buttons[i + 1].pressed);
  }

  EXPECT_EQ(g_recorder.buttons[0].button, 4U);  // Vertical up
  EXPECT_EQ(g_recorder.buttons[2].button, 4U);
  EXPECT_EQ(g_recorder.buttons[4].button, 4U);
  EXPECT_EQ(g_recorder.buttons[6].button, 5U);  // Vertical down
  EXPECT_EQ(g_recorder.buttons[8].button, 7U);  // Horizontal right
  EXPECT_EQ(g_recorder.buttons[10].button, 6U);  // Horizontal left
  EXPECT_EQ(g_recorder.buttons[12].button, 6U);
}

TEST_F(SpXtestTest, XtestBackendDropsUnicodeWithoutComposeInjection) {
  config::input.sp_kbm_backend = "xtest";

  auto input = platf::input();
  ASSERT_TRUE(input);

  platf::unicode(input, "A", 1);

  EXPECT_TRUE(g_recorder.keys.empty());
  EXPECT_TRUE(g_recorder.buttons.empty());
}

TEST_F(SpXtestTest, XtestBackendDoesNotCreateLibvirtualhidKbmDevices) {
  config::input.sp_kbm_backend = "xtest";

  platf::virtualhid::input_context_t context {lvh::BackendKind::fake};
  EXPECT_EQ(context.keyboard, nullptr);
  EXPECT_EQ(context.mouse, nullptr);

  platf::virtualhid::client_context_t client {context};
  EXPECT_EQ(client.touch, nullptr);
  EXPECT_EQ(client.pen, nullptr);
}

#endif

#endif
