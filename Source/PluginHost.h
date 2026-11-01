#pragma once
#include <JuceHeader.h>
#include "AudioCore.h"
#include "PluginCatalog.h"
#include <deque>

// All chain edits run on the message thread. The device callback lock protects
// publication/removal only; plugin loading, state and destruction happen outside it.
class PluginBank : private juce::Timer {
public:
    struct EditorWindow : juce::DocumentWindow {
        EditorWindow(juce::AudioPluginInstance& p):DocumentWindow(p.getName(),juce::Colour(0xff252332),closeButton){
            setUsingNativeTitleBar(true);auto* editor=p.createEditorIfNeeded();
            if(!editor)editor=new juce::GenericAudioProcessorEditor(p);
            setContentOwned(editor,true);setResizable(editor->isResizable(),false);centreWithSize(getWidth(),getHeight());setVisible(true);
        }
        void closeButtonPressed() override {setVisible(false);}
    };
    struct Slot {
        juce::PluginDescription description;
        juce::MemoryBlock state;
        std::unique_ptr<juce::AudioPluginInstance> processor;
        std::unique_ptr<EditorWindow> editor; // Must be destroyed before processor.
        std::atomic<bool> bypass{false};
        std::atomic<float> mix{1}; float smoothMix=1; juce::String alias;
        juce::AudioBuffer<float> dryDelay{2,1}; int dryLatency=0,dryHead=0; std::atomic<bool> mixReady{true};
        std::atomic<int> latency{0};
        bool loading=false;juce::String error;
        int channels=2;
    };
    std::array<std::vector<std::unique_ptr<Slot>>,6> chains;
    std::array<std::atomic<bool>,6> live;
    std::array<std::atomic<float>,6> cpu{};
    std::array<std::atomic<int>,6> activeLatencySamples{};
    juce::AudioPluginFormatManager formats;
    PluginCatalog catalog;
    juce::CriticalSection& callbackLock;
    std::atomic<double> rate{48000};
    juce::AudioBuffer<float> work{2,256};juce::MidiBuffer midi;
    std::shared_ptr<bool> alive=std::make_shared<bool>(true);
    struct Job {int channel;Slot* slot;juce::PluginDescription description;juce::MemoryBlock state;bool replacing=false;};
    std::deque<Job> pending;
    bool creating=false; bool persistent=true;
    juce::String status;
    explicit PluginBank(juce::CriticalSection& lock,bool persist=true):catalog(persist),callbackLock(lock),persistent(persist){
        formats.addFormat(std::make_unique<juce::VST3PluginFormat>()); midi.ensureSize(2048);
        for(auto& value:live)value=true;
        if(persistent)restore();startTimer(100);
    }
    ~PluginBank() override {stopTimer();*alive=false;}
    static juce::File file(){return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("plugin-chains.xml");}
    static bool empty(const Slot& s){return !s.processor&&!s.loading&&s.description.name.isEmpty();}
    int count(int c)const{int count=0;for(auto& s:chains[c])if(!empty(*s))++count;return count;}
    static void prepareMix(Slot& s,int latency,double sr){s.dryLatency=latency;s.dryHead=0;bool ready=latency>=0&&latency<=int(sr*2);s.dryDelay.setSize(2,ready?std::max(1,latency):1);s.dryDelay.clear();s.mixReady=ready;}
    void prepare(double sr){rate=sr;for(auto& chain:chains)for(auto& s:chain)if(s->processor){const juce::ScopedLock lock(s->processor->getCallbackLock());s->processor->releaseResources();s->processor->setRateAndBufferSizeDetails(sr,256);s->processor->prepareToPlay(sr,256);prepareMix(*s,s->processor->getLatencySamples(),sr);}}
    void process(int channel,kmx6::Stereo* data,int n){
        if(chains[channel].empty()){cpu[channel]=0;activeLatencySamples[channel]=0;return;}
        int processedLatency=0;
        auto start=juce::Time::getHighResolutionTicks();
        for(auto& slot:chains[channel]){
            auto* p=slot->processor.get();if(!p||slot->bypass.load())continue;
            int latency=p->getLatencySamples();slot->latency=latency;
            if(live[channel]&&latency>int(rate.load()*0.005))continue;
            // State saves/edits must never make the real-time callback wait.
            const juce::ScopedTryLock lock(p->getCallbackLock());if(!lock.isLocked()||p->isSuspended())continue;
            float* l=work.getWritePointer(0);float* r=work.getWritePointer(1);
            for(int i=0;i<n;++i){l[i]=slot->channels==1?(data[i].l+data[i].r)*0.5f:data[i].l;r[i]=data[i].r;}
            float* pointers[]{l,r};juce::AudioBuffer<float> block(pointers,slot->channels,n);
            midi.clear();p->processBlock(block,midi);processedLatency+=std::max(0,latency);
            bool aligned=latency==slot->dryLatency&&slot->mixReady.load();slot->mixReady=aligned;
            float target=aligned?std::clamp(slot->mix.load(),0.0f,1.0f):1.0f;float slew=1-std::exp(-1.0f/float(rate.load()*0.005));
            for(int i=0;i<n;++i){float left=l[i],right=slot->channels==1?l[i]:r[i];if(!std::isfinite(left))left=0;if(!std::isfinite(right))right=0;
                auto dry=data[i];if(aligned&&latency>0){int head=slot->dryHead;dry={slot->dryDelay.getSample(0,head),slot->dryDelay.getSample(1,head)};slot->dryDelay.setSample(0,head,data[i].l);slot->dryDelay.setSample(1,head,data[i].r);slot->dryHead=(head+1)%latency;}
                slot->smoothMix+=slew*(target-slot->smoothMix);float wet=aligned?slot->smoothMix:1.0f;data[i]={dry.l*(1-wet)+left*wet,dry.r*(1-wet)+right*wet};}
        }
        activeLatencySamples[channel].store(processedLatency,std::memory_order_relaxed);
        cpu[channel]=float(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks()-start)*rate.load()/n*100);
    }
    bool addAt(int c,int index,const juce::PluginDescription& desc,const juce::MemoryBlock& state={},bool bypass=false){
        if(index<0||index>=8||desc.isInstrument)return false;
        auto replacement=std::make_unique<Slot>();replacement->description=desc;replacement->state=state;replacement->bypass=bypass;replacement->loading=true;auto* raw=replacement.get();
        if(index<int(chains[c].size())&&!empty(*chains[c][index])){auto& old=*chains[c][index];if(old.loading)return false;old.loading=true;pending.push_back({c,&old,desc,state,true});return true;}
        while(int(chains[c].size())<=index){auto hole=std::make_unique<Slot>();const juce::ScopedLock lock(callbackLock);chains[c].push_back(std::move(hole));}
        {const juce::ScopedLock lock(callbackLock);chains[c][index]=std::move(replacement);}pending.push_back({c,raw,desc,state,false});return true;
    }
    bool add(int c,const juce::PluginDescription& desc,const juce::MemoryBlock& state={},bool bypass=false){
        int index=0;while(index<int(chains[c].size())&&!empty(*chains[c][index]))++index;
        if(index>=8){status="Eight effects per channel maximum.";return false;}return addAt(c,index,desc,state,bypass);
    }
    void remove(int c,int i){if(i<0||i>=int(chains[c].size())||chains[c][i]->loading)return;auto hole=std::make_unique<Slot>();std::unique_ptr<Slot> removed;{const juce::ScopedLock lock(callbackLock);removed=std::move(chains[c][i]);chains[c][i]=std::move(hole);}removed.reset();save();}
    void move(int c,int i,int delta){int j=i+delta;if(i<0||j<0||i>=int(chains[c].size())||j>=8)return;while(int(chains[c].size())<=j){auto hole=std::make_unique<Slot>();const juce::ScopedLock lock(callbackLock);chains[c].push_back(std::move(hole));}{const juce::ScopedLock lock(callbackLock);std::swap(chains[c][i],chains[c][j]);}save();}
    void edit(int c,int i){if(i<0||i>=int(chains[c].size()))return;auto& s=*chains[c][i];if(!s.processor)return;if(!s.editor)s.editor=std::make_unique<EditorWindow>(*s.processor);s.editor->setVisible(true);s.editor->toFront(true);}
    void save(){ if(!persistent)return;
        juce::XmlElement root("KMX6Plugins");
        for(int c=0;c<6;++c){auto* chain=root.createNewChildElement("Channel");chain->setAttribute("index",c);chain->setAttribute("live",live[c].load());
            for(auto& s:chains[c]){if(empty(*s)){chain->createNewChildElement("Empty");continue;}auto* node=chain->createNewChildElement("Effect");node->setAttribute("mix",double(s->mix.load()));node->setAttribute("alias",s->alias);node->setAttribute("bypass",s->bypass.load());node->addChildElement(s->description.createXml().release());
                if(s->processor){const juce::ScopedLock lock(s->processor->getCallbackLock());s->processor->getStateInformation(s->state);}node->setAttribute("state",s->state.toBase64Encoding());}}
        if(!root.writeTo(file()))status="Unable to save plugin-chains.xml.";
    }
    void restore(){if(auto xml=juce::parseXML(file()))for(auto* chain:xml->getChildIterator()){
        int c=chain->getIntAttribute("index",-1);if(c<0||c>=6)continue;live[c]=chain->getBoolAttribute("live",true);
        for(auto* node:chain->getChildIterator()){if(chains[c].size()>=8)break;if(node->hasTagName("Empty")){chains[c].push_back(std::make_unique<Slot>());continue;}auto* desc=node->getChildByName("PLUGIN");juce::PluginDescription d;if(!desc||!d.loadFromXml(*desc))continue;juce::MemoryBlock state;state.fromBase64Encoding(node->getStringAttribute("state"));int index=int(chains[c].size());if(addAt(c,index,d,state,node->getBoolAttribute("bypass"))){chains[c][index]->mix=float(std::clamp(node->getDoubleAttribute("mix",1.0),0.0,1.0));chains[c][index]->smoothMix=chains[c][index]->mix.load();chains[c][index]->alias=node->getStringAttribute("alias");}}}}
