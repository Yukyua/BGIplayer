#include "bgiplayer/host_files.hpp"
#include "bgiplayer/snapshot.hpp"
#include "bgiplayer/snapshot_container.hpp"
#include "bgiplayer/vm.hpp"
#include "bgiplayer/audio_backend.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <memory>
#include <type_traits>

namespace bgiplayer {
struct SnapshotCodec {
    template<class T> static void move_value(T& dst, T& src) {
        if constexpr (requires { transfer(dst, src); }) transfer(dst, src);
        else dst = std::move(src);
    }
#include "bgiplayer/snapshot_schema.inc"

    struct Archive {
        bool reading;
        std::vector<uint8_t>& bytes;
        std::size_t cursor = 0;
        static constexpr uint64_t limit = snapshot_payload_limit;
        void raw(void* ptr, std::size_t size) {
            if (size > limit || cursor > limit - size) throw Error("snapshot size limit exceeded");
            if (reading) {
                if (cursor > bytes.size() || size > bytes.size() - cursor)
                    throw Error("truncated VM snapshot");
                std::memcpy(ptr, bytes.data() + cursor, size);
            } else {
                auto* first = static_cast<uint8_t*>(ptr);
                bytes.insert(bytes.end(), first, first + size);
            }
            cursor += size;
        }
        uint64_t count(uint64_t size) {
            (*this)(size);
            if (size > limit || (reading && size > bytes.size() - cursor))
                throw Error("invalid snapshot container size");
            return size;
        }
        template<class T> void operator()(T& value) {
            if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
                static_assert(std::endian::native == std::endian::little);
                if constexpr (std::is_same_v<T,bool>) {
                    uint8_t byte = value ? 1 : 0; raw(&byte, 1);
                    if (byte > 1) throw Error("invalid snapshot boolean");
                    if (reading) value = byte != 0;
                } else raw(&value, sizeof(value));
            } else SnapshotCodec::fields(*this, value);
        }
        void operator()(std::string& value) {
            const auto size = count(value.size());
            if (reading) value.resize(static_cast<std::size_t>(size));
            if (size) raw(value.data(), static_cast<std::size_t>(size));
        }
        template<class T, class Alloc> void operator()(std::vector<T,Alloc>& value) {
            const auto size = count(value.size());
            if (reading) value.resize(static_cast<std::size_t>(size));
            if constexpr (std::is_same_v<T,uint8_t>) {
                if (size) raw(value.data(), static_cast<std::size_t>(size));
            } else if constexpr (std::is_same_v<T,bool>) {
                for (std::size_t i=0;i<size;++i) { bool bit=value[i]; (*this)(bit); if(reading)value[i]=bit; }
            } else for (auto& item : value) (*this)(item);
        }
        template<class T, class Alloc> void operator()(std::deque<T,Alloc>& value) {
            const auto size = count(value.size());
            if (reading) value.resize(static_cast<std::size_t>(size));
            for (auto& item : value) (*this)(item);
        }
        template<class T, std::size_t N> void operator()(std::array<T,N>& value) {
            for (auto& item : value) (*this)(item);
        }
        template<class K, class V, class C, class A> void operator()(std::map<K,V,C,A>& value) {
            const auto size = count(value.size());
            if (reading) {
                value.clear();
                for(uint64_t i=0;i<size;++i) {
                    K key{}; V item; (*this)(key); (*this)(item);
                    if (!value.emplace(std::move(key),std::move(item)).second)
                        throw Error("duplicate snapshot map key");
                }
            } else for (auto& [key,item] : value) { K copy=key; (*this)(copy); (*this)(item); }
        }
        template<class T, class C, class A> void operator()(std::set<T,C,A>& value) {
            const auto size = count(value.size());
            if (reading) {
                value.clear(); for(uint64_t i=0;i<size;++i) { T item{}; (*this)(item);
                    if(!value.insert(std::move(item)).second) throw Error("duplicate snapshot set item"); }
            } else for(const auto& item:value) { T copy=item; (*this)(copy); }
        }
        template<class T> void operator()(std::optional<T>& value) {
            bool present=value.has_value(); (*this)(present);
            if(reading) { if(present)value.emplace();else value.reset(); }
            if(present)(*this)(*value);
        }
        template<class T> void operator()(std::unique_ptr<T>& value) {
            bool present=bool(value); (*this)(present);
            if(reading) { if(present)value=std::make_unique<T>();else value.reset(); }
            if(present)(*this)(*value);
        }
        template<class T> void operator()(std::shared_ptr<const T>& value) {
            bool present=bool(value); (*this)(present);
            if(reading) {
                if(present){auto item=std::make_shared<T>();(*this)(*item);value=std::move(item);}
                else value.reset();
            } else if(present) { T copy=*value; (*this)(copy); }
        }
        template<class A,class B> void operator()(std::pair<A,B>& value) { (*this)(value.first);(*this)(value.second); }
        template<class... T> void operator()(std::tuple<T...>& value) { std::apply([this](auto&... item){ ((*this)(item),...); },value); }
        template<std::size_t I=0,class... T> void variant_read(std::variant<T...>& value,uint32_t index) {
            if constexpr (I < sizeof...(T)) {
                if(index==I) { value.template emplace<I>(); (*this)(std::get<I>(value)); }
                else variant_read<I+1>(value,index);
            } else throw Error("invalid snapshot variant tag");
        }
        template<class... T> void operator()(std::variant<T...>& value) {
            uint32_t index=static_cast<uint32_t>(value.index()); (*this)(index);
            if(reading)variant_read(value,index);else std::visit([this](auto& item){(*this)(item);},value);
        }
    };

