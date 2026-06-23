/*
 * media.cpp:
 * A convenient wrapper for the WinRT C++ API.
 * Allows us to easily poll the current Windows "now playing" session
 * and retrieve all the needed media information.
 */
#include "media.h"
#include <string>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>

using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Graphics::Imaging;

namespace {
    GlobalSystemMediaTransportControlsSessionManager manager(nullptr);
    std::string last_id; // identity the last track we reported, so we can detect changes

    // Decodes a thumbnail stream reference into a RGB888 buffer
    int decode_thumbnail(IRandomAccessStreamReference const &ref, media_info_t *out)
    {
        if (!ref)
            return 0;

        IRandomAccessStreamWithContentType stream = ref.OpenReadAsync().get();
        BitmapDecoder decoder = BitmapDecoder::CreateAsync(stream).get();

        // Ask for straight RGBA, no scaling, EXIF rotation, etc.
        PixelDataProvider provider = decoder.GetPixelDataAsync(
            BitmapPixelFormat::Rgba8,
            BitmapAlphaMode::Ignore,
            BitmapTransform{},
            ExifOrientationMode::IgnoreExifOrientation,
            ColorManagementMode::DoNotColorManage).get();
        com_array<uint8_t> rgba = provider.DetachPixelData();
        uint32_t w = decoder.PixelWidth();
        uint32_t h = decoder.PixelHeight();

        size_t pixel_count = (size_t) w * h;
        if (pixel_count == 0 || rgba.size() < pixel_count * 4)
            return -1;

        uint8_t *rgb = (uint8_t*) malloc(pixel_count * 3);
        if (!rgb)
            return -1;

        for (size_t i = 0; i < pixel_count; ++i)
        {
            rgb[i * 3]     = rgba[i * 4];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }

        out->thumb_data   = rgb;
        out->thumb_width  = (int) w;
        out->thumb_height = (int) h;
        return 0;
    }

    // Copies a WinRT hstring into a NUL-terminated UTF-8 buffer
    char *utf8_buf(hstring const &s)
    {
        std::string utf8 = to_string(s); // WinRT hstring (UTF-16) -> UTF-8
        char *out = (char*) malloc(utf8.size() + 1);
        if (out)
            memcpy(out, utf8.c_str(), utf8.size() + 1);
        return out;
    }
}

bool media_init(void)
{
    try
    {
        init_apartment(apartment_type::multi_threaded);
        manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        return manager ? true : false;
    }
    catch (...)
    {
        return false;
    }
}

void media_close(void)
{
    manager = nullptr;
    uninit_apartment();
}

media_status_t media_read(media_info_t *out)
{
    if (!out || !manager)
        return MEDIA_ERROR;

    try
    {
        GlobalSystemMediaTransportControlsSession session = manager.GetCurrentSession();
        if (!session)
        {
            // Nothing is playing. Reset the id so the next track counts as a change
            last_id.clear();
            return MEDIA_NONE;
        }

        GlobalSystemMediaTransportControlsSessionMediaProperties props =
            session.TryGetMediaPropertiesAsync().get();
        if (!props)
            return MEDIA_ERROR;

        hstring title  = props.Title();
        hstring artist = props.Artist();
        // Treat anything other than "Playing" as paused
        bool is_paused = session.GetPlaybackInfo().PlaybackStatus() !=
                         GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

        // Build an id of the track
        std::string id = to_string(title);
        id.push_back('\x1f');
        id.append(to_string(artist));
        id.push_back('\x1f');
        id.push_back(is_paused ? '1' : '0');
        if (id == last_id)
            return MEDIA_UNCHANGED; // if it's the same as last one - skip
        last_id = id;

        out->title        = utf8_buf(title);
        out->artist       = utf8_buf(artist);
        out->is_paused    = is_paused;
        out->thumb_data   = nullptr;
        out->thumb_width  = 0;
        out->thumb_height = 0;
        decode_thumbnail(props.Thumbnail(), out); // failure leaves thumb_data NULL
        return MEDIA_CHANGED;
    }
    catch (...)
    {
        return MEDIA_ERROR;
    }
}

void media_free(media_info_t *info)
{
    if (!info)
        return;
    free(info->title);
    free(info->artist);
    free(info->thumb_data);
    info->title        = nullptr;
    info->artist       = nullptr;
    info->thumb_data   = nullptr;
    info->thumb_width  = 0;
    info->thumb_height = 0;
}
