#pragma once
#include "VirtualOutput.h"
#include "NativeCapture.h"

// A private loopback listener verifies the exact production worker's protocol,
// PCM conversion and bus indexes. It never sends audio to Windows devices.
inline int runVirtualOutputChecks(juce::String& report){
    int passed=0,failed=0;auto expect=[&](bool okay,const juce::String& name){report+=(okay?"PASS ":"FAIL ")+name+"\n";okay?++passed:++failed;};
    for(int index=0;index<4;++index){
        juce::StreamingSocket listener;if(!listener.createListener(0,"127.0.0.1")){expect(false,"private transport listener");break;}
        auto output=std::make_unique<VirtualOutput>();output->start(index,listener.getBoundPort());
        if(listener.waitUntilReady(true,2000)<=0){expect(false,"B"+juce::String(index+1)+" connected");output->stop();continue;}
        std::unique_ptr<juce::StreamingSocket> connection(listener.waitForNextConnection());if(!connection){expect(false,"accepted private client");output->stop();continue;}
        unsigned char hello[5]{};bool read=connection->waitUntilReady(true,2000)>0&&connection->read(hello,5,true)==5;
        expect(read&&hello[0]=='K'&&hello[1]=='M'&&hello[2]=='X'&&hello[3]=='1'&&hello[4]==index,"B"+juce::String(index+1)+" uses host-compatible KMX1 and the correct bus index");
        expect(!output->connected.load(),"B"+juce::String(index+1)+" is not ready before host acknowledgement");
        output->queue.rate=48000;for(int i=0;i<2048;++i)output->queue.push({0.25f,-0.25f});
        const unsigned char request[]{240,0,0,0};connection->write(request,4);std::array<int16_t,480> pcm{};
        bool received=connection->waitUntilReady(true,2000)>0&&connection->read(pcm.data(),int(sizeof(pcm)),true)==sizeof(pcm);
        bool correct=received;for(int i=0;i<240;++i)correct&=pcm[i*2]==8192&&pcm[i*2+1]==-8192;
        expect(correct&&output->connected.load(),"B"+juce::String(index+1)+" returns the requested 48 kHz stereo PCM block");
        const unsigned char invalid[]{241,0,0,0};connection->write(invalid,4);for(int i=0;i<100&&output->connected.load();++i)juce::Thread::sleep(5);
        expect(!output->connected.load(),"B"+juce::String(index+1)+" clears ready status after a rejected request");connection->close();output->stop();listener.close();
    }
    report+="\n"+juce::String(passed)+" passed / "+juce::String(failed)+" failed. No Windows audio endpoints opened.\n";return failed?1:0;
}

// Observe existing bus audio through Windows shared capture, without adding test
// tones, changing routes, or saving recorded audio.
class VirtualMicMonitor : private juce::Timer {
    std::array<kmx6::NativeCapture,4> captures;
    std::array<bool,4> found{};
    std::array<uint64_t,4> frames{};
    std::array<double,4> energy{};
    std::array<float,4> peaks{};
    double began=juce::Time::getMillisecondCounterHiRes();
    void timerCallback()override{
        const double elapsed=juce::Time::getMillisecondCounterHiRes()-began;std::array<kmx6::Stereo,256> block;
        for(int bus=0;bus<4;++bus)while(captures[bus].running&&captures[bus].queue.available()>1024){captures[bus].queue.pull(block.data(),256,48000);if(elapsed<1000)continue;for(auto s:block){energy[bus]+=(s.l*s.l+s.r*s.r)*0.5;peaks[bus]=std::max({peaks[bus],std::abs(s.l),std::abs(s.r)});++frames[bus];}}
        if(elapsed<6500)return;stopTimer();int failed=0;juce::String report="Passive Windows virtual-microphone check. No tones or routing changes.\n";
        for(int bus=0;bus<4;++bus){bool okay=found[bus]&&captures[bus].running&&frames[bus]>48000;if(!okay)++failed;
            report+=(okay?"PASS ":"FAIL ")+juce::String("KMX6 Mic B")+juce::String(bus+1)+": "+juce::String(frames[bus])+" frames; RMS="+juce::String(frames[bus]?std::sqrt(energy[bus]/frames[bus]):0,6)+"; peak="+juce::String(peaks[bus],6)+"; "+juce::String(captures[bus].status())+"\n";captures[bus].stop();}
        report+="Silent buses may have no source routed or no input above the gate threshold.\nFailures: "+juce::String(failed)+"\n";
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("virtual-mic-monitor.txt").replaceWithText(report);auto* app=juce::JUCEApplication::getInstance();app->setApplicationReturnValue(failed?1:0);app->quit();
    }
public:
    VirtualMicMonitor(){auto endpoints=kmx6::enumerateEndpoints();for(int bus=0;bus<4;++bus)for(auto& ep:endpoints)if(!ep.playback&&juce::String(ep.name).contains("KMX6 Mic B"+juce::String(bus+1))){found[bus]=true;captures[bus].start(ep,-1);break;}startTimer(20);}
    ~VirtualMicMonitor()override{stopTimer();for(auto& capture:captures)capture.stop();}
};
