/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "edsvalidator.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace {

class Checker {
public:
    QList<Issue> issues;

    void error(const QString &where, const QString &text) { issues.append({Issue::Error, where, text}); }
    void warning(const QString &where, const QString &text) { issues.append({Issue::Warning, where, text}); }

    /* EDS strings are 8-bit. Anything outside printable ASCII is at the mercy
     * of the tool that reads the file. */
    void text(const QString &where, const QString &value, bool required, int maxLen = 0)
    {
        if (value.trimmed().isEmpty()) {
            if (required) {
                error(where, QStringLiteral("This required field is empty."));
            }
            return;
        }
        for (QChar c : value) {
            if (c.unicode() < 0x20 || c.unicode() > 0x7E) {
                error(where, QStringLiteral("Only printable ASCII characters are allowed."));
                break;
            }
        }
        if (maxLen > 0 && value.size() > maxLen) {
            error(where, QStringLiteral("Longer than %1 characters.").arg(maxLen));
        }
    }

    void range(const QString &where, int value, int min, int max)
    {
        if (value < min || value > max) {
            error(where, QStringLiteral("Field value %1 is out of range (%2-%3).").arg(value).arg(min).arg(max));
        }
    }
};

} /* namespace */

bool hasErrors(const QList<Issue> &issues)
{
    for (const Issue &i : issues) {
        if (i.severity == Issue::Error) {
            return true;
        }
    }
    return false;
}

