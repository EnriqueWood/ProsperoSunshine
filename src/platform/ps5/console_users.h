// SPDX-License-Identifier: GPL-3.0-only
// The console's profiles, for the web page's profile choice (control/device_profiles.hpp).
#ifndef PROSPERO_CONSOLE_USERS_H
#define PROSPERO_CONSOLE_USERS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;    // the user id, also the name of its folder in /user/home
    char name[64];  // as the console shows it (UTF-8), empty if the user service gave none
} ps_console_user;

// Every profile on the console, signed in or not, sorted by id: the hex folders of /user/home, each named by
// sceUserServiceGetUserName. Neither needs more than a payload's own credentials (measured on 10.20, 2026-11-06;
// sceUserServiceGetRegisteredUserIdList would, and stops the calling thread without them). Returns how many were written
// (at most max), or a negative errno when /user/home can't be read.
int ps_console_users(ps_console_user *users, int max);

#ifdef __cplusplus
}
#endif
#endif
