R"BGI(
// Exact Sprite/Icon operations, translated from the established CPU helpers.
// 16: no parameters; source1/2 -> destination of same format, ceil-size2x.
// 17: P0=mix0..256; P32..373 hold six RCPSS table entries per dword:
//     base12 bits then five3-bit descending deltas, in units of0x800.
// 18: P0=mode2/3/21/22; P1=parameter0..256.
// 19: P0/P1=startX/Y, P2/P3=columnStepX/Y, P4/P5=rowStepX/Y, all Q16.
if (OP==16u) {
    uint left=x*2u, right=MIN(left+1u,SW-1u);
    uint top=y*2u, bottom=MIN(top+1u,SH-1u);
    uint d=DOFF+y*DS+x*4u;
    for (uint c=0u;c<4u;++c) {
        uint a=(A(SOFF+top*SS+left*4u+c)+A(SOFF+bottom*SS+left*4u+c)+1u)>>1u;
        uint b=(A(SOFF+top*SS+right*4u+c)+A(SOFF+bottom*SS+right*4u+c)+1u)>>1u;
        W(d+c,(a+b+1u)>>1u);
    }
    return;
}
if (OP==17u) {
    if (x>=MIN(SW,BW) || y>=MIN(SH,BH)) return;
    uint a=SOFF+y*SS+x*4u, b=BOFF+y*BS+x*4u, d=DOFF+y*DS+x*4u;
    uint aw=A(a+3u)*(256u-P(0)), bw=B(b+3u)*P(0), sum=aw+bw;
    uint denominator=MAX(sum,1u), exponent=0u;
    for (uint v=denominator;v>1u;v>>=1u) ++exponent;
    uint pattern=(((denominator<<12u)>>exponent)&4095u)>>1u;
    uint packedValue=P(32u+pattern/6u), mantissa=4096u+(packedValue&4095u);
    for (uint i=0u;i<pattern%6u;++i) mantissa-=(packedValue>>(12u+i*3u))&7u;
    // RCPPS is exactly mantissa*2^(-13-exponent). The following float
    // multiplication has only29 significant product bits, so integer RNE
    // to24 bits reproduces MULPS before CVTTPS2DQ with no float builtins.
    uint product=aw*mantissa, highest=0u;
    for (uint v=product;v>1u;v>>=1u) ++highest;
    uint drop=highest>23u ? highest-23u : 0u;
    if (drop>0u) {
        uint roundHalf=1u<<(drop-1u);
        product=(product+roundHalf-1u+((product>>drop)&1u))&~((1u<<drop)-1u);
    }
    int weight=int(product>>(6u+exponent));
    for (uint c=0u;c<3u;++c) {
        int delta=(int(A(a+c))-int(B(b+c)))*weight;
        int step=delta>=0 ? delta/128 : -((-delta+127)/128);
        W(d+c,uint(CLAMP(int(B(b+c))+step,0,255)));
    }
    W(d+3u,sum>>8u);
    return;
}
if (OP==18u) {
    uint mode=P(0), strength=mode>=32u ? 256u-P(1) : P(1);
    uint a=SOFF+y*SS+x*4u, d=DOFF+y*DS+x*4u;
    uint weight=((A(a+3u)>>1u)*strength)>>8u;
    for (uint c=0u;c<3u;++c) {
        uint delta=(A(a+c)*weight)>>7u, old=D(d+c);
        W(d+c,(mode==3u || mode==34u) ? (old>delta ? old-delta : 0u) : MIN(255u,old+delta));
    }
    return;
}
if (OP==19u) {
    uint fixedX=P(0)+y*P(4)+x*P(2), fixedY=P(1)+y*P(5)+x*P(3);
    int sx=int(fixedX)>>16, sy=int(fixedY)>>16;
    int fx=int((fixedX>>12u)&15u), fy=int((fixedY>>12u)&15u);
    uint d=DOFF+y*DS+x*4u;
    for (uint c=0u;c<4u;++c) {
        int tl=0, tr=0, bl=0, br=0;
        if (sx>=0 && sy>=0 && uint(sx)<SW && uint(sy)<SH)
            tl=int(A(SOFF+uint(sy)*SS+uint(sx)*4u+c));
        if (sx+1>=0 && sy>=0 && uint(sx+1)<SW && uint(sy)<SH)
            tr=int(A(SOFF+uint(sy)*SS+uint(sx+1)*4u+c));
        if (sx>=0 && sy+1>=0 && uint(sx)<SW && uint(sy+1)<SH)
            bl=int(A(SOFF+uint(sy+1)*SS+uint(sx)*4u+c));
        if (sx+1>=0 && sy+1>=0 && uint(sx+1)<SW && uint(sy+1)<SH)
            br=int(A(SOFF+uint(sy+1)*SS+uint(sx+1)*4u+c));
        int topValue=tl+(((tr-tl)*fx)>>4), bottomValue=bl+(((br-bl)*fx)>>4);
        W(d+c,uint(CLAMP(topValue+(((bottomValue-topValue)*fy)>>4),0,255)));
    }
    return;
}
)BGI"
