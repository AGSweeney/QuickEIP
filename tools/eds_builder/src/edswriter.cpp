/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "edswriter.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

const char kIndent1[] = "        ";
const char kIndent2[] = "                ";

QString hexByte(int v)
{
    return QStringLiteral("%1").arg(v & 0xFF, 2, 16, QLatin1Char('0')).toUpper();
}

/* A logical segment for an instance (0x24) or connection point (0x2C): the
 * 8-bit form up to 255, else the 16-bit form with its pad byte. */
QString segment(int type8, int value)
{
    if (value <= 0xFF) {
        return hexByte(type8) + QLatin1Char(' ') + hexByte(value);
    }
    return hexByte(type8 + 1) + QStringLiteral(" 00 ") + hexByte(value) + QLatin1Char(' ') + hexByte(value >> 8);
}

QString quoted(const QString &text)
{
    QString s = text;
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

QString hex32(quint32 v)
{
    return QStringLiteral("0x%1").arg(v, 8, 16, QLatin1Char('0')).toUpper().replace(0, 2, QStringLiteral("0x"));
}

/* One multi-line entry: "Keyword =" then a field group per line, each with an
 * optional comment. The last line gets the terminating semicolon. */
class Entry {
public:
    explicit Entry(const QString &keyword) : m_keyword(keyword) {}
    void add(const QString &fields, const QString &comment = QString())
    {
        m_lines.append({fields, comment});
    }
    QString text() const
    {
        QString out = QLatin1String(kIndent1) + m_keyword + QStringLiteral(" =\n");
        for (int i = 0; i < m_lines.size(); i++) {
            QString fields = m_lines[i].first;
            if (i == m_lines.size() - 1) {
                fields += QLatin1Char(';');
            }
            QString line = QLatin1String(kIndent2) + fields;
            if (!m_lines[i].second.isEmpty()) {
                line = (line + QLatin1Char(' ')).leftJustified(40) + QStringLiteral("$ ") + m_lines[i].second;
            }
            out += line + QLatin1Char('\n');
        }
        return out;
    }

private:
    QString m_keyword;
    QList<QPair<QString, QString>> m_lines;
};

QString field(const char *keyword, const QString &value)
{
    return QLatin1String(kIndent1) + QLatin1String(keyword) + QStringLiteral(" = ") + value + QStringLiteral(";\n");
}

QString iconContents(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray b64 = f.readAll().toBase64();
    QString out;
    for (int i = 0; i < b64.size(); i += 60) {
        const bool last = (i + 60 >= b64.size());
        out += QLatin1String(kIndent2) + QLatin1Char('"') + QString::fromLatin1(b64.mid(i, 60)) +
               QLatin1Char('"') + (last ? QStringLiteral(";") : QString()) + QLatin1Char('\n');
    }
    return out;
}

QString classSection(const char *section, int revision, const char *name, int code, int instances)
{
    QString out = QStringLiteral("\n[%1]\n").arg(QLatin1String(section));
    out += field("Revision", QString::number(revision));
    out += field("Object_Name", quoted(QLatin1String(name)));
    out += field("Object_Class_Code", QStringLiteral("0x") + hexByte(code));
    out += field("MaxInst", QString::number(instances));
    out += field("Number_Of_Static_Instances", QString::number(instances));
    out += field("Max_Number_Of_Dynamic_Instances", QStringLiteral("0"));
    return out;
}

} /* namespace */

QString assemblyPath(int instance)
{
    return QStringLiteral("20 04 ") + segment(0x24, instance) + QStringLiteral(" 30 03");
}

QString connectionPath(int config, int o2t, int t2o)
{
    QString path = QStringLiteral("20 04");
    if (config != 0) {
        path += QLatin1Char(' ') + segment(0x24, config);
    }
    path += QLatin1Char(' ') + segment(0x2C, o2t);
    path += QLatin1Char(' ') + segment(0x2C, t2o);
    return path;
}

