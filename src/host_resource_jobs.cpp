#include "bgiplayer/host_resource_jobs.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cfenv>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <system_error>
#include <vector>

namespace bgiplayer {
namespace {
thread_local bool inside_resource_job=false;
struct Group {
    const std::function<void(size_t)>& prepare;
    std::vector<std::exception_ptr> errors;
    std::atomic<size_t> next{0};
    std::fenv_t environment{};
    bool valid_environment=false;
    explicit Group(size_t count,const std::function<void(size_t)>& fn):prepare(fn),errors(count),
        valid_environment(std::fegetenv(&environment)==0){}
    void drain() {
        const bool previous=inside_resource_job;inside_resource_job=true;
        std::fenv_t saved{};const bool restore=valid_environment && std::fegetenv(&saved)==0;
        if(restore)std::fesetenv(&environment);
        for(size_t n=next.fetch_add(1,std::memory_order_relaxed);n<errors.size();
                n=next.fetch_add(1,std::memory_order_relaxed)) {
            try{prepare(n);}catch(...){errors[n]=std::current_exception();}
        }
        if(restore)std::fesetenv(&saved);
        inside_resource_job=previous;
    }
};
class ResourceWorkers {
    std::mutex dispatch_,mutex_;
    std::condition_variable wake_,done_;
    std::array<std::thread,3> workers_;
    Group* group_=nullptr;
    size_t generation_=0,remaining_=0,participating_=0;
    bool stopping_=false;
public:
    ResourceWorkers() {
        try {
            for(size_t n=0;n<workers_.size();++n)workers_[n]=std::thread([this,n] {
                size_t generation=0;
                for(;;) {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock,[&]{return stopping_ || generation_!=generation;});
                    if(stopping_)return;
                    generation=generation_;
                    if(n>=participating_)continue;
                    auto* group=group_;lock.unlock();group->drain();lock.lock();
                    if(--remaining_==0)done_.notify_one();
                }
            });
        } catch(...) {stop();throw;}
    }
    ~ResourceWorkers(){stop();}
    void stop() {
        {std::lock_guard lock(mutex_);stopping_=true;}
        wake_.notify_all();
        for(auto& thread:workers_)if(thread.joinable())thread.join();
    }
    void run(Group& group,size_t workers) {
        std::unique_lock dispatch(dispatch_);
        {
            std::lock_guard lock(mutex_);
            group_=&group;remaining_=participating_=workers;++generation_;
        }
        wake_.notify_all();group.drain();
        std::unique_lock lock(mutex_);
        done_.wait(lock,[&]{return remaining_==0;});group_=nullptr;
    }
};
size_t worker_count(size_t count,size_t pixels) {
    if(count<2 || inside_resource_job)return 1;
    const char* configured=std::getenv("BGI_RESOURCE_WORKERS");
    if(configured) {
        char* end=nullptr;const auto n=std::strtol(configured,&end,10);
        if(end!=configured && *end=='\0' && n>=0 && n<=4)
            return std::min(count,static_cast<size_t>(std::max(1L,n)));
    }
    if(pixels<65536)return 1;
    return std::min({count,size_t{4},size_t(std::max(1U,std::thread::hardware_concurrency()))});
}
}
void prepare_resource_parts(size_t count,size_t work_pixels,
        const std::function<void(size_t)>& prepare) {
    const auto threads=worker_count(count,work_pixels);
    if(threads<=1) {
        for(size_t n=0;n<count;++n)prepare(n);
        return;
    }
    Group group(count,prepare);
    // One caller plus at most three sleeping, reusable workers. No GPU/VM
    // access or callback publication is permitted from these pure decode jobs.
    ResourceWorkers* workers=nullptr;
    try {static ResourceWorkers instance;workers=&instance;}
    catch(const std::system_error&) {
        // A host thread quota must not reject an otherwise valid resource.
        for(size_t n=0;n<count;++n)prepare(n);
        return;
    }
    workers->run(group,threads-1);
    for(const auto& error:group.errors)if(error)std::rethrow_exception(error);
}
}
