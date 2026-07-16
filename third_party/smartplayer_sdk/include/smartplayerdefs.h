#ifndef SMARTPLAYER_DEFS_H
#define SMARTPLAYER_DEFS_H

#include <cstdint>

// DLL export macros
#ifdef SMARTPLAYER_STATIC
  #define SMARTPLAYER_API
#elif defined(_WIN32)
  #ifdef SMARTPLAYER_EXPORTS
    #define SMARTPLAYER_API __declspec(dllexport)
  #else
    #define SMARTPLAYER_API __declspec(dllimport)
  #endif
#else
  #define SMARTPLAYER_API __attribute__((visibility("default")))
#endif

// Player states
enum SmartPlayerState : int {
    SP_STATE_STOPPED = 0,
    SP_STATE_RUNNING = 1,
    SP_STATE_PAUSED  = 2
};

// Pixel formats (used by the video frame callback)
enum SmartPixelFormat : int {
    SP_FMT_UNKNOWN  = 0,
    SP_FMT_YUV420P  = 1,
    SP_FMT_NV12     = 2,
    SP_FMT_RGBA     = 3,
    SP_FMT_BGRA     = 4
};

// Media types
enum SmartMediaType : int {
    SP_MEDIA_FILE  = 0,
    SP_MEDIA_RTSP  = 1,
    SP_MEDIA_RTMP  = 2,
    SP_MEDIA_HTTP  = 3,
    SP_MEDIA_HTTPS = 4,
    SP_MEDIA_HLS   = 5
};

// Media info DTO
struct SmartMediaInfo {
    char        fileName[512]       = {};
    char        filePath[512]       = {};
    char        formatName[128]     = {};
    int64_t     fileSize      = 0;       // bytes (local files only; 0 for streams)
    int64_t     durationMs   = 0;
    int64_t     bitRate      = 0;        // container bit rate (bits/s)
    int64_t     startTimeMs  = 0;        // first frame timestamp (ms)
    bool        hasVideo     = false;
    bool        hasAudio     = false;
    bool        hasSubtitle  = false;

    // Video parameters (valid only when hasVideo == true)
    char        videoCodecName[64]      = {}; // e.g. "h264", "hevc", "vp9"
    char        videoPixelFormat[64]    = {}; // e.g. "yuv420p"
    int         videoWidth              = 0;
    int         videoHeight             = 0;
    int         videoBitRate            = 0;  // bits/s (0 if unknown)
    double      videoFrameRate          = 0.0;
    int         videoFrameCount         = 0;  // total frames (0 if unknown)
    int         videoRotation           = 0;  // display rotation in degrees

    // Audio parameters (valid only when hasAudio == true)
    char        audioCodecName[64]      = {}; // e.g. "aac", "mp3"
    int         audioChannels           = 0;
    int         audioSampleRate         = 0;
    int         audioBitRate            = 0;  // bits/s (0 if unknown)
};

#endif // SMARTPLAYER_DEFS_H
