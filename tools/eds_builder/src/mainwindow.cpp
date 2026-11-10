/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "mainwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "edswriter.h"
#include "theme.h"

namespace {

enum { AsmInstance, AsmDirection, AsmSize, AsmName, AsmColumns };
enum { ConnUse, ConnName, ConnTypeCol, ConnO2T, ConnT2O, ConnConfig, ConnColumns };

QSpinBox *spin(int min, int max, int value = 0)
{
    QSpinBox *s = new QSpinBox;
    s->setRange(min, max);
    s->setValue(value);
    return s;
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    QTableWidgetItem *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

} /* namespace */

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("QuickEIP EDS Builder %1").arg(QLatin1String(EDS_BUILDER_VERSION)));
    resize(1280, 800);

    QWidget *central = new QWidget;
    QVBoxLayout *outer = new QVBoxLayout(central);

    /* Project row. */
    QHBoxLayout *projectRow = new QHBoxLayout;
    m_project = new QLineEdit;
    m_project->setPlaceholderText(QStringLiteral("Folder of a project that uses QuickEIP"));
    QPushButton *browse = new QPushButton(QStringLiteral("Browse..."));
    QPushButton *scanButton = new QPushButton(QStringLiteral("Scan"));
    projectRow->addWidget(new QLabel(QStringLiteral("Project:")));
    projectRow->addWidget(m_project, 1);
    projectRow->addWidget(browse);
    projectRow->addWidget(scanButton);
    outer->addLayout(projectRow);

    QHBoxLayout *candidateRow = new QHBoxLayout;
    m_candidates = new QComboBox;
    m_candidates->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    candidateRow->addWidget(new QLabel(QStringLiteral("Adapter definition:")));
    candidateRow->addWidget(m_candidates, 1);
    outer->addLayout(candidateRow);
    m_notes = new QLabel;
    m_notes->setWordWrap(true);
    m_notes->setObjectName(QStringLiteral("notes"));
    m_notes->setVisible(false);
    outer->addWidget(m_notes);

    /* Editors on the left, the EDS and what is wrong with it on the right. */
    QTabWidget *tabs = new QTabWidget;
    tabs->addTab(buildDeviceTab(), QStringLiteral("Device"));
    tabs->addTab(buildAssemblyTab(), QStringLiteral("Assemblies"));
    tabs->addTab(buildConnectionTab(), QStringLiteral("Connections"));
    tabs->addTab(buildAdvancedTab(), QStringLiteral("Advanced"));

    m_preview = new QPlainTextEdit;
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    {
        QFont mono(QStringLiteral("Consolas"));
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSizeF(10.5);
        m_preview->setFont(mono);
    }
    new EdsHighlighter(m_preview->document());
    m_issueList = new QListWidget;
    m_issueList->setWordWrap(true);
    m_issueList->setAlternatingRowColors(true);

    QSplitter *right = new QSplitter(Qt::Vertical);
    right->addWidget(m_preview);
    right->addWidget(m_issueList);
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 1);
    QSplitter *split = new QSplitter(Qt::Horizontal);
    split->addWidget(tabs);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    outer->addWidget(split, 1);

    QHBoxLayout *bottom = new QHBoxLayout;
    m_status = new QLabel;
    m_status->setTextFormat(Qt::PlainText);
    m_save = new QPushButton(QStringLiteral("Save EDS..."));
    m_save->setDefault(true);
    bottom->addWidget(m_status, 1);
    bottom->addWidget(m_save);
    outer->addLayout(bottom);
    setCentralWidget(central);

    connect(browse, &QPushButton::clicked, this, &MainWindow::browseProject);
    connect(scanButton, &QPushButton::clicked, this, [this] { scan(m_project->text()); });
    connect(m_project, &QLineEdit::returnPressed, this, [this] { scan(m_project->text()); });
    connect(m_candidates, &QComboBox::activated, this, &MainWindow::candidateChosen);
    connect(m_save, &QPushButton::clicked, this, &MainWindow::save);

    QSettings settings;
    m_model.vendName = settings.value(QStringLiteral("vendName")).toString();
    m_model.homeUrl = settings.value(QStringLiteral("homeUrl")).toString();
    m_model.createDate = QDate::currentDate();
    m_model.createTime = QTime::currentTime();
    m_project->setText(settings.value(QStringLiteral("project")).toString());
    loadUi();
    refresh();
}

