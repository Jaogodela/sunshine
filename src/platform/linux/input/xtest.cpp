/**
 * @file src/platform/linux/input/xtest.cpp
 * @brief Gaspar X11 XTest keyboard/mouse backend.
 */

// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

// local includes
#include "src/logging.h"
#include "xtest.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_X11)
  #include <X11/Xlib.h>
  #include <X11/extensions/XTest.h>
#endif

using namespace std::literals;

namespace platf::sp_xtest {

  namespace {

    // X11 keysym values from <X11/keysymdef.h>. Kept as literals so the VK table is
    // pure and testable even in builds without X11 headers.
    constexpr std::uint32_t SP_XK_a = 0x0061;
    constexpr std::uint32_t SP_XK_0 = 0x0030;
    constexpr std::uint32_t SP_XK_F1 = 0xFFBE;
    constexpr std::uint32_t SP_XK_KP_0 = 0xFFB0;

    constexpr std::uint32_t SP_XK_BackSpace = 0xFF08;
    constexpr std::uint32_t SP_XK_Tab = 0xFF09;
    constexpr std::uint32_t SP_XK_Clear = 0xFF0B;
    constexpr std::uint32_t SP_XK_Return = 0xFF0D;
    constexpr std::uint32_t SP_XK_Pause = 0xFF13;
    constexpr std::uint32_t SP_XK_Scroll_Lock = 0xFF14;
    constexpr std::uint32_t SP_XK_Escape = 0xFF1B;
    constexpr std::uint32_t SP_XK_Home = 0xFF50;
    constexpr std::uint32_t SP_XK_Left = 0xFF51;
    constexpr std::uint32_t SP_XK_Up = 0xFF52;
    constexpr std::uint32_t SP_XK_Right = 0xFF53;
    constexpr std::uint32_t SP_XK_Down = 0xFF54;
    constexpr std::uint32_t SP_XK_Prior = 0xFF55;
    constexpr std::uint32_t SP_XK_Next = 0xFF56;
    constexpr std::uint32_t SP_XK_End = 0xFF57;
    constexpr std::uint32_t SP_XK_Select = 0xFF60;
    constexpr std::uint32_t SP_XK_Print = 0xFF61;
    constexpr std::uint32_t SP_XK_Execute = 0xFF62;
    constexpr std::uint32_t SP_XK_Insert = 0xFF63;
    constexpr std::uint32_t SP_XK_Menu = 0xFF67;
    constexpr std::uint32_t SP_XK_Help = 0xFF6A;
    constexpr std::uint32_t SP_XK_Num_Lock = 0xFF7F;
    constexpr std::uint32_t SP_XK_KP_Multiply = 0xFFAA;
    constexpr std::uint32_t SP_XK_KP_Add = 0xFFAB;
    constexpr std::uint32_t SP_XK_KP_Subtract = 0xFFAD;
    constexpr std::uint32_t SP_XK_KP_Decimal = 0xFFAE;
    constexpr std::uint32_t SP_XK_KP_Divide = 0xFFAF;
    constexpr std::uint32_t SP_XK_Shift_L = 0xFFE1;
    constexpr std::uint32_t SP_XK_Shift_R = 0xFFE2;
    constexpr std::uint32_t SP_XK_Control_L = 0xFFE3;
    constexpr std::uint32_t SP_XK_Control_R = 0xFFE4;
    constexpr std::uint32_t SP_XK_Caps_Lock = 0xFFE5;
    constexpr std::uint32_t SP_XK_Alt_L = 0xFFE9;
    constexpr std::uint32_t SP_XK_Alt_R = 0xFFEA;
    constexpr std::uint32_t SP_XK_Super_L = 0xFFEB;
    constexpr std::uint32_t SP_XK_Super_R = 0xFFEC;
    constexpr std::uint32_t SP_XK_Delete = 0xFFFF;

