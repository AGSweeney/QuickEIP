/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#ifndef MAINWINDOW_H_
#define MAINWINDOW_H_

#include <QMainWindow>

#include "edsmodel.h"
#include "edsvalidator.h"
#include "projectscanner.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

/* The model is the truth. Widgets are filled from it (loadUi), every edit is
 * read back into it (refresh), and the preview and issue list are rebuilt. */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    void scan(const QString &path);

private slots:
    void browseProject();
    void candidateChosen(int index);
    void refresh();
    void assembliesEdited();
    void addAssembly();
    void removeAssembly();
    void browseIcon();
    void save();

private:
    QWidget *buildDeviceTab();
    QWidget *buildAssemblyTab();
    QWidget *buildConnectionTab();
    QWidget *buildAdvancedTab();
    void loadUi();
    void loadAssemblyTable();
    void loadConnectionTable();
    void readUi();

    EdsModel m_model;
    ScanResult m_scan;
    QList<Issue> m_issues;
    bool m_loading = false;

    QLineEdit *m_project;
    QComboBox *m_candidates;
    QLabel *m_notes;

    QLineEdit *m_descText;
    QSpinBox *m_fileMajor;
    QSpinBox *m_fileMinor;
    QLineEdit *m_homeUrl;
    QSpinBox *m_vendCode;
    QLineEdit *m_vendName;
    QSpinBox *m_prodType;
    QLineEdit *m_prodTypeStr;
    QSpinBox *m_prodCode;
    QSpinBox *m_majRev;
    QSpinBox *m_minRev;
    QLineEdit *m_prodName;
    QLineEdit *m_catalog;
    QLineEdit *m_icon;

    QTableWidget *m_asmTable;
    QTableWidget *m_connTable;
    QSpinBox *m_rpiMin;
    QSpinBox *m_rpiDefault;
    QSpinBox *m_rpiMax;
    QCheckBox *m_multicast;

    QSpinBox *m_maxMsg;
    QSpinBox *m_ethInstances;
    QCheckBox *m_classSections;

    QPlainTextEdit *m_preview;
    QListWidget *m_issueList;
    QLabel *m_status;
    QPushButton *m_save;
};

#endif /* MAINWINDOW_H_ */
