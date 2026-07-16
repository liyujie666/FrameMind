#include "view/player/playercontrolbar.h"

#include <QHBoxLayout>
#include <QSlider>
#include <QToolButton>
#include <QLabel>
#include <QComboBox>
#include <QIcon>
#include <QSizePolicy>

#include "service/themeservice.h"

PlayerControlBar::PlayerControlBar(QWidget* parent)
    : QWidget(parent)
    , m_position(0)
    , m_duration(0)
    , m_dragging(false)
    , m_themeService(nullptr)
    , m_isMuted(false)
    , m_currentVolume(50)
{
    setFixedHeight(40);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    setStyleSheet("background-color: rgba(20, 20, 20, 180);");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(12);

    // 播放 / 暂停
    m_playButton = new QToolButton(this);
    m_playButton->setToolTip(tr("播放/暂停"));
    m_playButton->setAutoRaise(true);
    m_playButton->setIconSize(QSize(24, 24));
    m_playButton->setStyleSheet(QStringLiteral(
        "QToolButton { border:none; padding:4px; border-radius:4px; background:transparent; }"
        "QToolButton:hover { background:rgba(255,255,255,20); }"));
    connect(m_playButton, &QToolButton::clicked, this, &PlayerControlBar::playClicked);

    // 进度条
    m_positionSlider = new QSlider(Qt::Horizontal, this);
    m_positionSlider->setRange(0, 0);
    m_positionSlider->setStyleSheet(QStringLiteral(
        "QSlider { background:transparent; }"
        "QSlider::groove:horizontal { background:rgba(255,255,255,30); height:4px; border-radius:2px; }"
        "QSlider::handle:horizontal { background:#2979FF; width:12px; height:12px; margin:-4px 0; border-radius:6px; }"
        "QSlider::sub-page:horizontal { background:#2979FF; border-radius:2px; }"));
    connect(m_positionSlider, &QSlider::sliderPressed, this,
            [this]() { m_dragging = true; });
    connect(m_positionSlider, &QSlider::sliderMoved, this,
            [this](int value) {
                m_position = value;
                updateTimeLabel();
            });
    connect(m_positionSlider, &QSlider::sliderReleased, this,
            [this]() {
                m_dragging = false;
                emit seekRequested(static_cast<int64_t>(m_positionSlider->value()));
            });

    // 时间显示
    m_timeLabel = new QLabel(QStringLiteral("00:00 / 00:00"), this);
    m_timeLabel->setMinimumWidth(100);
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setStyleSheet(QStringLiteral(
        "QLabel { color:#CCCCCC; font-size:12px; background:transparent; }"));

    // 音量
    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(50);
    m_volumeSlider->setFixedWidth(80);
    m_volumeSlider->setStyleSheet(QStringLiteral(
        "QSlider { background:transparent; }"
        "QSlider::groove:horizontal { background:rgba(255,255,255,30); height:4px; border-radius:2px; }"
        "QSlider::handle:horizontal { background:#CCCCCC; width:10px; height:10px; margin:-3px 0; border-radius:5px; }"
        "QSlider::sub-page:horizontal { background:#CCCCCC; border-radius:2px; }"));
    connect(m_volumeSlider, &QSlider::valueChanged, this,
            [this](int value) {
                m_currentVolume = value;
                updateVolumeIcon(m_isMuted, value);
                emit volumeChanged(value);
            });

    // 静音
    m_muteButton = new QToolButton(this);
    m_muteButton->setToolTip(tr("静音"));
    m_muteButton->setAutoRaise(true);
    m_muteButton->setIconSize(QSize(24, 24));
    m_muteButton->setStyleSheet(QStringLiteral(
        "QToolButton { border:none; padding:4px; border-radius:4px; background:transparent; }"
        "QToolButton:hover { background:rgba(255,255,255,20); }"));
    connect(m_muteButton, &QToolButton::clicked, this, &PlayerControlBar::muteClicked);

    // 倍速
    m_speedCombo = new QComboBox(this);
    m_speedCombo->addItem(QStringLiteral("0.5x"), 0.5f);
    m_speedCombo->addItem(QStringLiteral("1.0x"), 1.0f);
    m_speedCombo->addItem(QStringLiteral("1.25x"), 1.25f);
    m_speedCombo->addItem(QStringLiteral("1.5x"), 1.5f);
    m_speedCombo->addItem(QStringLiteral("2.0x"), 2.0f);
    m_speedCombo->setCurrentIndex(1);
    m_speedCombo->setStyleSheet(QStringLiteral(
        "QComboBox { background:rgba(255,255,255,20); color:#CCCCCC; border:1px solid rgba(255,255,255,30); "
        "border-radius:4px; padding:4px 8px; }"
        "QComboBox:hover { border-color:#2979FF; }"
        "QComboBox::drop-down { border:none; }"
        "QComboBox QAbstractItemView { color:#FFFFFF; background:#141414; border-radius:4px; selection-background-color:#2979FF; }"));
    connect(m_speedCombo, &QComboBox::currentIndexChanged, this,
            [this](int) {
                emit speedChanged(m_speedCombo->currentData().toFloat());
            });

    // 全屏
    m_fullscreenButton = new QToolButton(this);
    m_fullscreenButton->setToolTip(tr("全屏"));
    m_fullscreenButton->setAutoRaise(true);
    m_fullscreenButton->setIconSize(QSize(24, 24));
    m_fullscreenButton->setStyleSheet(QStringLiteral(
        "QToolButton { border:none; padding:4px; border-radius:4px; background:transparent; }"
        "QToolButton:hover { background:rgba(255,255,255,20); }"));
    connect(m_fullscreenButton, &QToolButton::clicked, this, &PlayerControlBar::fullscreenClicked);

    layout->addWidget(m_playButton);
    layout->addWidget(m_positionSlider, 1);
    layout->addWidget(m_timeLabel);
    layout->addWidget(m_muteButton);
    layout->addWidget(m_volumeSlider);
    layout->addWidget(m_speedCombo);
    layout->addWidget(m_fullscreenButton);

    updateIcons();
}

