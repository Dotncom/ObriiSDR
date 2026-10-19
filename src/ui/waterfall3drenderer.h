#ifndef WATERFALL3DRENDERER_H
#define WATERFALL3DRENDERER_H

#include <array>
#include <cstdint>
#include <deque>
#include <vector>
#include <QMatrix4x4>
#include <QColor>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QPointF>
#include <QVector3D>

class Waterfall3DRenderer {
public:
    struct SliceStatistics {
        bool valid = false;
        float minimumLevelDb = 0.0f;
        float averageLevelDb = 0.0f;
        float maximumLevelDb = 0.0f;
        int sampleCount = 0;
        int firstColumn = -1;
        int lastColumn = -1;
        int columnCount = 0;
        int peakColumn = -1;
        int firstRow = -1;
        int lastRow = -1;
        int rowCount = 0;
        int peakRow = -1;
    };

    void appendRow(const std::vector<float> &levels,
                   const std::vector<unsigned char> &rgb,
                   float levelMin,
                   float levelMax,
                   int repeatRows);
    void clear();
    bool hasData() const;
    void setResolutionDivisor(int divisor);
    void setHistoryCapacity(int rows);
    void setHighlightedHistoryRow(int row);
    void setSliceScrollStep(int points);
    void setSliceWidth(int points);
    void setSpectrumSliceScrollStep(int rows);
    void setSpectrumSliceWidth(int rows);
    void setSpectrumSliceCapture(bool enabled);
    void setSpectrumSliceCaptureFixed(bool enabled);
    void setFixedFrontPresentation(bool enabled);
    void setFixedFrontExpanded(bool enabled);
    void setFrontFaceGradient(bool enabled, int opacityPercent);
    void setDensityAxes(bool enabled);
    void setDensityAxisMapping(int xDimension, int yDimension);
    void setDensityFrontProfile(const std::vector<float> &normalizedLevels);
    void setDensityFrontProfileStyle(int style);
    void setSurfaceStyle(int style);
    void setSurfaceSmoothing(int smoothing);
    void setSurfaceLighting(int lighting);
    void setMonochrome(bool enabled);
    void setHeightGradient(bool enabled);
    void setMonochromeColor(const QColor &color);
    void setMonochromeGradientDepth(int percent);
    bool beginFrequencySlice(int screenX, int screenY, int viewportWidth, int viewportHeight);
    bool beginSpectrumSlice(int screenX, int screenY, int viewportWidth, int viewportHeight);
    void stepFrequencySlice(int direction);
    void stepSpectrumSlice(int direction);
    void endFrequencySlice();
    void endSpectrumSlice();
    double selectedFrequencyRatio() const;
    bool frequencySliceRange(double &centerRatio, double &firstRatio, double &lastRatio) const;
    bool spectrumSliceRange(int &firstRow, int &lastRow, int &rowCount) const;
    bool frequencySliceStatistics(SliceStatistics &statistics) const;
    bool spectrumSliceStatistics(SliceStatistics &statistics) const;
    void orbitCamera(float deltaX, float deltaY);
    void panCamera(float deltaX, float deltaY, int viewportWidth, int viewportHeight);
    void zoomCamera(int wheelDelta);
    bool densityAxisScreenPoints(int viewportWidth,
                                 int viewportHeight,
                                 QPointF &origin,
                                 QPointF &frequencyEnd,
                                 QPointF &densityEnd,
                                 QPointF &levelEnd) const;
    bool densityPointToScreen(float densityRatio,
                              float frequencyRatio,
                              float levelRatio,
                              int viewportWidth,
                              int viewportHeight,
                              QPointF &screen) const;
    void render(int viewportWidth, int viewportHeight);
    void releaseGpuResources();

private:
    struct VertexSample {
        float height = 0.0f;
        float levelDb = 0.0f;
        std::array<std::uint8_t, 3> color{{0, 0, 0}};
    };

    struct GpuGridVertex {
        float row = 0.0f;
        float column = 0.0f;
        float densityScale = 1.0f;
    };

    struct ViewParameters {
        QMatrix4x4 projection;
        QMatrix4x4 model;
        QMatrix4x4 transform;
    };

    using HistoryRow = std::vector<VertexSample>;

    std::deque<HistoryRow> historyRows;
    int maxHistoryRows = 128;
    int highlightedHistoryRow = -1;
    int resolutionDivisor = 4;
    float cameraYawDegrees = -38.0f;
    float cameraTiltDegrees = -60.0f;
    float cameraDistance = 3.45f;
    float cameraZoom = 1.0f;
    float cameraPanX = 0.0f;
    float cameraPanY = 0.0f;
    int sliceScrollStep = 1;
    int sliceWidth = 1;
    int selectedSliceColumn = -1;
    bool frequencySliceActive = false;
    int spectrumSliceScrollStep = 1;
    int spectrumSliceWidth = 1;
    int selectedSpectrumRow = -1;
    bool spectrumSliceActive = false;
    bool spectrumSliceCapture = false;
    bool spectrumSliceCaptureFixed = false;
    bool fixedFrontPresentation = false;
    bool fixedFrontExpanded = false;
    bool frontFaceGradient = false;
    int frontFaceGradientOpacity = 70;
    bool densityAxes = false;
    int densityAxisXDimension = 0;
    int densityAxisYDimension = 1;
    int densityAxisZDimension = 2;
    std::vector<float> densityFrontProfile;
    int densityFrontProfileStyle = 0;
    int surfaceStyle = 0;
    int surfaceSmoothing = 0;
    int surfaceLighting = 0;
    bool monochrome = false;
    bool heightGradient = false;
    QColor monochromeColor = QColor(45, 104, 196);
    int monochromeGradientDepth = 100;
    std::vector<HistoryRow> capturedSpectrumRows;
    int capturedSpectrumFirstRow = -1;
    int capturedSpectrumRowsRemaining = 0;
    QOpenGLBuffer surfaceVbo;
    QOpenGLShaderProgram surfaceProgram;
    bool surfaceProgramReady = false;
    bool surfaceProgramTried = false;
    bool gpuMeshDirty = true;
    std::vector<GpuGridVertex> gpuGridVertices;
    std::deque<HistoryRow> pendingGpuRows;
    unsigned int heightTexture = 0;
    unsigned int colorTexture = 0;
    int gpuTextureColumns = 0;
    int gpuTextureRows = 0;
    int gpuTextureRowCount = 0;
    int gpuTextureWriteRow = 0;
    int gpuTextureOldestRow = 0;
    bool gpuTexturesResetRequired = true;
    std::vector<std::uint8_t> gpuHeightScratch;
    std::vector<std::uint8_t> gpuColorScratch;

    void refreshCapturedSpectrumRows();
    void resampleHistoryColumns(int outputColumns);
    ViewParameters viewParameters(int viewportWidth, int viewportHeight) const;
    QVector3D densityPosition(float densityRatio,
                              float frequencyRatio,
                              float levelRatio) const;
    bool ensureSurfaceProgram();
    void resetGpuSurfaceData();
    void rebuildGpuSurface();
    bool uploadGpuSurfaceRows();
    bool renderGpuSurface(const QMatrix4x4 &transform);
    void applySampleColor(const VertexSample &sample) const;
    void applyHeightColor(float height) const;
};

#endif // WATERFALL3DRENDERER_H
