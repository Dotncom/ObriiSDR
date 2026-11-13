#ifndef APPCONSTANTS_H
#define APPCONSTANTS_H

#include <QtGlobal>

#include <cstddef>
#include <cstdint>

inline constexpr double RF_MIN_CENTER_FREQUENCY = 50000000.0;
inline constexpr double RF_MIN_LISTENING_FREQUENCY = 25000000.0;
inline constexpr double RF_EXPERIMENTAL_MAX_FREQUENCY = 7750000000.0;
inline constexpr double HACKRF_MIN_FREQUENCY = 1000000.0;
inline constexpr double HACKRF_MAX_FREQUENCY = 6000000000.0;
inline constexpr double DIRECT_MIN_FREQUENCY = 1.0;

inline constexpr int SCALE_SLIDER_FACTOR = 10;
inline constexpr double MIN_SCALE_PERCENT = 0.1;
inline constexpr double MAX_SCALE_PERCENT = 100.0;
inline constexpr int LEVEL_SLIDER_FACTOR = 10;
inline constexpr int MIN_LEVEL_SLIDER_VALUE = -1600;
inline constexpr int MAX_LEVEL_SLIDER_VALUE = 200;
inline constexpr float MIN_LEVEL_GAP = 0.1f;

inline constexpr int NETWORK_SPECTRUM_MAX_BINS = 2048;
inline constexpr int NETWORK_CHANNEL_SPECTRUM_MAX_BINS = 768;
inline constexpr int NETWORK_SPECTRUM_INTERVAL_MS = 100;
inline constexpr int NETWORK_CHANNEL_SPECTRUM_INTERVAL_MS = 160;
inline constexpr int NETWORK_FULL_RESOLUTION_SPECTRUM_INTERVAL_MS = 50;
inline constexpr int NETWORK_SPECTRUM_FPS_MIN = 1;
inline constexpr int NETWORK_SPECTRUM_FPS_MAX = 60;
inline constexpr int NETWORK_SPECTRUM_FPS_DEFAULT = 20;
inline constexpr qint64 NETWORK_FULL_RESOLUTION_SPECTRUM_BYTES_PER_SECOND = 1536 * 1024;
inline constexpr qint64 NETWORK_IQ_MAX_PENDING_BYTES = 8 * 1024 * 1024;
inline constexpr qint64 NETWORK_CHANNEL_IQ_LOW_LATENCY_PENDING_BYTES = 2 * 1024 * 1024;
inline constexpr std::uint64_t NETWORK_IQ_DROP_LOG_INTERVAL = 200;
inline constexpr int NETWORK_SETTINGS_ACK_TIMEOUT_MS = 1000;
inline constexpr double NETWORK_AUDIO_PREBUFFER_SECONDS = 0.55;
inline constexpr qint64 NETWORK_SPECTRUM_MAX_PENDING_BYTES = 64 * 1024;

inline constexpr int RTL_TCP_DEVICE_INDEX = -1000;
inline constexpr int RTLSDR_NATIVE_DEVICE_INDEX_BASE = -2000;
inline constexpr int SOAPY_SDR_DEVICE_INDEX = -3000;
inline constexpr int BLADERF_NATIVE_DEVICE_INDEX_BASE = -4000;
inline constexpr int HACKRF_NATIVE_DEVICE_INDEX_BASE = -5000;
inline constexpr int VIRTUAL_SIGNAL_DEVICE_INDEX = -6000;
inline constexpr int UHD_NATIVE_DEVICE_INDEX_BASE = -7000;
// Keep the prototype compiled for future hardware work, but do not expose or
// start it until enumeration and streaming are validated on a physical unit.
inline constexpr bool BLADERF_NATIVE_BACKEND_ENABLED = false;
inline constexpr int NETWORK_REMOTE_RECEIVER_DEVICE_INDEX_BASE = 100000000;
inline constexpr int NETWORK_REMOTE_RECEIVER_DEVICE_INDEX_MIN = 90000000;
inline constexpr int NETWORK_REMOTE_RECEIVER_DEVICE_INDEX_MAX = 110000000;
inline constexpr const char *RTL_TCP_DEFAULT_HOST = "127.0.0.1";
inline constexpr quint16 RTL_TCP_DEFAULT_PORT = 1234;
inline constexpr quint16 VIRTUAL_SIGNAL_DEFAULT_PORT = 17661;
inline constexpr double FOBOS_DEFAULT_SAMPLE_RATE = 50000000.0;
inline constexpr double RTL_TCP_SAFE_SAMPLE_RATE = 2048000.0;

