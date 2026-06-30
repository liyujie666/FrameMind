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

    void open(const std::string& url);
    void play();
    void pause();
    void stop();
    void seek(int64_t posMs);

    void setSpeed(float speed);
    void setVolume(int volume);
    void setMute(bool mute);
    void setHardwareDecode(bool enable);
    void setDecoderType(const std::string& decoder);
    void takeScreenshot(const std::string& savePath);

    SmartPlayerState state() const;
    int64_t duration() const;       // ms
    int64_t position() const;       // ms
    bool    hasAudio() const;
    bool    hasVideo() const;
    SmartMediaType mediaType() const;
    const SmartMediaInfo& mediaInfo() const;

    // 回调
    void setCallback(SmartPlayerCallback* callback);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

#endif // SMARTPLAYER_H
