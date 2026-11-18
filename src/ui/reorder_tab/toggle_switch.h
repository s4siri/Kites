/**
 * @file toggle_switch.h
 * @brief A small sliding on/off switch (like the ones on phones) built on QAbstractButton.
 */

#pragma once

#include <QAbstractButton>
#include <QPropertyAnimation>

namespace Kites
{

class ToggleSwitch : public QAbstractButton
{
    Q_OBJECT
    Q_PROPERTY(qreal knobPosition READ knobPosition WRITE setKnobPosition)

  public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    qreal knobPosition() const;
    void setKnobPosition(qreal position);

    /// Sets the state without playing the slide animation.
    void setCheckedInstantly(bool checked);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void nextCheckState() override;

  private:
    void animateTo(bool checked);

    qreal m_knobPosition = 0.0; ///< 0.0 = off (left), 1.0 = on (right)
    QPropertyAnimation *m_animation = nullptr;
};

} // namespace Kites
