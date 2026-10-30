#include "bgiplayer/compressed_bg_v2.hpp"
#include "bgiplayer/bmv_idct.hpp"
#include "bgiplayer/validation.hpp"
#include "bgiplayer/host_resource_jobs.hpp"
#include "bgiplayer/gpu_bg_decode.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace bgiplayer {
namespace {
struct DecodeFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
uint32_t word(std::span<const uint8_t> bytes, std::size_t offset) {
    if (offset + 2U > bytes.size()) throw DecodeFailure("v2 word bounds");
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1U]) << 8U);
}
uint32_t dword(std::span<const uint8_t> bytes, std::size_t offset) {
    return word(bytes, offset) | (word(bytes, offset + 2U) << 16U);
}
uint32_t varint(std::span<const uint8_t> bytes, std::size_t& position) {
    uint32_t value = 0;
    for (uint32_t shift = 0; shift < 35U; shift += 7U) {
        if (position == bytes.size()) throw DecodeFailure("v2 varint bounds");
        const uint32_t byte = bytes[position++];
        if (shift == 28U && (byte & 0x70U) != 0) throw DecodeFailure("v2 varint overflow");
        value |= (byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) return value;
    }
    throw DecodeFailure("v2 varint length");
}
// 004AD760: first active candidate, then the target's side-dependent scan
// starting at side+1, with strict-less-than tie handling. A sole leaf gets
// an internal parent with a missing right child and a one-bit zero code.
struct Huffman {
    struct Node { uint32_t weight = 0; bool active = false; int child[2]{-1, -1}; };
    std::vector<Node> nodes;
    uint32_t alphabet = 0;
    int root = -1;
    struct Prefix { int16_t node; uint8_t bits; };
    static constexpr uint32_t prefix_bits=9;
    std::array<Prefix,1U<<prefix_bits> prefixes{};
    void prepare_prefixes() {
        for(uint32_t prefix=0;prefix<prefixes.size();++prefix) {
            int node=root;uint8_t depth=0;
            while(node>=static_cast<int>(alphabet) && depth<prefix_bits) {
                node=nodes[static_cast<size_t>(node)].child[(prefix>>(prefix_bits-1U-depth))&1U];
                ++depth;
            }
            prefixes[prefix]={static_cast<int16_t>(node),depth};
        }
    }
    explicit Huffman(std::span<const uint32_t> frequencies)
        : nodes(frequencies.size() * 2U - 1U),
          alphabet(static_cast<uint32_t>(frequencies.size())) {
        uint64_t total = 0;
        for (std::size_t index = 0; index < frequencies.size(); ++index) {
            nodes[index].weight = frequencies[index];
            nodes[index].active = frequencies[index] != 0;
            total += frequencies[index];
        }
        if (total == 0 || total > std::numeric_limits<uint32_t>::max())
            throw DecodeFailure("v2 Huffman frequencies");
        for (std::size_t next = frequencies.size(); next < nodes.size(); ++next) {
            uint32_t sum = 0;
            for (std::size_t side = 0; side < 2U; ++side) {
                int selected = -1;
                for (std::size_t index = 0; index < next; ++index)
                    if (nodes[index].active) { selected = static_cast<int>(index); break; }
                if (selected >= 0)
                    for (std::size_t index = side + 1U; index < next; ++index)
                        if (nodes[index].active &&
                            nodes[index].weight < nodes[static_cast<std::size_t>(selected)].weight)
                            selected = static_cast<int>(index);
                nodes[next].child[side] = selected;
                if (selected >= 0) {
                    auto& leaf = nodes[static_cast<std::size_t>(selected)];
                    leaf.active = false;
                    sum += leaf.weight;
                }
            }
            nodes[next].active = true;
            nodes[next].weight = sum;
            if (sum >= total) { root = static_cast<int>(next); prepare_prefixes(); return; }
        }
        throw DecodeFailure("v2 Huffman tree");
    }
};
struct Bits {
    std::span<const uint8_t> bytes;
    std::size_t position = 0;
    uint32_t peek(uint32_t count) const {
        if(count==0)return 0;
        const uint32_t width=static_cast<uint32_t>(position&7U)+count;
        const uint32_t byte_count=(width+7U)/8U;
        const size_t start=position/8U;
        uint32_t value=bytes[start];
        for(uint32_t byte=1;byte<byte_count;++byte)value=(value<<8U)|bytes[start+byte];
        return (value>>(byte_count*8U-width))&((1U<<count)-1U);
    }
    uint32_t read(uint32_t count) {
        if (count > 16U || position + count > bytes.size() * 8U)
            throw DecodeFailure("v2 bitstream bounds");
        const uint32_t value=peek(count);
        position+=count;
        return value;
    }
    uint32_t symbol(const Huffman& tree) {
        int node = tree.root;
        if(bytes.size()*8U-position>=Huffman::prefix_bits) {
            const auto prefix=tree.prefixes[peek(Huffman::prefix_bits)];
            node=prefix.node;position+=prefix.bits;
        }
        while (node >= static_cast<int>(tree.alphabet))
            node = tree.nodes[static_cast<std::size_t>(node)].child[read(1)];
        if (node < 0) throw DecodeFailure("v2 missing Huffman branch");
        return static_cast<uint32_t>(node);
    }
    int32_t magnitude(uint32_t count) {
        if (count > 15U) throw DecodeFailure("v2 magnitude width");
        const uint32_t value = read(count);
        if (count != 0 && (value & (1U << (count - 1U))) == 0)
            return static_cast<int32_t>(value) + 1 - static_cast<int32_t>(1U << count);
        return static_cast<int32_t>(value);
    }
    std::size_t consumed() const { return (position + 7U) / 8U; }
};
// Original PE bytes at 005009A0.
constexpr std::array<uint8_t, 64> zigzag{
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

// 004AE450 supplies padded dimensions and BF_Movie version 10001 to
// 004AAD80: the extra row offset points to a dword alpha method.
std::vector<uint8_t> decode_alpha(std::span<const uint8_t> encoded,
                                 uint32_t width, uint32_t height) {
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    std::vector<uint8_t> alpha(pixel_count);
    const uint32_t method = dword(encoded, 0);
    std::size_t position = 4;
    if (method == 1U) {
        // 004ADE40: eight LSB-first command flags, literal bytes or a
        // two-byte (signed dx:6, nonpositive dy:3, length-minus-three:7).
        std::size_t output = 0;
        while (output < pixel_count) {
            if (position >= encoded.size()) throw DecodeFailure("v2 alpha flags bounds");
            const uint32_t flags = encoded[position++];
            for (uint32_t bit = 0; bit < 8U && output < pixel_count; ++bit) {
                if ((flags & (1U << bit)) == 0) {
                    if (position >= encoded.size()) throw DecodeFailure("v2 alpha literal bounds");
                    alpha[output++] = encoded[position++];
                } else {
                    const uint32_t code = word(encoded, position);
                    position += 2U;
                    const int32_t dx = static_cast<int32_t>(code & 63U) -
                        ((code & 32U) != 0 ? 64 : 0);
                    const int32_t dy = static_cast<int32_t>((code >> 6U) & 7U) -
                        ((code & 448U) != 0 ? 8 : 0);
                    const uint32_t count = (code >> 9U) + 3U;
                    const int64_t source = static_cast<int64_t>(output) + dx +
                        static_cast<int64_t>(dy) * width;
                    if (source < 0 || source >= static_cast<int64_t>(output) ||
                        count > pixel_count - output)
                        throw DecodeFailure("v2 alpha copy bounds");
                    for (uint32_t index = 0; index < count; ++index)
                        alpha[output++] = alpha[static_cast<std::size_t>(source) + index];
                }
            }
        }
    } else if (method == 2U) {
        // 004AE0D0 decodes the byte Huffman stream; 004ADAB0 scatters its
        // tile-mask-prefixed bytes into the fourth byte of each 8x8 tile.
        const uint32_t decoded_size = dword(encoded, position);
        position += 4U;
        if (decoded_size > pixel_count * 2U) throw DecodeFailure("v2 alpha decoded bounds");
        std::array<uint32_t,256> frequencies{};
        for (auto& frequency : frequencies) frequency = varint(encoded, position);
        const Huffman tree(frequencies);
        Bits bits{encoded.subspan(position)};
        std::vector<uint8_t> bytes(decoded_size);
        for (auto& byte : bytes) byte = static_cast<uint8_t>(bits.symbol(tree));
        const uint32_t blocks_x = width / 8U, blocks_y = height / 8U;
        const std::size_t mask_size = (static_cast<std::size_t>(blocks_x) * blocks_y + 7U) / 8U;
        if (mask_size > bytes.size()) throw DecodeFailure("v2 alpha tile mask bounds");
        position = mask_size;
        for (uint32_t y = 0; y < blocks_y; ++y) {
            for (uint32_t x = 0; x < blocks_x; ++x) {
                const std::size_t tile = static_cast<std::size_t>(y) * blocks_x + x;
                if ((bytes[tile / 8U] & (1U << (tile & 7U))) == 0)
                    throw DecodeFailure("v2 retained alpha tile is unsupported");
                if (bytes.size() - position < 64U) throw DecodeFailure("v2 alpha tile bounds");
                for (uint32_t row = 0; row < 8U; ++row)
                    for (uint32_t column = 0; column < 8U; ++column)
                        alpha[static_cast<std::size_t>(y*8U+row)*width+x*8U+column] = bytes[position++];
            }
        }
    } else {
        throw DecodeFailure("v2 alpha method");
    }
    return alpha;
}
}  // namespace

