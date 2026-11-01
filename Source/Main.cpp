#include <JuceHeader.h>
#include "UIScale.h"
#include "CreditsAssets.h"
#include "AudioCore.h"
#include "PluginHost.h"
#include "PluginUI.h"
#include "PluginFeatureChecks.h"
#include "NativeCapture.h"
#include "VirtualOutput.h"
#include "VirtualOutputChecks.h"
#include <fstream>
#include <functional>
#include <windows.h>
#include <dwmapi.h>

using namespace juce;
using kmx6::Stereo;
namespace palette {
const Colour bg{0xff13121b}, panel{0xff252332}, raised{0xff333044}, line{0xff56516b}, text{0xfff1eaf6}, dim{0xffb0a6bd}, teal{0xffbb85f5}, orange{0xfff876be};
}
static bool forbidden(const String& name) {return name.containsIgnoreCase("voicemeeter")||name.containsIgnoreCase("vb-audio")||name.containsIgnoreCase("vbaudio");}
static File appFolder(){return File::getSpecialLocation(File::currentExecutableFile).getParentDirectory();}
static File settingsFile(){return appFolder().getChildFile("settings.json");}

class FilteredType : public AudioIODeviceType, private AudioIODeviceType::Listener {
    std::unique_ptr<AudioIODeviceType> inner;
    void audioDeviceListChanged()override{callDeviceChangeListeners();}
public:
    explicit FilteredType(AudioIODeviceType* type):AudioIODeviceType(type->getTypeName()),inner(type){inner->addListener(this);}
    ~FilteredType()override{inner->removeListener(this);}
    void scanForDevices()override{inner->scanForDevices();}
    StringArray getDeviceNames(bool input=false)const override{StringArray result;for(auto& n:inner->getDeviceNames(input))if(!forbidden(n)&&!n.containsIgnoreCase("KMX6"))result.add(n);return result;}
    int getDefaultDeviceIndex(bool input)const override{auto original=inner->getDeviceNames(input);int index=inner->getDefaultDeviceIndex(input);return index>=0&&index<original.size()?getDeviceNames(input).indexOf(original[index]):-1;}
    int getIndexOfDevice(AudioIODevice* d,bool input)const override{auto original=inner->getDeviceNames(input);int index=inner->getIndexOfDevice(d,input);return index>=0&&index<original.size()?getDeviceNames(input).indexOf(original[index]):-1;}
    bool hasSeparateInputsAndOutputs()const override{return inner->hasSeparateInputsAndOutputs();}
    AudioIODevice* createDevice(const String& output,const String& input)override{return forbidden(output)||forbidden(input)?nullptr:inner->createDevice(output,input);}
};
class HardwareManager : public AudioDeviceManager {
public:
    void createAudioDeviceTypes(OwnedArray<AudioIODeviceType>& types)override{
        auto add=[&](AudioIODeviceType* type){if(type)types.add(new FilteredType(type));};
        add(AudioIODeviceType::createAudioIODeviceType_ASIO());
        add(AudioIODeviceType::createAudioIODeviceType_WASAPI(WASAPIDeviceMode::shared));
        add(AudioIODeviceType::createAudioIODeviceType_WASAPI(WASAPIDeviceMode::sharedLowLatency));
        add(AudioIODeviceType::createAudioIODeviceType_WASAPI(WASAPIDeviceMode::exclusive));
    }
};

class MixerEngine : public AudioIODeviceCallback {
public:
    HardwareManager hardware;
    PluginBank plugins;
    AudioDeviceManager busDevice;
    std::array<kmx6::Channel,6> channel;
    std::array<kmx6::NativeCapture,6> capture;
    std::array<String,6> sourceIDs;
    kmx6::StereoQueue sendQueue;
    VirtualOutput b1,b2,b3,b4;
    std::atomic<bool> active{false},sending{false};
    std::atomic<bool> a2Internal{false};
    std::atomic<int> a1Pair{0},a2Pair{2};
    std::array<int,256> activeOutputs{};int physicalOutputCount=0;
    String configureOutputs(AudioDeviceManager& manager,int first,int extra=-1){auto* device=manager.getCurrentAudioDevice();if(!device)return "Output device is unavailable.";int count=device->getOutputChannelNames().size();if(first<0||first>=count||extra>=count)return "The selected output channels are unavailable on this device.";AudioDeviceManager::AudioDeviceSetup setup;manager.getAudioDeviceSetup(setup);setup.useDefaultOutputChannels=false;setup.outputChannels.clear();setup.outputChannels.setBit(first);if(first+1<count)setup.outputChannels.setBit(first+1);if(extra>=0){setup.outputChannels.setBit(extra);if(extra+1<count)setup.outputChannels.setBit(extra+1);}return manager.setAudioDeviceSetup(setup,true);}
    String setOutputPairs(int first,int second){int oldFirst=a1Pair,oldSecond=a2Pair;a1Pair=first;a2Pair=second;auto result=configureOutputs(hardware,first,a2Internal?second:-1);if(result.isEmpty()&&sending)result=configureOutputs(busDevice,second);if(result.isNotEmpty()){a1Pair=oldFirst;a2Pair=oldSecond;configureOutputs(hardware,oldFirst,a2Internal?oldSecond:-1);if(sending)configureOutputs(busDevice,oldSecond);}return result;}
    static void outputPair(float*const* out,int count,int left,int right,int frame,float l,float r){if(left==right){if(left>=0&&left<count&&out[left])out[left][frame]+=(l+r)*0.5f;}else{if(left>=0&&left<count&&out[left])out[left][frame]+=l;if(right>=0&&right<count&&out[right])out[right][frame]+=r;}}
    std::atomic<float> masterDb{-6},masterPeak{0},busPeak{0},cpu{0};
    std::atomic<float> a2Db{0},b1Db{0},b2Db{0},b3Db{0},b4Db{0},busPeak2{0},busPeak3{0},busPeak4{0},a2Peak{0};
    std::atomic<bool> masterClip{false},busClip{false};
    std::atomic<double> sampleRate{48000};
    std::atomic<int> bufferSize{0};
    std::atomic<uint64_t> callbackCount{0},lateCallbacks{0},callbackOverruns{0};
    std::atomic<uint64_t> deliveredFrames{0};std::atomic<int64> callbackStart{0};
    std::atomic<bool> suppressPhysicalOutput{false};
    std::atomic<bool> measureContinuity{false},resetContinuity{false};std::array<std::atomic<int>,4> longestBusSilence{};std::array<int,4> busSilenceRun{};
    int64 lastCallbackTick=0;
    std::array<kmx6::GlowDetector,6> busGlow;

    String error,sendName;
    std::unique_ptr<XmlElement> savedDevice;
    std::array<int,256> activeInputs{};
    float smoothMaster=0;
    float smoothA2=0,smoothB1=0,smoothB2=0,smoothB3=0,smoothB4=0;
    struct SendCallback : AudioIODeviceCallback {
        MixerEngine& e;double rate=48000;int left=0,right=1;
        explicit SendCallback(MixerEngine& x):e(x){}
        void audioDeviceAboutToStart(AudioIODevice* d)override {rate=d->getCurrentSampleRate();auto bits=d->getActiveOutputChannels();int pair=e.a2Pair.load(),other=std::min(pair+1,d->getOutputChannelNames().size()-1);left=right=-1;int compact=0;for(int i=0;i<d->getOutputChannelNames().size();++i)if(bits[i]){if(i==pair)left=compact;if(i==other)right=compact;++compact;}e.sendQueue.target=uint32_t(std::max({256,d->getCurrentBufferSizeSamples()*2,e.bufferSize.load()*2}));}
        void audioDeviceStopped()override{}
        void audioDeviceIOCallbackWithContext(const float*const*,int,float*const* out,int outs,int n,const AudioIODeviceCallbackContext&)override {
            e.sendQueue.minimumTarget=uint32_t(std::max(256.0,std::ceil(n*e.sendQueue.rate.load()/rate)*2+2));
            const bool audible=!e.suppressPhysicalOutput.load();const ScopedNoDenormals noDenormals;for(int c=0;c<outs;++c)if(out[c])FloatVectorOperations::clear(out[c],n);
            std::array<Stereo,256> samples;
            for(int offset=0;offset<n;offset+=256){int count=std::min(256,n-offset);e.sendQueue.pull(samples.data(),count,rate);
                for(int i=0;i<count;++i)if(audible)outputPair(out,outs,left,right,offset+i,std::clamp(samples[i].l,-1.0f,1.0f),std::clamp(samples[i].r,-1.0f,1.0f));}
        }
    } sendCallback{*this};
    explicit MixerEngine(bool persistPlugins=true):plugins(hardware.getAudioCallbackLock(),persistPlugins){channel[0].limiterEnabled=true;channel[1].limiterEnabled=true;channel[0].gateEnabled=true;channel[1].gateEnabled=true;channel[0].monitor=false;channel[1].monitor=false;channel[0].inputChannel=0;channel[1].inputChannel=1;}
    ~MixerEngine(){stop();}
    bool start() {
        if(active)return true;
        error=hardware.getCurrentAudioDevice()?String{}:hardware.initialise(2,2,savedDevice.get(),true);
        if(error.isNotEmpty())return false;
        if(auto* d=hardware.getCurrentAudioDevice();d&&forbidden(d->getName())) {hardware.closeAudioDevice();error="Select a hardware device in Hardware settings. VB-Audio devices are excluded.";return false;}
        if(auto* device=hardware.getCurrentAudioDevice()){int count=device->getOutputChannelNames().size();if(count<1){error="The selected interface has no output channels.";return false;}if(a1Pair>=count)a1Pair=0;error=configureOutputs(hardware,a1Pair);if(error.isNotEmpty())return false;}b1.start(0);b2.start(1);b3.start(2);b4.start(3);hardware.addAudioCallback(this);active=true;return true;
    }
    void stopSend(){sending=false;a2Internal=false;busDevice.removeAudioCallback(&sendCallback);busDevice.closeAudioDevice();sendName.clear();}
    void stop(){stopSend();hardware.removeAudioCallback(this);hardware.closeAudioDevice();b1.stop();b2.stop();b3.stop();b4.stop();for(auto& c:capture)c.stop();active=false;}
    void selectSource(int index,int mode,int input,const kmx6::Endpoint* ep) {
        if(mode==1){auto* device=hardware.getCurrentAudioDevice();if(!device||input<0||input>=device->getInputChannelNames().size()){error="Selected hardware input is unavailable.";return;}if(!device->getActiveInputChannels()[input]){AudioDeviceManager::AudioDeviceSetup setup;hardware.getAudioDeviceSetup(setup);setup.useDefaultInputChannels=false;setup.inputChannels.setBit(input);error=hardware.setAudioDeviceSetup(setup,true);if(error.isNotEmpty())return;}}
        {const ScopedLock guard(hardware.getAudioCallbackLock());channel[index].source=0;}
        capture[index].stop();sourceIDs[index].clear();
        if(mode==2&&ep){sourceIDs[index]=String(ep->id.c_str());if(active)capture[index].start(*ep,input);}
        channel[index].inputChannel=input;channel[index].source=mode;
    }
    String openSend(const String& deviceName) {
        stopSend();if(deviceName.isEmpty())return {};
        String backend=deviceName.contains(" | ")?deviceName.upToFirstOccurrenceOf(" | ",false,false):"Windows Audio",target=deviceName.contains(" | ")?deviceName.fromFirstOccurrenceOf(" | ",false,false):deviceName;
        if(auto* main=hardware.getCurrentAudioDevice();main&&main->getTypeName()==backend&&main->getName()==target){int count=main->getOutputChannelNames().size();if(a2Pair>=count)a2Pair=count>2?2:0;auto result=configureOutputs(hardware,a1Pair,a2Pair);if(result.isNotEmpty())return result;a2Internal=true;sendName=deviceName;return {};}
        if(deviceName=="Main device outputs 3 + 4"){auto* device=hardware.getCurrentAudioDevice();if(!device||device->getActiveOutputChannels().countNumberOfSetBits()<4)return "Enable four output channels in Hardware first.";a2Internal=true;sendName=deviceName;return {};}
        if(forbidden(deviceName))return "VB-Audio devices are excluded from this project.";
        sendQueue.reset();sendQueue.rate=sampleRate.load();
        auto result=busDevice.initialise(0,0,nullptr,false);if(result.isNotEmpty())return result;busDevice.setCurrentAudioDeviceType(backend,true);if(busDevice.getCurrentAudioDeviceType()!=backend)return "The selected A2 driver is unavailable.";
        AudioDeviceManager::AudioDeviceSetup setup;setup.outputDeviceName=target;setup.sampleRate=0;setup.bufferSize=0;setup.useDefaultInputChannels=false;setup.useDefaultOutputChannels=false;auto* type=busDevice.getCurrentDeviceTypeObject();std::unique_ptr<AudioIODevice> probe(type?type->createDevice(target,{}):nullptr);if(!probe)return "The selected output could not be inspected. Check its driver and connection.";auto supportedRates=probe->getAvailableSampleRates();if(supportedRates.contains(sampleRate.load()))setup.sampleRate=sampleRate.load();int count=probe->getOutputChannelNames().size();if(count<1)return "The selected device has no output channels.";if(a2Pair>=count)a2Pair=0;setup.outputChannels.setBit(a2Pair);if(a2Pair+1<count)setup.outputChannels.setBit(a2Pair+1);probe.reset();
        result=busDevice.setAudioDeviceSetup(setup,true);
        if(result.isNotEmpty())return result;
        sendName=deviceName;busDevice.addAudioCallback(&sendCallback);sending=true;return {};
    }
    void audioDeviceAboutToStart(AudioIODevice* d)override {
        deliveredFrames=0;callbackStart=Time::getHighResolutionTicks();lastCallbackTick=0;const auto sr=d->getCurrentSampleRate();sampleRate=sr;sendQueue.rate=sr;b1.queue.rate=sr;b2.queue.rate=sr;b3.queue.rate=sr;b4.queue.rate=sr;bufferSize=d->getCurrentBufferSizeSamples();
        activeInputs.fill(-1);auto bits=d->getActiveInputChannels();int compact=0;
        for(int i=0;i<256;++i)if(bits[i])activeInputs[i]=compact++;
        physicalOutputCount=d->getOutputChannelNames().size();activeOutputs.fill(-1);bits=d->getActiveOutputChannels();compact=0;for(int i=0;i<256;++i)if(bits[i])activeOutputs[i]=compact++;
        plugins.prepare(sr);for(auto& c:channel)c.prepare(sr);for(auto& detector:busGlow)detector.prepare(sr);
    }
    void audioDeviceStopped()override {bufferSize=0;}
    void audioDeviceError(const String& s)override {MessageManager::callAsync([s]{AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon,"Audio device",s);});}
    void audioDeviceIOCallbackWithContext(const float*const* inputs,int numInputs,float*const* outputs,int numOutputs,int n,const AudioIODeviceCallbackContext&)override {
        const ScopedNoDenormals noDenormals;const auto begin=Time::getHighResolutionTicks();if(lastCallbackTick&&Time::highResolutionTicksToSeconds(begin-lastCallbackTick)>1.8*n/sampleRate.load())++lateCallbacks;lastCallbackTick=begin;
        for(int c=0;c<numOutputs;++c)if(outputs[c])FloatVectorOperations::clear(outputs[c],n);

        std::array<bool,6> analyseBus;for(int i=0;i<6;++i){analyseBus[i]=busGlow[i].enabled.load(std::memory_order_relaxed);if(analyseBus[i])busGlow[i].begin();}
        const bool measure=measureContinuity.load();if(resetContinuity.exchange(false)){for(auto& value:longestBusSilence)value=0;busSilenceRun.fill(0);}
        bool solo=false;for(auto& c:channel)solo|=c.solo.load();
        sendQueue.target=uint32_t(std::max(256,n*2));
        for(auto& input:capture)input.queue.minimumTarget=uint32_t(std::max(128.0,std::ceil(n*input.queue.rate.load()/sampleRate.load())+2));
        for(auto* out:{&b1,&b2,&b3,&b4})out->queue.minimumTarget=uint32_t(std::max(480,n*2+2));
        std::array<Stereo,256> source,monitor,bus,bus2,bus3,bus4,monitor2;
        float mp=0,bp=0;const float master=kmx6::gain(masterDb.load());const float masterRamp=float(1-std::exp(-1.0/(sampleRate.load()*0.005)));
        float bp2=0,bp3=0,bp4=0,ap2=0;const float targetA2=kmx6::gain(a2Db.load()),targetB1=kmx6::gain(b1Db.load()),targetB2=kmx6::gain(b2Db.load()),targetB3=kmx6::gain(b3Db.load()),targetB4=kmx6::gain(b4Db.load());
        const int first=std::clamp(a1Pair.load(),0,255),second=std::clamp(a2Pair.load(),0,255),physicalCount=physicalOutputCount;const int leftA=activeOutputs[first],rightA=activeOutputs[std::min(255,std::min(first+1,std::max(0,physicalCount-1)))],leftB=activeOutputs[second],rightB=activeOutputs[std::min(255,std::min(second+1,std::max(0,physicalCount-1)))];const bool audible=!suppressPhysicalOutput.load(),internalA2=a2Internal.load(),externalA2=sending.load();const std::array<bool,4> virtualB{b1.connected.load(),b2.connected.load(),b3.connected.load(),b4.connected.load()};
        for(int offset=0;offset<n;offset+=256) {
            int count=std::min(256,n-offset);std::fill_n(monitor.data(),count,Stereo{});std::fill_n(bus.data(),count,Stereo{});
            std::fill_n(monitor2.data(),count,Stereo{});std::fill_n(bus2.data(),count,Stereo{});std::fill_n(bus3.data(),count,Stereo{});std::fill_n(bus4.data(),count,Stereo{});
            for(int c=0;c<6;++c) {
                std::fill_n(source.data(),count,Stereo{});auto& strip=channel[c];int mode=strip.source.load();
                if(mode==1){int physical=strip.inputChannel.load();int in=physical>=0&&physical<256?activeInputs[physical]:-1;
                    int right=c>=4&&physical+1<256?activeInputs[physical+1]:in;
                    for(int i=0;i<count;++i){float l=in>=0&&in<numInputs&&inputs[in]?inputs[in][offset+i]:0;float r=right>=0&&right<numInputs&&inputs[right]?inputs[right][offset+i]:l;source[i]={l,r};}}
                else if(mode==2&&capture[c].running) {capture[c].queue.pull(source.data(),count,sampleRate.load());if(c<4)for(int i=0;i<count;++i)source[i].r=source[i].l;}
                plugins.process(c,source.data(),count);
                strip.process(source.data(),monitor.data(),bus.data(),count,solo,monitor2.data(),bus2.data(),bus3.data(),bus4.data());
            }
            for(int i=0;i<count;++i) {
                smoothMaster+=masterRamp*(master-smoothMaster);
                smoothA2+=masterRamp*(targetA2-smoothA2);smoothB1+=masterRamp*(targetB1-smoothB1);smoothB2+=masterRamp*(targetB2-smoothB2);smoothB3+=masterRamp*(targetB3-smoothB3);smoothB4+=masterRamp*(targetB4-smoothB4);
                bus[i].l*=smoothB1;bus[i].r*=smoothB1;bus2[i].l*=smoothB2;bus2[i].r*=smoothB2;bus3[i].l*=smoothB3;bus3[i].r*=smoothB3;bus4[i].l*=smoothB4;bus4[i].r*=smoothB4;monitor2[i].l*=smoothA2;monitor2[i].r*=smoothA2;
                if(measure){Stereo measured[4]{bus[i],bus2[i],bus3[i],bus4[i]};for(int b=0;b<4;++b){busSilenceRun[b]=(std::abs(measured[b].l)<0.00001f&&std::abs(measured[b].r)<0.00001f)?busSilenceRun[b]+1:0;longestBusSilence[b].store(std::max(longestBusSilence[b].load(),busSilenceRun[b]));}}
                auto l=monitor[i].l*smoothMaster,r=monitor[i].r*smoothMaster;
                mp=std::max({mp,std::abs(l),std::abs(r)});bp=std::max({bp,std::abs(bus[i].l),std::abs(bus[i].r)});

                if(analyseBus[0])busGlow[0].add(l,r);if(analyseBus[1])busGlow[1].add(monitor2[i].l,monitor2[i].r);if(analyseBus[2])busGlow[2].add(bus[i].l,bus[i].r);if(analyseBus[3])busGlow[3].add(bus2[i].l,bus2[i].r);if(analyseBus[4])busGlow[4].add(bus3[i].l,bus3[i].r);if(analyseBus[5])busGlow[5].add(bus4[i].l,bus4[i].r);
                if(audible)outputPair(outputs,numOutputs,leftA,rightA,offset+i,l,r);if(audible&&internalA2)outputPair(outputs,numOutputs,leftB,rightB,offset+i,monitor2[i].l,monitor2[i].r);


                bp2=std::max({bp2,std::abs(bus2[i].l),std::abs(bus2[i].r)});bp3=std::max({bp3,std::abs(bus3[i].l),std::abs(bus3[i].r)});bp4=std::max({bp4,std::abs(bus4[i].l),std::abs(bus4[i].r)});ap2=std::max({ap2,std::abs(monitor2[i].l),std::abs(monitor2[i].r)});
            }
            if(externalA2)sendQueue.pushBlock(monitor2.data(),count);if(virtualB[0])b1.queue.pushBlock(bus.data(),count);if(virtualB[1])b2.queue.pushBlock(bus2.data(),count);if(virtualB[2])b3.queue.pushBlock(bus3.data(),count);if(virtualB[3])b4.queue.pushBlock(bus4.data(),count);
        }

        for(int i=0;i<6;++i)if(analyseBus[i])busGlow[i].finish();
        for(int out=0;out<numOutputs;++out)if(outputs[out])FloatVectorOperations::clip(outputs[out],outputs[out],-1.0f,1.0f,n);
        masterPeak=std::max(mp,masterPeak.load()*0.9f);busPeak=std::max(bp,busPeak.load()*0.9f);
        busPeak2=std::max(bp2,busPeak2.load()*0.9f);busPeak3=std::max(bp3,busPeak3.load()*0.9f);busPeak4=std::max(bp4,busPeak4.load()*0.9f);a2Peak=std::max(ap2,a2Peak.load()*0.9f);
        if(mp>=1)masterClip=true;if(bp>=1)busClip=true;++callbackCount;deliveredFrames.fetch_add(n,std::memory_order_relaxed);
        auto elapsed=Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks()-begin);
        if(elapsed>n/sampleRate.load())++callbackOverruns;
        cpu=cpu.load()*0.9f+float(elapsed/(n/sampleRate.load()))*0.1f;
    }
};

