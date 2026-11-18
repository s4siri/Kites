#include "reordertab.h"

#include "toggle_switch.h"

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>
#include <utility>

namespace Kites
{

namespace
{

QPlainTextEdit *makeCodeView(QWidget *parent)
{
    auto *view = new QPlainTextEdit(parent);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    return view;
}

QString percentFaster(int before, int after)
{
    if (before <= 0 || after >= before)
    {
        return QString();
    }
    double percent = 100.0 * (before - after) / before;
    return QString(" (%1% faster)").arg(percent, 0, 'f', 1);
}

} // namespace

ReorderTab::ReorderTab(QWidget *parent) : KitesTab(parent)
{
    auto *layout = new QVBoxLayout(this);

    // --- control row ---------------------------------------------------------------------
    auto *controls = new QHBoxLayout();
    controls->addWidget(new QLabel(tr("Target pipeline:"), this));

    m_noForwardingLabel = new QLabel(tr("No forwarding"), this);
    m_modeSwitch = new ToggleSwitch(this);
    m_modeSwitch->setCheckedInstantly(true);
    m_modeSwitch->setToolTip(tr("Off: no forwarding (every dependent instruction waits for "
                                "write-back).\nOn: forwarding (only load-use hazards stall)."));
    m_forwardingLabel = new QLabel(tr("Forwarding"), this);
    controls->addWidget(m_noForwardingLabel);
    controls->addWidget(m_modeSwitch);
    controls->addWidget(m_forwardingLabel);
    controls->addStretch(1);

    m_reorderButton = new QPushButton(tr("Reorder"), this);
    m_reorderButton->setToolTip(tr("Reorder the program in the main editor for the selected pipeline"));
    m_copyButton = new QPushButton(tr("Copy to main editor"), this);
    m_copyButton->setToolTip(tr("Replace the code in the main editor with the reordered version"));
    m_copyButton->setEnabled(false);
    controls->addWidget(m_reorderButton);
    controls->addWidget(m_copyButton);
    layout->addLayout(controls);

    // --- side by side views --------------------------------------------------------------
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    auto *leftPane = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    m_originalHeader = new QLabel(tr("Original"), leftPane);
    m_originalView = makeCodeView(leftPane);
    leftLayout->addWidget(m_originalHeader);
    leftLayout->addWidget(m_originalView);

    auto *rightPane = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    m_reorderedHeader = new QLabel(tr("Reordered"), rightPane);
    m_reorderedView = makeCodeView(rightPane);
    rightLayout->addWidget(m_reorderedHeader);
    rightLayout->addWidget(m_reorderedView);

    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    // --- summary -------------------------------------------------------------------------
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    layout->addWidget(m_summary);

    showMessage(tr("Press <b>Reorder</b> to analyse the program in the main editor."));
    updateModeLabels();

    connect(m_reorderButton, &QPushButton::clicked, this, &ReorderTab::onReorderClicked);
    connect(m_copyButton, &QPushButton::clicked, this, &ReorderTab::onCopyClicked);
    connect(m_modeSwitch, &QAbstractButton::toggled, this, &ReorderTab::onModeToggled);
}

void ReorderTab::setSourceProvider(std::function<std::string()> provider)
{
    m_sourceProvider = std::move(provider);
}

bool ReorderTab::forwardingEnabled() const
{
    return m_modeSwitch->isChecked();
}

void ReorderTab::setForwardingEnabled(bool enabled)
{
    if (m_modeSwitch->isChecked() == enabled)
    {
        return;
    }
    m_modeSwitch->setChecked(enabled); // triggers onModeToggled()
}

void ReorderTab::onReorderClicked()
{
    runReorder();
}

void ReorderTab::onModeToggled()
{
    updateModeLabels();
    if (m_hasResult)
    {
        runReorder(); // keep the display in step with the switch
    }
}

void ReorderTab::onCopyClicked()
{
    if (m_hasResult)
    {
        emit applyToEditorRequested(QString::fromStdString(m_lastResult.reorderedText));
    }
}

void ReorderTab::updateModeLabels()
{
    QFont active = font();
    active.setBold(true);
    QFont inactive = font();
    inactive.setBold(false);
    const bool forwarding = forwardingEnabled();
    m_forwardingLabel->setFont(forwarding ? active : inactive);
    m_noForwardingLabel->setFont(forwarding ? inactive : active);
}

void ReorderTab::runReorder()
{
    if (!m_sourceProvider)
    {
        return;
    }
    const std::string source = m_sourceProvider();
    if (source.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        m_hasResult = false;
        m_copyButton->setEnabled(false);
        m_originalView->clear();
        m_reorderedView->clear();
        m_originalHeader->setText(tr("Original"));
        m_reorderedHeader->setText(tr("Reordered"));
        showMessage(tr("The main editor is empty - write or open a program first."));
        return;
    }

    const reorder::PipelineMode mode = forwardingEnabled() ? reorder::PipelineMode::Forwarding
                                                           : reorder::PipelineMode::NoForwarding;
    m_lastResult = reorder::reorderProgram(source, mode);
    m_hasResult = true;
    showResult(m_lastResult);
}

void ReorderTab::showResult(const reorder::ReorderResult &result)
{
    const reorder::ReorderStats &stats = result.stats;
    const QString original = QString::fromStdString(result.originalText);
    const QString reordered = QString::fromStdString(result.reorderedText);

    m_originalView->setPlainText(original);
    m_reorderedView->setPlainText(reordered);
    highlightChangedLines(m_reorderedView, original, reordered);

    const int saved = stats.stallsBefore - stats.stallsAfter;
    m_originalHeader->setText(tr("Original  -  %1 stall(s)").arg(stats.stallsBefore));
    m_reorderedHeader->setText(saved > 0
                                   ? tr("Reordered  -  %1 stall(s)  (%2 fewer)")
                                         .arg(stats.stallsAfter)
                                         .arg(saved)
                                   : tr("Reordered  -  %1 stall(s)").arg(stats.stallsAfter));

    QStringList lines;
    lines << tr("<b>Stalls:</b> %1 &rarr; %2 &nbsp;&nbsp; <b>Estimated cycles:</b> %3 &rarr; %4%5 "
                "&nbsp;&nbsp; <b>Instructions:</b> %6 &nbsp;&nbsp; <b>Blocks changed:</b> %7 of %8")
                 .arg(stats.stallsBefore)
                 .arg(stats.stallsAfter)
                 .arg(stats.cyclesBefore())
                 .arg(stats.cyclesAfter())
                 .arg(percentFaster(stats.cyclesBefore(), stats.cyclesAfter()))
                 .arg(stats.instructionCount)
                 .arg(stats.blocksChanged)
                 .arg(stats.blockCount);

    if (saved <= 0)
    {
        lines << tr("No better order was found: either the code already stalls as little as "
                    "possible, or dependencies prevent moving instructions.");
    }
    if (stats.barrierLineCount > 0)
    {
        lines << tr("%1 instruction(s) (ecall, fence, CSR or not recognised, such as custom "
                    "pseudo-instructions) were left in place, and nothing was moved across them.")
                     .arg(stats.barrierLineCount);
    }
    if (stats.usedGreedyFallback)
    {
        lines << tr("Some large blocks used a fast heuristic instead of an exhaustive search, so "
                    "they may not be optimal.");
    }
    lines << tr("<i>Counts are static estimates for straight-line execution. Instructions only "
                "move within a basic block and never past a label, branch or jump.</i>");

    m_summary->setText(lines.join("<br>"));
    m_copyButton->setEnabled(reordered != original);
}

void ReorderTab::showMessage(const QString &message)
{
    m_summary->setText(message);
}

void ReorderTab::highlightChangedLines(QPlainTextEdit *view, const QString &original,
                                       const QString &reordered)
{
    const QStringList before = original.split('\n');
    const QStringList after = reordered.split('\n');

    QColor color = view->palette().color(QPalette::Highlight);
    color.setAlpha(60);

    QList<QTextEdit::ExtraSelection> selections;
    for (int i = 0; i < after.size(); ++i)
    {
        const bool same = i < before.size() && before[i] == after[i];
        if (same)
        {
            continue;
        }
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(color);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = QTextCursor(view->document()->findBlockByNumber(i));
        selection.cursor.clearSelection();
        selections.append(selection);
    }
    view->setExtraSelections(selections);
}

} // namespace Kites
