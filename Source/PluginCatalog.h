#pragma once
#include <JuceHeader.h>
#include <deque>
#include <set>

// Discovery never touches the realtime path. Enumeration is a background job;
// each candidate binary is inspected in a separate, bounded process.
class PluginCatalog : private juce::Timer {
public:
    struct Module {
        juce::File path;juce::String stamp;bool failed=false,checked=false;
        juce::Array<juce::PluginDescription> effects;
    };
    struct Enumeration {
        std::atomic<bool> done{false},cancelled{false};
        std::vector<juce::File> files;
    };
    juce::StringArray customFolders;
    std::vector<Module> modules;
    int revision=0,checkedCount=0,total=0,failedCount=0;
    juce::String progress="Plugin discovery is ready.";
    bool busy=false;
    juce::File storage;
    explicit PluginCatalog(bool runAutomatically=true,juce::File storageFile=cacheFile()):storage(storageFile){
        if(!runAutomatically)return;
        readCache();startTimer(100);scan(false);
    }
    ~PluginCatalog() override {stopTimer();if(enumeration)enumeration->cancelled=true;if(child.isRunning())child.kill();if(result.existsAsFile())result.deleteFile();}
    static juce::File cacheFile(){return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("plugin-library.xml");}
    static juce::String key(const juce::File& file){return file.getFullPathName().toLowerCase();}
    static juce::String fingerprint(const juce::File& module){
        auto binary=module.isDirectory()?module.getChildFile("Contents/x86_64-win").getChildFile(module.getFileName()):module;
        if(!binary.existsAsFile())binary=module;
        return juce::String(binary.getLastModificationTime().toMilliseconds())+":"+juce::String(binary.getSize());
    }
    static juce::StringArray defaultFolders(){
        juce::StringArray roots;
        auto common=juce::File::getSpecialLocation(juce::File::windowsProgramFilesCommon);
        auto program=juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory);
        auto local=juce::File::getSpecialLocation(juce::File::windowsLocalAppData);
        roots.add(common.getChildFile("VST3").getFullPathName());
        roots.add(local.getChildFile("Programs/Common/VST3").getFullPathName());
        roots.add(program.getChildFile("VSTPlugins").getFullPathName());
        roots.add(program.getChildFile("Steinberg/VSTPlugins").getFullPathName());
        roots.add(program.getChildFile("Steinberg/VST3").getFullPathName());
        roots.add(program.getChildFile("VST3").getFullPathName());
        // Windows drive letters must not be split at ':'.
        roots.addArray(juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("VST3_PATH",{}),";","\""));
        roots.removeEmptyStrings();roots.removeDuplicates(true);return roots;
    }
    juce::StringArray folders()const{auto roots=defaultFolders();roots.addArray(customFolders);roots.removeDuplicates(true);return roots;}
    static void enumerate(const juce::File& root,Enumeration& job,std::set<juce::String>& visited,int depth=0){
        if(job.cancelled||depth>32||!root.exists()||root.isSymbolicLink())return;
        if(!visited.insert(key(root)).second)return;
        if(root.hasFileExtension("vst3")){job.files.push_back(root);return;} // Never descend into bundles.
        if(!root.isDirectory())return;
        for(const auto& entry:juce::RangedDirectoryIterator(root,false,"*",juce::File::findFilesAndDirectories)){
            if(job.cancelled)return;auto f=entry.getFile();if(f.isDirectory()||f.hasFileExtension("vst3"))enumerate(f,job,visited,depth+1);
        }
    }
    bool addFolder(const juce::File& folder){
        if(!folder.isDirectory())return false;
        auto path=folder.getFullPathName();if(!customFolders.contains(path,true)){customFolders.add(path);writeCache();}
        scan(false);return true;
    }
    void removeFolder(int index){if(index<0||index>=customFolders.size())return;customFolders.remove(index);writeCache();scan(false);}
    void scan(bool retryFailed){
        if(busy){rescanRequested=true;retryRequested|=retryFailed;return;}
        busy=true;force=retryFailed;checkedCount=total=failedCount=0;progress="Looking for VST3 plugins...";
        auto roots=folders();enumeration=std::make_shared<Enumeration>();auto job=enumeration;
        juce::Thread::launch([job,roots]{std::set<juce::String> visited;for(auto& path:roots)enumerate(juce::File(path),*job,visited);job->done.store(true,std::memory_order_release);});
    }
    juce::Array<juce::PluginDescription> available()const{
        juce::Array<juce::PluginDescription> found;
        for(auto& module:modules)if(!module.failed)for(auto& desc:module.effects){bool duplicate=false;for(auto& d:found)if(d.isDuplicateOf(desc)){duplicate=true;break;}if(!duplicate)found.add(desc);}
        std::sort(found.begin(),found.end(),[](const auto& a,const auto& b){return a.name.compareIgnoreCase(b.name)<0;});return found;
    }
    // Direct-file imports also enter the shared library, without loading an effect.
    void remember(const juce::File& path,const juce::Array<juce::PluginDescription>& descriptions){
        auto* module=find(path);if(!module){modules.push_back({});module=&modules.back();module->path=path;}
        module->stamp=fingerprint(path);module->failed=false;module->checked=true;module->effects=descriptions;++revision;writeCache();
    }
    void writeCache(){
        juce::XmlElement xml("KMX6PluginLibrary");
        for(auto& path:customFolders)xml.createNewChildElement("Folder")->setAttribute("path",path);
        for(auto& module:modules){auto* node=xml.createNewChildElement("Module");node->setAttribute("path",module.path.getFullPathName());node->setAttribute("stamp",module.stamp);node->setAttribute("failed",module.failed);node->setAttribute("checked",module.checked);for(auto& d:module.effects)node->addChildElement(d.createXml().release());}
        if(!xml.writeTo(storage))progress="Could not save the plugin library beside KMX6.";
    }
    void readCache(){if(auto xml=juce::parseXML(storage))for(auto* node:xml->getChildIterator()){
        if(node->hasTagName("Folder")){auto path=node->getStringAttribute("path");if(path.isNotEmpty())customFolders.addIfNotAlreadyThere(path,true);}
        else if(node->hasTagName("Module")){auto path=node->getStringAttribute("path");if(path.isEmpty())continue;Module m;m.path=juce::File(path);m.stamp=node->getStringAttribute("stamp");m.failed=node->getBoolAttribute("failed");m.checked=node->getBoolAttribute("checked");for(auto* desc:node->getChildIterator()){juce::PluginDescription d;if(d.loadFromXml(*desc)&&!d.isInstrument)m.effects.add(d);}modules.push_back(std::move(m));}}
    }
