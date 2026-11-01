#pragma once
#include "PluginHost.h"

class PluginManagerPanel : public juce::Component,private juce::Timer,private juce::TableListBoxModel {
    PluginCatalog& catalog;
    juce::Label title,pathsTitle,progress,summary;
    juce::TextButton scan{"FIND PLUGINS"},addFolder{"ADD FOLDER"},removeFolder{"REMOVE"};
    juce::TextEditor search;
    struct Entry{juce::String name,vendor,format,status,path;};std::vector<Entry> entries;
    struct Paths : public juce::ListBoxModel {
        PluginCatalog& catalog;juce::StringArray paths;int standardCount=0;
        explicit Paths(PluginCatalog& c):catalog(c){refresh();}
        void refresh(){paths=PluginCatalog::defaultFolders();standardCount=paths.size();paths.addArray(catalog.customFolders);}
        int getNumRows()override{return paths.size();}
        void paintListBoxItem(int row,juce::Graphics& g,int width,int height,bool selected)override{if(row<0||row>=paths.size())return;g.fillAll(selected?juce::Colour(0xff514061):juce::Colour(0xff1b1824));g.setFont(11);g.setColour(row<standardCount?juce::Colour(0xffb0a6bd):juce::Colour(0xfff1eaf6));g.drawText((row<standardCount?"AUTO  ":"CUSTOM  ")+paths[row],6,0,width-12,height,juce::Justification::centredLeft);}
    } paths;
    juce::ListBox pathList{"Plugin search folders",&paths};
    juce::TableListBox table{"Installed plugins",this};
    std::unique_ptr<juce::FileChooser> chooser;int seenRevision=-1;
    int getNumRows()override{return int(entries.size());}
    void paintRowBackground(juce::Graphics& g,int row,int,int,bool selected)override{g.fillAll(selected?juce::Colour(0xff514061):row%2?juce::Colour(0xff24202e):juce::Colour(0xff1f1b28));}
    void paintCell(juce::Graphics& g,int row,int column,int width,int height,bool)override{if(row<0||row>=int(entries.size()))return;auto& e=entries[row];juce::String value=column==1?e.name:column==2?e.vendor:column==3?e.format:column==4?e.status:e.path;g.setColour(e.status=="Unavailable"?juce::Colour(0xffefad99):juce::Colour(0xffeee7f4));g.setFont(12);g.drawText(value,6,0,width-12,height,juce::Justification::centredLeft);}
    void rebuild(){
        entries.clear();auto filter=search.getText().trim();for(auto& module:catalog.modules){
            if(module.failed){if(filter.isEmpty()||module.path.getFullPathName().containsIgnoreCase(filter))entries.push_back({module.path.getFileNameWithoutExtension(),{},"VST3","Unavailable",module.path.getFullPathName()});continue;}
            for(auto& d:module.effects)if(filter.isEmpty()||(d.name+" "+d.manufacturerName+" "+module.path.getFullPathName()).containsIgnoreCase(filter))entries.push_back({d.name,d.manufacturerName,"VST3 / 64-bit","Ready",module.path.getFullPathName()});
        }
        std::sort(entries.begin(),entries.end(),[](auto& a,auto& b){return a.name.compareIgnoreCase(b.name)<0;});table.updateContent();table.repaint();seenRevision=catalog.revision;
    }
    void timerCallback()override{if(seenRevision!=catalog.revision)rebuild();scan.setEnabled(!catalog.busy);int row=pathList.getSelectedRow();removeFolder.setEnabled(row>=paths.standardCount&&row<paths.paths.size());progress.setText(catalog.progress,juce::dontSendNotification);summary.setText(juce::String(catalog.available().size())+" effects available  /  select effects from the channel slots",juce::dontSendNotification);}
public:
    explicit PluginManagerPanel(PluginCatalog& c):catalog(c),paths(c){
        for(auto* label:{&title,&pathsTitle,&progress,&summary}){addAndMakeVisible(*label);label->setColour(juce::Label::textColourId,juce::Colour(0xffeee7f4));label->setFont(juce::FontOptions(12));}
        title.setText("PLUGIN MANAGER",juce::dontSendNotification);title.setFont(juce::FontOptions(18,juce::Font::bold));pathsTitle.setText("PLUGIN SEARCH FOLDERS",juce::dontSendNotification);
        for(juce::Component* control:std::array<juce::Component*,6>{&scan,&addFolder,&removeFolder,&search,&pathList,&table})addAndMakeVisible(control);
        pathList.setRowHeight(36);pathList.setColour(juce::ListBox::backgroundColourId,juce::Colour(0xff1b1824));table.setRowHeight(26);table.setColour(juce::ListBox::backgroundColourId,juce::Colour(0xff1b1824));
        auto& header=table.getHeader();header.addColumn("Plugin",1,200,100,400);header.addColumn("Vendor",2,160,90,300);header.addColumn("Format",3,100,70,140);header.addColumn("Status",4,92,70,120);header.addColumn("File location",5,420,180,900);table.setHeaderHeight(28);
        search.setTextToShowWhenEmpty("Search installed plugins...",juce::Colour(0xffb0a6bd));search.onTextChange=[this]{rebuild();};scan.onClick=[this]{catalog.scan(true);};
        addFolder.onClick=[this]{chooser=std::make_unique<juce::FileChooser>("Add a VST3 search folder",juce::File::getSpecialLocation(juce::File::userHomeDirectory));auto safe=juce::Component::SafePointer<PluginManagerPanel>(this);chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectDirectories,[safe](const juce::FileChooser& fc){if(safe&&fc.getResult().isDirectory()){safe->catalog.addFolder(fc.getResult());safe->paths.refresh();safe->pathList.updateContent();}});};
        removeFolder.onClick=[this]{int i=pathList.getSelectedRow()-paths.standardCount;if(i>=0){catalog.removeFolder(i);paths.refresh();pathList.updateContent();}};
        setSize(1040,560);rebuild();timerCallback();startTimer(250);
    }
    ~PluginManagerPanel()override{stopTimer();pathList.setModel(nullptr);table.setModel(nullptr);}
    void paint(juce::Graphics& g)override{g.fillAll(juce::Colour(0xff252332));g.setColour(juce::Colour(0xff494052));g.drawVerticalLine(292,58.0f,float(getHeight()-58));}
    void resized()override{int w=getWidth(),h=getHeight();title.setBounds(16,10,280,30);scan.setBounds(16,56,260,32);pathsTitle.setBounds(16,100,260,24);pathList.setBounds(16,128,260,h-236);addFolder.setBounds(16,h-98,150,28);removeFolder.setBounds(174,h-98,102,28);search.setBounds(308,14,w-324,28);table.setBounds(308,56,w-324,h-126);progress.setBounds(16,h-52,w-32,22);summary.setBounds(16,h-28,w-32,20);}
};

