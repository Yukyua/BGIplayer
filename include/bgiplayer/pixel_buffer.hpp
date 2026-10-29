#pragma once
#include "bgiplayer/pixel_storage.hpp"
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>
namespace bgiplayer {
// Native bitmap bytes with value semantics. Immutable GPU storage is shared on
// copy; a mutable CPU access detaches before returning a writable pointer.
class PixelBuffer {
    mutable std::vector<uint8_t> cpu_;
    mutable std::shared_ptr<GpuPixelStorage> gpu_;
    void materialize(bool write) const {
        if(cpu_.empty() && gpu_) {
            std::vector<uint8_t> bytes(gpu_->size());
            if(!gpu_->read(bytes))throw std::runtime_error("GPU bitmap readback failed");
            cpu_=std::move(bytes);
        }
        if(write)gpu_.reset();
    }
public:
    using value_type=uint8_t;
    using iterator=std::vector<uint8_t>::iterator;
    using const_iterator=std::vector<uint8_t>::const_iterator;
    using reverse_iterator=std::vector<uint8_t>::reverse_iterator;
    using const_reverse_iterator=std::vector<uint8_t>::const_reverse_iterator;
    PixelBuffer()=default;
    PixelBuffer(const PixelBuffer& other):gpu_(other.gpu_) {if(!gpu_)cpu_=other.cpu_;}
    PixelBuffer(PixelBuffer&&)=default;
    explicit PixelBuffer(const std::vector<uint8_t>& bytes):cpu_(bytes) {}
    explicit PixelBuffer(std::vector<uint8_t>&& bytes):cpu_(std::move(bytes)) {}
    PixelBuffer(std::initializer_list<uint8_t> bytes):cpu_(bytes) {}
    PixelBuffer& operator=(const PixelBuffer& other) {if(this!=&other){PixelBuffer copy(other);swap(copy);}return *this;}
    PixelBuffer& operator=(PixelBuffer&&)=default;
    PixelBuffer& operator=(const std::vector<uint8_t>& bytes) {cpu_=bytes;gpu_.reset();return *this;}
    PixelBuffer& operator=(std::vector<uint8_t>&& bytes) {cpu_=std::move(bytes);gpu_.reset();return *this;}
    PixelBuffer& operator=(std::initializer_list<uint8_t> bytes) {cpu_=bytes;gpu_.reset();return *this;}
    size_t size() const {return cpu_.empty() && gpu_?gpu_->size():cpu_.size();}
    bool empty() const {return size()==0;}
    size_t capacity() const {return cpu_.capacity();}
    size_t gpu_bytes() const {return gpu_?gpu_->resident_bytes():0;}
    const std::shared_ptr<GpuPixelStorage>& gpu_storage() const {return gpu_;}
    std::span<const uint8_t> upload_bytes() const {return cpu_;}
    bool adopt_gpu_storage(std::shared_ptr<GpuPixelStorage> storage) {
        if(!storage || storage->size()!=size())return false;
        gpu_=std::move(storage);std::vector<uint8_t>().swap(cpu_);return true;
    }
    void replace_gpu_storage(std::shared_ptr<GpuPixelStorage> storage) {
        gpu_=std::move(storage);std::vector<uint8_t>().swap(cpu_);
    }
    uint8_t* data() {materialize(true);return cpu_.data();}
    const uint8_t* data() const {materialize(false);return cpu_.data();}
    uint8_t& operator[](size_t at) {materialize(true);return cpu_[at];}
    const uint8_t& operator[](size_t at) const {materialize(false);return cpu_[at];}
    uint8_t& front() {materialize(true);return cpu_.front();}
    const uint8_t& front() const {materialize(false);return cpu_.front();}
    uint8_t& back() {materialize(true);return cpu_.back();}
    const uint8_t& back() const {materialize(false);return cpu_.back();}
    uint8_t& at(size_t n) {materialize(true);return cpu_.at(n);}
    const uint8_t& at(size_t n) const {materialize(false);return cpu_.at(n);}
    iterator begin() {materialize(true);return cpu_.begin();}
    iterator end() {materialize(true);return cpu_.end();}
    const_iterator begin() const {materialize(false);return cpu_.begin();}
    const_iterator end() const {materialize(false);return cpu_.end();}
    const_iterator cbegin() const {return begin();}
    const_iterator cend() const {return end();}
    reverse_iterator rbegin() {materialize(true);return cpu_.rbegin();}
    reverse_iterator rend() {materialize(true);return cpu_.rend();}
    const_reverse_iterator rbegin() const {materialize(false);return cpu_.rbegin();}
    const_reverse_iterator rend() const {materialize(false);return cpu_.rend();}
    size_t max_size() const {return cpu_.max_size();}
    void reserve(size_t n) {materialize(true);cpu_.reserve(n);}
    void shrink_to_fit() {materialize(true);cpu_.shrink_to_fit();}
    void push_back(uint8_t byte) {materialize(true);cpu_.push_back(byte);}
    void pop_back() {materialize(true);cpu_.pop_back();}
    iterator erase(const_iterator at) {materialize(true);return cpu_.erase(at);}
    iterator erase(const_iterator first,const_iterator last) {materialize(true);return cpu_.erase(first,last);}
    iterator insert(const_iterator at,uint8_t byte) {materialize(true);return cpu_.insert(at,byte);}
    template<class I> iterator insert(const_iterator at,I first,I last) {materialize(true);return cpu_.insert(at,first,last);}
    // Full replacement never needs to download the previous GPU contents.
    uint8_t* overwrite(size_t size) {gpu_.reset();cpu_.resize(size);return cpu_.data();}
    void clear() {cpu_.clear();gpu_.reset();}
    void resize(size_t size) {if(size==this->size())return;if(!size){clear();return;}materialize(true);cpu_.resize(size);}
    void resize(size_t size,uint8_t value) {if(size==this->size())return;if(!size){clear();return;}materialize(true);cpu_.resize(size,value);}
    template<class I> void assign(I first,I last) {gpu_.reset();cpu_.assign(first,last);}
    void assign(size_t count,uint8_t value) {gpu_.reset();cpu_.assign(count,value);}
    void swap(PixelBuffer& other) noexcept {cpu_.swap(other.cpu_);gpu_.swap(other.gpu_);}
    void swap(std::vector<uint8_t>& other) {materialize(true);cpu_.swap(other);}
    const std::vector<uint8_t>& cpu_pixels() const {materialize(false);return cpu_;}
    std::vector<uint8_t>& cpu_pixels() {materialize(true);return cpu_;}
    operator const std::vector<uint8_t>&() const & {return cpu_pixels();}
    operator std::vector<uint8_t>&() & {return cpu_pixels();}
    std::vector<uint8_t> take_cpu_pixels() && {materialize(true);return std::move(cpu_);}
    bool offload() {
        if(cpu_.empty() && !gpu_) {std::vector<uint8_t>().swap(cpu_);return true;}
        if(!gpu_ && !cpu_.empty())gpu_=store_host_pixel_storage(cpu_);
        if(!gpu_)return cpu_.empty();
        std::vector<uint8_t>().swap(cpu_);return true;
    }
    friend bool operator==(const PixelBuffer& a,const PixelBuffer& b) {return a.cpu_pixels()==b.cpu_pixels();}
    friend bool operator==(const PixelBuffer& a,const std::vector<uint8_t>& b) {return a.cpu_pixels()==b;}
    friend bool operator==(const std::vector<uint8_t>& a,const PixelBuffer& b) {return a==b.cpu_pixels();}
};
}
