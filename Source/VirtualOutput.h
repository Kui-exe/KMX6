#pragma once
#include <JuceHeader.h>
#include "AudioCore.h"
#include <thread>
#include <windows.h>
#include <avrt.h>

class VirtualOutput {
    std::thread worker;
    std::atomic<bool> quit{true};
public:
    kmx6::StereoQueue queue;
    std::atomic<bool> connected{false};
    ~VirtualOutput(){stop();}
    void stop(){quit=true;if(worker.joinable())worker.join();connected=false;}
    void start(int index,int port=3242){
        stop();queue.reset();queue.target=480;quit=false;
        worker=std::thread([this,index,port]{
            struct AudioScheduling {HANDLE task=nullptr;AudioScheduling(){DWORD index=0;task=AvSetMmThreadCharacteristicsW(L"Pro Audio",&index);if(task)AvSetMmThreadPriority(task,AVRT_PRIORITY_HIGH);}~AudioScheduling(){if(task)AvRevertMmThreadCharacteristics(task);}} scheduling;
            while(!quit){
                juce::StreamingSocket socket;
                if(!socket.connect("127.0.0.1",port,500)){for(int i=0;i<10&&!quit;++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));continue;}
                const unsigned char hello[]{'K','M','X','1',static_cast<unsigned char>(index)};
                if(socket.write(hello,5)!=5)continue;

                std::array<kmx6::Stereo,240> audio;std::array<int16_t,480> pcm;
                while(!quit){
                    if(socket.waitUntilReady(true,100)<=0)continue;
                    uint8_t request[4];if(socket.read(request,4,true)!=4)break;
                    if(request[0]!=240||request[1]||request[2]||request[3])break;
                    connected=true; // Host accepted the protocol and requested audio.
                    queue.discardOld(std::max(1024u,(std::max(queue.target.load(),queue.minimumTarget.load())+queue.safetyReserve.load())*3));
                    queue.pull(audio.data(),240,48000);
                    for(int i=0;i<240;++i){pcm[size_t(i)*2]=int16_t(std::clamp(audio[i].l,-1.0f,0.999969f)*32768);pcm[size_t(i)*2+1]=int16_t(std::clamp(audio[i].r,-1.0f,0.999969f)*32768);}
                    const char* bytes=reinterpret_cast<const char*>(pcm.data());int remaining=int(sizeof(pcm));
                    while(remaining>0&&!quit){int sent=socket.write(bytes,remaining);if(sent<=0){remaining=-1;break;}remaining-=sent;bytes+=sent;}
                    if(remaining<0)break;
                }
                connected=false;
                for(int i=0;i<10&&!quit;++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
};
