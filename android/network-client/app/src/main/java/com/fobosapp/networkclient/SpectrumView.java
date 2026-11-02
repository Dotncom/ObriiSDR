package com.fobosapp.networkclient;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;

import java.util.Locale;

public final class SpectrumView extends View {
    public interface TuneRequestListener {
        void onTuneRequested(double frequencyHz);
    }

    public interface CenterTuneRequestListener {
        void onCenterTuneRequested(double frequencyHz);
    }

    public interface PanTuneRequestListener {
        void onPanTuneRequested(double deltaHz);
    }

    public interface ZoomProgressListener {
        void onZoomProgressChanged(int progress);
    }

    private static final int[] WATERFALL_COLORS = new int[] {
            Color.rgb(0, 0, 32),
            Color.rgb(0, 0, 80),
            Color.rgb(0, 0, 144),
            Color.rgb(0, 0, 240),
            Color.rgb(0, 80, 255),
            Color.rgb(80, 240, 48),
            Color.rgb(30, 144, 255),
            Color.rgb(255, 255, 255),
            Color.rgb(255, 255, 0),
            Color.rgb(254, 109, 22),
            Color.rgb(255, 0, 0),
            Color.rgb(198, 0, 0),
            Color.rgb(117, 0, 0)
    };

    private static final class BandMarker {
        final double startHz;
        final double endHz;
        final String label;
        final boolean amateur;

        BandMarker(String label, double startMhz, double endMhz, boolean amateur) {
            this.label = label;
            this.startHz = startMhz * 1_000_000.0;
            this.endHz = endMhz * 1_000_000.0;
            this.amateur = amateur;
        }
    }

    private static final BandMarker[] BAND_MARKERS = new BandMarker[] {
            new BandMarker("MW BC", 0.5265, 1.705, false),
            new BandMarker("SW 49m", 5.9, 6.2, false),
            new BandMarker("SW 41m", 7.2, 7.45, false),
            new BandMarker("SW 31m", 9.4, 9.9, false),
            new BandMarker("SW 25m", 11.6, 12.1, false),
            new BandMarker("SW 19m", 15.1, 15.8, false),
            new BandMarker("CB", 26.965, 27.405, false),
            new BandMarker("FM BC", 87.5, 108.0, false),
            new BandMarker("Air", 118.0, 137.0, false),
            new BandMarker("WX Sat", 137.0, 138.0, false),
            new BandMarker("Marine", 156.0, 162.05, false),
            new BandMarker("UHF Satcom", 240.0, 270.0, false),
            new BandMarker("TETRA", 380.0, 430.0, false),
            new BandMarker("PMR446", 446.0, 446.2, false),
            new BandMarker("ADS-B", 1089.5, 1090.5, false),
            new BandMarker("L-band Sat", 1525.0, 1660.5, false),
            new BandMarker("GNSS L1", 1559.0, 1610.0, false),
            new BandMarker("ISM 2.4", 2400.0, 2483.5, false),
            new BandMarker("FPV 5.8", 5650.0, 5925.0, false),

            new BandMarker("2200m", 0.1357, 0.1378, true),
            new BandMarker("630m", 0.472, 0.479, true),
            new BandMarker("160m", 1.81, 2.0, true),
            new BandMarker("80m", 3.5, 3.8, true),
            new BandMarker("60m", 5.3515, 5.3665, true),
            new BandMarker("40m", 7.0, 7.2, true),
            new BandMarker("30m", 10.1, 10.15, true),
            new BandMarker("20m", 14.0, 14.35, true),
            new BandMarker("17m", 18.068, 18.168, true),
            new BandMarker("15m", 21.0, 21.45, true),
            new BandMarker("12m", 24.89, 24.99, true),
            new BandMarker("10m", 28.0, 29.7, true),
            new BandMarker("6m", 50.0, 52.0, true),
            new BandMarker("4m", 70.0, 70.5, true),
            new BandMarker("2m", 144.0, 146.0, true),
            new BandMarker("70cm", 430.0, 440.0, true),
            new BandMarker("23cm", 1240.0, 1300.0, true),
            new BandMarker("13cm", 2300.0, 2450.0, true),
            new BandMarker("6cm", 5650.0, 5850.0, true)
    };

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private String rxStatus = "Off";
    private int rxStatusColor = Color.GRAY;

