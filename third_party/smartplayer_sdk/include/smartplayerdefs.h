#ifndef SMARTPLAYER_DEFS_H
#define SMARTPLAYER_DEFS_H

#include <cstdint>
#include <string>

// DLL 导出宏
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

// 播放器状态
enum SmartPlayerState : int {
    SP_STATE_STOPPED = 0,
    SP_STATE_RUNNING = 1,
    SP_STATE_PAUSED  = 2
};

// 像素格式（帧回调用）
enum SmartPixelFormat : int {
    SP_FMT_UNKNOWN  = 0,
    SP_FMT_YUV420P  = 1,
    SP_FMT_NV12     = 2,
    SP_FMT_RGBA     = 3,
    SP_FMT_BGRA     = 4
};

// 媒体类型
enum SmartMediaType : int {
    SP_MEDIA_FILE  = 0,
    SP_MEDIA_RTSP  = 1,
    SP_MEDIA_RTMP  = 2,
    SP_MEDIA_HTTP  = 3,
    SP_MEDIA_HTTPS = 4,
    SP_MEDIA_HLS   = 5
};

// 媒体信息 DTO
struct SmartMediaInfo {
    std::string fileName;
    std::string filePath;
    std::string formatName;
    int64_t     durationMs   = 0;
    int64_t     bitRate      = 0;
    bool        hasVideo     = false;
    bool        hasAudio     = false;
    std::string videoPixelFormat;
    double      videoFrameRate = 0.0;
    int         audioChannels   = 0;
    int         audioSampleRate = 0;
};

#endif // SMARTPLAYER_DEFS_H
