#include "fpgalogicblock.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <functional>
#include <limits>

namespace {
bool jsonInteger(const QJsonValue &value, int *number) {
    if (!value.isDouble()) return false;
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || std::floor(raw) != raw ||
        raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) return false;
    *number = static_cast<int>(raw);
    return true;
}

QString signal(int id) {
    if (id == 0) return QStringLiteral("in_i");
    if (id == 1) return QStringLiteral("in_q");
    return QStringLiteral("n%1").arg(id);
}

QString word16(int value) {
    return QStringLiteral("16'h%1").arg(static_cast<unsigned int>(value) & 0xffffu, 4, 16,
                                       QLatin1Char('0')).toUpper();
}

QHash<int, int> nodeDepths(const FpgaLogicBlock &block) {
    QHash<int, const FpgaLogicNode *> byId;
    for (const auto &node : block.nodes) byId.insert(node.id, &node);
    QHash<int, int> depths;
    depths.insert(0, 0);
    depths.insert(1, 0);
    std::function<int(int)> depth = [&](int id) -> int {
        if (depths.contains(id)) return depths.value(id);
        const auto *node = byId.value(id);
        int inputDepth = 0;
        if (node && !node->inputs.isEmpty()) inputDepth = depth(node->inputs.first());
        const int result = inputDepth + (node && node->op == QStringLiteral("reg16") ? 1 : 0);
        depths.insert(id, result);
        return result;
    };
    for (const auto &node : block.nodes) depth(node.id);
    return depths;
}
}

int FpgaLogicBlock::inputCount(const QString &op) {
    if (op == QStringLiteral("const16")) return 0;
    if (op == QStringLiteral("not16") || op == QStringLiteral("reg16")) return 1;
    if (op == QStringLiteral("add16") || op == QStringLiteral("sub16") ||
        op == QStringLiteral("and16") || op == QStringLiteral("or16") ||
        op == QStringLiteral("xor16") || op == QStringLiteral("mul_q15")) return 2;
    if (op == QStringLiteral("lut4")) return 4;
    return -1;
}

QString FpgaLogicBlock::opLabel(const QString &op) {
    if (op == QStringLiteral("const16")) return QStringLiteral("Константа 16 біт");
    if (op == QStringLiteral("add16")) return QStringLiteral("Сума 16 біт");
    if (op == QStringLiteral("sub16")) return QStringLiteral("Різниця 16 біт");
    if (op == QStringLiteral("and16")) return QStringLiteral("AND 16 біт");
    if (op == QStringLiteral("or16")) return QStringLiteral("OR 16 біт");
    if (op == QStringLiteral("xor16")) return QStringLiteral("XOR 16 біт");
    if (op == QStringLiteral("not16")) return QStringLiteral("NOT 16 біт");
    if (op == QStringLiteral("mul_q15")) return QStringLiteral("Множення Q15");
    if (op == QStringLiteral("reg16")) return QStringLiteral("Регістр 16 біт");
    if (op == QStringLiteral("lut4")) return QStringLiteral("LUT4 по бітах");
    return op;
}

