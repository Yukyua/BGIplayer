#include "bgiplayer/host_files.hpp"
#include "bgiplayer/font_discovery.hpp"
#include "bgiplayer/text.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <set>
#include <fstream>
#include <optional>
namespace bgiplayer {
namespace {
std::string family_name(const FT_SfntName& name) {
    if (name.platform_id == 0 || (name.platform_id == 3 &&
        (name.encoding_id == 0 || name.encoding_id == 1 || name.encoding_id == 10))) {
        if ((name.string_len & 1U) != 0) return {};
        std::string text;
        for (FT_UInt i=0; i<name.string_len; i+=2) {
            uint32_t c=static_cast<uint32_t>(name.string[i])*256U+name.string[i+1];
            if(c>=0xD800 && c<=0xDBFF && i+3<name.string_len){
                const uint32_t next=static_cast<uint32_t>(name.string[i+2])*256U+name.string[i+3];
                if(next<0xDC00 || next>0xDFFF)return {};
                c=((c-0xD800)<<10)+(next-0xDC00)+0x10000;i+=2;
            }
            if(c==0 || (c>=0xD800 && c<=0xDFFF))return {};
            text+=encode_code_point(c,TextEncoding::Utf8);
        }
        return text;
    }
    if(name.platform_id==1 && std::all_of(name.string,name.string+name.string_len,[](unsigned char c){return c>0 && c<128;}))
        return {reinterpret_cast<const char*>(name.string),name.string_len};
    return {};
}
}

std::vector<HostFontReplacement> read_wine_font_replacements(const std::filesystem::path& registry){
    std::vector<HostFontReplacement> result;auto in=host_files::open_read(registry,std::ios::in);std::string line;bool active=false;
    const auto quoted=[](const std::string& text,std::size_t& i)->std::optional<std::string>{
        if(i>=text.size() || text[i++]!='"')return std::nullopt;
        std::string out;
        while(i<text.size()){
            const char c=text[i++];if(c=='"')return out;
            if(c!='\\'){out+=c;continue;}
            if(i>=text.size())return std::nullopt;
            const char escape=text[i++];
            if(escape=='\\' || escape=='"'){out+=escape;continue;}
            if(escape!='x' || i+4>text.size())return std::nullopt;
            uint32_t value=0;
            for(unsigned digit=0;digit<4;++digit){const unsigned char h=static_cast<unsigned char>(text[i++]);
                const int n=h>='0' && h<='9'?h-'0':h>='a' && h<='f'?h-'a'+10:h>='A' && h<='F'?h-'A'+10:-1;
                if(n<0)return std::nullopt;value=value*16U+static_cast<uint32_t>(n);
            }
            if(value==0 || (value>=0xD800 && value<=0xDFFF))return std::nullopt;
            out+=encode_code_point(value,TextEncoding::Utf8);
        }
        return std::nullopt;
    };
    while(std::getline(in,line)){
        if(line.starts_with('[')){active=line.starts_with(R"([Software\\Wine\\Fonts\\Replacements])");continue;}
        if(!active)continue;
        std::size_t i=0;const auto requested=quoted(line,i);
        if(!requested || i>=line.size() || line[i++]!='=')continue;
        const auto family=quoted(line,i);
        if(family && !requested->empty() && !family->empty() && requested->front()!='@')result.push_back({*requested,*family,registry});
    }
    return result;
}
std::vector<HostFontReplacement> host_font_replacements(){
    std::vector<HostFontReplacement> result;
#ifdef __APPLE__
    for(const auto& root:host_font_search_roots()){
        if(root.filename()!="Fonts" || root.parent_path().filename()!="windows" || root.parent_path().parent_path().filename()!="drive_c")continue;
        const auto registry=root.parent_path().parent_path().parent_path()/"user.reg";
        auto replacements=read_wine_font_replacements(registry);result.insert(result.end(),replacements.begin(),replacements.end());
    }
#endif
    return result;
}
std::vector<HostFontFileFace> inspect_font_file_faces(const std::filesystem::path& file) {
    std::vector<HostFontFileFace> result;
    FT_Library library=nullptr;
    if(FT_Init_FreeType(&library))return result;
    FT_Face first=nullptr;
    if(FT_New_Face(library,file.string().c_str(),0,&first)){FT_Done_FreeType(library);return result;}
    const FT_Long count=std::min<FT_Long>(first->num_faces,1024);
    FT_Done_Face(first);
    for(FT_Long index=0;index<count;++index){
        FT_Face face=nullptr;if(FT_New_Face(library,file.string().c_str(),index,&face))continue;
        FT_Select_Charmap(face,FT_ENCODING_UNICODE);
        const uint32_t charset=FT_Get_Char_Index(face,0x3042)||FT_Get_Char_Index(face,0x30A2)?0x80U:
            FT_Get_Char_Index(face,0xAC00)?0x88U:FT_Get_Char_Index(face,0x4E00)?0x86U:0U;
        const bool cjk = FT_Get_Char_Index(face,0x3042) && FT_Get_Char_Index(face,0x30A2) &&
                         FT_Get_Char_Index(face,0x4E2D) && FT_Get_Char_Index(face,0x6587);
        const auto* os2=static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face,ft_sfnt_os2));
        const uint32_t weight=os2 && os2->usWeightClass?os2->usWeightClass:400U;
        std::set<std::string> names;
        if(face->family_name)names.insert(face->family_name);
        for(FT_UInt n=0;n<FT_Get_Sfnt_Name_Count(face);++n){
            FT_SfntName name{};
            if(!FT_Get_Sfnt_Name(face,n,&name) && (name.name_id==1 || name.name_id==16)){
                auto value=family_name(name);if(!value.empty())names.insert(std::move(value));
            }
        }
        for(const auto& name:names)result.push_back({name,file,index,charset,cjk,weight});
        FT_Done_Face(face);
    }
    FT_Done_FreeType(library);return result;
}
std::optional<HostFontFileFace> select_default_cjk_font(
    std::span<const HostFontFileFace> faces, std::string_view requested){
    const auto serif=[](std::string_view name){return name.find("Mincho")!=name.npos || name.find("Serif")!=name.npos ||
        name.find("明朝")!=name.npos || name.find("明體")!=name.npos || name.find("宋")!=name.npos;};
    const bool prefer_serif=serif(requested);
    const HostFontFileFace* best=nullptr;int best_score=10000;
    for(const auto& face:faces){
        if(!face.cjk_coverage)continue;
        int score=serif(face.family)==prefer_serif?0:100;
        score+=static_cast<int>((face.weight>400U?face.weight-400U:400U-face.weight)/20U);
        if(face.family==(prefer_serif?"Hiragino Mincho ProN":"Hiragino Sans"))score-=30;
        else if(face.family.find(prefer_serif?"Noto Serif CJK":"Noto Sans CJK")!=std::string::npos)score-=20;
        const auto path=face.file.generic_string();
        if(path.starts_with("/System/Library/Fonts/") || path.starts_with("/Library/Fonts/"))score-=10;
        if(face.family.starts_with('.'))score+=5;
        if(score<best_score){best=&face;best_score=score;}
    }
    if(!best)return std::nullopt;
    return *best;
}
std::vector<std::filesystem::path> host_font_search_roots(){
    std::vector<std::filesystem::path> result;
    const char* home=std::getenv("HOME");
#ifdef __APPLE__
    // CrossOver fonts need not be registered with CoreText. Read their actual
    // metadata alongside native fonts; never infer a face from its filename.
    if(home){
        const auto bottles=std::filesystem::path(home)/"Library/Application Support/CrossOver/Bottles";
        std::error_code ec;
        for(const auto& entry:host_files::entries(bottles,false,ec))
            result.push_back(entry.path/"drive_c/windows/Fonts");
    }
    result.emplace_back("/Library/Fonts");result.emplace_back("/System/Library/Fonts");
    if(home)result.emplace_back(std::filesystem::path(home)/"Library/Fonts");
#elif defined(_WIN32)
    if(const char* windows=std::getenv("WINDIR"))result.emplace_back(std::filesystem::path(windows)/"Fonts");
#else
    result.emplace_back("/usr/share/fonts");result.emplace_back("/usr/local/share/fonts");
    if(home){result.emplace_back(std::filesystem::path(home)/".local/share/fonts");result.emplace_back(std::filesystem::path(home)/".fonts");}
#endif
    if(const char* directory=std::getenv("BGIPLAYER_FONT_DIR"))result.insert(result.begin(),directory);
    return result;
}
std::vector<HostFontFileFace> discover_host_font_faces(std::span<const std::filesystem::path> roots){
    std::vector<HostFontFileFace> result;std::set<std::filesystem::path> visited;
    for(const auto& root:roots){
        std::error_code ec;std::vector<std::filesystem::path> files;
        for(const auto& entry:host_files::entries(root,true,ec)){
            if(!entry.regular)continue;
            auto ext=entry.path.extension().string();std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            if(ext==".ttf" || ext==".ttc" || ext==".otf" || ext==".otc")files.push_back(entry.path);
        }
        std::sort(files.begin(),files.end());
        for(const auto& file:files){const auto canonical=std::filesystem::weakly_canonical(file,ec);if(ec || !visited.insert(canonical).second)continue;
            auto faces=inspect_font_file_faces(file);result.insert(result.end(),faces.begin(),faces.end());}
    }
    return result;
}
}
