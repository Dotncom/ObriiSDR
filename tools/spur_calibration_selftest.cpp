#ifdef NDEBUG
#undef NDEBUG
#endif
#include "spurcalibrationprofile.h"
#include "fobosfrequencylimits.h"
#include "spurlinecalibrator.h"
#include "spurmaskresolution.h"
#include "spectrumordering.h"
#include "fobosifselection.h"
#include <QJsonDocument>
#include <QFile>
#include <random>
#include <cassert>
#include <iostream>

int main(int argc, char **argv) {
    // Optional firmware is never assumed from a saved preference.
    assert(fobosRequestedIfTestMode(3, 0, 50e6) == 0); // stock / missing SDK export
    assert(fobosRequestedIfTestMode(3, 1, 50e6) == 0); // revision4
    assert(fobosRequestedIfTestMode(3, 3, 50e6) == 0); // revision5 is row3-only
    assert(fobosRequestedIfTestMode(3, 7, 50e6) == 3); // revision6
    assert(fobosRequestedIfTestMode(3, 3, 16e6) == 0);
    assert(fobosRequestedIfTestMode(2, 1, 50e6) == 0);
    assert(fobosRequestedIfTestMode(2, 7, 50e6) == 2);
    assert(fobosRequestedIfTestMode(2, 0, 50e6) == 0);
    assert(obriiIfAutoUses2450(2736.5e6, 50e6));
    assert(obriiIfAutoUses2450(2744.56391e6, 50e6));
    assert(!obriiIfAutoUses2450(2747.5e6, 50e6));
    assert(!obriiIfAutoUses2450(2744.56391e6, 16e6));
    const auto centers = SpurCalibrationProfile::sweepCenters(50e6, 6900e6, 50e6);
    assert(centers.size() == 275 && centers.front() == 50e6 && centers.back() == 6900e6);
    for (int i = 1; i < centers.size(); ++i)
        assert(centers[i] > centers[i - 1] && centers[i] - centers[i - 1] <= 25e6);
    const auto uneven = SpurCalibrationProfile::sweepCenters(50e6, 6911e6, 50e6);
    assert(uneven.back() == 6911e6 && uneven[uneven.size() - 2] == 6900e6);
    assert(SpurCalibrationProfile::sweepCenters(50e6, 50e6, 50e6).size() == 1);
    assert(SpurCalibrationProfile::sweepCenters(50e6, NAN, 50e6).isEmpty());
    assert(SpurCalibrationProfile::sweepCenters(50e6, 6900e6, 0).isEmpty());
    assert(fobosAgileHasKnown6900MhzLimit("3.0.0", "3.2.0 Jun 18 2026"));
    assert(!fobosAgileHasKnown6900MhzLimit("4.0.0", "3.2.0"));

    SpurCalibrationProfile p;
    p.context = {{"serial", "receiver A"}, {"sampleRate", 50e6}, {"vga", 16}, {"correctedIf", true}};
    p.stepHz = 25e6;
    p.points = {{50e6, {{-1e6, 500, 20, 12}}}, {75e6, {{2e6, 500, 18, 11}}}, {100e6, {}}};
    assert(p.maskAt(50e6, p.context)->at(0).offsetHz == -1e6);
    assert(p.maskAt(60e6, p.context)->at(0).offsetHz == -1e6);
    assert(p.maskAt(65e6, p.context)->at(0).offsetHz == 2e6); // no global union
    assert(p.maskAt(100e6, p.context)->isEmpty()); // measured clean band is meaningful
    assert(!p.maskAt(49e6, p.context) && !p.maskAt(101e6, p.context));
    auto changed = p.context; changed.insert("vga", 17);
    assert(p.maskAt(50e6, changed));
    changed.insert("lna", 1); changed.insert("fftLength", 32768);
    assert(p.maskAt(50e6, changed));
    changed.insert("sampleRate", 16e6);
    assert(!p.maskAt(50e6, changed));
    changed = p.context; changed.insert("correctedIf", false);
    assert(!p.compatibleWith(changed));
    changed = p.context; changed.insert("row3If2450", true);
    assert(!p.compatibleWith(changed));
    changed.insert("row3If2450", false);
    assert(p.compatibleWith(changed));
    changed.insert("row3AutoPolicy", 1);
    assert(!p.compatibleWith(changed));
    changed.remove("row3AutoPolicy");
    changed = p.context; changed.insert("mixerBiasProfile", 0);
    assert(p.compatibleWith(changed)); // missing native profile remains compatible
    changed.insert("mixerBiasProfile", 2);
    assert(!p.compatibleWith(changed));
    auto alternateMixer = p; alternateMixer.context = changed;
    assert(alternateMixer.compatibleWith(changed));
    changed.insert("mixerBiasProfile", 3);
    assert(!alternateMixer.compatibleWith(changed));
    assert(!alternateMixer.compatibleWith(p.context));
#ifdef OBRII_LOCAL_FOBOS_IF_HEADER
    SpurCalibrationProfile automatic;
    automatic.context = p.context; automatic.context.insert("globalIfPolicy", 2);
    changed=p.context; changed.insert("globalIfMode",1); assert(!p.compatibleWith(changed));
    changed=p.context; changed.insert("globalIfMode",2); assert(!p.compatibleWith(changed));
    changed=p.context; changed.insert("globalIfPolicy",1); assert(!automatic.compatibleWith(changed));
    assert(obriiIfAutoMhz(4700e6,50e6)==2450);
    assert(obriiIfAutoMhz(5875e6,50e6)==2450);
    assert(obriiIfAutoMhz(2446e6,50e6)==0);
    assert(obriiIfAutoMhz(4500e6,50e6,false)==2450);
    assert(obriiIfAutoMhz(4500e6,50e6,true)==2350);
    automatic.stepHz = 25e6;
    automatic.points = {{2725e6, {{1e6, 500, 20, 12}}},
                        {2742e6, {{2e6, 500, 20, 12}}},
                        {2750e6, {{3e6, 500, 20, 12}}}};
    assert(automatic.maskAt(2735e6, automatic.context)->at(0).offsetHz == 1e6);
    assert(automatic.maskAt(2736.5e6, automatic.context)->at(0).offsetHz == 2e6);
    assert(automatic.maskAt(2747.49e6, automatic.context)->at(0).offsetHz == 2e6);
    assert(automatic.maskAt(2747.5e6, automatic.context)->at(0).offsetHz == 3e6);
    automatic.points.removeAt(1);
    assert(!automatic.maskAt(2744e6, automatic.context));
    // An absolute-RF coincidence across different IFs must not add another mask.
    automatic.points = {{2735e6, {{10e6, 500, 20, 12}}}, {2740e6, {{5e6, 500, 20, 12}}}};
    const auto autoMask = automatic.maskFor(2738e6, automatic.context);
    assert(autoMask.size() == 1 && autoMask[0].offsetHz == 5e6);
    {
        using FobosRuntimeIf::Frame;
        const auto withCustom = [](QJsonObject context, const Frame &frame) {
            context.insert("globalIfPolicy", static_cast<int>(FobosRuntimeIf::PolicyVersion));
            context.insert("globalIfCustomRulesActive", true);
            context.insert("globalIfCustomRulesCrc", static_cast<double>(FobosRuntimeIf::rulesCrc(frame)));
            context.insert("globalIfCustomRules", FobosRuntimeIf::frameToJson(frame));
            return context;
        };
        Frame customFrame;
        assert(FobosRuntimeIf::frameForRules({{2740000000ULL,2741000000ULL,2450}}, &customFrame));
        SpurCalibrationProfile customProfile;
        customProfile.context = withCustom(p.context, customFrame); customProfile.stepHz = 25e6;
        customProfile.points = {{2739e6, {{10e6,500,20,12}}},
                                {2740.5e6, {{8.5e6,500,20,12}}},
                                {2742e6, {{3e6,500,20,12}}}};
        // The nearest point may use another IF. Half-open custom boundaries,
        // not the built-in LO6 interval, determine which measurements qualify.
        assert(customProfile.maskAt(2739.9e6,customProfile.context)->at(0).offsetHz == 10e6);
        assert(customProfile.maskAt(2740e6,customProfile.context)->at(0).offsetHz == 8.5e6);
        assert(customProfile.maskAt(2741e6,customProfile.context)->at(0).offsetHz == 3e6);
        const auto customMask = customProfile.maskFor(2740.25e6,customProfile.context);
        assert(customMask.size()==1 && customMask[0].offsetHz==8.5e6); // no cross-IF RF-fixed inference
        auto missing = customProfile; missing.points.removeAt(1);
        assert(!missing.maskAt(2740.25e6,missing.context));
        assert(!customProfile.compatibleWith(automatic.context)); // built-in qualification is distinct
        auto badCrc = customProfile.context;
        badCrc.insert("globalIfCustomRulesCrc", static_cast<double>(customFrame[3]^1u));
        assert(!customProfile.compatibleWith(badCrc));
        auto corrupt = customProfile; corrupt.context=badCrc;
        assert(!corrupt.maskAt(2740.25e6,corrupt.context));
        assert(SpurCalibrationProfile::fromJson(corrupt.toJson()).points.isEmpty());
        Frame otherFrame;
        assert(FobosRuntimeIf::frameForRules({{2740000000ULL,2741000000ULL,2350}}, &otherFrame));
        assert(!customProfile.compatibleWith(withCustom(p.context,otherFrame)));
        const auto customRestored=SpurCalibrationProfile::fromJson(customProfile.toJson());
        assert(customRestored.compatibleWith(customProfile.context));
        assert(customRestored.maskAt(2740e6,customProfile.context)->at(0).offsetHz==8.5e6);
        Frame emptyFrame; assert(FobosRuntimeIf::frameForRules({},&emptyFrame) && emptyFrame[3]==0);
        SpurCalibrationProfile emptyCustom;
        emptyCustom.context=withCustom(p.context,emptyFrame); emptyCustom.stepHz=25e6;
        emptyCustom.points={{2735e6,{{1e6,500,20,12}}},{2742e6,{{2e6,500,20,12}}},{2750e6,{}}};
        assert(emptyCustom.maskAt(2736.5e6,emptyCustom.context)->at(0).offsetHz==1e6); // base2350, not built-in2450
        assert(!emptyCustom.compatibleWith(automatic.context)); // CRC0 still means a distinct custom table
        emptyCustom.points={{2300.4e6,{{1e6,500,20,12}}},{2300.6e6,{{2e6,500,20,12}}}};
        assert(emptyCustom.maskAt(2300.5e6,emptyCustom.context)->at(0).offsetHz==2e6); // RF bypass is another path
        auto manualBypass=emptyCustom; manualBypass.context=p.context; manualBypass.context.insert("globalIfMode",1);
        assert(manualBypass.maskAt(2300.5e6,manualBypass.context)->at(0).offsetHz==2e6);
        assert(FobosRuntimeIf::chooseEffectiveIf(emptyFrame,7500e6,50e6,true,3)==2350);
        assert(FobosRuntimeIf::chooseEffectiveIf(emptyFrame,7750e6,50e6,true,2)==2450);
        auto oldVersion=p.toJson(); oldVersion.insert("version",1);
        assert(SpurCalibrationProfile::fromJson(oldVersion).compatibleWith(p.context));
    }
#endif

    changed.insert("correctedIf", false);
    assert(!p.maskAt(50e6, changed));
    changed = p.context; changed.insert("serial", "receiver B");
    assert(!p.maskAt(50e6, changed));
    SpurCalibrationProfile rfFixed;
    rfFixed.context = p.context; rfFixed.stepHz = 25e6;
    rfFixed.points = {{75e6, {{15e6, 500, 20, 12}, {-10e6, 500, 20, 12}}},
                      {100e6, {{-10e6, 500, 20, 12}}}};
    // The second line has the same baseband offset, so it cannot be used as
    // evidence for an RF-fixed spur at90MHz.
    assert(rfFixed.maskFor(80e6, p.context).size() == 2);
    rfFixed.points[1].entries[0].offsetHz = -10e6; rfFixed.points[0].entries.removeLast();
    const auto moved = rfFixed.maskFor(80e6, p.context);
    assert(std::any_of(moved.begin(), moved.end(), [](const auto &e) { return std::abs(e.offsetHz - 10e6) < 1; }));
    const auto encoded = QJsonDocument(p.toJson()).toJson();
    const auto restored = SpurCalibrationProfile::fromJson(QJsonDocument::fromJson(encoded).object());
    assert(restored.points.size() == 3 && restored.context == p.context);
    assert(restored.maskAt(75e6, p.context)->at(0).hits == 11);
    auto malformed = p.toJson(); malformed.insert("stepHz", -1);
    assert(SpurCalibrationProfile::fromJson(malformed).points.isEmpty());
    malformed = p.toJson(); malformed.insert("version", 999);
    assert(SpurCalibrationProfile::fromJson(malformed).points.isEmpty());
    auto unordered = p; std::swap(unordered.points[0], unordered.points[1]);
    assert(SpurCalibrationProfile::fromJson(unordered.toJson()).points.isEmpty());
    auto invalid = p; invalid.points[0].entries[0].widthHz = 1e9;
    assert(SpurCalibrationProfile::fromJson(invalid.toJson()).points.isEmpty());

    SpurCalibrationFrameGate gate{8, 0, 150e6};
    assert(!gate.accepts(7, 100, 150e6, true)); // pre-retune IQ
    assert(!gate.accepts(8, 100, 150e6, false)); // settling
    assert(!gate.accepts(8, 100, 175e6, true)); // worker from another center
    assert(gate.accepts(8, 100, 150e6, true));
    assert(!gate.accepts(8, 100, 150e6, true)); // duplicate FFT
    assert(!gate.accepts(8, 99, 150e6, true));
    assert(gate.accepts(8, 200, 150e6, true));
    gate = {9, 0, 175e6};
    assert(!gate.accepts(8, 300, 150e6, true));
    assert(gate.accepts(9, 400, 175e6, true));

    constexpr double sampleRate = 50e6;
    const std::vector<double> lines = {-20e6, -16.666666e6, -10e6, -3.765432e6,
        1.23e6, 5e6, 10e6, 16.666666e6, 20e6};
    std::mt19937 random(1701);
    std::normal_distribution<double> noise(0, 5.5);
    auto makeFrame = [&](int n, double gain, bool noisy, double windowSigma = 0.5) {
        std::vector<float> data(n);
        for (int i = 0; i < n; ++i) {
            const double hz = (i - n / 2) * sampleRate / n;
            double db = -115 + gain + 20 * std::exp(-0.5 * std::pow((hz + 7e6) / 150000.0, 2));
            if (noisy) db += noise(random);
            double power = std::pow(10.0, db / 10);
            for (double line : lines) {
                const double sigma = (std::max)(line == 1.23e6 ? 1300.0 : 100.0, sampleRate / n * windowSigma);
                power += std::pow(10.0, (-83 + gain) / 10) * std::exp(-0.5 * std::pow((hz - line) / sigma, 2));
            }
            data[(i + n / 2) % n] = static_cast<float>(10 * std::log10(power));
        }
        return data;
    };
    SpurLineCalibrator calibrator;
    for (int frame = 0; frame < 12; ++frame) assert(calibrator.addFrame(makeFrame(262144, 0, true), sampleRate));
    const auto learned = calibrator.finish();
    for (double line : lines) {
        assert(std::any_of(learned.begin(), learned.end(), [&](const auto &e) {
            return std::abs(e.offsetHz - line) <= e.widthHz * 0.5;
        }));
    }
    auto windowMask = learned;
    adaptSpurMaskToResolution(windowMask, sampleRate, 32768, FFT_WINDOW_FLAT_TOP);
    auto flatTop = makeFrame(32768, -12, false, 1.25);
    assert(applyCalibratedSpurLines(flatTop, windowMask, sampleRate) >= static_cast<int>(lines.size()));
    for (double line : lines) {
        const int bin = static_cast<int>(std::llround(line / (sampleRate / 32768))) + 32768 / 2;
        assert(flatTop[(bin + 32768 / 2) % 32768] < -122);
    }
    adaptSpurMaskToResolution(windowMask, sampleRate, 1024, FFT_WINDOW_RECTANGULAR);
    assert(windowMask.isEmpty()); // unresolved bins must not erase broad VGA humps
    SpurLineCalibrator transient;
    for (int frame = 0; frame < 12; ++frame) {
        std::vector<float> data(65536, -115);
        if (frame == 5) data[17000] += 90; // one transient must not become a persistent spur
        assert(transient.addFrame(data, sampleRate));
    }
    assert(transient.finish().isEmpty());
    transient.reset();
    std::vector<float> wide(65536, -115);
    for (int i = 0; i < 65536; ++i) {
        const double hz = (i - 32768) * sampleRate / 65536;
        wide[(i + 32768) % 65536] += static_cast<float>(40 * std::exp(-0.5 * std::pow((hz - 8e6) / 25000, 2)));
    }
    for (int frame = 0; frame < 12; ++frame) assert(transient.addFrame(wide, sampleRate));
    assert(transient.finish().isEmpty()); // >20kHz width despite a very prominent maximum
    assert(learned.size() < 32); // random noise must not fill the profile cap
    assert(std::none_of(learned.begin(), learned.end(), [](const auto &e) { return std::abs(e.offsetHz + 7e6) < 50000; }));
    for (int n : {32768, 1048576}) {
        auto live = makeFrame(n, -12, false);
        const int hump = static_cast<int>(std::llround(-7e6 / (sampleRate / n))) + n / 2;
        const float before = live[(hump + n / 2) % n];
        assert(applyCalibratedSpurLines(live, learned, sampleRate) >= static_cast<int>(lines.size()));
        assert(live[(hump + n / 2) % n] == before); // broad VGA hump is untouched
        for (double line : lines) {
            const int bin = static_cast<int>(std::llround(line / (sampleRate / n))) + n / 2;
            assert(live[(bin + n / 2) % n] < -122); // adaptive local floor after gain/FFT changes
        }
    }
    assert(calibrator.frameCount() == 12);
    auto invalidFrame = makeFrame(262144, 0, false); invalidFrame[12] = NAN;
    assert(!calibrator.addFrame(invalidFrame, sampleRate) && calibrator.frameCount() == 12);
    calibrator.reset(); assert(calibrator.finish().isEmpty());
    std::vector<float> thresholdFrame(65536, -115.0f);
    thresholdFrame[4000] += 5.0f;
    for (int i = 0; i < 12; ++i) calibrator.addFrame(thresholdFrame, sampleRate);
    assert(calibrator.finish(6).isEmpty());
    assert(calibrator.finish(3).size() == 1);
    assert(calibrator.finish(15).isEmpty());
    const std::vector<float> rawOrder{0,1,2,3,4,5,6,7};
    assert(spectrumInFrequencyOrder(rawOrder) == std::vector<float>({4,5,6,7,0,1,2,3}));
    assert(rawOrder.front() == 0); // calibration conversion must not mutate the live FFT
    if (argc > 1) {
        QFile file(QString::fromLocal8Bit(argv[1])); assert(file.open(QIODevice::ReadOnly));
        const auto real = SpurCalibrationProfile::fromJson(QJsonDocument::fromJson(file.readAll()).object());
        auto current = real.context; current.insert("lna", 1); current.insert("vga", 8); current.insert("fftLength", 32768);
        const auto *mask = real.maskAt(83721624.85065709, current);
        assert(mask && !mask->isEmpty());
        std::cout << "Existing user profile: " << real.points.size() << " centers, " << mask->size() << " nearest lines; gain/FFT adaptation PASS\n";
    }
    std::cout << "PASS: sweep/persistence, compatible gain/FFT, distinct Fs/IF/receiver, RF-fixed tracking, fresh IQ, averaged narrow lines, broad-hump exclusion and adaptive correction\n";
}