    constexpr std::uint32_t SP_XK_space = 0x0020;
    constexpr std::uint32_t SP_XK_apostrophe = 0x0027;
    constexpr std::uint32_t SP_XK_comma = 0x002C;
    constexpr std::uint32_t SP_XK_minus = 0x002D;
    constexpr std::uint32_t SP_XK_period = 0x002E;
    constexpr std::uint32_t SP_XK_slash = 0x002F;
    constexpr std::uint32_t SP_XK_semicolon = 0x003B;
    constexpr std::uint32_t SP_XK_equal = 0x003D;
    constexpr std::uint32_t SP_XK_bracketleft = 0x005B;
    constexpr std::uint32_t SP_XK_backslash = 0x005C;
    constexpr std::uint32_t SP_XK_bracketright = 0x005D;
    constexpr std::uint32_t SP_XK_grave = 0x0060;
    constexpr std::uint32_t SP_XK_less = 0x003C;

    // Windows virtual-key codes as received from the client.
    constexpr std::uint16_t SP_VK_BACK = 0x08;
    constexpr std::uint16_t SP_VK_TAB = 0x09;
    constexpr std::uint16_t SP_VK_CLEAR = 0x0C;
    constexpr std::uint16_t SP_VK_RETURN = 0x0D;
    constexpr std::uint16_t SP_VK_SHIFT = 0x10;
    constexpr std::uint16_t SP_VK_CONTROL = 0x11;
    constexpr std::uint16_t SP_VK_MENU = 0x12;
    constexpr std::uint16_t SP_VK_PAUSE = 0x13;
    constexpr std::uint16_t SP_VK_CAPITAL = 0x14;
    constexpr std::uint16_t SP_VK_ESCAPE = 0x1B;
    constexpr std::uint16_t SP_VK_SPACE = 0x20;
    constexpr std::uint16_t SP_VK_PRIOR = 0x21;
    constexpr std::uint16_t SP_VK_NEXT = 0x22;
    constexpr std::uint16_t SP_VK_END = 0x23;
    constexpr std::uint16_t SP_VK_HOME = 0x24;
    constexpr std::uint16_t SP_VK_LEFT = 0x25;
    constexpr std::uint16_t SP_VK_UP = 0x26;
    constexpr std::uint16_t SP_VK_RIGHT = 0x27;
    constexpr std::uint16_t SP_VK_DOWN = 0x28;
    constexpr std::uint16_t SP_VK_SELECT = 0x29;
    constexpr std::uint16_t SP_VK_PRINT = 0x2A;
    constexpr std::uint16_t SP_VK_EXECUTE = 0x2B;
    constexpr std::uint16_t SP_VK_SNAPSHOT = 0x2C;
    constexpr std::uint16_t SP_VK_INSERT = 0x2D;
    constexpr std::uint16_t SP_VK_DELETE = 0x2E;
    constexpr std::uint16_t SP_VK_HELP = 0x2F;
    constexpr std::uint16_t SP_VK_LWIN = 0x5B;
    constexpr std::uint16_t SP_VK_RWIN = 0x5C;
    constexpr std::uint16_t SP_VK_APPS = 0x5D;
    constexpr std::uint16_t SP_VK_MULTIPLY = 0x6A;
    constexpr std::uint16_t SP_VK_ADD = 0x6B;
    constexpr std::uint16_t SP_VK_SUBTRACT = 0x6D;
    constexpr std::uint16_t SP_VK_DECIMAL = 0x6E;
    constexpr std::uint16_t SP_VK_DIVIDE = 0x6F;
    constexpr std::uint16_t SP_VK_NUMLOCK = 0x90;
    constexpr std::uint16_t SP_VK_SCROLL = 0x91;
    constexpr std::uint16_t SP_VK_LSHIFT = 0xA0;
    constexpr std::uint16_t SP_VK_RSHIFT = 0xA1;
    constexpr std::uint16_t SP_VK_LCONTROL = 0xA2;
    constexpr std::uint16_t SP_VK_RCONTROL = 0xA3;
    constexpr std::uint16_t SP_VK_LMENU = 0xA4;
    constexpr std::uint16_t SP_VK_RMENU = 0xA5;
    constexpr std::uint16_t SP_VK_OEM_1 = 0xBA;
    constexpr std::uint16_t SP_VK_OEM_PLUS = 0xBB;
    constexpr std::uint16_t SP_VK_OEM_COMMA = 0xBC;
    constexpr std::uint16_t SP_VK_OEM_MINUS = 0xBD;
    constexpr std::uint16_t SP_VK_OEM_PERIOD = 0xBE;
    constexpr std::uint16_t SP_VK_OEM_2 = 0xBF;
    constexpr std::uint16_t SP_VK_OEM_3 = 0xC0;
    constexpr std::uint16_t SP_VK_OEM_4 = 0xDB;
    constexpr std::uint16_t SP_VK_OEM_5 = 0xDC;
    constexpr std::uint16_t SP_VK_OEM_6 = 0xDD;
    constexpr std::uint16_t SP_VK_OEM_7 = 0xDE;
    constexpr std::uint16_t SP_VK_OEM_102 = 0xE2;