QStringList FpgaLogicBlock::validate() const {
    QStringList errors;
    if (name.trimmed().isEmpty() || name.size() > 64 || name.contains(QLatin1Char('\n')) ||
        name.contains(QLatin1Char('\r')))
        errors << QStringLiteral("Назва блока має містити 1...64 символи без перенесення рядка.");
    if (nodes.size() > 64) errors << QStringLiteral("Підтримується не більш ніж 64 вузли.");
    QHash<int, const FpgaLogicNode *> byId;
    for (const auto &node : nodes) {
        if (node.id < 2 || node.id > 255 || byId.contains(node.id)) {
            errors << QStringLiteral("ID вузлів мають бути унікальними в межах 2...255.");
            continue;
        }
        byId.insert(node.id, &node);
        const int required = inputCount(node.op);
        if (required < 0) errors << QStringLiteral("Вузол %1: невідома операція.").arg(node.id);
        else if (node.inputs.size() != required)
            errors << QStringLiteral("Вузол %1: очікується %2 входів.").arg(node.id).arg(required);
        if (node.op == QStringLiteral("const16") && (node.constant < -32768 || node.constant > 32767))
            errors << QStringLiteral("Вузол %1: константа поза signed IQ16.").arg(node.id);
        if (node.op == QStringLiteral("lut4") && node.truthTable > 0xffffu)
            errors << QStringLiteral("Вузол %1: LUT4 потребує 16-бітної таблиці.").arg(node.id);
        if (!std::isfinite(node.position.x()) || !std::isfinite(node.position.y()) ||
            std::abs(node.position.x()) > 10000 || std::abs(node.position.y()) > 10000)
            errors << QStringLiteral("Вузол %1: координати поза межами схеми.").arg(node.id);
    }
    const auto exists = [&byId](int id) { return id == 0 || id == 1 || byId.contains(id); };
    if (!exists(outputI) || !exists(outputQ))
        errors << QStringLiteral("Виходи I/Q мають посилатися на наявні вузли.");
    for (const auto &node : nodes) {
        for (int input : node.inputs) {
            if (!exists(input))
                errors << QStringLiteral("Вузол %1: відсутнє джерело %2.").arg(node.id).arg(input);
        }
    }
    if (!errors.isEmpty()) return errors;

    QHash<int, int> state;
    QHash<int, int> depths;
    depths.insert(0, 0);
    depths.insert(1, 0);
    std::function<int(int)> visit = [&](int id) -> int {
        if (depths.contains(id)) return depths.value(id);
        if (state.value(id) == 1) {
            errors << QStringLiteral("Вузол %1: зворотний зв'язок поки не підтримується.").arg(id);
            return -1;
        }
        if (state.value(id) == 2) return depths.value(id, -1);
        state.insert(id, 1);
        const auto *node = byId.value(id);
        int inputDepth = 0;
        for (int input : node->inputs) {
            const int depth = visit(input);
            if (depth < 0) inputDepth = -1;
            else if (inputDepth >= 0 && input != node->inputs.first() && inputDepth != depth)
                errors << QStringLiteral("Вузол %1: входи належать різним тактам даних.").arg(id);
            else if (inputDepth >= 0) inputDepth = depth;
        }
        const int result = inputDepth < 0 ? -1 : inputDepth + (node->op == QStringLiteral("reg16") ? 1 : 0);
        depths.insert(id, result);
        state.insert(id, 2);
        return result;
    };
    for (const auto &node : nodes) visit(node.id);
    if (visit(outputI) != visit(outputQ))
        errors << QStringLiteral("Виходи I і Q мають різну затримку в тактах.");
    if (depths.value(outputI, 0) > 32)
        errors << QStringLiteral("Ліміт затримки виходу: 32 такти.");
    errors.removeDuplicates();
    return errors;
}

int FpgaLogicBlock::outputLatencyClocks() const {
    if (!validate().isEmpty()) return -1;
    return nodeDepths(*this).value(outputI);
}

QJsonObject FpgaLogicBlock::toJson() const {
    QJsonArray array;
    for (const auto &node : nodes) {
        QJsonArray inputs;
        for (int input : node.inputs) inputs.append(input);
        QJsonObject object;
        object.insert(QStringLiteral("id"), node.id);
        object.insert(QStringLiteral("op"), node.op);
        object.insert(QStringLiteral("inputs"), inputs);
        object.insert(QStringLiteral("constant"), node.constant);
        object.insert(QStringLiteral("truthTable"), static_cast<int>(node.truthTable));
        object.insert(QStringLiteral("x"), node.position.x());
        object.insert(QStringLiteral("y"), node.position.y());
        array.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("name"), name);
    root.insert(QStringLiteral("format"), QStringLiteral("signed-IQ16"));
    root.insert(QStringLiteral("clockDomain"), QStringLiteral("clk"));
    root.insert(QStringLiteral("outputI"), outputI);
    root.insert(QStringLiteral("outputQ"), outputQ);
    root.insert(QStringLiteral("nodes"), array);
    return root;
}