std::optional<CompressedBgV2Decoded> decode_compressed_bg_v2(
    std::span<const uint8_t> encoded, std::string* error, bool allow_resident) {
    try {
        if (encoded.size() < 0x30U ||
            std::string_view(reinterpret_cast<const char*>(encoded.data()), 16) !=
                std::string_view("CompressedBG___\0", 16) || word(encoded, 0x2E) != 2U)
            throw DecodeFailure("v2 header");
        const uint32_t width = word(encoded, 0x10);
        const uint32_t height = word(encoded, 0x12);
        const uint32_t bits_per_pixel = word(encoded, 0x14);
        const uint32_t bytes_per_pixel = bits_per_pixel / 8U;
        if (width == 0 || height == 0 || (bits_per_pixel != 24U && bits_per_pixel != 32U))
            throw DecodeFailure("v2 unsupported dimensions or alpha format");
        const uint32_t quant_size = dword(encoded, 0x28);
        if (quant_size != 0x84U || 0x30ULL + quant_size > encoded.size())
            throw DecodeFailure("v2 quantization header");
        std::array<uint8_t, 0x84> quant{};
        uint32_t seed = dword(encoded, 0x24);
        uint8_t sum = 0, parity = 0;
        for (std::size_t index = 0; index < quant.size(); ++index) {
            const uint32_t previous = seed;
            const uint32_t low = (previous & 0xFFFFU) * 0x4E35U;
            const uint32_t high = previous >> 16U;
            seed = low + 1U + previous * 0x015A0000U + high * 0x4E350000U;
            const uint32_t key = (low >> 16U) - high * 0x31CBU + previous * 0x15AU;
            quant[index] = static_cast<uint8_t>(encoded[0x30U + index] - key);
            sum = static_cast<uint8_t>(sum + quant[index]);
            parity ^= quant[index];
        }
        if ((sum != encoded[0x2C] || parity != encoded[0x2D]) &&
            !allow_content_check_failure("CompressedBG v2 quantization checksum mismatch"))
            throw DecodeFailure("v2 quantization checksum");
        const auto y_quant = bmv_scaled_quantization(std::span<const uint8_t,64>(quant.data(),64));
        const auto c_quant = bmv_scaled_quantization(std::span<const uint8_t,64>(quant.data()+64,64));
        const std::size_t data_base = 0x30U + quant_size;
        std::size_t cursor = data_base;
        std::array<uint32_t,16> dc_frequencies{};
        std::array<uint32_t,176> ac_frequencies{};
        for (auto& frequency : dc_frequencies) frequency = varint(encoded,cursor);
        for (auto& frequency : ac_frequencies) frequency = varint(encoded,cursor);
        const Huffman dc_tree(dc_frequencies), ac_tree(ac_frequencies);
        const uint32_t blocks_x = (width + 7U) / 8U;
        const uint32_t stripes = (height + 7U) / 8U;
        if (static_cast<uint64_t>(width) * height > 0x4000000U ||
            cursor + (static_cast<uint64_t>(stripes) + (bits_per_pixel == 32U ? 1U : 0U)) * 4U > encoded.size())
            throw DecodeFailure("v2 stripe table bounds");
        std::vector<uint32_t> offsets(stripes + (bits_per_pixel == 32U ? 1U : 0U));
        for (auto& offset : offsets) { offset = dword(encoded,cursor); cursor += 4U; }
        CompressedBgV2Decoded result{width,height,bits_per_pixel,bits_per_pixel == 32U ? 2U : 1U,{}};
        const bool use_gpu=allow_resident && gpu_bg_decode_enabled(uint64_t(width)*height) && host_background_decode_available();
        if(!use_gpu)result.pixels.resize(static_cast<std::size_t>(width) * height * bytes_per_pixel);
        const std::size_t flags_size = (blocks_x + 7U) / 8U;
        const std::size_t block_count = static_cast<std::size_t>(blocks_x) * 3U;
        std::vector<std::array<int16_t,64>> resident_coefficients;
        std::shared_ptr<BgCoefficientWorkspace> workspace;
        std::span<std::array<int16_t,64>> resident;
        if(use_gpu) {
            workspace=allocate_host_background_coefficients(block_count*stripes);
            if(workspace && workspace->coefficients.size()==block_count*stripes)
                resident=workspace->coefficients;
            else {
                workspace.reset();resident_coefficients.resize(block_count*stripes);
                resident=resident_coefficients;
            }
        }
        prepare_resource_parts(stripes,static_cast<size_t>(width)*height,[&](size_t stripe_index) {
            const auto stripe=static_cast<uint32_t>(stripe_index);
            const uint64_t start64 = static_cast<uint64_t>(data_base) + offsets[stripe];
            const uint64_t end64 = stripe + 1U == offsets.size()
                ? encoded.size() : static_cast<uint64_t>(data_base) + offsets[stripe+1U];
            if (start64 < cursor || start64 > end64 || end64 > encoded.size() ||
                flags_size > end64 - start64) throw DecodeFailure("v2 stripe bounds");
            std::size_t position = static_cast<std::size_t>(start64);
            // 004AB5C0 starts each flag byte at mask1 and shifts left.
            for (uint32_t block = 0; block < blocks_x; ++block) {
                if ((encoded[position + block / 8U] & (1U << (block & 7U))) == 0)
                    throw DecodeFailure("v2 retained block is unsupported");
            }
            position += flags_size;
            const uint32_t coefficients_count = varint(encoded,position);
            if (coefficients_count != block_count * 64U || position > end64)
                throw DecodeFailure("v2 coefficient count");
            std::vector<std::array<int16_t,64>> local_coefficients;
            if(!use_gpu)local_coefficients.resize(block_count);
            std::span<std::array<int16_t,64>> coefficients=use_gpu
                ?resident.subspan(stripe_index*block_count,block_count)
                :std::span(local_coefficients);
            Bits dc{encoded.subspan(position,static_cast<std::size_t>(end64)-position)};
            uint32_t previous_dc = 0;
            for (auto& block : coefficients) {
                const uint32_t count = dc.symbol(dc_tree);
                previous_dc = (previous_dc + static_cast<uint32_t>(dc.magnitude(count))) & 0xFFFFU;
                block[0] = static_cast<int16_t>(previous_dc);
            }
            position += dc.consumed();
            Bits ac{encoded.subspan(position,static_cast<std::size_t>(end64)-position)};
            for (auto& block : coefficients) {
                uint32_t index = 1;
                while (index < 64U) {
                    const uint32_t symbol = ac.symbol(ac_tree);
                    if (symbol == 0) break;
                    if (symbol == 0xFU) { index += 16U; continue; }
                    index += symbol & 0xFU;
                    if (index >= 64U) throw DecodeFailure("v2 AC run bounds");
                    block[zigzag[index++]] = static_cast<int16_t>(ac.magnitude(symbol >> 4U));
                }
            }
            if(use_gpu)return;
            std::vector<std::array<int16_t,64>> samples(block_count);
            for (std::size_t block = 0; block < block_count; ++block) {
                if (!bmv_idct_block(coefficients[block],block < blocks_x ? y_quant : c_quant,samples[block]))
                    throw DecodeFailure("v2 transform failure");
            }
            for (uint32_t block = 0; block < blocks_x; ++block) {
                for (uint32_t row = 0; row < 8U && stripe*8U+row < height; ++row) {
                    for (uint32_t column = 0; column < 8U && block*8U+column < width; ++column) {
                        const std::size_t local = row*8U+column;
                        std::array<uint8_t,3> bgr{};
                        if (!bmv_ycbcr_to_bgr(samples[block][local],samples[blocks_x+block][local],
                                              samples[blocks_x*2U+block][local],bgr))
                            throw DecodeFailure("v2 color conversion");
                        const std::size_t output = (static_cast<std::size_t>(stripe*8U+row)*width+
                                                   block*8U+column)*bytes_per_pixel;
                        std::copy(bgr.begin(),bgr.end(),result.pixels.begin()+static_cast<std::ptrdiff_t>(output));
                    }
                }
            }
        });
        if(use_gpu) {
            std::vector<uint8_t> alpha;
            if(bits_per_pixel==32U) {
                const uint64_t start=uint64_t(data_base)+offsets.back();
                if(start<cursor || start>encoded.size())throw DecodeFailure("v2 alpha offset bounds");
                alpha=decode_alpha(encoded.subspan(size_t(start)),blocks_x*8U,stripes*8U);
            }
            result.resident_pixels=decode_host_background({width,height,resident,y_quant,c_quant,alpha,workspace});
            if(result.resident_pixels)return result;
            // The original CPU route owns failure precedence as well as pixels.
            // No partially decoded bitmap has been installed at this point.
            return decode_compressed_bg_v2(encoded,error,false);
        }
        if (bits_per_pixel == 32U) {
            const uint64_t alpha_start = static_cast<uint64_t>(data_base) + offsets.back();
            if (alpha_start < cursor || alpha_start > encoded.size())
                throw DecodeFailure("v2 alpha offset bounds");
            const uint32_t padded_width = blocks_x * 8U;
            const auto alpha = decode_alpha(encoded.subspan(static_cast<std::size_t>(alpha_start)),
                                            padded_width, stripes * 8U);
            for (uint32_t row = 0; row < height; ++row)
                for (uint32_t column = 0; column < width; ++column)
                    result.pixels[(static_cast<std::size_t>(row)*width+column)*4U+3U] =
                        alpha[static_cast<std::size_t>(row)*padded_width+column];
        }
        return result;
    } catch (const DecodeFailure& failure) {
        if(allow_resident)return decode_compressed_bg_v2(encoded,error,false);
        if (error) *error = failure.what();
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        if(allow_resident)return decode_compressed_bg_v2(encoded,error,false);
        throw;
    }
}
}  // namespace bgiplayer
