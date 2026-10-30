#include "bgiplayer/compressed_bg_mt.hpp"
#include "bgiplayer/host_resource_jobs.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>
namespace bgiplayer {
namespace {
// The original predictor averages unsigned channels with truncation, then adds
// the residual modulo 256. Packed operations keep carries within each byte.
uint32_t add_channels(uint32_t a, uint32_t b) {
 return ((a & 0x7f7f7f7fU) + (b & 0x7f7f7f7fU)) ^ ((a ^ b) & 0x80808080U);
}
template<unsigned Channels>
void predict_mt_stripe(const std::vector<uint8_t>& residual, uint8_t* output,
                       uint32_t width, uint32_t rows) {
 if constexpr(Channels == 1) {
  uint8_t left=0;
  for(uint32_t x=0;x<width;++x)output[x]=left=uint8_t(residual[x]+left);
  for(uint32_t y=1;y<rows;++y) {
   auto* row=output+size_t(y)*width;
   const auto* prior=row-width;
   const auto* input=residual.data()+size_t(y)*width;
   row[0]=uint8_t(input[0]+prior[0]);
   for(uint32_t x=1;x<width;++x)row[x]=uint8_t(input[x]+(unsigned(prior[x])+row[x-1])/2);
  }
 } else {
  const auto load=[](const uint8_t* p) {
   // memcpy is alignment-safe. Byte assembly for RGB also preserves the zero
   // padding channel, including on big-endian hosts.
   if constexpr(Channels == 3)return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16;
   else {uint32_t value;std::memcpy(&value,p,4);return value;}
  };
  const auto store=[](uint8_t* p,uint32_t value) {
   if constexpr(Channels == 3) {
    p[0]=uint8_t(value);p[1]=uint8_t(value>>8);p[2]=uint8_t(value>>16);p[3]=0;
   } else std::memcpy(p,&value,4);
  };
  uint32_t left=0;
  for(uint32_t x=0;x<width;++x) {
   left=add_channels(load(residual.data()+size_t(x)*Channels),left);
   store(output+size_t(x)*4,left);
  }
  for(uint32_t y=1;y<rows;++y) {
   auto* row=output+size_t(y)*width*4;
   const auto* prior=row-size_t(width)*4;
   const auto* input=residual.data()+size_t(y)*width*Channels;
   left=add_channels(load(input),load(prior));store(row,left);
   for(uint32_t x=1;x<width;++x) {
    const uint32_t top=load(prior+size_t(x)*4);
    const uint32_t prediction=(top & left)+(((top ^ left) & 0xfefefefeU)>>1);
    left=add_channels(load(input+size_t(x)*Channels),prediction);
    store(row+size_t(x)*4,left);
   }
  }
 }
}
}
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
  prepare_resource_parts(count,size_t(w)*h,[&](size_t index){
   const auto s=static_cast<uint32_t>(index);
   const size_t begin=dword(48+size_t(s)*4),end=s+1<count?dword(52+size_t(s)*4):b.size();
   if(begin<48+size_t(count)*4||end<begin||end>b.size()||end-begin<4)fail("MT stripe bounds");
   size_t p=begin;
   auto varint=[&](){uint32_t n=0;for(unsigned sh=0;sh<35;sh+=7){if(p>=end)fail("MT varint");uint8_t v=b[p++];n|=uint32_t(v&127)<<sh;if(!(v&128))return n;}fail("MT varint overflow");return 0U;};
   const uint32_t rle_size=dword(p);p+=4;
   const uint32_t rows=std::min(stripe,h-s*stripe);const size_t raw=size_t(w)*rows*channels;
   if(!rle_size||rle_size>raw*2+16)fail("MT RLE allocation");
   struct Node{uint64_t weight;unsigned left=0,right=0;};std::array<Node,511> nodes{};
   std::array<unsigned,256> leaves{};unsigned leaf_count=0;
   for(unsigned i=0;i<256;++i){nodes[i].weight=varint();if(nodes[i].weight)leaves[leaf_count++]=i;}
   if(!leaf_count)fail("MT empty Huffman tree");
   // The original repeated active-node scan selects by (weight,node index).
   // Sorted leaves and monotonically generated parents give the same two
   // minima without scanning up to 511 nodes for every pair.
   std::sort(leaves.begin(),leaves.begin()+leaf_count,[&](unsigned a,unsigned c){
    return nodes[a].weight!=nodes[c].weight?nodes[a].weight<nodes[c].weight:a<c;
   });
   unsigned leaf=0,parent=256,used=256;
   const auto next=[&]() {
    if(leaf<leaf_count && (parent==used || nodes[leaves[leaf]].weight<=nodes[parent].weight))return leaves[leaf++];
    return parent++;
   };
   while(leaf_count-leaf+used-parent>1) {
    const unsigned first=next(),second=next();
    if(used>=nodes.size())fail("MT Huffman tree");
    nodes[used++]={nodes[first].weight+nodes[second].weight,first,second};
   }
   const unsigned root=next();
   // Eight LSB-first bits usually resolve a leaf in one lookup. Long codes
   // resume at the exact original node; short tails use the bounded bit walk.
   struct Prefix {unsigned node,bits;};std::array<Prefix,256> prefixes{};
   for(unsigned prefix=0;prefix<prefixes.size();++prefix) {
    unsigned node=root;unsigned depth=0;
    while(node>=256 && depth<8) {
     node=(prefix & (1U<<depth))?nodes[node].right:nodes[node].left;++depth;
    }
    prefixes[prefix]={node,depth};
   }
   std::vector<uint8_t> rle(rle_size);unsigned bit=0;
   for(auto& symbol:rle) {
    unsigned node=root;
    if(p<end && (bit==0 || end-p>=2)) {
     unsigned prefix=b[p];if(bit)prefix|=unsigned(b[p+1])<<8;
     const auto entry=prefixes[(prefix>>bit)&255U];node=entry.node;
     const unsigned consumed=bit+entry.bits;p+=consumed/8;bit=consumed%8;
    }
    while(node>=256) {
     if(p>=end)fail("MT Huffman bitstream");
     node=(b[p] & (1U<<bit))?nodes[node].right:nodes[node].left;
     if(++bit==8){bit=0;++p;}
    }
    symbol=uint8_t(node);
   }
   size_t rp=0;std::vector<uint8_t> residual;residual.reserve(raw+1);bool literal=true;
   while(rp<rle.size()){uint32_t length=0;unsigned sh=0;for(;;){if(rp>=rle.size()||sh>=35)fail("MT RLE count");uint8_t v=rle[rp++];length|=uint32_t(v&127)<<sh;sh+=7;if(!(v&128))break;}
    if(length>raw+1-residual.size())fail("MT RLE output");if(literal){if(length>rle.size()-rp)fail("MT RLE literal");residual.insert(residual.end(),rle.data()+rp,rle.data()+rp+length);rp+=length;}else residual.insert(residual.end(),length,0);literal=!literal;}
   // Original allocates raw+1 bytes; only raw prediction bytes are consumed.
   if(residual.size()<raw)fail("MT residual size");
   auto* output=out.pixels.data()+size_t(s)*stripe*w*internal;
   if(channels==1)predict_mt_stripe<1>(residual,output,w,rows);
   else if(channels==3)predict_mt_stripe<3>(residual,output,w,rows);
   else predict_mt_stripe<4>(residual,output,w,rows);
  });
  return out;
 }catch(const std::exception& e){if(error)*error=e.what();return std::nullopt;}
}
}
