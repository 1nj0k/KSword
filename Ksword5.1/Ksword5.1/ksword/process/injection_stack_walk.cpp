#include "injection_stack_walk.h"

#include "../dbghelp_serialization.h"

#include <DbgHelp.h>
#include <TlHelp32.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

namespace ev = Ksword::Evidence;

namespace ks::process
{
    namespace
    {
        // --- NtQuerySystemInformation：线程状态 ---------------------------------
        // 取线程是不是停在等待里。Toolhelp 给不了这个，GetThreadContext 也不会告诉你
        // 它拿到的是不是一个正在变的快照。

        using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

        constexpr ULONG kSystemProcessInformation = 5UL;
        // KTHREAD_STATE::Waiting。取上下文要求线程**停着**，只有这一个值算数：
        // Ready/Running/Standby 都表示它随时可能在别的核上继续跑。
        constexpr ULONG kThreadStateWaiting = 5UL;

        struct SystemThreadInformation final
        {
            LARGE_INTEGER kernelTime;
            LARGE_INTEGER userTime;
            LARGE_INTEGER createTime;
            ULONG waitTime;
            PVOID startAddress;
            struct
            {
                HANDLE uniqueProcess;
                HANDLE uniqueThread;
            } clientId;
            LONG priority;
            LONG basePriority;
            ULONG contextSwitches;
            ULONG threadState;
            ULONG waitReason;
        };

        // UNICODE_STRING 声明在 winternl.h/ntdef.h 里，本文件只 include Windows.h。
        // 这里只需要它的**布局**（进程名我们不读），按公开文档手写。
        struct UnicodeStringField final
        {
            USHORT length;
            USHORT maximumLength;
            PWSTR buffer;
        };

        struct SystemProcessInformationHeader final
        {
            ULONG nextEntryOffset;
            ULONG numberOfThreads;
            LARGE_INTEGER workingSetPrivateSize;
            ULONG hardFaultCount;
            ULONG numberOfThreadsHighWatermark;
            ULONGLONG cycleTime;
            LARGE_INTEGER createTime;
            LARGE_INTEGER userTime;
            LARGE_INTEGER kernelTime;
            UnicodeStringField imageName;
            LONG basePriority;
            HANDLE uniqueProcessId;
            HANDLE inheritedFromUniqueProcessId;
            ULONG handleCount;
            ULONG sessionId;
            ULONG_PTR uniqueProcessKey;
            SIZE_T peakVirtualSize;
            SIZE_T virtualSize;
            ULONG pageFaultCount;
            SIZE_T peakWorkingSetSize;
            SIZE_T workingSetSize;
            SIZE_T quotaPeakPagedPoolUsage;
            SIZE_T quotaPagedPoolUsage;
            SIZE_T quotaPeakNonPagedPoolUsage;
            SIZE_T quotaNonPagedPoolUsage;
            SIZE_T pagefileUsage;
            SIZE_T peakPagefileUsage;
            SIZE_T privatePageCount;
            LARGE_INTEGER readOperationCount;
            LARGE_INTEGER writeOperationCount;
            LARGE_INTEGER otherOperationCount;
            LARGE_INTEGER readTransferCount;
            LARGE_INTEGER writeTransferCount;
            LARGE_INTEGER otherTransferCount;
        };

        NtQuerySystemInformationFn ResolveNtQuerySystemInformation()
        {
            static const NtQuerySystemInformationFn fn = []() -> NtQuerySystemInformationFn {
                const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
                if (ntdll == nullptr)
                {
                    return nullptr;
                }
                return reinterpret_cast<NtQuerySystemInformationFn>(
                    reinterpret_cast<void*>(
                        ::GetProcAddress(ntdll, "NtQuerySystemInformation")));
            }();
            return fn;
        }

        // tid -> threadState。查不到就是查不到，返回空表而不是"全都算等待"。
        std::map<std::uint32_t, ULONG> QueryThreadStates(const std::uint32_t pid)
        {
            std::map<std::uint32_t, ULONG> states;
            const NtQuerySystemInformationFn query = ResolveNtQuerySystemInformation();
            if (query == nullptr)
            {
                return states;
            }

            // 进程表在两次调用之间会变大，所以按返回的需求量重试几轮。
            ULONG needed = 512U * 1024U;
            std::vector<std::uint8_t> buffer;
            LONG status = 0;
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                buffer.assign(needed, 0U);
                ULONG produced = 0U;
                status = query(kSystemProcessInformation, buffer.data(),
                               static_cast<ULONG>(buffer.size()), &produced);
                if (status >= 0)
                {
                    break;
                }
                // STATUS_INFO_LENGTH_MISMATCH：按内核报的量再加一截余量重来。
                needed = (produced > needed ? produced : needed * 2U) + 64U * 1024U;
            }
            if (status < 0)
            {
                return states;
            }

