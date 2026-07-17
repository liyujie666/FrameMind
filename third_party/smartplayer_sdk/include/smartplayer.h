#ifndef SMARTPLAYER_H
#define SMARTPLAYER_H

#include <memory>
#include <string>
#include "smartplayerdefs.h"
#include "smartplayercallback.h"


class SMARTPLAYER_API SmartPlayer {
public:
    SmartPlayer();
    ~SmartPlayer();

    SmartPlayer(const SmartPlayer&) = delete;
    SmartPlayer& operator=(const SmartPlayer&) = delete;

    void open(const char* url);
    void play();
    void pause();
    void stop();
    void seek(int64_t posMs);

    void setSpeed(float speed);
    void setVolume(int volume);
    void setMute(bool mute);
    void setHardwareDecode(bool enable);
    void setDecoderType(const char* decoder);
    void takeScreenshot(const char* savePath);

    SmartPlayerState state() const;
    int64_t duration() const;       // ms
    int64_t position() const;       // ms
    bool    hasAudio() const;
    bool    hasVideo() const;
    SmartMediaType mediaType() const;
    const SmartMediaInfo& mediaInfo() const;

    // ===== Thumbnail extraction =====
    //
    // `extractThumbnail` is a standalone utility — it opens the file on
    // its own (no need for an existing SmartPlayer instance) and is the
    // canonical way to build a thumbnail gallery without spinning up the
    // full playback pipeline.
    //
    // `takeThumbnailAsync` is a convenience wrapper that runs in the
    // background and delivers the result via SmartPlayerCallback::
    // onThumbnailReady() — useful when you already have a player open and
    // want to share the callback flow with screenshot notifications.

    struct ThumbnailOptions {
        int64_t positionMs   = -1;   // <0 → pick 10% of duration
        int     targetWidth  = 320;  // 0 → keep native size
        int     jpegQuality  = 2;    // 1..5 (lower = better)
    };

    /**
     * Synchronously extract one thumbnail frame from a media file.
     *
     * @param mediaUrl  local file or URL
     * @param savePath  output directory OR a full output path
     *                  (auto-detected by file extension). The directory
     *                  will be created if it does not exist.
     * @param opts      extraction options
     * @return          true on success
     */
    static bool extractThumbnail(const char* mediaUrl,
                                 const char* savePath,
                                 const ThumbnailOptions& opts = ThumbnailOptions());

    /**
     * Asynchronously extract a thumbnail from the currently-open media.
     * Result is delivered through SmartPlayerCallback::onThumbnailReady().
     *
     * Safe to call before play() (uses the player's cached media path) or
     * while playback is running. Internally decodes a fresh copy of the
     * frame so it does not disturb the playback pipeline.
     *
     * @return false if no media has been opened yet
     */
    bool takeThumbnailAsync(const char* savePath,
                            const ThumbnailOptions& opts = ThumbnailOptions());

    // Callback
    void setCallback(SmartPlayerCallback* callback);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

#endif // SMARTPLAYER_H