    /**
     * @brief Convert a Windows virtual-key code to an X11 keysym.
     *
     * @param vk Windows virtual-key code.
     * @return X11 keysym, or `std::nullopt` when the key is not mapped.
     */
    std::optional<std::uint32_t> vk_to_keysym(std::uint16_t vk) {
      if (vk >= 'A' && vk <= 'Z') {
        return SP_XK_a + (vk - 'A');
      }
      if (vk >= '0' && vk <= '9') {
        return SP_XK_0 + (vk - '0');
      }
      if (vk >= 0x70 && vk <= 0x7B) {
        // VK_F1 .. VK_F12
        return SP_XK_F1 + (vk - 0x70);
      }
      if (vk >= 0x60 && vk <= 0x69) {
        // VK_NUMPAD0 .. VK_NUMPAD9
        return SP_XK_KP_0 + (vk - 0x60);
      }

      switch (vk) {
        case SP_VK_BACK: return SP_XK_BackSpace;
        case SP_VK_TAB: return SP_XK_Tab;
        case SP_VK_CLEAR: return SP_XK_Clear;
        case SP_VK_RETURN: return SP_XK_Return;
        case SP_VK_SHIFT: return SP_XK_Shift_L;
        case SP_VK_CONTROL: return SP_XK_Control_L;
        case SP_VK_MENU: return SP_XK_Alt_L;
        case SP_VK_PAUSE: return SP_XK_Pause;
        case SP_VK_CAPITAL: return SP_XK_Caps_Lock;
        case SP_VK_ESCAPE: return SP_XK_Escape;
        case SP_VK_SPACE: return SP_XK_space;
        case SP_VK_PRIOR: return SP_XK_Prior;
        case SP_VK_NEXT: return SP_XK_Next;
        case SP_VK_END: return SP_XK_End;
        case SP_VK_HOME: return SP_XK_Home;
        case SP_VK_LEFT: return SP_XK_Left;
        case SP_VK_UP: return SP_XK_Up;
        case SP_VK_RIGHT: return SP_XK_Right;
        case SP_VK_DOWN: return SP_XK_Down;
        case SP_VK_SELECT: return SP_XK_Select;
        case SP_VK_PRINT: return SP_XK_Print;
        case SP_VK_EXECUTE: return SP_XK_Execute;
        case SP_VK_SNAPSHOT: return SP_XK_Print;
        case SP_VK_INSERT: return SP_XK_Insert;
        case SP_VK_DELETE: return SP_XK_Delete;
        case SP_VK_HELP: return SP_XK_Help;
        case SP_VK_LWIN: return SP_XK_Super_L;
        case SP_VK_RWIN: return SP_XK_Super_R;
        case SP_VK_APPS: return SP_XK_Menu;
        case SP_VK_MULTIPLY: return SP_XK_KP_Multiply;
        case SP_VK_ADD: return SP_XK_KP_Add;
        case SP_VK_SUBTRACT: return SP_XK_KP_Subtract;
        case SP_VK_DECIMAL: return SP_XK_KP_Decimal;
        case SP_VK_DIVIDE: return SP_XK_KP_Divide;
        case SP_VK_NUMLOCK: return SP_XK_Num_Lock;
        case SP_VK_SCROLL: return SP_XK_Scroll_Lock;
        case SP_VK_LSHIFT: return SP_XK_Shift_L;
        case SP_VK_RSHIFT: return SP_XK_Shift_R;
        case SP_VK_LCONTROL: return SP_XK_Control_L;
        case SP_VK_RCONTROL: return SP_XK_Control_R;
        case SP_VK_LMENU: return SP_XK_Alt_L;
        case SP_VK_RMENU: return SP_XK_Alt_R;
        case SP_VK_OEM_1: return SP_XK_semicolon;
        case SP_VK_OEM_PLUS: return SP_XK_equal;
        case SP_VK_OEM_COMMA: return SP_XK_comma;
        case SP_VK_OEM_MINUS: return SP_XK_minus;
        case SP_VK_OEM_PERIOD: return SP_XK_period;
        case SP_VK_OEM_2: return SP_XK_slash;
        case SP_VK_OEM_3: return SP_XK_grave;
        case SP_VK_OEM_4: return SP_XK_bracketleft;
        case SP_VK_OEM_5: return SP_XK_backslash;
        case SP_VK_OEM_6: return SP_XK_bracketright;
        case SP_VK_OEM_7: return SP_XK_apostrophe;
        case SP_VK_OEM_102: return SP_XK_less;
        default: return std::nullopt;
      }
    }

#if defined(__linux__) && defined(SUNSHINE_BUILD_X11)

