/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* One place for every colour the window uses. The native Windows 11 style
 * draws fields with hairline borders on a near-white page, which is hard to
 * read, so the application uses the Fusion style with its own palette: dark
 * text, visible field borders, and a light or dark set chosen from the
 * system setting. */
#ifndef THEME_H_
#define THEME_H_

#include <QColor>
#include <QSyntaxHighlighter>

class QApplication;

struct Theme {
    bool dark = false;
    QColor window;      /* page background */
    QColor base;        /* fields, tables, the preview */
    QColor alternate;   /* every other table row */
    QColor text;
    QColor muted;       /* hints and secondary text */
    QColor border;
    QColor accent;      /* focus, selection, section names */
    QColor error;
    QColor warning;
    QColor ok;
    QColor edsComment;
    QColor edsString;
    QColor edsNumber;
};

/* The theme in use. Valid after applyTheme. */
const Theme &theme();

/* Set the style, palette and style sheet. EDS_BUILDER_THEME=light or dark
 * overrides the system setting. */
void applyTheme(QApplication &app);

/* Colours the EDS preview: sections, entry keywords, strings, numbers, comments. */
class EdsHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit EdsHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;
};

#endif /* THEME_H_ */