    void setRxStatus(String text, int color) {
        if (rxStatus.equals(text) && rxStatusColor == color) return;
        rxStatus = text;
        rxStatusColor = color;
        postInvalidate();
    }
    private double[] frequencies = new double[0];
    private float[] magnitudes = new float[0];
    private double minFrequency = 0.0;
    private double maxFrequency = 1.0;
    private double rawMinFrequency = 0.0;
    private double rawMaxFrequency = 1.0;
    private double visibleMinFrequency = 0.0;
    private double visibleMaxFrequency = 1.0;
    private double listeningFrequency = 0.0;
    private double bandwidth = 0.0;
    private int modulationType = RadioSettings.MOD_AM;
    private float levelMin = -130.0f;
    private float levelMax = -50.0f;
    private Bitmap waterfall;
    private int[] waterfallPixels = new int[0];
    private TuneRequestListener tuneRequestListener;
    private CenterTuneRequestListener centerTuneRequestListener;
    private PanTuneRequestListener panTuneRequestListener;
    private ZoomProgressListener zoomProgressListener;
    private float downX = 0.0f;
    private float downY = 0.0f;
    private float lastTapX = Float.NaN;
    private float lastTapY = Float.NaN;
    private long lastTapTimeMs = 0L;
    private long doubleTapTimeoutMs = 280L;
    private float lastPanX = 0.0f;
    private float tapSlopPx = 24.0f;
    private float pinchStartDistance = 0.0f;
    private float pinchStartFocusX = 0.0f;
    private double pinchStartSpan = 1.0;
    private double pinchFocusFrequency = 0.0;
    private boolean gestureMoved = false;
    private boolean pinching = false;
    private boolean userZoomed = false;
    private boolean allowNegativeFrequencies = false;
    private boolean showGeneralBandMarkers = false;
    private boolean showAmateurBandMarkers = false;

    public SpectrumView(Context context) {
        super(context);
        init();
    }

    public synchronized void setTuneRequestListener(TuneRequestListener listener) {
        tuneRequestListener = listener;
    }

    public synchronized void setCenterTuneRequestListener(CenterTuneRequestListener listener) {
        centerTuneRequestListener = listener;
    }

    public synchronized void setPanTuneRequestListener(PanTuneRequestListener listener) {
        panTuneRequestListener = listener;
    }

    public synchronized void setZoomProgressListener(ZoomProgressListener listener) {
        zoomProgressListener = listener;
    }

    public SpectrumView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public synchronized void setLevelRange(float minLevel, float maxLevel) {
        if (!Float.isFinite(minLevel) || !Float.isFinite(maxLevel) || maxLevel <= minLevel) {
            return;
        }
        levelMin = minLevel;
        levelMax = maxLevel;
        updateWaterfallRow();
        invalidate();
    }

    public synchronized void setBandMarkersEnabled(boolean generalEnabled, boolean amateurEnabled) {
        showGeneralBandMarkers = generalEnabled;
        showAmateurBandMarkers = amateurEnabled;
        invalidate();
    }

    public synchronized void setAllowNegativeFrequencies(boolean allowNegative) {
        if (allowNegativeFrequencies == allowNegative) {
            return;
        }
        allowNegativeFrequencies = allowNegative;
        updateVisibleRange((visibleMinFrequency + visibleMaxFrequency) * 0.5,
                visibleMaxFrequency - visibleMinFrequency);
        invalidate();
    }

    public synchronized double visibleSpanHz() {
        return Math.max(1.0, visibleMaxFrequency - visibleMinFrequency);
    }

    public synchronized int zoomProgress() {
        double lowerLimit = rangeLowerLimit();
        double upperLimit = rangeUpperLimit();
        double fullSpan = Math.max(1.0, upperLimit - lowerLimit);
        double visibleSpan = Math.max(1.0, visibleMaxFrequency - visibleMinFrequency);
        double zoom = Math.max(1.0, Math.min(100.0, fullSpan / visibleSpan));
        return (int) Math.round(Math.log(zoom) / Math.log(100.0) * 100.0);
    }

    public synchronized void setZoomProgress(int progress) {
        int clampedProgress = Math.max(0, Math.min(100, progress));
        double lowerLimit = rangeLowerLimit();
        double upperLimit = rangeUpperLimit();
        double fullSpan = Math.max(1.0, upperLimit - lowerLimit);
        double zoom = Math.pow(100.0, clampedProgress / 100.0);
        double span = fullSpan / Math.max(1.0, zoom);
        double center = (visibleMinFrequency + visibleMaxFrequency) * 0.5;
        if (!Double.isFinite(center) || center <= 0.0) {
            center = (lowerLimit + upperLimit) * 0.5;
        }
        setVisibleRange(center, span);
        invalidate();
        notifyZoomProgressChanged();
    }