    // WHEEL_DELTA convention. Sub-notch remainders are intentionally truncated.
    constexpr int sp_wheel_delta = 120;

    /**
     * @brief Serialize access to the shared X display.
     *
     * Xlib is not thread-safe unless `XInitThreads()` was called before its first
     * use, so every XTest call is serialized on this mutex as well.
     *
     * @return Process-wide backend mutex.
     */
    std::mutex &backend_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    /**
     * @brief Return the event sink installed by a unit test.
     *
     * @return Installed test sink, or `nullptr` when the production sink is active.
     */
    const ops_t *&test_ops() {
      static const ops_t *ops = nullptr;
      return ops;
    }

    /**
     * @brief Return the lazily opened X display.
     *
     * @return X display, or `nullptr` when it has not been opened.
     */
    Display *&display() {
      static Display *value = nullptr;
      return value;
    }

    /**
     * @brief Track a permanent display-open failure so the error is logged once.
     *
     * @return Whether opening the display has already failed.
     */
    bool &display_failed() {
      static bool failed = false;
      return failed;
    }

    /**
     * @brief Emit a key event through XTest.
     *
     * The caller must hold `backend_mutex()` and have opened the display.
     *
     * @param keysym X11 keysym to press or release.
     * @param pressed Whether the key is pressed.
     */
    void default_key_event(std::uint32_t keysym, bool pressed);

    /**
     * @brief Move the pointer relatively through XTest.
     *
     * @param dx Horizontal delta.
     * @param dy Vertical delta.
     */
    void default_relative_motion(int dx, int dy);

    /**
     * @brief Move the pointer absolutely through XTest.
     *
     * @param x Root-window X coordinate.
     * @param y Root-window Y coordinate.
     */
    void default_absolute_motion(int x, int y);

    /**
     * @brief Emit a mouse button event through XTest.
     *
     * @param button X11 button number.
     * @param pressed Whether the button is pressed.
     */
    void default_button_event(unsigned int button, bool pressed);

    /**
     * @brief Flush pending XTest events.
     */
    void default_flush();