quint32 triggerTransportMask(ConnType type)
{
    /* bit 1 transport class 1, bit 16 cyclic trigger, then the application
     * type: bit 25 input only, bit 26 exclusive owner. Bit 31 clear: the
     * device is the server (target) of the connection. */
    const quint32 base = 0x00010002u;
    return base | (type == ConnType::InputOnly ? 0x02000000u : 0x04000000u);
}

quint32 connectionParamsMask(ConnType type, bool t2oMulticast)
{
    quint32 v = 0;
    v |= 0x00000001u;            /* bit 0: O->T fixed size */
    v |= 0x00000004u;            /* bit 2: T->O fixed size */
    /* bits 8-10, O->T real time format: 4 = 32-bit run/idle header, 3 = heartbeat.
     * bits 12-14, T->O format, stay 0: modeless (pure data). */
    v |= (type == ConnType::InputOnly) ? 0x00000300u : 0x00000400u;
    v |= 0x00040000u;            /* bit 18: O->T point-to-point */
    if (t2oMulticast) {
        v |= 0x00200000u;        /* bit 21: T->O multicast */
    }
    v |= 0x00400000u;            /* bit 22: T->O point-to-point */
    v |= 0x04000000u;            /* bit 26: O->T priority scheduled */
    v |= 0x40000000u;            /* bit 30: T->O priority scheduled */
    return v;
}

