#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kmx6 {
inline float gain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float dbfs(float v) { return 20.0f * std::log10(std::max(v, 0.000001f)); }
struct Stereo { float l=0, r=0; };

// Single producer / single consumer. The producer never changes the read cursor.
class StereoQueue {
    static constexpr uint32_t capacity = 65536, mask = capacity - 1;
    std::array<Stereo, capacity> data{};
    alignas(64) std::atomic<uint32_t> write{0};
    alignas(64) std::atomic<uint32_t> read{0};
    double fraction=0;
    bool primed=false,recovering=false;
    int fadeIn=0;
    double stableFrames=0,reserveRelease=0,filteredError=0,integral=0,smoothedCorrection=0;
    Stereo lastSample{},crossfadeFrom{};bool hasPlayed=false;
public:
    std::atomic<double> rate{48000};
    std::atomic<double> bufferedMs{0}; // Published by the consumer, never sample two cursors from the UI.
    std::atomic<uint32_t> target{512};
    std::atomic<uint32_t> minimumTarget{512};
    std::atomic<uint64_t> underflows{0}, overflows{0};
    std::atomic<uint64_t> overflowEvents{0},backlogDrops{0},discardedFrames{0};
    static uint32_t captureReserve(uint32_t packet,double sampleRate){return std::max(128u,packet+uint32_t(std::ceil(std::max(1.0,sampleRate)*0.002))+2);}
    std::atomic<uint32_t> safetyReserve{0};
    std::atomic<uint32_t> backlogMilliseconds{40};
    uint32_t available() const { return write.load(std::memory_order_acquire)-read.load(std::memory_order_acquire); }
    void reset() { bufferedMs=0; overflowEvents=0;backlogDrops=0;discardedFrames=0; write=0; read=0; fraction=0; primed=false;recovering=false;fadeIn=0;stableFrames=reserveRelease=filteredError=integral=smoothedCorrection=0;lastSample=crossfadeFrom={};hasPlayed=false;safetyReserve=0; underflows=0; overflows=0; }
    void discardOld(uint32_t keep){auto w=write.load(std::memory_order_acquire);auto r=read.load(std::memory_order_relaxed);if(w-r>keep){++backlogDrops;discardedFrames.fetch_add(w-r-keep,std::memory_order_relaxed);read.store(w-keep,std::memory_order_release);fraction=0;primed=false;if(hasPlayed){crossfadeFrom=lastSample;fadeIn=64;}}}
    bool push(Stereo s) {
        auto w=write.load(std::memory_order_relaxed);
        if (w-read.load(std::memory_order_acquire)>=capacity-1) { ++overflows; ++overflowEvents; return false; }
        data[w & mask]=s;
        write.store(w+1,std::memory_order_release); return true;
    }
    void pushBlock(const Stereo* samples,int count){auto w=write.load(std::memory_order_relaxed);auto r=read.load(std::memory_order_acquire);auto accepted=std::min(uint32_t(count),capacity-1-(w-r));for(uint32_t i=0;i<accepted;++i)data[(w+i)&mask]=samples[i];write.store(w+accepted,std::memory_order_release);if(accepted<uint32_t(count)){overflows.fetch_add(uint32_t(count)-accepted,std::memory_order_relaxed);++overflowEvents;}}
    void pull(Stereo* out, int count, double outputRate) {
        std::fill_n(out,count,Stereo{});
        auto r=read.load(std::memory_order_relaxed);
        const auto w=write.load(std::memory_order_acquire);
        auto ready=w-r;
        const double seconds=double(count)/std::max(1.0,outputRate);
        // Once stable for 10 seconds, release 1 ms of extra reserve per second.
        // This adjusts consumption smoothly rather than discarding buffered audio.
        auto extraReserve=safetyReserve.load(std::memory_order_relaxed);
        if(stableFrames>outputRate*10&&extraReserve){reserveRelease+=seconds*rate.load()*0.001;auto reduction=std::min(extraReserve,uint32_t(reserveRelease));if(reduction){extraReserve-=reduction;reserveRelease-=reduction;safetyReserve.store(extraReserve,std::memory_order_relaxed);}}
        const auto desired=std::max(target.load(),minimumTarget.load())+extraReserve;
        const double nominal=rate.load()/std::max(1.0,outputRate);
        // Recover from scheduler stalls instead of playing an ever-growing delayed backlog.
        const auto block=uint32_t(std::ceil(count*nominal))+2;
        const auto limit=desired+std::max(desired*3,uint32_t(rate.load()*backlogMilliseconds.load(std::memory_order_relaxed)*0.001));
        if(ready>limit){const auto keep=std::min(ready,desired+block);++backlogDrops;discardedFrames.fetch_add(ready-keep,std::memory_order_relaxed);r=w-keep;ready=keep;fraction=0;primed=false;if(hasPlayed){crossfadeFrom=lastSample;fadeIn=64;}}
        bufferedMs.store(1000.0*ready/std::max(1.0,rate.load()),std::memory_order_relaxed);
        if (!primed) { if (ready < std::max(desired,block)) return; primed=true;if(recovering){fadeIn=64;crossfadeFrom={};recovering=false;} }
        // Very small asynchronous rate correction keeps independent device clocks aligned.
        const double error=(double(ready)-desired)/double(std::max(256u,desired));
        filteredError+=(1-std::exp(-seconds/0.15))*(error-filteredError);
        integral=std::clamp(integral+filteredError*seconds*0.0001,-0.003,0.003);
        const double wantedCorrection=std::clamp(filteredError*0.003+integral,-0.005,0.005);
        const double startCorrection=smoothedCorrection;
        smoothedCorrection+=(1-std::exp(-seconds/0.10))*(wantedCorrection-smoothedCorrection);

        bool shortfall=false;int rendered=0;
        for(int i=0;i<count;++i) {
            if(w-r<2) { shortfall=true; break; }
            const auto a=data[r & mask], b=data[(r+1)&mask];
            out[i]={a.l+float(fraction)*(b.l-a.l),a.r+float(fraction)*(b.r-a.r)};
            if(fadeIn>0){float envelope=1.0f-float(fadeIn)/64.0f;out[i].l=crossfadeFrom.l*(1-envelope)+out[i].l*envelope;out[i].r=crossfadeFrom.r*(1-envelope)+out[i].r*envelope;--fadeIn;}lastSample=out[i];hasPlayed=true;rendered=i+1;
            fraction+=nominal*(1.0+startCorrection+(smoothedCorrection-startCorrection)*double(i+1)/count);
            const auto advance=uint32_t(fraction);
            if (advance > w-r-1) { shortfall=true; r=w-1; fraction=0; break; }
            r+=advance; fraction-=advance;
        }
        read.store(r,std::memory_order_release);
        if(shortfall) {
            ++underflows;primed=false;fraction=0;recovering=true;stableFrames=reserveRelease=0;filteredError=integral=smoothedCorrection=0;lastSample={};
            // Add 2 ms only after a real shortage, bounded to 20 ms per queue.
            auto extra=safetyReserve.load(std::memory_order_relaxed);safetyReserve.store(std::min(uint32_t(rate.load()*0.020),extra+uint32_t(std::ceil(rate.load()*0.002))),std::memory_order_relaxed);
            const int fade=std::min(64,rendered);for(int j=0;j<fade;++j){float envelope=float(fade-1-j)/float(std::max(1,fade-1));out[rendered-fade+j].l*=envelope;out[rendered-fade+j].r*=envelope;}
        } else stableFrames+=count;
    }
};