    /**
     * @brief Return the production event sink.
     *
     * @return Production XTest sink.
     */
    const ops_t &default_ops() {
      static const ops_t ops {
        default_key_event,
        default_relative_motion,
        default_absolute_motion,
        default_button_event,
        default_flush,
      };
      return ops;
    }

    /**
     * @brief Return the active event sink.
     *
     * @return Test sink when installed, otherwise the production sink.
     */
    const ops_t &current_ops() {
      return test_ops() ? *test_ops() : default_ops();
    }

    /**
     * @brief Open the X display once.
     *
     * @return True when the display is usable.
     */
    bool ensure_display_locked() {
      if (test_ops()) {
        return true;
      }
      if (display()) {
        return true;
      }
      if (display_failed()) {
        return false;
      }

      // x11grab also calls XInitThreads(), but capture may initialize after input.
      static std::once_flag threads_once;
      std::call_once(threads_once, [] {
        XInitThreads();
      });

      display() = XOpenDisplay(nullptr);
      if (!display()) {
        display_failed() = true;
        BOOST_LOG(error) << "XTest keyboard/mouse backend: unable to open X display; "
                            "dropping keyboard and mouse input"sv;
        return false;
      }

      int event_base = 0;
      int error_base = 0;
      int major = 0;
      int minor = 0;
      if (!XTestQueryExtension(display(), &event_base, &error_base, &major, &minor)) {
        display_failed() = true;
        XCloseDisplay(display());
        display() = nullptr;
        BOOST_LOG(error) << "XTest keyboard/mouse backend: XTEST extension is not available; "
                            "dropping keyboard and mouse input"sv;
        return false;
      }

      BOOST_LOG(info) << "XTest keyboard/mouse backend initialized"sv;
      return true;
    }

    /**
     * @brief Map a Moonlight mouse button to an X11 button number.
     *
     * @param button Moonlight mouse button code.
     * @return X11 button number, or `std::nullopt` when the button is unknown.
     */
    std::optional<unsigned int> x11_button(int button) {
      switch (button) {
        case BUTTON_LEFT: return 1;
        case BUTTON_MIDDLE: return 2;
        case BUTTON_RIGHT: return 3;
        case BUTTON_X1: return 8;
        case BUTTON_X2: return 9;
        default: return std::nullopt;
      }
    }

    /**
     * @brief Emit scroll notches as button clicks.
     *
     * @param button X11 wheel button number.
     * @param ticks Number of notches to emit.
     */
    void emit_scroll_ticks_locked(unsigned int button, int ticks) {
      auto &ops = current_ops();
      const int count = ticks < 0 ? -ticks : ticks;
      for (int i = 0; i < count; ++i) {
        ops.button_event(button, true);
        ops.button_event(button, false);
      }
      ops.flush();
    }

    void default_key_event(std::uint32_t keysym, bool pressed) {
      if (!display()) {
        return;
      }

      const auto keycode = XKeysymToKeycode(display(), static_cast<KeySym>(keysym));
      if (!keycode) {
        BOOST_LOG(warning) << "XTest keyboard/mouse backend: no keycode for keysym "sv << keysym;
        return;
      }

      XTestFakeKeyEvent(display(), keycode, pressed ? True : False, CurrentTime);
    }

    void default_relative_motion(int dx, int dy) {
      if (display()) {
        XTestFakeRelativeMotionEvent(display(), dx, dy, CurrentTime);
      }
    }

    void default_absolute_motion(int x, int y) {
      if (display()) {
        XTestFakeMotionEvent(display(), DefaultScreen(display()), x, y, CurrentTime);
      }
    }

    void default_button_event(unsigned int button, bool pressed) {
      if (display()) {
        XTestFakeButtonEvent(display(), button, pressed ? True : False, CurrentTime);
      }
    }

    void default_flush() {
      if (display()) {
        XFlush(display());
      }
    }

#endif

  }  // namespace

  std::optional<std::uint32_t> sp_vk_to_keysym(std::uint16_t vk) {
    return vk_to_keysym(vk);
  }

