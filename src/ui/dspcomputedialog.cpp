#include "dspcomputedialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

SpectrumComputeControls::SpectrumComputeControls(bool uk, QWidget *parent)
    : QWidget(parent) {
    const auto text = [uk](const char *ua, const char *en) { return QString::fromUtf8(uk ? ua : en); };
    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("mainFftComputeMode"));
    mode_->addItem(text("Локально", "Local"), int(SpectrumComputeMode::Local));
    mode_->addItem(text("Окремі вікна IQ", "Individual IQ windows"), int(SpectrumComputeMode::Windows));
    mode_->addItem(text("Повний безперервний IQ", "Full continuous IQ"), int(SpectrumComputeMode::Continuous));
    backend_ = new QComboBox(this);
    backend_->setObjectName(QStringLiteral("mainFftBackend"));
    backend_->addItem(QStringLiteral("CPU (FFTW)"), 1);
    backend_->addItem(QStringLiteral("GPU"), 2);
    backend_->addItem(text("Авто", "Auto"), 0);
    uploadLimit_ = new QSpinBox(this);
    uploadLimit_->setObjectName(QStringLiteral("mainFftUploadLimitMBps"));
    uploadLimit_->setRange(1, 2000);
    uploadLimit_->setSuffix(QStringLiteral(" MB/s"));
    fallback_ = new QCheckBox(text("Дозволити локальний FFT при відмові зовнішнього",
                                  "Allow local FFT if remote processing fails"), this);
    fallback_->setObjectName(QStringLiteral("mainFftFallbackToLocal"));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("mainFftComputeStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setMinimumHeight(status_->fontMetrics().lineSpacing() * 4);
    form->addRow(text("Обчислення головного FFT:", "Main FFT compute:"), mode_);
    form->addRow(text("Backend на обчислювачі:", "Remote compute backend:"), backend_);
    form->addRow(text("Ліміт передавання:", "Upload budget:"), uploadLimit_);
    form->addRow(fallback_);
    form->addRow(new QLabel(text("Стан головного FFT:", "Main FFT status:"), this));
    form->addRow(status_);
    auto *apply = new QPushButton(text("Застосувати", "Apply"), this);
    apply->setObjectName(QStringLiteral("mainFftApply"));
    form->addRow(apply);
    connect(apply, &QPushButton::clicked, this, &SpectrumComputeControls::applyRequested);
    const auto updateMode = [this]() {
        const bool remote = mode_->currentData().toInt() != int(SpectrumComputeMode::Local);
        backend_->setEnabled(remote);
        uploadLimit_->setEnabled(remote);
        fallback_->setEnabled(remote);
    };
    connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [updateMode](int) { updateMode(); });
    setOptions(SpectrumComputeOptions{});
    setStatus(QString());
    updateMode();
}

SpectrumComputeOptions SpectrumComputeControls::options() const {
    SpectrumComputeOptions options;
    options.mode = SpectrumComputeMode(mode_->currentData().toInt());
    options.backend = backend_->currentData().toInt();
    options.uploadLimitMBps = uploadLimit_->value();
    options.fallbackToLocal = fallback_->isChecked();
    return options;
}

void SpectrumComputeControls::setOptions(const SpectrumComputeOptions &options) {
    const int modeIndex = mode_->findData(int(options.mode));
    mode_->setCurrentIndex(modeIndex < 0 ? 0 : modeIndex);
    const int backendIndex = backend_->findData(options.backend);
    backend_->setCurrentIndex(backendIndex < 0 ? 0 : backendIndex);
    uploadLimit_->setValue(options.uploadLimitMBps);
    fallback_->setChecked(options.fallbackToLocal);
}

void SpectrumComputeControls::setStatus(const QString &status) {
    status_->setText(status.isEmpty() ? QStringLiteral("--") : status);
}

