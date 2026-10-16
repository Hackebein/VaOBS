// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stddef.h>

// A C interface keeps Qt's MSVC ABI separate from the audio engine.
struct VaobsDockHost {
    unsigned abi_version;
    size_t (*read_status)(char *buffer, size_t capacity);
    bool (*retry)(const char *uuid);
    void (*settings)(const char *uuid);
    bool (*add_dock)(const char *id, const char *title, void *widget);
    void (*remove_dock)(const char *id);
    bool (*next)(const char *uuid);
};
