#include "bgiplayer/vm.hpp"
#include "bgiplayer/dcfs.hpp"
#include "bgiplayer/sdc_encode.hpp"
#include <chrono>
namespace bgiplayer {
namespace {
std::string codec_word(uint32_t value){std::string result(4,'\0');for(unsigned i=0;i<4;++i)result[i]=static_cast<char>(value>>(8U*i));return result;}
uint32_t codec_word(const std::string& bytes){
    if(bytes.size()!=4)throw BpFault("invalid serialized codec buffer arguments");
    uint32_t value=0;for(unsigned i=0;i<4;++i)value|=static_cast<uint32_t>(static_cast<uint8_t>(bytes[i]))<<(8U*i);return value;
}
}
void VM::install_codec_buffer_wait(Thread& thread,CodecProcedureKind kind,
    uint32_t output,uint32_t input,uint32_t row_bytes,uint32_t rows){
    if(kind!=CodecProcedureKind::EncodeData && kind!=CodecProcedureKind::EncodeStruct)
        throw BpFault("unsupported raw codec procedure");
    const uint32_t id=next_procedure_id_++;
    const uint32_t token=0xE8000000U|id;
    // Existing serialized fields carry raw arguments rather than fabricated
    // filenames: string_a=rowbytes, string_b=rows, field_a=dst, field_b=src.
    codec_procedures_[token]={id,kind,codec_word(row_bytes),codec_word(rows),output,input,true,false};
    thread.snapshot_wait_kind=SnapshotWaitKind::Codec;
    thread.snapshot_wait_index=token;
    thread.wait_procedure=[this,token]{return advance_codec_procedure(token);};
    thread.status_flags|=1U;thread.state=ThreadState::Waiting;
}
WaitProcedureResult VM::advance_codec_procedure(uint32_t token){
    auto& state=codec_procedures_.at(token);
    if(state.complete)return {1,{state.field_a}};
    const uint32_t row_bytes=codec_word(state.string_a),rows=codec_word(state.string_b);
    uint32_t result=0;
    try{
        // F10A0/F0E90 maps VM0 to a native null pointer, unlike a
        // general VM global-frame load at offset0.
        if(state.field_a==0 || (state.field_b==0 && row_bytes!=0 &&
            (state.kind==CodecProcedureKind::EncodeData || rows!=0)))
            throw Error("null raw codec buffer");
        std::vector<uint8_t> payload;
        if(state.kind==CodecProcedureKind::EncodeStruct){
            // Original FB110 ignores the DCFS status. Invalid zero dimensions
            // leave its initialized raw size0, which is then SDC-encoded.
            if(row_bytes!=0 && rows!=0){
                const uint64_t size=static_cast<uint64_t>(row_bytes)*rows;
                if(size>UINT32_MAX)throw Error("DCFS input size overflow");
                payload=encode_dcfs_payload(memory_.read_bytes(state.field_b,
                    static_cast<std::size_t>(size),0),row_bytes,rows);
            }
        }else if(state.kind==CodecProcedureKind::EncodeData){
            payload=memory_.read_bytes(state.field_b,row_bytes,0);
        }else throw BpFault("raw codec procedure kind is unresolved");
        const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto encoded=encode_original_sdc(payload,static_cast<uint16_t>(milliseconds%1000));
        memory_.write_bytes(state.field_a,encoded,0);
        result=static_cast<uint32_t>(encoded.size());
    }catch(const std::exception&){
        // Original FF360/FB110 exception branches leave initialized result0
        // and still publish completion. No token masquerades as a byte count.
    }
    state.field_a=result;state.complete=true;
    return {1,{result}};
}
}
