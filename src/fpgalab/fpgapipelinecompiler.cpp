#include "fpgapipelinecompiler.h"
#include "fpgalogicblock.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
bool powerOfTwo(int value) {
    return value >= 2 && value <= 1024 && (value & (value - 1)) == 0;
}

QString signedCoefficient(int value) {
    return value < 0 ? QStringLiteral("-17'sd%1").arg(-value)
                     : QStringLiteral("17'sd%1").arg(value);
}

QVector<int> lowpassCoefficients(double cutoffHz, double sampleRateHz) {
    constexpr int taps = 17;
    constexpr double pi = 3.14159265358979323846;
    const double fc = cutoffHz / sampleRateHz;
    QVector<double> floating;
    double total = 0.0;
    for (int index = 0; index < taps; ++index) {
        const int offset = index - taps / 2;
        const double sinc = offset == 0 ? 2.0 * fc
            : std::sin(2.0 * pi * fc * offset) / (pi * offset);
        const double hamming = 0.54 - 0.46 * std::cos(2.0 * pi * index / (taps - 1));
        floating << sinc * hamming;
        total += floating.last();
    }
    QVector<int> quantized;
    int integerTotal = 0;
    for (double value : floating) {
        const int coefficient = static_cast<int>(std::lround(value / total * 32768.0));
        quantized << coefficient;
        integerTotal += coefficient;
    }
    quantized[taps / 2] += 32768 - integerTotal;
    return quantized;
}

QString ddcDefinition() {
    constexpr double pi = 3.14159265358979323846;
    QStringList lines = {
        QStringLiteral("module obrii_iq_ddc #(parameter [31:0] STEP = 32'd0) ("),
        QStringLiteral("    input wire clk, reset, in_valid,"),
        QStringLiteral("    input wire signed [15:0] in_i, in_q,"),
        QStringLiteral("    output reg out_valid,"),
        QStringLiteral("    output reg signed [15:0] out_i, out_q"),
        QStringLiteral(");"),
        QStringLiteral("reg [31:0] phase;"),
        QStringLiteral("function signed [15:0] quarter_sin;"),
        QStringLiteral("input [8:0] address;"),
        QStringLiteral("begin case (address)")};
    for (int index = 0; index <= 256; ++index) {
        const int value = static_cast<int>(std::lround(std::sin(pi * index / 512.0) * 32767.0));
        lines << QStringLiteral("9'd%1: quarter_sin = 16'sd%2;").arg(index).arg(value);
    }
    lines << QStringLiteral("default: quarter_sin = 16'sd0;")
          << QStringLiteral("endcase end endfunction")
          << QString::fromLatin1(R"verilog(
function signed [15:0] full_sin;
input [9:0] angle;
reg [8:0] mirror;
begin
    mirror = 9'd256 - {1'b0, angle[7:0]};
    case (angle[9:8])
        2'd0: full_sin = quarter_sin({1'b0, angle[7:0]});
        2'd1: full_sin = quarter_sin(mirror);
        2'd2: full_sin = -quarter_sin({1'b0, angle[7:0]});
        default: full_sin = -quarter_sin(mirror);
    endcase