class AnimatedFader : public Slider {
    void valueChanged()override {repaint();}
};

class DeskLook : public LookAndFeel_V4, private MouseListener {
public:
    static Font numberFont(){return Font(FontOptions("Segoe UI",11.0f,Font::plain));}
    static void styleNumber(Label& label){label.setFont(numberFont());label.setBorderSize(BorderSize<int>(0));label.setMinimumHorizontalScale(1.0f);label.setColour(Label::backgroundColourId,Colours::transparentBlack);label.setColour(Label::outlineColourId,Colours::transparentBlack);label.setColour(Label::textColourId,palette::text);label.setOpaque(false);}
    Label* createSliderTextBox(Slider& slider)override {auto* label=LookAndFeel_V4::createSliderTextBox(slider);styleNumber(*label);return label;}
    bool areScrollbarButtonsVisible()override {return false;}
    int getDefaultScrollbarWidth()override {return 6;}
    void drawScrollbar(Graphics& g,ScrollBar&,int x,int y,int w,int h,bool vertical,int start,int size,bool hovered,bool dragging)override {g.setColour(palette::bg);g.fillRect(x,y,w,h);g.setColour(palette::teal.withAlpha(dragging?1.0f:hovered?0.85f:0.55f));if(vertical)g.fillRect(x+1,start,std::max(1,w-2),size);else g.fillRect(start,y+1,size,std::max(1,h-2));}
    void drawComboBox(Graphics& g,int w,int h,bool,int,int,int,int,ComboBox&)override {g.setColour(Colour(0xff1a1725));g.fillRect(0,0,w,h);g.setColour(palette::line);g.drawRect(0,0,w,h);Path arrow;arrow.startNewSubPath(float(w-16),float(h/2-2));arrow.lineTo(float(w-10),float(h/2+2));arrow.lineTo(float(w-4),float(h/2-2));g.setColour(palette::teal);g.strokePath(arrow,PathStrokeType(1.3f));}
    void drawCallOutBoxBackground(CallOutBox& box,Graphics& g,const Path&,Image&)override {g.setColour(palette::panel);g.fillRect(box.getLocalBounds());g.setColour(palette::line);g.drawRect(box.getLocalBounds(),1);}
    void drawDocumentWindowTitleBar(DocumentWindow& window,Graphics& g,int w,int h,int,int,const Image*,bool)override {g.fillAll(palette::raised);g.setColour(palette::text);g.setFont(14);g.drawText(window.getName(),50,0,w-100,h,Justification::centred);}
    void mouseDown(const MouseEvent& e)override {if(auto* slider=dynamic_cast<Slider*>(e.eventComponent))slider->repaint();}
    void mouseUp(const MouseEvent& e)override {if(auto* slider=dynamic_cast<Slider*>(e.eventComponent))slider->repaint();}
    ~DeskLook()override {Desktop::getInstance().removeGlobalMouseListener(this);}
    DeskLook(){Desktop::getInstance().addGlobalMouseListener(this);setColour(ResizableWindow::backgroundColourId,palette::bg);setColour(Label::textColourId,palette::text);setColour(ComboBox::backgroundColourId,Colour(0xff1b1826));setColour(ComboBox::textColourId,palette::text);setColour(ComboBox::outlineColourId,palette::line);setColour(PopupMenu::backgroundColourId,palette::panel);setColour(PopupMenu::textColourId,palette::text);setColour(TextButton::buttonColourId,Colour(0xff3b344b));setColour(TextButton::buttonOnColourId,Colour(0xff583966));setColour(TextButton::textColourOffId,palette::text);setColour(TextButton::textColourOnId,palette::teal);setColour(Slider::textBoxTextColourId,palette::text);setColour(Slider::textBoxOutlineColourId,Colours::transparentBlack);setColour(Slider::textBoxBackgroundColourId,Colours::transparentBlack);setColour(Slider::rotarySliderFillColourId,palette::teal);setColour(Slider::rotarySliderOutlineColourId,palette::line);}
    void drawRotarySlider(Graphics& g,int x,int y,int w,int h,float position,float start,float end,Slider& slider)override {
        auto bounds=juce::Rectangle<float>(float(x),float(y),float(w),float(h)).reduced(5.0f,2.0f);float ring=std::min(bounds.getWidth(),bounds.getHeight())*0.5f-1.0f,cx=bounds.getCentreX(),cy=bounds.getCentreY(),radius=ring-(slider.getProperties().contains("busMeter")?8.0f:4.0f),angle=start+position*(end-start);
        const bool eqAccent=slider.getProperties().contains("eqAccent");const Colour accent=eqAccent?slider.findColour(Slider::rotarySliderFillColourId):palette::teal;const Colour gradientStart=eqAccent?Colour::fromString(slider.getProperties()["eqGradientStart"].toString()):Colour(0xff8299ff);const Colour gradientEnd=eqAccent?Colour::fromString(slider.getProperties()["eqGradientEnd"].toString()):Colour(0xfff876be);
        Graphics::ScopedSaveState knobState(g);
        if(slider.isMouseButtonDown()){
            for(int spread=4;spread>=1;--spread){g.setColour(accent.withAlpha(0.025f*float(5-spread)));g.fillEllipse(cx-ring-spread,cy-ring-spread,(ring+spread)*2,(ring+spread)*2);}
            g.addTransform(AffineTransform::scale(1.12f,1.12f,cx,cy));
        }
        Path track;track.addCentredArc(cx,cy,ring,ring,0,start,end,true);g.setColour(Colour(0xff50575b));g.strokePath(track,PathStrokeType(2.0f,PathStrokeType::curved,PathStrokeType::rounded));
        for(int segment=0;segment<64;++segment){float from=start+(end-start)*float(segment)/64.0f,to=std::min(angle,start+(end-start)*float(segment+1)/64.0f);if(to<=from)break;Path arc;arc.addCentredArc(cx,cy,ring,ring,0,from,to,true);g.setColour(gradientStart.interpolatedWith(gradientEnd,float(segment)/63.0f));g.strokePath(arc,PathStrokeType(2.2f,PathStrokeType::curved,PathStrokeType::rounded));}
        if(slider.getProperties().contains("busMeter")){float peak=float(slider.getProperties()["busMeter"]);float level=std::clamp((kmx6::dbfs(peak)+60.0f)/60.0f,0.0f,1.0f);Path meterTrack;meterTrack.addCentredArc(cx,cy,ring-4,ring-4,0,start,end,true);g.setColour(Colour(0xff252a29));g.strokePath(meterTrack,PathStrokeType(2));if(level>0){Path lit;lit.addCentredArc(cx,cy,ring-4,ring-4,0,start,start+(end-start)*level,true);g.setColour(peak>=0.891f?Colour(0xffff596b):Colour(0xff7ee2a0));g.strokePath(lit,PathStrokeType(2.2f));}}
        g.setColour(Colours::black.withAlpha(0.35f));g.fillEllipse(cx-radius,cy-radius+2,radius*2,radius*2);ColourGradient rim(Colour(0xffa9afb1),cx-radius,cy-radius,Colour(0xff35393c),cx+radius,cy+radius,false);g.setGradientFill(rim);g.fillEllipse(cx-radius,cy-radius,radius*2,radius*2);radius-=1.4f;
        ColourGradient face(Colour(0xff46494c),cx-radius,cy-radius,Colour(0xff17191c),cx+radius,cy+radius,false);face.addColour(0.45,Colour(0xff292c2f));g.setGradientFill(face);g.fillEllipse(cx-radius,cy-radius,radius*2,radius*2);
        for(int facet=0;facet<8;++facet){float a=float(facet)*MathConstants<float>::twoPi/8.0f;Path wedge;wedge.startNewSubPath(cx,cy);wedge.lineTo(cx+std::sin(a)*radius,cy-std::cos(a)*radius);wedge.lineTo(cx+std::sin(a+0.30f)*radius,cy-std::cos(a+0.30f)*radius);wedge.closeSubPath();g.setColour(Colours::white.withAlpha(facet%2==0?0.045f:0.015f));g.fillPath(wedge);}
        g.setColour(Colour(0xffe0e3e4));g.drawLine(cx+std::sin(angle)*radius*0.73f,cy-std::cos(angle)*radius*0.73f,cx+std::sin(angle)*radius*0.9f,cy-std::cos(angle)*radius*0.9f,1.6f);
    }
    void drawButtonBackground(Graphics& g,Button& button,const Colour&,bool hovered,bool down)override {
        if(bool(button.getProperties()["routeSwitch"])){
            const bool on=button.getToggleState();const float width=float(button.getWidth());
            const float progress=button.getProperties().contains("switchProgress")?float(button.getProperties()["switchProgress"]):(on?1.0f:0.0f);
            const float eased=progress*progress*(3.0f-2.0f*progress);
            auto track=juce::Rectangle<float>(width*0.5f-17.0f,15.0f,34.0f,16.0f);
            g.setColour(Colour(0xff0f1113));g.fillRoundedRectangle(track.expanded(0.5f),8.5f);
            ColourGradient surface(Colour(0xff202426).interpolatedWith(Colour(0xff503565),eased),track.getX(),track.getY(),Colour(0xff13121b).interpolatedWith(Colour(0xff352341),eased),track.getX(),track.getBottom(),false);g.setGradientFill(surface);g.fillRoundedRectangle(track,8.0f);
            g.setColour(palette::line.interpolatedWith(palette::teal,eased).withAlpha(0.55f));g.drawRoundedRectangle(track,8.0f,0.7f);
            auto thumb=juce::Rectangle<float>(track.getX()+2.0f+eased*18.0f,track.getY()+2.0f,12.0f,12.0f);
            if(eased>0){g.setColour(palette::teal.withAlpha(0.12f*eased));g.fillEllipse(thumb.expanded(1.5f));}
            g.setColour(Colours::black.withAlpha(0.5f));g.fillEllipse(thumb.translated(0,0.7f));
            ColourGradient cap(Colour(0xffc2c8c7).interpolatedWith(Colour(0xffebc8f3),eased),thumb.getCentreX(),thumb.getY(),Colour(0xff757d7e).interpolatedWith(Colour(0xffbd84e5),eased),thumb.getCentreX(),thumb.getBottom(),false);g.setGradientFill(cap);g.fillEllipse(thumb);
            if(hovered){g.setColour(Colours::white.withAlpha(0.18f));g.drawEllipse(thumb,0.6f);}            return;
        }
        if(button.getProperties()["actionIcon"].toString()=="power"){bool on=button.getButtonText().contains("STOP");auto area=button.getLocalBounds().toFloat().reduced(1);g.setColour(Colour(0xff141018));g.fillRect(area);if(on){for(int spread=4;spread>=1;--spread){g.setColour(palette::teal.withAlpha(0.025f*float(5-spread)));g.fillRect(area.reduced(float(4-spread)));}}auto face=area.reduced(5);if(down)face=face.translated(0,1);ColourGradient surface(on?Colour(0xffc494fb):Colour(0xff403447),face.getCentreX(),face.getY(),on?Colour(0xff8057a5):Colour(0xff28212f),face.getCentreX(),face.getBottom(),false);g.setGradientFill(surface);g.fillRect(face);g.setColour(on?Colour(0xfff3b6e9):Colour(0xff64516f));g.drawRect(face,1);g.setColour(Colours::white.withAlpha(on?0.2f:0.06f));g.drawHorizontalLine(int(face.getY()+1),face.getX()+1,face.getRight()-1);if(on){g.setColour(palette::teal.withAlpha(hovered?0.42f:0.28f));g.drawRect(face.expanded(1),1);g.setColour(palette::teal.withAlpha(0.12f));g.drawRect(face.expanded(2),1);}return;}
        auto area=button.getLocalBounds().toFloat().reduced(0.5f);bool active=button.getToggleState();Colour fill=active?Colour(0xff583966):Colour(0xff3b344b);if(down)fill=fill.brighter(0.12f);else if(hovered)fill=fill.brighter(0.07f);g.setColour(fill);g.fillRoundedRectangle(area,0.0f);g.setColour(active?palette::teal:Colour(0xff65576f));g.drawRoundedRectangle(area,0.0f,1.0f);
    }
    void drawButtonText(Graphics& g,TextButton& button,bool hovered,bool down)override {
        if(bool(button.getProperties()["routeSwitch"])){g.setColour(button.getToggleState()?palette::teal:palette::dim);g.setFont(FontOptions(11,Font::bold));g.drawText(button.getButtonText(),0,0,button.getWidth(),14,Justification::centred);return;}
        String icon=button.getProperties()["actionIcon"].toString();if(icon.isEmpty()){LookAndFeel_V4::drawButtonText(g,button,hovered,down);return;}float x=button.getWidth()*0.5f,y=button.getHeight()*0.5f;g.setColour(hovered?palette::teal:palette::text);Path p;
        if(icon=="power"){bool on=button.getButtonText().contains("STOP");g.setColour(on?Colour(0xffffe6b0):Colour(0xff786885));if(on){g.setColour(palette::teal.withAlpha(0.2f));g.fillRect(x-10,6.0f,20.0f,6.0f);}g.setColour(on?Colour(0xffffedcd):Colour(0xff786885));g.fillRect(x-7,8.0f,14.0f,2.0f);g.setColour(on?Colour(0xfffff1d6):Colour(0xffaa9bb7));g.setFont(FontOptions(12,Font::bold));g.drawText(on?"ON":"OFF",4,11,button.getWidth()-8,button.getHeight()-15,Justification::centred);return;}
        if(icon=="save"){g.drawRect(x-7,y-7,14.0f,14.0f,1.5f);g.drawRect(x-4,y-7,8.0f,5.0f,1.3f);g.drawRect(x-4,y+2,8.0f,5.0f,1.3f);return;}
        if(icon=="hardware"){p.startNewSubPath(x-7,y-3);p.lineTo(x-3,y-3);p.lineTo(x+2,y-7);p.lineTo(x+2,y+7);p.lineTo(x-3,y+3);p.lineTo(x-7,y+3);p.closeSubPath();g.strokePath(p,PathStrokeType(1.5f));p.clear();p.addCentredArc(x+1,y,6,6,0,MathConstants<float>::pi*0.2f,MathConstants<float>::pi*0.8f,true);g.strokePath(p,PathStrokeType(1.5f));return;}
        if(icon=="virtual"){g.drawRect(x-8,y-6,6.0f,8.0f,1.3f);g.drawRect(x+2,y-2,6.0f,8.0f,1.3f);g.drawLine(x-5,y+2,x-5,y+5,1.3f);g.drawLine(x-5,y+5,x+2,y+5,1.3f);g.drawLine(x+5,y-2,x+5,y-5,1.3f);g.drawLine(x+5,y-5,x-2,y-5,1.3f);return;}
        p.addCentredArc(x,y,7,7,0,0.5f,5.6f,true);g.strokePath(p,PathStrokeType(1.5f));p.clear();p.addTriangle(x-7,y-6,x-1,y-7,x-4,y-2);g.fillPath(p);if(icon=="engine"){g.drawLine(x-3,y,x+3,y,1.4f);g.drawLine(x,y-3,x,y+3,1.4f);}else{g.fillEllipse(x-1.5f,y-1.5f,3,3);}
    }
    void drawLinearSlider(Graphics& g,int x,int y,int w,int h,float pos,float,float,Slider::SliderStyle style,Slider& s)override {
        if(style==Slider::LinearHorizontal){float cy=y+h*0.5f;g.setColour(Colour(0xff19171e));g.fillRoundedRectangle(float(x),cy-2,float(w),4,0);g.setColour(palette::teal.withAlpha(0.35f));g.fillRoundedRectangle(float(x),cy-2,std::max(0.0f,pos-x),4,0);Path thumb;thumb.startNewSubPath(pos-5,cy-5);thumb.lineTo(pos+5,cy-5);thumb.lineTo(pos+5,cy+1);thumb.lineTo(pos,cy+6);thumb.lineTo(pos-5,cy+1);thumb.closeSubPath();if(s.isMouseButtonDown()){thumb.applyTransform(AffineTransform::scale(1.3f,1.3f,pos,cy));for(int spread=3;spread>=1;--spread){g.setColour(palette::teal.withAlpha(0.04f*float(4-spread)));g.strokePath(thumb,PathStrokeType(float(spread*2)));}}g.setColour(palette::teal);g.fillPath(thumb);return;}
        if(style!=Slider::LinearVertical){LookAndFeel_V4::drawLinearSlider(g,x,y,w,h,pos,0,0,style,s);return;}
        float cx=x+w*0.5f;g.setColour(Colour(0xff1b1826));g.fillRoundedRectangle(cx-3,float(y),6,float(h),0);
        float fillY=pos;auto gainColour=[](float db){return db>6?Colour(0xffffb18c):db>0?Colour(0xfff876be):palette::teal;};Colour active=gainColour(float(s.getValue()));
        for(int step=0;step<=84;++step){float db=float(step-60);if(db<s.getMinimum()||db>s.getMaximum())continue;float yy=y+h*(1-float(s.valueToProportionOfLength(db)));bool major=step%6==0;float proximity=std::max(0.0f,1.0f-std::abs(yy-fillY)/24.0f),pop=proximity*proximity*(3.0f-2.0f*proximity),length=(major?20.0f:12.0f)+pop*10.0f;g.setColour(proximity>0?active.withAlpha(0.25f+proximity*0.75f):palette::line.withAlpha(0.55f));g.fillRect(cx-7-length,yy-0.5f-pop*0.4f,length,1.0f+pop*0.8f);g.fillRect(cx+7,yy-0.5f-pop*0.4f,length,1.0f+pop*0.8f);}
        for(float db:{-60.0f,-40.0f,-24.0f,-20.0f,-10.0f,0.0f,6.0f,12.0f,24.0f}){if(db<s.getMinimum()||db>s.getMaximum())continue;float yy=y+h*(1-float(s.valueToProportionOfLength(db)));g.setFont(9);g.setColour(palette::dim);g.drawText(String(int(db)),int(cx-51),int(yy)-6,20,12,Justification::centredRight);}
        Graphics::ScopedSaveState faderState(g);
        if(s.isMouseButtonDown()){
            auto glow=juce::Rectangle<float>(cx-15.0f,pos-14.0f,30.0f,28.0f);
            for(int spread=3;spread>=1;--spread){g.setColour(palette::teal.withAlpha(0.035f*float(4-spread)));g.fillRect(glow.expanded(float(spread)));}
            g.addTransform(AffineTransform::scale(1.12f,1.12f,cx,pos));
        }
        auto cap=juce::Rectangle<float>(cx-13.0f,pos-12.0f,26.0f,24.0f);g.setColour(Colour(0xff090c0d).withAlpha(0.55f));g.fillRoundedRectangle(cap.translated(0.0f,2.0f),0.0f);ColourGradient bevel(Colour(0xff899191),cx,pos-12,Colour(0xff43494b),cx,pos+12,false);g.setGradientFill(bevel);g.fillRoundedRectangle(cap,0.0f);g.setColour(Colour(0xffb8c0bf));g.drawRoundedRectangle(cap,0.0f,1.0f);auto inset=cap.reduced(3.0f,3.0f);g.setColour(Colour(0xff333044));g.fillRoundedRectangle(inset,0.0f);g.setColour(Colour(0xffa7afae).withAlpha(0.75f));g.drawHorizontalLine(int(pos-5),inset.getX()+2,inset.getRight()-2);g.setColour(palette::teal);g.drawHorizontalLine(int(pos),inset.getX()+2,inset.getRight()-2);g.setColour(Colour(0xffebd5ff).withAlpha(0.7f));g.drawHorizontalLine(int(pos+1),inset.getX()+2,inset.getRight()-2);
    }
};