    public synchronized void shiftFrequencyDisplay(double deltaHz) {
        if (!Double.isFinite(deltaHz) || Math.abs(deltaHz) < 0.01) {
            return;
        }
        minFrequency += deltaHz;
        maxFrequency += deltaHz;
        visibleMinFrequency += deltaHz;
        visibleMaxFrequency += deltaHz;
        setVisibleRange((visibleMinFrequency + visibleMaxFrequency) * 0.5,
                visibleMaxFrequency - visibleMinFrequency);
        invalidate();
    }

    public synchronized void setListeningFrequency(double frequencyHz) {
        if (!Double.isFinite(frequencyHz)) {
            return;
        }
        listeningFrequency = frequencyHz;
        invalidate();
    }

    public synchronized void setSpectrum(double[] newFrequencies,
                                         float[] newMagnitudes,
                                         double newMinFrequency,
                                         double newMaxFrequency,
                                         double newListeningFrequency,
                                         double newBandwidth,
                                         int newModulationType) {
        frequencies = newFrequencies != null ? newFrequencies : new double[0];
        magnitudes = newMagnitudes != null ? newMagnitudes : new float[0];
        double oldVisibleCenter = (visibleMinFrequency + visibleMaxFrequency) * 0.5;
        double oldVisibleSpan = visibleMaxFrequency - visibleMinFrequency;
        rawMinFrequency = newMinFrequency;
        rawMaxFrequency = newMaxFrequency > newMinFrequency ? newMaxFrequency : newMinFrequency + 1.0;
        minFrequency = rawMinFrequency;
        maxFrequency = rawMaxFrequency;
        updateVisibleRange(oldVisibleCenter, oldVisibleSpan);
        listeningFrequency = newListeningFrequency;
        bandwidth = newBandwidth;
        modulationType = newModulationType;
        updateWaterfallRow();
        invalidate();
    }

    @Override
    protected synchronized void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int width = getWidth();
        int height = getHeight();
        if (width <= 0 || height <= 0) {
            return;
        }

