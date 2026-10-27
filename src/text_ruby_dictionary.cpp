#include "bgiplayer/text_layout.hpp"
#include <algorithm>

namespace bgiplayer {
std::optional<std::vector<TextRubyAlias>> parse_text_ruby_dictionary(std::u16string_view text) {
    text=text.substr(0,text.find(char16_t(0)));
    std::vector<TextRubyAlias> aliases;
    // 1400786F0 splits at the encoding-aware backslash and then newline.
    // Decode before searching so a CP932 trail byte 5C is never a delimiter.
    while(!text.empty() && text.front()!=0) {
        const auto delimiter=text.find(u'\\');
        if(delimiter==text.npos || delimiter==0)return std::nullopt;
        const auto next=text.find(u'\n',delimiter+1U);
        const auto reading=text.substr(delimiter+1U,
            next==text.npos?text.npos:next-delimiter-1U);
        if(reading.empty())return std::nullopt;
        TextRubyAlias entry{std::u16string(text.substr(0,delimiter)),std::u16string(reading)};
        const auto duplicate=std::find_if(aliases.begin(),aliases.end(),[&](const auto& item){
            return item.base==entry.base;
        });
        if(duplicate!=aliases.end())duplicate->reading=std::move(entry.reading);
        else aliases.push_back(std::move(entry));
        if(next==text.npos)break;
        text.remove_prefix(next+1U);
    }
    // 140078C70 inserts longer base strings before shorter prefixes. The
    // original compares encoded byte counts; Unicode length has the same
    // ordering whenever two candidates can both match one input prefix.
    std::stable_sort(aliases.begin(),aliases.end(),[](const auto& a,const auto& b){
        return a.base.size()>b.base.size();
    });
    return aliases;
}
char16_t tag_lower(char16_t c){return c>='A'&&c<='Z'?char16_t(c+('a'-'A')):c;}
bool closing_ruby(std::u16string_view tag){return tag.size()==2 && tag[0]=='/' && tag_lower(tag[1])=='r';}
// 140075D25 lowercases tag names; 140076360..76430 registers the
// explicit reading for the following plain base run, and /r is a parser
// no-op. The separate 14007943D gate decides whether auxiliary nodes are
// emitted afterwards, so ruby-disabled calls still parse these delimiters.
std::optional<ParsedRubyText> parse_text_ruby(std::u16string_view text,
    std::span<const TextRubyAlias> aliases) {
    ParsedRubyText parsed;
    for(size_t i=0;i<text.size() && text[i];) {
        if(text[i]!='<') {
            const auto alias=std::find_if(aliases.begin(),aliases.end(),[&](const auto& candidate) {
                return !candidate.base.empty() && text.substr(i).starts_with(candidate.base);
            });
            if(alias!=aliases.end()) {
                const auto first=parsed.visible.size();
                parsed.boundaries.push_back(first);
                parsed.visible.append(alias->base);
                parsed.boundaries.push_back(parsed.visible.size());
                parsed.ruby.push_back({first,alias->base.size(),alias->reading});
                i+=alias->base.size();
            } else parsed.visible.push_back(text[i++]);
            continue;
        }
        const auto end=text.find('>',i+1);
        if(end==text.npos){parsed.visible.append(text.substr(i));break;}
        if(end==i+1){parsed.visible.append(text.substr(i,2));i=end+1;continue;}
        const auto tag=text.substr(i+1,end-i-1);
        if(tag.size()<2 || tag_lower(tag[0])!='r')return std::nullopt;
        auto reading=tag.substr(1);
        while(!reading.empty() && reading.front()==' ')reading.remove_prefix(1);
        const auto next=text.find('<',end+1);
        if(reading.empty() || next==text.npos || next==end+1)return std::nullopt;
        const auto close=text.find('>',next+1);
        if(close==text.npos || !closing_ruby(text.substr(next+1,close-next-1)))return std::nullopt;
        const auto base=text.substr(end+1,next-end-1);
        const auto first=parsed.visible.size();
        parsed.boundaries.push_back(first);
        parsed.visible.append(base);
        parsed.boundaries.push_back(parsed.visible.size());
        std::u16string lowered(reading);
        std::transform(lowered.begin(),lowered.end(),lowered.begin(),tag_lower);
        parsed.ruby.push_back({first,base.size(),std::move(lowered)});
        i=close+1;
    }
    return parsed;
}
} // namespace bgiplayer
