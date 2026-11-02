#include "apphelp.h"

#include <QApplication>
#include <QDialog>
#include <QListWidget>
#include <QString>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void checkLanguage(const QString &language) {
    const QString fullText = applicationHelpText(language);
    const auto sections = applicationHelpSections(language);
    require(sections.size() == 25, "A help topic is missing");
    int cursor = 0;
    for (const auto &section : sections) {
        require(!section.title.isEmpty() && !section.text.isEmpty(), "Empty help topic");
        const int position = fullText.indexOf(section.text, cursor);
        require(position >= cursor, "Help topic order or content changed");
        cursor = position + section.text.size();
    }
    require(sections[1].title == QStringLiteral("Obrii FPGA Lab") &&
            sections[1].text.contains(QStringLiteral("openXC7")) &&
            sections[2].title == QStringLiteral("Obrii CLI") &&
            sections[2].text.contains(QStringLiteral("ObriiCli")) &&
            sections[3].text.contains(QStringLiteral("control mode")) &&
            sections[4].text.contains(QStringLiteral("control spectrum")) &&
            sections[5].text.contains(QStringLiteral("control multisdr")) &&
            sections[6].text.contains(QStringLiteral("module tx sstv")) &&
            sections[7].title == QStringLiteral("Obrii Analyzer"),
            "Standalone application help topics are incomplete");
    require(fullText.contains(QStringLiteral("BFP8")) &&
            fullText.contains(QStringLiteral("KISS")),
            "Recent DSP and open-radio features are missing from help");
    require(sections[5].text.contains(QStringLiteral("FT4")) &&
            sections[5].text.contains(QStringLiteral("M17")) &&
            sections[5].text.contains(QStringLiteral("--module-voice-stdout")) &&
            sections[5].text.contains(QStringLiteral("docs/ft4_m17_receive.md")),
            "Live CLI FT4/M17 setup and output help is incomplete");
    require(sections[4].text.contains(QStringLiteral("preset list")) &&
            sections[4].text.contains(QStringLiteral("experiment inspect")) &&
            sections[4].text.contains(QStringLiteral("10000")) &&
            fullText.contains(QStringLiteral("ObriiIqHub --listen")) &&
            fullText.contains(QStringLiteral("--collision-ms")) &&
            fullText.contains(QStringLiteral("--scenario docs/virtual_iq_link_scenario.json")) &&
            fullText.contains(QStringLiteral("--virtual-node-id")) &&
            fullText.contains(QStringLiteral("hubSessionId")),
            "CLI experiment or virtual IQ hub help is incomplete");
    require(sections[3].text.contains(QStringLiteral("sharedCores")) &&
            sections[3].text.contains(QStringLiteral("docs/shared_runtime_core.md")) &&
            sections[3].text.contains(QStringLiteral("docs/command_console.md")) &&
            sections[3].text.contains(QStringLiteral("docs/command_dispatcher.md")) &&
            sections[3].text.contains(QStringLiteral("docs/command_control_api.md")) &&
            sections[3].text.contains(QStringLiteral("control agent start")) &&
            sections[3].text.contains(QStringLiteral("control commands")) &&
            sections[3].text.contains(QStringLiteral("trace live 0 128")) &&
            sections[3].text.contains(QStringLiteral("ObriiCli doctor")) &&
            sections[3].text.contains(QStringLiteral("--doctor-write-test")) &&
            sections[3].text.contains(QStringLiteral("--doctor-port")) &&
            sections[3].text.contains(QStringLiteral("fftWorker")) &&
            sections[4].text.contains(QStringLiteral("control gnss start")) &&
            sections[4].text.contains(QStringLiteral("control research request")) &&
            sections[4].text.contains(QStringLiteral("capture FILE.json")) &&
            sections[6].text.contains(QStringLiteral("control record start")) &&
            sections[6].text.contains(QStringLiteral("record stop")) &&
            sections[6].text.contains(QStringLiteral("tx control ptt on")),
            "Recent CLI diagnostic, asynchronous FFT, GNSS, research, runtime-recording or PTT help is incomplete");
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        checkLanguage(QStringLiteral("uk"));
        checkLanguage(QStringLiteral("uk-UA"));
        checkLanguage(QStringLiteral("en"));
        if (qEnvironmentVariableIsSet("OBRII_HELP_RENDER_TEST")) {
#ifdef _WIN32
            require(qEnvironmentVariable("QT_QPA_PLATFORM") != QStringLiteral("offscreen"),
                    "Help render test requires the Windows Qt platform");
#endif
            bool rendered = false;
            QTimer capture;
            capture.setInterval(50);
            QObject::connect(&capture, &QTimer::timeout, [&]() {
                for (QWidget *window : QApplication::topLevelWidgets()) {
                    auto *dialog = qobject_cast<QDialog *>(window);
                    if (!dialog || !dialog->isVisible()) continue;
                    auto *topics = dialog->findChild<QListWidget *>();
                    if (!topics) continue;
                    rendered = topics->count() == 25 && topics->currentRow() == 1 &&
                               dialog->grab().save(QStringLiteral("build/help-dialog-preview.png"));
                    capture.stop();
                    dialog->reject();
                    break;
                }
            });
            capture.start();
            QTimer::singleShot(3000, &app, [&]() {
                if (rendered) return;
                for (QWidget *window : QApplication::topLevelWidgets()) {
                    if (auto *dialog = qobject_cast<QDialog *>(window)) dialog->reject();
                }
            });
            showApplicationHelpDialog(nullptr, QStringLiteral("uk"), QStringLiteral("Obrii FPGA Lab"));
            require(rendered, "Help dialog did not render or select FPGA Lab");
        }
        std::cout << "Application help self-test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Application help self-test failed: " << error.what() << '\n';
        return 1;
    }
}
