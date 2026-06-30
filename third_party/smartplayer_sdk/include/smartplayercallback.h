#ifndef SMARTPLAYER_CALLBACK_H
#define SMARTPLAYER_CALLBACK_H

#include <cstdint>
#include <string>
#include "smartplayerdefs.h"

/**
 * 回调接口 —— 用户继承此类，实现需要的事件，注册给 SmartPlayer。
 * 所有回调在播放器内部线程调用，用户不应在其中执行耗时操作或阻塞。
 */
class SmartPlayerCallback {
public:
    virtual ~SmartPlayerCallback() = default;

    /// 播放状态变化
    virtual void onStateChanged(SmartPlayerState /*state*/) {}

    /// 播放进度变化 (ms)
    virtual void onPositionChanged(int64_t /*posMs*/) {}

    /// 总时长就绪 (ms)
    virtual void onDurationChanged(int64_t /*durationMs*/) {}

    /// open 结果
    virtual void onOpenResult(bool /*success*/, const std::string& /*errMsg*/) {}

    /// 媒体信息就绪
    virtual void onMediaInfoReady(const SmartMediaInfo& /*info*/) {}

    /// 播放结束
    virtual void onPlayFinished() {}

    /// 错误
    virtual void onError(const std::string& /*msg*/) {}

    /// 截图完成
    virtual void onScreenshot(const std::string& /*path*/, bool /*success*/) {}

    /**
     * 视频帧回调 —— 上层自行渲染。
     *
     * 数据布局由 pixelFormat 决定：
     *   YUV420P: data = [Y: w*h][U: w*h/4][V: w*h/4] 连续存储
     *   NV12:    data = [Y: w*h][interleaved UV: w*h/2]
     *   RGBA:    data = 每像素 4 字节，共 w*h*4
     *   BGRA:    data = 每像素 4 字节，共 w*h*4
     *
     * 注意: data 指针仅在回调期间有效，如需保留请拷贝。
     */
    virtual void onVideoFrame(const uint8_t* /*data*/, int /*width*/, int /*height*/,
                              SmartPixelFormat /*pixelFormat*/) {}
};

#endif // SMARTPLAYER_CALLBACK_H
