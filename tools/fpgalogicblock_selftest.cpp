#include "fpgalogicblock.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    FpgaLogicBlock block;
    if (!block.validate().isEmpty()) return 1;
    QString error;
    if (!block.generateVerilog(&error).contains("assign out_i = in_i;")) return 2;

    FpgaLogicNode constant;
    constant.id = 2;
    constant.op = QStringLiteral("const16");
    constant.constant = 100;
    block.nodes << constant;
    FpgaLogicNode add;
    add.id = 3;
    add.op = QStringLiteral("add16");
    add.inputs = {0, 2};
    block.nodes << add;
    FpgaLogicNode delayI;
    delayI.id = 4;
    delayI.op = QStringLiteral("reg16");
    delayI.inputs = {3};
    block.nodes << delayI;
    FpgaLogicNode delayQ = delayI;
    delayQ.id = 5;
    delayQ.inputs = {1};
    block.nodes << delayQ;
    block.outputI = 4;
    block.outputQ = 5;
    if (!block.validate().isEmpty()) return 3;
    const QByteArray rtl = block.generateVerilog(&error);
    if (!rtl.contains("valid_1") || !rtl.contains("n4 <= n3;") ||
        !rtl.contains("assign n3 = in_i + n2;")) return 4;
    if (!block.generateVerilog(&error, QStringLiteral("bad;module")).isEmpty() ||
        !block.generateVerilog(&error, QStringLiteral("logic_variant")).contains("module logic_variant")) return 14;
    FpgaLogicBlock restored;
    if (!FpgaLogicBlock::fromJson(QJsonDocument::fromJson(
        QJsonDocument(block.toJson()).toJson()).object(), &restored, &error) ||
        restored.nodes.size() != 4 || restored.outputI != 4 ||
        restored.generateVerilog(&error) != rtl) return 5;
    if (argc > 1) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        if (!file.open(QIODevice::WriteOnly) || file.write(rtl) != rtl.size()) return 6;
    }

    block.outputQ = 1;
    if (block.validate().isEmpty()) return 7;
    block.outputQ = 5;
    block.nodes[1].inputs[1] = 99;
    if (block.validate().isEmpty()) return 8;
    block.nodes[1].inputs[1] = 2;
    block.nodes[0].op = QStringLiteral("add16");
    block.nodes[0].inputs = {3, 0};
    if (block.validate().isEmpty()) return 9;
    block.nodes[0] = constant;

    FpgaLogicNode lut;
    lut.id = 6;
    lut.op = QStringLiteral("lut4");
    lut.inputs = {0, 1, 0, 1};
    lut.truthTable = 0x6996;
    block.nodes << lut;
    if (!block.generateVerilog(&error).contains("genvar bit_6")) return 10;
    FpgaLogicNode multiply;
    multiply.id = 7;
    multiply.op = QStringLiteral("mul_q15");
    multiply.inputs = {0, 1};
    block.nodes << multiply;
    block.outputI = 7;
    block.outputQ = 6;
    if (argc > 2) {
        const QByteArray complexRtl = block.generateVerilog(&error);
        QFile file(QString::fromLocal8Bit(argv[2]));
        if (complexRtl.isEmpty() || !file.open(QIODevice::WriteOnly) ||
            file.write(complexRtl) != complexRtl.size()) return 13;
    }
    block.outputI = 4;
    block.outputQ = 5;
    block.nodes.removeLast();
    block.nodes.last().truthTable = 0x10000;
    if (block.validate().isEmpty()) return 11;
    block.nodes.removeLast();

    QJsonObject broken = block.toJson();
    broken.insert(QStringLiteral("clockDomain"), QStringLiteral("other"));
    if (FpgaLogicBlock::fromJson(broken, &restored, &error) ||
        restored.generateVerilog(nullptr) != rtl) return 12;
    return 0;
}