QWidget *MainWindow::buildDeviceTab()
{
    QWidget *page = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(page);

    QGroupBox *deviceBox = new QGroupBox(QStringLiteral("[Device] - must equal what the adapter reports"));
    QFormLayout *device = new QFormLayout(deviceBox);
    m_vendCode = spin(0, 65535);
    m_vendName = new QLineEdit;
    m_prodType = spin(0, 65535);
    m_prodTypeStr = new QLineEdit;
    m_prodCode = spin(0, 65535);
    m_majRev = spin(0, 255);
    m_minRev = spin(0, 255);
    m_prodName = new QLineEdit;
    m_catalog = new QLineEdit;
    m_icon = new QLineEdit;
    m_icon->setPlaceholderText(QStringLiteral("Optional .ico file, embedded in the EDS"));
    QPushButton *iconBrowse = new QPushButton(QStringLiteral("..."));
    QHBoxLayout *iconRow = new QHBoxLayout;
    iconRow->addWidget(m_icon, 1);
    iconRow->addWidget(iconBrowse);
    QHBoxLayout *revRow = new QHBoxLayout;
    revRow->addWidget(m_majRev);
    revRow->addWidget(new QLabel(QStringLiteral(".")));
    revRow->addWidget(m_minRev);
    revRow->addStretch(1);
    device->addRow(QStringLiteral("Vendor id (VendCode):"), m_vendCode);
    device->addRow(QStringLiteral("Vendor name:"), m_vendName);
    device->addRow(QStringLiteral("Device type (ProdType):"), m_prodType);
    device->addRow(QStringLiteral("Device type name:"), m_prodTypeStr);
    device->addRow(QStringLiteral("Product code:"), m_prodCode);
    device->addRow(QStringLiteral("Revision (major.minor):"), revRow);
    device->addRow(QStringLiteral("Product name:"), m_prodName);
    device->addRow(QStringLiteral("Catalog number:"), m_catalog);
    device->addRow(QStringLiteral("Icon:"), iconRow);
    layout->addWidget(deviceBox);

    QGroupBox *fileBox = new QGroupBox(QStringLiteral("[File]"));
    QFormLayout *file = new QFormLayout(fileBox);
    m_descText = new QLineEdit;
    m_fileMajor = spin(0, 255, 1);
    m_fileMinor = spin(0, 255, 0);
    m_homeUrl = new QLineEdit;
    QHBoxLayout *fileRev = new QHBoxLayout;
    fileRev->addWidget(m_fileMajor);
    fileRev->addWidget(new QLabel(QStringLiteral(".")));
    fileRev->addWidget(m_fileMinor);
    fileRev->addStretch(1);
    file->addRow(QStringLiteral("Description:"), m_descText);
    file->addRow(QStringLiteral("EDS revision:"), fileRev);
    file->addRow(QStringLiteral("Home URL:"), m_homeUrl);
    layout->addWidget(fileBox);
    layout->addStretch(1);

    for (QLineEdit *e : {m_vendName, m_prodTypeStr, m_prodName, m_catalog, m_icon, m_descText, m_homeUrl}) {
        connect(e, &QLineEdit::textEdited, this, &MainWindow::refresh);
    }
    for (QSpinBox *s : {m_vendCode, m_prodCode, m_majRev, m_minRev, m_fileMajor, m_fileMinor}) {
        connect(s, &QSpinBox::valueChanged, this, &MainWindow::refresh);
    }
    connect(m_prodType, &QSpinBox::valueChanged, this, [this](int value) {
        /* Keep the name in step with the number when ODVA has a name for it. */
        const QString name = deviceTypeName(value);
        if (!m_loading && !name.isEmpty()) {
            m_prodTypeStr->setText(name);
        }
        refresh();
    });
    connect(iconBrowse, &QPushButton::clicked, this, &MainWindow::browseIcon);
    return page;
}

