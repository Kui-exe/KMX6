#pragma once
#include <JuceHeader.h>

namespace uiScale {
constexpr int baseWidth=1440,baseHeight=900,minimum=25,maximum=200,step=5;
inline int normalise(double percent){if(!std::isfinite(percent))return 100;return juce::jlimit(minimum,maximum,int(std::round(juce::jlimit(double(minimum),double(maximum),percent)/step))*step);}
inline juce::File settingsFile(){return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("ui-settings.json");}
inline int load(){auto data=juce::JSON::parse(settingsFile());return normalise(double(data.getProperty("scalePercent",100)));}
inline void save(int percent){auto* data=new juce::DynamicObject;data->setProperty("scalePercent",normalise(percent));settingsFile().replaceWithText(juce::JSON::toString(juce::var(data)));}

// Zoom the fixed canvas as a unit. Never resize or individually reposition its
// descendants when the scale changes; JUCE maps pointer input through the transform.
class Canvas final : public juce::Component {
    std::unique_ptr<juce::Component> content;
    double presentedPercent=100;
    bool animationCached=false;
    struct FrozenFrame final : juce::CachedComponentImage {
        juce::Component& owner;juce::Image image;
        FrozenFrame(juce::Component& component,juce::Image frame):owner(component),image(std::move(frame)){}
        void paint(juce::Graphics& g)override{if(image.isValid()){g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);g.drawImage(image,owner.getLocalBounds().toFloat());}else owner.paintEntireComponent(g,true);}
        bool invalidateAll()override{return false;}
        bool invalidate(const juce::Rectangle<int>&)override{return false;}
        void releaseResources()override{image=juce::Image();}
    };
public:
    explicit Canvas(std::unique_ptr<juce::Component> c):content(std::move(c)){setOpaque(true);content->setOpaque(true);content->setBounds(0,0,baseWidth,baseHeight);addAndMakeVisible(*content);setPercent(100);}
    ~Canvas()override{endAnimation();}
    void paint(juce::Graphics& g)override{g.fillAll(juce::Colour(0xff13121b));}
    void beginAnimation(){if(animationCached)return;auto frame=content->createComponentSnapshot(content->getLocalBounds(),true,1.0f);if(frame.isValid()){content->setCachedComponentImage(new FrozenFrame(*content,std::move(frame)));animationCached=true;}}
    void endAnimation(){if(!animationCached)return;animationCached=false;content->setCachedComponentImage(nullptr);content->repaint();repaint();}
    bool hasAnimationFrame()const{return animationCached;}
    juce::Component& root()const{return *content;}
    int getPercent()const{return normalise(presentedPercent);}
    double getPresentedPercent()const{return presentedPercent;}
    void setPercent(double value){setContinuousPercent(normalise(value));}
    void setContinuousPercent(double value){presentedPercent=juce::jlimit(double(minimum),double(maximum),value);const float scale=float(presentedPercent/100.0);content->setTransform(juce::AffineTransform::scale(scale));setSize(int(std::ceil(baseWidth*presentedPercent/100.0)),int(std::ceil(baseHeight*presentedPercent/100.0)));repaint();}
};

struct Transition {
    static constexpr double duration=0.18;
    double current=100,from=100,to=100,elapsed=0;bool running=false;
    void reset(int percent){current=from=to=normalise(percent);elapsed=0;running=false;}
    void request(int target){from=current;to=normalise(target);elapsed=0;running=std::abs(to-from)>0.000001;}
    void advance(double seconds){if(!running)return;elapsed+=std::max(0.0,seconds);double t=std::min(1.0,elapsed/duration);double eased=t*t*(3.0-2.0*t);current=from+(to-from)*eased;if(t>=1){current=to;running=false;}}
};

class StepButton final : public juce::TextButton {
    bool rightPress=false;
public:
    std::function<void(int)> onStep;
    StepButton():TextButton("SCALE 100%"){
        onClick=[this]{if(onStep)onStep(step);};
        setTooltip("Left-click: +5%. Right-click: -5%. Range: 25%-200%. Ctrl+0: 100%.");
    }
    void mouseDown(const juce::MouseEvent& event)override{
        rightPress=event.mods.isRightButtonDown();
        if(rightPress)setState(juce::Button::buttonDown);else juce::TextButton::mouseDown(event);
    }
    void mouseUp(const juce::MouseEvent& event)override{
        if(!rightPress){juce::TextButton::mouseUp(event);return;}
        rightPress=false;bool inside=getLocalBounds().toFloat().contains(event.position);
        setState(inside?juce::Button::buttonOver:juce::Button::buttonNormal);
        if(inside&&!event.mouseWasDraggedSinceMouseDown()&&onStep)onStep(-step);
    }
};

