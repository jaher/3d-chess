#pragma once

// Options screen: voice, hints, backdrop, robotic board, environment,
// and the persisted classic-piece solid/jelly style.

// One scanned BLE device, rendered as a clickable row in the
// Chessnut picker.
struct OptionsScannedDevice {
    const char* address;  // null-terminated MAC
    const char* name;     // null-terminated, may be empty
};

// hover semantics:
//   0 = none, 1 = back, 3 = continuous-voice,
//   4 = chessnut toggle, 5 = picker cancel/refresh button,
//   6 = picker "forget cached device" button,
//   7 = BLE verbose-log toggle,
//   8 = "Speak moves" (TTS) toggle,
//   9 = "Move hints" tri-state cycle (Off / Auto / OnDemand),
//   10 = "Gaussian splats" backdrop toggle (Marble medieval-room
//        splat cloud behind the chess board),
//   11 = "Environment" cycle — click steps through the registered
//        splat backdrops (e.g. Medieval room → …),
//   100+i = picker row #i.
//   12 = classic-piece style (solid/jelly; retro models stay unchanged).
// When `picker_open` is true, the renderer draws the picker
// underneath the toggles instead of the chessnut row label
// changing — toggles still render and can be clicked.
//
// `hint_mode`: 0 = Off (grey), 1 = Auto (green), 2 = OnDemand
// (amber, distinct from the binary on/off toggles around it).
// `environment_label`: text shown after "Environment: " on row 7;
// caller is responsible for the lookup (board_renderer's
// renderer_environment_label / current value of
// AppState::environment).
void renderer_draw_options(bool jelly_pieces, bool splats_enabled,
                           bool voice_continuous_enabled,
                           bool continuous_voice_supported,
                           bool voice_tts_enabled,
                           int  hint_mode,
                           bool chessnut_enabled,
                           bool chessnut_supported,
                           bool ble_verbose_log_enabled,
                           const char* environment_label,
                           bool picker_open,
                           bool picker_scanning,
                           const OptionsScannedDevice* picker_devices,
                           int picker_device_count,
                           int width, int height,
                           int hover);

// Returns one of the codes above based on mouse position. Pass
// *_supported=false to skip the corresponding hit zone. Picker
// hit-tests are disabled when `picker_open` is false.
int options_hit_test(double mx, double my, int width, int height,
                     bool continuous_voice_supported,
                     bool chessnut_supported,
                     bool picker_open,
                     int picker_device_count);
