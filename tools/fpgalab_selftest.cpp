#include "fpgalabwindow.h"
#include "fpgalogiceditor.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QTabWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

static bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    const QString source = temp.filePath(QStringLiteral("top.v"));
    const QString project = temp.filePath(QStringLiteral("clone.xpr"));
    const QString image = temp.filePath(QStringLiteral("fpga.bin"));
    if (!writeFile(source, "module top;\r\nendmodule\r\n") ||
        !writeFile(project, "<Project/>\n") || !writeFile(image, "\x01\x02\x03")) return 2;

    FpgaLabWindow window;
    if (!window.openPath(project)) return 3;
    auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("fpgaFileTree"));
    auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("fpgaSourceEditor"));
    auto *add = window.findChild<QPushButton *>(QStringLiteral("fpgaAddStage"));
    if (!tree || !editor || !add) return 4;
    const auto files = tree->findItems(QStringLiteral("top.v"), Qt::MatchExactly | Qt::MatchRecursive);
    if (files.size() != 1) return 5;
    tree->setCurrentItem(files.first());
    if (editor->isReadOnly() || !editor->toPlainText().contains(QStringLiteral("module top"))) return 6;
    editor->setPlainText(QStringLiteral("module top;\nwire ready;\nendmodule\n"));
    bool saved = false;
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("Зберегти код")) {
            action->trigger();
            saved = true;
            break;
        }
    }
    QFile updated(source);
    if (!saved || !updated.open(QIODevice::ReadOnly) ||
        updated.readAll() != "module top;\r\nwire ready;\r\nendmodule\r\n") return 7;
    add->click();
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("Зберегти схему")) action->trigger();
    }
    QFile pipeline(temp.filePath(QStringLiteral(".obrii-fpga.json")));
    if (!pipeline.open(QIODevice::ReadOnly) || !pipeline.readAll().contains("ddc")) return 8;
    pipeline.close();
    auto *logicButton = window.findChild<QPushButton *>(QStringLiteral("fpgaLogicEditor"));
    if (!logicButton || !logicButton->isEnabled()) return 14;
    bool logicWorked = false;
    QTimer::singleShot(0, [&]() {
        auto *dialog = dynamic_cast<FpgaLogicEditor *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *addNode = dialog->findChild<QPushButton *>(QStringLiteral("logicAddNode"));
        auto *saveLogic = dialog->findChild<QPushButton *>(QStringLiteral("logicSave"));
        if (!addNode || !saveLogic) { static_cast<QDialog *>(dialog)->reject(); return; }
        addNode->click();
        if (argc > 1) {
            QPixmap preview(dialog->size());
            preview.fill(Qt::black);
            dialog->render(&preview);
            preview.save(QFileInfo(QString::fromLocal8Bit(argv[1])).absolutePath() +
                         QStringLiteral("/fpga-logic-editor.png"));
        }
        saveLogic->click();
        QFile logic(temp.filePath(QStringLiteral(".obrii-logic.json")));
        logicWorked = logic.open(QIODevice::ReadOnly) && logic.readAll().contains("const16");
        static_cast<QDialog *>(dialog)->reject();
    });
    logicButton->click();
    if (!logicWorked) return 15;
    auto *stageType = window.findChild<QComboBox *>(QStringLiteral("fpgaPipelineStageType"));
    auto *summary = window.findChild<QLabel *>(QStringLiteral("fpgaPipelineSummary"));
    if (!stageType || !summary) return 16;
    stageType->setCurrentIndex(stageType->findData(QStringLiteral("custom")));
    const QString logicFilePath = temp.filePath(QStringLiteral(".obrii-logic.json"));
    if (!window.addCustomStageFromFile(logicFilePath) ||
        !summary->text().contains(QStringLiteral("RTL доступний"))) return 17;
    for (QAction *action : window.findChildren<QAction *>())
        if (action->text() == QStringLiteral("Зберегти схему")) action->trigger();
    QFile savedGraph(temp.filePath(QStringLiteral(".obrii-fpga.json")));
    if (!savedGraph.open(QIODevice::ReadOnly) ||
        !QJsonDocument::fromJson(savedGraph.readAll()).object()
            .value(QStringLiteral("stages")).toArray().last().toObject()
            .value(QStringLiteral("customLogic")).isObject()) return 18;
    savedGraph.close();
    if (!QFile::remove(logicFilePath) || !window.openPath(project) ||
        !summary->text().contains(QStringLiteral("RTL доступний"))) return 19;
    if (argc > 1 && argc <= 2) {
        auto *tabs = window.findChild<QTabWidget *>();
        if (!tabs) return 11;
        tabs->setCurrentIndex(1);
        QPixmap preview(window.size());
        preview.fill(Qt::black);
        window.render(&preview);
        if (!preview.save(QString::fromLocal8Bit(argv[1]))) return 10;
    }
    if (!window.openPath(image) || !editor->isReadOnly() || logicButton->isEnabled()) return 9;
    const QString nested = temp.filePath(QStringLiteral("b210/src/B210_Project_Firmwire.srcs"));
    const QString marker = temp.filePath(QStringLiteral("b210/.obrii-b210.json"));
    if (!QDir().mkpath(nested) ||
        !writeFile(nested + QStringLiteral("/radio_legacy.v"), "module radio_legacy; endmodule\n") ||
        !writeFile(marker, "{\"schema\":1,\"variant\":\"b210-dual-fabric\"}") ||
        !window.openPath(marker) || !window.findChild<QWidget *>(QStringLiteral("b210LabPanel")) ||
        !QFile::exists(QStringLiteral(":/fpga/b210_fpga_check.py"))) return 20;
    const auto radio = tree->findItems(QStringLiteral("radio_legacy.v"), Qt::MatchExactly | Qt::MatchRecursive);
    if (radio.size() != 1) return 21;
    tree->setCurrentItem(radio.first());
    if (editor->isReadOnly() || !editor->toPlainText().contains(QStringLiteral("module radio_legacy"))) return 22;
    const int identity = stageType->findData(QStringLiteral("passthrough"));
    if (identity < 0) return 23;
    if (argc > 2) {
        if (!window.openPath(QString::fromLocal8Bit(argv[2]))) return 12;
        auto *report = window.findChild<QPlainTextEdit *>(QStringLiteral("openXc7Report"));
        if (!report || !report->toPlainText().contains(QStringLiteral("xc7k325tffg676-2")) ||
            !report->toPlainText().contains(QStringLiteral("EDIF: 2")) ||
            !report->toPlainText().contains(QStringLiteral("MIF: 2"))) return 13;
    }
    if (argc > 2) {
        auto *tabs = window.findChild<QTabWidget *>();
        if (!tabs) return 11;
        tabs->setCurrentIndex(2);
        QPixmap preview(window.size());
        preview.fill(Qt::black);
        window.render(&preview);
        if (!preview.save(QString::fromLocal8Bit(argv[1]))) return 10;
    }
    return 0;
}