class DynamicsSettings : public Component {
    DeskLook look;kmx6::Channel& channel;bool gate;
    Label heading;ToggleButton enabled;OwnedArray<Slider> sliders;OwnedArray<Label> labels;
    void row(String name,std::atomic<float>& value,double lo,double hi,String suffix){auto* label=labels.add(new Label);label->setText(name,dontSendNotification);label->setFont(FontOptions(11));addAndMakeVisible(label);auto* slider=sliders.add(new Slider);slider->setSliderStyle(Slider::LinearHorizontal);slider->setTextBoxStyle(Slider::TextBoxRight,false,66,20);slider->setRange(lo,hi,0.1);slider->setValue(value.load());slider->setTextValueSuffix(suffix);auto* field=&value;slider->onValueChange=[slider,field]{field->store(float(slider->getValue()));};addAndMakeVisible(slider);}
public:
    DynamicsSettings(kmx6::Channel& c,bool isGate):channel(c),gate(isGate){setLookAndFeel(&look);heading.setText(gate?"MIC GATE":"MIC LIMITER",dontSendNotification);heading.setFont(FontOptions(12,Font::bold));heading.setColour(Label::textColourId,gate?Colour(0xff91adff):Colour(0xffffdc91));addAndMakeVisible(heading);enabled.setButtonText("Enabled");enabled.setToggleState(gate?c.gateEnabled.load():c.limiterEnabled.load(),dontSendNotification);enabled.onClick=[this]{if(gate)channel.gateEnabled=enabled.getToggleState();else channel.limiterEnabled=enabled.getToggleState();};addAndMakeVisible(enabled);
        if(gate){row("Threshold",c.gateThreshold,-60,0," dB");row("Attack",c.gateAttack,0.1,30," ms");row("Hold",c.gateHold,0,500," ms");row("Release",c.gateRelease,10,1000," ms");row("Hysteresis",c.gateHysteresis,0,18," dB");}else{row("Threshold",c.limiterThreshold,-30,0," dB");row("Release",c.limiterRelease,10,1000," ms");}setSize(292,58+sliders.size()*34);}
    ~DynamicsSettings()override {setLookAndFeel(nullptr);}
    void paint(Graphics& g)override {g.fillAll(palette::panel);}
    void resized()override {heading.setBounds(10,4,160,24);enabled.setBounds(194,4,88,24);for(int i=0;i<sliders.size();++i){labels[i]->setBounds(10,40+i*34,72,24);sliders[i]->setBounds(82,40+i*34,200,24);}}
};
class Meter : public Component, public SettableTooltipClient {
    float left=0,right=0,holdL=0,holdR=0,reduction=0;bool clip=false;
    bool showLimiter=false;kmx6::Channel* dynamics=nullptr;bool draggingGate=false,draggingThreshold=false,meterGlow=true;
    kmx6::GlowDetector* detector=nullptr;float glowBands[2][3]{};double glowTime=0;


    float thresholdY(float db)const {return 14.0f+(getHeight()-28.0f)*(1.0f-std::clamp((db+60.0f)/60.0f,0.0f,1.0f));}
    void setThreshold(float y){float db=std::clamp((1-(y-14)/std::max(1.0f,getHeight()-28.0f))*60-60,-60.0f,0.0f);if(draggingGate)dynamics->gateThreshold=db;else dynamics->limiterThreshold=std::max(-30.0f,db);repaint();}
public:
    Meter(){setTooltip("Glow reacts to frequency and loudness.");}
    void attachGlow(kmx6::GlowDetector& input){detector=&input;}
    void attachDynamics(kmx6::Channel& channel){dynamics=&channel;setTooltip("Left green arrow: gate. Right magenta arrow: limiter. Drag to set threshold; right-click an arrow for settings. Glow reacts to frequency and loudness.");}
    bool hitTest(int x,int y)override {return Component::hitTest(x,y);}
    void mouseDown(const MouseEvent& e)override {draggingThreshold=dynamics&&(e.x<10||e.x>=getWidth()-10);if(!draggingThreshold)return;draggingGate=e.x<getWidth()/2;if(e.mods.isPopupMenu()){auto panel=std::make_unique<DynamicsSettings>(*dynamics,draggingGate);auto* parent=getTopLevelComponent();CallOutBox::launchAsynchronously(std::move(panel),parent->getLocalArea(this,juce::Rectangle<int>(draggingGate?0:getWidth()-10,int(thresholdY(draggingGate?dynamics->gateThreshold.load():dynamics->limiterThreshold.load()))-6,10,12)),parent);}else setThreshold(float(e.y));}
    void mouseDrag(const MouseEvent& e)override {if(dynamics&&draggingThreshold&&!e.mods.isPopupMenu())setThreshold(float(e.y));}
    void setLimiterReduction(float db){showLimiter=true;reduction=std::max(db,reduction*0.94f);}
    void update(float l,float r,bool c){if(!isShowing())return;if(l<0.000001f&&r<0.000001f&&left<0.000001f&&right<0.000001f&&holdL<0.000001f&&holdR<0.000001f&&clip==c)return;double now=Time::getMillisecondCounterHiRes()*0.001;float dt=float(glowTime>0?std::min(0.1,now-glowTime):0);glowTime=now;if(meterGlow&&detector)for(int side=0;side<2;++side)for(int band=0;band<3;++band){float target=detector->bands[side][band].load(std::memory_order_relaxed);float coefficient=1-std::exp(-dt*(target>glowBands[side][band]?35.0f:8.0f));glowBands[side][band]+=coefficient*(target-glowBands[side][band]);}left=std::max(l,left*0.90f);right=std::max(r,right*0.90f);holdL=std::max(left,holdL*0.995f);holdR=std::max(right,holdR*0.995f);clip=c;repaint();}
    void paint(Graphics& g)override {
        auto area=getLocalBounds().toFloat().reduced(0.5f);g.setColour(Colour(0xff151319));g.fillRoundedRectangle(area,0);g.setColour(Colour(0xff46404e));g.drawRoundedRectangle(area,0,1);
        const float inset=dynamics?9.0f:std::min(6.0f,area.getWidth()*0.16f);auto inner=area.reduced(inset,14.0f);float gap=std::min(4.0f,inner.getWidth()*0.18f),barWidth=(inner.getWidth()-gap)*0.5f;
        const Colour leftColour(0xffa994fa),rightColour(0xffed92d4);g.setFont(8.0f);g.setColour(leftColour);g.drawText("L",int(inner.getX()),2,int(barWidth),10,Justification::centred);g.setColour(rightColour);g.drawText("R",int(inner.getX()+barWidth+gap),2,int(barWidth),10,Justification::centred);
        for(float db:{-48.0f,-36.0f,-24.0f,-12.0f,-6.0f,0.0f}){float yy=inner.getBottom()-(db+60.0f)/60.0f*inner.getHeight();g.setColour(Colour(0xff66596f));g.drawHorizontalLine(int(yy),inner.getX()+barWidth+1,inner.getX()+barWidth+gap-1);}
        for(int c=0;c<2;++c){
            auto bar=juce::Rectangle<float>(inner.getX()+c*(barWidth+gap),inner.getY(),barWidth,inner.getHeight());
            g.setColour(Colour(0xff29232f));g.fillRect(bar);
            float value=c?right:left,peak=c?holdR:holdL;
            float level=std::clamp((kmx6::dbfs(value)+60.0f)/60.0f,0.0f,1.0f);
            float fillHeight=level*bar.getHeight();
            if(fillHeight>1.0f){
                auto active=juce::Rectangle<float>(bar.getX(),bar.getBottom()-fillHeight,bar.getWidth(),fillHeight);
                if(meterGlow){
                                        float bass=glowBands[c][0],mid=glowBands[c][1],treble=glowBands[c][2];float total=bass+mid+treble;
                    float loudness=std::clamp(std::sqrt(total*1.8f),0.0f,1.0f);
                    float bassWeight=total>0.00001f?bass/total:0,trebleWeight=total>0.00001f?treble/total:0;
                    const Colour glow=(c==0?leftColour:rightColour).interpolatedWith(Colour(0xffffe598),trebleWeight*0.65f);
                    const float intensity=detector?loudness*(0.60f+0.4f*std::max(bassWeight,trebleWeight)):level;
                    for(int spread=4;spread>=1;--spread){g.setColour(glow.withAlpha((0.025f+0.014f*float(5-spread))*intensity));g.fillRoundedRectangle(active.expanded(float(spread)*(1+0.7f*bassWeight),1.5f*spread),float(spread));}
                    g.setColour(glow.brighter(0.4f).withAlpha(0.28f*intensity));g.fillRect(active.getX(),active.getY(),active.getWidth(),2.0f);
                }
                ColourGradient gradient(c==0?Colour(0xff6467b4):Colour(0xffa362af),bar.getCentreX(),bar.getBottom(),Colour(0xffffe6b0),bar.getCentreX(),bar.getY(),false);
                gradient.addColour(0.55,c==0?leftColour:rightColour);gradient.addColour(0.82,Colour(0xffe8b0e8));g.setGradientFill(gradient);g.fillRect(active);
                g.setColour(Colours::white.withAlpha(0.17f));g.fillRect(active.getX(),active.getY(),1.0f,active.getHeight());
            }
            float peakY=bar.getBottom()-std::clamp((kmx6::dbfs(peak)+60.0f)/60.0f,0.0f,1.0f)*bar.getHeight();
            if(peak>0.001f){g.setColour(Colour(0xffffedcd));g.fillRect(bar.getX(),peakY,bar.getWidth(),1.5f);}
            g.setColour(level>0.9f?Colour(0xffff4f68):(c==0?leftColour:rightColour).withAlpha(0.5f));g.fillEllipse(bar.getCentreX()-1.5f,float(getHeight()-8),3.0f,3.0f);
        }
        if(dynamics){for(int type=0;type<2;++type){bool gate=type==0;float yy=thresholdY(gate?dynamics->gateThreshold.load():dynamics->limiterThreshold.load());bool on=gate?dynamics->gateEnabled.load():dynamics->limiterEnabled.load();Colour colour=gate?Colour(0xff91adff):Colour(0xffffdc91);if(!on)colour=palette::dim;g.setColour(colour.withAlpha(on?0.7f:0.3f));g.drawLine(inner.getX(),yy,inner.getRight(),yy,1.0f);Path arrow;if(gate)arrow.addTriangle(1,yy-5,1,yy+5,9,yy);else arrow.addTriangle(float(getWidth()-1),yy-5,float(getWidth()-1),yy+5,float(getWidth()-9),yy);g.setColour(colour);g.fillPath(arrow);}if(dynamics->gateEnabled.load()&&dynamics->gateReduction.load()>3){g.setColour(Colour(0xff91adff));g.fillRect(1,getHeight()-4,8,2);}}
        if(showLimiter){float amount=std::clamp(reduction/24.0f,0.0f,1.0f);g.setColour(Colour(0xffffc69c));g.fillRect(inner.getCentreX()-1.5f,inner.getY(),3.0f,amount*inner.getHeight());g.setFont(8);g.setColour(reduction>0.1f?Colour(0xffffe598):palette::dim);g.drawText("LIM",1,getHeight()-12,getWidth()-2,10,Justification::centred);}
        if(clip){g.setColour(Colour(0xffff4f68));g.fillRoundedRectangle(3,1,float(getWidth()-6),3,0);}
    }
};
class MainComponent;
class CreditsPanel : public Component {
    DeskLook look;
    Image juceLogo=ImageCache::getFromMemory(CreditsAssets::Juce_png,CreditsAssets::Juce_pngSize),cablesLogo=ImageCache::getFromMemory(CreditsAssets::VirtualCables_png,CreditsAssets::VirtualCables_pngSize);
    struct Credit {const char* name;const char* creator;int icon;};
    void row(Graphics& g,int y,const Credit& credit){
        g.setColour(palette::line.withAlpha(0.45f));g.drawHorizontalLine(y-2,10,float(getWidth()-10));g.setColour(palette::teal);
        if(credit.icon<2)g.drawImageWithin(credit.icon==0?juceLogo:cablesLogo,10,y+5,22,22,RectanglePlacement::centred);
        else if(credit.icon==5){for(int yy=0;yy<2;++yy)for(int xx=0;xx<2;++xx)g.fillRect(12+xx*10,y+7+yy*10,8,8);}
        else if(credit.icon==2){float x=22,cy=float(y+16);g.drawLine(x,cy-10,x,cy+8,1.4f);g.drawLine(x,cy-2,x-7,cy-7,1.4f);g.drawLine(x,cy+2,x+7,cy-4,1.4f);g.fillRect(x-9,cy-9,4.0f,4.0f);g.fillEllipse(x+5,cy-7,4.0f,4.0f);Path arrow;arrow.addTriangle(x-4,cy-9,x+4,cy-9,x,cy-14);g.fillPath(arrow);}
        else {String mark=credit.icon==3?"AS":credit.icon==8?"V3":credit.icon==4?"Go":credit.icon==6?"ma":"VB";g.setFont(FontOptions(12,Font::bold));g.drawText(mark,9,y+5,25,24,Justification::centred);}
        g.setColour(palette::text);g.setFont(FontOptions(11,Font::bold));g.drawText(credit.name,40,y,getWidth()-50,16,Justification::centredLeft);
        g.setColour(palette::dim);g.setFont(FontOptions(10));g.drawText(credit.creator,40,y+16,getWidth()-50,14,Justification::centredLeft);
    }
public:
    CreditsPanel(){setLookAndFeel(&look);setSize(300,362);}
    ~CreditsPanel()override{setLookAndFeel(nullptr);}
    void paint(Graphics& g)override{
        g.fillAll(palette::panel);g.setColour(palette::text);g.setFont(FontOptions(11,Font::bold));g.drawText("BUILT WITH",10,4,280,18,Justification::centredLeft);
        const Credit integrated[]{
            {"JUCE","Raw Material Software Limited",0},
            {"Virtual-Cables","Tarek Wasfy and contributors",1},
            {"ASIO SDK","Steinberg Media Technologies",3},
            {"VST3 SDK / plugin hosting","Steinberg Media Technologies",8},
            {"Go runtime","The Go Authors",4},
            {"Windows Audio","Microsoft",5},
            {"miniaudio","David Reid",6}
        };
        for(int i=0;i<7;++i)row(g,28+i*34,integrated[i]);
        g.setColour(palette::dim);g.setFont(FontOptions(10,Font::bold));g.drawText("SEPARATELY INSTALLED DRIVERS",10,268,280,16,Justification::centredLeft);
        row(g,290,{"USB/IP Client for Windows","Vadym Hrynchyshyn and contributors",2});
        row(g,324,{"VB-CABLE / Hi-Fi Cable (optional)","Vincent Burel / VB-Audio",7});
    }
};


