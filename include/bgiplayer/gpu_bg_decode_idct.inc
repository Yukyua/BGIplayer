R"BGI(
uint total_blocks=blocks_x*stripes*3u;
if(block<total_blocks) {
    uint base=block*64u,q=(block%(blocks_x*3u))<blocks_x?0u:64u;
    bool zero_ac=true;
    uint first=component&~3u;
    for(uint row=1u;row<8u;++row)for(uint col=first;col<first+4u;++col)
        zero_ac=zero_ac && BG_COEFF(base+row*8u+col)==0;
    BG_PRECISE float column[8];
    for(uint row=0u;row<8u;++row)column[row]=float(BG_COEFF(base+row*8u+component))*BG_TABLE(q+row*8u+component);
    if(zero_ac)for(uint row=1u;row<8u;++row)column[row]=column[0];
    else bgi_inverse(column);
    for(uint row=0u;row<8u;++row)BG_WORK(row*8u+component)=column[row];
}
// Four independent blocks share one 32-thread group. Invalid tail blocks
// still participate in the transpose barrier, then leave without reading.
BG_SYNC();
if(block>=total_blocks)return;
BG_PRECISE float lane[8];
for(uint col=0u;col<8u;++col)lane[col]=BG_WORK(component*8u+col);
bgi_inverse(lane);
uint packed_samples[2];packed_samples[0]=0u;packed_samples[1]=0u;
for(uint col=0u;col<8u;++col) {
    float v=lane[col];
    if(isnan(v) || isinf(v) || v>=2147483648.0 || v< -2147483648.0) {BG_ERROR();return;}
    int packed=clamp(int(v),-32768,32767);
    packed_samples[col/4u]|=uint(clamp((packed>>3)+128,0,255))<<((col%4u)*8u);
}
BG_SAMPLE_WORD(block*16u+component*2u,packed_samples[0]);
BG_SAMPLE_WORD(block*16u+component*2u+1u,packed_samples[1]);
)BGI"
