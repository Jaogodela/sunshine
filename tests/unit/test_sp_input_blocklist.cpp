/**
 * @file tests/unit/test_sp_input_blocklist.cpp
 * @brief Tests for the Gaspar keyboard input blocklist in src/input.*.
 *
 * The blocklist is opt-in. With an empty list and function-key blocking disabled, keyboard and
 * UTF-8 text passthrough must behave exactly as upstream. When configured, the matching key-down
 * is swallowed before the platform backend, key-repeat scheduling, or the shortcut handler sees it.
 * Every test installs a recorder so nothing is typed into the machine running the suite.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include "src/config.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"

namespace {
  using namespace std::chrono_literals;

  // Windows virtual-key codes as they arrive from the client.
  constexpr std::uint16_t VKEY_BACK = 0x08;
  constexpr std::uint16_t VKEY_TAB = 0x09;
  constexpr std::uint16_t VKEY_PAUSE = 0x13;
  constexpr std::uint16_t VKEY_ESCAPE = 0x1B;
  constexpr std::uint16_t VKEY_SPACE = 0x20;
  constexpr std::uint16_t VKEY_PRINTSCREEN = 0x2C;
  constexpr std::uint16_t VKEY_DELETE = 0x2E;
  constexpr std::uint16_t VKEY_A = 0x41;
  constexpr std::uint16_t VKEY_C = 0x43;
  constexpr std::uint16_t VKEY_T = 0x54;
  constexpr std::uint16_t VKEY_LWIN = 0x5B;
  constexpr std::uint16_t VKEY_RWIN = 0x5C;
  constexpr std::uint16_t VKEY_F1 = 0x70;
  constexpr std::uint16_t VKEY_F2 = 0x71;
  constexpr std::uint16_t VKEY_F4 = 0x73;
  constexpr std::uint16_t VKEY_F12 = 0x7B;
  constexpr std::uint16_t VKEY_SCROLLLOCK = 0x91;
  constexpr std::uint16_t VKEY_LMENU = 0xA4;

  /**
   * @brief The reference blocklist documented in the Gaspar plan, section 7.3.
   */
  constexpr std::string_view blocklist_7_3 {
    "LEFTMETA,RIGHTMETA,SYSRQ,ALT+F4,ALT+TAB,ALT+SHIFT+TAB,ALT+ESC,CTRL+ESC,"
    "CTRL+ALT+*,ALT+F2,ALT+SPACE,SCROLLLOCK,PAUSE"
  };

  /**
   * @brief One client key-down that a configured blocklist must swallow.
   */
  struct blocked_case_t {
    std::uint16_t key_code;  ///< Windows virtual-key code sent by the client.
    unsigned modifiers;  ///< Client modifier bitmask carried by the packet.
    const char *label;  ///< Combination name used in assertion traces.
  };

  /**
   * @brief Fixture that records keyboard and text output instead of delivering it to the host.
   */
  class SpInputBlocklistTest: public ::testing::Test {
  protected:
    /**
     * @brief Install a fake platform backend and fresh recorders for one test.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.keyboard = true;
      config::input.mouse = false;
      config::input.keybindings.clear();
      config::input.key_rightalt_to_key_win = false;
      config::input.key_repeat_delay = 0ms;
      config::input.sp_input_blocklist.clear();
      config::input.sp_input_block_function_keys = false;

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      input::testing::set_platform_input(std::move(platform_input));

      input::testing::reset_keyboard_state();
      input::testing::set_keyboard_sink([this](const input::testing::keyboard_event_t &event) {
        events_.push_back(event);
      });
      input::testing::set_unicode_sink([this](std::string_view text) {
        unicode_.emplace_back(text);
      });

      static int session = 0;
      stream_ = input::alloc(std::make_shared<safe::mail_raw_t>(), std::format("sp-blocklist-test-{}", ++session));
      ASSERT_NE(stream_, nullptr);
    }

    /**
     * @brief Stop recording, drop tracked key state, and restore configuration.
     */
    void TearDown() override {
      input::testing::set_unicode_sink({});
      input::testing::set_keyboard_sink({});
      input::testing::reset_keyboard_state();
      stream_.reset();
      input::testing::set_platform_input({});
      config::input = std::move(original_input_);
    }

    /**
     * @brief Set the blocklist from a string view.
     *
     * @param list Blocklist to install.
     */
    void set_blocklist(std::string_view list) {
      config::input.sp_input_blocklist = std::string {list};
    }

    /**
     * @brief Deliver a key press packet.
     *
     * @param key_code Windows virtual-key code sent by the client.
     * @param modifiers Modifier bitmask the client reports as held.
     */
    void press(std::uint16_t key_code, unsigned modifiers = 0) {
      input::testing::send_keyboard_packet(stream_, key_code, static_cast<std::uint8_t>(modifiers), 0, false);
    }

    /**
     * @brief Deliver a key release packet.
     *
     * @param key_code Windows virtual-key code sent by the client.
     * @param modifiers Modifier bitmask the client reports as held.
     */
    void release(std::uint16_t key_code, unsigned modifiers = 0) {
      input::testing::send_keyboard_packet(stream_, key_code, static_cast<std::uint8_t>(modifiers), 0, true);
    }

    /**
     * @brief Deliver one UTF-8 text packet.
     *
     * @param text Text carried by the packet.
     */
    void type_text(std::string_view text) {
      input::testing::send_unicode_text(stream_, text);
    }

    /**
     * @brief Take the recorded keyboard events without converting them.
     *
     * @return Events recorded since the last call.
     */
    std::vector<input::testing::keyboard_event_t> taken_events() {
      return std::exchange(events_, {});
    }

    /**
     * @brief Take the recorded text emissions.
     *
     * @return Text emitted since the last call.
     */
    std::vector<std::string> taken_text() {
      return std::exchange(unicode_, {});
    }

    /**
     * @brief Check whether a key event of a given direction was recorded.
     *
     * @param events Recorded events.
     * @param key_code Windows virtual-key code to look for.
     * @param release Whether to look for a release instead of a press.
     * @return True when the event was recorded.
     */
    static bool saw_key(
      const std::vector<input::testing::keyboard_event_t> &events,
      std::uint16_t key_code,
      bool release
    ) {
      return std::ranges::any_of(events, [key_code, release](const auto &event) {
        return event.key_code == key_code && event.release == release;
      });
    }

  private:
    std::shared_ptr<input::input_t> stream_;  ///< Stream input state under test.
    std::vector<input::testing::keyboard_event_t> events_;  ///< Keyboard output recorded for the current test.
    std::vector<std::string> unicode_;  ///< UTF-8 text output recorded for the current test.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };
}  // namespace