inline int runChecks(juce::String& report){
    int passed=0,failed=0;auto expect=[&](bool okay,const juce::String& text){report+=(okay?"PASS ":"FAIL ")+text+"\n";okay?++passed:++failed;};
    struct PaintCounter : juce::Component {int paints=0;void paint(juce::Graphics& g)override{++paints;g.fillAll(juce::Colour(0xff13121b));}};auto root=std::make_unique<PaintCounter>();auto* paintedRoot=root.get();std::array<juce::Component,6> strips;std::array<juce::Component,6> controls;
    for(int i=0;i<6;++i){strips[i].setBounds(18+i*234,100,228,752);controls[i].setBounds(12,620,204,26);strips[i].addAndMakeVisible(controls[i]);root->addAndMakeVisible(strips[i]);}
    Canvas canvas(std::move(root));
    bool geometry=true,positions=true,hitTests=true,sizes=true;
    for(int repeat=0;repeat<3;++repeat)for(int value=minimum;value<=maximum;value+=step){
        canvas.setPercent(value);float factor=value/100.0f;sizes&=canvas.getWidth()==baseWidth*value/100&&canvas.getHeight()==baseHeight*value/100;
        geometry&=canvas.root().getBounds()==juce::Rectangle<int>(0,0,baseWidth,baseHeight);
        for(int i=0;i<6;++i){geometry&=strips[i].getBounds()==juce::Rectangle<int>(18+i*234,100,228,752)&&controls[i].getBounds()==juce::Rectangle<int>(12,620,204,26);
            auto point=canvas.getLocalPoint(&controls[i],juce::Point<float>(102,13));auto logical=controls[i].getLocalPoint(&canvas,point);positions&=std::abs(point.x-(18+i*234+12+102)*factor)<0.002f&&std::abs(point.y-(100+620+13)*factor)<0.002f;hitTests&=logical.getDistanceFrom({102,13})<0.002f;
        }
    }
    expect(geometry,"all control bounds stay unchanged through 108 scale changes (25%-200%)");expect(positions,"all six strips use exactly one uniform transform");expect(hitTests,"pointer coordinates map back to the original control positions at every scale");expect(sizes,"canvas dimensions match every requested 5% step");
    canvas.setPercent(200);canvas.setPercent(25);canvas.setPercent(100);expect(canvas.root().getTransform().isIdentity()&&canvas.getWidth()==1440&&canvas.getHeight()==900,"100% restores exact original geometry without cumulative drift");expect(normalise(24)==25&&normalise(201)==200&&normalise(102)==100&&normalise(103)==105,"scale clamps to 25%-200% and snaps to 5% increments");
    juce::ResizableWindow frame("Scale geometry check",juce::Colours::black,false);frame.setContentNonOwned(&canvas,false);bool fit=true;
    for(int value=minimum;value<=maximum;value+=step){canvas.setPercent(value);frame.setContentComponentSize(baseWidth*value/100,baseHeight*value/100);fit&=canvas.getWidth()==baseWidth*value/100&&canvas.getHeight()==baseHeight*value/100;}
    expect(fit&&frame.getContentComponent()==&canvas,"window client area follows the complete scaled canvas at every size without a scroll viewport");frame.clearContentComponent();
    Transition transition;transition.reset(100);transition.request(105);expect(transition.current==100&&transition.running,"scale requests begin at the current visual size without jumping");
    bool bounded=true,animatedGeometry=true;double previous=transition.current;
    for(int i=0;i<12;++i){transition.advance(Transition::duration/11.0);bounded&=transition.current>=previous&&transition.current<=105;previous=transition.current;canvas.setContinuousPercent(transition.current);for(int c=0;c<6;++c)animatedGeometry&=strips[c].getBounds()==juce::Rectangle<int>(18+c*234,100,228,752)&&controls[c].getBounds()==juce::Rectangle<int>(12,620,204,26);}
    expect(bounded&&!transition.running&&transition.current==105,"animation eases monotonically to the exact 5% target without overshoot");expect(animatedGeometry,"control layout bounds remain fixed throughout fractional animation frames");
    transition.reset(100);transition.request(150);transition.advance(Transition::duration*0.4);double visual=transition.current;transition.request(95);expect(transition.current==visual&&transition.from==visual,"rapid reverse clicks retarget from the current frame without a size jump");transition.advance(Transition::duration);expect(transition.current==95&&!transition.running,"retargeted animation settles at the exact saved percentage");
    canvas.setPercent(100);canvas.beginAnimation();int capturedPaints=paintedRoot->paints;bool opaqueFrames=true;
    for(int i=0;i<8;++i){canvas.setContinuousPercent(100+i*0.625);auto image=canvas.createComponentSnapshot(canvas.getLocalBounds());auto corner=image.getPixelAt(image.getWidth()-1,image.getHeight()-1);opaqueFrames&=corner.getAlpha()==255&&corner!=juce::Colours::white;}
    expect(canvas.hasAnimationFrame()&&paintedRoot->paints==capturedPaints,"zoom frames reuse the captured mixer image instead of repainting all controls");expect(canvas.isOpaque()&&opaqueFrames,"fractional resize frames fill their full bounds with an opaque dark background");canvas.endAnimation();canvas.createComponentSnapshot(canvas.getLocalBounds());expect(!canvas.hasAnimationFrame()&&paintedRoot->paints>capturedPaints,"live control rendering resumes after the zoom frame is released");
    report+="\n"+juce::String(passed)+" passed / "+juce::String(failed)+" failed. No audio devices opened.\n";return failed?1:0;
}
}
