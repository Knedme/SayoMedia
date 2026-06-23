/*
 * sayo_image.c  -  push custom images and animations to a SayoDevice O3C screen.
 *
 * Ported from the official SayoDeviceStreamingAssistant (Sources/SayoHid.cs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <hidapi.h>
#include <windows.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#include <math.h>
#include "assets.h"
#include "media.h"

#define SAYO_VID 0x8089
#define SAYO_PID 0x0009
#define REPORT_LEN 1024
#define FRAME_MS (1000 / 30)
#define POLL_MS 250
#define PAUSE_TIMEOUT_MS 7500
#define TEXT_SCROLL_SPEED 2
#define TEXT_SCROLL_HOLD  60 // frames to pause at each end before reversing

#pragma pack(push, 1) // avoid the padding for our structures
// SayoDevice's v2 HID packet
typedef struct {
    uint8_t  report_id;
    uint8_t  echo;      // source field
    uint16_t checksum;  // sum of the packet as LE uint16 words over (length + 4) bytes
    uint16_t length;    // payload length + 4 (covers length, cmd, index and data[])
    uint8_t  cmd;
    uint8_t  index;     // which target the command applies to
    uint8_t  data[];    // command payload
} sayo_packet_t;

// Payload of a stream-frame command (cmd 0x25)
typedef struct {
    uint32_t offset;  // where this chunk belongs in the full framebuffer
    uint8_t pixels[]; // RGB565 pixels, each consisting of two array elements
} sayo_stream_t;

// Payload of a query-screen reply (cmd 0x02)
typedef struct {
    uint16_t width;
    uint16_t height;
} sayo_screen_t;
#pragma pack(pop)

// Stores all the text element data
typedef struct {
    char    *text;        // utf-8 string
    int      x, y;
    int      line_height;
    int      width;       // container width. the text wider than this scrolls
    uint8_t  r, g, b;

    int      text_width;  // actual full width of the text

    // Scroll animation stuff
    int      scroll;
    int      dir;
    int      hold;        // frames left to pause at an end before reversing
} text_element_t;

// Opens the high-speed v2 interface (8000hz, 1024-byte reports)
hid_device *open_v2(void);
// Asks the screen for its dimensions (cmd 0x02)
bool query_screen(hid_device *h, int *w, int *ht, uint8_t *buf);
// Streams a full RGB565 framebuffer to the screen (cmd 0x25)
bool send_frame(hid_device *h, const uint8_t *fb, int fb_size, uint8_t *buf);
// Adds a RGB888 image to the framebuffer
void put_image(uint8_t *fb, uint8_t *image, int scr_w, int scr_h, int x, int y,
               int img_w, int img_h, int src_x, int src_y, int crop_w, int crop_h);
// Scales a baked RGB888 image into a fresh malloc'd buffer
uint8_t *scale_image(const uint8_t *src, int src_w, int src_h, int w, int h);
// Creates a text element
text_element_t create_text(const char *utf8, int x, int y, int line_height, int width,
                           uint8_t r, uint8_t g, uint8_t b);
// Renders and adds the text element to the framebuffer
void draw_text(uint8_t *fb, int scr_w, int scr_h, text_element_t *t);
// Frees the utf8 buffer and nulls the struct
void free_text(text_element_t *t);
// Draws a small "now playing" equalizer
void draw_eq(uint8_t *fb, int scr_w, int scr_h, bool is_paused, int tick);

int main(void)
{
    // Initialize everything
    if (hid_init())
    {
        printf("ERROR: hid_init failed!\n");
        return 1;
    }
    hid_device *h = open_v2();
    if (!h)
    {
        printf("ERROR: fast v2 interface was not found!\n"
               "You probably need to set your device to 8000Hz polling on sayodevice.com website.\n");
        hid_exit();
        return 1;
    }
    if (!media_init())
    {
        printf("ERROR: failed to init the media manager!\n");
        return 1;
    }

    // Allocate stuff, figure out screen dimensions
    uint8_t buf[REPORT_LEN] = {0};
    int w, ht;
    if (!query_screen(h, &w, &ht, buf))
    {
        // On error we assume the dimensions are these
        w = 160; ht = 80;
    }

    size_t fb_size = w * ht * 2; // *2 because `one pixel = two elements in fb`
    uint8_t *fb = calloc(fb_size, 1);
    media_info_t *media = calloc(1, sizeof(media_info_t));
    if (!fb || !media)
    {
        printf("Buy more RAM >_<\n");
        return 1;
    }

    // Scale the playback icons
    const int SCALED_ICON_SIZE = ht * 0.1;
    uint8_t *back_icon = scale_image(
        back_rgb, ICON_SIZE, ICON_SIZE, SCALED_ICON_SIZE, SCALED_ICON_SIZE);
    uint8_t *pause_icon = scale_image(
        pause_rgb, ICON_SIZE, ICON_SIZE, SCALED_ICON_SIZE, SCALED_ICON_SIZE);
    uint8_t *play_icon = scale_image(
        play_rgb, ICON_SIZE, ICON_SIZE, SCALED_ICON_SIZE, SCALED_ICON_SIZE);
    uint8_t *next_icon = scale_image(
        next_rgb, ICON_SIZE, ICON_SIZE, SCALED_ICON_SIZE, SCALED_ICON_SIZE);

    text_element_t title = {0};
    text_element_t artist = {0};

    bool have_fb = false;
    bool present = false;    // is a session currently active (playing or paused)?
    bool paused  = false;
    int  pause_frames   = 0; // frames left to keep sending after a pause
    int  frames_to_poll = 0; // frames left until the next media poll
    int  eq_tick = 0;        // count ticks for the equalizer
    
    // Main loop
    for (;;)
    {
        // Only poll every POLL_MS, not every frame
        if (frames_to_poll <= 0)
        {
            frames_to_poll = POLL_MS / FRAME_MS;

            switch (media_read(media))
            {
            case MEDIA_CHANGED:
                memset(fb, 0, fb_size); // Clear the framebuffer
                
                // Draw the thumbnail
                uint8_t *thumb;
                int th_orig_w, th_orig_h;
                if (media->thumb_data != NULL)
                {
                    thumb = media->thumb_data;
                    th_orig_w = media->thumb_width;
                    th_orig_h = media->thumb_height;
                }
                else
                {
                    // Show the placeholder if there's no thumbnail
                    thumb = (uint8_t*) placeholder_rgb;
                    th_orig_w = PLACEHOLDER_WIDTH;
                    th_orig_h = PLACEHOLDER_HEIGHT;
                }
                // Resize the image
                const int th_h = 0.8 * ht;
                const int th_w = (th_orig_w * th_h) / th_orig_h; // width preserves the aspect ratio
                uint8_t *resized_thumb = scale_image(thumb, th_orig_w, th_orig_h, th_w, th_h);
                // Put the pixels
                const TH_MARGIN = 3;
                put_image(fb, resized_thumb, w, ht,
                    TH_MARGIN, (ht - th_h) / 2, // center the image on Y axis
                    th_w, th_h,
                    // Draw only the square (new_height x new_height) at the center, crop everything else
                    (th_w - th_h) / 2, 0,
                    th_h, th_h);
                free(resized_thumb);

                // Create the title and the artist
                free_text(&title);
                free_text(&artist);
                const int TEXT_PADDING = 6;
                const int TITLE_LH  = ht * 0.2;
                const int ARTIST_LH = TITLE_LH - 4;
                artist = create_text(
                    media->artist,
                    TH_MARGIN + th_h + TEXT_PADDING, (ht - ARTIST_LH) / 2,
                    ARTIST_LH, w - TH_MARGIN - th_h - 2 * TEXT_PADDING,
                    213, 213, 213);
                title = create_text(
                    media->title,
                    TH_MARGIN + th_h + TEXT_PADDING, (ht - ARTIST_LH) / 2 - 4 - TITLE_LH,
                    TITLE_LH, w - TH_MARGIN - th_h - 2 * TEXT_PADDING,
                    255, 255, 255);
                
                // Draw the playback icons
                const int ICON_MARGIN_X = 8, ICON_MARGIN_Y = 3, ICON_GAP = 3;
                uint8_t *row[] = {
                    back_icon,
                    media->is_paused ? play_icon : pause_icon, // display resume or pause
                    next_icon,
                };
                for (size_t i = 0; i < sizeof(row) / sizeof(row[0]); i++)
                    put_image(fb, row[i], w, ht,
                        TH_MARGIN + th_h + ICON_MARGIN_X + i * (ICON_GAP + SCALED_ICON_SIZE),
                        (ht + th_h) / 2 - ICON_MARGIN_Y - SCALED_ICON_SIZE,
                        SCALED_ICON_SIZE, SCALED_ICON_SIZE, 0, 0, SCALED_ICON_SIZE, SCALED_ICON_SIZE);

                have_fb = true;
                present = true;
                paused  = media->is_paused;
                // Begin a timeout after which we stop displaying everything
                if (paused)
                    pause_frames = PAUSE_TIMEOUT_MS / FRAME_MS;
                media_free(media);
                eq_tick = 0;
                break;

            case MEDIA_NONE:
                present = false; // nothing is playing, let the screen sleep
                break;

            default:
                break;
            }
        }
        frames_to_poll--;

        // Draw the title and the artist
        // We do it here because text must be able to scroll if it's too long
        draw_text(fb, w, ht, &title);
        draw_text(fb, w, ht, &artist);

        // Draw the equalizer
        draw_eq(fb, w, ht, paused, eq_tick);
        eq_tick++;

        // Do we need to keep the screen awake by resending the framebuffer?
        bool keep_alive = have_fb && present;
        if (keep_alive && paused)
        {
            if (pause_frames > 0)
                pause_frames--;
            else
                keep_alive = false;
        }

        if (keep_alive && !send_frame(h, fb, fb_size, buf)) // Draw the framebuffer
        {
            // A failure usually means the device was unplugged
            // Drop the stale handle and wait for it to reappear
            hid_close(h);
            h = NULL;
            while (!h)
            {
                Sleep(500);
                h = open_v2();
            }
        }

        Sleep(FRAME_MS);
    }

    free(fb);
    free(back_icon);
    free(pause_icon);
    free(play_icon);
    free(next_icon);
    free_text(&title);
    free_text(&artist);
    media_close();
    hid_close(h);
    hid_exit();
    return 0;
}

hid_device *open_v2(void)
{
    struct hid_device_info *devs = hid_enumerate(SAYO_VID, SAYO_PID);
    hid_device *h = NULL;
    for (struct hid_device_info *d = devs; d; d = d->next)
        if (d->usage_page == 0xFF12) // 0xFF12 corresponds to that high-speed interface
        {
            h = hid_open_path(d->path);
            if (h) break;
        }
    hid_free_enumeration(devs);
    return h;
}

// Fills in all the header fields of a packet
// Must be called AFTER the data[] payload is in place
static void set_header(uint8_t *buf, uint8_t cmd, uint8_t index, size_t data_len)
{
    sayo_packet_t *pkt = (sayo_packet_t*) buf;
    pkt->report_id = 0x22;                     // always 0x22 on the 1024-byte interface
    pkt->echo      = 0x04;                     // tags this as application traffic
    pkt->checksum  = 0;                        // zero while we sum
    pkt->length    = (uint16_t)(data_len + 4); // +4 covers additional fields
    pkt->cmd       = cmd;
    pkt->index     = index;

    // Count the checksum
    uint16_t hash = 0;
    // Iterate through the whole packet
    for (size_t i = 0; i < data_len+sizeof(sayo_packet_t); i += 2)
        hash += (uint16_t)(buf[i] | (buf[i + 1] << 8));
    pkt->checksum = hash;
}

bool query_screen(hid_device *h, int *w, int *ht, uint8_t *buf)
{
    // Send a request
    memset(buf, 0, REPORT_LEN);
    set_header(buf, 0x02, 0, 0); // it has no data
    if (hid_write(h, buf, REPORT_LEN) < 0)
        return false;

    // Recieve the response
    uint8_t res_buf[1024] = {0};
    int nrd = hid_read_timeout(h, res_buf, REPORT_LEN, 1000);
    sayo_packet_t *resp = (sayo_packet_t*) res_buf;
    if (nrd < sizeof(sayo_packet_t) + sizeof(sayo_screen_t) || resp->cmd != 0x02) // check the response
        return false;
    sayo_screen_t *scr = (sayo_screen_t*) resp->data;
    *w  = scr->width; *ht = scr->height;
    return (*w && *ht) ? true : false; // another check of the response
}

bool send_frame(hid_device *h, const uint8_t *fb, int fb_size, uint8_t *buf)
{
    // Current packet and data
    sayo_packet_t *pkt = (sayo_packet_t*) buf;
    sayo_stream_t *str = (sayo_stream_t*) pkt->data;

    // Maximum amount of pixels we can transfer in one packet (subtract the header and the offset)
    const int MAX_PIXELS = REPORT_LEN - sizeof(sayo_packet_t) - sizeof(sayo_stream_t);

    // Send the fb in little chunks
    int chunk = 0;
    for (int i = 0; i < fb_size; i += chunk)
    {
        chunk = (fb_size - i < MAX_PIXELS) ? (fb_size - i) : MAX_PIXELS;

        memset(buf, 0, REPORT_LEN);
        str->offset = (uint32_t) i;
        memcpy(str->pixels, fb + i, chunk);

        set_header(buf, 0x25, 0, chunk + sizeof(sayo_stream_t)); // data_len is the chunk + the offset field len
        if (hid_write(h, buf, REPORT_LEN) < 0)
            return false;
    }

    return true;
}

// Adds a RGB888 pixel to the framebuffer
static void put_pixel(uint8_t *fb, int scr_w, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    size_t idx = (y * scr_w + x) * 2; // each pixel = 2 bytes
    uint16_t px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); // standard RGB565
    fb[idx]     = (uint8_t)(px & 0xFF);
    fb[idx + 1] = (uint8_t)(px >> 8);
}

void put_image(uint8_t *fb, uint8_t *image, int scr_w, int scr_h, int x, int y,
               int img_w, int img_h, int src_x, int src_y, int crop_w, int crop_h)
{
    if (!image) return;

    // Clamp the iteration range so everything stays inbounds
    int col0 = 0, col1 = crop_w, row0 = 0, row1 = crop_h;
    if (col0 < -x)            col0 = -x;            // off the left of the screen
    if (col0 < -src_x)        col0 = -src_x;        // before the image's left edge
    if (col1 > scr_w - x)     col1 = scr_w - x;     // off the right of the screen
    if (col1 > img_w - src_x) col1 = img_w - src_x; // past the image's right edge
    if (row0 < -y)            row0 = -y;
    if (row0 < -src_y)        row0 = -src_y;
    if (row1 > scr_h - y)     row1 = scr_h - y;
    if (row1 > img_h - src_y) row1 = img_h - src_y;

    for (int row = row0; row < row1; row++)
        for (int col = col0; col < col1; col++)
        {
            size_t sidx = ((size_t)(src_y + row) * img_w + (src_x + col)) * 3;
            put_pixel(fb, scr_w, x + col, y + row,
                      image[sidx], image[sidx + 1], image[sidx + 2]);
        }
}

uint8_t *scale_image(const uint8_t *src, int src_w, int src_h, int w, int h)
{
    uint8_t *dst = malloc(w * h * 3);
    if (dst)
        stbir_resize_uint8_linear(src, src_w, src_h, 0, dst, w, h, 0, STBIR_RGB);
    return dst;
}

// Decodes and returns one UTF-8 codepoint from *p and advances *p past it
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char*) *p;
    if (s[0] < 0x80) { *p += 1; return s[0]; }

    int n;
    uint32_t cp;
    if      ((s[0] & 0xE0) == 0xC0) { n = 1; cp = s[0] & 0x1F; }
    else if ((s[0] & 0xF0) == 0xE0) { n = 2; cp = s[0] & 0x0F; }
    else if ((s[0] & 0xF8) == 0xF0) { n = 3; cp = s[0] & 0x07; }
    else { *p += 1; return 0xFFFD; }

    for (int i = 1; i <= n; i++)
    {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; } // bad/truncated
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += n + 1;
    return cp;
}

// Looks up a glyph. Sets *width to its actual pixel width
static const uint8_t *glyph_lookup(uint32_t cp, int *width)
{
    if (cp >= UNIFONT_MAX_CP || !unifont_widths[cp])
        cp = '?'; // unknown codepoint -> question mark
    *width = unifont_widths[cp];
    return &unifont_bitmaps[cp * UNIFONT_GLYPH_BYTES];
}

// Scales the glyph width while preserving the aspect ratio
static int scale_glyph_width(int orig_w, int target_height)
{
    return orig_w * target_height / UNIFONT_GLYPH_HEIGHT;
}

// Measures the width of the entire text when drawn `height` px tall
static int measure_text(const char *s, int height)
{
    int total = 0, orig_w;
    while (*s)
    {
        glyph_lookup(utf8_next(&s), &orig_w);
        total += scale_glyph_width(orig_w, height);
    }
    return total;
}

// Adds one glyph scaled to `height` px tall at (x, y) to the framebuffer
static void draw_glyph(uint8_t *fb, int scr_w, int x, int y,
                       const uint8_t *bmp, int orig_w, int target_height,
                       uint8_t r, uint8_t g, uint8_t b,
                       int clip_x0, int clip_x1, int clip_y0, int clip_y1)
{
    int display_width = scale_glyph_width(orig_w, target_height);
    if (!bmp || display_width <= 0 || target_height <= 0) return;

    for (int dy = 0; dy < target_height; dy++)
    {
        int py = y + dy;
        if (py < clip_y0 || py >= clip_y1) // Y clipping
            continue;
        // the source rows this destination row covers (always at least one)
        int sy0 = dy * UNIFONT_GLYPH_HEIGHT / target_height;
        int sy1 = (dy + 1) * UNIFONT_GLYPH_HEIGHT / target_height;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;

        for (int dx = 0; dx < display_width; dx++)
        {
            int px = x + dx;
            if (px < clip_x0 || px >= clip_x1) // X clipping
                continue;
            // the source columns this destination column covers (always at least one)
            int sx0 = dx * orig_w / display_width;
            int sx1 = (dx + 1) * orig_w / display_width;
            if (sx1 <= sx0)
                sx1 = sx0 + 1;

            // lit if any of the covered source pixels are on
            bool lit = false;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++)
                    // each row is 2 bytes: cols 0..7 in the first, 8..15 in the second
                    lit |= bmp[sy * 2 + (sx / 8)] & (0x80 >> (sx % 8));
            if (lit) // draw the pixel
                put_pixel(fb, scr_w, px, py, r, g, b);
        }
    }
}

text_element_t create_text(const char *utf8, int x, int y, int line_height, int width,
                           uint8_t r, uint8_t g, uint8_t b)
{
    text_element_t t;
    t.text        = utf8 ? strdup(utf8) : NULL;
    t.x           = x;
    t.y           = y;
    t.line_height = line_height;
    t.width       = width;
    t.r = r; t.g = g; t.b = b;
    t.text_width  = t.text ? measure_text(t.text, line_height) : 0;
    t.scroll      = 0;
    t.dir         = 1;
    t.hold        = TEXT_SCROLL_HOLD; // start with a pause at the left edge
    return t;
}

void draw_text(uint8_t *fb, int scr_w, int scr_h, text_element_t *t)
{
    // Clip region
    int x0 = t->x < 0 ? 0 : t->x;
    int y0 = t->y < 0 ? 0 : t->y;
    int x1 = t->x + t->width;       if (x1 > scr_w) x1 = scr_w;
    int y1 = t->y + t->line_height; if (y1 > scr_h) y1 = scr_h;

    // Clear the rect behind the text
    for (int yy = y0; yy < y1; yy++)
        if (x1 > x0)
            memset(fb + ((size_t)yy * scr_w + x0) * 2, 0, (size_t)(x1 - x0) * 2);

    if (!t->text) return;

    // Advance the scroll
    int max_scroll = t->text_width - t->width;
    if (max_scroll <= 0)
    {
        t->scroll = 0; // text fits
    }
    else if (t->hold > 0)
    {
        t->hold--; // paused at an end
    }
    else
    {
        t->scroll += t->dir * TEXT_SCROLL_SPEED;
        if (t->scroll >= max_scroll)
        {
            t->scroll = max_scroll;
            t->dir = -1;
            t->hold = TEXT_SCROLL_HOLD;
        }
        else if (t->scroll <= 0)
        {
            t->scroll = 0;
            t->dir = 1;
            t->hold = TEXT_SCROLL_HOLD;
        }
    }

    // Draw the glyphs, skipping any that fall outside the container
    int pen_x = t->x - t->scroll; // text scrolls in the other direction
    const char *s = t->text;
    while (*s)
    {
        int g_orig_w;
        const uint8_t *bmp = glyph_lookup(utf8_next(&s), &g_orig_w);
        int glyph_display_width = scale_glyph_width(g_orig_w, t->line_height);
        if (pen_x + glyph_display_width > t->x && pen_x < t->x + t->width)
            draw_glyph(fb, scr_w, pen_x, t->y, bmp, g_orig_w, t->line_height, t->r, t->g, t->b, x0, x1, y0, y1);
        pen_x += glyph_display_width;
        if (pen_x >= t->x + t->width)
            break; // everything after this is off the right edge
    }
}

void free_text(text_element_t *t)
{
    if (!t) return;
    free(t->text);
    t->text = NULL;
}

void draw_eq(uint8_t *fb, int scr_w, int scr_h, bool is_paused, int tick)
{
    // Constants
    const int eq_w = 0.1 * scr_w, eq_h = 0.125 * scr_h;
    const int eq_x = scr_w - 6 - eq_w,
              eq_y = 0.9 * scr_h - 3 - eq_h;

    const int BARS = 3;
    const int gap = 0.125 * eq_w, bar_w = eq_w / BARS - gap / 2;
    const int min_h = eq_h / 5; // never fully collapse

    const float freq[]  = { 0.20f, 0.29f, 0.25f }; // radians/frame, per bar
    const float phase[] = { 0.0f,  2.1f,  4.2f };  // desync offsets

    // Clear the rect of the eq
    int total_w = BARS * (bar_w + gap) - gap;
    for (int yy = eq_y; yy < eq_y + eq_h; yy++)
        memset(fb + (yy * scr_w + eq_x) * 2, 0, total_w * 2);

    // Draw the eq
    const int cap_r = bar_w / 2;
    for (int i = 0; i < BARS; i++)
    {
        // Bars sit at their minimum while paused, otherwise they bob
        float s = is_paused ? 0.0f : 0.5f + 0.5f * sinf(tick * freq[i] + phase[i]);
        int bh = min_h + (eq_h - min_h) * s;
        int bx = eq_x + i * (bar_w + gap);
        int top_y = eq_y + eq_h - bh;
        for (int yy = top_y; yy < eq_y + eq_h; yy++)
        {
            // Inset both sides near the top so the cap is a rounded dome
            int inset = 0;
            float ry = (top_y + cap_r) - (yy + 0.5f); // height into the cap
            if (cap_r > 0 && ry > 0)
                inset = cap_r - (int)(sqrtf(cap_r * cap_r - ry * ry) + 0.5f);
            for (int xx = bx + inset; xx < bx + bar_w - inset; xx++)
                put_pixel(fb, scr_w, xx, yy, 26, 184, 228);
        }
    }
}
