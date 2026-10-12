#include "hfpredictiondialog.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDate>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

HfPredictionDialog::HfPredictionDialog(QWidget *parent) : HfPredictionDialog(parent, Translation{}) {}

HfPredictionDialog::HfPredictionDialog(QWidget *parent, Translation translation)
    : QDialog(parent), translate(std::move(translation)), job(this) {
    setWindowTitle(uiText("hf_prediction_title", "HF prediction / ITU-R P.533"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1120, 720); setMinimumSize(720, 480);
    auto *root = new QVBoxLayout(this);
    auto *splitter = new QSplitter(this);
    root->addWidget(splitter, 1);
    inputs = new QTabWidget(splitter);
    inputs->setMinimumWidth(280); inputs->setMaximumWidth(360);
    auto form = [&](const QString &title) {
        auto *scroll = new QScrollArea(inputs); scroll->setWidgetResizable(true);
        auto *contents = new QWidget(scroll); auto *layout = new QFormLayout(contents);
        layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scroll->setWidget(contents); inputs->addTab(scroll, title); return layout;
    };
    auto number = [&](QFormLayout *layout, const QString &label, const char *name, double low, double high, int precision, const QString &unit) {
        auto *spin = new QDoubleSpinBox(this); spin->setObjectName(name);
        spin->setRange(low, high); spin->setDecimals(precision); spin->setSuffix(unit); spin->setKeyboardTracking(false);
        layout->addRow(label, spin);
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &HfPredictionDialog::invalidate);
        return spin;
    };
    auto integer = [&](QFormLayout *layout, const QString &label, const char *name, int low, int high) {
        auto *spin = new QSpinBox(this); spin->setObjectName(name); spin->setRange(low, high); spin->setKeyboardTracking(false);
        layout->addRow(label, spin); connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, &HfPredictionDialog::invalidate); return spin;
    };
    auto *link = form(uiText("hf_prediction_tab_link", "Link"));
    txLat = number(link, uiText("hf_prediction_tx_latitude", "TX latitude"), "hfTxLatitude", -90, 90, 6, {});
    txLon = number(link, uiText("hf_prediction_tx_longitude", "TX longitude"), "hfTxLongitude", -180, 180, 6, {});
    rxLat = number(link, uiText("hf_prediction_rx_latitude", "RX latitude"), "hfRxLatitude", -90, 90, 6, {});
    rxLon = number(link, uiText("hf_prediction_rx_longitude", "RX longitude"), "hfRxLongitude", -180, 180, 6, {});
    power = number(link, uiText("hf_prediction_tx_power", "TX power"), "hfPowerW", 1, 1e9, 3, uiText("hf_prediction_unit_watts", " W"));
    txGain = number(link, uiText("hf_prediction_tx_gain", "TX gain offset"), "hfTxGain", -50, 60, 2, uiText("hf_prediction_unit_dbi", " dBi"));
    rxGain = number(link, uiText("hf_prediction_rx_gain", "RX gain offset"), "hfRxGain", -50, 60, 2, uiText("hf_prediction_unit_dbi", " dBi"));
    txGain->setToolTip(uiText("hf_prediction_isotropic_tooltip", "Direction-independent isotropic offset; no antenna height or directional pattern model."));
    rxGain->setToolTip(txGain->toolTip());
    longPath = new QCheckBox(uiText("hf_prediction_long_path", "Long great-circle path"), this); link->addRow(longPath);
    connect(longPath, &QCheckBox::toggled, this, &HfPredictionDialog::invalidate);
    auto *model = form(uiText("hf_prediction_tab_climatology", "Climatology"));
    year = integer(model, uiText("hf_prediction_year", "Year"), "hfYear", 1900, 2100);
    month = integer(model, uiText("hf_prediction_month", "Month"), "hfMonth", 1, 12);
    ssn = integer(model, uiText("hf_prediction_ssn", "Smoothed SSN (R12)"), "hfSsn", 1, 311);
    ssn->setToolTip(uiText("hf_prediction_ssn_tooltip", "Smoothed sunspot number on the model's R12 scale, not daily spots, Kp, or F10.7. SSN=0 is unsupported upstream."));
    bandwidth = number(model, uiText("hf_prediction_bandwidth", "Noise bandwidth"), "hfBandwidth", 0.005, 3e6, 3, uiText("hf_prediction_unit_hz", " Hz"));
    snr = number(model, uiText("hf_prediction_required_snr", "Required SNR"), "hfRequiredSnr", -30, 200, 2, uiText("hf_prediction_unit_db", " dB"));
    percent = integer(model, uiText("hf_prediction_snr_exceedance", "SNR exceedance"), "hfSnrPercent", 1, 99); percent->setSuffix(uiText("hf_prediction_unit_percent", " %"));
    noise = new QComboBox(this); noise->setObjectName("hfNoise");
    noise->addItems({uiText("hf_prediction_noise_city", "City"), uiText("hf_prediction_noise_residential", "Residential"),
        uiText("hf_prediction_noise_rural", "Rural"), uiText("hf_prediction_noise_quiet_rural", "Quiet rural"),
        uiText("hf_prediction_noise_quiet", "Quiet"), uiText("hf_prediction_noise_noisy", "Noisy")});
    model->addRow(uiText("hf_prediction_noise_environment", "P.372 environment"), noise);
    connect(noise, qOverload<int>(&QComboBox::currentIndexChanged), this, &HfPredictionDialog::invalidate);
    frequencies = new QLineEdit(this); frequencies->setObjectName("hfFrequencies"); model->addRow(uiText("hf_prediction_frequencies", "Frequencies (MHz)"), frequencies);
    frequencies->setToolTip(uiText("hf_prediction_frequencies_tooltip", "1..32 comma-separated values, 1.60..30.00 MHz, 0.01 MHz precision."));
    connect(frequencies, &QLineEdit::textChanged, this, &HfPredictionDialog::invalidate);
    auto *engine = form(uiText("hf_prediction_tab_engine", "Engine"));
    helper = new QLineEdit(this); helper->setObjectName("hfHelper");
    data = new QLineEdit(this); data->setObjectName("hfData");
    auto path = [&](const QString &label, QLineEdit *edit, bool executable) {
        auto *row = new QHBoxLayout; row->addWidget(edit);
        auto *browse = new QPushButton(this); browse->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
        browse->setToolTip(uiText("hf_prediction_browse", "Browse")); row->addWidget(browse); engine->addRow(label, row);
        connect(edit, &QLineEdit::textChanged, this, &HfPredictionDialog::invalidate);
        connect(browse, &QPushButton::clicked, this, [this, edit, executable] {
            const QString file = executable ? QFileDialog::getOpenFileName(this, uiText("hf_prediction_helper_file_dialog", "ITU HF helper"), edit->text(), uiText("hf_prediction_helper_filter", "itu_hf_helper.exe (*.exe)"))
                : QFileDialog::getExistingDirectory(this, uiText("hf_prediction_data_directory_dialog", "ITU HF model data"), edit->text());
            if (!file.isEmpty()) edit->setText(file);
        });
    };
    path(uiText("hf_prediction_helper", "Native helper"), helper, true); path(uiText("hf_prediction_data", "Model data"), data, false);
    auto *results = new QWidget(splitter); auto *resultLayout = new QVBoxLayout(results); resultLayout->setContentsMargins(0, 0, 0, 0);
    metric = new QComboBox(results); metric->setObjectName("hfMetric");
    metric->addItems({uiText("hf_prediction_metric_bcr", "Basic reliability (%)"), uiText("hf_prediction_metric_snr", "Median SNR (dB)"),
        uiText("hf_prediction_metric_percentile_snr", "Percentile SNR (dB)"), uiText("hf_prediction_metric_rx_power", "Median RX (dBm)"),
        uiText("hf_prediction_metric_muf", "Basic MUF (MHz)")});
    resultLayout->addWidget(metric);
    table = new QTableWidget(results); table->setObjectName("hfGrid"); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::SingleSelection); table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed); table->horizontalHeader()->setDefaultSectionSize(70);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed); table->verticalHeader()->setDefaultSectionSize(30);
    resultLayout->addWidget(table, 1);
    connect(metric, qOverload<int>(&QComboBox::currentIndexChanged), this, &HfPredictionDialog::render);
    auto *buttons = new QHBoxLayout;
    run = new QPushButton(uiText("hf_prediction_calculate", "Calculate"), this); run->setObjectName("hfCalculate"); run->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    stop = new QPushButton(this); stop->setObjectName("hfCancel"); stop->setIcon(style()->standardIcon(QStyle::SP_BrowserStop)); stop->setToolTip(uiText("hf_prediction_cancel", "Cancel")); stop->setEnabled(false);
    exportButton = new QPushButton(this); exportButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton)); exportButton->setToolTip(uiText("hf_prediction_export_tooltip", "Export CSV (includes coordinates)")); exportButton->setEnabled(false);
    buttons->addWidget(run); buttons->addWidget(stop); buttons->addStretch(); buttons->addWidget(exportButton); root->addLayout(buttons);
    status = new QLabel(this); status->setObjectName("hfStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); root->addWidget(status);
    connect(run, &QPushButton::clicked, this, &HfPredictionDialog::calculate);
    connect(stop, &QPushButton::clicked, &job, &hf::PredictionJob::cancel);
    connect(exportButton, &QPushButton::clicked, this, &HfPredictionDialog::exportCsv);
    connect(&job, &hf::PredictionJob::completed, this, [this](const hf::Result &r) {
        result = r; setBusy(false); render();
        status->setText(uiText("hf_prediction_completed", "Monthly climatology: %1 samples; %2-%3; smoothed SSN %4; isotropic gain offsets. No real-time storm correction.")
            .arg(r.samples.size()).arg(r.request.year).arg(r.request.month, 2, 10, QChar('0')).arg(r.request.smoothedSsn));
        emit resultReady(r);
    });
    connect(&job, &hf::PredictionJob::failed, this, [this](const QString &s) { setBusy(false); status->setText(s); });
    connect(&job, &hf::PredictionJob::cancelled, this, [this] { setBusy(false); status->setText(uiText("hf_prediction_cancelled", "Cancelled")); });
    hf::Request initial; const auto today = QDate::currentDate(); initial.year = today.year(); initial.month = today.month();
    applyRequest(initial); setRuntime(hf::Runtime::defaults());
}
HfPredictionDialog::~HfPredictionDialog() = default;
QString HfPredictionDialog::uiText(const char *key, const char *fallback) const {
    const QString english = QString::fromUtf8(fallback);
    if (translate) {
        const QString translated = translate(QString::fromLatin1(key), english);
        if (!translated.isEmpty()) return translated;
    }
    return english;
}
bool HfPredictionDialog::setLink(hf::Point tx, hf::Point rx, double watts, double gt, double gr) {
    auto r = request(); r.tx = tx; r.rx = rx; r.powerW = watts; r.txGainDbi = gt; r.rxGainDbi = gr; return setRequest(r);
}
bool HfPredictionDialog::setRequest(const hf::Request &r) {
    const QString error = hf::validate(r);
    if (!error.isEmpty()) {
        job.cancel(); invalidate(); status->setText(error); return false;
    }
    applyRequest(r); return true;
}
void HfPredictionDialog::applyRequest(const hf::Request &r) {
    if (job.isRunning()) job.cancel();
    loading = true;
    txLat->setValue(r.tx.latitude); txLon->setValue(r.tx.longitude); rxLat->setValue(r.rx.latitude); rxLon->setValue(r.rx.longitude);
    power->setValue(r.powerW); txGain->setValue(r.txGainDbi); rxGain->setValue(r.rxGainDbi);
    year->setValue(r.year); month->setValue(r.month); ssn->setValue(r.smoothedSsn); percent->setValue(r.snrPercent);
    bandwidth->setValue(r.bandwidthHz); snr->setValue(r.requiredSnrDb); longPath->setChecked(r.longPath); noise->setCurrentIndex(int(r.noise));
    QStringList f; for (double frequency : r.frequenciesMHz) f << QString::number(frequency, 'g', 12);
    frequencies->setText(f.join(", ")); selectedHours = r.hoursUtc;
    loading = false; invalidate();
}
hf::Request HfPredictionDialog::request() const {
    hf::Request r;
    r.tx = {txLat->value(), txLon->value()}; r.rx = {rxLat->value(), rxLon->value()};
    r.powerW = power->value(); r.txGainDbi = txGain->value(); r.rxGainDbi = rxGain->value();
    r.year = year->value(); r.month = month->value(); r.smoothedSsn = ssn->value(); r.snrPercent = percent->value();
    r.bandwidthHz = bandwidth->value(); r.requiredSnrDb = snr->value(); r.longPath = longPath->isChecked(); r.noise = hf::NoiseEnvironment(noise->currentIndex());
    r.hoursUtc = selectedHours; r.frequenciesMHz.clear();
    for (const auto &token : frequencies->text().split(',')) {
        bool ok = false; const double f = QLocale::c().toDouble(token.trimmed(), &ok);
        r.frequenciesMHz.append(ok ? f : std::numeric_limits<double>::quiet_NaN());
    }
    return r;
}
void HfPredictionDialog::setRuntime(const hf::Runtime &r) {
    if (job.isRunning()) job.cancel();
    loading = true; helper->setText(r.helperExecutable); data->setText(r.dataDirectory); timeoutMs = r.timeoutMs; loading = false; invalidate();
}
hf::Runtime HfPredictionDialog::runtime() const { return {helper->text(), data->text(), timeoutMs}; }
void HfPredictionDialog::invalidate() {
    if (loading) return;
    result = {}; table->clear(); table->setRowCount(0); table->setColumnCount(0); exportButton->setEnabled(false);
    const QString issue = hf::validate(request());
    status->setText(issue.isEmpty() ? uiText("hf_prediction_ready", "Monthly climatology / ITU-R P.533-14 + P.372") : issue);
}
void HfPredictionDialog::setBusy(bool busy) { inputs->setEnabled(!busy); run->setEnabled(!busy); stop->setEnabled(busy); exportButton->setEnabled(!busy && !result.samples.isEmpty()); }
void HfPredictionDialog::calculate() {
    invalidate(); QString error;
    if (!job.start(request(), runtime(), &error)) { status->setText(error); return; }
    setBusy(true); status->setText(uiText("hf_prediction_calculating", "Calculating monthly climatology..."));
}
void HfPredictionDialog::render() {
    if (result.samples.isEmpty()) return;
    QVector<int> hours = result.request.hoursUtc;
    if (hours.isEmpty()) for (int h = 0; h < 24; ++h) hours.append(h);
    table->clear(); table->setColumnCount(hours.size()); table->setRowCount(result.request.frequenciesMHz.size());
    QStringList hh, ff;
    for (int h : hours) hh << uiText("hf_prediction_hour_header", "%1 UTC").arg(h, 2, 10, QChar('0'));
    for (double f : result.request.frequenciesMHz) ff << uiText("hf_prediction_frequency_header", "%1 MHz").arg(QString::number(f, 'f', 2));
    table->setHorizontalHeaderLabels(hh); table->setVerticalHeaderLabels(ff);
    for (const auto &s : result.samples) {
        int row = -1;
        for (int i = 0; i < result.request.frequenciesMHz.size(); ++i)
            if (std::abs(result.request.frequenciesMHz[i] - s.frequencyMHz) < 1e-6) row = i;
        if (row < 0) continue;
        const double values[]{s.basicReliabilityPercent, s.medianSnrDb, s.percentileSnrDb, s.receivedDbm, s.basicMufMHz};
        const int mode = metric->currentIndex();
        const bool signalMetric = mode >= 1 && mode <= 3;
        auto *cell = new QTableWidgetItem(signalMetric && !s.hasSignal ? "--" : QString::number(values[mode], 'f', 1));
        cell->setTextAlignment(Qt::AlignCenter);
        if (mode == 0) {
            const double value = values[mode];
            cell->setBackground(value >= 80 ? QColor(193, 227, 196) : value >= 40 ? QColor(247, 225, 159) : QColor(239, 190, 189));
            cell->setForeground(QColor(25, 29, 28));
        }
        cell->setToolTip(uiText("hf_prediction_sample_tooltip", "%1 km; %2 MHz; %3 UTC\nBasic MUF %4 MHz; MUF90 %5; MUF10 %6\nOperational MUF %7 MHz; OPMUF90 %8; OPMUF10 %9\nMedian RX %10 dBm; SNR %11 dB; SNR at %12%: %13 dB\nBasic circuit reliability %14%")
            .arg(s.distanceKm, 0, 'f', 1).arg(s.frequencyMHz, 0, 'f', 2).arg(s.hourUtc, 2, 10, QChar('0'))
            .arg(s.basicMufMHz, 0, 'f', 2).arg(s.muf90MHz, 0, 'f', 2).arg(s.muf10MHz, 0, 'f', 2)
            .arg(s.operationalMufMHz, 0, 'f', 2).arg(s.operationalMuf90MHz, 0, 'f', 2).arg(s.operationalMuf10MHz, 0, 'f', 2)
            .arg(s.hasSignal ? QString::number(s.receivedDbm, 'f', 2) : "--").arg(s.hasSignal ? QString::number(s.medianSnrDb, 'f', 2) : "--")
            .arg(result.request.snrPercent).arg(s.hasSignal ? QString::number(s.percentileSnrDb, 'f', 2) : "--")
            .arg(s.basicReliabilityPercent, 0, 'f', 1));
        table->setItem(row, hours.indexOf(s.hourUtc), cell);
    }
}
void HfPredictionDialog::exportCsv() {
    if (result.samples.isEmpty()) return;
    const QString file = QFileDialog::getSaveFileName(this, uiText("hf_prediction_export_dialog", "Export HF results (includes coordinates)"), "hf-prediction.csv", uiText("hf_prediction_csv_filter", "CSV (*.csv)"));
    if (file.isEmpty()) return;
    QSaveFile out(file); if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) { status->setText(out.errorString()); return; }
    QTextStream csv(&out); csv.setCodec("UTF-8"); csv.setLocale(QLocale::c()); csv.setRealNumberPrecision(15);
    const auto &r = result.request;
    csv << "# ITU-R-HF commit 82017594a1c6cacfaa7e86954c4ae7b3a5825a3d; monthly climatology; isotropic offsets; ANALOG\n"
        << "# TX=" << r.tx.latitude << ',' << r.tx.longitude << "; RX=" << r.rx.latitude << ',' << r.rx.longitude
        << "; year=" << r.year << "; SSN_R12=" << r.smoothedSsn << "; watts=" << r.powerW << "; TX_dBi=" << r.txGainDbi
        << "; RX_dBi=" << r.rxGainDbi << "; bandwidth_Hz=" << r.bandwidthHz << "; required_SNR_dB=" << r.requiredSnrDb
        << "; SNR_percent=" << r.snrPercent << "; noise_enum=" << int(r.noise) << "; long_path=" << r.longPath << '\n'
        << "month,hour_UTC,frequency_MHz,distance_km,basic_MUF_MHz,MUF50_MHz,MUF90_MHz,MUF10_MHz,OPMUF_MHz,OPMUF90_MHz,OPMUF10_MHz,RX_dBm,external_noise_dB,median_SNR_dB,DuSN_dB,DlSN_dB,SNR_percentile_dB,BCR_percent,has_signal\n";
    for (const auto &s : result.samples) {
        csv << s.month << ',' << s.hourUtc << ',' << s.frequencyMHz << ',' << s.distanceKm << ',' << s.basicMufMHz << ','
            << s.muf50MHz << ',' << s.muf90MHz << ',' << s.muf10MHz << ',' << s.operationalMufMHz << ',' << s.operationalMuf90MHz << ','
            << s.operationalMuf10MHz << ',';
        if (s.hasSignal) csv << s.receivedDbm;
        csv << ',' << s.externalNoiseDb << ','; if (s.hasSignal) csv << s.medianSnrDb;
        csv << ',' << s.upperSnrDecileDb << ',' << s.lowerSnrDecileDb << ','; if (s.hasSignal) csv << s.percentileSnrDb;
        csv << ',' << s.basicReliabilityPercent << ',' << int(s.hasSignal) << '\n';
    }
    csv.flush(); status->setText(out.commit() ? uiText("hf_prediction_exported", "HF results exported") : out.errorString());
}
void HfPredictionDialog::closeEvent(QCloseEvent *event) { job.cancel(); QDialog::closeEvent(event); }
