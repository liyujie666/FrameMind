#include "view/player/playercontrolbar.h"

#include <QHBoxLayout>
#include <QSlider>
#include <QToolButton>
#include <QLabel>
#include <QComboBox>

PlayerControlBar::PlayerControlBar(QWidget* parent)
    : QWidget(parent)
    , m_position(0)
    , m_duration(0)
    , m_dragging(false)
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
    m_playButton->setText(QStringLiteral("\u25B6"));
    m_playButton->setToolTip(tr("播放/暂停"));
    m_playButton->setAutoRaise(true);
    m_playButton->setStyleSheet(QStringLiteral(
        "QToolButton { border:none; padding:4px; border-radius:4px; color:#CCCCCC; background:transparent; }"
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
            &PlayerControlBar::volumeChanged);

    // 静音
    m_muteButton = new QToolButton(this);
    m_muteButton->setText(QStringLiteral("Vol"));
    m_muteButton->setToolTip(tr("静音"));
    m_muteButton->setAutoRaise(true);
    m_muteButton->setStyleSheet(QStringLiteral(
        "QToolButton { border:none; padding:4px; border-radius:4px; color:#CCCCCC; background:transparent; font-size:11px; }"
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
    m_fullscreenButton->setText(QStringLiteral("[]"));
    m_fullscreenButton->setToolTip(tr("全屏"));
    m_fullscreenButton->setAutoRaise(true);
    m_fullscreenButton->setStyleSheet(QStringLiteral(
        "QToolButton { border:none; padding:4px; border-radius:4px; color:#CCCCCC; background:transparent; font-size:11px; }"
        "QToolButton:hover { background:rgba(255,255,255,20); }"));

    layout->addWidget(m_playButton);
    layout->addWidget(m_positionSlider, 1);
    layout->addWidget(m_timeLabel);
    layout->addWidget(m_muteButton);
    layout->addWidget(m_volumeSlider);
    layout->addWidget(m_speedCombo);
    layout->addWidget(m_fullscreenButton);
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
    if (state == PlayerState::Playing) {
        m_playButton->setText(QStringLiteral("\u23F8"));  // ⏸
    } else {
        m_playButton->setText(QStringLiteral("\u25B6"));  // ▶
    }
}

void PlayerControlBar::setVolumeDisplay(int vol)
{
    QSignalBlocker blocker(m_volumeSlider);
    m_volumeSlider->setValue(qBound(0, vol, 100));
}
