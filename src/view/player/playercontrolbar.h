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
class ThemeService;

class PlayerControlBar : public QWidget {
    Q_OBJECT
public:
    explicit PlayerControlBar(QWidget* parent = nullptr);
    void setThemeService(ThemeService* theme);

public slots:
    void setPosition(int64_t posMs);
    void setDuration(int64_t durationMs);
    void setPlayState(PlayerState state);
    void setVolumeDisplay(int vol);
    void setMuted(bool muted);

public slots:
    void setFullscreen(bool fullscreen);

signals:
    void seekRequested(int64_t posMs);
    void playClicked();
    void volumeChanged(int vol);
    void speedChanged(float speed);
    void muteClicked();
    void fullscreenClicked();

private:
    static QString formatTime(int64_t ms);
    void updateTimeLabel();
    void updateIcons();
    void updatePlayPauseIcon(PlayerState state);
    void updateVolumeIcon(bool muted, int volume);
    void updateFullscreenIcon();

    QSlider*     m_positionSlider = nullptr;
    QToolButton* m_playButton = nullptr;
    QToolButton* m_muteButton = nullptr;
    QToolButton* m_fullscreenButton = nullptr;
    QLabel*      m_timeLabel = nullptr;
    QSlider*     m_volumeSlider = nullptr;
    QComboBox*   m_speedCombo = nullptr;
    ThemeService* m_themeService = nullptr;
    bool         m_isMuted = false;
    bool         m_isFullscreen = false;
    int          m_currentVolume = 50;
    PlayerState  m_lastPlayState = PlayerState::Stopped;

    int64_t m_position = 0;
    int64_t m_duration = 0;
    bool    m_dragging = false;
};

#endif // FRAMEMIND_PLAYERCONTROLBAR_H
