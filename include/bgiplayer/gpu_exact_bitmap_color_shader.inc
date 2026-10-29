R"BGI(
// P5: 004043F0 ->00409210 ->00412520, format3 byte inversion.
if (OP == 34u) {
    uint d=DOFF+y*DS+x;
    W(d,D(d)^255u); return;
}
// P6: Grp2:13 core004029B0, ghproj/ghidra_grp2_10_18; exact full-word
// format2 comparison only when old color has a nonzero high byte.
else if (OP == 35u) {
    uint d=DOFF+y*DS+x*4u;
    uint pixel=D(d)|(D(d+1u)<<8u)|(D(d+2u)<<16u)|(D(d+3u)<<24u);
    if (DF==1u) {
        if ((pixel&0xffffffu)==(P(0)&0xffffffu)) pixel=P(1)&0xffffffu;
    } else if ((P(0)&0xff000000u)==0u) {
        if ((pixel&0xffffffu)==(P(0)&0xffffffu)) pixel=(pixel&0xff000000u)|(P(1)&0xffffffu);
    } else if (pixel==P(0)) pixel=P(1);
    for(uint c=0u;c<4u;++c) { W(d+c,(pixel>>(c*8u))&255u); }
    return;
}
// P7: 004043E0 ->00409190 ->004123B0, native BGRA weights28/151/77.
else if (OP == 36u) {
    uint s=SOFF+y*SS+x*4u;
    uint weighted=A(s)*28u+A(s+1u)*151u+A(s+2u)*77u;
    W(DOFF+y*DS+x,SF==1u?weighted>>8u:(weighted*A(s+3u))>>16u); return;
}
// P4: 004187B0/0040D280 ->00418960. Host-generated original factor
// table retains threshold-wrap and floating LUT construction semantics.
else if (OP == 42u) {
    uint s=SOFF+y*SS+x*4u,d=DOFF+y*DS+x*4u;
    uint encoded=P(0)==0u?P(1):P(16u+B(BOFF+y*BS+x));
    if (P(0)!=0u && encoded==0x10001u) return;
    if (P(0)!=0u && encoded==0x10000u) {
        for(uint c=0u;c<4u;++c) { W(d+c,A(s+c)); } return;
    }
    int factor=int(encoded);
    for(uint c=0u;c<4u;++c) {
        int destination=int(D(d+c)), delta=(int(A(s+c))-destination)*16;
        W(d+c,uint(CLAMP(destination+((delta*factor)>>16),0,255)));
    }
    return;
}
)BGI"