class AppVolumes : public Component {
    DeskLook look;
    std::wstring endpoint;Label heading,empty;TextButton refresh{"Refresh"};Viewport viewport;Component rows;std::vector<std::unique_ptr<Label>> names;std::vector<std::unique_ptr<ImageComponent>> icons;std::vector<std::unique_ptr<Slider>> volumes;
    void reload(){names.clear();icons.clear();volumes.clear();auto sessions=kmx6::appSessions(endpoint);for(auto& app:sessions){auto icon=std::make_unique<ImageComponent>();Image image(Image::ARGB,24,24,true);if(app.icon.size()==576){for(int yy=0;yy<24;++yy)for(int xx=0;xx<24;++xx)image.setPixelAt(xx,yy,Colour(app.icon[yy*24+xx]));}else{Graphics g(image);g.setColour(palette::teal);g.fillRoundedRectangle(2,2,20,20,0);g.setColour(palette::bg);g.setFont(14);g.drawText(String(app.name).substring(0,1).toUpperCase(),0,0,24,24,Justification::centred);}icon->setImage(image);rows.addAndMakeVisible(*icon);icons.push_back(std::move(icon));auto name=std::make_unique<Label>();name->setText(app.name,dontSendNotification);name->setFont(FontOptions(11));rows.addAndMakeVisible(*name);auto slider=std::make_unique<Slider>();slider->getProperties().set("appVolume",true);slider->setSliderStyle(Slider::LinearHorizontal);slider->setTextBoxStyle(Slider::TextBoxRight,false,34,16);slider->setRange(0,100,1);slider->setTextValueSuffix("%");slider->setValue(app.volume*100,dontSendNotification);auto id=app.id;auto* value=slider.get();slider->onValueChange=[this,id,value]{if(!kmx6::setAppVolume(endpoint,id,float(value->getValue()/100)))empty.setText("Session ended. Refresh the list.",dontSendNotification);};rows.addAndMakeVisible(*slider);names.push_back(std::move(name));volumes.push_back(std::move(slider));}empty.setText(sessions.empty()?"No apps yet. Play audio through this virtual input.":"Changes the app's Windows session volume.",dontSendNotification);empty.setVisible(sessions.empty());setSize(230,sessions.empty()?88:32+std::min(3,int(sessions.size()))*40);resized();}
public:
    ~AppVolumes()override {setLookAndFeel(nullptr);}
    explicit AppVolumes(std::wstring id):endpoint(std::move(id)){setLookAndFeel(&look);heading.setText("APP VOLUMES",dontSendNotification);heading.setFont(FontOptions(11,Font::bold));addAndMakeVisible(heading);addAndMakeVisible(empty);empty.setFont(FontOptions(11));addAndMakeVisible(refresh);refresh.onClick=[this]{reload();};viewport.setViewedComponent(&rows,false);viewport.setScrollBarThickness(6);viewport.setScrollBarsShown(true,false);addAndMakeVisible(viewport);setSize(230,152);reload();}
    void paint(Graphics& g)override {g.fillAll(palette::panel);}
    void resized()override {heading.setBounds(8,4,140,20);refresh.setBounds(164,4,58,20);empty.setBounds(8,30,214,48);viewport.setBounds(4,30,222,getHeight()-34);rows.setSize(214,std::max(1,int(names.size())*40));for(size_t i=0;i<names.size();++i){icons[i]->setBounds(4,int(i)*40+10,18,18);names[i]->setBounds(28,int(i)*40,182,17);volumes[i]->setBounds(28,int(i)*40+17,182,21);}}
};
class Strip : public Component {
    MixerEngine& engine;int index;
    struct MonochromeEffect : ImageEffectFilter {
        void applyEffect(Image& image,Graphics& g,float,float alpha)override {image.desaturate();g.setOpacity(alpha);g.drawImageAt(image,0,0);}
    } monochrome;
    bool monochromeApplied=false;
    int activityHold=0; float inactiveShade=0.55f;
    Label title,subtitle,sourceLabel,sourceValue,lowLabel,midLabel,highLabel,panLabel,gainLabel,volumeLabel,meterLabel,statusLabel,readout,latencyReadout,healthReadout;
    int latencyTicks=0;
    ComboBox source;
    Slider trim,low,mid,high,pan;
    AnimatedFader fader;
    TextButton hpf{"HPF"},mute{"MUTE"},solo{"SOLO"},monitor{"A1"},send{"B1"},monitor2{"A2"},send2{"B2"},send3{"B3"},send4{"B4"},clear{"CLR"};
    Meter meter;
    TextButton apps{"APPS"},pluginsButton{"PLUGINS"};
    struct Choice{int mode=0,input=0;kmx6::Endpoint ep;};std::vector<Choice> choices;
    std::function<void(int,int,int,const kmx6::Endpoint*)> choose;
    void label(Label& l,String text){l.setText(text,dontSendNotification);l.setFont(FontOptions(10));l.setColour(Label::textColourId,palette::dim);addAndMakeVisible(l);}
    void knob(Slider& s,std::atomic<float>& v,double min,double max,String suffix){s.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);s.setTextBoxStyle(Slider::TextBoxBelow,false,70,18);s.setColour(Slider::textBoxTextColourId,palette::text);s.setColour(Slider::textBoxBackgroundColourId,Colours::transparentBlack);s.setColour(Slider::textBoxOutlineColourId,Colours::transparentBlack);s.setRange(min,max,0.1);s.setValue(v.load());s.setTextValueSuffix(suffix);s.setDoubleClickReturnValue(true,0);s.onValueChange=[&s,&v]{v=float(s.getValue());};addAndMakeVisible(s);}
    void toggle(TextButton& b,std::atomic<bool>& v){b.setClickingTogglesState(true);b.setToggleState(v.load(),dontSendNotification);b.onClick=[&b,&v]{v=b.getToggleState();};addAndMakeVisible(b);}
public:
    Strip(MixerEngine& e,int i,std::function<void(int,int,int,const kmx6::Endpoint*)> fn):engine(e),index(i),choose(std::move(fn)) {
        addAndMakeVisible(pluginsButton); pluginsButton.onClick=[this]{auto panel=std::make_unique<PluginPanel>(engine.plugins,index);auto* parent=getTopLevelComponent();CallOutBox::launchAsynchronously(std::move(panel),parent->getLocalArea(&pluginsButton,pluginsButton.getLocalBounds()),parent);};
        auto& c=engine.channel[i];label(title,i<4?"MIC "+String(i+1):"VIRTUAL "+String(i-3));title.setFont(FontOptions(15,Font::bold));title.setColour(Label::textColourId,palette::text);
        label(subtitle,i<4?"MONO / HARDWARE":"STEREO / COMPUTER");label(sourceLabel,i<4?"SOURCE":"VIRTUAL CABLE / WINDOWS DEVICE");label(sourceValue,i==4?"KMX6 Computer 1":"KMX6 Computer 2");source.setVisible(i<4);sourceValue.setVisible(i>=4);label(lowLabel,"LOW");label(midLabel,"MID");label(highLabel,"HIGH");for(auto* eqName:{&lowLabel,&midLabel,&highLabel}){eqName->setFont(FontOptions(10,Font::bold));eqName->setJustificationType(Justification::centred);}label(panLabel,i<4?"PAN":"BALANCE");label(gainLabel,"GAIN");label(volumeLabel,"LEVEL");label(meterLabel,"METER");label(statusLabel,"Disconnected");label(readout,"-12.0 dB");statusLabel.setMinimumHorizontalScale(0.78f);sourceValue.setMinimumHorizontalScale(0.86f);
        source.setTextWhenNothingSelected("Select source");source.onChange=[this]{int n=source.getSelectedId()-1;if(n>=0&&n<int(choices.size())){auto& x=choices[size_t(n)];choose(index,x.mode,x.input,x.mode==2?&x.ep:nullptr);}};addAndMakeVisible(source);source.setVisible(i<4);sourceValue.setVisible(i>=4);
        label(healthReadout,"U --  D --");healthReadout.setFont(FontOptions("Segoe UI",9.0f,Font::plain));healthReadout.setBorderSize(BorderSize<int>(0));healthReadout.setJustificationType(Justification::centredRight);
        label(latencyReadout,"LOCAL -- ms");latencyReadout.setFont(FontOptions("Segoe UI",9.0f,Font::plain));latencyReadout.setBorderSize(BorderSize<int>(0));latencyReadout.setJustificationType(Justification::centredLeft);
        DeskLook::styleNumber(readout);readout.setJustificationType(Justification::centred);
        knob(trim,c.trim,-24,24," dB");knob(low,c.low,-15,15," dB");knob(mid,c.mid,-15,15," dB");knob(high,c.high,-15,15," dB");
        auto eqColour=[](Slider& control,Label& name,Colour colour){control.getProperties().set("eqAccent",true);control.setColour(Slider::rotarySliderFillColourId,colour);control.setColour(Slider::textBoxTextColourId,palette::text);name.setColour(Label::textColourId,colour);};
        eqColour(low,lowLabel,Colour(0xff91adff));eqColour(mid,midLabel,Colour(0xfff895ca));eqColour(high,highLabel,Colour(0xffffdc91));
        auto gradient=[](Slider& control,Colour from,Colour to){control.getProperties().set("eqGradientStart",from.toString());control.getProperties().set("eqGradientEnd",to.toString());};
        gradient(low,Colour(0xff8299ff),Colour(0xffc494fb));gradient(mid,Colour(0xfff876be),Colour(0xffffb18c));gradient(high,Colour(0xffffbd8c),Colour(0xffffedab));
        knob(pan,c.pan,-1,1,"");trim.setSliderStyle(Slider::LinearVertical);trim.setTextBoxStyle(Slider::TextBoxBelow,false,40,18);trim.setTooltip("Input Gain: level adjustment before EQ and the channel fader.");pan.setSliderStyle(Slider::LinearHorizontal);pan.setTextBoxStyle(Slider::TextBoxRight,false,44,18);
        toggle(hpf,c.hpf);toggle(mute,c.mute);toggle(solo,c.solo);toggle(monitor,c.monitor);toggle(send,c.send);
        toggle(monitor2,c.monitor2);toggle(send2,c.send2);toggle(send3,c.send3);toggle(send4,c.send4);for(auto* route:{&monitor,&monitor2,&send,&send2,&send3,&send4,&mute,&solo}){route->getProperties().set("routeSwitch",true);route->getProperties().set("switchProgress",route->getToggleState()?1.0f:0.0f);}
        if(index>=4){addAndMakeVisible(apps);apps.setTooltip("Show apps using this virtual input and adjust their volumes.");apps.onClick=[this]{auto* parent=getTopLevelComponent();CallOutBox::launchAsynchronously(std::make_unique<AppVolumes>(std::wstring(engine.sourceIDs[index].toWideCharPointer())),parent->getLocalArea(&apps,apps.getLocalBounds()),parent);};}
        fader.setSliderStyle(Slider::LinearVertical);fader.setTextBoxStyle(Slider::NoTextBox,false,0,0);fader.setRange(-60,12,0.1);fader.setSkewFactorFromMidPoint(-12);fader.setValue(c.fader.load());fader.setDoubleClickReturnValue(true,0);fader.onValueChange=[this]{engine.channel[index].fader=float(fader.getValue());};fader.setTooltip("Channel gain: -60 to +12 dB. Double-click for unity (0 dB).");addAndMakeVisible(fader);addAndMakeVisible(meter);meter.attachGlow(c.glow);if(i<4)meter.attachDynamics(c);
        clear.onClick=[this]{engine.channel[index].clip=false;};clear.setTooltip("Clear the clipping indicator");addAndMakeVisible(clear);
        monitor.setTooltip("A1: route to the main hardware output");monitor2.setTooltip("A2: route to the second hardware output");send.setTooltip("B1: route to KMX6 Mic B1");send2.setTooltip("B2: route to KMX6 Mic B2");send3.setTooltip("B3: route to KMX6 Mic B3");send4.setTooltip("B4: route to KMX6 Mic B4");hpf.setTooltip("80 Hz high-pass filter");
    }
    void refresh(const std::vector<kmx6::Endpoint>& endpoints){
        auto& c=engine.channel[index];
        if(index>=4){
            const String wanted="KMX6 Computer "+String(index-3);
            if(auto* ep=kmx6::computerEndpoint(endpoints,index-4)){
                engine.selectSource(index,2,0,ep);
                const String device(ep->name);
                String display=device;
                if(device=="CABLE Input (VB-Audio Virtual Cable)")display="VB-CABLE / CABLE Input";
                else if(device=="Hi-Fi Cable Input (VB-Audio Hi-Fi Cable)")display="Hi-Fi Cable / Input";
                sourceValue.setText(display,dontSendNotification);
                sourceValue.setColour(Label::textColourId,palette::text);
                sourceValue.setTooltip("Virtual "+String(index-3)+" captures audio from this Windows playback device:\n"+device+"\nSelect this device as an app's audio output to send it here.");
            }else{
                engine.selectSource(index,0,0,nullptr);
                sourceValue.setText("No virtual cable found",dontSendNotification);
                sourceValue.setColour(Label::textColourId,palette::dim);
                sourceValue.setTooltip("Waiting for "+wanted+" or its supported virtual cable. Refresh devices after connecting it.");
            }
            return;
        }
        int oldMode=c.source.load(),oldInput=c.inputChannel.load();String oldID=engine.sourceIDs[index];
        source.clear(dontSendNotification);choices.clear();source.addItem("Disconnected",1);choices.push_back({});
        if(auto* d=engine.hardware.getCurrentAudioDevice()) {
            auto names=d->getInputChannelNames();source.addSectionHeading("Interface inputs (direct)");
            for(int i=0;i<names.size();++i){choices.push_back({1,i,{}});source.addItem(names[i],int(choices.size()));}
        }
        source.addSectionHeading("Other microphones");
        for(auto& ep:endpoints){if(forbidden(ep.name)||ep.playback||String(ep.name).containsIgnoreCase("KMX6"))continue;for(int channel=0;channel<std::min(ep.channels,256);++channel){choices.push_back({2,channel,ep});source.addItem(String(ep.name)+(ep.channels>1?" / input "+String(channel+1):String{}),int(choices.size()));}}
        source.setSelectedId(1,dontSendNotification);
        for(size_t i=0;i<choices.size();++i){auto& x=choices[i];if((oldMode==1&&x.mode==1&&x.input==oldInput)||(oldMode==2&&x.mode==2&&x.input==oldInput&&String(x.ep.id.c_str())==oldID))source.setSelectedId(int(i+1),dontSendNotification);}
    }
    void syncControls(){auto& c=engine.channel[index];fader.setValue(c.fader.load(),dontSendNotification);trim.setValue(c.trim.load(),dontSendNotification);low.setValue(c.low.load(),dontSendNotification);mid.setValue(c.mid.load(),dontSendNotification);high.setValue(c.high.load(),dontSendNotification);pan.setValue(c.pan.load(),dontSendNotification);hpf.setToggleState(c.hpf.load(),dontSendNotification);mute.setToggleState(c.mute.load(),dontSendNotification);solo.setToggleState(c.solo.load(),dontSendNotification);monitor.setToggleState(c.monitor.load(),dontSendNotification);send.setToggleState(c.send.load(),dontSendNotification);monitor2.setToggleState(c.monitor2.load(),dontSendNotification);send2.setToggleState(c.send2.load(),dontSendNotification);send3.setToggleState(c.send3.load(),dontSendNotification);send4.setToggleState(c.send4.load(),dontSendNotification);}
    ~Strip()override {for(int child=0;child<getNumChildComponents();++child)getChildComponent(child)->setComponentEffect(nullptr);}
    void updateLatency(){
        auto& c=engine.channel[index];const int mode=c.source.load();
        auto& queue=engine.capture[index].queue;
        const auto shortCount=[](uint64_t value){return value>9999?String("9k+"):String(value);};
        if(mode==2){const auto drops=queue.overflowEvents.load()+queue.backlogDrops.load()+engine.capture[index].discontinuities.load();healthReadout.setText("U "+shortCount(queue.underflows.load())+"  D "+shortCount(drops),dontSendNotification);}
        else healthReadout.setText("U --  D --",dontSendNotification);
        String health="Source counters since its last connection. U = queue underrun events. D = rejected packet/block, stale-backlog discard, or Windows capture discontinuity events; not a count of missing samples or guaranteed audible glitches.\n";
        if(mode==2)health+="Underruns: "+String(queue.underflows.load())+"\nOverflow events: "+String(queue.overflowEvents.load())+" ("+String(queue.overflows.load())+" frames rejected)\nBacklog discards: "+String(queue.backlogDrops.load())+" ("+String(queue.discardedFrames.load())+" frames skipped)\nCapture discontinuities: "+String(engine.capture[index].discontinuities.load())+"\nAdaptive safety reserve: "+String(queue.safetyReserve.load()*1000.0/std::max(1.0,queue.rate.load()),1)+" ms\n";
        else health+="Per-source counts are unavailable for inputs sharing the hardware driver.\n";
        health+="Shared engine callback overruns: "+String(engine.callbackOverruns.load())+"; late callbacks: "+String(engine.lateCallbacks.load());
        VirtualOutput* healthOutputs[]{&engine.b1,&engine.b2,&engine.b3,&engine.b4};const bool healthRoutes[]{c.send.load(),c.send2.load(),c.send3.load(),c.send4.load()};
        for(int b=0;b<4;++b)if(healthRoutes[b]){auto& q=healthOutputs[b]->queue;health+="\nB"+String(b+1)+" shared bus: U "+String(q.underflows.load())+" / D "+String(q.overflowEvents.load()+q.backlogDrops.load());}
        healthReadout.setTooltip(health);
        bool ready=engine.active.load()&&engine.bufferSize.load()>0&&mode!=0&&(mode!=2||engine.capture[index].running.load());
        if(!ready){latencyReadout.setText("LOCAL -- ms",dontSendNotification);latencyReadout.setTooltip("Local latency unavailable while audio is stopped or this source is disconnected.");return;}
        const double sr=std::max(1.0,engine.sampleRate.load());
        const double queued=mode==2?engine.capture[index].queue.bufferedMs.load():0.0;
        const double block=1000.0*engine.bufferSize.load()/sr;
        const double effects=1000.0*engine.plugins.activeLatencySamples[index].load()/sr;
        const double local=queued+block+effects;
        latencyReadout.setText("LOCAL ~"+String(local,1)+" ms",dontSendNotification);
        String details="Live local latency estimate (not microphone-to-app measurement).\nCapture queue: "+String(queued,1)+" ms\nEngine block allowance: "+String(block,1)+" ms\nActive plugin delay: "+String(effects,1)+" ms";
        const bool routed[]{c.send.load(),c.send2.load(),c.send3.load(),c.send4.load()};
        VirtualOutput* outputs[]{&engine.b1,&engine.b2,&engine.b3,&engine.b4};
        for(int b=0;b<4;++b)if(routed[b]){details+="\nB"+String(b+1)+": ";details+=outputs[b]->connected.load()?"~"+String(local+outputs[b]->queue.bufferedMs.load(),1)+" ms through local output queue":"disconnected";}
        details+="\nExcludes hardware/driver latency, upstream cable buffering, virtual host/Windows buffers, and receiving-app buffering. A loopback test is required for end-to-end latency.";
        latencyReadout.setTooltip(details);
    }
    void tick(){if(++latencyTicks%6==0)updateLatency();for(auto* route:{&monitor,&monitor2,&send,&send2,&send3,&send4,&mute,&solo}){float progress=float(route->getProperties()["switchProgress"]),target=route->getToggleState()?1.0f:0.0f;if(progress!=target){progress+=std::clamp(target-progress,-0.12f,0.12f);if(std::abs(progress-target)<0.001f)progress=target;route->getProperties().set("switchProgress",progress);route->repaint();}}auto& c=engine.channel[index];float activity=c.inputActivity.exchange(0,std::memory_order_relaxed);if(engine.active&&c.source.load()!=0&&activity>0.00056f)activityHold=300;else if(activityHold>0)--activityHold;float target=engine.active&&c.source.load()!=0&&activityHold>0?0.0f:0.55f;bool dimmed=target>0;if(dimmed!=monochromeApplied){monochromeApplied=dimmed;for(int child=0;child<getNumChildComponents();++child){auto* component=getChildComponent(child);if(component!=&source)component->setComponentEffect(dimmed?&monochrome:nullptr);}}float next=inactiveShade+(target-inactiveShade)*0.18f;if(std::abs(next-target)<0.002f)next=target;if(next!=inactiveShade){inactiveShade=next;repaint();}if(index<4)meter.setLimiterReduction(c.limiterReduction.load());meter.update(c.peakL.load(),c.peakR.load(),c.clip.load());readout.setText(String(c.fader.load(),1)+" dB",dontSendNotification);int mode=c.source.load();statusLabel.setText(mode==2?String(engine.capture[index].status()):mode==1?"Main hardware input":index>=4?"Waiting for fixed loopback source":"Disconnected",dontSendNotification);}
    void paintOverChildren(Graphics& g)override {if(inactiveShade>0){Graphics::ScopedSaveState state(g);g.excludeClipRegion(source.getBounds());g.setColour(Colour(0xff141414).withAlpha(inactiveShade));g.fillRect(getLocalBounds());}}
    void paint(Graphics& g)override {
        const int deckTop=116;const bool microphone=index<4;
        // A real background-colored gap separates source selection from controls.
        g.fillAll(palette::bg);g.setColour(palette::panel);g.fillRect(0,0,getWidth(),108);
        ColourGradient surface(microphone?Colour(0xff564660):Colour(0xff51546a),1.0f,float(deckTop),microphone?Colour(0xff292230):Colour(0xff262635),float(getWidth()-1),float(getHeight()-1),false);
        surface.addColour(0.4,microphone?Colour(0xff40324d):Colour(0xff3a3b50));
        g.setGradientFill(surface);g.fillRect(0,deckTop,getWidth(),std::max(0,getHeight()-deckTop));
        g.setColour(Colour(0xff1b1826));g.drawRect(0,0,getWidth(),108,1);g.drawRect(0,deckTop,getWidth(),getHeight()-deckTop,1);
        g.setColour(Colours::white.withAlpha(0.09f));g.drawHorizontalLine(deckTop,1,float(getWidth()-1));
        g.setColour(palette::raised);g.fillRect(1,1,getWidth()-2,43);
        bool active=engine.active&&engine.channel[index].source.load()!=0&&activityHold>0;
        if(active){for(int spread=7;spread>=1;--spread){g.setColour(palette::teal.withAlpha(0.025f*float(8-spread)));g.fillRect(1,1,getWidth()-2,3+spread);}g.setColour(palette::teal.brighter(0.5f));}else g.setColour(palette::line);
        g.fillRect(1,1,getWidth()-2,3);
    }
    void resized()override {
        latencyReadout.setBounds(12,getHeight()-16,getWidth()-110,14);healthReadout.setBounds(getWidth()-94,getHeight()-16,82,14);
        int w=getWidth();title.setBounds(14,6,w-28,22);subtitle.setBounds(14,27,w-28,16);sourceLabel.setBounds(12,48,w-24,14);source.setBounds(12,65,w-24,24);sourceValue.setBounds(12,65,w-24,24);statusLabel.setBounds(12,91,w-24,17);
        hpf.setBounds(12,120,64,24);lowLabel.setBounds(8,148,70,14);low.setBounds(8,164,70,58);midLabel.setBounds(8,226,70,14);mid.setBounds(8,242,70,58);highLabel.setBounds(8,304,70,14);high.setBounds(8,320,70,58);
        int gainX=w-140,levelX=w-96,meterX=w-52,bottom=getHeight()-174;volumeLabel.setBounds(levelX,150,40,18);gainLabel.setBounds(gainX,150,40,18);meterLabel.setBounds(meterX,150,40,18);for(auto* name:{&volumeLabel,&gainLabel,&meterLabel})name->setJustificationType(Justification::centred);fader.setBounds(levelX,176,40,bottom-176);trim.setBounds(gainX,176,40,bottom-176+22);meter.setBounds(meterX,176,40,bottom-176);readout.setBounds(levelX,bottom+8,40,22);clear.setBounds(meterX,bottom+8,40,22);
        monitor.setBounds(6,388,38,34);monitor2.setBounds(46,388,38,34);send.setBounds(6,428,38,34);send2.setBounds(46,428,38,34);send3.setBounds(6,468,38,34);send4.setBounds(46,468,38,34);apps.setBounds(82,120,64,24);pluginsButton.setBounds(12,getHeight()-134,w-24,26);mute.setBounds(12,getHeight()-102,(w-28)/2,34);solo.setBounds(16+(w-28)/2,getHeight()-102,(w-28)/2,34);panLabel.setBounds(12,getHeight()-63,w-24,14);pan.setBounds(12,getHeight()-45,w-24,28);
    }
};

