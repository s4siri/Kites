#include "toggle_switch.h"

#include <QPainter>
#include <QPainterPath>

namespace Kites
{

namespace
{
constexpr int kTrackWidth = 44;
constexpr int kTrackHeight = 22;
constexpr int kKnobMargin = 3;
} // namespace

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_animation = new QPropertyAnimation(this, "knobPosition", this);
    m_animation->setDuration(120);

    connect(this, &QAbstractButton::toggled, this, &ToggleSwitch::animateTo);
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(kTrackWidth, kTrackHeight);
}

qreal ToggleSwitch::knobPosition() const
{
    return m_knobPosition;
}

void ToggleSwitch::setKnobPosition(qreal position)
{
    m_knobPosition = position;
    update();
}

void ToggleSwitch::setCheckedInstantly(bool checked)
{
    QSignalBlocker blocker(this);
    setChecked(checked);
    m_animation->stop();
    setKnobPosition(checked ? 1.0 : 0.0);
}

void ToggleSwitch::nextCheckState()
{
    setChecked(!isChecked());
}

void ToggleSwitch::animateTo(bool checked)
{
    m_animation->stop();
    m_animation->setStartValue(m_knobPosition);
    m_animation->setEndValue(checked ? 1.0 : 0.0);
    m_animation->start();
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPalette &colors = palette();
    QColor offColor = colors.color(QPalette::Mid);
    QColor onColor = colors.color(QPalette::Highlight);
    if (!isEnabled())
    {
        offColor = colors.color(QPalette::Disabled, QPalette::Mid);
        onColor = colors.color(QPalette::Disabled, QPalette::Highlight);
    }

    // Blend the track colour with the knob position so it fades while sliding.
    QColor track(static_cast<int>(offColor.red() + (onColor.red() - offColor.red()) * m_knobPosition),
                 static_cast<int>(offColor.green() + (onColor.green() - offColor.green()) * m_knobPosition),
                 static_cast<int>(offColor.blue() + (onColor.blue() - offColor.blue()) * m_knobPosition));

    const QRectF trackRect((width() - kTrackWidth) / 2.0, (height() - kTrackHeight) / 2.0,
                           kTrackWidth, kTrackHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(trackRect, kTrackHeight / 2.0, kTrackHeight / 2.0);

    const qreal knobDiameter = kTrackHeight - 2 * kKnobMargin;
    const qreal travel = kTrackWidth - kTrackHeight;
    const qreal knobX = trackRect.left() + kKnobMargin + travel * m_knobPosition;
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawEllipse(QRectF(knobX, trackRect.top() + kKnobMargin, knobDiameter, knobDiameter));

    if (hasFocus())
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(colors.color(QPalette::Highlight), 1));
        painter.drawRoundedRect(trackRect.adjusted(-2, -2, 2, 2), (kTrackHeight + 4) / 2.0,
                                (kTrackHeight + 4) / 2.0);
    }
}

} // namespace Kites
