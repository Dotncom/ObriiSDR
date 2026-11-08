"""Offline IQ16 capture review: integrity, original waterfall, analog cadence and IQ periodicity.

No device access. Run with --output and optional --deps pointing to local scientific wheels.
"""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
from fractions import Fraction

parser=argparse.ArgumentParser()
parser.add_argument('files',nargs='+',type=Path)
parser.add_argument('--output',required=True,type=Path)
parser.add_argument('--deps',type=Path)
args=parser.parse_args()
if args.deps:sys.path.insert(0,str(args.deps.resolve()))
os.environ['OMP_NUM_THREADS']='1'
os.environ['OPENBLAS_NUM_THREADS']='1'
if os.name=='nt':ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),0x4000)
import numpy as np
from scipy import signal
from scipy.ndimage import gaussian_filter1d
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
args.output.mkdir(parents=True,exist_ok=True)
def emit(x):print(json.dumps(x,ensure_ascii=False),flush=True)
def db(x):return 10*np.log10(np.maximum(x,1e-30))

def spectrum(path,rate):
    rows=[];times=[];centers=[]
    with path.open('rb') as stream:
        assert stream.read(8)==b'FBSPEC1\n'
        meta=json.loads(stream.read(struct.unpack('<I',stream.read(4))[0]))
        while header:=stream.read(52):
            assert len(header)==52
            magic,elapsed,utc,center,lo,hi,fft,count=struct.unpack('<Iqqdddii',header)
            assert magic==0x314d5246 and 0<count<=4_194_304
            payload=stream.read(count*4);assert len(payload)==count*4
            values=np.fft.fftshift(np.frombuffer(payload,dtype='<f4'))
            if count%512==0:coarse=values.reshape(512,-1).mean(axis=1)
            else:coarse=np.interp(np.linspace(0,count-1,512),np.arange(count),values)
            rows.append(coarse);times.append(utc);centers.append(center)
    rows=np.asarray(rows);times=(np.asarray(times,dtype=np.float64)-times[0])/1000
    center=float(centers[0]);freq=center+(np.arange(512)+.5)*rate/512-rate/2
    return {'metadata':meta,'frames':len(rows),'uniqueCentersHz':np.unique(centers).tolist()},times,freq,rows

def raw_chunk(path,rate,t,duration=.24):
    raw=np.memmap(path,dtype='<i2',mode='r').reshape(-1,2)
    start=round(t*rate);n=min(round(duration*rate),len(raw)-start)
    a=np.asarray(raw[start:start+n],dtype=np.float32)/32768
    return a[:,0]+1j*a[:,1]

def coherence(v,rate,f,offset=0,bins=96):
    ix=np.floor((((np.arange(len(v))+offset)*f/rate)%1)*bins).astype(np.int32)
    profile=np.bincount(ix,weights=v,minlength=bins)/np.maximum(1,np.bincount(ix,minlength=bins))
    value=np.sqrt(max(0,float(np.var(profile)/max(np.var(v),1e-20))-bins/len(v)))
    return float(value),profile

def analog(v,rate):
    v=v-v.mean();half=len(v)//2
    frequencies=np.arange(15500,15831,1.)
    scores=np.array([coherence(v[:half],rate,f)[0] for f in frequencies])
    best=float(frequencies[scores.argmax()])
    validation,profile=coherence(v[half:],rate,best,half)
    f,p=signal.periodogram(v,fs=rate,window='hann',nfft=max(262144,2**math.ceil(math.log2(len(v)))))
    standard={}
    for name,hz in [('PAL',15625.),('NTSC',15734.2657)]:
        values=[]
        for harmonic in range(1,5):
            target=hz*harmonic
            mask=abs(f-target)<25;baseline=(abs(f-target)>150)&(abs(f-target)<1000)
            values.append(float(db(p[mask].max()/np.median(p[baseline]))))
        standard[name]={'coherence':coherence(v,rate,hz)[0],'harmonicPeakOverLocalMedianDb':values}
    return {'fitFrequencyHz':best,'trainingCoherence':float(scores.max()),'heldOutCoherence':validation,'standards':standard},profile,f,p

def channel(z,rate,offset,bw):
    z=z*np.exp(-2j*np.pi*offset/rate*np.arange(len(z))).astype(np.complex64)
    decimation=max(1,int(rate/max(bw*1.2,6e6)))
    if decimation>1:z=signal.resample_poly(z,1,decimation,window=('kaiser',8))
    new_rate=rate/decimation
    taps=signal.firwin(161,min(bw*.46,new_rate*.45),fs=new_rate,window=('kaiser',8))
    return signal.oaconvolve(z,taps,mode='same')[256:-256].astype(np.complex64),new_rate