bool FpgaLogicBlock::fromJson(const QJsonObject &json, FpgaLogicBlock *out, QString *error) {
    FpgaLogicBlock parsed;
    int schema = 0;
    if (!out || !jsonInteger(json.value(QStringLiteral("schema")), &schema) || schema != 1 ||
        json.value(QStringLiteral("format")).toString() != QStringLiteral("signed-IQ16") ||
        json.value(QStringLiteral("clockDomain")).toString() != QStringLiteral("clk") ||
        !json.value(QStringLiteral("nodes")).isArray() ||
        !jsonInteger(json.value(QStringLiteral("outputI")), &parsed.outputI) ||
        !jsonInteger(json.value(QStringLiteral("outputQ")), &parsed.outputQ)) {
        if (error) *error = QStringLiteral("Невідомий формат або неповний контракт блока.");
        return false;
    }
    parsed.name = json.value(QStringLiteral("name")).toString();
    const auto array = json.value(QStringLiteral("nodes")).toArray();
    if (array.size() > 64) {
        if (error) *error = QStringLiteral("Забагато вузлів.");
        return false;
    }
    for (const auto &value : array) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("Пошкоджений вузол.");
            return false;
        }
        const auto object = value.toObject();
        FpgaLogicNode node;
        int truth = 0;
        if (!jsonInteger(object.value(QStringLiteral("id")), &node.id) ||
            !jsonInteger(object.value(QStringLiteral("constant")), &node.constant) ||
            !jsonInteger(object.value(QStringLiteral("truthTable")), &truth) || truth < 0 ||
            !object.value(QStringLiteral("inputs")).isArray() ||
            !object.value(QStringLiteral("x")).isDouble() ||
            !object.value(QStringLiteral("y")).isDouble()) {
            if (error) *error = QStringLiteral("Неповні або некоректні параметри вузла.");
            return false;
        }
        node.op = object.value(QStringLiteral("op")).toString();
        node.truthTable = static_cast<unsigned int>(truth);
        node.position = QPointF(object.value(QStringLiteral("x")).toDouble(),
                                object.value(QStringLiteral("y")).toDouble());
        for (const auto &input : object.value(QStringLiteral("inputs")).toArray()) {
            int id = 0;
            if (!jsonInteger(input, &id)) {
                if (error) *error = QStringLiteral("Некоректне з'єднання вузла.");
                return false;
            }
            node.inputs << id;
        }
        parsed.nodes << node;
    }
    const QStringList issues = parsed.validate();
    if (!issues.isEmpty()) {
        if (error) *error = issues.join(QLatin1Char('\n'));
        return false;
    }
    *out = parsed;
    return true;
}

