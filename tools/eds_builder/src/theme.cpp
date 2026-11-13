/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTextCharFormat>

namespace {

Theme g_theme;

Theme lightTheme()
{
    Theme t;
    t.dark = false;
    t.window = QColor(0xEC, 0xEF, 0xF3);
    t.base = QColor(0xFF, 0xFF, 0xFF);
    t.alternate = QColor(0xF3, 0xF5, 0xF8);
    t.text = QColor(0x11, 0x18, 0x27);
    t.muted = QColor(0x4B, 0x55, 0x63);
    t.border = QColor(0x8A, 0x94, 0xA3);
    t.accent = QColor(0x1D, 0x4E, 0xD8);
    t.error = QColor(0xB9, 0x1C, 0x1C);
    t.warning = QColor(0x92, 0x40, 0x0E);
    t.ok = QColor(0x16, 0x65, 0x34);
    t.edsComment = QColor(0x4D, 0x6B, 0x53);
    t.edsString = QColor(0x9A, 0x34, 0x12);
    t.edsNumber = QColor(0x6D, 0x28, 0xD9);
    return t;
}

Theme darkTheme()
{
    Theme t;
    t.dark = true;
    t.window = QColor(0x20, 0x24, 0x2B);
    t.base = QColor(0x14, 0x17, 0x1C);
    t.alternate = QColor(0x1B, 0x1F, 0x26);
    t.text = QColor(0xE8, 0xEA, 0xED);
    t.muted = QColor(0xA8, 0xB0, 0xBB);
    t.border = QColor(0x5C, 0x66, 0x75);
    t.accent = QColor(0x7A, 0xB0, 0xFF);
    t.error = QColor(0xFF, 0x8A, 0x80);
    t.warning = QColor(0xFF, 0xC1, 0x6B);
    t.ok = QColor(0x6E, 0xE7, 0x9A);
    t.edsComment = QColor(0x8F, 0xB3, 0x96);
    t.edsString = QColor(0xF2, 0xA8, 0x7A);
    t.edsNumber = QColor(0xC4, 0xA7, 0xFF);
    return t;
}

QString css(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

} /* namespace */

const Theme &theme()
{
    return g_theme;
}

void applyTheme(QApplication &app)
{
    const QByteArray forced = qgetenv("EDS_BUILDER_THEME").toLower();
    bool dark = (app.styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    if (forced == "dark") {
        dark = true;
    } else if (forced == "light") {
        dark = false;
    }
    g_theme = dark ? darkTheme() : lightTheme();
    const Theme &t = g_theme;

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFont font = app.font();
    font.setPointSizeF(qMax(font.pointSizeF(), 10.0));
    app.setFont(font);

    const QColor selectedText = dark ? QColor(0x10, 0x14, 0x1A) : QColor(Qt::white);
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.base);
    p.setColor(QPalette::AlternateBase, t.alternate);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, dark ? QColor(0x2C, 0x32, 0x3B) : QColor(0xFB, 0xFC, 0xFD));
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::ToolTipBase, t.base);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.muted);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, selectedText);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::Mid, t.border);
    p.setColor(QPalette::Dark, t.border);
    p.setColor(QPalette::Disabled, QPalette::Text, t.muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.muted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.muted);
    app.setPalette(p);

    /* Borders and spacing the palette cannot express. */
    const QString sheet = QStringLiteral(R"(
        QLineEdit, QPlainTextEdit, QListWidget, QTableWidget {
            border: 1px solid %border%; border-radius: 4px; background: %base%; color: %text%;
        }
        QLineEdit { padding: 4px 6px; min-height: 20px; }
        /* Spin boxes and combo boxes keep the Fusion drawing: a style sheet border would cost them their arrows. */
        QSpinBox, QComboBox { min-height: 26px; }
        QLineEdit:focus, QTableWidget:focus, QPlainTextEdit:focus {
            border: 2px solid %accent%;
        }
        QGroupBox {
            border: 1px solid %border%; border-radius: 6px; margin-top: 14px; padding: 12px 10px 10px 10px;
            font-weight: 600;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: %accent%; }
        QGroupBox QLabel, QGroupBox QCheckBox { font-weight: 400; }
        QPushButton { padding: 6px 16px; border: 1px solid %border%; border-radius: 4px; }
        QPushButton:hover { border-color: %accent%; }
        QPushButton:default { background: %accent%; color: %selected%; border-color: %accent%; font-weight: 600; }
        QTabWidget::pane { border: 1px solid %border%; border-radius: 4px; top: -1px; }
        QTabBar::tab {
            padding: 7px 16px; border: 1px solid %border%; border-bottom: none; margin-right: 2px;
            border-top-left-radius: 4px; border-top-right-radius: 4px; color: %muted%; background: %alternate%;
        }
        QTabBar::tab:selected { color: %text%; background: %window%; font-weight: 600; border-top: 3px solid %accent%; }
        QHeaderView::section {
            background: %alternate%; color: %text%; padding: 5px 8px; border: none;
            border-right: 1px solid %border%; border-bottom: 1px solid %border%; font-weight: 600;
        }
        QTableWidget { gridline-color: %border%; }
        QListWidget::item { padding: 5px 4px; }
        QLabel#hint { color: %muted%; }
        QLabel#notes { color: %text%; background: %alternate%; border: 1px solid %border%; border-radius: 4px; padding: 6px 8px; }
        QToolTip { border: 1px solid %border%; background: %base%; color: %text%; padding: 4px; }
    )")
                              .replace(QLatin1String("%border%"), css(t.border))
                              .replace(QLatin1String("%base%"), css(t.base))
                              .replace(QLatin1String("%text%"), css(t.text))
                              .replace(QLatin1String("%muted%"), css(t.muted))
                              .replace(QLatin1String("%accent%"), css(t.accent))
                              .replace(QLatin1String("%alternate%"), css(t.alternate))
                              .replace(QLatin1String("%window%"), css(t.window))
                              .replace(QLatin1String("%selected%"), css(selectedText));
    app.setStyleSheet(sheet);
}

