// SPDX-License-Identifier: GPL-3.0-only
// Seats a virtual pad in a chosen console profile, without "Who's using this controller?". Measured on 10.20 (PS5 Pro,
// 2026-11-06): the user id in the pad's add descriptor (pad.c) is not what the system goes by; SceShellCore's login
// manager asks for every new controller unless exactly one profile is signed in and has none. What it calls once a
// profile is picked is sceMbusBindDeviceWithUserId(device, user), and a pad's handle from the add ioctl is that device
// id. Bound within a millisecond of the add, before the login manager looks at the new pad, the system logs
// "[onDeviceAdded] Assigned device userId:..." and shows no question. A profile that isn't signed in is signed in first
// with sceUserServiceLogin, which only SceShellCore's credentials may call: this process takes them for that one call.
#ifndef PROSPERO_PROFILE_SEAT_H
#define PROSPERO_PROFILE_SEAT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Whether the profile is signed in now.
int ps_profile_signed_in(uint32_t user);
// Signs the profile in (0, or the system's error). It doesn't ask for the profile's passcode.
int ps_profile_sign_in(uint32_t user);
// Signs the profile out (0, or the system's error), with the same borrowed credentials as signing in.
int ps_profile_sign_out(uint32_t user);
// The profile in front on the console (whoever the TV shows), 0 if none.
uint32_t ps_profile_foreground(void);
// Gives the device with this bus id to the profile: a pad by ps_pad_device_id, a keyboard or mouse by its handle with a
// device type above it (platform_ps5.cpp, seat_kbm_device). 0, or the system's error; -1 while libSceMbus is not up.
int ps_profile_bind(uint64_t device, uint32_t user);
// The profile's name as the console shows it, "" if unknown.
void ps_profile_name(uint32_t user, char *name, size_t size);
// A notification on the console's screen, for everyone in the room and in the stream.
void ps_notify(const char *text);

#ifdef __cplusplus
}
#endif
#endif
