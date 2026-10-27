#include "bgiplayer/compressed_bg_mt.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
namespace bgiplayer {
std::optional<CompressedBgV2Decoded> decode_compressed_bg_mt(std::span<const uint8_t> b,std::string* error) {
 try {
  auto fail=[](const char* s)->void{throw std::runtime_error(s);};
  auto dword=[&](size_t p){if(p>b.size()||b.size()-p<4)fail("MT truncated dword");return uint32_t(b[p])|uint32_t(b[p+1])<<8|uint32_t(b[p+2])<<16|uint32_t(b[p+3])<<24;};
  if(b.size()<52||std::string_view((const char*)b.data(),15)!=std::string_view("CompressedBG_MT\0",15))fail("MT header");
  const auto w=dword(16),h=dword(20),stripe=dword(24),bits=dword(28);
  if(!w||!h||!stripe||uint64_t(w)*h>0x4000000||(bits!=8&&bits!=24&&bits!=32))fail("MT descriptor");
  // 1400BE9A0:24 also has a four-byte internal pixel; format7 retains alpha0.
  const uint32_t channels=bits/8,internal=bits==8?1:4,count=(h-1)/stripe+1;
  if(uint64_t(count)*4+48>b.size())fail("MT stripe table");
  CompressedBgV2Decoded out{w,h,bits,bits==8?3U:bits==24?7U:2U,std::vector<uint8_t>(size_t(w)*h*internal)};
  for(uint32_t s=0;s<count;++s){
   const size_t begin=dword(48+size_t(s)*4),end=s+1<count?dword(52+size_t(s)*4):b.size();
   if(begin<48+size_t(count)*4||end<begin||end>b.size()||end-begin<4)fail("MT stripe bounds");
   size_t p=begin;
   auto varint=[&](){uint32_t n=0;for(unsigned sh=0;sh<35;sh+=7){if(p>=end)fail("MT varint");uint8_t v=b[p++];n|=uint32_t(v&127)<<sh;if(!(v&128))return n;}fail("MT varint overflow");return 0U;};
   const uint32_t rle_size=dword(p);p+=4;
   const uint32_t rows=std::min(stripe,h-s*stripe);const size_t raw=size_t(w)*rows*channels;
   if(!rle_size||rle_size>raw*2+16)fail("MT RLE allocation");
   struct Node{uint64_t weight;int left=-1,right=-1;bool active=false;};std::array<Node,511> nodes{};
   for(unsigned i=0;i<256;++i){nodes[i].weight=varint();nodes[i].active=nodes[i].weight!=0;}
   unsigned used=256;int root=-1;
   for(;;){int first=-1,second=-1;for(unsigned i=0;i<used;++i)if(nodes[i].active){if(first<0||nodes[i].weight<nodes[first].weight){second=first;first=int(i);}else if(second<0||nodes[i].weight<nodes[second].weight)second=int(i);}
    if(first<0)fail("MT empty Huffman tree");if(second<0){root=first;break;}if(used>=nodes.size())fail("MT Huffman tree");nodes[first].active=nodes[second].active=false;nodes[used]={nodes[first].weight+nodes[second].weight,first,second,true};++used;}
   std::vector<uint8_t> rle;rle.reserve(rle_size);unsigned bit=0;
   while(rle.size()<rle_size){int n=root;while(n>=256){if(p>=end)fail("MT Huffman bitstream");n=(b[p]&(1U<<bit))?nodes[n].right:nodes[n].left;if(++bit==8){bit=0;++p;}}rle.push_back(uint8_t(n));}
   size_t rp=0;std::vector<uint8_t> residual;residual.reserve(raw+1);bool literal=true;
   while(rp<rle.size()){uint32_t length=0;unsigned sh=0;for(;;){if(rp>=rle.size()||sh>=35)fail("MT RLE count");uint8_t v=rle[rp++];length|=uint32_t(v&127)<<sh;sh+=7;if(!(v&128))break;}
    if(length>raw+1-residual.size())fail("MT RLE output");if(literal){if(length>rle.size()-rp)fail("MT RLE literal");residual.insert(residual.end(),rle.begin()+rp,rle.begin()+rp+length);rp+=length;}else residual.insert(residual.end(),length,0);literal=!literal;}
   // Original allocates raw+1 bytes; only raw prediction bytes are consumed.
   if(residual.size()<raw)fail("MT residual size");
   for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<w;++x)for(uint32_t c=0;c<channels;++c){size_t q=(size_t(s*stripe+y)*w+x)*internal+c;unsigned predict=0;if(y&&x)predict=(unsigned(out.pixels[q-w*internal])+out.pixels[q-internal])/2;else if(y)predict=out.pixels[q-w*internal];else if(x)predict=out.pixels[q-internal];out.pixels[q]=uint8_t(residual[(size_t(y)*w+x)*channels+c]+predict);}
  }
  return out;
 }catch(const std::exception& e){if(error)*error=e.what();return std::nullopt;}
}
}