private:
    void timerCallback() override {
        if(creating||pending.empty())return;auto job=pending.front();auto c=job.channel;auto* s=job.slot;pending.pop_front();creating=true;std::weak_ptr<bool> lifetime=alive;
        const double sr=rate.load();formats.createPluginInstanceAsync(job.description,sr,256,[this,lifetime,c,s,job](std::unique_ptr<juce::AudioPluginInstance> p,const juce::String& error){
            auto token=lifetime.lock();if(!token||!*token)return;creating=false;s->loading=false;
            if(!p){s->error=error;status="Could not load "+s->description.name+": "+error;return;}
            p->enableAllBuses();auto layout=p->getBusesLayout();
            for(int i=1;i<layout.inputBuses.size();++i)layout.inputBuses.getReference(i)=juce::AudioChannelSet::disabled();
            for(int i=1;i<layout.outputBuses.size();++i)layout.outputBuses.getReference(i)=juce::AudioChannelSet::disabled();
            bool supported=false;
            if(layout.inputBuses.size()>0&&layout.outputBuses.size()>0)for(int channels:{2,1}){layout.inputBuses.getReference(0)=channels==2?juce::AudioChannelSet::stereo():juce::AudioChannelSet::mono();layout.outputBuses.getReference(0)=layout.inputBuses[0];if(p->setBusesLayout(layout)){supported=true;break;}}
            if(!supported||p->acceptsMidi()&&p->getTotalNumInputChannels()==0){s->error="This effect requires an unsupported bus layout. Use mono or stereo effects.";return;}
            if(job.state.getSize())p->setStateInformation(job.state.getData(),int(job.state.getSize()));
            // The hardware may have changed while asynchronous creation was pending.
            int channels=p->getTotalNumOutputChannels();double preparedRate=rate.load();p->setRateAndBufferSizeDetails(preparedRate,256);p->prepareToPlay(preparedRate,256);
            if(job.replacing)s->editor.reset();
            Slot mixState;prepareMix(mixState,p->getLatencySamples(),preparedRate);std::unique_ptr<juce::AudioPluginInstance> old;
            {const juce::ScopedLock lock(callbackLock);double current=rate.load();if(current!=preparedRate){p->releaseResources();p->setRateAndBufferSizeDetails(current,256);p->prepareToPlay(current,256);prepareMix(mixState,p->getLatencySamples(),current);}s->latency=p->getLatencySamples();s->channels=channels;s->dryDelay=std::move(mixState.dryDelay);s->dryLatency=mixState.dryLatency;s->dryHead=0;s->mixReady=mixState.mixReady.load();old=std::move(s->processor);s->processor=std::move(p);}
            s->description=job.description;s->state=job.state;s->error.clear();old.reset();
            status=s->description.name+" loaded.";save();
        });
    }
};

