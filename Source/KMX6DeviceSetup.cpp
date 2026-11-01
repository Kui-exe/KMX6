#include <usbip/vhci.h>
#include <usbip/persistent.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <string>
using Device=usbip::persistent_device;
static int kmxIndex(const usbip::device_location& location){
    if(location.hostname!="127.0.0.1"||location.service!="3241"||location.busid.size()!=3||location.busid.substr(0,2)!="1-")return -1;
    return location.busid[2]>='1'&&location.busid[2]<='6'?location.busid[2]-'1':-1;
}
static std::vector<Device> merged(std::vector<Device> saved){
    saved.erase(std::remove_if(saved.begin(),saved.end(),[](const auto& d){return kmxIndex(d.location)>=0;}),saved.end());
    for(int i=1;i<=6;++i){Device device;device.location={"127.0.0.1","3241","1-"+std::to_string(i)};device.serial="KMX600"+std::to_string(i);saved.push_back(device);}return saved;
}
int main(int argc,char** argv){
    const std::string mode=argc>1?argv[1]:"--check";
    if(mode=="--self-test"){
        Device unrelated;unrelated.location={"other-host","3240","1-1"};unrelated.serial="preserve";
        Device old;old.location={"127.0.0.1","3241","1-1"};old.serial="old";
        auto devices=merged({unrelated,old,old});auto again=merged(devices);
        if(devices.size()!=7||again.size()!=7||again[0].serial!="preserve")return 1;
        for(int i=0;i<6;++i)if(kmxIndex(again[i+1].location)!=i||again[i+1].once||again[i+1].serial!="KMX600"+std::to_string(i+1))return 1;
        std::cout<<"PASS preserves unrelated devices, replaces duplicate KMX6 records, saves all six stable identities, and is idempotent\n";return 0;
    }
    auto driver=usbip::vhci::open();if(driver.get()==INVALID_HANDLE_VALUE){std::cerr<<"Driver unavailable: "<<GetLastError()<<"\n";return 1;}
    auto saved=usbip::vhci::get_persistent(driver.get());if(!saved){std::cerr<<"Cannot read saved devices: "<<GetLastError()<<"\n";return 1;}
    if(mode=="--persist"){
        auto attached=usbip::vhci::get_imported_devices(driver.get());if(!attached)return 1;
        std::array<bool,6> valid{};
        for(const auto& d:*attached){int i=kmxIndex(d.location);if(i>=0&&d.serial=="KMX600"+std::to_string(i+1)&&d.vendor==0xffff&&d.product==0xcb01+i)valid[i]=true;}
        if(std::find(valid.begin(),valid.end(),false)!=valid.end()){std::cerr<<"All six matching KMX6 devices must be attached before saving.\n";return 1;}
        auto devices=merged(*saved);if(!usbip::vhci::set_persistent(driver.get(),devices)){std::cerr<<"Cannot save devices: "<<GetLastError()<<"\n";return 1;}
        saved=usbip::vhci::get_persistent(driver.get());if(!saved)return 1;
    }else if(mode!="--check")return 2;
    std::array<bool,6> found{};for(const auto& d:*saved){int i=kmxIndex(d.location);if(i>=0&&d.serial=="KMX600"+std::to_string(i+1))found[i]=true;}
    int count=int(std::count(found.begin(),found.end(),true));std::cout<<count<<"/6 KMX6 automatic attachments saved; "<<saved->size()-count<<" other records preserved\n";return count==6?0:1;
}