inline constexpr int DMR_BACKEND_FOBOS_MBELIB = 0;
inline constexpr int DMR_BACKEND_FOBOS_OPENDMR = 1;
inline constexpr int DMR_BACKEND_DSD_NEO = 2;
inline constexpr int DMR_BACKEND_GOPHERTRUNK = 3;
inline constexpr double DMR_CENTER_MIN_OFFSET_HZ = 25000.0;

inline constexpr int AUDIO_RELAY_HEADER_BYTES = 12;
inline constexpr int AUDIO_RELAY_BUFFER_MIN_MS = 40;
inline constexpr int AUDIO_RELAY_BUFFER_DEFAULT_MS = 160;
inline constexpr int AUDIO_RELAY_BUFFER_MAX_MS = 400;
inline constexpr qint64 AUDIO_HTTP_MAX_PENDING_BYTES = 512 * 1024;
inline constexpr int AUDIO_LOW_PASS_SLIDER_STEP_HZ = 100;
inline constexpr int AUDIO_LOW_PASS_SLIDER_MAX = 200;
inline constexpr int AUDIO_HIGH_PASS_SLIDER_STEP_HZ = 25;
inline constexpr int AUDIO_HIGH_PASS_SLIDER_MAX = 40;

inline constexpr int FINE_TUNE_DIAL_MIN = -100;
inline constexpr int FINE_TUNE_DIAL_MAX = 100;
inline constexpr double FINE_TUNE_VISIBLE_RANGE_DIVISOR = 20.0;
inline constexpr double FINE_TUNE_MIN_RANGE_HZ = 500.0;
inline constexpr double FINE_TUNE_MAX_RANGE_HZ = 500000.0;
inline constexpr int FINE_TUNE_MODE_SCALE = 0;
inline constexpr int FINE_TUNE_MODE_DIAL = 1;

inline constexpr int HF_NOISE_CANCEL_DEPTH_MIN = 0;
inline constexpr int HF_NOISE_CANCEL_DEPTH_MAX = 200;
inline constexpr int HF_NOISE_CANCEL_REF_GAIN_MIN = -400;
inline constexpr int HF_NOISE_CANCEL_REF_GAIN_MAX = 400;
inline constexpr int HF_NOISE_CANCEL_REF_DELAY_MIN_NS = -2000;
inline constexpr int HF_NOISE_CANCEL_REF_DELAY_MAX_NS = 2000;
inline constexpr int HF_NOISE_CANCEL_REF_TILT_MIN = -300;
inline constexpr int HF_NOISE_CANCEL_REF_TILT_MAX = 300;
inline constexpr int HF_AUDIO_BLANKER_THRESHOLD_MIN = 20;
inline constexpr int HF_AUDIO_BLANKER_THRESHOLD_MAX = 200;

inline constexpr int VIDEO_SNAPSHOT_INTERVAL_MS = 90;
inline constexpr std::size_t VIDEO_SNAPSHOT_MAX_FLOATS = 262144 * 2;

inline constexpr qint64 FPV_HUNTER_TRACK_HOLD_MS = 120000;
inline constexpr int FPV_HUNTER_TRACK_HOLD_FRAMES = 1200;
inline constexpr int FPV_HUNTER_TRACK_STABLE_MISS_FRAMES = 3;
inline constexpr int FPV_HUNTER_MAX_EVENTS = 32;

inline constexpr qint64 LIVE_RETUNE_SETTLE_MS = 80;
inline constexpr qint64 AGILE_RF_LOW_RATE_RETUNE_SETTLE_MS = 550;
inline constexpr qint64 AGILE_RF_LOW_RATE_SAMPLE_SETTLE_MS = 900;
inline constexpr qint64 AGILE_RF_MID_RATE_RETUNE_SETTLE_MS = 320;
inline constexpr qint64 AGILE_RF_MID_RATE_SAMPLE_SETTLE_MS = 600;
inline constexpr qint64 AGILE_RF_HIGH_RATE_RETUNE_SETTLE_MS = 160;
inline constexpr qint64 AGILE_RF_HIGH_RATE_SAMPLE_SETTLE_MS = 280;
inline constexpr int AGILE_LIVE_RETUNE_MIN_COMMAND_INTERVAL_MS = 120;
inline constexpr int AGILE_LIVE_RETUNE_DEFAULT_COMMAND_INTERVAL_MS = 160;
inline constexpr int AGILE_LIVE_RETUNE_MAX_COMMAND_INTERVAL_MS = 1000;
inline constexpr int PERSISTENT_SETTINGS_MIN_SAVE_INTERVAL_MS = 750;

