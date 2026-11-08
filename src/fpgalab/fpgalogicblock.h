#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

struct FpgaLogicNode {
    int id = -1;
    QString op;
    QVector<int> inputs;
    int constant = 0;
    unsigned int truthTable = 0x6996;
    QPointF position;
};

class FpgaLogicBlock {
public:
    QString name = QStringLiteral("Власний блок");
    QVector<FpgaLogicNode> nodes;
    int outputI = 0;
    int outputQ = 1;

    static int inputCount(const QString &op);
    static QString opLabel(const QString &op);
    QStringList validate() const;
    int outputLatencyClocks() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, FpgaLogicBlock *out, QString *error);
    QByteArray generateVerilog(QString *error, const QString &moduleName = QStringLiteral("obrii_user_logic")) const;
};