class HardwarePanel : public Component,private Timer {
    DeskLook look;
    Label title,a1Label,a2Label,a1Help,a2Help,channelsA,channelsB,rateLabel,bufferLabel,note;
    ComboBox a1,a2,mapA,mapB,rates,buffers;
    TextButton driver{"DRIVER SETTINGS"},advanced{"ADVANCED"};
    ComboBox& destination;AudioDeviceManager& manager;MixerEngine& mixer;std::function<void()> save;
    Array<double> rateChoices;Array<int> bufferChoices;
    Viewport advancedViewport;
    void label(Label& target,const String& text,Colour color=palette::dim,float size=11){addAndMakeVisible(target);target.setText(text,dontSendNotification);target.setColour(Label::textColourId,color);target.setFont(FontOptions(size));}
    void showError(const String& error){if(error.isNotEmpty())AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon,"Hardware settings",error);}
    void updateMap(ComboBox& box,AudioIODevice* device,int selected){StringArray wanted;if(device){auto channels=device->getOutputChannelNames();for(int i=0;i<std::min(256,channels.size());++i)wanted.add(channels[i]+(i+1<channels.size()?" + "+channels[i+1]:" (mono)"));}StringArray present;for(int i=0;i<box.getNumItems();++i)present.add(box.getItemText(i));if(wanted!=present){box.clear(dontSendNotification);for(int i=0;i<wanted.size();++i)box.addItem(wanted[i],i+1);}box.setSelectedId(selected+1,dontSendNotification);box.setEnabled(!wanted.isEmpty());}
    void updateTiming(AudioIODevice* device){
        if(!device){rates.setEnabled(false);buffers.setEnabled(false);driver.setEnabled(false);return;}
        rates.setEnabled(true);buffers.setEnabled(true);driver.setEnabled(device->hasControlPanel());
        auto availableRates=device->getAvailableSampleRates();auto availableBuffers=device->getAvailableBufferSizes();
        if(availableRates!=rateChoices){rateChoices=availableRates;rates.clear(dontSendNotification);for(int i=0;i<rateChoices.size();++i)rates.addItem(String(rateChoices[i]/1000.0,1)+" kHz",i+1);}
        if(availableBuffers!=bufferChoices){bufferChoices=availableBuffers;buffers.clear(dontSendNotification);for(int i=0;i<bufferChoices.size();++i)buffers.addItem(String(bufferChoices[i])+" samples",i+1);}
        int selectedRate=rateChoices.indexOf(device->getCurrentSampleRate());rates.setSelectedId(selectedRate+1,dontSendNotification);
        int selectedBuffer=bufferChoices.indexOf(device->getCurrentBufferSizeSamples());buffers.setSelectedId(selectedBuffer+1,dontSendNotification);
        buffers.setTooltip("One audio buffer: "+String(device->getCurrentBufferSizeSamples()*1000.0/device->getCurrentSampleRate(),2)+" ms. This is not total microphone-to-app latency.");
    }
    void timerCallback()override{
        for(auto* box:{&a1,&a2,&mapA,&mapB,&rates,&buffers})if(box->isPopupActive())return;
        auto* main=manager.getCurrentAudioDevice();updateMap(mapA,main,mixer.a1Pair);updateMap(mapB,mixer.a2Internal?main:mixer.busDevice.getCurrentAudioDevice(),mixer.a2Pair);mapB.setEnabled(mapB.isEnabled()&&(mixer.a2Internal||mixer.sending));updateTiming(main);
        if(main){auto selected=main->getTypeName()+" | "+main->getName();for(int i=0;i<a1.getNumItems();++i)if(a1.getItemText(i)==selected)a1.setSelectedItemIndex(i,dontSendNotification);}
        StringArray choices,current;for(int i=0;i<destination.getNumItems();++i)choices.add(i==0?"Off - no second output":destination.getItemText(i));for(int i=0;i<a2.getNumItems();++i)current.add(a2.getItemText(i));
        if(choices!=current){a2.clear(dontSendNotification);for(int i=0;i<destination.getNumItems();++i)a2.addItem(i==0?"Off - no second output":destination.getItemText(i),destination.getItemId(i));}
        a2.setSelectedId(destination.getSelectedId(),dontSendNotification);
    }
    void changeTiming(){AudioDeviceManager::AudioDeviceSetup setup;manager.getAudioDeviceSetup(setup);int r=rates.getSelectedId()-1,b=buffers.getSelectedId()-1;if(r>=0&&r<rateChoices.size())setup.sampleRate=rateChoices[r];if(b>=0&&b<bufferChoices.size())setup.bufferSize=bufferChoices[b];auto error=manager.setAudioDeviceSetup(setup,true);if(error.isNotEmpty())showError(error);else save();timerCallback();}
public:
    HardwarePanel(MixerEngine& engine,ComboBox& output,std::function<void()> fn):destination(output),manager(engine.hardware),mixer(engine),save(std::move(fn)){
        setLookAndFeel(&look);label(title,"HARDWARE OUTPUTS",palette::text,14);title.setFont(FontOptions(14,Font::bold));
        label(a1Label,"A1 / MAIN OUTPUT",palette::teal,11);label(a2Label,"A2 / SECOND OUTPUT",palette::teal,11);
        label(a1Help,"Choose headphones or speakers. A1 sets the audio timing.");label(a2Help,"Optional extra output. Turn on A2 in a channel to send audio here.");
        label(channelsA,"Channels");label(channelsB,"Channels");label(rateLabel,"Sample rate");label(bufferLabel,"Buffer size");
        label(note,"Smaller buffers reduce delay but may cause crackles.\nAdvanced: enable microphone inputs or additional interface outputs.");
        for(auto* box:{&a1,&a2,&mapA,&mapB,&rates,&buffers})addAndMakeVisible(*box);addAndMakeVisible(driver);addAndMakeVisible(advanced);addChildComponent(advancedViewport);
        mapA.setTooltip("Which physical output pair receives A1.");mapB.setTooltip("Which physical output pair receives A2.");rates.setTooltip("A1 sample rate. Virtual microphones always use 48 kHz.");driver.setTooltip("Open the selected audio interface's own driver settings.");advanced.setTooltip("A1 input and output channel enablement, device reset and advanced driver controls.");
        int choice=1;for(auto* type:manager.getAvailableDeviceTypes()){auto backend=type->getTypeName();if(!backend.containsIgnoreCase("ASIO")&&!backend.containsIgnoreCase("Windows Audio"))continue;type->scanForDevices();for(auto& name:type->getDeviceNames(false))if(!forbidden(name)&&!name.containsIgnoreCase("KMX6"))a1.addItem(backend+" | "+name,choice++);}
        a1.onChange=[this]{auto selected=a1.getText();auto backend=selected.upToFirstOccurrenceOf(" | ",false,false),name=selected.fromFirstOccurrenceOf(" | ",false,false);auto oldType=manager.getCurrentAudioDeviceType();AudioDeviceManager::AudioDeviceSetup old;manager.getAudioDeviceSetup(old);manager.setCurrentAudioDeviceType(backend,true);AudioDeviceManager::AudioDeviceSetup setup;manager.getAudioDeviceSetup(setup);setup.outputDeviceName=name;if(backend.containsIgnoreCase("ASIO"))setup.inputDeviceName=name;setup.useDefaultOutputChannels=true;setup.outputChannels.clear();setup.sampleRate=0;setup.bufferSize=0;auto error=manager.setAudioDeviceSetup(setup,true);if(error.isNotEmpty()){manager.setCurrentAudioDeviceType(oldType,true);manager.setAudioDeviceSetup(old,true);showError(error);}if(auto* d=manager.getCurrentAudioDevice()){if(mixer.a1Pair>=d->getOutputChannelNames().size())mixer.a1Pair=0;showError(mixer.configureOutputs(manager,mixer.a1Pair));}if(destination.onChange)destination.onChange();save();timerCallback();};
        a2.onChange=[this]{destination.setSelectedId(a2.getSelectedId(),sendNotificationSync);timerCallback();};
        auto mapping=[this]{auto error=mixer.setOutputPairs(std::max(0,mapA.getSelectedId()-1),std::max(0,mapB.getSelectedId()-1));if(error.isNotEmpty())showError(error);else save();timerCallback();};mapA.onChange=mapping;mapB.onChange=mapping;
        rates.onChange=[this]{changeTiming();};buffers.onChange=[this]{changeTiming();};driver.onClick=[this]{if(auto* d=manager.getCurrentAudioDevice();d&&d->hasControlPanel()){Component::SafePointer<HardwarePanel> safe(this);d->showControlPanel();if(safe){safe->save();safe->timerCallback();}}};
        advanced.setClickingTogglesState(true);advanced.onClick=[this]{bool open=advanced.getToggleState();if(open&&!advancedViewport.getViewedComponent()){auto* selector=new AudioDeviceSelectorComponent(manager,0,256,0,256,false,false,true,false);selector->setItemHeight(24);selector->setSize(480,470);advancedViewport.setViewedComponent(selector,true);}advancedViewport.setVisible(open);if(!open)save();advanced.setButtonText(open?"LESS":"ADVANCED");setSize(520,open?746:394);};
        setSize(520,394);timerCallback();startTimer(500);
    }
    ~HardwarePanel()override{stopTimer();advancedViewport.setViewedComponent(nullptr);setLookAndFeel(nullptr);}
    void paint(Graphics& g)override{g.fillAll(palette::panel);g.setColour(palette::line.withAlpha(0.4f));g.drawHorizontalLine(144,14,float(getWidth()-14));g.drawHorizontalLine(260,14,float(getWidth()-14));}
    void resized()override{

        int w=getWidth();title.setBounds(14,8,w-28,26);a1Label.setBounds(14,40,w-28,18);a1Help.setBounds(14,58,w-28,20);a1.setBounds(14,82,w-28,26);channelsA.setBounds(14,114,66,24);mapA.setBounds(84,114,w-98,24);
        a2Label.setBounds(14,152,w-28,18);a2Help.setBounds(14,170,w-28,20);a2.setBounds(14,194,w-28,26);channelsB.setBounds(14,226,66,24);mapB.setBounds(84,226,w-98,24);
        rateLabel.setBounds(14,270,74,26);rates.setBounds(92,270,130,26);bufferLabel.setBounds(238,270,66,26);buffers.setBounds(310,270,w-324,26);note.setBounds(14,304,w-28,42);driver.setBounds(14,354,150,28);advanced.setBounds(174,354,110,28);advancedViewport.setBounds(8,394,w-16,344);
    }
};

