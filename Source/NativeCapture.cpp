#include "NativeCapture.h"
#include <JuceHeader.h>
#include <miniaudio.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <shellapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <wrl/client.h>
#include <sstream>
#include <cstring>

namespace kmx6 {
using Microsoft::WRL::ComPtr;
static std::string utf8(const wchar_t* w) {
    if(!w)return {};int n=WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr);
    std::string s(size_t(n),0);WideCharToMultiByte(CP_UTF8,0,w,-1,s.data(),n,nullptr,nullptr);s.pop_back();return s;
}
static std::vector<AppSession> visitSessions(const std::wstring& endpoint,const std::wstring* selected=nullptr,float volume=1,bool* changed=nullptr){
    HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::vector<AppSession> result;
    {ComPtr<IMMDeviceEnumerator> devices;ComPtr<IMMDevice> device;ComPtr<IAudioSessionManager2> manager;ComPtr<IAudioSessionEnumerator> sessions;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices)))&&SUCCEEDED(devices->GetDevice(endpoint.c_str(),&device))&&SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,&manager))&&SUCCEEDED(manager->GetSessionEnumerator(&sessions))){int count=0;sessions->GetCount(&count);for(int i=0;i<count;++i){ComPtr<IAudioSessionControl> control;ComPtr<IAudioSessionControl2> info;ComPtr<ISimpleAudioVolume> level;if(FAILED(sessions->GetSession(i,&control))||FAILED(control.As(&info))||FAILED(control.As(&level)))continue;AudioSessionState state; if(FAILED(control->GetState(&state))||state==AudioSessionStateExpired)continue;LPWSTR id=nullptr;if(FAILED(info->GetSessionInstanceIdentifier(&id)))continue;AppSession app;app.id=id;CoTaskMemFree(id);level->GetMasterVolume(&app.volume);DWORD pid=0;info->GetProcessId(&pid);app.name="System sounds";if(pid){app.name="Application "+std::to_string(pid);HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(process){wchar_t path[32768];DWORD length=32768;if(QueryFullProcessImageNameW(process,0,path,&length)){HICON icon=nullptr;if(!selected)ExtractIconExW(path,0,nullptr,&icon,1);if(icon){BITMAPINFO bitmap{};bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bitmap.bmiHeader.biWidth=24;bitmap.bmiHeader.biHeight=-24;bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP dib=CreateDIBSection(dc,&bitmap,DIB_RGB_COLORS,&pixels,nullptr,0);if(dib){auto old=SelectObject(dc,dib);memset(pixels,0,24*24*4);DrawIconEx(dc,0,0,icon,24,24,0,nullptr,DI_NORMAL);auto* data=static_cast<uint32_t*>(pixels);app.icon.assign(data,data+24*24);for(auto& pixel:app.icon)if(!(pixel&0xff000000)&&pixel)pixel|=0xff000000;SelectObject(dc,old);DeleteObject(dib);}DeleteDC(dc);DestroyIcon(icon);}std::wstring name(path,length);auto slash=name.find_last_of(L"\\/");if(slash!=std::wstring::npos)name=name.substr(slash+1);auto dot=name.find_last_of(L'.');if(dot!=std::wstring::npos)name.resize(dot);app.name=utf8(name.c_str());}CloseHandle(process);}}if(selected&&app.id==*selected){if(SUCCEEDED(level->SetMasterVolume(volume,nullptr))&&changed)*changed=true;}result.push_back(std::move(app));}}}
    if(SUCCEEDED(init))CoUninitialize();return result;
}
std::vector<AppSession> appSessions(const std::wstring& endpoint){return visitSessions(endpoint);}
bool setAppVolume(const std::wstring& endpoint,const std::wstring& session,float volume){bool changed=false;visitSessions(endpoint,&session,std::clamp(volume,0.0f,1.0f),&changed);return changed;}
std::vector<Endpoint> enumerateEndpoints() {
    HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    std::vector<Endpoint> out;
    {
        ComPtr<IMMDeviceEnumerator> en;
        if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en)))) {
            for(auto flow:{eCapture,eRender}) {
                ComPtr<IMMDeviceCollection> devices;
                if(FAILED(en->EnumAudioEndpoints(flow,DEVICE_STATE_ACTIVE,&devices)))continue;
                UINT count=0;devices->GetCount(&count);
                for(UINT i=0;i<count;++i) {
                    ComPtr<IMMDevice> d;ComPtr<IPropertyStore> props;LPWSTR id=nullptr;
                    if(FAILED(devices->Item(i,&d))||FAILED(d->GetId(&id)))continue;
                    Endpoint ep;ep.id=id;CoTaskMemFree(id);ep.playback=flow==eRender;
                    PROPVARIANT name;PropVariantInit(&name);
                    if(SUCCEEDED(d->OpenPropertyStore(STGM_READ,&props))&&SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&name))&&name.vt==VT_LPWSTR)ep.name=utf8(name.pwszVal);
                    PropVariantClear(&name);{ComPtr<IAudioClient> inspect;WAVEFORMATEX* mix=nullptr;if(SUCCEEDED(d->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&inspect))&&SUCCEEDED(inspect->GetMixFormat(&mix))){ep.channels=mix->nChannels;ep.sampleRate=int(mix->nSamplesPerSec);ep.bitDepth=int(mix->wBitsPerSample);CoTaskMemFree(mix);}}out.push_back(std::move(ep));
                }
            }
        }
    }
    if(SUCCEEDED(init))CoUninitialize();return out;
}
const Endpoint* computerEndpoint(const std::vector<Endpoint>& endpoints,int index){
    if(index<0||index>1)return nullptr;
    const std::string renamed="KMX6 Computer "+std::to_string(index+1);
    for(const auto& ep:endpoints)if(ep.playback&&ep.name==renamed)return &ep;
    for(const auto& ep:endpoints)if(ep.playback&&ep.name.ends_with(" Computer "+std::to_string(index+1)))return &ep;
    const std::string native=index==0?"CABLE Input (VB-Audio Virtual Cable)":"Hi-Fi Cable Input (VB-Audio Hi-Fi Cable)";
    for(const auto& ep:endpoints)if(ep.playback&&ep.name==native)return &ep;
    const std::string legacy="KMX6 Computer "+std::to_string(index+1);
    for(const auto& ep:endpoints)if(ep.playback&&ep.name.find(legacy)!=std::string::npos)return &ep;
    return nullptr;
}
void NativeCapture::stop() {quit=true;if(worker.joinable())worker.join();running=false;}
void NativeCapture::start(Endpoint e,int channel) {stop();inputChannel=channel;queue.reset();discontinuities=0;largestPacketGapMs=0;usesSharedMemory=false;quit=false;setStatus("Opening...");worker=std::thread([this,e]{run(e);});}
void NativeCapture::runComputer(Endpoint ep){
    ma_context context{};const ma_backend backend=ma_backend_wasapi;
    ma_result result=ma_context_init(&backend,1,nullptr,&context);
    if(result!=MA_SUCCESS){setStatus("Windows capture backend unavailable");return;}
    ma_device_id id{};for(size_t i=0;i<std::min(ep.id.size(),size_t(63));++i)id.wasapi[i]=ma_wchar_win32(ep.id[i]);
    ma_device_config config=ma_device_config_init(ma_device_type_loopback);
    config.capture.pDeviceID=&id;config.capture.format=ma_format_f32;config.capture.channels=2;config.sampleRate=ep.sampleRate>0?ep.sampleRate:48000;
    config.periodSizeInFrames=480;config.periods=4;config.noFixedSizedCallback=MA_TRUE;
    config.wasapi.usage=ma_wasapi_usage_pro_audio;config.wasapi.noAutoStreamRouting=MA_TRUE;config.pUserData=this;
    config.dataCallback=[](ma_device* device,void*,const void* input,ma_uint32 frames){
        auto* self=static_cast<NativeCapture*>(device->pUserData);
        const double now=juce::Time::getMillisecondCounterHiRes();if(self->lastPacketTick>0)self->largestPacketGapMs.store(std::max(self->largestPacketGapMs.load(),now-self->lastPacketTick));self->lastPacketTick=now;
        self->queue.target.store(StereoQueue::captureReserve(frames,self->queue.rate.load()),std::memory_order_relaxed);
        const auto* pcm=static_cast<const float*>(input);std::array<Stereo,256> audio;
        for(ma_uint32 offset=0;offset<frames;offset+=256){int count=int(std::min(ma_uint32(256),frames-offset));for(int i=0;i<count;++i){float l=pcm?pcm[(offset+i)*2]:0,r=pcm?pcm[(offset+i)*2+1]:0;audio[i]={std::isfinite(l)?l:0,std::isfinite(r)?r:0};}self->queue.pushBlock(audio.data(),count);}
    };
    ma_device device{};queue.rate=config.sampleRate;queue.target=StereoQueue::captureReserve(config.sampleRate/100,config.sampleRate);queue.backlogMilliseconds=40;lastPacketTick=0;
    result=ma_device_init(&context,&config,&device);
    if(result==MA_SUCCESS){result=ma_device_start(&device);if(result==MA_SUCCESS){running=true;setStatus("Computer audio - "+std::to_string(config.sampleRate)+" Hz");while(!quit.load()&&ma_device_get_state(&device)==ma_device_state_started)std::this_thread::sleep_for(std::chrono::milliseconds(20));}ma_device_uninit(&device);}
    running=false;ma_context_uninit(&context);if(result!=MA_SUCCESS)setStatus("Computer audio unavailable ("+std::to_string(result)+")");else setStatus("Disconnected");
}
void NativeCapture::run(Endpoint ep) {
    if(ep.playback){runComputer(ep);return;}
    const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    DWORD taskIndex=0;HANDLE task=AvSetMmThreadCharacteristicsW(L"Pro Audio",&taskIndex);if(task)AvSetMmThreadPriority(task,AVRT_PRIORITY_HIGH);
    WAVEFORMATEX* format=nullptr;
    try {
        auto check=[](HRESULT hr){if(FAILED(hr))throw hr;};
        ComPtr<IMMDeviceEnumerator> en;ComPtr<IMMDevice> device;ComPtr<IAudioClient> client;ComPtr<IAudioCaptureClient> capture;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en)));
        check(en->GetDevice(ep.id.c_str(),&device));
        check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client));
        check(client->GetMixFormat(&format));
        const DWORD flags=AUDCLNT_STREAMFLAGS_EVENTCALLBACK|(ep.playback?AUDCLNT_STREAMFLAGS_LOOPBACK:0);
        // Keep loopback on the endpoint default shared period; forcing a new engine period can disrupt other apps. Request 4 ms only for independent microphone capture.
        HRESULT opened=E_FAIL;ComPtr<IAudioClient3> lowLatency;
        if(!ep.playback&&ep.name.find("KMX6")==std::string::npos&&SUCCEEDED(client.As(&lowLatency))) {
            UINT32 def=0,fundamental=0,min=0,max=0;
            if(SUCCEEDED(lowLatency->GetSharedModeEnginePeriod(format,&def,&fundamental,&min,&max)))
                {UINT32 quantum=std::max(1u,fundamental);UINT32 wanted=UINT32(std::ceil(format->nSamplesPerSec*(ep.playback?0.008:0.004)));UINT32 period=std::clamp(((wanted+quantum-1)/quantum)*quantum,min,max);opened=lowLatency->InitializeSharedAudioStream(flags,period,format,nullptr);}
        }
        if(FAILED(opened)) {
            // A failed low-latency initialization may leave a client unusable; activate a fresh one.
            lowLatency.Reset();client.Reset();
            check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client));
            check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,0,0,format,nullptr));
        }
        check(client->SetEventHandle(event));check(client->GetService(IID_PPV_ARGS(&capture)));
        bool isFloat=format->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
        bool isPcm=format->wFormatTag==WAVE_FORMAT_PCM;
        if(format->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&format->cbSize>=22) {
            auto* ex=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
            isFloat=IsEqualGUID(ex->SubFormat,KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            isPcm=IsEqualGUID(ex->SubFormat,KSDATAFORMAT_SUBTYPE_PCM);
        }
        if(!((isFloat&&format->wBitsPerSample==32)||(isPcm&&(format->wBitsPerSample==16||format->wBitsPerSample==24||format->wBitsPerSample==32))))throw E_NOTIMPL;
        queue.rate=format->nSamplesPerSec;queue.target=256;
        check(client->Start());running=true;
        setStatus(std::string(ep.playback?"Loopback - ":"WASAPI - ")+std::to_string(format->nSamplesPerSec)+" Hz");
        while(!quit.load()) {
            WaitForSingleObject(event,5);
            UINT32 packet=0;check(capture->GetNextPacketSize(&packet));
            while(packet>0) {
                BYTE* bytes=nullptr;UINT32 frames=0;DWORD state=0;
                check(capture->GetBuffer(&bytes,&frames,&state,nullptr,nullptr));
                if(state&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)++discontinuities;
                queue.target.store(StereoQueue::captureReserve(frames,format->nSamplesPerSec));
                auto sample=[&](UINT32 frame,int ch)->float {
                    if((state&AUDCLNT_BUFFERFLAGS_SILENT)||!bytes)return 0;
                    const BYTE* p=bytes+frame*format->nBlockAlign+ch*(format->wBitsPerSample/8);
                    if(isFloat){float v;std::memcpy(&v,p,4);return std::isfinite(v)?v:0;}
                    if(format->wBitsPerSample==16){int16_t v;std::memcpy(&v,p,2);return v/32768.0f;}
                    if(format->wBitsPerSample==24){int32_t v=int32_t(p[0])|(int32_t(p[1])<<8)|(int32_t(p[2])<<16);if(v&0x800000)v|=~0xffffff;return v/8388608.0f;}
                    int32_t v;std::memcpy(&v,p,4);return float(v/2147483648.0);
                };
                std::array<Stereo,256> converted;const int left=ep.playback?0:std::clamp(inputChannel,0,int(format->nChannels)-1),right=(ep.playback||inputChannel<0)?(format->nChannels>1?1:0):left;
                for(UINT32 offset=0;offset<frames;offset+=256){int count=int(std::min(256u,frames-offset));for(int i=0;i<count;++i)converted[i]={sample(offset+i,left),sample(offset+i,right)};queue.pushBlock(converted.data(),count);}
                check(capture->ReleaseBuffer(frames));check(capture->GetNextPacketSize(&packet));
            }
        }
        client->Stop();running=false;setStatus("Disconnected");
    } catch(HRESULT hr) {
        running=false;std::ostringstream s;s<<"Unavailable (0x"<<std::hex<<uint32_t(hr)<<"). Check device access / format.";setStatus(s.str());
    }
    if(format)CoTaskMemFree(format);if(task)AvRevertMmThreadCharacteristics(task);if(event)CloseHandle(event);
    if(SUCCEEDED(init))CoUninitialize();
}
}
