
#ifndef MEDIA_H
#define MEDIA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stores all the needed media information
typedef struct {
    char    *title;
    char    *artist;
    bool     is_paused;
    // Thumbnail data
    uint8_t *thumb_data; // RGB888 pixels, each one consisting of three array elements
    int      thumb_width;
    int      thumb_height;
} media_info_t;

bool media_init(void);
void media_close(void);

// Outcomes of media_poll()
typedef enum {
    MEDIA_ERROR     = -1,
    MEDIA_NONE      =  0,
    MEDIA_UNCHANGED =  1,
    MEDIA_CHANGED   =  2
} media_status_t;

// Polls whether the media info has changed. If so, it fills `out` with new data.
// Otherwise it doesn't do anything.
media_status_t media_read(media_info_t *out);

// Frees the buffer inside a thumbnail and zeroes the struct
void media_free(media_info_t *thumb);

#ifdef __cplusplus
}
#endif

#endif // MEDIA_H
