#pragma once

// Options screen: reached from the main menu, currently exposes a
// single toggle for the cartoon-outline post-process used by
// gameplay. Future settings would live here too.

// One scanned BLE device, rendered as a clickable row in the
// Chessnut picker.
struct OptionsScannedDevice {
    const char* address;  // null-terminated MAC
    const char* name;     // null-terminated, may be empty
};

// hover semantics:
//   0 = none, 1 = back, 2 = outline, 3 = continuous-voice,
//   4 = chessnut toggle, 5 = picker cancel/refresh button,
//   6 = picker "forget cached device" button,
//   100+i = picker row #i.
// When `picker_open` is true, the renderer draws the picker
// underneath the toggles instead of the chessnut row label
// changing — toggles still render and can be clicked.
void renderer_draw_options(bool cartoon_outline_enabled,
                           bool voice_continuous_enabled,
                           bool continuous_voice_supported,
                           bool chessnut_enabled,
                           bool chessnut_supported,
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