    static uint64_t hash(std::span<const uint8_t> bytes) {
        uint64_t result=14695981039346656037ULL;
        for(uint8_t byte:bytes) { result^=byte;result*=1099511628211ULL; }
        return result;
    }
    static void check_wait(VM& vm, Thread& thread, bool bind) {
        if(thread.wait_procedure && thread.snapshot_wait_kind==SnapshotWaitKind::None)
            throw Error("snapshot cannot serialize an unregistered custom thread wait");
        const auto index=static_cast<std::size_t>(thread.snapshot_wait_index);
        switch(thread.snapshot_wait_kind) {
        case SnapshotWaitKind::None: if(bind)thread.wait_procedure={}; break;
        case SnapshotWaitKind::DisplayControl:
            if(index>=vm.control_display_procedures_.size())throw Error("invalid snapshot display wait");
            if(bind)thread.wait_procedure=[&vm,index]{return vm.advance_control_display_procedure(index,monotonic_ms());}; break;
        case SnapshotWaitKind::ScreenShake:
            if(bind)thread.wait_procedure=[&vm]{vm.advance_screen_shake(monotonic_ms());return WaitProcedureResult{vm.screen_shake_.active?0:1,{}};};break;
        case SnapshotWaitKind::RichText:
            if(index>=vm.rich_text_procedure_states_.size())throw Error("invalid snapshot text wait");
            if(bind)thread.wait_procedure=[&vm,index]{return vm.advance_rich_text_procedure(index);};break;
        case SnapshotWaitKind::SoundLoad:
            if(!vm.sound_.load_tasks_.contains(static_cast<uint32_t>(index)))throw Error("invalid snapshot sound wait");
            if(bind)thread.wait_procedure=[&vm,index]{auto p=vm.sound_.poll_load(static_cast<uint32_t>(index));return WaitProcedureResult{p.status,p.outputs};};break;
        case SnapshotWaitKind::MfMovie:
            if(!vm.movie_procedures_.contains(static_cast<uint32_t>(index)))throw Error("invalid snapshot MF movie wait");
            if(bind)thread.wait_procedure=[&vm,index]{return vm.advance_mf_movie_procedure(static_cast<uint32_t>(index));};break;
        case SnapshotWaitKind::ResourceIntegrity:
            if(!vm.resource_integrity_procedures_.contains(static_cast<uint32_t>(index)))throw Error("invalid snapshot resource integrity wait");
            if(bind)thread.wait_procedure=[&vm,index]{return vm.advance_resource_integrity_procedure(static_cast<uint32_t>(index));};break;
        case SnapshotWaitKind::Codec:
            if(!vm.codec_procedures_.contains(static_cast<uint32_t>(index)))throw Error("invalid snapshot codec wait");
            if(bind)thread.wait_procedure=[&vm,index]{return vm.advance_codec_procedure(static_cast<uint32_t>(index));};break;
        default:throw Error("unknown snapshot wait kind");
        }
    }
    static void waits(VM& vm,bool bind) {
        check_wait(vm,vm.thread_,bind);check_wait(vm,vm.root_container_thread_,bind);
        if(vm.scheduler_saved_thread_)check_wait(vm,*vm.scheduler_saved_thread_,bind);
        for(auto& [id,program]:vm.program_threads_){(void)id;check_wait(vm,program.thread,bind);}
    }
    static void stop_workers(D0Subsystem& d0) {
        for(auto& [id,worker]:d0.workers_){(void)id;d0.stop_worker(*worker);}
    }
    static void save(VM& vm,const std::filesystem::path& path,std::string_view game, SdlAudioBackend* audio) {
        if(!vm.plugins_.empty())throw Error("snapshot cannot checkpoint external plugin state");
        waits(vm,false);
        std::vector<std::unique_lock<std::mutex>> locks;
        for(auto& [id,w]:vm.d0_.workers_) {
            (void)id; locks.emplace_back(w->critical_section);
            if(!w->event.wait_for(locks.back(),std::chrono::seconds(2),[&w]{return w->state==0;}))
                throw Error("snapshot D0 worker did not reach a checkpoint");
        }
        std::unique_lock<std::mutex> audio_lock;
        if(audio)audio_lock=std::unique_lock<std::mutex>(audio->mutex_);
        const uint64_t clock=monotonic_ms();
        std::vector<uint8_t> payload; Archive state{false,payload};state(vm);
        bool has_audio=audio!=nullptr; state(has_audio);
        if(audio)state(*audio);
        auto staging=path; staging += ".tmp";
        try {
            auto out=host_files::open_write(staging);
            if(!out)throw Error("cannot create VM snapshot: "+staging.string());
            write_snapshot_container(out,payload,schema_identity,game,clock,hash(payload),
                                     snapshot_compression_enabled());
            out.close();if(!out)throw Error("cannot finish VM snapshot: "+staging.string());
            host_files::rename(staging,path);
        } catch(...) {
            std::error_code ignored;host_files::remove(staging,ignored);throw;
        }
        restore_snapshot_clock(clock);
    }
    static void load(VM& vm,const std::filesystem::path& path,std::string_view game, SdlAudioBackend* audio) {
        auto in=host_files::open_read(path,std::ios::binary|std::ios::ate);
        if(!in)throw Error("cannot open VM snapshot: "+path.string());
        auto container=read_snapshot_container(in,schema_identity);
        if(container.game_identity!=game && !allow_content_check_failure("VM snapshot game-resource identity mismatch"))
            throw Error("VM snapshot belongs to different game resources");
        auto& bytes=container.payload;
        if(hash(bytes)!=container.checksum &&
           !allow_content_check_failure("VM snapshot payload checksum mismatch"))
            throw Error("VM snapshot checksum mismatch");
        const auto timestamp=container.timestamp;
        VM candidate({}); Archive state{true,bytes};state(candidate);
        bool has_audio=false;state(has_audio);SdlAudioBackend candidate_audio;
        if(has_audio) {if(!audio)throw Error("snapshot requires audio host state");state(candidate_audio);}
        if(state.cursor!=bytes.size())throw Error("trailing VM snapshot data");
        if(candidate.memory_.code_!=vm.memory_.code_ &&
           !allow_content_check_failure("VM snapshot startup-code content mismatch"))
            throw Error("VM snapshot startup code mismatch");
        if(!candidate.plugins_.empty())throw Error("VM snapshot contains external plugin state");
        waits(candidate,false);
        // Validate the owning code buffers before installing non-owning views.
        (void)candidate.current_code();
        std::vector<uint64_t> opened;
        try {
            for(auto& [id,mutex]:candidate.named_mutexes_) {
                (void)id;
                if(!vm.named_mutex_hooks_.open||!vm.named_mutex_hooks_.close)throw Error("snapshot requires named mutex host hooks");
                auto handle=vm.named_mutex_hooks_.open(mutex.name);
                if(!handle)throw Error("cannot restore named mutex");mutex.handle=*handle;opened.push_back(*handle);
            }
        } catch(...) {for(auto handle:opened)vm.named_mutex_hooks_.close(handle);throw;}
        stop_workers(vm.d0_);
        for(auto& [id,mutex]:vm.named_mutexes_){(void)id;if(vm.named_mutex_hooks_.close)vm.named_mutex_hooks_.close(mutex.handle);}
        transfer(vm,candidate);
        if(has_audio) {std::scoped_lock audio_lock(audio->mutex_);transfer(*audio,candidate_audio);}
        // IPL registers aliases in both the VM and its host ArchiveSet. A
        // new process retains the registrar closure, but starts with an empty
        // host alias table; uncached reads must work before any thread resumes.
        if(vm.composite_archive_registrar_) {
            for(const auto& [name,components]:vm.composite_archives_)
                vm.composite_archive_registrar_(name,components);
        }
        vm.memory_.set_active_code(vm.has_active_code_resource_?vm.current_code():std::span<const uint8_t>{},vm.active_code_resource_base_);
        vm.sync_code_fallbacks();waits(vm,true);
        restore_snapshot_clock(timestamp);
        for(auto& [id,w]:vm.d0_.workers_) {(void)id;w->shutdown=false;w->thread=std::thread([&d0=vm.d0_,ptr=w.get()]{d0.worker_main(*ptr);});}
    }
};
void save_vm_snapshot(VM& vm,const std::filesystem::path& path,std::string_view game_identity,SdlAudioBackend* audio) { SnapshotCodec::save(vm,path,game_identity,audio); }
void load_vm_snapshot(VM& vm,const std::filesystem::path& path,std::string_view game_identity,SdlAudioBackend* audio) { SnapshotCodec::load(vm,path,game_identity,audio); }
}
