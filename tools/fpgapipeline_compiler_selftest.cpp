#include "fpgapipelinecompiler.h"
#include "fpgalogicblock.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QVector<FpgaPipelineStage> stages;
    const auto passthrough = FpgaPipelineCompiler::analyze(stages, 1000000.0, 2000000.0);
    if (!passthrough.canGenerateRtl() || passthrough.outputRateHz != 1000000.0) return 1;
    QString error;
    const auto emptyRtl = FpgaPipelineCompiler::generateVerilog(stages, 1000000.0, 2000000.0, &error);
    if (!emptyRtl.contains("assign out_i = in_i;") || emptyRtl.contains("obrii_iq_avg_decimator #")) return 2;

    FpgaPipelineStage decimator;
    decimator.type = QStringLiteral("decimator");
    decimator.factor = 2;
    stages << decimator;
    decimator.factor = 4;
    stages << decimator;
    const auto check = FpgaPipelineCompiler::analyze(stages, 1000000.0, 2000000.0);
    if (!check.canGenerateRtl() || check.outputRateHz != 125000.0 ||
        check.contracts.size() != 2 || check.contracts[0].outputRateHz != 500000.0 ||
        check.contracts[1].inputRateHz != 500000.0 || check.warnings.isEmpty()) return 3;
    const auto rtl = FpgaPipelineCompiler::generateVerilog(stages, 1000000.0, 2000000.0, &error);
    if (!rtl.contains(".FACTOR(2)") || !rtl.contains(".FACTOR(4)") ||
        !rtl.contains("sum_i >>> SHIFT") || !rtl.contains("assign out_i = s1_i;")) return 4;
    QTemporaryDir temp;
    const QString filePath = temp.filePath(QStringLiteral("obrii_iq_pipeline.v"));
    if (!temp.isValid() || !FpgaPipelineCompiler::writeVerilog(stages, 1000000.0,
                                                                2000000.0, filePath, &error)) return 5;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != rtl) return 6;
    if (FpgaPipelineCompiler::analyze(stages, 3000000.0, 2000000.0).canGenerateRtl()) return 7;
    stages[0].factor = 3;
    if (FpgaPipelineCompiler::analyze(stages, 1000000.0, 2000000.0).canGenerateRtl()) return 8;
    stages[0].type = QStringLiteral("ddc");
    stages[0].frequencyHz = 600000.0;
    if (FpgaPipelineCompiler::analyze(stages, 1000000.0, 2000000.0).canGenerateRtl()) return 9;
    if (!FpgaPipelineCompiler::generateVerilog(stages, 1000000.0, 2000000.0, &error).isEmpty() ||
        error.isEmpty()) return 10;

    QVector<FpgaPipelineStage> complete;
    FpgaPipelineStage ddc;
    ddc.type = QStringLiteral("ddc");
    ddc.frequencyHz = 250000.0;
    complete << ddc;
    FpgaPipelineStage fir;
    fir.type = QStringLiteral("fir");
    fir.bandwidthHz = 100000.0;
    complete << fir;
    decimator.factor = 2;
    complete << decimator;
    FpgaPipelineStage trigger;
    trigger.type = QStringLiteral("trigger");
    trigger.thresholdDbfs = -30.0;
    complete << trigger;
    const auto fullCheck = FpgaPipelineCompiler::analyze(complete, 1000000.0, 2000000.0);
    if (!fullCheck.canGenerateRtl() || fullCheck.contracts.size() != 4 ||
        fullCheck.outputRateHz != 500000.0 || !fullCheck.contracts.last().eventOutput ||
        fullCheck.contracts.last().inputRateHz != 500000.0) return 11;
    const auto fullRtl = FpgaPipelineCompiler::generateVerilog(complete, 1000000.0, 2000000.0, &error);
    if (!fullRtl.contains("module obrii_iq_ddc") ||
        !fullRtl.contains("module obrii_iq_fir_s1") ||
        !fullRtl.contains("module obrii_iq_avg_decimator") ||
        !fullRtl.contains("module obrii_iq_level_trigger") ||
        !fullRtl.contains("assign out_trigger = s3_trigger;") ||
        !fullRtl.contains("pair_i_0") || !fullRtl.contains("pair_q_8") ||
        !fullRtl.contains(".STEP(32'd") || !fullRtl.contains(".THRESHOLD(33'd")) return 12;
    if (argc > 1 && !FpgaPipelineCompiler::writeVerilog(complete, 1000000.0, 2000000.0,
        QString::fromLocal8Bit(argv[1]), &error)) return 13;
    complete.swapItemsAt(2, 3);
    if (FpgaPipelineCompiler::analyze(complete, 1000000.0, 2000000.0).canGenerateRtl()) return 14;
    complete.swapItemsAt(2, 3);
    complete[1].bandwidthHz = 500000.0;
    if (FpgaPipelineCompiler::analyze(complete, 1000000.0, 2000000.0).canGenerateRtl()) return 15;
    complete[1].bandwidthHz = 499999.0;
    const auto edgeRtl = FpgaPipelineCompiler::generateVerilog(complete, 1000000.0, 2000000.0, &error);
    if (edgeRtl.isEmpty() || edgeRtl.contains("pair_i_8 * 16'sd32768") ||
        !edgeRtl.contains("pair_i_8 * 17'sd32768")) return 16;
    complete[0].frequencyHz = -250000.0;
    const auto negativeRtl = FpgaPipelineCompiler::generateVerilog(complete, 1000000.0, 2000000.0, &error);
    if (!negativeRtl.contains(".STEP(32'd3221225472)")) return 17;

    FpgaLogicBlock logic;
    FpgaLogicNode constant;
    constant.id = 2;
    constant.op = QStringLiteral("const16");
    constant.constant = 100;
    logic.nodes << constant;
    FpgaLogicNode sum;
    sum.id = 3;
    sum.op = QStringLiteral("add16");
    sum.inputs = {0, 2};
    logic.nodes << sum;
    logic.outputI = 3;
    FpgaPipelineStage custom;
    custom.type = QStringLiteral("custom");
    custom.customLogic = logic.toJson();
    stages = {custom, custom, decimator};
    const auto customCheck = FpgaPipelineCompiler::analyze(stages, 1000000.0, 2000000.0);
    if (!customCheck.canGenerateRtl() || customCheck.outputRateHz != 500000.0 ||
        customCheck.contracts.size() != 3) return 18;
    const QByteArray customRtl = FpgaPipelineCompiler::generateVerilog(stages, 1000000.0, 2000000.0, &error);
    if (!customRtl.contains("module obrii_user_logic_s0") ||
        !customRtl.contains("module obrii_user_logic_s1") ||
        !customRtl.contains("obrii_user_logic_s0 s0 (") ||
        !customRtl.contains("obrii_user_logic_s1 s1 (")) return 19;
    if (argc > 2 && !FpgaPipelineCompiler::writeVerilog(stages, 1000000.0, 2000000.0,
        QString::fromLocal8Bit(argv[2]), &error)) return 20;
    stages[0].customLogic.insert(QStringLiteral("clockDomain"), QStringLiteral("other"));
    if (FpgaPipelineCompiler::analyze(stages, 1000000.0, 2000000.0).canGenerateRtl() ||
        !FpgaPipelineCompiler::generateVerilog(stages, 1000000.0, 2000000.0, &error).isEmpty()) return 21;
    return 0;
}
