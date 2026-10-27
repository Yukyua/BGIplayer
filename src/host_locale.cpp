#include "bgiplayer/host_locale.hpp"
#include "bgiplayer/common.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
namespace bgiplayer {
uint32_t host_default_ui_language() {
#ifdef _WIN32
    return GetUserDefaultUILanguage();
#elif defined(__APPLE__)
    CFArrayRef languages=CFLocaleCopyPreferredLanguages();
    uint32_t result=0;
    if (languages && CFArrayGetCount(languages)>0) {
        auto preferred=static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages,0));
        // Apple preferred languages use BCP 47 separators (zh-Hans-CN),
        // whereas this CoreFoundation Windows-code API expects locale IDs
        // (zh_Hans_CN). Canonicalization alone does not change separators.
        auto locale_id=CFStringCreateMutableCopy(nullptr,0,preferred);
        if(locale_id){
            CFStringFindAndReplace(locale_id,CFSTR("-"),CFSTR("_"),
                CFRangeMake(0,CFStringGetLength(locale_id)),0);
            auto canonical=CFLocaleCreateCanonicalLocaleIdentifierFromString(nullptr,locale_id);
            if(canonical){
                result=CFLocaleGetWindowsLocaleCodeFromLocaleIdentifier(canonical)&0xFFFFU;
                CFRelease(canonical);
            }
            CFRelease(locale_id);
        }
    }
    if(languages)CFRelease(languages);
    if(result==0)throw Error("host UI language has no Windows LANGID mapping");
    return result;
#else
    throw Error("host UI language adapter is unavailable on this platform");
#endif
}
}