DspComputeDialog::DspComputeDialog(const DspStreamEndpoint &endpoint, bool uk, QWidget *parent)
    : QDialog(parent, Qt::Window), ukrainian_(uk), endpoint_(endpoint) {
    setAttribute(Qt::WA_DeleteOnClose); setModal(false); resize(600, 450);
    const auto text = [uk](const char *ua, const char *en) { return QString::fromUtf8(uk ? ua : en); };
    setWindowTitle(text("Менеджер потоків DSP", "DSP stream manager"));
    auto *root = new QVBoxLayout(this);
    auto *notice = new QLabel(text("IQ передається без шифрування. Лише довірена LAN або захищений тунель. Ключі сеансу не зберігаються.",
        "IQ is sent unencrypted. Trusted LAN or secured tunnel only. Session tokens are not saved."), this);
    notice->setWordWrap(true); root->addWidget(notice);
    auto *tabs = new QTabWidget(this); root->addWidget(tabs);
    tabs->setObjectName(QStringLiteral("dspComputeTabs"));
    auto *client = new QWidget(tabs); auto *clientForm = new QFormLayout(client);
    tabs->addTab(client, text("Зовнішній обробник", "External processor"));
    auto *enabled = new QCheckBox(text("Дозволити передавання IQ", "Allow IQ transfer"), client);
    enabled->setChecked(endpoint.enabled); clientForm->addRow(enabled);
    auto *address = new QLineEdit(endpoint.address, client); address->setObjectName("dspRemoteAddress");
    auto *port = new QSpinBox(client); port->setRange(1, 65535); port->setValue(endpoint.port);
    auto *token = new QLineEdit(endpoint.token, client); token->setEchoMode(QLineEdit::Password); token->setMaxLength(128);
    port->setObjectName("dspRemotePort"); token->setObjectName("dspRemoteToken");
    endpointEnabled_ = enabled; endpointAddress_ = address; endpointPort_ = port; endpointToken_ = token;
    clientForm->addRow(text("IP-адреса:", "IP address:"), address); clientForm->addRow(text("Порт:", "Port:"), port);
    clientForm->addRow(text("Ключ сеансу:", "Session token:"), token);
    auto *apply = new QPushButton(text("Застосувати", "Apply"), client); clientForm->addRow(apply);
    connect(apply, &QPushButton::clicked, this, [this, enabled, address, port, token]() {
        endpoint_.enabled = enabled->isChecked(); endpoint_.address = address->text().trimmed();
        endpoint_.port = quint16(port->value()); endpoint_.token = token->text().trimmed(); emit endpointChanged();
    });
    auto *mainFft = new QWidget(tabs);
    auto *mainFftLayout = new QVBoxLayout(mainFft);
    spectrumControls_ = new SpectrumComputeControls(uk, mainFft);
    mainFftLayout->addWidget(spectrumControls_);
    mainFftLayout->addStretch();
    tabs->addTab(mainFft, text("Головний FFT", "Main FFT"));
    connect(spectrumControls_, &SpectrumComputeControls::applyRequested, this, [this]() {
        const auto options = spectrumControls_->options();
        if (spectrumOptions_ == options) return;
        spectrumOptions_ = options;
        emit spectrumComputeOptionsChanged();
    });
    addProbeTab(tabs, uk);
    auto *worker = new QWidget(tabs); auto *form = new QFormLayout(worker);
    tabs->addTab(worker, text("Обчислювач на цьому ПК", "Compute on this PC"));
    auto *bind = new QLineEdit(QStringLiteral("127.0.0.1"), worker);
    auto *serverPort = new QSpinBox(worker); serverPort->setRange(1, 65535); serverPort->setValue(7358);
    auto *memory = new QSpinBox(worker);
    memory->setObjectName(QStringLiteral("mainFftMemoryMiB"));
    memory->setRange(64, 8192); memory->setValue(512); memory->setSuffix(QStringLiteral(" MiB"));
    auto *session = new QLineEdit(QUuid::createUuid().toString(QUuid::Id128)+QUuid::createUuid().toString(QUuid::Id128), worker);
    session->setReadOnly(true); session->setEchoMode(QLineEdit::Password);
    auto *copy = new QPushButton(text("Копіювати ключ", "Copy token"), worker);
    connect(copy, &QPushButton::clicked, this, [session]() { QApplication::clipboard()->setText(session->text()); });
    form->addRow(text("Локальна IP-адреса:", "Local IP address:"), bind);
    form->addRow(text("Порт:", "Port:"), serverPort); form->addRow(text("Ключ сеансу:", "Session token:"), session); form->addRow(copy);
    form->addRow(text("Бюджет пам'яті головного FFT:", "Main FFT memory budget:"), memory);
    auto *consent = new QCheckBox(text("Дозволити обчислення для підключеного клієнта", "Allow computation for a connected client"), worker);
    form->addRow(consent);
    auto *start = new QPushButton(text("Запустити", "Start"), worker);
    auto *status = new QLabel(worker); status->setWordWrap(true); form->addRow(start); form->addRow(status);
    connect(start, &QPushButton::clicked, this, [this, consent, bind, serverPort, session, memory]() {
        if (server_.running()) server_.requestStop();
        else if (consent->isChecked()) server_.start(bind->text().trimmed(), quint16(serverPort->value()), session->text(), memory->value());
        memory->setEnabled(!server_.running());
    });
    connect(consent, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked && server_.running()) server_.requestStop();
    });
    auto *timer = new QTimer(this); timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this, status, start, bind, serverPort, consent, memory, text]() {
        const bool active = server_.running();
        start->setText(active ? text("Зупинити", "Stop") : text("Запустити", "Start"));
        const QString rawStatus = server_.status();
        start->setEnabled(!(active && rawStatus == QStringLiteral("Stopping")) && (active || consent->isChecked()));
        bind->setEnabled(!active); serverPort->setEnabled(!active);
        memory->setEnabled(!active);
        QString state = rawStatus;
        if (state == "Stopped") state = text("Зупинено", "Stopped");
        else if (state == "Stopping") state = text("Зупинка", "Stopping");
        else if (state == "Starting") state = text("Запуск", "Starting");
        else if (state.startsWith("Cannot bind compute service")) state.replace("Cannot bind compute service", text("Не вдалося відкрити адресу/порт", "Cannot bind compute service"));
        else if (state == "Cannot start compute service") state = text("Не вдалося запустити обчислювач", "Cannot start compute service");
        else if (state == "Compute service resource limit") state = text("Обчислювач: недостатньо ресурсів", "Compute service resource limit");
        else if (state == "Client connected") state = text("Клієнт підключений", "Client connected");
        else if (state == "Connection test; synthetic data only") state = text("Тест з'єднання; лише синтетичні дані", "Connection test; synthetic data only");
        else if (state == "Listening; no receiver is opened") state = text("Очікування IQ від клієнта; власний приймач не потрібний", "Listening for client IQ; a local receiver is not required");
        else if (state == "Listening; previous stream ended") state = text("Очікування клієнта; попередній потік завершено", "Listening; previous stream ended");
        else if (state == "Listening; rejected a client with a different session token")
            state = text("Клієнта відхилено: ключі сеансу не збігаються. Очікування клієнта.", "Client rejected: session tokens differ. Listening for clients.");
        else if (state == "Listening; rejected an incomplete or incompatible handshake")
            state = text("Незавершене або несумісне привітання клієнта. Очікування клієнта.", "Incomplete or incompatible handshake. Listening for clients.");
        else if (state == "Listening; handshake reply failed")
            state = text("Не вдалося відповісти клієнту. Очікування клієнта.", "Could not reply to client. Listening for clients.");
        else if (state.startsWith("Blocks ")) {
            state.replace("Blocks ", text("Блоків ", "Blocks "));
            state.replace("samples ", text("семплів ", "samples "));
        }
        if (active && server_.port()) {
            state += QStringLiteral("\nTCP %1:%2").arg(bind->text()).arg(server_.port());
            if (QHostAddress(bind->text()).isLoopback())
                state += text("\nЛише цей ПК (loopback); недоступно через LAN", "\nThis PC only (loopback); not accessible over LAN");
        }
        status->setText(state);
    });
    start->setEnabled(false); timer->start();
}

