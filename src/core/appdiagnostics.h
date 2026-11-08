#ifndef APPDIAGNOSTICS_H
#define APPDIAGNOSTICS_H

void installDiagnosticLogger();
void installCrashLogger();
void logMemorySnapshot(const char *tag);

#endif // APPDIAGNOSTICS_H
