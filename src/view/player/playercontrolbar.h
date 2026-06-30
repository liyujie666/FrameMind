#ifndef FRAMEMIND_PLAYERCONTROLBAR_H
#define FRAMEMIND_PLAYERCONTROLBAR_H

#include <QWidget>
#include <cstdint>

#include "model/playertypes.h"

class QSlider;
class QToolButton;
class QLabel;
class QComboBox;
class QPaintEvent;

/**
 * 播放控制栏：进度条 + 播放/暂停 + 时长 + 音量 + 倍速 + 全屏占位。
 *
 * 控制栏不直接调用 PlayerService，所有用户操作通过信号上抛给 ViewModel；
 * 位置/时长/状态由 ViewModel 推送（setPosition / setDuration / setPlayState）。
 */
class PlayerControlBar : public QWidget {
    Q_OBJECT
public:
    explicit PlayerControlBar(QWidget* parent = nullptr);

public slots:
    void setPosition(int64_t posMs);
    void setDuration(int64_t durationMs);
    void setPlayState(PlayerState state);
    void setVolumeDisplay(int vol);

signals:
    void seekRequested(int64_t posMs);
    void playClicked();
    void volumeChanged(int vol);
    void speedChanged(float speed);
    void muteClicked();

private:
    static QString formatTime(int64_t ms);
    void updateTimeLabel();

    QSlider*     m_positionSlider = nullptr;
    QToolButton* m_playButton = nullptr;
    QToolButton* m_muteButton = nullptr;
    QToolButton* m_fullscreenButton = nullptr;
    QLabel*      m_timeLabel = nullptr;
    QSlider*     m_volumeSlider = nullptr;
    QComboBox*   m_speedCombo = nullptr;

    int64_t m_position = 0;
    int64_t m_duration = 0;
    bool    m_dragging = false;
};

#endif // FRAMEMIND_PLAYERCONTROLBAR_H