QWidget *MainWindow::buildAssemblyTab()
{
    QWidget *page = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(page);
    QLabel *hint = new QLabel(QStringLiteral(
        "Read from the project. They must match the adapter exactly: it refuses a Forward_Open that names another "
        "instance or size. Change the source, then scan again, rather than editing here."));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("hint"));
    layout->addWidget(hint);

    m_asmTable = new QTableWidget(0, AsmColumns);
    m_asmTable->setHorizontalHeaderLabels({QStringLiteral("Instance"), QStringLiteral("Direction"),
                                           QStringLiteral("Size (bytes)"), QStringLiteral("Name")});
    m_asmTable->horizontalHeader()->setSectionResizeMode(AsmName, QHeaderView::Stretch);
    m_asmTable->horizontalHeader()->setSectionResizeMode(AsmDirection, QHeaderView::ResizeToContents);
    m_asmTable->verticalHeader()->setVisible(false);
    m_asmTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_asmTable->setAlternatingRowColors(true);
    m_asmTable->verticalHeader()->setDefaultSectionSize(34);
    layout->addWidget(m_asmTable, 1);

    QHBoxLayout *buttons = new QHBoxLayout;
    QPushButton *add = new QPushButton(QStringLiteral("Add"));
    QPushButton *remove = new QPushButton(QStringLiteral("Remove"));
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(m_asmTable, &QTableWidget::itemChanged, this, &MainWindow::assembliesEdited);
    connect(add, &QPushButton::clicked, this, &MainWindow::addAssembly);
    connect(remove, &QPushButton::clicked, this, &MainWindow::removeAssembly);
    return page;
}

QWidget *MainWindow::buildConnectionTab()
{
    QWidget *page = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(page);
    QLabel *hint = new QLabel(QStringLiteral(
        "Every connection the assemblies allow. Untick the ones the PLC should not be offered. Input only needs a "
        "size-0 consumed assembly in the project to act as the heartbeat."));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("hint"));
    layout->addWidget(hint);

    m_connTable = new QTableWidget(0, ConnColumns);
    m_connTable->setHorizontalHeaderLabels({QStringLiteral("Use"), QStringLiteral("Name"), QStringLiteral("Type"),
                                            QStringLiteral("O->T"), QStringLiteral("T->O"), QStringLiteral("Config")});
    m_connTable->horizontalHeader()->setSectionResizeMode(ConnName, QHeaderView::Stretch);
    m_connTable->horizontalHeader()->setSectionResizeMode(ConnUse, QHeaderView::ResizeToContents);
    m_connTable->verticalHeader()->setVisible(false);
    m_connTable->setAlternatingRowColors(true);
    m_connTable->verticalHeader()->setDefaultSectionSize(34);
    layout->addWidget(m_connTable, 1);

    QGroupBox *rpiBox = new QGroupBox(QStringLiteral("Requested packet interval (microseconds)"));
    QFormLayout *rpi = new QFormLayout(rpiBox);
    m_rpiMin = spin(1, 100000000);
    m_rpiDefault = spin(1, 100000000);
    m_rpiMax = spin(1, 100000000);
    for (QSpinBox *s : {m_rpiMin, m_rpiDefault, m_rpiMax}) {
        s->setSingleStep(1000);
        s->setGroupSeparatorShown(true);
    }
    rpi->addRow(QStringLiteral("Minimum:"), m_rpiMin);
    rpi->addRow(QStringLiteral("Default:"), m_rpiDefault);
    rpi->addRow(QStringLiteral("Maximum:"), m_rpiMax);
    layout->addWidget(rpiBox);
    m_multicast = new QCheckBox(QStringLiteral("Offer multicast for T->O as well as point-to-point"));
    layout->addWidget(m_multicast);

    connect(m_connTable, &QTableWidget::itemChanged, this, &MainWindow::refresh);
    for (QSpinBox *s : {m_rpiMin, m_rpiDefault, m_rpiMax}) {
        connect(s, &QSpinBox::valueChanged, this, &MainWindow::refresh);
    }
    connect(m_multicast, &QCheckBox::toggled, this, &MainWindow::refresh);
    return page;
}

