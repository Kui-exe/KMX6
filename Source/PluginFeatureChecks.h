#pragma once
#include "PluginHost.h"

class HostArithmeticEffect final : public juce::AudioPluginInstance {
    float multiplier,offset;
public:
    HostArithmeticEffect(float m,float b,int latency=0):AudioPluginInstance(BusesProperties().withInput("In",juce::AudioChannelSet::stereo(),true).withOutput("Out",juce::AudioChannelSet::stereo(),true)),multiplier(m),offset(b){setLatencySamples(latency);}
    const juce::String getName()const override{return "Offline chain check";}
    void fillInPluginDescription(juce::PluginDescription& d)const override{d.name=getName();d.pluginFormatName="Test";}
    void prepareToPlay(double,int)override{} void releaseResources()override{}
    void processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&)override{for(int c=0;c<b.getNumChannels();++c)for(int i=0;i<b.getNumSamples();++i)b.setSample(c,i,b.getSample(c,i)*multiplier+offset);}
    double getTailLengthSeconds()const override{return 0;}bool acceptsMidi()const override{return false;}bool producesMidi()const override{return false;}
    juce::AudioProcessorEditor* createEditor()override{return nullptr;}bool hasEditor()const override{return false;}
    int getNumPrograms()override{return 1;}int getCurrentProgram()override{return 0;}void setCurrentProgram(int)override{}const juce::String getProgramName(int)override{return {}; }void changeProgramName(int,const juce::String&)override{}
    void getStateInformation(juce::MemoryBlock&)override{}void setStateInformation(const void*,int)override{}
};

inline int runPluginFeatureChecks(juce::String& report){
    int passed=0,failed=0;auto expect=[&](bool condition,const juce::String& name){report+=(condition?"PASS ":"FAIL ")+name+"\n";condition?++passed:++failed;};
    juce::CriticalSection lock;PluginBank bank(lock,false);
    auto insert=[&](int c,float gain,float offset,int latency=0){auto slot=std::make_unique<PluginBank::Slot>();slot->processor=std::make_unique<HostArithmeticEffect>(gain,offset,latency);slot->processor->prepareToPlay(48000,256);bank.chains[c].push_back(std::move(slot));};
    insert(0,2,0);insert(0,1,0.25f);insert(1,0.5f,0);
    auto render=[&](int c,float left,float right){std::array<kmx6::Stereo,256> audio;audio.fill({0.125f,-0.0625f});bank.process(c,audio.data(),256);for(auto sample:audio)if(std::abs(sample.l-left)>0.000001f||std::abs(sample.r-right)>0.000001f)return false;return true;};
    expect(render(0,0.5f,0.125f),"two simultaneous effects process in series");expect(render(1,0.0625f,-0.03125f),"another channel runs an independent effect at the same time");
    bool dry=true;for(int c=2;c<6;++c)dry&=render(c,0.125f,-0.0625f);expect(dry,"other channels retain dry audio");
    bank.move(0,1,-1);expect(render(0,0.75f,0.375f),"reordering changes noncommutative processing order");bank.chains[0][0]->bypass=true;expect(render(0,0.25f,-0.125f),"bypass only affects the selected slot");bank.remove(0,0);expect(render(0,0.25f,-0.125f),"removing one effect leaves the other running");
    insert(0,1,0.5f,288);expect(render(0,0.25f,-0.125f),"Live mode skips a high-latency slot without bypassing other effects");bank.live[0]=false;expect(render(0,0.75f,0.375f),"disabling Live mode processes the full chain");
    juce::PluginDescription desc;desc.name="Capacity check";while(bank.chains[2].size()<8)insert(2,1,0);expect(!bank.add(2,desc)&&bank.chains[2].size()==8,"eight effects per channel limit preserves the existing chain");
    expect(PluginBank::empty(*bank.chains[0][0]),"removing an effect preserves its empty slot");
    bank.move(1,0,3);expect(PluginBank::empty(*bank.chains[1][0])&&render(1,0.0625f,-0.03125f),"effects move into empty slots without changing audio");
    bank.chains[1][3]->mix=0.5f;bank.chains[1][3]->smoothMix=0.5f;expect(render(1,0.09375f,-0.046875f),"slot mix blends dry and processed audio");
    bank.chains[1][3]->mix=0;bank.chains[1][3]->smoothMix=0;expect(render(1,0.125f,-0.0625f),"zero percent mix preserves dry stereo audio");
    bank.chains[1][3]->mix=1;bank.chains[1][3]->smoothMix=1;expect(render(1,0.0625f,-0.03125f),"full mix preserves fully processed audio");
    auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("kmx6-discovery-check-"+juce::Uuid().toString(),{});root.createDirectory();
    auto bundle=root.getChildFile("Nested/Effect.vst3");bundle.getChildFile("Contents/x86_64-win").createDirectory();bundle.getChildFile("Contents/x86_64-win/Effect.vst3").replaceWithText("fixture");
    root.getChildFile("Legacy.dll").replaceWithText("ignored");auto single=root.getChildFile("Single.vst3");single.replaceWithText("fixture");
    PluginCatalog::Enumeration job;std::set<juce::String> visited;PluginCatalog::enumerate(root,job,visited);PluginCatalog::enumerate(root.getChildFile("Nested"),job,visited);PluginCatalog::enumerate(bundle,job,visited);
    expect(job.files.size()==2,"recursive custom-folder discovery deduplicates overlapping roots and VST3 bundles");
    auto stamp=PluginCatalog::fingerprint(bundle);bundle.getChildFile("Contents/x86_64-win/Effect.vst3").appendText("changed");expect(stamp!=PluginCatalog::fingerprint(bundle),"bundle binary changes invalidate cached scan results");
    auto manifest=root.getChildFile("library.xml");PluginCatalog first(false,manifest);first.customFolders.add(root.getFullPathName());juce::PluginDescription effect;effect.name="Fixture effect";effect.manufacturerName="Test";effect.fileOrIdentifier=single.getFullPathName();effect.pluginFormatName="VST3";effect.uniqueId=1;
    juce::Array<juce::PluginDescription> types;types.add(effect);first.remember(single,types);first.remember(single,types);PluginCatalog second(false,manifest);second.readCache();expect(second.customFolders.size()==1&&second.customFolders[0]==root.getFullPathName(),"custom scan folders persist");expect(second.available().size()==1&&second.modules.size()==1,"cached discovery results persist without duplicate effect entries");
    second.modules[0].failed=true;expect(second.available().isEmpty(),"failed scans are excluded from the plugin browser");root.deleteRecursively();
    report+="\n"+juce::String(passed)+" passed / "+juce::String(failed)+" failed. No audio devices opened.\n";return failed?1:0;
}