struct Biquad {
    double b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    float process(float x) { double y=b0*x+z1; z1=b1*x-a1*y+z2; z2=b2*x-a2*y; return float(y); }
    void clear() { z1=z2=0; }
    void set(double B0,double B1,double B2,double A0,double A1,double A2) {
        b0=B0/A0; b1=B1/A0; b2=B2/A0; a1=A1/A0; a2=A2/A0;
    }
    void highpass(double sr) {
        auto w=2*3.141592653589793*80/sr,c=std::cos(w),s=std::sin(w),a=s/(2*0.70710678);
        set((1+c)/2,-(1+c),(1+c)/2,1+a,-2*c,1-a);
    }
    void peak(double sr,double hz,float db) {
        auto A=std::pow(10.0,double(db)/40),w=2*3.141592653589793*hz/sr,c=std::cos(w),a=std::sin(w)/(2*0.70710678);
        set(1+a*A,-2*c,1-a*A,1+a/A,-2*c,1-a/A);
    }
    void shelf(double sr,double hz,float db,bool high) {
        auto A=std::pow(10.0,double(db)/40),w=2*3.141592653589793*hz/sr,c=std::cos(w),s=std::sin(w),b=2*std::sqrt(A)*s/std::sqrt(2.0);
        if(high) set(A*((A+1)+(A-1)*c+b),-2*A*((A-1)+(A+1)*c),A*((A+1)+(A-1)*c-b),(A+1)-(A-1)*c+b,2*((A-1)-(A+1)*c),(A+1)-(A-1)*c-b);
        else set(A*((A+1)-(A-1)*c+b),2*A*((A-1)-(A+1)*c),A*((A+1)-(A-1)*c-b),(A+1)+(A-1)*c+b,-2*((A-1)+(A+1)*c),(A+1)+(A-1)*c-b);
    }
};

