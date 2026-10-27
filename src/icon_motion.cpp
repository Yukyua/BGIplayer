#include "bgiplayer/vm.hpp"
#include "bgiplayer/display_recorded_curve.hpp"
#include "bgiplayer/display_easing.hpp"
#include "bgiplayer/rcpss.hpp"
#include <cmath>
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
namespace bgiplayer {
uint32_t VM::icon_motion_render_priority(const IconInputIconRecord& icon, uint32_t sequence) {
    // PE64 68460 creates class-2 sprites with default priority mode 0.
    // 56810 packs the accepted layer and signed wrapped Q16 depth.
    uint32_t layer = icon_input_priority(icon, sequence) - 0x8000U;
    if (layer >= 0x10000U) layer = 0;
    const int32_t depth = static_cast<int32_t>(icon.render_depth << 16U) >> 19;
    return (layer << 16U) + 0x4000U + ((0xFFFU - static_cast<uint32_t>(depth)) & 0x1FFFU);
}

uint32_t VM::configure_icon_motion(uint32_t id,const std::array<uint32_t,10>& a,uint32_t base) {
 auto p=icon_input_processors_.find(id);if(p==icon_input_processors_.end())return 1;
 auto& processor=p->second;if(processor.mode!=2)return 4;
 if(a[0]>=processor.layout.groups.size())return 2;
 auto& group=processor.layout.groups[a[0]];
 if(a[1]>=group.icons.size() || !group.icons[a[1]].runtime_present)return 3;
 if(a[2]<2)return 5;
 if(a[8]<2)return 8;
 try {
 IconMotion motion;motion.group=a[0];motion.icon=a[1];motion.state=a[9]!=0;
 motion.effect_first=a[4];motion.effect_last=a[5];motion.reverse_easing=a[6];
 motion.forward_easing=a[7];motion.duration=a[8];
 if(uint64_t(a[2])*16U>UINT32_MAX || uint64_t(a[3])+uint64_t(a[2])*16U>uint64_t(UINT32_MAX)+1U)
     throw BpFault("mode2 icon path source span overflows VM address space");
 motion.points.reserve(std::min(a[2],100U));
 for(uint32_t i=0;i<a[2];++i) {
   std::array<uint32_t,3> point{};
   for(uint32_t axis=0;axis<3;++axis)point[axis]=memory_.read_u32(a[3]+i*16+axis*4,base)<<16;
   if(i<100)motion.points.push_back(point);
   if(i+1==a[2])motion.final_point=point;
 }
 const auto initial=a[9]?motion.points.front():motion.final_point;
 auto& icon=group.icons[a[1]];
 set_icon_motion_transform(processor,icon,static_cast<int32_t>(initial[0])>>16,
   static_cast<int32_t>(initial[1])>>16,static_cast<int32_t>(initial[2])>>16,motion.effect_first);
 icon.runtime_enabled=(icon.words[48]&0x10000U)?a[9]:1U;
 processor.motions[static_cast<int32_t>((a[0]<<16)|a[1])]=std::move(motion);
 refresh_icon_input_regions(id);return 0;
 } catch(const std::bad_alloc&) { return 7; }
}
void VM::set_icon_motion_transform(IconInputProcessor& processor,IconInputIconRecord& icon,
 int32_t x,int32_t y,int32_t z,uint32_t effect) {
 if(!icon.runtime_present)return; // 913A0 detail+4: no live child, no transform.
 // 68320 -> 6FF80 removes and reinserts after existing equal keys.
 if(auto d=display_object_states_.find(icon.display_object_id);d!=display_object_states_.end())
   d->second.sequence=next_display_sequence_++;
 icon.words[2]=static_cast<uint32_t>(x);icon.words[3]=static_cast<uint32_t>(y);
 icon.render_x=static_cast<int32_t>(uint32_t(x)+icon.words[4]);
 icon.render_y=static_cast<int32_t>(uint32_t(y)+icon.words[5]);
 icon.render_depth=static_cast<uint32_t>(z);icon.render_effect=effect;
 refresh_icon_input_regions(processor.id);display_dirty_=true;
}
bool VM::transition_icon_motion(IconInputProcessor& processor,uint32_t group,uint32_t index,uint32_t flag,uint32_t delay,uint64_t now) {
 if(group>=processor.layout.groups.size() || index>=processor.layout.groups[group].icons.size())return false;
 auto f=processor.motions.find(static_cast<int32_t>((group<<16)|index));if(f==processor.motions.end())return false;
 auto& m=f->second;auto& icon=processor.layout.groups[group].icons[index];
 if(flag) {
   if(m.state==0) {if(delay)m.delay=delay;m.started=now;m.state=2;}
   else if(m.state==3) {
     if(m.delay) {m.state=1;m.delay=0;icon.runtime_enabled=1;}
     else {m.state=2;m.started=now*2-m.duration-m.started;}
   } else if(m.state==2 && m.delay)m.delay=delay;
 } else {
   icon.runtime_enabled=~(icon.words[48]>>16)&1U;
   if(m.state==1) {if(delay)m.delay=delay;m.started=now;m.state=3;}
   else if(m.state==2) {
     if(m.delay) {m.delay=0;m.state=0;}
     else {m.state=3;m.started=now*2-m.duration-m.started;}
   } else if(m.state==3 && m.delay)m.delay=delay;
 }
 refresh_icon_input_regions(processor.id);return true;
}
void VM::advance_icon_motions(IconInputProcessor& processor,uint64_t now) {
 for(auto& [key,m]:processor.motions) {
   (void)key;if(m.state!=2 && m.state!=3)continue;
   uint32_t elapsed=static_cast<uint32_t>(now)-static_cast<uint32_t>(m.started);
   if(m.delay) {
     if(m.delay>elapsed)continue;
     m.started+=m.delay;elapsed-=m.delay;m.delay=0;
   }
   elapsed=std::min(elapsed,m.duration);
   const float duration=static_cast<float>(m.duration),e=detail::rcpss_estimate(duration);
   const float square=e*e,twice=e+e,error=duration*square,refined=twice-error;
   const float ticks=static_cast<float>(elapsed)*16777216.0F;
   const float product=refined*ticks;
   const int32_t phase=static_cast<int32_t>(std::floor(product+0.5F));
   const auto factor=evaluate_display_easing(phase,m.state==2?m.reverse_easing:m.forward_easing);
   if(!factor)throw BpFault("mode2 icon motion easing selector is unresolved");
   const uint32_t forward=static_cast<uint32_t>(m.duration*static_cast<uint32_t>(*factor))>>16;
   const uint32_t sample=m.state==2?m.duration-forward:forward;
   // F7C60 rejects sample >= configured duration before entering the curve.
   const auto position=sample>=m.duration ? m.final_point
       : evaluate_recorded_display_curve(m.points,sample,m.duration).value_or(m.final_point);
   const uint32_t from=m.state==2?m.effect_last:m.effect_first;
   const uint32_t to=m.state==2?m.effect_first:m.effect_last;
   const uint32_t effect=from+(((to-from)*static_cast<uint32_t>(*factor))>>16);
   if(m.group<processor.layout.groups.size() && m.icon<processor.layout.groups[m.group].icons.size()) {
     auto& icon=processor.layout.groups[m.group].icons[m.icon];
     const auto rounded=[](uint32_t v){return static_cast<int32_t>(v+0x8000U)>>16;};
     set_icon_motion_transform(processor,icon,rounded(position[0]),rounded(position[1]),rounded(position[2]),effect);
     if(elapsed==m.duration) {m.state=m.state==2?1U:0U;m.started=0;if(m.state==1)icon.runtime_enabled=1;refresh_icon_input_regions(processor.id);}
   }
 }
}
}