class PluginManagerWindow : public juce::DocumentWindow {
public:
    explicit PluginManagerWindow(PluginCatalog& catalog):DocumentWindow("KMX6 Plugin Manager",juce::Colour(0xff252332),closeButton){setUsingNativeTitleBar(true);setContentOwned(new PluginManagerPanel(catalog),true);setResizable(true,false);setResizeLimits(860,480,1400,900);centreWithSize(1040,560);setVisible(true);}
    void closeButtonPressed()override{setVisible(false);}
};

class PluginPanel : public juce::Component,private juce::Timer {
    struct LiveSwitch : public juce::ToggleButton {
        explicit LiveSwitch(const juce::String& label):ToggleButton(label){setClickingTogglesState(true);setTooltip("Live mode skips effects reporting more than 5 ms latency.");}
        void paintButton(juce::Graphics& g,bool hovered,bool down)override{
            bool on=getToggleState();float y=getHeight()*0.5f;auto track=juce::Rectangle<float>(2,y-8,32,16);
            if(on){g.setColour(juce::Colour(0xffbb85f5).withAlpha(0.12f));g.fillRoundedRectangle(track.expanded(2),10);}
            g.setColour(on?juce::Colour(0xff81549e):juce::Colour(0xff514a5b));g.fillRoundedRectangle(track,8);
            g.setColour(on?juce::Colour(0xffc99aee):juce::Colour(0xff706778));g.drawRoundedRectangle(track,8,0.8f);
            float x=on?25.0f:11.0f;g.setColour(on?juce::Colour(0xffefdcff):juce::Colour(0xffa59da9));g.fillEllipse(x-5,y-5,10,10);
            if(hovered){g.setColour(juce::Colours::white.withAlpha(down?0.22f:0.1f));g.fillRoundedRectangle(track,8);}
            g.setColour(on?juce::Colour(0xffeee5f4):juce::Colour(0xffb2a7bd));g.setFont(12);g.drawText(getButtonText(),44,0,getWidth()-44,getHeight(),juce::Justification::centredLeft);
        }
    };
    PluginBank& bank;int channel;juce::Label title,summary,mixLabel,onLabel;LiveSwitch live{"Live mode (5 ms limit)"};
    struct Light : public juce::ToggleButton {
        Light(){setClickingTogglesState(true);setTitle("Effect on / off");}
        void paintButton(juce::Graphics& g,bool hover,bool down)override{
            bool on=isEnabled()&&getToggleState();float cx=getWidth()*0.5f,cy=getHeight()*0.5f;auto face=juce::Rectangle<float>(cx-5,cy-5,10,10);
            if(on){for(int i=3;i>=1;--i){g.setColour(juce::Colour(0xff9cdda8).withAlpha(0.045f*(4-i)));g.fillEllipse(face.expanded(float(i)*1.4f));}}
            g.setColour(on?juce::Colour(0xffa6e4ad):juce::Colour(0xff68626f));g.fillEllipse(face);
            g.setColour(on?juce::Colour(0xffdcf8dd):juce::Colour(0xff807986));g.drawEllipse(face,0.7f);
            if(on){g.setColour(juce::Colours::white.withAlpha(0.55f));g.fillEllipse(cx-2.3f,cy-3,3.3f,2.2f);}
            if(hover&&isEnabled()){g.setColour(juce::Colours::white.withAlpha(down?0.4f:0.18f));g.drawEllipse(face.expanded(2),0.8f);}
        }
    };
    struct MenuButton : public juce::TextButton {
        void paintButton(juce::Graphics& g,bool hover,bool)override{g.setColour(juce::Colour(hover?0xffefe0ff:0xff9d91aa));float x=getWidth()*0.5f,y=getHeight()*0.5f;juce::Path p;p.addTriangle(x-2,y-3,x+2,y,x-2,y+3);g.fillPath(p);}
    };
    struct MixKnob : public juce::Slider {
        void paint(juce::Graphics& g)override{float cx=getWidth()*0.5f,cy=getHeight()*0.5f;auto face=juce::Rectangle<float>(cx-8,cy-8,16,16);bool active=isEnabled();g.setColour(juce::Colour(0xff24202c));g.fillEllipse(face);g.setColour(juce::Colour(active?0xff766482:0xff443d4d));g.drawEllipse(face,1);
            float start=juce::MathConstants<float>::pi*1.2f,end=juce::MathConstants<float>::pi*2.8f;float angle=start+float(getValue())*(end-start);if(active){juce::Path arc;arc.addCentredArc(cx,cy,8,8,0,start,angle,true);g.setColour(juce::Colour(0xffbd92e3));g.strokePath(arc,juce::PathStrokeType(1.5f));}
            g.setColour(juce::Colour(active?0xffdfc7f1:0xff615868));g.drawLine(cx+std::sin(angle)*3,cy-std::cos(angle)*3,cx+std::sin(angle)*5.5f,cy-std::cos(angle)*5.5f,1);
        }
    };
    struct NameButton : public juce::TextButton {using juce::TextButton::TextButton;void paintButton(juce::Graphics& g,bool hover,bool)override{if(hover){g.setColour(juce::Colour(0xff46374f));g.fillRect(getLocalBounds());}g.setColour(findColour(juce::TextButton::textColourOffId));g.setFont(12);g.drawText(getButtonText(),5,0,getWidth()-10,getHeight(),juce::Justification::centredLeft);}};
    struct Row : public juce::Component {
        PluginPanel& panel;int index;MenuButton menu;NameButton name;MixKnob mix;Light enable;
        Row(PluginPanel& p,int i):panel(p),index(i){for(juce::Component* c:std::array<juce::Component*,4>{&menu,&name,&mix,&enable})addAndMakeVisible(c);
            menu.onClick=[this]{panel.slotMenu(index);};name.onClick=[this]{if(panel.valid(index)&&panel.bank.chains[panel.channel][index]->processor)panel.bank.edit(panel.channel,index);else panel.slotMenu(index);};
            mix.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);mix.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);mix.setRange(0,1,0.01);mix.setValue(1);mix.setDoubleClickReturnValue(true,1);mix.onValueChange=[this]{if(panel.valid(index))panel.bank.chains[panel.channel][index]->mix=float(mix.getValue());};mix.onDragEnd=[this]{panel.bank.save();};
            enable.setTooltip("Turn this plugin on / off");enable.onClick=[this]{if(panel.valid(index)){panel.bank.chains[panel.channel][index]->bypass=!enable.getToggleState();panel.bank.save();}};
        }
        void refresh(){bool filled=panel.valid(index);auto* slot=filled?panel.bank.chains[panel.channel][index].get():nullptr;name.setButtonText(slot?(slot->alias.isEmpty()?slot->description.name:slot->alias)+(slot->loading?" ...":""):"Slot "+juce::String(index+1));name.setColour(juce::TextButton::textColourOffId,slot?juce::Colour(0xffeee7f4):juce::Colour(0xff94899e));
            bool ready=slot&&slot->processor;bool blend=ready&&slot->mixReady;mix.setEnabled(blend);if(!mix.isMouseButtonDown())mix.setValue(slot?slot->mix.load():1,juce::dontSendNotification);mix.setTooltip(blend?"Wet / dry mix: "+juce::String(int(mix.getValue()*100))+"% wet. Double-click for 100%.":ready?"Mix unavailable until the engine is refreshed for this plugin latency.":"Wet / dry mix");enable.setEnabled(ready);enable.setToggleState(ready&&!slot->bypass,juce::dontSendNotification);name.setTooltip(slot?!slot->error.isEmpty()?slot->error:"Click to open. Slot menu: rename, replace, move or remove.":"Choose an installed effect. Scan folders in Plugin Manager.");}
        void paint(juce::Graphics& g)override{g.fillAll(index%2?juce::Colour(0xff2b2533):juce::Colour(0xff2d2735));g.setColour(juce::Colour(0xff3a3243));g.drawHorizontalLine(getHeight()-1,0,float(getWidth()));}
        void resized()override{int w=getWidth();menu.setBounds(0,0,18,34);name.setBounds(18,0,w-84,34);mix.setBounds(w-60,0,24,34);enable.setBounds(w-30,0,24,34);}
    };
    std::array<std::unique_ptr<Row>,8> rows;
    bool valid(int i)const{return i>=0&&i<int(bank.chains[channel].size())&&!PluginBank::empty(*bank.chains[channel][i]);}
    void rename(int i){if(!valid(i))return;auto* alert=new juce::AlertWindow("Rename effect slot","Name this effect slot",juce::MessageBoxIconType::NoIcon);auto& slot=*bank.chains[channel][i];alert->addTextEditor("name",slot.alias.isEmpty()?slot.description.name:slot.alias);alert->addButton("Save",1,juce::KeyPress(juce::KeyPress::returnKey));alert->addButton("Cancel",0,juce::KeyPress(juce::KeyPress::escapeKey));auto safe=juce::Component::SafePointer<PluginPanel>(this);alert->enterModalState(true,juce::ModalCallbackFunction::create([safe,alert,i](int result){if(safe&&result==1&&safe->valid(i)){safe->bank.chains[safe->channel][i]->alias=alert->getTextEditorContents("name").trim();safe->bank.save();}delete alert;}),false);}
    void slotMenu(int i){
        juce::PopupMenu menu,choose;auto plugins=bank.catalog.available();for(int n=0;n<plugins.size();++n)choose.addItem(100+n,plugins[n].name);
        bool filled=valid(i);bool loading=filled&&bank.chains[channel][i]->loading;
        if(!plugins.isEmpty())menu.addSubMenu(filled?"Replace effect":"Select effect",choose,!loading);else menu.addItem(99,"No plugins found - open Plugin Manager",false);
        if(filled){menu.addSeparator();menu.addItem(1,"Open editor",bank.chains[channel][i]->processor!=nullptr);menu.addItem(2,"Enabled",bank.chains[channel][i]->processor!=nullptr,!bank.chains[channel][i]->bypass.load());menu.addItem(3,"Rename...");menu.addSeparator();menu.addItem(4,"Move up",i>0&&!loading);menu.addItem(5,"Move down",i<7&&!loading);menu.addItem(6,"Remove",!loading);}
        auto safe=juce::Component::SafePointer<PluginPanel>(this);menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&rows[i]->menu),[safe,i,plugins](int result){if(!safe||result==0)return;if(result>=100){int n=result-100;if(n<plugins.size())safe->bank.addAt(safe->channel,i,plugins[n]);}else if(safe->valid(i)){if(result==1)safe->bank.edit(safe->channel,i);if(result==2){auto& slot=*safe->bank.chains[safe->channel][i];slot.bypass=!slot.bypass.load();safe->bank.save();}if(result==3)safe->rename(i);if(result==4)safe->bank.move(safe->channel,i,-1);if(result==5)safe->bank.move(safe->channel,i,1);if(result==6)safe->bank.remove(safe->channel,i);}safe->timerCallback();});
    }
    void timerCallback()override{int latency=0;for(auto& slot:bank.chains[channel])if(slot->processor&&!slot->bypass&&(!bank.live[channel]||slot->latency<=int(bank.rate.load()*0.005)))latency+=slot->latency.load();summary.setText(juce::String(bank.count(channel))+" / 8 effects   /   "+juce::String(latency*1000.0/bank.rate.load(),2)+" ms",juce::dontSendNotification);for(auto& row:rows)row->refresh();}