class HeaderVisualizer : public Component {
    struct Particle {float u,v,size;};std::array<Particle,720> particles;
    float phase=0,energy=0,bass=0,treble=0;double previous=0;
public:
    HeaderVisualizer(){setInterceptsMouseClicks(false,false);Random random(78231);for(auto& p:particles)p={random.nextFloat()*MathConstants<float>::twoPi,random.nextFloat()*MathConstants<float>::twoPi,0.45f+random.nextFloat()*0.5f};}
    void update(MixerEngine& engine){const double now=Time::getMillisecondCounterHiRes()*0.001;if(previous&&now-previous<1.0/40.0)return;float dt=float(previous?std::min(0.1,now-previous):0);previous=now;float low=0,mid=0,high=0;if(engine.active){for(auto& channel:engine.channel)for(int side=0;side<2;++side){low=std::max(low,channel.glow.bands[side][0].load(std::memory_order_relaxed));mid=std::max(mid,channel.glow.bands[side][1].load(std::memory_order_relaxed));high=std::max(high,channel.glow.bands[side][2].load(std::memory_order_relaxed));}}float target=std::clamp(std::sqrt((low+mid+high)*4.0f),0.0f,1.0f),k=1-std::exp(-dt*(target>energy?48.0f:10.0f));energy+=k*(target-energy);bass+=k*(std::clamp(std::sqrt(low)*2.2f,0.0f,1.0f)-bass);treble+=k*(std::clamp(std::sqrt(high)*3.0f,0.0f,1.0f)-treble);phase+=dt*(0.28f+energy*2.1f);repaint();}
    void paint(Graphics& g)override {
        const float width=float(getWidth()),cy=getHeight()*0.5f,amplitude=getHeight()*0.43f;
        const int count=std::clamp(getWidth()/5,32,240);
        Path bars;for(int i=0;i<count;++i){float u=float(i)/float(count-1),x=4+u*(width-8),edge=std::pow(std::sin(u*MathConstants<float>::pi),0.7f);float shape=std::abs(std::sin(u*15-phase*2.5f))*bass+std::abs(std::sin(u*27+phase*3.1f))*energy*0.65f+std::abs(std::sin(u*43-phase*4))*treble*0.5f;float height=1+amplitude*edge*std::clamp(shape,0.0f,1.0f);bars.addRectangle(x-0.8f,cy-height,1.6f,height*2);}ColourGradient gradient(Colour(0xffa775df).withAlpha(0.05f),0,cy-amplitude,Colour(0xffa775df).withAlpha(0.05f),0,cy+amplitude,false);gradient.addColour(0.5,Colour(0xffc4e9e6).withAlpha(0.9f));g.setGradientFill(gradient);g.fillPath(bars);        for(int wave=0;wave<3;++wave){Path line;for(int i=0;i<=160;++i){float u=float(i)/160,x=u*width,y=cy+std::sin(u*(12+wave*4)+phase*(wave%2?-1.8f:1.6f)+wave)*amplitude*energy*(0.3f+wave*0.13f)*std::sin(u*MathConstants<float>::pi);if(i==0)line.startNewSubPath(x,y);else line.lineTo(x,y);}g.setColour(Colour(0xffb891ec).withAlpha(0.16f+energy*0.18f));g.strokePath(line,PathStrokeType(0.7f));}
    }
};
class MainComponent : public Component,private Timer,private ChangeListener {
public:
    MixerEngine engine;
private:
    DeskLook look;TooltipWindow tips{this,700};ComponentDragger windowDragger;
    std::array<std::unique_ptr<Strip>,6> strips;
    TextButton power{"START AUDIO"},settings{"HARDWARE"},refresh{"RESCAN"},save{"SAVE"},cables{"VIRTUAL I/O"},information{"i"};
    TextButton minimiseWindow{"-"},closeWindow{"X"};
    uiScale::StepButton uiScaleButton;
    ChildProcess cableHost, virtualSetup;
    bool waitingForVirtualSetup=false,automaticReconnectBlocked=false,repairFailed=false;
    bool manualVirtualSetup=false;double lastVirtualAttempt=0,virtualDisconnectedSince=0;
    ComboBox sendDevice;
    TextButton engineRefresh{"REFRESH ENGINE"},pluginManager{"PLUGIN MANAGER"};
    std::unique_ptr<PluginManagerWindow> pluginManagerWindow;
    AnimatedFader master;
    AnimatedFader a2Master,b1Master,b2Master,b3Master,b4Master;
    Label a2Label,b2Label,b3Label,b4Label,hardwareGroupLabel,virtualGroupLabel;
    HeaderVisualizer headerVisualizer;ImageComponent brandIcon;
    Label headline,tagline,status,monitorLabel,sendLabel,masterValue,engineInfo,driverInfo,hint,a2MasterValue,b1MasterValue,b2MasterValue,b3MasterValue,b4MasterValue;
    Meter masterMeter,sendMeter;
    Meter a2Meter,b2Meter,b3Meter,b4Meter;
    std::vector<kmx6::Endpoint> endpoints;
    std::vector<std::pair<int,int>> storedModes;
    std::array<String,6> storedIDs;
    String storedA2Device;
    std::unique_ptr<XmlElement> cachedXml;
    bool refreshing=false;
    int ticks=0;
    void text(Label& l,String s,float size=12){l.setText(s,dontSendNotification);l.setFont(FontOptions(size));addAndMakeVisible(l);}
    void showError(String s){AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon,"KMX6",s);}
    bool virtualEndpointsReady(const std::vector<kmx6::Endpoint>& devices)const{
        for(int i=1;i<=4;++i){bool found=false;for(const auto& ep:devices)if(!ep.playback&&String(ep.name).containsIgnoreCase("KMX6 Mic B"+String(i))){found=true;break;}if(!found)return false;}
        return kmx6::computerEndpoint(devices,0)&&kmx6::computerEndpoint(devices,1);
    }
    void beginVirtualSetup(bool manual){
        if(waitingForVirtualSetup)return;
        const auto script=appFolder().getParentDirectory().getChildFile("Setup-VirtualIO.ps1");
        if(!script.existsAsFile()){repairFailed=true;cables.setTooltip("Setup-VirtualIO.ps1 is missing beside the application folder. Restore it from the KMX6 package.");return;}
        manualVirtualSetup=manual;lastVirtualAttempt=Time::getMillisecondCounterHiRes();
        // ReconnectOnly never self-elevates, installs drivers, renames, or detaches devices.
        waitingForVirtualSetup=virtualSetup.start(StringArray{File::getSpecialLocation(File::windowsSystemDirectory).getChildFile("WindowsPowerShell/v1.0/powershell.exe").getFullPathName(),"-NoProfile","-ExecutionPolicy","Bypass","-WindowStyle","Hidden","-File",script.getFullPathName(),manual?"-InstallDriver":"-ReconnectOnly"},0);
        if(!waitingForVirtualSetup){repairFailed=true;if(!manual)automaticReconnectBlocked=true;}
        cables.setEnabled(!waitingForVirtualSetup);
    }
    void maintainVirtualDevices(){
        const double now=Time::getMillisecondCounterHiRes();
        const int linked=int(engine.b1.connected)+int(engine.b2.connected)+int(engine.b3.connected)+int(engine.b4.connected);
        if(linked==4){virtualDisconnectedSince=0;if(repairFailed&&!waitingForVirtualSetup&&virtualEndpointsReady(kmx6::enumerateEndpoints())){repairFailed=false;automaticReconnectBlocked=false;}}
        else if(engine.active){if(virtualDisconnectedSince==0)virtualDisconnectedSince=now;else if(now-virtualDisconnectedSince>60000&&!waitingForVirtualSetup)repairFailed=true;}
        if(!waitingForVirtualSetup&&linked<4&&now-lastVirtualAttempt>15000){
            auto host=appFolder().getChildFile("KMX6CableHost.exe");if(!cableHost.isRunning()&&host.existsAsFile())cableHost.start(StringArray{host.getFullPathName()},0);
            auto devices=kmx6::enumerateEndpoints();
            if(!virtualEndpointsReady(devices)&&!automaticReconnectBlocked)beginVirtualSetup(false);
            else lastVirtualAttempt=now;
        }
    }
    void refreshDevices(){
        refreshing=true;endpoints=kmx6::enumerateEndpoints();
        
        for(auto& strip:strips)strip->refresh(endpoints);
        String current=sendDevice.getText().isNotEmpty()?sendDevice.getText():storedA2Device;sendDevice.clear(dontSendNotification);sendDevice.addItem("A2 - disconnected",1);int id=2;
        if(auto* device=engine.hardware.getCurrentAudioDevice();device&&device->getActiveOutputChannels().countNumberOfSetBits()>=4)sendDevice.addItem("Main device outputs 3 + 4",id++);
        for(auto* type:engine.hardware.getAvailableDeviceTypes()){auto backend=type->getTypeName();if(!backend.containsIgnoreCase("ASIO")&&!backend.containsIgnoreCase("Windows Audio"))continue;type->scanForDevices();for(auto& name:type->getDeviceNames(false))if(!forbidden(name)&&!name.containsIgnoreCase("KMX6"))sendDevice.addItem(backend+" | "+name,id++);}
        sendDevice.setSelectedId(1,dontSendNotification);for(int i=0;i<sendDevice.getNumItems();++i)if(sendDevice.getItemText(i)==current||sendDevice.getItemText(i).endsWith(" | "+current))sendDevice.setSelectedItemIndex(i,dontSendNotification);
        refreshing=false;
    }
    bool feedbackRisk(const kmx6::Endpoint& ep) {
        if(!ep.playback)return false;String name=ep.name;
        if(name==engine.sendName)return true;
        if(auto* d=engine.hardware.getCurrentAudioDevice()){
            auto n=d->getName();if(name==n)return true;

        }return false;
    }
    void choose(int i,int mode,int in,const kmx6::Endpoint* ep){
        if(ep&&feedbackRisk(*ep)){showError("That playback device is also a mixer output. Choose a separate source to prevent a feedback loop.");return;}

        engine.error.clear();engine.selectSource(i,mode,in,ep);if(engine.error.isNotEmpty()){showError(engine.error);strips[i]->refresh(endpoints);}
    }
    void restoreSources(){
        std::array<int,4> modes{engine.channel[0].source.load(),engine.channel[1].source.load(),engine.channel[2].source.load(),engine.channel[3].source.load()};auto microphoneIDs=engine.sourceIDs;
        refreshDevices();
        for(int i=0;i<4;++i){if(modes[i]==1)engine.selectSource(i,1,engine.channel[i].inputChannel.load(),nullptr);else if(modes[i]==2){bool found=false;for(auto& ep:endpoints)if(!ep.playback&&String(ep.id.c_str())==microphoneIDs[i]){choose(i,2,engine.channel[i].inputChannel.load(),&ep);found=true;break;}if(!found){engine.capture[i].stop();engine.sourceIDs[i]=microphoneIDs[i];engine.channel[i].source=2;status.setText("Mic "+String(i+1)+" disconnected; waiting to reconnect",dontSendNotification);}}}
        if(storedA2Device.isNotEmpty()){
            int found=-1;
            for(int i=0;i<sendDevice.getNumItems();++i)if(sendDevice.getItemText(i)==storedA2Device||sendDevice.getItemText(i).endsWith(" | "+storedA2Device)){found=i;break;}
            if(found>=0){sendDevice.setSelectedItemIndex(found,dontSendNotification);auto error=engine.openSend(sendDevice.getText());if(error.isNotEmpty())showError("Could not reopen the saved A2 output: "+error);}
            else showError("The saved A2 output is unavailable. Choose it again from the A2 selector.");
        }
    }    void load(){
        auto v=JSON::parse(settingsFile());auto* root=v.getDynamicObject();if(!root)return;
        engine.masterDb=float(root->getProperty("masterDb"));if(root->hasProperty("a1Pair"))engine.a1Pair=std::clamp(int(root->getProperty("a1Pair")),0,255);if(root->hasProperty("a2Pair"))engine.a2Pair=std::clamp(int(root->getProperty("a2Pair")),0,255);
        if(root->hasProperty("a2Db"))engine.a2Db=float(root->getProperty("a2Db"));if(root->hasProperty("a2Device"))storedA2Device=root->getProperty("a2Device").toString();if(root->hasProperty("b1Db"))engine.b1Db=float(root->getProperty("b1Db"));if(root->hasProperty("b2Db"))engine.b2Db=float(root->getProperty("b2Db"));if(root->hasProperty("b3Db"))engine.b3Db=float(root->getProperty("b3Db"));if(root->hasProperty("b4Db"))engine.b4Db=float(root->getProperty("b4Db"));
        auto xml=root->getProperty("hardwareXml").toString();if(xml.isNotEmpty())engine.savedDevice=parseXML(xml);
        if(auto* array=root->getProperty("channels").getArray())for(int i=0;i<std::min(6,array->size());++i)if(auto* obj=(*array)[i].getDynamicObject()){
            int target=(array->size()<=4&&i>=2)?i+2:i;
            auto& c=engine.channel[i];auto read=[&](String k,std::atomic<float>& field,float min,float max){if(obj->hasProperty(k))field=std::clamp(float(obj->getProperty(k)),min,max);};
            read("fader",c.fader,-60,12);read("trim",c.trim,-24,24);read("low",c.low,-15,15);read("mid",c.mid,-15,15);read("high",c.high,-15,15);read("pan",c.pan,-1,1);read("limiterThreshold",c.limiterThreshold,-30,0);read("limiterRelease",c.limiterRelease,10,1000);read("gateThreshold",c.gateThreshold,-60,0);read("gateAttack",c.gateAttack,0.1f,30);read("gateRelease",c.gateRelease,10,1000);read("gateHold",c.gateHold,0,500);read("gateHysteresis",c.gateHysteresis,0,18);if(obj->hasProperty("gateEnabled"))c.gateEnabled=bool(obj->getProperty("gateEnabled"));if(obj->hasProperty("limiterEnabled"))c.limiterEnabled=bool(obj->getProperty("limiterEnabled"));
            c.hpf=bool(obj->getProperty("hpf"));c.mute=bool(obj->getProperty("mute"));c.monitor=bool(obj->getProperty("monitor"));c.send=bool(obj->getProperty("send"));
            c.monitor2=bool(obj->getProperty("monitor2"));c.send2=bool(obj->getProperty("send2"));if(obj->hasProperty("send3"))c.send3=bool(obj->getProperty("send3"));if(obj->hasProperty("send4"))c.send4=bool(obj->getProperty("send4"));
            storedModes.push_back({int(obj->getProperty("source")),int(obj->getProperty("input"))});storedIDs[target]=obj->getProperty("endpoint").toString();
            if(target!=i){auto& migrated=engine.channel[target];migrated.fader=c.fader.load();migrated.trim=c.trim.load();migrated.low=c.low.load();migrated.mid=c.mid.load();migrated.high=c.high.load();migrated.pan=c.pan.load();migrated.hpf=c.hpf.load();migrated.mute=c.mute.load();migrated.monitor=c.monitor.load();migrated.monitor2=c.monitor2.load();migrated.send=c.send.load();migrated.send2=c.send2.load();migrated.send3=c.send3.load();migrated.send4=c.send4.load();migrated.source=int(obj->getProperty("source"));migrated.inputChannel=int(obj->getProperty("input"));engine.sourceIDs[target]=storedIDs[target];}else{c.source=storedModes.back().first;c.inputChannel=storedModes.back().second;engine.sourceIDs[i]=storedIDs[i];}
        }
    }
    void saveSettings(){
        engine.plugins.save();auto root=new DynamicObject;root->setProperty("version",1);root->setProperty("masterDb",engine.masterDb.load());
        root->setProperty("a1Pair",engine.a1Pair.load());root->setProperty("a2Pair",engine.a2Pair.load());root->setProperty("a2Db",engine.a2Db.load());root->setProperty("a2Device",storedA2Device);root->setProperty("b1Db",engine.b1Db.load());root->setProperty("b2Db",engine.b2Db.load());root->setProperty("b3Db",engine.b3Db.load());root->setProperty("b4Db",engine.b4Db.load());
        auto xml=engine.hardware.createStateXml();if(xml)engine.savedDevice=std::move(xml);if(engine.savedDevice)root->setProperty("hardwareXml",engine.savedDevice->toString());
        Array<var> array;for(int i=0;i<6;++i){auto& c=engine.channel[i];auto o=new DynamicObject;
            o->setProperty("fader",c.fader.load());o->setProperty("trim",c.trim.load());o->setProperty("low",c.low.load());o->setProperty("mid",c.mid.load());o->setProperty("high",c.high.load());o->setProperty("pan",c.pan.load());o->setProperty("hpf",c.hpf.load());o->setProperty("mute",c.mute.load());o->setProperty("monitor",c.monitor.load());o->setProperty("send",c.send.load());
            o->setProperty("limiterEnabled",c.limiterEnabled.load());o->setProperty("gateEnabled",c.gateEnabled.load());o->setProperty("limiterThreshold",c.limiterThreshold.load());o->setProperty("limiterRelease",c.limiterRelease.load());o->setProperty("gateThreshold",c.gateThreshold.load());o->setProperty("gateAttack",c.gateAttack.load());o->setProperty("gateRelease",c.gateRelease.load());o->setProperty("gateHold",c.gateHold.load());o->setProperty("gateHysteresis",c.gateHysteresis.load());o->setProperty("source",engine.channel[i].source.load());o->setProperty("input",c.inputChannel.load());o->setProperty("endpoint",engine.sourceIDs[i]);array.add(var(o));}
        for(int i=0;i<array.size();++i){auto* o=array[i].getDynamicObject();o->setProperty("monitor2",engine.channel[i].monitor2.load());o->setProperty("send2",engine.channel[i].send2.load());o->setProperty("send3",engine.channel[i].send3.load());o->setProperty("send4",engine.channel[i].send4.load());}
        root->setProperty("channels",array);if(!settingsFile().replaceWithText(JSON::toString(var(root))))showError("Could not save settings beside the application.");
    }
    void changeListenerCallback(ChangeBroadcaster*)override {
        if(auto* d=engine.hardware.getCurrentAudioDevice();d&&forbidden(d->getName())) {engine.stop();showError("VB-Audio devices are excluded. Select a physical audio device.");}
        refreshDevices();
    }