inline constexpr qint64 STANDARD_SCAN_SETTLE_MS = 60;
inline constexpr int STANDARD_SCAN_MIN_SETTLE_MS = 0;
inline constexpr int STANDARD_SCAN_MAX_SETTLE_MS = 1000;
inline constexpr int STANDARD_SCAN_MIN_DWELL_MS = 20;
inline constexpr int STANDARD_SCAN_MAX_DWELL_MS = 5000;
inline constexpr int LISTENING_SCAN_MIN_DWELL_MS = 50;
inline constexpr int LISTENING_SCAN_MAX_DWELL_MS = 600000;
inline constexpr int LISTENING_SCAN_MIN_SETTLE_MS = 0;
inline constexpr int LISTENING_SCAN_MAX_SETTLE_MS = 10000;

inline constexpr int SPECTRUM_UPDATE_AUTO_MS = 0;
inline constexpr int SPECTRUM_UPDATE_MIN_MS = 1;
inline constexpr int SPECTRUM_UPDATE_MAX_MS = 250;
inline constexpr int WATERFALL_ROWS_PER_FRAME_MIN = 1;
inline constexpr int WATERFALL_ROWS_PER_FRAME_DEFAULT = 1;
inline constexpr int WATERFALL_ROWS_PER_FRAME_MAX = 8;
inline constexpr int ADDITIONAL_SCALE_DIVISOR_MAX = 100;

inline constexpr double AGILE_RF_LOW_RATE_AUTO_BANDWIDTH_RATIO = 1.00;
inline constexpr double AGILE_RF_MID_RATE_AUTO_BANDWIDTH_RATIO = 1.00;
inline constexpr double AGILE_RF_HIGH_RATE_AUTO_BANDWIDTH_RATIO = 1.00;
inline constexpr int AGILE_SCAN_MIN_POINTS = 2;
inline constexpr int AGILE_SCAN_MAX_POINTS = 256;
inline constexpr double AGILE_SCAN_MIN_STEP_MHZ = 0.001;
inline constexpr double AGILE_SCAN_MAX_STEP_MHZ = 1000.0;

inline constexpr double SCAN_MEASUREMENT_MIN_BIN_MHZ = 0.001;
inline constexpr double SCAN_MEASUREMENT_MAX_BIN_MHZ = 10.0;
inline constexpr float SCAN_MEASUREMENT_COVERAGE_DELTA_DB = 6.0f;
inline constexpr int SCAN_MEASUREMENT_MIN_UPDATE_MS = 20;
inline constexpr int SCAN_MEASUREMENT_DEFAULT_UPDATE_MS = 120;
inline constexpr int SCAN_MEASUREMENT_MAX_UPDATE_MS = 1000;

inline constexpr int SPUR_CALIBRATION_TARGET_FRAMES = 32;
inline constexpr float SPUR_CALIBRATION_MIN_PROMINENCE_DB = 8.0f;
inline constexpr float SPUR_CALIBRATION_MIN_NARROW_DB = 2.0f;
inline constexpr int SPUR_CALIBRATION_INNER_BINS = 4;
inline constexpr int SPUR_CALIBRATION_OUTER_BINS = 36;
inline constexpr int SPUR_MAX_MASK_ENTRIES = 64;
inline constexpr double SPUR_MIN_MASK_WIDTH_HZ = 50.0;
inline constexpr double SPUR_MAX_MASK_WIDTH_HZ = 20000.0;

inline constexpr int GNSS_SATELLITE_TABLE_ROWS = 64;
inline constexpr int GNSS_SATELLITE_TABLE_GROW_STEP = 32;
inline constexpr int GNSS_SATELLITE_TABLE_MAX_ROWS = 256;

#endif // APPCONSTANTS_H