QWidget *MainWindow::buildAdvancedTab()
{
    QWidget *page = new QWidget;
    QFormLayout *form = new QFormLayout(page);
    m_maxMsg = spin(0, 65535);
    m_maxMsg->setToolTip(QStringLiteral("Class 3 (connected explicit messaging) connections. The QuickEIP adapter "
                                        "refuses them: its explicit messages are unconnected."));
    m_ethInstances = spin(1, 255);
    m_ethInstances->setToolTip(QStringLiteral("Instances of the Ethernet Link object. QuickEIP reports 3 "
                                              "(ETH_LINK_INSTANCES in eip_adapter.c)."));
    m_classSections = new QCheckBox(QStringLiteral("Write [TCP/IP Interface Class], [Ethernet Link Class] and [Identity Class]"));
    form->addRow(QStringLiteral("MaxMsgConnections:"), m_maxMsg);
    form->addRow(QStringLiteral("Ethernet Link instances:"), m_ethInstances);
    form->addRow(m_classSections);
    connect(m_maxMsg, &QSpinBox::valueChanged, this, &MainWindow::refresh);
    connect(m_ethInstances, &QSpinBox::valueChanged, this, &MainWindow::refresh);
    connect(m_classSections, &QCheckBox::toggled, this, &MainWindow::refresh);
    return page;
}

void MainWindow::browseProject()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Project folder"), m_project->text());
    if (!dir.isEmpty()) {
        m_project->setText(QDir::toNativeSeparators(dir));
        scan(dir);
    }
}

void MainWindow::scan(const QString &path)
{
    if (path.trimmed().isEmpty() || !QDir(path).exists()) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Choose the folder of a project first."));
        return;
    }
    m_project->setText(QDir::toNativeSeparators(path));
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    m_scan = scanProject(path);
    QGuiApplication::restoreOverrideCursor();
    QSettings().setValue(QStringLiteral("project"), m_project->text());

    m_candidates->clear();
    for (const ScanCandidate &c : m_scan.candidates) {
        m_candidates->addItem(c.label);
    }
    if (m_scan.candidates.isEmpty()) {
        m_notes->setText(QStringLiteral("Scanned %1 source files and found no adapter definition: no EipAdapterConfig, "
                                        "EipAssemblyConfig array or eip_adapter_add_assembly call. Enter the device by hand.")
                             .arg(m_scan.filesScanned));
        m_notes->setVisible(true);
        return;
    }
    m_candidates->setCurrentIndex(0);
    candidateChosen(0);
}

void MainWindow::candidateChosen(int index)
{
    if (index < 0 || index >= m_scan.candidates.size()) {
        return;
    }
    readUi(); /* keep what the user typed in fields the scan has no value for */
    const ScanCandidate &c = m_scan.candidates[index];
    applyCandidate(m_scan, c, m_model);
    QString notes = QStringLiteral("Scanned %1 source files, %2 adapter definition(s).")
                        .arg(m_scan.filesScanned).arg(m_scan.candidates.size());
    for (const QString &n : c.notes) {
        notes += QStringLiteral("\nNote: ") + n;
    }
    m_notes->setText(notes);
    m_notes->setVisible(true);
    loadUi();
    refresh();
}