public:
    PluginPanel(PluginBank& b,int c):bank(b),channel(c){for(auto* label:{&title,&summary,&mixLabel,&onLabel}){addAndMakeVisible(*label);label->setColour(juce::Label::textColourId,juce::Colour(0xffeee7f4));label->setFont(juce::FontOptions(11));}title.setText((c<4?"MIC "+juce::String(c+1):"VIRTUAL "+juce::String(c-3))+"  /  EFFECTS",juce::dontSendNotification);title.setFont(juce::FontOptions(13,juce::Font::bold));summary.setFont(juce::FontOptions(10));summary.setColour(juce::Label::textColourId,juce::Colour(0xffa69ab1));mixLabel.setText("MIX",juce::dontSendNotification);onLabel.setText("ON",juce::dontSendNotification);for(auto* header:{&mixLabel,&onLabel}){header->setFont(juce::FontOptions(9));header->setJustificationType(juce::Justification::centred);header->setColour(juce::Label::textColourId,juce::Colour(0xffa69ab1));}for(int i=0;i<8;++i){rows[i]=std::make_unique<Row>(*this,i);addAndMakeVisible(*rows[i]);}addAndMakeVisible(live);live.setToggleState(bank.live[c],juce::dontSendNotification);live.onClick=[this]{bank.live[channel]=live.getToggleState();bank.save();};setSize(280,374);timerCallback();startTimer(150);}
    ~PluginPanel()override{stopTimer();bank.save();}
    void paint(juce::Graphics& g)override{g.fillAll(juce::Colour(0xff25212e));}
    void resized()override{title.setBounds(12,8,256,22);summary.setBounds(12,32,186,18);mixLabel.setBounds(208,32,24,18);onLabel.setBounds(238,32,24,18);for(int i=0;i<8;++i)rows[i]->setBounds(12,56+i*34,256,34);live.setBounds(12,340,256,24);}
};