        int scaleHeight = frequencyScaleHeight();
        int spectrumHeight = spectrumHeightFor(height);
        int waterfallTop = spectrumHeight + scaleHeight;
        canvas.drawColor(Color.rgb(10, 14, 18));
        drawSpectrum(canvas, width, spectrumHeight);
        drawFrequencyScale(canvas, width, spectrumHeight, scaleHeight);
        drawWaterfall(canvas, width, waterfallTop, Math.max(1, height - waterfallTop));
        drawTuning(canvas, width, height);
        paint.setStyle(Paint.Style.FILL);
        paint.setTextSize(26.0f);
        float badgeWidth = paint.measureText(rxStatus) + 44;
        float left = width - badgeWidth - 8;
        paint.setColor(Color.argb(220, 10, 14, 18));
        canvas.drawRoundRect(left, 4, width - 6, 42, 8, 8, paint);
        paint.setColor(rxStatusColor);
        canvas.drawCircle(left + 12, 23, 5, paint);
        canvas.drawText(rxStatus, left + 24, 32, paint);
    }

    @Override
    public synchronized boolean onTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = event.getX();
                downY = event.getY();
                lastPanX = downX;
                gestureMoved = false;
                pinching = false;
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                if (event.getPointerCount() >= 2) {
                    pinching = true;
                    gestureMoved = true;
                    pinchStartDistance = pointerDistance(event);
                    pinchStartSpan = Math.max(1.0, visibleMaxFrequency - visibleMinFrequency);
                    pinchStartFocusX = Math.max(0.0f,
                            Math.min(pointerCenterX(event), Math.max(1.0f, getWidth() - 1.0f)));
                    pinchFocusFrequency = frequencyAtX(pinchStartFocusX, getWidth());
                }
                return true;
            case MotionEvent.ACTION_MOVE:
                if (event.getPointerCount() >= 2 && pinching) {
                    applyPinchZoom(event);
                    return true;
                }
                if (event.getPointerCount() == 1 && !pinching) {
                    applyPan(event.getX(), event.getY());
                    return true;
                }
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                pinching = event.getPointerCount() > 2;
                if (event.getPointerCount() >= 1) {
                    lastPanX = event.getX(0);
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (!gestureMoved) {
                    handleTap(event);
                }
                pinching = false;
                return true;
            case MotionEvent.ACTION_CANCEL:
                pinching = false;
                return true;
            default:
                return true;
        }
    }

    @Override
    protected synchronized void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        if (width <= 0 || height <= 0) {
            waterfall = null;
            return;
        }
        int waterfallHeight = Math.max(1, height - spectrumHeightFor(height) - frequencyScaleHeight());
        resizeWaterfall(width, waterfallHeight);
    }

    private void resizeWaterfall(int width, int waterfallHeight) {
        if (width <= 0 || waterfallHeight <= 0) {
            return;
        }
        if (waterfall != null && waterfall.getWidth() == width && waterfall.getHeight() == waterfallHeight) {
            return;
        }
        Bitmap next = Bitmap.createBitmap(width, waterfallHeight, Bitmap.Config.ARGB_8888);
        int[] nextPixels = new int[width * waterfallHeight];
        for (int i = 0; i < nextPixels.length; ++i) {
            nextPixels[i] = Color.rgb(4, 8, 12);
        }
        if (waterfall != null) {
            Rect src = new Rect(0, 0, waterfall.getWidth(), waterfall.getHeight());
            Rect dst = new Rect(0, 0, width, waterfallHeight);
            Canvas canvas = new Canvas(next);
            canvas.drawColor(Color.rgb(4, 8, 12));
            canvas.drawBitmap(waterfall, src, dst, null);
            next.getPixels(nextPixels, 0, width, 0, 0, width, waterfallHeight);
        }
        waterfall = next;
        waterfallPixels = nextPixels;
    }

    private void init() {
        tapSlopPx = Math.max(18.0f, ViewConfiguration.get(getContext()).getScaledTouchSlop() * 2.5f);
        doubleTapTimeoutMs = ViewConfiguration.getDoubleTapTimeout();
        paint.setTypeface(android.graphics.Typeface.MONOSPACE);
    }

    private void handleTap(MotionEvent event) {
        float x = event.getX();
        float y = event.getY();
        long now = event.getEventTime();
        boolean doubleTap = lastTapTimeMs > 0 &&
                now - lastTapTimeMs <= doubleTapTimeoutMs &&
                Math.abs(x - lastTapX) <= tapSlopPx &&
                Math.abs(y - lastTapY) <= tapSlopPx;
        double frequency = frequencyAtX(x, getWidth());
        if (doubleTap && centerTuneRequestListener != null) {
            lastTapTimeMs = 0L;
            centerTuneRequestListener.onCenterTuneRequested(frequency);
            return;
        }
        lastTapTimeMs = now;
        lastTapX = x;
        lastTapY = y;
        if (tuneRequestListener != null) {
            tuneRequestListener.onTuneRequested(frequency);
        }
    }

    private void drawSpectrum(Canvas canvas, int width, int spectrumHeight) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.rgb(15, 21, 27));
        canvas.drawRect(0, 0, width, spectrumHeight, paint);

        drawBandMarkers(canvas, width, spectrumHeight);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(1.0f);
        paint.setColor(Color.rgb(38, 50, 58));
        for (int i = 1; i < 4; ++i) {
            float y = spectrumHeight * i / 4.0f;
            canvas.drawLine(0, y, width, y, paint);
        }

        int count = Math.min(frequencies.length, magnitudes.length);
        if (count < 2) {
            drawEmptyLabel(canvas, spectrumHeight);
            return;
        }

        paint.setColor(Color.rgb(88, 214, 141));
        paint.setStrokeWidth(2.0f);
        boolean havePrevious = false;
        float previousX = 0.0f;
        float previousY = 0.0f;
        for (int i = 0; i < count; ++i) {
            double frequency = frequencies[i];
            if (!Double.isFinite(frequency) ||
                    frequency < visibleMinFrequency ||
                    frequency > visibleMaxFrequency) {
                continue;
            }
            float x = frequencyToX(frequency, width);
            float y = magnitudeToY(displayMagnitudeAt(i, count), spectrumHeight);
            if (havePrevious) {
                canvas.drawLine(previousX, previousY, x, y, paint);
            }
            previousX = x;
            previousY = y;
            havePrevious = true;
        }
    }

    private void drawBandMarkers(Canvas canvas, int width, int spectrumHeight) {
        if ((!showGeneralBandMarkers && !showAmateurBandMarkers) ||
                visibleMaxFrequency <= visibleMinFrequency ||
                width <= 1 ||
                spectrumHeight <= 1) {
            return;
        }

        paint.setTextSize(17.0f);
        paint.setStyle(Paint.Style.FILL);
        for (boolean amateurLayer : new boolean[] {false, true}) {
            if (amateurLayer && !showAmateurBandMarkers) {
                continue;
            }
            if (!amateurLayer && !showGeneralBandMarkers) {
                continue;
            }
            int fill = amateurLayer ? Color.argb(42, 255, 198, 66)
                                    : Color.argb(34, 76, 162, 255);
            int edge = amateurLayer ? Color.argb(120, 255, 220, 96)
                                    : Color.argb(96, 112, 196, 255);
            int text = amateurLayer ? Color.argb(220, 255, 232, 150)
                                    : Color.argb(205, 176, 224, 255);

            for (BandMarker marker : BAND_MARKERS) {
                if (marker.amateur != amateurLayer ||
                        marker.endHz < visibleMinFrequency ||
                        marker.startHz > visibleMaxFrequency) {
                    continue;
                }
                double clippedStart = Math.max(marker.startHz, visibleMinFrequency);
                double clippedEnd = Math.min(marker.endHz, visibleMaxFrequency);
                int x1 = Math.max(0, Math.min(width - 1, (int) Math.floor(frequencyToX(clippedStart, width))));
                int x2 = Math.max(0, Math.min(width - 1, (int) Math.ceil(frequencyToX(clippedEnd, width))));
                if (x2 < x1) {
                    int temp = x1;
                    x1 = x2;
                    x2 = temp;
                }
                int markerWidth = Math.max(1, x2 - x1);
                paint.setColor(fill);
                paint.setStyle(Paint.Style.FILL);
                canvas.drawRect(x1, 0, x1 + markerWidth, spectrumHeight, paint);
                paint.setColor(edge);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(1.0f);
                canvas.drawLine(x1, 0, x1, spectrumHeight, paint);
                if (markerWidth > 2) {
                    canvas.drawLine(x1 + markerWidth, 0, x1 + markerWidth, spectrumHeight, paint);
                }
                if (markerWidth >= dp(44)) {
                    paint.setColor(text);
                    paint.setStyle(Paint.Style.FILL);
                    String label = ellipsize(marker.label, markerWidth - dp(6));
                    canvas.drawText(label, x1 + dp(3), dp(17), paint);
                }
            }
        }
    }

    private void drawWaterfall(Canvas canvas, int width, int top, int waterfallHeight) {
        if (waterfall == null || waterfall.getWidth() != width || waterfall.getHeight() != waterfallHeight) {
            resizeWaterfall(width, waterfallHeight);
        }
        if (waterfall != null) {
            canvas.drawBitmap(waterfall, 0, top, null);
        }
    }

    private void drawTuning(Canvas canvas, int width, int height) {
        float x = frequencyToX(listeningFrequency, width);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(2.0f);
        paint.setColor(Color.rgb(255, 218, 121));
        canvas.drawLine(x, 0, x, height, paint);

        if (bandwidth > 0.0) {
            float half = (float) ((bandwidth / Math.max(1.0, visibleMaxFrequency - visibleMinFrequency)) * width / 2.0);
            paint.setColor(Color.argb(70, 255, 218, 121));
            paint.setStyle(Paint.Style.FILL);
            canvas.drawRect(x - half, 0, x + half, height, paint);
        }

        paint.setColor(Color.rgb(225, 232, 238));
        paint.setTextSize(28.0f);
        paint.setStyle(Paint.Style.FILL);
        String label = String.format(Locale.US,
                "%s %.6f MHz",
                RadioSettings.modulationLabel(modulationType),
                listeningFrequency / 1_000_000.0);
        canvas.drawText(label, 14.0f, 32.0f, paint);
    }

    private void drawFrequencyScale(Canvas canvas, int width, int top, int scaleHeight) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.rgb(7, 12, 16));
        canvas.drawRect(0, top, width, top + scaleHeight, paint);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(1.0f);
        paint.setColor(Color.rgb(48, 70, 76));
        canvas.drawLine(0, top + 0.5f, width, top + 0.5f, paint);
        canvas.drawLine(0, top + scaleHeight - 0.5f, width, top + scaleHeight - 0.5f, paint);

        int majorTicks = width >= 900 ? 9 : (width >= 540 ? 7 : 5);
        int minorDivisions = 4;
        double visibleSpan = Math.max(1.0, visibleMaxFrequency - visibleMinFrequency);
        paint.setStyle(Paint.Style.STROKE);
        for (int tick = 0; tick < majorTicks; ++tick) {
            double fraction = majorTicks <= 1 ? 0.0 : tick / (double) (majorTicks - 1);
            float x = (float) (fraction * (width - 1.0));
            paint.setColor(Color.rgb(86, 115, 112));
            paint.setStrokeWidth(1.2f);
            canvas.drawLine(x, top, x, top + dp(12), paint);
            canvas.drawLine(x, top + scaleHeight - dp(8), x, top + scaleHeight, paint);

            if (tick < majorTicks - 1) {
                for (int minor = 1; minor < minorDivisions; ++minor) {
                    double minorFraction = (tick + minor / (double) minorDivisions) / (majorTicks - 1.0);
                    float minorX = (float) (minorFraction * (width - 1.0));
                    paint.setColor(Color.rgb(42, 62, 67));
                    canvas.drawLine(minorX, top, minorX, top + dp(6), paint);
                }
            }
        }

        paint.setColor(Color.rgb(185, 199, 208));
        paint.setStyle(Paint.Style.FILL);
        paint.setTextSize(width >= 540 ? 20.0f : 17.0f);
        for (int tick = 0; tick < majorTicks; ++tick) {
            boolean drawLabel = width >= 540 ||
                    tick == 0 ||
                    tick == majorTicks / 2 ||
                    tick == majorTicks - 1;
            if (!drawLabel) {
                continue;
            }
            double fraction = majorTicks <= 1 ? 0.0 : tick / (double) (majorTicks - 1);
            double frequency = visibleMinFrequency + fraction * visibleSpan;
            String label = formatFrequencyLabel(frequency);
            float labelWidth = paint.measureText(label);
            float x = (float) (fraction * (width - 1.0));
            float labelX = Math.max(dp(3), Math.min(width - labelWidth - dp(3), x - labelWidth * 0.5f));
            canvas.drawText(label, labelX, top + scaleHeight - dp(12), paint);
        }

        double fullSpan = Math.max(1.0, maxFrequency - minFrequency);
        double zoom = fullSpan / visibleSpan;
        if (zoom > 1.05) {
            String zoomLabel = String.format(Locale.US, "x%.1f", zoom);
            paint.setColor(Color.rgb(255, 218, 121));
            paint.setTextSize(18.0f);
            float zoomWidth = paint.measureText(zoomLabel);
            canvas.drawText(zoomLabel, width - zoomWidth - dp(8), top + dp(16), paint);
        }
    }

    private void drawEmptyLabel(Canvas canvas, int spectrumHeight) {
        paint.setColor(Color.rgb(130, 145, 155));
        paint.setTextSize(28.0f);
        paint.setStyle(Paint.Style.FILL);
        canvas.drawText("Waiting for network spectrum...", 14.0f, spectrumHeight / 2.0f, paint);
    }

    private void updateWaterfallRow() {
        if (waterfall == null || waterfall.getWidth() <= 0 || waterfall.getHeight() <= 0) {
            return;
        }
        int width = waterfall.getWidth();
        int height = waterfall.getHeight();
        int requiredPixels = width * height;
        if (waterfallPixels.length != requiredPixels) {
            waterfallPixels = new int[requiredPixels];
            for (int i = 0; i < waterfallPixels.length; ++i) {
                waterfallPixels[i] = Color.rgb(4, 8, 12);
            }
        }
        if (height > 1) {
            System.arraycopy(waterfallPixels, 0, waterfallPixels, width, width * (height - 1));
        }

        int count = Math.min(frequencies.length, magnitudes.length);
        if (count <= 0) {
            for (int x = 0; x < width; ++x) {
                waterfallPixels[x] = Color.rgb(4, 8, 12);
            }
            waterfall.setPixels(waterfallPixels, 0, width, 0, 0, width, height);
            return;
        }

        for (int x = 0; x < width; ++x) {
            double frequency = visibleMinFrequency +
                    (x / (double) Math.max(1, width - 1)) *
                            (visibleMaxFrequency - visibleMinFrequency);
            int index = nearestIndexForFrequency(frequency, count);
            waterfallPixels[x] = waterfallColor(displayMagnitudeAt(index, count));
        }
        waterfall.setPixels(waterfallPixels, 0, width, 0, 0, width, height);
    }

    private float magnitudeToY(float value, int spectrumHeight) {
        float normalized = normalizedLevel(value);
        return spectrumHeight - normalized * (spectrumHeight - 12.0f) - 6.0f;
    }

    private int waterfallColor(float value) {
        float normalized = normalizedLevel(value);
        float position = normalized * (WATERFALL_COLORS.length - 1);
        int index = Math.max(0, Math.min(WATERFALL_COLORS.length - 2, (int) Math.floor(position)));
        float t = position - index;
        return lerpColor(WATERFALL_COLORS[index], WATERFALL_COLORS[index + 1], t);
    }

    private float normalizedLevel(float value) {
        if (!Float.isFinite(value) || levelMax <= levelMin) {
            return 0.0f;
        }
        return Math.max(0.0f, Math.min(1.0f, (value - levelMin) / (levelMax - levelMin)));
    }

    private float frequencyToX(double frequency, int width) {
        double normalized = (frequency - visibleMinFrequency) /
                Math.max(1.0, visibleMaxFrequency - visibleMinFrequency);
        normalized = Math.max(0.0, Math.min(1.0, normalized));
        return (float) (normalized * (width - 1.0));
    }

    private double frequencyAtX(float x, int width) {
        double normalized = x / Math.max(1.0, width - 1.0);
        normalized = Math.max(0.0, Math.min(1.0, normalized));
        return visibleMinFrequency + normalized * (visibleMaxFrequency - visibleMinFrequency);
    }

    private String formatFrequencyLabel(double frequency) {
        double mhz = frequency / 1_000_000.0;
        double spanMhz = Math.abs(visibleMaxFrequency - visibleMinFrequency) / 1_000_000.0;
        if (spanMhz < 0.2) {
            return String.format(Locale.US, "%.5f", mhz);
        }
        if (spanMhz < 2.0) {
            return String.format(Locale.US, "%.4f", mhz);
        }
        return String.format(Locale.US, "%.3f", mhz);
    }

    private String ellipsize(String label, int maxWidth) {
        if (label == null || label.isEmpty() || paint.measureText(label) <= maxWidth) {
            return label == null ? "" : label;
        }
        String ellipsis = "...";
        int end = label.length();
        while (end > 0 && paint.measureText(label.substring(0, end) + ellipsis) > maxWidth) {
            --end;
        }
        return end <= 0 ? "" : label.substring(0, end) + ellipsis;
    }

    private int frequencyScaleHeight() {
        return Math.max(34, dp(36));
    }

    private int spectrumHeightFor(int totalHeight) {
        int scaleHeight = frequencyScaleHeight();
        int available = Math.max(1, totalHeight - scaleHeight);
        int minSpectrum = dp(96);
        int minWaterfall = dp(80);
        int spectrumHeight = Math.max(minSpectrum, (int) (available * 0.40f));
        if (available - spectrumHeight < minWaterfall) {
            spectrumHeight = Math.max(1, available - minWaterfall);
        }
        return spectrumHeight;
    }

    private int lerpColor(int a, int b, float t) {
        t = Math.max(0.0f, Math.min(1.0f, t));
        int ar = Color.red(a);
        int ag = Color.green(a);
        int ab = Color.blue(a);
        int br = Color.red(b);
        int bg = Color.green(b);
        int bb = Color.blue(b);
        return Color.rgb(
                (int) (ar + (br - ar) * t),
                (int) (ag + (bg - ag) * t),
                (int) (ab + (bb - ab) * t));
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private float displayMagnitudeAt(int frequencyIndex, int count) {
        if (count <= 0 || magnitudes.length == 0) {
            return -160.0f;
        }
        int index = Math.max(0, Math.min(magnitudes.length - 1, frequencyIndex));
        float value = magnitudes[index];
        return Float.isFinite(value) ? value : -160.0f;
    }

    private int nearestIndexForFrequency(double frequency, int count) {
        if (count <= 1) {
            return 0;
        }
        if (frequencies.length < count ||
                frequencies[0] >= frequencies[count - 1]) {
            double normalized = (frequency - visibleMinFrequency) /
                    Math.max(1.0, visibleMaxFrequency - visibleMinFrequency);
            return Math.max(0, Math.min(count - 1,
                    (int) Math.round(normalized * (count - 1))));
        }
        int low = 0;
        int high = count - 1;
        while (low <= high) {
            int mid = (low + high) >>> 1;
            if (frequencies[mid] < frequency) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        if (low <= 0) {
            return 0;
        }
        if (low >= count) {
            return count - 1;
        }
        return Math.abs(frequencies[low] - frequency) < Math.abs(frequencies[low - 1] - frequency)
                ? low
                : low - 1;
    }

    private void updateVisibleRange(double oldCenter, double oldSpan) {
        double lowerLimit = rangeLowerLimit();
        double upperLimit = rangeUpperLimit();
        double fullSpan = Math.max(1.0, upperLimit - lowerLimit);
        if (!userZoomed || !Double.isFinite(oldSpan) || oldSpan <= 0.0) {
            visibleMinFrequency = lowerLimit;
            visibleMaxFrequency = upperLimit;
            userZoomed = false;
            return;
        }
        double span = Math.max(fullSpan / 100.0, Math.min(fullSpan, oldSpan));
        double center = Double.isFinite(oldCenter) ? oldCenter : (lowerLimit + upperLimit) * 0.5;
        setVisibleRange(center, span);
    }

    private void setVisibleRange(double center, double span) {
        double lowerLimit = rangeLowerLimit();
        double upperLimit = rangeUpperLimit();
        double fullSpan = Math.max(1.0, upperLimit - lowerLimit);
        double clampedSpan = Math.max(fullSpan / 100.0, Math.min(fullSpan, span));
        double half = clampedSpan * 0.5;
        double clampedCenter = Math.max(lowerLimit + half, Math.min(upperLimit - half, center));
        visibleMinFrequency = clampedCenter - half;
        visibleMaxFrequency = clampedCenter + half;
        userZoomed = clampedSpan < fullSpan * 0.99;
    }

    private double rangeLowerLimit() {
        return allowNegativeFrequencies ? minFrequency : Math.max(0.0, minFrequency);
    }

    private double rangeUpperLimit() {
        double lowerLimit = rangeLowerLimit();
        return Math.max(lowerLimit + 1.0, maxFrequency);
    }

    private void applyPan(float x, float y) {
        float dx = x - lastPanX;
        if (Math.abs(x - downX) > tapSlopPx || Math.abs(y - downY) > tapSlopPx) {
            gestureMoved = true;
        }
        if (Math.abs(dx) > 0.5f && userZoomed) {
            double span = visibleMaxFrequency - visibleMinFrequency;
            double deltaHz = -dx / Math.max(1.0, getWidth() - 1.0) * span;
            if (panTuneRequestListener != null) {
                panTuneRequestListener.onPanTuneRequested(deltaHz);
            } else {
                setVisibleRange((visibleMinFrequency + visibleMaxFrequency) * 0.5 + deltaHz, span);
                invalidate();
            }
        } else if (Math.abs(dx) > 0.5f && !userZoomed) {
            double span = visibleMaxFrequency - visibleMinFrequency;
            double deltaHz = -dx / Math.max(1.0, getWidth() - 1.0) * span;
            if (panTuneRequestListener != null) {
                panTuneRequestListener.onPanTuneRequested(deltaHz);
            }
        }
        lastPanX = x;
    }

    private void applyPinchZoom(MotionEvent event) {
        float distance = pointerDistance(event);
        if (pinchStartDistance <= 1.0f || distance <= 1.0f) {
            return;
        }
        double scale = distance / pinchStartDistance;
        double nextSpan = pinchStartSpan / Math.max(0.1, scale);
        double oldVisibleSpan = Math.max(1.0, visibleMaxFrequency - visibleMinFrequency);
        double focusFraction = pinchStartFocusX / Math.max(1.0, getWidth() - 1.0);
        double nextCenter = pinchFocusFrequency + (0.5 - focusFraction) * nextSpan;
        setVisibleRange(nextCenter, nextSpan);
        if (Math.abs(oldVisibleSpan - (visibleMaxFrequency - visibleMinFrequency)) > 0.5) {
            invalidate();
            notifyZoomProgressChanged();
        }
    }

    private void notifyZoomProgressChanged() {
        if (zoomProgressListener != null) {
            zoomProgressListener.onZoomProgressChanged(zoomProgress());
        }
    }

    private float pointerDistance(MotionEvent event) {
        if (event.getPointerCount() < 2) {
            return 0.0f;
        }
        float dx = event.getX(0) - event.getX(1);
        float dy = event.getY(0) - event.getY(1);
        return (float) Math.sqrt(dx * dx + dy * dy);
    }

    private float pointerCenterX(MotionEvent event) {
        if (event.getPointerCount() < 2) {
            return event.getX();
        }
        return (event.getX(0) + event.getX(1)) * 0.5f;
    }
}
