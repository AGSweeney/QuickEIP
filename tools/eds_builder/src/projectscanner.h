/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Finds the adapter definitions in a QuickEIP project tree by reading its C
 * and C++ sources: EipAdapterConfig variables, EipAssemblyConfig arrays, and
 * calls to eip_adapter_set_identity / eip_adapter_add_assembly. Nothing is
 * compiled, so values are worked out from literals, #defines, enums and
 * integer constants. A value it cannot work out (sizeof, a computed size) is
 * reported as unresolved for the user to enter. */
#ifndef PROJECTSCANNER_H_
#define PROJECTSCANNER_H_

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "edsmodel.h"

struct ScanCandidate {
    QString file;                   /* relative to the scanned root, forward slashes */
    int line = 0;
    QString label;                  /* one line for a list: where it is and what was found */
    bool builtInTestSet = false;    /* eip_adapter_start(0): the identity and assemblies of eip_adapter_init */
    std::optional<int> vendorId;
    std::optional<int> deviceType;
    std::optional<int> productCode;
    std::optional<int> revisionMajor;
    std::optional<int> revisionMinor;
    std::optional<QString> productName;
    QList<Assembly> assemblies;
    QStringList notes;              /* what could not be read */

    bool identityComplete() const
    {
        return vendorId && deviceType && productCode && revisionMajor && revisionMinor && productName;
    }
};

struct ScanResult {
    QString root;
    int filesScanned = 0;
    QList<ScanCandidate> candidates;    /* most complete first */
    /* eip_config.h values of the project, or the library defaults. */
    int limitAssemblies = 8;
    int limitPool = 512;
    int maxClients = 4;
};

ScanResult scanProject(const QString &root);

/* Put a candidate's identity and assemblies into the model and rebuild its
 * connections. Fields the candidate does not have are left as they are. */
void applyCandidate(const ScanResult &result, const ScanCandidate &candidate, EdsModel &model);

#endif /* PROJECTSCANNER_H_ */
