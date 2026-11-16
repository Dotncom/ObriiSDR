#include "usrplabdialog.h"
#include "usrplabjob.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>

void showUsrpLabDialog(QWidget *owner, bool ukrainian) {
    const auto text = [ukrainian](const char *en, const char *uk) {
        return QString::fromUtf8(ukrainian ? uk : en);
    };
    auto *dialog = owner->findChild<QDialog *>(QStringLiteral("obriiUsrpLab"), Qt::FindDirectChildrenOnly);
    if (dialog) {
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        return;
    }

    auto *job = UsrpLabJob::forOwner(owner);
    dialog = new QDialog(owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("obriiUsrpLab"));
    dialog->setWindowTitle(text("USRP software lab", "Програмний стенд USRP"));
    dialog->resize(720, 440);
    auto *layout = new QVBoxLayout(dialog);
    auto *intro = new QLabel(text("Run a local software test or analyze a GSM or UMTS IQ recording.",
        "Запустіть локальну перевірку або проаналізуйте запис IQ GSM чи UMTS."), dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *mode = new QComboBox(dialog);
    mode->addItem(text("GSM BCCH / PDCH from IQ", "GSM BCCH / PDCH із IQ"), QStringLiteral("gsm-decode"));
    mode->addItem(text("UMTS cell and BCH from IQ", "Сота UMTS і BCH із IQ"), QStringLiteral("umts-decode"));
    mode->addItem(text("UMTS synthetic IQ demo", "UMTS: демо із синтетичного IQ"), QStringLiteral("umts-demo"));
    mode->addItem(text("GSM synthetic IQ demo", "GSM: демо із синтетичного IQ"), QStringLiteral("gsm-demo"));
    mode->addItem(text("GPRS replay from MAC JSONL", "GPRS: повторний розбір MAC JSONL"), QStringLiteral("gsm-replay"));
    mode->addItem(QStringLiteral("Wi-Fi PHY loopback"), QStringLiteral("wifi-loopback"));
    mode->addItem(QStringLiteral("LTE ZMQ loopback"), QStringLiteral("lte-zmq"));
    mode->addItem(text("ZeroMQ IQ transport (requires PyZMQ)", "Транспорт IQ через ZeroMQ (потрібен PyZMQ)"),
        QStringLiteral("zmq-loopback"));
    mode->addItem(text("Shared worker and IQ contracts", "Спільний каркас і формати IQ"), QStringLiteral("contracts"));
    form->addRow(text("Job", "Завдання"), mode);
    auto *input = new QLineEdit(dialog);
    input->setObjectName(QStringLiteral("usrpLabInput"));
    auto *context = new QLineEdit(dialog);
    auto *inputBrowse = new QPushButton(text("Browse", "Огляд"), dialog);
    auto *contextBrowse = new QPushButton(text("Browse", "Огляд"), dialog);
    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(input);
    inputRow->addWidget(inputBrowse);
    form->addRow(text("Input file", "Вхідний файл"), inputRow);
    auto *contextRow = new QHBoxLayout;
    contextRow->addWidget(context);
    contextRow->addWidget(contextBrowse);
    form->addRow(text("GSM IQ result folder", "Папка результату GSM IQ"), contextRow);
    auto *arfcn = new QSpinBox(dialog);
    arfcn->setRange(0, 1023);
    arfcn->setValue(62);
    form->addRow(QStringLiteral("ARFCN"), arfcn);
    auto *slot = new QSpinBox(dialog);
    slot->setRange(0, 7);
    slot->setSpecialValueText(text("BCCH only", "Лише BCCH"));
    form->addRow(QStringLiteral("PDCH TS"), slot);
    auto *rate = new QSpinBox(dialog);
    rate->setRange(3840000, 32000000);
    rate->setValue(7680000);
    form->addRow(text("UMTS sample rate (Hz)", "Частота дискретизації UMTS (Гц)"), rate);
    auto *offset = new QSpinBox(dialog);
    offset->setRange(-13500000, 13500000);
    form->addRow(text("UMTS carrier offset (Hz)", "Зсув несучої UMTS (Гц)"), offset);
    auto *format = new QComboBox(dialog);
    format->addItems({QStringLiteral("cf32"), QStringLiteral("cs16"), QStringLiteral("ci8")});
    form->addRow(text("UMTS IQ format", "Формат IQ UMTS"), format);
    auto *startSample = new QLineEdit(QStringLiteral("0"), dialog);
    form->addRow(text("First UMTS sample", "Перший відлік UMTS"), startSample);
    auto *scenario = new QComboBox(dialog);
    scenario->addItem(QStringLiteral("PRACH / AICH"), QStringLiteral("prach"));
    scenario->addItem(text("RRC connection", "З’єднання RRC"), QStringLiteral("rrc"));
    scenario->addItem(text("NAS attach", "Реєстрація NAS"), QStringLiteral("attach"));
    scenario->addItem(text("IPv4 exchange over DTCH", "Обмін IPv4 через DTCH"), QStringLiteral("ipv4"));
    scenario->addItem(text("RLC AM loss recovery", "Відновлення після втрат RLC AM"), QStringLiteral("rlc-recovery"));
    scenario->setCurrentIndex(scenario->findData(QStringLiteral("ipv4")));
    form->addRow(text("UMTS scenario", "Сценарій UMTS"), scenario);

    auto *buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    auto *start = new QPushButton(text("Run", "Запустити"), dialog);
    auto *cancel = new QPushButton(text("Cancel", "Скасувати"), dialog);
    auto *open = new QPushButton(text("Open result folder", "Відкрити папку результату"), dialog);
    buttons->addWidget(start);
    buttons->addWidget(cancel);
    buttons->addWidget(open);
    buttons->addStretch();
    auto *output = new QPlainTextEdit(dialog);
    output->setReadOnly(true);
    layout->addWidget(output, 1);

    const auto refresh = [=] {
        const auto state = job->status();
        const bool active = state.value("active").toBool();
        const bool decode = mode->currentData().toString() == QStringLiteral("gsm-decode");
        const bool replay = mode->currentData().toString() == QStringLiteral("gsm-replay");
        const bool umts = mode->currentData().toString() == QStringLiteral("umts-decode");
        const bool umtsDemo = mode->currentData().toString() == QStringLiteral("umts-demo");
        const bool zmq = mode->currentData().toString() == QStringLiteral("zmq-loopback");
        const bool available = state.value(zmq ? "zmqPythonAvailable" : "available").toBool();
        mode->setEnabled(!active);
        input->setEnabled(!active && (decode || replay || umts));
        inputBrowse->setEnabled(input->isEnabled());
        context->setEnabled(!active && replay);
        contextBrowse->setEnabled(context->isEnabled());
        arfcn->setEnabled(!active && (decode || replay));
        slot->setEnabled(!active && decode);
        for (auto *field : {static_cast<QWidget *>(rate), static_cast<QWidget *>(offset),
                            static_cast<QWidget *>(format), static_cast<QWidget *>(startSample)}) {
            field->setVisible(umts);
            if (auto *label = form->labelForField(field)) label->setVisible(umts);
            field->setEnabled(!active);
        }
        scenario->setVisible(umtsDemo);
        form->labelForField(scenario)->setVisible(umtsDemo);
        scenario->setEnabled(!active);
        start->setEnabled(!active && available);
        cancel->setEnabled(active && !state.value("cancelling").toBool());
        const auto result = state.value("lastResult").toObject();
        open->setEnabled(result.contains("directory"));
        QString message;
        if (active) {
            message = state.value("cancelling").toBool()
                ? text("Cancelling and waiting for cleanup…", "Скасування. Очікуємо завершення процесів…")
                : text("Running…", "Виконується…");
        } else if (!result.isEmpty()) {
            message = result.value("status").toString() + QLatin1Char('\n');
            const auto summary = result.value("summary").toObject();
            if (summary.contains("scramblingCodes")) {
                QStringList codes;
                for (const auto &code : summary.value("scramblingCodes").toArray()) codes.append(QString::number(code.toInt()));
                message += text("UMTS: %1    PSC: %2    BCH blocks: %3\n", "UMTS: %1    PSC: %2    Блоки BCH: %3\n")
                    .arg(summary.value("status").toString()).arg(codes.join(QStringLiteral(", ")))
                    .arg(summary.value("bchBlocks").toInt());
            } else if (summary.contains("stages")) {
                message += text("UMTS scenario: %1    Verified files: %2\n",
                                "Сценарій UMTS: %1    Звірені файли: %2\n")
                    .arg(summary.value("scenario").toString()).arg(summary.value("artifacts").toInt());
                const auto stages = summary.value("stages").toObject();
                for (const auto *key : {"aich", "rrc-complete", "nas-attach-finish", "nas-pdp-activate", "ipv4-dtch"})
                    if (stages.contains(QLatin1String(key)))
                        message += stages.value(QLatin1String(key)).toString() + QLatin1Char('\n');
            } else if (summary.contains("cells")) {
                message += text("Cells: %1    PDCH blocks: %2\n", "Соти: %1    Блоки PDCH: %2\n")
                    .arg(summary.value("cells").toInt()).arg(summary.value("pdchBlocks").toInt());
                message += text("Decode status: %1\n", "Стан декодування: %1\n")
                    .arg(summary.value("status").toString());
            } else if (summary.contains("samplesPerDirection")) {
                message += text("Checks %1. Samples per direction %2.\n", "Перевірок %1. Відліків у кожному напрямку %2.\n")
                    .arg(summary.value("checks").toInt()).arg(summary.value("samplesPerDirection").toInt());
            } else if (summary.contains("checks")) {
                message += text("Checks: %1    Verified files: %2\n", "Перевірки: %1    Звірені файли: %2\n")
                    .arg(summary.value("checks").toInt()).arg(summary.value("artifacts").toInt());
            }
            for (const QString &detail : {result.value("error").toString(), summary.value("reason").toString(),
                                          result.value("diagnostic").toString()})
                if (!detail.isEmpty()) message += detail + QLatin1Char('\n');
        } else if (!available && zmq) {
            message = text("Select Python with PyZMQ using OBRII_USRP_LAB_PYTHON.",
                           "Укажіть Python із PyZMQ в OBRII_USRP_LAB_PYTHON.");
        } else if (!available) {
            message = text("Local lab unavailable. Set OBRII_USRP_LAB_ROOT and install Python 3.11+.",
                           "Локальний стенд недоступний. Задайте OBRII_USRP_LAB_ROOT і встановіть Python 3.11+.");
        } else if (umtsDemo) {
            message = text("Generates synthetic IQ and verifies the selected exchange offline. Requires NumPy.",
                           "Генерує синтетичний IQ і перевіряє вибраний обмін офлайн. Потрібен NumPy.");
        } else if (zmq) {
            message = text("Requires PyZMQ in the selected Python environment.",
                           "У вибраному середовищі Python потрібен PyZMQ.");
        } else {
            message = text("Ready.", "Готово до запуску.");
        }
        output->setPlainText(message);
    };
    QObject::connect(job, &UsrpLabJob::changed, dialog, refresh);
    QObject::connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), dialog, refresh);
    QObject::connect(inputBrowse, &QPushButton::clicked, dialog, [=] {
        const QString file = QFileDialog::getOpenFileName(dialog, text("Open input", "Відкрити вхідний файл"));
        if (!file.isEmpty()) input->setText(QDir::toNativeSeparators(file));
    });
    QObject::connect(contextBrowse, &QPushButton::clicked, dialog, [=] {
        const QString directory = QFileDialog::getExistingDirectory(dialog, text("Open GSM IQ result", "Відкрити результат GSM IQ"));
        if (!directory.isEmpty()) context->setText(QDir::toNativeSeparators(directory));
    });
    QObject::connect(start, &QPushButton::clicked, dialog, [=] {
        const QString action = mode->currentData().toString();
        QJsonObject request{{"action", action}};
        if (action == QStringLiteral("gsm-decode") || action == QStringLiteral("gsm-replay")) {
            request.insert("input", input->text().trimmed());
            request.insert("arfcn", arfcn->value());
            if (action == QStringLiteral("gsm-replay")) request.insert("context", context->text().trimmed());
            else if (slot->value()) request.insert("timeslot", slot->value());
        } else if (action == QStringLiteral("umts-decode")) {
            request = UsrpLabJob::arguments({action, input->text().trimmed(), QString::number(rate->value()),
                QString::number(offset->value()), format->currentText(), startSample->text().trimmed()});
            if (request.isEmpty()) {
                output->setPlainText(text("Enter a valid first sample index.", "Вкажіть коректний номер першого відліку."));
                return;
            }
        } else if (action == QStringLiteral("umts-demo")) {
            request.insert("scenario", scenario->currentData().toString());
        }
        const auto reply = job->command(request);
        if (!reply.value("ok").toBool()) output->setPlainText(reply.value("error").toString());
    });
    QObject::connect(cancel, &QPushButton::clicked, dialog, [=] {
        const auto reply = job->command({{"action", "cancel"}});
        if (!reply.value("ok").toBool()) output->setPlainText(reply.value("error").toString());
    });
    QObject::connect(open, &QPushButton::clicked, dialog, [=] {
        const QString directory = job->status().value("lastResult").toObject().value("directory").toString();
        if (!directory.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    refresh();
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