void DspComputeDialog::addProbeTab(QTabWidget *tabs, bool uk) {
    const auto text = [uk](const char *ua, const char *en) { return QString::fromUtf8(uk ? ua : en); };
    auto *scroll = new QScrollArea(tabs);
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    auto *form = new QFormLayout(body);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto label = [body]() {
        auto *value = new QLabel(body); value->setWordWrap(true);
        value->setTextFormat(Qt::PlainText); value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return value;
    };
    probeTarget_ = label(); probeTarget_->setObjectName("dspProbeTarget"); form->addRow(probeTarget_);
    auto *buttons = new QHBoxLayout;
    probeStart_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), text("Тест з'єднання", "Test connection"), body);
    probeStart_->setObjectName("dspProbeStart");
    probeStart_->setToolTip(text("Використовує введені адресу, порт і ключ без застосування налаштувань. До 128 MiB в кожен бік; приймач не потрібний.",
        "Uses draft address, port and token without applying settings. Up to 128 MiB each way; no receiver required."));
    probeCancel_ = new QPushButton(style()->standardIcon(QStyle::SP_BrowserStop), text("Скасувати", "Cancel"), body);
    probeCancel_->setObjectName("dspProbeCancel"); probeCancel_->setEnabled(false);
    buttons->addWidget(probeStart_); buttons->addWidget(probeCancel_); buttons->addStretch(); form->addRow(buttons);
    probeStatus_ = label(); probeStatus_->setObjectName("dspProbeStatus"); form->addRow(probeStatus_);
    probeTimings_ = label(); probeTimings_->setObjectName("dspProbeTimings");
    probeUpload_ = label(); probeUpload_->setObjectName("dspProbeUpload");
    probeDownload_ = label(); probeDownload_->setObjectName("dspProbeDownload");
    form->addRow(text("Затримка:", "Latency:"), probeTimings_);
    form->addRow(text("До обчислювача:", "To compute PC:"), probeUpload_);
    form->addRow(text("Від обчислювача:", "From compute PC:"), probeDownload_);
    probeRate_ = new QDoubleSpinBox(body); probeRate_->setObjectName("dspProbeRateMSps");
    probeRate_->setDecimals(6); probeRate_->setRange(0.001, 1000); probeRate_->setSuffix(" MS/s"); probeRate_->setValue(50);
    probeLength_ = new QSpinBox(body); probeLength_->setObjectName("dspProbeFftLength");
    probeLength_->setRange(512, 67108864); probeLength_->setValue(65536);
    probeInterval_ = new QSpinBox(body); probeInterval_->setObjectName("dspProbeIntervalMs");
    probeInterval_->setRange(1, 5000); probeInterval_->setSuffix(" ms"); probeInterval_->setValue(100);
    probeOverlap_ = new QSpinBox(body); probeOverlap_->setObjectName("dspProbeOverlap");
    probeOverlap_->setRange(0, 95); probeOverlap_->setSuffix(" %");
    form->addRow(text("Fs для оцінки:", "Estimated Fs:"), probeRate_);
    form->addRow(text("Довжина FFT:", "FFT length:"), probeLength_);
    form->addRow(text("Інтервал кадрів:", "Frame interval:"), probeInterval_);
    form->addRow(text("Перекриття (безперервний IQ):", "Overlap (continuous IQ):"), probeOverlap_);
    for (QWidget *field : QList<QWidget *>{probeRate_, probeLength_, probeInterval_, probeOverlap_})
        field->setToolTip(text("Лише розрахунок навантаження; налаштування приймача не змінюються.",
                               "Traffic estimate only; receiver settings are not changed."));
    probeEstimate_ = label(); probeEstimate_->setObjectName("dspProbeEstimate"); form->addRow(probeEstimate_);
    auto *note = label();
    note->setText(text("МБ/с = 10⁶ байтів/с. RTT — час запит-відповідь; джитер — стандартне відхилення RTT. Оцінка із запасом 25% не включає час FFT, навантаження RX та інший трафік. Тест не запускається під час активної обробки на цьому обчислювачі.",
        "MB/s = 10^6 bytes/s. RTT is round-trip time; jitter is its standard deviation. The 25% headroom estimate excludes FFT time, RX load and other traffic. Testing requires idle compute sessions."));
    form->addRow(note);
    scroll->setWidget(body);
    tabs->addTab(scroll, text("Тест з'єднання", "Connection test"));
    connect(probeStart_, &QPushButton::clicked, this, [this, text]() {
        DspStreamEndpoint draft;
        draft.address = endpointAddress_->text().trimmed(); draft.port = quint16(endpointPort_->value());
        draft.token = endpointToken_->text().trimmed();
        if (probe_.start(draft)) {
            const bool loopback = QHostAddress(draft.address).isLoopback();
            probeTarget_->setText(QStringLiteral("TCP %1:%2%3").arg(draft.address).arg(draft.port)
                .arg(loopback ? text(" — loopback, не оцінка LAN", " — loopback, not a LAN measurement") : QString()));
        }
        refreshProbeResults();
    });
    connect(probeCancel_, &QPushButton::clicked, this, [this]() { probe_.cancel(); });
    connect(probeRate_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this]() { refreshProbeResults(); });
    for (auto *field : {probeLength_, probeInterval_, probeOverlap_})
        connect(field, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { refreshProbeResults(); });
    auto *timer = new QTimer(this); timer->setInterval(200);
    connect(timer, &QTimer::timeout, this, &DspComputeDialog::refreshProbeResults); timer->start();
    refreshProbeResults();
}