def periodicity(z,rate):
    ratio=Fraction(7680000/round(rate)).limit_denominator(1000)
    z=signal.resample_poly(z,ratio.numerator,ratio.denominator,window=('kaiser',8))[256:-256]
    z=z-z.mean();n=len(z)
    nfft=signal.fftconvolve(z,np.conj(z[::-1]),mode='full')
    ac=nfft[n-1:n+min(80000,n//2)]
    ac=abs(ac/ac[0])
    lags=[128,256,384,512,768,1024,1536,2048,2560,5120,7680,38400,76800]
    entries=[{'lag':lag,'microseconds':lag/7.68,'normalized':float(ac[lag])} for lag in lags if lag<len(ac)]
    # LTE 15 kHz useful symbol: 512 samples at 7.68 MS/s; test the CP pattern separately from a tone.
    p=z[512:]*np.conj(z[:-512]);count=len(p)//7680
    profile=p[:count*7680].reshape(count,7680).mean(axis=0)
    profile=abs(profile)/max(np.mean(abs(z)**2),1e-20)
    return {'rate':7680000,'lags':entries,'cp66usPatternMedian':float(np.median(profile)),
        'cp66usPattern99Percentile':float(np.percentile(profile,99))},ac,profile

inventory=[];demod_results=[];plotrows=[]
for path in args.files:
    meta=json.loads(path.with_suffix('.json').read_text())
    rate=float(meta['sampleRate']);center=float(meta['centerFrequency']);listen=float(meta['listeningFrequency'])
    count=path.stat().st_size//4;duration=count/rate
    assert path.stat().st_size==meta['sampleCount']*4 and path.stat().st_size==meta['bytesWritten']
    # One streaming pass: full-file hash, clipping and amplitude statistics.
    digest=hashlib.sha256();clips=zeros=components=0;square=0.;minimum=32767;maximum=-32768
    with path.open('rb') as stream:
        while data:=stream.read(16*1024*1024):
            digest.update(data);a=np.frombuffer(data,dtype='<i2')
            clips+=int(np.count_nonzero((a==32767)|(a==-32768)));zeros+=int(np.count_nonzero(a==0));components+=len(a)
            minimum=min(minimum,int(a.min()));maximum=max(maximum,int(a.max()))
            x=a.astype(np.float32)/32768;square+=float(np.sum(x*x,dtype=np.float64))
    specpath=path.with_name(path.name.replace('_full_iq_s16.cs16','_spectrum.fbspec'))
    si,times,freq,rows=spectrum(specpath,rate)
    entry={'file':str(path.resolve()),'sha256':digest.hexdigest(),'sampleRate':rate,'centerHz':center,'listeningHz':listen,
        'seconds':duration,'samples':count,'clipFraction':clips/components,'zeroFraction':zeros/components,
        'minS16':minimum,'maxS16':maximum,'rmsComponent':math.sqrt(square/components),'metadata':meta,'spectrum':si}
    inventory.append(entry);emit({k:v for k,v in entry.items() if k not in ['metadata']})
    plotrows.append((path,times,freq,rows,rate,listen))
    # Save a compact, durable copy of original spectrum history, without duplicating the IQ file.
    np.savez_compressed(args.output/(path.stem+'_overview.npz'),seconds=times,frequencyHz=freq,spectrumDb=rows)
    # Use the longest capture at several independent times, plus the initial 50 MS/s record.
    if duration>60:probes=[2.,30.,60.,min(85.,duration-.5)]
    elif rate>=50e6 and duration>5:probes=[2.,min(7.,duration-.5)]
    else:probes=[]
    for t in probes:
        z=raw_chunk(path,rate,t)
        for bandwidth in ([5e6,10e6] if t==probes[0] else [10e6]):
            zc,out_rate=channel(z,rate,listen-center,bandwidth)
            structure,ac,cp=periodicity(zc,out_rate)
            record={'file':path.name,'seconds':t,'channelCenterHz':listen,'bandwidthHz':bandwidth,'iqPeriodicity':structure,'demod':{}}
            phase=np.angle(zc[1:]*np.conj(zc[:-1]));am=abs(zc[1:])
            for method,v in [('FM',phase),('AM',am)]:
                fraction=Fraction(1250000/round(out_rate)).limit_denominator(1000)
                video=signal.resample_poly(v,fraction.numerator,fraction.denominator,window=('kaiser',8))[256:-256]
                metrics,profile,f,p=analog(video,1250000.)
                record['demod'][method]=metrics
                if method=='FM':
                    np.savez_compressed(args.output/f'{path.stem}_{t:g}_{bandwidth/1e6:g}MHz_probe.npz',
                        fmVideo=video,sampleRate=1250000.,lineProfile=profile,iqAutocorrelation=ac,cpProfile=cp)
            demod_results.append(record);emit({'probe':path.name,'seconds':t,'bandwidth':bandwidth,
                'FM':record['demod']['FM'],'periodicity':structure})
    (args.output/'inventory.json').write_text(json.dumps(inventory,indent=2,ensure_ascii=False),encoding='utf-8')
    (args.output/'signal-tests.json').write_text(json.dumps(demod_results,indent=2,ensure_ascii=False),encoding='utf-8')

fig,axes=plt.subplots(len(plotrows),1,figsize=(13,max(3,len(plotrows)*2.8)),layout='constrained',squeeze=False)
for ax,(path,times,freq,rows,rate,listen) in zip(axes[:,0],plotrows):
    lo,hi=np.percentile(rows,[10,99])
    im=ax.pcolormesh(freq/1e6,times,rows,shading='nearest',cmap='magma',vmin=lo,vmax=hi,rasterized=True)
    ax.axvline(listen/1e6,color='cyan',lw=.8)
    ax.set(xlabel='Frequency (MHz)',ylabel='Time (s)',title=f'{path.name} | {rate/1e6:g} MS/s')
    fig.colorbar(im,ax=ax,label='Recorded spectrum (dB)')
fig.savefig(args.output/'captured-spectrum.png',dpi=130);plt.close(fig)
rng=np.random.default_rng(512);t=np.arange(300000)/1250000
controls={}
for label,f in [('PAL',15625.),('NTSC',15734.2657)]:
    phase=t*f%1;v=np.where(phase<.073,-1.,.2*np.sin(6*np.pi*phase))+rng.normal(0,.15,len(t))
    controls[label]=analog(v,1250000.)[0]
controls['noise']=analog(rng.normal(size=len(t)),1250000.)[0]
(args.output/'positive-negative-controls.json').write_text(json.dumps(controls,indent=2),encoding='utf-8')
emit({'status':'ANALYSIS_COMPLETE','output':str(args.output.resolve()),'controls':controls})
