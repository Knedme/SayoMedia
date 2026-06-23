
#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>

// Fallback cover art (RGB888)
#define PLACEHOLDER_WIDTH  200
#define PLACEHOLDER_HEIGHT 200
extern const uint8_t placeholder_rgb[PLACEHOLDER_WIDTH * PLACEHOLDER_HEIGHT * 3];

// Playback icons (RGB888)
#define ICON_SIZE 50
extern const uint8_t back_rgb[ICON_SIZE * ICON_SIZE * 3];
extern const uint8_t play_rgb[ICON_SIZE * ICON_SIZE * 3];
extern const uint8_t next_rgb[ICON_SIZE * ICON_SIZE * 3];
extern const uint8_t pause_rgb[ICON_SIZE * ICON_SIZE * 3];

// One glyph: 16 rows x 2 bytes
#define UNIFONT_GLYPH_BYTES  32
#define UNIFONT_GLYPH_HEIGHT 16
#define UNIFONT_MAX_CP       0x10000 // basic multilingual plane only

// The entire unifont
extern const uint8_t unifont_bitmaps[UNIFONT_MAX_CP * UNIFONT_GLYPH_BYTES];
extern const uint8_t unifont_widths[UNIFONT_MAX_CP]; // 8, 16, or 0 if missing

#endif // ASSETS_H
