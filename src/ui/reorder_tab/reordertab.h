/**
 * @file reordertab.h
 * @brief The "Reorder" tab: shows the program in the main editor next to a reordered copy, with
 * before/after stall counts, for either a forwarding or a non-forwarding pipeline.
 *
 * The tab is built in code (no .ui file) and does not know about the editor; MainWindow gives it
 * a function that returns the current source and connects applyToEditorRequested() to the editor.
 */

#pragma once

#include "reorder/code_reorderer.h"
#include "ui/common/kitestab.h"

#include <QString>
#include <functional>
#include <string>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace Kites
{

class ToggleSwitch;

class ReorderTab : public KitesTab
{
    Q_OBJECT

  public:
    explicit ReorderTab(QWidget *parent = nullptr);

    /// Function that returns the assembly currently in the main editor.
    void setSourceProvider(std::function<std::string()> provider);

    /// Selects the forwarding / no-forwarding mode (re-runs the reordering if a result is shown).
    void setForwardingEnabled(bool enabled);
    bool forwardingEnabled() const;

  signals:
    /// The user pressed "Copy to main editor".
    void applyToEditorRequested(const QString &reorderedText);

  private slots:
    void onReorderClicked();
    void onModeToggled();
    void onCopyClicked();

  private:
    void runReorder();
    void showResult(const reorder::ReorderResult &result);
    void showMessage(const QString &message);
    void updateModeLabels();
    static void highlightChangedLines(QPlainTextEdit *view, const QString &original,
                                      const QString &reordered);

    std::function<std::string()> m_sourceProvider;
    reorder::ReorderResult m_lastResult;
    bool m_hasResult = false;

    ToggleSwitch *m_modeSwitch = nullptr;
    QLabel *m_noForwardingLabel = nullptr;
    QLabel *m_forwardingLabel = nullptr;
    QPushButton *m_reorderButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QLabel *m_originalHeader = nullptr;
    QLabel *m_reorderedHeader = nullptr;
    QPlainTextEdit *m_originalView = nullptr;
    QPlainTextEdit *m_reorderedView = nullptr;
    QLabel *m_summary = nullptr;
};

} // namespace Kites
