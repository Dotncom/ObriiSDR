#ifndef APPHELP_H
#define APPHELP_H

#include <QString>
#include <QVector>

class QWidget;

struct ApplicationHelpSection {
    QString title;
    QString text;
};

QString applicationHelpText(const QString &language);
QVector<ApplicationHelpSection> applicationHelpSections(const QString &language);
void showApplicationHelpDialog(QWidget *parent, const QString &language,
                               const QString &initialSection = QString());

#endif // APPHELP_H