EdsHighlighter::EdsHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}

void EdsHighlighter::highlightBlock(const QString &text)
{
    const Theme &t = theme();
    static const QRegularExpression section(QStringLiteral("^\\s*\\[[^\\]]+\\]"));
    static const QRegularExpression keyword(QStringLiteral("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*="));
    static const QRegularExpression number(QStringLiteral("\\b(0x[0-9A-Fa-f]+|\\d+)\\b"));
    static const QRegularExpression string(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\""));

    QTextCharFormat f;
    QRegularExpressionMatch m = section.match(text);
    if (m.hasMatch()) {
        f.setForeground(t.accent);
        f.setFontWeight(QFont::Bold);
        setFormat(0, int(text.size()), f);
        return;
    }

    /* A comment runs from a $ outside a string to the end of the line. */
    int comment = -1;
    bool inString = false;
    for (int i = 0; i < text.size(); i++) {
        if (text[i] == QLatin1Char('\\') && inString) {
            i++;
        } else if (text[i] == QLatin1Char('"')) {
            inString = !inString;
        } else if (text[i] == QLatin1Char('$') && !inString) {
            comment = i;
            break;
        }
    }
    const QString code = comment < 0 ? text : text.left(comment);

    f = QTextCharFormat();
    f.setForeground(t.edsNumber);
    for (QRegularExpressionMatchIterator it = number.globalMatch(code); it.hasNext();) {
        m = it.next();
        setFormat(int(m.capturedStart()), int(m.capturedLength()), f);
    }
    m = keyword.match(code);
    if (m.hasMatch()) {
        f = QTextCharFormat();
        f.setForeground(t.text);
        f.setFontWeight(QFont::Bold);
        setFormat(int(m.capturedStart(1)), int(m.capturedLength(1)), f);
    }
    f = QTextCharFormat();
    f.setForeground(t.edsString);
    for (QRegularExpressionMatchIterator it = string.globalMatch(code); it.hasNext();) {
        m = it.next();
        setFormat(int(m.capturedStart()), int(m.capturedLength()), f);
    }
    if (comment >= 0) {
        f = QTextCharFormat();
        f.setForeground(t.edsComment);
        f.setFontItalic(true);
        setFormat(comment, int(text.size()) - comment, f);
    }
}
