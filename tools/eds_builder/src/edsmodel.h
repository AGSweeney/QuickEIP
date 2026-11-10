/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Everything an EDS file for a QuickEIP adapter is generated from. */
#ifndef EDSMODEL_H_
#define EDSMODEL_H_

#include <QDate>
#include <QList>
#include <QString>
#include <QTime>

/* The values match EIP_ASM_CONSUMED / PRODUCED / CONFIG in eip.h. */
enum class AsmDir { Unknown = 0, Consumed = 1, Produced = 2, Config = 3 };

struct Assembly {
    int instance = 0;
    AsmDir dir = AsmDir::Unknown;
    int size = 0;               /* bytes */
    QString name;
    bool implicitOnly = false;
    /* Source text of a value the scanner could not turn into a number
     * (sizeof, an unknown macro). Empty when the value is known. */
    QString unresolvedInstance;
    QString unresolvedSize;
};

enum class ConnType { ExclusiveOwner, InputOnly };

struct Connection {
    bool enabled = true;
    ConnType type = ConnType::ExclusiveOwner;
    QString name;
    QString help;
    int o2t = 0;                /* consumed instance; the size-0 heartbeat for input only */
    int t2o = 0;                /* produced instance */
    int config = 0;             /* config instance, 0 for none */
};

struct EdsModel {
    /* [File] */
    QString descText;
    QDate createDate;
    QTime createTime;
    QDate modDate;
    QTime modTime;
    int fileMajor = 1;
    int fileMinor = 0;
    QString homeUrl;

    /* [Device] */
    int vendCode = 0;
    QString vendName;
    int prodType = 43;
    QString prodTypeStr;
    int prodCode = 0;
    int majRev = 1;
    int minRev = 1;
    QString prodName;
    QString catalog;
    QString iconPath;           /* .ico file embedded as IconContents; may be empty */

    QList<Assembly> assemblies;
    QList<Connection> connections;

    /* The one RPI parameter every connection refers to, in microseconds. */
    int rpiMinUs = 10000;
    int rpiMaxUs = 1000000;
    int rpiDefaultUs = 20000;
    bool t2oMulticast = true;   /* T->O may be multicast as well as point-to-point */

    /* [Capacity] and the object class sections. */
    int maxIoConnections = 1;   /* the adapter has one Class 1 connection */
    int maxMsgConnections = 0;  /* Class 3 is refused; explicit messages are unconnected */
    int ethLinkInstances = 3;   /* ETH_LINK_INSTANCES in eip_adapter.c */
    bool classSections = true;  /* write [Identity Class] and the two network class sections */

    /* Limits of the build the EDS is for (eip_config.h). Only the validator uses them. */
    int limitAssemblies = 8;
    int limitPool = 512;

    QString sourceNote;         /* "examples/x/main.c:31", for the header comment */

    const Assembly *findAssembly(int instance) const;
};

/* The name ODVA gives a device type, or an empty string for one not in the table. */
QString deviceTypeName(int prodType);

QString asmDirName(AsmDir dir);

/* Replace model.connections with every connection the assemblies allow: one
 * exclusive owner per consumed/produced pair, and one input only per produced
 * assembly when a size-0 consumed assembly can serve as the heartbeat. The
 * enabled flag and name of a connection that still exists are kept. */
void rebuildConnections(EdsModel &model);

#endif /* EDSMODEL_H_ */
