#pragma once
#include <thread>
#include <numeric>
#include <chrono>
// Isolated production-callback benchmark. No Windows audio device is opened.
inline int runMixerBenchmark(bool heavy=false){
    using Clock=std::chrono::steady_clock;
    const auto folder=appFolder();juce::Array<juce::var> rows;
    juce::String csv="scenario,sample_rate,buffer_frames,blocks,mean_us,p50_us,p95_us,p99_us,max_us,deadline_us,over_budget,wall_seconds\n";
    const int workers=heavy?std::clamp(juce::SystemStats::getNumCpus()-2,1,8):std::clamp(juce::SystemStats::getNumCpus()/4,1,4);
    const juce::String stem=heavy?"benchmark-heavy":"benchmark";
    const std::vector<double> rates=heavy?std::vector<double>{48000}:std::vector<double>{44100,48000,96000};
    const std::vector<int> sizes=heavy?std::vector<int>{128,256,512}:std::vector<int>{64,128,256,512,1024};
    for(int loaded=0;loaded<2;++loaded){
        std::atomic<bool> finish{false};std::vector<std::thread> load;
        if(loaded)for(int i=0;i<workers;++i)load.emplace_back([&finish,i,heavy]{double x=0.1+i;while(!finish.load(std::memory_order_relaxed)){auto until=Clock::now()+std::chrono::milliseconds(heavy?3:2);while(Clock::now()<until){x=std::sin(x)+1.000001;std::atomic_signal_fence(std::memory_order_seq_cst);}std::this_thread::sleep_for(std::chrono::milliseconds(heavy?1:2));}if(!std::isfinite(x))std::terminate();});
        for(double rate:rates)for(int frames:sizes){
            auto mixer=std::make_unique<MixerEngine>(false);mixer->sampleRate=rate;mixer->bufferSize=frames;mixer->masterDb=-12;mixer->physicalOutputCount=2;mixer->activeOutputs.fill(-1);mixer->activeOutputs[0]=0;mixer->activeOutputs[1]=1;
            std::array<std::vector<float>,6> input;std::array<const float*,6> pointers;std::array<std::vector<float>,2> output;std::array<float*,2> out;
            for(int c=0;c<6;++c){input[c].resize(frames);for(int i=0;i<frames;++i)input[c][i]=0.025f*float(std::sin((i+c*17)*0.037));pointers[c]=input[c].data();auto& channel=mixer->channel[c];channel.source=1;channel.inputChannel=c;channel.fader=-6;channel.trim=3;channel.low=2;channel.mid=-3;channel.high=2;channel.hpf=true;channel.gateEnabled=true;channel.limiterEnabled=true;channel.monitor=true;channel.monitor2=true;channel.send=true;channel.send2=true;channel.send3=true;channel.send4=true;channel.prepare(rate);mixer->activeInputs[c]=c;}
            for(int c=0;c<2;++c){output[c].resize(frames);out[c]=output[c].data();}
            // Queue writes are part of the production callback. Consumption is outside its timing.
            std::array<VirtualOutput*,4> buses{&mixer->b1,&mixer->b2,&mixer->b3,&mixer->b4};for(auto* bus:buses){bus->connected=true;bus->queue.rate=rate;bus->queue.target=frames*2+2;bus->queue.minimumTarget=frames*2+2;}
            std::vector<kmx6::Stereo> drained(frames);std::vector<double> times;times.reserve(50000);
            const auto deadline=frames*1000000.0/rate;
            for(int warm=0;warm<100;++warm){mixer->audioDeviceIOCallbackWithContext(pointers.data(),6,out.data(),2,frames,{});for(auto* bus:buses)bus->queue.pull(drained.data(),frames,rate);}
            const auto start=Clock::now();
            do{auto begin=Clock::now();mixer->audioDeviceIOCallbackWithContext(pointers.data(),6,out.data(),2,frames,{});auto end=Clock::now();times.push_back(std::chrono::duration<double,std::micro>(end-begin).count());for(auto* bus:buses)bus->queue.pull(drained.data(),frames,rate);}while(std::chrono::duration<double>(Clock::now()-start).count()<(heavy?(loaded?10.0:5.0):1.0)||times.size()<1000);
            const double wall=std::chrono::duration<double>(Clock::now()-start).count();std::sort(times.begin(),times.end());
            const auto percentile=[&](double p){return times[std::min(times.size()-1,size_t(std::ceil(p*times.size())-1))];};
            const double mean=std::accumulate(times.begin(),times.end(),0.0)/times.size();const auto missed=std::count_if(times.begin(),times.end(),[&](double t){return t>deadline;});
            auto* row=new juce::DynamicObject;const juce::String scenario=loaded?"bounded_cpu_contention":"baseline";
            row->setProperty("scenario",scenario);row->setProperty("sampleRate",rate);row->setProperty("bufferFrames",frames);row->setProperty("blocks",int(times.size()));row->setProperty("meanUs",mean);row->setProperty("p50Us",percentile(0.5));row->setProperty("p95Us",percentile(0.95));row->setProperty("p99Us",percentile(0.99));row->setProperty("maxUs",times.back());row->setProperty("deadlineUs",deadline);row->setProperty("overBudget",int(missed));row->setProperty("wallSeconds",wall);rows.add(juce::var(row));
            csv+=scenario+","+juce::String(rate,0)+","+juce::String(frames)+","+juce::String(times.size())+","+juce::String(mean,3)+","+juce::String(percentile(0.5),3)+","+juce::String(percentile(0.95),3)+","+juce::String(percentile(0.99),3)+","+juce::String(times.back(),3)+","+juce::String(deadline,3)+","+juce::String(missed)+","+juce::String(wall,3)+"\n";
            folder.getChildFile(stem+"-progress.txt").replaceWithText(scenario+" / "+juce::String(rate,0)+" Hz / "+juce::String(frames)+" frames\n");
        }
        finish=true;for(auto& thread:load)thread.join();
    }
    auto* report=new juce::DynamicObject;report->setProperty("timestamp",juce::Time::getCurrentTime().toISO8601(true));report->setProperty("os",juce::SystemStats::getOperatingSystemName());report->setProperty("cpu",juce::SystemStats::getCpuModel());report->setProperty("logicalCpus",juce::SystemStats::getNumCpus());report->setProperty("ramMB",juce::SystemStats::getMemorySizeInMegabytes());report->setProperty("loadWorkers",workers);report->setProperty("loadDutyCycle",heavy?0.75:0.5);report->setProperty("description","Six sources; HPF, three-band EQ, gate, limiter, glow analysis; six route mixes; four output queue writes. No external VSTs. Actual production callback invoked offline with synthetic input. Standard run: one-second minimum per case. Heavy run: five seconds baseline and ten seconds contention per case after warmup. Queue consumption outside callback timing. No devices opened, no audio broadcast, no settings saved. Wall-clock callback cost includes scheduling preemption; over-budget calls are not measured live audio dropouts. Does not measure hardware/Windows/transport latency, driver scheduling, UI responsiveness, or another mixer.");report->setProperty("results",rows);
    folder.getChildFile(stem+".json").replaceWithText(juce::JSON::toString(juce::var(report)));folder.getChildFile(stem+".csv").replaceWithText(csv);return 0;
}