struct GlowDetector {
    std::atomic<bool> enabled{true};
    std::array<std::array<std::atomic<float>,3>,2> bands{};
    float low[2]{},upper[2]{},power[2][3]{},a=0,b=0;int count=0;
    void prepare(double rate){a=1-std::exp(float(-2*3.141592653589793*180/rate));b=1-std::exp(float(-2*3.141592653589793*2500/rate));for(int side=0;side<2;++side){low[side]=upper[side]=0;for(auto& band:bands[side])band=0;}begin();}
    void begin(){count=0;for(auto& side:power)for(auto& value:side)value=0;}
    void add(float l,float r){float input[2]{l,r};for(int side=0;side<2;++side){low[side]+=a*(input[side]-low[side]);upper[side]+=b*(input[side]-upper[side]);float values[3]{low[side],upper[side]-low[side],input[side]-upper[side]};for(int band=0;band<3;++band)power[side][band]+=values[band]*values[band];}++count;}
    void finish(){if(count)for(int side=0;side<2;++side)for(int band=0;band<3;++band)bands[side][band].store(std::sqrt(power[side][band]/count),std::memory_order_relaxed);}
};
struct Channel {
    std::atomic<float> fader{-12}, trim{0}, low{0}, mid{0}, high{0}, pan{0};
    std::atomic<bool> mute{false}, solo{false}, monitor{true}, send{false}, monitor2{false},send2{false},send3{false},send4{false},hpf{false};
    // 0 = disconnected, 1 = primary device input, 2 = independent hardware capture or direct computer PCM source.
    std::atomic<int> source{0}, inputChannel{0};
    std::atomic<float> peakL{0},peakR{0},rms{0},inputActivity{0};
    std::atomic<bool> clip{false};
    std::array<std::array<Biquad,4>,2> filters;
    float smTrim=1,smGain=0,smLeft=1,smRight=1,smMonitor=0,smSend=0,smHpf=0,smMonitor2=0,smSend2=0,smSend3=0,smSend4=0;
    std::array<float,3> smoothEq{0,0,0};
    bool eqDirty=true; std::atomic<bool> limiterEnabled{false},gateEnabled{false};
    std::atomic<float> limiterThreshold{-1},limiterRelease{80},gateThreshold{-50},gateAttack{3},gateRelease{120},gateHold{100},gateHysteresis{6},gateReduction{0};
    float gateGain=0;int gateHoldSamples=0;bool gateOpen=false;
    float limiterGain=1; std::atomic<float> limiterReduction{0};