            std::size_t offset = 0U;
            while (offset + sizeof(SystemProcessInformationHeader) <= buffer.size())
            {
                const auto* const header =
                    reinterpret_cast<const SystemProcessInformationHeader*>(buffer.data() + offset);
                if (reinterpret_cast<ULONG_PTR>(header->uniqueProcessId) ==
                    static_cast<ULONG_PTR>(pid))
                {
                    const std::size_t threadsOffset =
                        offset + sizeof(SystemProcessInformationHeader);
                    for (ULONG index = 0U; index < header->numberOfThreads; ++index)
                    {
                        const std::size_t at =
                            threadsOffset + index * sizeof(SystemThreadInformation);
                        if (at + sizeof(SystemThreadInformation) > buffer.size())
                        {
                            break;
                        }
                        const auto* const thread =
                            reinterpret_cast<const SystemThreadInformation*>(buffer.data() + at);
                        states[static_cast<std::uint32_t>(
                            reinterpret_cast<ULONG_PTR>(thread->clientId.uniqueThread))] =
                            thread->threadState;
                    }
                    break;
                }
                if (header->nextEntryOffset == 0U)
                {
                    break;
                }
                offset += header->nextEntryOffset;
            }
            return states;
        }

        // --- 每个模块的 .pdata ---------------------------------------------------

        struct ModuleUnwindTable final
        {
            std::uint64_t imageBase = 0U;
            std::uint64_t imageSize = 0U;
            // 从目标读回来的 RUNTIME_FUNCTION 表，按 BeginAddress 升序（PE 规范要求
            // 它本来就是有序的；不额外排序，乱序的表说明映像不可信，宁可查不到）。
            std::vector<RUNTIME_FUNCTION> functions;
            // 映像里有没有非空的异常目录。这一位才是判"下一帧靠不靠展开数据"的依据：
            // 在这样的映像里，函数要么有表项，要么是叶函数（ABI 保证 [RSP] 就是返回
            // 地址），两种情况下一帧都是被算出来的而不是猜的。
            bool hasExceptionDirectory = false;
        };

        // 走一次展开期间的全部状态。DbgHelp 的回调签名里没有用户上下文指针，
        // 只能靠 thread_local 把当前 walker 递进去 —— 这是这套 API 的标准用法。
        class StackWalkContext final
        {
        public:
            explicit StackWalkContext(HANDLE process) : m_process(process) {}

            bool readMemory(const DWORD64 address, void* const buffer, const DWORD size,
                            LPDWORD readOut)
            {
                SIZE_T read = 0U;
                const BOOL ok = ::ReadProcessMemory(
                    m_process, reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(address)),
                    buffer, size, &read);
                if (readOut != nullptr)
                {
                    *readOut = static_cast<DWORD>(read);
                }
                return ok != FALSE && read == size;
            }

            // 返回 PC 所属映像的基址；不在映像里（私有内存、映射的非映像）返回 0。
            std::uint64_t moduleBase(const DWORD64 address)
            {
                const ModuleUnwindTable* const table = tableFor(address);
                return table == nullptr ? 0U : table->imageBase;
            }

            // StackWalk64 要一个**本进程地址空间里**有效的 RUNTIME_FUNCTION 指针，
            // 所以返回的是缓存里那一份的地址，缓存活到整次展开结束。
            PRUNTIME_FUNCTION functionEntry(const DWORD64 address)
            {
                const ModuleUnwindTable* const table = tableFor(address);
                if (table == nullptr || table->functions.empty())
                {
                    return nullptr;
                }
                const auto rva = static_cast<std::uint32_t>(address - table->imageBase);
                // 找最后一个 BeginAddress <= rva 的表项，再验 rva 真的落在它里面。
                const auto hit = std::upper_bound(
                    table->functions.begin(), table->functions.end(), rva,
                    [](const std::uint32_t value, const RUNTIME_FUNCTION& entry) {
                        return value < entry.BeginAddress;
                    });
                if (hit == table->functions.begin())
                {
                    return nullptr;
                }
                const RUNTIME_FUNCTION& entry = *(hit - 1);
                if (rva >= entry.EndAddress)
                {
                    // 落在两个函数之间：这是叶函数或表里没有的代码，不是表项。
                    return nullptr;
                }
                return const_cast<PRUNTIME_FUNCTION>(&entry);
            }

