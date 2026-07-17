#ifndef SMARTPLAYER_CALLBACK_H
#define SMARTPLAYER_CALLBACK_H

#include <cstdint>
#include "smartplayerdefs.h"

/**
 * Callback interface — users inherit this class, implement the events they need,
 * and register an instance with SmartPlayer.
 * All callbacks are invoked on the player's internal threads; do not perform
 * long-running or blocking work inside them.
 *
 * ============================================================================
 * CHARACTER ENCODING CONVENTIONS (IMPORTANT — READ BEFORE USE IN Qt)
 * ============================================================================
 *
 * All string parameters in this interface use UTF-8 encoding.
 *
 * When receiving callbacks in Qt (C++ subclass of SmartPlayerCallback),
 * you MUST convert const char* to QString using QString::fromUtf8().
 * NEVER use QString::fromLocal8Bit() or QString::fromLatin1() — they will
 * produce garbage characters for non-ASCII text (Chinese, Japanese, etc.).
 *
 * CORRECT (Qt side):
 *   void onError(const char* msg) override {
 *       ui->errorLabel->setText(QString::fromUtf8(msg));
 *   }
 *
 * WRONG (will cause mojibake for CJK paths / error messages):
 *   void onError(const char* msg) override {
 *       ui->errorLabel->setText(QString::fromLocal8Bit(msg));  // BAD
 *       ui->errorLabel->setText(QString::fromLatin1(msg));     // BAD
 *       ui->errorLabel->setText(QString(msg));                 // BAD
 *   }
 *
 * ============================================================================
 * CALLBACK RETURN VALUE REFERENCE FOR Qt DEVELOPERS
 * ============================================================================
 *
 * onStateChanged(state)
 *   - state: enum int (SP_STATE_STOPPED=0, SP_STATE_RUNNING=1, SP_STATE_PAUSED=2)
 *   - Qt: no string conversion needed
 *
 * onPositionChanged(posMs)
 *   - posMs: current playback position in milliseconds (int64_t)
 *   - Qt: cast to qint64
 *
 * onDurationChanged(durationMs)
 *   - durationMs: total media duration in milliseconds (int64_t)
 *   - Qt: cast to qint64
 *
 * onOpenResult(success, errMsg)
 *   - success: true if open succeeded, false otherwise
 *   - errMsg: error description in UTF-8. Qt: QString::fromUtf8(errMsg)
 *
 * onMediaInfoReady(info)
 *   - info.fileName:        file name (UTF-8). Qt: QString::fromUtf8(...)
 *   - info.filePath:        full path (UTF-8). Qt: QString::fromUtf8(...)
 *   - info.formatName:      container format name, e.g. "mov,mp4,m4a,3gp"
 *                          Qt: QString::fromUtf8(...)
 *   - info.fileSize:        media file size in bytes (0 for streams)
 *   - info.durationMs:      duration (int64_t)
 *   - info.startTimeMs:     first frame timestamp in ms (int64_t)
 *   - info.bitRate:         container bitrate in bits/s (int64_t)
 *   - info.hasVideo / info.hasAudio / info.hasSubtitle: bool
 *   - info.videoCodecName:  codec short name (e.g. "h264"). Qt: QString::fromUtf8(...)
 *   - info.videoPixelFormat: pixel format string, e.g. "yuv420p"
 *                            Qt: QString::fromUtf8(...)
 *   - info.videoWidth / info.videoHeight: pixel dimensions (int)
 *   - info.videoBitRate:    video bitrate in bits/s (int64_t)
 *   - info.videoFrameRate:  frame rate (double), 0.0 if unknown
 *   - info.videoFrameCount: total frames (int, 0 if unknown)
 *   - info.videoRotation:   display rotation in degrees (0/90/180/270)
 *   - info.audioCodecName:  codec short name (e.g. "aac"). Qt: QString::fromUtf8(...)
 *   - info.audioChannels:   number of audio channels (int)
 *   - info.audioSampleRate: audio sample rate in Hz (int)
 *   - info.audioBitRate:    audio bitrate in bits/s (int64_t)
 *
 * onPlayFinished()
 *   - no parameters
 *
 * onError(msg)
 *   - msg: FFmpeg error or internal error message in UTF-8
 *   - Qt: QString::fromUtf8(msg)
 *
 * onScreenshot(path, success)
 *   - path: screenshot file path (UTF-8). Qt: QString::fromUtf8(path)
 *   - success: true if saved successfully, false otherwise
 *
 * onThumbnailReady(path, success, width, height)
 *   - path: thumbnail file path (UTF-8). Qt: QString::fromUtf8(path)
 *   - success: true if generated successfully
 *   - width / height: thumbnail pixel dimensions
 *   - Notes: invoked on an internal worker thread; do not block. The path
 *     may be empty when success==false.
 *
 * onVideoFrame(data, width, height, pixelFormat)
 *   - data: raw pixel data, pointer valid only during the callback
 *   - width / height: frame dimensions in pixels
 *   - pixelFormat: SmartPixelFormat enum
 *         SP_FMT_YUV420P (1) → YUV 4:2:0 planar
 *         SP_FMT_NV12     (2) → YUV 4:2:0 with interleaved UV
 *         SP_FMT_RGBA     (3) → RGBA, 4 bytes per pixel
 *         SP_FMT_BGRA     (4) → BGRA, 4 bytes per pixel
 *   - Qt: copy data before returning from callback if needed
 */
class SmartPlayerCallback {
public:
    virtual ~SmartPlayerCallback() = default;

    /// Playback state changed
    virtual void onStateChanged(SmartPlayerState /*state*/) {}

    /// Playback progress changed (ms)
    virtual void onPositionChanged(int64_t /*posMs*/) {}

    /// Total duration ready (ms)
    virtual void onDurationChanged(int64_t /*durationMs*/) {}

    /// open result
    virtual void onOpenResult(bool /*success*/, const char* /*errMsg*/) {}

    /// Media info ready
    virtual void onMediaInfoReady(const SmartMediaInfo& /*info*/) {}

    /// Playback finished
    virtual void onPlayFinished() {}

    /// Error
    virtual void onError(const char* /*msg*/) {}

    /// Screenshot completed
    virtual void onScreenshot(const char* /*path*/, bool /*success*/) {}

    /// Thumbnail extraction completed
    virtual void onThumbnailReady(const char* /*path*/, bool /*success*/,
                                  int /*width*/, int /*height*/) {}

    /**
     * Video frame callback — render the frame yourself.
     *
     * Data layout depends on pixelFormat:
     *   YUV420P: data = [Y: w*h][U: w*h/4][V: w*h/4] stored contiguously
     *   NV12:    data = [Y: w*h][interleaved UV: w*h/2]
     *   RGBA:    data = 4 bytes per pixel, w*h*4 total
     *   BGRA:    data = 4 bytes per pixel, w*h*4 total
     *
     * Note: the data pointer is only valid during the callback; copy it
     * if you need to keep the frame.
     */
    virtual void onVideoFrame(const uint8_t* /*data*/, int /*width*/, int /*height*/,
                              SmartPixelFormat /*pixelFormat*/) {}
};

#endif // SMARTPLAYER_CALLBACK_H
