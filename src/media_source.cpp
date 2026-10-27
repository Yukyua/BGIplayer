#include "bgiplayer/media_source.hpp"
#include "bgiplayer/host_files.hpp"
#include "bgiplayer/common.hpp"
#include <algorithm>
#include <cwchar>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace bgiplayer {
namespace {
std::optional<std::string> media_prefix(std::string_view path,int32_t index,TextEncoding encoding) {
    std::string result;
    int32_t remaining=index;
    std::size_t cursor=0;
    while(remaining>=0&&cursor<path.size()) {
        const auto ch=decode_first_character(path.substr(cursor),encoding);
        const auto width=std::max<std::size_t>(1,ch.byte_length);
        if(path[cursor]=='\\'&&cursor+1<path.size())--remaining;
        result.append(path.substr(cursor,width));cursor+=width;
        if(cursor==path.size())--remaining;
    }
    if(remaining>=0)return std::nullopt;
    if(index>0&&cursor<path.size()&&!result.empty())result.pop_back();
    return result;
}
}
std::string media_marker_prefix(std::string_view path,TextEncoding encoding) {
    int32_t count=path.empty()?0:1;
    for(std::size_t cursor=0;cursor<path.size();){
        const auto character=decode_first_character(path.substr(cursor),encoding);
        if(path[cursor]=='\\'&&cursor+1<path.size())++count;
        cursor+=std::max<std::size_t>(1,character.byte_length);
    }
    return media_prefix(path,count-2,encoding).value_or(std::string());
}
std::vector<std::filesystem::path> host_removable_volumes() {
    std::vector<std::filesystem::path> result;
#ifdef _WIN32
    const DWORD count=GetLogicalDriveStringsW(0,nullptr);
    std::vector<wchar_t> roots(count+1);
    if(!GetLogicalDriveStringsW(count,roots.data()))return result;
    for(const wchar_t* root=roots.data();*root;root+=std::wcslen(root)+1){
        const auto type=GetDriveTypeW(root);
        if(type==DRIVE_REMOVABLE||type==DRIVE_CDROM)result.emplace_back(root);
    }
#elif defined(__APPLE__)
    std::error_code error;
    for(const auto& entry:host_files::entries("/Volumes",false,error)) {
        auto path=entry.path.string();
        auto url=CFURLCreateFromFileSystemRepresentation(nullptr,
            reinterpret_cast<const UInt8*>(path.data()),static_cast<CFIndex>(path.size()),true);
        if(!url)continue;
        bool accepted=false;
        for(auto key:{kCFURLVolumeIsRemovableKey,kCFURLVolumeIsEjectableKey}){
            CFTypeRef property=nullptr;
            if(CFURLCopyResourcePropertyForKey(url,key,&property,nullptr)&&property){
                if(CFGetTypeID(property)==CFBooleanGetTypeID()&&CFBooleanGetValue(static_cast<CFBooleanRef>(property)))accepted=true;
                CFRelease(property);
            }
        }
        CFRelease(url);
        if(accepted)result.push_back(entry.path);
    }
#else
    throw Error("removable-volume host adapter is unavailable on this platform");
#endif
    return result;
}
std::optional<std::string> find_media_source(const MediaSourceRequest& request,
    const std::vector<std::filesystem::path>& volumes) {
    const auto marker=media_marker_prefix(request.path,request.encoding);
    std::string utf8;
    for(const auto code:decode_text_code_points(marker,request.encoding))utf8+=encode_code_point(code,TextEncoding::Utf8);
    for(const auto& root:volumes){
        std::error_code error;
        // Original 004A3B70 uses a literal "%s%s" concatenation. A leading
        // slash in the marker must not replace the candidate volume root.
        auto combined=root.generic_string();
        if(!combined.empty()&&combined.back()!='/')combined+='/';
        combined+=host_files::separators(utf8);
        if(host_files::exists(std::filesystem::path(combined),error)){
            auto selected=root.generic_string();
            if(!selected.empty()&&selected.back()!='/')selected+='/';
            return selected;
        }
    }
    return std::nullopt;
}
std::optional<std::string> run_media_source_probe(const MediaSourceRequest& request,
    const MediaVolumeProbe& volumes,const MediaProbeDelay& delay,const MediaProbePrompt& prompt) {
    if(!volumes)throw Error("removable-volume host provider is unavailable");
    while(true){
        for(unsigned attempt=0;attempt<10;++attempt){
            if(auto found=find_media_source(request,volumes()))return found;
            if(delay&&!delay())return std::nullopt;
        }
        if(request.prompt==0)return std::nullopt;
        if(!prompt)throw Error("removable-media prompt host provider is unavailable");
        if(prompt(false,request)!=1&&prompt(true,request)==6)return std::nullopt;
    }
}

}
