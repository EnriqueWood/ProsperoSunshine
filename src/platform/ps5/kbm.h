// SPDX-License-Identifier: GPL-3.0-only
// Native virtual keyboard and mouse: the devices behind libSceKeyboard's and libSceMouse's
// VirtualDeviceAddDevice / InsertData. Like the pad (pad.h) they are built directly on /dev/hid; layouts
// measured on FW 13.00 (docs/re/keyboard-mouse.md), unverified on any other firmware.
#ifndef PROSPERO_KBM_H
#define PROSPERO_KBM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Mouse buttons as SceMouseData reports them. Back and forward follow the HID order, unverified.
enum ps_mouse_button {
    PS_MOUSE_PRIMARY = 0x1, PS_MOUSE_SECONDARY = 0x2, PS_MOUSE_MIDDLE = 0x4, PS_MOUSE_BACK = 0x8, PS_MOUSE_FORWARD = 0x10
};

typedef struct ps_keyboard ps_keyboard;
typedef struct ps_mouse ps_mouse;

// Adds a USB-type keyboard for user_id; the system shows "Keyboard connected". Returns 0 or a negative errno.
int ps_keyboard_open(uint32_t user_id, ps_keyboard **out);
// The whole keyboard state: HID modifier bits (0x01 left Ctrl ... 0x80 right GUI) and the held keys as HID
// usages, at most 16. It stays in effect until the next one.
int ps_keyboard_send(ps_keyboard *keyboard, uint8_t modifiers, const uint8_t *usages, uint8_t count);
// Releases every key and removes the keyboard. NULL is harmless.
int ps_keyboard_close(ps_keyboard *keyboard);
// The handle the add returned; the bus knows the keyboard as handle | 3 << 48 (profile_seat.h). 0 for NULL.
uint32_t ps_keyboard_device_id(const ps_keyboard *keyboard);

// Adds a mouse for user_id; the system shows "Mouse connected". Returns 0 or a negative errno.
int ps_mouse_open(uint32_t user_id, ps_mouse **out);
// One report: movement since the last one, the held buttons, and wheel and tilt steps.
int ps_mouse_send(ps_mouse *mouse, int16_t dx, int16_t dy, uint8_t buttons, int8_t wheel, int8_t tilt);
// Releases every button and removes the mouse. NULL is harmless.
int ps_mouse_close(ps_mouse *mouse);
// The handle the add returned; the bus knows the mouse as handle | 4 << 48. 0 for NULL.
uint32_t ps_mouse_device_id(const ps_mouse *mouse);

#ifdef __cplusplus
}
#endif
#endif