QList<Issue> validateModel(const EdsModel &m)
{
    Checker c;

    /* [File] */
    c.text(QStringLiteral("[File] DescText"), m.descText, true);
    if (!m.createDate.isValid() || !m.createTime.isValid()) {
        c.error(QStringLiteral("[File] CreateDate"), QStringLiteral("This required field is empty."));
    }
    if (m.modDate.isValid() != m.modTime.isValid()) {
        c.error(QStringLiteral("[File] ModDate"), QStringLiteral("ModDate and ModTime must be given together."));
    }
    c.range(QStringLiteral("[File] Revision major"), m.fileMajor, 0, 255);
    c.range(QStringLiteral("[File] Revision minor"), m.fileMinor, 0, 255);
    if (m.fileMajor == 0 && m.fileMinor == 0) {
        c.error(QStringLiteral("[File] Revision"), QStringLiteral("Revision 0.0 is not allowed."));
    }
    c.text(QStringLiteral("[File] HomeURL"), m.homeUrl, false);

    /* [Device] */
    c.range(QStringLiteral("[Device] VendCode"), m.vendCode, 1, 65535);
    if (m.vendCode == 65000) {
        c.warning(QStringLiteral("[Device] VendCode"),
                  QStringLiteral("65000 is the QuickEIP placeholder, not a vendor id assigned by ODVA."));
    }
    c.text(QStringLiteral("[Device] VendName"), m.vendName, true);
    c.range(QStringLiteral("[Device] ProdType"), m.prodType, 0, 65535);
    if (m.prodType == 0) {
        c.warning(QStringLiteral("[Device] ProdType"), QStringLiteral("0 is deprecated for new devices."));
    }
    c.text(QStringLiteral("[Device] ProdTypeStr"), m.prodTypeStr, true);
    c.range(QStringLiteral("[Device] ProdCode"), m.prodCode, 1, 65535);
    c.range(QStringLiteral("[Device] MajRev"), m.majRev, 1, 127);
    c.range(QStringLiteral("[Device] MinRev"), m.minRev, 1, 255);
    c.text(QStringLiteral("[Device] ProdName"), m.prodName, true, 32);
    if (m.prodName.size() > 31 && m.prodName.size() <= 32) {
        c.warning(QStringLiteral("[Device] ProdName"),
                  QStringLiteral("QuickEIP truncates product_name to 31 characters, so the device will report a "
                                 "different name."));
    }
    c.text(QStringLiteral("[Device] Catalog"), m.catalog, false);
    if (!m.iconPath.isEmpty()) {
        QFile icon(m.iconPath);
        if (!icon.open(QIODevice::ReadOnly)) {
            c.error(QStringLiteral("[Device] Icon"), QStringLiteral("The icon file cannot be read."));
        } else {
            /* An .ico file starts with reserved 0, type 1, then an image count. */
            const QByteArray head = icon.read(6);
            if (head.size() < 6 || head[0] != 0 || head[1] != 0 || head[2] != 1 || head[3] != 0) {
                c.error(QStringLiteral("[Device] Icon"), QStringLiteral("Shall have the *.ICO MS Windows format."));
            }
            c.text(QStringLiteral("[Device] Icon"), QFileInfo(m.iconPath).fileName(), true);
        }
    }

    /* [Params] */
    if (m.rpiMinUs < 1 || m.rpiMinUs > m.rpiMaxUs) {
        c.error(QStringLiteral("Param1 (RPI)"), QStringLiteral("Minimum must be at least 1 and no more than the maximum."));
    }
    if (m.rpiDefaultUs < m.rpiMinUs || m.rpiDefaultUs > m.rpiMaxUs) {
        c.error(QStringLiteral("Param1 (RPI)"), QStringLiteral("Default must lie between the minimum and the maximum."));
    }

    /* [Assembly] */
    QSet<int> seen;
    int pool = 0;
    for (const Assembly &a : m.assemblies) {
        const QString where = QStringLiteral("Assem%1").arg(a.instance);
        if (!a.unresolvedInstance.isEmpty()) {
            c.error(where, QStringLiteral("Instance \"%1\" could not be read from the source. Enter it.")
                               .arg(a.unresolvedInstance));
        }
        if (!a.unresolvedSize.isEmpty()) {
            c.error(where, QStringLiteral("Size \"%1\" could not be read from the source. Enter it.")
                               .arg(a.unresolvedSize));
        }
        c.range(where + QStringLiteral(" instance"), a.instance, 1, 65535);
        if (seen.contains(a.instance)) {
            c.error(where, QStringLiteral("Duplicate entry keyword: the instance is used twice."));
        }
        seen.insert(a.instance);
        if (a.dir == AsmDir::Unknown) {
            c.error(where, QStringLiteral("The direction is not set."));
        }
        /* A Forward_Open size field has 9 bits: 2 (sequence count) + 4 (run/idle header) + data <= 511. */
        c.range(where + QStringLiteral(" size"), a.size, 0, 505);
        c.text(where + QStringLiteral(" name"), a.name, true);
        if (a.name.size() > 15) {
            c.warning(where, QStringLiteral("QuickEIP keeps only 15 characters of an assembly name."));
        }
        if (a.dir == AsmDir::Config && a.size > 0) {
            if (a.size % 2 != 0) {
                c.warning(where, QStringLiteral("Config data travels in a data segment of 16-bit words. An odd size "
                                                "cannot be sent exactly, and the adapter refuses any other size."));
            }
            c.warning(where, QStringLiteral("The EDS describes the config data as one block with no parameters, so a "
                                            "PLC sends it as zeros unless the user enters values by hand."));
        }
        pool += a.size;
    }
    if (m.assemblies.size() > m.limitAssemblies) {
        c.error(QStringLiteral("[Assembly]"),
                QStringLiteral("%1 assemblies, but EIP_ADAPTER_MAX_ASSEMBLIES is %2.")
                    .arg(m.assemblies.size()).arg(m.limitAssemblies));
    }
    if (pool > m.limitPool) {
        c.error(QStringLiteral("[Assembly]"),
                QStringLiteral("The assemblies take %1 bytes, but EIP_ADAPTER_DATA_POOL is %2.").arg(pool).arg(m.limitPool));
    }

    /* [Connection Manager] */
    int enabled = 0;
    QSet<QString> names;
    bool anyWithoutConfig = false;
    for (const Connection &conn : m.connections) {
        if (!conn.enabled) {
            continue;
        }
        enabled++;
        const QString where = QStringLiteral("Connection%1").arg(enabled);
        c.text(where + QStringLiteral(" name"), conn.name, true);
        c.text(where + QStringLiteral(" help"), conn.help, false);
        if (names.contains(conn.name)) {
            c.error(where, QStringLiteral("Two connections are named \"%1\".").arg(conn.name));
        }
        names.insert(conn.name);

        const Assembly *o = m.findAssembly(conn.o2t);
        const Assembly *t = m.findAssembly(conn.t2o);
        if (o == nullptr || o->dir != AsmDir::Consumed) {
            c.error(where, QStringLiteral("O->T point %1 is not a consumed assembly (the adapter answers 0x012A).")
                               .arg(conn.o2t));
        } else if (conn.type == ConnType::ExclusiveOwner && o->size == 0) {
            c.error(where, QStringLiteral("An exclusive owner connection needs O->T data."));
        } else if (conn.type == ConnType::InputOnly && o->size != 0) {
            c.error(where, QStringLiteral("An input only connection needs a size-0 consumed assembly as its heartbeat."));
        }
        if (t == nullptr || t->dir != AsmDir::Produced) {
            c.error(where, QStringLiteral("T->O point %1 is not a produced assembly (the adapter answers 0x012B).")
                               .arg(conn.t2o));
        }
        if (conn.config != 0) {
            const Assembly *cfg = m.findAssembly(conn.config);
            if (cfg == nullptr || cfg->dir != AsmDir::Config) {
                c.error(where, QStringLiteral("Config instance %1 is not a config assembly (the adapter answers 0x0129).")
                                   .arg(conn.config));
            }
        } else {
            anyWithoutConfig = true;
        }
    }
    if (enabled == 0) {
        c.error(QStringLiteral("[Connection Manager]"),
                QStringLiteral("No connection. A PLC cannot exchange I/O with the device. The project needs a consumed "
                               "and a produced assembly."));
    }
    if (anyWithoutConfig) {
        c.warning(QStringLiteral("[Connection Manager]"),
                  QStringLiteral("No config assembly. Most PLCs name a config instance in the connection path: add a "
                                 "size-0 EIP_ASM_CONFIG assembly to the project."));
    }

    /* [Capacity] */
    c.range(QStringLiteral("[Capacity] MaxIOConnections"), m.maxIoConnections, 1, 65535);
    c.range(QStringLiteral("[Capacity] MaxMsgConnections"), m.maxMsgConnections, 0, 65535);
    if (m.maxIoConnections != 1) {
        c.warning(QStringLiteral("[Capacity] MaxIOConnections"),
                  QStringLiteral("The QuickEIP adapter has one Class 1 connection."));
    }
    c.range(QStringLiteral("[Ethernet Link Class] MaxInst"), m.ethLinkInstances, 1, 255);

    return c.issues;
}
