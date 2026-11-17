/**
 * @file src/platform/linux/input/xtest.h
 * @brief Declarations for the Gaspar X11 XTest keyboard/mouse backend.
 */
#pragma once

// standard includes
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

// local includes
#include "src/platform/common.h"

namespace platf::sp_xtest {

  /**
   * @brief Keyboard/mouse backend selected by `sp_kbm_backend`.
   */
  enum class kbm_backend_e {
    uinput,  ///< libvirtualhid/uinput backend (upstream default).
    xtest,  ///< X11 XTest backend for Xvfb sessions.
    wlr_virtual,  ///< Reserved for a future wlr-virtual-pointer backend.
  };

  /**
   * @brief Return the configured keyboard/mouse backend.
   *
   * @return Backend selected in configuration.
   */
  inline kbm_backend_e sp_configured_kbm_backend() {
    if (config::input.sp_kbm_backend == "xtest") {
      return kbm_backend_e::xtest;
    }
    if (config::input.sp_kbm_backend == "wlr_virtual") {
      return kbm_backend_e::wlr_virtual;
    }

    return kbm_backend_e::uinput;
  }

  /**
   * @brief Return whether this build contains the XTest backend.
   *
   * @return True when the build is Linux with X11 support.
   */
  inline bool sp_xtest_compiled() {
#if defined(__linux__) && defined(SUNSHINE_BUILD_X11)
    return true;
#else
    return false;
#endif
  }

  /**
   * @brief Return whether keyboard/mouse input should be delivered through XTest.
   *
   * @return True when `sp_kbm_backend = xtest` in a build with XTest support.
   */
  inline bool sp_kbm_uses_xtest() {
    return sp_xtest_compiled() && sp_configured_kbm_backend() == kbm_backend_e::xtest;
  }

  /**
   * @brief Return whether the backend must drop keyboard/mouse input.
   *
   * `wlr_virtual` is reserved and not implemented. An `xtest` selection is also
   * disabled when this build has no XTest support. Both fail closed rather than
   * silently creating uinput devices on the host.
   *
   * @return True when keyboard/mouse input must not be created or delivered.
   */
  inline bool sp_kbm_disabled() {
    const auto backend = sp_configured_kbm_backend();
    if (backend == kbm_backend_e::wlr_virtual) {
      return true;
    }
    if (backend == kbm_backend_e::xtest) {
      return !sp_xtest_compiled();
    }

    return false;
  }

  /**
   * @brief Return whether keyboard/mouse input should use the upstream libvirtualhid path.
   *
   * @return True when the configured backend is `uinput`.
   */
  inline bool sp_kbm_uses_uinput() {
    return !sp_kbm_uses_xtest() && !sp_kbm_disabled();
  }

  /**
   * @brief Log once when a configured backend cannot deliver keyboard/mouse input.
   *
   * The default `uinput` backend is silent so upstream behavior is unchanged.
   */
  inline void sp_log_disabled_backend_once() {
    if (!sp_kbm_disabled()) {
      return;
    }

    static std::once_flag once;
    std::call_once(once, [] {
      const auto backend = sp_configured_kbm_backend();
      if (backend == kbm_backend_e::wlr_virtual) {
        BOOST_LOG(error) << "sp_kbm_backend='wlr_virtual' is reserved and not implemented; "
                            "keyboard and mouse input will be dropped";
      } else {
        BOOST_LOG(error) << "sp_kbm_backend='xtest' requires a Linux build with X11/XTest support; "
                            "keyboard and mouse input will be dropped. Use sp_kbm_backend=uinput "
                            "to keep the libvirtualhid path.";
      }
    });
  }

  /**
   * @brief Sink used by the XTest backend to emit events.
   *
   * The production sink calls libX11/libXtst. Unit tests install a recording sink
   * so the backend can be exercised without an X server.
   */
  struct ops_t {
    void (*key_event)(std::uint32_t keysym, bool pressed);  ///< Emit a key press or release.
    void (*relative_motion)(int dx, int dy);  ///< Move the pointer relatively.
    void (*absolute_motion)(int x, int y);  ///< Move the pointer to root coordinates.
    void (*button_event)(unsigned int button, bool pressed);  ///< Emit a mouse button event.
    void (*flush)(void);  ///< Flush pending events.
  };

  /**
   * @brief Install or clear the event sink used by unit tests.
   *
   * @param ops Sink to install, or `nullptr` to restore the production sink.
   */
  void set_ops_for_testing(const ops_t *ops);

  /**
   * @brief Move the pointer relatively.
   *
   * @param dx Horizontal delta.
   * @param dy Vertical delta.
   */
  void move_mouse(int dx, int dy);

  /**
   * @brief Move the pointer to an absolute coordinate inside a touch port.
   *
   * @param touch_port Coordinate space reported by the capture backend.
   * @param x Absolute X coordinate.
   * @param y Absolute Y coordinate.
   */
  void abs_mouse(const touch_port_t &touch_port, float x, float y);

  /**
   * @brief Emit a mouse button transition.
   *
   * @param button Moonlight mouse button code.
   * @param release Whether the button was released.
   */
  void button_mouse(int button, bool release);

  /**
   * @brief Emit vertical scroll ticks.
   *
   * @param high_res_distance High-resolution scroll distance in WHEEL_DELTA units.
   */
  void scroll(int high_res_distance);

  /**
   * @brief Emit horizontal scroll ticks.
   *
   * @param high_res_distance High-resolution scroll distance in WHEEL_DELTA units.
   */
  void hscroll(int high_res_distance);

  /**
   * @brief Emit a keyboard key transition.
   *
   * @param modcode Windows virtual-key code received from the client.
   * @param release Whether the key was released.
   */
  void keyboard_update(std::uint16_t modcode, bool release);

  /**
   * @brief Handle UTF-8 text input.
   *
   * The XTest backend drops text input. It deliberately does not use Ctrl+Shift+U
   * or an IBus compose sequence, which would type into a terminal and bypass the
   * T-021 input blocklist.
   *
   * @param utf8 UTF-8 text buffer.
   * @param size Text buffer size.
   */
  void unicode(const char *utf8, int size);

  /**
   * @brief Map a Windows virtual-key code to an X11 keysym.
   *
   * The mapping assumes a fixed US layout.
   *
   * @param vk Windows virtual-key code.
   * @return X11 keysym, or `std::nullopt` when the key has no mapping.
   */
  std::optional<std::uint32_t> sp_vk_to_keysym(std::uint16_t vk);

  /**
   * @brief Map absolute touch-port coordinates onto root-window coordinates.
   *
   * Client coordinates already include the capture offset, so only the touch-port
   * extent is used to clamp the result.
   *
   * @param touch_port Coordinate space reported by the capture backend.
   * @param x Absolute X coordinate.
   * @param y Absolute Y coordinate.
   * @return Clamped root-window coordinate pair.
   */
  std::pair<int, int> sp_abs_coordinates(const touch_port_t &touch_port, float x, float y);

}  // namespace platf::sp_xtest