    GlowDetector glow;
    double sampleRate=48000;
    void prepare(double sr) {
        glow.prepare(sr);sampleRate=sr;gateGain=0;gateOpen=false;gateHoldSamples=0; limiterGain=1;limiterReduction=0; smTrim=gain(trim.load()); smGain=smMonitor=smSend=smHpf=smMonitor2=smSend2=smSend3=smSend4=0;
        for(auto& side:filters) { for(auto& f:side)f.clear(); side[0].highpass(sr); }
        smoothEq={low.load(),mid.load(),high.load()};eqDirty=true; updateEQ(4096);
    }
    void updateEQ(int n) {
        const float k=1-std::exp(-float(n)/float(sampleRate*0.025));
        const std::array<float,3> requested{low.load(),mid.load(),high.load()};
        if(!eqDirty&&std::abs(requested[0]-smoothEq[0])<0.00001f&&std::abs(requested[1]-smoothEq[1])<0.00001f&&std::abs(requested[2]-smoothEq[2])<0.00001f)return;eqDirty=false;
        for(int j=0;j<3;++j) smoothEq[j]+=k*(requested[j]-smoothEq[j]);
        for(auto& side:filters) {
            side[1].shelf(sampleRate,120,smoothEq[0],false);
            side[2].peak(sampleRate,1000,smoothEq[1]);
            side[3].shelf(sampleRate,std::min(8000.0,sampleRate*0.4),smoothEq[2],true);
        }
    }
    void process(const Stereo* in,Stereo* mon,Stereo* bus,int n,bool anySolo,Stereo* secondMonitor=nullptr,Stereo* secondBus=nullptr,Stereo* thirdBus=nullptr,Stereo* fourthBus=nullptr) {
        updateEQ(n);const bool analyseGlow=glow.enabled.load(std::memory_order_relaxed);if(analyseGlow)glow.begin();
        const float wantedGain=mute.load()||(anySolo&&!solo.load())?0:gain(fader.load());
        const float wantedTrim=gain(trim.load());
        const float p=pan.load(), wantedL=p>0?1-p:1, wantedR=p<0?1+p:1;
        const float k=1-std::exp(-1.0f/float(sampleRate*0.005));
        const float m=monitor.load()?1.0f:0.0f,b=send.load()?1.0f:0.0f,h=hpf.load()?1.0f:0.0f;
        const float m2=monitor2.load()?1.0f:0.0f,b2=send2.load()?1.0f:0.0f,b3=send3.load()?1.0f:0.0f,b4=send4.load()?1.0f:0.0f;
        float pl=0,pr=0,inputPeak=0,minLimiterGain=1; double squares=0;
                const float release=1-std::exp(-1.0f/float(sampleRate*std::max(1.0f,limiterRelease.load())*0.001)),ceiling=gain(limiterThreshold.load());
        const bool useGate=gateEnabled.load(),useLimiter=limiterEnabled.load();
        const float gateOn=gain(gateThreshold.load()),gateOff=gain(gateThreshold.load()-gateHysteresis.load());
        const float gateRise=1-std::exp(-1.0f/float(sampleRate*std::max(0.1f,gateAttack.load())*0.001)),gateFall=1-std::exp(-1.0f/float(sampleRate*std::max(1.0f,gateRelease.load())*0.001));
        const int holdSamples=int(sampleRate*gateHold.load()*0.001);
        for(int i=0;i<n;++i) {
            inputPeak=std::max(inputPeak,std::max(std::abs(in[i].l),std::abs(in[i].r)));
            smTrim+=k*(wantedTrim-smTrim);
            smGain+=k*(wantedGain-smGain); smLeft+=k*(wantedL-smLeft); smRight+=k*(wantedR-smRight);
            smMonitor+=k*(m-smMonitor); smSend+=k*(b-smSend); smHpf+=k*(h-smHpf);
            smMonitor2+=k*(m2-smMonitor2);smSend2+=k*(b2-smSend2);
            smSend3+=k*(b3-smSend3);smSend4+=k*(b4-smSend4);
            // Input gain precedes the high-pass filter and EQ; channel gain follows them.
            float v[2]{in[i].l*smTrim,in[i].r*smTrim};
            for(int s=0;s<2;++s) { auto hp=filters[s][0].process(v[s]); v[s]+=smHpf*(hp-v[s]); for(int j=1;j<4;++j)v[s]=filters[s][j].process(v[s]); }
            const float detector=std::max(std::abs(v[0]),std::abs(v[1]));
            if(useGate){if(detector>=gateOn){gateOpen=true;gateHoldSamples=holdSamples;}else if(gateOpen){if(detector>=gateOff)gateHoldSamples=holdSamples;else if(gateHoldSamples>0)--gateHoldSamples;else gateOpen=false;}}
            const float gateTarget=(!useGate||gateOpen)?1.0f:0.0f;
            gateGain+=(gateTarget>gateGain?gateRise:gateFall)*(gateTarget-gateGain);
            float l=v[0]*gateGain*smGain*smLeft,r=v[1]*gateGain*smGain*smRight;
            if(useLimiter){
                const float peak=std::max(std::abs(l),std::abs(r));
                const float required=peak>ceiling?ceiling/peak:1.0f;
                if(required<limiterGain)limiterGain=required;else limiterGain+=release*(required-limiterGain);
                l*=limiterGain;r*=limiterGain;minLimiterGain=std::min(minLimiterGain,limiterGain);
            }

            if(analyseGlow)glow.add(l,r);
            mon[i].l+=l*smMonitor; mon[i].r+=r*smMonitor;
            bus[i].l+=l*smSend; bus[i].r+=r*smSend;
            if(secondMonitor){secondMonitor[i].l+=l*smMonitor2;secondMonitor[i].r+=r*smMonitor2;}
            if(secondBus){secondBus[i].l+=l*smSend2;secondBus[i].r+=r*smSend2;}
            if(thirdBus){thirdBus[i].l+=l*smSend3;thirdBus[i].r+=r*smSend3;}
            if(fourthBus){fourthBus[i].l+=l*smSend4;fourthBus[i].r+=r*smSend4;}
            pl=std::max(pl,std::abs(l));pr=std::max(pr,std::abs(r));squares+=(double(l)*l+double(r)*r)*0.5;
        }

        if(analyseGlow)glow.finish();
        gateReduction.store(-20.0f*std::log10(std::max(gateGain,0.000001f)),std::memory_order_relaxed);
        limiterReduction.store(-20.0f*std::log10(std::max(minLimiterGain,0.000001f)),std::memory_order_relaxed);
        inputActivity.store(std::max(inputPeak,inputActivity.load(std::memory_order_relaxed)),std::memory_order_relaxed);
        peakL.store(std::max(pl,peakL.load()*0.90f));peakR.store(std::max(pr,peakR.load()*0.90f));
        rms.store(float(std::sqrt(squares/std::max(1,n)))); if(pl>=1||pr>=1)clip=true;
    }
};
}