TEST_F(SpInputBlocklistTest, EmptyListBlocksNothing) {
  press(VKEY_A);
  release(VKEY_A);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_A, false));

  press(VKEY_F4, MODIFIER_ALT);
  release(VKEY_F4, MODIFIER_ALT);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_F4, false));

  press(VKEY_LWIN);
  release(VKEY_LWIN);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_LWIN, false));
}

TEST_F(SpInputBlocklistTest, SkipsInvalidTokensAndKeepsTheRest) {
  // Whitespace around commas and lowercase token names are accepted.
  set_blocklist(" BOGUS ,  alt+f4 , CTRL+NOSUCHKEY ");

  press(VKEY_A);
  release(VKEY_A);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_A, false));

  press(VKEY_F4, MODIFIER_ALT);
  release(VKEY_F4, MODIFIER_ALT);
  EXPECT_TRUE(taken_events().empty());
}

TEST_F(SpInputBlocklistTest, BlocksEveryDocumentedCombination) {
  set_blocklist(blocklist_7_3);

  const std::vector<blocked_case_t> cases {
    {VKEY_LWIN, 0, "LEFTMETA"},
    {VKEY_RWIN, 0, "RIGHTMETA"},
    {VKEY_PRINTSCREEN, 0, "SYSRQ"},
    {VKEY_F4, MODIFIER_ALT, "ALT+F4"},
    {VKEY_TAB, MODIFIER_ALT, "ALT+TAB"},
    {VKEY_TAB, MODIFIER_ALT | MODIFIER_SHIFT, "ALT+SHIFT+TAB"},
    {VKEY_ESCAPE, MODIFIER_ALT, "ALT+ESC"},
    {VKEY_ESCAPE, MODIFIER_CTRL, "CTRL+ESC"},
    {VKEY_DELETE, MODIFIER_CTRL | MODIFIER_ALT, "CTRL+ALT+DEL"},
    {VKEY_BACK, MODIFIER_CTRL | MODIFIER_ALT, "CTRL+ALT+Backspace"},
    {VKEY_F1, MODIFIER_CTRL | MODIFIER_ALT, "CTRL+ALT+F1"},
    {VKEY_T, MODIFIER_CTRL | MODIFIER_ALT, "CTRL+ALT+T"},
    {VKEY_F2, MODIFIER_ALT, "ALT+F2"},
    {VKEY_SPACE, MODIFIER_ALT, "ALT+SPACE"},
    {VKEY_SCROLLLOCK, 0, "SCROLLLOCK"},
    {VKEY_PAUSE, 0, "PAUSE"},
  };

  for (const auto &blocked : cases) {
    SCOPED_TRACE(blocked.label);
    press(blocked.key_code, blocked.modifiers);
    EXPECT_TRUE(taken_events().empty());

    release(blocked.key_code, blocked.modifiers);
    EXPECT_TRUE(taken_events().empty());
  }
}

