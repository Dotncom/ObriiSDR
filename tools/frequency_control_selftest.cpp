#include "frequencycontrol.h"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QTimer>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
QComboBox *combo(FrequencyControl *control, const QString &entry) {
    for (auto *candidate : control->findChildren<QComboBox *>()) {
        if (candidate->findText(entry) >= 0) return candidate;
    }
    throw std::runtime_error("Missing copied preset");
}
void activate(QComboBox *control, const QString &entry) {
    const int index = control->findText(entry);
    require(index >= 0, "Preset entry not found");
    control->setCurrentIndex(index);
    QMetaObject::invokeMethod(control, "activated", Qt::DirectConnection, Q_ARG(int, index));
}
void tick(QTimer &timer) {
    QMetaObject::invokeMethod(&timer, "timeout", Qt::DirectConnection);
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QWidget owner;
        QTimer timer;
        auto *source = new FrequencyControl;
        source->setRangeHz(1000000, 6000000000.0);
        source->setStepPresets({{"12.5 kHz", 12500}, {"25 kHz", 25000}});
        source->setValuePresets({{"VHF", 145500000}, {"UHF", 433500000}});
        source->setSelectedStepName("25 kHz");
        source->setSelectedUnitIndex(2);
        source->setSelectedValuePresetName("VHF");
        source->setValueHz(145500000);
        int applies = 0;
        bool reject = false;
        auto *copy = source->createLinkedControl(&owner, &timer, [&]() {
            ++applies;
            if (reject) source->setValueHz(145500000);
        });
        int sourceCommits = 0;
        QObject::connect(source, &FrequencyControl::valueCommitted, [&]() { ++sourceCommits; });
        require(copy->valueHz() == source->valueHz() && copy->selectedStepName() == "25 kHz" &&
                copy->selectedValuePresetName() == "VHF", "Initial configuration differs");
        require(combo(copy, "VHF")->isEnabled(), "Presets disabled in linked editor");
        copy->setValueHz(0);
        require(copy->valueHz() == 1000000, "Receiver lower bound not copied");
        copy->setValueHz(9000000000.0);
        require(copy->valueHz() == 6000000000.0, "Receiver upper bound not copied");
        tick(timer);
        require(copy->valueHz() == source->valueHz() && applies == 0, "Passive sync retunes receiver");
        owner.resize(400, 200);
        owner.show();
        auto *spin = copy->findChild<QDoubleSpinBox *>();
        spin->setFocus();
        QApplication::processEvents();
        auto *edit = spin->findChild<QLineEdit *>();
        edit->setText("146.123");
        edit->setModified(true);
        source->setValueHz(147000000);
        tick(timer);
        require(edit->text() == "146.123", "Passive sync erased an unfinished edit");
        edit->setModified(false);
        tick(timer);
        require(copy->valueHz() == source->valueHz(), "Idle focused editor kept a stale frequency");
        owner.hide();
        activate(combo(copy, "UHF"), "UHF");
        require(source->valueHz() == 433500000 && source->selectedValuePresetName() == "UHF" &&
                applies == 1 && sourceCommits == 0, "Preset did not apply exactly once");
        combo(copy, "12.5 kHz")->setCurrentIndex(0);
        combo(copy, "Hz")->setCurrentIndex(1);
        require(source->selectedStepName() == "12.5 kHz" && source->selectedUnitIndex() == 1 &&
                applies == 1, "Step/unit selection lost or caused a retune");
        source->setRangeHz(2500000, 7250000000.0);
        source->setValuePresets({{"New preset", 7000000000.0}});
        source->setStepPresets({{"2 kHz", 2000}});
        tick(timer);
        require(combo(copy, "New preset")->count() == 2 && copy->selectedStepName() == "2 kHz",
                "Preset edit did not reach open editor");
        copy->setValueHz(1);
        require(copy->valueHz() == 2500000, "Live receiver range change ignored");
        activate(combo(copy, "New preset"), "New preset");
        require(source->valueHz() == 7000000000.0 && applies == 2, "New range/preset rejected");
        reject = true;
        activate(combo(copy, "New preset"), "New preset");
        require(copy->valueHz() == 145500000 && applies == 3, "Rejected request not reflected");
        source->setEnabled(false);
        tick(timer);
        require(!copy->isEnabled(), "Disabled state not synchronized");
        delete source;
        tick(timer);
        require(!copy->isEnabled(), "Deleted source left editor enabled");

        FrequencyControl bandwidth;
        bandwidth.setRangeHz(10, 400000);
        bandwidth.setValuePresets({{"Voice", 12500}, {"Broadcast", 200000}});
        bandwidth.setValueHz(12500);
        int bandwidthApplies = 0;
        auto *first = bandwidth.createLinkedControl(&owner, &timer, [&]() { ++bandwidthApplies; });
        auto *second = bandwidth.createLinkedControl(&owner, &timer, [&]() { ++bandwidthApplies; });
        activate(combo(first, "Broadcast"), "Broadcast");
        tick(timer);
        require(bandwidth.valueHz() == 200000 && second->valueHz() == 200000 && bandwidthApplies == 1,
                "Demodulator and channel filter editors diverged");
        bandwidth.setValuePresets({});
        tick(timer);
        require(!combo(first, "Preset")->isEnabled() && first->selectedValuePresetName().isEmpty(),
                "Deleted presets remained available");
        std::cout << "Frequency controls: ranges, presets, live sync, single apply and lifetime passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
