// SPDX-License-Identifier: GPL-3.0-only
#include "console_users.h"
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceUserServiceGetUserName(int32_t, char *, size_t);

static int by_id(const void *a, const void *b)
{
    const uint32_t x = ((const ps_console_user *)a)->id, y = ((const ps_console_user *)b)->id;
    return x < y ? -1 : x > y;
}

int ps_console_users(ps_console_user *users, int max)
{
    if (!users || max <= 0) return -EINVAL;
    DIR *dir = opendir("/user/home");
    if (!dir) return -errno;
    // Something else in this process may have initialized it already (title_launch.c does the same).
    const int already_initialized = (int)0x80960003;
    const int init = sceUserServiceInitialize(NULL);
    int count = 0;
    for (struct dirent *e; count < max && (e = readdir(dir));) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 8) continue;
        char *end = NULL;
        const unsigned long id = strtoul(e->d_name, &end, 16);
        if (!end || *end || id == 0 || id == 0xffffffffUL) continue;
        ps_console_user *user = &users[count++];
        user->id = (uint32_t)id;
        memset(user->name, 0, sizeof user->name);
        if (init == 0 || init == already_initialized) {
            if (sceUserServiceGetUserName((int32_t)id, user->name, sizeof user->name - 1) != 0) user->name[0] = '\0';
        }
        // One line for the control socket's answer.
        for (char *c = user->name; *c; ++c)
            if ((unsigned char)*c < 0x20 || *c == 0x7f) *c = ' ';
    }
    closedir(dir);
    if (init == 0) sceUserServiceTerminate();
    qsort(users, (size_t)count, sizeof *users, by_id);
    return count;
}