void MainWindow::loadUi()
{
    m_loading = true;
    m_descText->setText(m_model.descText);
    m_fileMajor->setValue(m_model.fileMajor);
    m_fileMinor->setValue(m_model.fileMinor);
    m_homeUrl->setText(m_model.homeUrl);
    m_vendCode->setValue(m_model.vendCode);
    m_vendName->setText(m_model.vendName);
    m_prodType->setValue(m_model.prodType);
    m_prodTypeStr->setText(m_model.prodTypeStr.isEmpty() ? deviceTypeName(m_model.prodType) : m_model.prodTypeStr);
    m_prodCode->setValue(m_model.prodCode);
    m_majRev->setValue(m_model.majRev);
    m_minRev->setValue(m_model.minRev);
    m_prodName->setText(m_model.prodName);
    m_catalog->setText(m_model.catalog);
    m_icon->setText(m_model.iconPath);
    m_rpiMin->setValue(m_model.rpiMinUs);
    m_rpiDefault->setValue(m_model.rpiDefaultUs);
    m_rpiMax->setValue(m_model.rpiMaxUs);
    m_multicast->setChecked(m_model.t2oMulticast);
    m_maxMsg->setValue(m_model.maxMsgConnections);
    m_ethInstances->setValue(m_model.ethLinkInstances);
    m_classSections->setChecked(m_model.classSections);
    m_loading = false;
    loadAssemblyTable();
    loadConnectionTable();
}

void MainWindow::loadAssemblyTable()
{
    m_loading = true;
    m_asmTable->setRowCount(int(m_model.assemblies.size()));
    for (int row = 0; row < m_model.assemblies.size(); row++) {
        const Assembly &a = m_model.assemblies[row];
        /* A value the scanner could not read shows as its source text, for the user to replace. */
        m_asmTable->setItem(row, AsmInstance, new QTableWidgetItem(a.unresolvedInstance.isEmpty()
                                                                       ? QString::number(a.instance) : a.unresolvedInstance));
        m_asmTable->setItem(row, AsmSize, new QTableWidgetItem(a.unresolvedSize.isEmpty() ? QString::number(a.size)
                                                                                          : a.unresolvedSize));
        m_asmTable->setItem(row, AsmName, new QTableWidgetItem(a.name));
        QComboBox *dir = new QComboBox;
        dir->addItem(asmDirName(AsmDir::Consumed), int(AsmDir::Consumed));
        dir->addItem(asmDirName(AsmDir::Produced), int(AsmDir::Produced));
        dir->addItem(asmDirName(AsmDir::Config), int(AsmDir::Config));
        dir->setCurrentIndex(dir->findData(int(a.dir)));
        connect(dir, &QComboBox::activated, this, &MainWindow::assembliesEdited);
        m_asmTable->setCellWidget(row, AsmDirection, dir);
    }
    m_loading = false;
}