TEST_F(SpInputBlocklistTest, HarmlessKeysStillPass) {
  set_blocklist(blocklist_7_3);

  const std::vector<blocked_case_t> allowed {
    {VKEY_A, 0, "A"},
    {VKEY_A, MODIFIER_SHIFT, "Shift+A"},
    {VKEY_F4, 0, "F4"},
    {VKEY_TAB, 0, "Tab"},
    {VKEY_ESCAPE, 0, "Esc"},
    {VKEY_C, MODIFIER_CTRL, "Ctrl+C"},
    {VKEY_A, MODIFIER_ALT, "Alt+A"},
  };

  for (const auto &key : allowed) {
    SCOPED_TRACE(key.label);
    press(key.key_code, key.modifiers);
    EXPECT_TRUE(saw_key(taken_events(), key.key_code, false));

    release(key.key_code, key.modifiers);
    EXPECT_TRUE(saw_key(taken_events(), key.key_code, true));
  }
}

TEST_F(SpInputBlocklistTest, FunctionKeyBlockingIsOptIn) {
  // Plain F1 and F12 pass while the flag is off.
  press(VKEY_F1);
  release(VKEY_F1);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_F1, false));

  press(VKEY_F12);
  release(VKEY_F12);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_F12, false));

  config::input.sp_input_block_function_keys = true;

  press(VKEY_F1);
  EXPECT_TRUE(taken_events().empty());
  release(VKEY_F1);
  EXPECT_TRUE(taken_events().empty());

  press(VKEY_F12);
  EXPECT_TRUE(taken_events().empty());
  release(VKEY_F12);
  EXPECT_TRUE(taken_events().empty());
}

TEST_F(SpInputBlocklistTest, DefaultsMatchUpstreamBehavior) {
  // Empty list and disabled function-key blocking: nothing may be swallowed.
  press(VKEY_A);
  release(VKEY_A);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_A, false));

  press(VKEY_LMENU, MODIFIER_ALT);
  press(VKEY_F4, MODIFIER_ALT);
  release(VKEY_F4, MODIFIER_ALT);
  release(VKEY_LMENU, 0);
  auto events = taken_events();
  EXPECT_TRUE(saw_key(events, VKEY_F4, false));
  EXPECT_TRUE(saw_key(events, VKEY_F4, true));

  press(VKEY_LWIN);
  release(VKEY_LWIN);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_LWIN, false));
}

TEST_F(SpInputBlocklistTest, BlockedKeysIncrementTheCounter) {
  set_blocklist("ALT+F4");

  const auto before = input::sp_input_blocks_total();

  press(VKEY_LMENU, MODIFIER_ALT);
  EXPECT_EQ(input::sp_input_blocks_total(), before);
  taken_events();

  press(VKEY_F4, MODIFIER_ALT);
  EXPECT_EQ(input::sp_input_blocks_total(), before + 1);
  release(VKEY_F4, MODIFIER_ALT);
  EXPECT_EQ(input::sp_input_blocks_total(), before + 1);
  release(VKEY_LMENU, 0);
  taken_events();

  press(VKEY_A);
  release(VKEY_A);
  EXPECT_EQ(input::sp_input_blocks_total(), before + 1);
  EXPECT_TRUE(saw_key(taken_events(), VKEY_A, false));
}

TEST_F(SpInputBlocklistTest, ABlockedComboLeavesNoStuckKey) {
  set_blocklist(blocklist_7_3);

  press(VKEY_LMENU, MODIFIER_ALT);
  press(VKEY_F4, MODIFIER_ALT);
  release(VKEY_F4, MODIFIER_ALT);
  release(VKEY_LMENU, 0);

  press(VKEY_A);
  release(VKEY_A);

  const auto events = taken_events();
  EXPECT_FALSE(saw_key(events, VKEY_F4, false));
  EXPECT_FALSE(saw_key(events, VKEY_F4, true));
  EXPECT_TRUE(saw_key(events, VKEY_A, false));
  EXPECT_TRUE(saw_key(events, VKEY_A, true));
}

TEST_F(SpInputBlocklistTest, UnicodeTextCannotBypassTheBlocklist) {
  // Without any rule, text passthrough is unchanged, including control characters.
  type_text("a\x1b");
  EXPECT_EQ(taken_text(), (std::vector<std::string> {"a\x1b"}));

  // A configured combination drops the whole text event, so ESC cannot be typed instead of
  // sending a key packet. Dropping the event is intentionally broader than filtering C0/C1.
  set_blocklist(blocklist_7_3);
  type_text("a\x1b");
  EXPECT_TRUE(taken_text().empty());

  // Function-key blocking alone also activates the drop.
  set_blocklist("");
  config::input.sp_input_block_function_keys = true;
  type_text("\x1b");
  EXPECT_TRUE(taken_text().empty());

  // Back to the upstream default: text is delivered again.
  config::input.sp_input_block_function_keys = false;
  type_text("hello");
  EXPECT_EQ(taken_text(), (std::vector<std::string> {"hello"}));
}
