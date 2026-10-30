#include "bgiplayer/vm.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
using namespace bgiplayer;
int main() {
    // pushd0; loop: pushb1; add; relative-code-pointer(loop); jump.
    const std::vector<uint8_t> code{2,0,0,0,0,0,1,0x20,6,0xfd,0xff,0x14};
    for(bool resource:{false,true}) {
        std::vector<double> times;
        for(unsigned sample=0;sample<11;++sample) {
            VM vm(resource?std::vector<uint8_t>{2,0,0,0,0,0x14}:code);
            if(resource) {
                vm.set_resource_loader([&](auto,auto){std::vector<uint8_t> data{8,0,0,0,uint8_t(code.size()),0,0,0};data.insert(data.end(),code.begin(),code.end());return data;});
                const uint32_t address=vm.load_resource("bench","loop");
                vm.main_thread().stack.push(address);
                // Start via the root jump without changing benchmarked opcodes.
                vm.main_thread().ip=5;if(vm.step()==VM::StepResult::Fault)return 2;
            }
            constexpr size_t instructions=400001;
            const auto start=std::chrono::steady_clock::now();size_t count=0;
            if(vm.run(instructions,&count)==VM::StepResult::Fault || count!=instructions || vm.main_thread().stack.pop()!=100000)return 3;
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        std::sort(times.begin(),times.end());
        std::cout<<"VM_BENCH resource="<<resource<<" instructions=400001 median_ms="<<times[5]<<'\n';
    }
    {
        std::vector<double> times;
        for(unsigned sample=0;sample<11;++sample) {
            VM vm(std::vector<uint8_t>(8192,0));
            auto fill=[](Thread& thread) {
                thread.state=ThreadState::Waiting;thread.status_flags=1;
                thread.wait_procedure=[] {return WaitProcedureResult{0,{}};};
                for(uint32_t n=0;n<512;++n){thread.stack.push(n);thread.command_queue.push_back(n);}
            };
            fill(vm.main_thread());
            for(unsigned n=0;n<20;++n) {
                const auto id=vm.create_program_thread("idle",std::vector<uint8_t>(8192,0),1024,8192,64);
                fill(vm.program_threads().at(id).thread);
            }
            const auto start=std::chrono::steady_clock::now();
            for(unsigned pass=0;pass<1000;++pass)vm.service_scheduler_pass();
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            if(vm.main_thread().stack.size()!=512 || vm.main_thread().command_queue.size()!=512)return 4;
        }
        std::sort(times.begin(),times.end());
        std::cout<<"VM_SCHED_BENCH threads=21 passes=1000 median_ms="<<times[5]<<'\n';
    }

}
