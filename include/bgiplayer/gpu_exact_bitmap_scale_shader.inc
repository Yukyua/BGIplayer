R"BGI(
// 37: P0=mode0/1, P1=opacity0..256. Opacity0 copies all format bytes.
// 38: same ceil-size vertical-then-horizontal PAVGB as SpriteReduction.
// 39: P0/P1=signed output offset; P2=has-mask; P3=mode0..256.
//     A mask uses source row stride, matching the established CPU arm.
// 40: P0=radius1..3, centered at source+8, saturate sum then punch center.
// 41: P0=nearest0/bilinear1/middle-box2/integer-box3;
//     P1/2=initial Q16 coordinates, P3/4=Q16 steps;
//     P5/6=output start, P7/8=output end exclusive;
//     P9/10=box width/height in8.8. Host keeps invalid/faulting geometry CPU.
if (OP==37u) {
    uint d=DOFF+y*DS+x*DB;
    if (P(1)==0u) {
        for (uint c=0u;c<DB;++c) W(d+c,A(SOFF+y*SS+x*SB+c));
        return;
    }
    uint halfRadius=MIN(P(1),255u), radius=halfRadius*2u+1u;
    uint factor=(radius*2u+131072u)/radius;
    for (uint c=0u;c<4u;++c) {
        uint sum=0u;
        for (int delta=-int(halfRadius);delta<=int(halfRadius);++delta) {
            int sx=int(x)+delta;
            if (sx<0 || uint(sx)>=SW) {
                if (P(0)==0u) continue;
                sx=CLAMP(sx,0,int(SW)-1);
            }
            sum+=A(SOFF+y*SS+uint(sx)*4u+c)>>1u;
        }
        W(d+c,MIN((sum*factor)>>16u,255u));
    }
    return;
}
if (OP==38u) {
    uint left=x*2u, right=MIN(left+1u,SW-1u);
    uint top=y*2u, bottom=MIN(top+1u,SH-1u), d=DOFF+y*DS+x*4u;
    for (uint c=0u;c<4u;++c) {
        uint a=(A(SOFF+top*SS+left*4u+c)+A(SOFF+bottom*SS+left*4u+c)+1u)>>1u;
        uint b=(A(SOFF+top*SS+right*4u+c)+A(SOFF+bottom*SS+right*4u+c)+1u)>>1u;
        W(d+c,(a+b+1u)>>1u);
    }
    return;
}
if (OP==39u) {
    uint sx=x-P(0), sy=y-P(1);
    uint alpha=0u;
    if (sx<SW && sy<SH && (P(2)==0u || (sx<BW && sy<BH))) {
        uint a=SOFF+sy*SS+sx*4u;
        alpha=A(a+3u);
        if (P(2)!=0u) alpha=uint(int(alpha)+(((int(B(BOFF+sy*SS+sx*4u+3u))-int(alpha))*int(P(3)))>>8));
    }
    W(DOFF+y*DS+x,alpha&255u);
    return;
}
if (OP==40u) {
    int centerX=int(x)-8, centerY=int(y)-8, radius=int(P(0));
    uint sum=0u;
    for (int dy=-radius;dy<=radius;++dy) for (int dx=-radius;dx<=radius;++dx) {
        int sx=centerX+dx, sy=centerY+dy;
        if (sx>=0 && sy>=0 && uint(sx)<SW && uint(sy)<SH)
            sum+=A(SOFF+uint(sy)*SS+uint(sx)*4u+3u);
    }
    sum=MIN(sum,255u);
    if (centerX>=0 && centerY>=0 && uint(centerX)<SW && uint(centerY)<SH) {
        uint alpha=A(SOFF+uint(centerY)*SS+uint(centerX)*4u+3u);
        if (alpha!=0u) sum=(sum*(256u-alpha))>>8u;
    }
    W(DOFF+y*DS+x,sum);
    return;
}
if (OP==41u) {
    if (x<P(5) || y<P(6) || x>=P(7) || y>=P(8)) return;
    uint fixedX=P(1)+(x-P(5))*P(3), fixedY=P(2)+(y-P(6))*P(4);
    uint sx=fixedX>>16u, sy=fixedY>>16u, d=DOFF+y*DS+x*4u;
    if (P(0)==0u) {
        if (sx<SW && sy<SH) for (uint c=0u;c<4u;++c) W(d+c,A(SOFF+sy*SS+sx*4u+c));
        return;
    }
    if (P(0)==1u) {
        uint fx=(fixedX>>8u)&255u, fy=(fixedY>>8u)&255u;
        for (uint c=0u;c<4u;++c) {
            uint tl=0u,tr=0u,bl=0u,br=0u;
            if (sx<SW && sy<SH) tl=A(SOFF+sy*SS+sx*4u+c);
            if (sx+1u<SW && sy<SH) tr=A(SOFF+sy*SS+(sx+1u)*4u+c);
            if (sx<SW && sy+1u<SH) bl=A(SOFF+(sy+1u)*SS+sx*4u+c);
            if (sx+1u<SW && sy+1u<SH) br=A(SOFF+(sy+1u)*SS+(sx+1u)*4u+c);
            uint top=tl*(256u-fx)+tr*fx, bottom=bl*(256u-fx)+br*fx;
            W(d+c,(top*(256u-fy)+bottom*fy)>>16u);
        }
        return;
    }
    if (P(0)==2u) {
        uint left=fixedX>>8u, top=fixedY>>8u;
        uint right=MIN(left+P(9),SW<<8u), bottom=MIN(top+P(10),SH<<8u);
        uint area=((bottom-top)*(right-left))>>8u;
        for (uint c=0u;c<4u;++c) {
            uint sum=0u;
            for (uint yy=top;yy<bottom;) {
                uint nextY=MIN((yy+256u)&~255u,bottom);
                for (uint xx=left;xx<right;) {
                    uint nextX=MIN((xx+256u)&~255u,right);
                    uint fragmentArea=(nextX-xx)*(nextY-yy);
                    uint weight=((fragmentArea*256u)/area)>>8u;
                    sum+=A(SOFF+(yy>>8u)*SS+(xx>>8u)*4u+c)*weight;
                    xx=nextX;
                }
                yy=nextY;
            }
            W(d+c,MIN(sum>>8u,255u));
        }
        return;
    }
    uint right=MIN(sx+((P(9)+255u)>>8u),SW), bottom=MIN(sy+((P(10)+255u)>>8u),SH);
    for (uint c=0u;c<4u;++c) {
        uint sum=0u,included=0u,area=0u;
        if (sx<right && sy<bottom) {
            area=(right-sx)*(bottom-sy);
            for (uint yy=sy;yy<bottom;++yy) for (uint xx=sx;xx<right;++xx) {
                uint a=SOFF+yy*SS+xx*4u;
                if (SF==2u && A(a+3u)==0u) continue;
                sum+=A(a+c);++included;
            }
        }
        W(d+c,(sum/MAX(c==3u ? area : included,1u))&255u);
    }
    return;
}
)BGI"