QString writeEds(const EdsModel &m)
{
    QString out;

    out += QStringLiteral("$ EtherNet/IP Electronic Data Sheet\n");
    out += QStringLiteral("$ Generated by QuickEIP EDS Builder %1").arg(QLatin1String(EDS_BUILDER_VERSION));
    if (!m.sourceNote.isEmpty()) {
        out += QStringLiteral(" from %1").arg(m.sourceNote);
    }
    out += QStringLiteral("\n\n[File]\n");
    out += field("DescText", quoted(m.descText));
    out += field("CreateDate", m.createDate.toString(QStringLiteral("MM-dd-yyyy")));
    out += field("CreateTime", m.createTime.toString(QStringLiteral("HH:mm:ss")));
    {
        /* EZ-EDS reports an error on a file without ModDate and ModTime. A file
         * that has not been modified since it was made carries the creation time. */
        const bool modified = m.modDate.isValid() && m.modTime.isValid();
        out += field("ModDate", (modified ? m.modDate : m.createDate).toString(QStringLiteral("MM-dd-yyyy")));
        out += field("ModTime", (modified ? m.modTime : m.createTime).toString(QStringLiteral("HH:mm:ss")));
    }
    out += field("Revision", QStringLiteral("%1.%2").arg(m.fileMajor).arg(m.fileMinor));
    if (!m.homeUrl.isEmpty()) {
        out += field("HomeURL", quoted(m.homeUrl));
    }

    out += QStringLiteral("\n[Device]\n");
    out += field("VendCode", QString::number(m.vendCode));
    out += field("VendName", quoted(m.vendName));
    out += field("ProdType", QString::number(m.prodType));
    out += field("ProdTypeStr", quoted(m.prodTypeStr));
    out += field("ProdCode", QString::number(m.prodCode));
    out += field("MajRev", QString::number(m.majRev));
    out += field("MinRev", QString::number(m.minRev));
    out += field("ProdName", quoted(m.prodName));
    if (!m.catalog.isEmpty()) {
        out += field("Catalog", quoted(m.catalog));
    }
    if (!m.iconPath.isEmpty()) {
        const QString contents = iconContents(m.iconPath);
        if (!contents.isEmpty()) {
            out += field("Icon", quoted(QFileInfo(m.iconPath).fileName()));
            out += QLatin1String(kIndent1) + QStringLiteral("IconContents =\n") + contents;
        }
    }

    out += QStringLiteral("\n[Device Classification]\n");
    out += field("Class1", QStringLiteral("EtherNetIP"));

    out += QStringLiteral("\n[Params]\n");
    {
        Entry p(QStringLiteral("Param1"));
        p.add(QStringLiteral("0,"), QStringLiteral("reserved, shall equal 0"));
        p.add(QStringLiteral(",,"), QStringLiteral("link path size, link path"));
        p.add(QStringLiteral("0x0000,"), QStringLiteral("descriptor"));
        p.add(QStringLiteral("0xC8,"), QStringLiteral("data type: UDINT"));
        p.add(QStringLiteral("4,"), QStringLiteral("data size in bytes"));
        p.add(QStringLiteral("\"RPI\","), QStringLiteral("name"));
        p.add(QStringLiteral("\"Microsecond\","), QStringLiteral("units"));
        p.add(QStringLiteral("\"Requested packet interval\","), QStringLiteral("help string"));
        p.add(QStringLiteral("%1,%2,%3,").arg(m.rpiMinUs).arg(m.rpiMaxUs).arg(m.rpiDefaultUs),
              QStringLiteral("min, max, default data values"));
        p.add(QStringLiteral(",,,,"), QStringLiteral("mult, div, base, offset scaling"));
        p.add(QStringLiteral(",,,,"), QStringLiteral("mult, div, base, offset links"));
        p.add(QString(), QStringLiteral("decimal places"));
        out += p.text();
    }
    /* EZ-EDS gives an assembly with no data one zero-size member that refers
     * to a placeholder parameter, and names the same parameter as the config
     * format of a connection. Param2 is that placeholder. */
    bool emptyConfig = false;
    bool dataConfig = false;
    for (const Assembly &a : m.assemblies) {
        if (a.dir == AsmDir::Config && a.size == 0) {
            emptyConfig = true;
        }
        if (a.dir == AsmDir::Config && a.size > 0) {
            dataConfig = true;
        }
    }
    /* A config assembly with data is described byte by byte: one member for
     * each byte, all of them Param3. The two reference files with config data
     * (ClearLink, and the EZ-EDS output for the port GmbH sample) give every
     * config member a parameter, because the parameter carries the default
     * value that configuration software sends in the Forward_Open. A member
     * with a size and no parameter gave an error list in EZ-EDS. */
    if (dataConfig) {
        Entry p(QStringLiteral("Param3"));
        p.add(QStringLiteral("0,"), QStringLiteral("reserved, shall equal 0"));
        p.add(QStringLiteral(",,"), QStringLiteral("link path size, link path"));
        p.add(QStringLiteral("0x0000,"), QStringLiteral("descriptor"));
        p.add(QStringLiteral("0xC6,"), QStringLiteral("data type: USINT"));
        p.add(QStringLiteral("1,"), QStringLiteral("data size in bytes"));
        p.add(QStringLiteral("\"Config Byte\","), QStringLiteral("name"));
        p.add(QStringLiteral("\"\","), QStringLiteral("units"));
        p.add(QStringLiteral("\"One byte of the configuration assembly\","), QStringLiteral("help string"));
        p.add(QStringLiteral("0,255,0,"), QStringLiteral("min, max, default data values"));
        p.add(QStringLiteral(",,,,"), QStringLiteral("mult, div, base, offset scaling"));
        p.add(QStringLiteral(",,,,"), QStringLiteral("mult, div, base, offset links"));
        p.add(QString(), QStringLiteral("decimal places"));
        out += p.text();
    }
    if (emptyConfig) {
        Entry p(QStringLiteral("Param2"));
        p.add(QStringLiteral("0,"), QStringLiteral("reserved, shall equal 0"));
        p.add(QStringLiteral(",,"), QStringLiteral("link path size, link path"));
        p.add(QStringLiteral("0x0000,"), QStringLiteral("descriptor"));
        p.add(QStringLiteral("0xC6,"), QStringLiteral("data type: USINT"));
        p.add(QStringLiteral("1,"), QStringLiteral("data size in bytes"));
        p.add(QStringLiteral("\"Config Data\","), QStringLiteral("name"));
        p.add(QStringLiteral("\"\","), QStringLiteral("units"));
        p.add(QStringLiteral("\"Placeholder: the configuration assembly has no data\","), QStringLiteral("help string"));
        p.add(QStringLiteral("0,0,0,"), QStringLiteral("min, max, default data values"));
        p.add(QStringLiteral(",,,,"), QStringLiteral("mult, div, base, offset scaling"));
        p.add(QStringLiteral(",,,,"), QStringLiteral("mult, div, base, offset links"));
        p.add(QString(), QStringLiteral("decimal places"));
        out += p.text();
    }

    out += QStringLiteral("\n[Assembly]\n");
    out += field("Revision", QStringLiteral("2"));
    out += field("Object_Name", quoted(QStringLiteral("Assembly Object")));
    out += field("Object_Class_Code", QStringLiteral("0x04"));
    out += field("Number_Of_Static_Instances", QString::number(m.assemblies.size()));
    for (const Assembly &a : m.assemblies) {
        Entry e(QStringLiteral("Assem%1").arg(a.instance));
        QString what;
        switch (a.dir) {
        case AsmDir::Consumed: what = a.size == 0 ? QStringLiteral("heartbeat, O->T with no data")
                                                   : QStringLiteral("consumed: originator -> device"); break;
        case AsmDir::Produced: what = QStringLiteral("produced: device -> originator"); break;
        case AsmDir::Config: what = QStringLiteral("configuration"); break;
        default: break;
        }
        e.add(quoted(a.name) + QLatin1Char(','), what);
        e.add(quoted(assemblyPath(a.instance)) + QLatin1Char(','), QStringLiteral("path"));
        e.add(QString::number(a.size) + QLatin1Char(','), QStringLiteral("size in bytes"));
        /* Descriptor bit 0, allow value edit, is set on what the originator writes:
         * consumed data, and config data. */
        e.add(a.dir != AsmDir::Produced && a.size > 0 ? QStringLiteral("0x0001,") : QStringLiteral("0x0000,"),
              QStringLiteral("descriptor"));
        e.add(QStringLiteral(",,"), QStringLiteral("reserved"));
        if (a.dir == AsmDir::Config && a.size > 0) {
            for (int i = 0; i < a.size; i++) {
                e.add(i + 1 < a.size ? QStringLiteral("8,Param3,") : QStringLiteral("8,Param3"),
                      i == 0 ? QStringLiteral("one member for each byte: size in bits, parameter") : QString());
            }
        } else if (a.size > 0) {
            e.add(QStringLiteral("%1,").arg(a.size * 8), QStringLiteral("member size in bits, no member reference"));
        } else if (a.dir == AsmDir::Config) {
            e.add(QStringLiteral("0,Param2"), QStringLiteral("no data: the member is a placeholder"));
        } else {
            e.add(QStringLiteral("0,"), QStringLiteral("no data"));
        }
        out += e.text();
    }

    out += QStringLiteral("\n[Connection Manager]\n");
    out += field("Object_Name", quoted(QStringLiteral("Connection Manager Object")));
    out += field("Object_Class_Code", QStringLiteral("0x06"));
    QSet<int> rxSizes;
    QSet<int> txSizes;
    int n = 0;
    for (const Connection &c : m.connections) {
        if (!c.enabled) {
            continue;
        }
        const Assembly *o = m.findAssembly(c.o2t);
        const Assembly *t = m.findAssembly(c.t2o);
        const Assembly *cfg = c.config != 0 ? m.findAssembly(c.config) : nullptr;
        const int oSize = o != nullptr ? o->size : 0;
        const int tSize = t != nullptr ? t->size : 0;
        const bool input = (c.type == ConnType::InputOnly);
        rxSizes.insert(oSize);
        txSizes.insert(tSize);

        Entry e(QStringLiteral("Connection%1").arg(++n));
        e.add(hex32(triggerTransportMask(c.type)) + QLatin1Char(','),
              input ? QStringLiteral("class 1, cyclic, input only") : QStringLiteral("class 1, cyclic, exclusive owner"));
        e.add(hex32(connectionParamsMask(c.type, m.t2oMulticast)) + QLatin1Char(','),
              QStringLiteral("fixed size; O->T %1, P2P; T->O modeless, %2")
                  .arg(input ? QStringLiteral("heartbeat") : QStringLiteral("run/idle header"),
                       m.t2oMulticast ? QStringLiteral("multicast or P2P") : QStringLiteral("P2P")));
        if (input) {
            e.add(QStringLiteral("Param1,0,,"), QStringLiteral("O->T RPI, size, format (heartbeat: no data)"));
        } else {
            e.add(QStringLiteral("Param1,%1,Assem%2,").arg(oSize).arg(c.o2t), QStringLiteral("O->T RPI, size, format"));
        }
        e.add(QStringLiteral("Param1,%1,Assem%2,").arg(tSize).arg(c.t2o), QStringLiteral("T->O RPI, size, format"));
        e.add(QStringLiteral(",,"), QStringLiteral("config part 1 (none)"));
        if (cfg != nullptr && cfg->size > 0) {
            e.add(QStringLiteral("%1,Assem%2,").arg(cfg->size).arg(cfg->instance),
                  QStringLiteral("config part 2: size, format"));
        } else if (cfg != nullptr) {
            e.add(QStringLiteral("0,Param2,"), QStringLiteral("config part 2: size, format (no data)"));
        } else {
            e.add(QStringLiteral(",,"), QStringLiteral("config part 2 (none)"));
        }
        e.add(quoted(c.name) + QLatin1Char(','), QStringLiteral("connection name"));
        e.add(quoted(c.help) + QLatin1Char(','), QStringLiteral("help string"));
        e.add(quoted(connectionPath(c.config, c.o2t, c.t2o)),
              c.config != 0 ? QStringLiteral("path: assembly class, config, O->T point, T->O point")
                            : QStringLiteral("path: assembly class, O->T point, T->O point"));
        out += e.text();
    }

    out += QStringLiteral("\n[Port]\n");
    out += field("Object_Name", quoted(QStringLiteral("Port Object")));
    out += field("Object_Class_Code", QStringLiteral("0xF4"));
    {
        Entry p(QStringLiteral("Port1"));
        p.add(QStringLiteral("TCP,"), QStringLiteral("port type"));
        p.add(QStringLiteral("\"EtherNet/IP Port\","), QStringLiteral("name"));
        p.add(QStringLiteral("\"20 F5 24 01\","), QStringLiteral("TCP/IP Interface object"));
        p.add(QStringLiteral("1,"), QStringLiteral("port number"));
        p.add(QStringLiteral("0x00,"), QStringLiteral("port routing capabilities: none"));
        p.add(QString());
        out += p.text();
    }

    out += QStringLiteral("\n[Capacity]\n");
    out += field("MaxIOConnections", QString::number(m.maxIoConnections));
    out += field("MaxMsgConnections", QString::number(m.maxMsgConnections));
    {
        /* Packets per second at the fastest RPI the EDS allows. */
        const int rate = m.rpiMinUs > 0 ? qMax(1, 1000000 / m.rpiMinUs) : 1;
        QList<int> rx = rxSizes.values();
        QList<int> tx = txSizes.values();
        std::sort(rx.begin(), rx.end());
        std::sort(tx.begin(), tx.end());
        int t = 0;
        for (int size : rx) {
            out += field(qPrintable(QStringLiteral("TSpec%1").arg(++t)), QStringLiteral("Rx, %1, %2").arg(size).arg(rate));
        }
        for (int size : tx) {
            out += field(qPrintable(QStringLiteral("TSpec%1").arg(++t)), QStringLiteral("Tx, %1, %2").arg(size).arg(rate));
        }
    }

    if (m.classSections) {
        out += classSection("TCP/IP Interface Class", 4, "TCP/IP Interface Object", 0xF5, 1);
        out += classSection("Ethernet Link Class", 3, "Ethernet Link Object", 0xF6, m.ethLinkInstances);
        out += classSection("Identity Class", 1, "Identity Object", 0x01, 1);
    }
    return out;
}