QByteArray FpgaLogicBlock::generateVerilog(QString *error, const QString &moduleName) const {
    const QStringList issues = validate();
    if (!issues.isEmpty()) {
        if (error) *error = issues.join(QLatin1Char('\n'));
        return {};
    }
    if (!QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z_0-9]*$"))
             .match(moduleName).hasMatch() || moduleName.size() > 80) {
        if (error) *error = QStringLiteral("Некоректна назва RTL-модуля.");
        return {};
    }
    const auto depths = nodeDepths(*this);
    const int latency = depths.value(outputI);
    QStringList lines = {
        QStringLiteral("// Standalone custom logic; signed IQ16, one clock domain, no B210 integration."),
        QStringLiteral("// Arithmetic add/sub wraps at 16 bits; Q15 multiply saturates."),
        QStringLiteral("module %1 (").arg(moduleName),
        QStringLiteral("    input wire clk, reset, in_valid,"),
        QStringLiteral("    input wire signed [15:0] in_i, in_q,"),
        QStringLiteral("    output wire out_valid,"),
        QStringLiteral("    output wire signed [15:0] out_i, out_q"),
        QStringLiteral(");")};
    for (int depth = 1; depth <= latency; ++depth) {
        lines << QStringLiteral("reg valid_%1;").arg(depth)
              << QStringLiteral("always @(posedge clk) if (reset) valid_%1 <= 1'b0; else valid_%1 <= %2;")
                     .arg(depth).arg(depth == 1 ? QStringLiteral("in_valid")
                                               : QStringLiteral("valid_%1").arg(depth - 1));
    }
    bool needsMultiply = false;
    for (const auto &node : nodes) {
        lines << QStringLiteral("%1 signed [15:0] n%2;")
                     .arg(node.op == QStringLiteral("reg16") ? QStringLiteral("reg") : QStringLiteral("wire"))
                     .arg(node.id);
        if (node.op == QStringLiteral("mul_q15")) needsMultiply = true;
    }
    if (needsMultiply) lines << QString::fromLatin1(R"verilog(
function signed [15:0] saturate16;
input signed [31:0] value;
begin
    if (value > 32'sd32767) saturate16 = 16'sd32767;
    else if (value < -32'sd32768) saturate16 = -16'sd32768;
    else saturate16 = value[15:0];
end
endfunction
)verilog");
    for (const auto &node : nodes) {
        const QString target = signal(node.id);
        const QString first = node.inputs.isEmpty() ? QString() : signal(node.inputs[0]);
        const QString second = node.inputs.size() < 2 ? QString() : signal(node.inputs[1]);
        if (node.op == QStringLiteral("const16")) {
            lines << QStringLiteral("assign %1 = %2;").arg(target, word16(node.constant));
        } else if (node.op == QStringLiteral("reg16")) {
            const int sourceDepth = depths.value(node.inputs[0]);
            const QString valid = sourceDepth == 0 ? QStringLiteral("in_valid")
                : QStringLiteral("valid_%1").arg(sourceDepth);
            lines << QStringLiteral("always @(posedge clk) if (reset) %1 <= 16'd0; else if (%2) %1 <= %3;")
                         .arg(target, valid, first);
        } else if (node.op == QStringLiteral("not16")) {
            lines << QStringLiteral("assign %1 = ~%2;").arg(target, first);
        } else if (node.op == QStringLiteral("lut4")) {
            const QString mask = QStringLiteral("16'h%1")
                .arg(node.truthTable, 4, 16, QLatin1Char('0')).toUpper();
            lines << QStringLiteral("localparam [15:0] LUT_%1 = %2;").arg(node.id).arg(mask)
                  << QStringLiteral("genvar bit_%1;").arg(node.id)
                  << QStringLiteral("generate for (bit_%1 = 0; bit_%1 < 16; bit_%1 = bit_%1 + 1) begin: bits_%1")
                         .arg(node.id)
                  << QStringLiteral("assign %1[bit_%2] = LUT_%2[{%3[bit_%2], %4[bit_%2], %5[bit_%2], %6[bit_%2]}];")
                         .arg(target).arg(node.id).arg(signal(node.inputs[3]), signal(node.inputs[2]),
                                                         signal(node.inputs[1]), signal(node.inputs[0]))
                  << QStringLiteral("end endgenerate");
        } else if (node.op == QStringLiteral("mul_q15")) {
            lines << QStringLiteral("wire signed [31:0] product_%1 = %2 * %3;")
                         .arg(node.id).arg(first, second)
                  << QStringLiteral("assign %1 = saturate16(product_%2 >>> 15);")
                         .arg(target).arg(node.id);
        } else {
            const QString operation = node.op == QStringLiteral("add16") ? QStringLiteral("+")
                : node.op == QStringLiteral("sub16") ? QStringLiteral("-")
                : node.op == QStringLiteral("and16") ? QStringLiteral("&")
                : node.op == QStringLiteral("or16") ? QStringLiteral("|") : QStringLiteral("^");
            lines << QStringLiteral("assign %1 = %2 %3 %4;").arg(target, first, operation, second);
        }
    }
    lines << QStringLiteral("assign out_valid = %1;").arg(latency == 0 ? QStringLiteral("in_valid")
        : QStringLiteral("valid_%1").arg(latency))
          << QStringLiteral("assign out_i = %1;").arg(signal(outputI))
          << QStringLiteral("assign out_q = %1;").arg(signal(outputQ))
          << QStringLiteral("endmodule");
    return (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
}
