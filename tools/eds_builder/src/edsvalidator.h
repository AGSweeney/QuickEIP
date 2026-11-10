/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#ifndef EDSVALIDATOR_H_
#define EDSVALIDATOR_H_

#include <QList>
#include <QString>

#include "edsmodel.h"

struct Issue {
    enum Severity { Error, Warning };
    Severity severity;
    QString where;      /* "[Device] ProdCode", "Assem100", ... */
    QString text;
};

/* Check the model against the EDS rules EZ-EDS enforces (CIP Vol 1 chapter 7)
 * and against what the QuickEIP adapter accepts in a Forward_Open. An EDS
 * written from a model with no errors is a valid file. */
QList<Issue> validateModel(const EdsModel &model);

bool hasErrors(const QList<Issue> &issues);

#endif /* EDSVALIDATOR_H_ */
