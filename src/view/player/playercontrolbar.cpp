#include "view/player/playercontrolbar.h"

#include <QHBoxLayout>
#include <QSlider>
#include <QToolButton>
#include <QLabel>
#include <QComboBox>

PlayerControlBar::PlayerControlBar(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(8);

    // 播放 / 暂停
    m_playButton = new QToolButton(this);
    m_playButton->setText(QStringLiteral("▶"));
    m_playButton->setToolTip(tr("播放/暂停"));
    m_playButton->setAutoRaise(true);
    connect(m_playButton, &QToolButton::clicked, this, &PlayerControlBar::playClicked);

    // 进度条
    m_positionSlider = new QSlider(Qt::Horizontal, this);
    m_positionSlider->setRange(0, 0);
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
    m_timeLabel->setMinimumWidth(110);
    m_timeLabel->setAlignment(Qt::AlignCenter);

    // 静音
    m_muteButton = new QToolButton(this);
    m_muteButton->setText(QStringLiteral("🔊"));
    m_muteButton->setToolTip(tr("静音"));
    m_muteButton->setAutoRaise(true);
    connect(m_muteButton, &QToolButton::clicked, this, &PlayerControlBar::muteClicked);

    // 音量
    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(50);
    m_volumeSlider->setFixedWidth(90);
    connect(m_volumeSlider, &QSlider::valueChanged, this,
            &PlayerControlBar::volumeChanged);

    // 倍速
    m_speedCombo = new QComboBox(this);
    m_speedCombo->addItem(QStringLiteral("0.5x"), 0.5f);
    m_speedCombo->addItem(QStringLiteral("1.0x"), 1.0f);
    m_speedCombo->addItem(QStringLiteral("1.25x"), 1.25f);
    m_speedCombo->addItem(QStringLiteral("1.5x"), 1.5f);
    m_speedCombo->addItem(QStringLiteral("2.0x"), 2.0f);
    m_speedCombo->setCurrentIndex(1);  // 1.0x
    connect(m_speedCombo, &QComboBox::currentIndexChanged, this,
            [this](int) {
                emit speedChanged(m_speedCombo->currentData().toFloat());
            });

    // 全屏占位（M1 暂不实现功能）
    m_fullscreenButton = new QToolButton(this);
    m_fullscreenButton->setText(QStringLiteral("⛶"));
    m_fullscreenButton->setToolTip(tr("全屏（待实现）"));
    m_fullscreenButton->setAutoRaise(true);
    m_fullscreenButton->setEnabled(false);

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
        m_playButton->setText(QStringLiteral("⏸"));
    } else {
        m_playButton->setText(QStringLiteral("▶"));
    }
}

void PlayerControlBar::setVolumeDisplay(int vol)
{
    QSignalBlocker blocker(m_volumeSlider);
    m_volumeSlider->setValue(qBound(0, vol, 100));
}
