#pragma once

// 注入检查的跨进程 x64 栈展开。
//
// 这一层只做采集，**不判可靠性**：它把"展开到了哪儿"和"这个 PC 在不在带展开数据
// 的映像里"两项事实交给 shared/evidence 的 AdmitStackFrames 去判。判据放那边是因为
// 那边有离线测试，这边没有。
//
// 为什么不自己实现 x64 展开：应用 .pdata/.xdata 的展开码要正确处理非易失寄存器、
// UWOP_SET_FPREG、链式 unwind info 与尾声。写错的展开器会把**错的帧标成可靠**，
// 而"可靠帧进入载荷内存"是把结论抬到 DifferenceObserved 的三道闸门之一 ——
// 错在这里比没有栈回溯糟得多。所以展开本身交给系统自己的 StackWalk64，
// 我们只提供内存读取、模块基址与 RUNTIME_FUNCTION 三个回调。
//
// 为什么不挂起目标：本功能的硬约束是只读、不干扰。取而代之的是**只认等待中的线程**
// —— 微软那句"运行中的线程取不到有效上下文"针对的是正在别的核上跑的线程，
// 阻塞在等待里的线程上下文本来就是稳定的。而最值得查的 shellcode 形态（打个盹
// 再醒的信标）恰好就停在等待里。

#include "../../../../shared/evidence/InjectionSurvey.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ks::process
{
    struct StackWalkOptions final
    {
        std::size_t maxThreads = 64U;
        // 单个线程最多展开多少帧。正常调用栈几十帧就到底了；上限只用来防畸形栈里
        // 打转，命中上限会写进 terminationReason，不会被当成"走到栈底了"。
        std::size_t maxFrames = 64U;
    };

    struct StackWalkResult final
    {
        std::vector<Ksword::Evidence::ThreadStackInput> stacks;

        std::uint32_t threadsConsidered = 0U;   // 枚举到的本进程线程数
        std::uint32_t threadsWaiting = 0U;      // 其中前后两次查都在等待的
        std::uint32_t threadsWalked = 0U;       // 实际走了展开的
        bool attempted = false;                 // false 表示整节没做（架构不支持等）
        std::string diagnostic;                 // 只在整节没做成时非空

        bool completed() const noexcept { return attempted && !stacks.empty(); }
    };

    // process 需要 PROCESS_QUERY_INFORMATION | PROCESS_VM_READ。
    // 只支持原生 x64 目标：WOW64 与其它架构直接返回 attempted=false + 诊断，
    // 不做"用 x64 展开器硬走 x86 栈"这种会产出垃圾帧的降级。
    StackWalkResult WalkProcessStacks(HANDLE process,
                                      std::uint32_t pid,
                                      const Ksword::Evidence::ProcessInstanceId& owner,
                                      Ksword::Evidence::ProcessArchitecture architecture,
                                      const StackWalkOptions& options);
}
