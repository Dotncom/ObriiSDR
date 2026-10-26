#include "fobosbackend.h"

// The generic backend keeps compatibility accessors for the main application.
// AnalyzerReceiver itself owns per-session handles; these remain null in the
// standalone analyzer and only satisfy that compatibility ABI.
fobos_dev_t *device = nullptr;
fobos_sdr_dev_t *agileDevice = nullptr;
FobosApiKind activeFobosApiKind = FobosApiKind::Standard;
