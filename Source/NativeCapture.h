#pragma once
#include "AudioCore.h"
#include <string>
#include <vector>
#include <thread>
#include <mutex>

namespace kmx6 {
struct Endpoint { std::wstring id; std::string name; bool playback=false; int channels=2;int sampleRate=0,bitDepth=0; };
std::vector<Endpoint> enumerateEndpoints();
// Native cables take priority; USB/IP is retained until migration is verified.
const Endpoint* computerEndpoint(const std::vector<Endpoint>&, int index);
struct AppSession {std::wstring id;std::string name;std::vector<uint32_t> icon;float volume=1;};
std::vector<AppSession> appSessions(const std::wstring& endpoint);
bool setAppVolume(const std::wstring& endpoint,const std::wstring& session,float volume);
class NativeCapture {
public:
    StereoQueue queue;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> discontinuities{0};
    std::atomic<double> largestPacketGapMs{0};std::atomic<bool> usesSharedMemory{false};
    NativeCapture()=default;
    ~NativeCapture(){stop();}
    void start(Endpoint e,int channel=0);
    void stop();
    std::string status()const { std::lock_guard lock(statusMutex);return message; }
private:
    std::thread worker;
    std::atomic<bool> quit{false};
    int inputChannel=0;double lastPacketTick=0;
    mutable std::mutex statusMutex;
    std::string message="Disconnected";
    void setStatus(std::string s){std::lock_guard lock(statusMutex);message=std::move(s);}
    void run(Endpoint);
    void runComputer(Endpoint ep);
};
}