void PlayerControlBar::setThemeService(ThemeService* theme)
{
    m_themeService = theme;
    updateIcons();
}

void PlayerControlBar::updateIcons()
{
    // 播放器始终使用亮色图标（在视频播放控件上）
    // 图标文件使用 play_light.png, pause_light.png, volume_light.png, mute_light.png, fullscreen_light.png

    // 播放/暂停图标
    updatePlayPauseIcon(m_lastPlayState);

    // 音量图标
    updateVolumeIcon(m_isMuted, m_currentVolume);

    // 全屏图标
    updateFullscreenIcon();
}

void PlayerControlBar::updateFullscreenIcon()
{
    if (m_isFullscreen) {
        // 使用 emptyscreen 图标表示"退出全屏"
        QIcon exitIcon(":/icons/emptyscreen_light.png");
        m_fullscreenButton->setIcon(exitIcon);
        m_fullscreenButton->setToolTip(tr("退出全屏"));
    } else {
        QIcon fullscreenIcon(":/icons/fullscreen_light.png");
        m_fullscreenButton->setIcon(fullscreenIcon);
        m_fullscreenButton->setToolTip(tr("全屏"));
    }
}

void PlayerControlBar::setFullscreen(bool fullscreen)
{
    if (m_isFullscreen == fullscreen) return;
    m_isFullscreen = fullscreen;
    updateFullscreenIcon();
}

void PlayerControlBar::updatePlayPauseIcon(PlayerState state)
{
    m_lastPlayState = state;
    if (state == PlayerState::Playing) {
        QIcon pauseIcon(":/icons/pause_light.png");
        m_playButton->setIcon(pauseIcon);
    } else {
        QIcon playIcon(":/icons/play_light.png");
        m_playButton->setIcon(playIcon);
    }
}

void PlayerControlBar::updateVolumeIcon(bool muted, int volume)
{
    if (muted || volume == 0) {
        QIcon muteIcon(":/icons/mute_light.png");
        m_muteButton->setIcon(muteIcon);
    } else {
        QIcon volumeIcon(":/icons/volume_light.png");
        m_muteButton->setIcon(volumeIcon);
    }
}

void PlayerControlBar::setMuted(bool muted)
{
    m_isMuted = muted;
    updateVolumeIcon(muted, m_currentVolume);
}

QString PlayerControlBar::formatTime(int64_t ms)
{
    if (ms < 0) ms = 0;
    const int64_t totalSec = ms / 1000;
    const int64_t s = totalSec % 60;
    const int64_t m = (totalSec / 60) % 60;
    const int64_t h = totalSec / 3600;
    if (h > 0) {
        return QString::asprintf("%lld:%02lld:%02lld",
                                 (long long)h, (long long)m, (long long)s);
    }
    return QString::asprintf("%02lld:%02lld", (long long)m, (long long)s);
}

void PlayerControlBar::updateTimeLabel()
{
    m_timeLabel->setText(formatTime(m_position) + QStringLiteral(" / ")
                         + formatTime(m_duration));
}

void PlayerControlBar::setPosition(int64_t posMs)
{
    m_position = posMs;
    if (!m_dragging) {
        QSignalBlocker blocker(m_positionSlider);
        m_positionSlider->setValue(static_cast<int>(posMs));
        updateTimeLabel();
    }
}

void PlayerControlBar::setDuration(int64_t durationMs)
{
    m_duration = durationMs;
    m_positionSlider->setRange(0, static_cast<int>(durationMs));
    updateTimeLabel();
}

void PlayerControlBar::setPlayState(PlayerState state)
{
    updatePlayPauseIcon(state);
}

void PlayerControlBar::setVolumeDisplay(int vol)
{
    QSignalBlocker blocker(m_volumeSlider);
    m_volumeSlider->setValue(qBound(0, vol, 100));
}