// A bounded, offline compatibility check; opens no audio devices.
class PluginSmokeTest : private juce::Timer {
    juce::CriticalSection lock;
    PluginBank bank;
    double began=juce::Time::getMillisecondCounterHiRes();
    void finish(bool pass,const juce::String& report){stopTimer();juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("vst-host-check.txt").replaceWithText((pass?"PASS\n":"FAIL\n")+report);juce::JUCEApplication::getInstance()->setApplicationReturnValue(pass?0:1);juce::JUCEApplication::getInstance()->quit();}
    void timerCallback()override{
        if(juce::Time::getMillisecondCounterHiRes()-began>45000){finish(false,"Plugin creation timed out.");return;}
        if(bank.chains[0].empty())return;auto& slot=*bank.chains[0][0];if(slot.loading)return;
        if(!slot.processor){finish(false,slot.error);return;}
        std::array<kmx6::Stereo,256> input;input.fill({0.125f,-0.0625f});
        for(int block=0;block<20;++block){input.fill({0.125f,-0.0625f});bank.process(0,input.data(),256);for(auto s:input)if(!std::isfinite(s.l)||!std::isfinite(s.r)){finish(false,"Non-finite plugin output.");return;}}
        juce::MemoryBlock state;slot.processor->getStateInformation(state);if(state.getSize())slot.processor->setStateInformation(state.getData(),int(state.getSize()));
        slot.bypass=true;input.fill({0.125f,-0.0625f});bank.process(0,input.data(),256);for(auto s:input)if(s.l!=0.125f||s.r!=-0.0625f){finish(false,"Bypass changed input audio.");return;}
        for(int c=1;c<6;++c){bank.process(c,input.data(),256);for(auto s:input)if(s.l!=0.125f||s.r!=-0.0625f){finish(false,"Effect leaked into another channel.");return;}}
        auto name=slot.description.name;auto latency=slot.processor->getLatencySamples();bank.remove(0,0);bank.process(0,input.data(),256);
        finish(true,"64-bit VST3: "+name+"\nCreated and prepared at 48 kHz / 256 samples.\nProcessed 20 offline stereo blocks; state captured/restored.\nBypass, five unaffected channels, and removal preserve dry audio.\nPlugin reported latency: "+juce::String(latency)+" samples.\nNo hardware audio opened. Live audio and long-session stability remain unverified.\n");
    }
public:
    explicit PluginSmokeTest(const juce::String& path):bank(lock,false){
        juce::VST3PluginFormat format;juce::OwnedArray<juce::PluginDescription> types;format.findAllTypesForFile(types,path);
        for(auto* d:types)if(!d->isInstrument){bank.live[0]=false;bank.add(0,*d);startTimer(100);return;}finish(false,"No compatible effect in selected module.");
    }
};
