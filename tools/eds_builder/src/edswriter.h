/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#ifndef EDSWRITER_H_
#define EDSWRITER_H_

#include <QString>

#include "edsmodel.h"

#define EDS_BUILDER_VERSION "0.1"

/* The EDS text for the model. It writes whatever the model holds: run
 * validateModel first to know whether the result is a valid file. */
QString writeEds(const EdsModel &model);

/* EPATH strings as an EDS wants them: hex bytes separated by spaces. */
QString assemblyPath(int instance);
QString connectionPath(int config, int o2t, int t2o);

/* The 32-bit masks of a ConnectionN entry. */
quint32 triggerTransportMask(ConnType type);
quint32 connectionParamsMask(ConnType type, bool t2oMulticast);

#endif /* EDSWRITER_H_ */
