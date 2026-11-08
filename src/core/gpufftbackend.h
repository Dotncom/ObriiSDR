#ifndef GPUFFTBACKEND_H
#define GPUFFTBACKEND_H

#include <QString>

#include <memory>

class GpuFftBackend {
public:
    GpuFftBackend();
    ~GpuFftBackend();

    GpuFftBackend(const GpuFftBackend &) = delete;
    GpuFftBackend &operator=(const GpuFftBackend &) = delete;

    static bool isCompiled();
    bool execute(const float *interleavedInput,
                 float *interleavedOutput,
                 int complexLength,
                 QString *errorMessage = nullptr);
    QString deviceName() const;
    void releasePlan();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // GPUFFTBACKEND_H
