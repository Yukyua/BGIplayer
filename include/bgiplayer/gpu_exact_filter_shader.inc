R"BGI(
// F1: PE32 00411B30/00411B80; PE64 140046F30/140046E00.
// Full color word zero plus strength zero selects raw copy (bit15 retained).
if (OP == 1u) {
    uint s=SOFF+y*SS+x*2u, d=DOFF+y*DS+x*2u;
    uint v=A(s)|(A(s+1u)<<8u), color=P(0), weight=P(1), result=v;
    if (color!=0u || weight!=0u) {
        uint red=(color>>9u)&0x7c00u, green=(color>>6u)&0x3e0u, blue=(color>>3u)&31u;
        result=((((v&0x7c00u)*(256u-weight)+red*weight)>>8u)&0x7c00u)|
            ((((v&0x3e0u)*(256u-weight)+green*weight)>>8u)&0x3e0u)|
            (((v&31u)*(256u-weight)+blue*weight)>>8u);
    }
    W(d,result&255u); W(d+1u,result>>8u); return;
}
// F2: 00412130/00412150 and 1400461E0/1400460C0. Shift/add wrap at32 bits.
else if (OP == 2u) {
    uint d=DOFF+y*DS+x*4u, shift=P(2)&31u, weight=P(1);
    int parameter=int((A(SOFF+y*SS+x)<<shift)+256u-(weight<<shift)-weight);
    if (parameter>=256) return;
    for (uint c=0u;c<3u;++c) {
        int tint=int((P(0)>>(c*8u))&255u);
        int value=parameter<=0?tint:tint+((int(D(d+c))-tint)*(parameter>>1)>>7);
        W(d+c,uint(CLAMP(value,0,255)));
    }
    W(d+3u,0u); return;
}
// F3: complete-color-zero subtraction00412010/140046310, addition00411EE0/140046450.
else if (OP == 3u) {
    uint s=SOFF+y*SS+x*4u, d=DOFF+y*DS+x*4u;
    bool subtract=P(0)==0u; uint tint=subtract?0xffffffu:P(0);
    for (uint c=0u;c<3u;++c) {
        int delta=int((((tint>>(c*8u))&255u)*P(1))>>8u);
        W(d+c,uint(CLAMP(int(A(s+c))+(subtract?-delta:delta),0,255)));
    }
    W(d+3u,A(s+3u)); return;
}
// F4/F5: luminance140046950 and XOR004114D0/140046C90.
// Preserve the original two-pixel transparent shortcut, including odd tails.
else if (OP == 4u || OP == 5u) {
    uint s=SOFF+y*SS+x*4u, d=DOFF+y*DS+x*4u, pair=x&~1u;
    bool zero=SF==2u && A(SOFF+y*SS+pair*4u+3u)==0u &&
        (pair+1u>=DW || A(SOFF+y*SS+pair*4u+7u)==0u);
    if (zero) { for(uint c=0u;c<4u;++c) { W(d+c,0u); } return; }
    uint luminance=A(s)*29u+A(s+1u)*150u+A(s+2u)*77u;
    for(uint c=0u;c<3u;++c) {
        int srcValue=int(A(s+c)); uint tint=(P(0)>>(c*8u))&255u;
        int target=OP==4u?int((luminance*tint)>>16u):(srcValue^int(tint));
        W(d+c,uint(CLAMP(srcValue+(((target-srcValue)*int(P(1)>>1u))>>7),0,255)));
    }
    W(d+3u,A(s+3u)); return;
}
// F6: 00411B30 ->0040EA60 ->0040ED50. Each Q16 coefficient divides separately.
else if (OP == 6u) {
    uint s=SOFF+y*SS+x*4u, d=DOFF+y*DS+x*4u, weight=P(1);
    if(weight==0u) { for(uint c=0u;c<4u;++c) { W(d+c,A(s+c)); } return; }
    uint alpha=A(s+3u); if(alpha==0u) return;
    uint retained=(256u-alpha)*D(d+3u), denominator=alpha*256u+retained;
    uint foreground=(alpha*(256u-weight)<<16u)/denominator;
    uint background=(retained<<16u)/denominator;
    for(uint c=0u;c<3u;++c) { W(d+c,(A(s+c)*foreground+D(d+c)*background)>>16u); }
    W(d+3u,denominator>>8u); return;
}
)BGI"
