R"BGI(
// record_bitmap_blit integer arms. P0=mode, P1=parameter. Views are
// preclipped by the host; source and destination owners cannot alias.
#define I16(v) (int(uint(v)<<16u)>>16)
#define U16(v) (uint(v)&65535u)
#define BLEND(d,s,w) uint(CLAMP(int(d)+(((int(s)-int(d))*int(w))>>7),0,255))
if(OP==32u) {
 uint mode=P(0),parameter=P(1),ep=(mode>=0x21u&&mode<=0x27u)?256u-parameter:parameter;
 uint di=DOFF+y*DS+x*DB,si=SOFF+y*SS+x*SB;
 if(mode==255u) {
  uint color=A(si)|(A(si+1u)<<8u)|(A(si+2u)<<16u)|(A(si+3u)<<24u),value=0u;
  if(parameter==0u)value=(color&255u)|0xff000000u;
  if(parameter==1u)value=(color&65280u)|0xff000000u;
  if(parameter==2u)value=(color&0xffff0000u)|0xff000000u;
  if(parameter==3u)value=(color>>24u)*0x01010101u;
  for(uint c=0u;c<4u;++c)W(di+c,value>>(c*8u));return;
 }
 if(mode==128u) {for(uint c=0u;c<3u;++c)W(di+c,0u);W(di+3u,A(si));return;}
 if(mode==0u) {uint v=A(si)|(A(si+1u)<<8u);if(v!=0u){W(di,v);W(di+1u,v>>8u);}return;}
 if(mode==65u) {for(uint c=0u;c<DB;++c)W(di+c,0u);return;}
 if(mode==64u) {
  bool clear=false;
  if(SF==0u&&DF==0u)clear=(A(si)|A(si+1u))!=0u;
  else if(SF==1u&&(DF==1u||DF==2u))clear=(A(si)|A(si+1u)|A(si+2u))!=0u;
  else if(SF==2u&&(DF==1u||DF==2u))clear=parameter==0u?A(si+3u)==255u:A(si+3u)!=0u;
  if(clear)for(uint c=0u;c<DB;++c)W(di+c,0u);return;
 }
 if(mode==193u) {
  if(DF==0u){uint d=D(di)|(D(di+1u)<<8u),v=0u;for(uint c=0u;c<3u;++c){uint mask=c==0u?0x7c00u:(c==1u?0x3e0u:31u);v|=(((d&mask)*(256u-parameter)+mask*parameter)>>8u)&mask;}W(di,v);W(di+1u,v>>8u);}
  else {for(uint c=0u;c<3u;++c)W(di+c,BLEND(D(di+c),255u,parameter>>1u));if(DF==2u)W(di+3u,A(si+3u));}return;
 }
 if(mode==240u) {
  if(SF==0u&&DF==0u){uint a0=A(si)|(A(si+1u)<<8u),d0=D(di)|(D(di+1u)<<8u),v=0u;for(uint c=0u;c<3u;++c){uint mask=c==0u?0x7c00u:(c==1u?0x3e0u:31u);v|=((a0&mask)*(256u-parameter)+(d0&mask)*parameter)>>8u;}W(di,v);W(di+1u,v>>8u);}
  else for(uint c=0u;c<4u;++c)W(di+c,BLEND(D(di+c),A(si+c),parameter>>1u));return;
 }
 if(mode==8u||mode==9u||mode==38u||mode==39u) {
  uint alpha=A(si+3u);bool sourceThreshold=mode==9u||mode==39u;
  for(uint c=0u;c<4u;++c){int s=int(A(si+c)),d=int(D(di+c)),threshold=sourceThreshold?s:d;
   uint sign=threshold>127?65535u:0u,doubled=U16((d+s)*2),product=U16(d*s)>>7u;
   int delta=I16((doubled&sign)-(sign>>8u)+(product^sign)+(sign!=0u?1u:0u)-uint(s)),value;
   if(SF==2u)value=d+((I16(uint(delta)<<4u)*I16((alpha*ep)>>4u))>>16);
   else if(ep<256u)value=d+((I16(uint(delta)<<4u)*I16(ep<<4u))>>16);
   else value=delta;
   W(di+c,CLAMP(value,0,255));
  }return;
 }
 if(mode==7u||mode==37u) {
  uint factor=256u-((A(si+3u)*ep)>>8u);
  if(DF==1u)for(uint c=0u;c<4u;++c)W(di+c,(D(di+c)*factor)>>8u);
  else if(DF==2u)W(di+3u,(D(di+3u)*factor)>>8u);
  else W(di,(D(di)*factor)>>8u);return;
 }
 if(mode==6u||mode==36u) {
  uint factor=SF==1u?ep:((A(si+3u)>>1u)*ep)>>8u;
  for(uint c=0u;c<4u;++c){uint weighted=(A(si+c)*factor)>>8u,d=D(di+c);W(di+c,MIN(weighted+d-((weighted*d)>>8u),255u));}return;
 }
 if(mode==4u||mode==35u) {
  if(SF==1u&&DF==1u)for(uint c=0u;c<4u;++c){int a0=int(A(si+c)),d0=int(D(di+c));int term=I16((c<3u?a0-1:a0)*int(ep>>1u));W(di+c,CLAMP(a0+((term*(d0*2))>>16),0,255));}
  else if(SF==1u&&DF==2u){for(uint c=0u;c<3u;++c){int a0=int(A(si+c)),difference=int((D(di+c)*uint(a0))>>8u)-a0;W(di+c,CLAMP(a0+((difference*int(ep>>1u))>>7),0,255));}W(di+3u,A(si+3u));}
  else if(SF==2u&&DF==1u){uint factor=((A(si+3u)>>1u)*ep)>>8u;for(uint c=0u;c<4u;++c){uint d0=D(di+c),product=(A(si+c)*d0)>>8u;W(di+c,CLAMP(int(d0)+(((int(product)-int(d0))*int(factor))>>7),0,255));}}
  else if(SF==3u&&DF==3u){uint a0=A(si),d0=D(di),product=((d0*a0+1u)*257u)>>16u;W(di,CLAMP(int(d0)+(((int(product)-int(d0))*int(ep))>>8),0,255));}return;
 }
 if(mode==3u||mode==34u){uint factor=((A(si+3u)>>1u)*ep)>>8u;for(uint c=0u;c<4u;++c){uint weighted=(A(si+c)*factor)>>7u,d0=D(di+c);W(di+c,weighted<d0?d0-weighted:0u);}return;}
 if(mode==2u||mode==33u){uint a0=A(si)|(A(si+1u)<<8u);if(a0==0u)return;uint d0=D(di)|(D(di+1u)<<8u);uint r=(((a0&0x7c00u)*ep)>>8u)+(d0&0x7c00u),g=(((a0&0x3e0u)*ep)>>8u)+(d0&0x3e0u),b0=(((a0&31u)*ep)>>8u)+(d0&31u);uint v=(r>32767u?0x7c00u:r&0x7c00u)+(g>1023u?0x3e0u:g&0x3e0u)+MIN(b0,31u);W(di,v);W(di+1u,v>>8u);return;}
}
if(OP==43u) {
 uint si=SOFF+y*SS+x*SB,di=DOFF+y*DS+x*4u;bool clear=P(0)==65u;
 if(P(0)==64u) {
  if(SF==0u&&P(2)==0u)clear=(A(si)|A(si+1u))!=0u;
  else if(SF==1u&&(P(2)==1u||P(2)==2u))clear=(A(si)|A(si+1u)|A(si+2u))!=0u;
  else if(SF==2u&&(P(2)==1u||P(2)==2u))clear=P(1)==0u?A(si+3u)==255u:A(si+3u)!=0u;
 }
 if(clear)for(uint c=0u;c<4u;++c)W(di+c,0u);return;
}
if(OP==33u){uint sx=P(0)==1u?SW-1u-x:x,sy=SH-1u-y,si=SOFF+sy*SS+sx*SB,di=DOFF+y*DS+x*DB;for(uint c=0u;c<DB;++c)W(di+c,A(si+c));return;}
)BGI"
