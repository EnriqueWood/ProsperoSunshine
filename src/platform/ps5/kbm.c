// SPDX-License-Identifier: GPL-3.0-only
// libSceKeyboard's and libSceMouse's virtual-device calls each reduce to one ioctl on /dev/hid, as the pad's do
// (pad.c); docs/re/keyboard-mouse.md has the disassembly behind these:
//   add       0xC018482A  { u32 op; u64 desc*; u64 int* handleOut }, desc as the pad's: class 2 @0, userId @0x1C,
//                         subtype @0x20; keyboard op 1 subtype 0xa, mouse op 0 subtype 0xb
//   keyboard  0x80104874  { u32 handle; u64 block* }, block 0x20 bytes: modifiers @9, LEDs @0xa, key count @0xb,
//                         connected @0xc, HID usages @0x10
//   mouse     0x80104875  { u32 handle; u64 block* }, block 0x50 bytes: 1 @0x10, s16 dx @0x16, s16 dy @0x18,
//                         buttons @0x1a, wheel @0x1b, tilt @0x1c, u32 4 @0x24
//   delete    0x80104850  { u8 op = 5; u32 handle @8 }
#include "kbm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define IOCTL_ADD 0xC018482AUL
#define IOCTL_DELETE 0x80104850UL
#define IOCTL_KEYBOARD 0x80104874UL
#define IOCTL_MOUSE 0x80104875UL
#define DEVICE_CLASS 2
#define KEYBOARD_OP 1
#define KEYBOARD_SUBTYPE 0xa
#define MOUSE_OP 0
#define MOUSE_SUBTYPE 0xb
#define KEYBOARD_BLOCK 0x20
#define KEYBOARD_KEYS 16
#define MOUSE_BLOCK 0x50

struct device {
    int fd;
    int handle;
};

struct ps_keyboard {
    struct device device;
};

struct ps_mouse {
    struct device device;
};

static int add(uint32_t user_id, uint32_t op, uint8_t subtype, struct device *device)
{
    int fd = open("/dev/hid", O_RDONLY);
    if (fd < 0) return -errno;
    uint8_t desc[0xA8] = {0};
    desc[0] = DEVICE_CLASS;
    memcpy(desc + 0x1C, &user_id, 4);
    desc[0x20] = subtype;
    int handle = -1;
    struct { uint32_t op; uint32_t pad; uint64_t desc; uint64_t handle; } request = {
        op, 0, (uint64_t)(uintptr_t)desc, (uint64_t)(uintptr_t)&handle
    };
    if (ioctl(fd, IOCTL_ADD, &request) != 0 || handle < 0) {
        int error = errno ? -errno : -EIO;
        close(fd);
        return error;
    }
    device->fd = fd;
    device->handle = handle;
    return 0;
}

static int insert(const struct device *device, unsigned long code, uint8_t *block)
{
    struct { uint32_t handle; uint32_t pad; uint64_t block; } request = {
        (uint32_t)device->handle, 0, (uint64_t)(uintptr_t)block
    };
    return ioctl(device->fd, code, &request) == 0 ? 0 : (errno ? -errno : -EIO);
}

static int remove_device(const struct device *device, int rc)
{
    struct { uint8_t op; uint8_t pad[7]; uint32_t handle; uint32_t pad2; } del = {5, {0}, (uint32_t)device->handle, 0};
    if (ioctl(device->fd, IOCTL_DELETE, &del) != 0 && !rc) rc = errno ? -errno : -EIO;
    close(device->fd);
    return rc;
}

int ps_keyboard_open(uint32_t user_id, ps_keyboard **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    ps_keyboard *keyboard = malloc(sizeof *keyboard);
    if (!keyboard) return -ENOMEM;
    int rc = add(user_id, KEYBOARD_OP, KEYBOARD_SUBTYPE, &keyboard->device);
    if (rc) {
        free(keyboard);
        return rc;
    }
    *out = keyboard;
    return 0;
}

int ps_keyboard_send(ps_keyboard *keyboard, uint8_t modifiers, const uint8_t *usages, uint8_t count)
{
    if (!keyboard || count > KEYBOARD_KEYS || (count && !usages)) return -EINVAL;
    uint8_t block[KEYBOARD_BLOCK] = {0};
    block[0x9] = modifiers;
    block[0xb] = count;
    block[0xc] = 1;
    if (count) memcpy(block + 0x10, usages, count);
    return insert(&keyboard->device, IOCTL_KEYBOARD, block);
}

int ps_keyboard_close(ps_keyboard *keyboard)
{
    if (!keyboard) return 0;
    int rc = remove_device(&keyboard->device, ps_keyboard_send(keyboard, 0, NULL, 0));
    free(keyboard);
    return rc;
}

int ps_mouse_open(uint32_t user_id, ps_mouse **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    ps_mouse *mouse = malloc(sizeof *mouse);
    if (!mouse) return -ENOMEM;
    int rc = add(user_id, MOUSE_OP, MOUSE_SUBTYPE, &mouse->device);
    if (rc) {
        free(mouse);
        return rc;
    }
    *out = mouse;
    return 0;
}

int ps_mouse_send(ps_mouse *mouse, int16_t dx, int16_t dy, uint8_t buttons, int8_t wheel, int8_t tilt)
{
    if (!mouse) return -EINVAL;
    uint8_t block[MOUSE_BLOCK] = {0};
    const uint32_t four = 4;
    block[0x10] = 1;
    memcpy(block + 0x16, &dx, 2);
    memcpy(block + 0x18, &dy, 2);
    block[0x1a] = buttons;
    block[0x1b] = (uint8_t)wheel;
    block[0x1c] = (uint8_t)tilt;
    memcpy(block + 0x24, &four, 4);
    return insert(&mouse->device, IOCTL_MOUSE, block);
}

int ps_mouse_close(ps_mouse *mouse)
{
    if (!mouse) return 0;
    int rc = remove_device(&mouse->device, ps_mouse_send(mouse, 0, 0, 0, 0, 0));
    free(mouse);
    return rc;
}

uint32_t ps_keyboard_device_id(const ps_keyboard *keyboard)
{
    return keyboard ? (uint32_t)keyboard->device.handle : 0;
}

uint32_t ps_mouse_device_id(const ps_mouse *mouse)
{
    return mouse ? (uint32_t)mouse->device.handle : 0;
}
