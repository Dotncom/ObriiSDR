#include "main.h"
#include "appconstants.h"
#include "appruntimeutils.h"
#include "appsettingsutils.h"
#include "appdiagnostics.h"
#include "gnssqthhelpers.h"
#include "presethelpers.h"
#include "iqbuffer.h"
#include "diagnosticlogging.h"
#include "dmrbackendpaths.h"
#include "dsdneobridge.h"
#include "gophertrunkbridge.h"
#include "finetunewidget.h"
#include "multivfowidget.h"
#include "modulationutils.h"
#include "qthlocator.h"
#include "qthmapwidget.h"
#include "receiverbackendregistry.h"
#include "receiverdeviceutils.h"
#include "samplefileutils.h"
#include "bladerfbackend.h"
#include "spectrumfftworker.h"
#include "scanvisualutils.h"
#include "gnssserialutils.h"
#include "tuningutils.h"
#include "zoomspectrumdialog.h"
#include "zoomdensitydialog.h"
#include "transmitdialog.h"
#include "ft8stationdialog.h"
#include "dspflowpanel.h"
#include "wirelesslabdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMenu>
#include <QAction>
#include <QMetaObject>
#include <QHostAddress>
#include <QHostInfo>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPointer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QTextStream>
#include <QTextCursor>
#include <QPushButton>
#include <QToolButton>
#include <QKeyEvent>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QColor>
#include <QColorDialog>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QScrollArea>
#include <QFrame>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QSplashScreen>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QtMath>
#include <QUrl>
#include "audiofilterchainwidget.h"
#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
#include <QAudioDeviceInfo>
#endif
#include <cmath>
#include <cstddef>
#include <utility>
#include <cstring>
#include <limits>
#include <array>
#include <exception>
#include <new>
#include <utility>


fobos_dev_t* device = nullptr;
fobos_sdr_dev_t* agileDevice = nullptr;
FobosApiKind activeFobosApiKind = FobosApiKind::Standard;
float* iqData = nullptr; 
float* dataq = nullptr;
//size_t dataqSize = DEFAULT_BUF_LEN/8;
double globalFrequency = 100000000; 
double actualFrequency = 100000000; 
double listeningFrequency = 100000000; 
double globalSampleRate = 50000000;
double globalBandwidth = 200000;
double minFrequency = 60000000;
double maxFrequency = 140000000;
int globalModulationType = MOD_WFM;
int globalMode = 0;
std::vector<float> fftMagnitudes;
std::vector<float> fftFrequencies;
int fftLength = 65536;
int DEFAULT_BUF_LEN = 32768;
double currentScale = 100.0;
bool secondGraph = false;
bool syncWariable = false;
float sensitivity = 10;
float contrast = 10;
bool colorf = true;
int deviceID = 0;

YourClassName::YourClassName(QWidget *parent)
    : QMainWindow(parent), deviceOpened(false)
    {

    loadUiTranslations();

    setWindowTitle(QStringLiteral("Obrii SDR"));

    resize(1920, 1000);
    setMinimumSize(1180, 720);

    refreshFobosDeviceList();

    centralWidget = new QWidget(this);
    QScrollArea *graphScrollArea = new QScrollArea(this);
    graphScrollArea->setWidgetResizable(true);
    graphScrollArea->setWidget(centralWidget);
    setCentralWidget(graphScrollArea);

    QWidget *controlsWidget = new QWidget(this);
    QScrollArea *controlsScrollArea = new QScrollArea(this);
    controlsScrollArea->setWidgetResizable(true);
    controlsScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    controlsScrollArea->setWidget(controlsWidget);
    controlsWidget->setMinimumWidth(0);
    controlsWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    controlsScrollArea->setMinimumWidth(0);
    controlsScrollArea->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    controlsDock = new QDockWidget("Controls", this);
    controlsDock->setObjectName("controlsDock");
    markTranslatable(controlsDock, QStringLiteral("controls"), QStringLiteral("Controls"));
    controlsDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    controlsDock->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable);
    controlsDock->setMinimumWidth(220);
    controlsDock->setWidget(controlsScrollArea);
    addDockWidget(Qt::LeftDockWidgetArea, controlsDock);

    modulationButtonGroup = new QButtonGroup(this);
    auto addModulationRadioButton = [this](QWidget *parent,
                                           QHBoxLayout *targetLayout,
                                           const QString &label,
                                           int modulationId,
                                           const QString &toolTip) -> QRadioButton * {
        QRadioButton *button = new QRadioButton(label, parent);
        if (!toolTip.isEmpty()) {
            button->setToolTip(toolTip);
        }
        modulationButtonGroup->addButton(button, modulationId);
        targetLayout->addWidget(button);
        return button;
    };

    QWidget *digitalWidget = new QWidget(this);
    QVBoxLayout *digitalLayout = new QVBoxLayout(digitalWidget);
    QHBoxLayout *digitalHeaderLayout = new QHBoxLayout();
    QHBoxLayout *digitalModeLayout = new QHBoxLayout();
    QGridLayout *dmrLabLayout = new QGridLayout();
    digitalDecodeCheckbox = new QCheckBox("Decode", digitalWidget);
    markTranslatable(digitalDecodeCheckbox, QStringLiteral("decode"), QStringLiteral("Decode"));
    digitalDecodeCheckbox->setChecked(digitalDecodeEnabled);
    dmrBackendCombo = new QComboBox(digitalWidget);
    dmrBackendCombo->addItem(uiText(QStringLiteral("dmr_backend_fobos_mbelib"),
                                    QStringLiteral("Obrii SDR + mbelib")),
                             DMR_BACKEND_FOBOS_MBELIB);
    dmrBackendCombo->addItem(uiText(QStringLiteral("dmr_backend_fobos_opendmr"),
                                    QStringLiteral("Obrii SDR + OpenDMR/OP25")),
                             DMR_BACKEND_FOBOS_OPENDMR);
    dmrBackendCombo->addItem(QStringLiteral("DSD-neo"), DMR_BACKEND_DSD_NEO);
    dmrBackendCombo->addItem(uiText(QStringLiteral("dmr_backend_gopher_future"),
                                    QStringLiteral("GopherTrunk bridge")),
                             DMR_BACKEND_GOPHERTRUNK);
    dmrBackendCombo->setMaximumWidth(170);
    dmrBackendCombo->setToolTip(uiText(QStringLiteral("dmr_backend_tooltip"),
                                       QStringLiteral("Choose which DMR decoder path receives the selected channel.")));
    dmrLabCaptureCheckbox = new QCheckBox("Lock DMR", digitalWidget);
    markTranslatable(dmrLabCaptureCheckbox, QStringLiteral("dmr_lock"), QStringLiteral("Lock DMR"));
    dmrLabCaptureCheckbox->setToolTip(uiText(QStringLiteral("dmr_lock_tooltip"),
                                             QStringLiteral("Unchecked: auto-fill DMR metadata from the signal. Checked: decode only the selected CC/slot/TG/SRC.")));
    dmrLabColorCodeCombo = new QComboBox(digitalWidget);
    dmrLabColorCodeCombo->addItem("?", -1);
    for (int cc = 0; cc <= 15; ++cc) {
        dmrLabColorCodeCombo->addItem(QString::number(cc), cc);
    }
    dmrLabSlotCombo = new QComboBox(digitalWidget);
    dmrLabSlotCombo->addItem("?", 0);
    dmrLabSlotCombo->addItem("TS1", 1);
    dmrLabSlotCombo->addItem("TS2", 2);
    dmrLabCallTypeCombo = new QComboBox(digitalWidget);
    dmrLabCallTypeCombo->addItem("?", QStringLiteral("unknown"));
    dmrLabCallTypeCombo->addItem(uiText(QStringLiteral("dmr_group"), QStringLiteral("Group")), QStringLiteral("group"));
    dmrLabCallTypeCombo->addItem(uiText(QStringLiteral("dmr_private"), QStringLiteral("Private")), QStringLiteral("private"));
    dmrLabCallTypeCombo->addItem(uiText(QStringLiteral("dmr_all"), QStringLiteral("All")), QStringLiteral("all_call"));
    dmrBasebandRateCombo = new QComboBox(digitalWidget);
    dmrBasebandRateCombo->addItem("24 kHz", 24000);
    dmrBasebandRateCombo->addItem("48 kHz", 48000);
    dmrBasebandRateCombo->addItem("96 kHz", 96000);
    dmrBasebandRateCombo->addItem("192 kHz", 192000);
    dmrBasebandRateCombo->addItem("384 kHz", 384000);
    dmrBasebandRateCombo->setToolTip(uiText(QStringLiteral("dmr_4fsk_rate_tooltip"),
                                            QStringLiteral("DMR 4FSK discriminator output sample rate before symbol slicing.")));
    dmrChannelRateCombo = new QComboBox(digitalWidget);
    dmrChannelRateCombo->addItem(uiText(QStringLiteral("auto"), QStringLiteral("Auto")), 0);
    dmrChannelRateCombo->addItem("192 kHz", 192000);
    dmrChannelRateCombo->addItem("384 kHz", 384000);
    dmrChannelRateCombo->addItem("768 kHz", 768000);
    dmrChannelRateCombo->addItem("1536 kHz", 1536000);
    dmrChannelRateCombo->setToolTip(uiText(QStringLiteral("dmr_channel_rate_tooltip"),
                                           QStringLiteral("Intermediate DMR IQ channel rate before FM/4FSK discrimination. Use this to compare high-rate receiver channelizers.")));
    dmrAmbeLayoutCombo = new QComboBox(digitalWidget);
    dmrAmbeLayoutCombo->addItem(uiText(QStringLiteral("auto"), QStringLiteral("Auto")), DMR_AMBE_LAYOUT_AUTO);
    dmrAmbeLayoutCombo->addItem("Linear72", DMR_AMBE_LAYOUT_LINEAR72);
    dmrAmbeLayoutCombo->addItem("Split36", DMR_AMBE_LAYOUT_SPLIT36);
    dmrAmbeLayoutCombo->addItem("Dibit stripe", DMR_AMBE_LAYOUT_DIBIT_STRIPE);
    dmrAmbeLayoutCombo->addItem("Bit stripe", DMR_AMBE_LAYOUT_BIT_STRIPE);
    dmrAmbeLayoutCombo->setToolTip(uiText(QStringLiteral("dmr_ambe_layout_tooltip"),
                                          QStringLiteral("How the 216 DMR voice burst bits are mapped into three 72-bit AMBE frames.")));
    dmrPrivacyModeCombo = new QComboBox(digitalWidget);
    dmrPrivacyModeCombo->addItem(uiText(QStringLiteral("dmr_privacy_none"), QStringLiteral("No privacy")), DMR_PRIVACY_NONE);
    dmrPrivacyModeCombo->addItem(QStringLiteral("ARC4"), DMR_PRIVACY_ARC4);
    dmrPrivacyModeCombo->addItem(QStringLiteral("AES-256"), DMR_PRIVACY_AES256);
    dmrPrivacyModeCombo->addItem(uiText(QStringLiteral("dmr_privacy_hytera_basic"),
        QStringLiteral("Hytera Basic (beta)")), DMR_PRIVACY_HYTERA_BASIC);
    dmrPrivacyModeCombo->setToolTip(uiText(QStringLiteral("dmr_privacy_mode_tooltip"),
                                           QStringLiteral("The selected mode and saved key automatically apply to the selected decoder. No privacy disables decryption. No key guessing is performed.")));
    dmrPrivacyKeyIdCombo = new QComboBox(digitalWidget);
    dmrPrivacyKeyIdCombo->setToolTip(uiText(QStringLiteral("dmr_privacy_key_id_tooltip"),
                                            QStringLiteral("DMR privacy key ID from the selected ARC4/AES-256 key table.")));
    dmrPrivacyCapabilityLabel = new QLabel(digitalWidget);
    dmrPrivacyCapabilityLabel->setWordWrap(true);
    dmrPrivacyCapabilityLabel->setMinimumWidth(0);
    dmrPrivacyCapabilityLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    dmrPrivacyKeysButton = new QPushButton("Keys...", digitalWidget);
    markTranslatable(dmrPrivacyKeysButton, QStringLiteral("dmr_privacy_keys"), QStringLiteral("Keys..."));
    dmrPrivacyKeysButton->setToolTip(uiText(QStringLiteral("dmr_privacy_keys_tooltip"),
                                            QStringLiteral("Open the DMR privacy key table and choose the active key for backend tests.")));
    dmrPrivacyFrameOffsetCombo = new QComboBox(digitalWidget);
    for (int offset = 0; offset < 18; ++offset) {
        dmrPrivacyFrameOffsetCombo->addItem(QStringLiteral("Off %1").arg(offset), offset);
    }
    dmrPrivacyFrameOffsetCombo->setToolTip(uiText(QStringLiteral("dmr_privacy_offset_tooltip"),
                                                  QStringLiteral("ARC4 AMBE frame offset for live known-key backend testing.")));
    dmrPrivacyDropCombo = new QComboBox(digitalWidget);
    dmrPrivacyDropCombo->addItem(QStringLiteral("Drop 256"), QStringLiteral("dmra"));
    dmrPrivacyDropCombo->addItem(QStringLiteral("Drop 0"), QStringLiteral("drop0"));
    dmrPrivacyDropCombo->addItem(QStringLiteral("Fixed MI"), QStringLiteral("fixed-mi"));
    dmrPrivacyDropCombo->addItem(QStringLiteral("Key rev"), QStringLiteral("key-reverse"));
    dmrPrivacyDropCombo->addItem(QStringLiteral("MI rev"), QStringLiteral("mi-reverse"));
    dmrPrivacyDropCombo->addItem(QStringLiteral("Key+MI rev"), QStringLiteral("key-mi-reverse"));
    dmrPrivacyDropCombo->setToolTip(uiText(QStringLiteral("dmr_privacy_drop_tooltip"),
                                           QStringLiteral("ARC4 keystream drop/MI alignment variant for GopherTrunk live tests.")));
    dmrPrivacyBitLayoutCombo = new QComboBox(digitalWidget);
    dmrPrivacyBitLayoutCombo->addItem(QStringLiteral("Bits normal"), QStringLiteral("normal"));
    dmrPrivacyBitLayoutCombo->addItem(QStringLiteral("Reverse49"), QStringLiteral("reverse49"));
    dmrPrivacyBitLayoutCombo->addItem(QStringLiteral("Bit rev bytes"), QStringLiteral("bit-reverse-bytes"));
    dmrPrivacyBitLayoutCombo->addItem(QStringLiteral("Byte reverse"), QStringLiteral("byte-reverse"));
    dmrPrivacyBitLayoutCombo->setToolTip(uiText(QStringLiteral("dmr_privacy_layout_tooltip"),
                                                QStringLiteral("ARC4 bit ordering variant applied before/after decrypt.")));
    dmrManualTimingCheckbox = new QCheckBox("Timing", digitalWidget);
    markTranslatable(dmrManualTimingCheckbox, QStringLiteral("dmr_timing"), QStringLiteral("Timing"));
    dmrManualTimingCheckbox->setToolTip(uiText(QStringLiteral("dmr_timing_tooltip"),
                                               QStringLiteral("Force the DMR symbol sampling offset instead of searching around the detected timing.")));
    dmrTimingOffsetSpin = new QSpinBox(digitalWidget);
    dmrTimingOffsetSpin->setRange(-80, 80);
    dmrTimingOffsetSpin->setSingleStep(1);
    dmrTimingOffsetSpin->setValue(0);
    dmrTimingOffsetSpin->setToolTip(uiText(QStringLiteral("dmr_timing_offset_tooltip"),
                                           QStringLiteral("Manual symbol timing offset in discriminator samples.")));
    dmrSlicerRatioSpin = new QDoubleSpinBox(digitalWidget);
    dmrSlicerRatioSpin->setRange(0.45, 0.80);
    dmrSlicerRatioSpin->setDecimals(3);
    dmrSlicerRatioSpin->setSingleStep(0.005);
    dmrSlicerRatioSpin->setValue(0.625);
    dmrSlicerRatioSpin->setToolTip(uiText(QStringLiteral("dmr_slicer_tooltip"),
                                          QStringLiteral("Manual 4FSK inner/outer threshold ratio when adaptive slicing is disabled.")));
    dmrAdaptiveSlicerCheckbox = new QCheckBox("Adaptive", digitalWidget);
    markTranslatable(dmrAdaptiveSlicerCheckbox, QStringLiteral("dmr_adaptive"), QStringLiteral("Adaptive"));
    dmrAdaptiveSlicerCheckbox->setChecked(true);
    dmrAdaptiveSlicerCheckbox->setToolTip(uiText(QStringLiteral("dmr_adaptive_tooltip"),
                                                 QStringLiteral("Use local 4-level clustering for voice payloads; disable to force the manual slicer ratio.")));
    dsdNeoAutoStartCheckbox = new QCheckBox("Auto start", digitalWidget);
    markTranslatable(dsdNeoAutoStartCheckbox, QStringLiteral("auto_start"), QStringLiteral("Auto start"));
    dsdNeoAutoStartCheckbox->setToolTip(uiText(QStringLiteral("dsd_neo_autostart_tooltip"),
                                               QStringLiteral("Start dsd-neo automatically with PCM input through stdin and UDP decoded-audio output.")));
    dsdNeoProgramEdit = new QLineEdit(digitalWidget);
    dsdNeoProgramEdit->setPlaceholderText(defaultDsdNeoProgramPath());
    dsdNeoProgramEdit->setToolTip(uiText(QStringLiteral("dsd_neo_program_tooltip"),
                                         QStringLiteral("Path to dsd-neo executable. Default release layout uses dsd-neo/dsd-neo.exe next to ObriiSDR.")));
    dsdNeoInputPortSpin = new QSpinBox(digitalWidget);
    dsdNeoInputPortSpin->setRange(1024, 65535);
    dsdNeoInputPortSpin->setValue(7355);
    dsdNeoInputPortSpin->setPrefix(QStringLiteral("UDP in "));
    dsdNeoInputPortSpin->setToolTip(uiText(QStringLiteral("dsd_neo_tcp_tooltip"),
                                           QStringLiteral("PCM input port for a manually started decoder. Auto start uses stdin instead of UDP.")));
    dsdNeoUdpOutputPortSpin = new QSpinBox(digitalWidget);
    dsdNeoUdpOutputPortSpin->setRange(1024, 65535);
    dsdNeoUdpOutputPortSpin->setValue(23456);
    dsdNeoUdpOutputPortSpin->setPrefix(QStringLiteral("UDP "));
    dsdNeoUdpOutputPortSpin->setToolTip(uiText(QStringLiteral("dsd_neo_udp_tooltip"),
                                               QStringLiteral("Local UDP port where Obrii SDR listens for decoded PCM audio from dsd-neo.")));
    dsdNeoStatusLabel = new QLabel(uiText(QStringLiteral("dsd_neo_idle"), QStringLiteral("DSD-neo bridge idle")), digitalWidget);
    dsdNeoStatusLabel->setWordWrap(false);
    dsdNeoStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dsdNeoStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    dmrLabSourceIdEdit = new QLineEdit(digitalWidget);
    dmrLabSourceIdEdit->setPlaceholderText(uiText(QStringLiteral("dmr_src_id"), QStringLiteral("Src ID")));
    dmrLabTargetIdEdit = new QLineEdit(digitalWidget);
    dmrLabTargetIdEdit->setPlaceholderText(uiText(QStringLiteral("dmr_tg_target"), QStringLiteral("TG/Target")));
    dmrLabRadioEdit = new QLineEdit(digitalWidget);
    dmrLabRadioEdit->setPlaceholderText(uiText(QStringLiteral("dmr_radio"), QStringLiteral("Radio")));
    dmrLabNotesEdit = new QLineEdit(digitalWidget);
    dmrLabNotesEdit->setPlaceholderText(uiText(QStringLiteral("note"), QStringLiteral("Note")));
    dmrLabColorCodeCombo->setMaximumWidth(64);
    dmrLabSlotCombo->setMaximumWidth(72);
    dmrLabCallTypeCombo->setMaximumWidth(92);
    dmrBasebandRateCombo->setMaximumWidth(96);
    dmrChannelRateCombo->setMaximumWidth(96);
    dmrAmbeLayoutCombo->setMaximumWidth(128);
    dmrPrivacyModeCombo->setMaximumWidth(112);
    dmrPrivacyKeyIdCombo->setMaximumWidth(120);
    dmrPrivacyFrameOffsetCombo->setMaximumWidth(78);
    dmrPrivacyDropCombo->setMaximumWidth(118);
    dmrPrivacyBitLayoutCombo->setMaximumWidth(118);
    dmrTimingOffsetSpin->setMaximumWidth(70);
    dmrSlicerRatioSpin->setMaximumWidth(84);
    dsdNeoProgramEdit->setMaximumWidth(160);
    dsdNeoInputPortSpin->setMaximumWidth(82);
    dsdNeoUdpOutputPortSpin->setMaximumWidth(82);
    QPushButton *digitalClearButton = new QPushButton("Clear", digitalWidget);
    markTranslatable(digitalClearButton, QStringLiteral("clear"), QStringLiteral("Clear"));
    ft8StationButton = new QPushButton(QStringLiteral("FT8 Station..."), digitalWidget);
    markTranslatable(ft8StationButton, QStringLiteral("ft8_station"), QStringLiteral("FT8 Station..."));
    digitalStatusLabel = new QLabel(uiText(QStringLiteral("digital_audio_idle"), QStringLiteral("Digital audio decoder idle")), digitalWidget);
    digitalStatusLabel->setWordWrap(false);
    digitalStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    digitalStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    digitalTextEdit = new QPlainTextEdit(digitalWidget);
    digitalTextEdit->setReadOnly(true);
    digitalTextEdit->setMaximumBlockCount(2000);
    digitalTextEdit->setMinimumHeight(120);
    digitalTextEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    digitalTextEdit->setPlaceholderText(uiText(QStringLiteral("decoded_digital_text_placeholder"),
                                               QStringLiteral("Decoded digital-audio text will appear here.")));
    QLabel *digitalModeLabel = new QLabel("Mode:", digitalWidget);
    markTranslatable(digitalModeLabel, QStringLiteral("mode"), QStringLiteral("Mode:"));
    digitalModeLayout->addWidget(digitalModeLabel);
    addModulationRadioButton(digitalWidget, digitalModeLayout, "FT8", MOD_FT8, "FT8 weak-signal decoder");
    addModulationRadioButton(digitalWidget, digitalModeLayout, "RTTY", MOD_RTTY, "AFSK RTTY decoder");
    addModulationRadioButton(digitalWidget, digitalModeLayout, "FSK", MOD_FSK, "Frequency-shift keying decoder");
    addModulationRadioButton(digitalWidget, digitalModeLayout, "BPSK31", MOD_PSK, "BPSK31 audio decoder");
    addModulationRadioButton(digitalWidget, digitalModeLayout, "DMR", MOD_DMR, "DMR 4FSK sync monitor");
    digitalModeLayout->addStretch();
    cwDecoderControlsWidget = new QWidget(digitalWidget);
    QGridLayout *cwDecoderLayout = new QGridLayout(cwDecoderControlsWidget);
    cwDecoderLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *cwToneLabel = new QLabel("CW tone:", cwDecoderControlsWidget);
    markTranslatable(cwToneLabel, QStringLiteral("cw_decoder_tone"), QStringLiteral("CW tone:"));
    cwDecoderToneSpin = new QSpinBox(cwDecoderControlsWidget);
    cwDecoderToneSpin->setRange(100, 3000);
    cwDecoderToneSpin->setValue(700);
    cwDecoderToneSpin->setSuffix(QStringLiteral(" Hz"));
    QLabel *cwWpmLabel = new QLabel("Initial speed:", cwDecoderControlsWidget);
    markTranslatable(cwWpmLabel, QStringLiteral("cw_decoder_speed"), QStringLiteral("Initial speed:"));
    cwDecoderWpmSpin = new QSpinBox(cwDecoderControlsWidget);
    cwDecoderWpmSpin->setRange(5, 60);
    cwDecoderWpmSpin->setValue(18);
    cwDecoderWpmSpin->setSuffix(QStringLiteral(" WPM"));
    cwDecoderAdaptiveCheckbox = new QCheckBox("Adaptive speed", cwDecoderControlsWidget);
    markTranslatable(cwDecoderAdaptiveCheckbox,
                     QStringLiteral("cw_decoder_adaptive"),
                     QStringLiteral("Adaptive speed"));
    cwDecoderAdaptiveCheckbox->setChecked(true);
    QLabel *cwSelectivityLabel = new QLabel("Selectivity:", cwDecoderControlsWidget);
    markTranslatable(cwSelectivityLabel,
                     QStringLiteral("cw_decoder_selectivity"),
                     QStringLiteral("Selectivity:"));
    cwDecoderSelectivitySpin = new QSpinBox(cwDecoderControlsWidget);
    cwDecoderSelectivitySpin->setRange(1, 10);
    cwDecoderSelectivitySpin->setValue(5);
    cwDecoderSelectivitySpin->setToolTip(
        QStringLiteral("1 accepts weak/noisy tones; 10 narrows the tone detector and rejects more noise"));
    QLabel *cwAlphabetLabel = new QLabel("Alphabet:", cwDecoderControlsWidget);
    markTranslatable(cwAlphabetLabel,
                     QStringLiteral("cw_decoder_alphabet"),
                     QStringLiteral("Alphabet:"));
    cwDecoderAlphabetCombo = new QComboBox(cwDecoderControlsWidget);
    cwDecoderAlphabetCombo->addItem(QStringLiteral("English / International"), 0);
    cwDecoderAlphabetCombo->addItem(QString::fromUtf8(u8"Українська"), 1);
    cwDecoderAlphabetCombo->addItem(QString::fromUtf8(u8"English + Українська"), 2);
    cwDecoderAlphabetCombo->setCurrentIndex(2);
    cwDecoderLayout->addWidget(cwToneLabel, 0, 0);
    cwDecoderLayout->addWidget(cwDecoderToneSpin, 0, 1);
    cwDecoderLayout->addWidget(cwWpmLabel, 0, 2);
    cwDecoderLayout->addWidget(cwDecoderWpmSpin, 0, 3);
    cwDecoderLayout->addWidget(cwDecoderAdaptiveCheckbox, 1, 0, 1, 2);
    cwDecoderLayout->addWidget(cwSelectivityLabel, 1, 2);
    cwDecoderLayout->addWidget(cwDecoderSelectivitySpin, 1, 3);
    cwDecoderLayout->addWidget(cwAlphabetLabel, 2, 0);
    cwDecoderLayout->addWidget(cwDecoderAlphabetCombo, 2, 1, 1, 3);
    cwDecoderLayout->setColumnStretch(3, 1);
    cwDecoderControlsWidget->setVisible(pendingSettings.modulationType == MOD_CW);
    pskDecoderControlsWidget = new QWidget(digitalWidget);
    auto *pskLayout = new QGridLayout(pskDecoderControlsWidget); pskLayout->setContentsMargins(0,0,0,0);
    pskDecoderToneSpin = new QDoubleSpinBox(pskDecoderControlsWidget);
    pskDecoderToneSpin->setRange(100,3500); pskDecoderToneSpin->setDecimals(1); pskDecoderToneSpin->setSingleStep(.1);
    pskDecoderToneSpin->setSuffix(" Hz"); pskDecoderToneSpin->setValue(pendingSettings.pskDecoderToneHz);
    pskDecoderSquelchSpin = new QSpinBox(pskDecoderControlsWidget);
    pskDecoderSquelchSpin->setRange(1,100); pskDecoderSquelchSpin->setSuffix(" %"); pskDecoderSquelchSpin->setValue(pendingSettings.pskDecoderSquelchPercent);
    pskDecoderAfcCheckbox = new QCheckBox("AFC", pskDecoderControlsWidget); pskDecoderAfcCheckbox->setChecked(pendingSettings.pskDecoderAfc);
    pskDecoderEncodingCombo = new QComboBox(pskDecoderControlsWidget);
    pskDecoderEncodingCombo->addItem("Latin-1",0); pskDecoderEncodingCombo->addItem("UTF-8",1);
    auto *pskToneLabel = new QLabel(uiText("psk_tone", "PSK audio tone:"), pskDecoderControlsWidget);
    markTranslatable(pskToneLabel,"psk_tone","PSK audio tone:");
    auto *pskSquelchLabel = new QLabel(uiText("psk_squelch", "PSK selectivity:"), pskDecoderControlsWidget);
    markTranslatable(pskSquelchLabel,"psk_squelch","PSK selectivity:");
    pskLayout->addWidget(pskToneLabel,0,0); pskLayout->addWidget(pskDecoderToneSpin,0,1);
    pskLayout->addWidget(pskSquelchLabel,1,0); pskLayout->addWidget(pskDecoderSquelchSpin,1,1);
    pskLayout->addWidget(pskDecoderAfcCheckbox,2,0); pskLayout->addWidget(pskDecoderEncodingCombo,2,1);
    pskDecoderControlsWidget->setVisible(pendingSettings.modulationType == MOD_PSK);
    dmrLabLayout->addWidget(dmrLabCaptureCheckbox, 0, 0);
    QLabel *dmrCcLabel = new QLabel("CC:", digitalWidget);
    markTranslatable(dmrCcLabel, QStringLiteral("dmr_cc"), QStringLiteral("CC:"));
    dmrLabLayout->addWidget(dmrCcLabel, 0, 1);
    dmrLabLayout->addWidget(dmrLabColorCodeCombo, 0, 2);
    QLabel *dmrSlotLabel = new QLabel("Slot:", digitalWidget);
    markTranslatable(dmrSlotLabel, QStringLiteral("dmr_slot"), QStringLiteral("Slot:"));
    dmrLabLayout->addWidget(dmrSlotLabel, 0, 3);
    dmrLabLayout->addWidget(dmrLabSlotCombo, 0, 4);
    QLabel *dmrCallLabel = new QLabel("Call:", digitalWidget);
    markTranslatable(dmrCallLabel, QStringLiteral("dmr_call"), QStringLiteral("Call:"));
    dmrLabLayout->addWidget(dmrCallLabel, 0, 5);
    dmrLabLayout->addWidget(dmrLabCallTypeCombo, 0, 6);
    dmrLabLayout->addWidget(dmrLabSourceIdEdit, 1, 0, 1, 2);
    dmrLabLayout->addWidget(dmrLabTargetIdEdit, 1, 2, 1, 2);
    dmrLabLayout->addWidget(dmrLabRadioEdit, 1, 4, 1, 3);
    QLabel *dmr4fskLabel = new QLabel("4FSK:", digitalWidget);
    markTranslatable(dmr4fskLabel, QStringLiteral("dmr_4fsk"), QStringLiteral("4FSK:"));
    dmrLabLayout->addWidget(dmr4fskLabel, 2, 0);
    dmrLabLayout->addWidget(dmrBasebandRateCombo, 2, 1);
    QLabel *dmrAmbeLayoutLabel = new QLabel("AMBE layout:", digitalWidget);
    markTranslatable(dmrAmbeLayoutLabel, QStringLiteral("dmr_ambe_layout"), QStringLiteral("AMBE layout:"));
    QLabel *dmrChannelRateLabel = new QLabel("Ch:", digitalWidget);
    markTranslatable(dmrChannelRateLabel, QStringLiteral("dmr_channel_rate"), QStringLiteral("Ch:"));
    dmrLabLayout->addWidget(dmrChannelRateLabel, 2, 2);
    dmrLabLayout->addWidget(dmrChannelRateCombo, 2, 3);
    dmrLabLayout->addWidget(dmrAmbeLayoutLabel, 2, 4);
    dmrLabLayout->addWidget(dmrAmbeLayoutCombo, 2, 5, 1, 2);
    dmrLabLayout->addWidget(dmrManualTimingCheckbox, 3, 0);
    dmrLabLayout->addWidget(dmrTimingOffsetSpin, 3, 1);
    QLabel *dmrSlicerLabel = new QLabel("Slicer:", digitalWidget);
    markTranslatable(dmrSlicerLabel, QStringLiteral("dmr_slicer"), QStringLiteral("Slicer:"));
    dmrLabLayout->addWidget(dmrSlicerLabel, 3, 2);
    dmrLabLayout->addWidget(dmrSlicerRatioSpin, 3, 3);
    dmrLabLayout->addWidget(dmrAdaptiveSlicerCheckbox, 3, 4, 1, 3);
    dmrLabLayout->addWidget(dmrLabNotesEdit, 4, 0, 1, 7);
    QLabel *dmrPrivacyLabel = new QLabel("Privacy:", digitalWidget);
    markTranslatable(dmrPrivacyLabel, QStringLiteral("dmr_privacy"), QStringLiteral("Privacy:"));
    dmrLabLayout->addWidget(dmrPrivacyLabel, 5, 0);
    dmrLabLayout->addWidget(dmrPrivacyModeCombo, 5, 1);
    dmrLabLayout->addWidget(dmrPrivacyKeyIdCombo, 5, 2);
    dmrLabLayout->addWidget(dmrPrivacyKeysButton, 5, 3, 1, 2);
    QLabel *dmrPrivacyArc4Label = new QLabel("ARC4 test:", digitalWidget);
    markTranslatable(dmrPrivacyArc4Label, QStringLiteral("dmr_privacy_arc4_test"), QStringLiteral("ARC4 test:"));
    dmrLabLayout->addWidget(dmrPrivacyArc4Label, 6, 0);
    dmrLabLayout->addWidget(dmrPrivacyFrameOffsetCombo, 6, 1);
    dmrLabLayout->addWidget(dmrPrivacyDropCombo, 6, 2);
    dmrLabLayout->addWidget(dmrPrivacyBitLayoutCombo, 6, 3, 1, 2);
    dmrLabLayout->addWidget(dsdNeoAutoStartCheckbox, 7, 0, 1, 2);
    dmrLabLayout->addWidget(dsdNeoProgramEdit, 7, 2, 1, 2);
    QLabel *dsdNeoPortLabel = new QLabel("Ports:", digitalWidget);
    markTranslatable(dsdNeoPortLabel, QStringLiteral("ports"), QStringLiteral("Ports:"));
    dmrLabLayout->addWidget(dsdNeoPortLabel, 7, 4);
    dmrLabLayout->addWidget(dsdNeoInputPortSpin, 7, 5);
    dmrLabLayout->addWidget(dsdNeoUdpOutputPortSpin, 7, 6);
    dmrLabLayout->addWidget(dsdNeoStatusLabel, 8, 0, 1, 7);
    digitalHeaderLayout->addWidget(digitalDecodeCheckbox);
    QLabel *dmrBackendLabel = new QLabel("DMR backend:", digitalWidget);
    markTranslatable(dmrBackendLabel, QStringLiteral("dmr_backend"), QStringLiteral("DMR backend:"));
    digitalHeaderLayout->addWidget(dmrBackendLabel);
    digitalHeaderLayout->addWidget(dmrBackendCombo);
    digitalHeaderLayout->addStretch();
    digitalHeaderLayout->addWidget(ft8StationButton);
    digitalHeaderLayout->addWidget(digitalClearButton);
    digitalLayout->addLayout(digitalHeaderLayout);
    digitalLayout->addWidget(dmrPrivacyCapabilityLabel);
    digitalLayout->addLayout(digitalModeLayout);
    digitalLayout->addWidget(pskDecoderControlsWidget);
    digitalLayout->addLayout(dmrLabLayout);
    digitalLayout->addWidget(digitalStatusLabel);
    digitalLayout->addWidget(digitalTextEdit);
    digitalDock = new QDockWidget("Digital Audio", this);
    digitalDock->setObjectName("digitalDock");
    markTranslatable(digitalDock, QStringLiteral("digital_audio"), QStringLiteral("Digital Audio"));
    digitalDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    digitalDock->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
    digitalWidget->setMinimumWidth(0);
    digitalWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QScrollArea *digitalScrollArea = new QScrollArea(digitalDock);
    digitalScrollArea->setWidgetResizable(true);
    digitalScrollArea->setFrameShape(QFrame::NoFrame);
    digitalScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    digitalScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    digitalScrollArea->setMinimumWidth(0);
    digitalScrollArea->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    digitalScrollArea->setWidget(digitalWidget);
    digitalDock->setWidget(digitalScrollArea);
    addDockWidget(Qt::RightDockWidgetArea, digitalDock);
    digitalDock->hide();

    QWidget *videoPanel = new QWidget(this);
    QVBoxLayout *videoLayout = new QVBoxLayout(videoPanel);
    QHBoxLayout *videoHeaderLayout = new QHBoxLayout();
    QHBoxLayout *videoModeLayout = new QHBoxLayout();
    videoDecodeCheckbox = new QCheckBox("Decode", videoPanel);
    markTranslatable(videoDecodeCheckbox, QStringLiteral("decode"), QStringLiteral("Decode"));
    videoDecodeCheckbox->setChecked(videoDecodeEnabled);
    videoDemodCombo = new QComboBox(videoPanel);
    videoDemodCombo->addItem("FM video", VideoProcessor::FmVideo);
    videoDemodCombo->addItem("AM video", VideoProcessor::AmVideo);
    videoStandardCombo = new QComboBox(videoPanel);
    videoStandardCombo->addItem("PAL 15.625 kHz", 15625.0);
    videoStandardCombo->addItem("NTSC 15.734 kHz", 15734.2657);
    sstvDemodulationCombo = new QComboBox(videoPanel);
    sstvDemodulationCombo->addItem(QStringLiteral("USB"), SSTV_DEMOD_USB);
    sstvDemodulationCombo->addItem(QStringLiteral("LSB"), SSTV_DEMOD_LSB);
    sstvDemodulationCombo->addItem(QStringLiteral("NFM"), SSTV_DEMOD_NFM);
    sstvDemodulationCombo->setToolTip(
        QStringLiteral("Select USB/LSB for HF SSTV or NFM for audio sent through an FM handheld radio"));
    videoInvertCheckbox = new QCheckBox("Invert", videoPanel);
    markTranslatable(videoInvertCheckbox, QStringLiteral("invert"), QStringLiteral("Invert"));
    videoHSyncCheckbox = new QCheckBox("HSync", videoPanel);
    videoHSyncCheckbox->setChecked(true);
    videoHSyncCheckbox->setToolTip("Align video lines by the darkest horizontal sync pulse");
    videoVSyncCheckbox = new QCheckBox("VSync", videoPanel);
    videoVSyncCheckbox->setChecked(true);
    videoVSyncCheckbox->setToolTip("Reset analog video frame on broad vertical sync pulses");
    videoTestPatternCheckbox = new QCheckBox("Test", videoPanel);
    markTranslatable(videoTestPatternCheckbox, QStringLiteral("test"), QStringLiteral("Test"));
    videoTestPatternCheckbox->setToolTip("Generate an internal test pattern for the selected video mode");
    videoStatusLabel = new QLabel(uiText(QStringLiteral("video_decoder_disabled"),
                                         QStringLiteral("Video decoder disabled")), videoPanel);
    videoWidget = new VideoWidget(videoPanel);
    QPushButton *videoSaveImageButton = new QPushButton(
        uiText(QStringLiteral("save_received_image"), QStringLiteral("Save received image...")),
        videoPanel);
    markTranslatable(videoSaveImageButton,
                     QStringLiteral("save_received_image"),
                     QStringLiteral("Save received image..."));
    videoSaveImageButton->setEnabled(false);
    QLabel *videoModeLabel = new QLabel("Mode:", videoPanel);
    markTranslatable(videoModeLabel, QStringLiteral("mode"), QStringLiteral("Mode:"));
    videoModeLayout->addWidget(videoModeLabel);
    addModulationRadioButton(videoPanel, videoModeLayout, "ATV", MOD_ATV, "Analog television video demodulator");
    addModulationRadioButton(videoPanel, videoModeLayout, "SSTV", MOD_SSTV, "Slow-scan television image decoder");
    addModulationRadioButton(videoPanel, videoModeLayout, "APT", MOD_APT, "NOAA APT weather satellite image decoder");
    addModulationRadioButton(videoPanel, videoModeLayout, "WEFAX", MOD_WEFAX, "HF weather fax image decoder");
    addModulationRadioButton(videoPanel, videoModeLayout, "LRPT", MOD_LRPT, "Meteor LRPT beta IQ monitor");
    QLabel *sstvDemodulationLabel = new QLabel(QStringLiteral("SSTV demod:"), videoPanel);
    markTranslatable(sstvDemodulationLabel,
                     QStringLiteral("sstv_demodulation"),
                     QStringLiteral("SSTV demod:"));
    videoModeLayout->addWidget(sstvDemodulationLabel);
    videoModeLayout->addWidget(sstvDemodulationCombo);
    videoModeLayout->addStretch();
    videoHeaderLayout->addWidget(videoDecodeCheckbox);
    videoHeaderLayout->addWidget(videoDemodCombo);
    videoHeaderLayout->addWidget(videoStandardCombo);
    videoHeaderLayout->addWidget(videoInvertCheckbox);
    videoHeaderLayout->addWidget(videoHSyncCheckbox);
    videoHeaderLayout->addWidget(videoVSyncCheckbox);
    videoHeaderLayout->addWidget(videoTestPatternCheckbox);
    videoHeaderLayout->addWidget(videoSaveImageButton);
    lrptDecodeButton = new QPushButton(uiText(QStringLiteral("lrpt_decode"), QStringLiteral("LRPT decode")), videoPanel);
    markTranslatable(lrptDecodeButton, QStringLiteral("lrpt_decode"), QStringLiteral("LRPT decode"));
    videoHeaderLayout->addWidget(lrptDecodeButton);
    connect(lrptDecodeButton, &QPushButton::clicked, this, &YourClassName::showLrptDecoder);
    videoHeaderLayout->addStretch();
    videoLayout->addLayout(videoModeLayout);
    videoLayout->addLayout(videoHeaderLayout);
    videoLayout->addWidget(videoStatusLabel);
    videoLayout->addWidget(videoWidget, 1);
    videoDock = new QDockWidget("Video", this);
    markTranslatable(videoDock, QStringLiteral("video"), QStringLiteral("Video"));
    videoDock->setObjectName("videoDock");
    videoDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    videoDock->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    videoDock->setWidget(videoPanel);
    addDockWidget(Qt::RightDockWidgetArea, videoDock);
    videoDock->hide();

    connect(videoSaveImageButton, &QPushButton::clicked, this, [this]() {
        if (!videoWidget || videoWidget->frame().isNull()) return;
        const QString suggested = QDir::home().filePath(
            QStringLiteral("obrii_sstv_%1.png")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
        QString path = QFileDialog::getSaveFileName(
            this,
            uiText(QStringLiteral("save_received_image"), QStringLiteral("Save received image...")),
            suggested,
            QStringLiteral("PNG image (*.png);;JPEG image (*.jpg *.jpeg);;Bitmap image (*.bmp)"));
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".png");
        if (!videoWidget->frame().save(path)) {
            QMessageBox::warning(this,
                                 uiText(QStringLiteral("save_received_image"),
                                        QStringLiteral("Save received image...")),
                                 uiText(QStringLiteral("save_received_image_failed"),
                                        QStringLiteral("Could not save the received image.")));
            return;
        }
        videoStatusLabel->setText(
            uiText(QStringLiteral("received_image_saved"),
                   QStringLiteral("Received image saved: %1")).arg(path));
    });

    QHBoxLayout *scaleLayout = new QHBoxLayout();
    QVBoxLayout *contrastLayout = new QVBoxLayout();
    QVBoxLayout *sensLayout = new QVBoxLayout();
    QGridLayout *scaleControlGrid = new QGridLayout();
    QVBoxLayout *levelMinLayout = new QVBoxLayout();
    QVBoxLayout *levelMaxLayout = new QVBoxLayout();
    QVBoxLayout *layout = new QVBoxLayout();
    QGridLayout *checkboxLayout = new QGridLayout();
    graphLayout = new QVBoxLayout();
    QHBoxLayout *graphToolLayout = new QHBoxLayout();
    
    for (int i = 0; i < 8; ++i) {
        checkBoxes[i] = new QCheckBox(QString("GPIO %1").arg(i + 1), this);
        checkboxLayout->addWidget(checkBoxes[i], i / 4, i % 4);
        connect(checkBoxes[i], &QCheckBox::stateChanged, this, &YourClassName::onCheckboxStateChanged);
    }
    fftComboBox = new QComboBox(this);
    fftComboBox->addItem("512");
    fftComboBox->addItem("1024");
    fftComboBox->addItem("2048");
    fftComboBox->addItem("4096");
    fftComboBox->addItem("8192");
    fftComboBox->addItem("16384");
    fftComboBox->addItem("32768");
    fftComboBox->addItem("65536");
    fftComboBox->addItem("131072");
    fftComboBox->addItem("262144");
    fftComboBox->addItem("524288");
    fftComboBox->addItem("1048576");
    fftComboBox->addItem("2097152");
    fftComboBox->addItem("4194304");
    fftComboBox->addItem("8388608");
    fftComboBox->addItem("16777216");
    fftComboBox->addItem("33554432");
    fftComboBox->addItem("67108864");
    fftComboBox->setEditable(true);
    if (fftComboBox->lineEdit()) {
        fftComboBox->lineEdit()->setReadOnly(true);
    }
    fftComboBox->setCurrentText(QString::number(pendingSettings.fftLength));

    fftBinWidthModeCheckbox = new QCheckBox(QStringLiteral("Hz/point"), this);
    markTranslatable(fftBinWidthModeCheckbox,
                     QStringLiteral("fft_hz_per_point"),
                     QStringLiteral("Hz/point"));
    fftBinWidthSpin = new QDoubleSpinBox(this);
    fftBinWidthSpin->setRange(0.1, 1000000.0);
    fftBinWidthSpin->setDecimals(3);
    fftBinWidthSpin->setSingleStep(0.1);
    fftBinWidthSpin->setKeyboardTracking(false);
    fftBinWidthSpin->setSuffix(QStringLiteral(" Hz"));
    fftBinWidthSpin->setValue(fftTargetBinWidthHz);
    
    lnaGainSlider = new QSlider(Qt::Horizontal, this);
    lnaGainSlider->setRange(1, 3);
    lnaGainSlider->setValue(1);
    
    vgaGainSlider = new QSlider(Qt::Horizontal, this);
    vgaGainSlider->setRange(0, 31);
    vgaGainSlider->setValue(3);

    rtlAgcCheckbox = new QCheckBox("RTL AGC", this);
    markTranslatable(rtlAgcCheckbox, QStringLiteral("rtl_agc"), QStringLiteral("RTL AGC"));
    rtlAgcCheckbox->setToolTip("Use RTL-SDR automatic tuner gain. Disable for stable DMR 4FSK levels.");

    rtlGainSlider = new QSlider(Qt::Horizontal, this);
    rtlGainSlider->setRange(0, 496);
    rtlGainSlider->setSingleStep(1);
    rtlGainSlider->setPageStep(10);
    rtlGainSlider->setValue(pendingSettings.rtlTunerGainTenthsDb);

    volumeSlider = new QSlider(Qt::Horizontal, this);
    volumeSlider->setRange(0, 200);
    volumeSlider->setValue(100);

    audioLowPassSlider = new QSlider(Qt::Horizontal, this);
    audioLowPassSlider->setRange(0, AUDIO_LOW_PASS_SLIDER_MAX);
    audioLowPassSlider->setSingleStep(1);
    audioLowPassSlider->setPageStep(10);
    audioLowPassSlider->setValue(0);

    audioHighPassSlider = new QSlider(Qt::Horizontal, this);
    audioHighPassSlider->setRange(0, AUDIO_HIGH_PASS_SLIDER_MAX);
    audioHighPassSlider->setSingleStep(1);
    audioHighPassSlider->setPageStep(4);
    audioHighPassSlider->setValue(0);

    hfNoiseCancelDepthSlider = new QSlider(Qt::Horizontal, this);
    hfNoiseCancelDepthSlider->setRange(HF_NOISE_CANCEL_DEPTH_MIN, HF_NOISE_CANCEL_DEPTH_MAX);
    hfNoiseCancelDepthSlider->setSingleStep(5);
    hfNoiseCancelDepthSlider->setPageStep(25);
    hfNoiseCancelDepthSlider->setValue(hfNoiseCancelDepthToSliderValue(pendingSettings.hfNoiseCancelDepth));

    hfNoiseCancelRefGainSlider = new QSlider(Qt::Horizontal, this);
    hfNoiseCancelRefGainSlider->setRange(HF_NOISE_CANCEL_REF_GAIN_MIN, HF_NOISE_CANCEL_REF_GAIN_MAX);
    hfNoiseCancelRefGainSlider->setSingleStep(5);
    hfNoiseCancelRefGainSlider->setPageStep(30);
    hfNoiseCancelRefGainSlider->setValue(hfNoiseCancelRefGainToSliderValue(pendingSettings.hfNoiseCancelRefGainDb));

    hfNoiseCancelRefDelaySlider = new QSlider(Qt::Horizontal, this);
    hfNoiseCancelRefDelaySlider->setRange(HF_NOISE_CANCEL_REF_DELAY_MIN_NS, HF_NOISE_CANCEL_REF_DELAY_MAX_NS);
    hfNoiseCancelRefDelaySlider->setSingleStep(10);
    hfNoiseCancelRefDelaySlider->setPageStep(100);
    hfNoiseCancelRefDelaySlider->setValue(hfNoiseCancelRefDelayToSliderValue(pendingSettings.hfNoiseCancelRefDelayNs));

    hfNoiseCancelRefTiltSlider = new QSlider(Qt::Horizontal, this);
    hfNoiseCancelRefTiltSlider->setRange(HF_NOISE_CANCEL_REF_TILT_MIN, HF_NOISE_CANCEL_REF_TILT_MAX);
    hfNoiseCancelRefTiltSlider->setSingleStep(5);
    hfNoiseCancelRefTiltSlider->setPageStep(30);
    hfNoiseCancelRefTiltSlider->setValue(hfNoiseCancelRefTiltToSliderValue(pendingSettings.hfNoiseCancelRefTiltDb));

    hfAudioBlankerThresholdSlider = new QSlider(Qt::Horizontal, this);
    hfAudioBlankerThresholdSlider->setRange(HF_AUDIO_BLANKER_THRESHOLD_MIN, HF_AUDIO_BLANKER_THRESHOLD_MAX);
    hfAudioBlankerThresholdSlider->setSingleStep(5);
    hfAudioBlankerThresholdSlider->setPageStep(20);
    hfAudioBlankerThresholdSlider->setValue(static_cast<int>(std::lround(
        (std::clamp)(pendingSettings.hfAudioBlankerThreshold, 2.0, 20.0) * 10.0)));

    hfNoiseCancelFreezeCheckbox = new QCheckBox("Freeze", this);
    markTranslatable(hfNoiseCancelFreezeCheckbox, QStringLiteral("freeze"), QStringLiteral("Freeze"));
    hfNoiseCancelFreezeCheckbox->setToolTip("Hold the small adaptive trim added on top of the manual HF2 reference");

    hfAudioBlankerCheckbox = new QCheckBox("Audio blanker", this);
    markTranslatable(hfAudioBlankerCheckbox, QStringLiteral("hf_audio_blanker"), QStringLiteral("Audio blanker"));
    hfAudioBlankerCheckbox->setChecked(pendingSettings.hfAudioBlankerEnabled);
    hfAudioBlankerCheckbox->setToolTip("Blank short impulse spikes in the selected HF audio channel.");

    hfInterferenceBaselineCheckbox = new QCheckBox("Baseline", this);
    markTranslatable(hfInterferenceBaselineCheckbox, QStringLiteral("hf_baseline"), QStringLiteral("Baseline"));
    hfInterferenceBaselineCheckbox->setChecked(hfInterferenceBaselineEnabled);
    hfInterferenceBaselineCheckbox->setToolTip("Apply a learned HF noise-floor curve to the visual spectrum/waterfall only.");

    hfInterferenceRawOverlayCheckbox = new QCheckBox("Raw overlay", this);
    markTranslatable(hfInterferenceRawOverlayCheckbox, QStringLiteral("hf_raw_overlay"), QStringLiteral("Raw overlay"));
    hfInterferenceRawOverlayCheckbox->setChecked(hfInterferenceRawOverlayEnabled);
    hfInterferenceRawOverlayCheckbox->setToolTip("Draw the original raw spectrum over the cleaned HF baseline view.");

    hfInterferenceBaselineLearnButton = new QPushButton("Learn", this);
    markTranslatable(hfInterferenceBaselineLearnButton, QStringLiteral("hf_baseline_learn"), QStringLiteral("Learn"));
    hfInterferenceBaselineLearnButton->setToolTip("Capture the current visible spectrum as a smoothed HF interference baseline.");

    hfInterferenceBaselineClearButton = new QPushButton("Clear", this);
    markTranslatable(hfInterferenceBaselineClearButton, QStringLiteral("clear"), QStringLiteral("Clear"));
    hfInterferenceBaselineClearButton->setToolTip("Remove the learned HF interference baseline.");

    hfInterferenceDefaultsButton = new QPushButton("Default", this);
    markTranslatable(hfInterferenceDefaultsButton, QStringLiteral("hf_interference_defaults"), QStringLiteral("Default"));
    hfInterferenceDefaultsButton->setToolTip(uiText(
        QStringLiteral("hf_interference_default_tooltip"),
        QStringLiteral("Reset HF interference lab controls and clear the learned baseline.")));

    hfInterferenceAnalyzeButton = new QPushButton("Analysis...", this);
    markTranslatable(hfInterferenceAnalyzeButton,
                     QStringLiteral("research_interference_open"),
                     QStringLiteral("Analysis..."));
    hfInterferenceAnalyzeButton->setToolTip(uiText(
        QStringLiteral("research_interference_open_tooltip"),
        QStringLiteral("Open comb, harmonic and before/after interference analysis.")));

    hfInterferenceBaselineDepthSlider = new QSlider(Qt::Horizontal, this);
    hfInterferenceBaselineDepthSlider->setRange(0, 200);
    hfInterferenceBaselineDepthSlider->setSingleStep(5);
    hfInterferenceBaselineDepthSlider->setPageStep(25);
    hfInterferenceBaselineDepthSlider->setValue(static_cast<int>(std::lround(hfInterferenceBaselineDepth * 100.0)));

    hfInterferenceBaselineSmoothSlider = new QSlider(Qt::Horizontal, this);
    hfInterferenceBaselineSmoothSlider->setRange(1, 301);
    hfInterferenceBaselineSmoothSlider->setSingleStep(10);
    hfInterferenceBaselineSmoothSlider->setPageStep(40);
    hfInterferenceBaselineSmoothSlider->setValue(hfInterferenceBaselineSmoothBins);

    volumeLabel = new QLabel("Volume: 100%", this);
    audioLowPassLabel = new QLabel("Audio LPF: Auto", this);
    audioHighPassLabel = new QLabel("Audio HPF: Off", this);
    hfNoiseCancelDepthLabel = new QLabel(hfNoiseCancelDepthLabelText(pendingSettings.hfNoiseCancelDepth), this);
    hfNoiseCancelRefGainLabel = new QLabel(hfNoiseCancelRefGainLabelText(pendingSettings.hfNoiseCancelRefGainDb), this);
    hfNoiseCancelRefDelayLabel = new QLabel(hfNoiseCancelRefDelayLabelText(pendingSettings.hfNoiseCancelRefDelayNs), this);
    hfNoiseCancelRefTiltLabel = new QLabel(hfNoiseCancelRefTiltLabelText(pendingSettings.hfNoiseCancelRefTiltDb), this);
    hfAudioBlankerThresholdLabel = new QLabel("Blanker: 8.0x", this);
    hfInterferenceBaselineDepthLabel = new QLabel("Baseline depth: 100%", this);
    hfInterferenceBaselineSmoothLabel = new QLabel("Smooth: 121 bins", this);
    hfInterferenceBaselineStatusLabel = new QLabel("Baseline: empty", this);
    lnaGainLabel = new QLabel("LNA Gain: 1", this);
    vgaGainLabel = new QLabel("VGA Gain: 3", this);
    rtlGainLabel = new QLabel("RTL gain: 16.6 dB", this);
    
    scaleSlider = new QSlider(Qt::Horizontal, this); 
    scaleSlider->setRange(scalePercentToSliderValue(MIN_SCALE_PERCENT),
                          scalePercentToSliderValue(MAX_SCALE_PERCENT));
    scaleSlider->setSingleStep(1);
    scaleSlider->setPageStep(10);
    scaleSlider->setValue(scalePercentToSliderValue(currentScale));
    scaleLabel = new QLabel(scaleLabelText(currentScale), this);
    additionalScaleDivisorSlider = new QSlider(Qt::Horizontal, this);
    additionalScaleDivisorSlider->setRange(1, ADDITIONAL_SCALE_DIVISOR_MAX);
    additionalScaleDivisorSlider->setSingleStep(1);
    additionalScaleDivisorSlider->setPageStep(5);
    additionalScaleDivisorSlider->setInvertedAppearance(true);
    additionalScaleDivisorSlider->setValue(additionalScaleDivisor);
    additionalScaleDivisorSlider->setToolTip(uiText(
        QStringLiteral("additional_scale_divisor_tooltip"),
        QStringLiteral("Divide the main display scale by 1 to 100 for deeper spectrum and waterfall zoom.")));
    additionalScaleDivisorLabel = new QLabel(
        QStringLiteral("%1: 1:%2")
            .arg(uiText(QStringLiteral("additional_scale"), QStringLiteral("Extra zoom")))
            .arg(additionalScaleDivisor),
        this);
    
    audioDeviceComboBox = new QComboBox(this);
    comboBox = new QComboBox(this);
    modeBox = new QComboBox(this);
    sampleBox = new QComboBox(this);
    clkBox = new QComboBox(this);
    languageComboBox = new QComboBox(this);
    populateLanguageCombo(languageComboBox);
    languageComboBox->hide();
    
    audioCheckbox = new QCheckBox("Audio", this);
    markTranslatable(audioCheckbox, QStringLiteral("audio"), QStringLiteral("Audio"));
    audioCheckbox->setChecked(pendingSettings.audioEnabled);
    syncCheckbox = new QCheckBox("Sync", this);
    markTranslatable(syncCheckbox, QStringLiteral("sync"), QStringLiteral("Sync"));
    syncCheckbox->setChecked(false);
    syncCheckbox->setEnabled(false);
    syncCheckbox->setToolTip("Async reader is forced for continuous streaming tests.");
    graphCheckbox = new QCheckBox("Spectr 2", this);
    markTranslatable(graphCheckbox, QStringLiteral("spectrum2"), QStringLiteral("Spectr 2"));
    colorCheckbox = new QCheckBox("Color spectrum", this);
    markTranslatable(colorCheckbox, QStringLiteral("colorful"), QStringLiteral("Color spectrum"));
    audioCheckbox->hide();
    syncCheckbox->hide();
    graphCheckbox->hide();
    colorCheckbox->hide();

    rebuildReceiverDeviceCombo();
    rebuildReceiverModeChoices();
    updateReceiverSpecificControls();
    
    clkBox->addItem("Internal", 0);
    clkBox->addItem("External", 1);

    processor = new DataProcessor( this);
    audioProcessor = new AudioProcessor(this);
    digitalDecoderThread = new QThread(this);
    digitalDecoderThread->setObjectName(QStringLiteral("DigitalDecoderThread"));
    digitalDecoder = new DigitalDecoder();
    digitalDecoder->moveToThread(digitalDecoderThread);
    connect(digitalDecoderThread, &QThread::finished, digitalDecoder, &QObject::deleteLater);
    digitalDecoderThread->start();
    dsdNeoThread = new QThread(this);
    dsdNeoThread->setObjectName(QStringLiteral("DsdNeoTransportThread"));
    dsdNeoBridge = new DsdNeoBridge();
    dsdNeoBridge->moveToThread(dsdNeoThread);
    connect(dsdNeoThread, &QThread::finished, dsdNeoBridge, &QObject::deleteLater);
    dsdNeoThread->start();
    gopherTrunkBridge = new GopherTrunkBridge(this);
    videoProcessorThread = new QThread(this);
    videoProcessorThread->setObjectName(QStringLiteral("VideoProcessorThread"));
    videoProcessor = new VideoProcessor();
    videoProcessor->moveToThread(videoProcessorThread);
    connect(videoProcessorThread, &QThread::finished, videoProcessor, &QObject::deleteLater);
    videoProcessorThread->start();
    playbackManager = new PlaybackManager(this);
    recordingManager = new RecordingManager(this);
    networkController = new NetworkController(this);
    remoteAudioPlayer = new RemoteAudioPlayer(this);
    audioRelaySocket = new QUdpSocket(this);
    gnssNtpSocket = new QUdpSocket(this);
    gnssSerialPort = new QSerialPort(this);
    connect(gnssSerialPort, &QSerialPort::readyRead, this, &YourClassName::handleGnssSerialReadyRead);
    connect(gnssSerialPort,
            QOverload<QSerialPort::SerialPortError>::of(&QSerialPort::errorOccurred),
            this,
            &YourClassName::handleGnssSerialError);
    connect(audioRelaySocket, &QUdpSocket::readyRead, this, &YourClassName::receiveAudioRelayDatagrams);
    audioHttpServer = new QTcpServer(this);
    connect(audioHttpServer, &QTcpServer::newConnection, this, &YourClassName::acceptAudioHttpClient);
    networkSettingsDebounceTimer = new QTimer(this);
    networkSettingsDebounceTimer->setSingleShot(true);
    connect(networkSettingsDebounceTimer, &QTimer::timeout, this, [this]() {
        sendRemoteControlCommand("setParameters");
    });
    networkSettingsAckTimer = new QTimer(this);
    networkSettingsAckTimer->setSingleShot(true);
    networkSettingsAckTimer->setInterval(NETWORK_SETTINGS_ACK_TIMEOUT_MS);
    connect(networkSettingsAckTimer, &QTimer::timeout, this, &YourClassName::handleNetworkSettingsAckTimeout);
    connectDataProcessorSignals();
    
    graphWidget = new MyGraphWidget(this);
    graphWidget->setMinimumSize(760, 180);
    graphWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    graphWidget->setPauseControlVisible(true);
    
    scaleWidget = new ScaleWidget(this);
    scaleWidget->setMinimumWidth(760);
    scaleWidget->setFixedHeight(50);
    scaleWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    
    waterfallWidget = new MyWaterfallWidget(this);
    waterfallWidget->setMinimumSize(760, 300);
    waterfallWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    waterfallWidget->setPauseControlVisible(true);
    waterfallWidget->setRowsPerFrame(waterfallRowsPerFrame);
    waterfallWidget->setAreaMeasurementEnabled(waterfallAreaMeasurementEnabled);
    waterfallWidget->set3DHistoryRows(waterfall3DHistoryRows);
    waterfallWidget->set3DSurfaceStyle(waterfall3DSurfaceStyle);
    waterfallWidget->set3DSurfaceSmoothing(waterfall3DSmoothing);
    waterfallWidget->set3DSurfaceLighting(waterfall3DLighting);
    waterfallWidget->set3DSpectrumSliceScrollStep(waterfall3DSpectrumSliceScrollStep);
    waterfallWidget->set3DSpectrumSliceWidth(waterfall3DSpectrumSliceRows);
    waterfallWidget->set3DSpectrumSliceCapture(waterfall3DSpectrumSliceCapture);
    waterfallWidget->set3DSpectrumSliceCaptureFixed(waterfall3DSpectrumSliceCaptureFixed);
    waterfallWidget->set3DFixedPlane(waterfall3DFixedPlane);
    waterfallWidget->set3DMonochrome(waterfall3DMonochrome);
    waterfallWidget->set3DHeightGradient(waterfall3DHeightGradient);
    waterfallWidget->set3DMonochromeColor(waterfall3DMonochromeColor);
    waterfallWidget->set3DMonochromeGradientDepth(waterfall3DMonochromeGradientDepth);
    waterfallWidget->set3DModifierFreeSliceInput(waterfall3DVncSliceInput);
    waterfallWidget->setAlternativeSpectrumGradientFill(alternativeSpectrumGradientFill);
    waterfallWidget->setAlternativeSpectrumGradientOpacity(alternativeSpectrumGradientOpacity);
    waterfallWidget->setRenderBackend(experimentalGpuWaterfall
                                          ? MyWaterfallWidget::RenderBackend::GpuPrepared
                                          : MyWaterfallWidget::RenderBackend::CpuTexture);

    waterfallDisplayModeLabel = new QLabel(QStringLiteral("Waterfall:"), this);
    markTranslatable(waterfallDisplayModeLabel,
                     QStringLiteral("waterfall_display_mode"),
                     QStringLiteral("Waterfall:"));
    waterfallDisplayModeCombo = new QComboBox(this);
    waterfallDisplayModeCombo->addItem(
        uiText(QStringLiteral("waterfall_display_2d"), QStringLiteral("2D waterfall")),
        static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall2D));
    waterfallDisplayModeCombo->addItem(
        uiText(QStringLiteral("waterfall_display_3d"), QStringLiteral("3D waterfall")),
        static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3D));
    waterfallDisplayModeCombo->addItem(
        uiText(QStringLiteral("waterfall_display_3d_mini"), QStringLiteral("3D + mini waterfall")),
        static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3DWithMini));
    waterfallDisplayModeCombo->setToolTip(uiText(
        QStringLiteral("waterfall_display_mode_tooltip"),
        QStringLiteral("Choose the main waterfall visualization mode.")));
    waterfallDisplayModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    waterfallDisplayModeCombo->setMaximumWidth(260);
    QHBoxLayout *waterfallDisplayModeLayout = new QHBoxLayout();
    waterfallDisplayModeLayout->setContentsMargins(0, 0, 0, 0);
    waterfallDisplayModeLayout->addWidget(waterfallDisplayModeLabel);
    waterfallDisplayModeLayout->addWidget(waterfallDisplayModeCombo);
    waterfallDisplayModeLayout->addStretch(1);

    waterfall3DResolutionLabel = new QLabel(QStringLiteral("3D resolution:"), this);
    markTranslatable(waterfall3DResolutionLabel,
                     QStringLiteral("waterfall_3d_resolution"),
                     QStringLiteral("3D resolution:"));
    waterfall3DResolutionCombo = new QComboBox(this);
    for (const int divisor : {1, 2, 4, 8, 16, 32, 64}) {
        waterfall3DResolutionCombo->addItem(QStringLiteral("1/%1").arg(divisor), divisor);
    }
    waterfall3DResolutionCombo->setCurrentIndex(waterfall3DResolutionCombo->findData(
        waterfall3DResolutionDivisor));
    waterfall3DResolutionCombo->setToolTip(uiText(
        QStringLiteral("waterfall_3d_resolution_tooltip"),
        QStringLiteral("3D frequency columns as a fraction of the visible 2D waterfall width.")));
    waterfall3DResolutionCombo->setMaximumWidth(100);
    QHBoxLayout *waterfall3DResolutionLayout = new QHBoxLayout();
    waterfall3DResolutionLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DResolutionLayout->addWidget(waterfall3DResolutionLabel);
    waterfall3DResolutionLayout->addWidget(waterfall3DResolutionCombo);
    waterfall3DResolutionLayout->addStretch(1);

    QLabel *waterfall3DSurfaceStyleLabel = new QLabel(QStringLiteral("3D surface:"), this);
    markTranslatable(waterfall3DSurfaceStyleLabel,
                     QStringLiteral("waterfall_3d_surface_style"),
                     QStringLiteral("3D surface:"));
    waterfall3DSurfaceStyleCombo = new QComboBox(this);
    waterfall3DSurfaceStyleCombo->addItem(
        uiText(QStringLiteral("surface_style_original"), QStringLiteral("Original / needles")), 0);
    waterfall3DSurfaceStyleCombo->addItem(
        uiText(QStringLiteral("surface_style_solid"), QStringLiteral("Solid surface")), 1);
    waterfall3DSurfaceStyleCombo->setCurrentIndex(
        waterfall3DSurfaceStyleCombo->findData(waterfall3DSurfaceStyle));

    QLabel *waterfall3DSmoothingLabel = new QLabel(QStringLiteral("Smoothing:"), this);
    markTranslatable(waterfall3DSmoothingLabel,
                     QStringLiteral("surface_smoothing"),
                     QStringLiteral("Smoothing:"));
    waterfall3DSmoothingCombo = new QComboBox(this);
    waterfall3DSmoothingCombo->addItem(
        uiText(QStringLiteral("surface_smoothing_off"), QStringLiteral("Off")), 0);
    waterfall3DSmoothingCombo->addItem(
        uiText(QStringLiteral("surface_smoothing_soft"), QStringLiteral("Soft")), 1);
    waterfall3DSmoothingCombo->addItem(
        uiText(QStringLiteral("surface_smoothing_strong"), QStringLiteral("Strong")), 2);
    waterfall3DSmoothingCombo->setCurrentIndex(
        waterfall3DSmoothingCombo->findData(waterfall3DSmoothing));

    QLabel *waterfall3DLightingLabel = new QLabel(QStringLiteral("Lighting:"), this);
    markTranslatable(waterfall3DLightingLabel,
                     QStringLiteral("surface_lighting"),
                     QStringLiteral("Lighting:"));
    waterfall3DLightingCombo = new QComboBox(this);
    waterfall3DLightingCombo->addItem(
        uiText(QStringLiteral("surface_lighting_off"), QStringLiteral("Off")), 0);
    waterfall3DLightingCombo->addItem(
        uiText(QStringLiteral("surface_lighting_soft"), QStringLiteral("Soft shadows")), 1);
    waterfall3DLightingCombo->addItem(
        uiText(QStringLiteral("surface_lighting_strong"), QStringLiteral("Strong shadows")), 2);
    waterfall3DLightingCombo->setCurrentIndex(
        waterfall3DLightingCombo->findData(waterfall3DLighting));
    const QString surfaceProcessingTooltip = uiText(
        QStringLiteral("surface_processing_tooltip"),
        QStringLiteral("GPU surface processing. Smoothing and lighting cost additional GPU time; keep them off on weak systems."));
    waterfall3DSurfaceStyleCombo->setToolTip(surfaceProcessingTooltip);
    waterfall3DSmoothingCombo->setToolTip(surfaceProcessingTooltip);
    waterfall3DLightingCombo->setToolTip(surfaceProcessingTooltip);
    QGridLayout *waterfall3DSurfaceLayout = new QGridLayout();
    waterfall3DSurfaceLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DSurfaceLayout->addWidget(waterfall3DSurfaceStyleLabel, 0, 0);
    waterfall3DSurfaceLayout->addWidget(waterfall3DSurfaceStyleCombo, 0, 1);
    waterfall3DSurfaceLayout->addWidget(waterfall3DSmoothingLabel, 1, 0);
    waterfall3DSurfaceLayout->addWidget(waterfall3DSmoothingCombo, 1, 1);
    waterfall3DSurfaceLayout->addWidget(waterfall3DLightingLabel, 2, 0);
    waterfall3DSurfaceLayout->addWidget(waterfall3DLightingCombo, 2, 1);
    waterfall3DSurfaceLayout->setColumnStretch(1, 1);

    waterfall3DHistoryRowsLabel = new QLabel(QStringLiteral("3D memory:"), this);
    markTranslatable(waterfall3DHistoryRowsLabel,
                     QStringLiteral("waterfall_3d_history_rows"),
                     QStringLiteral("3D memory:"));
    waterfall3DHistoryRowsSpin = new QSpinBox(this);
    waterfall3DHistoryRowsSpin->setRange(16, 2048);
    waterfall3DHistoryRowsSpin->setSingleStep(16);
    waterfall3DHistoryRowsSpin->setValue(waterfall3DHistoryRows);
    waterfall3DHistoryRowsSpin->setSuffix(uiText(QStringLiteral("waterfall_3d_rows_suffix"),
                                                 QStringLiteral(" rows")));
    waterfall3DHistoryRowsSpin->setToolTip(uiText(
        QStringLiteral("waterfall_3d_history_rows_tooltip"),
        QStringLiteral("Number of recent waterfall rows retained in the 3D volume. Larger values use more CPU and memory.")));
    QHBoxLayout *waterfall3DHistoryRowsLayout = new QHBoxLayout();
    waterfall3DHistoryRowsLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DHistoryRowsLayout->addWidget(waterfall3DHistoryRowsLabel);
    waterfall3DHistoryRowsLayout->addWidget(waterfall3DHistoryRowsSpin);
    waterfall3DHistoryRowsLayout->addStretch(1);

    waterfall3DSliceStepLabel = new QLabel(QStringLiteral("Frequency slice step:"), this);
    markTranslatable(waterfall3DSliceStepLabel,
                     QStringLiteral("waterfall_3d_slice_step"),
                     QStringLiteral("Frequency slice step:"));
    waterfall3DSliceStepSpin = new QSpinBox(this);
    waterfall3DSliceStepSpin->setRange(1, 256);
    waterfall3DSliceStepSpin->setValue(waterfall3DSliceScrollStep);
    waterfall3DSliceStepSpin->setSuffix(QStringLiteral(" pt"));
    waterfall3DSliceStepSpin->setToolTip(uiText(
        QStringLiteral("waterfall_3d_slice_step_tooltip"),
        QStringLiteral("Frequency points moved by each wheel step while an Alt slice is held.")));
    QHBoxLayout *waterfall3DSliceStepLayout = new QHBoxLayout();
    waterfall3DSliceStepLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DSliceStepLayout->addWidget(waterfall3DSliceStepLabel);
    waterfall3DSliceStepLayout->addWidget(waterfall3DSliceStepSpin);
    waterfall3DSliceStepLayout->addStretch(1);

    waterfall3DSliceWidthLabel = new QLabel(QStringLiteral("Frequency slice width:"), this);
    markTranslatable(waterfall3DSliceWidthLabel,
                     QStringLiteral("waterfall_3d_slice_width"),
                     QStringLiteral("Frequency slice width:"));
    waterfall3DSliceWidthSpin = new QSpinBox(this);
    waterfall3DSliceWidthSpin->setRange(1, 4096);
    waterfall3DSliceWidthSpin->setValue(waterfall3DSliceWidth);
    waterfall3DSliceWidthSpin->setSuffix(QStringLiteral(" pt"));
    waterfall3DSliceWidthSpin->setToolTip(uiText(
        QStringLiteral("waterfall_3d_slice_width_tooltip"),
        QStringLiteral("Number of adjacent frequency points displayed in the held Alt slice; 1 shows one line.")));
    QHBoxLayout *waterfall3DSliceWidthLayout = new QHBoxLayout();
    waterfall3DSliceWidthLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DSliceWidthLayout->addWidget(waterfall3DSliceWidthLabel);
    waterfall3DSliceWidthLayout->addWidget(waterfall3DSliceWidthSpin);
    waterfall3DSliceWidthLayout->addStretch(1);

    waterfall3DSpectrumSliceStepLabel = new QLabel(QStringLiteral("Spectrum slice step:"), this);
    markTranslatable(waterfall3DSpectrumSliceStepLabel,
                     QStringLiteral("waterfall_3d_spectrum_slice_step"),
                     QStringLiteral("Spectrum slice step:"));
    waterfall3DSpectrumSliceStepSpin = new QSpinBox(this);
    waterfall3DSpectrumSliceStepSpin->setRange(1, 2048);
    waterfall3DSpectrumSliceStepSpin->setValue(waterfall3DSpectrumSliceScrollStep);
    waterfall3DSpectrumSliceStepSpin->setSuffix(
        uiText(QStringLiteral("waterfall_3d_rows_suffix"), QStringLiteral(" rows")));
    waterfall3DSpectrumSliceStepSpin->setToolTip(uiText(
        QStringLiteral("waterfall_3d_spectrum_slice_step_tooltip"),
        QStringLiteral("Rows moved by each wheel step while an Alt + right-button spectrum slice is held.")));
    QHBoxLayout *waterfall3DSpectrumSliceStepLayout = new QHBoxLayout();
    waterfall3DSpectrumSliceStepLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DSpectrumSliceStepLayout->addWidget(waterfall3DSpectrumSliceStepLabel);
    waterfall3DSpectrumSliceStepLayout->addWidget(waterfall3DSpectrumSliceStepSpin);
    waterfall3DSpectrumSliceStepLayout->addStretch(1);

    waterfall3DSpectrumSliceRowsLabel = new QLabel(QStringLiteral("Spectrum slice rows:"), this);
    markTranslatable(waterfall3DSpectrumSliceRowsLabel,
                     QStringLiteral("waterfall_3d_spectrum_slice_rows"),
                     QStringLiteral("Spectrum slice rows:"));
    waterfall3DSpectrumSliceRowsSpin = new QSpinBox(this);
    waterfall3DSpectrumSliceRowsSpin->setRange(1, 2048);
    waterfall3DSpectrumSliceRowsSpin->setValue(waterfall3DSpectrumSliceRows);
    waterfall3DSpectrumSliceRowsSpin->setSuffix(
        uiText(QStringLiteral("waterfall_3d_rows_suffix"), QStringLiteral(" rows")));
    waterfall3DSpectrumSliceRowsSpin->setToolTip(uiText(
        QStringLiteral("waterfall_3d_spectrum_slice_rows_tooltip"),
        QStringLiteral("Number of adjacent time rows displayed by the Alt + right-button spectrum slice.")));
    waterfall3DSpectrumSliceCaptureCheckbox = new QCheckBox(
        uiText(QStringLiteral("waterfall_3d_spectrum_slice_capture"), QStringLiteral("Capture")),
        this);
    waterfall3DSpectrumSliceCaptureCheckbox->setChecked(waterfall3DSpectrumSliceCapture);
    waterfall3DSpectrumSliceCaptureCheckbox->setToolTip(uiText(
        QStringLiteral("waterfall_3d_spectrum_slice_capture_tooltip"),
        QStringLiteral("Follow the selected data rows as they move through the rolling 3D history. Off keeps the slice at a fixed model position and shows the current rows there.")));
    waterfall3DSpectrumSliceCaptureFixedCheckbox = new QCheckBox(
        uiText(QStringLiteral("waterfall_3d_spectrum_slice_capture_fixed"),
               QStringLiteral("Fix capture")),
        this);
    waterfall3DSpectrumSliceCaptureFixedCheckbox->setChecked(waterfall3DSpectrumSliceCaptureFixed);
    waterfall3DSpectrumSliceCaptureFixedCheckbox->setEnabled(waterfall3DSpectrumSliceCapture);
    waterfall3DSpectrumSliceCaptureFixedCheckbox->setToolTip(uiText(
        QStringLiteral("waterfall_3d_spectrum_slice_capture_fixed_tooltip"),
        QStringLiteral("Freeze a copy of the captured rows at the picked model position until those rows expire from the rolling history.")));
    waterfall3DFixedPlaneCheckbox = new QCheckBox(
        uiText(QStringLiteral("waterfall_3d_fixed_plane"),
               QStringLiteral("Fix plane")),
        this);
    waterfall3DFixedPlaneCheckbox->setChecked(waterfall3DFixedPlane);
    waterfall3DFixedPlaneCheckbox->setToolTip(uiText(
        QStringLiteral("waterfall_3d_fixed_plane_tooltip"),
        QStringLiteral("Use the fixed front-facing 3D plane from Alternative interface mode and lock camera controls.")));
    waterfall3DMonochromeCheckbox = new QCheckBox(
        uiText(QStringLiteral("waterfall_3d_monochrome"),
               QStringLiteral("Monochrome blue 3D")),
        this);
    markTranslatable(waterfall3DMonochromeCheckbox,
                     QStringLiteral("waterfall_3d_monochrome"),
                     QStringLiteral("Monochrome blue 3D"));
    waterfall3DMonochromeCheckbox->setChecked(waterfall3DMonochrome);
    waterfall3DMonochromeCheckbox->setToolTip(uiText(
        QStringLiteral("waterfall_3d_monochrome_tooltip"),
        QStringLiteral("Render the 3D surface in a calm solid blue instead of amplitude colors.")));
    waterfall3DHeightGradientCheckbox = new QCheckBox(
        uiText(QStringLiteral("waterfall_3d_height_gradient"),
               QStringLiteral("Height gradient")),
        this);
    markTranslatable(waterfall3DHeightGradientCheckbox,
                     QStringLiteral("waterfall_3d_height_gradient"),
                     QStringLiteral("Height gradient"));
    waterfall3DHeightGradientCheckbox->setChecked(waterfall3DHeightGradient);
    waterfall3DHeightGradientCheckbox->setToolTip(uiText(
        QStringLiteral("waterfall_3d_height_gradient_tooltip"),
        QStringLiteral("Color the entire 3D geometry by absolute height, from a dark base to the peak color.")));
    waterfall3DMonochromeColorLabel = new QLabel(
        uiText(QStringLiteral("waterfall_3d_monochrome_color"),
               QStringLiteral("3D color:")),
        this);
    markTranslatable(waterfall3DMonochromeColorLabel,
                     QStringLiteral("waterfall_3d_monochrome_color"),
                     QStringLiteral("3D color:"));
    waterfall3DMonochromeColorButton = new QPushButton(this);
    waterfall3DMonochromeColorButton->setMinimumWidth(96);
    const auto updateMonochromeColorButton = [this]() {
        if (!waterfall3DMonochromeColorButton) return;
        const QColor color = waterfall3DMonochromeColor.isValid()
                                 ? waterfall3DMonochromeColor
                                 : QColor(45, 104, 196);
        const QColor textColor = color.lightnessF() > 0.58 ? Qt::black : Qt::white;
        waterfall3DMonochromeColorButton->setText(color.name(QColor::HexRgb).toUpper());
        waterfall3DMonochromeColorButton->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; color: %2; border: 1px solid #707780; padding: 3px 8px; }")
            .arg(color.name(QColor::HexRgb), textColor.name(QColor::HexRgb)));
    };
    updateMonochromeColorButton();
    waterfall3DMonochromeColorButton->setToolTip(uiText(
        QStringLiteral("waterfall_3d_monochrome_color_tooltip"),
        QStringLiteral("Choose the base color used by monochrome 3D rendering.")));
    waterfall3DMonochromeGradientDepthLabel = new QLabel(
        uiText(QStringLiteral("waterfall_3d_monochrome_gradient_depth"),
               QStringLiteral("Gradient depth:")),
        this);
    markTranslatable(waterfall3DMonochromeGradientDepthLabel,
                     QStringLiteral("waterfall_3d_monochrome_gradient_depth"),
                     QStringLiteral("Gradient depth:"));
    waterfall3DMonochromeGradientDepthSlider = new QSlider(Qt::Horizontal, this);
    waterfall3DMonochromeGradientDepthSlider->setRange(0, 100);
    waterfall3DMonochromeGradientDepthSlider->setValue(waterfall3DMonochromeGradientDepth);
    waterfall3DMonochromeGradientDepthSlider->setMinimumWidth(0);
    waterfall3DMonochromeGradientDepthSlider->setSizePolicy(QSizePolicy::Ignored,
                                                             QSizePolicy::Fixed);
    waterfall3DMonochromeGradientDepthSlider->setToolTip(uiText(
        QStringLiteral("waterfall_3d_monochrome_gradient_depth_tooltip"),
        QStringLiteral("Set the brightness range between the base and the tops of monochrome 3D geometry.")));
    waterfall3DMonochromeGradientDepthValueLabel = new QLabel(
        QStringLiteral("%1%").arg(waterfall3DMonochromeGradientDepth), this);
    waterfall3DMonochromeGradientDepthValueLabel->setMinimumWidth(38);
    waterfall3DAlternativeViewCheckbox = new QCheckBox(
        uiText(QStringLiteral("waterfall_3d_alternative_view"),
               QStringLiteral("Alternative view")),
        this);
    waterfall3DAlternativeViewCheckbox->setChecked(waterfall3DAlternativeView);
    waterfall3DAlternativeViewCheckbox->setToolTip(uiText(
        QStringLiteral("waterfall_3d_alternative_view_tooltip"),
        QStringLiteral("Use the fixed front-facing 3D waterfall with the live spectrum at its near edge.")));
    waterfall3DVncSliceInputCheckbox = new QCheckBox(
        uiText(QStringLiteral("waterfall_3d_vnc_slice_input"),
               QStringLiteral("VNC slice control")),
        this);
    waterfall3DVncSliceInputCheckbox->setChecked(waterfall3DVncSliceInput);
    waterfall3DVncSliceInputCheckbox->setToolTip(uiText(
        QStringLiteral("waterfall_3d_vnc_slice_input_tooltip"),
        QStringLiteral("Use left/right mouse buttons for frequency/time slices without keyboard modifiers. Disable it to restore normal tuning, panning and context-menu actions.")));
    alternativeSpectrumGradientCheckbox = new QCheckBox(
        uiText(QStringLiteral("alternative_spectrum_gradient"),
               QStringLiteral("Gradient fill")),
        this);
    markTranslatable(alternativeSpectrumGradientCheckbox,
                     QStringLiteral("alternative_spectrum_gradient"),
                     QStringLiteral("Gradient fill"));
    alternativeSpectrumGradientCheckbox->setChecked(alternativeSpectrumGradientFill);
    alternativeSpectrumGradientCheckbox->setEnabled(
        waterfall3DAlternativeView || waterfall3DFixedPlane);
    alternativeSpectrumGradientCheckbox->setToolTip(uiText(
        QStringLiteral("alternative_spectrum_gradient_tooltip"),
        QStringLiteral("Fill below the spectrum contour in Alternative interface mode, or fill the visible front face of a fixed 3D plane.")));
    QLabel *alternativeSpectrumGradientOpacityLabel = new QLabel(
        uiText(QStringLiteral("alternative_spectrum_gradient_opacity"),
               QStringLiteral("Gradient opacity:")),
        this);
    markTranslatable(alternativeSpectrumGradientOpacityLabel,
                     QStringLiteral("alternative_spectrum_gradient_opacity"),
                     QStringLiteral("Gradient opacity:"));
    alternativeSpectrumGradientOpacitySlider = new QSlider(Qt::Horizontal, this);
    alternativeSpectrumGradientOpacitySlider->setRange(0, 100);
    alternativeSpectrumGradientOpacitySlider->setValue(alternativeSpectrumGradientOpacity);
    alternativeSpectrumGradientOpacitySlider->setEnabled(
        (waterfall3DAlternativeView || waterfall3DFixedPlane) &&
        alternativeSpectrumGradientFill);
    alternativeSpectrumGradientOpacitySlider->setMinimumWidth(150);
    alternativeSpectrumGradientOpacitySlider->setSizePolicy(QSizePolicy::Expanding,
                                                              QSizePolicy::Fixed);
    alternativeSpectrumGradientOpacitySlider->setToolTip(uiText(
        QStringLiteral("alternative_spectrum_gradient_opacity_tooltip"),
        QStringLiteral("Set gradient opacity from transparent to solid.")));
    alternativeSpectrumGradientOpacityValueLabel = new QLabel(
        QStringLiteral("%1%").arg(alternativeSpectrumGradientOpacity), this);
    alternativeSpectrumGradientOpacityValueLabel->setMinimumWidth(38);
    QHBoxLayout *waterfall3DSpectrumSliceRowsLayout = new QHBoxLayout();
    waterfall3DSpectrumSliceRowsLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DSpectrumSliceRowsLayout->addWidget(waterfall3DSpectrumSliceRowsLabel);
    waterfall3DSpectrumSliceRowsLayout->addWidget(waterfall3DSpectrumSliceRowsSpin);
    waterfall3DSpectrumSliceRowsLayout->addStretch(1);
    QHBoxLayout *waterfall3DSpectrumSliceCaptureLayout = new QHBoxLayout();
    waterfall3DSpectrumSliceCaptureLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DSpectrumSliceCaptureLayout->addWidget(waterfall3DSpectrumSliceCaptureCheckbox);
    waterfall3DSpectrumSliceCaptureLayout->addWidget(waterfall3DSpectrumSliceCaptureFixedCheckbox);
    waterfall3DSpectrumSliceCaptureLayout->addStretch(1);
    QHBoxLayout *waterfall3DVncSliceInputLayout = new QHBoxLayout();
    waterfall3DVncSliceInputLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DVncSliceInputLayout->addWidget(waterfall3DAlternativeViewCheckbox);
    waterfall3DVncSliceInputLayout->addWidget(waterfall3DFixedPlaneCheckbox);
    waterfall3DVncSliceInputLayout->addWidget(waterfall3DMonochromeCheckbox);
    waterfall3DVncSliceInputLayout->addWidget(waterfall3DVncSliceInputCheckbox);
    waterfall3DVncSliceInputLayout->addStretch(1);
    QHBoxLayout *waterfall3DGradientLayout = new QHBoxLayout();
    waterfall3DGradientLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DGradientLayout->addWidget(alternativeSpectrumGradientCheckbox);
    waterfall3DGradientLayout->addWidget(waterfall3DHeightGradientCheckbox);
    waterfall3DGradientLayout->addStretch(1);
    QGridLayout *waterfall3DMonochromeStyleLayout = new QGridLayout();
    waterfall3DMonochromeStyleLayout->setContentsMargins(0, 0, 0, 0);
    waterfall3DMonochromeStyleLayout->addWidget(waterfall3DMonochromeColorLabel, 0, 0);
    waterfall3DMonochromeStyleLayout->addWidget(waterfall3DMonochromeColorButton, 0, 1, 1, 2);
    waterfall3DMonochromeStyleLayout->addWidget(waterfall3DMonochromeGradientDepthLabel, 1, 0);
    waterfall3DMonochromeStyleLayout->addWidget(
        waterfall3DMonochromeGradientDepthValueLabel, 1, 1, 1, 2, Qt::AlignRight);
    waterfall3DMonochromeStyleLayout->addWidget(
        waterfall3DMonochromeGradientDepthSlider, 2, 0, 1, 3);
    waterfall3DMonochromeStyleLayout->setColumnStretch(0, 1);
    waterfall3DMonochromeStyleLayout->setColumnStretch(1, 1);
    QGridLayout *alternativeSpectrumGradientOpacityLayout = new QGridLayout();
    alternativeSpectrumGradientOpacityLayout->setContentsMargins(0, 0, 0, 0);
    alternativeSpectrumGradientOpacityLayout->addWidget(alternativeSpectrumGradientOpacityLabel,
                                                         0,
                                                         0);
    alternativeSpectrumGradientOpacityLayout->addWidget(alternativeSpectrumGradientOpacityValueLabel,
                                                         0,
                                                         1,
                                                         Qt::AlignRight);
    alternativeSpectrumGradientOpacityLayout->addWidget(alternativeSpectrumGradientOpacitySlider,
                                                         1,
                                                         0,
                                                         1,
                                                         2);
    alternativeSpectrumGradientOpacityLayout->setColumnStretch(0, 1);
    
    refreshButton = new QPushButton("Refresh USB Devices", this);
    markTranslatable(refreshButton, QStringLiteral("refresh_usb"), QStringLiteral("Refresh USB Devices"));
    fobosButton = new QPushButton("Show Fobos Details", this);
    markTranslatable(fobosButton, QStringLiteral("show_fobos_details"), QStringLiteral("Show Fobos Details"));
    fobosButton->hide();
    networkButton = new QPushButton("Network", this);
    markTranslatable(networkButton, QStringLiteral("network"), QStringLiteral("Network"));
    hackRfSettingsButton = new QPushButton("HackRF RX...", this);
    markTranslatable(hackRfSettingsButton,
                     QStringLiteral("hackrf_rx_settings"),
                     QStringLiteral("HackRF RX..."));
    hackRfSettingsButton->setToolTip(uiText(
        QStringLiteral("hackrf_rx_settings_tooltip"),
        QStringLiteral("RF path, filtering, clock, Opera Cake, sweep and diagnostics")));
    hackRfSettingsButton->hide();
    appSettingsButton = new QPushButton("Settings...", this);
    markTranslatable(appSettingsButton, QStringLiteral("settings"), QStringLiteral("Settings..."));
    dspPathButton = new QPushButton("DSP", this);
    markTranslatable(dspPathButton, QStringLiteral("dsp_path_short"), QStringLiteral("DSP"));
    dspPathButton->setFixedWidth(44);
    dspPathButton->setToolTip("Open the DSP path designer");
    controlsToggleButton = new QPushButton("Cfg", this);
    markTranslatable(controlsToggleButton, QStringLiteral("settings_short"), QStringLiteral("Cfg"));
    controlsToggleButton->setCheckable(true);
    controlsToggleButton->setChecked(true);
    controlsToggleButton->setFixedWidth(44);
    controlsToggleButton->setToolTip("Show, hide, or redock the settings panel");
    controlsToggleButton->installEventFilter(this);
    digitalToggleButton = new QPushButton("Digital\nAudio", this);
    markTranslatable(digitalToggleButton, QStringLiteral("digital_audio_compact"), QStringLiteral("Digital\nAudio"));
    digitalToggleButton->setCheckable(true);
    digitalToggleButton->setMaximumWidth(76);
    digitalToggleButton->setFixedHeight(44);
    digitalToggleButton->setToolTip("Show or hide the digital audio decoder panel");
    videoToggleButton = new QPushButton("Video", this);
    markTranslatable(videoToggleButton, QStringLiteral("video"), QStringLiteral("Video"));
    videoToggleButton->setCheckable(true);
    videoToggleButton->setMaximumWidth(54);
    videoToggleButton->setFixedHeight(44);
    videoToggleButton->setToolTip("Show or hide the video/image decoder panel");
    transmitButton = new QPushButton(QStringLiteral("TX..."), this);
    markTranslatable(transmitButton, QStringLiteral("transmitter"), QStringLiteral("TX..."));
    transmitButton->setMaximumWidth(58);
    transmitButton->setFixedHeight(44);
    transmitButton->setToolTip(uiText(
        QStringLiteral("transmitter_tooltip"),
        QStringLiteral("Open the simulator-first transmitter laboratory; RF output is locked.")));
    recordingModeCombo = new QComboBox(this);
    recordingModeCombo->addItem("Audio WAV", static_cast<int>(RecordingManager::Mode::AudioWav));
    recordingModeCombo->addItem("Channel IQ WAV", static_cast<int>(RecordingManager::Mode::ChannelIqWav));
    recordButton = new QPushButton("Record", this);
    markTranslatable(recordButton, QStringLiteral("record"), QStringLiteral("Record"));
    recordButton->setCheckable(true);
    recordButton->setToolTip("Start/stop recording. Hold F9 for momentary recording.");
    spectrumFrameRecordButton = new QPushButton("Spectrum Rec", this);
    markTranslatable(spectrumFrameRecordButton, QStringLiteral("spectrum_rec"), QStringLiteral("Spectrum Rec"));
    spectrumFrameRecordButton->setCheckable(true);
    spectrumFrameRecordButton->setToolTip(uiText(
        QStringLiteral("spectrum_rec_tooltip"),
        QStringLiteral("Record compact FFT spectrum frames for event replay and measurement.")));
    spectrumFrameReplayButton = new QPushButton("Replay", this);
    markTranslatable(spectrumFrameReplayButton, QStringLiteral("spectrum_replay"), QStringLiteral("Replay"));
    spectrumFrameReplayButton->setToolTip(uiText(
        QStringLiteral("spectrum_replay_tooltip"),
        QStringLiteral("Open a spectrum-frame recording and inspect the waterfall timeline.")));
    spectrumEventModeCombo = new QComboBox(this);
    spectrumEventModeCombo->addItem(uiText(QStringLiteral("spectrum_event_spectrum_only"),
                                           QStringLiteral("Spectrum only")),
                                    0);
    spectrumEventModeCombo->addItem(uiText(QStringLiteral("spectrum_event_channel_iq"),
                                           QStringLiteral("Spectrum + Channel IQ")),
                                    1);
    spectrumEventModeCombo->addItem(uiText(QStringLiteral("spectrum_event_full_iq"),
                                           QStringLiteral("Spectrum + Full IQ short")),
                                    2);
    spectrumEventModeCombo->setToolTip(uiText(
        QStringLiteral("spectrum_event_mode_tooltip"),
        QStringLiteral("Choose what is saved when the spectrum event trigger is pressed.")));
    spectrumFrameBufferCheckbox = new QCheckBox("Buffer", this);
    markTranslatable(spectrumFrameBufferCheckbox, QStringLiteral("spectrum_buffer"), QStringLiteral("Buffer"));
    spectrumFrameBufferCheckbox->setChecked(spectrumFrameBufferEnabled);
    spectrumFrameBufferCheckbox->setToolTip(uiText(
        QStringLiteral("spectrum_buffer_tooltip"),
        QStringLiteral("Keep a rolling pre-trigger spectrum buffer in RAM.")));
    spectrumFrameBinsCombo = new QComboBox(this);
    const QVector<int> spectrumFrameBinOptions = {
        256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
        65536, 131072, 262144, 524288, 1048576, 2097152
    };
    for (int bins : spectrumFrameBinOptions) {
        spectrumFrameBinsCombo->addItem(QString::number(bins), bins);
    }
    spectrumFrameBinsCombo->setCurrentText(QStringLiteral("4096"));
    spectrumFrameBinsCombo->setToolTip(uiText(
        QStringLiteral("spectrum_bins_tooltip"),
        QStringLiteral("Maximum bins stored per spectrum frame. Higher values preserve detail but increase file size.")));
    spectrumFramePrebufferSpin = new QSpinBox(this);
    spectrumFramePrebufferSpin->setRange(1, 120);
    spectrumFramePrebufferSpin->setValue(spectrumFramePrebufferSeconds);
    spectrumFramePrebufferSpin->setSuffix(QStringLiteral(" s"));
    spectrumFramePrebufferSpin->setToolTip(uiText(
        QStringLiteral("spectrum_prebuffer_tooltip"),
        QStringLiteral("Seconds of spectrum frames kept before the hotkey trigger.")));
    recordingStatusLabel = new QLabel(localizedStatusText(QStringLiteral("Recording: idle")), this);
    recordingStatusLabel->setProperty("statusRawText", QStringLiteral("Recording: idle"));
    spectrumFrameRecordingStatusLabel = new QLabel(localizedStatusText(QStringLiteral("Spectrum frames: idle")), this);
    spectrumFrameRecordingStatusLabel->setProperty("statusRawText", QStringLiteral("Spectrum frames: idle"));
    playbackStatusLabel = new QLabel(localizedStatusText(QStringLiteral("Playback: idle")), this);
    playbackStatusLabel->setProperty("statusRawText", QStringLiteral("Playback: idle"));
    auto prepareCompactStatusLabel = [](QLabel *label) {
        if (!label) {
            return;
        }
        label->setMinimumWidth(0);
        label->setMaximumWidth(260);
        label->setWordWrap(false);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    };
    prepareCompactStatusLabel(recordingStatusLabel);
    prepareCompactStatusLabel(spectrumFrameRecordingStatusLabel);
    prepareCompactStatusLabel(playbackStatusLabel);
    recordingStatusLabel->setToolTip(recordingStatusLabel->text());
    spectrumFrameRecordingStatusLabel->setToolTip(spectrumFrameRecordingStatusLabel->text());
    playbackStatusLabel->setToolTip(playbackStatusLabel->text());
    playbackFileCombo = new QComboBox(this);
    playbackFileCombo->setMinimumContentsLength(24);
    playbackRefreshButton = new QPushButton("Refresh Playback", this);
    markTranslatable(playbackRefreshButton, QStringLiteral("refresh_playback"), QStringLiteral("Refresh Playback"));
    playbackButton = new QPushButton("Play", this);
    markTranslatable(playbackButton, QStringLiteral("play"), QStringLiteral("Play"));
    playbackButton->setCheckable(true);
    startButton = new QPushButton("Start", this);
    markTranslatable(startButton, QStringLiteral("start"), QStringLiteral("Start"));
    stopButton = new QPushButton("Stop", this);
    markTranslatable(stopButton, QStringLiteral("stop"), QStringLiteral("Stop"));
    
    contrastSlider = new QSlider(Qt::Horizontal, this);
    contrastSlider->setRange(1, 20);
    contrastSlider->setValue(10);
    contrastLabel = new QLabel(QString("Contrast: %1").arg(contrast), this);
    
    sensitivitySlider = new QSlider(Qt::Horizontal, this);
    sensitivitySlider->setRange(1, 30);
    sensitivitySlider->setValue(10);
    sensitivityLabel = new QLabel(QString("Sensitivity: %1").arg(sensitivity), this);

    levelMinSlider = new QSlider(Qt::Horizontal, this);
    levelMinSlider->setRange(MIN_LEVEL_SLIDER_VALUE, MAX_LEVEL_SLIDER_VALUE);
    levelMinSlider->setValue(levelToSliderValue(displayLevelMin));
    levelMinLabel = new QLabel(levelLabelText("Min", displayLevelMin), this);

    levelMaxSlider = new QSlider(Qt::Horizontal, this);
    levelMaxSlider->setRange(MIN_LEVEL_SLIDER_VALUE, MAX_LEVEL_SLIDER_VALUE);
    levelMaxSlider->setValue(levelToSliderValue(displayLevelMax));
    levelMaxLabel = new QLabel(levelLabelText("Max", displayLevelMax), this);

    auto prepareBottomScaleControl = [](QLabel *label, QSlider *slider, int maxWidth) {
        if (label) {
            label->setAlignment(Qt::AlignCenter);
            label->setWordWrap(false);
        }
        if (slider) {
            slider->setMinimumWidth((std::min)(maxWidth, 80));
            slider->setMaximumWidth(QWIDGETSIZE_MAX);
            slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
    };
    prepareBottomScaleControl(contrastLabel, contrastSlider, 120);
    prepareBottomScaleControl(sensitivityLabel, sensitivitySlider, 120);
    prepareBottomScaleControl(levelMinLabel, levelMinSlider, 150);
    prepareBottomScaleControl(levelMaxLabel, levelMaxSlider, 150);
    if (scaleLabel) {
        scaleLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        scaleLabel->setWordWrap(false);
    }
    if (scaleSlider) {
        scaleSlider->setMinimumWidth(160);
        scaleSlider->setMaximumWidth(QWIDGETSIZE_MAX);
        scaleSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    if (additionalScaleDivisorLabel) {
        additionalScaleDivisorLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        additionalScaleDivisorLabel->setWordWrap(false);
    }
    if (additionalScaleDivisorSlider) {
        additionalScaleDivisorSlider->setMinimumWidth(160);
        additionalScaleDivisorSlider->setMaximumWidth(QWIDGETSIZE_MAX);
        additionalScaleDivisorSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    fineTuneLabel = new QLabel(this);
    fineTuneLabel->setAlignment(Qt::AlignCenter);
    fineTuneLabel->setWordWrap(false);
    fineTuneLabel->setToolTip("Relative fine tuning for the listening frequency");
    fineTuneDial = new QDial(this);
    fineTuneDial->setRange(FINE_TUNE_DIAL_MIN, FINE_TUNE_DIAL_MAX);
    fineTuneDial->setValue(0);
    fineTuneDial->setNotchesVisible(true);
    fineTuneDial->setWrapping(false);
    fineTuneDial->setFixedSize(74, 74);
    fineTuneDial->setToolTip("Drag or wheel to nudge the listening frequency. It returns to center on release.");
    fineTuneScaleWidget = new FineTuneScaleWidget(this);
    fineTuneScaleWidget->setMinimumWidth(180);
    fineTuneScaleModeButton = new QToolButton(this);
    fineTuneScaleModeButton->setCheckable(true);
    fineTuneScaleModeButton->setAutoRaise(true);
    fineTuneScaleModeButton->setFixedSize(18, 18);
    fineTuneScaleModeButton->setToolTip("Fine tune scale mode");
    fineTuneStack = new QStackedWidget(this);
    fineTuneStack->setMinimumWidth(180);
    fineTuneStack->setFixedHeight(58);
    updateFineTuneLabel();
    
    sensLayout->addWidget(sensitivityLabel);
    sensLayout->addWidget(sensitivitySlider);
    contrastLayout->addWidget(contrastLabel);
    contrastLayout->addWidget(contrastSlider);
    scaleControlGrid->addWidget(scaleLabel, 0, 0);
    scaleControlGrid->addWidget(scaleSlider, 0, 1);
    scaleControlGrid->addWidget(additionalScaleDivisorLabel, 1, 0);
    scaleControlGrid->addWidget(additionalScaleDivisorSlider, 1, 1);
    scaleControlGrid->setColumnStretch(0, 0);
    scaleControlGrid->setColumnStretch(1, 1);
    levelMinLayout->addWidget(levelMinLabel);
    levelMinLayout->addWidget(levelMinSlider);
    levelMaxLayout->addWidget(levelMaxLabel);
    levelMaxLayout->addWidget(levelMaxSlider);
    
    QLabel *centralFrequencyLabel = new QLabel("Central Frequency:", this);
    markTranslatable(centralFrequencyLabel, QStringLiteral("central_frequency"), QStringLiteral("Central Frequency:"));
    frequencyControl = new FrequencyControl(this);
    frequencyControl->setRangeHz(0.0, RF_EXPERIMENTAL_MAX_FREQUENCY);
    frequencyControl->setValueHz(100000000.0);
    
    QLabel *listeningFrequencyLabel = new QLabel("Listening Frequency:", this);
    markTranslatable(listeningFrequencyLabel, QStringLiteral("listening_frequency"), QStringLiteral("Listening Frequency:"));
    listeningFrequencyControl = new FrequencyControl(this);
    listeningFrequencyControl->setRangeHz(0.0, RF_EXPERIMENTAL_MAX_FREQUENCY);
    listeningFrequencyControl->setValueHz(100000000.0);

    presetManagerButton = new QPushButton("Presets...", this);
    markTranslatable(presetManagerButton, QStringLiteral("presets"), QStringLiteral("Presets..."));
    zoomSpectrumButton = new QPushButton(QStringLiteral("Zoom spectrum"), this);
    markTranslatable(zoomSpectrumButton,
                     QStringLiteral("zoom_spectrum"),
                     QStringLiteral("Zoom spectrum"));
    zoomSpectrumButton->setToolTip(QString::fromUtf8(u8"Відкрити швидкий вузькосмуговий аналіз для діапазону, виділеного мишею на спектрі"));
    zoomDensityButton = new QPushButton(QStringLiteral("Density"), this);
    markTranslatable(zoomDensityButton,
                     QStringLiteral("zoom_density"),
                     QStringLiteral("Density"));
    zoomDensityButton->setToolTip(QString::fromUtf8(u8"Відкрити окремий графік щільності для смуги, виділеної мишею на спектрі"));
    
    QLabel *clockSourceLabel = new QLabel("Clock:", this);
    markTranslatable(clockSourceLabel, QStringLiteral("clock"), QStringLiteral("Clock:"));
    clockSourceLabel->setToolTip("Receiver clock source");
    QLabel *inputModeLabel = new QLabel("Mode:", this);
    markTranslatable(inputModeLabel, QStringLiteral("mode"), QStringLiteral("Mode:"));
    inputModeLabel->setToolTip("Receiver input mode");
    QLabel *sampleRateLabel = new QLabel("Sample:", this);
    markTranslatable(sampleRateLabel, QStringLiteral("sample"), QStringLiteral("Sample:"));
    sampleRateLabel->setToolTip("ADC sample rate");
    QLabel *fftLabel = new QLabel("FFT:", this);
    markTranslatable(fftLabel, QStringLiteral("fft"), QStringLiteral("FFT:"));
    fftLabel->setToolTip("Spectrum FFT length");

    QGroupBox *agileScanBox = new QGroupBox("Agile scan", this);
    markTranslatable(agileScanBox, QStringLiteral("agile_scan"), QStringLiteral("Agile scan"));
    QGroupBox *spectrumMeasurementBox = new QGroupBox("Spectrum measurement", this);
    markTranslatable(spectrumMeasurementBox, QStringLiteral("spectrum_measurement"), QStringLiteral("Spectrum measurement"));
    agileScanCheckbox = new QCheckBox("Enable scan", agileScanBox);
    markTranslatable(agileScanCheckbox, QStringLiteral("enable_scan"), QStringLiteral("Enable scan"));
    agileScanPresetCombo = new QComboBox(agileScanBox);
    agileScanPresetCombo->setEditable(true);
    agileScanPresetCombo->setMinimumContentsLength(8);
    agileScanPresetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    agileScanPresetCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    agileScanRangesEdit = new QLineEdit(agileScanRangesMhz, agileScanBox);
    agileScanRangesEdit->setPlaceholderText(QStringLiteral("430-470, 600-900\\1100-1300"));
    agileScanRangesEdit->setMinimumWidth(0);
    agileScanRangesEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    agileScanStepSpin = new QDoubleSpinBox(agileScanBox);
    agileScanStepSpin->setRange(AGILE_SCAN_MIN_STEP_MHZ, AGILE_SCAN_MAX_STEP_MHZ);
    agileScanStepSpin->setDecimals(4);
    agileScanStepSpin->setSingleStep(0.0125);
    agileScanStepSpin->setSuffix(QStringLiteral(" MHz"));
    agileScanStepSpin->setValue(agileScanStepMhz);
    agileScanStepSpin->setMaximumWidth(104);
    agileScanAutoStepCheckbox = new QCheckBox("Step = SR", agileScanBox);
    markTranslatable(agileScanAutoStepCheckbox,
                     QStringLiteral("agile_auto_step"),
                     QStringLiteral("Step = SR"));
    agileScanAutoStepCheckbox->setToolTip(uiText(
        QStringLiteral("agile_auto_step_tooltip"),
        QStringLiteral("Use the current sample rate in MHz as the Agile scan step.")));
    scanVisualModeCombo = new QComboBox(this);
    scanVisualModeCombo->addItem(uiText(QStringLiteral("scan_visual_compressed"),
                                        QStringLiteral("Compressed/Mosaic")),
                                 static_cast<int>(ScanVisualMode::CompressedMosaic));
    scanVisualModeCombo->addItem(uiText(QStringLiteral("scan_visual_true_axis"),
                                        QStringLiteral("Floating/True axis")),
                                 static_cast<int>(ScanVisualMode::FloatingTrueAxis));
    scanVisualModeCombo->addItem(uiText(QStringLiteral("scan_visual_pass_composite"),
                                        QStringLiteral("Pass composite")),
                                 static_cast<int>(ScanVisualMode::PassComposite));
    scanVisualModeCombo->setMinimumContentsLength(14);
    scanVisualModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    scanVisualModeCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    scanVisualModeCombo->setToolTip(uiText(
        QStringLiteral("scan_visual_mode_tooltip"),
        QStringLiteral("Choose how scan centers are stitched on the spectrum and waterfall.")));
    {
        const int index = scanVisualModeCombo->findData(normalizedScanVisualMode(scanVisualMode));
        scanVisualModeCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    scanMeasurementCheckbox = new QCheckBox("Measure spectrum", spectrumMeasurementBox);
    markTranslatable(scanMeasurementCheckbox, QStringLiteral("measure"), QStringLiteral("Measure spectrum"));
    scanMeasurementCheckbox->setToolTip(uiText(
        QStringLiteral("measure_tooltip"),
        QStringLiteral("Collect current, peak-hold, baseline and delta values for spectrum coverage checks")));
    scanMeasurementCheckbox->setChecked(scanMeasurementEnabled);
    scanMeasurementBinSpin = new QDoubleSpinBox(spectrumMeasurementBox);
    scanMeasurementBinSpin->setRange(SCAN_MEASUREMENT_MIN_BIN_MHZ, SCAN_MEASUREMENT_MAX_BIN_MHZ);
    scanMeasurementBinSpin->setDecimals(3);
    scanMeasurementBinSpin->setSingleStep(0.1);
    scanMeasurementBinSpin->setSuffix(QStringLiteral(" MHz"));
    scanMeasurementBinSpin->setValue(scanMeasurementBinMhz);
    scanMeasurementBaselineButton = new QPushButton("BG Rec", spectrumMeasurementBox);
    markTranslatable(scanMeasurementBaselineButton, QStringLiteral("bg_rec"), QStringLiteral("BG Rec"));
    scanMeasurementBaselineButton->setCheckable(true);
    scanMeasurementBaselineButton->setToolTip(uiText(
        QStringLiteral("bg_rec_tooltip"),
        QStringLiteral("Record baseline while the source under test is off")));
    scanMeasurementResetPeakButton = new QPushButton("Reset Peak", spectrumMeasurementBox);
    markTranslatable(scanMeasurementResetPeakButton, QStringLiteral("reset_peak"), QStringLiteral("Reset Peak"));
    scanMeasurementResetPeakButton->setToolTip(uiText(
        QStringLiteral("reset_peak_tooltip"),
        QStringLiteral("Clear peak-hold values without clearing baseline")));
    scanMeasurementExportButton = new QPushButton("CSV", spectrumMeasurementBox);
    scanMeasurementExportButton->setToolTip(uiText(QStringLiteral("csv_tooltip"),
                                                   QStringLiteral("Export spectrum measurement bins to CSV")));
    spectrumScienceMarkerCombo = new QComboBox(spectrumMeasurementBox);
    spectrumScienceMarkerCombo->addItem(QStringLiteral("A"), 0);
    spectrumScienceMarkerCombo->addItem(QStringLiteral("B"), 1);
    spectrumScienceMarkerCombo->setMaximumWidth(48);
    auto *addScienceMarker = new QPushButton(QStringLiteral("+"), spectrumMeasurementBox);
    auto *removeScienceMarker = new QPushButton(QStringLiteral("-"), spectrumMeasurementBox);
    addScienceMarker->setMaximumWidth(28);
    removeScienceMarker->setMaximumWidth(28);
    addScienceMarker->setToolTip(uiText("science_marker_add", "Add marker"));
    removeScienceMarker->setToolTip(uiText("science_marker_remove", "Remove selected marker (A/B are retained)"));
    connect(addScienceMarker, &QPushButton::clicked, this, [this]() {
        const int index = spectrumScienceAnalyzer.addMarker();
        if (index < 0) return;
        spectrumScienceMarkerCombo->addItem(spectrumScienceAnalyzer.marker(index).label, index);
        spectrumScienceMarkerCombo->setCurrentIndex(index);
    });
    connect(removeScienceMarker, &QPushButton::clicked, this, [this]() {
        const int index = spectrumScienceMarkerCombo->currentIndex();
        if (!spectrumScienceAnalyzer.removeMarker(index)) return;
        const QSignalBlocker blocker(spectrumScienceMarkerCombo);
        spectrumScienceMarkerCombo->clear();
        for (int i = 0; i < spectrumScienceAnalyzer.markerCount(); ++i)
            spectrumScienceMarkerCombo->addItem(spectrumScienceAnalyzer.marker(i).label, i);
        spectrumScienceActiveMarker = std::min(index, spectrumScienceAnalyzer.markerCount() - 1);
        spectrumScienceMarkerCombo->setCurrentIndex(spectrumScienceActiveMarker);
        updateSpectrumScienceUi();
    });
    spectrumScienceSetButton = new QPushButton("Set", spectrumMeasurementBox);
    markTranslatable(spectrumScienceSetButton, QStringLiteral("science_set"), QStringLiteral("Set"));
    spectrumScienceSetButton->setToolTip(uiText(
        QStringLiteral("science_set_tooltip"),
        QStringLiteral("Set the active A/B marker to the listening frequency. Shift-click on the spectrum also sets it.")));
    spectrumSciencePeakButton = new QPushButton("Peak", spectrumMeasurementBox);
    markTranslatable(spectrumSciencePeakButton, QStringLiteral("science_peak"), QStringLiteral("Peak"));
    spectrumSciencePreviousButton = new QPushButton("Prev", spectrumMeasurementBox);
    markTranslatable(spectrumSciencePreviousButton, QStringLiteral("previous_short"), QStringLiteral("Prev"));
    spectrumSciencePreviousButton->setToolTip(uiText(QStringLiteral("science_previous_peak_tooltip"),
                                                      QStringLiteral("Move the active marker to the previous local peak")));
    spectrumScienceNextButton = new QPushButton("Next", spectrumMeasurementBox);
    markTranslatable(spectrumScienceNextButton, QStringLiteral("next_short"), QStringLiteral("Next"));
    spectrumScienceNextButton->setToolTip(uiText(QStringLiteral("science_next_peak_tooltip"),
                                                  QStringLiteral("Move the active marker to the next local peak")));
    spectrumScienceClearButton = new QPushButton("Clear", spectrumMeasurementBox);
    markTranslatable(spectrumScienceClearButton, QStringLiteral("clear"), QStringLiteral("Clear"));
    spectrumScienceMaxHoldCheckbox = new QCheckBox("Max", spectrumMeasurementBox);
    markTranslatable(spectrumScienceMaxHoldCheckbox, QStringLiteral("science_max_hold"), QStringLiteral("Max"));
    spectrumScienceMaxHoldCheckbox->setToolTip(uiText(QStringLiteral("science_max_hold_tooltip"), QStringLiteral("Show the maximum level retained at every visible frequency")));
    spectrumScienceMaxHoldCheckbox->setChecked(spectrumScienceMaxHoldEnabled);
    spectrumScienceMinHoldCheckbox = new QCheckBox("Min", spectrumMeasurementBox);
    markTranslatable(spectrumScienceMinHoldCheckbox, QStringLiteral("science_min_hold"), QStringLiteral("Min"));
    spectrumScienceMinHoldCheckbox->setToolTip(uiText(QStringLiteral("science_min_hold_tooltip"), QStringLiteral("Show the minimum level retained at every visible frequency")));
    spectrumScienceMinHoldCheckbox->setChecked(spectrumScienceMinHoldEnabled);
    spectrumScienceAverageCheckbox = new QCheckBox("Avg", spectrumMeasurementBox);
    markTranslatable(spectrumScienceAverageCheckbox, QStringLiteral("science_average"), QStringLiteral("Avg"));
    spectrumScienceAverageCheckbox->setToolTip(uiText(QStringLiteral("science_average_tooltip"), QStringLiteral("Show an exponential average calculated in linear power")));
    spectrumScienceAverageCheckbox->setChecked(spectrumScienceAverageEnabled);
    spectrumScienceAverageSpin = new QDoubleSpinBox(spectrumMeasurementBox);
    spectrumScienceAverageSpin->setRange(0.05, 60.0);
    spectrumScienceAverageSpin->setDecimals(2);
    spectrumScienceAverageSpin->setSingleStep(0.25);
    spectrumScienceAverageSpin->setSuffix(QStringLiteral(" s"));
    spectrumScienceAverageSpin->setValue(spectrumScienceAverageSeconds);
    spectrumScienceAverageSpin->setMaximumWidth(74);
    spectrumScienceResetButton = new QPushButton("Reset", spectrumMeasurementBox);
    markTranslatable(spectrumScienceResetButton, QStringLiteral("reset"), QStringLiteral("Reset"));
    spectrumScienceResetButton->setToolTip(uiText(QStringLiteral("science_reset_tooltip"),
                                                   QStringLiteral("Reset Max/Min hold and linear-power average traces")));
    spectrumScienceExportButton = new QPushButton("Report", spectrumMeasurementBox);
    markTranslatable(spectrumScienceExportButton, QStringLiteral("science_report"), QStringLiteral("Report"));
    spectrumScienceExportButton->setToolTip(uiText(QStringLiteral("science_report_tooltip"),
                                                    QStringLiteral("Export the current spectrum, traces, markers and metrics to CSV")));
    zeroSpanButton = new QPushButton("Zero Span...", spectrumMeasurementBox);
    markTranslatable(zeroSpanButton, QStringLiteral("zero_span_open"), QStringLiteral("Zero Span..."));
    zeroSpanButton->setToolTip(uiText(QStringLiteral("zero_span_open_tooltip"),
                                      QStringLiteral("Open time-domain level analysis at one selected frequency")));
    researchToolsButton = new QPushButton("Research...", spectrumMeasurementBox);
    markTranslatable(researchToolsButton,
                     QStringLiteral("research_tools_open"),
                     QStringLiteral("Research..."));
    researchToolsButton->setToolTip(uiText(
        QStringLiteral("research_tools_open_tooltip"),
        QStringLiteral("Open interference, statistics, IQ and dual-input analysis tools.")));
    waterfallAreaMeasurementCheckbox = new QCheckBox("Waterfall ruler", spectrumMeasurementBox);
    markTranslatable(waterfallAreaMeasurementCheckbox,
                     QStringLiteral("waterfall_area_measurement"),
                     QStringLiteral("Waterfall ruler"));
    waterfallAreaMeasurementCheckbox->setToolTip(uiText(
        QStringLiteral("waterfall_area_measurement_tooltip"),
        QStringLiteral("Enable a 2D waterfall selection tool that measures frequency span and elapsed time.")));
    waterfallAreaMeasurementCheckbox->setChecked(waterfallAreaMeasurementEnabled);
    spurSuppressionCheckbox = new QCheckBox("Spur", spectrumMeasurementBox);
    markTranslatable(spurSuppressionCheckbox, QStringLiteral("spur"), QStringLiteral("Spur"));
    spurSuppressionCheckbox->setToolTip(uiText(
        QStringLiteral("spur_tooltip"),
        QStringLiteral("Suppress calibrated internal receiver spurs in spectrum, waterfall and spectrum measurements")));
    spurSuppressionCheckbox->setChecked(spurSuppressionEnabled);
    spurCalibrateButton = new QPushButton("Cal", spectrumMeasurementBox);
    markTranslatable(spurCalibrateButton, QStringLiteral("cal"), QStringLiteral("Cal"));
    spurCalibrateButton->setToolTip(uiText(
        QStringLiteral("cal_tooltip"),
        QStringLiteral("Calibrate stable narrow spurs with a 50 ohm load connected")));
    spurClearButton = new QPushButton("Clear", spectrumMeasurementBox);
    markTranslatable(spurClearButton, QStringLiteral("clear"), QStringLiteral("Clear"));
    spurClearButton->setToolTip(uiText(QStringLiteral("clear_spur_tooltip"),
                                       QStringLiteral("Clear calibrated spur mask")));
    agileScanSavePresetButton = new QPushButton("Save", agileScanBox);
    markTranslatable(agileScanSavePresetButton, QStringLiteral("save_short"), QStringLiteral("Save"));
    agileScanSavePresetButton->setToolTip(uiText(QStringLiteral("save"), QStringLiteral("Save")));
    agileScanDeletePresetButton = new QPushButton("Del", agileScanBox);
    markTranslatable(agileScanDeletePresetButton, QStringLiteral("delete_short"), QStringLiteral("Del"));
    agileScanStatusLabel = new QLabel(uiText(QStringLiteral("agile_scan_off"),
                                             QStringLiteral("Agile scan: off")), agileScanBox);
    agileScanStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    agileScanStatusLabel->setMinimumWidth(0);
    agileScanStatusLabel->setWordWrap(false);
    agileScanStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    scanMeasurementStatusLabel = new QLabel(uiText(QStringLiteral("scan_measurement_idle"),
                                                   QStringLiteral("Spectrum measurement: idle")), spectrumMeasurementBox);
    scanMeasurementStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    scanMeasurementStatusLabel->setMinimumWidth(0);
    scanMeasurementStatusLabel->setWordWrap(false);
    scanMeasurementStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    spectrumScienceMarkerStatusLabel = new QLabel(QStringLiteral("A -- | B --"), spectrumMeasurementBox);
    spectrumScienceMarkerStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    spectrumScienceMarkerStatusLabel->setMinimumWidth(0);
    spectrumScienceMarkerStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    spectrumScienceMetricsLabel = new QLabel(uiText(QStringLiteral("science_waiting"),
                                                     QStringLiteral("Scientific analysis: waiting for spectrum")),
                                               spectrumMeasurementBox);
    spectrumScienceMetricsLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    spectrumScienceMetricsLabel->setMinimumWidth(0);
    spectrumScienceMetricsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    spurSuppressionStatusLabel = new QLabel(uiText(QStringLiteral("spur_mask_off"),
                                                   QStringLiteral("Spur mask: off")), spectrumMeasurementBox);
    spurSuppressionStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    spurSuppressionStatusLabel->setMinimumWidth(0);
    spurSuppressionStatusLabel->setWordWrap(false);
    spurSuppressionStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QVBoxLayout *agileScanLayout = new QVBoxLayout(agileScanBox);
    QHBoxLayout *agileScanPresetLayout = new QHBoxLayout();
    QHBoxLayout *agileScanPresetButtonLayout = new QHBoxLayout();
    QHBoxLayout *agileScanRangeLayout = new QHBoxLayout();
    QHBoxLayout *scanVisualModeLayout = new QHBoxLayout();
    QVBoxLayout *spectrumMeasurementLayout = new QVBoxLayout(spectrumMeasurementBox);
    QVBoxLayout *scanMeasurementPanelLayout = new QVBoxLayout();
    QHBoxLayout *scanMeasurementTopLayout = new QHBoxLayout();
    QHBoxLayout *scanMeasurementButtonLayout = new QHBoxLayout();
    QVBoxLayout *spectrumSciencePanelLayout = new QVBoxLayout();
    QHBoxLayout *spectrumScienceMarkerLayout = new QHBoxLayout();
    QHBoxLayout *spectrumScienceMarkerNavigationLayout = new QHBoxLayout();
    QHBoxLayout *spectrumScienceTraceLayout = new QHBoxLayout();
    QGridLayout *spectrumScienceActionLayout = new QGridLayout();
    QVBoxLayout *spurSuppressionPanelLayout = new QVBoxLayout();
    QHBoxLayout *spurSuppressionLayout = new QHBoxLayout();
    agileScanPresetLayout->addWidget(agileScanCheckbox);
    agileScanPresetLayout->addWidget(agileScanPresetCombo, 1);
    QLabel *agileScanRangeLabel = new QLabel("Ranges MHz:", agileScanBox);
    markTranslatable(agileScanRangeLabel, QStringLiteral("ranges_mhz"), QStringLiteral("Ranges MHz:"));
    QLabel *agileScanStepLabel = new QLabel("Step:", agileScanBox);
    markTranslatable(agileScanStepLabel, QStringLiteral("step"), QStringLiteral("Step:"));
    QLabel *scanVisualModeLabel = new QLabel("Scan visual:", this);
    markTranslatable(scanVisualModeLabel, QStringLiteral("scan_visual_mode"), QStringLiteral("Scan visual:"));
    agileScanPresetButtonLayout->setContentsMargins(0, 0, 0, 0);
    agileScanPresetButtonLayout->setSpacing(4);
    agileScanPresetButtonLayout->addWidget(agileScanSavePresetButton);
    agileScanPresetButtonLayout->addWidget(agileScanDeletePresetButton);
    agileScanPresetButtonLayout->addWidget(agileScanAutoStepCheckbox);
    agileScanPresetButtonLayout->addStretch(1);
    agileScanPresetButtonLayout->addWidget(agileScanStepLabel);
    agileScanPresetButtonLayout->addWidget(agileScanStepSpin);
    agileScanRangeLayout->addWidget(agileScanRangeLabel);
    agileScanRangeLayout->addWidget(agileScanRangesEdit, 1);
    scanVisualModeLayout->setContentsMargins(0, 0, 0, 0);
    scanVisualModeLayout->setSpacing(4);
    scanVisualModeLayout->addWidget(scanVisualModeLabel);
    scanVisualModeLayout->addWidget(scanVisualModeCombo, 1);
    QLabel *scanMeasurementBinLabel = new QLabel("Bin:", spectrumMeasurementBox);
    markTranslatable(scanMeasurementBinLabel, QStringLiteral("bin"), QStringLiteral("Bin:"));
    scanMeasurementBinSpin->setMaximumWidth(92);
    scanMeasurementBaselineButton->setMaximumWidth(68);
    scanMeasurementResetPeakButton->setMaximumWidth(86);
    scanMeasurementExportButton->setMaximumWidth(44);
    agileScanSavePresetButton->setMaximumWidth(52);
    agileScanDeletePresetButton->setMaximumWidth(42);
    agileScanSavePresetButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    agileScanDeletePresetButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    spurCalibrateButton->setMaximumWidth(44);
    spurClearButton->setMaximumWidth(54);

    scanMeasurementTopLayout->setContentsMargins(0, 0, 0, 0);
    scanMeasurementTopLayout->setSpacing(4);
    scanMeasurementTopLayout->addWidget(scanMeasurementCheckbox);
    scanMeasurementTopLayout->addWidget(scanMeasurementBinLabel);
    scanMeasurementTopLayout->addWidget(scanMeasurementBinSpin);
    scanMeasurementTopLayout->addStretch(1);
    scanMeasurementButtonLayout->setContentsMargins(0, 0, 0, 0);
    scanMeasurementButtonLayout->setSpacing(4);
    scanMeasurementButtonLayout->addWidget(scanMeasurementBaselineButton);
    scanMeasurementButtonLayout->addWidget(scanMeasurementResetPeakButton);
    scanMeasurementButtonLayout->addWidget(scanMeasurementExportButton);
    scanMeasurementButtonLayout->addStretch(1);
    scanMeasurementPanelLayout->setContentsMargins(0, 0, 0, 0);
    scanMeasurementPanelLayout->setSpacing(2);
    scanMeasurementPanelLayout->addLayout(scanMeasurementTopLayout);
    scanMeasurementPanelLayout->addLayout(scanMeasurementButtonLayout);
    scanMeasurementPanelLayout->addWidget(scanMeasurementStatusLabel);
    spectrumMeasurementLayout->addLayout(scanMeasurementPanelLayout);

    QLabel *scienceMarkerLabel = new QLabel("Marker:", spectrumMeasurementBox);
    markTranslatable(scienceMarkerLabel, QStringLiteral("science_marker"), QStringLiteral("Marker:"));
    spectrumScienceSetButton->setMaximumWidth(48);
    spectrumSciencePeakButton->setMaximumWidth(52);
    spectrumSciencePreviousButton->setMaximumWidth(72);
    spectrumScienceNextButton->setMaximumWidth(72);
    spectrumScienceClearButton->setMaximumWidth(54);
    spectrumScienceMarkerLayout->setContentsMargins(0, 4, 0, 0);
    spectrumScienceMarkerLayout->setSpacing(4);
    spectrumScienceMarkerLayout->addWidget(scienceMarkerLabel);
    spectrumScienceMarkerLayout->addWidget(spectrumScienceMarkerCombo);
    spectrumScienceMarkerLayout->addWidget(addScienceMarker);
    spectrumScienceMarkerLayout->addWidget(removeScienceMarker);
    spectrumScienceMarkerLayout->addWidget(spectrumScienceSetButton);
    spectrumScienceMarkerLayout->addWidget(spectrumSciencePeakButton);
    spectrumScienceMarkerLayout->addStretch(1);
    spectrumScienceMarkerNavigationLayout->setContentsMargins(0, 0, 0, 0);
    spectrumScienceMarkerNavigationLayout->setSpacing(4);
    spectrumScienceMarkerNavigationLayout->addWidget(spectrumSciencePreviousButton);
    spectrumScienceMarkerNavigationLayout->addWidget(spectrumScienceNextButton);
    spectrumScienceMarkerNavigationLayout->addWidget(spectrumScienceClearButton);
    spectrumScienceMarkerNavigationLayout->addStretch(1);
    spectrumScienceTraceLayout->setContentsMargins(0, 0, 0, 0);
    spectrumScienceTraceLayout->setSpacing(4);
    spectrumScienceTraceLayout->addWidget(spectrumScienceMaxHoldCheckbox);
    spectrumScienceTraceLayout->addWidget(spectrumScienceMinHoldCheckbox);
    spectrumScienceTraceLayout->addWidget(spectrumScienceAverageCheckbox);
    spectrumScienceTraceLayout->addWidget(spectrumScienceAverageSpin);
    spectrumScienceTraceLayout->addStretch(1);
    spectrumScienceActionLayout->setContentsMargins(0, 0, 0, 0);
    spectrumScienceActionLayout->setSpacing(4);
    spectrumScienceActionLayout->addWidget(spectrumScienceResetButton, 0, 0);
    spectrumScienceActionLayout->addWidget(spectrumScienceExportButton, 0, 1);
    spectrumScienceActionLayout->addWidget(zeroSpanButton, 1, 0);
    spectrumScienceActionLayout->addWidget(researchToolsButton, 1, 1);
    spectrumScienceActionLayout->addWidget(waterfallAreaMeasurementCheckbox, 2, 0, 1, 2);
    auto *wirelessLabButton = new QPushButton("Wireless lab (beta)...", spectrumMeasurementBox);
    markTranslatable(wirelessLabButton, QStringLiteral("wireless_lab"), QStringLiteral("Wireless lab (beta)..."));
    spectrumScienceActionLayout->addWidget(wirelessLabButton, 3, 0, 1, 2);
    connect(wirelessLabButton, &QPushButton::clicked, this, &YourClassName::openWirelessLab);
    auto *propagationButton = new QPushButton("Radio propagation...", spectrumMeasurementBox);
    markTranslatable(propagationButton, QStringLiteral("radio_propagation"), QStringLiteral("Radio propagation..."));
    spectrumScienceActionLayout->addWidget(propagationButton, 4, 0, 1, 2);
    connect(propagationButton, &QPushButton::clicked, this, &YourClassName::openPropagationLab);
    spectrumScienceActionLayout->setColumnStretch(0, 1);
    spectrumScienceActionLayout->setColumnStretch(1, 1);
    spectrumSciencePanelLayout->setContentsMargins(0, 0, 0, 0);
    spectrumSciencePanelLayout->setSpacing(2);
    spectrumSciencePanelLayout->addLayout(spectrumScienceMarkerLayout);
    spectrumSciencePanelLayout->addLayout(spectrumScienceMarkerNavigationLayout);
    spectrumSciencePanelLayout->addLayout(spectrumScienceTraceLayout);
    spectrumSciencePanelLayout->addLayout(spectrumScienceActionLayout);
    spectrumSciencePanelLayout->addWidget(spectrumScienceMarkerStatusLabel);
    spectrumSciencePanelLayout->addWidget(spectrumScienceMetricsLabel);
    spectrumMeasurementLayout->addLayout(spectrumSciencePanelLayout);

    spurSuppressionLayout->setContentsMargins(0, 0, 0, 0);
    spurSuppressionLayout->setSpacing(4);
    spurSuppressionLayout->addWidget(spurSuppressionCheckbox);
    spurSuppressionLayout->addWidget(spurCalibrateButton);
    spurSuppressionLayout->addWidget(spurClearButton);
    spurSuppressionLayout->addStretch(1);
    spurSuppressionPanelLayout->setContentsMargins(0, 0, 0, 0);
    spurSuppressionPanelLayout->setSpacing(2);
    spurSuppressionPanelLayout->addLayout(spurSuppressionLayout);
    spurSuppressionPanelLayout->addWidget(spurSuppressionStatusLabel);
    spectrumMeasurementLayout->addLayout(spurSuppressionPanelLayout);

    agileScanLayout->addLayout(agileScanPresetLayout);
    agileScanLayout->addLayout(agileScanPresetButtonLayout);
    agileScanLayout->addLayout(agileScanRangeLayout);
    agileScanLayout->addWidget(agileScanStatusLabel);

    QGroupBox *standardScanBox = new QGroupBox("Standard scan", this);
    markTranslatable(standardScanBox, QStringLiteral("standard_scan"), QStringLiteral("Standard scan"));
    standardScanCheckbox = new QCheckBox("Enable standard scan", standardScanBox);
    markTranslatable(standardScanCheckbox,
                     QStringLiteral("enable_standard_scan"),
                     QStringLiteral("Enable standard scan"));
    standardScanCheckbox->setToolTip(uiText(
        QStringLiteral("standard_scan_tooltip"),
        QStringLiteral("Slow manual retune scan by cycling through listed center frequencies")));
    standardScanCheckbox->setChecked(standardScanEnabled);
    scanListeningLockCheckbox = new QCheckBox("Lock listening frequency", standardScanBox);
    markTranslatable(scanListeningLockCheckbox,
                     QStringLiteral("scan_lock_listen"),
                     QStringLiteral("Lock listening frequency"));
    scanListeningLockCheckbox->setToolTip(uiText(
        QStringLiteral("scan_lock_listen_tooltip"),
        QStringLiteral("Keep listening frequency and marker fixed while scan retunes between centers")));
    scanListeningLockCheckbox->setChecked(scanListeningLockEnabled);
    standardScanPresetCombo = new QComboBox(standardScanBox);
    standardScanPresetCombo->setEditable(true);
    standardScanPresetCombo->setInsertPolicy(QComboBox::NoInsert);
    standardScanPresetCombo->setMinimumWidth(150);
    standardScanPresetCombo->setToolTip(uiText(
        QStringLiteral("standard_scan_preset_tooltip"),
        QStringLiteral("Saved standard-scan presets with centers, dwell and settle time")));
    standardScanSavePresetButton = new QPushButton(uiText(QStringLiteral("save"), QStringLiteral("Save")), standardScanBox);
    standardScanSavePresetButton->setMaximumWidth(58);
    standardScanDeletePresetButton = new QPushButton(uiText(QStringLiteral("delete_short"), QStringLiteral("Del")), standardScanBox);
    standardScanDeletePresetButton->setMaximumWidth(48);
    standardScanDwellSpin = new QSpinBox(standardScanBox);
    standardScanDwellSpin->setRange(STANDARD_SCAN_MIN_DWELL_MS, STANDARD_SCAN_MAX_DWELL_MS);
    standardScanDwellSpin->setSingleStep(50);
    standardScanDwellSpin->setSuffix(QStringLiteral(" ms"));
    standardScanDwellSpin->setValue(standardScanDwellMs);
    standardScanDwellSpin->setMaximumWidth(92);
    standardScanDwellSpin->setToolTip(uiText(
        QStringLiteral("standard_scan_dwell_tooltip"),
        QStringLiteral("How long to stay on each standard-scan center before retuning")));
    standardScanSettleSpin = new QSpinBox(standardScanBox);
    standardScanSettleSpin->setRange(STANDARD_SCAN_MIN_SETTLE_MS, STANDARD_SCAN_MAX_SETTLE_MS);
    standardScanSettleSpin->setSingleStep(10);
    standardScanSettleSpin->setSuffix(QStringLiteral(" ms"));
    standardScanSettleSpin->setValue(standardScanSettleMs);
    standardScanSettleSpin->setMaximumWidth(92);
    standardScanSettleSpin->setToolTip(uiText(
        QStringLiteral("standard_scan_settle_tooltip"),
        QStringLiteral("How long to discard IQ after each retune before drawing the next scan center")));
    standardScanCentersEdit = new QLineEdit(standardScanCentersMhz, standardScanBox);
    standardScanCentersEdit->setPlaceholderText(QStringLiteral("430, 480, 530"));
    standardScanCentersEdit->setToolTip(uiText(
        QStringLiteral("standard_scan_centers_tooltip"),
        QStringLiteral("Comma-separated center frequencies in MHz. Adjacent centers must be at least one sample rate apart.")));
    standardScanRangeStartEdit = new QLineEdit(standardScanRangeStartMhz, standardScanBox);
    standardScanRangeStartEdit->setPlaceholderText(QStringLiteral("100"));
    standardScanRangeStartEdit->setMaximumWidth(80);
    standardScanRangeStartEdit->setToolTip(uiText(
        QStringLiteral("standard_scan_range_start_tooltip"),
        QStringLiteral("First scan center in MHz for auto-filled center list")));
    standardScanRangeEndEdit = new QLineEdit(standardScanRangeEndMhz, standardScanBox);
    standardScanRangeEndEdit->setPlaceholderText(QStringLiteral("300"));
    standardScanRangeEndEdit->setMaximumWidth(80);
    standardScanRangeEndEdit->setToolTip(uiText(
        QStringLiteral("standard_scan_range_end_tooltip"),
        QStringLiteral("Last scan center in MHz for auto-filled center list")));
    standardScanRemoveLowerButton = new QPushButton("Rm-", standardScanBox);
    markTranslatable(standardScanRemoveLowerButton,
                     QStringLiteral("standard_scan_remove_lower"),
                     QStringLiteral("Rm-"));
    standardScanRemoveLowerButton->setToolTip(uiText(
        QStringLiteral("standard_scan_remove_lower_tooltip"),
        QStringLiteral("Remove the lowest center from the scan list")));
    standardScanRemoveLowerButton->setMaximumWidth(48);
    standardScanAddLowerButton = new QPushButton("-SR", standardScanBox);
    markTranslatable(standardScanAddLowerButton,
                     QStringLiteral("standard_scan_add_lower"),
                     QStringLiteral("-SR"));
    standardScanAddLowerButton->setToolTip(uiText(
        QStringLiteral("standard_scan_add_lower_tooltip"),
        QStringLiteral("Add one lower center using the current sample rate step")));
    standardScanAddLowerButton->setMaximumWidth(48);
    standardScanAddUpperButton = new QPushButton("+SR", standardScanBox);
    markTranslatable(standardScanAddUpperButton,
                     QStringLiteral("standard_scan_add_upper"),
                     QStringLiteral("+SR"));
    standardScanAddUpperButton->setToolTip(uiText(
        QStringLiteral("standard_scan_add_upper_tooltip"),
        QStringLiteral("Add one higher center using the current sample rate step")));
    standardScanAddUpperButton->setMaximumWidth(48);
    standardScanRemoveUpperButton = new QPushButton("Rm+", standardScanBox);
    markTranslatable(standardScanRemoveUpperButton,
                     QStringLiteral("standard_scan_remove_upper"),
                     QStringLiteral("Rm+"));
    standardScanRemoveUpperButton->setToolTip(uiText(
        QStringLiteral("standard_scan_remove_upper_tooltip"),
        QStringLiteral("Remove the highest center from the scan list")));
    standardScanRemoveUpperButton->setMaximumWidth(48);
    standardScanFillRangeButton = new QPushButton("Fill", standardScanBox);
    markTranslatable(standardScanFillRangeButton,
                     QStringLiteral("standard_scan_fill_range"),
                     QStringLiteral("Fill"));
    standardScanFillRangeButton->setToolTip(uiText(
        QStringLiteral("standard_scan_fill_range_tooltip"),
        QStringLiteral("Replace centers with a sample-rate-spaced list from start to end")));
    standardScanFillRangeButton->setMaximumWidth(56);
    standardScanStatusLabel = new QLabel(
        uiText(QStringLiteral("standard_scan_off"), QStringLiteral("Standard scan: off")),
        standardScanBox);
    standardScanStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    standardScanStatusLabel->setMinimumWidth(0);
    standardScanStatusLabel->setWordWrap(false);
    standardScanStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QLabel *standardScanCentersLabel = new QLabel("Centers MHz:", standardScanBox);
    markTranslatable(standardScanCentersLabel,
                     QStringLiteral("centers_mhz"),
                     QStringLiteral("Centers MHz:"));
    standardScanCentersLabel->setAlignment(Qt::AlignCenter);
    standardScanCentersLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QLabel *standardScanDwellLabel = new QLabel("Dwell:", standardScanBox);
    markTranslatable(standardScanDwellLabel,
                     QStringLiteral("dwell"),
                     QStringLiteral("Dwell:"));
    QLabel *standardScanSettleLabel = new QLabel("Settle:", standardScanBox);
    markTranslatable(standardScanSettleLabel,
                     QStringLiteral("settle"),
                     QStringLiteral("Settle:"));
    QLabel *standardScanRangeStartLabel = new QLabel("Start MHz:", standardScanBox);
    markTranslatable(standardScanRangeStartLabel,
                     QStringLiteral("range_start_mhz"),
                     QStringLiteral("Start MHz:"));
    QLabel *standardScanRangeEndLabel = new QLabel("End MHz:", standardScanBox);
    markTranslatable(standardScanRangeEndLabel,
                     QStringLiteral("range_end_mhz"),
                     QStringLiteral("End MHz:"));
    QVBoxLayout *standardScanLayout = new QVBoxLayout(standardScanBox);
    QHBoxLayout *standardScanTopLayout = new QHBoxLayout();
    QHBoxLayout *standardScanPresetLayout = new QHBoxLayout();
    QHBoxLayout *standardScanTimingLayout = new QHBoxLayout();
    QHBoxLayout *standardScanLockLayout = new QHBoxLayout();
    QHBoxLayout *standardScanCentersHeaderLayout = new QHBoxLayout();
    QHBoxLayout *standardScanCentersEditLayout = new QHBoxLayout();
    QHBoxLayout *standardScanRangeLayout = new QHBoxLayout();
    standardScanTopLayout->setContentsMargins(0, 0, 0, 0);
    standardScanTopLayout->setSpacing(4);
    standardScanTopLayout->addWidget(standardScanCheckbox);
    standardScanTopLayout->addStretch(1);
    standardScanPresetLayout->setContentsMargins(0, 0, 0, 0);
    standardScanPresetLayout->setSpacing(4);
    standardScanPresetLayout->addWidget(new QLabel(uiText(QStringLiteral("presets"), QStringLiteral("Presets...")), standardScanBox));
    standardScanPresetLayout->addWidget(standardScanPresetCombo, 1);
    standardScanPresetLayout->addWidget(standardScanSavePresetButton);
    standardScanPresetLayout->addWidget(standardScanDeletePresetButton);
    standardScanTimingLayout->setContentsMargins(0, 0, 0, 0);
    standardScanTimingLayout->setSpacing(4);
    standardScanTimingLayout->addWidget(standardScanDwellLabel);
    standardScanTimingLayout->addWidget(standardScanDwellSpin);
    standardScanTimingLayout->addWidget(standardScanSettleLabel);
    standardScanTimingLayout->addWidget(standardScanSettleSpin);
    standardScanTimingLayout->addStretch(1);
    standardScanLockLayout->setContentsMargins(0, 0, 0, 0);
    standardScanLockLayout->setSpacing(4);
    standardScanLockLayout->addWidget(scanListeningLockCheckbox);
    standardScanLockLayout->addStretch(1);
    standardScanCentersHeaderLayout->setContentsMargins(0, 0, 0, 0);
    standardScanCentersHeaderLayout->setSpacing(4);
    standardScanCentersHeaderLayout->addWidget(standardScanRemoveLowerButton);
    standardScanCentersHeaderLayout->addWidget(standardScanCentersLabel, 1);
    standardScanCentersHeaderLayout->addWidget(standardScanRemoveUpperButton);
    standardScanCentersEditLayout->setContentsMargins(0, 0, 0, 0);
    standardScanCentersEditLayout->setSpacing(4);
    standardScanCentersEditLayout->addWidget(standardScanAddLowerButton);
    standardScanCentersEditLayout->addWidget(standardScanCentersEdit, 1);
    standardScanCentersEditLayout->addWidget(standardScanAddUpperButton);
    standardScanRangeLayout->setContentsMargins(0, 0, 0, 0);
    standardScanRangeLayout->setSpacing(4);
    standardScanRangeLayout->addWidget(standardScanRangeStartLabel);
    standardScanRangeLayout->addWidget(standardScanRangeStartEdit);
    standardScanRangeLayout->addWidget(standardScanRangeEndLabel);
    standardScanRangeLayout->addWidget(standardScanRangeEndEdit);
    standardScanRangeLayout->addWidget(standardScanFillRangeButton);
    standardScanRangeLayout->addStretch(1);
    standardScanLayout->addLayout(standardScanTopLayout);
    standardScanLayout->addLayout(standardScanPresetLayout);
    standardScanLayout->addLayout(standardScanTimingLayout);
    standardScanLayout->addLayout(standardScanLockLayout);
    standardScanLayout->addLayout(standardScanCentersHeaderLayout);
    standardScanLayout->addLayout(standardScanCentersEditLayout);
    standardScanLayout->addLayout(standardScanRangeLayout);
    standardScanLayout->addWidget(standardScanStatusLabel);

    QGroupBox *listeningScanBox = new QGroupBox("Listening scan", this);
    markTranslatable(listeningScanBox, QStringLiteral("listening_scan"), QStringLiteral("Listening scan"));
    listeningScanCheckbox = new QCheckBox("Enable listening scan", listeningScanBox);
    markTranslatable(listeningScanCheckbox,
                     QStringLiteral("enable_listening_scan"),
                     QStringLiteral("Enable listening scan"));
    listeningScanCheckbox->setToolTip(uiText(
        QStringLiteral("listening_scan_tooltip"),
        QStringLiteral("Cycle only the listening/marker frequency inside the current visible IQ span.")));
    listeningScanCheckbox->setChecked(listeningScanEnabled);
    listeningScanPresetCombo = new QComboBox(listeningScanBox);
    listeningScanPresetCombo->setEditable(true);
    listeningScanPresetCombo->setInsertPolicy(QComboBox::NoInsert);
    listeningScanPresetCombo->setMinimumWidth(150);
    listeningScanPresetCombo->setToolTip(uiText(
        QStringLiteral("listening_scan_preset_tooltip"),
        QStringLiteral("Preset of listening target frequencies in MHz.")));
    listeningScanSavePresetButton = new QPushButton(uiText(QStringLiteral("save"), QStringLiteral("Save")), listeningScanBox);
    listeningScanSavePresetButton->setMaximumWidth(58);
    listeningScanDeletePresetButton = new QPushButton(uiText(QStringLiteral("delete_short"), QStringLiteral("Del")), listeningScanBox);
    listeningScanDeletePresetButton->setMaximumWidth(48);
    listeningScanTargetsEdit = new QLineEdit(listeningScanTargetsMhz, listeningScanBox);
    listeningScanTargetsEdit->setPlaceholderText(QStringLiteral("1561.098, 1575.420, 1602.000"));
    listeningScanTargetsEdit->setToolTip(uiText(
        QStringLiteral("listening_scan_targets_tooltip"),
        QStringLiteral("Listening target frequencies in MHz. They must fit inside the current center +/- sample-rate/2 span.")));
    listeningScanDwellSpin = new QSpinBox(listeningScanBox);
    listeningScanDwellSpin->setRange(LISTENING_SCAN_MIN_DWELL_MS, LISTENING_SCAN_MAX_DWELL_MS);
    listeningScanDwellSpin->setSingleStep(250);
    listeningScanDwellSpin->setSuffix(QStringLiteral(" ms"));
    listeningScanDwellSpin->setValue(listeningScanDwellMs);
    listeningScanDwellSpin->setMaximumWidth(104);
    listeningScanDwellSpin->setToolTip(uiText(
        QStringLiteral("listening_scan_dwell_tooltip"),
        QStringLiteral("How long to stay on each listening target.")));
    listeningScanSettleSpin = new QSpinBox(listeningScanBox);
    listeningScanSettleSpin->setRange(LISTENING_SCAN_MIN_SETTLE_MS, LISTENING_SCAN_MAX_SETTLE_MS);
    listeningScanSettleSpin->setSingleStep(50);
    listeningScanSettleSpin->setSuffix(QStringLiteral(" ms"));
    listeningScanSettleSpin->setValue(listeningScanSettleMs);
    listeningScanSettleSpin->setMaximumWidth(104);
    listeningScanSettleSpin->setToolTip(uiText(
        QStringLiteral("listening_scan_settle_tooltip"),
        QStringLiteral("Short mute/settle delay after moving the listening marker.")));
    listeningScanStatusLabel = new QLabel(
        uiText(QStringLiteral("listening_scan_off"), QStringLiteral("Listening scan: off")),
        listeningScanBox);
    listeningScanStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    listeningScanStatusLabel->setMinimumWidth(0);
    listeningScanStatusLabel->setWordWrap(false);
    listeningScanStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QLabel *listeningScanTargetsLabel = new QLabel("Targets MHz:", listeningScanBox);
    markTranslatable(listeningScanTargetsLabel,
                     QStringLiteral("targets_mhz"),
                     QStringLiteral("Targets MHz:"));
    QLabel *listeningScanDwellLabel = new QLabel("Dwell:", listeningScanBox);
    markTranslatable(listeningScanDwellLabel,
                     QStringLiteral("dwell"),
                     QStringLiteral("Dwell:"));
    QLabel *listeningScanSettleLabel = new QLabel("Settle:", listeningScanBox);
    markTranslatable(listeningScanSettleLabel,
                     QStringLiteral("settle"),
                     QStringLiteral("Settle:"));
    QVBoxLayout *listeningScanLayout = new QVBoxLayout(listeningScanBox);
    QHBoxLayout *listeningScanTopLayout = new QHBoxLayout();
    QHBoxLayout *listeningScanPresetLayout = new QHBoxLayout();
    QHBoxLayout *listeningScanTargetsLayout = new QHBoxLayout();
    QHBoxLayout *listeningScanTimingLayout = new QHBoxLayout();
    listeningScanTopLayout->setContentsMargins(0, 0, 0, 0);
    listeningScanTopLayout->setSpacing(4);
    listeningScanTopLayout->addWidget(listeningScanCheckbox);
    listeningScanTopLayout->addStretch(1);
    listeningScanPresetLayout->setContentsMargins(0, 0, 0, 0);
    listeningScanPresetLayout->setSpacing(4);
    listeningScanPresetLayout->addWidget(new QLabel(uiText(QStringLiteral("presets"), QStringLiteral("Presets...")), listeningScanBox));
    listeningScanPresetLayout->addWidget(listeningScanPresetCombo, 1);
    listeningScanPresetLayout->addWidget(listeningScanSavePresetButton);
    listeningScanPresetLayout->addWidget(listeningScanDeletePresetButton);
    listeningScanTargetsLayout->setContentsMargins(0, 0, 0, 0);
    listeningScanTargetsLayout->setSpacing(4);
    listeningScanTargetsLayout->addWidget(listeningScanTargetsLabel);
    listeningScanTargetsLayout->addWidget(listeningScanTargetsEdit, 1);
    listeningScanTimingLayout->setContentsMargins(0, 0, 0, 0);
    listeningScanTimingLayout->setSpacing(4);
    listeningScanTimingLayout->addWidget(listeningScanDwellLabel);
    listeningScanTimingLayout->addWidget(listeningScanDwellSpin);
    listeningScanTimingLayout->addWidget(listeningScanSettleLabel);
    listeningScanTimingLayout->addWidget(listeningScanSettleSpin);
    listeningScanTimingLayout->addStretch(1);
    listeningScanLayout->addLayout(listeningScanTopLayout);
    listeningScanLayout->addLayout(listeningScanPresetLayout);
    listeningScanLayout->addLayout(listeningScanTargetsLayout);
    listeningScanLayout->addLayout(listeningScanTimingLayout);
    listeningScanLayout->addWidget(listeningScanStatusLabel);

    QGroupBox *qthBox = new QGroupBox("GPS / QTH", this);
    markTranslatable(qthBox, QStringLiteral("gps_qth"), QStringLiteral("GPS / QTH"));
    qthSourceCombo = new QComboBox(qthBox);
    qthSourceCombo->addItem(uiText(QStringLiteral("manual"), QStringLiteral("Manual")), QStringLiteral("manual"));
    qthSourceCombo->addItem(QStringLiteral("NMEA GPS"), QStringLiteral("nmea"));
    qthSourceCombo->addItem(uiText(QStringLiteral("os_location"), QStringLiteral("OS location")), QStringLiteral("os"));
    qthSourceCombo->setItemData(1,
                                uiText(QStringLiteral("nmea_paste_tooltip"),
                                       QStringLiteral("Paste NMEA GGA/RMC text from the clipboard and use it as the current QTH.")),
                                Qt::ToolTipRole);
    qthSourceCombo->setItemData(2,
                                uiText(QStringLiteral("gps_source_future_tooltip"),
                                       QStringLiteral("OS location input is planned; manual coordinates are used for now.")),
                                Qt::ToolTipRole);
    qthSourceCombo->setCurrentIndex(0);
    gnssSystemCombo = new QComboBox(qthBox);
    for (const GnssSystemPreset &preset : gnssSystemPresets()) {
        gnssSystemCombo->addItem(uiText(preset.textKey, preset.fallbackName), preset.id);
    }
    gnssSystemCombo->setToolTip(uiText(
        QStringLiteral("gnss_system_tooltip"),
        QStringLiteral("Select which GNSS signal family the tune, scan and acquisition controls should target.")));
    gnssIntegrationSpin = new QSpinBox(qthBox);
    gnssIntegrationSpin->setRange(GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                                  GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    gnssIntegrationSpin->setSingleStep(4);
    gnssIntegrationSpin->setValue(gnssAcquisitionIntegrationMs);
    gnssIntegrationSpin->setSuffix(QStringLiteral(" ms"));
    gnssIntegrationSpin->setToolTip(uiText(
        QStringLiteral("gnss_acq_integration_tooltip"),
        QStringLiteral("How many milliseconds of GPS C/A code to accumulate from the current IQ snapshot.")));
    gnssChannelFilterSpin = new QDoubleSpinBox(qthBox);
    gnssChannelFilterSpin->setRange(GNSS_CHANNEL_FILTER_MIN_HZ / 1000000.0,
                                    GNSS_CHANNEL_FILTER_MAX_HZ / 1000000.0);
    gnssChannelFilterSpin->setDecimals(3);
    gnssChannelFilterSpin->setSingleStep(0.1);
    gnssChannelFilterSpin->setValue(gnssChannelFilterCutoffHz / 1000000.0);
    gnssChannelFilterSpin->setSuffix(QStringLiteral(" MHz"));
    gnssChannelFilterSpin->setToolTip(uiText(
        QStringLiteral("gnss_channel_lpf_tooltip"),
        QStringLiteral("Low-pass cutoff for the GPS L1 C/A IQ channelizer before PRN correlation.")));
    gnssDopplerSpanSpin = new QSpinBox(qthBox);
    gnssDopplerSpanSpin->setRange(1, 50);
    gnssDopplerSpanSpin->setSingleStep(1);
    gnssDopplerSpanSpin->setValue((std::clamp)(gnssDopplerSpanHz / 1000, 1, 50));
    gnssDopplerSpanSpin->setSuffix(QStringLiteral(" kHz"));
    gnssDopplerSpanSpin->setToolTip(uiText(
        QStringLiteral("gnss_doppler_span_tooltip"),
        QStringLiteral("GPS C/A acquisition Doppler search half-span. Lower values are faster; wider values tolerate larger clock/frequency error.")));
    gnssDopplerStepSpin = new QSpinBox(qthBox);
    gnssDopplerStepSpin->setRange(250, 5000);
    gnssDopplerStepSpin->setSingleStep(250);
    gnssDopplerStepSpin->setValue((std::clamp)(gnssDopplerStepHz, 250, 5000));
    gnssDopplerStepSpin->setSuffix(QStringLiteral(" Hz"));
    gnssDopplerStepSpin->setToolTip(uiText(
        QStringLiteral("gnss_doppler_step_tooltip"),
        QStringLiteral("GPS C/A acquisition Doppler bin spacing. Smaller values are slower but can refine weak candidates.")));
    qthLatitudeSpin = new QDoubleSpinBox(qthBox);
    qthLatitudeSpin->setRange(-90.0, 90.0);
    qthLatitudeSpin->setDecimals(6);
    qthLatitudeSpin->setSingleStep(0.0001);
    qthLatitudeSpin->setValue(qthLatitude);
    qthLatitudeSpin->setSuffix(QStringLiteral(" deg"));
    qthLongitudeSpin = new QDoubleSpinBox(qthBox);
    qthLongitudeSpin->setRange(-180.0, 180.0);
    qthLongitudeSpin->setDecimals(6);
    qthLongitudeSpin->setSingleStep(0.0001);
    qthLongitudeSpin->setValue(qthLongitude);
    qthLongitudeSpin->setSuffix(QStringLiteral(" deg"));
    qthLocatorLabel = new QLabel(qthBox);
    qthLocatorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto prepareGnssStatusLabel = [](QLabel *label) {
        if (!label) {
            return;
        }
        label->setWordWrap(true);
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    };
    qthStatusLabel = new QLabel(qthBox);
    prepareGnssStatusLabel(qthStatusLabel);
    qthMapButton = new QPushButton("Map", qthBox);
    markTranslatable(qthMapButton, QStringLiteral("qth_map"), QStringLiteral("Map"));
    qthCopyButton = new QPushButton("Position", qthBox);
    markTranslatable(qthCopyButton, QStringLiteral("qth_position"), QStringLiteral("Position"));
    qthCopyButton->setToolTip(uiText(
        QStringLiteral("qth_position_tooltip"),
        QStringLiteral("Show the last valid QTH position on the map and copy its Maidenhead locator.")));
    qthClearButton = new QPushButton("Clear", qthBox);
    markTranslatable(qthClearButton, QStringLiteral("clear_qth"), QStringLiteral("Clear"));
    qthClearButton->setToolTip(uiText(
        QStringLiteral("clear_qth_tooltip"),
        QStringLiteral("Hide the current real QTH position from the map and clear the live location state.")));
    qthPasteNmeaButton = nullptr;
    gnssSerialPortEdit = new QComboBox(qthBox);
    gnssSerialPortEdit->setEditable(true);
    gnssSerialPortEdit->setMaximumWidth(98);
    QStringList serialPortNames;
    QString preferredSerialPortName = gnssSerialPortName.trimmed();
    for (const QSerialPortInfo &portInfo : QSerialPortInfo::availablePorts()) {
        serialPortNames << portInfo.portName();
        const QString description =
            QStringList {portInfo.description(), portInfo.manufacturer(), portInfo.systemLocation()}
                .join(QLatin1Char(' '))
                .toLower();
        if (preferredSerialPortName.isEmpty() &&
            ((portInfo.hasVendorIdentifier() && portInfo.vendorIdentifier() == 0x1546) ||
             description.contains(QStringLiteral("u-blox")) ||
             description.contains(QStringLiteral("ublox")) ||
             description.contains(QStringLiteral("gnss")) ||
             description.contains(QStringLiteral("gps")))) {
            preferredSerialPortName = portInfo.portName();
        }
    }
    serialPortNames.removeDuplicates();
    serialPortNames.sort(Qt::CaseInsensitive);
    if (preferredSerialPortName.isEmpty() && serialPortNames.size() == 1) {
        preferredSerialPortName = serialPortNames.constFirst();
    }
    if (!preferredSerialPortName.isEmpty() &&
        !serialPortNames.contains(preferredSerialPortName, Qt::CaseInsensitive)) {
        serialPortNames.prepend(preferredSerialPortName);
    }
    gnssSerialPortName = preferredSerialPortName;
    gnssSerialPortEdit->addItems(serialPortNames);
    if (!gnssSerialPortName.isEmpty()) {
        gnssSerialPortEdit->setCurrentText(gnssSerialPortName);
    }
    gnssSerialPortEdit->setToolTip(uiText(
        QStringLiteral("gnss_serial_port_tooltip"),
        QStringLiteral("Serial port for an external NMEA GNSS receiver, for example COM4 or /dev/ttyUSB0.")));
    gnssSerialBaudSpin = new QSpinBox(qthBox);
    gnssSerialBaudSpin->setRange(1200, 921600);
    gnssSerialBaudSpin->setSingleStep(4800);
    gnssSerialBaudSpin->setValue(gnssSerialBaud);
    gnssSerialBaudSpin->setMaximumWidth(86);
    gnssSerialBaudSpin->setToolTip(uiText(
        QStringLiteral("gnss_serial_baud_tooltip"),
        QStringLiteral("NMEA serial baud rate. Most NEO-M8N modules use 9600 by default.")));
    gnssSerialButton = new QPushButton("Connect", qthBox);
    markTranslatable(gnssSerialButton, QStringLiteral("connect"), QStringLiteral("Connect"));
    gnssSerialButton->setToolTip(uiText(
        QStringLiteral("gnss_serial_connect_tooltip"),
        QStringLiteral("Open the serial NMEA stream and use valid GGA/RMC fixes as the current QTH.")));
    gnssNmeaLogButton = new QPushButton("Log", qthBox);
    markTranslatable(gnssNmeaLogButton, QStringLiteral("nmea_log"), QStringLiteral("Log"));
    gnssNmeaLogButton->setToolTip(uiText(
        QStringLiteral("nmea_log_tooltip"),
        QStringLiteral("Start or stop writing live NMEA sentences to recordings/nmea.")));
    gnssNmeaReplayButton = new QPushButton("Replay", qthBox);
    markTranslatable(gnssNmeaReplayButton, QStringLiteral("nmea_replay"), QStringLiteral("Replay"));
    gnssNmeaReplayButton->setToolTip(uiText(
        QStringLiteral("nmea_replay_tooltip"),
        QStringLiteral("Replay a saved NMEA log through the same parser, map and satellite diagnostics.")));
    gnssSerialRawLogButton = new QPushButton("Raw", qthBox);
    markTranslatable(gnssSerialRawLogButton, QStringLiteral("gnss_raw_serial_log"), QStringLiteral("Raw"));
    gnssSerialRawLogButton->setToolTip(uiText(
        QStringLiteral("gnss_raw_serial_log_tooltip"),
        QStringLiteral("Start or stop a raw binary UBX/NMEA serial capture in recordings/gnss_raw.")));
    gnssUbxSystemsButton = new QPushButton("UBX sys", qthBox);
    markTranslatable(gnssUbxSystemsButton, QStringLiteral("gnss_ubx_systems"), QStringLiteral("UBX sys"));
    gnssUbxSystemsButton->setToolTip(uiText(
        QStringLiteral("gnss_ubx_systems_tooltip"),
        QStringLiteral("Apply the GPS/GLO/GAL/BDS/QZSS/SBAS checkboxes to the receiver through UBX CFG-GNSS. If needed, the module is polled first.")));
    gnssUbxSaveButton = new QPushButton("Save cfg", qthBox);
    markTranslatable(gnssUbxSaveButton, QStringLiteral("gnss_ubx_save_cfg"), QStringLiteral("Save cfg"));
    gnssUbxSaveButton->setToolTip(uiText(
        QStringLiteral("gnss_ubx_save_cfg_tooltip"),
        QStringLiteral("Ask the u-blox module to save its current configuration to non-volatile memory.")));
    gnssPositionPolicyCombo = new QComboBox(qthBox);
    gnssPositionPolicyCombo->addItem(uiText(QStringLiteral("gnss_position_policy_auto"),
                                            QStringLiteral("Auto")),
                                     QStringLiteral("auto"));
    gnssPositionPolicyCombo->addItem(uiText(QStringLiteral("gnss_position_policy_ubx_preferred"),
                                            QStringLiteral("UBX preferred")),
                                     QStringLiteral("ubx_preferred"));
    gnssPositionPolicyCombo->addItem(uiText(QStringLiteral("gnss_position_policy_nmea_only"),
                                            QStringLiteral("NMEA only")),
                                     QStringLiteral("nmea_only"));
    gnssPositionPolicyCombo->addItem(uiText(QStringLiteral("gnss_position_policy_ubx_only"),
                                            QStringLiteral("UBX only")),
                                     QStringLiteral("ubx_only"));
    {
        const int policyIndex = gnssPositionPolicyCombo->findData(normalizedGnssPositionPolicy(gnssPositionPolicy));
        gnssPositionPolicyCombo->setCurrentIndex(policyIndex >= 0 ? policyIndex : 0);
    }
    gnssPositionPolicyCombo->setToolTip(uiText(
        QStringLiteral("gnss_position_policy_tooltip"),
        QStringLiteral("Choose which external GNSS stream is allowed to update the current QTH position.")));
    gnssTimeZoneCombo = new QComboBox(qthBox);
    gnssTimeZoneCombo->addItem(QStringLiteral("UTC"), 0);
    gnssTimeZoneCombo->addItem(uiText(QStringLiteral("local_time"), QStringLiteral("Local")), 100000);
    for (int hour = -12; hour <= 14; ++hour) {
        if (hour == 0) {
            continue;
        }
        gnssTimeZoneCombo->addItem(QStringLiteral("UTC%1%2:00")
                                       .arg(hour > 0 ? QStringLiteral("+") : QStringLiteral("-"))
                                       .arg(std::abs(hour), 2, 10, QLatin1Char('0')),
                                   hour * 60);
    }
    {
        const int zoneIndex = gnssTimeZoneCombo->findData(gnssTimeZoneOffsetMinutes);
        gnssTimeZoneCombo->setCurrentIndex(zoneIndex >= 0 ? zoneIndex : 0);
    }
    gnssTimeZoneCombo->setToolTip(uiText(
        QStringLiteral("gnss_timezone_tooltip"),
        QStringLiteral("Time zone used only for displaying GNSS UTC time. Raw NMEA/UBX data remains unchanged.")));
    gnssTuneButton = new QPushButton("Tune", qthBox);
    gnssPvtButton = new QPushButton(uiText(QStringLiteral("gnss_sdr_pvt"), QStringLiteral("SDR PVT")), qthBox);
    markTranslatable(gnssPvtButton, QStringLiteral("gnss_sdr_pvt"), QStringLiteral("SDR PVT"));
    connect(gnssPvtButton, &QPushButton::clicked, this, &YourClassName::showGnssSdrPvt);
    markTranslatable(gnssTuneButton, QStringLiteral("tune_gnss_l1"), QStringLiteral("Tune"));
    gnssScanButton = new QPushButton("Scan", qthBox);
    markTranslatable(gnssScanButton, QStringLiteral("use_gnss_scan"), QStringLiteral("Scan"));
    gnssRawLogButton = new QPushButton("Save IQ", qthBox);
    markTranslatable(gnssRawLogButton, QStringLiteral("log_raw_context"), QStringLiteral("Save IQ"));
    gnssRawLogButton->setToolTip(uiText(
        QStringLiteral("gnss_raw_context_tooltip"),
        QStringLiteral("Save the current live GNSS IQ snapshot as stereo PCM16 WAV and write its tuning context to the diagnostic log.")));
    gnssAcquireButton = new QPushButton("Accum", qthBox);
    markTranslatable(gnssAcquireButton, QStringLiteral("gps_ca_scan"), QStringLiteral("Accum"));
    gnssAcquireButton->setToolTip(uiText(
        QStringLiteral("gps_ca_scan_tooltip"),
        QStringLiteral("Run accumulated GPS L1 C/A PRN correlation on the current IQ snapshot.")));
    gnssDeepAcquireButton = new QPushButton("Auto", qthBox);
    gnssDeepAcquireButton->setCheckable(true);
    gnssDeepAcquireButton->setChecked(gnssContinuousAcquisitionEnabled);
    markTranslatable(gnssDeepAcquireButton, QStringLiteral("gps_ca_deep_scan"), QStringLiteral("Auto"));
    gnssDeepAcquireButton->setToolTip(uiText(
        QStringLiteral("gps_ca_deep_scan_tooltip"),
        QStringLiteral("Continuously run GPS L1 C/A acquisition on the rolling live IQ buffer.")));
    gnssOfflineAcquireButton = new QPushButton("Replay", qthBox);
    markTranslatable(gnssOfflineAcquireButton, QStringLiteral("gps_ca_replay_scan"), QStringLiteral("Replay"));
    gnssOfflineAcquireButton->setToolTip(uiText(
        QStringLiteral("gps_ca_replay_scan_tooltip"),
        QStringLiteral("Run GPS L1 C/A acquisition directly on the selected Channel IQ WAV recording.")));
    gnssSelfTestButton = new QPushButton("Self-test", qthBox);
    markTranslatable(gnssSelfTestButton, QStringLiteral("gps_ca_self_test"), QStringLiteral("Self-test"));
    gnssSelfTestButton->setToolTip(uiText(
        QStringLiteral("gps_ca_self_test_tooltip"),
        QStringLiteral("Generate an ideal GPS L1 C/A IQ signal and run it through the same acquisition path.")));
    gnssPositionSelfTestButton = new QPushButton("Pos test", qthBox);
    markTranslatable(gnssPositionSelfTestButton, QStringLiteral("gnss_position_self_test"), QStringLiteral("Pos test"));
    gnssPositionSelfTestButton->setToolTip(uiText(
        QStringLiteral("gnss_position_self_test_tooltip"),
        QStringLiteral("Solve a synthetic multi-satellite pseudorange fix and show the result on the QTH map.")));
    gnssMonitorCheckbox = new QCheckBox("IQ monitor", qthBox);
    markTranslatable(gnssMonitorCheckbox, QStringLiteral("gnss_iq_monitor"), QStringLiteral("IQ monitor"));
    gnssMonitorCheckbox->setToolTip(uiText(
        QStringLiteral("gnss_iq_monitor_tooltip"),
        QStringLiteral("Measure live/playback IQ level, DC offset, clipping and I/Q balance before attempting GNSS acquisition.")));
    gnssUseGpsCheckbox = new QCheckBox(QStringLiteral("GPS"), qthBox);
    gnssUseGlonassCheckbox = new QCheckBox(QStringLiteral("GLO"), qthBox);
    gnssUseGalileoCheckbox = new QCheckBox(QStringLiteral("GAL"), qthBox);
    gnssUseBeidouCheckbox = new QCheckBox(QStringLiteral("BDS"), qthBox);
    gnssUseQzssCheckbox = new QCheckBox(QStringLiteral("QZSS"), qthBox);
    gnssUseSbasCheckbox = new QCheckBox(QStringLiteral("SBAS"), qthBox);
    gnssUseOtherCheckbox = new QCheckBox(QStringLiteral("Other"), qthBox);
    for (QCheckBox *box : {gnssUseGpsCheckbox,
                           gnssUseGlonassCheckbox,
                           gnssUseGalileoCheckbox,
                           gnssUseBeidouCheckbox,
                           gnssUseQzssCheckbox,
                           gnssUseSbasCheckbox,
                           gnssUseOtherCheckbox}) {
        box->setChecked(true);
        box->setMinimumWidth(0);
        box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        box->setToolTip(uiText(QStringLiteral("gnss_system_filter_tooltip"),
                               QStringLiteral("Use this constellation in NMEA satellite diagnostics and filtered GNSS decisions.")));
    }
    markTranslatable(gnssUseOtherCheckbox, QStringLiteral("other"), QStringLiteral("Other"));
    gnssMonitorResetButton = new QPushButton("IQ reset", qthBox);
    markTranslatable(gnssMonitorResetButton, QStringLiteral("gnss_iq_reset"), QStringLiteral("IQ reset"));
    gnssMonitorResetButton->setToolTip(uiText(
        QStringLiteral("gnss_iq_monitor_reset_tooltip"),
        QStringLiteral("Reset only the GNSS IQ monitor peak history and accumulated SDR statistics. It does not clear the QTH position.")));
    gnssNetworkTimeButton = new QPushButton("NTP", qthBox);
    markTranslatable(gnssNetworkTimeButton, QStringLiteral("gnss_ntp_time"), QStringLiteral("NTP"));
    gnssNetworkTimeButton->setToolTip(uiText(
        QStringLiteral("gnss_ntp_time_tooltip"),
        QStringLiteral("Query an internet NTP server for UTC time. This is only an assisted-GNSS hint, not code-phase lock.")));
    gnssPlotButton = new QPushButton("Plot", qthBox);
    markTranslatable(gnssPlotButton, QStringLiteral("gnss_acq_plot"), QStringLiteral("Plot"));
    gnssPlotButton->setToolTip(uiText(
        QStringLiteral("gnss_acq_plot_tooltip"),
        QStringLiteral("GPS acquisition diagnostics: PRN/Doppler heatmap, best code-phase correlation and peak history.")));
    gnssSatellitesButton = new QPushButton("Sats", qthBox);
    markTranslatable(gnssSatellitesButton, QStringLiteral("gnss_satellites"), QStringLiteral("Sats"));
    gnssSatellitesButton->setToolTip(uiText(
        QStringLiteral("gnss_satellites_tooltip"),
        QStringLiteral("Open the live NMEA satellite list in a separate resizable window.")));
    gnssMonitorStatusLabel = new QLabel(qthBox);
    prepareGnssStatusLabel(gnssMonitorStatusLabel);
    gnssSerialStatusLabel = new QLabel(qthBox);
    prepareGnssStatusLabel(gnssSerialStatusLabel);
    gnssAcquireStatusLabel = new QLabel(qthBox);
    prepareGnssStatusLabel(gnssAcquireStatusLabel);
    gnssAcquisitionPlotDialog = new QDialog(this);
    gnssAcquisitionPlotDialog->setWindowTitle(uiText(QStringLiteral("gnss_acq_plot_title"),
                                                     QStringLiteral("GNSS acquisition diagnostics")));
    gnssAcquisitionPlotDialog->resize(760, 360);
    QVBoxLayout *gnssPlotDialogLayout = new QVBoxLayout(gnssAcquisitionPlotDialog);
    gnssAcquisitionPlotLabel = new QLabel(gnssAcquisitionPlotDialog);
    gnssAcquisitionPlotLabel->setMinimumSize(520, 260);
    gnssAcquisitionPlotLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    gnssAcquisitionPlotLabel->setScaledContents(false);
    gnssAcquisitionPlotLabel->setAlignment(Qt::AlignCenter);
    gnssAcquisitionPlotLabel->setText(uiText(QStringLiteral("gnss_acq_plot_waiting"),
                                             QStringLiteral("Run Accum/Auto/Replay/Self-test to draw SDR correlation heatmap.")));
    gnssAcquisitionPlotLabel->setToolTip(uiText(
        QStringLiteral("gnss_acq_plot_tooltip"),
        QStringLiteral("GPS acquisition diagnostics: PRN/Doppler heatmap, best code-phase correlation and peak history.")));
    gnssPlotDialogLayout->addWidget(gnssAcquisitionPlotLabel, 1);

    gnssSatelliteDialog = new QDialog(this);
    gnssSatelliteDialog->setWindowTitle(uiText(QStringLiteral("gnss_satellite_window_title"),
                                               QStringLiteral("GNSS satellites")));
    gnssSatelliteDialog->resize(720, 620);
    QVBoxLayout *gnssSatelliteLayout = new QVBoxLayout(gnssSatelliteDialog);
    gnssSatelliteStatusLabel = new QLabel(gnssSatelliteDialog);
    gnssSatelliteStatusLabel->setTextFormat(Qt::RichText);
    gnssSatelliteStatusLabel->setWordWrap(true);
    gnssSatelliteStatusLabel->setText(uiText(QStringLiteral("gnss_satellite_status_waiting"),
                                             QStringLiteral("Waiting for GNSS satellite data.")));
    gnssSatelliteStatusLabel->setToolTip(uiText(
        QStringLiteral("gnss_satellite_status_tooltip"),
        QStringLiteral("Live GNSS fix summary from NMEA and UBX: source, fix type, satellites, DOP, altitude, speed and UTC.")));
    gnssSatelliteLayout->addWidget(gnssSatelliteStatusLabel);
    gnssSatelliteTableCheckbox = new QCheckBox(uiText(QStringLiteral("show_table"),
                                                      QStringLiteral("Table")),
                                               gnssSatelliteDialog);
    gnssSatelliteTableCheckbox->setChecked(gnssSatelliteTableVisible);
    gnssSatelliteTableCheckbox->setToolTip(uiText(
        QStringLiteral("gnss_satellite_table_toggle_tooltip"),
        QStringLiteral("Open or hide the live satellite table in a separate resizable window.")));
    gnssSatelliteLayout->addWidget(gnssSatelliteTableCheckbox);
    gnssSatelliteSkyLabel = new QLabel(gnssSatelliteDialog);
    gnssSatelliteSkyLabel->setMinimumSize(520, 150);
    gnssSatelliteSkyLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    gnssSatelliteSkyLabel->setScaledContents(false);
    gnssSatelliteSkyLabel->setAlignment(Qt::AlignCenter);
    gnssSatelliteSkyLabel->setText(uiText(QStringLiteral("gnss_sky_waiting"),
                                          QStringLiteral("Waiting for live GNSS satellites.")));
    gnssSatelliteSkyLabel->setToolTip(uiText(
        QStringLiteral("gnss_sky_tooltip"),
        QStringLiteral("Sky view from NMEA GSV or UBX NAV-SAT azimuth/elevation values. Color follows C/N0; white outline means the satellite is used in the module fix.")));
    gnssSatelliteLayout->addWidget(gnssSatelliteSkyLabel, 1);

    gnssSatelliteTableDialog = new QDialog(this);
    gnssSatelliteTableDialog->setWindowTitle(uiText(QStringLiteral("gnss_satellite_table_window_title"),
                                                    QStringLiteral("GNSS satellite table")));
    gnssSatelliteTableDialog->resize(980, 560);
    QVBoxLayout *gnssSatelliteTableLayout = new QVBoxLayout(gnssSatelliteTableDialog);
    gnssSatelliteTable = new QTableWidget(GNSS_SATELLITE_TABLE_ROWS, 10, gnssSatelliteTableDialog);
    gnssSatelliteTable->setHorizontalHeaderLabels({
        uiText(QStringLiteral("use"), QStringLiteral("Use")),
        uiText(QStringLiteral("source_short"), QStringLiteral("Src")),
        uiText(QStringLiteral("system"), QStringLiteral("System")),
        QStringLiteral("SVID"),
        uiText(QStringLiteral("elevation_short"), QStringLiteral("El")),
        uiText(QStringLiteral("azimuth_short"), QStringLiteral("Az")),
        QStringLiteral("C/N0"),
        uiText(QStringLiteral("fix"), QStringLiteral("Fix")),
        uiText(QStringLiteral("age"), QStringLiteral("Age")),
        uiText(QStringLiteral("quality"), QStringLiteral("Quality"))
    });
    gnssSatelliteTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gnssSatelliteTable->setSelectionMode(QAbstractItemView::NoSelection);
    gnssSatelliteTable->setAlternatingRowColors(true);
    gnssSatelliteTable->setMinimumSize(520, 180);
    gnssSatelliteTable->verticalHeader()->setVisible(false);
    gnssSatelliteTable->horizontalHeader()->setStretchLastSection(false);
    gnssSatelliteTable->horizontalHeader()->setSectionsClickable(true);
    gnssSatelliteTable->horizontalHeader()->setSortIndicatorShown(true);
    gnssSatelliteTable->horizontalHeader()->setSortIndicator(gnssSatelliteSortColumn,
                                                             gnssSatelliteSortAscending ? Qt::AscendingOrder
                                                                                        : Qt::DescendingOrder);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(8, QHeaderView::ResizeToContents);
    gnssSatelliteTable->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Stretch);
    gnssSatelliteTable->setToolTip(uiText(
        QStringLiteral("gnss_satellite_table_tooltip"),
        QStringLiteral("Live GNSS satellites from NMEA GSV/GSA and UBX NAV-SAT. Click headers to sort; right-click Use for bulk selection.")));
    for (int row = 0; row < GNSS_SATELLITE_TABLE_ROWS; ++row) {
        for (int column = 0; column < gnssSatelliteTable->columnCount(); ++column) {
            auto *item = new QTableWidgetItem(column == 0 ? QString() : QStringLiteral("-"));
            item->setTextAlignment(Qt::AlignCenter);
            item->setFlags(Qt::ItemIsEnabled);
            if (column == 0) {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Unchecked);
            }
            gnssSatelliteTable->setItem(row, column, item);
        }
        gnssSatelliteTable->setRowHidden(row, true);
    }
    gnssSatelliteTable->setContextMenuPolicy(Qt::CustomContextMenu);
    gnssSatelliteTableLayout->addWidget(gnssSatelliteTable, 1);

    auto prepareGnssButton = [](QPushButton *button) {
        if (!button) {
            return;
        }
        button->setMinimumWidth(0);
        button->setMaximumWidth(82);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    };
    for (QPushButton *button : {
             qthMapButton,
             qthCopyButton,
             gnssTuneButton,
             gnssScanButton,
             gnssRawLogButton,
             gnssAcquireButton,
             gnssDeepAcquireButton,
             gnssOfflineAcquireButton,
             gnssSelfTestButton,
             gnssPositionSelfTestButton,
             gnssSerialButton,
             gnssNmeaLogButton,
             gnssNmeaReplayButton,
             gnssSerialRawLogButton,
             gnssUbxSystemsButton,
             gnssUbxSaveButton,
             gnssNetworkTimeButton,
             gnssPlotButton,
             gnssSatellitesButton,
             gnssMonitorResetButton}) {
        prepareGnssButton(button);
    }

    QLabel *qthSourceLabel = new QLabel("Source:", qthBox);
    markTranslatable(qthSourceLabel, QStringLiteral("source"), QStringLiteral("Source:"));
    QLabel *gnssSystemLabel = new QLabel("System:", qthBox);
    markTranslatable(gnssSystemLabel, QStringLiteral("gnss_system"), QStringLiteral("System:"));
    QLabel *gnssIntegrationLabel = new QLabel("Accum:", qthBox);
    markTranslatable(gnssIntegrationLabel, QStringLiteral("gnss_acq_integration"), QStringLiteral("Accum:"));
    QLabel *gnssFilterLabel = new QLabel("GPS LPF:", qthBox);
    markTranslatable(gnssFilterLabel, QStringLiteral("gnss_channel_lpf"), QStringLiteral("GPS LPF:"));
    QLabel *gnssDopplerLabel = new QLabel("Doppler:", qthBox);
    markTranslatable(gnssDopplerLabel, QStringLiteral("gnss_doppler"), QStringLiteral("Doppler:"));
    QLabel *qthLatLabel = new QLabel("Lat:", qthBox);
    markTranslatable(qthLatLabel, QStringLiteral("latitude_short"), QStringLiteral("Lat:"));
    QLabel *qthLonLabel = new QLabel("Lon:", qthBox);
    markTranslatable(qthLonLabel, QStringLiteral("longitude_short"), QStringLiteral("Lon:"));
    QLabel *qthLocatorTitleLabel = new QLabel("QTH:", qthBox);
    markTranslatable(qthLocatorTitleLabel, QStringLiteral("qth_locator_short"), QStringLiteral("QTH:"));
    QLabel *gnssSerialLabel = new QLabel("NMEA:", qthBox);
    markTranslatable(gnssSerialLabel, QStringLiteral("nmea_serial"), QStringLiteral("NMEA:"));
    QLabel *gnssTimeZoneLabel = new QLabel("Time:", qthBox);
    markTranslatable(gnssTimeZoneLabel, QStringLiteral("gnss_time_zone"), QStringLiteral("Time:"));
    QLabel *gnssFilterLabel2 = new QLabel("Use:", qthBox);
    markTranslatable(gnssFilterLabel2, QStringLiteral("use"), QStringLiteral("Use:"));
    QWidget *gnssFilterWidget = new QWidget(qthBox);
    QGridLayout *gnssFilterLayout = new QGridLayout(gnssFilterWidget);
    gnssFilterLayout->setContentsMargins(0, 0, 0, 0);
    gnssFilterLayout->setHorizontalSpacing(4);
    gnssFilterLayout->setVerticalSpacing(0);
    gnssFilterLayout->addWidget(gnssUseGpsCheckbox, 0, 0);
    gnssFilterLayout->addWidget(gnssUseGlonassCheckbox, 0, 1);
    gnssFilterLayout->addWidget(gnssUseGalileoCheckbox, 0, 2);
    gnssFilterLayout->addWidget(gnssUseBeidouCheckbox, 1, 0);
    gnssFilterLayout->addWidget(gnssUseQzssCheckbox, 1, 1);
    gnssFilterLayout->addWidget(gnssUseSbasCheckbox, 1, 2);
    gnssFilterLayout->addWidget(gnssUseOtherCheckbox, 2, 0, 1, 3);

    QGridLayout *qthLayout = new QGridLayout(qthBox);
    qthLayout->setContentsMargins(6, 8, 6, 6);
    qthLayout->setHorizontalSpacing(6);
    qthLayout->setVerticalSpacing(4);
    qthLayout->addWidget(qthSourceLabel, 0, 0);
    qthLayout->addWidget(qthSourceCombo, 0, 1, 1, 2);
    qthLayout->addWidget(gnssSystemLabel, 1, 0);
    qthLayout->addWidget(gnssSystemCombo, 1, 1, 1, 2);
    qthLayout->addWidget(gnssIntegrationLabel, 2, 0);
    qthLayout->addWidget(gnssIntegrationSpin, 2, 1, 1, 2);
    qthLayout->addWidget(gnssFilterLabel, 3, 0);
    qthLayout->addWidget(gnssChannelFilterSpin, 3, 1, 1, 2);
    qthLayout->addWidget(gnssDopplerLabel, 4, 0);
    qthLayout->addWidget(gnssDopplerSpanSpin, 4, 1);
    qthLayout->addWidget(gnssDopplerStepSpin, 4, 2);
    qthLayout->addWidget(qthLatLabel, 5, 0);
    qthLayout->addWidget(qthLatitudeSpin, 5, 1, 1, 2);
    qthLayout->addWidget(qthLonLabel, 6, 0);
    qthLayout->addWidget(qthLongitudeSpin, 6, 1, 1, 2);
    qthLayout->addWidget(qthLocatorTitleLabel, 7, 0);
    qthLayout->addWidget(qthLocatorLabel, 7, 1, 1, 2);
    qthLayout->addWidget(qthMapButton, 8, 0);
    qthLayout->addWidget(gnssSatellitesButton, 8, 1);
    qthLayout->addWidget(gnssPlotButton, 8, 2);
    qthLayout->addWidget(qthCopyButton, 9, 0);
    qthLayout->addWidget(qthClearButton, 9, 1);
    qthLayout->addWidget(gnssMonitorResetButton, 9, 2);
    qthLayout->addWidget(gnssSerialLabel, 10, 0);
    qthLayout->addWidget(gnssSerialPortEdit, 10, 1);
    qthLayout->addWidget(gnssSerialBaudSpin, 10, 2);
    qthLayout->addWidget(gnssSerialButton, 11, 0);
    qthLayout->addWidget(gnssNmeaLogButton, 11, 1);
    qthLayout->addWidget(gnssNmeaReplayButton, 11, 2);
    qthLayout->addWidget(gnssSerialRawLogButton, 12, 0);
    qthLayout->addWidget(gnssUbxSystemsButton, 12, 1);
    qthLayout->addWidget(gnssUbxSaveButton, 12, 2);
    qthLayout->addWidget(gnssSerialStatusLabel, 13, 0, 1, 3);
    qthLayout->addWidget(gnssTuneButton, 14, 0);
    qthLayout->addWidget(gnssScanButton, 14, 1);
    qthLayout->addWidget(gnssSelfTestButton, 14, 2);
    qthLayout->addWidget(gnssRawLogButton, 15, 0);
    qthLayout->addWidget(gnssAcquireButton, 15, 1);
    qthLayout->addWidget(gnssDeepAcquireButton, 15, 2);
    qthLayout->addWidget(gnssNetworkTimeButton, 16, 0);
    qthLayout->addWidget(gnssOfflineAcquireButton, 16, 1);
    qthLayout->addWidget(gnssPositionSelfTestButton, 16, 2);
    qthLayout->addWidget(gnssPvtButton, 17, 0, 1, 3);
    qthLayout->addWidget(gnssFilterLabel2, 18, 0);
    qthLayout->addWidget(gnssFilterWidget, 18, 1, 1, 2);
    qthLayout->addWidget(gnssMonitorCheckbox, 19, 0);
    qthLayout->addWidget(gnssPositionPolicyCombo, 19, 1, 1, 2);
    qthLayout->addWidget(gnssTimeZoneLabel, 20, 0);
    qthLayout->addWidget(gnssTimeZoneCombo, 20, 1, 1, 2);
    qthLayout->addWidget(gnssMonitorStatusLabel, 21, 0, 1, 3);
    qthLayout->addWidget(gnssAcquireStatusLabel, 22, 0, 1, 3);
    qthLayout->addWidget(qthStatusLabel, 23, 0, 1, 3);

    dmrHunterControls = new SpectrumHunterControls(
        QStringLiteral("DMR Hunter"),
        QStringLiteral("Detect narrow DMR-like 4FSK channel candidates in the current spectrum"),
        QStringLiteral(" kHz"),
        DmrHunterDetector::MinWidthKhz,
        DmrHunterDetector::MaxWidthKhz,
        dmrHunterSettings.minWidthKhz,
        dmrHunterSettings.maxWidthKhz,
        DmrHunterDetector::MinThresholdDb,
        DmrHunterDetector::MaxThresholdDb,
        dmrHunterSettings.thresholdDb,
        this);
    dmrHunterControls->addPreset(QStringLiteral("Narrow DMR example"), QStringLiteral("430-432\t0.012500"));
    dmrHunterControls->addPreset(QStringLiteral("VHF DMR 160-174"), QStringLiteral("160-174\t0.012500"));
    dmrHunterControls->addPreset(QStringLiteral("UHF DMR 400-470"), QStringLiteral("400-470\t0.012500"));
    dmrHunterControls->addPreset(QStringLiteral("DMR 2m/70cm sparse"), QStringLiteral("160-174\\400-470\t0.012500"));
    dmrHunterControls->setCandidateNavigationVisible(true);

    fpvHunterControls = new SpectrumHunterControls(
        QStringLiteral("FPV Hunter"),
        QStringLiteral("Detect wide FPV-like video carriers in the current spectrum"),
        QStringLiteral(" MHz"),
        FpvHunterDetector::MinWidthMhz,
        FpvHunterDetector::MaxWidthMhz,
        fpvHunterSettings.minWidthMhz,
        fpvHunterSettings.maxWidthMhz,
        FpvHunterDetector::MinThresholdDb,
        FpvHunterDetector::MaxThresholdDb,
        fpvHunterSettings.thresholdDb,
        this);
    fpvHunterControls->addPreset(QStringLiteral("FPV 5.8 analog"), QStringLiteral("5650-5925\t5.000000"));
    fpvHunterControls->addPreset(QStringLiteral("FPV 1.2/1.3"), QStringLiteral("1080-1360\t5.000000"));
    fpvHunterControls->addPreset(QStringLiteral("FPV 2.4"), QStringLiteral("2300-2500\t5.000000"));
    fpvHunterControls->addPreset(QStringLiteral("FPV 3.3"), QStringLiteral("3200-3500\t5.000000"));
    fpvHunterControls->addPreset(QStringLiteral("All FPV sparse"), QStringLiteral("1080-1360\\2300-2500\\3200-3500\\5650-5925\t5.000000"));
    fpvHunterControls->setCandidateNavigationVisible(true);
    fpvHunterControls->setFollowVisible(true);
    fpvHunterControls->setAutoScanVisible(true);
    fpvHunterControls->setAutoScanValues(fpvAutoScanDwellMs,
                                         fpvAutoScanLossTimeoutMs,
                                         fpvAutoScanMinimumLockPercent);

    fpvHunterHistoryLabel = new QLabel(QStringLiteral("Recent FPV:"), this);
    markTranslatable(fpvHunterHistoryLabel, QStringLiteral("recent_fpv"), QStringLiteral("Recent FPV:"));
    fpvHunterHistoryCombo = new QComboBox(this);
    fpvHunterHistoryCombo->setMinimumContentsLength(10);
    fpvHunterHistoryCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    fpvHunterHistoryCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    fpvHunterHistoryTuneButton = new QPushButton(QStringLiteral("Tune"), this);
    markTranslatable(fpvHunterHistoryTuneButton, QStringLiteral("tune"), QStringLiteral("Tune"));
    fpvHunterHistoryTuneButton->setMaximumWidth(58);
    fpvHunterHistoryTuneButton->setToolTip(uiText(QStringLiteral("tune_fpv_event_tooltip"),
                                                  QStringLiteral("Tune to the selected remembered FPV event")));
    fpvHunterHistoryClearButton = new QPushButton(QStringLiteral("Clear"), this);
    markTranslatable(fpvHunterHistoryClearButton, QStringLiteral("clear"), QStringLiteral("Clear"));
    fpvHunterHistoryClearButton->setMaximumWidth(58);
    fpvHunterHistoryClearButton->setToolTip(uiText(QStringLiteral("clear_fpv_events_tooltip"),
                                                   QStringLiteral("Clear remembered FPV events")));
    updateFpvHunterHistoryControls();

    digitalVideoHunterControls = new SpectrumHunterControls(
        QStringLiteral("Digital Video Hunter"),
        QStringLiteral("Detect wide digital video / OFDM-like carriers in the current spectrum"),
        QStringLiteral(" MHz"),
        DigitalVideoHunterDetector::MinWidthMhz,
        DigitalVideoHunterDetector::MaxWidthMhz,
        digitalVideoHunterSettings.minWidthMhz,
        digitalVideoHunterSettings.maxWidthMhz,
        DigitalVideoHunterDetector::MinThresholdDb,
        DigitalVideoHunterDetector::MaxThresholdDb,
        digitalVideoHunterSettings.thresholdDb,
        this);
    digitalVideoHunterControls->addPreset(QStringLiteral("DATV/DVB 1.2/1.3"), QStringLiteral("1080-1360\t2.000000"));
    digitalVideoHunterControls->addPreset(QStringLiteral("DATV/DVB 2.4"), QStringLiteral("2300-2500\t2.000000"));
    digitalVideoHunterControls->addPreset(QStringLiteral("DATV/DVB 3.3"), QStringLiteral("3200-3500\t5.000000"));
    digitalVideoHunterControls->addPreset(QStringLiteral("DATV/DVB 5.8"), QStringLiteral("4900-5925\t5.000000"));
    digitalVideoHunterControls->addPreset(QStringLiteral("Digital video sparse"), QStringLiteral("1080-1360\\2300-2500\\3200-3500\\4900-5925\t10.000000"));
    digitalVideoHunterControls->setCandidateNavigationVisible(true);

    QLabel *bandwidthLabel = new QLabel("Audio Bandwidth:", this);
    markTranslatable(bandwidthLabel, QStringLiteral("audio_bandwidth"), QStringLiteral("Audio Bandwidth:"));
    bandwidthControl = new FrequencyControl(this);
    bandwidthControl->setRangeHz(10.0, 20000000.0);
    bandwidthControl->setStepPresets({
        {"10 Hz", 10.0},
        {"100 Hz", 100.0},
        {"500 Hz", 500.0},
        {"1 kHz", 1000.0},
        {"2.5 kHz", 2500.0},
        {"5 kHz", 5000.0},
        {"10 kHz", 10000.0},
        {"25 kHz", 25000.0},
        {"100 kHz", 100000.0},
        {"1 MHz", 1000000.0},
        {"5 MHz", 5000000.0},
        {"10 MHz", 10000000.0},
    });
    ensureDefaultFrequencyPresets();
    updateFrequencyPresetControls();
    bandwidthControl->setValueHz(pendingSettings.bandwidth);
   
    QStringList modulationNames = {"AM", "NFM", "SAM", "USB", "LSB", "DSB", "CW", "WFM"};
    QVector<int> modulationIds = {MOD_AM, MOD_NFM, MOD_SAM, MOD_USB, MOD_LSB, MOD_DSB,
                                  MOD_CW, MOD_WFM};
    
    QHBoxLayout* row1 = new QHBoxLayout();
    QHBoxLayout* row2 = new QHBoxLayout();
    
    QVBoxLayout *controlsToggleLayout = new QVBoxLayout();
    controlsToggleLayout->addStretch();
    controlsToggleLayout->addWidget(dspPathButton);
    controlsToggleLayout->addWidget(controlsToggleButton);

    scaleLayout->addLayout(controlsToggleLayout, 0);
    scaleLayout->addLayout(contrastLayout, 1);
    scaleLayout->addLayout(sensLayout, 1);
    scaleLayout->addLayout(scaleControlGrid, 4);
    scaleLayout->addLayout(levelMinLayout, 2);
    scaleLayout->addLayout(levelMaxLayout, 2);
    scaleLayout->addLayout(graphToolLayout, 0);

    graphToolLayout->addWidget(digitalToggleButton);
    graphToolLayout->addWidget(videoToggleButton);
    graphToolLayout->addWidget(transmitButton);

    graphLayout->addWidget(graphWidget);
    graphLayout->addWidget(scaleWidget); 
    graphLayout->addWidget(waterfallWidget);
    graphLayout->addLayout(scaleLayout);
    
    for (int i = 0; i < modulationNames.size(); ++i) {
        QRadioButton* radioButton = addModulationRadioButton(controlsWidget,
                                                             i < 4 ? row1 : row2,
                                                             modulationNames[i],
                                                             modulationIds[i],
                                                             QString());
        if (modulationIds[i] == pendingSettings.modulationType) {
            radioButton->setChecked(true); 
        }
    }
    
    auto prepareCompactPanelButton = [](QPushButton *button) {
        if (!button) {
            return;
        }
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    };
    prepareCompactPanelButton(refreshButton);
    prepareCompactPanelButton(fobosButton);
    prepareCompactPanelButton(networkButton);
    prepareCompactPanelButton(appSettingsButton);

    QHBoxLayout *deviceButtonLayout = new QHBoxLayout();
    deviceButtonLayout->setContentsMargins(0, 0, 0, 0);
    deviceButtonLayout->setSpacing(4);
    deviceButtonLayout->addWidget(refreshButton, 1);
    deviceButtonLayout->addWidget(networkButton, 1);
    deviceButtonLayout->addWidget(appSettingsButton, 1);

    QHBoxLayout *gainLabelLayout = new QHBoxLayout();
    lnaGainLabel->setAlignment(Qt::AlignCenter);
    vgaGainLabel->setAlignment(Qt::AlignCenter);
    gainLabelLayout->addWidget(lnaGainLabel, 1);
    gainLabelLayout->addWidget(vgaGainLabel, 4);

    QHBoxLayout *gainSliderLayout = new QHBoxLayout();
    gainSliderLayout->addWidget(lnaGainSlider, 1);
    gainSliderLayout->addWidget(vgaGainSlider, 4);

    QHBoxLayout *rtlGainLayout = new QHBoxLayout();
    rtlGainLayout->setContentsMargins(0, 0, 0, 0);
    rtlGainLayout->setSpacing(4);
    rtlGainLabel->setAlignment(Qt::AlignCenter);
    rtlGainLayout->addWidget(rtlAgcCheckbox, 0);
    rtlGainLayout->addWidget(rtlGainLabel, 0);
    rtlGainLayout->addWidget(rtlGainSlider, 1);

    QGridLayout *hfNoiseCancelLayout = new QGridLayout();
    hfNoiseCancelLayout->setColumnStretch(1, 1);
    hfNoiseCancelLayout->addWidget(hfInterferenceBaselineCheckbox, 0, 0);
    hfNoiseCancelLayout->addWidget(hfInterferenceRawOverlayCheckbox, 0, 1);
    hfNoiseCancelLayout->addWidget(hfInterferenceBaselineLearnButton, 0, 2);
    hfNoiseCancelLayout->addWidget(hfInterferenceBaselineClearButton, 0, 3);
    hfNoiseCancelLayout->addWidget(hfInterferenceDefaultsButton, 0, 4);
    hfNoiseCancelLayout->addWidget(hfInterferenceBaselineStatusLabel, 1, 0, 1, 4);
    hfNoiseCancelLayout->addWidget(hfInterferenceAnalyzeButton, 1, 4);
    hfNoiseCancelLayout->addWidget(hfInterferenceBaselineDepthLabel, 2, 0);
    hfNoiseCancelLayout->addWidget(hfInterferenceBaselineDepthSlider, 2, 1, 1, 4);
    hfNoiseCancelLayout->addWidget(hfInterferenceBaselineSmoothLabel, 3, 0);
    hfNoiseCancelLayout->addWidget(hfInterferenceBaselineSmoothSlider, 3, 1, 1, 4);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelDepthLabel, 4, 0);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelDepthSlider, 4, 1, 1, 3);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelFreezeCheckbox, 4, 4);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelRefGainLabel, 5, 0);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelRefGainSlider, 5, 1, 1, 4);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelRefDelayLabel, 6, 0);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelRefDelaySlider, 6, 1, 1, 4);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelRefTiltLabel, 7, 0);
    hfNoiseCancelLayout->addWidget(hfNoiseCancelRefTiltSlider, 7, 1, 1, 4);
    hfNoiseCancelLayout->addWidget(hfAudioBlankerCheckbox, 8, 0);
    hfNoiseCancelLayout->addWidget(hfAudioBlankerThresholdLabel, 8, 1);
    hfNoiseCancelLayout->addWidget(hfAudioBlankerThresholdSlider, 8, 2, 1, 3);

    QHBoxLayout *recordingLayout = new QHBoxLayout();
    recordingLayout->addWidget(recordingModeCombo);
    recordingLayout->addWidget(recordButton);

    QVBoxLayout *spectrumFrameRecordingLayout = new QVBoxLayout();
    QHBoxLayout *spectrumFrameRecordingModeLayout = new QHBoxLayout();
    spectrumFrameRecordingModeLayout->setContentsMargins(0, 0, 0, 0);
    spectrumFrameRecordingModeLayout->setSpacing(4);
    spectrumFrameRecordingModeLayout->addWidget(spectrumFrameBufferCheckbox);
    spectrumFrameRecordingModeLayout->addWidget(spectrumEventModeCombo, 1);
    QHBoxLayout *spectrumFrameRecordingActionLayout = new QHBoxLayout();
    spectrumFrameRecordingActionLayout->setContentsMargins(0, 0, 0, 0);
    spectrumFrameRecordingActionLayout->setSpacing(4);
    spectrumFrameRecordingActionLayout->addWidget(spectrumFrameRecordButton);
    spectrumFrameRecordingActionLayout->addWidget(spectrumFrameReplayButton);
    QLabel *spectrumFramePreLabel = new QLabel(QStringLiteral("Pre:"), this);
    markTranslatable(spectrumFramePreLabel, QStringLiteral("prebuffer_short"), QStringLiteral("Pre:"));
    spectrumFrameRecordingActionLayout->addWidget(spectrumFramePreLabel);
    spectrumFrameRecordingActionLayout->addWidget(spectrumFramePrebufferSpin);
    QLabel *spectrumFrameBinsLabel = new QLabel(QStringLiteral("Bins:"), this);
    markTranslatable(spectrumFrameBinsLabel, QStringLiteral("bins_short"), QStringLiteral("Bins:"));
    spectrumFrameRecordingActionLayout->addWidget(spectrumFrameBinsLabel);
    spectrumFrameRecordingActionLayout->addWidget(spectrumFrameBinsCombo);
    spectrumFrameRecordingLayout->addLayout(spectrumFrameRecordingModeLayout);
    spectrumFrameRecordingLayout->addLayout(spectrumFrameRecordingActionLayout);

    QHBoxLayout *playbackButtonLayout = new QHBoxLayout();
    playbackButtonLayout->addWidget(playbackRefreshButton);
    playbackButtonLayout->addWidget(playbackButton);

    auto prepareReceiverCombo = [](QComboBox *combo) {
        if (!combo) {
            return;
        }
        combo->setMinimumContentsLength(8);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    };
    prepareReceiverCombo(clkBox);
    prepareReceiverCombo(modeBox);
    prepareReceiverCombo(fftComboBox);
    prepareReceiverCombo(sampleBox);

    QGridLayout *receiverLeftColumnLayout = new QGridLayout();
    receiverLeftColumnLayout->setContentsMargins(0, 0, 0, 0);
    receiverLeftColumnLayout->setHorizontalSpacing(4);
    receiverLeftColumnLayout->setVerticalSpacing(2);
    QGridLayout *receiverRightColumnLayout = new QGridLayout();
    receiverRightColumnLayout->setContentsMargins(0, 0, 0, 0);
    receiverRightColumnLayout->setHorizontalSpacing(4);
    receiverRightColumnLayout->setVerticalSpacing(2);
    for (QLabel *label : {clockSourceLabel, inputModeLabel, fftLabel, sampleRateLabel}) {
        if (label) label->setMinimumWidth(0);
    }
    receiverLeftColumnLayout->addWidget(clockSourceLabel, 0, 0);
    receiverLeftColumnLayout->addWidget(clkBox, 0, 1);
    receiverLeftColumnLayout->addWidget(inputModeLabel, 1, 0);
    receiverLeftColumnLayout->addWidget(modeBox, 1, 1);
    receiverLeftColumnLayout->addWidget(fftBinWidthModeCheckbox, 2, 0, 1, 2);
    receiverLeftColumnLayout->setColumnStretch(1, 1);

    receiverRightColumnLayout->addWidget(fftLabel, 0, 0);
    receiverRightColumnLayout->addWidget(fftComboBox, 0, 1);
    receiverRightColumnLayout->addWidget(sampleRateLabel, 1, 0);
    receiverRightColumnLayout->addWidget(sampleBox, 1, 1);
    receiverRightColumnLayout->addWidget(fftBinWidthSpin, 2, 0, 1, 2);
    receiverRightColumnLayout->setColumnStretch(1, 1);

    fftBinWidthModeCheckbox->setMinimumWidth(0);
    fftBinWidthModeCheckbox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    fftBinWidthSpin->setMinimumWidth(0);
    fftBinWidthSpin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    QWidget *receiverLeftColumn = new QWidget(this);
    receiverLeftColumn->setLayout(receiverLeftColumnLayout);
    receiverLeftColumn->setMinimumWidth(0);
    receiverLeftColumn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    QWidget *receiverRightColumn = new QWidget(this);
    receiverRightColumn->setLayout(receiverRightColumnLayout);
    receiverRightColumn->setMinimumWidth(0);
    receiverRightColumn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    QHBoxLayout *receiverRowsLayout = new QHBoxLayout();
    receiverRowsLayout->setContentsMargins(0, 0, 0, 0);
    receiverRowsLayout->setSpacing(4);
    receiverRowsLayout->addWidget(receiverLeftColumn, 1);
    receiverRowsLayout->addWidget(receiverRightColumn, 1);

    QVBoxLayout *fineTuneLayout = new QVBoxLayout();
    fineTuneLayout->setSpacing(2);
    QWidget *fineTuneDialPage = new QWidget(this);
    QVBoxLayout *fineTuneDialPageLayout = new QVBoxLayout(fineTuneDialPage);
    fineTuneDialPageLayout->setContentsMargins(0, 0, 0, 0);
    fineTuneDialPageLayout->addWidget(fineTuneDial, 0, Qt::AlignHCenter);
    QWidget *fineTuneScalePage = new QWidget(this);
    QVBoxLayout *fineTuneScalePageLayout = new QVBoxLayout(fineTuneScalePage);
    fineTuneScalePageLayout->setContentsMargins(0, 0, 0, 0);
    fineTuneScalePageLayout->addWidget(fineTuneScaleWidget);
    fineTuneStack->addWidget(fineTuneScalePage);
    fineTuneStack->addWidget(fineTuneDialPage);
    QHBoxLayout *fineTuneHeaderLayout = new QHBoxLayout();
    fineTuneHeaderLayout->setContentsMargins(0, 0, 0, 0);
    fineTuneHeaderLayout->setSpacing(4);
    fineTuneHeaderLayout->addWidget(fineTuneLabel, 1);
    fineTuneHeaderLayout->addWidget(fineTuneScaleModeButton, 0, Qt::AlignRight | Qt::AlignVCenter);
    fineTuneLayout->addLayout(fineTuneHeaderLayout);
    fineTuneLayout->addWidget(fineTuneStack);
    QHBoxLayout *presetZoomLayout = new QHBoxLayout();
    presetZoomLayout->setContentsMargins(0, 0, 0, 0);
    presetZoomLayout->setSpacing(4);
    presetZoomLayout->addWidget(presetManagerButton, 1);
    presetZoomLayout->addWidget(zoomSpectrumButton, 0);
    presetZoomLayout->addWidget(zoomDensityButton, 0);
    fineTuneLayout->addLayout(presetZoomLayout);

    QVBoxLayout *receiverControlLayout = new QVBoxLayout();
    receiverControlLayout->setContentsMargins(0, 0, 0, 0);
    receiverControlLayout->setSpacing(3);
    receiverControlLayout->addLayout(receiverRowsLayout);
    receiverControlLayout->addLayout(fineTuneLayout);

    QWidget *quickHamContainer = new QWidget(this);
    quickHamContainer->setMinimumWidth(0);
    quickHamContainer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    QVBoxLayout *quickHamContainerLayout = new QVBoxLayout(quickHamContainer);
    quickHamContainerLayout->setContentsMargins(0, 0, 0, 0);
    quickHamContainerLayout->setSpacing(2);
    QToolButton *quickHamHeader = new QToolButton(quickHamContainer);
    quickHamHeader->setCheckable(true);
    QSettings quickHamSettings(persistentSettingsFilePath(), QSettings::IniFormat);
    const bool quickHamExpanded =
        quickHamSettings.value(QStringLiteral("uiSections/quickHamExpanded"), true).toBool();
    quickHamHeader->setChecked(quickHamExpanded);
    quickHamHeader->setArrowType(quickHamExpanded ? Qt::DownArrow : Qt::RightArrow);
    quickHamHeader->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    quickHamHeader->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    markTranslatable(quickHamHeader,
                     QStringLiteral("quick_ham_panel"),
                     QStringLiteral("Quick HAM tuning"));
    quickHamContainerLayout->addWidget(quickHamHeader);
    QWidget *quickHamPanel = new QWidget(quickHamContainer);
    quickHamPanel->setObjectName(QStringLiteral("quickHamPanel"));
    quickHamPanel->setMinimumWidth(0);
    quickHamPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    QVBoxLayout *quickHamLayout = new QVBoxLayout(quickHamPanel);
    quickHamLayout->setContentsMargins(0, 2, 0, 2);
    quickHamLayout->setSpacing(2);
    QLabel *quickHamBandsLabel = new QLabel(QStringLiteral("Amateur bands:"), quickHamPanel);
    markTranslatable(quickHamBandsLabel,
                     QStringLiteral("quick_ham_bands"),
                     QStringLiteral("Amateur bands:"));
    quickHamLayout->addWidget(quickHamBandsLabel);
    QGridLayout *quickHamBandsLayout = new QGridLayout();
    quickHamBandsLayout->setContentsMargins(0, 0, 0, 0);
    quickHamBandsLayout->setHorizontalSpacing(2);
    quickHamBandsLayout->setVerticalSpacing(2);
    quickHamBandButtonGroup = new QButtonGroup(this);
    quickHamBandButtonGroup->setExclusive(false);
    struct QuickHamBand {
        const char *name;
        double lowMhz;
        double highMhz;
        int defaultMode;
    };
    const std::array<QuickHamBand, 17> quickHamBands = {{
        {"2200m", 0.1357, 0.1378, MOD_LSB}, {"630m", 0.472, 0.479, MOD_LSB},
        {"160m", 1.81, 2.0, MOD_LSB},     {"80m", 3.5, 3.8, MOD_LSB},
        {"60m", 5.3515, 5.3665, MOD_USB}, {"40m", 7.0, 7.2, MOD_LSB},
        {"30m", 10.1, 10.15, MOD_USB},    {"20m", 14.0, 14.35, MOD_USB},
        {"17m", 18.068, 18.168, MOD_USB}, {"15m", 21.0, 21.45, MOD_USB},
        {"12m", 24.89, 24.99, MOD_USB},   {"10m", 28.0, 29.7, MOD_USB},
        {"6m", 50.0, 52.0, MOD_USB},      {"4m", 70.0, 70.5, MOD_NFM},
        {"2m", 144.0, 146.0, MOD_NFM},    {"70cm", 430.0, 440.0, MOD_NFM},
        {"23cm", 1240.0, 1300.0, MOD_NFM},
    }};
    const QString quickHamButtonStyle = QStringLiteral(
        "QToolButton { padding:2px 3px; border:1px solid #5c626b; }"
        "QToolButton:checked { background:#247a48; color:white; border-color:#5fce88; font-weight:600; }");
    for (int index = 0; index < static_cast<int>(quickHamBands.size()); ++index) {
        const QuickHamBand &band = quickHamBands[static_cast<size_t>(index)];
        QToolButton *button = new QToolButton(quickHamPanel);
        button->setText(QString::fromLatin1(band.name));
        button->setCheckable(true);
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        button->setStyleSheet(quickHamButtonStyle);
        button->setProperty("bandLowHz", band.lowMhz * 1.0e6);
        button->setProperty("bandHighHz", band.highMhz * 1.0e6);
        button->setProperty("defaultModulation", band.defaultMode);
        button->setToolTip(QStringLiteral("%1-%2 MHz")
                               .arg(band.lowMhz, 0, 'f', band.lowMhz < 1.0 ? 4 : 3)
                               .arg(band.highMhz, 0, 'f', band.highMhz < 1.0 ? 4 : 3));
        quickHamBandButtonGroup->addButton(button, index);
        quickHamBandsLayout->addWidget(button, index / 4, index % 4);
    }
    for (int column = 0; column < 4; ++column) quickHamBandsLayout->setColumnStretch(column, 1);
    quickHamLayout->addLayout(quickHamBandsLayout);

    QLabel *quickHamModesLabel = new QLabel(QStringLiteral("Demodulation:"), quickHamPanel);
    markTranslatable(quickHamModesLabel,
                     QStringLiteral("quick_ham_demodulation"),
                     QStringLiteral("Demodulation:"));
    quickHamLayout->addWidget(quickHamModesLabel);
    QGridLayout *quickHamModesLayout = new QGridLayout();
    quickHamModesLayout->setContentsMargins(0, 0, 0, 0);
    quickHamModesLayout->setHorizontalSpacing(2);
    quickHamModesLayout->setVerticalSpacing(2);
    quickHamModeButtonGroup = new QButtonGroup(this);
    quickHamModeButtonGroup->setExclusive(false);
    const std::array<std::pair<const char *, int>, 8> quickHamModes = {{
        {"AM", MOD_AM}, {"NFM", MOD_NFM}, {"SAM", MOD_SAM}, {"USB", MOD_USB},
        {"LSB", MOD_LSB}, {"DSB", MOD_DSB}, {"CW", MOD_CW}, {"WFM", MOD_WFM},
    }};
    for (int index = 0; index < static_cast<int>(quickHamModes.size()); ++index) {
        QToolButton *button = new QToolButton(quickHamPanel);
        button->setText(QString::fromLatin1(quickHamModes[static_cast<size_t>(index)].first));
        button->setCheckable(true);
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        button->setStyleSheet(quickHamButtonStyle);
        const int modulation = quickHamModes[static_cast<size_t>(index)].second;
        quickHamModeButtonGroup->addButton(button, modulation);
        quickHamModesLayout->addWidget(button, index / 4, index % 4);
    }
    for (int column = 0; column < 4; ++column) quickHamModesLayout->setColumnStretch(column, 1);
    quickHamLayout->addLayout(quickHamModesLayout);
    quickHamPanel->setVisible(quickHamExpanded);
    quickHamContainerLayout->addWidget(quickHamPanel);
    connect(quickHamHeader, &QToolButton::toggled, quickHamContainer,
            [quickHamHeader, quickHamPanel](bool checked) {
                quickHamHeader->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
                quickHamPanel->setVisible(checked);
                QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
                settings.setValue(QStringLiteral("uiSections/quickHamExpanded"), checked);
            });

    connect(quickHamBandButtonGroup,
            QOverload<int>::of(&QButtonGroup::idClicked),
            this,
            [this](int id) {
                QAbstractButton *button = quickHamBandButtonGroup
                                              ? quickHamBandButtonGroup->button(id)
                                              : nullptr;
                if (!button) return;
                const double centerHz =
                    0.5 * (button->property("bandLowHz").toDouble() +
                           button->property("bandHighHz").toDouble());
                const int modulation = button->property("defaultModulation").toInt();
                if (modulationButtonGroup) {
                    if (QAbstractButton *modeButton = modulationButtonGroup->button(modulation)) {
                        const QSignalBlocker blocker(modulationButtonGroup);
                        modeButton->setChecked(true);
                    }
                }
                if (pendingSettings.modulationType != modulation) onModulationChanged(modulation);
                updateTuningFromScale(centerHz, centerHz);
                savePersistentSettings();
                updateQuickHamPanel();
            });
    connect(quickHamModeButtonGroup,
            QOverload<int>::of(&QButtonGroup::idClicked),
            this,
            [this](int modulation) {
                if (modulationButtonGroup) {
                    if (QAbstractButton *button = modulationButtonGroup->button(modulation)) {
                        const QSignalBlocker blocker(modulationButtonGroup);
                        button->setChecked(true);
                    }
                }
                if (pendingSettings.modulationType != modulation) onModulationChanged(modulation);
                updateQuickHamPanel();
            });

    QHBoxLayout *startStopLayout = new QHBoxLayout();
    startStopLayout->addWidget(startButton, 2);
    startStopLayout->addWidget(stopButton, 1);

    QHBoxLayout *centralFrequencyHeaderLayout = new QHBoxLayout();
    centralFrequencyHeaderLayout->addWidget(centralFrequencyLabel);
    centralFrequencyHeaderLayout->addStretch();

    QHBoxLayout *listeningFrequencyHeaderLayout = new QHBoxLayout();
    listeningFrequencyHeaderLayout->addWidget(listeningFrequencyLabel);
    listeningFrequencyHeaderLayout->addStretch();

    struct CollapsibleSection {
        QWidget *widget = nullptr;
        QVBoxLayout *contentLayout = nullptr;
    };
    auto createCollapsibleSection = [this](const QString &key, const QString &fallback, bool expanded) -> CollapsibleSection {
        const QString settingsKey = QStringLiteral("uiSections/%1Expanded").arg(key);
        QSettings sectionSettings(persistentSettingsFilePath(), QSettings::IniFormat);
        const bool isExpanded = sectionSettings.value(settingsKey, expanded).toBool();
        QWidget *section = new QWidget(this);
        QVBoxLayout *sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(2);

        QToolButton *header = new QToolButton(section);
        header->setObjectName(QStringLiteral("uiSectionHeader_%1").arg(key));
        header->setCheckable(true);
        header->setChecked(isExpanded);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setArrowType(isExpanded ? Qt::DownArrow : Qt::RightArrow);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        markTranslatable(header, key, fallback);

        QWidget *content = new QWidget(section);
        QVBoxLayout *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(12, 0, 0, 4);
        contentLayout->setSpacing(4);
        content->setVisible(isExpanded);

        connect(header, &QToolButton::toggled, section, [header, content, settingsKey](bool checked) {
            header->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
            content->setVisible(checked);
            QSettings sectionSettings(persistentSettingsFilePath(), QSettings::IniFormat);
            sectionSettings.setValue(settingsKey, checked);
        });

        sectionLayout->addWidget(header);
        sectionLayout->addWidget(content);
        return {section, contentLayout};
    };

    CollapsibleSection deviceSection = createCollapsibleSection(QStringLiteral("device"), QStringLiteral("Device"), true);
    deviceSection.contentLayout->addLayout(deviceButtonLayout);
    deviceSection.contentLayout->addWidget(comboBox);

    CollapsibleSection receiverSection = createCollapsibleSection(QStringLiteral("receiver"), QStringLiteral("Receiver"), true);
    receiverSection.contentLayout->addLayout(receiverControlLayout);
    receiverSection.contentLayout->addLayout(centralFrequencyHeaderLayout);
    receiverSection.contentLayout->addWidget(frequencyControl);
    receiverSection.contentLayout->addLayout(listeningFrequencyHeaderLayout);
    receiverSection.contentLayout->addWidget(listeningFrequencyControl);
    receiverSection.contentLayout->addWidget(quickHamContainer);
    receiverSection.contentLayout->addLayout(gainLabelLayout);
    receiverSection.contentLayout->addLayout(gainSliderLayout);
    receiverSection.contentLayout->addLayout(rtlGainLayout);
    receiverSection.contentLayout->addWidget(hackRfSettingsButton);
    receiverSection.contentLayout->addLayout(startStopLayout);

    multiVfoWidget = new MultiVfoWidget(this);
    multiVfoWidget->setLanguage(uiLanguage == QStringLiteral("uk"));
    multiVfoWidget->setReceiverContext(pendingSettings.centerFrequency,
                                       pendingSettings.sampleRate,
                                       pendingSettings.listeningFrequency,
                                       pendingSettings.bandwidth,
                                       pendingSettings.modulationType);
    connect(multiVfoWidget, &MultiVfoWidget::monitorRequested, this,
            [this](double frequencyHz, double bandwidthHz, int modulationType) {
                if (!std::isfinite(frequencyHz) || !std::isfinite(bandwidthHz)) return;
                if (pendingSettings.modulationType != modulationType) {
                    if (modulationButtonGroup) {
                        if (QAbstractButton *button = modulationButtonGroup->button(modulationType)) {
                            const QSignalBlocker blocker(modulationButtonGroup);
                            button->setChecked(true);
                        }
                    }
                    onModulationChanged(modulationType);
                }
                pendingSettings.bandwidth = (std::clamp)(bandwidthHz, 10.0, 20000000.0);
                if (bandwidthControl) {
                    const QSignalBlocker blocker(bandwidthControl);
                    bandwidthControl->setValueHz(pendingSettings.bandwidth);
                }
                updateTuningFromScale(frequencyHz, pendingSettings.centerFrequency);
                savePersistentSettings();
            });
    connect(multiVfoWidget, &MultiVfoWidget::configurationChanged, this, [this]() {
        if (persistentSettingsReady) savePersistentSettings();
    });
    CollapsibleSection multiVfoSection = createCollapsibleSection(
        QStringLiteral("multi_vfo"), QStringLiteral("Multi-VFO / channelizer"), false);
    multiVfoSection.contentLayout->addWidget(multiVfoWidget);

    CollapsibleSection hfCancelSection = createCollapsibleSection(QStringLiteral("hf_interference_lab_section"), QStringLiteral("HF interference lab"), false);
    hfCancelSection.contentLayout->addLayout(hfNoiseCancelLayout);

    CollapsibleSection scanSection = createCollapsibleSection(QStringLiteral("scan"), QStringLiteral("Scan"), false);
    scanSection.contentLayout->addLayout(scanVisualModeLayout);
    scanSection.contentLayout->addWidget(agileScanBox);
    scanSection.contentLayout->addWidget(standardScanBox);

    CollapsibleSection spectrumMeasurementSection = createCollapsibleSection(QStringLiteral("spectrum_measurement"), QStringLiteral("Spectrum measurement"), false);
    spectrumMeasurementSection.contentLayout->addWidget(spectrumMeasurementBox);

    CollapsibleSection listeningScanSection = createCollapsibleSection(QStringLiteral("listening_scan"), QStringLiteral("Listening scan"), false);
    listeningScanSection.contentLayout->addWidget(listeningScanBox);

    CollapsibleSection qthSection = createCollapsibleSection(QStringLiteral("gps_qth"), QStringLiteral("GPS / QTH"), false);
    qthSection.contentLayout->addWidget(qthBox);

    CollapsibleSection dmrHunterSection = createCollapsibleSection(QStringLiteral("dmr_hunter"), QStringLiteral("DMR Hunter"), false);
    dmrHunterSection.contentLayout->addWidget(dmrHunterControls);

    CollapsibleSection fpvHunterSection = createCollapsibleSection(QStringLiteral("fpv_hunter"), QStringLiteral("FPV Hunter"), false);
    fpvHunterSection.contentLayout->addWidget(fpvHunterControls);
    QHBoxLayout *fpvHistoryRow = new QHBoxLayout();
    fpvHistoryRow->setContentsMargins(0, 0, 0, 0);
    fpvHistoryRow->setSpacing(4);
    fpvHistoryRow->addWidget(fpvHunterHistoryLabel);
    fpvHistoryRow->addWidget(fpvHunterHistoryCombo, 1);
    QHBoxLayout *fpvHistoryButtonRow = new QHBoxLayout();
    fpvHistoryButtonRow->setContentsMargins(0, 0, 0, 0);
    fpvHistoryButtonRow->setSpacing(4);
    fpvHistoryButtonRow->addStretch(1);
    fpvHistoryButtonRow->addWidget(fpvHunterHistoryTuneButton);
    fpvHistoryButtonRow->addWidget(fpvHunterHistoryClearButton);
    fpvHunterSection.contentLayout->addLayout(fpvHistoryRow);
    fpvHunterSection.contentLayout->addLayout(fpvHistoryButtonRow);

    CollapsibleSection digitalVideoHunterSection = createCollapsibleSection(QStringLiteral("digital_video_hunter"), QStringLiteral("Digital Video Hunter"), false);
    digitalVideoHunterSection.contentLayout->addWidget(digitalVideoHunterControls);

    CollapsibleSection gpioSection = createCollapsibleSection(QStringLiteral("gpio"), QStringLiteral("GPIO"), false);
    gpioSection.contentLayout->addLayout(checkboxLayout);

    CollapsibleSection audioSection = createCollapsibleSection(QStringLiteral("audio_demod"), QStringLiteral("Audio / demod"), false);
    audioSection.contentLayout->addWidget(volumeLabel);
    audioSection.contentLayout->addWidget(volumeSlider);
    audioSection.contentLayout->addWidget(audioLowPassLabel);
    audioSection.contentLayout->addWidget(audioLowPassSlider);
    audioSection.contentLayout->addWidget(audioHighPassLabel);
    audioSection.contentLayout->addWidget(audioHighPassSlider);
    audioSection.contentLayout->addWidget(bandwidthLabel);
    audioSection.contentLayout->addWidget(bandwidthControl);
    audioSection.contentLayout->addLayout(row1);
    audioSection.contentLayout->addLayout(row2);
    audioSection.contentLayout->addWidget(cwDecoderControlsWidget);
    audioSection.contentLayout->addWidget(audioDeviceComboBox);

    audioFilterChainWidget = new AudioFilterChainWidget(this);
    audioFilterChainWidget->setLanguage(uiLanguage == QStringLiteral("uk"));
    connect(audioFilterChainWidget,
            &AudioFilterChainWidget::configurationChanged,
            this,
            [this](const QString &json) {
                pendingSettings.audioFilterChainJson = json;
                if (audioProcessor) {
                    audioProcessor->configure(audioProcessorSettings());
                }
                savePersistentSettings();
                if (isNetworkClientMode()) {
                    scheduleRemoteSettingsCommand();
                }
            });
    CollapsibleSection audioFilterChainSection = createCollapsibleSection(
        QStringLiteral("audio_filter_chain"),
        QStringLiteral("Audio filter chain"),
        false);
    audioFilterChainSection.contentLayout->addWidget(audioFilterChainWidget);

    CollapsibleSection recordingSection = createCollapsibleSection(QStringLiteral("recording_playback"), QStringLiteral("Recording / playback"), false);
    recordingSection.contentLayout->addWidget(recordingStatusLabel);
    recordingSection.contentLayout->addLayout(recordingLayout);
    recordingSection.contentLayout->addWidget(spectrumFrameRecordingStatusLabel);
    recordingSection.contentLayout->addLayout(spectrumFrameRecordingLayout);
    recordingSection.contentLayout->addWidget(playbackStatusLabel);
    recordingSection.contentLayout->addWidget(playbackFileCombo);
    recordingSection.contentLayout->addLayout(playbackButtonLayout);

    CollapsibleSection waterfall3DSection = createCollapsibleSection(
        QStringLiteral("waterfall_3d_section"),
        QStringLiteral("Waterfall visualization"),
        false);
    waterfall3DSection.contentLayout->addLayout(waterfallDisplayModeLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DResolutionLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DSurfaceLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DHistoryRowsLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DSliceStepLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DSliceWidthLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DSpectrumSliceStepLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DSpectrumSliceRowsLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DSpectrumSliceCaptureLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DVncSliceInputLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DGradientLayout);
    waterfall3DSection.contentLayout->addLayout(waterfall3DMonochromeStyleLayout);
    waterfall3DSection.contentLayout->addLayout(alternativeSpectrumGradientOpacityLayout);

    layout->addWidget(deviceSection.widget);
    layout->addWidget(receiverSection.widget);
    layout->addWidget(multiVfoSection.widget);
    layout->addWidget(audioSection.widget);
    layout->addWidget(audioFilterChainSection.widget);
    layout->addWidget(hfCancelSection.widget);
    layout->addWidget(scanSection.widget);
    layout->addWidget(spectrumMeasurementSection.widget);
    layout->addWidget(listeningScanSection.widget);
    layout->addWidget(qthSection.widget);
    layout->addWidget(dmrHunterSection.widget);
    layout->addWidget(fpvHunterSection.widget);
    layout->addWidget(digitalVideoHunterSection.widget);
    layout->addWidget(gpioSection.widget);
    layout->addWidget(recordingSection.widget);
    layout->addWidget(waterfall3DSection.widget);
    layout->addStretch(1);

    controlsWidget->setLayout(layout);
    centralWidget->setLayout(graphLayout);
    graphLayout->setStretch(0, 2);
    graphLayout->setStretch(1, 0);
    graphLayout->setStretch(2, 5);
    graphLayout->setStretch(3, 0);
    
    scaleWidget->setTuning(
        listeningFrequency,
        globalFrequency,
        effectiveDemodulationBandwidth(globalBandwidth,
                                       globalModulationType,
                                       pendingSettings.sstvDemodulationMode),
        effectiveDemodulationModulationType(globalModulationType,
                                            pendingSettings.sstvDemodulationMode));
    scaleWidget->setMarkerPosition(0.5);
    scaleWidget->setRange(minFrequency, maxFrequency);

    updateTimer = new QTimer(this);
    updateTimer->setTimerType(Qt::PreciseTimer);
    updateSpectrumTimerInterval();
    stopPollTimer = new QTimer(this);
    stopPollTimer->setInterval(100);
    streamWatchdogTimer = new QTimer(this);
    streamWatchdogTimer->setInterval(250);
    standardScanAdvanceTimer = new QTimer(this);
    standardScanAdvanceTimer->setTimerType(Qt::PreciseTimer);
    standardScanAdvanceTimer->setInterval(10);
    listeningScanAdvanceTimer = new QTimer(this);
    listeningScanAdvanceTimer->setTimerType(Qt::PreciseTimer);
    listeningScanAdvanceTimer->setInterval(25);
    videoSnapshotTimer = new QTimer(this);
    videoSnapshotTimer->setInterval(VIDEO_SNAPSHOT_INTERVAL_MS);
    agileLiveRetuneTimer = new QTimer(this);
    agileLiveRetuneTimer->setSingleShot(true);
    gnssContinuousAcquireTimer = new QTimer(this);
    gnssContinuousAcquireTimer->setSingleShot(true);
    persistentSettingsSaveTimer = new QTimer(this);
    persistentSettingsSaveTimer->setSingleShot(true);
    
    connect(updateTimer, &QTimer::timeout, this, &YourClassName::updateSpectrum);
    connect(stopPollTimer, &QTimer::timeout, this, &YourClassName::pollStopCompletion);
    connect(streamWatchdogTimer, &QTimer::timeout, this, &YourClassName::checkStreamStartup);
    connect(standardScanAdvanceTimer, &QTimer::timeout, this, &YourClassName::advanceStandardScanIfNeeded);
    connect(listeningScanAdvanceTimer, &QTimer::timeout, this, &YourClassName::advanceListeningScanIfNeeded);
    connect(videoSnapshotTimer, &QTimer::timeout, this, &YourClassName::processVideoSnapshotFrame);
    connect(agileLiveRetuneTimer, &QTimer::timeout, this, &YourClassName::flushQueuedLiveAgileCenterRetune);
    connect(gnssContinuousAcquireTimer, &QTimer::timeout, this, &YourClassName::handleGnssContinuousAcquisitionTick);
    connect(persistentSettingsSaveTimer, &QTimer::timeout, this, &YourClassName::flushPendingPersistentSettingsSave);
    connect(graphCheckbox, &QCheckBox::toggled, this, &YourClassName::doubleGraphEnable);
    connect(colorCheckbox, &QCheckBox::toggled, this, &YourClassName::colorGraphEnable);
    connect(syncCheckbox, &QCheckBox::toggled, this, &YourClassName::syncEnable);
    connect(audioDeviceComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onAudioDeviceChanged(int)));
    connect(modulationButtonGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, &YourClassName::onModulationChanged);
    connect(scaleSlider, &QSlider::valueChanged, this, &YourClassName::onScaleChanged);
    connect(additionalScaleDivisorSlider,
            &QSlider::valueChanged,
            this,
            &YourClassName::onAdditionalScaleDivisorChanged);
    connect(frequencyControl, &FrequencyControl::valueCommitted, this, [this](double) {
        onFrequencyEntered();
    });
    connect(fftComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(onfftLengthEntered()));
    connect(fftBinWidthModeCheckbox,
            &QCheckBox::toggled,
            this,
            &YourClassName::onFftBinWidthModeChanged);
    connect(fftBinWidthSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &YourClassName::onFftBinWidthChanged);
    connect(listeningFrequencyControl, &FrequencyControl::valueCommitted, this, [this](double) {
        onListeningFrequencyEntered();
    });
    connect(lnaGainSlider, &QSlider::valueChanged, this, &YourClassName::onLnaGainChanged);
    connect(vgaGainSlider, &QSlider::valueChanged, this, &YourClassName::onVgaGainChanged);
    connect(rtlAgcCheckbox, &QCheckBox::stateChanged, this, &YourClassName::onRtlAgcChanged);
    connect(rtlGainSlider, &QSlider::valueChanged, this, &YourClassName::onRtlGainChanged);
    connect(contrastSlider, &QSlider::valueChanged, this, &YourClassName::onContrastChanged);
    connect(sensitivitySlider, &QSlider::valueChanged, this, &YourClassName::onSensitivityChanged);
    connect(levelMinSlider, &QSlider::valueChanged, this, &YourClassName::onLevelMinChanged);
    connect(levelMaxSlider, &QSlider::valueChanged, this, &YourClassName::onLevelMaxChanged);
    connect(fineTuneDial, &QDial::valueChanged, this, &YourClassName::onFineTuneDialChanged);
    connect(fineTuneDial, &QDial::sliderReleased, this, &YourClassName::onFineTuneDialReleased);
    connect(fineTuneScaleWidget, &FineTuneScaleWidget::fineTuneDelta, this, [this](double deltaHz) {
        applyListeningFrequencyDelta(deltaHz, 60);
    });
    connect(fineTuneScaleWidget, &FineTuneScaleWidget::holdOffsetModeChanged, this, [this](bool enabled) {
        fineTuneScaleHoldMode = enabled;
        updateFineTuneScaleModeButton();
        savePersistentSettings();
    });
    connect(fineTuneScaleModeButton, &QToolButton::toggled, this, [this](bool checked) {
        fineTuneScaleHoldMode = checked;
        if (fineTuneScaleWidget) {
            fineTuneScaleWidget->setHoldOffsetMode(checked);
        }
        updateFineTuneScaleModeButton();
        savePersistentSettings();
    });
    connect(scaleSlider, &QSlider::sliderReleased, this, [this]() {
        if (isNetworkClientMode() && !isFullIqProcessingMode()) {
            scheduleRemoteSettingsCommand();
        }
    });
    connect(additionalScaleDivisorSlider, &QSlider::sliderReleased, this, [this]() {
        if (isNetworkClientMode() && !isFullIqProcessingMode()) {
            scheduleRemoteSettingsCommand();
        }
    });
    connect(volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        volumePercent = value;
        volumeLabel->setText(QStringLiteral("%1: %2%").arg(uiText(QStringLiteral("volume"), QStringLiteral("Volume"))).arg(value));

        const float volume = value / 100.0f;

        if (audioProcessor) {
            audioProcessor->setVolume(volume);
        }

        if (remoteAudioPlayer) {
            remoteAudioPlayer->setVolume(volume);
        }
    });
    connect(audioLowPassSlider, &QSlider::valueChanged, this, [this](int value) {
        pendingSettings.audioLowPassHz = audioLowPassSliderValueToHz(value);
        updateAudioFilterLabels();
        publishSettingsToGlobals();
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
        }
    });
    connect(audioHighPassSlider, &QSlider::valueChanged, this, [this](int value) {
        pendingSettings.audioHighPassHz = audioHighPassSliderValueToHz(value);
        updateAudioFilterLabels();
        publishSettingsToGlobals();
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
        }
    });
    auto applyHfNoiseCancelControls = [this]() {
        if (hfNoiseCancelDepthSlider) {
            pendingSettings.hfNoiseCancelDepth =
                hfNoiseCancelSliderValueToDepth(hfNoiseCancelDepthSlider->value());
        }
        if (hfNoiseCancelRefGainSlider) {
            pendingSettings.hfNoiseCancelRefGainDb =
                hfNoiseCancelSliderValueToRefGainDb(hfNoiseCancelRefGainSlider->value());
        }
        if (hfNoiseCancelRefDelaySlider) {
            pendingSettings.hfNoiseCancelRefDelayNs =
                hfNoiseCancelSliderValueToRefDelayNs(hfNoiseCancelRefDelaySlider->value());
        }
        if (hfNoiseCancelRefTiltSlider) {
            pendingSettings.hfNoiseCancelRefTiltDb =
                hfNoiseCancelSliderValueToRefTiltDb(hfNoiseCancelRefTiltSlider->value());
        }
        if (hfNoiseCancelFreezeCheckbox) {
            pendingSettings.hfNoiseCancelFreeze = hfNoiseCancelFreezeCheckbox->isChecked();
        }
        if (hfAudioBlankerCheckbox) {
            pendingSettings.hfAudioBlankerEnabled = hfAudioBlankerCheckbox->isChecked();
        }
        if (hfAudioBlankerThresholdSlider) {
            pendingSettings.hfAudioBlankerThreshold =
                (std::clamp)(hfAudioBlankerThresholdSlider->value() / 10.0, 2.0, 20.0);
        }
        updateHfNoiseCancelControls();
        if (fftResult) {
            fftResult->resetHfNoiseCancelState();
        }
        if (spectrumFftWorker) {
            spectrumFftWorker->resetHfNoiseCancelState();
        }
        if (audioProcessor) {
            audioProcessor->resetHfNoiseCancelState();
        }
        publishSettingsToGlobals();
        if (audioProcessor) {
            audioProcessor->configure(audioProcessorSettings());
        }
        if (processor && processor->isRunning()) {
            updateIqFrameProducerSettings();
        }
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
        }
    };
    connect(hfNoiseCancelDepthSlider, &QSlider::valueChanged, this, applyHfNoiseCancelControls);
    connect(hfNoiseCancelRefGainSlider, &QSlider::valueChanged, this, applyHfNoiseCancelControls);
    connect(hfNoiseCancelRefDelaySlider, &QSlider::valueChanged, this, applyHfNoiseCancelControls);
    connect(hfNoiseCancelRefTiltSlider, &QSlider::valueChanged, this, applyHfNoiseCancelControls);
    connect(hfNoiseCancelFreezeCheckbox, &QCheckBox::toggled, this, applyHfNoiseCancelControls);
    connect(hfAudioBlankerCheckbox, &QCheckBox::toggled, this, applyHfNoiseCancelControls);
    connect(hfAudioBlankerThresholdSlider, &QSlider::valueChanged, this, applyHfNoiseCancelControls);
    connect(hfInterferenceBaselineCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        hfInterferenceBaselineEnabled = checked;
        updateHfNoiseCancelControls();
        savePersistentSettings();
    });
    connect(hfInterferenceRawOverlayCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        hfInterferenceRawOverlayEnabled = checked;
        updateHfNoiseCancelControls();
        savePersistentSettings();
    });
    connect(hfInterferenceBaselineDepthSlider, &QSlider::valueChanged, this, [this](int value) {
        hfInterferenceBaselineDepth = (std::clamp)(value / 100.0, 0.0, 2.0);
        updateHfNoiseCancelControls();
        savePersistentSettings();
    });
    connect(hfInterferenceBaselineSmoothSlider, &QSlider::valueChanged, this, [this](int value) {
        hfInterferenceBaselineSmoothBins = (std::max)(1, value | 1);
        if (hfInterferenceBaselineSmoothSlider && hfInterferenceBaselineSmoothSlider->value() != hfInterferenceBaselineSmoothBins) {
            QSignalBlocker blocker(hfInterferenceBaselineSmoothSlider);
            hfInterferenceBaselineSmoothSlider->setValue(hfInterferenceBaselineSmoothBins);
        }
        updateHfNoiseCancelControls();
        savePersistentSettings();
    });
    connect(hfInterferenceBaselineLearnButton, &QPushButton::clicked, this, [this]() {
        learnHfInterferenceBaseline();
    });
    connect(hfInterferenceBaselineClearButton, &QPushButton::clicked, this, [this]() {
        clearHfInterferenceBaseline();
    });
    connect(hfInterferenceDefaultsButton, &QPushButton::clicked, this, [this]() {
        resetHfInterferenceDefaults();
    });
    connect(qthSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        qthSource = qthSourceCombo ? qthSourceCombo->currentData().toString() : QStringLiteral("manual");
        updateQthControls();
        savePersistentSettings();
    });
    connect(gnssSystemCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        updateGnssSystemSelection();
        applyGnssSystemPresetToReceiver(gnssSystemId);
    });
    connect(gnssIntegrationSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        gnssAcquisitionIntegrationMs =
            (std::clamp)(value,
                         GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                         GNSS_ACQUISITION_MAX_INTEGRATION_MS);
        savePersistentSettings();
    });
    connect(gnssChannelFilterSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double valueMhz) {
        const double cutoffHz = valueMhz * 1000000.0;
        gnssChannelFilterCutoffHz =
            (std::clamp)(std::isfinite(cutoffHz) ? cutoffHz : 1800000.0,
                         GNSS_CHANNEL_FILTER_MIN_HZ,
                         GNSS_CHANNEL_FILTER_MAX_HZ);
        savePersistentSettings();
    });
    connect(gnssDopplerSpanSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int valueKhz) {
        gnssDopplerSpanHz = (std::clamp)(valueKhz, 1, 50) * 1000;
        savePersistentSettings();
    });
    connect(gnssDopplerStepSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int valueHz) {
        gnssDopplerStepHz = (std::clamp)(valueHz, 250, 5000);
        savePersistentSettings();
    });
    connect(qthLatitudeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        applyQthPositionFromUi();
    });
    connect(qthLongitudeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        applyQthPositionFromUi();
    });
    connect(qthMapButton, &QPushButton::clicked, this, &YourClassName::openQthMapWindow);
    connect(qthCopyButton, &QPushButton::clicked, this, &YourClassName::copyQthLocator);
    connect(qthClearButton, &QPushButton::clicked, this, &YourClassName::clearQthPosition);
    connect(gnssSerialButton, &QPushButton::clicked, this, &YourClassName::toggleGnssSerial);
    connect(gnssNmeaLogButton, &QPushButton::clicked, this, &YourClassName::toggleGnssNmeaLogging);
    connect(gnssNmeaReplayButton, &QPushButton::clicked, this, &YourClassName::replayGnssNmeaLog);
    connect(gnssSerialRawLogButton, &QPushButton::clicked, this, &YourClassName::toggleGnssRawSerialLogging);
    connect(gnssUbxSystemsButton, &QPushButton::clicked, this, &YourClassName::applyGnssUbxConstellationConfig);
    connect(gnssUbxSaveButton, &QPushButton::clicked, this, &YourClassName::saveGnssUbxConfigurationToModule);
    connect(gnssPositionPolicyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        gnssPositionPolicy = normalizedGnssPositionPolicy(gnssPositionPolicyCombo
                                                              ? gnssPositionPolicyCombo->currentData().toString()
                                                              : QString());
        savePersistentSettings();
        updateGnssSatelliteView(true);
    });
    connect(gnssSatelliteTableCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        gnssSatelliteTableVisible = checked;
        if (gnssSatelliteTableDialog) {
            if (checked) {
                updateGnssSatelliteView(true);
                gnssSatelliteTableDialog->show();
                gnssSatelliteTableDialog->raise();
                if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"), Qt::CaseInsensitive)) {
                    gnssSatelliteTableDialog->activateWindow();
                }
            } else {
                gnssSatelliteTableDialog->hide();
            }
        }
        savePersistentSettings();
        updateGnssSatelliteView(checked);
    });
    if (gnssSatelliteTableDialog) {
        connect(gnssSatelliteTableDialog, &QDialog::finished, this, [this](int) {
            gnssSatelliteTableVisible = false;
            if (gnssSatelliteTableCheckbox) {
                QSignalBlocker blocker(gnssSatelliteTableCheckbox);
                gnssSatelliteTableCheckbox->setChecked(false);
            }
            savePersistentSettings();
        });
    }
    connect(gnssTimeZoneCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        gnssTimeZoneOffsetMinutes = gnssTimeZoneCombo ? gnssTimeZoneCombo->currentData().toInt() : 0;
        savePersistentSettings();
        updateGnssSatelliteView(true);
    });
    connect(gnssSerialPortEdit, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (gnssSerialPortEdit) {
            gnssSerialPortName = text.trimmed();
            savePersistentSettings();
        }
    });
    connect(gnssSerialBaudSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        gnssSerialBaud = value;
        savePersistentSettings();
    });
    auto connectGnssSystemFilter = [this](QCheckBox *box, bool *target) {
        if (!box || !target) {
            return;
        }
        connect(box, &QCheckBox::toggled, this, [this, target](bool checked) {
            *target = checked;
            updateGnssSatelliteView(true);
            savePersistentSettings();
        });
    };
    connectGnssSystemFilter(gnssUseGpsCheckbox, &gnssUseGps);
    connectGnssSystemFilter(gnssUseGlonassCheckbox, &gnssUseGlonass);
    connectGnssSystemFilter(gnssUseGalileoCheckbox, &gnssUseGalileo);
    connectGnssSystemFilter(gnssUseBeidouCheckbox, &gnssUseBeidou);
    connectGnssSystemFilter(gnssUseQzssCheckbox, &gnssUseQzss);
    connectGnssSystemFilter(gnssUseSbasCheckbox, &gnssUseSbas);
    connectGnssSystemFilter(gnssUseOtherCheckbox, &gnssUseOther);
    if (gnssSatelliteTable) {
        connect(gnssSatelliteTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
            if (!item || item->column() != 0) {
                return;
            }
            const QString key = item->data(Qt::UserRole).toString();
            if (key.isEmpty()) {
                return;
            }
            const bool enabled = item->checkState() == Qt::Checked;
            gnssNmeaSatelliteEnabled.insert(key, enabled);
            if (enabled) {
                gnssDisabledSatelliteKeys.remove(key);
            } else {
                gnssDisabledSatelliteKeys.insert(key);
            }
            gnssSatelliteTableDirty = true;
            updateGnssSatelliteView(true);
            savePersistentSettings();
        });
        connect(gnssSatelliteTable->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int section) {
            if (section < 0 || section >= gnssSatelliteTable->columnCount()) {
                return;
            }
            if (gnssSatelliteSortColumn == section) {
                gnssSatelliteSortAscending = !gnssSatelliteSortAscending;
            } else {
                gnssSatelliteSortColumn = section;
                gnssSatelliteSortAscending = section != 6 && section != 7 && section != 8 && section != 9;
            }
            gnssSatelliteTable->horizontalHeader()->setSortIndicator(
                gnssSatelliteSortColumn,
                gnssSatelliteSortAscending ? Qt::AscendingOrder : Qt::DescendingOrder);
            updateGnssSatelliteView(true);
        });
        connect(gnssSatelliteTable,
                &QTableWidget::customContextMenuRequested,
                this,
                [this](const QPoint &pos) {
                    if (!gnssSatelliteTable || gnssSatelliteTable->columnAt(pos.x()) != 0) {
                        return;
                    }
                    QMenu menu(gnssSatelliteTable);
                    QAction *selectAllAction = menu.addAction(uiText(QStringLiteral("select_all"),
                                                                      QStringLiteral("Select all")));
                    QAction *clearAllAction = menu.addAction(uiText(QStringLiteral("clear_all"),
                                                                     QStringLiteral("Clear all")));
                    QAction *chosenAction = menu.exec(gnssSatelliteTable->viewport()->mapToGlobal(pos));
                    if (chosenAction == selectAllAction) {
                        setGnssSatelliteRowsEnabled(true);
                    } else if (chosenAction == clearAllAction) {
                        setGnssSatelliteRowsEnabled(false);
                    }
                });
    }
    connect(gnssTuneButton, &QPushButton::clicked, this, &YourClassName::tuneGnssL1Preset);
    connect(gnssScanButton, &QPushButton::clicked, this, &YourClassName::applyGnssScanPreset);
    connect(gnssRawLogButton, &QPushButton::clicked, this, &YourClassName::logGnssRawContext);
    connect(gnssAcquireButton, &QPushButton::clicked, this, &YourClassName::runGnssAcquisitionTest);
    connect(gnssDeepAcquireButton, &QPushButton::toggled, this, &YourClassName::setGnssContinuousAcquisitionEnabled);
    if (gnssContinuousAcquisitionEnabled) {
        setGnssContinuousAcquisitionEnabled(true);
    }
    connect(gnssOfflineAcquireButton, &QPushButton::clicked, this, &YourClassName::runGnssOfflineReplayTest);
    connect(gnssSelfTestButton, &QPushButton::clicked, this, &YourClassName::runGnssSyntheticSelfTest);
    connect(gnssPositionSelfTestButton, &QPushButton::clicked, this, &YourClassName::runGnssPositionSelfTest);
    connect(gnssNetworkTimeButton, &QPushButton::clicked, this, &YourClassName::requestGnssNetworkTime);
    connect(gnssPlotButton, &QPushButton::clicked, this, [this]() {
        if (!gnssAcquisitionPlotDialog) {
            return;
        }
        gnssAcquisitionPlotDialog->show();
        gnssAcquisitionPlotDialog->raise();
        if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"), Qt::CaseInsensitive)) {
            gnssAcquisitionPlotDialog->activateWindow();
        }
    });
    connect(gnssSatellitesButton, &QPushButton::clicked, this, [this]() {
        if (!gnssSatelliteDialog) {
            return;
        }
        updateGnssSatelliteView(true);
        gnssSatelliteDialog->show();
        gnssSatelliteDialog->raise();
        if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"), Qt::CaseInsensitive)) {
            gnssSatelliteDialog->activateWindow();
        }
        if (gnssSatelliteTableVisible && gnssSatelliteTableDialog) {
            gnssSatelliteTableDialog->show();
            gnssSatelliteTableDialog->raise();
        }
    });
    connect(gnssNtpSocket, &QUdpSocket::readyRead, this, &YourClassName::handleGnssNetworkTimeResponse);
    connect(gnssMonitorCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        gnssMonitorEnabled = checked;
        if (checked) {
            resetGnssMonitor();
        } else if (gnssMonitorStatusLabel) {
            gnssMonitorStatusLabel->setText(uiText(QStringLiteral("gnss_iq_monitor_idle"),
                                                   QStringLiteral("GNSS IQ monitor: off")));
        }
        savePersistentSettings();
    });
    connect(gnssMonitorResetButton, &QPushButton::clicked, this, &YourClassName::resetGnssMonitor);
    connect(startButton, &QPushButton::clicked, this, &YourClassName::startFobosProcessing);
    connect(stopButton, &QPushButton::clicked, this, &YourClassName::stopFobosProcessing);
    connect(modeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &YourClassName::onDirectSamplingChanged);
    connect(clkBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &YourClassName::onClkChanged);
    connect(refreshButton, &QPushButton::clicked, this, &YourClassName::requestUsbDeviceRefresh);
    connect(comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || isRunningOrTransitioning()) {
            return;
        }
        if (usbDeviceRefreshTimer) {
            usbDeviceRefreshTimer->stop();
        }
        bool ok = false;
        const int selectedIndex = comboBox->currentData().toInt(&ok);
        pendingSettings.deviceIndex = ok ? receiverDeviceIndexFromComboValue(selectedIndex) : index;
        rebuildReceiverModeChoices();
        if (isRtlBackendSelected()) {
            if (!isKnownRtlSampleRate(pendingSettings.sampleRate)) {
                pendingSettings.sampleRate = RTL_TCP_SAFE_SAMPLE_RATE;
            }
        } else if (isHackRfNativeSelected()) {
            if (!isKnownHackRfSampleRate(pendingSettings.sampleRate)) {
                pendingSettings.sampleRate = 10000000.0;
            }
        } else if (isSoapySdrSelected()) {
            const QVector<double> soapyCommonRates = {
                1000000.0, 1024000.0, 1536000.0, 2048000.0, 2400000.0,
                3200000.0, 8000000.0, 10000000.0, 20000000.0, 50000000.0
            };
            bool knownSoapyRate = false;
            for (const double rate : soapyCommonRates) {
                if (std::abs(rate - pendingSettings.sampleRate) < 0.5) {
                    knownSoapyRate = true;
                    break;
                }
            }
            if (!knownSoapyRate) {
                pendingSettings.sampleRate = 2048000.0;
            }
        } else if (isKnownRtlSampleRate(pendingSettings.sampleRate)) {
            pendingSettings.sampleRate = FOBOS_DEFAULT_SAMPLE_RATE;
        }
        if (sampleBox) {
            sampleBox->clear();
            populateSampleRates();
        }
        normalizeTuning(pendingSettings, true);
        updateReceiverSpecificControls();
        publishSettingsToGlobals();
        settingRange();
        qDebug() << "[FobosDevices] selected logical device" << pendingSettings.deviceIndex;
        if (persistentSettingsReady) {
            savePersistentSettings();
        }
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
        } else if (persistentSettingsReady && isIdle()) {
            if (hasActiveFobosDevice()) {
                closeFobosSession(false);
            }
            if (!isExternalReceiverBackendSelected()) {
                QTimer::singleShot(0, this, [this]() {
                    prepareFobosSessionFromSettings(QStringLiteral("receiver selection"));
                });
            }
        }
    });
    connect(fobosButton, &QPushButton::clicked, this, &YourClassName::listFobosDevices);
    connect(networkButton, &QPushButton::clicked, this, &YourClassName::openNetworkSettingsDialog);
    connect(hackRfSettingsButton,
            &QPushButton::clicked,
            this,
            &YourClassName::showHackRfRxSettings);
    updateReceiverSpecificControls();
    connect(transmitButton, &QPushButton::clicked, this, &YourClassName::openTransmitDialog);
    connect(networkController, &NetworkController::statusChanged, this, &YourClassName::onNetworkStatusChanged);
    connect(networkController, &NetworkController::channelReady, this, [this](const QString &status) {
        Q_UNUSED(status);
        if (isNetworkClientMode()) {
            networkClientReconnectPending = false;
            if (runState == RadioRunState::Running) {
                if (isClientIqProcessingMode()) {
                    startNetworkClientProcessing();
                } else {
                    stopNetworkClientProcessing();
                }
            }
            QTimer::singleShot(0, this, [this]() {
                sendRemoteControlCommand(QStringLiteral("requestServerState"));
            });
        } else if (networkMode == NetworkMode::Server && runState == RadioRunState::Running) {
            applyServerLocalOutputPolicy();
            sendServerStateToClients();
        } else if (networkMode == NetworkMode::Server) {
            sendServerStateToClients();
        }
    });
    connect(networkController, &NetworkController::channelError, this, [this](const QString &message) {
        qDebug() << "[Network]" << message;
        if (networkMode == NetworkMode::Server && runState == RadioRunState::Running) {
            applyServerLocalOutputPolicy();
        }
        if (isNetworkClientMode() && runState == RadioRunState::Running) {
            if (isClientIqProcessingMode()) {
                stopNetworkClientProcessing();
            } else if (remoteAudioPlayer) {
                remoteAudioPlayer->stop();
            }
            updateUiForRunState();
        }
        if (!isNetworkClientMode() ||
            runState != RadioRunState::Running ||
            networkClientReconnectPending) {
            return;
        }

        networkClientReconnectPending = true;
        QTimer::singleShot(1500, this, [this]() {
            networkClientReconnectPending = false;
            if (isNetworkClientMode() &&
                runState == RadioRunState::Running &&
                networkController &&
                !networkController->isControlReady()) {
                qDebug() << "[Network] reconnecting client after channel loss";
                networkController->testClientConnection(networkServerAddress, networkControlPort);
            }
        });
    });
    connect(networkController, &NetworkController::controlCommandReceived, this, &YourClassName::onNetworkControlCommandReceived);
    connect(networkController, &NetworkController::binaryCommandReceived, this, [this](const QJsonObject &command, const QByteArray &payload) {
        if (networkMode != NetworkMode::Client) {
            return;
        }
        const QString type = command.value("type").toString();
        if (type == QStringLiteral("iqbin")) {
            receiveNetworkIqFrameBinary(command, payload);
        } else if (type == QStringLiteral("spectrumbin")) {
            displayNetworkSpectrumFrameBinary(command, payload);
        }
    });
    connect(controlsToggleButton, &QPushButton::toggled, this, [this](bool checked) {
        setControlsPanelVisible(checked);
    });
    connect(controlsDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (controlsToggleButton && controlsToggleButton->isChecked() != visible) {
            QSignalBlocker blocker(controlsToggleButton);
            controlsToggleButton->setChecked(visible);
        }
    });
    connect(ft8StationButton, &QPushButton::clicked, this, &YourClassName::openFt8StationDialog);
    connect(digitalClearButton, &QPushButton::clicked, this, [this]() {
        if (digitalTextEdit) {
            digitalTextEdit->clear();
        }
    });
    connect(digitalToggleButton, &QPushButton::toggled, this, [this](bool checked) {
        if (digitalDock) {
            digitalDock->setVisible(checked);
        }
    });
    connect(digitalDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (digitalToggleButton && digitalToggleButton->isChecked() != visible) {
            QSignalBlocker blocker(digitalToggleButton);
            digitalToggleButton->setChecked(visible);
        }
    });
    connect(digitalDecodeCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        digitalDecodeEnabled = checked;
        updateDigitalDecoderMode();
    });
    const auto applyCwDecoderSettings = [this]() {
        pendingSettings.cwDecoderToneHz = cwDecoderToneSpin ? cwDecoderToneSpin->value() : 700.0;
        pendingSettings.cwDecoderWpm = cwDecoderWpmSpin ? cwDecoderWpmSpin->value() : 18;
        pendingSettings.cwDecoderAdaptiveSpeed =
            !cwDecoderAdaptiveCheckbox || cwDecoderAdaptiveCheckbox->isChecked();
        pendingSettings.cwDecoderSelectivity =
            cwDecoderSelectivitySpin ? cwDecoderSelectivitySpin->value() : 5;
        pendingSettings.cwDecoderAlphabet =
            cwDecoderAlphabetCombo ? cwDecoderAlphabetCombo->currentData().toInt() : 2;
        if (pendingSettings.modulationType == MOD_CW) updateDigitalDecoderMode();
        savePersistentSettings();
    };
    connect(cwDecoderToneSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [applyCwDecoderSettings](int) { applyCwDecoderSettings(); });
    connect(cwDecoderWpmSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [applyCwDecoderSettings](int) { applyCwDecoderSettings(); });
    connect(cwDecoderAdaptiveCheckbox,
            &QCheckBox::toggled,
            this,
            [applyCwDecoderSettings](bool) { applyCwDecoderSettings(); });
    connect(cwDecoderSelectivitySpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [applyCwDecoderSettings](int) { applyCwDecoderSettings(); });
    connect(cwDecoderAlphabetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [applyCwDecoderSettings](int) { applyCwDecoderSettings(); });
    const auto applyPsk = [this]() {
        pendingSettings.pskDecoderToneHz = pskDecoderToneSpin->value();
        pendingSettings.pskDecoderSquelchPercent = pskDecoderSquelchSpin->value();
        pendingSettings.pskDecoderAfc = pskDecoderAfcCheckbox->isChecked();
        pendingSettings.pskDecoderEncoding = pskDecoderEncodingCombo->currentData().toInt();
        if (pendingSettings.modulationType == MOD_PSK) updateDigitalDecoderMode();
        savePersistentSettings();
    };
    connect(pskDecoderToneSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyPsk](double) { applyPsk(); });
    connect(pskDecoderSquelchSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [applyPsk](int) { applyPsk(); });
    connect(pskDecoderAfcCheckbox, &QCheckBox::toggled, this, [applyPsk](bool) { applyPsk(); });
    connect(pskDecoderEncodingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [applyPsk](int) { applyPsk(); });
    auto updateDmrLabControls = [this](bool locked) {
        const QList<QWidget *> controls = {
            dmrLabColorCodeCombo,
            dmrLabSlotCombo,
            dmrLabCallTypeCombo,
            dmrLabSourceIdEdit,
            dmrLabTargetIdEdit,
            dmrLabRadioEdit,
            dmrLabNotesEdit,
            dmrBasebandRateCombo,
            dmrChannelRateCombo,
            dmrAmbeLayoutCombo,
            dmrManualTimingCheckbox,
            dmrTimingOffsetSpin,
            dmrSlicerRatioSpin,
            dmrAdaptiveSlicerCheckbox,
            dmrPrivacyModeCombo,
            dmrPrivacyKeyIdCombo,
            dmrPrivacyKeysButton,
            dmrBackendCombo,
            dsdNeoAutoStartCheckbox,
            dsdNeoProgramEdit,
            dsdNeoInputPortSpin,
            dsdNeoUdpOutputPortSpin,
        };
        for (QWidget *control : controls) {
            if (control) {
                control->setEnabled(true);
            }
        }
        if (dmrTimingOffsetSpin && dmrManualTimingCheckbox) {
            dmrTimingOffsetSpin->setEnabled(dmrManualTimingCheckbox->isChecked());
        }
        if (dmrLabCaptureCheckbox) {
            dmrLabCaptureCheckbox->setToolTip(
                locked
                    ? QStringLiteral("Locked: DMR decode is filtered by the selected CC/slot/TG/SRC.")
                    : QStringLiteral("Auto: DMR metadata is learned from the signal and written into the fields."));
        }
    };
    connect(dmrLabCaptureCheckbox, &QCheckBox::toggled, this, updateDmrLabControls);
    const auto resetDmrLabDecoderState = [this](const char *reason) {
        pendingDmrDecoderPcm.clear();
        pendingDmrDecoderSampleRate =
            dmrBasebandRateCombo
                ? normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt())
                : normalizedDmrBasebandSampleRate(pendingSettings.dmrBasebandSampleRate);
        digitalDecoderGeneration.fetch_add(1, std::memory_order_relaxed);
        if (digitalDecoder) {
            QMetaObject::invokeMethod(digitalDecoder,
                                      [decoder = digitalDecoder]() {
                                          decoder->reset();
                                      },
                                      Qt::QueuedConnection);
        }
        qDebug() << "[DMR lab] decoder reset after control change"
                 << "reason" << reason
                 << "manualTiming" << (dmrManualTimingCheckbox && dmrManualTimingCheckbox->isChecked())
                 << "timingOffset" << (dmrTimingOffsetSpin ? dmrTimingOffsetSpin->value() : 0)
                 << "slicerRatio" << (dmrSlicerRatioSpin ? dmrSlicerRatioSpin->value() : 0.625)
                 << "adaptiveSlicer" << (!dmrAdaptiveSlicerCheckbox || dmrAdaptiveSlicerCheckbox->isChecked())
                 << "channelRate" << (dmrChannelRateCombo ? dmrChannelRateCombo->currentData().toInt() : 0)
                 << "ambeLayout"
                 << (dmrAmbeLayoutCombo
                         ? dmrAmbeLayoutName(dmrAmbeLayoutCombo->currentData().toInt())
                         : dmrAmbeLayoutName(pendingSettings.dmrAmbeLayout))
                 << "privacyMode" << dmrPrivacyModeId(pendingSettings.dmrPrivacyMode)
                 << "privacyKeyId" << pendingSettings.dmrPrivacyKeyId
                 << "privacyKeySet" << !pendingSettings.dmrPrivacyKeyHex.isEmpty()
                 << "privacyEnabled" << (pendingSettings.dmrPrivacyMode != DMR_PRIVACY_NONE);
    };
    connect(dmrBasebandRateCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this, resetDmrLabDecoderState]() {
                if (!dmrBasebandRateCombo) {
                    return;
                }
                const int selectedRate =
                    normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt());
                if (pendingSettings.dmrBasebandSampleRate == selectedRate) {
                    return;
                }
                const int previousRate = pendingSettings.dmrBasebandSampleRate;
                pendingSettings.dmrBasebandSampleRate = selectedRate;
                resetDmrLabDecoderState("basebandRate");
                qDebug() << "[DMR lab] 4FSK baseband sample-rate selected"
                         << "oldRate" << previousRate
                         << "newRate" << selectedRate
                         << "samplesPerSymbol"
                         << (static_cast<double>(selectedRate) / 4800.0);
                if (audioProcessor) {
                    audioProcessor->configure(audioProcessorSettings());
                }
                updateDsdNeoBridgeSettings();
                updateDigitalDecoderMode();
                updateIqFrameProducerSettings();
            });
    connect(dmrChannelRateCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this, resetDmrLabDecoderState]() {
                if (!dmrChannelRateCombo) {
                    return;
                }
                const int selectedRate = dmrChannelRateCombo->currentData().toInt();
                if (pendingSettings.dmrChannelSampleRate == selectedRate) {
                    return;
                }
                const int previousRate = pendingSettings.dmrChannelSampleRate;
                pendingSettings.dmrChannelSampleRate = selectedRate;
                resetDmrLabDecoderState("channelRate");
                qDebug() << "[DMR lab] channel sample-rate selected"
                         << "oldRate" << previousRate
                         << "newRate" << selectedRate;
                if (audioProcessor) {
                    audioProcessor->configure(audioProcessorSettings());
                }
                updateDigitalDecoderMode();
                savePersistentSettings();
            });
    connect(dmrAmbeLayoutCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this, resetDmrLabDecoderState]() {
                if (!dmrAmbeLayoutCombo) {
                    return;
                }
                const int selectedLayout =
                    normalizedDmrAmbeLayout(dmrAmbeLayoutCombo->currentData().toInt());
                if (pendingSettings.dmrAmbeLayout == selectedLayout) {
                    return;
                }
                const int previousLayout = pendingSettings.dmrAmbeLayout;
                pendingSettings.dmrAmbeLayout = selectedLayout;
                resetDmrLabDecoderState("ambeLayout");
                qDebug() << "[DMR lab] AMBE layout selected"
                         << "old" << dmrAmbeLayoutName(previousLayout)
                         << "new" << dmrAmbeLayoutName(selectedLayout);
                updateDigitalDecoderMode();
            });
    connect(dmrManualTimingCheckbox, &QCheckBox::toggled, this, [this, updateDmrLabControls, resetDmrLabDecoderState](bool) {
        pendingSettings.dmrManualTimingEnabled =
            dmrManualTimingCheckbox && dmrManualTimingCheckbox->isChecked();
        if (dmrTimingOffsetSpin) {
            pendingSettings.dmrManualTimingOffset = dmrTimingOffsetSpin->value();
        }
        updateDmrLabControls(dmrLabCaptureCheckbox && dmrLabCaptureCheckbox->isChecked());
        resetDmrLabDecoderState("manualTiming");
        updateDigitalDecoderMode();
    });
    connect(dmrTimingOffsetSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [this, resetDmrLabDecoderState](int value) {
                pendingSettings.dmrManualTimingOffset = value;
                if (dmrManualTimingCheckbox && dmrManualTimingCheckbox->isChecked()) {
                    resetDmrLabDecoderState("timingOffset");
                    updateDigitalDecoderMode();
                }
            });
    connect(dmrSlicerRatioSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            [this, resetDmrLabDecoderState](double value) {
                pendingSettings.dmrSlicerRatio = value;
                resetDmrLabDecoderState("slicerRatio");
                updateDigitalDecoderMode();
            });
    connect(dmrAdaptiveSlicerCheckbox, &QCheckBox::toggled, this, [this, resetDmrLabDecoderState](bool checked) {
        pendingSettings.dmrAdaptiveSlicer = checked;
        resetDmrLabDecoderState("adaptiveSlicer");
        updateDigitalDecoderMode();
    });
    const auto updateDmrPrivacyDraft = [this, resetDmrLabDecoderState](const char *reason) {
        pendingSettings.dmrPrivacyMode =
            dmrPrivacyModeCombo
                ? normalizedDmrPrivacyMode(dmrPrivacyModeCombo->currentData().toInt())
                : DMR_PRIVACY_NONE;
        if (reason && QString::fromLatin1(reason) == QStringLiteral("privacyMode")) {
            refreshDmrPrivacyKeyIdCombo();
        }
        applyDmrPrivacyKeySelection();
        pendingSettings.dmrPrivacyFrameOffset =
            dmrPrivacyFrameOffsetCombo
                ? (std::clamp)(dmrPrivacyFrameOffsetCombo->currentData().toInt(), 0, 17)
                : (std::clamp)(pendingSettings.dmrPrivacyFrameOffset, 0, 17);
        pendingSettings.dmrPrivacyVariant =
            dmrPrivacyDropCombo
                ? dmrPrivacyDropCombo->currentData().toString()
                : pendingSettings.dmrPrivacyVariant;
        pendingSettings.dmrPrivacyLayout =
            dmrPrivacyBitLayoutCombo
                ? dmrPrivacyBitLayoutCombo->currentData().toString()
                : pendingSettings.dmrPrivacyLayout;
        const QString reasonText = QString::fromLatin1(reason ? reason : "");
        const bool privacyTuningOnly =
            reasonText == QStringLiteral("privacyOffset") ||
            reasonText == QStringLiteral("privacyDrop") ||
            reasonText == QStringLiteral("privacyLayout");
        if (!privacyTuningOnly) {
            resetDmrLabDecoderState(reason);
        }
        updateDsdNeoBridgeSettings();
        updateGopherTrunkBridgeSettings();
        updateDigitalDecoderMode();
        qDebug() << "[DMR privacy] draft config changed"
                 << "reason" << reason
                 << "mode" << dmrPrivacyModeId(pendingSettings.dmrPrivacyMode)
                 << "keyId" << pendingSettings.dmrPrivacyKeyId
                 << "keyHexLen" << pendingSettings.dmrPrivacyKeyHex.size()
                 << "enabled" << (pendingSettings.dmrPrivacyMode != DMR_PRIVACY_NONE)
                 << "variant" << pendingSettings.dmrPrivacyVariant
                 << "layout" << pendingSettings.dmrPrivacyLayout
                 << "offset" << pendingSettings.dmrPrivacyFrameOffset;
        savePersistentSettings();
    };
    connect(dmrPrivacyModeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [updateDmrPrivacyDraft]() { updateDmrPrivacyDraft("privacyMode"); });
    connect(dmrPrivacyKeyIdCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [updateDmrPrivacyDraft](int) { updateDmrPrivacyDraft("privacyKeyId"); });
    connect(dmrPrivacyFrameOffsetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [updateDmrPrivacyDraft](int) { updateDmrPrivacyDraft("privacyOffset"); });
    connect(dmrPrivacyDropCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [updateDmrPrivacyDraft](int) { updateDmrPrivacyDraft("privacyDrop"); });
    connect(dmrPrivacyBitLayoutCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [updateDmrPrivacyDraft](int) { updateDmrPrivacyDraft("privacyLayout"); });
    connect(dmrPrivacyKeysButton, &QPushButton::clicked, this, &YourClassName::showDmrPrivacyKeyDialog);
    const auto updateDsdNeoUi = [this]() {
        const bool enabled = selectedDmrBackend() == DMR_BACKEND_DSD_NEO;
        if (dsdNeoAutoStartCheckbox) {
            dsdNeoAutoStartCheckbox->setEnabled(enabled);
        }
        if (dsdNeoProgramEdit) {
            dsdNeoProgramEdit->setEnabled(enabled && dsdNeoAutoStartCheckbox && dsdNeoAutoStartCheckbox->isChecked());
        }
        if (dsdNeoInputPortSpin) {
            dsdNeoInputPortSpin->setEnabled(enabled && dsdNeoAutoStartCheckbox && !dsdNeoAutoStartCheckbox->isChecked());
        }
        if (dsdNeoUdpOutputPortSpin) {
            dsdNeoUdpOutputPortSpin->setEnabled(enabled);
        }
    };
    const auto onDsdNeoChanged = [this, updateDsdNeoUi]() {
        updateDsdNeoUi();
        updateDsdNeoBridgeSettings();
        updateGopherTrunkBridgeSettings();
        updateDigitalDecoderMode();
        if (audioProcessor) {
            audioProcessor->clearExternalPcm();
        }
        savePersistentSettings();
    };
    connect(dmrBackendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onDsdNeoChanged);
    connect(dsdNeoAutoStartCheckbox, &QCheckBox::toggled, this, onDsdNeoChanged);
    connect(dsdNeoProgramEdit, &QLineEdit::editingFinished, this, onDsdNeoChanged);
    connect(dsdNeoInputPortSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onDsdNeoChanged);
    connect(dsdNeoUdpOutputPortSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onDsdNeoChanged);
    if (dsdNeoBridge) {
        connect(dsdNeoBridge, &DsdNeoBridge::statusChanged, this, [this](const QString &status) {
            if (dsdNeoStatusLabel) {
                dsdNeoStatusLabel->setText(status);
            }
        });
        connect(dsdNeoBridge,
                &DsdNeoBridge::decodedPcmReady,
                audioProcessor,
                &AudioProcessor::enqueueExternalPcm,
                Qt::DirectConnection);
    }
    if (gopherTrunkBridge) {
        connect(gopherTrunkBridge, &GopherTrunkBridge::statusChanged, this, [this](const QString &status) {
            if (dsdNeoStatusLabel) {
                dsdNeoStatusLabel->setText(status);
            }
        });
        connect(gopherTrunkBridge,
                &GopherTrunkBridge::decodedPcmReady,
                audioProcessor,
                &AudioProcessor::enqueueExternalPcm,
                Qt::QueuedConnection);
        connect(digitalDecoder,
                &DigitalDecoder::dmrDibitBurstReady,
                gopherTrunkBridge,
                &GopherTrunkBridge::sendDibitBurst,
                Qt::QueuedConnection);
    }
    updateDsdNeoUi();
    updateDmrLabControls(dmrLabCaptureCheckbox && dmrLabCaptureCheckbox->isChecked());
    connect(digitalDecoder, &DigitalDecoder::textDecoded, this, &YourClassName::onDigitalTextDecoded);
    connect(digitalDecoder, &DigitalDecoder::ft8Decoded, this, &YourClassName::onFt8Decoded);
    connect(digitalDecoder, &DigitalDecoder::statusChanged, this, &YourClassName::onDigitalDecoderStatusChanged);
    connect(digitalDecoder, &DigitalDecoder::dmrMetadataDetected, this, &YourClassName::onDmrMetadataDetected);
    connect(digitalDecoder,
            &DigitalDecoder::voicePcmReady,
            audioProcessor,
            &AudioProcessor::enqueueExternalPcm,
            Qt::QueuedConnection);
    connect(videoToggleButton, &QPushButton::toggled, this, [this](bool checked) {
        if (videoDock) {
            videoDock->setVisible(checked);
        }
    });
    connect(videoDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (videoToggleButton && videoToggleButton->isChecked() != visible) {
            QSignalBlocker blocker(videoToggleButton);
            videoToggleButton->setChecked(visible);
        }
        updateVideoProcessorMode();
        updateIqFrameProducerSettings();
    });
    connect(videoDecodeCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        videoDecodeEnabled = checked;
        updateVideoProcessorMode();
        updateIqFrameProducerSettings();
    });
    connect(videoDemodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateVideoProcessorMode();
    });
    connect(videoStandardCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateVideoProcessorMode();
    });
    connect(sstvDemodulationCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this]() {
                pendingSettings.sstvDemodulationMode = normalizedSstvDemodulationMode(
                    sstvDemodulationCombo->currentData().toInt());
                if (scaleWidget) {
                    scaleWidget->setTuning(
                        pendingSettings.listeningFrequency,
                        pendingSettings.centerFrequency,
                        effectiveDemodulationBandwidth(
                            pendingSettings.bandwidth,
                            pendingSettings.modulationType,
                            pendingSettings.sstvDemodulationMode),
                        effectiveDemodulationModulationType(
                            pendingSettings.modulationType,
                            pendingSettings.sstvDemodulationMode));
                }
                if (audioProcessor) {
                    audioProcessor->configure(audioProcessorSettings());
                }
                if (videoProcessor && videoProcessorThread) {
                    QMetaObject::invokeMethod(videoProcessor,
                                              [processor = videoProcessor]() {
                                                  processor->resetSstvDecoder();
                                              },
                                              Qt::QueuedConnection);
                }
                savePersistentSettings();
            });
    connect(videoInvertCheckbox, &QCheckBox::toggled, this, [this]() {
        updateVideoProcessorMode();
    });
    connect(videoHSyncCheckbox, &QCheckBox::toggled, this, [this]() {
        updateVideoProcessorMode();
    });
    connect(videoVSyncCheckbox, &QCheckBox::toggled, this, [this]() {
        updateVideoProcessorMode();
    });
    connect(videoTestPatternCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        const bool analogVideoTest = checked && pendingSettings.modulationType == MOD_ATV;
        const bool sstvTest = checked && pendingSettings.modulationType == MOD_SSTV;
        const bool aptTest = checked && pendingSettings.modulationType == MOD_APT;
        const bool wefaxTest = checked && pendingSettings.modulationType == MOD_WEFAX;
        if (videoProcessor && videoProcessorThread) {
            QMetaObject::invokeMethod(videoProcessor,
                                      [processor = videoProcessor, analogVideoTest]() {
                                          processor->setTestPatternEnabled(analogVideoTest);
                                      },
                                      Qt::QueuedConnection);
            QMetaObject::invokeMethod(videoProcessor,
                                      [processor = videoProcessor, sstvTest]() {
                                          processor->setSstvTestPatternEnabled(sstvTest);
                                      },
                                      Qt::QueuedConnection);
            QMetaObject::invokeMethod(videoProcessor,
                                      [processor = videoProcessor, aptTest]() {
                                          processor->setAptTestPatternEnabled(aptTest);
                                      },
                                      Qt::QueuedConnection);
            QMetaObject::invokeMethod(videoProcessor,
                                      [processor = videoProcessor, wefaxTest]() {
                                          processor->setWefaxTestPatternEnabled(wefaxTest);
                                      },
                                      Qt::QueuedConnection);
        }
        updateVideoProcessorMode();
        updateIqFrameProducerSettings();
    });
    connect(videoProcessor, &VideoProcessor::frameReady, videoWidget, &VideoWidget::setFrame);
    connect(videoProcessor, &VideoProcessor::frameReady, this,
            [videoSaveImageButton](const QImage &frame) {
                videoSaveImageButton->setEnabled(!frame.isNull());
            });
    connect(videoProcessor, &VideoProcessor::statusChanged, this, &YourClassName::onVideoStatusChanged);
    connect(videoProcessor,
            &VideoProcessor::analogLockMetrics,
            this,
            &YourClassName::handleFpvAnalogLockMetrics,
            Qt::QueuedConnection);
    connect(recordingManager, &RecordingManager::statusChanged, this, &YourClassName::updateRecordingStatus);
    connect(recordButton, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            startRecording(false);
        } else {
            stopRecording(false);
        }
    });
    connect(spectrumFrameRecordButton, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            startSpectrumFrameRecording();
        } else {
            stopSpectrumFrameRecording();
        }
    });
    connect(spectrumFrameReplayButton, &QPushButton::clicked, this, &YourClassName::openSpectrumFrameReplay);
    connect(spectrumEventModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        spectrumEventCaptureMode = spectrumEventModeCombo ? spectrumEventModeCombo->currentData().toInt() : 0;
        spectrumIqPrebuffer.clear();
        updateIqFrameProducerSettings();
        savePersistentSettings();
    });
    connect(spectrumFrameBufferCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        spectrumFrameBufferEnabled = checked;
        if (!checked) {
            spectrumFramePrebuffer.clear();
            spectrumIqPrebuffer.clear();
            if (!spectrumFrameRecorder.isRecording()) {
                updateSpectrumFrameRecordingStatus(QStringLiteral("Spectrum frames: buffer off"));
            }
        } else if (!spectrumFrameRecorder.isRecording()) {
            updateSpectrumFrameRecordingStatus(
                QStringLiteral("Spectrum frames: buffering %1 s").arg(spectrumFramePrebufferSeconds));
        }
        updateIqFrameProducerSettings();
        savePersistentSettings();
    });
    connect(spectrumFramePrebufferSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        spectrumFramePrebufferSeconds = (std::clamp)(value, 1, 120);
        savePersistentSettings();
    });
    connect(spectrumFrameBinsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        spectrumFramePrebuffer.clear();
        savePersistentSettings();
    });
    connect(playbackRefreshButton, &QPushButton::clicked, this, &YourClassName::refreshPlaybackFiles);
    connect(playbackButton, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            startPlayback();
        } else {
            stopPlayback();
        }
    });
    connect(playbackManager, &PlaybackManager::audioFrameReady, this, &YourClassName::handlePlaybackAudioFrame);
    connect(playbackManager, &PlaybackManager::iqFrameReady, this, &YourClassName::handlePlaybackIqFrame);
    connect(playbackManager, &PlaybackManager::started, this, &YourClassName::onPlaybackStarted);
    connect(playbackManager, &PlaybackManager::stopped, this, &YourClassName::onPlaybackStopped);
    connect(playbackManager, &PlaybackManager::statusChanged, this, &YourClassName::onPlaybackStatusChanged);
    connect(audioProcessor,
            &AudioProcessor::demodulatorFrameReady,
            this,
            [this](const QByteArray &pcmData) {
                if (pendingSettings.modulationType != MOD_DMR) {
                    processDigitalAudioFrame(pcmData);
                    sendAudioRelayFrame(pcmData);
                    sendAudioHttpFrame(pcmData);
                }
                processSstvAudioFrame(pcmData);
                processAptAudioFrame(pcmData);
                processWefaxAudioFrame(pcmData);
            },
            Qt::QueuedConnection);
    connect(audioProcessor,
            &AudioProcessor::dmrBasebandFrameReady,
            dsdNeoBridge,
            &DsdNeoBridge::submitInputPcm,
            Qt::DirectConnection);
    connect(audioProcessor,
            &AudioProcessor::dmrBasebandFrameReady,
            this,
            [this](const QByteArray &pcmData, int sampleRate) {
                if (pendingSettings.modulationType == MOD_DMR) {
                    processDigitalAudioFrame(pcmData, sampleRate);
                }
            },
            Qt::QueuedConnection);
    connect(audioProcessor, &AudioProcessor::dmrChannelIqFrameReady, this,
            [this](const QByteArray &iqData, double rate, int count) {
                if (dmrAudioIqProducerNeeded()) handleLocalIqFrame(iqData, rate, count, true);
            }, Qt::QueuedConnection);
    connect(audioProcessor,
            &AudioProcessor::audioFrameReady,
            this,
            [this](const QByteArray &pcmData) {
                if (recordingManager &&
                    recordingManager->isRecording() &&
                    recordingManager->mode() == RecordingManager::Mode::AudioWav) {
                    recordingManager->appendAudioFrame(pcmData);
                }
                sendNetworkAudioFrame(pcmData);
                if (pendingSettings.modulationType == MOD_DMR) {
                    sendAudioRelayFrame(pcmData);
                    sendAudioHttpFrame(pcmData);
                }
#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
                const bool suppressServerLocalOutput =
                    networkMode == NetworkMode::Server &&
                    serverDisableLocalVisualAudio &&
                    networkController &&
                    networkController->isControlReady();
                const bool localQtPlayback =
                    remoteAudioPlayer &&
                    audioCheckbox &&
                    audioCheckbox->isChecked() &&
                    !suppressServerLocalOutput &&
                    (networkMode != NetworkMode::Client || isClientIqProcessingMode());
                if (localQtPlayback) {
                    remoteAudioPlayer->playPcmFrame(pcmData);
                }
#endif
            },
            Qt::QueuedConnection);
    connect(audioCheckbox, &QCheckBox::toggled,
            this, &YourClassName::onAudioEnabledChanged);
    connect(languageComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!languageComboBox || index < 0) {
            return;
        }
        const QString nextLanguage = languageComboBox->itemData(index).toString();
        if (nextLanguage.isEmpty() || nextLanguage == uiLanguage) {
            return;
        }
        uiLanguage = normalizedUiLanguage(nextLanguage);
        applyUiLanguage();
        savePersistentSettings();
    });
    connect(sampleBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &YourClassName::onSampleRateChanged);
    auto applyScanUiChange = [this](bool applyNow) {
        const bool previousAgileScanEnabled = agileScanEnabled;
        const bool previousStandardScanEnabled = standardScanEnabled;
        refreshSettingsFromUi();
        updateAgileScanControls();
        publishSettingsToGlobals();
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
            return;
        }
        if (applyNow && !isIdle() && (hasActiveFobosDevice() || isExternalReceiverBackendSelected())) {
            if (isExternalReceiverBackendSelected()) {
                applyStandardScanSettings(false);
            } else if (activeFobosApiKind == FobosApiKind::Agile &&
                previousAgileScanEnabled != agileScanEnabled) {
                restartStreamForHardwareChange();
            } else if (activeFobosApiKind == FobosApiKind::Standard &&
                       previousStandardScanEnabled != standardScanEnabled) {
                applyStandardScanSettings(false);
            } else {
                applyAgileScanSettings(false);
                applyStandardScanSettings(false);
            }
        }
    };
    auto applyListeningScanUiChange = [this](bool applyNow) {
        refreshSettingsFromUi();
        updateListeningScanControls();
        publishSettingsToGlobals();
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
            if (persistentSettingsReady) {
                savePersistentSettings();
            }
            return;
        }
        if (applyNow) {
            applyListeningScanSettings(false);
        }
        if (persistentSettingsReady) {
            savePersistentSettings();
        }
    };
    connect(listeningScanCheckbox, &QCheckBox::toggled, this, [applyListeningScanUiChange](bool) {
        applyListeningScanUiChange(true);
    });
    connect(listeningScanPresetCombo, QOverload<int>::of(&QComboBox::activated), this, [this, applyListeningScanUiChange](int index) {
        const QString name = listeningScanPresetCombo ? listeningScanPresetCombo->itemData(index).toString() : QString();
        if (name.isEmpty() || !listeningScanPresets.contains(name)) {
            return;
        }
        const QString spec = listeningScanPresets.value(name);
        listeningScanTargetsMhz = listeningScanPresetTargets(spec, listeningScanTargetsMhz);
        listeningScanDwellMs = listeningScanPresetDwellMs(spec, listeningScanDwellMs);
        listeningScanSettleMs = listeningScanPresetSettleMs(spec, listeningScanSettleMs);
        if (listeningScanTargetsEdit) {
            listeningScanTargetsEdit->setText(listeningScanTargetsMhz);
        }
        if (listeningScanDwellSpin) {
            listeningScanDwellSpin->setValue(listeningScanDwellMs);
        }
        if (listeningScanSettleSpin) {
            listeningScanSettleSpin->setValue(listeningScanSettleMs);
        }
        applyListeningScanUiChange(true);
    });
    connect(listeningScanSavePresetButton, &QPushButton::clicked, this, [this]() {
        refreshSettingsFromUi();
        QString name = listeningScanPresetCombo ? listeningScanPresetCombo->currentText().trimmed() : QString();
        if (name.isEmpty()) {
            name = QStringLiteral("Listening scan %1").arg(listeningScanPresets.size() + 1);
        }
        QString error;
        parseListeningScanTargetsMhz(listeningScanTargetsMhz,
                                     0.0,
                                     RF_EXPERIMENTAL_MAX_FREQUENCY,
                                     1,
                                     &error);
        if (!error.isEmpty()) {
            if (listeningScanStatusLabel) {
                listeningScanStatusLabel->setText(error);
            }
            return;
        }
        listeningScanPresets[name] =
            listeningScanPresetSpec(listeningScanTargetsMhz, listeningScanDwellMs, listeningScanSettleMs);
        if (!listeningScanPresetOrder.contains(name)) {
            listeningScanPresetOrder.append(name);
        }
        savePersistentSettings();
        updateListeningScanControls();
    });
    connect(listeningScanDeletePresetButton, &QPushButton::clicked, this, [this]() {
        const QString name = listeningScanPresetCombo ? listeningScanPresetCombo->currentText().trimmed() : QString();
        if (name.isEmpty()) {
            return;
        }
        listeningScanPresets.remove(name);
        listeningScanPresetOrder.removeAll(name);
        savePersistentSettings();
        updateListeningScanControls();
    });
    connect(listeningScanTargetsEdit, &QLineEdit::editingFinished, this, [applyListeningScanUiChange]() {
        applyListeningScanUiChange(true);
    });
    connect(listeningScanTargetsEdit, &QLineEdit::returnPressed, this, [applyListeningScanUiChange]() {
        applyListeningScanUiChange(true);
    });
    connect(listeningScanDwellSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, applyListeningScanUiChange](int value) {
        listeningScanDwellMs = (std::clamp)(value,
                                            LISTENING_SCAN_MIN_DWELL_MS,
                                            LISTENING_SCAN_MAX_DWELL_MS);
        applyListeningScanUiChange(false);
    });
    connect(listeningScanDwellSpin, &QSpinBox::editingFinished, this, [applyListeningScanUiChange]() {
        applyListeningScanUiChange(true);
    });
    connect(listeningScanSettleSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, applyListeningScanUiChange](int value) {
        listeningScanSettleMs = (std::clamp)(value,
                                             LISTENING_SCAN_MIN_SETTLE_MS,
                                             LISTENING_SCAN_MAX_SETTLE_MS);
        applyListeningScanUiChange(false);
    });
    connect(listeningScanSettleSpin, &QSpinBox::editingFinished, this, [applyListeningScanUiChange]() {
        applyListeningScanUiChange(true);
    });
    connect(agileScanCheckbox, &QCheckBox::toggled, this, [this, applyScanUiChange](bool checked) {
        if (checked && standardScanCheckbox && standardScanCheckbox->isChecked()) {
            QSignalBlocker blocker(standardScanCheckbox);
            standardScanCheckbox->setChecked(false);
            standardScanEnabled = false;
            resetStandardScanState(true);
        }
        applyScanUiChange(true);
    });
    connect(agileScanRangesEdit, &QLineEdit::editingFinished, this, [applyScanUiChange]() {
        applyScanUiChange(true);
    });
    connect(agileScanStepSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyScanUiChange](double) {
        applyScanUiChange(false);
    });
    connect(agileScanStepSpin, &QDoubleSpinBox::editingFinished, this, [applyScanUiChange]() {
        applyScanUiChange(true);
    });
    connect(agileScanAutoStepCheckbox, &QCheckBox::toggled, this, [this, applyScanUiChange](bool checked) {
        agileScanAutoStepSampleRate = checked;
        applyAgileScanAutoStep(true);
        updateAgileScanControls();
        applyScanUiChange(true);
    });
    connect(scanVisualModeCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (!scanVisualModeCombo || index < 0) {
            return;
        }
        scanVisualMode = normalizedScanVisualMode(scanVisualModeCombo->itemData(index).toInt());
        scanVisualAssembler.reset();
        if (persistentSettingsReady) {
            savePersistentSettings();
        }
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
        }
    });
    connect(agileScanPresetCombo, QOverload<int>::of(&QComboBox::activated), this, [this, applyScanUiChange](int index) {
        const QString name = agileScanPresetCombo ? agileScanPresetCombo->itemData(index).toString() : QString();
        if (name.isEmpty() || !agileScanPresets.contains(name)) {
            return;
        }
        const QString spec = agileScanPresets.value(name);
        if (agileScanRangesEdit) {
            agileScanRangesEdit->setText(agileScanPresetRanges(spec, agileScanRangesMhz));
        }
        if (agileScanStepSpin) {
            agileScanStepSpin->setValue(agileScanPresetStepMhz(spec, agileScanStepMhz));
        }
        applyScanUiChange(true);
    });
    connect(standardScanCheckbox, &QCheckBox::toggled, this, [this, applyScanUiChange](bool checked) {
        if (checked && agileScanCheckbox && agileScanCheckbox->isChecked()) {
            QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(false);
            agileScanEnabled = false;
        }
        applyScanUiChange(true);
    });
    connect(scanListeningLockCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        scanListeningLockEnabled = checked;
        settingRange();
        if (persistentSettingsReady) {
            savePersistentSettings();
        }
    });
    connect(standardScanPresetCombo, QOverload<int>::of(&QComboBox::activated), this, [this, applyScanUiChange](int index) {
        const QString name = standardScanPresetCombo ? standardScanPresetCombo->itemData(index).toString() : QString();
        if (name.isEmpty() || !standardScanPresets.contains(name)) {
            return;
        }
        const QString spec = standardScanPresets.value(name);
        standardScanCentersMhz = standardScanPresetCenters(spec, standardScanCentersMhz);
        standardScanDwellMs = standardScanPresetDwellMs(spec, standardScanDwellMs);
        standardScanSettleMs = standardScanPresetSettleMs(spec, standardScanSettleMs);
        if (standardScanCentersEdit) {
            standardScanCentersEdit->setText(standardScanCentersMhz);
        }
        if (standardScanDwellSpin) {
            standardScanDwellSpin->setValue(standardScanDwellMs);
        }
        if (standardScanSettleSpin) {
            standardScanSettleSpin->setValue(standardScanSettleMs);
        }
        applyScanUiChange(true);
    });
    connect(standardScanSavePresetButton, &QPushButton::clicked, this, [this]() {
        refreshSettingsFromUi();
        QString name = standardScanPresetCombo ? standardScanPresetCombo->currentText().trimmed() : QString();
        if (name.isEmpty()) {
            name = QStringLiteral("Standard scan %1").arg(standardScanPresets.size() + 1);
        }
        QString error;
        parseStandardScanCentersMhz(standardScanCentersMhz,
                                    pendingSettings.sampleRate,
                                    AGILE_SCAN_MIN_POINTS,
                                    &error,
                                    nullptr);
        if (!error.isEmpty()) {
            if (standardScanStatusLabel) {
                standardScanStatusLabel->setText(error);
            }
            return;
        }
        standardScanPresets[name] =
            standardScanPresetSpec(standardScanCentersMhz, standardScanDwellMs, standardScanSettleMs);
        if (!standardScanPresetOrder.contains(name)) {
            standardScanPresetOrder.append(name);
        }
        savePersistentSettings();
        updateAgileScanControls();
    });
    connect(standardScanDeletePresetButton, &QPushButton::clicked, this, [this]() {
        const QString name = standardScanPresetCombo ? standardScanPresetCombo->currentText().trimmed() : QString();
        if (name.isEmpty()) {
            return;
        }
        standardScanPresets.remove(name);
        standardScanPresetOrder.removeAll(name);
        savePersistentSettings();
        updateAgileScanControls();
    });
    connect(standardScanCentersEdit, &QLineEdit::editingFinished, this, [applyScanUiChange]() {
        applyScanUiChange(true);
    });
    auto addStandardScanCenter = [this, applyScanUiChange](int direction) {
        refreshSettingsFromUi();
        const double stepHz = pendingSettings.sampleRate;
        if (!std::isfinite(stepHz) || stepHz <= 0.0) {
            if (standardScanStatusLabel) {
                standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_bad_sample_rate"),
                                                        QStringLiteral("Standard scan: bad sample rate")));
            }
            return;
        }

        QString error;
        bool adjusted = false;
        QVector<double> centers = parseStandardScanCentersMhz(standardScanCentersMhz,
                                                              stepHz,
                                                              0,
                                                              &error,
                                                              &adjusted);
        if (!error.isEmpty()) {
            if (standardScanStatusLabel) {
                standardScanStatusLabel->setText(error);
            }
            return;
        }
        if (centers.isEmpty()) {
            const double seed = pendingSettings.inputMode == INPUT_RF && std::isfinite(pendingSettings.centerFrequency)
                                    ? pendingSettings.centerFrequency
                                    : RF_MIN_CENTER_FREQUENCY;
            centers.push_back(seed);
        }

        std::sort(centers.begin(), centers.end());
        const double nextCenter = direction < 0 ? centers.first() - stepHz
                                                : centers.last() + stepHz;
        if (!std::isfinite(nextCenter) ||
            nextCenter < RF_MIN_CENTER_FREQUENCY ||
            nextCenter > RF_EXPERIMENTAL_MAX_FREQUENCY) {
            if (standardScanStatusLabel) {
                standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_add_out_of_range"),
                                                        QStringLiteral("Standard scan: new center is out of range")));
            }
            return;
        }
        centers.push_back(nextCenter);
        std::sort(centers.begin(), centers.end());
        standardScanCentersMhz = formatMhzList(centers);
        if (standardScanCentersEdit) {
            QSignalBlocker blocker(standardScanCentersEdit);
            standardScanCentersEdit->setText(standardScanCentersMhz);
        }
        applyScanUiChange(true);
    };
    connect(standardScanAddLowerButton, &QPushButton::clicked, this, [addStandardScanCenter]() {
        addStandardScanCenter(-1);
    });
    connect(standardScanAddUpperButton, &QPushButton::clicked, this, [addStandardScanCenter]() {
        addStandardScanCenter(1);
    });
    auto removeStandardScanCenter = [this, applyScanUiChange](int direction) {
        refreshSettingsFromUi();
        QString error;
        bool adjusted = false;
        QVector<double> centers = parseStandardScanCentersMhz(standardScanCentersMhz,
                                                              pendingSettings.sampleRate,
                                                              0,
                                                              &error,
                                                              &adjusted);
        if (!error.isEmpty()) {
            if (standardScanStatusLabel) {
                standardScanStatusLabel->setText(error);
            }
            return;
        }
        if (centers.isEmpty()) {
            if (standardScanStatusLabel) {
                standardScanStatusLabel->setText(uiText(QStringLiteral("standard_scan_empty_list"),
                                                        QStringLiteral("Standard scan centers are empty")));
            }
            return;
        }
        std::sort(centers.begin(), centers.end());
        if (direction < 0) {
            centers.removeFirst();
        } else {
            centers.removeLast();
        }
        standardScanCentersMhz = formatMhzList(centers);
        if (standardScanCentersEdit) {
            QSignalBlocker blocker(standardScanCentersEdit);
            standardScanCentersEdit->setText(standardScanCentersMhz);
        }
        applyScanUiChange(true);
    };
    connect(standardScanRemoveLowerButton, &QPushButton::clicked, this, [removeStandardScanCenter]() {
        removeStandardScanCenter(-1);
    });
    connect(standardScanRemoveUpperButton, &QPushButton::clicked, this, [removeStandardScanCenter]() {
        removeStandardScanCenter(1);
    });
    auto fillStandardScanRange = [this, applyScanUiChange]() {
        refreshSettingsFromUi();
        applyStandardScanRangeToCenters();
        applyScanUiChange(true);
    };
    connect(standardScanFillRangeButton, &QPushButton::clicked, this, fillStandardScanRange);
    connect(standardScanRangeStartEdit, &QLineEdit::returnPressed, this, fillStandardScanRange);
    connect(standardScanRangeEndEdit, &QLineEdit::returnPressed, this, fillStandardScanRange);
    connect(standardScanDwellSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        standardScanDwellMs = (std::clamp)(value,
                                           STANDARD_SCAN_MIN_DWELL_MS,
                                           STANDARD_SCAN_MAX_DWELL_MS);
        if (persistentSettingsReady) {
            savePersistentSettings();
        }
    });
    connect(standardScanSettleSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        standardScanSettleMs = (std::clamp)(value,
                                            STANDARD_SCAN_MIN_SETTLE_MS,
                                            STANDARD_SCAN_MAX_SETTLE_MS);
        if (persistentSettingsReady) {
            savePersistentSettings();
        }
    });
    connect(agileScanSavePresetButton, &QPushButton::clicked, this, &YourClassName::saveAgileScanPreset);
    connect(agileScanDeletePresetButton, &QPushButton::clicked, this, &YourClassName::deleteAgileScanPreset);
    connect(scanMeasurementCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        scanMeasurementEnabled = checked;
        if (!checked && scanMeasurementBaselineButton) {
            QSignalBlocker blocker(scanMeasurementBaselineButton);
            scanMeasurementBaselineButton->setChecked(false);
            scanMeasurementBaselineRecording = false;
            scanMeasurementBaselineButton->setText(uiText(QStringLiteral("bg_rec"), QStringLiteral("BG Rec")));
        }
        updateAgileScanControls();
        updateScanMeasurementStatus();
        if (persistentSettingsReady) {
            savePersistentSettings();
        }
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
        }
    });
    connect(scanMeasurementBinSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            [this](double value) {
                scanMeasurementBinMhz = (std::clamp)(value,
                                                     SCAN_MEASUREMENT_MIN_BIN_MHZ,
                                                     SCAN_MEASUREMENT_MAX_BIN_MHZ);
                clearScanMeasurement();
                if (persistentSettingsReady) {
                    savePersistentSettings();
                }
                if (isNetworkClientMode()) {
                    scheduleRemoteSettingsCommand();
                }
            });
    connect(scanMeasurementBaselineButton, &QPushButton::toggled, this, [this](bool checked) {
        scanMeasurementBaselineRecording = checked;
        if (scanMeasurementBaselineButton) {
            scanMeasurementBaselineButton->setText(checked
                                                       ? uiText(QStringLiteral("stop_bg"), QStringLiteral("Stop BG"))
                                                       : uiText(QStringLiteral("bg_rec"), QStringLiteral("BG Rec")));
        }
        if (checked) {
            scanMeasurementEnabled = true;
            if (scanMeasurementCheckbox) {
                QSignalBlocker blocker(scanMeasurementCheckbox);
                scanMeasurementCheckbox->setChecked(true);
            }
            for (auto &bin : scanMeasurementBins) {
                bin.baselineDb = -160.0f;
                bin.baselineCount = 0;
            }
        }
        updateScanMeasurementStatus();
    });
    connect(scanMeasurementResetPeakButton, &QPushButton::clicked, this, &YourClassName::resetScanMeasurementPeaks);
    connect(scanMeasurementExportButton, &QPushButton::clicked, this, &YourClassName::exportScanMeasurementCsv);
    connect(spectrumScienceMarkerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        spectrumScienceActiveMarker = (std::clamp)(index, 0, spectrumScienceAnalyzer.markerCount() - 1);
        updateSpectrumScienceUi();
    });
    connect(spectrumScienceSetButton, &QPushButton::clicked, this, [this]() {
        setSpectrumScienceMarker(pendingSettings.listeningFrequency);
    });
    connect(spectrumSciencePeakButton, &QPushButton::clicked, this, [this]() {
        spectrumSciencePendingAction = SpectrumScienceActionPeak;
    });
    connect(spectrumSciencePreviousButton, &QPushButton::clicked, this, [this]() {
        spectrumSciencePendingAction = SpectrumScienceActionPreviousPeak;
    });
    connect(spectrumScienceNextButton, &QPushButton::clicked, this, [this]() {
        spectrumSciencePendingAction = SpectrumScienceActionNextPeak;
    });
    connect(spectrumScienceClearButton, &QPushButton::clicked, this, [this]() {
        spectrumScienceAnalyzer.clearMarker(spectrumScienceActiveMarker);
        updateSpectrumScienceUi();
    });
    auto applyScienceTraceSettings = [this]() {
        spectrumScienceMaxHoldEnabled = spectrumScienceMaxHoldCheckbox && spectrumScienceMaxHoldCheckbox->isChecked();
        spectrumScienceMinHoldEnabled = spectrumScienceMinHoldCheckbox && spectrumScienceMinHoldCheckbox->isChecked();
        spectrumScienceAverageEnabled = spectrumScienceAverageCheckbox && spectrumScienceAverageCheckbox->isChecked();
        spectrumScienceAnalyzer.setTraceEnabled(spectrumScienceMaxHoldEnabled,
                                                spectrumScienceMinHoldEnabled,
                                                spectrumScienceAverageEnabled);
        updateSpectrumScienceUi();
        if (persistentSettingsReady) savePersistentSettings();
    };
    connect(spectrumScienceMaxHoldCheckbox, &QCheckBox::toggled, this, [applyScienceTraceSettings](bool) { applyScienceTraceSettings(); });
    connect(spectrumScienceMinHoldCheckbox, &QCheckBox::toggled, this, [applyScienceTraceSettings](bool) { applyScienceTraceSettings(); });
    connect(spectrumScienceAverageCheckbox, &QCheckBox::toggled, this, [applyScienceTraceSettings](bool) { applyScienceTraceSettings(); });
    connect(spectrumScienceAverageSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double seconds) {
        spectrumScienceAverageSeconds = (std::clamp)(seconds, 0.05, 60.0);
        spectrumScienceAnalyzer.setAverageTimeSeconds(spectrumScienceAverageSeconds);
        if (persistentSettingsReady) savePersistentSettings();
    });
    connect(spectrumScienceResetButton, &QPushButton::clicked, this, [this]() {
        spectrumScienceAnalyzer.resetTraces();
        updateSpectrumScienceUi();
    });
    connect(spectrumScienceExportButton, &QPushButton::clicked, this, [this]() {
        spectrumScienceExportPending = true;
    });
    connect(zeroSpanButton, &QPushButton::clicked, this, &YourClassName::openZeroSpanDialog);
    connect(researchToolsButton, &QPushButton::clicked, this, [this]() {
        openResearchAnalysis(0);
    });
    connect(waterfallAreaMeasurementCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        waterfallAreaMeasurementEnabled = checked;
        if (waterfallWidget) {
            waterfallWidget->setAreaMeasurementEnabled(checked);
        }
        if (persistentSettingsReady) {
            savePersistentSettings();
        }
    });
    connect(hfInterferenceAnalyzeButton, &QPushButton::clicked, this, [this]() {
        openResearchAnalysis(0);
    });
    connect(dmrHunterControls, &SpectrumHunterControls::detectToggled, this, [this](bool checked) {
        dmrHunterSettings.enabled = checked;
        if (!checked) {
            dmrHunterCandidates.clear();
            dmrHunterCandidateIndex = -1;
        }
        updateDmrHunterControls();
        savePersistentSettings();
    });
    connect(dmrHunterControls,
            &SpectrumHunterControls::minWidthChanged,
            this,
            [this](double value) {
                dmrHunterSettings.minWidthKhz = (std::clamp)(value,
                                                             DmrHunterDetector::MinWidthKhz,
                                                             DmrHunterDetector::MaxWidthKhz);
                dmrHunterSettings = DmrHunterDetector::normalizedSettings(dmrHunterSettings);
                dmrHunterCandidates.clear();
                dmrHunterCandidateIndex = -1;
                updateDmrHunterControls();
                savePersistentSettings();
            });
    connect(dmrHunterControls,
            &SpectrumHunterControls::maxWidthChanged,
            this,
            [this](double value) {
                dmrHunterSettings.maxWidthKhz = (std::clamp)(value,
                                                             DmrHunterDetector::MinWidthKhz,
                                                             DmrHunterDetector::MaxWidthKhz);
                dmrHunterSettings = DmrHunterDetector::normalizedSettings(dmrHunterSettings);
                dmrHunterCandidates.clear();
                dmrHunterCandidateIndex = -1;
                updateDmrHunterControls();
                savePersistentSettings();
            });
    connect(dmrHunterControls,
            &SpectrumHunterControls::thresholdChanged,
            this,
            [this](double value) {
                dmrHunterSettings.thresholdDb = (std::clamp)(value,
                                                             DmrHunterDetector::MinThresholdDb,
                                                             DmrHunterDetector::MaxThresholdDb);
                dmrHunterSettings = DmrHunterDetector::normalizedSettings(dmrHunterSettings);
                dmrHunterCandidates.clear();
                dmrHunterCandidateIndex = -1;
                updateDmrHunterControls();
                savePersistentSettings();
            });
    connect(dmrHunterControls,
            &SpectrumHunterControls::applyPresetRequested,
            this,
            &YourClassName::applyDmrHunterPresetToScan);
    connect(dmrHunterControls,
            &SpectrumHunterControls::tuneRequested,
            this,
            &YourClassName::tuneDmrHunterCandidate);
    connect(dmrHunterControls,
            &SpectrumHunterControls::previousCandidateRequested,
            this,
            [this]() {
                selectDmrHunterCandidate(-1);
            });
    connect(dmrHunterControls,
            &SpectrumHunterControls::nextCandidateRequested,
            this,
            [this]() {
                selectDmrHunterCandidate(1);
            });
    connect(fpvHunterControls, &SpectrumHunterControls::detectToggled, this, [this](bool checked) {
        fpvHunterSettings.enabled = checked;
        if (!checked) {
            if (fpvAutoScanArmed) {
                fpvAutoScanArmed = false;
                stopFpvAutoScan(true);
            }
            fpvHunterTrack = {};
            fpvHunterCandidates.clear();
            fpvHunterCandidateIndex = -1;
            fpvHunterLastFollowCenterHz = std::numeric_limits<double>::quiet_NaN();
            fpvHunterLastFollowBandwidthHz = std::numeric_limits<double>::quiet_NaN();
        }
        updateFpvHunterControls();
        savePersistentSettings();
    });
    connect(fpvHunterControls,
            &SpectrumHunterControls::minWidthChanged,
            this,
            [this](double value) {
                fpvHunterSettings.minWidthMhz = (std::clamp)(value,
                                                             FpvHunterDetector::MinWidthMhz,
                                                             FpvHunterDetector::MaxWidthMhz);
                fpvHunterSettings = FpvHunterDetector::normalizedSettings(fpvHunterSettings);
                fpvHunterTrack = {};
                fpvHunterCandidates.clear();
                fpvHunterCandidateIndex = -1;
                fpvHunterLastFollowCenterHz = std::numeric_limits<double>::quiet_NaN();
                fpvHunterLastFollowBandwidthHz = std::numeric_limits<double>::quiet_NaN();
                updateFpvHunterControls();
                savePersistentSettings();
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::maxWidthChanged,
            this,
            [this](double value) {
                fpvHunterSettings.maxWidthMhz = (std::clamp)(value,
                                                             FpvHunterDetector::MinWidthMhz,
                                                             FpvHunterDetector::MaxWidthMhz);
                fpvHunterSettings = FpvHunterDetector::normalizedSettings(fpvHunterSettings);
                fpvHunterTrack = {};
                fpvHunterCandidates.clear();
                fpvHunterCandidateIndex = -1;
                fpvHunterLastFollowCenterHz = std::numeric_limits<double>::quiet_NaN();
                fpvHunterLastFollowBandwidthHz = std::numeric_limits<double>::quiet_NaN();
                updateFpvHunterControls();
                savePersistentSettings();
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::thresholdChanged,
            this,
            [this](double value) {
                fpvHunterSettings.thresholdDb = (std::clamp)(value,
                                                             FpvHunterDetector::MinThresholdDb,
                                                             FpvHunterDetector::MaxThresholdDb);
                fpvHunterSettings = FpvHunterDetector::normalizedSettings(fpvHunterSettings);
                fpvHunterTrack = {};
                fpvHunterCandidates.clear();
                fpvHunterCandidateIndex = -1;
                fpvHunterLastFollowCenterHz = std::numeric_limits<double>::quiet_NaN();
                fpvHunterLastFollowBandwidthHz = std::numeric_limits<double>::quiet_NaN();
                updateFpvHunterControls();
                savePersistentSettings();
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::applyPresetRequested,
            this,
            &YourClassName::applyFpvHunterPresetToScan);
    connect(fpvHunterControls,
            &SpectrumHunterControls::tuneRequested,
            this,
            &YourClassName::tuneFpvHunterCandidate);
    connect(fpvHunterControls,
            &SpectrumHunterControls::followToggled,
            this,
            [this](bool checked) {
                fpvHunterFollowEnabled = checked;
                fpvHunterLastFollowCenterHz = std::numeric_limits<double>::quiet_NaN();
                fpvHunterLastFollowBandwidthHz = std::numeric_limits<double>::quiet_NaN();
                updateFpvHunterControls();
                if (checked &&
                    fpvHunterCandidateIndex >= 0 &&
                    fpvHunterCandidateIndex < static_cast<int>(fpvHunterCandidates.size())) {
                    tuneFpvHunterCandidateIndex(fpvHunterCandidateIndex);
                }
                savePersistentSettings();
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::previousCandidateRequested,
            this,
            [this]() {
                selectFpvHunterCandidate(-1);
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::nextCandidateRequested,
            this,
            [this]() {
                selectFpvHunterCandidate(1);
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::autoScanToggled,
            this,
            [this](bool checked) {
                fpvAutoScanArmed = checked;
                if (checked) {
                    fpvHunterSettings.enabled = true;
                    fpvHunterFollowEnabled = false;
                    updateFpvHunterControls();
                    if (runState == RadioRunState::Running) {
                        startFpvAutoScan();
                    } else {
                        updateFpvAutoScanStatus();
                    }
                } else {
                    stopFpvAutoScan(true);
                }
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::autoScanDwellChanged,
            this,
            [this](int value) {
                fpvAutoScanDwellMs = (std::clamp)(value, 50, 5000);
                if (fpvAutoScanController.active() &&
                    fpvAutoScanBackend == FpvAutoScanBackend::StandardRetune) {
                    standardScanDwellMs = fpvAutoScanDwellMs;
                }
                savePersistentSettings();
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::autoScanLossTimeoutChanged,
            this,
            [this](int value) {
                fpvAutoScanLossTimeoutMs = (std::clamp)(value, 500, 30000);
                FpvAutoScanController::Settings settings = fpvAutoScanController.settings();
                settings.signalLossTimeoutMs = fpvAutoScanLossTimeoutMs;
                fpvAutoScanController.setSettings(settings);
                savePersistentSettings();
            });
    connect(fpvHunterControls,
            &SpectrumHunterControls::autoScanMinimumLockChanged,
            this,
            [this](int value) {
                fpvAutoScanMinimumLockPercent = (std::clamp)(value, 1, 100);
                FpvAutoScanController::Settings settings = fpvAutoScanController.settings();
                settings.minimumHsyncPercent = fpvAutoScanMinimumLockPercent;
                fpvAutoScanController.setSettings(settings);
                savePersistentSettings();
            });
    connect(fpvHunterHistoryTuneButton,
            &QPushButton::clicked,
            this,
            &YourClassName::tuneFpvHunterHistorySelection);
    connect(fpvHunterHistoryClearButton,
            &QPushButton::clicked,
            this,
            &YourClassName::clearFpvHunterHistory);
    connect(digitalVideoHunterControls, &SpectrumHunterControls::detectToggled, this, [this](bool checked) {
        digitalVideoHunterSettings.enabled = checked;
        if (!checked) {
            digitalVideoHunterCandidates.clear();
            digitalVideoHunterCandidateIndex = -1;
        }
        updateDigitalVideoHunterControls();
        savePersistentSettings();
    });
    connect(digitalVideoHunterControls,
            &SpectrumHunterControls::minWidthChanged,
            this,
            [this](double value) {
                digitalVideoHunterSettings.minWidthMhz = (std::clamp)(value,
                                                                      DigitalVideoHunterDetector::MinWidthMhz,
                                                                      DigitalVideoHunterDetector::MaxWidthMhz);
                digitalVideoHunterSettings = DigitalVideoHunterDetector::normalizedSettings(digitalVideoHunterSettings);
                digitalVideoHunterCandidates.clear();
                digitalVideoHunterCandidateIndex = -1;
                updateDigitalVideoHunterControls();
                savePersistentSettings();
            });
    connect(digitalVideoHunterControls,
            &SpectrumHunterControls::maxWidthChanged,
            this,
            [this](double value) {
                digitalVideoHunterSettings.maxWidthMhz = (std::clamp)(value,
                                                                      DigitalVideoHunterDetector::MinWidthMhz,
                                                                      DigitalVideoHunterDetector::MaxWidthMhz);
                digitalVideoHunterSettings = DigitalVideoHunterDetector::normalizedSettings(digitalVideoHunterSettings);
                digitalVideoHunterCandidates.clear();
                digitalVideoHunterCandidateIndex = -1;
                updateDigitalVideoHunterControls();
                savePersistentSettings();
            });
    connect(digitalVideoHunterControls,
            &SpectrumHunterControls::thresholdChanged,
            this,
            [this](double value) {
                digitalVideoHunterSettings.thresholdDb = (std::clamp)(value,
                                                                      DigitalVideoHunterDetector::MinThresholdDb,
                                                                      DigitalVideoHunterDetector::MaxThresholdDb);
                digitalVideoHunterSettings = DigitalVideoHunterDetector::normalizedSettings(digitalVideoHunterSettings);
                digitalVideoHunterCandidates.clear();
                digitalVideoHunterCandidateIndex = -1;
                updateDigitalVideoHunterControls();
                savePersistentSettings();
            });
    connect(digitalVideoHunterControls,
            &SpectrumHunterControls::applyPresetRequested,
            this,
            &YourClassName::applyDigitalVideoHunterPresetToScan);
    connect(digitalVideoHunterControls,
            &SpectrumHunterControls::tuneRequested,
            this,
            &YourClassName::tuneDigitalVideoHunterCandidate);
    connect(digitalVideoHunterControls,
            &SpectrumHunterControls::previousCandidateRequested,
            this,
            [this]() {
                selectDigitalVideoHunterCandidate(-1);
            });
    connect(digitalVideoHunterControls,
            &SpectrumHunterControls::nextCandidateRequested,
            this,
            [this]() {
                selectDigitalVideoHunterCandidate(1);
            });
    connect(spurSuppressionCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        spurSuppressionEnabled = checked;
        updateSpurSuppressionStatus();
        savePersistentSettings();
    });
    connect(spurCalibrateButton, &QPushButton::clicked, this, &YourClassName::startSpurCalibration);
    connect(spurClearButton, &QPushButton::clicked, this, &YourClassName::clearSpurMask);
    connect(presetManagerButton, &QPushButton::clicked, this, &YourClassName::openPresetManager);
    connect(zoomSpectrumButton, &QPushButton::clicked, this, &YourClassName::openZoomSpectrum);
    connect(zoomDensityButton, &QPushButton::clicked, this, &YourClassName::openZoomDensity);
    connect(appSettingsButton, &QPushButton::clicked, this, &YourClassName::openApplicationSettings);
    connect(dspPathButton, &QPushButton::clicked, this, [this]() {
        ensureDspFlowPanel();
        dspFlowPanel->setWorkspaceMode(false);
        dspFlowPanel->show();
        dspFlowPanel->raise();
        dspFlowPanel->activateWindow();
    });
    connect(bandwidthControl, &FrequencyControl::valueCommitted, this, [this](double) {
        onBandwidthChanged();
    });
    connect(scaleWidget, SIGNAL(frequencyChanged()), this, SLOT(updateFrequency()));
    connect(scaleWidget, SIGNAL(centralFrequencyChanged()), this, SLOT(updateCentralFrequency()));
    connect(scaleWidget, &ScaleWidget::tuningChanged, this, &YourClassName::updateTuningFromScale);
    connect(waterfallWidget, &MyWaterfallWidget::scaleChanged, this, &YourClassName::onWaterfallScaleChanged);
    connect(graphWidget, &MyGraphWidget::scaleChanged, this, &YourClassName::onWaterfallScaleChanged);
    connect(waterfallWidget, &MyWaterfallWidget::panRequested, this, &YourClassName::panSpectrumView);
    connect(graphWidget, &MyGraphWidget::panRequested, this, &YourClassName::panSpectrumView);
    connect(graphWidget, &MyGraphWidget::tuneContextRequested, this, &YourClassName::showTuneContextMenu);
    connect(waterfallWidget, &MyWaterfallWidget::tuneContextRequested, this, &YourClassName::showTuneContextMenu);
    connect(graphWidget, &MyGraphWidget::autoTuneRequested, this, &YourClassName::tuneSignalCenterAt);
    connect(waterfallWidget, &MyWaterfallWidget::autoTuneRequested, this, &YourClassName::tuneSignalCenterAt);
    connect(graphWidget, &MyGraphWidget::scienceMarkerRequested, this, &YourClassName::setSpectrumScienceMarker);
    connect(graphWidget, &MyGraphWidget::multiVfoSelectionRequested, this,
            [this](double lowHz, double highHz) {
                if (!multiVfoWidget) return;
                const int channelIndex = multiVfoWidget->addSelection(lowHz, highHz);
                if (dspFlowPanel && channelIndex >= 0) {
                    dspFlowPanel->addMultiVfoBranch(channelIndex);
                }
            });
    spectrumScienceAnalyzer.setTraceEnabled(spectrumScienceMaxHoldEnabled,
                                            spectrumScienceMinHoldEnabled,
                                            spectrumScienceAverageEnabled);
    spectrumScienceAnalyzer.setAverageTimeSeconds(spectrumScienceAverageSeconds);
    spectrumScienceAnalyzer.setAverageFrameCount(spectrumAverageFrameCount);
    spectrumScienceAnalyzer.setDetector(spectrumDetectorMode, spectrumDetectorFrames);
    spectrumScienceAnalyzer.setVbwHz(spectrumVbwHz);
    spectrumScienceAnalyzer.setPercentileTraces(spectrumPercentile50Enabled,
                                                spectrumPercentile90Enabled,
                                                spectrumPercentile99Enabled);
    connect(waterfallDisplayModeCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                if (!waterfallDisplayModeCombo || !waterfallWidget || index < 0) {
                    return;
                }
                const int requestedMode = waterfallDisplayModeCombo->itemData(index).toInt();
                if (waterfall3DAlternativeView &&
                    requestedMode == static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall2D)) {
                    applyAlternativeInterfaceMode();
                    return;
                }
                waterfallDisplayMode = requestedMode;
                waterfallWidget->setDisplayMode(
                    static_cast<MyWaterfallWidget::DisplayMode>(waterfallDisplayMode));
                savePersistentSettings();
            });
    connect(waterfall3DResolutionCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                if (!waterfall3DResolutionCombo || !waterfallWidget || index < 0) {
                    return;
                }
                waterfall3DResolutionDivisor = waterfall3DResolutionCombo->itemData(index).toInt();
                waterfallWidget->set3DResolutionDivisor(waterfall3DResolutionDivisor);
                savePersistentSettings();
            });
    connect(waterfall3DSurfaceStyleCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                if (!waterfall3DSurfaceStyleCombo || index < 0) return;
                waterfall3DSurfaceStyle = waterfall3DSurfaceStyleCombo->itemData(index).toInt();
                if (waterfallWidget) waterfallWidget->set3DSurfaceStyle(waterfall3DSurfaceStyle);
                savePersistentSettings();
            });
    connect(waterfall3DSmoothingCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                if (!waterfall3DSmoothingCombo || index < 0) return;
                waterfall3DSmoothing = waterfall3DSmoothingCombo->itemData(index).toInt();
                if (waterfallWidget) waterfallWidget->set3DSurfaceSmoothing(waterfall3DSmoothing);
                savePersistentSettings();
            });
    connect(waterfall3DLightingCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                if (!waterfall3DLightingCombo || index < 0) return;
                waterfall3DLighting = waterfall3DLightingCombo->itemData(index).toInt();
                if (waterfallWidget) waterfallWidget->set3DSurfaceLighting(waterfall3DLighting);
                savePersistentSettings();
            });
    connect(waterfall3DHistoryRowsSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                waterfall3DHistoryRows = value;
                if (waterfallWidget) {
                    waterfallWidget->set3DHistoryRows(value);
                }
                savePersistentSettings();
            });
    connect(waterfall3DSliceStepSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                waterfall3DSliceScrollStep = value;
                if (waterfallWidget) {
                    waterfallWidget->set3DSliceScrollStep(value);
                }
                savePersistentSettings();
            });
    connect(waterfall3DSliceWidthSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                waterfall3DSliceWidth = value;
                if (waterfallWidget) {
                    waterfallWidget->set3DSliceWidth(value);
                }
                savePersistentSettings();
            });
    connect(waterfall3DSpectrumSliceStepSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                waterfall3DSpectrumSliceScrollStep = value;
                if (waterfallWidget) {
                    waterfallWidget->set3DSpectrumSliceScrollStep(value);
                }
                savePersistentSettings();
            });
    connect(waterfall3DSpectrumSliceRowsSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                waterfall3DSpectrumSliceRows = value;
                if (waterfallWidget) {
                    waterfallWidget->set3DSpectrumSliceWidth(value);
                }
                savePersistentSettings();
            });
    connect(waterfall3DSpectrumSliceCaptureCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                waterfall3DSpectrumSliceCapture = checked;
                if (waterfallWidget) {
                    waterfallWidget->set3DSpectrumSliceCapture(checked);
                }
                if (waterfall3DSpectrumSliceCaptureFixedCheckbox) {
                    waterfall3DSpectrumSliceCaptureFixedCheckbox->setEnabled(checked);
                }
                savePersistentSettings();
            });
    connect(waterfall3DSpectrumSliceCaptureFixedCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                waterfall3DSpectrumSliceCaptureFixed = checked;
                if (waterfallWidget) {
                    waterfallWidget->set3DSpectrumSliceCaptureFixed(checked);
                }
                savePersistentSettings();
            });
    connect(waterfall3DFixedPlaneCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                waterfall3DFixedPlane = checked;
                if (waterfallWidget) {
                    waterfallWidget->set3DFixedPlane(checked);
                }
                const bool gradientAvailable = waterfall3DAlternativeView || checked;
                if (alternativeSpectrumGradientCheckbox) {
                    alternativeSpectrumGradientCheckbox->setEnabled(gradientAvailable);
                }
                if (alternativeSpectrumGradientOpacitySlider) {
                    alternativeSpectrumGradientOpacitySlider->setEnabled(
                        gradientAvailable && alternativeSpectrumGradientFill);
                }
                savePersistentSettings();
            });
    connect(waterfall3DMonochromeCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                waterfall3DMonochrome = checked;
                if (waterfallWidget) waterfallWidget->set3DMonochrome(checked);
                savePersistentSettings();
            });
    connect(waterfall3DHeightGradientCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                waterfall3DHeightGradient = checked;
                if (waterfallWidget) waterfallWidget->set3DHeightGradient(checked);
                savePersistentSettings();
            });
    connect(waterfall3DMonochromeColorButton,
            &QPushButton::clicked,
            this,
            [this, updateMonochromeColorButton]() {
                const QColor selected = QColorDialog::getColor(
                    waterfall3DMonochromeColor,
                    this,
                    uiText(QStringLiteral("waterfall_3d_monochrome_color_dialog"),
                           QStringLiteral("Choose 3D color")));
                if (!selected.isValid()) return;
                waterfall3DMonochromeColor = selected;
                updateMonochromeColorButton();
                if (waterfallWidget) waterfallWidget->set3DMonochromeColor(selected);
                savePersistentSettings();
            });
    connect(waterfall3DMonochromeGradientDepthSlider,
            &QSlider::valueChanged,
            this,
            [this](int value) {
                waterfall3DMonochromeGradientDepth = value;
                if (waterfall3DMonochromeGradientDepthValueLabel) {
                    waterfall3DMonochromeGradientDepthValueLabel->setText(
                        QStringLiteral("%1%").arg(value));
                }
                if (waterfallWidget) {
                    waterfallWidget->set3DMonochromeGradientDepth(value);
                }
                savePersistentSettings();
            });
    connect(waterfall3DAlternativeViewCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                waterfall3DAlternativeView = checked;
                applyAlternativeInterfaceMode();
                savePersistentSettings();
            });
    connect(waterfall3DVncSliceInputCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                waterfall3DVncSliceInput = checked;
                if (waterfallWidget) {
                    waterfallWidget->set3DModifierFreeSliceInput(checked);
                }
                savePersistentSettings();
            });
    connect(alternativeSpectrumGradientCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                alternativeSpectrumGradientFill = checked;
                if (alternativeSpectrumGradientOpacitySlider) {
                    alternativeSpectrumGradientOpacitySlider->setEnabled(
                        (waterfall3DAlternativeView || waterfall3DFixedPlane) && checked);
                }
                if (waterfallWidget) {
                    waterfallWidget->setAlternativeSpectrumGradientFill(checked);
                }
                savePersistentSettings();
            });
    connect(alternativeSpectrumGradientOpacitySlider,
            &QSlider::valueChanged,
            this,
            [this](int value) {
                alternativeSpectrumGradientOpacity = value;
                if (alternativeSpectrumGradientOpacityValueLabel) {
                    alternativeSpectrumGradientOpacityValueLabel->setText(
                        QStringLiteral("%1%").arg(value));
                }
                if (waterfallWidget) {
                    waterfallWidget->setAlternativeSpectrumGradientOpacity(value);
                }
                savePersistentSettings();
            });
    onVgaGainChanged(3);
    onLnaGainChanged(1);
    populateSampleRates();
    populateAudioDevices();
    refreshSettingsFromUi();
    loadPersistentSettings();
    publishSettingsToGlobals();
    updateFineTuneControlMode();
    updateUiFromPendingSettings();
    settingRange();
    updateGraphBandMarkers();
    updateAgileScanControls();
    updateScanMeasurementStatus();
    updateSpurSuppressionStatus();
    updateDmrHunterControls();
    updateFpvHunterControls();
    updateDigitalVideoHunterControls();
    updateDigitalDecoderMode();
    updateVideoProcessorMode();
    updateAudioRelaySocket();
    updateAudioHttpStreamServer();
    updateUiForRunState();
    refreshPlaybackFiles();
    persistentSettingsReady = true;
    qApp->installEventFilter(this);
    QTimer::singleShot(100, this, [this]() {
        prepareFobosSessionFromSettings(QStringLiteral("startup"));
    });
}

YourClassName::~YourClassName() {
    qApp->removeEventFilter(this);
    if (dspFlowPanel) {
        dspFlowPanel->hide();
        delete dspFlowPanel;
        dspFlowPanel = nullptr;
    }
    const bool closeAlreadyFinalized = closeShutdownFinalized;
    if (!closeAlreadyFinalized) {
        refreshSettingsFromUi();
        savePersistentSettings();
    }
    stopPlayback();
    stopRecording(false);
    stopSpectrumFrameRecording();

    if (stopPollTimer) {
        stopPollTimer->stop();
    }
    if (streamWatchdogTimer) {
        streamWatchdogTimer->stop();
    }
    if (listeningScanAdvanceTimer) {
        listeningScanAdvanceTimer->stop();
    }
    if (videoSnapshotTimer) {
        videoSnapshotTimer->stop();
    }
    pendingAudioStartAfterStreamReady = false;
    pendingNetworkAudioStartAfterIqPrebuffer = false;
    pendingPlaybackAudioStartAfterIqPrebuffer = false;
    if (updateTimer) {
        updateTimer->stop();
    }
    if (remoteAudioPlayer) {
        remoteAudioPlayer->stop();
    }
    stopGnssSdrAcquisition(QStringLiteral("destructor"), false);
    stopGnssSerial();
    if (audioHttpServer) {
        audioHttpServer->close();
    }
    for (QTcpSocket *client : std::as_const(audioHttpClients)) {
        if (client) {
            client->disconnectFromHost();
            client->deleteLater();
        }
    }
    audioHttpClients.clear();
    if (networkController) {
        networkController->stop();
    }
    if (audioProcessor && !closeAlreadyFinalized) {
        audioProcessor->stopDemodulation();
        audioProcessor->setLocalPlaybackEnabled(true);
    }
    if (processor && !closeAlreadyFinalized) {
        processor->requestStop();
        if (processor->isRunning() && !processor->wait(1500)) {
            processor->forceStop(1000);
        }
        processor->finalizeStopped();
    } else if (processor) {
        processor->finalizeStopped();
    }

    if (dsdNeoThread) {
        // The receiver worker is stopped above; no more direct PCM submissions.
        disconnect(dsdNeoBridge, nullptr, audioProcessor, nullptr);
        QMetaObject::invokeMethod(dsdNeoBridge, [bridge = dsdNeoBridge]() {
            bridge->setEnabled(false);
            QThread::currentThread()->quit();
        }, Qt::QueuedConnection);
        dsdNeoThread->wait();
        dsdNeoBridge = nullptr;
        dsdNeoThread = nullptr;
    }
    if (digitalDecoderThread) {
        digitalDecoderThread->quit();
        if (!digitalDecoderThread->wait(3000)) {
            qDebug() << "[Digital] decoder thread did not stop in time; terminating";
            digitalDecoderThread->terminate();
            digitalDecoderThread->wait();
        }
        digitalDecoder = nullptr;
        digitalDecoderThread = nullptr;
    }

    if (videoProcessorThread) {
        videoProcessorThread->quit();
        if (!videoProcessorThread->wait(3000)) {
            qDebug() << "[Video] processor thread did not stop in time; terminating";
            videoProcessorThread->terminate();
            videoProcessorThread->wait();
        }
        videoProcessor = nullptr;
        videoProcessorThread = nullptr;
    }

    if (audioProcessor) { 
    delete audioProcessor;
    audioProcessor = nullptr;
    }
    if (processor) {
        delete processor;
        processor = nullptr;
    }

    if (device) {
        if (fobosCloseKnownUnsafe) {
            qDebug() << "[FobosLifecycle] destructor: skipping fobos_rx_close because a previous close failed or hung";
        } else {
            closeFobosDeviceSafely(device);
        }
        device = nullptr;
    }
    if (agileDevice) {
        if (agileScanRunning) {
            stopFobosAgileScanSafely(agileDevice);
            agileScanRunning = false;
            activeAgileScanFrequencies.clear();
        }
        closeFobosAgileDeviceSafely(agileDevice);
        agileDevice = nullptr;
    }
    if (iqData) {
        iqData = nullptr;
    }
}

bool YourClassName::eventFilter(QObject *watched, QEvent *event) {
    if (watched == controlsToggleButton && event->type() == QEvent::MouseButtonDblClick) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent && keyEvent->key() == Qt::Key_F9 && !keyEvent->isAutoRepeat()) {
            if (event->type() == QEvent::KeyPress) {
                startRecording(true);
            } else {
                stopRecording(true);
            }
            return true;
        }
        if (keyEvent && keyEvent->key() == Qt::Key_F8 && event->type() == QEvent::KeyPress && !keyEvent->isAutoRepeat()) {
            if (spectrumFrameRecorder.isRecording()) {
                stopSpectrumFrameRecording();
            } else {
                startSpectrumFrameRecording();
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void YourClassName::closeEvent(QCloseEvent *event) {
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[FobosLifecycle] closeEvent enter"
                 << "state" << runStateName(runState)
                 << "deviceOpened" << deviceOpened
                 << "processorRunning" << (processor && processor->isRunning())
                 << "device" << activeFobosDevice()
                 << "apiKind" << fobosApiKindName(activeFobosApiKind)
                 << "closeShutdownInProgress" << closeShutdownInProgress
                 << "closeShutdownFinalized" << closeShutdownFinalized;
    }

    if (closeShutdownFinalized) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] closeEvent already finalized; accepting window close";
        }
        QMainWindow::closeEvent(event);
        return;
    }

    if (closeShutdownInProgress) {
        event->ignore();
        if (runState == RadioRunState::Stopping &&
            stopPollTimer &&
            !stopPollTimer->isActive()) {
            stopElapsedTimer.restart();
            stopCancelRetryCount = 0;
            stopPollTimer->start();
        }
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] closeEvent already in progress; waiting for shutdown";
        }
        return;
    }

    pendingAudioStartAfterStreamReady = false;
    pendingNetworkAudioStartAfterIqPrebuffer = false;
    pendingPlaybackAudioStartAfterIqPrebuffer = false;
    stopGnssSdrAcquisition(QStringLiteral("close"), false);

    const bool processorRunning = processor && processor->isRunning();
    const bool needsAsyncStop =
        processorRunning ||
        runState == RadioRunState::Running ||
        runState == RadioRunState::Starting ||
        runState == RadioRunState::Stopping ||
        deviceOpened;

    if (needsAsyncStop) {
        closeShutdownInProgress = true;
        event->ignore();
        if (runState != RadioRunState::Stopping) {
            stopFobosProcessing();
            if (isNetworkClientMode() &&
                !(processor && processor->isRunning()) &&
                !deviceOpened) {
                closeShutdownFinalized = false;
                QTimer::singleShot(0, this, [this]() {
                    if (closeShutdownInProgress &&
                        !closeShutdownFinalized &&
                        !(processor && processor->isRunning()) &&
                        !deviceOpened) {
                        closeShutdownInProgress = false;
                        close();
                    }
                });
            }
        } else if (stopPollTimer && !stopPollTimer->isActive()) {
            stopElapsedTimer.restart();
            stopCancelRetryCount = 0;
            stopPollTimer->start();
        }
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] closeEvent deferred until reader stops";
        }
        return;
    }

    closeShutdownInProgress = true;
    if (streamWatchdogTimer) {
        streamWatchdogTimer->stop();
    }
    if (stopPollTimer) {
        stopPollTimer->stop();
    }
    if (updateTimer) {
        updateTimer->stop();
    }
    if (audioProcessor) {
        audioProcessor->stopDemodulation();
    }
    if (processor) {
        processor->finalizeStopped();
    }
    if (hasActiveFobosDevice()) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] closeEvent: closing idle Fobos session";
        }
        closeFobosSession(false);
    }
    deviceOpened = false;
    runState = RadioRunState::Idle;
    savePersistentSettings();
    closeShutdownFinalized = true;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[FobosLifecycle] closeEvent exit";
    }
    QMainWindow::closeEvent(event);
}

ReceiverCalibrationContext YourClassName::receiverCalibrationContext(const RadioSettings &settings) const {
    ReceiverCalibrationContext context;
    context.backendId = isRtlSdrNativeSelected() ? QStringLiteral("rtl-native") :
        isRtlTcpSelected() ? QStringLiteral("rtl-tcp") :
        isHackRfNativeSelected() ? QStringLiteral("hackrf-native") :
        isSoapySdrSelected() ? QStringLiteral("soapy") :
        selectedFobosDeviceInfo().apiKind == FobosApiKind::Agile ? QStringLiteral("fobos-agile") : QStringLiteral("fobos-standard");
    context.inputMode = settings.inputMode;
    context.sampleRateHz = settings.sampleRate;
    if (context.backendId.startsWith(QStringLiteral("rtl"))) {
        context.rtlGainTenthsDb = settings.rtlTunerGainTenthsDb;
        context.gainState = settings.rtlAgc ? 1 : 0;
    } else if (context.backendId == QStringLiteral("hackrf-native")) {
        context.lnaGain = settings.hackRfLnaGainDb;
        context.vgaGain = settings.hackRfVgaGainDb;
        context.gainState = settings.hackRfAmpEnabled ? 1 : 0;
    } else { context.lnaGain = settings.lnaGain; context.vgaGain = settings.vgaGain; }
    return context;
}

double YourClassName::effectiveFrequencyCalibrationOffsetHz(double logicalFrequencyHz) const {
    double offsetHz = std::isfinite(frequencyCalibrationOffsetHz)
                          ? frequencyCalibrationOffsetHz
                          : 0.0;
    if (calibrationTableEnabled) {
        const ReceiverCalibrationCorrection correction =
            receiverCalibrationTable.correctionAt(logicalFrequencyHz, receiverCalibrationContext(pendingSettings));
        if (correction.valid && std::isfinite(correction.frequencyOffsetHz)) {
            offsetHz += correction.frequencyOffsetHz;
        }
    }
    return offsetHz;
}

double YourClassName::effectiveAmplitudeCalibrationOffsetDb(double frequencyHz) const {
    double offsetDb = std::isfinite(amplitudeCalibrationOffsetDb)
                          ? amplitudeCalibrationOffsetDb
                          : 0.0;
    if (calibrationTableEnabled) {
        const ReceiverCalibrationCorrection correction =
            receiverCalibrationTable.correctionAt(frequencyHz, receiverCalibrationContext(pendingSettings));
        if (correction.valid && std::isfinite(correction.amplitudeOffsetDb)) {
            offsetDb += correction.amplitudeOffsetDb;
        }
    }
    return offsetDb;
}

double YourClassName::calibratedHardwareFrequency(double logicalFrequencyHz) const {
    if (!std::isfinite(logicalFrequencyHz)) {
        return logicalFrequencyHz;
    }
    return logicalFrequencyHz + effectiveFrequencyCalibrationOffsetHz(logicalFrequencyHz);
}

int YourClassName::setCalibratedActiveFrequencySafely(double logicalFrequencyHz,
                                                       double *logicalActualFrequencyHz) {
    const double offsetHz = effectiveFrequencyCalibrationOffsetHz(logicalFrequencyHz);
    double hardwareActualFrequencyHz = logicalFrequencyHz + offsetHz;
    const int result = setActiveFrequencySafely(hardwareActualFrequencyHz,
                                                &hardwareActualFrequencyHz);
    if (logicalActualFrequencyHz) {
        *logicalActualFrequencyHz = result == FOBOS_ERR_OK
                                        ? hardwareActualFrequencyHz - offsetHz
                                        : logicalFrequencyHz;
    }
    return result;
}

void YourClassName::applyReceiverCalibrationLive() {
    if (processor) {
        processor->setFrequencyCalibrationOffset(
            effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency));
    }
    if (pendingSettings.inputMode != INPUT_RF || pendingSettings.centerFrequency <= 0.0) {
        return;
    }

    bool applied = false;
    double logicalActualFrequencyHz = pendingSettings.centerFrequency;
    if (isExternalReceiverBackendSelected()) {
        applied = processor && processor->isRunning() &&
                  processor->retuneCenterFrequency(pendingSettings.centerFrequency);
    } else if (hasActiveFobosDevice()) {
        if (agileScanRunning) {
            applied = applyAgileScanSettings(false);
        } else {
            applied = setCalibratedActiveFrequencySafely(pendingSettings.centerFrequency,
                                                         &logicalActualFrequencyHz) == FOBOS_ERR_OK;
        }
    }

    if (applied) {
        const uint64_t epoch = processor ? processor->beginIqRetuneBarrier() : 0;
        clearLiveSpectrumSnapshot(false, epoch);
        pendingSettings.actualFrequency = logicalActualFrequencyHz;
        actualFrequency = logicalActualFrequencyHz;
        if (hardwareSettingsApplied) {
            appliedHardwareSettings.actualFrequency = logicalActualFrequencyHz;
        }
        publishSettingsToGlobals();
        liveRetuneSettleDurationMs = LIVE_RETUNE_SETTLE_MS;
        liveRetuneSettleTimer.start();
    }
    qDebug() << "[Calibration] receiver offsets applied"
             << "baseFrequencyHz" << frequencyCalibrationOffsetHz
             << "effectiveFrequencyHz" << effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency)
             << "baseAmplitudeDb" << amplitudeCalibrationOffsetDb
             << "effectiveAmplitudeDb" << effectiveAmplitudeCalibrationOffsetDb(pendingSettings.centerFrequency)
             << "tableEnabled" << calibrationTableEnabled
             << "tablePoints" << receiverCalibrationTable.points().size()
             << "logicalCenter" << pendingSettings.centerFrequency
             << "hardwareCenter" << calibratedHardwareFrequency(pendingSettings.centerFrequency)
             << "liveRetuned" << applied;
}

static void logIqBufferRetuneState(const char *stage,
                                   const QString &reason,
                                   std::uint64_t retuneEpoch,
                                   double previousFrequency,
                                   double requestedFrequency,
                                   double actualFrequency);

bool YourClassName::restartStreamForHardwareChange() {
    QElapsedTimer restartTimer;
    restartTimer.start();
    qint64 audioStopMs = 0;
    qint64 readerStopMs = 0;
    qint64 settingsApplyMs = 0;
    if (isChannelIqRecordingActive() &&
        networkMode != NetworkMode::Disabled &&
        isFullIqProcessingMode()) {
        stopRecording(false);
        updateRecordingStatus(QStringLiteral("Recording stopped: Channel IQ cannot run during Full IQ streaming"));
    }

    const bool externalBackendSelected = isExternalReceiverBackendSelected();
    if (externalBackendSelected && processor && processor->isRunning()) {
        qDebug() << "[LiveHardware] restarting external receiver stream for hardware change"
                 << "state" << runStateName(runState)
                 << "deviceOpened" << deviceOpened
                 << "sampleRate" << pendingSettings.sampleRate;

        runState = RadioRunState::Stopping;
        updateUiForRunState();
        if (streamWatchdogTimer) streamWatchdogTimer->stop();
        if (stopPollTimer) stopPollTimer->stop();
        if (updateTimer) updateTimer->stop();
        pendingAudioStartAfterStreamReady = false;
        if (audioProcessor) audioProcessor->stopDemodulationForStreamRestart();
        audioStopMs = restartTimer.elapsed();
        digitalDecoderGeneration.fetch_add(1, std::memory_order_relaxed);
        pendingDmrDecoderPcm.clear();
        pendingDmrDecoderSampleRate = 48000;
        droppedDigitalDecoderFramesSinceLog.store(0);
        if (digitalDecoder) {
            QMetaObject::invokeMethod(digitalDecoder,
                                      [decoder = digitalDecoder]() {
                                          decoder->reset();
                                      },
                                      Qt::QueuedConnection);
        }

        processor->requestStop();
        if (!processor->wait(3000)) {
            const bool forced = processor->forceStop(1000);
            qDebug() << "[LiveHardware] forced external DataProcessor stop during live restart"
                     << forced
                     << "processorRunning" << processor->isRunning();
            if (!forced || processor->isRunning()) {
                qDebug() << "[LiveHardware] external DataProcessor is still running; restart aborted";
                stopCancelRetryCount = 0;
                stopElapsedTimer.restart();
                if (stopPollTimer) stopPollTimer->start();
                updateUiForRunState();
                return false;
            }
        }
        processor->finalizeStopped();
        readerStopMs = restartTimer.elapsed() - audioStopMs;
        deviceOpened = false;
        runState = RadioRunState::Idle;
        updateUiForRunState();
        QTimer::singleShot(0, this, &YourClassName::startFobosProcessing);
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[LiveHardware timing] external restart prepared"
                     << "audioStopMs" << audioStopMs << "readerStopMs" << readerStopMs
                     << "totalMs" << restartTimer.elapsed();
        }
        return true;
    }

    if (isIdle()) {
        if (!hasActiveFobosDevice()) {
            qDebug() << "[LiveHardware] settings changed while idle; preparing Fobos session";
            return prepareFobosSessionFromSettings(QStringLiteral("idle hardware change"));
        }
        return applyFobosSettings();
    }

    if (!hasActiveFobosDevice()) {
        qDebug() << "[LiveHardware] cannot restart stream because Fobos session is missing"
                 << "externalBackend" << externalBackendSelected
                 << "processorRunning" << (processor && processor->isRunning());
        if (!(processor && processor->isRunning())) {
            deviceOpened = false;
            runState = RadioRunState::Idle;
        }
        updateUiForRunState();
        return false;
    }

    qDebug() << "[LiveHardware] restarting stream for hardware change";

    runState = RadioRunState::Stopping;
    updateUiForRunState();
    if (fpvAutoScanController.active()) {
        stopFpvAutoScan(true);
    }
    if (streamWatchdogTimer) streamWatchdogTimer->stop();
    if (stopPollTimer) stopPollTimer->stop();
    if (updateTimer) updateTimer->stop();
    pendingAudioStartAfterStreamReady = false;
    if (audioProcessor) audioProcessor->stopDemodulationForStreamRestart();
    audioStopMs = restartTimer.elapsed();
    digitalDecoderGeneration.fetch_add(1, std::memory_order_relaxed);
    pendingDmrDecoderPcm.clear();
    pendingDmrDecoderSampleRate = 48000;
    droppedDigitalDecoderFramesSinceLog.store(0);
    if (digitalDecoder) {
        QMetaObject::invokeMethod(digitalDecoder,
                                  [decoder = digitalDecoder]() {
                                      decoder->reset();
                                  },
                                  Qt::QueuedConnection);
    }

    if (processor) {
        processor->requestStop();
        if (processor->isRunning() && !processor->wait(1500)) {
            const bool forced = processor->forceStop(1000);
            qDebug() << "[LiveHardware] forced DataProcessor stop during live restart" << forced;
            if (!forced || processor->isRunning()) {
                qDebug() << "[LiveHardware] DataProcessor is still running; live restart is deferred to stop recovery";
                stopCancelRetryCount = 0;
                stopElapsedTimer.restart();
                if (stopPollTimer) stopPollTimer->start();
                updateUiForRunState();
                return false;
            }
        }
        processor->finalizeStopped();
    }

    clearLiveSpectrumSnapshot(false);

    readerStopMs = restartTimer.elapsed() - audioStopMs;

    runState = RadioRunState::Starting;
    updateUiForRunState();
    if (!applyFobosSettings()) {
        qDebug() << "[LiveHardware] applyFobosSettings failed";
        closeFobosSession(true);
        clearLiveSpectrumSnapshot();
        deviceOpened = false;
        runState = RadioRunState::Idle;
        updateUiForRunState();
        return false;
    }

    updateSpectrumTimerInterval();
    settingsApplyMs = restartTimer.elapsed() - audioStopMs - readerStopMs;
    settingRange();
    spectrumTuningDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 8 : 0;

    const bool serverIqStreaming = networkMode == NetworkMode::Server && isClientIqProcessingMode();
    const bool serverFullIqStreaming = networkMode == NetworkMode::Server && isFullIqProcessingMode();
    const bool serverChannelIqStreaming = networkMode == NetworkMode::Server && isChannelIqProcessingMode();
    const bool channelIqRecording = isChannelIqRecordingActive();
    const bool eventIq = spectrumEventIqProducerNeeded();
    const bool suppressServerLocalOutput =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();
    const bool serverAudioStreamingForFullIq = serverFullIqStreaming && pendingSettings.audioEnabled;
    const bool serverLocalAudioEnabled =
        pendingSettings.audioEnabled &&
        (!serverIqStreaming || serverAudioStreamingForFullIq);
    const bool queueAudioBlocks =
        (!serverIqStreaming || serverAudioStreamingForFullIq);
    const bool publishIqSnapshot = true;
    if (audioProcessor) {
        audioProcessor->setLocalPlaybackEnabled(!suppressServerLocalOutput);
    }

    const bool emitProcessorIqFrames = serverIqStreaming ||
        (!dmrAudioIqProducerNeeded() && (channelIqRecording || eventIq));
    updateIqFrameProducerSettings();

    processor->startProcessing(makeFobosStreamDescriptor(activeFobosDevice(),
                                                         activeFobosApiKind,
                                                         pendingSettings.syncEnabled,
                                                         pendingSettings.sampleRate,
                                                         pendingSettings.centerFrequency,
                                                         queueAudioBlocks,
                                                         publishIqSnapshot,
                                                         emitProcessorIqFrames,
                                                         agileScanEnabled &&
                                                             !standardScanEnabled &&
                                                             activeFobosApiKind == FobosApiKind::Agile,
                                                         agileScanEnabled &&
                                                                 !standardScanEnabled &&
                                                                 activeFobosApiKind == FobosApiKind::Agile
                                                             ? activeAgileScanFrequencies
                                                             : QVector<double>(),
                                                         effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency)));

    if (activeFobosApiKind == FobosApiKind::Agile &&
        pendingSettings.inputMode == INPUT_RF &&
        !agileScanEnabled) {
        liveRetuneSettleDurationMs = agileRfLiveSettleMs(pendingSettings.sampleRate, false);
        clearLiveSpectrumSnapshot(false);
        liveRetuneSettleTimer.start();
        qDebug() << "[LiveHardware] Agile reader restart settle armed"
                 << "settleMs" << liveRetuneSettleDurationMs;
    }

    deviceOpened = true;

    if (updateTimer && !(serverFullIqStreaming && suppressServerLocalOutput)) {
        updateTimer->start();
    }

    pendingAudioStartAfterStreamReady = serverLocalAudioEnabled;
    streamStartCallbackCount = processor ? processor->callbackCount() : 0;
    streamStartElapsedTimer.restart();
    if (streamWatchdogTimer) streamWatchdogTimer->start();

    runState = RadioRunState::Running;
    updateUiForRunState();
    applyServerLocalOutputPolicy();
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[LiveHardware timing] restart done"
                 << "audioStopMs" << audioStopMs << "readerStopMs" << readerStopMs
                 << "settingsApplyMs" << settingsApplyMs
                 << "startMs" << (restartTimer.elapsed() - audioStopMs - readerStopMs - settingsApplyMs)
                 << "totalMs" << restartTimer.elapsed();
    }
    return true;
}

uint8_t YourClassName::currentGpoValue() const {
    uint8_t value = 0;
    for (int i = 0; i < 8; ++i) {
        if (checkBoxes[i] && checkBoxes[i]->isChecked()) {
            value |= (1 << i);
        }
    }
    return value;
}

void YourClassName::setControlsPanelVisible(bool visible) {
    if (!controlsDock) {
        return;
    }

    if (!visible) {
        controlsDock->hide();
        return;
    }

    if (controlsDock->isFloating()) {
        controlsDock->hide();
        controlsDock->setFloating(false);
        addDockWidget(Qt::LeftDockWidgetArea, controlsDock);
    }
    controlsDock->show();
}

bool YourClassName::isIdle() const {
    return runState == RadioRunState::Idle;
}

bool YourClassName::isRunningOrTransitioning() const {
    return runState != RadioRunState::Idle;
}

void YourClassName::refreshSettingsFromUi() {
    if (comboBox) {
        bool ok = false;
        const int selectedIndex = comboBox->currentData().toInt(&ok);
        pendingSettings.deviceIndex =
            ok ? receiverDeviceIndexFromComboValue(selectedIndex) : std::max(0, comboBox->currentIndex());
    }
    if (clkBox) {
        pendingSettings.clockSource = clkBox->currentData().toInt();
    }
    if (modeBox) {
        pendingSettings.inputMode = modeBox->currentData().toInt();
    }
    if (sampleBox) {
        bool ok = false;
        const double sampleRate = sampleBox->currentData().toDouble(&ok);
        if (ok && sampleRate > 0.0) {
            pendingSettings.sampleRate = sampleRate;
        }
    }
    if (frequencyControl) {
        frequencyControl->commitPendingValue();
        double frequency = frequencyControl->valueHz();
        frequency = (std::clamp)(frequency,
                                 rfMinimumCenterFrequency(pendingSettings),
                                 rfMaximumCenterFrequency(pendingSettings));
        pendingSettings.centerFrequency = pendingSettings.inputMode == INPUT_RF ? frequency : 0.0;
    }
    if (listeningFrequencyControl) {
        listeningFrequencyControl->commitPendingValue();
        pendingSettings.listeningFrequency = listeningFrequencyControl->valueHz();
    }
    if (bandwidthControl) {
        bandwidthControl->commitPendingValue();
        const double bandwidth = bandwidthControl->valueHz();
        if (bandwidth > 0.0) {
            pendingSettings.bandwidth = bandwidth;
        }
    }
    if (fftComboBox) {
        fftBinWidthModeEnabled = fftBinWidthModeCheckbox && fftBinWidthModeCheckbox->isChecked();
        if (fftBinWidthSpin) {
            fftTargetBinWidthHz = fftBinWidthSpin->value();
        }
        if (fftBinWidthModeEnabled) {
            pendingSettings.fftLength = fftLengthForBinWidth(pendingSettings.sampleRate,
                                                              fftTargetBinWidthHz);
        } else {
            const int selectedFftLength = fftComboBox->currentText().toInt();
            if (selectedFftLength > 0) {
                pendingSettings.fftLength = selectedFftLength;
            }
        }
    }
    if (lnaGainSlider) {
        if (isHackRfNativeSelected()) {
            pendingSettings.hackRfLnaGainDb =
                (std::clamp)(lnaGainSlider->value(), 0, 40) / 8 * 8;
        } else {
            pendingSettings.lnaGain = lnaGainSlider->value();
        }
    }
    if (vgaGainSlider) {
        if (isHackRfNativeSelected()) {
            pendingSettings.hackRfVgaGainDb =
                (std::clamp)(vgaGainSlider->value(), 0, 62) / 2 * 2;
        } else {
            pendingSettings.vgaGain = vgaGainSlider->value();
        }
    }
    if (rtlAgcCheckbox) {
        pendingSettings.rtlAgc = rtlAgcCheckbox->isChecked();
    }
    if (rtlGainSlider) {
        pendingSettings.rtlTunerGainTenthsDb =
            (std::clamp)(rtlGainSlider->value(), 0, 496);
    }
    if (audioDeviceComboBox) {
        pendingSettings.audioDeviceId = audioDeviceComboBox->currentData().toInt();
    }
    if (audioLowPassSlider) {
        pendingSettings.audioLowPassHz = audioLowPassSliderValueToHz(audioLowPassSlider->value());
    }
    if (audioHighPassSlider) {
        pendingSettings.audioHighPassHz = audioHighPassSliderValueToHz(audioHighPassSlider->value());
    }
    if (audioFilterChainWidget) {
        pendingSettings.audioFilterChainJson = audioFilterChainWidget->configurationJson();
    }
    if (hfNoiseCancelDepthSlider) {
        pendingSettings.hfNoiseCancelDepth =
            hfNoiseCancelSliderValueToDepth(hfNoiseCancelDepthSlider->value());
    }
    if (hfNoiseCancelRefGainSlider) {
        pendingSettings.hfNoiseCancelRefGainDb =
            hfNoiseCancelSliderValueToRefGainDb(hfNoiseCancelRefGainSlider->value());
    }
    if (hfNoiseCancelRefDelaySlider) {
        pendingSettings.hfNoiseCancelRefDelayNs =
            hfNoiseCancelSliderValueToRefDelayNs(hfNoiseCancelRefDelaySlider->value());
    }
    if (hfNoiseCancelRefTiltSlider) {
        pendingSettings.hfNoiseCancelRefTiltDb =
            hfNoiseCancelSliderValueToRefTiltDb(hfNoiseCancelRefTiltSlider->value());
    }
    if (hfNoiseCancelFreezeCheckbox) {
        pendingSettings.hfNoiseCancelFreeze = hfNoiseCancelFreezeCheckbox->isChecked();
    }
    if (hfAudioBlankerCheckbox) {
        pendingSettings.hfAudioBlankerEnabled = hfAudioBlankerCheckbox->isChecked();
    }
    if (hfAudioBlankerThresholdSlider) {
        pendingSettings.hfAudioBlankerThreshold =
            (std::clamp)(hfAudioBlankerThresholdSlider->value() / 10.0, 2.0, 20.0);
    }
    if (audioCheckbox) {
        pendingSettings.audioEnabled = audioCheckbox->isChecked();
    }
    if (syncCheckbox) {
        if (syncCheckbox->isChecked()) {
            syncCheckbox->blockSignals(true);
            syncCheckbox->setChecked(false);
            syncCheckbox->blockSignals(false);
        }
        pendingSettings.syncEnabled = false;
    }
    if (dmrBasebandRateCombo) {
        pendingSettings.dmrBasebandSampleRate =
            normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt());
    } else {
        pendingSettings.dmrBasebandSampleRate =
            normalizedDmrBasebandSampleRate(pendingSettings.dmrBasebandSampleRate);
    }
    if (dmrAmbeLayoutCombo) {
        pendingSettings.dmrAmbeLayout =
            normalizedDmrAmbeLayout(dmrAmbeLayoutCombo->currentData().toInt());
    } else {
        pendingSettings.dmrAmbeLayout =
            normalizedDmrAmbeLayout(pendingSettings.dmrAmbeLayout);
    }
    if (dmrPrivacyModeCombo) {
        pendingSettings.dmrPrivacyMode =
            normalizedDmrPrivacyMode(dmrPrivacyModeCombo->currentData().toInt());
    } else {
        pendingSettings.dmrPrivacyMode =
            normalizedDmrPrivacyMode(pendingSettings.dmrPrivacyMode);
    }
    applyDmrPrivacyKeySelection();
    if (dmrPrivacyFrameOffsetCombo) {
        pendingSettings.dmrPrivacyFrameOffset =
            (std::clamp)(dmrPrivacyFrameOffsetCombo->currentData().toInt(), 0, 17);
    }
    if (dmrPrivacyDropCombo) {
        pendingSettings.dmrPrivacyVariant = dmrPrivacyDropCombo->currentData().toString();
    }
    if (dmrPrivacyBitLayoutCombo) {
        pendingSettings.dmrPrivacyLayout = dmrPrivacyBitLayoutCombo->currentData().toString();
    }
    if (dmrManualTimingCheckbox) {
        pendingSettings.dmrManualTimingEnabled = dmrManualTimingCheckbox->isChecked();
    }
    if (dmrTimingOffsetSpin) {
        pendingSettings.dmrManualTimingOffset = dmrTimingOffsetSpin->value();
    }
    if (dmrSlicerRatioSpin) {
        pendingSettings.dmrSlicerRatio = dmrSlicerRatioSpin->value();
    }
    if (dmrAdaptiveSlicerCheckbox) {
        pendingSettings.dmrAdaptiveSlicer = dmrAdaptiveSlicerCheckbox->isChecked();
    }
    if (agileScanCheckbox) {
        agileScanEnabled = agileScanCheckbox->isChecked();
    }
    if (agileScanAutoStepCheckbox) {
        agileScanAutoStepSampleRate = agileScanAutoStepCheckbox->isChecked();
    }
    if (agileScanRangesEdit) {
        agileScanRangesMhz = agileScanRangesEdit->text().trimmed();
    }
    if (agileScanStepSpin) {
        agileScanStepMhz = (std::clamp)(agileScanStepSpin->value(),
                                        AGILE_SCAN_MIN_STEP_MHZ,
                                        AGILE_SCAN_MAX_STEP_MHZ);
    }
    applyAgileScanAutoStep(false);
    if (scanVisualModeCombo) {
        scanVisualMode = normalizedScanVisualMode(scanVisualModeCombo->currentData().toInt());
    }
    if (standardScanCheckbox) {
        standardScanEnabled = standardScanCheckbox->isChecked();
    }
    if (scanListeningLockCheckbox) {
        scanListeningLockEnabled = scanListeningLockCheckbox->isChecked();
    }
    if (standardScanCentersEdit) {
        standardScanCentersMhz = standardScanCentersEdit->text().trimmed();
    }
    if (standardScanDwellSpin) {
        standardScanDwellMs = (std::clamp)(standardScanDwellSpin->value(),
                                           STANDARD_SCAN_MIN_DWELL_MS,
                                           STANDARD_SCAN_MAX_DWELL_MS);
    }
    if (standardScanSettleSpin) {
        standardScanSettleMs = (std::clamp)(standardScanSettleSpin->value(),
                                            STANDARD_SCAN_MIN_SETTLE_MS,
                                            STANDARD_SCAN_MAX_SETTLE_MS);
    }
    if (standardScanRangeStartEdit) {
        standardScanRangeStartMhz = standardScanRangeStartEdit->text().trimmed();
    }
    if (standardScanRangeEndEdit) {
        standardScanRangeEndMhz = standardScanRangeEndEdit->text().trimmed();
    }
    if (listeningScanCheckbox) {
        listeningScanEnabled = listeningScanCheckbox->isChecked();
    }
    if (listeningScanTargetsEdit) {
        listeningScanTargetsMhz = listeningScanTargetsEdit->text().trimmed();
    }
    if (listeningScanDwellSpin) {
        listeningScanDwellMs = (std::clamp)(listeningScanDwellSpin->value(),
                                            LISTENING_SCAN_MIN_DWELL_MS,
                                            LISTENING_SCAN_MAX_DWELL_MS);
    }
    if (listeningScanSettleSpin) {
        listeningScanSettleMs = (std::clamp)(listeningScanSettleSpin->value(),
                                             LISTENING_SCAN_MIN_SETTLE_MS,
                                             LISTENING_SCAN_MAX_SETTLE_MS);
    }
    if (gnssIntegrationSpin) {
        gnssAcquisitionIntegrationMs =
            (std::clamp)(gnssIntegrationSpin->value(),
                         GNSS_ACQUISITION_MIN_INTEGRATION_MS,
                         GNSS_ACQUISITION_MAX_INTEGRATION_MS);
    }
    if (gnssChannelFilterSpin) {
        const double cutoffHz = gnssChannelFilterSpin->value() * 1000000.0;
        gnssChannelFilterCutoffHz =
            (std::clamp)(std::isfinite(cutoffHz) ? cutoffHz : 1800000.0,
                         GNSS_CHANNEL_FILTER_MIN_HZ,
                         GNSS_CHANNEL_FILTER_MAX_HZ);
    }
    if (gnssDopplerSpanSpin) {
        gnssDopplerSpanHz = (std::clamp)(gnssDopplerSpanSpin->value(), 1, 50) * 1000;
    }
    if (gnssDopplerStepSpin) {
        gnssDopplerStepHz = (std::clamp)(gnssDopplerStepSpin->value(), 250, 5000);
    }
    if (qthLatitudeSpin) {
        qthLatitude = qthLatitudeSpin->value();
    }
    if (qthLongitudeSpin) {
        qthLongitude = qthLongitudeSpin->value();
    }
    if (qthSourceCombo) {
        qthSource = qthSourceCombo->currentData().toString();
        if (qthSource.isEmpty()) {
            qthSource = QStringLiteral("manual");
        }
    }
    if (gnssSerialPortEdit) {
        gnssSerialPortName = gnssSerialPortEdit->currentText().trimmed();
    }
    if (gnssSerialBaudSpin) {
        gnssSerialBaud = (std::clamp)(gnssSerialBaudSpin->value(), 1200, 921600);
    }
    if (gnssPositionPolicyCombo) {
        gnssPositionPolicy =
            normalizedGnssPositionPolicy(gnssPositionPolicyCombo->currentData().toString());
    }
    if (gnssTimeZoneCombo) {
        gnssTimeZoneOffsetMinutes = gnssTimeZoneCombo->currentData().toInt();
    }
    if (gnssSystemCombo) {
        gnssSystemId = gnssSystemCombo->currentData().toString().trimmed();
        if (gnssSystemId.isEmpty()) {
            gnssSystemId = QStringLiteral("gps_l1_ca");
        }
        gnssSystemId = gnssSystemPreset(gnssSystemId).id;
    }
    if (gnssMonitorCheckbox) {
        gnssMonitorEnabled = gnssMonitorCheckbox->isChecked();
    }
    if (gnssDeepAcquireButton) {
        const bool uiContinuousEnabled = gnssDeepAcquireButton->isChecked();
        if (uiContinuousEnabled != gnssContinuousAcquisitionEnabled) {
            setGnssContinuousAcquisitionEnabled(uiContinuousEnabled);
        } else if (uiContinuousEnabled && gnssContinuousAcquireTimer &&
                   !gnssContinuousAcquireTimer->isActive() &&
                   !gnssAcquisitionRunning) {
            scheduleGnssContinuousAcquisition(GNSS_CONTINUOUS_ACQUISITION_FIRST_DELAY_MS);
            qDebug() << "[GNSS acquisition] continuous re-armed from UI refresh"
                     << "intervalMs" << gnssContinuousAcquisitionIntervalMs;
        }
    }
    if (gnssUseGpsCheckbox) {
        gnssUseGps = gnssUseGpsCheckbox->isChecked();
    }
    if (gnssUseGlonassCheckbox) {
        gnssUseGlonass = gnssUseGlonassCheckbox->isChecked();
    }
    if (gnssUseGalileoCheckbox) {
        gnssUseGalileo = gnssUseGalileoCheckbox->isChecked();
    }
    if (gnssUseBeidouCheckbox) {
        gnssUseBeidou = gnssUseBeidouCheckbox->isChecked();
    }
    if (gnssUseQzssCheckbox) {
        gnssUseQzss = gnssUseQzssCheckbox->isChecked();
    }
    if (gnssUseSbasCheckbox) {
        gnssUseSbas = gnssUseSbasCheckbox->isChecked();
    }
    if (gnssUseOtherCheckbox) {
        gnssUseOther = gnssUseOtherCheckbox->isChecked();
    }
    if (qthMapLayerCombo) {
        qthMapLayer = qthMapLayerCombo->currentData().toInt();
    }
    if (qthOnlineProviderCombo) {
        qthOnlineProviderId = qthOnlineProviderCombo->currentData().toString();
        if (qthOnlineProviderId.isEmpty()) {
            qthOnlineProviderId = QStringLiteral("custom");
        }
    }
    if (qthMapZoomSpin) {
        qthMapZoom = qthMapZoomSpin->value();
    }
    if (qthGridPrecisionCombo) {
        qthGridPrecision = qthGridPrecisionCombo->currentData().toInt();
    }
    if (qthMapOverlayCombo) {
        qthMapOverlayMode = (std::clamp)(qthMapOverlayCombo->currentData().toInt(), 0, 3);
    }
    if (qthTileDirectoryEdit) {
        qthTileDirectory = QDir::fromNativeSeparators(qthTileDirectoryEdit->text().trimmed());
    }
    if (qthOnlineTileUrlEdit) {
        qthOnlineTileUrlTemplate = qthOnlineTileUrlEdit->text().trimmed();
    }
    if (qthOnlineAttributionEdit) {
        qthOnlineAttribution = qthOnlineAttributionEdit->text().trimmed();
    }
    if (qthOnlineApiKeyEdit) {
        qthOnlineApiKey = qthOnlineApiKeyEdit->text().trimmed();
    }
    if (qthOnlineNoDiskCacheCheckbox) {
        qthOnlineNoDiskCache = qthOnlineNoDiskCacheCheckbox->isChecked();
    }
    if (agileScanEnabled && standardScanEnabled) {
        agileScanEnabled = false;
        if (agileScanCheckbox) {
            QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(false);
        }
    }
    normalizeStandardScanCentersUi(false);
    if (scanMeasurementCheckbox) {
        scanMeasurementEnabled = scanMeasurementCheckbox->isChecked();
    }
    if (scanMeasurementBinSpin) {
        scanMeasurementBinMhz = (std::clamp)(scanMeasurementBinSpin->value(),
                                             SCAN_MEASUREMENT_MIN_BIN_MHZ,
                                             SCAN_MEASUREMENT_MAX_BIN_MHZ);
    }
    if (spurSuppressionCheckbox) {
        spurSuppressionEnabled = spurSuppressionCheckbox->isChecked();
    }
    pendingSettings.gpoValue = currentGpoValue();
    normalizeTuning(pendingSettings);
}

void YourClassName::publishSettingsToGlobals() {
    globalMode = pendingSettings.inputMode;
    globalFrequency = pendingSettings.centerFrequency;
    actualFrequency = pendingSettings.actualFrequency;
    listeningFrequency = pendingSettings.listeningFrequency;
    globalSampleRate = pendingSettings.sampleRate;
    globalBandwidth = pendingSettings.bandwidth;
    globalModulationType = pendingSettings.modulationType;
    fftLength = pendingSettings.fftLength;
    syncWariable = pendingSettings.syncEnabled;
    deviceID = pendingSettings.audioDeviceId;
    if (audioProcessor) {
        audioProcessor->configure(audioProcessorSettings());
    }
    const bool iqFrameProducerActive =
        isChannelIqRecordingActive() ||
        spectrumEventIqProducerNeeded() ||
        (networkMode == NetworkMode::Server && isClientIqProcessingMode());
    if (runState == RadioRunState::Running && iqFrameProducerActive) {
        updateIqFrameProducerSettings();
    }
}

void YourClassName::handleDataProcessorFailure(int errorCode, bool stoppedByRequest) {
    const bool unexpectedCleanEnd = errorCode == FOBOS_ERR_OK;
    qDebug() << "[FobosLifecycle] DataProcessor reader failure"
             << "error" << errorCode
             << "unexpectedCleanEnd" << unexpectedCleanEnd
             << "stoppedByRequest" << stoppedByRequest
             << "state" << runStateName(runState)
             << "deviceOpened" << deviceOpened
             << "processorRunning" << (processor && processor->isRunning())
             << "device" << activeFobosDevice()
             << "apiKind" << fobosApiKindName(activeFobosApiKind)
             << "sampleRate" << pendingSettings.sampleRate
             << "inputMode" << pendingSettings.inputMode;

    if (stoppedByRequest || isNetworkClientMode()) {
        return;
    }

    restartAfterStartupWatchdog = false;
    automaticStreamRestart = false;
    pendingAudioStartAfterStreamReady = false;
    const bool externalBackendFailure = isExternalReceiverBackendSelected();
    closeFobosSessionAfterStop = !externalBackendFailure;
    if (streamWatchdogTimer) {
        streamWatchdogTimer->stop();
    }
    clearSpectrumAfterStop = true;
    const bool startupFailure =
        streamStartElapsedTimer.isValid() &&
        streamStartElapsedTimer.elapsed() < 2500 &&
        streamStartupRetryCount < 1;
    if (startupFailure) {
        ++streamStartupRetryCount;
        restartAfterStartupWatchdog = true;
        qDebug() << "[FobosLifecycle] reader startup failure will retry once"
                 << "retryCount" << streamStartupRetryCount
                 << "error" << errorCode
                 << "externalBackend" << externalBackendFailure;
    } else if (externalBackendFailure) {
        qDebug() << "[FobosLifecycle] external receiver startup failure retry already used"
                 << "error" << errorCode;
    }

    if (runState == RadioRunState::Stopping) {
        return;
    }

    if (runState == RadioRunState::Idle && !deviceOpened && !hasActiveFobosDevice()) {
        return;
    }

    qDebug() << "[FobosLifecycle] recovering from reader failure; closing Fobos session";
    if (recordingManager && recordingManager->isRecording()) {
        qDebug() << "[Recording] stopping because receiver stream ended unexpectedly";
        stopRecording(false);
        updateRecordingStatus(unexpectedCleanEnd
                                  ? QStringLiteral("Recording stopped: receiver stream ended")
                                  : QStringLiteral("Recording stopped: receiver error %1").arg(errorCode));
    }
    if (digitalStatusLabel) {
        onDigitalDecoderStatusChanged(unexpectedCleanEnd
                                          ? QStringLiteral("Receiver stream ended unexpectedly; check USB connection")
                                          : QStringLiteral("Receiver stream failed: error %1").arg(errorCode));
    }
    if (updateTimer) {
        updateTimer->stop();
    }
    if (audioProcessor) {
        audioProcessor->stopDemodulation();
    }

    runState = RadioRunState::Stopping;
    updateUiForRunState();

    if (processor && processor->isRunning()) {
        stopCancelRetryCount = 0;
        processor->requestStop();
        stopElapsedTimer.restart();
        if (stopPollTimer) {
            stopPollTimer->start();
        }
        return;
    }

    finishFobosStop(false);
}

void YourClassName::connectDataProcessorSignals() {
    if (!processor) {
        return;
    }

    DataProcessor *connectedProcessor = processor;
    connect(processor,
            &DataProcessor::readerFailed,
            this,
            [this, connectedProcessor](int errorCode, bool stoppedByRequest) {
                if (connectedProcessor != processor) {
                    qDebug() << "[FobosLifecycle] ignoring stale DataProcessor reader failure"
                             << "error" << errorCode;
                    return;
                }
                handleDataProcessorFailure(errorCode, stoppedByRequest);
            },
            Qt::QueuedConnection);

    connect(processor,
            &DataProcessor::iqFrameReady,
            this,
            [this, connectedProcessor](const QByteArray &iqData, double sampleRate, int sampleCount,
                                       bool channelizedFrame) {
                if (connectedProcessor != processor) {
                    return;
                }
                if (!dmrAudioIqProducerNeeded()) {
                    handleLocalIqFrame(iqData, sampleRate, sampleCount, channelizedFrame);
                }
            },
            Qt::QueuedConnection);
    if (wirelessLabDialog) {
        connect(processor, &DataProcessor::wifiIqFrameReady,
                wirelessLabDialog, &WirelessLabDialog::appendLiveIq,
                static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));
        processor->setWifiIqStreamingEnabled(wirelessLabDialog->isLiveIqActive());
    }
}

void YourClassName::applyServerLocalOutputPolicy() {
    const bool suppressServerLocalOutput =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();

    if (audioProcessor) {
        audioProcessor->setLocalPlaybackEnabled(!suppressServerLocalOutput);
    }
    updateVideoProcessorMode();
    updateIqFrameProducerSettings();

    if (!updateTimer || runState != RadioRunState::Running || networkMode != NetworkMode::Server) {
        return;
    }

    const bool spectrumNeededForNetwork =
        networkController &&
        networkController->isControlReady() &&
        !isFullIqProcessingMode();
    const bool localVisualNeeded = !suppressServerLocalOutput;

    if (spectrumNeededForNetwork || localVisualNeeded) {
        if (!updateTimer->isActive()) {
            updateTimer->start();
        }
    } else if (updateTimer->isActive()) {
        updateTimer->stop();
    }
}

static void logIqBufferRetuneState(const char *stage,
                                   const QString &reason,
                                   std::uint64_t retuneEpoch,
                                   double previousFrequency,
                                   double requestedFrequency,
                                   double actualFrequency) {
    const IqBuffer::Stats stats = IqBuffer::stats();
    qDebug() << "[LiveTuneIQ]" << stage
             << "reason" << reason
             << "retuneEpoch" << static_cast<qulonglong>(retuneEpoch)
             << "bufferEpoch" << static_cast<qulonglong>(stats.epoch)
             << "sequence" << static_cast<qulonglong>(stats.sequence)
             << "previousHz" << previousFrequency
             << "requestedHz" << requestedFrequency
             << "actualHz" << actualFrequency
             << "snapshotStart" << stats.snapshotStart
             << "snapshotSize" << stats.snapshotSize
             << "queuedBlocks" << stats.queuedBlocks
             << "queuedFloats" << stats.queuedFloatCount
             << "sampleRateEstimate" << stats.sampleRateEstimate;
}

bool YourClassName::applyCenterFrequencyToHardwareIfNeeded(const RadioSettings &previousSettings,
                                                           const char *reason) {
    if (pendingSettings.inputMode != INPUT_RF ||
        isIdle() ||
        std::abs(previousSettings.centerFrequency - pendingSettings.centerFrequency) <= 0.5) {
        return true;
    }

    const QString reasonText = QString::fromUtf8(reason ? reason : "");
    const bool directExternalRetune =
        isRtlBackendSelected() ||
        isSoapySdrSelected() ||
        isBladeRfNativeSelected() ||
        isHackRfNativeSelected();
    if (directExternalRetune) {
        const QString backendName = isHackRfNativeSelected()
                                        ? QStringLiteral("HackRF")
                                    : isBladeRfNativeSelected()
                                        ? QStringLiteral("bladeRF")
                                    : isSoapySdrSelected()
                                        ? QStringLiteral("SoapySDR")
                                        : QStringLiteral("RTL");
        const uint64_t preRetuneIqEpoch =
            processor ? processor->beginIqRetuneBarrier() : 0;
        clearLiveSpectrumSnapshot(false, preRetuneIqEpoch);
        IqBuffer::armRetuneTrace(preRetuneIqEpoch, 6, 4, 6, 6);
        logIqBufferRetuneState("external-pre-retune-clear",
                               reasonText,
                               preRetuneIqEpoch,
                               previousSettings.centerFrequency,
                               pendingSettings.centerFrequency,
                               pendingSettings.actualFrequency);
        if (processor) {
            processor->setFrequencyCalibrationOffset(
                effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency));
        }
        const bool retuned = processor &&
                             processor->isRunning() &&
                             processor->retuneCenterFrequency(pendingSettings.centerFrequency);
        qDebug() << "[LiveTune]" << reason
                 << backendName << "center retune"
                 << "previous" << previousSettings.centerFrequency
                 << "requested" << pendingSettings.centerFrequency
                 << "result" << retuned;
        if (retuned) {
            const uint64_t postRetuneIqEpoch =
                processor ? processor->beginIqRetuneBarrier() : 0;
            pendingSettings.actualFrequency = pendingSettings.centerFrequency;
            if (hardwareSettingsApplied) {
                appliedHardwareSettings.centerFrequency = pendingSettings.centerFrequency;
                appliedHardwareSettings.actualFrequency = pendingSettings.actualFrequency;
            }
            networkSpectrumFrameMetadataValid = false;
            networkSpectrumFrameMinFrequency = 0.0;
            networkSpectrumFrameMaxFrequency = 0.0;
            networkSpectrumFrameFftLength = 0;
            clearLiveSpectrumSnapshot(false, postRetuneIqEpoch);
            IqBuffer::armRetuneTrace(postRetuneIqEpoch);
            logIqBufferRetuneState("external-post-retune-clear",
                                   reasonText,
                                   postRetuneIqEpoch,
                                   previousSettings.centerFrequency,
                                   pendingSettings.centerFrequency,
                                   pendingSettings.actualFrequency);
            liveRetuneSettleDurationMs = LIVE_RETUNE_SETTLE_MS;
            liveRetuneSettleTimer.start();
            spectrumTuningDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 8 : 0;
        }
        return retuned;
    }

    if (!hasActiveFobosDevice()) {
        return true;
    }

    const bool liveAgileRfRetune =
        activeFobosApiKind == FobosApiKind::Agile &&
        !agileScanEnabled &&
        processor &&
        processor->isRunning();
    if (liveAgileRfRetune) {
        return scheduleLiveAgileCenterRetune(QString::fromUtf8(reason ? reason : "live"));
    }

    const uint64_t preRetuneIqEpoch =
        processor ? processor->beginIqRetuneBarrier() : 0;
    clearLiveSpectrumSnapshot(false, preRetuneIqEpoch);
    IqBuffer::armRetuneTrace(preRetuneIqEpoch, 6, 4, 6, 6);
    logIqBufferRetuneState("fobos-pre-retune-clear",
                           reasonText,
                           preRetuneIqEpoch,
                           previousSettings.centerFrequency,
                           pendingSettings.centerFrequency,
                           pendingSettings.actualFrequency);
    double tunedFrequency = pendingSettings.centerFrequency;
    const int result = setCalibratedActiveFrequencySafely(pendingSettings.centerFrequency, &tunedFrequency);
    if (result == FOBOS_ERR_OK) {
        const uint64_t postRetuneIqEpoch =
            processor ? processor->beginIqRetuneBarrier() : 0;
        clearLiveSpectrumSnapshot(false, postRetuneIqEpoch);
        IqBuffer::armRetuneTrace(postRetuneIqEpoch);
        pendingSettings.actualFrequency = tunedFrequency;
        if (hardwareSettingsApplied) {
            appliedHardwareSettings.centerFrequency = pendingSettings.centerFrequency;
            appliedHardwareSettings.actualFrequency = tunedFrequency;
        }
        logIqBufferRetuneState("fobos-post-retune-clear",
                               reasonText,
                               postRetuneIqEpoch,
                               previousSettings.centerFrequency,
                               pendingSettings.centerFrequency,
                               tunedFrequency);
        qDebug() << "[LiveTune]" << reason
                 << "center applied"
                 << "requested" << pendingSettings.centerFrequency
                 << "actual" << tunedFrequency
                 << "iqEpoch" << postRetuneIqEpoch;
        networkSpectrumFrameMetadataValid = false;
        networkSpectrumFrameMinFrequency = 0.0;
        networkSpectrumFrameMaxFrequency = 0.0;
        networkSpectrumFrameFftLength = 0;
        liveRetuneSettleDurationMs = LIVE_RETUNE_SETTLE_MS;
        liveRetuneSettleTimer.start();
        spectrumTuningDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 8 : 0;
        qDebug() << "[LiveTune]" << reason
                 << "cleared live IQ after center retune; preserving visual history"
                 << "settleMs" << liveRetuneSettleDurationMs;
        return true;
    }

    qDebug() << "[LiveTune]" << reason
             << "center apply failed"
             << "requested" << pendingSettings.centerFrequency
             << "error" << result;
    return false;
}

bool YourClassName::scheduleLiveAgileCenterRetune(const QString &reason) {
    const uint64_t generation = ++liveCenterRetuneGeneration;
    const int commandIntervalMs = (std::clamp)(agileLiveRetuneCommandIntervalMs,
                                               AGILE_LIVE_RETUNE_MIN_COMMAND_INTERVAL_MS,
                                               AGILE_LIVE_RETUNE_MAX_COMMAND_INTERVAL_MS);
    const qint64 elapsedMs = agileLiveRetuneCommandTimer.isValid()
                                 ? agileLiveRetuneCommandTimer.elapsed()
                                 : commandIntervalMs;
    const bool timerActive = agileLiveRetuneTimer && agileLiveRetuneTimer->isActive();
    if (commandIntervalMs <= 0 ||
        (!timerActive && elapsedMs >= commandIntervalMs)) {
        queuedLiveCenterRetuneGeneration = 0;
        queuedLiveCenterRetuneReason.clear();
        return applyLiveAgileCenterRetune(generation, reason);
    }

    queuedLiveCenterRetuneGeneration = generation;
    queuedLiveCenterRetuneReason = reason;
    const qint64 delayMs = (std::max<qint64>)(
        1,
        static_cast<qint64>(commandIntervalMs) - elapsedMs);
    if (agileLiveRetuneTimer) {
        agileLiveRetuneTimer->start(static_cast<int>((std::min<qint64>)(delayMs, 1000)));
    }
    return true;
}

void YourClassName::flushQueuedLiveAgileCenterRetune() {
    const uint64_t generation = queuedLiveCenterRetuneGeneration;
    if (generation == 0) {
        return;
    }

    const int commandIntervalMs = (std::clamp)(agileLiveRetuneCommandIntervalMs,
                                               AGILE_LIVE_RETUNE_MIN_COMMAND_INTERVAL_MS,
                                               AGILE_LIVE_RETUNE_MAX_COMMAND_INTERVAL_MS);
    const qint64 elapsedMs = agileLiveRetuneCommandTimer.isValid()
                                 ? agileLiveRetuneCommandTimer.elapsed()
                                 : commandIntervalMs;
    if (elapsedMs < commandIntervalMs) {
        const qint64 delayMs = (std::max<qint64>)(1, commandIntervalMs - elapsedMs);
        if (agileLiveRetuneTimer) {
            agileLiveRetuneTimer->start(static_cast<int>((std::min<qint64>)(delayMs, 1000)));
        }
        return;
    }

    const QString reason = queuedLiveCenterRetuneReason.isEmpty()
                               ? QStringLiteral("queued live retune")
                               : queuedLiveCenterRetuneReason;
    queuedLiveCenterRetuneGeneration = 0;
    queuedLiveCenterRetuneReason.clear();
    applyLiveAgileCenterRetune(generation, reason);
}

bool YourClassName::applyLiveAgileCenterRetune(uint64_t generation, const QString &reason) {
    if (generation != liveCenterRetuneGeneration) {
        qDebug() << "[LiveTune]" << reason
                 << "skipping stale Agile live retune"
                 << "generation" << generation
                 << "current" << liveCenterRetuneGeneration;
        return true;
    }

    if (runState != RadioRunState::Running ||
        pendingSettings.inputMode != INPUT_RF ||
        !hasActiveFobosDevice() ||
        activeFobosApiKind != FobosApiKind::Agile ||
        agileScanEnabled ||
        !processor ||
        !processor->isRunning()) {
        qDebug() << "[LiveTune]" << reason
                 << "skipping Agile live retune because stream is not in normal RF running state"
                 << "generation" << generation
                 << "state" << static_cast<int>(runState)
                 << "inputMode" << pendingSettings.inputMode
                 << "apiKind" << static_cast<int>(activeFobosApiKind)
                 << "agileScan" << agileScanEnabled;
        return true;
    }

    if (liveRetuneSettleTimer.isValid()) {
        const qint64 elapsedMs = liveRetuneSettleTimer.elapsed();
        const qint64 settleMs =
            liveRetuneSettleDurationMs > 0 ? liveRetuneSettleDurationMs : LIVE_RETUNE_SETTLE_MS;
        if (elapsedMs < settleMs) {
            queuedLiveCenterRetuneGeneration = generation;
            queuedLiveCenterRetuneReason = reason;
            const qint64 delayMs = (std::max<qint64>)(1, settleMs - elapsedMs);
            if (agileLiveRetuneTimer) {
                agileLiveRetuneTimer->start(static_cast<int>((std::min<qint64>)(delayMs, 1000)));
            }
            return true;
        }

        liveRetuneSettleTimer.invalidate();
    }

    if (!stopAgileScanForNormalRf("live center retune")) {
        qDebug() << "[LiveTune]" << reason
                 << "Agile live center retune aborted because scan mode could not be stopped"
                 << "generation" << generation;
        return false;
    }

    const double requestedFrequency = pendingSettings.centerFrequency;
    const double previousFrequency = actualFrequency;
    agileLiveRetuneCommandTimer.restart();

    const uint64_t preRetuneIqEpoch =
        processor ? processor->beginIqRetuneBarrier() : 0;
    clearLiveSpectrumSnapshot(false, preRetuneIqEpoch);
    IqBuffer::armRetuneTrace(preRetuneIqEpoch, 6, 4, 6, 6);
    logIqBufferRetuneState("agile-live-pre-retune-clear",
                           reason,
                           preRetuneIqEpoch,
                           previousFrequency,
                           requestedFrequency,
                           pendingSettings.actualFrequency);

    QElapsedTimer tuneTimer;
    tuneTimer.start();
    double tunedFrequency = requestedFrequency;
    const int tuneResult = setCalibratedActiveFrequencySafely(requestedFrequency, &tunedFrequency);
    const qint64 tuneMs = tuneTimer.elapsed();
    if (tuneResult != FOBOS_ERR_OK) {
        qDebug() << "[LiveTune]" << reason
                 << "Agile live set_frequency failed"
                 << "requested" << requestedFrequency
                 << "result" << tuneResult
                 << "tuneMs" << tuneMs;
        return false;
    }

    pendingSettings.actualFrequency = tunedFrequency;
    actualFrequency = tunedFrequency;
    if (hardwareSettingsApplied) {
        appliedHardwareSettings.centerFrequency = requestedFrequency;
        appliedHardwareSettings.actualFrequency = tunedFrequency;
    }
    publishSettingsToGlobals();
    updateIqFrameProducerSettings();
    if (processor) {
        processor->setCenterFrequencyHint(tunedFrequency);
    }
    networkSpectrumFrameMetadataValid = false;
    networkSpectrumFrameMinFrequency = 0.0;
    networkSpectrumFrameMaxFrequency = 0.0;
    networkSpectrumFrameFftLength = 0;

    const uint64_t postRetuneIqEpoch =
        processor ? processor->beginIqRetuneBarrier() : 0;
    clearLiveSpectrumSnapshot(false, postRetuneIqEpoch);
    IqBuffer::armRetuneTrace(postRetuneIqEpoch);
    logIqBufferRetuneState("agile-live-post-retune-clear",
                           reason,
                           postRetuneIqEpoch,
                           previousFrequency,
                           requestedFrequency,
                           tunedFrequency);

    liveRetuneSettleDurationMs = agileRfLiveSettleMs(pendingSettings.sampleRate, false);
    liveRetuneSettleTimer.start();
    spectrumTuningDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 8 : 0;

    qDebug() << "[LiveTune]" << reason
             << "Agile live set_frequency done without reader restart"
             << "previous" << previousFrequency
             << "requested" << requestedFrequency
             << "actual" << tunedFrequency
             << "tuneMs" << tuneMs
             << "settleMs" << liveRetuneSettleDurationMs
             << "iqEpoch" << postRetuneIqEpoch;
    return true;
}

void YourClassName::clearLiveSpectrumSnapshot(bool clearVisualHistory, uint64_t iqEpoch) {
    IqBuffer::clear(iqEpoch);
    fftResult = std::make_unique<FFTResult>();
    if (spectrumFftWorker) {
        spectrumFftWorker->resetHfNoiseCancelState();
    }
    scanVisualAssembler.reset();
    networkSpectrumFrameMetadataValid = false;
    networkSpectrumFrameMinFrequency = 0.0;
    networkSpectrumFrameMaxFrequency = 0.0;
    networkSpectrumFrameFftLength = 0;

    if (!clearVisualHistory) {
        return;
    }

    if (graphWidget) {
        graphWidget->clearData();
        graphWidget->update();
    }
    if (waterfallWidget) {
        waterfallWidget->clearData();
        waterfallWidget->update();
    }
}

void YourClassName::updateUiForRunState() {
    const bool processorRunning = processor && processor->isRunning();
    const bool idle = isIdle() && !processorRunning;
    if (!idle && usbDeviceRefreshTimer) {
        usbDeviceRefreshTimer->stop();
    }
    const bool clientCanControl =
        !isNetworkClientMode() ||
        !networkController ||
        networkController->clientHasControl();

    if (startButton) startButton->setEnabled(idle && clientCanControl);
    if (stopButton) {
        const bool canStop =
            runState == RadioRunState::Starting ||
            runState == RadioRunState::Running ||
            processorRunning;
        stopButton->setEnabled(canStop && (clientCanControl || isNetworkClientMode()));
    }
    if (comboBox) {
        const bool receiverListReady =
            !isNetworkClientMode() || remoteReceiverDeviceListValid;
        comboBox->setEnabled(idle && clientCanControl && receiverListReady);
    }
    if (refreshButton) refreshButton->setEnabled(idle && clientCanControl);
    if (fobosButton) fobosButton->setEnabled(idle);
    if (modeBox) modeBox->setEnabled(true);
    if (sampleBox) sampleBox->setEnabled(true);
    if (clkBox) clkBox->setEnabled(idle && !isHackRfNativeSelected());
    if (fftComboBox) {
        fftComboBox->setEnabled((idle || runState == RadioRunState::Running) &&
                                !fftBinWidthModeEnabled);
    }
    if (fftBinWidthModeCheckbox) {
        fftBinWidthModeCheckbox->setEnabled(idle || runState == RadioRunState::Running);
    }
    if (fftBinWidthSpin) {
        fftBinWidthSpin->setEnabled((idle || runState == RadioRunState::Running) &&
                                    fftBinWidthModeEnabled);
    }
    if (audioDeviceComboBox) audioDeviceComboBox->setEnabled(idle);
    const bool liveAudioControlsEnabled =
        idle || runState == RadioRunState::Running;

    if (audioCheckbox) audioCheckbox->setEnabled(liveAudioControlsEnabled);
    if (wirelessLabDialog) wirelessLabDialog->setAudioControlEnabled(liveAudioControlsEnabled);
    if (wirelessLabDialog) {
        wirelessLabDialog->setLiveIqSource(pendingSettings.sampleRate,
            pendingSettings.centerFrequency,
            runState == RadioRunState::Running && pendingSettings.inputMode == INPUT_RF &&
                !isNetworkClientMode());
    }
    if (syncCheckbox) syncCheckbox->setEnabled(false);
    const bool liveDemodControlsEnabled =
        idle || runState == RadioRunState::Running;

    if (bandwidthControl) bandwidthControl->setEnabled(liveDemodControlsEnabled);
    const bool gainControlsEnabled =
        idle || runState == RadioRunState::Running;

    const bool nativeGainControlsAvailable =
        !isExternalReceiverBackendSelected() || isHackRfNativeSelected();
    if (lnaGainSlider) {
        lnaGainSlider->setEnabled(gainControlsEnabled && nativeGainControlsAvailable);
    }
    if (vgaGainSlider) {
        vgaGainSlider->setEnabled(gainControlsEnabled && nativeGainControlsAvailable);
    }
    const bool rtlGainControlsVisible = isRtlBackendSelected();
    if (rtlAgcCheckbox) {
        rtlAgcCheckbox->setVisible(rtlGainControlsVisible);
        rtlAgcCheckbox->setEnabled(gainControlsEnabled && rtlGainControlsVisible);
    }
    if (rtlGainLabel) {
        rtlGainLabel->setVisible(rtlGainControlsVisible);
    }
    if (rtlGainSlider) {
        rtlGainSlider->setVisible(rtlGainControlsVisible);
        rtlGainSlider->setEnabled(gainControlsEnabled && rtlGainControlsVisible && !pendingSettings.rtlAgc);
    }
    const bool gpioEnabled =
        idle || runState == RadioRunState::Running;
    for (int i = 0; i < 8; ++i) {
        if (checkBoxes[i]) {
            checkBoxes[i]->setEnabled(gpioEnabled);
        }
    }
}

void YourClassName::onAudioEnabledChanged(bool checked) {
    pendingSettings.audioEnabled = checked;
    publishSettingsToGlobals();

    qDebug() << "[Audio] checkbox changed" << checked
             << "networkMode" << static_cast<int>(networkMode)
             << "processingMode" << static_cast<int>(networkProcessingMode)
             << "runState" << runStateName(runState);

    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();

        if (isClientIqProcessingMode()) {
            if (checked) {
                if (isFullIqProcessingMode()) {
                    pendingNetworkAudioStartAfterIqPrebuffer = false;
                    if (audioProcessor) {
                        audioProcessor->stopDemodulation();
                    }
                } else {
                    IqBuffer::clear();
                    pendingNetworkAudioStartAfterIqPrebuffer = true;
                }
            } else {
                pendingNetworkAudioStartAfterIqPrebuffer = false;
                if (audioProcessor) {
                    audioProcessor->stopDemodulation();
                }
                if (remoteAudioPlayer) {
                    remoteAudioPlayer->stop();
                }
            }
        } else {
            if (!checked && remoteAudioPlayer) {
                remoteAudioPlayer->stop();
            }
        }

        return;
    }

    if (offlineIqPlaybackActive) {
        if (checked) {
            if (audioProcessor) {
                audioProcessor->configure(audioProcessorSettings());
            }
            pendingPlaybackAudioStartAfterIqPrebuffer = true;
        } else if (audioProcessor) {
            pendingPlaybackAudioStartAfterIqPrebuffer = false;
            audioProcessor->stopDemodulation();
        }
        return;
    }

    if (isIdle()) {
        return;
    }

    if (checked) {
        if (processor && processor->isRunning() && audioProcessor) {
            audioProcessor->startDemodulation();
        } else {
            pendingAudioStartAfterStreamReady = true;
        }
    } else {
        pendingAudioStartAfterStreamReady = false;
        if (audioProcessor) {
            audioProcessor->stopDemodulation();
        }
    }
}

void YourClassName::updateSpectrumTimerInterval() {
    if (!updateTimer) {
        return;
    }

    if (spectrumUpdateIntervalMs > SPECTRUM_UPDATE_AUTO_MS) {
        updateTimer->setInterval((std::clamp)(spectrumUpdateIntervalMs,
                                              SPECTRUM_UPDATE_MIN_MS,
                                              SPECTRUM_UPDATE_MAX_MS));
        return;
    }

    int intervalMs = 33;
    if (pendingSettings.fftLength >= 100000000) {
        intervalMs = 5000;
    } else if (pendingSettings.fftLength >= 67108864) {
        intervalMs = 2800;
    } else if (pendingSettings.fftLength >= 33554432) {
        intervalMs = 1400;
    } else if (pendingSettings.fftLength >= 16777216) {
        intervalMs = 700;
    } else if (pendingSettings.fftLength >= 8388608) {
        intervalMs = 400;
    } else if (pendingSettings.fftLength >= 4194304) {
        intervalMs = 250;
    } else if (pendingSettings.fftLength >= 2097152) {
        intervalMs = 160;
    } else if (pendingSettings.fftLength >= 1048576) {
        intervalMs = 120;
    } else if (pendingSettings.fftLength >= 262144) {
        intervalMs = 80;
    } else if (pendingSettings.fftLength >= 131072) {
        intervalMs = 50;
    } else if (pendingSettings.fftLength >= 65536) {
        intervalMs = 40;
    }
    updateTimer->setInterval(intervalMs);
}

void YourClassName::revertHardwareControlsToSettings() {
    if (modeBox) {
        modeBox->blockSignals(true);
        const int index = modeBox->findData(pendingSettings.inputMode);
        if (index >= 0) {
            modeBox->setCurrentIndex(index);
        }
        modeBox->blockSignals(false);
    }
    if (clkBox) {
        clkBox->blockSignals(true);
        const int index = clkBox->findData(pendingSettings.clockSource);
        if (index >= 0) {
            clkBox->setCurrentIndex(index);
        }
        clkBox->blockSignals(false);
    }
    if (sampleBox) {
        sampleBox->blockSignals(true);
        const int index = sampleBox->findData(pendingSettings.sampleRate);
        if (index >= 0) {
            sampleBox->setCurrentIndex(index);
        }
        sampleBox->blockSignals(false);
    }
    syncFftResolutionControls();
    if (audioDeviceComboBox) {
        audioDeviceComboBox->blockSignals(true);
        const int index = audioDeviceComboBox->findData(pendingSettings.audioDeviceId);
        if (index >= 0) {
            audioDeviceComboBox->setCurrentIndex(index);
        }
        audioDeviceComboBox->blockSignals(false);
    }
}

bool YourClassName::openFobosSession() {
    int selectedDevice = pendingSettings.deviceIndex;
    if (selectedDevice < 0) {
        selectedDevice = 0;
    }
    if (availableFobosDevices.isEmpty() || !selectedFobosDeviceInfo().present) {
        refreshFobosDeviceList();
        rebuildReceiverDeviceCombo();
        updateAgileScanControls();
    }
    FobosDeviceInfo selectedInfo = selectedFobosDeviceInfo();
    if (!selectedInfo.present) {
        qWarning() << "[FobosDevices] selected Fobos is not detected; refusing a different receiver";
        return false;
    }
    selectedDevice = selectedInfo.nativeIndex;

    qDebug() << "[FobosLifecycle] openFobosSession enter"
             << "logicalDevice" << pendingSettings.deviceIndex
             << "nativeDevice" << selectedDevice
             << "apiKind" << fobosApiKindName(selectedInfo.apiKind)
             << "device" << activeFobosDevice()
             << "openedDeviceIndex" << openedDeviceIndex
             << "openedNativeDeviceIndex" << openedNativeDeviceIndex
             << "openedApiKind" << fobosApiKindName(openedDeviceApiKind)
             << "appliedSampleRate" << appliedSampleRate
             << "pendingSampleRate" << pendingSettings.sampleRate
             << "sampleRateReopenRequired" << sampleRateReopenRequired
             << "fobosCloseKnownUnsafe" << fobosCloseKnownUnsafe;

    if (hasActiveFobosDevice() &&
        openedDeviceIndex == pendingSettings.deviceIndex &&
        openedNativeDeviceIndex == selectedInfo.nativeIndex &&
        openedDeviceApiKind == selectedInfo.apiKind) {
        if (selectedInfo.apiKind == FobosApiKind::Agile) {
            qDebug() << "[FobosLifecycle] reusing idle Agile session; settings will be applied in place"
                     << activeFobosDevice();
            return true;
        } else {
            qDebug() << "[FobosLifecycle] reusing idle Fobos session; settings will be applied in place"
                     << activeFobosDevice();
            return true;
        }
    }

    if (hasActiveFobosDevice()) {
        qDebug() << "[FobosLifecycle] closing mismatched existing Fobos session before open";
        if (!closeFobosSession(false)) {
            qDebug() << "[FobosLifecycle] existing Fobos session could not be closed; open aborted";
            return false;
        }
        qDebug() << "[FobosLifecycle] mismatched Fobos session closed before fresh open; waiting before reopen";
        QThread::msleep(350);
    }

    activeFobosApiKind = selectedInfo.apiKind;
    int ret = FOBOS_ERR_OK;
    if (selectedInfo.apiKind == FobosApiKind::Agile) {
        qDebug() << "[FobosLifecycle] fobos_sdr_open begin" << "selectedDevice" << selectedDevice;
        ret = openFobosAgileDeviceSafely(&agileDevice, static_cast<uint32_t>(selectedDevice));
        qDebug() << "[FobosLifecycle] fobos_sdr_open end" << "result" << ret << "device" << agileDevice;
        if (ret != FOBOS_ERR_OK || !agileDevice) {
            qDebug() << "[FobosLifecycle] fobos_sdr_open failed; waiting before one recovery retry"
                     << "result" << ret
                     << "selectedDevice" << selectedDevice;
            agileDevice = nullptr;
            QThread::msleep(700);
            ret = openFobosAgileDeviceSafely(&agileDevice, static_cast<uint32_t>(selectedDevice));
            qDebug() << "[FobosLifecycle] fobos_sdr_open retry end"
                     << "result" << ret
                     << "device" << agileDevice;
        }
    } else {
        qDebug() << "[FobosLifecycle] fobos_rx_open begin" << "selectedDevice" << selectedDevice;
        ret = openFobosDeviceSafely(&device, static_cast<uint32_t>(selectedDevice));
        qDebug() << "[FobosLifecycle] fobos_rx_open end" << "result" << ret << "device" << device;
    }

    if (ret != FOBOS_ERR_OK || !hasActiveFobosDevice()) {
        qDebug() << "Failed to open Fobos device, error code:" << ret;
        device = nullptr;
        agileDevice = nullptr;
        agileScanRunning = false;
        activeAgileScanFrequencies.clear();
        resetStandardScanState(true);
        resetListeningScanState();
        activeFobosApiKind = FobosApiKind::Standard;
        openedDeviceIndex = -1;
        openedNativeDeviceIndex = -1;
        return false;
    }

    openedDeviceIndex = pendingSettings.deviceIndex;
    openedNativeDeviceIndex = selectedInfo.nativeIndex;
    openedDeviceApiKind = selectedInfo.apiKind;
    appliedHardwareSettings = RadioSettings{};
    hardwareSettingsApplied = false;
    sampleRateReopenRequired = false;
    fobosCloseKnownUnsafe = false;
    return true;
}

bool YourClassName::closeFobosSession(bool clearIq) {
    qDebug() << "[FobosLifecycle] closeFobosSession enter"
             << "clearIq" << clearIq
             << "device" << activeFobosDevice()
             << "openedDeviceIndex" << openedDeviceIndex
             << "openedNativeDeviceIndex" << openedNativeDeviceIndex
             << "apiKind" << fobosApiKindName(openedDeviceApiKind);
    bool closeOk = true;
    if (clearIq) {
        qDebug() << "[FobosLifecycle] clearing IQ buffer before close";
        IqBuffer::clear();
    }

    if (device) {
        qDebug() << "[FobosLifecycle] fobos_rx_close begin" << device;
        const int closeResult = closeFobosDeviceSafely(device);
        qDebug() << "[FobosLifecycle] fobos_rx_close end" << "result" << closeResult;
        closeOk = closeResult == FOBOS_ERR_OK;
        if (!closeOk) {
            qDebug() << "Fobos close returned error code:" << closeResult;
            qDebug() << "[FobosLifecycle] close failed; skipping synchronous USB reset on stop to keep UI responsive"
                     << device;
            qDebug() << "[FobosLifecycle] abandoning Fobos session pointer after close failure; next start will try a fresh open"
                     << device;
        }
        device = nullptr;
    }
    if (agileDevice) {
        const int scanning = isFobosAgileScanningSafely(agileDevice);
        if (agileScanRunning || scanning > 0) {
            qDebug() << "[FobosLifecycle] stopping Agile scan before close";
            const int scanStopResult = stopFobosAgileScanSafely(agileDevice);
            qDebug() << "[FobosLifecycle] fobos_sdr_stop_scan end"
                     << "result" << scanStopResult
                     << "isScanningBefore" << scanning;
            agileScanRunning = false;
            activeAgileScanFrequencies.clear();
            scanVisualAssembler.reset();
        }
        qDebug() << "[FobosLifecycle] fobos_sdr_close begin" << agileDevice;
        const int closeResult = closeFobosAgileDeviceSafely(agileDevice);
        qDebug() << "[FobosLifecycle] fobos_sdr_close end" << "result" << closeResult;
        closeOk = closeOk && closeResult == FOBOS_ERR_OK;
        if (closeResult != FOBOS_ERR_OK) {
            qDebug() << "Fobos agile close returned error code:" << closeResult;
            qDebug() << "[FobosLifecycle] close failed; skipping synchronous Agile USB reset on stop to keep UI responsive"
                     << agileDevice;
            qDebug() << "[FobosLifecycle] abandoning Agile session pointer after close failure; next start will try a fresh open"
                     << agileDevice;
        }
        agileDevice = nullptr;
    }
    openedDeviceIndex = -1;
    openedNativeDeviceIndex = -1;
    appliedSampleRate = 0.0;
    appliedHardwareSettings = RadioSettings{};
    hardwareSettingsApplied = false;
    sampleRateReopenRequired = false;
    fobosCloseKnownUnsafe = false;
    closeFobosSessionAfterStop = false;
    resetStandardScanState(true);
    resetListeningScanState();
    activeFobosApiKind = FobosApiKind::Standard;
    openedDeviceApiKind = FobosApiKind::Standard;
    qDebug() << "[FobosLifecycle] closeFobosSession exit";
    return closeOk;
}

bool YourClassName::prepareFobosSessionFromSettings(const QString &reason) {
    if (isNetworkClientMode()) {
        qDebug() << "[FobosLifecycle] idle Fobos prepare skipped in network client mode"
                 << "reason" << reason;
        return false;
    }
    if (isExternalReceiverBackendSelected()) {
        qDebug() << "[FobosLifecycle] idle Fobos prepare skipped for external receiver backend"
                 << "reason" << reason;
        if (hasActiveFobosDevice()) {
            closeFobosSession(false);
        }
        return false;
    }
    const bool canPrepare = isIdle() || runState == RadioRunState::Starting;
    if (!canPrepare || deviceOpened || (processor && processor->isRunning())) {
        qDebug() << "[FobosLifecycle] idle Fobos prepare skipped because stream is active"
                 << "reason" << reason
                 << "state" << runStateName(runState)
                 << "deviceOpened" << deviceOpened
                 << "processorRunning" << (processor && processor->isRunning());
        return false;
    }

    refreshSettingsFromUi();
    if (offsetDmrCenterFromListening(pendingSettings)) {
        qDebug() << "[FobosLifecycle] DMR idle prepare offset RF center from listening frequency"
                 << "reason" << reason
                 << "center" << pendingSettings.centerFrequency
                 << "listening" << pendingSettings.listeningFrequency;
        if (frequencyControl) {
            QSignalBlocker blocker(frequencyControl);
            frequencyControl->setValueHz(pendingSettings.centerFrequency);
        }
        if (listeningFrequencyControl) {
            QSignalBlocker blocker(listeningFrequencyControl);
            listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
        }
        settingRange();
    }

    qDebug() << "[FobosLifecycle] prepare idle Fobos session begin"
             << "reason" << reason
             << "deviceIndex" << pendingSettings.deviceIndex
             << "sampleRate" << pendingSettings.sampleRate
             << "centerFrequency" << pendingSettings.centerFrequency
             << "inputMode" << pendingSettings.inputMode
             << "existingDevice" << activeFobosDevice()
             << "apiKind" << fobosApiKindName(activeFobosApiKind);

    if (!openFobosSession()) {
        qDebug() << "[FobosLifecycle] prepare idle Fobos session failed while opening"
                 << "reason" << reason;
        return false;
    }

    const bool forceFrequencyApply = pendingSettings.inputMode == INPUT_RF;
    if (!applyFobosSettings(forceFrequencyApply)) {
        qDebug() << "[FobosLifecycle] prepare idle Fobos session failed while applying settings"
                 << "reason" << reason;
        closeFobosSession(false);
        return false;
    }
    if (!applyAgileStartupFrequencyKick(reason)) {
        qDebug() << "[FobosLifecycle] prepare idle Fobos session failed during Agile startup frequency kick"
                 << "reason" << reason;
        closeFobosSession(false);
        return false;
    }

    qDebug() << "[FobosLifecycle] prepare idle Fobos session ready"
             << "reason" << reason
             << "device" << activeFobosDevice()
             << "apiKind" << fobosApiKindName(activeFobosApiKind)
             << "sampleRate" << appliedSampleRate
             << "centerFrequency" << pendingSettings.centerFrequency
             << "actualFrequency" << pendingSettings.actualFrequency;
    return true;
}

bool YourClassName::applyAgileStartupFrequencyKick(const QString &reason) {
    if (activeFobosApiKind != FobosApiKind::Agile ||
        pendingSettings.inputMode != INPUT_RF ||
        agileScanEnabled ||
        !hasActiveFobosDevice()) {
        return true;
    }

    const double targetFrequency = pendingSettings.centerFrequency;
    if (!std::isfinite(targetFrequency) || targetFrequency <= 0.0) {
        qDebug() << "[FobosLifecycle] Agile startup frequency kick skipped invalid target"
                 << "reason" << reason
                 << "target" << targetFrequency;
        return true;
    }

    // Some Agile units can start with a shifted IQ/frequency view after the
    // first tuning command. A second command with a different frequency, even
    // by 1 Hz, reliably settles the driver/device state before the reader
    // starts. Keep this before async IQ read; live retune uses its own path.
    constexpr double kickOffsetHz = 1.0;
    double kickActualFrequency = targetFrequency;
    qDebug() << "[FobosLifecycle] Agile startup frequency kick begin"
             << "reason" << reason
             << "target" << targetFrequency
             << "kick" << (targetFrequency + kickOffsetHz);
    int result = setCalibratedActiveFrequencySafely(targetFrequency + kickOffsetHz, &kickActualFrequency);
    qDebug() << "[FobosLifecycle] Agile startup frequency kick detune end"
             << "result" << result
             << "actual" << kickActualFrequency;
    if (result != FOBOS_ERR_OK) {
        double fallbackActualFrequency = targetFrequency;
        qDebug() << "[FobosLifecycle] Agile startup frequency kick detune +1Hz failed, trying -1Hz"
                 << "result" << result;
        result = setCalibratedActiveFrequencySafely(targetFrequency - kickOffsetHz, &fallbackActualFrequency);
        qDebug() << "[FobosLifecycle] Agile startup frequency kick fallback detune end"
                 << "result" << result
                 << "actual" << fallbackActualFrequency;
    }

    double finalActualFrequency = targetFrequency;
    const int finalResult = setCalibratedActiveFrequencySafely(targetFrequency, &finalActualFrequency);
    qDebug() << "[FobosLifecycle] Agile startup frequency kick restore end"
             << "result" << finalResult
             << "actual" << finalActualFrequency;
    if (finalResult != FOBOS_ERR_OK) {
        qDebug() << "[FobosLifecycle] Agile startup frequency kick failed to restore target"
                 << "result" << finalResult;
        return false;
    }

    pendingSettings.actualFrequency = finalActualFrequency;
    appliedHardwareSettings.centerFrequency = targetFrequency;
    appliedHardwareSettings.actualFrequency = finalActualFrequency;
    publishSettingsToGlobals();
    qDebug() << "[FobosLifecycle] Agile startup frequency kick done";
    return true;
}

void YourClassName::abandonFobosSessionWithoutClose(const char *reason) {
    qDebug() << "[FobosLifecycle] abandoning Fobos session without USB close"
             << "reason" << (reason ? reason : "")
             << "device" << activeFobosDevice()
             << "openedDeviceIndex" << openedDeviceIndex
             << "openedNativeDeviceIndex" << openedNativeDeviceIndex
             << "apiKind" << fobosApiDisplayName(openedDeviceApiKind);
    device = nullptr;
    agileDevice = nullptr;
    agileScanRunning = false;
    activeAgileScanFrequencies.clear();
    scanVisualAssembler.reset();
    openedDeviceIndex = -1;
    openedNativeDeviceIndex = -1;
    appliedSampleRate = 0.0;
    appliedHardwareSettings = RadioSettings{};
    hardwareSettingsApplied = false;
    sampleRateReopenRequired = false;
    fobosCloseKnownUnsafe = false;
    closeFobosSessionAfterStop = false;
    resetStandardScanState(true);
    resetListeningScanState();
    activeFobosApiKind = FobosApiKind::Standard;
    openedDeviceApiKind = FobosApiKind::Standard;
}

bool YourClassName::applyFobosSettings(bool forceFrequencyApply) {
    if (!hasActiveFobosDevice()) {
        qDebug() << "Cannot apply Fobos settings without an active device.";
        return false;
    }

    normalizeTuning(pendingSettings);
    publishSettingsToGlobals();
    const bool firstApply = !hardwareSettingsApplied;
    auto changedDouble = [](double a, double b) {
        return std::abs(a - b) > 0.5;
    };

    qDebug() << "[FobosLifecycle] applyFobosSettings enter"
             << "device" << activeFobosDevice()
             << "apiKind" << fobosApiKindName(activeFobosApiKind)
             << "firstApply" << firstApply
             << "clock" << pendingSettings.clockSource
             << "inputMode" << pendingSettings.inputMode
             << "sampleRate" << pendingSettings.sampleRate
             << "centerFrequency" << pendingSettings.centerFrequency
             << "forceFrequencyApply" << forceFrequencyApply
             << "lna" << pendingSettings.lnaGain
             << "vga" << pendingSettings.vgaGain
             << "gpo" << pendingSettings.gpoValue;

    int result = FOBOS_ERR_OK;
    const bool agileNormalRfMode =
        activeFobosApiKind == FobosApiKind::Agile &&
        pendingSettings.inputMode == INPUT_RF &&
        !agileScanEnabled;
    const bool skipDefaultAgileClockSource =
        agileNormalRfMode &&
        pendingSettings.clockSource == 0 &&
        (firstApply || appliedHardwareSettings.clockSource == pendingSettings.clockSource);
    if (!skipDefaultAgileClockSource &&
        (firstApply || appliedHardwareSettings.clockSource != pendingSettings.clockSource)) {
        qDebug() << "[FobosLifecycle] set clock source begin";
        result = setActiveClockSourceSafely(pendingSettings.clockSource);
        qDebug() << "[FobosLifecycle] set clock source end" << "result" << result;
        if (result != FOBOS_ERR_OK) {
            qDebug() << "Failed to set clock source, error code:" << result;
            return false;
        }
    } else if (skipDefaultAgileClockSource) {
        qDebug() << "[FobosLifecycle] Agile RF default clock source skipped to match uSDR startup";
    } else {
        qDebug() << "[FobosLifecycle] fobos_rx_set_clk_source skipped unchanged";
    }

    const int libfobosMode = (pendingSettings.inputMode == INPUT_RF) ? 0 : 1;
    const bool skipDefaultAgileDirectSampling =
        agileNormalRfMode &&
        libfobosMode == 0 &&
        (firstApply || appliedHardwareSettings.inputMode == pendingSettings.inputMode);
    if (!skipDefaultAgileDirectSampling &&
        (firstApply || appliedHardwareSettings.inputMode != pendingSettings.inputMode)) {
        qDebug() << "[FobosLifecycle] set direct sampling begin" << "libfobosMode" << libfobosMode;
        result = setActiveDirectSamplingSafely(static_cast<unsigned int>(libfobosMode));
        qDebug() << "[FobosLifecycle] set direct sampling end" << "result" << result;
        if (result != FOBOS_ERR_OK) {
            qDebug() << "Failed to set direct sampling mode, error code:" << result;
            return false;
        }
    } else if (skipDefaultAgileDirectSampling) {
        qDebug() << "[FobosLifecycle] Agile RF default direct sampling skipped to match uSDR startup";
    } else {
        qDebug() << "[FobosLifecycle] fobos_rx_set_direct_sampling skipped unchanged";
    }

    if (pendingSettings.sampleRate > 0.0 &&
        (firstApply || changedDouble(appliedHardwareSettings.sampleRate, pendingSettings.sampleRate))) {
        qDebug() << "[FobosLifecycle] set sample rate begin" << "requested" << pendingSettings.sampleRate;
        result = setActiveSampleRateSafely(pendingSettings.sampleRate, &globalSampleRate);
        qDebug() << "[FobosLifecycle] set sample rate end"
                 << "result" << result
                 << "actual" << globalSampleRate;
        if (result != FOBOS_ERR_OK) {
            qDebug() << "Failed to set sample rate, error code:" << result;
            globalSampleRate = pendingSettings.sampleRate;
            sampleRateReopenRequired = true;
            hardwareSettingsApplied = false;
            return false;
        }
        pendingSettings.sampleRate = globalSampleRate;
        appliedSampleRate = globalSampleRate;
    } else {
        qDebug() << "[FobosLifecycle] fobos_rx_set_samplerate skipped unchanged";
        if (appliedSampleRate > 0.0) {
            globalSampleRate = appliedSampleRate;
            pendingSettings.sampleRate = appliedSampleRate;
        }
    }

    if (activeFobosApiKind == FobosApiKind::Agile &&
        pendingSettings.inputMode == INPUT_RF &&
        !agileScanEnabled &&
        !stopAgileScanForNormalRf("apply settings")) {
        qDebug() << "[FobosLifecycle] failed to leave Agile scan mode before normal RF tuning";
        return false;
    }

    if (pendingSettings.inputMode == INPUT_RF) {
        if (firstApply ||
            forceFrequencyApply ||
            appliedHardwareSettings.inputMode != pendingSettings.inputMode ||
            changedDouble(appliedHardwareSettings.centerFrequency, pendingSettings.centerFrequency)) {
            qDebug() << "[FobosLifecycle] set frequency begin" << "requested" << pendingSettings.centerFrequency;
            result = setCalibratedActiveFrequencySafely(pendingSettings.centerFrequency, &actualFrequency);
            qDebug() << "[FobosLifecycle] set frequency end"
                     << "result" << result
                     << "actual" << actualFrequency;
            if (result != FOBOS_ERR_OK) {
                qDebug() << "Failed to set frequency, error code:" << result;
            }
            pendingSettings.actualFrequency = actualFrequency;
        } else {
            qDebug() << "[FobosLifecycle] fobos_rx_set_frequency skipped unchanged";
            actualFrequency = pendingSettings.actualFrequency;
        }
    } else {
        pendingSettings.centerFrequency = 0.0;
        actualFrequency = 0.0;
        pendingSettings.actualFrequency = 0.0;
    }

    if (activeFobosApiKind == FobosApiKind::Agile &&
        pendingSettings.inputMode == INPUT_RF &&
        !agileScanEnabled) {
        const bool applyAutoBandwidth =
            firstApply ||
            appliedHardwareSettings.inputMode != pendingSettings.inputMode ||
            changedDouble(appliedHardwareSettings.sampleRate, pendingSettings.sampleRate);
        if (applyAutoBandwidth) {
            const double autoBandwidthRatio = agileRfAutoBandwidthRatio(pendingSettings.sampleRate);
            qDebug() << "[FobosLifecycle] set Agile auto bandwidth begin" << autoBandwidthRatio;
            const int bandwidthResult = setFobosAgileAutoBandwidthSafely(agileDevice, autoBandwidthRatio);
            qDebug() << "[FobosLifecycle] set Agile auto bandwidth end" << "result" << bandwidthResult;
            if (bandwidthResult != FOBOS_ERR_OK) {
                qDebug() << "Failed to set Agile auto bandwidth, error code:" << bandwidthResult;
            }
        } else {
            qDebug() << "[FobosLifecycle] Agile auto bandwidth skipped unchanged";
        }

    }

    if (firstApply || appliedHardwareSettings.lnaGain != pendingSettings.lnaGain) {
        qDebug() << "[FobosLifecycle] set LNA gain begin" << pendingSettings.lnaGain;
        result = setActiveLnaGainSafely(static_cast<unsigned int>(pendingSettings.lnaGain));
        qDebug() << "[FobosLifecycle] set LNA gain end" << "result" << result;
        if (result != FOBOS_ERR_OK) {
            qDebug() << "Failed to set LNA gain, error code:" << result;
        }
    } else {
        qDebug() << "[FobosLifecycle] fobos_rx_set_lna_gain skipped unchanged";
    }

    if (firstApply || appliedHardwareSettings.vgaGain != pendingSettings.vgaGain) {
        qDebug() << "[FobosLifecycle] set VGA gain begin" << pendingSettings.vgaGain;
        result = setActiveVgaGainSafely(static_cast<unsigned int>(pendingSettings.vgaGain));
        qDebug() << "[FobosLifecycle] set VGA gain end" << "result" << result;
        if (result != FOBOS_ERR_OK) {
            qDebug() << "Failed to set VGA gain, error code:" << result;
        }
    } else {
        qDebug() << "[FobosLifecycle] fobos_rx_set_vga_gain skipped unchanged";
    }

    const bool skipDefaultAgileGpo =
        agileNormalRfMode &&
        pendingSettings.gpoValue == 0 &&
        (firstApply || appliedHardwareSettings.gpoValue == pendingSettings.gpoValue);
    if (!skipDefaultAgileGpo &&
        (firstApply || appliedHardwareSettings.gpoValue != pendingSettings.gpoValue)) {
        qDebug() << "[FobosLifecycle] set user GPO begin" << pendingSettings.gpoValue;
        const int gpoResult = setActiveGpoSafely(pendingSettings.gpoValue);
        qDebug() << "[FobosLifecycle] set user GPO end" << "result" << gpoResult;
        if (gpoResult != FOBOS_ERR_OK) {
            qDebug() << "Failed to set user GPO, error code:" << gpoResult;
        }
    } else if (skipDefaultAgileGpo) {
        qDebug() << "[FobosLifecycle] Agile RF default user GPO skipped to match uSDR startup";
    } else {
        qDebug() << "[FobosLifecycle] fobos_rx_set_user_gpo skipped unchanged";
    }

    if (!applyAgileScanSettings(false)) {
        qDebug() << "[FobosLifecycle] Agile scan settings failed";
        return false;
    }
    if (!applyStandardScanSettings(false)) {
        qDebug() << "[FobosLifecycle] Standard scan settings failed";
        return false;
    }
    if (!applyListeningScanSettings(false)) {
        qDebug() << "[FobosLifecycle] Listening scan settings failed";
    }

    appliedHardwareSettings = pendingSettings;
    appliedHardwareSettings.sampleRate = globalSampleRate;
    appliedHardwareSettings.actualFrequency = pendingSettings.actualFrequency;
    hardwareSettingsApplied = true;
    publishSettingsToGlobals();
    if (frequencyControl) {
        QSignalBlocker blocker(frequencyControl);
        frequencyControl->setValueHz(pendingSettings.centerFrequency);
    }
    if (listeningFrequencyControl) {
        QSignalBlocker blocker(listeningFrequencyControl);
        listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
    }
    settingRange();
    qDebug() << "[FobosLifecycle] applyFobosSettings exit";
    return true;
}

void YourClassName::startFobosProcessing() {
    if (playbackManager && playbackManager->isPlaying()) {
        stopPlayback();
    }

    if (isChannelIqRecordingActive() &&
        networkMode != NetworkMode::Disabled &&
        isFullIqProcessingMode()) {
        stopRecording(false);
        updateRecordingStatus(QStringLiteral("Recording stopped: Channel IQ cannot run during Full IQ streaming"));
    }

    if (isNetworkClientMode()) {
        if (sendRemoteControlCommand("start")) {
            runState = RadioRunState::Running;
            updateUiForRunState();
            if (isClientIqProcessingMode()) {
                startNetworkClientProcessing();
            } else if (remoteAudioPlayer) {
                remoteAudioPlayer->stop();
            }
        }
        return;
    }

    const bool watchdogRestart = automaticStreamRestart;
    automaticStreamRestart = false;
    if (!watchdogRestart) {
        streamStartupRetryCount = 0;
        restartAfterStartupWatchdog = false;
    }
    qDebug() << "[FobosLifecycle] Start requested"
             << "state" << runStateName(runState)
             << "deviceOpened" << deviceOpened
             << "processorRunning" << (processor && processor->isRunning())
             << "device" << activeFobosDevice()
             << "apiKind" << fobosApiKindName(activeFobosApiKind)
             << "appliedSampleRate" << appliedSampleRate
             << "pendingSampleRate" << pendingSettings.sampleRate
             << "sampleRateReopenRequired" << sampleRateReopenRequired
             << "fobosCloseKnownUnsafe" << fobosCloseKnownUnsafe
             << "watchdogRestart" << watchdogRestart
             << "startupRetryCount" << streamStartupRetryCount;
    logMemorySnapshot("before start");
    if (!isIdle() || deviceOpened || (processor && processor->isRunning())) {
        if (isIdle() && !deviceOpened && processor && processor->isRunning()) {
            qDebug() << "[FobosLifecycle] stale DataProcessor detected while UI is idle; stopping before start";
            processor->requestStop();
            if (!processor->wait(1500)) {
                qDebug() << "[FobosLifecycle] stale DataProcessor wait timed out; forcing stop before start";
                processor->forceStop(1000);
            }
            processor->finalizeStopped();
            if (processor->isRunning()) {
                qDebug() << "[FobosLifecycle] stale DataProcessor is still running; start aborted";
                return;
            }
            qDebug() << "[FobosLifecycle] stale DataProcessor stopped; continuing start";
        } else {
            qDebug() << "Warning: Processor is already running!";
            return;
        }
    }

    runState = RadioRunState::Starting;
    qDebug() << "[FobosLifecycle] state changed" << runStateName(runState);
    updateUiForRunState();
    refreshSettingsFromUi();
    if (offsetDmrCenterFromListening(pendingSettings)) {
        qDebug() << "[FobosLifecycle] DMR start offset RF center from listening frequency"
                 << "center" << pendingSettings.centerFrequency
                 << "listening" << pendingSettings.listeningFrequency;
        if (frequencyControl) {
            QSignalBlocker blocker(frequencyControl);
            frequencyControl->setValueHz(pendingSettings.centerFrequency);
        }
        if (listeningFrequencyControl) {
            QSignalBlocker blocker(listeningFrequencyControl);
            listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
        }
        settingRange();
    }
    const bool rtlTcpSelected = isRtlTcpSelected();
    const bool rtlSdrNativeSelected = isRtlSdrNativeSelected();
    const bool soapySdrSelected = isSoapySdrSelected();
    const bool bladeRfNativeSelected = isBladeRfNativeSelected();
    const bool hackRfNativeSelected = isHackRfNativeSelected();
    if (bladeRfNativeSelected && !BLADERF_NATIVE_BACKEND_ENABLED) {
        qWarning() << "[bladeRF] start rejected: native backend is disabled until physical hardware validation";
        clearLiveSpectrumSnapshot();
        runState = RadioRunState::Idle;
        updateUiForRunState();
        return;
    }
    const bool rtlBackendSelected = rtlTcpSelected || rtlSdrNativeSelected;
    const bool externalBackendSelected = rtlBackendSelected || soapySdrSelected ||
                                         bladeRfNativeSelected || hackRfNativeSelected;
    if (rtlBackendSelected) {
        normalizeRtlSdrSettings();
    }
    if (externalBackendSelected) {
        if (hasActiveFobosDevice()) {
            qDebug() << "[ReceiverBackend] closing idle Fobos USB session before external backend start"
                     << "device" << activeFobosDevice()
                     << "apiKind" << fobosApiKindName(activeFobosApiKind);
            closeFobosSession(false);
            QThread::msleep(250);
        }
    }
    const bool sampleRateDiffersFromOpenSession =
        !externalBackendSelected &&
        hasActiveFobosDevice() && appliedSampleRate > 0.0 &&
        std::abs(appliedSampleRate - pendingSettings.sampleRate) > 0.5;
    if (sampleRateDiffersFromOpenSession) {
        sampleRateReopenRequired = true;
        qDebug() << "[FobosLifecycle] sample rate differs from previous run; reopening Fobos session before applying"
                 << "appliedSampleRate" << appliedSampleRate
                 << "pendingSampleRate" << pendingSettings.sampleRate;
    }
    publishSettingsToGlobals();

    const bool agileNormalRfOpenSession =
        hasActiveFobosDevice() &&
        activeFobosApiKind == FobosApiKind::Agile &&
        pendingSettings.inputMode == INPUT_RF &&
        !agileScanEnabled;
    if (agileNormalRfOpenSession && sampleRateReopenRequired) {
        qDebug() << "[FobosLifecycle] Agile RF sample-rate difference will be applied live; keeping session open"
                 << "appliedSampleRate" << appliedSampleRate
                 << "pendingSampleRate" << pendingSettings.sampleRate;
        sampleRateReopenRequired = false;
    }

    if (hasActiveFobosDevice() && sampleRateReopenRequired) {
        if (fobosCloseKnownUnsafe) {
            qDebug() << "[FobosLifecycle] previous Fobos close was unsafe; abandoning stale session pointer before reopen"
                     << activeFobosDevice();
            device = nullptr;
            agileDevice = nullptr;
            agileScanRunning = false;
            activeAgileScanFrequencies.clear();
            activeFobosApiKind = FobosApiKind::Standard;
            openedDeviceIndex = -1;
            openedNativeDeviceIndex = -1;
            appliedSampleRate = 0.0;
            appliedHardwareSettings = RadioSettings{};
            hardwareSettingsApplied = false;
            sampleRateReopenRequired = false;
            fobosCloseKnownUnsafe = false;
        }
    }

    if (hasActiveFobosDevice() && sampleRateReopenRequired) {
        if (processor && !processor->isRunning()) {
            processor->finalizeStopped();
        }
        qDebug() << "[FobosLifecycle] closing Fobos session before sample-rate change"
                 << "device" << activeFobosDevice()
                 << "apiKind" << fobosApiKindName(activeFobosApiKind)
                 << "appliedSampleRate" << appliedSampleRate
                 << "pendingSampleRate" << pendingSettings.sampleRate;
        if (!closeFobosSession(false)) {
            qDebug() << "[FobosLifecycle] Fobos session close returned an error before sample-rate change; trying a fresh open anyway";
        }
        qDebug() << "[FobosLifecycle] Fobos session closed for sample-rate change; waiting before reopen";
        QThread::msleep(500);
    }

    if (externalBackendSelected) {
        qDebug() << "[ReceiverBackend] starting without Fobos USB session"
                 << "native" << rtlSdrNativeSelected
                 << "tcp" << rtlTcpSelected
                 << "soapy" << soapySdrSelected
                 << "bladerf" << bladeRfNativeSelected
                 << "hackrf" << hackRfNativeSelected
                 << "host" << RTL_TCP_DEFAULT_HOST
                 << "port" << RTL_TCP_DEFAULT_PORT
                 << "frequency" << pendingSettings.centerFrequency
                 << "sampleRate" << pendingSettings.sampleRate;
        appliedSampleRate = pendingSettings.sampleRate;
        appliedHardwareSettings = pendingSettings;
        hardwareSettingsApplied = true;
        sampleRateReopenRequired = false;
        if (!applyStandardScanSettings(false)) {
            qDebug() << "[ReceiverBackend] Standard scan settings failed before external backend start";
            clearLiveSpectrumSnapshot();
            runState = RadioRunState::Idle;
            updateUiForRunState();
            return;
        }
    } else {
        bool fobosSessionAlreadyPrepared =
            hasActiveFobosDevice() &&
            hardwareSettingsApplied &&
            appliedHardwareSettings.inputMode == pendingSettings.inputMode &&
            appliedHardwareSettings.clockSource == pendingSettings.clockSource &&
            appliedHardwareSettings.lnaGain == pendingSettings.lnaGain &&
            appliedHardwareSettings.vgaGain == pendingSettings.vgaGain &&
            appliedHardwareSettings.gpoValue == pendingSettings.gpoValue &&
            std::abs(appliedHardwareSettings.centerFrequency - pendingSettings.centerFrequency) <= 0.5 &&
            std::abs(appliedHardwareSettings.sampleRate - pendingSettings.sampleRate) <= 0.5;

        if (!fobosSessionAlreadyPrepared && hasActiveFobosDevice()) {
            qDebug() << "[FobosLifecycle] prepared Fobos session is stale; applying pending hardware settings before reader start"
                     << "appliedInputMode" << appliedHardwareSettings.inputMode
                     << "pendingInputMode" << pendingSettings.inputMode
                     << "appliedSampleRate" << appliedSampleRate
                     << "pendingSampleRate" << pendingSettings.sampleRate
                     << "appliedCenter" << appliedHardwareSettings.centerFrequency
                     << "pendingCenter" << pendingSettings.centerFrequency;
            const bool forceFrequencyApply = pendingSettings.inputMode == INPUT_RF;
            if (!applyFobosSettings(forceFrequencyApply)) {
                qDebug() << "[FobosLifecycle] Start failed while applying stale prepared Fobos settings";
                clearLiveSpectrumSnapshot();
                runState = RadioRunState::Idle;
                updateUiForRunState();
                return;
            }
            fobosSessionAlreadyPrepared =
                hardwareSettingsApplied &&
                appliedHardwareSettings.inputMode == pendingSettings.inputMode &&
                appliedHardwareSettings.clockSource == pendingSettings.clockSource &&
                appliedHardwareSettings.lnaGain == pendingSettings.lnaGain &&
                appliedHardwareSettings.vgaGain == pendingSettings.vgaGain &&
                appliedHardwareSettings.gpoValue == pendingSettings.gpoValue &&
                std::abs(appliedHardwareSettings.centerFrequency - pendingSettings.centerFrequency) <= 0.5 &&
                std::abs(appliedHardwareSettings.sampleRate - pendingSettings.sampleRate) <= 0.5;
        }

        if (fobosSessionAlreadyPrepared) {
            qDebug() << "[FobosLifecycle] prepared Fobos session reused; Start will only launch reader"
                     << "device" << activeFobosDevice()
                     << "apiKind" << fobosApiKindName(activeFobosApiKind)
                     << "sampleRate" << appliedSampleRate
                     << "centerFrequency" << pendingSettings.centerFrequency;
        } else {
            qDebug() << "[FobosLifecycle] Start aborted because prepared Fobos session is missing or stale; Start does not apply hardware settings"
                     << "hasDevice" << hasActiveFobosDevice()
                     << "hardwareSettingsApplied" << hardwareSettingsApplied
                     << "sampleRateReopenRequired" << sampleRateReopenRequired
                     << "appliedSampleRate" << appliedSampleRate
                     << "pendingSampleRate" << pendingSettings.sampleRate
                     << "appliedCenter" << appliedHardwareSettings.centerFrequency
                     << "pendingCenter" << pendingSettings.centerFrequency;
            clearLiveSpectrumSnapshot();
            runState = RadioRunState::Idle;
            updateUiForRunState();
            return;
        }
    }
    clearLiveSpectrumSnapshot(false);
    digitalDecoderGeneration.fetch_add(1, std::memory_order_relaxed);
    pendingDmrDecoderPcm.clear();
    pendingDmrDecoderSampleRate = 48000;
    droppedDigitalDecoderFramesSinceLog.store(0);
    if (digitalDecoder) {
        QMetaObject::invokeMethod(digitalDecoder,
                                  [decoder = digitalDecoder]() {
                                      decoder->reset();
                                  },
                                  Qt::QueuedConnection);
    }
    spectrumDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 8 : 0;
    spectrumTuningDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 8 : 0;
    updateSpectrumTimerInterval();
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[FobosLifecycle] clearing IQ buffer before reader start; preserving visual history";
    }

    deviceOpened = true;
    const bool serverIqStreaming = networkMode == NetworkMode::Server && isClientIqProcessingMode();
    const bool serverFullIqStreaming = networkMode == NetworkMode::Server && isFullIqProcessingMode();
    const bool serverChannelIqStreaming = networkMode == NetworkMode::Server && isChannelIqProcessingMode();
    const bool channelIqRecording = isChannelIqRecordingActive();
    const bool eventIq = spectrumEventIqProducerNeeded();
    const bool suppressServerLocalOutput =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();
    const bool serverAudioStreamingForFullIq = serverFullIqStreaming && pendingSettings.audioEnabled;
    const bool serverLocalAudioEnabled =
        pendingSettings.audioEnabled &&
        (!serverIqStreaming || serverAudioStreamingForFullIq);
    const bool queueAudioBlocks =
        (!serverIqStreaming || serverAudioStreamingForFullIq);
    const bool publishIqSnapshot = true;
    if (audioProcessor) {
        audioProcessor->setLocalPlaybackEnabled(!suppressServerLocalOutput);
    }
    const bool emitProcessorIqFrames = serverIqStreaming ||
        (!dmrAudioIqProducerNeeded() && (channelIqRecording || eventIq));
    updateIqFrameProducerSettings();
    qDebug() << "[FobosLifecycle] starting DataProcessor"
             << "backend" << (hackRfNativeSelected ? "hackrf-native" :
                              (bladeRfNativeSelected ? "bladerf-native" :
                               (soapySdrSelected ? "soapy-sdr" :
                               (rtlSdrNativeSelected ? "rtl-sdr-native" :
                                (rtlTcpSelected ? "rtl_tcp" : fobosApiKindName(activeFobosApiKind))))))
             << "device" << activeFobosDevice()
             << "sampleRate" << pendingSettings.sampleRate
             << "syncEnabled" << pendingSettings.syncEnabled
             << "queueAudioBlocks" << queueAudioBlocks
             << "publishIqSnapshot" << publishIqSnapshot
             << "serverIqStreaming" << serverIqStreaming
             << "serverChannelIqStreaming" << serverChannelIqStreaming
             << "channelIqRecording" << channelIqRecording
             << "eventIq" << eventIq
             << "eventMode" << spectrumEventCaptureMode;
    if (rtlSdrNativeSelected) {
        processor->startProcessing(makeRtlSdrNativeStreamDescriptor(queueAudioBlocks,
                                                                    publishIqSnapshot,
                                                                    emitProcessorIqFrames));
    } else if (rtlTcpSelected) {
        processor->startProcessing(makeRtlTcpStreamDescriptor(queueAudioBlocks,
                                                              publishIqSnapshot,
                                                              emitProcessorIqFrames));
    } else if (soapySdrSelected) {
        processor->startProcessing(makeSoapySdrStreamDescriptor(queueAudioBlocks,
                                                                publishIqSnapshot,
                                                                emitProcessorIqFrames));
    } else if (bladeRfNativeSelected) {
        processor->startProcessing(makeBladeRfNativeStreamDescriptor(queueAudioBlocks,
                                                                     publishIqSnapshot,
                                                                     emitProcessorIqFrames));
    } else if (hackRfNativeSelected) {
        processor->startProcessing(makeHackRfNativeStreamDescriptor(queueAudioBlocks,
                                                                    publishIqSnapshot,
                                                                    emitProcessorIqFrames));
    } else {
        processor->startProcessing(makeFobosStreamDescriptor(activeFobosDevice(),
                                                             activeFobosApiKind,
                                                             pendingSettings.syncEnabled,
                                                             pendingSettings.sampleRate,
                                                             pendingSettings.centerFrequency,
                                                             queueAudioBlocks,
                                                             publishIqSnapshot,
                                                             emitProcessorIqFrames,
                                                             agileScanEnabled &&
                                                                 !standardScanEnabled &&
                                                                 activeFobosApiKind == FobosApiKind::Agile,
                                                             agileScanEnabled &&
                                                                     !standardScanEnabled &&
                                                                     activeFobosApiKind == FobosApiKind::Agile
                                                                 ? activeAgileScanFrequencies
                                                                 : QVector<double>(),
                                                             effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency)));
    }
    if (!externalBackendSelected &&
        activeFobosApiKind == FobosApiKind::Agile &&
        pendingSettings.inputMode == INPUT_RF &&
        !agileScanEnabled) {
        liveRetuneSettleDurationMs = agileRfLiveSettleMs(pendingSettings.sampleRate, false);
        clearLiveSpectrumSnapshot(false);
        liveRetuneSettleTimer.start();
        qDebug() << "[FobosLifecycle] Agile start settle armed"
                 << "settleMs" << liveRetuneSettleDurationMs;
    }
    pendingAudioStartAfterStreamReady = serverLocalAudioEnabled;
    streamStartCallbackCount = processor ? processor->callbackCount() : 0;
    streamStartElapsedTimer.restart();
    if (streamWatchdogTimer) {
        qDebug() << "[FobosLifecycle] stream startup watchdog armed"
                 << "callbackCount" << streamStartCallbackCount
                 << "retryCount" << streamStartupRetryCount;
        streamWatchdogTimer->start();
    }
    runState = RadioRunState::Running;
    qDebug() << "[FobosLifecycle] state changed" << runStateName(runState)
             << "processorRunning" << (processor && processor->isRunning());
    if (externalBackendSelected && standardScanRunning) {
        standardScanDwellTimer.restart();
    }
    updateUiForRunState();
    qDebug() << "Fobos fft Started";

    if (serverLocalAudioEnabled) {
        qDebug() << "[FobosLifecycle] audio start deferred until stream produces IQ callbacks";
    } else if (serverChannelIqStreaming) {
        qDebug() << "[NetworkIQ] server is streaming channelized IQ frames; local server audio is disabled";
    } else if (serverFullIqStreaming) {
        qDebug() << "[NetworkIQ] server is streaming full IQ frames; local server audio is disabled";
    }

    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[FobosLifecycle] starting spectrum timer";
    }
    if (serverFullIqStreaming && suppressServerLocalOutput) {
        qDebug() << "[NetworkIQ] server local spectrum timer disabled in full-IQ processing mode";
    } else {
        updateTimer->start();
    }
    applyServerLocalOutputPolicy();
    logMemorySnapshot("after start");
    qDebug() << "[FobosLifecycle] Start sequence complete";
}

void YourClassName::recreateDataProcessor() {
    qDebug() << "[FobosLifecycle] recreateDataProcessor enter"
             << "processor" << processor
             << "processorRunning" << (processor && processor->isRunning());
    if (processor) {
        delete processor;
    }
    processor = new DataProcessor(this);
    connectDataProcessorSignals();
    if (zoomSpectrumDialog && zoomSpectrumDialog->isVisible()) {
        zoomSpectrumDialog->setSource(processor->zoomSpectrumProcessor(),
                                      zoomSpectrumDialog->selectedLowHz(),
                                      zoomSpectrumDialog->selectedHighHz());
    }
    qDebug() << "[FobosLifecycle] recreateDataProcessor exit" << "processor" << processor;
}

void YourClassName::openZoomSpectrum() {
    double selectedLowHz = 0.0;
    double selectedHighHz = 0.0;
    if (!graphWidget || !graphWidget->bandwidthSelection(selectedLowHz, selectedHighHz)) {
        QMessageBox::information(this,
                                 QStringLiteral("Zoom spectrum"),
                                 QString::fromUtf8(u8"Спочатку затисніть ліву кнопку миші та виділіть потрібну смугу на основному спектрі."));
        return;
    }
    if (!processor) {
        QMessageBox::warning(this,
                             QStringLiteral("Zoom spectrum"),
                             QString::fromUtf8(u8"IQ-процесор ще не створений."));
        return;
    }
    if (!zoomSpectrumDialog) {
        zoomSpectrumDialog = new ZoomSpectrumDialog(this);
    }
    zoomSpectrumDialog->show();
    zoomSpectrumDialog->raise();
    zoomSpectrumDialog->activateWindow();
    zoomSpectrumDialog->setSource(processor->zoomSpectrumProcessor(),
                                  selectedLowHz,
                                  selectedHighHz);
}

void YourClassName::openZoomDensity() {
    double selectedLowHz = 0.0;
    double selectedHighHz = 0.0;
    if (!graphWidget || !graphWidget->bandwidthSelection(selectedLowHz, selectedHighHz)) {
        QMessageBox::information(this,
                                 uiText(QStringLiteral("zoom_density_title"),
                                        QStringLiteral("Selected signal density")),
                                 uiText(QStringLiteral("zoom_density_select_range"),
                                        QStringLiteral("First drag over the required band on the main spectrum.")));
        return;
    }
    if (!zoomDensityDialog) {
        zoomDensityDialog = new ZoomDensityDialog(
            [this](const QString &key, const QString &fallback) {
                return uiText(key, fallback);
            },
            this);
    }
    zoomDensityDialog->setRange(selectedLowHz, selectedHighHz);
    zoomDensityDialog->show();
    zoomDensityDialog->raise();
    zoomDensityDialog->activateWindow();
}

void YourClassName::openTransmitDialog() {
    if (!transmitDialog) {
        transmitDialog = new TransmitDialog(uiLanguage, this);
    }
    transmitDialog->setHackRfReceiverUseChecker([this](int deviceIndex) {
        return runState != RadioRunState::Idle &&
               isHackRfNativeSelected() &&
               selectedHackRfNativeIndex() == deviceIndex;
    });
    transmitDialog->setLanguage(uiLanguage);
    transmitDialog->show();
    transmitDialog->raise();
    transmitDialog->activateWindow();
}

void YourClassName::finishFobosStop(bool forcedRecovery) {
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[FobosLifecycle] finishFobosStop enter"
                 << "forcedRecovery" << forcedRecovery
                 << "processorRunning" << (processor && processor->isRunning())
                 << "device" << activeFobosDevice()
                 << "apiKind" << fobosApiKindName(activeFobosApiKind);
    }

    if (stopPollTimer) {
        stopPollTimer->stop();
    }
    if (streamWatchdogTimer) {
        streamWatchdogTimer->stop();
    }
    resetListeningScanState();

    if (processor) {
        processor->finalizeStopped();
    }

    if (closeShutdownInProgress) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] finish stop for pending window close; closing USB session after reader stop";
        }
        if (hasActiveFobosDevice()) {
            closeFobosSession(false);
        }
        if (clearSpectrumAfterStop) {
            clearLiveSpectrumSnapshot();
            clearSpectrumAfterStop = false;
        }
        deviceOpened = false;
        runState = RadioRunState::Idle;
        updateUiForRunState();
        savePersistentSettings();
        closeShutdownFinalized = true;
        QTimer::singleShot(0, this, &QWidget::close);
        return;
    }

    bool closeSucceeded = true;
    if (forcedRecovery) {
        qDebug() << "[FobosLifecycle] forced stop recovery: abandoning Fobos session without close and recreating DataProcessor"
                 << "device" << activeFobosDevice()
                 << "apiKind" << fobosApiKindName(activeFobosApiKind);
        abandonFobosSessionWithoutClose("forced stop recovery");
        if (processor && !processor->isRunning()) {
            recreateDataProcessor();
        }
    } else if (hasActiveFobosDevice()) {
        if (closeFobosSessionAfterStop) {
            qDebug() << "[FobosLifecycle] stop recovery: closing Fobos session after reader failure"
                     << activeFobosDevice()
                     << "apiKind" << fobosApiKindName(activeFobosApiKind);
            closeSucceeded = closeFobosSession(false);
            closeFobosSessionAfterStop = false;
            if (processor && !processor->isRunning()) {
                recreateDataProcessor();
            }
        } else if (agileScanRunning && agileDevice) {
            if (fobosVerboseLoggingEnabled()) {
                qDebug() << "[FobosLifecycle] clean stop: stopping Agile scan but keeping device session open";
            }
            const int stopScanResult = stopFobosAgileScanSafely(agileDevice);
            if (stopScanResult != 0 || fobosVerboseLoggingEnabled()) {
                qDebug() << "[FobosLifecycle] fobos_sdr_stop_scan end"
                         << "result" << stopScanResult;
            }
            agileScanRunning = false;
            activeAgileScanFrequencies.clear();
            scanVisualAssembler.reset();
        }
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] clean stop: keeping idle Fobos session open to avoid UI-thread USB close"
                     << activeFobosDevice()
                     << "apiKind" << fobosApiKindName(activeFobosApiKind)
                     << "clearSpectrumAfterStop" << clearSpectrumAfterStop
                     << "sampleRateReopenRequired" << sampleRateReopenRequired;
        }
        if (processor && !processor->isRunning()) {
            recreateDataProcessor();
        }
    }

    if (clearSpectrumAfterStop) {
        clearLiveSpectrumSnapshot();
        clearSpectrumAfterStop = false;
    }

    deviceOpened = false;
    runState = RadioRunState::Idle;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[FobosLifecycle] state changed" << runStateName(runState);
    }
    updateUiForRunState();
    savePersistentSettings();
    logMemorySnapshot("after stop");
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "Stop requested: complete.";
    }

    if (restartAfterStartupWatchdog && !forcedRecovery && closeSucceeded) {
        restartAfterStartupWatchdog = false;
        qDebug() << "[FobosLifecycle] scheduling automatic prepare and restart after stream startup watchdog"
                 << "retryCount" << streamStartupRetryCount;
        QTimer::singleShot(350, this, [this]() {
            if (!prepareFobosSessionFromSettings(QStringLiteral("reader failure retry"))) {
                qDebug() << "[FobosLifecycle] automatic restart skipped because Fobos session could not be prepared";
                automaticStreamRestart = false;
                return;
            }
            automaticStreamRestart = true;
            startFobosProcessing();
        });
    } else if (restartAfterStartupWatchdog && !closeSucceeded) {
        restartAfterStartupWatchdog = false;
        automaticStreamRestart = false;
        qDebug() << "[FobosLifecycle] automatic restart skipped because device close failed";
    }
}

void YourClassName::checkStreamStartup() {
    if (runState != RadioRunState::Running || !processor || !processor->isRunning()) {
        if (streamWatchdogTimer) {
            streamWatchdogTimer->stop();
        }
        return;
    }

    const uint64_t callbackCount = processor->callbackCount();
    if (callbackCount > streamStartCallbackCount) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] stream startup watchdog satisfied"
                     << "callbackCount" << callbackCount
                     << "elapsedMs" << streamStartElapsedTimer.elapsed();
        }
        if (streamWatchdogTimer) {
            streamWatchdogTimer->stop();
        }
        if (pendingAudioStartAfterStreamReady) {
            pendingAudioStartAfterStreamReady = false;
            if (deviceOpened && processor && processor->isRunning()) {
                qDebug() << "[FobosLifecycle] starting deferred audio after IQ stream became active";
                audioProcessor->startDemodulation();
                qDebug() << "Fobos audio Started";
            } else {
                qDebug() << "[FobosLifecycle] deferred audio start skipped because SDR is no longer running";
            }
        }
        return;
    }

    const qint64 elapsedMs = streamStartElapsedTimer.isValid() ? streamStartElapsedTimer.elapsed() : 0;
    if (elapsedMs < 1800) {
        return;
    }

    qDebug() << "[FobosLifecycle] stream startup watchdog: no IQ callbacks after start"
             << "elapsedMs" << elapsedMs
             << "callbackCount" << callbackCount
             << "retryCount" << streamStartupRetryCount
             << "device" << activeFobosDevice()
             << "apiKind" << fobosApiKindName(activeFobosApiKind)
             << "sampleRate" << pendingSettings.sampleRate;
    if (streamWatchdogTimer) {
        streamWatchdogTimer->stop();
    }
    pendingAudioStartAfterStreamReady = false;
    clearSpectrumAfterStop = true;

    const bool directSamplingStartup = pendingSettings.inputMode != INPUT_RF;
    const bool externalBackend = isExternalReceiverBackendSelected();
    if (streamStartupRetryCount < 1) {
        ++streamStartupRetryCount;
        restartAfterStartupWatchdog = true;
        qDebug() << "[FobosLifecycle] stream startup watchdog will retry once"
                 << "retryCount" << streamStartupRetryCount
                 << "directSampling" << directSamplingStartup
                 << "externalBackend" << externalBackend;
    } else if (externalBackend) {
        restartAfterStartupWatchdog = false;
        qDebug() << "[FobosLifecycle] external receiver stream startup watchdog retry already used";
    } else {
        restartAfterStartupWatchdog = false;
        qDebug() << "[FobosLifecycle] stream startup watchdog retry already used; leaving receiver stopped";
    }

    stopFobosProcessing();
}

void YourClassName::pollStopCompletion() {
    if (!processor) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] pollStopCompletion: no processor";
        }
        finishFobosStop(false);
        return;
    }

    if (!processor->isRunning()) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[FobosLifecycle] DataProcessor stopped asynchronously"
                     << "elapsedMs" << stopElapsedTimer.elapsed();
        }
        finishFobosStop(false);
        return;
    }

    const qint64 elapsedMs = stopElapsedTimer.isValid() ? stopElapsedTimer.elapsed() : 0;
    if (stopCancelRetryCount < 4 && elapsedMs >= (stopCancelRetryCount + 1) * 1000) {
        ++stopCancelRetryCount;
        qDebug() << "[FobosLifecycle] retrying async cancel"
                 << "retry" << stopCancelRetryCount
                 << "elapsedMs" << elapsedMs;
        processor->requestStop();
        return;
    }

    if (elapsedMs < 6000) {
        return;
    }

    qDebug() << "[FobosLifecycle] DataProcessor did not stop after async cancel; forcing recovery"
             << "elapsedMs" << elapsedMs;
    if (stopPollTimer) {
        stopPollTimer->stop();
    }

    if (hasActiveFobosDevice() && !isExternalReceiverBackendSelected()) {
        qDebug() << "[FobosLifecycle] unsafe Fobos thread terminate skipped; USB reader is still inside driver callback"
                 << "device" << activeFobosDevice()
                 << "apiKind" << fobosApiKindName(activeFobosApiKind);
        closeFobosSessionAfterStop = true;
        updateUiForRunState();
        return;
    }

    const bool forced = processor->forceStop(1000);
    qDebug() << "[FobosLifecycle] forced recovery result" << forced;
    if (forced) {
        finishFobosStop(true);
        return;
    }

    qDebug() << "[FobosLifecycle] forced recovery failed; UI remains in Stopping to avoid unsafe restart";
    updateUiForRunState();
}
 
void YourClassName::stopFobosProcessing() {
    if (spectrumFftWorker) spectrumFftWorker->resetHfNoiseCancelState();
    if (playbackManager && playbackManager->isPlaying()) {
        stopPlayback();
        return;
    }

    if (recordingManager && recordingManager->isRecording()) {
        stopRecording(false);
    }

    if (isNetworkClientMode()) {
        const bool sent = sendRemoteControlCommand("stop");
        if (!sent) {
            qDebug() << "[Network] remote stop could not be sent; stopping local client state";
        }
        if (isClientIqProcessingMode()) {
            stopNetworkClientProcessing();
        } else if (remoteAudioPlayer) {
            remoteAudioPlayer->stop();
        }
        networkClientReconnectPending = false;
        runState = RadioRunState::Idle;
        updateUiForRunState();
        return;
    }

    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[FobosLifecycle] Stop requested"
                 << "state" << runStateName(runState)
                 << "deviceOpened" << deviceOpened
                 << "processorRunning" << (processor && processor->isRunning())
                 << "device" << activeFobosDevice()
                 << "apiKind" << fobosApiKindName(activeFobosApiKind);
    }
    logMemorySnapshot("before stop");
    stopGnssSdrAcquisition(QStringLiteral("stop"), false);
    resetListeningScanState();

    if (runState == RadioRunState::Idle && !deviceOpened && !(processor && processor->isRunning())) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "Stop ignored because radio is already idle.";
        }
        return;
    }
    if (runState == RadioRunState::Stopping) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "Stop ignored because radio is already stopping.";
        }
        return;
    }

    runState = RadioRunState::Stopping;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[FobosLifecycle] state changed" << runStateName(runState);
    }
    updateUiForRunState();
    if (fpvAutoScanController.active()) {
        stopFpvAutoScan(true);
    }
    if (streamWatchdogTimer) {
        streamWatchdogTimer->stop();
    }
    pendingAudioStartAfterStreamReady = false;
    updateTimer->stop();
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "Stop requested: spectrum timer stopped.";
    }

    bool processorStopRequested = false;
    if (processor && (deviceOpened || processor->isRunning())) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "Stop requested: requesting DataProcessor stop before audio shutdown.";
        }
        stopCancelRetryCount = 0;
        processor->requestStop();
        stopElapsedTimer.restart();
        if (stopPollTimer) {
            stopPollTimer->start();
        }
        processorStopRequested = true;
    }

    if (audioProcessor) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "Stop requested: stopping AudioProcessor.";
        }
        audioProcessor->stopDemodulation();
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "Stop requested: AudioProcessor stopped.";
        }
    }

    if (processorStopRequested) {
        return;
    }

    finishFobosStop(false);
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ObriiSDR"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Obrii SDR"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/obriisdr.png")));

    installDiagnosticLogger();
    installCrashLogger();
    qDebug() << "[Startup] loading splash screen";

    QPixmap splashPixmap(QStringLiteral(":/splash/obrii.png"));
    if (!splashPixmap.isNull()) {
        const QScreen *screen = QGuiApplication::primaryScreen();
        const QSize availableSize = screen ? screen->availableGeometry().size()
                                           : QSize(1280, 720);
        const QSize maximumSplashSize(qMin(1100, qRound(availableSize.width() * 0.78)),
                                      qMin(620, qRound(availableSize.height() * 0.72)));
        splashPixmap = splashPixmap.scaled(maximumSplashSize,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
    }
    QSplashScreen splash(splashPixmap, Qt::WindowStaysOnTopHint);
    if (!splashPixmap.isNull()) {
        splash.show();
        splash.repaint();
        app.processEvents(QEventLoop::ExcludeUserInputEvents, 25);
    }

    qDebug() << "[Startup] splash screen ready";
    logFobosApiInfo();
    logReceiverBackendRegistry();
    YourClassName window;
    window.showConfiguredInterface();

    if (splash.isVisible()) {
        splash.close();
    }

    qDebug() << "App started";
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
#endif
    return app.exec();
}