public:
    bool scaleAnimationActive=false;
    void setScaleAnimationActive(bool active){if(active==scaleAnimationActive)return;scaleAnimationActive=active;if(active)stopTimer();else startTimerHz(60);}
    std::function<void(int)> stepUiScale;
    void setUiScaleLabel(int value){uiScaleButton.setButtonText("SCALE "+String(value)+"%");}
    void mouseDown(const MouseEvent& event)override {if(event.y<96)windowDragger.startDraggingComponent(getTopLevelComponent(),event);}
    void mouseDrag(const MouseEvent& event)override {if(event.getMouseDownPosition().y<96)windowDragger.dragComponent(getTopLevelComponent(),event,nullptr);}
    MainComponent(){
        setLookAndFeel(&look);load();addAndMakeVisible(pluginManager);pluginManager.setTooltip("Plugin Manager: installed effects and scan folders");pluginManager.onClick=[this]{if(!pluginManagerWindow){pluginManagerWindow=std::make_unique<PluginManagerWindow>(engine.plugins.catalog);pluginManagerWindow->setLookAndFeel(&look);}pluginManagerWindow->setVisible(true);pluginManagerWindow->toFront(true);};addAndMakeVisible(information);information.setTooltip("Dependencies and credits");information.onClick=[this]{CallOutBox::launchAsynchronously(std::make_unique<CreditsPanel>(),getLocalArea(&information,information.getLocalBounds()),this);};
         addAndMakeVisible(headerVisualizer);text(headline,"KMX6",28);headline.setFont(FontOptions(28,Font::bold));tagline.setVisible(false);addAndMakeVisible(brandIcon);brandIcon.setImage(ImageCache::getFromMemory(CreditsAssets::KMX6Icon_png,CreditsAssets::KMX6Icon_pngSize));brandIcon.setTooltip("K — KUI\nM — Mixer\nX — Advanced mixing\n6 — Six independent audio channels");
        addAndMakeVisible(uiScaleButton);uiScaleButton.onStep=[this](int delta){if(stepUiScale)stepUiScale(delta);};
        addAndMakeVisible(minimiseWindow);addAndMakeVisible(closeWindow);minimiseWindow.setTooltip("Minimize");closeWindow.setTooltip("Close KMX6");minimiseWindow.onClick=[this]{if(auto* peer=getTopLevelComponent()->getPeer())peer->setMinimised(true);};closeWindow.onClick=[]{JUCEApplication::getInstance()->systemRequestedQuit();};
        for(auto* button:{&power,&settings,&refresh,&engineRefresh,&save,&cables}){addAndMakeVisible(*button);button->setTooltip(button->getButtonText());}cables.getProperties().set("actionIcon","virtual");power.getProperties().set("actionIcon","power");settings.getProperties().set("actionIcon","hardware");refresh.getProperties().set("actionIcon","scan");engineRefresh.getProperties().set("actionIcon","engine");save.getProperties().set("actionIcon","save");
        for(int i=0;i<6;++i){strips[i]=std::make_unique<Strip>(engine,i,[this](int a,int b,int c,const kmx6::Endpoint* d){choose(a,b,c,d);});strips[i]->syncControls();addAndMakeVisible(*strips[i]);}
        text(hardwareGroupLabel,"A  /  HARDWARE OUTPUTS",11);hardwareGroupLabel.setFont(FontOptions(11,Font::bold));hardwareGroupLabel.setColour(Label::textColourId,palette::teal);text(monitorLabel,"A1 / MAIN",12);text(a2Label,"A2 / SECOND",12);text(virtualGroupLabel,"B  / VIRTUAL MICROPHONES",11);virtualGroupLabel.setFont(FontOptions(11,Font::bold));virtualGroupLabel.setColour(Label::textColourId,palette::teal);text(sendLabel,"B1 / MIC B1",10);text(b2Label,"B2 / MIC B2",10);text(b3Label,"B3 / MIC B3",10);text(b4Label,"B4 / MIC B4",10);text(masterValue,"-6.0 dB",14);text(a2MasterValue,"0.0 dB",12);text(b1MasterValue,"0.0 dB",10);text(b2MasterValue,"0.0 dB",10);text(b3MasterValue,"0.0 dB",10);text(b4MasterValue,"0.0 dB",10);text(engineInfo,"Audio engine stopped",12);text(driverInfo,"ASIO + WASAPI",11);text(hint,"A = hardware outputs\nB = virtual microphone mixes",11);hint.setColour(Label::textColourId,palette::dim);text(status,"Loading audio devices...",12);status.setColour(Label::textColourId,palette::dim);
        addAndMakeVisible(masterMeter);addAndMakeVisible(sendMeter);sendDevice.setVisible(false);
        addAndMakeVisible(a2Meter);addAndMakeVisible(b2Meter);addAndMakeVisible(b3Meter);addAndMakeVisible(b4Meter);masterMeter.attachGlow(engine.busGlow[0]);a2Meter.attachGlow(engine.busGlow[1]);sendMeter.attachGlow(engine.busGlow[2]);b2Meter.attachGlow(engine.busGlow[3]);b3Meter.attachGlow(engine.busGlow[4]);b4Meter.attachGlow(engine.busGlow[5]);
        auto busFader=[this](Slider& s,std::atomic<float>& value){s.setSliderStyle(Slider::LinearVertical);s.setTextBoxStyle(Slider::NoTextBox,false,0,0);s.setRange(-60,12,0.1);s.setSkewFactorFromMidPoint(-12);s.setValue(value.load());s.setTextValueSuffix(" dB");s.setDoubleClickReturnValue(true,0);s.onValueChange=[&s,&value]{value=float(s.getValue());};addAndMakeVisible(s);};
        busFader(a2Master,engine.a2Db);busFader(b1Master,engine.b1Db);busFader(b2Master,engine.b2Db);busFader(b3Master,engine.b3Db);busFader(b4Master,engine.b4Db);
        master.setSliderStyle(Slider::LinearVertical);master.setTextBoxStyle(Slider::NoTextBox,false,0,0);master.setRange(-60,12,0.1);master.setSkewFactorFromMidPoint(-12);master.setValue(engine.masterDb.load());master.setDoubleClickReturnValue(true,0);master.onValueChange=[this]{engine.masterDb=float(master.getValue());};addAndMakeVisible(master);
        power.onClick=[this]{if(engine.active){saveSettings();engine.stop();}else if(engine.start()){restoreSources();}else showError(engine.error);};
        cables.getProperties().remove("actionIcon");cables.setButtonText("REPAIR I/O");cables.setTooltip("Repair the virtual audio devices. Installs the transport only if missing and saves automatic reconnection. Windows may request administrator approval.");
        cables.onClick=[this]{automaticReconnectBlocked=false;repairFailed=false;beginVirtualSetup(true);};
        settings.onClick=[this]{
            // Device selection is available even when the engine is stopped.
            if(!engine.active){if(!engine.start()) {engine.hardware.initialise(0,0,nullptr,false);}}
            auto panel=std::make_unique<HardwarePanel>(engine,sendDevice,[this]{saveSettings();});panel->setSize(std::min(520,getWidth()-48),394);CallOutBox::launchAsynchronously(std::move(panel),getLocalArea(&settings,settings.getLocalBounds()),this);
        };
        refresh.setTooltip("Rescan Windows audio endpoints and refresh the device lists; it does not restart audio.");refresh.onClick=[this]{refreshDevices();};engineRefresh.setTooltip("Restart the audio engine, rescan hardware, and reconnect KMX6 sources.");engineRefresh.onClick=[this]{saveSettings();engine.stop();for(int i=0;i<engine.hardware.getAvailableDeviceTypes().size();++i)engine.hardware.getAvailableDeviceTypes()[i]->scanForDevices();if(engine.start())restoreSources();else showError("Audio engine refresh failed: "+engine.error);};save.onClick=[this]{saveSettings();status.setText("Mixer settings saved",dontSendNotification);};
        sendDevice.onChange=[this]{if(refreshing)return;if(!engine.active){showError("Start the audio engine before selecting a SEND destination.");return;}
            String name=sendDevice.getSelectedId()>1?sendDevice.getText():String{};String target=name.contains(" | ")?name.fromFirstOccurrenceOf(" | ",false,false):name;
            for(int i=0;i<6;++i)if(engine.channel[i].source==2)for(auto& ep:endpoints)if(String(ep.id.c_str())==engine.sourceIDs[i]&&ep.playback&&String(ep.name)==target){showError("This device is already a loopback source. Select a separate SEND destination.");return;}

            auto error=engine.openSend(name);if(error.isNotEmpty()){if(storedA2Device.isNotEmpty()&&storedA2Device!=name)engine.openSend(storedA2Device);for(int i=0;i<sendDevice.getNumItems();++i)if(sendDevice.getItemText(i)==storedA2Device||sendDevice.getItemText(i).endsWith(" | "+storedA2Device))sendDevice.setSelectedItemIndex(i,dontSendNotification);showError(error);}else{storedA2Device=name;saveSettings();}
        };
        engine.hardware.addChangeListener(this);setSize(1440,900);
        auto host=appFolder().getChildFile("KMX6CableHost.exe");if(host.existsAsFile())cableHost.start(StringArray{host.getFullPathName()},0);
        refreshDevices();
        lastVirtualAttempt=Time::getMillisecondCounterHiRes();
        if(!virtualEndpointsReady(endpoints))beginVirtualSetup(false);
        if(engine.start())restoreSources();else status.setText("Audio engine could not start: "+engine.error,dontSendNotification);startTimerHz(60);
    }
    ~MainComponent()override {pluginManagerWindow.reset();stopTimer();engine.hardware.removeChangeListener(this);saveSettings();engine.stop();if(cableHost.isRunning())cableHost.kill();setLookAndFeel(nullptr);}
    void timerCallback()override {
        if(waitingForVirtualSetup && !virtualSetup.isRunning()){
            waitingForVirtualSetup=false;cables.setEnabled(true);const int result=virtualSetup.getExitCode();
            if(result!=0){repairFailed=true;if(!manualVirtualSetup)automaticReconnectBlocked=true;}
            restoreSources();
            if(result==0&&virtualEndpointsReady(endpoints)){repairFailed=false;automaticReconnectBlocked=false;}
            cables.setTooltip(repairFailed?"Devices need attention. Click Repair I/O; Windows may request administrator approval. Details: virtual-setup.txt / virtual-reconnect.txt beside KMX6.exe.":"Repair virtual devices if needed. Normal startup reconnects without administrator prompts.");
        }
        if(ticks%180==0)maintainVirtualDevices();
        if(ticks%2==0){auto ringLevel=[](Slider& knob,float peak){float old=float(knob.getProperties()["busMeter"]);if(!knob.getProperties().contains("busMeter")||std::abs(old-peak)>0.002f||(old>0.000001f&&peak<=0.000001f)){knob.getProperties().set("busMeter",peak);knob.repaint();}};ringLevel(master,engine.masterPeak.load());ringLevel(a2Master,engine.a2Peak.load());ringLevel(b1Master,engine.busPeak.load());ringLevel(b2Master,engine.busPeak2.load());ringLevel(b3Master,engine.busPeak3.load());ringLevel(b4Master,engine.busPeak4.load());headerVisualizer.update(engine);for(auto& s:strips)s->tick();masterMeter.update(engine.masterPeak.load(),engine.masterPeak.load(),engine.masterClip.load());sendMeter.update(engine.busPeak.load(),engine.busPeak.load(),engine.busPeak.load()>=1);a2Meter.update(engine.a2Peak.load(),engine.a2Peak.load(),engine.a2Peak.load()>=1);b2Meter.update(engine.busPeak2.load(),engine.busPeak2.load(),engine.busPeak2.load()>=1);b3Meter.update(engine.busPeak3.load(),engine.busPeak3.load(),engine.busPeak3.load()>=1);b4Meter.update(engine.busPeak4.load(),engine.busPeak4.load(),engine.busPeak4.load()>=1);masterValue.setText(String(engine.masterDb.load(),1)+" dB",dontSendNotification);a2MasterValue.setText(String(engine.a2Db.load(),1)+" dB",dontSendNotification);b1MasterValue.setText(String(engine.b1Db.load(),1)+" dB",dontSendNotification);b2MasterValue.setText(String(engine.b2Db.load(),1)+" dB",dontSendNotification);b3MasterValue.setText(String(engine.b3Db.load(),1)+" dB",dontSendNotification);b4MasterValue.setText(String(engine.b4Db.load(),1)+" dB",dontSendNotification);}
        if(ticks>0&&ticks%180==0&&engine.active){if(engine.bufferSize.load()==0)engine.hardware.restartLastAudioDevice();bool missing=false;for(int i=0;i<6;++i)if((engine.channel[i].source==2||i>=4)&&!engine.capture[i].running)missing=true;if(missing){auto connected=kmx6::enumerateEndpoints();for(int i=0;i<6;++i)if((engine.channel[i].source==2||i>=4)&&!engine.capture[i].running)for(auto& ep:connected){bool match=i>=4?&ep==kmx6::computerEndpoint(connected,i-4):!ep.playback&&String(ep.id.c_str())==engine.sourceIDs[i];if(match){engine.selectSource(i,2,engine.channel[i].inputChannel.load(),&ep);break;}}}if(storedA2Device.isNotEmpty()&&!engine.a2Internal){auto* output=engine.busDevice.getCurrentAudioDevice();if(!output||!output->isOpen())engine.openSend(storedA2Device);}}
        if(ticks%600==0&&engine.active){String record=Time::getCurrentTime().toISO8601(true)+" rate="+String(engine.sampleRate.load())+" buffer="+String(engine.bufferSize.load())+" late="+String(engine.lateCallbacks.load())+" overruns="+String(engine.callbackOverruns.load());for(int i=0;i<6;++i){auto& capture=engine.capture[i];record+=" ch"+String(i+1)+"[underrun="+String(capture.queue.underflows.load())+",discontinuity="+String(capture.discontinuities.load())+",reserve="+String(capture.queue.safetyReserve.load())+"]";}record+=" A2underrun="+String(engine.sendQueue.underflows.load())+" B1="+String(engine.b1.queue.underflows.load())+" B2="+String(engine.b2.queue.underflows.load())+" B3="+String(engine.b3.queue.underflows.load())+" B4="+String(engine.b4.queue.underflows.load())+"\n";appFolder().getChildFile("audio-health.log").appendText(record);}
        if(++ticks%15==0){auto* d=engine.hardware.getCurrentAudioDevice();power.setButtonText(engine.active?"STOP AUDIO":"START AUDIO");power.setTooltip(engine.active?"Stop audio":"Start audio");power.setColour(TextButton::buttonColourId,engine.active?Colour(0xff583966):palette::raised);
            if(d){engineInfo.setText(String(engine.sampleRate.load()/1000,1)+" kHz  /  "+String(engine.bufferSize.load())+" samples\n"+String(engine.bufferSize.load()*1000.0/engine.sampleRate.load(),2)+" ms per buffer",dontSendNotification);driverInfo.setText(d->getTypeName()+"\n"+d->getName(),dontSendNotification);}
            else{engineInfo.setText("Audio engine stopped",dontSendNotification);driverInfo.setText("ASIO + WASAPI",dontSendNotification);}
            uint64_t gaps=engine.sendQueue.underflows.load()+engine.sendQueue.overflows.load()+engine.b1.queue.underflows.load()+engine.b2.queue.underflows.load()+engine.b3.queue.underflows.load()+engine.b4.queue.underflows.load();for(auto& c:engine.capture)gaps+=c.queue.underflows.load()+c.queue.overflows.load()+c.discontinuities.load();
            auto xruns=d?d->getXRunCount():0;
            int linked=int(engine.b1.connected)+int(engine.b2.connected)+int(engine.b3.connected)+int(engine.b4.connected);String virtualState=waitingForVirtualSetup?(manualVirtualSetup?"Repairing virtual devices":"Reconnecting virtual devices"):repairFailed?"Virtual I/O needs repair":!engine.active?"Virtual mics stopped":linked==4?"Virtual mics 4/4 linked":"Virtual mics reconnecting "+String(linked)+"/4";
            status.setText((engine.active?"RUNNING":"STOPPED")+String(" / DSP ")+String(engine.cpu.load()*100,1)+"% / "+virtualState+" / events "+String(gaps),dontSendNotification);status.setTooltip("Virtual mics are B1-B4 links to the background host. Linked does not mean audio is present. Select KMX6 Mic B1-B4 in the receiving app and enable the matching B route on a source channel. Disconnected links retry automatically.");
        }
    }
    void paint(Graphics& g)override {g.fillAll(palette::bg);g.setColour(palette::line);g.drawHorizontalLine(getHeight()-40,18,float(getWidth()-18));}
    void resized()override {
        int w=getWidth(),h=getHeight();brandIcon.setBounds(10,2,92,92);headline.setBounds(124,24,180,40);headerVisualizer.setBounds(0,8,w,80);headerVisualizer.toBack();headerVisualizer.setAlpha(0.42f);tagline.setBounds(0,0,0,0);
        uiScaleButton.setBounds(w-240,10,150,22);minimiseWindow.setBounds(w-78,10,28,22);closeWindow.setBounds(w-44,10,28,22);power.setBounds(w-70,42,52,42);
        cables.setBounds(w-418,h-34,118,26);pluginManager.setBounds(w-294,h-34,130,26);int actionX=w-158;for(auto* button:{&settings,&refresh,&engineRefresh,&save}){button->setBounds(actionX,h-34,30,26);actionX+=36;}
        int stripW=(w-36)/6;for(int i=0;i<6;++i)strips[i]->setBounds(18+i*stripW,100,stripW-6,h-148);
        engineInfo.setBounds(w-800,h-35,190,28);driverInfo.setBounds(w-600,h-35,175,28);engineInfo.setFont(FontOptions(10));driverInfo.setFont(FontOptions(10));virtualGroupLabel.setVisible(false);hardwareGroupLabel.setVisible(false);sendDevice.setVisible(false);
        auto masterCell=[&](int slot,Label& name,Slider& knob,Meter& meter,Label& value,Colour from,Colour to){int cx=290+slot*94;name.setBounds(cx,4,86,16);name.setJustificationType(Justification::centred);name.setFont(FontOptions(11,Font::bold));knob.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);knob.setBounds(cx+14,20,58,54);Colour category=slot<4?Colour(0xffbb85f5):Colour(0xffe4c17c);name.setColour(Label::textColourId,category);knob.getProperties().set("eqAccent",true);knob.getProperties().set("eqGradientStart",category.toString());knob.getProperties().set("eqGradientEnd",category.toString());knob.setColour(Slider::rotarySliderFillColourId,category);meter.setVisible(false);value.setBounds(cx,76,86,17);DeskLook::styleNumber(value);value.setJustificationType(Justification::centred);};monitorLabel.setText("A1",dontSendNotification);a2Label.setText("A2",dontSendNotification);masterCell(4,monitorLabel,master,masterMeter,masterValue,Colour(0xffffc783),Colour(0xffff9ba6));masterCell(5,a2Label,a2Master,a2Meter,a2MasterValue,Colour(0xff83dcce),Colour(0xff99baff));
        sendLabel.setText("B1",dontSendNotification);b2Label.setText("B2",dontSendNotification);b3Label.setText("B3",dontSendNotification);b4Label.setText("B4",dontSendNotification);masterCell(0,sendLabel,b1Master,sendMeter,b1MasterValue,Colour(0xffbb85f5),Colour(0xfff876be));masterCell(1,b2Label,b2Master,b2Meter,b2MasterValue,Colour(0xff94b3ff),Colour(0xffcba0fa));masterCell(2,b3Label,b3Master,b3Meter,b3MasterValue,Colour(0xffffc783),Colour(0xfff6a5b9));masterCell(3,b4Label,b4Master,b4Meter,b4MasterValue,Colour(0xff83dcce),Colour(0xffa5bcff));hint.setVisible(false);
        information.setBounds(18,h-34,28,26);status.setBounds(56,h-34,w-870,25);
    }
};