void DspComputeDialog::setProbeContext(double rate, int length, int interval, int overlap) {
    if (rate == contextRate_ && length == contextLength_ && interval == contextInterval_ && overlap == contextOverlap_) return;
    contextRate_ = rate; contextLength_ = length; contextInterval_ = interval; contextOverlap_ = overlap;
    probeRate_->setValue(rate / 1e6); probeLength_->setValue(length);
    probeInterval_->setValue(interval); probeOverlap_->setValue(overlap);
}

void DspComputeDialog::refreshProbeResults() {
    const auto text = [this](const char *ua, const char *en) { return QString::fromUtf8(ukrainian_ ? ua : en); };
    const auto r = probe_.result();
    probeStart_->setEnabled(!probe_.running()); probeCancel_->setEnabled(probe_.running());
    QString status;
    switch (r.stage) {
    case DspProbeStage::Idle: status = text("Ще не перевірено", "Not tested"); break;
    case DspProbeStage::Connecting: status = text("Підключення TCP…", "Connecting TCP..."); break;
    case DspProbeStage::Handshake: status = text("Перевірка ключа й протоколу…", "Checking token and protocol..."); break;
    case DspProbeStage::Latency: status = text("Вимірювання RTT…", "Measuring RTT..."); break;
    case DspProbeStage::Upload: status = text("Тест швидкості до обчислювача…", "Measuring upload..."); break;
    case DspProbeStage::Download: status = text("Тест швидкості від обчислювача…", "Measuring download..."); break;
    case DspProbeStage::Complete: status = text("Тест завершено", "Test complete"); break;
    case DspProbeStage::Cancelled: status = text("Тест скасовано", "Test cancelled"); break;
    case DspProbeStage::Failed:
        if (r.errorCode == "invalid_endpoint") status = text("Перевірте IP, порт і ключ сеансу (32–128 символів).", "Check IP, port and session token (32-128 characters).");
        else if (r.errorCode == "authentication_failed") status = text("Ключі сеансу не збігаються.", "Session tokens do not match.");
        else if (r.errorCode == "busy") status = text("Обчислювач зайнятий. Зупиніть зовнішні потоки або дочекайтеся іншого тесту.", "Compute PC busy. Stop external streams or wait for another test to finish.");
        else if (r.errorCode == "unsupported") status = text("Старий обчислювач не підтримує тест. Оновіть Obrii SDR на обох ПК.", "This worker does not support connection tests. Update Obrii SDR on both PCs.");
        else if (r.errorCode == "tcp") status = text("Не вдалося підключитися TCP: ", "TCP connection failed: ") + r.detail;
        else if (r.errorCode == "handshake") status = text("Немає відповіді на привітання. Перевірте порт і обчислювач; також можуть бути зайняті всі сеанси.", "No handshake reply. Check the port and compute service; all sessions may also be occupied.");
        else if (r.errorCode == "resources") status = text("Недостатньо ресурсів для тесту.", "Insufficient resources for the test.");
        else if (r.errorCode == "protocol") status = text("Некоректна відповідь або тестові дані.", "Invalid reply or test data.");
        else status = text("Тайм-аут або з'єднання розірвано. Тест не завершено.", "Timeout or disconnection. Test incomplete.");
        break;
    }
    probeStatus_->setText(status);
    probeTimings_->setText(r.pingCount ? text("TCP %1 ms; привітання %2 ms\nRTT min / median / p95: %3 / %4 / %5 ms\nДжитер %6 ms (%7 вимірів)",
        "TCP %1 ms; handshake %2 ms\nRTT min / median / p95: %3 / %4 / %5 ms\nJitter %6 ms (%7 samples)")
        .arg(r.connectMs, 0, 'f', 2).arg(r.handshakeMs, 0, 'f', 2).arg(r.rttMinMs, 0, 'f', 2)
        .arg(r.rttMedianMs, 0, 'f', 2).arg(r.rttP95Ms, 0, 'f', 2).arg(r.jitterMs, 0, 'f', 2).arg(r.pingCount) : QStringLiteral("--"));
    const auto speed = [](double rate, int bytes) { return rate > 0
        ? QStringLiteral("%1 MB/s (%2 Mbit/s), %3 MiB").arg(rate, 0, 'f', 2).arg(rate * 8, 0, 'f', 1).arg(bytes / 1048576.0, 0, 'f', 1)
        : QStringLiteral("--"); };
    probeUpload_->setText(speed(r.uploadMBps, r.uploadBytes));
    probeDownload_->setText(speed(r.downloadMBps, r.downloadBytes));
    const auto options = spectrumControls_->options();
    const auto e = estimateDspConnection(r, probeRate_->value() * 1e6, probeLength_->value(),
                                        probeInterval_->value(), probeOverlap_->value(), options.uploadLimitMBps);
    QString estimate = text("Потрібно: повний CF32 %1 MB/s; вікна %2 MB/s; спектри назад %3 MB/s при %4 кадр/с.",
        "Required: continuous CF32 %1 MB/s; windows %2 MB/s; returned spectra %3 MB/s at %4 frames/s.")
        .arg(e.continuousUploadMBps, 0, 'f', 2).arg(e.windowUploadMBps, 0, 'f', 2)
        .arg(e.spectrumDownloadMBps, 0, 'f', 2).arg(e.requestedFramesPerSecond, 0, 'f', 2);
    estimate += text("\nДля безперервного IQ: спектри %1 MB/s при до %2 кадр/с.",
        "\nContinuous IQ: returned spectra %1 MB/s at up to %2 frames/s.")
        .arg(e.continuousSpectrumDownloadMBps, 0, 'f', 2).arg(e.continuousFramesPerSecond, 0, 'f', 2);
    if (!e.windowsSnapshotFits) estimate += text("\nВікно більше локальної IQ-історії (8 388 608 семплів): потрібний безперервний режим.",
        "\nWindow exceeds local IQ history (8,388,608 samples): use continuous mode.");
    if (r.stage == DspProbeStage::Complete) {
        estimate += text("\nЛіміт FFT обчислювача: %1; бюджет передавання: %2 MB/s.\nЛише транспорт одного вікна: ≈%3 ms, до ≈%4 кадр/с без FFT.",
            "\nWorker FFT limit: %1; upload budget: %2 MB/s.\nOne-window transport only: ~%3 ms, up to ~%4 frames/s excluding FFT.")
            .arg(r.mainFftMaxLength).arg(options.uploadLimitMBps).arg(e.transportMsPerWindow, 0, 'f', 2).arg(e.networkFramesPerSecond, 0, 'f', 2);
        if (!e.memoryFits) estimate += text("\nДовжина FFT перевищує бюджет пам'яті обчислювача.", "\nFFT length exceeds the worker memory budget.");
        else estimate += text("\nЗа мережею: вікна — %1; повний IQ — %2.", "\nNetwork estimate: windows — %1; continuous IQ — %2.")
            .arg(e.windowsFit ? text("є запас", "headroom available") : text("не вкладається", "insufficient"))
            .arg(e.continuousFits ? text("є запас", "headroom available") : text("не вкладається", "insufficient"));
    }
    probeEstimate_->setText(estimate);
}
DspComputeDialog::~DspComputeDialog() = default;

void DspComputeDialog::setEndpoint(const DspStreamEndpoint &endpoint) {
    endpoint_ = endpoint;
    endpointEnabled_->setChecked(endpoint.enabled);
    endpointAddress_->setText(endpoint.address);
    endpointPort_->setValue(endpoint.port);
    endpointToken_->setText(endpoint.token);
}

void DspComputeDialog::setSpectrumComputeOptions(const SpectrumComputeOptions &options) {
    spectrumControls_->setOptions(options);
    spectrumOptions_ = spectrumControls_->options();
}

void DspComputeDialog::setSpectrumComputeStatus(const QString &status) {
    spectrumControls_->setStatus(status);
}
