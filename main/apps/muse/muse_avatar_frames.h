#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MUSE_AVATAR_FRAME_COUNT 30
#define MUSE_AVATAR_FRAME_PIXELS (64 * 64)
// Expands one frame into dst (MUSE_AVATAR_FRAME_PIXELS RGB565 values; 0x0000 is
// the transparent colorkey). Returns false for an out-of-range frame or NULL dst.
bool muse_avatar_expand_frame(unsigned frame, uint16_t *dst);
#ifdef __cplusplus
}
#endif