private:
    std::shared_ptr<Enumeration> enumeration;
    std::deque<juce::File> queue;
    juce::ChildProcess child;juce::File result,current;
    double began=0;bool running=false,force=false,rescanRequested=false,retryRequested=false;
    Module* find(const juce::File& f){for(auto& module:modules)if(key(module.path)==key(f))return &module;return nullptr;}
    void finishModule(bool timedOut){
        auto* module=find(current);if(module){auto xml=juce::parseXML(result);module->effects.clear();module->failed=timedOut||child.getExitCode()!=0||!xml;
            if(xml&&!module->failed)for(auto* node:xml->getChildIterator()){juce::PluginDescription d;if(d.loadFromXml(*node)&&!d.isInstrument)module->effects.add(d);}
            module->checked=true;if(module->failed)++failedCount;}
        result.deleteFile();running=false;++checkedCount;++revision;writeCache();
    }
    void timerCallback()override{
        if(!busy)return;
        if(enumeration){if(!enumeration->done.load(std::memory_order_acquire))return;
            std::set<juce::String> discovered;for(auto& f:enumeration->files)discovered.insert(key(f));
            // Remove stale catalog results, while preserving manually imported modules.
            modules.erase(std::remove_if(modules.begin(),modules.end(),[](auto& m){return !m.path.exists();}),modules.end());
            for(auto& f:enumeration->files){auto* module=find(f);if(!module){modules.push_back({});module=&modules.back();module->path=f;}
                auto stamp=fingerprint(f);if(force||!module->checked||stamp!=module->stamp){module->stamp=stamp;queue.push_back(f);}else if(module->failed)++failedCount;}
            enumeration.reset();total=int(queue.size());++revision;
        }
        if(running){bool timedOut=juce::Time::getMillisecondCounterHiRes()-began>45000;
            if(child.isRunning()&&!timedOut)return;
            if(timedOut)child.kill();finishModule(timedOut);
        }
        if(queue.empty()){
            busy=false;writeCache();progress=juce::String(available().size())+" effects available"+(failedCount?"; "+juce::String(failedCount)+" modules unavailable. RESCAN retries them.":". Scan complete.");
            if(rescanRequested){bool retry=retryRequested;rescanRequested=retryRequested=false;scan(retry);}return;
        }
        current=queue.front();queue.pop_front();auto* module=find(current);
        // Persist before starting: an interrupted scan is skipped on next startup.
        if(module){module->failed=true;module->checked=true;writeCache();}
        result=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("kmx6-scan-"+juce::Uuid().toString(),".xml");
        juce::StringArray args{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),"--scan-vst3",current.getFullPathName(),result.getFullPathName()};
        began=juce::Time::getMillisecondCounterHiRes();running=child.start(args);progress="Scanning "+juce::String(checkedCount+1)+" / "+juce::String(total)+": "+current.getFileName();
        if(!running)finishModule(true);
    }
};