static int runSelfTests(String& report) {
    int failed=0,total=0;auto test=[&](bool okay,String name){++total;if(!okay)++failed;report+=(okay?"PASS ":"FAIL ")+name+"\n";};
    {std::vector<kmx6::Endpoint> eps{{L"old1","Speakers (KMX6 Computer 1)",true},{L"old2","Speakers (KMX6 Computer 2)",true},{L"native1","CABLE Input (VB-Audio Virtual Cable)",true},{L"native2","Hi-Fi Cable Input (VB-Audio Hi-Fi Cable)",true}};
    test(kmx6::computerEndpoint(eps,0)==&eps[2]&&kmx6::computerEndpoint(eps,1)==&eps[3],"fixed computer channels prefer distinct native cable endpoints");
    eps={{L"old1","Speakers (KMX6 Computer 1)",true},{L"renamed1","KMX6 Computer 1",true},{L"old2","Speakers (KMX6 Computer 2)",true},{L"renamed2","KMX6 Computer 2",true}};
    test(kmx6::computerEndpoint(eps,0)==&eps[1]&&kmx6::computerEndpoint(eps,1)==&eps[3],"renamed cable endpoints take priority over legacy virtual outputs");
    eps={{L"old1","Speakers (KMX6 Computer 1)",true},{L"old2","Speakers (KMX6 Computer 2)",true}};test(kmx6::computerEndpoint(eps,0)==&eps[0]&&kmx6::computerEndpoint(eps,1)==&eps[1],"missing native cables retain existing computer sources");
    eps={{L"wrong","CABLE-A Input (VB-Audio Cable A)",true},{L"capture","CABLE Input (VB-Audio Virtual Cable)",false}};test(!kmx6::computerEndpoint(eps,0)&&!kmx6::computerEndpoint(eps,1)&&!kmx6::computerEndpoint(eps,2),"fixed sources reject unrelated cables and capture endpoints");}
    test(std::abs(kmx6::gain(0)-1)<1e-6,"unity gain");test(std::abs(kmx6::gain(-60)-0.001f)<1e-6,"-60 dB gain");test(std::abs(kmx6::gain(12)-3.9810717f)<1e-5,"+12 dB gain");
    {float a[2]{},b[2]{},c[2]{};float* outputs[]{a,b,c};MixerEngine::outputPair(outputs,3,2,0,0,0.2f,0.6f);test(std::abs(c[0]-0.2f)<1e-6&&std::abs(a[0]-0.6f)<1e-6&&b[0]==0,"hardware output mapping supports non-default channel destinations");MixerEngine::outputPair(outputs,3,1,1,0,0.2f,0.6f);test(std::abs(b[0]-0.4f)<1e-6,"mono output folds stereo down without dropping a side");MixerEngine::outputPair(outputs,3,-1,5,1,0.9f,0.9f);test(a[1]==0&&b[1]==0&&c[1]==0,"missing output channels remain silent without invalid writes");MixerEngine::outputPair(outputs,3,2,0,0,0.1f,0.1f);test(std::abs(c[0]-0.3f)<1e-6&&std::abs(a[0]-0.7f)<1e-6,"shared hardware destinations mix both buses instead of overwriting");}
    auto q=std::make_unique<kmx6::StereoQueue>();q->target=256;std::array<Stereo,128> samples;
    for(int i=0;i<1024;++i)q->push({0.25f,-0.25f});q->pull(samples.data(),128,48000);test(std::abs(samples[64].l-0.25f)<1e-6&&std::abs(samples[64].r+0.25f)<1e-6,"stereo bridge preserves channels");
    test(std::abs(q->bufferedMs.load()-1024.0/48.0)<0.001,"latency telemetry reports actual queue depth in milliseconds");
    q->reset();q->pull(samples.data(),128,48000);test(samples[0].l==0&&samples[127].r==0,"empty bridge produces silence");
    test(q->bufferedMs.load()==0,"empty queue latency is zero");q->reset();test(q->bufferedMs.load()==0,"reset clears latency telemetry");
    q->reset();q->minimumTarget=512;for(int i=0;i<300;++i)q->push({0.5f,0.5f});q->pull(samples.data(),128,48000);test(samples[0].l==0&&q->available()==300,"bridge waits for full callback safety reserve");
    q->reset();for(int i=0;i<70000;++i)q->push({});test(q->overflows>0&&q->available()<65536,"bridge overflow stays bounded");
    q->reset();q->target=128;q->minimumTarget=128;for(int i=0;i<10000;++i)q->push({float(i),float(i)});q->pull(samples.data(),128,48000);test(samples[0].l>9700&&q->available()<260&&q->underflows==0,"stalled bridge recovers to recent audio without draining stale backlog");
    q->reset();for(int i=0;i<512;++i)q->push({float(i),float(i)});q->pull(samples.data(),128,48000);test(samples[0].l==0&&samples[127].l>126&&q->available()>380,"normal bridge backlog remains continuous");
    q->reset();std::array<Stereo,128> batch;batch.fill({0.2f,-0.4f});q->pushBlock(batch.data(),128);q->pushBlock(batch.data(),128);q->pull(samples.data(),128,48000);test(std::abs(samples[64].l-0.2f)<1e-6&&std::abs(samples[64].r+0.4f)<1e-6,"batched queue writes preserve stereo samples");q->reset();for(int i=0;i<600;++i)q->pushBlock(batch.data(),128);test(q->available()==65535&&q->overflows==600*128-65535,"batched queue overflow is bounded and accounted for");
    kmx6::Channel c;c.fader=0;c.prepare(48000);std::array<Stereo,256> in,mon,bus;in.fill({0.25f,0.25f});
    for(int j=0;j<40;++j){mon.fill({});bus.fill({});c.process(in.data(),mon.data(),bus.data(),256,false);}
    test(std::abs(mon[255].l-0.25f)<0.0001f&&std::abs(bus[255].l)<1e-7,"monitor and send routing stay independent");
    c.send=true;c.monitor=false;for(int j=0;j<40;++j){mon.fill({});bus.fill({});c.process(in.data(),mon.data(),bus.data(),256,false);}
    test(std::abs(bus[255].l-0.25f)<0.0001f&&std::abs(mon[255].l)<1e-6,"routing changes ramp to destination");
    c.mute=true;for(int j=0;j<40;++j){mon.fill({});bus.fill({});c.process(in.data(),mon.data(),bus.data(),256,false);}test(std::abs(bus[255].l)<1e-7,"mute reaches silence");
    c.mute=false;c.pan=-1;for(int j=0;j<40;++j){mon.fill({});bus.fill({});c.process(in.data(),mon.data(),bus.data(),256,false);}test(std::abs(bus[255].r)<1e-7&&bus[255].l>0.24,"pan routes left");
    c.pan=0;c.hpf=true;for(int j=0;j<100;++j){mon.fill({});bus.fill({});c.process(in.data(),mon.data(),bus.data(),256,false);}test(std::abs(bus[255].l)<0.0001,"high-pass rejects DC");
    c.hpf=false;c.low=15;c.mid=-15;c.high=15;for(int j=0;j<40;++j){for(int i=0;i<256;++i)in[i]={float(std::sin((i+j*256)*0.13)),float(std::sin((i+j*256)*0.17))};mon.fill({});bus.fill({});c.process(in.data(),mon.data(),bus.data(),256,false);}test(std::isfinite(bus[255].l)&&std::isfinite(bus[255].r),"EQ remains finite at extreme settings");
    c.low=0;c.mid=0;c.high=0;c.pan=0;c.hpf=false;c.mute=false;c.monitor=false;c.send=false;c.monitor2=true;c.send2=false;c.prepare(48000);in.fill({0.25f,0.25f});std::array<Stereo,256> a2,b2;
    for(int j=0;j<40;++j){mon.fill({});bus.fill({});a2.fill({});b2.fill({});c.process(in.data(),mon.data(),bus.data(),256,false,a2.data(),b2.data());}
    test(a2[255].l>0.249f&&std::abs(mon[255].l)<1e-6&&std::abs(bus[255].l)<1e-6&&std::abs(b2[255].l)<1e-6,"A2 routes independently of A1 and B buses");
    c.monitor2=false;c.send2=true;for(int j=0;j<40;++j){mon.fill({});bus.fill({});a2.fill({});b2.fill({});c.process(in.data(),mon.data(),bus.data(),256,false,a2.data(),b2.data());}
    test(b2[255].l>0.249f&&std::abs(a2[255].l)<1e-6&&std::abs(bus[255].l)<1e-6,"B2 routes independently of B1 and hardware buses");
    c.send=true;for(int j=0;j<40;++j){mon.fill({});bus.fill({});a2.fill({});b2.fill({});c.process(in.data(),mon.data(),bus.data(),256,false,a2.data(),b2.data());}
    test(bus[255].l>0.249f&&b2[255].l>0.249f,"one channel can feed both virtual mixes");
    {kmx6::Channel limited;limited.limiterEnabled=true;limited.fader=12;limited.prepare(48000);in.fill({2.0f,1.0f});float maxPeak=0;for(int block=0;block<40;++block){mon.fill({});bus.fill({});limited.process(in.data(),mon.data(),bus.data(),256,false);for(auto sample:mon)maxPeak=std::max(maxPeak,std::max(std::abs(sample.l),std::abs(sample.r)));}test(maxPeak<=kmx6::gain(-1.0f)+0.00001f,"mic limiter enforces peak ceiling");test(std::abs(mon[255].l-2*mon[255].r)<0.00001f,"mic limiter preserves stereo balance");test(limited.limiterReduction.load()>10,"mic limiter reports gain reduction");in.fill({0.01f,0.01f});for(int block=0;block<200;++block){mon.fill({});bus.fill({});limited.process(in.data(),mon.data(),bus.data(),256,false);}test(limited.limiterReduction.load()<0.01f,"mic limiter releases after overload");}
    {kmx6::Channel gated;gated.fader=0;gated.gateEnabled=true;gated.gateThreshold=-40;gated.prepare(48000);in.fill({0.1f,0.1f});for(int block=0;block<40;++block){mon.fill({});bus.fill({});gated.process(in.data(),mon.data(),bus.data(),256,false);}test(std::abs(mon[255].l-0.1f)<0.0001f,"gate opens for signal above threshold");in.fill({0.0001f,0.0001f});for(int block=0;block<300;++block){mon.fill({});bus.fill({});gated.process(in.data(),mon.data(),bus.data(),256,false);}test(std::abs(mon[255].l)<0.000001f,"gate closes after hold and release");gated.gateEnabled=false;for(int block=0;block<40;++block){mon.fill({});bus.fill({});gated.process(in.data(),mon.data(),bus.data(),256,false);}test(std::abs(mon[255].l-0.0001f)<0.000001f,"disabled gate passes quiet signal");gated.limiterEnabled=true;gated.limiterThreshold=-12;in.fill({2,1});for(int block=0;block<40;++block){mon.fill({});bus.fill({});gated.process(in.data(),mon.data(),bus.data(),256,false);}test(std::abs(mon[255].l-kmx6::gain(-12))<0.00001f,"limiter respects adjustable threshold");}
    {q->reset();q->target=128;q->minimumTarget=128;for(int i=0;i<400;++i)q->push({0.5f,0.5f});for(int i=0;i<5;++i)q->pull(samples.data(),128,48000);test(q->underflows==1&&q->safetyReserve>=96,"scheduler shortage adds bounded safety reserve");for(int i=0;i<1024;++i)q->push({0.5f,0.5f});q->pull(samples.data(),128,48000);test(std::abs(samples[0].l)<0.00001f&&samples[100].l>0.49f,"audio resumes with a short fade after starvation");for(int stall=0;stall<100;++stall){for(int i=0;i<20;++i)q->pull(samples.data(),128,48000);for(int i=0;i<2048;++i)q->push({0.25f,0.25f});q->pull(samples.data(),128,48000);}test(q->safetyReserve<=960,"repeated scheduler stalls cannot grow safety delay beyond 20 ms");q->reset();q->target=8;q->minimumTarget=8;for(int i=0;i<64;++i)q->push({1,1});q->pull(samples.data(),128,48000);test(q->underflows==0&&samples[0].l==0&&q->available()==64,"queue waits for a whole callback before starting");}
    {kmx6::GlowDetector detector;detector.prepare(48000);for(int i=0;i<4800;++i)detector.add(float(0.25*std::sin(i*2*MathConstants<double>::pi*80/48000)),float(0.25*std::sin(i*2*MathConstants<double>::pi*8000/48000)));detector.finish();test(detector.bands[0][0].load()>detector.bands[0][2].load()*5&&detector.bands[1][2].load()>detector.bands[1][0].load()*5,"glow responds separately to left bass and right treble");float loud=detector.bands[0][0];detector.prepare(48000);for(int i=0;i<4800;++i)detector.add(float(0.125*std::sin(i*2*MathConstants<double>::pi*80/48000)),0);detector.finish();test(std::abs(detector.bands[0][0].load()*2-loud)<0.00001f,"glow detector tracks signal loudness");detector.prepare(48000);for(int i=0;i<256;++i)detector.add(0,0);detector.finish();test(detector.bands[0][0]==0&&detector.bands[1][2]==0,"glow detector has no energy for silence");}
    {q->reset();q->target=512;q->minimumTarget=512;q->rate=48000;q->safetyReserve=480;for(int i=0;i<1536;++i)q->push({0.2f,0.2f});for(int block=0;block<16000;++block){for(int i=0;i<128;++i)q->push({0.2f,0.2f});q->pull(samples.data(),128,48000);}test(q->safetyReserve==0&&q->underflows==0,"stable playback automatically releases extra latency without underruns");}
    {bool bounded=true;for(double sourceRate:{44100.0,48000.0,96000.0})for(double drift:{-0.001,0.001}){q->reset();q->rate=sourceRate;q->target=1024;q->minimumTarget=1024;for(int i=0;i<2048;++i)q->push({0.1f,-0.1f});double pending=0;for(int block=0;block<24000;++block){pending+=128*sourceRate/48000*(1+drift);int frames=int(pending);pending-=frames;for(int i=0;i<frames;++i)q->push({0.1f,-0.1f});q->pull(samples.data(),128,48000);bounded &= std::isfinite(samples[127].l)&&std::abs(samples[127].l)<=0.11f;}if(q->underflows||q->overflows)report+="CLOCK DETAIL rate="+String(sourceRate)+" drift="+String(drift,4)+" underruns="+String(q->underflows.load())+" overruns="+String(q->overflows.load())+" queued="+String(q->available())+"\n";bounded &= q->underflows==0&&q->overflows==0;}test(bounded,"six simulated clock and sample-rate combinations remain bounded for 64 seconds each");}
    {q->reset();q->rate=48000;q->target=128;q->minimumTarget=128;for(int i=0;i<512;++i)q->push({0.4f,0.4f});q->pull(samples.data(),128,48000);for(int i=0;i<10000;++i)q->push({-0.4f,-0.4f});q->pull(samples.data(),128,48000);test(std::abs(samples[0].l-0.4f)<0.0001f&&samples[100].l< -0.39f,"stale-backlog recovery crossfades instead of stepping between samples");}
    {auto burst=std::make_unique<kmx6::StereoQueue>();burst->rate=48000;burst->target=1024;burst->minimumTarget=194;std::array<Stereo,96> rendered;for(int tick=0;tick<48000;tick+=32){if(tick%512==0)for(int frame=0;frame<512;++frame)burst->push({0.2f,-0.2f});if(tick%96==0)burst->pull(rendered.data(),96,48000);}test(burst->underflows==0&&burst->overflows==0,"A2 small callbacks stay continuous when primary audio arrives in larger bursts");}
    test(kmx6::StereoQueue::captureReserve(480,48000)==578&&kmx6::StereoQueue::captureReserve(441,44100)==532,"capture reserve follows one packet plus 2 ms instead of a fixed 30 ms");
    {auto low=std::make_unique<kmx6::StereoQueue>();low->rate=48000;low->target=kmx6::StereoQueue::captureReserve(480,48000);low->minimumTarget=258;std::array<Stereo,256> rendered;for(int tick=0;tick<48000*10;tick+=32){if(tick%480==0)for(int frame=0;frame<480;++frame)low->push({0.2f,-0.2f});if(tick%256==0)low->pull(rendered.data(),256,48000);}test(low->underflows==0&&low->overflows==0,"reduced capture queue handles 10 ms packets and uneven 256-frame consumer timing for ten seconds");test(low->bufferedMs<30,"reduced capture queue stays below the old 30 ms target");low->discardOld(16);test(low->backlogDrops==1&&low->discardedFrames>0,"explicit backlog trims publish dropout event and frame counts");low->reset();test(low->backlogDrops==0&&low->overflowEvents==0&&low->discardedFrames==0,"source restart resets dropout counters");}
    report+="\n"+String(total-failed)+" / "+String(total)+" passed\n";return failed?1:0;
}

#include "MixerBenchmark.h"

class MixerApp : public JUCEApplication {
    class Window : public DocumentWindow,private Timer {
        DeskLook look;
        MainComponent* mixer=nullptr;
        uiScale::Canvas* canvas=nullptr;
        int percent=100;
        uiScale::Transition zoom;
        double lastZoomFrame=0;
        void presentScale(double value){
            canvas->setContinuousPercent(value);
            // Resize the window with the same transform, never relayout controls.
            setContentComponentSize(canvas->getWidth(),canvas->getHeight());
            canvas->repaint();repaint();if(auto* peer=getPeer()){peer->performAnyPendingRepaintsNow();RedrawWindow(static_cast<HWND>(peer->getNativeHandle()),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);}
        }
        void applyScale(int requested,bool animate=true){
            percent=uiScale::normalise(requested);mixer->setUiScaleLabel(percent);
            if(animate&&isShowing()){zoom.request(percent);if(zoom.running){mixer->setScaleAnimationActive(true);canvas->beginAnimation();lastZoomFrame=Time::getMillisecondCounterHiRes();startTimerHz(60);}else{stopTimer();canvas->endAnimation();mixer->setScaleAnimationActive(false);}}
            else{stopTimer();canvas->endAnimation();mixer->setScaleAnimationActive(false);zoom.reset(percent);presentScale(percent);}
        }
        void timerCallback()override{
            double now=Time::getMillisecondCounterHiRes();zoom.advance((now-lastZoomFrame)/1000.0);lastZoomFrame=now;if(!zoom.running){stopTimer();canvas->endAnimation();mixer->setScaleAnimationActive(false);}presentScale(zoom.current);
        }
    public:
        Window():DocumentWindow("KMX6",palette::bg,0){
            setLookAndFeel(&look);setOpaque(true);setUsingNativeTitleBar(false);setTitleBarHeight(0);setResizable(false,false);setIcon(ImageCache::getFromMemory(CreditsAssets::KMX6Icon_png,CreditsAssets::KMX6Icon_pngSize));
            auto main=std::make_unique<MainComponent>();mixer=main.get();mixer->stepUiScale=[this](int delta){applyScale(percent+delta);uiScale::save(percent);};canvas=new uiScale::Canvas(std::move(main));
            setContentOwned(canvas,false);
            // Use JUCE's synchronous software renderer for this resizable mixer window.
            // Direct2D swap-chain resizing can expose an unpainted strip before Present.
            if(auto* peer=getPeer())peer->setCurrentRenderingEngine(0);
            applyScale(uiScale::load(),false);centreWithSize(getWidth(),getHeight());setVisible(true);if(auto* peer=getPeer()){DWORD preference=1;DwmSetWindowAttribute(static_cast<HWND>(peer->getNativeHandle()),33,&preference,sizeof(preference));}
        }
        ~Window()override{stopTimer();uiScale::save(percent);clearContentComponent();setLookAndFeel(nullptr);}
        void closeButtonPressed()override{JUCEApplication::getInstance()->systemRequestedQuit();}
        bool keyPressed(const KeyPress& key)override{
            if(!key.getModifiers().isCommandDown())return false;
            if(key.getKeyCode()=='0'){applyScale(100);uiScale::save(percent);return true;}
            if(key.getTextCharacter()=='+'||key.getKeyCode()=='='){applyScale(percent+5);uiScale::save(percent);return true;}
            if(key.getKeyCode()=='-'){applyScale(percent-5);uiScale::save(percent);return true;}return false;
        }
    };

    std::unique_ptr<Window> window;
    std::unique_ptr<PluginSmokeTest> pluginSmokeTest;
    std::unique_ptr<VirtualMicMonitor> virtualMicMonitor;
public:
    const String getApplicationName()override{return "KMX6";}
    const String getApplicationVersion()override{return "1.0.0";}
    bool moreThanOneInstanceAllowed()override{return getCommandLineParameters().contains("--");}
    void initialise(const String& args)override {
        if(args.startsWith("--ui-scale-check")){String report;int result=uiScale::runChecks(report);appFolder().getChildFile("ui-scale-check.txt").replaceWithText(report);setApplicationReturnValue(result);quit();return;}
        if(args.startsWith("--virtual-output-check")){String report;int result=runVirtualOutputChecks(report);appFolder().getChildFile("virtual-output-check.txt").replaceWithText(report);setApplicationReturnValue(result);quit();return;}
        if(args.startsWith("--virtual-mic-monitor")){virtualMicMonitor=std::make_unique<VirtualMicMonitor>();return;}
        if(args.startsWith("--plugin-feature-check")){String report;int result=runPluginFeatureChecks(report);appFolder().getChildFile("plugin-feature-check.txt").replaceWithText(report);setApplicationReturnValue(result);quit();return;}
        if(args.startsWith("--vst-host-check")){auto tokens=StringArray::fromTokens(args,true);tokens.removeEmptyStrings();if(tokens.size()==2)pluginSmokeTest=std::make_unique<PluginSmokeTest>(tokens[1].unquoted());else quit();return;}
        if(args.startsWith("--scan-vst3")){auto tokens=StringArray::fromTokens(args,true);tokens.removeEmptyStrings();if(tokens.size()==3){VST3PluginFormat format;OwnedArray<PluginDescription> found;format.findAllTypesForFile(found,tokens[1].unquoted());XmlElement result("Plugins");for(auto* d:found)result.addChildElement(d->createXml().release());result.writeTo(File(tokens[2].unquoted()));}quit();return;}
        if(args=="--benchmark"||args=="--benchmark-heavy"){setApplicationReturnValue(runMixerBenchmark(args=="--benchmark-heavy"));quit();return;}
        if(args.contains("--self-test")){String report;int result=runSelfTests(report);appFolder().getChildFile("self-test.txt").replaceWithText(report);setApplicationReturnValue(result);quit();return;}
        if(args.contains("--diagnostics")){
            String report="KMX6 device inventory\nNo audio streams opened. Native cables are fixed computer sources; excluded from physical hardware selectors.\n\n";
            for(auto& ep:kmx6::enumerateEndpoints())report+=(ep.playback?"PLAYBACK ":"CAPTURE ")+String(ep.name)+" / "+String(ep.sampleRate)+" Hz / "+String(ep.bitDepth)+" bit"+(forbidden(ep.name)?" [excluded]":"")+"\n";
            AudioDeviceManager manager;for(auto* type:manager.getAvailableDeviceTypes()){type->scanForDevices();report+="\nBACKEND "+type->getTypeName()+"\n";for(auto& name:type->getDeviceNames(false))report+=" OUT "+name+"\n";for(auto& name:type->getDeviceNames(true))report+=" IN  "+name+"\n";}
            appFolder().getChildFile("devices.txt").replaceWithText(report);quit();return;
        }
        window=std::make_unique<Window>();
    }
    void shutdown()override{virtualMicMonitor.reset();pluginSmokeTest.reset();window.reset();}
    void systemRequestedQuit()override{quit();}
};
START_JUCE_APPLICATION(MixerApp)
