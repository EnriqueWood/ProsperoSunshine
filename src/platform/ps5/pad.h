// SPDX-License-Identifier: GPL-3.0-only
// Native virtual controller: the system's own virtual-pad device that Remote Play feeds.
// Ported from ps5-mcp's RemoteEye (C#); layouts measured on FW 13.00. There is no firmware check
// here: ioctl numbers, the 0xA8-byte blocks and their field offsets are unverified on any other
// firmware (checklist in docs/firmware-porting.md).
#ifndef PROSPERO_PAD_H
#define PROSPERO_PAD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// ScePadButton bits.
enum ps_pad_button {
    PS_PAD_CREATE = 0x1, PS_PAD_L3 = 0x2, PS_PAD_R3 = 0x4, PS_PAD_OPTIONS = 0x8,
    PS_PAD_UP = 0x10, PS_PAD_RIGHT = 0x20, PS_PAD_DOWN = 0x40, PS_PAD_LEFT = 0x80,
    PS_PAD_L2 = 0x100, PS_PAD_R2 = 0x200, PS_PAD_L1 = 0x400, PS_PAD_R1 = 0x800,
    PS_PAD_TRIANGLE = 0x1000, PS_PAD_CIRCLE = 0x2000, PS_PAD_CROSS = 0x4000, PS_PAD_SQUARE = 0x8000,
    PS_PAD_PS = 0x10000, PS_PAD_TOUCHPAD = 0x100000
};

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;  // 0..255, 128 is centre
    uint8_t l2, r2;          // 0..255 analog triggers
} ps_pad_state;

// What the system wants from a controller, the way Remote Play reads it. Field meanings beyond the
// motors are what the code that consumes them suggests; see docs/re/pad-output.md.
typedef struct {
    uint32_t flag_a;
    uint8_t flags[2];
    uint8_t motors[2];
    uint8_t color[3];
    uint8_t triggers[24];
    uint32_t values[6];
    uint32_t flag_b;
} ps_pad_output;

// One finger on the touchpad: x 0..1919, y 0..1079. The id stays the same while the finger is down, 7 bits.
typedef struct {
    uint8_t id;
    uint16_t x, y;
} ps_pad_touch;

typedef struct ps_pad ps_pad;

// The signed-in user owning the first home folder, or 0 if none could be found.
uint32_t ps_pad_default_user(void);
// A pad for no user: the system asks "Who's using this controller?" on it, offering the signed-in users and Add User
// (a guest, for one). Seen on 13.00.
#define PS_PAD_NO_USER 0xFFFFFFFFu
// Adds a DualSense-type virtual controller for user_id. Returns 0 or a negative error/errno.
// A new controller makes the system ask "Who's using this controller?" once.
int ps_pad_open(uint32_t user_id, ps_pad **out);
// The system's id for the pad: the handle the add ioctl gave, which SceShellCore logs as the device id (0x34030d on
// 10.20; profile_seat.h binds a profile to it). 0 for NULL.
uint32_t ps_pad_device_id(const ps_pad *pad);
// Feeds one state; it stays in effect until the next one.
int ps_pad_send(ps_pad *pad, const ps_pad_state *state);
// Touch and motion arrive apart from the buttons; each stays in effect until changed. Layouts: docs/re/pad-input.md.
// Fingers down, at most 2; a lifted finger is simply no longer listed.
int ps_pad_set_touch(ps_pad *pad, uint8_t count, const ps_pad_touch points[2]);
// In rad/s.
int ps_pad_set_angular_velocity(ps_pad *pad, const float xyz[3]);
// In g.
int ps_pad_set_acceleration(ps_pad *pad, const float xyz[3]);
// Lifts all fingers and puts motion at rest; buttons and sticks stay.
int ps_pad_rest(ps_pad *pad);
// Reads the current output state. Remote Play polls this every 10 ms.
int ps_pad_read_output(ps_pad *pad, ps_pad_output *output);
// Releases every button, centres both sticks, lifts all fingers and puts motion at rest.
int ps_pad_neutral(ps_pad *pad);
// Sends a neutral state, removes the controller and frees the handle. NULL is harmless.
int ps_pad_close(ps_pad *pad);

#ifdef __cplusplus
}
#endif
#endif
