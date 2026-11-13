/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Console front end: scan a project tree, list what was found, write an EDS.
 * It runs the same scanner, validator and writer as the GUI, for build
 * scripts and for testing them without a window. */
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QTextStream>

#include "edsmodel.h"
#include "edsvalidator.h"
#include "edswriter.h"
#include "projectscanner.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("eds_builder_cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(EDS_BUILDER_VERSION));

    QCommandLineParser args;
    args.setApplicationDescription(QStringLiteral("Write an EDS file for the QuickEIP adapter defined in a project tree."));
    args.addHelpOption();
    args.addVersionOption();
    args.addPositionalArgument(QStringLiteral("project"), QStringLiteral("Root of the project tree to scan."));
    const QCommandLineOption pick({QStringLiteral("p"), QStringLiteral("pick")},
                                  QStringLiteral("Which adapter definition to use, from the list (default 1)."),
                                  QStringLiteral("n"), QStringLiteral("1"));
    const QCommandLineOption outFile({QStringLiteral("o"), QStringLiteral("out")},
                                     QStringLiteral("EDS file to write. Without it the EDS goes to stdout."),
                                     QStringLiteral("file"));
    const QCommandLineOption vendorName(QStringLiteral("vendor-name"), QStringLiteral("VendName (no source for it in the project)."),
                                        QStringLiteral("text"));
    const QCommandLineOption catalog(QStringLiteral("catalog"), QStringLiteral("Catalog number. Default: the product name."),
                                     QStringLiteral("text"));
    const QCommandLineOption icon(QStringLiteral("icon"), QStringLiteral(".ico file to embed."), QStringLiteral("file"));
    const QCommandLineOption listOnly(QStringLiteral("list"), QStringLiteral("List the adapter definitions found and stop."));
    const QCommandLineOption force(QStringLiteral("force"), QStringLiteral("Write the file even when validation finds errors."));
    args.addOptions({pick, outFile, vendorName, catalog, icon, listOnly, force});
    args.process(app);

    QTextStream out(stdout);
    QTextStream err(stderr);
    if (args.positionalArguments().size() != 1) {
        err << args.helpText();
        return 2;
    }

    const ScanResult scan = scanProject(args.positionalArguments().first());
    err << "Scanned " << scan.filesScanned << " source files under " << scan.root << "\n";
    for (int i = 0; i < scan.candidates.size(); i++) {
        err << "  " << (i + 1) << ". " << scan.candidates[i].label << "\n";
        for (const QString &note : scan.candidates[i].notes) {
            err << "       note: " << note << "\n";
        }
    }
    if (scan.candidates.isEmpty()) {
        err << "No adapter definition found (no EipAdapterConfig, EipAssemblyConfig or eip_adapter_add_assembly).\n";
        return 1;
    }
    if (args.isSet(listOnly)) {
        return 0;
    }

    const int index = args.value(pick).toInt();
    if (index < 1 || index > scan.candidates.size()) {
        err << "--pick must be 1 to " << scan.candidates.size() << "\n";
        return 2;
    }

    EdsModel model;
    model.createDate = QDate::currentDate();
    model.createTime = QTime::currentTime();
    applyCandidate(scan, scan.candidates[index - 1], model);
    model.vendName = args.value(vendorName);
    if (args.isSet(catalog)) {
        model.catalog = args.value(catalog);
    }
    model.iconPath = args.value(icon);

    const QList<Issue> issues = validateModel(model);
    for (const Issue &i : issues) {
        err << (i.severity == Issue::Error ? "error:   " : "warning: ") << i.where << " - " << i.text << "\n";
    }
    if (hasErrors(issues) && !args.isSet(force)) {
        err << "Not written: fix the errors, or pass --force.\n";
        return 1;
    }

    const QString text = writeEds(model);
    if (args.isSet(outFile)) {
        QFile f(args.value(outFile));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            err << "Cannot write " << f.fileName() << "\n";
            return 1;
        }
        /* EDS files are DOS text: CR LF line ends. */
        f.write(QString(text).replace(QLatin1Char('\n'), QStringLiteral("\r\n")).toLatin1());
        err << "Wrote " << f.fileName() << "\n";
    } else {
        out << text;
    }
    return hasErrors(issues) ? 1 : 0;
}
