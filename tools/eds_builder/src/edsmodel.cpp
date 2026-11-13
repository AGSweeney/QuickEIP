/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "edsmodel.h"

const Assembly *EdsModel::findAssembly(int instance) const
{
    for (const Assembly &a : assemblies) {
        if (a.instance == instance) {
            return &a;
        }
    }
    return nullptr;
}

QString deviceTypeName(int prodType)
{
    /* CIP Vol 1 chapter 6, the profiles a QuickEIP device is likely to claim. */
    switch (prodType) {
    case 0x00: return QStringLiteral("Generic Device (deprecated)");
    case 0x02: return QStringLiteral("AC Drive");
    case 0x07: return QStringLiteral("General Purpose Discrete I/O");
    case 0x0C: return QStringLiteral("Communications Adapter");
    case 0x0E: return QStringLiteral("Programmable Logic Controller");
    case 0x10: return QStringLiteral("Position Controller");
    case 0x13: return QStringLiteral("DC Drive");
    case 0x18: return QStringLiteral("Human-Machine Interface");
    case 0x22: return QStringLiteral("Encoder");
    case 0x25: return QStringLiteral("CIP Motion Drive");
    case 0x2B: return QStringLiteral("Generic Device");
    default: return QString();
    }
}

QString asmDirName(AsmDir dir)
{
    switch (dir) {
    case AsmDir::Consumed: return QStringLiteral("Consumed (O->T)");
    case AsmDir::Produced: return QStringLiteral("Produced (T->O)");
    case AsmDir::Config: return QStringLiteral("Config");
    default: return QStringLiteral("Unknown");
    }
}

void rebuildConnections(EdsModel &model)
{
    const QList<Connection> old = model.connections;
    QList<Connection> out;
    int config = 0;
    int heartbeat = 0;

    for (const Assembly &a : model.assemblies) {
        if (a.dir == AsmDir::Config && config == 0) {
            config = a.instance;
        }
        if (a.dir == AsmDir::Consumed && a.size == 0 && heartbeat == 0) {
            heartbeat = a.instance;
        }
    }

    auto add = [&](ConnType type, const Assembly &o2t, const Assembly &t2o, const QString &name) {
        Connection c;
        c.type = type;
        c.o2t = o2t.instance;
        c.t2o = t2o.instance;
        c.config = config;
        c.name = name;
        for (const Connection &p : old) {
            if (p.type == c.type && p.o2t == c.o2t && p.t2o == c.t2o) {
                c.enabled = p.enabled;
                c.name = p.name;
                c.help = p.help;
                break;
            }
        }
        out.append(c);
    };

    QList<const Assembly *> consumed;
    QList<const Assembly *> produced;
    for (const Assembly &a : model.assemblies) {
        if (a.dir == AsmDir::Consumed && a.size > 0) {
            consumed.append(&a);
        } else if (a.dir == AsmDir::Produced) {
            produced.append(&a);
        }
    }
    const bool single = (consumed.size() == 1 && produced.size() == 1);
    for (const Assembly *o : consumed) {
        for (const Assembly *t : produced) {
            add(ConnType::ExclusiveOwner, *o, *t,
                single ? QStringLiteral("Exclusive Owner")
                       : QStringLiteral("Exclusive Owner %1/%2").arg(o->instance).arg(t->instance));
        }
    }
    if (heartbeat != 0) {
        const Assembly *hb = model.findAssembly(heartbeat);
        for (const Assembly *t : produced) {
            add(ConnType::InputOnly, *hb, *t,
                produced.size() == 1 ? QStringLiteral("Input Only")
                                     : QStringLiteral("Input Only %1").arg(t->instance));
        }
    }
    model.connections = out;
}