  std::pair<int, int> sp_abs_coordinates(const touch_port_t &touch_port, float x, float y) {
    const int max_x = touch_port.width > 0 ? touch_port.width - 1 : 0;
    const int max_y = touch_port.height > 0 ? touch_port.height - 1 : 0;
    const int root_x = static_cast<int>(std::lround(x));
    const int root_y = static_cast<int>(std::lround(y));

    return {
      std::clamp(root_x, 0, max_x),
      std::clamp(root_y, 0, max_y),
    };
  }

#if defined(__linux__) && defined(SUNSHINE_BUILD_X11)

  void set_ops_for_testing(const ops_t *ops) {
    std::lock_guard lock(backend_mutex());
    test_ops() = ops;
  }

  void move_mouse(int dx, int dy) {
    std::lock_guard lock(backend_mutex());
    if (!ensure_display_locked()) {
      return;
    }

    auto &ops = current_ops();
    ops.relative_motion(dx, dy);
    ops.flush();
  }

  void abs_mouse(const touch_port_t &touch_port, float x, float y) {
    const auto [root_x, root_y] = sp_abs_coordinates(touch_port, x, y);

    std::lock_guard lock(backend_mutex());
    if (!ensure_display_locked()) {
      return;
    }

    auto &ops = current_ops();
    ops.absolute_motion(root_x, root_y);
    ops.flush();
  }

  void button_mouse(int button, bool release) {
    const auto xbutton = x11_button(button);
    if (!xbutton) {
      BOOST_LOG(warning) << "XTest keyboard/mouse backend: unknown mouse button "sv << button;
      return;
    }

    std::lock_guard lock(backend_mutex());
    if (!ensure_display_locked()) {
      return;
    }

    auto &ops = current_ops();
    ops.button_event(*xbutton, !release);
    ops.flush();
  }

  void scroll(int high_res_distance) {
    const int ticks = high_res_distance / sp_wheel_delta;
    if (ticks == 0) {
      return;
    }

    std::lock_guard lock(backend_mutex());
    if (!ensure_display_locked()) {
      return;
    }

    emit_scroll_ticks_locked(ticks > 0 ? 4 : 5, ticks);
  }

  void hscroll(int high_res_distance) {
    const int ticks = high_res_distance / sp_wheel_delta;
    if (ticks == 0) {
      return;
    }

    std::lock_guard lock(backend_mutex());
    if (!ensure_display_locked()) {
      return;
    }

    emit_scroll_ticks_locked(ticks > 0 ? 7 : 6, ticks);
  }

  void keyboard_update(std::uint16_t modcode, bool release) {
    const auto keysym = sp_vk_to_keysym(modcode);
    if (!keysym) {
      BOOST_LOG(warning) << "XTest keyboard/mouse backend: no keysym mapping for virtual-key code "sv << modcode;
      return;
    }

    std::lock_guard lock(backend_mutex());
    if (!ensure_display_locked()) {
      return;
    }

    auto &ops = current_ops();
    ops.key_event(*keysym, !release);
    ops.flush();
  }

  void unicode(const char * /*utf8*/, int /*size*/) {
    static std::once_flag once;
    std::call_once(once, [] {
      BOOST_LOG(verbose) << "XTest keyboard/mouse backend ignores UTF-8 text input so it cannot bypass the input blocklist"sv;
    });
  }

#else

  void set_ops_for_testing(const ops_t * /*ops*/) {}

  void move_mouse(int /*dx*/, int /*dy*/) {}

  void abs_mouse(const touch_port_t & /*touch_port*/, float /*x*/, float /*y*/) {}

  void button_mouse(int /*button*/, bool /*release*/) {}

  void scroll(int /*high_res_distance*/) {}

  void hscroll(int /*high_res_distance*/) {}

  void keyboard_update(std::uint16_t /*modcode*/, bool /*release*/) {}

  void unicode(const char * /*utf8*/, int /*size*/) {}

#endif

}  // namespace platf::sp_xtest
