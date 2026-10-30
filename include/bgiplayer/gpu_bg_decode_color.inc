R"BGI(
if(x>=width || y>=height)return;
uint sample_base=((y/8u)*blocks_x*3u+x/8u)*64u+(y%8u)*8u+x%8u;
uint cb=uint(BG_SAMPLE(sample_base+blocks_x*64u)),cr=uint(BG_SAMPLE(sample_base+blocks_x*128u));
BG_PRECISE float luma=float(BG_SAMPLE(sample_base));
BG_PRECISE float red=luma+BG_TABLE(128u+cr);
BG_PRECISE float green_first=BG_TABLE(384u+cb)+luma;
BG_PRECISE float green=green_first+BG_TABLE(640u+cr);
BG_PRECISE float blue=BG_TABLE(896u+cb)+luma;
uint b=uint(clamp(int(blue),0,255)),g=uint(clamp(int(green),0,255)),r=uint(clamp(int(red),0,255));
uint sample_alpha=has_alpha!=0u?BG_ALPHA(y*blocks_x*8u+x):0u;
BG_OUTPUT(y*width+x,b|(g<<8u)|(r<<16u)|(sample_alpha<<24u));
)BGI"
