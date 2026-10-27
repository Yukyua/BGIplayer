#include "bgiplayer/host_files.hpp"
#include "bgiplayer/engine_identity.hpp"
#include "bgiplayer/common.hpp"
#include <array>
#include <cctype>
#include <iostream>
namespace bgiplayer {
namespace {
struct PeView {
    std::span<const uint8_t> data;
    uint32_t image_base=0;size_t sections=0;uint16_t count=0;
    std::span<const uint8_t> at(uint32_t va,size_t size) const {
        if(va<image_base)return {};
        const uint32_t rva=va-image_base;
        for(size_t i=0;i<count;++i){
            const size_t p=sections+i*40;
            const uint32_t start=read_u32(data,p+12),length=read_u32(data,p+16),offset=read_u32(data,p+20);
            if(rva<start || static_cast<uint64_t>(rva-start)+size>length)continue;
            const uint64_t raw=static_cast<uint64_t>(offset)+(rva-start);
            if(raw>data.size() || size>data.size()-raw)return {};
            return data.subspan(static_cast<size_t>(raw),size);
        }
        return {};
    }
};
uint64_t read_u64(std::span<const uint8_t> data,size_t at) {
    if(at+8>data.size())return 0;
    uint64_t value=0;
    for(unsigned index=0;index<8;++index)
        value|=static_cast<uint64_t>(data[at+index])<<(index*8U);
    return value;
}
int32_t read_i32(std::span<const uint8_t> data,size_t at) {
    return static_cast<int32_t>(read_u32(data,at));
}
// 64-bit target: the engine identity is a global std::string built by a
// static constructor. The accessor (Sys0:0xE8 provider) is the byte pattern
//   cmp qword [rip+capacity],0x10
//   lea rax,[rip+storage]
//   cmovae rax,[rip+storage]
// whose capacity is storage+0x18. The static constructor is the preceding
//   lea rdx,[rip+literal]
//   lea rcx,[rip+storage]
//   call <string ctor>
// so the literal is the engine identity.
struct Pe64View {
    std::span<const uint8_t> data;
    uint64_t image_base=0;size_t sections=0;uint16_t count=0;
    std::optional<uint64_t> va_for_offset(size_t offset) const {
        for(size_t i=0;i<count;++i){
            const size_t p=sections+i*40;
            const uint32_t start=read_u32(data,p+12);
            const uint32_t length=read_u32(data,p+16);
            const uint32_t raw=read_u32(data,p+20);
            if(offset<raw || static_cast<uint64_t>(offset-raw)>=length)continue;
            return image_base+start+(offset-raw);
        }
        return std::nullopt;
    }
    std::optional<size_t> offset_for_va(uint64_t va) const {
        if(va<image_base)return std::nullopt;
        const uint64_t rva=va-image_base;
        for(size_t i=0;i<count;++i){
            const size_t p=sections+i*40;
            const uint32_t start=read_u32(data,p+12);
            const uint32_t length=read_u32(data,p+16);
            const uint32_t raw=read_u32(data,p+20);
            if(rva<start)continue;
            if(rva-start>=length)continue;
            const uint64_t offset=static_cast<uint64_t>(raw)+(rva-start);
            if(offset>=data.size())continue;
            return static_cast<size_t>(offset);
        }
        return std::nullopt;
    }
};
std::optional<std::string> pe64_engine_identifier(const Pe64View& image) {
    const auto literal_string=[&](uint64_t va) -> std::optional<std::string> {
        const auto offset=image.offset_for_va(va);
        if(!offset)return std::nullopt;
        std::string value;
        for(size_t index=*offset;index<image.data.size();++index){
            const uint8_t byte=image.data[index];
            if(byte==0)break;
            if(byte<0x20 || byte>0x7e)return std::nullopt;
            if(value.size()==128)return std::nullopt;
            value.push_back(static_cast<char>(byte));
        }
        if(value.empty())return std::nullopt;
        return value;
    };
    for(size_t at=0;at+24<=image.data.size();++at){
        if(image.data[at]!=0x48 || image.data[at+1]!=0x83 ||
           image.data[at+2]!=0x3d || image.data[at+7]!=0x10)continue;
        if(image.data[at+8]!=0x48 || image.data[at+9]!=0x8d ||
           image.data[at+10]!=0x05)continue;
        if(image.data[at+15]!=0x48 || image.data[at+16]!=0x0f ||
           image.data[at+17]!=0x43 || image.data[at+18]!=0x05)continue;
        const auto va=image.va_for_offset(at);
        if(!va)continue;
        const uint64_t capacity=*va+8U+
            static_cast<uint64_t>(static_cast<int64_t>(read_i32(image.data,at+3)));
        const uint64_t storage=*va+15U+
            static_cast<uint64_t>(static_cast<int64_t>(read_i32(image.data,at+11)));
        const uint64_t storage_again=*va+23U+
            static_cast<uint64_t>(static_cast<int64_t>(read_i32(image.data,at+19)));
        if(storage!=storage_again || capacity!=storage+0x18U)continue;
        for(size_t init=7;init+7<=image.data.size();++init){
            if(image.data[init]!=0x48 || image.data[init+1]!=0x8d ||
               image.data[init+2]!=0x0d)continue;
            const auto init_va=image.va_for_offset(init);
            if(!init_va)continue;
            const uint64_t target=*init_va+7U+
                static_cast<uint64_t>(static_cast<int64_t>(read_i32(image.data,init+3)));
            if(target!=storage)continue;
            const size_t previous=init-7U;
            const auto previous_va=image.va_for_offset(previous);
            if(!previous_va)continue;
            if(image.data[previous]!=0x48 || image.data[previous+1]!=0x8d ||
               image.data[previous+2]!=0x15)continue;
            const uint64_t literal=*previous_va+7U+
                static_cast<uint64_t>(static_cast<int64_t>(read_i32(image.data,previous+3)));
            if(auto identifier=literal_string(literal))
                return identifier;
        }
    }
    return std::nullopt;
}
}
std::optional<std::string> engine_identifier_from_pe(std::span<const uint8_t> data) {
    if(data.size()<64 || data[0]!='M' || data[1]!='Z')return {};
    const uint32_t pe=read_u32(data,60);
    if(pe>data.size() || data.size()-pe<84)return {};
    if(read_u32(data,pe)!=0x4550)return {};
    const uint16_t machine=read_u16(data,pe+4);
    const uint16_t magic=read_u16(data,pe+24);
    if(machine==0x8664 && magic==0x20b){
        const uint16_t optional64=read_u16(data,pe+20),count64=read_u16(data,pe+6);
        if(optional64<60 || optional64>data.size()-pe-24)return {};
        const size_t sections64=static_cast<size_t>(pe)+24+optional64;
        if(count64>(data.size()-sections64)/40)return {};
        const Pe64View image{data,read_u64(data,pe+48),sections64,count64};
        return pe64_engine_identifier(image);
    }
    if(machine!=0x14c || magic!=0x10b)return {};
    const uint16_t optional=read_u16(data,pe+20),count=read_u16(data,pe+6);
    if(optional<60 || optional>data.size()-pe-24)return {};
    const size_t sections=static_cast<size_t>(pe)+24+optional;
    if(count>(data.size()-sections)/40)return {};
    const PeView image{data,read_u32(data,pe+52),sections,count};
    struct Profile {uint32_t wrapper,reader,copy;};
    std::optional<std::string> result;
    for(const Profile profile:std::array<Profile,2>{{{0x496130,0x499b20,0x4a1520},{0x499550,0x49d1a0,0x4a4dd0}}}){
        const auto code=image.at(profile.wrapper,29);
        if(code.empty())continue;
        constexpr std::array<uint8_t,7> prefix{0x55,0x8b,0xec,0x8b,0x4d,0x08,0xe8};
        if(!std::equal(prefix.begin(),prefix.end(),code.begin()) || code[11]!=0x68 || code[16]!=0x50 || code[17]!=0xe8)continue;
        constexpr std::array<uint8_t,7> suffix{0x83,0xc4,0x08,0x33,0xc0,0x5d,0xc3};
        if(!std::equal(suffix.begin(),suffix.end(),code.begin()+22))continue;
        if(profile.wrapper+11+read_u32(code,7)!=profile.reader || profile.wrapper+22+read_u32(code,18)!=profile.copy)continue;
        const uint32_t literal=read_u32(code,12);std::string identifier;
        for(uint32_t i=0;i<128;++i){const auto byte=image.at(literal+i,1);if(byte.empty())return {};if(byte[0]==0)break;if(byte[0]<0x20 || byte[0]>0x7e)return {};identifier.push_back(static_cast<char>(byte[0]));}
        if(identifier.empty() || identifier.size()==128)return {};
        if(result && *result!=identifier)throw Error("conflicting engine identifier wrappers");
        result=std::move(identifier);
    }
    if(result)return result;
    // Relocated PE32 builds retain the Sys0 dispatch-table ABI. Identify the
    // E8 entry through the byte dispatcher, not a string/file-name guess.
    // Yubisaki FD2: 00498AA1 / table00502418; older compiler KanojoStep:
    // 00491B4E / table00509300, using EAX index and ECX function pointer.
    for(size_t at=0;at+17<=data.size();++at) {
        constexpr std::array<uint8_t,6> modern_dispatch{0x0f,0xb6,0xc8,0x8b,0x04,0x8d};
        constexpr std::array<uint8_t,6> old_dispatch{0x0f,0xb6,0xc0,0x8b,0x0c,0x85};
        const auto current=data.subspan(at);
        const bool modern=std::equal(modern_dispatch.begin(),modern_dispatch.end(),current.begin());
        const bool older=std::equal(old_dispatch.begin(),old_dispatch.end(),current.begin());
        if(!modern && !older)continue;
        if(modern) {
            if(current[10]!=0x85 || current[11]!=0xc0 || current[12]!=0x74 ||
               current[14]!=0x56 || current[15]!=0xff || current[16]!=0xd0)continue;
        } else {
            // JNE skips the error arm and reaches push thread; call ECX.
            if(current[10]!=0x85 || current[11]!=0xc9 || current[12]!=0x75)continue;
            const size_t target=14U+current[13];
            if(target+3>current.size() || current[target]!=0x56 ||
               current[target+1]!=0xff || current[target+2]!=0xd1)continue;
        }
        const uint32_t table=read_u32(current,6);
        if(table>UINT32_MAX-0xe8U*4U)continue;
        const auto slot=image.at(table+0xe8U*4U,4);if(slot.empty())continue;
        const uint32_t wrapper=read_u32(slot,0);
        const auto code=image.at(wrapper,29);if(code.empty())continue;
        constexpr std::array<uint8_t,7> prefix{0x55,0x8b,0xec,0x8b,0x4d,0x08,0xe8};
        if(!std::equal(prefix.begin(),prefix.end(),code.begin()))continue;
        const uint32_t reader=wrapper+11+read_u32(code,7);
        const auto read_code=image.at(reader,17);if(read_code.empty())continue;
        // Both verified reader ABIs pop one operand and resolve its VM pointer.
        const bool modern_reader=read_code[0]==0x51 && read_code[1]==0xe8 &&
            read_code[6]==0x8b && read_code[7]==0xd1 && read_code[8]==0x8b &&
            read_code[9]==0xc8 && read_code[10]==0xe8 &&
            read_code[15]==0x59 && read_code[16]==0xc3;
        const bool old_reader=read_code[0]==0x51 && read_code[1]==0xe8 &&
            read_code[6]==0x8b && read_code[7]==0xd0 && read_code[8]==0xe8 &&
            read_code[13]==0x83 && read_code[14]==0xc4 && read_code[15]==4 && read_code[16]==0xc3;
        if(!modern_reader && !old_reader)continue;
        uint32_t literal=0;uint32_t limit=128;
        constexpr std::array<uint8_t,7> suffix{0x83,0xc4,0x08,0x33,0xc0,0x5d,0xc3};
        if(code[11]==0x68 && code[16]==0x50 && code[17]==0xe8 &&
           std::equal(suffix.begin(),suffix.end(),code.begin()+22)) {
            const uint32_t copy=wrapper+22+read_u32(code,18);
            if(image.at(copy,1).empty())continue;
            literal=read_u32(code,12);
        } else {
            // 004912B0: compiler-inlined constant copy, DWORD+DWORD+WORD+BYTE.
            // Check every source address and destination offset before reading
            // the literal. This remains relocation/name independent.
            const auto copy=image.at(wrapper,52);if(copy.empty() || !old_reader)continue;
            constexpr std::array<uint8_t,52> pattern{
                0x55,0x8b,0xec,0x8b,0x4d,8,0xe8,0,0,0,0,
                0x8b,0x0d,0,0,0,0,0x89,0x08,
                0x8b,0x15,0,0,0,0,0x89,0x50,4,
                0x66,0x8b,0x0d,0,0,0,0,0x66,0x89,0x48,8,
                0x8a,0x15,0,0,0,0,0x88,0x50,10,0x33,0xc0,0x5d,0xc3};
            bool matches=true;
            for(size_t index=11;index<pattern.size();++index) {
                if((index>=13 && index<17) || (index>=21 && index<25) ||
                   (index>=31 && index<35) || (index>=41 && index<45))continue;
                if(copy[index]!=pattern[index]){matches=false;break;}
            }
            if(!matches)continue;
            literal=read_u32(copy,13);limit=11;
            if(literal>UINT32_MAX-10U || read_u32(copy,21)!=literal+4U ||
               read_u32(copy,31)!=literal+8U || read_u32(copy,41)!=literal+10U)continue;
        }
        std::string identifier;bool terminated=false;
        for(uint32_t i=0;i<limit;++i) {
            if(literal>UINT32_MAX-i)break;
            const auto byte=image.at(literal+i,1);
            if(byte.empty() || (byte[0]!=0 && (byte[0]<0x20 || byte[0]>0x7e)))break;
            if(byte[0]==0){terminated=true;break;}
            identifier.push_back(static_cast<char>(byte[0]));
        }
        if(identifier.empty() || !terminated)continue;
        if(result && *result!=identifier)throw Error("conflicting engine identifier wrappers");
        result=std::move(identifier);
    }
    return result;
}
std::optional<std::string> discover_engine_identifier(const std::filesystem::path& game) {
    std::optional<std::string> result;
    std::error_code error;
    const auto entries = host_files::entries(game, false, error);
    if (error) throw std::filesystem::filesystem_error("cannot enumerate engine directory", game, error);
    for(const auto& entry:entries){
        if(!entry.regular)continue;
        auto extension=entry.path.extension().string();auto name=entry.path.filename().string();
        const auto lower=[](unsigned char ch){return static_cast<char>(std::tolower(ch));};
        std::transform(extension.begin(),extension.end(),extension.begin(),lower);
        std::transform(name.begin(),name.end(),name.begin(),lower);
        if(extension!=".exe" || name=="cinderella_cn.exe")continue;
        const auto id=engine_identifier_from_pe(read_file(entry.path));if(!id)continue;
        if(result && *result!=*id)throw Error("conflicting supported engine executables in game directory");
        result=*id;
        if (debug_enabled()) std::cout<<"ENGINE_IDENTITY source="<<entry.path<<" identifier="<<*id<<'\n';
    }
    return result;
}
std::optional<bool> engine_is_64bit_from_pe(std::span<const uint8_t> data) {
    if(data.size()<64 || data[0]!='M' || data[1]!='Z')return {};
    const uint32_t pe=read_u32(data,60);
    if(pe>data.size() || data.size()-pe<24)return {};
    if(read_u32(data,pe)!=0x4550)return {};
    const uint16_t machine=read_u16(data,pe+4);
    if(machine==0x8664)return true;
    if(machine==0x014c)return false;
    return {};
}
std::optional<uint32_t> engine_effector_mode_limit_from_pe(std::span<const uint8_t> data) {
    const auto wide=engine_is_64bit_from_pe(data);
    if(!wide || !engine_identifier_from_pe(data))return {};
    if(*wide)return 5U; // Verified PE64 configure supports selectors0..5.
    const size_t pe=read_u32(data,60),optional=read_u16(data,pe+20),sections=pe+24+optional;
    const uint16_t count=read_u16(data,pe+6);
    if(optional<60 || sections>data.size() || count>(data.size()-sections)/40)return {};
    const PeView image{data,read_u32(data,pe+52),sections,count};
    std::optional<uint32_t> result;
    for(size_t section=0;section<count;++section) {
        const size_t entry=sections+section*40;
        if(!(read_u32(data,entry+36)&0x20000000U))continue;
        const size_t raw=read_u32(data,entry+20),length=read_u32(data,entry+16);
        if(raw>data.size() || length>data.size()-raw)continue;
        for(size_t i=0;i+17<=length;++i) {
            const auto dispatch=data.subspan(raw+i);
            constexpr uint8_t prefix[]={0x0f,0xb6,0xc8,0x8b,0x04,0x8d};
            if(!std::equal(std::begin(prefix),std::end(prefix),dispatch.begin()))continue;
            if(dispatch[10]!=0x85 || dispatch[11]!=0xc0 || dispatch[12]!=0x74 ||
               dispatch[14]!=0x56 || dispatch[15]!=0xff || dispatch[16]!=0xd0)continue;
            const uint32_t table=read_u32(dispatch,6);
            if(table>UINT32_MAX-0x66U*4U)continue;
            const auto slot=image.at(table+0x66U*4U,4);if(slot.empty())continue;
            const uint32_t wrapper=read_u32(slot,0);
            const auto code=image.at(wrapper,0x66);if(code.empty())continue;
            constexpr uint8_t wrapper_prefix[]={0x55,0x8b,0xec,0x81,0xec,0x0c,0x01,0,0};
            if(!std::equal(std::begin(wrapper_prefix),std::end(wrapper_prefix),code.begin()) ||
               code[0x1f]!=0xe8 || code[0x26]!=0xe8 || code[0x2d]!=0xe8 ||
               code[0x38]!=0xe8 || code[0x61]!=0xe8)continue;
            const uint32_t shim_va=wrapper+0x66+read_u32(code,0x62);
            const auto shim=image.at(shim_va,18);if(shim.empty())continue;
            constexpr uint8_t shim_prefix[]={0x55,0x8b,0xec,0xff,0x75,0x0c,0xff,0x75,0x08,0x52,0x51,0xe8};
            if(!std::equal(std::begin(shim_prefix),std::end(shim_prefix),shim.begin()) || shim[16]!=0x5d || shim[17]!=0xc3)continue;
            const uint32_t manager_va=shim_va+16+read_u32(shim,12);
            const auto manager=image.at(manager_va,0x35);if(manager.empty())continue;
            constexpr uint8_t manager_prefix[]={0x55,0x8b,0xec,0x51,0x56,0x57,0x8b,0x3d};
            if(!std::equal(std::begin(manager_prefix),std::end(manager_prefix),manager.begin()) || manager[0x27]!=0xe8 || read_u32(manager,0x31)!=0x80000005U)continue;
            const auto core=image.at(manager_va+0x2c+read_u32(manager,0x28),0x4b);if(core.empty())continue;
            constexpr uint8_t core_prefix[]={0x55,0x8b,0xec,0x56,0x57,0x8b,0x7d,0x08,0x8b,0xf1,0x83,0xff};
            constexpr uint8_t core_body[]={0x8b,0x06,0x8b,0xce,0xff,0x75,0x0c,0xc7,0x86,0x44,0x01,0,0,0x01,0,0,0,0x89,0xbe,0x74,0x01,0,0,0xff,0x50,0x48,0x8b,0x06,0x8b,0xce,0xff,0x75,0x10,0xff,0x50,0x54,0x5f,0x33,0xc0,0x5e,0x5d,0xc2,0x0c,0,0x5f,0xb8,0x05,0,0,0x80,0x5e,0x5d,0xc2,0x0c,0};
            if(!std::equal(std::begin(core_prefix),std::end(core_prefix),core.begin()) ||
               (core[12]!=1 && core[12]!=5) || core[13]!=0x77 || core[14]!=0x31 || core[15]!=0xe8 ||
               !std::equal(std::begin(core_body),std::end(core_body),core.begin()+20))continue;
            const uint32_t limit=core[12];
            if(result && *result!=limit)throw Error("conflicting effector mode capabilities");
            result=limit;
        }
    }
    return result;
}
std::optional<uint32_t> discover_effector_mode_limit(const std::filesystem::path& game) {
    std::error_code error;std::optional<uint32_t> result;
    const auto files=host_files::entries(game,false,error);
    if(error)throw std::filesystem::filesystem_error("cannot enumerate engine directory",game,error);
    for(const auto& file:files) {
        if(!file.regular)continue;
        auto extension=file.path.extension().string(),name=file.path.filename().string();
        const auto lower=[](unsigned char c){return static_cast<char>(std::tolower(c));};
        std::transform(extension.begin(),extension.end(),extension.begin(),lower);
        std::transform(name.begin(),name.end(),name.begin(),lower);
        if(extension!=".exe" || name=="cinderella_cn.exe")continue;
        const auto limit=engine_effector_mode_limit_from_pe(host_files::read(file.path));if(!limit)continue;
        if(result && *result!=*limit)throw Error("conflicting engine effector mode limits");
        result=*limit;
    }
    return result;
}
std::optional<bool> discover_engine_64bit(const std::filesystem::path& game) {
    std::optional<bool> result;
    std::error_code error;
    const auto entries = host_files::entries(game, false, error);
    if (error) throw std::filesystem::filesystem_error("cannot enumerate engine directory", game, error);
    for(const auto& entry:entries){
        if(!entry.regular)continue;
        auto extension=entry.path.extension().string();auto name=entry.path.filename().string();
        const auto lower=[](unsigned char ch){return static_cast<char>(std::tolower(ch));};
        std::transform(extension.begin(),extension.end(),extension.begin(),lower);
        std::transform(name.begin(),name.end(),name.begin(),lower);
        if(extension!=".exe" || name=="cinderella_cn.exe")continue;
        const auto wide=engine_is_64bit_from_pe(read_file(entry.path));if(!wide)continue;
        if(result && *result!=*wide)throw Error("conflicting engine image types in game directory");
        result=*wide;
        if (debug_enabled()) std::cout<<"ENGINE_IMAGE source="<<entry.path
                 <<" machine="<<(*wide?"x86-64":"i386")<<'\n';
    }
    return result;
}
}