            // 这个 PC 所在的映像有没有异常目录。见 ModuleUnwindTable 的注释。
            bool pcCoveredByUnwindData(const std::uint64_t address)
            {
                const ModuleUnwindTable* const table = tableFor(address);
                return table != nullptr && table->hasExceptionDirectory;
            }

        private:
            const ModuleUnwindTable* tableFor(const DWORD64 address)
            {
                MEMORY_BASIC_INFORMATION info{};
                if (::VirtualQueryEx(m_process,
                                     reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(address)),
                                     &info, sizeof(info)) != sizeof(info))
                {
                    return nullptr;
                }
                if (info.Type != MEM_IMAGE || info.AllocationBase == nullptr)
                {
                    // 私有内存 / 非映像映射 —— 这正是 shellcode 所在。没有展开数据。
                    return nullptr;
                }
                const auto base =
                    static_cast<std::uint64_t>(reinterpret_cast<ULONG_PTR>(info.AllocationBase));
                const auto cached = m_tables.find(base);
                if (cached != m_tables.end())
                {
                    return cached->second ? &*cached->second : nullptr;
                }
                std::unique_ptr<ModuleUnwindTable> table = loadTable(base);
                const ModuleUnwindTable* const result = table ? table.get() : nullptr;
                m_tables.emplace(base, std::move(table));
                return result;
            }

            std::unique_ptr<ModuleUnwindTable> loadTable(const std::uint64_t base)
            {
                // 头：DOS -> NT。只读需要的那几个字段，PE 的完整解析归 PeImageMap，
                // 这里既不做重定位也不做节映射。
                std::uint8_t headers[0x1000] = {};
                SIZE_T read = 0U;
                if (::ReadProcessMemory(m_process,
                                        reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(base)),
                                        headers, sizeof(headers), &read) == FALSE ||
                    read < sizeof(IMAGE_DOS_HEADER))
                {
                    return nullptr;
                }
                const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(headers);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
                {
                    return nullptr;
                }
                const auto ntOffset = static_cast<std::size_t>(dos->e_lfanew);
                if (ntOffset + sizeof(IMAGE_NT_HEADERS64) > read)
                {
                    return nullptr;
                }
                const auto* const nt =
                    reinterpret_cast<const IMAGE_NT_HEADERS64*>(headers + ntOffset);
                if (nt->Signature != IMAGE_NT_SIGNATURE ||
                    nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
                    nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                {
                    return nullptr;
                }

                auto table = std::make_unique<ModuleUnwindTable>();
                table->imageBase = base;
                table->imageSize = nt->OptionalHeader.SizeOfImage;

                if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
                {
                    return table;  // 没有异常目录：合法，但下一帧就只能靠猜。
                }
                const IMAGE_DATA_DIRECTORY& directory =
                    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (directory.VirtualAddress == 0U || directory.Size < sizeof(RUNTIME_FUNCTION))
                {
                    return table;
                }
                // 目录必须整个落在映像里，否则这份头不可信，整块不要。
                if (static_cast<std::uint64_t>(directory.VirtualAddress) + directory.Size >
                    table->imageSize)
                {
                    return table;
                }

                const std::size_t count = directory.Size / sizeof(RUNTIME_FUNCTION);
                if (count == 0U || count > kMaxRuntimeFunctions)
                {
                    return table;
                }
                std::vector<RUNTIME_FUNCTION> functions(count);
                SIZE_T tableRead = 0U;
                if (::ReadProcessMemory(
                        m_process,
                        reinterpret_cast<LPCVOID>(
                            static_cast<ULONG_PTR>(base + directory.VirtualAddress)),
                        functions.data(), count * sizeof(RUNTIME_FUNCTION), &tableRead) == FALSE ||
                    tableRead != count * sizeof(RUNTIME_FUNCTION))
                {
                    return table;
                }
                table->functions = std::move(functions);
                table->hasExceptionDirectory = true;
                return table;
            }

            // ntdll 的 .pdata 约两万条；给三倍余量，再大就是头被改过。
            static constexpr std::size_t kMaxRuntimeFunctions = 65536U;

            HANDLE m_process = nullptr;
            // value 为空指针表示"这个基址查过了，没有可用的表"，避免反复重读。
            std::map<std::uint64_t, std::unique_ptr<ModuleUnwindTable>> m_tables;
        };

        thread_local StackWalkContext* t_context = nullptr;

        BOOL __stdcall ReadMemoryCallback(HANDLE, const DWORD64 address, PVOID buffer,
                                          const DWORD size, LPDWORD readOut)
        {
            if (t_context == nullptr)
            {
                return FALSE;
            }
            return t_context->readMemory(address, buffer, size, readOut) ? TRUE : FALSE;
        }

        PVOID __stdcall FunctionTableAccessCallback(HANDLE, const DWORD64 address)
        {
            if (t_context == nullptr)
            {
                return nullptr;
            }
            return t_context->functionEntry(address);
        }

        DWORD64 __stdcall GetModuleBaseCallback(HANDLE, const DWORD64 address)
        {
            if (t_context == nullptr)
            {
                return 0U;
            }
            return t_context->moduleBase(address);
        }

        struct ScopedThreadHandle final
        {
            HANDLE handle = nullptr;
            ~ScopedThreadHandle()
            {
                if (handle != nullptr)
                {
                    ::CloseHandle(handle);
                }
            }
        };
    }

    StackWalkResult WalkProcessStacks(HANDLE process,
                                      const std::uint32_t pid,
                                      const ev::ProcessInstanceId& owner,
                                      const ev::ProcessArchitecture architecture,
                                      const StackWalkOptions& options)
    {
        StackWalkResult result;

        // 只做原生 x64。用 x64 展开器去走 WOW64 的 32 位栈会产出一堆看着像帧的垃圾，
        // 而垃圾帧一旦进了可靠前缀就会凭空抬高结论。宁可整节不做。
        if (architecture != ev::ProcessArchitecture::X64)
        {
            result.diagnostic = "stack walk skipped: target is not native x64";
            return result;
        }
        if (process == nullptr)
        {
            result.diagnostic = "stack walk skipped: no process handle";
            return result;
        }

        const std::map<std::uint32_t, ULONG> statesBefore = QueryThreadStates(pid);
        if (statesBefore.empty())
        {
            // 查不到线程状态就没法判上下文可不可信。不做，而不是当成"都在等待"。
            result.diagnostic = "stack walk skipped: thread states unavailable";
            return result;
        }
        result.attempted = true;

        const ScopedThreadHandle snapshot{ ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0U) };
        if (snapshot.handle == INVALID_HANDLE_VALUE || snapshot.handle == nullptr)
        {
            result.diagnostic = "stack walk skipped: thread snapshot failed";
            result.attempted = false;
            return result;
        }

        std::vector<std::uint32_t> candidates;
        std::uint32_t matchedStates = 0U;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (::Thread32First(snapshot.handle, &entry) != FALSE)
        {
            do
            {
                if (entry.th32OwnerProcessID != pid)
                {
                    continue;
                }
                ++result.threadsConsidered;
                const auto state = statesBefore.find(entry.th32ThreadID);
                if (state == statesBefore.end())
                {
                    continue;
                }
                ++matchedStates;
                if (state->second != kThreadStateWaiting)
                {
                    continue;
                }
                ++result.threadsWaiting;
                if (candidates.size() < options.maxThreads)
                {
                    candidates.push_back(entry.th32ThreadID);
                }
            } while (::Thread32Next(snapshot.handle, &entry) != FALSE);
        }

        // SYSTEM_PROCESS_INFORMATION / SYSTEM_THREAD_INFORMATION 的布局是按公开文档
        // 手写的，写错了的表现是"解析出一堆垃圾 tid"—— 那会安静地退化成"没有等待中的
        // 线程"，和"这个进程确实都在跑"长得一模一样。所以这里立一道判据：Toolhelp
        // 数出来的线程，必须有相当一部分能在状态表里找到。一个都对不上就是布局错了，
        // 明确报出来，不冒充"查过了没发现"。
        if (result.threadsConsidered != 0U && matchedStates * 2U < result.threadsConsidered)
        {
            result.attempted = false;
            result.threadsWaiting = 0U;
            result.diagnostic = "stack walk skipped: thread state layout mismatch";
            return result;
        }

        if (candidates.empty())
        {
            return result;  // attempted=true 且 stacks 为空 —— 判据层会记成缺口。
        }

        StackWalkContext context(process);
        // DbgHelp 整个 DLL 单线程，锁必须盖住全部 StackWalk64 调用。
        std::lock_guard<std::mutex> dbgHelpLock(ks::dbghelp::SerializationMutex());
        t_context = &context;
        struct ContextReset final
        {
            ~ContextReset() { t_context = nullptr; }
        } contextReset;

        for (const std::uint32_t tid : candidates)
        {
            ev::ThreadStackInput stack;
            stack.thread.process = owner;
            stack.thread.tid = ev::OptionalU64::of(static_cast<std::uint64_t>(tid));

            const ScopedThreadHandle thread{
                ::OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid)
            };
            if (thread.handle == nullptr)
            {
                const DWORD error = ::GetLastError();
                stack.outcome = ev::CollectionOutcome::failure(
                    ev::CollectionStatus::AccessDenied, "Win32",
                    static_cast<std::uint64_t>(error), std::string());
                result.stacks.push_back(std::move(stack));
                continue;
            }

            // CONTEXT 要求 16 字节对齐；类型自带 DECLSPEC_ALIGN(16)，栈上局部量满足。
            CONTEXT threadContext{};
            threadContext.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (::GetThreadContext(thread.handle, &threadContext) == FALSE)
            {
                const DWORD error = ::GetLastError();
                stack.outcome = ev::CollectionOutcome::failure(
                    ev::CollectionStatus::Error, "Win32",
                    static_cast<std::uint64_t>(error), std::string());
                result.stacks.push_back(std::move(stack));
                continue;
            }

            // 再取一次，比对 RIP/RSP/RBP 是否一字不差。这比"再查一遍线程状态"
            // 更直接也更便宜：我们要的性质就是"这组值在我读的期间没有变"，
            // 而不是"内核此刻把它归为等待"。跑着的线程两次读到完全相同的三个值
            // 实际上不会发生；停在等待里的线程则保证相同。
            //
            // 窗口没法完全关死。但即使漏过去，从撕开的上下文展开出来的第一个返回
            // 地址几乎不可能落在带展开数据的映像里，可靠前缀会当场截断 ——
            // 失败方向是"不敢声称可靠"，这是对的那一侧。
            CONTEXT verifyContext{};
            verifyContext.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            const bool verified =
                ::GetThreadContext(thread.handle, &verifyContext) != FALSE &&
                verifyContext.Rip == threadContext.Rip &&
                verifyContext.Rsp == threadContext.Rsp &&
                verifyContext.Rbp == threadContext.Rbp;
            stack.trust = ev::ClassifyThreadContextTrust(
                /*captured=*/true, /*suspendedOrSnapshot=*/false,
                /*waitingBeforeAndAfter=*/verified);

            STACKFRAME64 frame{};
            frame.AddrPC.Offset = threadContext.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = threadContext.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = threadContext.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;

            // StackWalk64 会改写传进去的 CONTEXT，所以给它一份副本。
            CONTEXT walkContext = threadContext;
            bool reachedBottom = false;
            // 上一帧的 PC 在不在带展开数据的映像里 —— 它决定**本帧**是算出来的还是
            // 猜出来的。栈顶帧直接来自线程上下文，没有"上一帧"，恒为算出来的。
            bool previousPcCovered = true;
            for (std::size_t depth = 0U; depth < options.maxFrames; ++depth)
            {
                if (::StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread.handle, &frame,
                                  &walkContext, ReadMemoryCallback,
                                  FunctionTableAccessCallback, GetModuleBaseCallback,
                                  nullptr) == FALSE)
                {
                    reachedBottom = true;
                    break;
                }
                if (frame.AddrPC.Offset == 0U)
                {
                    reachedBottom = true;
                    break;
                }

                ev::RawStackFrame raw;
                raw.instructionPointer = ev::OptionalU64::of(frame.AddrPC.Offset);
                raw.stackPointer = ev::OptionalU64::of(frame.AddrStack.Offset);
                raw.derivedFromUnwindData = previousPcCovered;
                raw.unwindDataAvailableAtPc = context.pcCoveredByUnwindData(frame.AddrPC.Offset);
                previousPcCovered = raw.unwindDataAvailableAtPc;
                stack.frames.push_back(raw);
            }
            if (!reachedBottom && stack.frames.size() >= options.maxFrames)
            {
                // 命中上限不是"走到底了"。写出来，别让它被读成完整的栈。
                stack.terminationReason = "frame limit reached";
            }

            ++result.threadsWalked;
            result.stacks.push_back(std::move(stack));
        }

        return result;
    }
}