end
endfunction
wire signed [15:0] sine = full_sin(phase[31:22]);
wire signed [15:0] cosine = full_sin(phase[31:22] + 10'd256);
wire signed [31:0] ic = in_i * cosine;
wire signed [31:0] qs = in_q * sine;
wire signed [31:0] qc = in_q * cosine;
wire signed [31:0] isine = in_i * sine;
wire signed [32:0] mixed_i = {ic[31], ic} + {qs[31], qs};
wire signed [32:0] mixed_q = {qc[31], qc} - {isine[31], isine};
function signed [15:0] sat16;
input signed [32:0] value;
begin
    if (value > 33'sd32767) sat16 = 16'sd32767;
    else if (value < -33'sd32768) sat16 = -16'sd32768;
    else sat16 = value[15:0];
end
endfunction
always @(posedge clk) begin
    if (reset) begin
        phase <= 0;
        out_valid <= 0;
        out_i <= 0;
        out_q <= 0;
    end else begin
        out_valid <= in_valid;
        if (in_valid) begin
            out_i <= sat16(mixed_i >>> 15);
            out_q <= sat16(mixed_q >>> 15);
            phase <= phase + STEP;
        end
    end
end
endmodule
)verilog");
    return lines.join(QLatin1Char('\n'));
}

QString firDefinition(int stageIndex, double cutoffHz, double sampleRateHz) {
    const QVector<int> coefficients = lowpassCoefficients(cutoffHz, sampleRateHz);
    const QString name = QStringLiteral("obrii_iq_fir_s%1").arg(stageIndex);
    QStringList lines = {
        QStringLiteral("module %1 (").arg(name),
        QStringLiteral("    input wire clk, reset, in_valid,"),
        QStringLiteral("    input wire signed [15:0] in_i, in_q,"),
        QStringLiteral("    output reg out_valid,"),
        QStringLiteral("    output reg signed [15:0] out_i, out_q"),
        QStringLiteral(");"),
        QStringLiteral("reg signed [15:0] delay_i [0:15];"),
        QStringLiteral("reg signed [15:0] delay_q [0:15];")};
    QString sumI, sumQ;
    for (const QString &channel : {QStringLiteral("i"), QStringLiteral("q")}) {
        // Pair symmetric taps before multiplication to conserve DSP slices.
        const auto tapName = [&channel](int index) {
            return index == 0 ? QStringLiteral("in_%1").arg(channel)
                : QStringLiteral("delay_%1[%2]").arg(channel).arg(index - 1);
        };
        QStringList level;
        for (int index = 0; index <= coefficients.size() / 2; ++index) {
            const QString first = tapName(index);
            const QString last = tapName(coefficients.size() - 1 - index);
            const QString pair = QStringLiteral("pair_%1_%2").arg(channel).arg(index);
            if (index == coefficients.size() / 2) {
                lines << QStringLiteral("wire signed [16:0] %1 = {%2[15], %2};").arg(pair, first);
            } else {
                lines << QStringLiteral("wire signed [16:0] %1 = $signed({%2[15], %2}) + $signed({%3[15], %3});")
                             .arg(pair, first, last);
            }
            const QString product = QStringLiteral("p%1_%2").arg(channel).arg(index);
            lines << QStringLiteral("wire signed [33:0] %1 = %2 * %3;")
                         .arg(product, pair, signedCoefficient(coefficients[index]));
            const QString wire = QStringLiteral("s%1_0_%2").arg(channel).arg(index);
            lines << QStringLiteral("wire signed [39:0] %1 = {{6{%2[33]}}, %2};")
                         .arg(wire, product);
            level << wire;
        }
        for (int depth = 1; level.size() > 1; ++depth) {
            QStringList next;
            for (int index = 0; index < level.size(); index += 2) {
                if (index + 1 == level.size()) {
                    next << level[index];
                } else {
                    const QString wire = QStringLiteral("s%1_%2_%3").arg(channel).arg(depth).arg(index / 2);
                    lines << QStringLiteral("wire signed [39:0] %1 = %2 + %3;")
                                 .arg(wire, level[index], level[index + 1]);
                    next << wire;
                }
            }
            level = next;
        }
        if (channel == QStringLiteral("i")) sumI = level.first();
        else sumQ = level.first();
    }
    lines << QString::fromLatin1(R"verilog(
function signed [15:0] sat16;
input signed [39:0] value;
begin
    if (value > 40'sd32767) sat16 = 16'sd32767;
    else if (value < -40'sd32768) sat16 = -16'sd32768;
    else sat16 = value[15:0];
end
endfunction
integer tap;
always @(posedge clk) begin
    if (reset) begin
        out_valid <= 0;
        out_i <= 0;
        out_q <= 0;
        for (tap = 0; tap < 16; tap = tap + 1) begin
            delay_i[tap] <= 0;
            delay_q[tap] <= 0;
        end
    end else begin
        out_valid <= in_valid;
        if (in_valid) begin
            delay_i[0] <= in_i;
            delay_q[0] <= in_q;
            for (tap = 1; tap < 16; tap = tap + 1) begin
                delay_i[tap] <= delay_i[tap - 1];
                delay_q[tap] <= delay_q[tap - 1];
            end
)verilog")
          << QStringLiteral("            out_i <= sat16(%1 >>> 15);").arg(sumI)
          << QStringLiteral("            out_q <= sat16(%1 >>> 15);").arg(sumQ)
          << QStringLiteral("        end\n    end\nend\nendmodule");
    return lines.join(QLatin1Char('\n'));
}

QString triggerDefinition() {
    return QString::fromLatin1(R"verilog(
module obrii_iq_level_trigger #(parameter [32:0] THRESHOLD = 33'd1) (
    input wire clk, reset, in_valid,
    input wire signed [15:0] in_i, in_q,
    output reg out_valid, out_trigger,
    output reg signed [15:0] out_i, out_q
);
wire signed [31:0] square_i = in_i * in_i;
wire signed [31:0] square_q = in_q * in_q;
wire [32:0] power = {1'b0, square_i} + {1'b0, square_q};
always @(posedge clk) begin
    if (reset) begin
        out_valid <= 0;
        out_trigger <= 0;
        out_i <= 0;
        out_q <= 0;
    end else begin
        out_valid <= in_valid;
        out_trigger <= in_valid && power >= THRESHOLD;
        if (in_valid) begin
            out_i <= in_i;
            out_q <= in_q;
        end
    end
end
endmodule
)verilog");
}
}

FpgaPipelineAnalysis FpgaPipelineCompiler::analyze(const QVector<FpgaPipelineStage> &stages,
                                                   double inputRateHz, double clockRateHz) {
    FpgaPipelineAnalysis result;
    if (!std::isfinite(inputRateHz) || inputRateHz <= 0.0 || inputRateHz > 1.0e9)
        result.errors << QStringLiteral("Частота IQ-входу має бути в межах 1...1 000 000 000 S/s.");
    if (!std::isfinite(clockRateHz) || clockRateHz <= 0.0 || clockRateHz > 1.0e9)
        result.errors << QStringLiteral("Частота такту має бути в межах 1...1 000 000 000 Hz.");
    if (std::isfinite(inputRateHz) && std::isfinite(clockRateHz) &&
        inputRateHz > clockRateHz + 0.001)
        result.errors << QStringLiteral("Потік IQ швидший за один відлік на такт.");
    if (stages.size() > 64)
        result.errors << QStringLiteral("Ліміт ескізу: 64 блоки.");

    double rate = inputRateHz;
    for (int index = 0; index < stages.size(); ++index) {
        const auto &stage = stages.at(index);
        const QString prefix = QStringLiteral("Блок %1: ").arg(index + 1);
        FpgaPipelineContract contract;
        contract.type = stage.type;
        contract.inputRateHz = rate;
        contract.outputRateHz = rate;
        if (stage.type == QStringLiteral("ddc")) {
            if (!std::isfinite(stage.frequencyHz) || std::abs(stage.frequencyHz) > rate / 2.0)
                result.errors << prefix + QStringLiteral("зсув DDC за межами Найквіста.");
        } else if (stage.type == QStringLiteral("fir")) {
            if (!std::isfinite(stage.bandwidthHz) || stage.bandwidthHz <= 0.0 ||
                stage.bandwidthHz >= rate / 2.0)
                result.errors << prefix + QStringLiteral("частота зрізу FIR має бути нижче Найквіста.");
            else if (stage.bandwidthHz < rate / 32.0)
                result.warnings << prefix + QStringLiteral("17-tap FIR має широку перехідну смугу для такого зрізу.");
        } else if (stage.type == QStringLiteral("decimator")) {
            if (stage.factor < 2 || stage.factor > 1024)
                result.errors << prefix + QStringLiteral("коефіцієнт децимації має бути 2...1024.");
            else contract.outputRateHz = rate / stage.factor;
            if (!powerOfTwo(stage.factor))
                result.rtlBlockers << prefix + QStringLiteral("RTL усереднення поки підтримує лише степені двійки.");
            result.warnings << prefix + QStringLiteral("boxcar не гарантує придушення аліасів; потрібен FIR перед децимацією.");
        } else if (stage.type == QStringLiteral("trigger")) {
            if (!std::isfinite(stage.thresholdDbfs) || stage.thresholdDbfs < -160.0 ||
                stage.thresholdDbfs > 0.0)
                result.errors << prefix + QStringLiteral("поріг має бути -160...0 dBFS.");
            if (index + 1 != stages.size())
                result.errors << prefix + QStringLiteral("тригер має бути останнім блоком, щоб подія збігалася з вихідним IQ.");
            contract.eventOutput = true;
        } else if (stage.type == QStringLiteral("custom")) {
            FpgaLogicBlock block;
            QString error;
            if (!FpgaLogicBlock::fromJson(stage.customLogic, &block, &error))
                result.errors << prefix + QStringLiteral("власний блок: ") + error;
        } else if (stage.type != QStringLiteral("passthrough")) {
            result.errors << prefix + QStringLiteral("невідомий тип блоку.");
        }
        result.contracts << contract;
        rate = contract.outputRateHz;
    }
    result.outputRateHz = rate;
    return result;
}

QByteArray FpgaPipelineCompiler::generateVerilog(const QVector<FpgaPipelineStage> &stages,
                                                 double inputRateHz, double clockRateHz,
                                                 QString *error) {
    const auto check = analyze(stages, inputRateHz, clockRateHz);
    if (!check.canGenerateRtl()) {
        if (error) *error = (check.errors + check.rtlBlockers).join(QLatin1Char('\n'));
        return {};
    }
    QStringList lines = {
        QStringLiteral("// Standalone signed IQ16 stream; no B210 top-level integration."),
        QStringLiteral("// clk is a single processing clock; rates change through valid strobes."),
        QStringLiteral("module obrii_iq_pipeline ("),
        QStringLiteral("    input wire clk, input wire reset, input wire in_valid,"),
        QStringLiteral("    input wire signed [15:0] in_i, in_q,"),
        QStringLiteral("    output wire out_valid, out_trigger,"),
        QStringLiteral("    output wire signed [15:0] out_i, out_q"),
        QStringLiteral(");")};
    QString previousValid = QStringLiteral("in_valid");
    QString previousI = QStringLiteral("in_i");
    QString previousQ = QStringLiteral("in_q");
    QString triggerWire = QStringLiteral("1'b0");
    QStringList definitions;
    QStringList customDefinitions;
    for (int index = 0; index < stages.size(); ++index) {
        const auto &stage = stages.at(index);
        if (stage.type == QStringLiteral("passthrough")) continue;
        const QString prefix = QStringLiteral("s%1").arg(index);
        lines << QStringLiteral("wire %1_valid;").arg(prefix)
              << QStringLiteral("wire signed [15:0] %1_i, %1_q;").arg(prefix);
        if (stage.type == QStringLiteral("ddc")) {
            const auto step = static_cast<std::uint32_t>(
                std::llround(stage.frequencyHz / check.contracts[index].inputRateHz * 4294967296.0));
            lines << QStringLiteral("obrii_iq_ddc #(.STEP(32'd%1)) %2 (").arg(step).arg(prefix);
            if (!definitions.contains(QStringLiteral("ddc"))) definitions << QStringLiteral("ddc");
        } else if (stage.type == QStringLiteral("fir")) {
            lines << QStringLiteral("obrii_iq_fir_s%1 %2 (").arg(index).arg(prefix);
            definitions << firDefinition(index, stage.bandwidthHz, check.contracts[index].inputRateHz);
        } else if (stage.type == QStringLiteral("decimator")) {
            lines << QStringLiteral("obrii_iq_avg_decimator #(.FACTOR(%1)) %2 (")
                         .arg(stage.factor).arg(prefix);
            if (!definitions.contains(QStringLiteral("decimator"))) definitions << QStringLiteral("decimator");
        } else if (stage.type == QStringLiteral("trigger")) {
            const double ratio = std::pow(10.0, stage.thresholdDbfs / 10.0);
            const auto threshold = static_cast<std::uint64_t>(
                std::max(1.0, std::round(ratio * 32767.0 * 32767.0)));
            lines << QStringLiteral("wire %1_trigger;").arg(prefix)
                  << QStringLiteral("obrii_iq_level_trigger #(.THRESHOLD(33'd%1)) %2 (")
                         .arg(threshold).arg(prefix);
            triggerWire = prefix + QStringLiteral("_trigger");
            if (!definitions.contains(QStringLiteral("trigger"))) definitions << QStringLiteral("trigger");
        } else if (stage.type == QStringLiteral("custom")) {
            FpgaLogicBlock block;
            QString parseError;
            if (!FpgaLogicBlock::fromJson(stage.customLogic, &block, &parseError)) {
                if (error) *error = parseError;
                return {};
            }
            const QString moduleName = QStringLiteral("obrii_user_logic_s%1").arg(index);
            const QByteArray module = block.generateVerilog(&parseError, moduleName);
            if (module.isEmpty()) {
                if (error) *error = parseError;
                return {};
            }
            lines << QStringLiteral("%1 %2 (").arg(moduleName, prefix);
            customDefinitions << QString::fromUtf8(module);
        }
        lines << QStringLiteral("    .clk(clk), .reset(reset), .in_valid(%1),").arg(previousValid)
              << QStringLiteral("    .in_i(%1), .in_q(%2),").arg(previousI, previousQ);
        if (stage.type == QStringLiteral("trigger"))
            lines << QStringLiteral("    .out_trigger(%1_trigger),").arg(prefix);
        lines << QStringLiteral("    .out_valid(%1_valid), .out_i(%1_i), .out_q(%1_q)").arg(prefix)
              << QStringLiteral(");");
        previousValid = prefix + QStringLiteral("_valid");
        previousI = prefix + QStringLiteral("_i");
        previousQ = prefix + QStringLiteral("_q");
    }
    lines << QStringLiteral("assign out_valid = %1;").arg(previousValid)
          << QStringLiteral("assign out_i = %1;").arg(previousI)
          << QStringLiteral("assign out_q = %1;").arg(previousQ)
          << QStringLiteral("assign out_trigger = %1;").arg(triggerWire)
          << QStringLiteral("endmodule");
    if (definitions.contains(QStringLiteral("ddc"))) lines << ddcDefinition();
    if (definitions.contains(QStringLiteral("decimator"))) {
        lines << QString::fromLatin1(R"verilog(

// Boxcar averaging suppresses some aliases but is not a specified FIR response.
module obrii_iq_avg_decimator #(
    parameter integer WIDTH = 16,
    parameter integer FACTOR = 2,
    parameter integer SHIFT = $clog2(FACTOR),
    parameter integer ACC_WIDTH = WIDTH + SHIFT
) (
    input wire clk, input wire reset, input wire in_valid,
    input wire signed [WIDTH-1:0] in_i, in_q,
    output reg out_valid,
    output reg signed [WIDTH-1:0] out_i, out_q
);
    reg [SHIFT-1:0] count;
    reg signed [ACC_WIDTH-1:0] acc_i, acc_q;
    wire signed [ACC_WIDTH-1:0] sum_i = acc_i + in_i;
    wire signed [ACC_WIDTH-1:0] sum_q = acc_q + in_q;
    always @(posedge clk) begin
        if (reset) begin
            count <= 0;
            acc_i <= 0;
            acc_q <= 0;
            out_i <= 0;
            out_q <= 0;
            out_valid <= 0;
        end else begin
            out_valid <= 0;
            if (in_valid) begin
                if (count == FACTOR - 1) begin
                    out_i <= sum_i >>> SHIFT;
                    out_q <= sum_q >>> SHIFT;
                    out_valid <= 1;
                    count <= 0;
                    acc_i <= 0;
                    acc_q <= 0;
                end else begin
                    count <= count + 1'b1;
                    acc_i <= sum_i;
                    acc_q <= sum_q;
                end
            end
        end
    end
endmodule
)verilog");
    }
    if (definitions.contains(QStringLiteral("trigger"))) lines << triggerDefinition();
    for (const QString &definition : definitions) {
        if (definition.startsWith(QStringLiteral("module obrii_iq_fir_s"))) lines << definition;
    }
    lines << customDefinitions;
    return (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
}

bool FpgaPipelineCompiler::writeVerilog(const QVector<FpgaPipelineStage> &stages,
                                        double inputRateHz, double clockRateHz,
                                        const QString &path, QString *error) {
    const QByteArray content = generateVerilog(stages, inputRateHz, clockRateHz, error);
    if (content.isEmpty()) return false;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Не вдалося створити теку для RTL.");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        if (error) *error = QStringLiteral("Не вдалося записати згенерований RTL.");
        return false;
    }
    return true;
}
