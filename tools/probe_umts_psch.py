"""Offline P-SCH hypothesis test; does not decode traffic or access a receiver.

PSC: ETSI TS 125 213, 5.2.3.1. Slot: TS 125 211, 5.3.3.5.
The search score is a slot-folded matched-filter power peak / median.
An isolated peak alone is not a UMTS identification; check held-out data,
the 2560-chip repetition, frequency sensitivity, and shuffled-code control.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import sys
from fractions import Fraction

parser = argparse.ArgumentParser()
parser.add_argument('file', type=Path)
parser.add_argument('--deps', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
sys.path.insert(0, str(args.deps.resolve()))
os.environ['OPENBLAS_NUM_THREADS'] = '1'
os.environ['OMP_NUM_THREADS'] = '1'
if os.name == 'nt':
    ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
import numpy as np
from scipy import signal

meta = json.loads(args.file.with_suffix('.json').read_text())
rate = float(meta['sampleRate'])
rf = float(meta['centerFrequency'])
raw = np.memmap(args.file, dtype='<i2', mode='r').reshape(-1, 2)
a = np.array([1,1,1,1,1,1,-1,-1,1,-1,1,-1,1,-1,-1,1], dtype=np.float32)
b = np.array([1,1,1,-1,-1,1,-1,-1,1,1,1,-1,1,-1,1,1], dtype=np.float32)
psc = np.kron(b, a).astype(np.complex64) * (1+1j)


def rrc(beta=.22, samples_per_chip=2, span=12):
    t = np.arange(-span*samples_per_chip, span*samples_per_chip+1)/samples_per_chip
    h = np.empty_like(t)
    for i, v in enumerate(t):
        if abs(v) < 1e-12:
            h[i] = 1+beta*(4/np.pi-1)
        elif abs(abs(4*beta*v)-1) < 1e-10:
            h[i] = beta/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*beta))+(1-2/np.pi)*np.cos(np.pi/(4*beta)))
        else:
            h[i] = (np.sin(np.pi*v*(1-beta))+4*beta*v*np.cos(np.pi*v*(1+beta)))/(np.pi*v*(1-(4*beta*v)**2))
    return (h/np.linalg.norm(h)).astype(np.float32)


pulse = rrc()


def template(chips):
    up = np.zeros(len(chips)*2, dtype=np.complex64)
    up[::2] = chips
    return signal.convolve(up, pulse).astype(np.complex64)


reference = template(psc)
rng = np.random.default_rng(20261103)
wrong = template(rng.permutation(psc))


def chunk(seconds, duration=.08):
    start = round(seconds*rate)
    x = np.asarray(raw[start:start+round(duration*rate)], dtype=np.float32)/32768
    z = x[:,0]+1j*x[:,1]
    # Resample the selected vicinity to 7.68 MS/s at a fixed 2142.4 MHz.
    n = np.arange(len(z))
    z *= np.exp(-2j*np.pi*(2142400000-rf)/rate*n).astype(np.complex64)
    ratio = Fraction(7680000/round(rate)).limit_denominator(1000)
    return signal.resample_poly(z, ratio.numerator, ratio.denominator)[256:-256]


def score(z, offset, ref=reference):
    z = z*np.exp(-2j*np.pi*offset/7680000*np.arange(len(z))).astype(np.complex64)
    corr = signal.fftconvolve(z, np.conj(ref[::-1]), mode='valid')
    power = abs(corr)**2
    slots = len(power)//5120
    folded = power[:slots*5120].reshape(slots,5120).mean(axis=0)
    phase = int(folded.argmax())
    baseline = float(np.median(folded))
    # A narrow timing peak must persist across independent slots.
    ratio = float(folded[phase]/baseline)
    individual = power[:slots*5120].reshape(slots,5120)
    slot_ratios = individual[:, phase]/np.median(individual, axis=1)
    return {'offsetHz':float(offset), 'centerHz':2142400000+float(offset),
            'foldedPeakOverMedian':ratio, 'slotPeakSample':phase,
            'slots':slots, 'medianIndividualSlotRatio':float(np.median(slot_ratios)),
            'fractionSlotsAbove5x':float(np.mean(slot_ratios>5))}, folded


training = chunk(2)
search = [score(training, offset)[0] for offset in range(-200000,200001,10000)]
best = max(search, key=lambda x:x['foldedPeakOverMedian'])
fine = [score(training, offset)[0] for offset in np.arange(best['offsetHz']-10000,best['offsetHz']+10001,1000)]
best = max(fine, key=lambda x:x['foldedPeakOverMedian'])
results = []
profiles = {}
for t in [2,30,60,85]:
    z = training if t == 2 else chunk(t)
    measured, folded = score(z, best['offsetHz'])
    control, control_folded = score(z, best['offsetHz'], wrong)
    results.append({'seconds':t,'PSC':measured,'shuffledCodeControl':control})
    profiles[f'psc_{t}'] = folded
    profiles[f'shuffled_{t}'] = control_folded
report = {'file':str(args.file), 'sampleRate':rate, 'trainingSeconds':2,
          'search':search,'fineSearch':fine,'heldOutProbes':results,
          'note':'No payload decoding. Best offset trained only at t=2 s; later probes reuse it. PSC plus 2560-chip slot period are identification evidence; no inferred broadcaster identity.'}
args.output.mkdir(parents=True,exist_ok=True)
(args.output/'umts-psch-test.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
np.savez_compressed(args.output/'umts-psch-profiles.npz',**profiles)
print(json.dumps({'best':best,'probes':results},indent=2),flush=True)
