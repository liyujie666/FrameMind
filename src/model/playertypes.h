#ifndef FRAMEMIND_PLAYERTYPES_H
#define FRAMEMIND_PLAYERTYPES_H

#include <QMetaType>

/**
 * 播放器对外状态（屏蔽 SDK 的 SmartPlayerState 枚举值）。
 *
 * 该类型会通过 Qt 信号在 SDK 线程 → 主线程之间用 QueuedConnection 传递，
 * 需在 main() 中 qRegisterMetaType<PlayerState>()。
 */
enum class PlayerState {
    Stopped = 0,
    Playing = 1,
    Paused  = 2,
    Ended   = 3   // 播放到末尾自然结束（区别于 Stopped / Paused）
};

Q_DECLARE_METATYPE(PlayerState)

#endif // FRAMEMIND_PLAYERTYPES_H