void MainWindow::loadConnectionTable()
{
    m_loading = true;
    m_connTable->setRowCount(int(m_model.connections.size()));
    for (int row = 0; row < m_model.connections.size(); row++) {
        const Connection &c = m_model.connections[row];
        QTableWidgetItem *use = new QTableWidgetItem;
        use->setFlags((use->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        use->setCheckState(c.enabled ? Qt::Checked : Qt::Unchecked);
        m_connTable->setItem(row, ConnUse, use);
        m_connTable->setItem(row, ConnName, new QTableWidgetItem(c.name));
        m_connTable->setItem(row, ConnTypeCol, readOnlyItem(c.type == ConnType::InputOnly ? QStringLiteral("Input only")
                                                                                          : QStringLiteral("Exclusive owner")));
        m_connTable->setItem(row, ConnO2T, readOnlyItem(QString::number(c.o2t)));
        m_connTable->setItem(row, ConnT2O, readOnlyItem(QString::number(c.t2o)));
        m_connTable->setItem(row, ConnConfig, readOnlyItem(c.config != 0 ? QString::number(c.config) : QStringLiteral("none")));
    }
    m_loading = false;
}

void MainWindow::readUi()
{
    m_model.descText = m_descText->text();
    m_model.fileMajor = m_fileMajor->value();
    m_model.fileMinor = m_fileMinor->value();
    m_model.homeUrl = m_homeUrl->text().trimmed();
    m_model.vendCode = m_vendCode->value();
    m_model.vendName = m_vendName->text();
    m_model.prodType = m_prodType->value();
    m_model.prodTypeStr = m_prodTypeStr->text();
    m_model.prodCode = m_prodCode->value();
    m_model.majRev = m_majRev->value();
    m_model.minRev = m_minRev->value();
    m_model.prodName = m_prodName->text();
    m_model.catalog = m_catalog->text().trimmed();
    m_model.iconPath = m_icon->text().trimmed();
    m_model.rpiMinUs = m_rpiMin->value();
    m_model.rpiDefaultUs = m_rpiDefault->value();
    m_model.rpiMaxUs = m_rpiMax->value();
    m_model.t2oMulticast = m_multicast->isChecked();
    m_model.maxMsgConnections = m_maxMsg->value();
    m_model.ethLinkInstances = m_ethInstances->value();
    m_model.classSections = m_classSections->isChecked();

    for (int row = 0; row < m_connTable->rowCount() && row < m_model.connections.size(); row++) {
        if (m_connTable->item(row, ConnUse) != nullptr && m_connTable->item(row, ConnName) != nullptr) {
            m_model.connections[row].enabled = (m_connTable->item(row, ConnUse)->checkState() == Qt::Checked);
            m_model.connections[row].name = m_connTable->item(row, ConnName)->text();
        }
    }
}

void MainWindow::assembliesEdited()
{
    if (m_loading) {
        return;
    }
    QList<Assembly> list;
    for (int row = 0; row < m_asmTable->rowCount(); row++) {
        Assembly a;
        if (row < m_model.assemblies.size()) {
            a = m_model.assemblies[row];
        }
        const QTableWidgetItem *instance = m_asmTable->item(row, AsmInstance);
        const QTableWidgetItem *size = m_asmTable->item(row, AsmSize);
        const QTableWidgetItem *name = m_asmTable->item(row, AsmName);
        const QComboBox *dir = qobject_cast<QComboBox *>(m_asmTable->cellWidget(row, AsmDirection));
        bool ok = false;
        if (instance != nullptr) {
            const int v = instance->text().trimmed().toInt(&ok, 0);
            a.instance = ok ? v : 0;
            a.unresolvedInstance = ok ? QString() : instance->text();
        }
        if (size != nullptr) {
            const int v = size->text().trimmed().toInt(&ok, 0);
            a.size = ok ? v : 0;
            a.unresolvedSize = ok ? QString() : size->text();
        }
        if (name != nullptr) {
            a.name = name->text();
        }
        if (dir != nullptr) {
            a.dir = AsmDir(dir->currentData().toInt());
        }
        list.append(a);
    }
    readUi();
    m_model.assemblies = list;
    rebuildConnections(m_model);
    loadConnectionTable();
    refresh();
}

void MainWindow::addAssembly()
{
    int next = 100;
    for (const Assembly &a : m_model.assemblies) {
        next = qMax(next, a.instance + 1);
    }
    Assembly a;
    a.instance = next;
    a.dir = AsmDir::Produced;
    a.size = 8;
    a.name = QStringLiteral("Assembly %1").arg(next);
    readUi();
    m_model.assemblies.append(a);
    rebuildConnections(m_model);
    loadAssemblyTable();
    loadConnectionTable();
    refresh();
}

void MainWindow::removeAssembly()
{
    const int row = m_asmTable->currentRow();
    if (row < 0 || row >= m_model.assemblies.size()) {
        return;
    }
    readUi();
    m_model.assemblies.removeAt(row);
    rebuildConnections(m_model);
    loadAssemblyTable();
    loadConnectionTable();
    refresh();
}

void MainWindow::browseIcon()
{
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Icon"), m_icon->text(),
                                                      QStringLiteral("Icons (*.ico)"));
    if (!file.isEmpty()) {
        m_icon->setText(QDir::toNativeSeparators(file));
        refresh();
    }
}

void MainWindow::refresh()
{
    if (m_loading) {
        return;
    }
    readUi();
    m_issues = validateModel(m_model);

    const int scroll = m_preview->verticalScrollBar()->value();
    m_preview->setPlainText(writeEds(m_model));
    m_preview->verticalScrollBar()->setValue(scroll);

    int errors = 0;
    int warnings = 0;
    m_issueList->clear();
    for (const Issue &i : m_issues) {
        const bool error = (i.severity == Issue::Error);
        (error ? errors : warnings)++;
        QListWidgetItem *item = new QListWidgetItem(
            style()->standardIcon(error ? QStyle::SP_MessageBoxCritical : QStyle::SP_MessageBoxWarning),
            i.where + QStringLiteral(": ") + i.text);
        item->setForeground(error ? theme().error : theme().warning);
        m_issueList->addItem(item);
    }
    if (m_issues.isEmpty()) {
        m_issueList->addItem(new QListWidgetItem(style()->standardIcon(QStyle::SP_DialogApplyButton),
                                                 QStringLiteral("No errors or warnings.")));
    }
    /* A solid banner: red while the file is not valid, green once it is. */
    const QColor banner = errors > 0 ? theme().error : theme().ok;
    m_status->setText(errors > 0 ? QStringLiteral("Not a valid EDS yet: %1 error(s), %2 warning(s)").arg(errors).arg(warnings)
                                 : QStringLiteral("Valid EDS. %1 warning(s)").arg(warnings));
    m_status->setStyleSheet(QStringLiteral("QLabel { background: %1; color: %2; font-weight: 600; padding: 7px 12px; "
                                           "border-radius: 4px; }")
                                .arg(banner.name(), theme().dark ? QStringLiteral("#10141a") : QStringLiteral("#ffffff")));
}
void MainWindow::save()
{
    refresh();
    if (hasErrors(m_issues)) {
        const auto answer = QMessageBox::warning(
            this, windowTitle(),
            QStringLiteral("The EDS has errors and other tools may reject it. Save it anyway?"),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save) {
            return;
        }
    }
    QString base = m_model.prodName;
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("_"));
    if (base.isEmpty()) {
        base = QStringLiteral("device");
    }
    QSettings settings;
    const QString dir = settings.value(QStringLiteral("saveDir"), m_project->text()).toString();
    const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Save EDS"),
                                                      QDir(dir).filePath(base + QStringLiteral(".eds")),
                                                      QStringLiteral("Electronic Data Sheet (*.eds)"));
    if (file.isEmpty()) {
        return;
    }
    /* The dates say when this file was made, not when the window opened. */
    m_model.createDate = QDate::currentDate();
    m_model.createTime = QTime::currentTime();
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, windowTitle(), QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(file)));
        return;
    }
    /* EDS files are DOS text: CR LF line ends, 8-bit characters. */
    f.write(writeEds(m_model).replace(QLatin1Char('\n'), QStringLiteral("\r\n")).toLatin1());
    f.close();
    settings.setValue(QStringLiteral("saveDir"), QFileInfo(file).absolutePath());
    settings.setValue(QStringLiteral("vendName"), m_model.vendName);
    settings.setValue(QStringLiteral("homeUrl"), m_model.homeUrl);
    m_preview->setPlainText(writeEds(m_model));
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(file)), 8000);
}
