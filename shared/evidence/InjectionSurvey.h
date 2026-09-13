#pragma once

// J 模块：进程内存植入与完整性检查（事后现场，没有事前监控记录）。
//
// 这一层回答的是 issue #196 第一阶段的五件事：
//   J-01 全地址空间索引（子区域权限不得被 AllocationBase 聚合吃掉）
//   J-02 模块交叉视图（加载器 L / 映像映射 I / 非映像载荷候选 P）
//   J-03 工作集页筛选（Valid/Shared 的正确语义）
//   J-04 线程起点及其落点
//   J-05 归一化映像比较的**范围选择**（归一化本身复用 PeImageMap + ImageDiff）
// 外加两条贯穿全模块、"最不能省"的东西：归一化比较与**明确的检查缺口**。
//
// 刻意不提供的东西，同样是判据的一部分：
//   * 没有 injectedUtc / injectionMethod / injectorPid 之类的字段。事后内存状态证明
//     不了"谁在什么时候用哪种 API 注入"，给了字段就一定会有人去填。时间字段只有
//     firstObservedUtc100ns（首次观测时间），来源字段分成"载荷所在进程"和
//     "注入源进程"，后者默认 OwnerAttribution::Unknown。
//   * 没有 score / weight / +30 +40。那些分数没有样本校准，而且"私有执行页"
//     "不在模块内""无映像来源"经常是同一块内存的三种说法，相加就是重复计分。
//     结论只有 AnalysisConclusion 四态 + 一张观测语义表（SemanticsFor）。
//   * 没有"整进程豁免"或"整目录豁免"的入口。ExceptionRelation 必须绑定
//     目标程序版本 + 被修改模块身份 + 具体 RVA 范围，缺一即拒。
//   * 没有 isInjected / isClean。快速模式的结束条件是"完成了哪些检查、发现哪些
//     候选"，不是二选一；截断、拒绝访问、参考文件不确定一律进 coverageGapKeys，
//     绝不折叠成"未发现异常"。
//
// C++20、Qt-free、Win32-free。PAGE_* 这类 Win32 数值常量在本文件里按公开文档
// 重新声明，不 include <Windows.h> —— 与 PeImageMap 手工解析 PE 的理由相同。

#include "EvidenceEnvelope.h"
#include "ImageDiff.h"
#include "LosslessValue.h"
#include "MemoryRegionEvidence.h"
#include "ObjectIdentity.h"
#include "PeImageMap.h"
#include "ScanBudget.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Ksword::Evidence {

// 本模块的检测器版本。写进每条 finding，供导出与回溯。
inline constexpr std::uint32_t kInjectionSurveyRuleSetVersion = 1U;

// ---------------------------------------------------------------------------
// 模式与架构
// ---------------------------------------------------------------------------

// 快速模式用于筛选，深度模式用于验证和取证。两者的差别只在**采集范围与读取量**，
// 不在判据：归一化比较的 profile 两边必须相同（见 NormalizationProfileId）。
enum class SurveyMode {
    Fast,
    Deep,
};

const char* SurveyModeName(SurveyMode mode) noexcept;

enum class ProcessArchitecture {
    Unknown,
    X64,
    Wow64,      // 64 位系统上的 32 位进程
    X86Native,
    Arm64,
    Arm64Ec,
};

const char* ProcessArchitectureName(ProcessArchitecture architecture) noexcept;

enum class CollectorArchitecture {
    Unknown,
    Native64,
    Wow64,      // 采集器自己跑在 WOW64 下
};

const char* CollectorArchitectureName(CollectorArchitecture architecture) noexcept;

// 微软文档：32 位进程在 WOW64 下调用 EnumProcessModulesEx 时，模块过滤参数被忽略。
// 也就是说 WOW64 采集器拿到的永远只是 32 位视图，不能当作完整的 64 位模块列表。
enum class ModuleEnumerationTrust {
    Unknown,                  // 架构未知 —— 不能假设可信
    Trusted,                  // 原生 64 位采集器
    FilterIgnoredUnderWow64,  // WOW64 采集器：过滤参数被忽略，视图不完整
};

const char* ModuleEnumerationTrustName(ModuleEnumerationTrust trust) noexcept;

ModuleEnumerationTrust EvaluateModuleEnumerationTrust(CollectorArchitecture collector,
                                                      ProcessArchitecture target) noexcept;

// ---------------------------------------------------------------------------
// J-01：Win32 保护值的正确分类
// ---------------------------------------------------------------------------
//
// 低字节是**互斥的基本保护值**，不是一组可以相与的位：
//   PAGE_EXECUTE_READ (0x20) & PAGE_EXECUTE (0x10) == 0
// 所以 `protect & PAGE_EXECUTE` 会漏掉 R-X 和 RWX 页 —— 这正是 issue 点名的漏检。
// 高位才是可以叠加的修饰位（PAGE_GUARD / PAGE_NOCACHE / ...）。

inline constexpr std::uint32_t kWin32ProtectBaseMask = 0xFFU;

inline constexpr std::uint32_t kWin32PageNoAccess = 0x01U;
inline constexpr std::uint32_t kWin32PageReadOnly = 0x02U;
inline constexpr std::uint32_t kWin32PageReadWrite = 0x04U;
inline constexpr std::uint32_t kWin32PageWriteCopy = 0x08U;
inline constexpr std::uint32_t kWin32PageExecute = 0x10U;
inline constexpr std::uint32_t kWin32PageExecuteRead = 0x20U;
inline constexpr std::uint32_t kWin32PageExecuteReadWrite = 0x40U;
inline constexpr std::uint32_t kWin32PageExecuteWriteCopy = 0x80U;

inline constexpr std::uint32_t kWin32PageGuard = 0x100U;
inline constexpr std::uint32_t kWin32PageNoCache = 0x200U;
inline constexpr std::uint32_t kWin32PageWriteCombine = 0x400U;
inline constexpr std::uint32_t kWin32PageTargetsNoUpdate = 0x40000000U;

enum class ExecuteProtection {
    Unknown,           // 来源没给原始值 —— 不是"不可执行"
    NotExecutable,
    Execute,           // PAGE_EXECUTE
    ExecuteRead,       // PAGE_EXECUTE_READ
    ExecuteReadWrite,  // PAGE_EXECUTE_READWRITE
    ExecuteWriteCopy,  // PAGE_EXECUTE_WRITECOPY
};

const char* ExecuteProtectionName(ExecuteProtection protection) noexcept;

bool ExecuteProtectionIsExecutable(ExecuteProtection protection) noexcept;

// 从原始 PAGE_* 值解出来的事实。unrecognizedBase 表示低字节不是任何已知基本值 ——
// 那是"看不懂"，既不能当可执行也不能当不可执行。
struct ProtectionFacts final {
    ExecuteProtection execute = ExecuteProtection::Unknown;
    bool readable = false;
    bool writable = false;
    bool copyOnWrite = false;
    bool noAccess = false;
    bool guard = false;
    bool unrecognizedBase = false;
    OptionalU64 rawValue;
};

ProtectionFacts ClassifyWin32Protection(const OptionalU64& rawProtect) noexcept;

// 把解析结果写进通用的 RegionProtection（M-01）。两者并存是有意的：
// RegionProtection 是跨模块的通用形状，ProtectionFacts 保留 Win32 特有的
// "看不懂的基本值"和"PAGE_GUARD"这两种状态。
RegionProtection ToRegionProtection(const ProtectionFacts& facts) noexcept;

// 取一条区域记录"实际生效"的保护事实：
//   * 有原始 PAGE_* 值就以它为准 —— 这样即使调用方用错误的位与算出了
//     RegionProtection::executable，这一层也能纠正回来（issue 点名的漏检就是
//     `protect & PAGE_EXECUTE` 漏掉 R-X / RWX）。
//   * 没有原始值时才退回布尔字段；布尔字段**全是默认值**说明来源根本没填，
//     那是未知，不是"不可执行"。
ProtectionFacts EffectiveProtection(const RegionRecord& record) noexcept;

// ---------------------------------------------------------------------------
// J-01：区域的代码分类
// ---------------------------------------------------------------------------
//
// 第一轮要标记的是**两类**已提交区域：可执行的 MEM_PRIVATE 和可执行的 MEM_MAPPED。
// 只扫私有内存会漏掉映射型的非映像代码。
// 注意 MEM_IMAGE 里的可执行页不是"候选"，但也不是"放行" —— 它走 J-05 的比较路径。
enum class RegionCodeClass {
    Unknown,            // 类型或权限未知
    NotCommitted,       // 未提交，不参与代码分类
    NonExecutable,
    ImageExecutable,    // MEM_IMAGE 且可执行 —— 交给归一化比较
    PrivateExecutable,  // MEM_PRIVATE 且可执行 —— 动态代码候选
    MappedExecutable,   // MEM_MAPPED 且可执行 —— 动态代码候选
};

const char* RegionCodeClassName(RegionCodeClass codeClass) noexcept;

// 候选 = 非映像的可执行内存。这只说明"存在动态／非映像可执行内存"，
// 不说明"注入成立"。
bool IsDynamicCodeCandidate(RegionCodeClass codeClass) noexcept;

RegionCodeClass ClassifyRegionCode(const RegionRecord& record) noexcept;

// ---------------------------------------------------------------------------
// J-01：地址空间索引
// ---------------------------------------------------------------------------
//
// VirtualQueryEx 返回的是"属性相同的连续区域"，不是一个内核 VAD。所以按
// AllocationBase 聚合是**显示与归属**用的，绝不能把子区域自己的权限合并掉：
// 一块 RW 的私有分配里挖出一页 RX，聚合后只剩"这块分配是 RW"就把证据丢了。
struct AllocationGroup final {
    OptionalU64 allocationBase;
    std::vector<std::size_t> entryIndices;  // 指回 AddressSpaceIndex::entries

    RegionType type = RegionType::Unknown;
    bool typeMixed = false;        // 组内出现了不止一种 Type
    std::string mappedPath;        // 组内一致的映射路径；不一致时为空
    bool mappedPathMixed = false;

    std::uint64_t committedBytes = 0;
    std::uint64_t executableBytes = 0;

    bool anyExecutable = false;
    bool anyWritableExecutable = false;
    bool anyGuard = false;
    bool anyProtectionUnknown = false;  // 有子区域的保护值读不出来/看不懂
};

struct AddressSpaceIndex final {
    static constexpr std::size_t kNoEntry = static_cast<std::size_t>(-1);

    // 前 searchableCount 条按 base 升序且区间有效，可参与二分查找；其后是
    // base/size 缺失或相加溢出的记录 —— 它们仍然保留（证据不丢），但进不了查找，
    // 并计入 coverage.failed。
    std::vector<RegionRecord> entries;
    std::size_t searchableCount = 0;
    std::vector<AllocationGroup> groups;   // 按 allocationBase 聚合，顺序同 entries 首现
    std::vector<RegionCodeClass> codeClasses;  // 与 entries 等长
    std::vector<std::size_t> entryGroup;       // 与 entries 等长，指回 groups 下标

    CollectionOutcome outcome;
    CoverageAccount coverage;

    std::uint64_t committedBytes = 0;
    std::uint64_t executableBytes = 0;
    std::size_t dynamicCodeCandidateCount = 0;

    // 找到包含 va 的子区域下标。没有覆盖到就是 kNoEntry —— 那是覆盖缺口，
    // 不是"这个地址是空闲的"。
    std::size_t findEntry(std::uint64_t va) const noexcept;
    const AllocationGroup* groupForEntry(std::size_t entryIndex) const noexcept;

    // 索引是否具备"缺项推断"的资格：只有采集成功且账目正面证明完整覆盖才算。
    bool usableForAbsenceInference() const noexcept;
};

// records 的顺序不重要，内部按 base 排序；base/size 缺失的记录进不了区间查找，
// 但仍保留在 entries 里并计入 coverage.failed。
AddressSpaceIndex BuildAddressSpaceIndex(std::vector<RegionRecord> records,
                                         const CollectionOutcome& outcome);

// ---------------------------------------------------------------------------
// J-01b：进程列表用的廉价筛选汇总
// ---------------------------------------------------------------------------
//
// 这一档只做 J-01 的第一轮（枚举区域 + 分类），**不**碰模块、PE、工作集、线程。
// 用途是给进程列表一列，让用户一眼看出哪些进程的动态代码面异常大。
//
// 它**不是结论**，而且必须被当成不是结论来用。实测（2026-09-12，本机 496 个进程）：
// 310 个可打开的进程里 **284 个（92%）** 都有私有/映射可执行内存，280 个还带可写可执行。
// 所以"有没有动态代码"当告警信号等于全亮；有意义的是**数量**和它的离群程度
// （同一次采样里 avpui 841 块、kpm 682 块，而绝大多数进程只有个位数）。
// 代价：全机 496 个进程共 1073 ms，中位 2.52 ms/进程，p95 9.5 ms。
enum class SurfaceScreenState {
    NotScreened,       // 没做过 —— 列里必须显示"未筛选"，不能显示 0
    AccessDenied,      // 打不开目标（受保护进程等）—— 不是"没有动态代码"
    IdentityMismatch,  // PID 已复用
    Failed,
    Screened,
};

const char* SurfaceScreenStateName(SurfaceScreenState state) noexcept;

// 只有 Screened 的计数才有意义；其余状态下的 0 是"不知道"，不是"没有"。
bool SurfaceScreenCountsAreMeaningful(SurfaceScreenState state) noexcept;

struct ProcessSurfaceScreen final {
    SurfaceScreenState state = SurfaceScreenState::NotScreened;
    std::uint32_t regionCount = 0;
    std::uint32_t dynamicCodeRegions = 0;
    std::uint32_t writableExecutableRegions = 0;
    std::uint64_t dynamicCodeBytes = 0;
    // 首次观测时间，不是"注入时间"。
    OptionalU64 screenedUtc100ns;
};

// 从已建好的地址空间索引汇总。索引采集失败时返回对应的失败态，
// 绝不把"没查到"折叠成计数 0。
ProcessSurfaceScreen SummarizeSurfaceScreen(const AddressSpaceIndex& index,
                                            const OptionalU64& screenedUtc100ns);

// ---------------------------------------------------------------------------
// J-02：模块交叉视图
// ---------------------------------------------------------------------------

// L：加载器视图。同一个采集器的两层包装不算两个来源，所以这里不区分
// PSAPI / Toolhelp / PEB 三条链表 —— 它们的交叉价值在与 I 的比对上。
struct LoaderModuleEntry final {
    DriverInstanceId module;   // imagePath / imageBase / imageSize
    std::string listedName;    // 加载器列出的名字（可能与磁盘文件名不同）
    bool isMainImage = false;
};

// I：映像映射视图。mappedPath 用 GetMappedFileNameW 一类接口取得，
// **不只相信 PEB 字符串**；取不到就保留失败原因，绝不写成"无文件植入"。
struct ImageMappingEntry final {
    OptionalU64 allocationBase;
    OptionalU64 mappedSize;     // 该分配组内已提交跨度
    std::string mappedPath;
    CollectionOutcome pathOutcome;
};

// P：非映像载荷候选的结构判定。
// "两个字节的 MZ 不是充分证据"：数据缓冲区里躺着一个 PE 文件，与这个 PE 已经被
// 加载执行，是两件事。所以 PeWithHeaders 只是一种结构事实。
enum class PayloadStructure {
    NotExamined,     // 没检查 —— 覆盖缺口
    Unreadable,      // 想检查但读不到
    NoStructure,     // 检查过，没有可辨识结构
    DataOnlyPeFile,  // 有完整 PE 文件结构但不像被映射（节未按虚拟布局展开）
    MappedPeImage,   // 头部自洽且节布局与内存范围自洽
    HeaderErasedPe,  // 头被擦除，但仍有导入/展开/内部引用等残留结构
    BareCode,        // 没有 PE 结构的可执行代码
};

const char* PayloadStructureName(PayloadStructure structure) noexcept;

struct PayloadCandidateEntry final {
    OptionalU64 base;
    OptionalU64 size;
    RegionType type = RegionType::Unknown;
    PayloadStructure structure = PayloadStructure::NotExamined;
    std::vector<std::string> structureFacts;  // key=value，可回源
    CollectionOutcome outcome;
    // **可靠展开的调用帧**落进这块内存。不是"栈回溯这个能力可用"，也不是
    // "栈里扫到一个像这块内存的地址" —— 只有 StackEvidenceKind::ReliableUnwoundFrame
    // 才能置位。这一位是把"内存里有载荷结构"抬成"载荷与执行相关联"的唯一依据。
    bool reliableFrameEntersRegion = false;
};

enum class ModuleCrossIssue {
    ImageMappingWithoutLoaderEntry,  // 有映像映射，加载器列表没有对应项
    LoaderEntryWithoutImageMapping,  // 列表里的基址没有合理映射
    LoaderPathMismatch,              // 名称/路径与实际映射不一致
    LoaderSizeMismatch,              // 大小与实际映射不一致
    MainImageIdentityConflict,       // 主映像的几个来源自相矛盾
    MappedPathUnavailable,           // 路径查询失败 —— 缺口，不是结论
};

const char* ModuleCrossIssueName(ModuleCrossIssue issue) noexcept;

// 交叉视图的问题分两档，不能一视同仁：
//   * **矛盾**（本函数返回 true）：两个视图对**同一个对象**说了互相冲突的话 ——
//     加载器说是 ntdll.dll、映射却指向另一个文件；主映像的三个来源互不一致。
//     这撑得起 DifferenceObserved。
//   * **不对称**（返回 false）：一边有、另一边没有。它有大量已记录的合法成因 ——
//     只做资源映射的映像（LOAD_LIBRARY_AS_IMAGE_RESOURCE）、Windows 元数据映像、
//     .NET 相关映射；实测 explorer.exe 上稳定有十几条。它只能到 Indeterminate。
//   缺少 PEB 项"只能先报异常"，不是"观测到差异"。
bool ModuleCrossIssueIsContradiction(ModuleCrossIssue issue) noexcept;

struct ModuleCrossFinding final {
    ModuleCrossIssue issue = ModuleCrossIssue::MappedPathUnavailable;
    OptionalU64 base;
    std::string loaderPath;
    std::string mappedPath;
    OptionalU64 loaderSize;
    OptionalU64 mappedSize;
    std::vector<std::string> facts;
    CollectionOutcome inputOutcome;
};

struct ModuleCrossViewInput final {
    std::vector<LoaderModuleEntry> loaderView;
    CollectionOutcome loaderOutcome;
    ModuleEnumerationTrust loaderTrust = ModuleEnumerationTrust::Unknown;

    std::vector<ImageMappingEntry> imageView;
    CollectionOutcome imageOutcome;

    std::vector<PayloadCandidateEntry> payloadView;
    CollectionOutcome payloadOutcome;

    // 主映像的三个独立来源。任何一个为空表示该来源没取到。
    std::string mainImagePathFromLoader;   // PEB 加载器链表首项
    std::string mainImagePathFromKernel;   // QueryFullProcessImageName 一类
    std::string mainImagePathFromMapping;  // 主映像分配的 GetMappedFileNameW
    OptionalU64 mainImageBaseFromLoader;
    OptionalU64 mainImageBaseFromMapping;

    // 映射跨度与加载器 SizeOfImage 的容差。加载器报的是 SizeOfImage，
    // 映射跨度可能因对齐而略大，所以不给容差会产生大量噪声。
    std::uint64_t sizeToleranceBytes = 0x10000ULL;
};

struct ModuleCrossViewReport final {
    std::vector<ModuleCrossFinding> findings;
    std::vector<std::string> coverageGapKeys;

    std::size_t matchedModules = 0;
    std::size_t loaderOnly = 0;
    std::size_t mappingOnly = 0;
    std::size_t payloadCandidates = 0;

    // 只有两侧视图都 Success 才允许做"缺项"推断。任一侧失败/部分/不支持时，
    // 缺项一律不产出，并留下缺口键 —— 这与 X-06 是同一条规则。
    bool absenceInferenceAllowed = false;

    AnalysisConclusion conclusion = AnalysisConclusion::NoEvidence;
};

ModuleCrossViewReport EvaluateModuleCrossView(const ModuleCrossViewInput& input);

// ---------------------------------------------------------------------------
// J-03：工作集页筛选
// ---------------------------------------------------------------------------

// QueryWorkingSetEx 的一页结果。字段名与 PSAPI_WORKING_SET_EX_BLOCK 对齐。
struct WorkingSetPageFact final {
    std::uint64_t va = 0;
    bool queried = false;   // 有没有真的查过这一页
    bool valid = false;     // Valid==0 时其余字段不得按有效页解释
    bool shared = false;    // "是否可共享"，不是"当前恰好有两个进程在用"
    OptionalU64 shareCount; // 保留原值供导出；判据里**不使用**
    bool locked = false;
    bool largePage = false;
    bool bad = false;
    OptionalU64 win32Protection;
    OptionalU64 node;
};

enum class PageScreenVerdict {
    NotQueried,           // 没查 —— 覆盖缺口
    InvalidNeedsRecheck,  // Valid==0，保守标待补查，不按有效页解释
    PrivatizedCandidate,  // Valid && !Shared —— 私有化/改写候选，优先比较
    SharedNotCleared,     // Valid && Shared —— **不是放行**，深度模式仍要比较
};

const char* PageScreenVerdictName(PageScreenVerdict verdict) noexcept;

// 判据里刻意不看 shareCount：Shared 表示"页面是否可共享"，
// ShareCount == 1 不能代替它。而且 DFRWS 2023 记录过内存合并会让**已修改**的页面
// 重新呈现可共享状态，所以 Shared 也不能反向放行。
PageScreenVerdict ScreenWorkingSetPage(const WorkingSetPageFact& fact) noexcept;

// 快速模式优先比较不可共享的代码页；深度模式不使用共享状态排除任何映像页。
bool PageSelectedForComparison(PageScreenVerdict verdict, SurveyMode mode) noexcept;

// ---------------------------------------------------------------------------
// J-04：线程起点与落点
// ---------------------------------------------------------------------------

// 一个模块的代码范围。codeBegin/codeEnd 是模块内**应当执行**的 RVA 区间并集的
// 粗粒度包络，用于判断"落在映像内部却不符合该模块代码布局"。
struct ImageCodeExtent final {
    std::string path;
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint64_t codeBeginRva = 0;
    std::uint64_t codeEndRva = 0;
    bool codeExtentKnown = false;  // false 表示只知道模块范围，不知道代码布局

    bool containsAddress(std::uint64_t address) const noexcept;
    bool addressInCodeExtent(std::uint64_t address) const noexcept;
};

enum class ThreadStartLanding {
    NotCollected,        // 起始地址没取到 —— 没有观测，不是"归属不一致"
    OutsideIndex,        // 索引没覆盖到这个地址 —— 覆盖缺口
    FreeOrReserved,      // 落在已释放/未提交区域
    NonImagePrivate,     // 落在私有区域
    NonImageMapped,      // 落在映射（非映像）区域
    ImageCodeRange,      // 落在某映像的代码范围内
    ImageOutsideCode,    // 落在映像内部但不符合该模块代码布局
    ImageLayoutUnknown,  // 落在映像内，但代码布局未知，无法细分
};

const char* ThreadStartLandingName(ThreadStartLanding landing) noexcept;

// 现存线程的起点采集结果。
struct ThreadStartInput final {
    ThreadInstanceId thread;
    OptionalU64 startAddress;
    CollectionOutcome startAddressOutcome;

    // 入口附近指令检查（预算允许时才做）。三态：没做 / 做了读不到 / 做了读到。
    bool entryInspected = false;
    bool entryReadable = false;
    // 解析出的第一跳目标。unset 表示"没解析出来"，不是"没有跳转"。
    OptionalU64 immediateBranchTarget;
};

// 线程上下文的可信度。微软明确指出：运行中的线程无法通过 GetThreadContext 取得
// 有效上下文。所以"没挂起也没快照"拿到的上下文只能标成不可信，不得当执行证据。
enum class ThreadContextTrust {
    NotCaptured,
    RunningThreadUntrusted,
    SuspendedOrSnapshot,
};

const char* ThreadContextTrustName(ThreadContextTrust trust) noexcept;

ThreadContextTrust ClassifyThreadContextTrust(bool captured,
                                              bool suspendedOrSnapshot) noexcept;

bool ContextUsableAsExecutionEvidence(ThreadContextTrust trust) noexcept;

// 栈上看到的东西分三档存放。"在栈内存里扫到一个看起来像代码地址的数值"
// 不等于"恢复出一个调用帧"，三者混成同一种执行证据是明令禁止的。
enum class StackEvidenceKind {
    ReliableUnwoundFrame,            // 可靠展开的帧
    HeuristicReturnAddressCandidate, // 启发式候选返回地址
    PlainPointerReference,           // 普通指针引用
};

const char* StackEvidenceKindName(StackEvidenceKind kind) noexcept;

// 只有可靠展开的帧才算执行证据。
bool StackEvidenceCountsAsExecution(StackEvidenceKind kind) noexcept;

struct ThreadStartFinding final {
    ThreadInstanceId thread;
    OptionalU64 startAddress;
    ThreadStartLanding landing = ThreadStartLanding::NotCollected;

    // 起始地址所在页**现在**可不可执行。起点页现在不可执行不能据此忽略这条线索，
    // 所以它只是一个并列的事实位，不参与 landing 的判定。
    bool startPageExecutableKnown = false;
    bool startPageExecutable = false;

    std::string owningPath;   // 空表示归属未知，不是"没有归属"
    OptionalU64 branchTarget;
    ThreadStartLanding branchTargetLanding = ThreadStartLanding::NotCollected;
    bool branchLeavesOwningModule = false;
    bool entryInspected = false;

    std::vector<std::string> facts;
    CollectionOutcome outcome;
};

// 按地址空间索引 + 模块代码范围解释一批线程起点。
std::vector<ThreadStartFinding> EvaluateThreadStarts(
    const std::vector<ThreadStartInput>& threads,
    const AddressSpaceIndex& index,
    const std::vector<ImageCodeExtent>& images);

// ---------------------------------------------------------------------------
// J-05：归一化映像比较的范围选择
// ---------------------------------------------------------------------------
//
// 硬约束：快速模式与深度模式**必须使用同一套归一化逻辑**，不能快扫裸比较、
// 深扫才处理重定位。所以 profile id 与模式无关；两种模式只改比较范围。
inline constexpr const char* kNormalizationProfileId = "image.normalize.peimagemap.v1";
inline constexpr std::uint32_t kNormalizationProfileVersion = 1U;

const char* NormalizationProfileId(SurveyMode mode) noexcept;
std::uint32_t NormalizationProfileVersion(SurveyMode mode) noexcept;

enum class ComparisonReason {
    MainImageEntry,           // 主映像入口
    SuspiciousThreadEntry,    // 可疑线程入口
    WorkingSetScreenedPage,   // 工作集筛出的异常映像页
    ControlFlowReference,     // 已发现的控制流引用位置
    FullExecutableCoverage,   // 深度模式：全部可执行映像范围
};

const char* ComparisonReasonName(ComparisonReason reason) noexcept;

struct ComparisonTarget final {
    DriverInstanceId module;
    RvaRange range;
    ComparisonReason reason = ComparisonReason::MainImageEntry;
};

// 建计划所需的现场事实。
struct ComparisonPlanInput final {
    SurveyMode mode = SurveyMode::Fast;
    std::vector<ImageCodeExtent> images;

    // 主映像入口的 RVA 与要比较的跨度。unset 表示没取到 —— 计划里会留缺口键。
    std::string mainImagePath;
    OptionalU64 mainImageEntryRva;

    // 可疑线程入口（已解析成 模块路径 + RVA）。
    struct ThreadEntrySite final {
        std::string imagePath;
        std::uint32_t rva = 0;
    };
    std::vector<ThreadEntrySite> threadEntrySites;

    // 工作集筛出的异常映像页（模块路径 + 页 RVA）。
    struct ScreenedPage final {
        std::string imagePath;
        std::uint32_t pageRva = 0;
    };
    std::vector<ScreenedPage> screenedPages;

    // 已发现的控制流引用位置。
    std::vector<ThreadEntrySite> controlFlowSites;

    std::uint32_t entryWindowBytes = 64U;
    std::uint32_t pageSize = 4096U;
};

struct ComparisonPlan final {
    SurveyMode mode = SurveyMode::Fast;
    std::string normalizationProfileId;
    std::uint32_t normalizationProfileVersion = 0;
    std::vector<ComparisonTarget> targets;
    std::vector<std::string> coverageGapKeys;
};

ComparisonPlan BuildComparisonPlan(const ComparisonPlanInput& input);

// 参考文件的可信度。"当前路径下的文件可能已被更新或替换"——拿不到可靠参考时
// 必须报"参考映像不确定"，不能自动把所有差异归为恶意修改。
enum class ReferenceConfidence {
    NoReference,        // 根本没有参考文件
    ReferenceUncertain, // 有文件，但不能确认它对应目标映像版本
    ReferenceVerified,  // 身份已核对（PDB 签名 / TimeDateStamp+SizeOfImage 等）
};

const char* ReferenceConfidenceName(ReferenceConfidence confidence) noexcept;

// 只有 ReferenceVerified 才允许把差异表述成"映像代码修改已证实"。
bool ReferenceSupportsDifferenceClaim(ReferenceConfidence confidence) noexcept;

struct ImageComparisonOutcome final {
    DriverInstanceId module;
    ReferenceConfidence referenceConfidence = ReferenceConfidence::NoReference;
    ImageDiffReport report;
};

// ---------------------------------------------------------------------------
// J-06：R0 扫描后端的交叉视图（issue #196 §五 第一、二层）
// ---------------------------------------------------------------------------
//
// 这一节处理的是**第二、第三个独立来源**：
//   * VAD 树（内存管理器自己记的区域），与 R3 的 VirtualQueryEx 独立；
//   * 页表叶子（处理器实际怎么看），与前两者都独立。
//
// 三条硬规则：
//   * "内部结构按目标 build 验证，未支持的版本明确降级" —— profile 没验证过就
//     **一条 finding 都不产**，只留缺口。绝不用相近版本的偏移继续读。
//   * MMVAD_FLAGS 的位布局没有经过验证，所以 VAD 的 protection **不参与**与 R3
//     保护属性的矛盾判定；只比较范围。位猜错时那会变成整片假矛盾。
//   * "内核采集依赖内核可信" —— 有内核能力的对手可以改这里读到的元数据。
//     只要用了内核视图就恒挂 kLimitKernelTrustAssumption，不承诺"有驱动便无法隐藏"。

enum class KernelBackendState {
    NotRequested,       // 这次没打算用内核后端
    DriverUnavailable,  // 驱动没加载 / 打不开 / 权限不足
    ProfileUnverified,  // 驱动在，但 DynData 没为当前 build 验证过所需偏移
    Partial,            // 跑了，但有读不到的节点/表项，或被预算截断
    Available,          // 完整跑完
};

const char* KernelBackendStateName(KernelBackendState state) noexcept;

// 只有 Available 才谈得上"这边有那边没有"。Partial 会让缺项推断变成猜。
bool KernelBackendSupportsAbsenceInference(KernelBackendState state) noexcept;

// R0 VAD 视图的一条区域记录。
struct KernelVadRegion final {
    OptionalU64 startVa;
    OptionalU64 endVaExclusive;
    OptionalU64 vadNodeAddress;
    bool privateMemory = false;
    bool hasSection = false;          // controlArea != 0
    // 位布局未经验证 —— 恒为 true。protection/vadType 因此只能展示。
    bool flagsLayoutAssumed = true;
    OptionalU64 protectionRaw;
    OptionalU64 vadFlagsRaw;
};

struct KernelVadView final {
    std::vector<KernelVadRegion> regions;
    KernelBackendState state = KernelBackendState::NotRequested;
    CollectionOutcome outcome;
    std::uint64_t visitedCount = 0;
    std::uint64_t unreadableNodeCount = 0;
    bool truncated = false;

    bool usableForAbsenceInference() const noexcept;
};

// R0 页表视图的一段可执行叶子页。
struct KernelExecutableExtent final {
    OptionalU64 startVa;
    OptionalU64 byteLength;
    std::uint32_t pageSize = 0;
    bool executable = false;
    bool writable = false;
    bool userAccessible = false;
    bool largePage = false;
    OptionalU64 firstEntryValue;
};

struct KernelPteView final {
    std::vector<KernelExecutableExtent> extents;
    KernelBackendState state = KernelBackendState::NotRequested;
    CollectionOutcome outcome;
    std::uint64_t tableReads = 0;
    std::uint64_t failedTableReads = 0;
    bool truncated = false;
    OptionalU64 scannedBegin;
    OptionalU64 scannedEnd;

    bool usableForAbsenceInference() const noexcept;
};

enum class KernelRegionCrossIssue {
    VadOnlyRange,            // VAD 有这段，R3 的 VirtualQueryEx 没报
    R3OnlyCommittedRange,    // R3 报了已提交区域，VAD 树里没有对应
    ExecutableBeyondR3View,  // 页表说可执行，R3 索引里那段不可执行或不存在
    ExecutableBeyondVadView, // 页表说可执行，VAD 没有覆盖那段
};

const char* KernelRegionCrossIssueName(KernelRegionCrossIssue issue) noexcept;

struct KernelRegionCrossFinding final {
    KernelRegionCrossIssue issue = KernelRegionCrossIssue::VadOnlyRange;
    OptionalU64 startVa;
    OptionalU64 endVaExclusive;
    std::vector<std::string> facts;
    CollectionOutcome inputOutcome;
};

struct KernelCrossViewInput final {
    const AddressSpaceIndex* r3Index = nullptr;   // 必填；为空则整节不产出
    KernelVadView vadView;
    KernelPteView pteView;
    // 页表视图只扫了这个范围。范围外的"页表没报可执行"不构成缺项。
    OptionalU64 pteScanBegin;
    OptionalU64 pteScanEnd;
};

struct KernelCrossViewReport final {
    std::vector<KernelRegionCrossFinding> findings;
    std::vector<std::string> coverageGapKeys;
    std::vector<std::string> capabilityLimitKeys;

    std::size_t vadOnlyCount = 0;
    std::size_t r3OnlyCount = 0;
    std::size_t executableBeyondViewCount = 0;
    bool absenceInferenceAllowed = false;

    AnalysisConclusion conclusion = AnalysisConclusion::NoEvidence;
};

KernelCrossViewReport EvaluateKernelCrossView(const KernelCrossViewInput& input);

// ---------------------------------------------------------------------------
// 例外（白名单）：只针对具体关系
// ---------------------------------------------------------------------------

// 合法例外至少要覆盖这四类来源。Unspecified 一律拒绝准入 —— 一条没有类别的
// 豁免规则无法被复核。
enum class ExceptionCategory {
    Unspecified,
    RuntimeDynamicCode,       // JIT 等运行时动态代码
    SecurityInstrumentation,  // 安全产品插桩
    SoftwareProtection,       // 软件保护 / 打包 / DRM
    SystemCompatibility,      // 已验证的系统兼容性修改
};

const char* ExceptionCategoryName(ExceptionCategory category) noexcept;

struct ExceptionRelation final {
    std::string ruleId;
    std::uint32_t ruleVersion = 0;
    ExceptionCategory category = ExceptionCategory::Unspecified;

    std::string targetImageIdentity;     // 目标程序版本身份（必填）
    std::string modifiedModuleIdentity;  // 被修改模块身份（必填）
    RvaRange modifiedRange;              // 修改位置（必填，且有硬上限）

    // 允许的跳转关系。空表示这条规则不对跳转目标作要求。
    std::string allowedBranchTargetModuleIdentity;
    // 必要的目标字节检查。非空时**必须**能读到现场字节才可能命中。
    std::vector<std::uint8_t> expectedBytes;

    std::string evidenceText;  // 依据来源，不是结论
};

enum class ExceptionAdmission {
    Accepted,
    MissingRuleId,
    MissingCategory,
    MissingTargetImageIdentity,
    MissingModuleIdentity,
    EmptyRange,
    RangeTooWide,   // 超过 kExplanationRuleMaxSpanBytes —— 等于整模块放行
};

const char* ExceptionAdmissionName(ExceptionAdmission admission) noexcept;

// 白名单必须针对具体关系而不是整个进程或目录：四个必填项缺一即拒，
// 范围为空或过宽即拒。被拒的规则不参与匹配。
ExceptionAdmission AdmitExceptionRelation(const ExceptionRelation& rule) noexcept;

// 例外规则里的 modifiedModuleIdentity **必须**用这个函数生成，否则规则永远匹配
// 不上（匹配是严格等值比较，不做路径归一化）。优先取 DriverInstanceId 的跨会话
// 主键（带 PDB 签名 / TimeDateStamp，能区分同名不同版本）；身份不足时退化为
// 归一化路径（小写 + 反斜杠）。退化键区分不了版本，这正是"同名不等于同版本"
// 的代价，写规则的人应当补齐模块身份而不是依赖路径。
std::string ModuleIdentityKeyFor(const DriverInstanceId& module);

struct ExceptionQuery final {
    std::string targetImageIdentity;
    std::string modifiedModuleIdentity;
    RvaRange range;
    // 现场观测到的跳转目标模块身份。空表示"没有这个事实"。
    std::string actualBranchTargetModuleIdentity;
    // 现场读到的字节。bytesAvailable 为 false 表示没读到 —— 与"读到了空"不同。
    bool bytesAvailable = false;
    std::vector<std::uint8_t> actualBytes;
};

enum class ExceptionMatch {
    NoRule,                // 一条规则都没有
    AllRulesRejected,      // 有规则但全被准入检查拒了
    TargetImageMismatch,
    ModuleMismatch,
    RangeNotCovered,       // 部分覆盖不算命中
    BranchTargetMismatch,
    BytesUnavailable,      // 规则要求字节检查，但现场没读到 —— 不命中
    BytesMismatch,
    Matched,
};

const char* ExceptionMatchName(ExceptionMatch match) noexcept;

struct ExceptionMatchResult final {
    ExceptionMatch match = ExceptionMatch::NoRule;
    std::string ruleId;
    std::uint32_t ruleVersion = 0;
    ExceptionCategory category = ExceptionCategory::Unspecified;
    std::size_t rejectedRuleCount = 0;  // 丢弃必须可见
};

// 命中要求规则范围**完全包含**待判范围。返回"最接近的失败原因"：
// 只要有任何一条规则走到更靠后的检查，就报那一条的原因，便于定位规则写错在哪。
ExceptionMatchResult MatchExceptionRelation(const std::vector<ExceptionRelation>& rules,
                                            const ExceptionQuery& query);

// ---------------------------------------------------------------------------
// 结果条目
// ---------------------------------------------------------------------------

extern const char* const kRuleIdDynamicCodeRegion;          // inject.region.dynamic-code
extern const char* const kRuleIdImageBytesUnexplained;      // inject.image.unexplained-diff
extern const char* const kRuleIdImageReferenceUncertain;    // inject.image.reference-uncertain
extern const char* const kRuleIdImageWithoutLoaderEntry;    // inject.module.image-without-loader
extern const char* const kRuleIdLoaderEntryWithoutMapping;  // inject.module.loader-without-mapping
extern const char* const kRuleIdModuleIdentityMismatch;     // inject.module.identity-mismatch
extern const char* const kRuleIdMainImageConflict;          // inject.main-image.conflict
extern const char* const kRuleIdThreadStartOutsideImage;    // inject.thread.start-outside-image
extern const char* const kRuleIdThreadStartUnknown;         // inject.thread.start-unknown
extern const char* const kRuleIdThreadStartTrampoline;      // inject.thread.start-trampoline
extern const char* const kRuleIdPayloadStructure;           // inject.payload.structure
extern const char* const kRuleIdKernelRegionHiddenFromR3;   // inject.kernel.region-hidden-from-r3
extern const char* const kRuleIdKernelRegionMissingInVad;   // inject.kernel.region-missing-in-vad
extern const char* const kRuleIdKernelExecutableBeyondView; // inject.kernel.executable-beyond-view

// 可信度：不是分数，是"这条结果由多少独立观测撑起来"。
enum class EvidenceConfidence {
    InputIncomplete,          // 依赖的输入不完整 —— 只能当线索
    SingleObservation,        // 单一观测
    CorroboratedIndependent,  // 两个及以上独立观测互证
};

const char* EvidenceConfidenceName(EvidenceConfidence confidence) noexcept;

// 每条结果的字段集合，对应 issue 第六节的清单。
// 注意这里**没有** injectedUtc / injectorPid / score / isMalicious。
struct InjectionFinding final {
    std::string ruleId;
    std::uint32_t ruleVersion = kInjectionSurveyRuleSetVersion;
    std::string detectorVersion;

    ProcessInstanceId payloadProcess;  // 载荷所在进程
    // 注入源进程。没有事前记录就是 Unknown，不许降格成"某个系统进程"。
    OwnerAttribution injectorAttribution = OwnerAttribution::Unknown;
    std::vector<std::string> injectorCandidates;

    OptionalU64 firstObservedUtc100ns;  // 首次观测时间，不是注入时间

    OptionalU64 address;
    OptionalU64 size;
    RegionType regionType = RegionType::Unknown;
    RegionProtection protection;
    std::string mappedPath;

    std::string moduleName;
    std::string sectionName;
    OptionalU64 rva;

    std::vector<std::string> facts;  // 原始证据，key=value，可回源
    std::vector<ThreadInstanceId> relatedThreads;
    std::vector<std::string> relatedFrameKeys;

    EvidenceConfidence confidence = EvidenceConfidence::InputIncomplete;
    ExceptionMatchResult exception;
    std::vector<std::string> coverageGapKeys;
    CollectionOutcome inputOutcome;

    // 例外命中的结果仍然保留在列表里（可核对），但不计入"未解释"计数。
    bool explainedByException() const noexcept;
};

// ---------------------------------------------------------------------------
// 观测语义表（issue 第六节）
// ---------------------------------------------------------------------------

enum class ObservationClass {
    PrivateOrMappedExecutablePresent,   // 存在私有 RX／RWX（或映射可执行）
    NormalizedImageDiffers,             // 归一化后代码仍与可靠参考不同
    PayloadStructureWithReliableFrame,  // 自洽载荷结构 + 可靠栈帧进入其中
    MappedModuleOutsideBaseline,        // 正常映射的 DLL 不符合可信应用基线
    ScanCompleteNoStrongEvidence,       // 扫完了但没有强证据
    KeyInputUnavailable,                // 关键页面／线程／参考文件不可获得
};

const char* ObservationClassName(ObservationClass observation) noexcept;

// 一条观测能给出什么、不能给出什么。两个键都是 i18n 键，UI 必须把"不能给出的
// 结论"一并显示 —— 否则用户会把"没检查到"读成"没有"。
struct ObservationSemantics final {
    ObservationClass observation = ObservationClass::KeyInputUnavailable;
    const char* allowedConclusionKey = "";
    const char* forbiddenConclusionKey = "";
    AnalysisConclusion contribution = AnalysisConclusion::NoEvidence;
};

ObservationSemantics SemanticsFor(ObservationClass observation) noexcept;

// ---------------------------------------------------------------------------
// 身份复核
// ---------------------------------------------------------------------------

enum class IdentityRecheckVerdict {
    Same,
    Changed,        // 退出、重建或 PID 复用 —— 证据可能串到另一个进程
    Unverifiable,   // 身份信息不足
};

const char* IdentityRecheckVerdictName(IdentityRecheckVerdict verdict) noexcept;

IdentityRecheckVerdict RecheckProcessIdentity(const ProcessInstanceId& before,
                                              const ProcessInstanceId& after) noexcept;

// ---------------------------------------------------------------------------
// 覆盖缺口键
// ---------------------------------------------------------------------------

extern const char* const kGapAddressSpaceIncomplete;   // inject.gap.address-space
extern const char* const kGapLoaderViewUnavailable;    // inject.gap.loader-view
extern const char* const kGapImageViewUnavailable;     // inject.gap.image-view
extern const char* const kGapPayloadViewUnavailable;   // inject.gap.payload-view
extern const char* const kGapMappedPathUnavailable;    // inject.gap.mapped-path
extern const char* const kGapWorkingSetUnavailable;    // inject.gap.working-set
extern const char* const kGapThreadStartUnavailable;   // inject.gap.thread-start
extern const char* const kGapReferenceUncertain;       // inject.gap.reference-uncertain
extern const char* const kGapBudgetTruncated;          // inject.gap.budget-truncated
extern const char* const kGapIdentityChanged;          // inject.gap.identity-changed
extern const char* const kGapIdentityUnverifiable;     // inject.gap.identity-unverifiable
extern const char* const kGapModuleEnumerationWow64;   // inject.gap.module-enum-wow64
extern const char* const kGapMainImageSourceMissing;   // inject.gap.main-image-source
extern const char* const kGapKernelBackendUnavailable; // inject.gap.kernel-backend
extern const char* const kGapKernelProfileUnverified;  // inject.gap.kernel-profile

// ---------------------------------------------------------------------------
// 能力限制键：与覆盖缺口是**两类东西**，不能混在一张表里
// ---------------------------------------------------------------------------
//
//   * 覆盖缺口（上面那一组）＝"我打算查的东西没查成"：页读不到、线程拿不到、
//     参考文件对不上、预算截断、身份存疑。它**必须**压制"未发现差异"——
//     你声明的范围本身破了。
//   * 能力限制（下面这一组）＝"本版本根本不做这件事"：没有 CLR 运行时归因、
//     不识别擦头载荷、没有可靠栈回溯后端。它**不压制**结论，只缩小结论的适用范围。
//
// 为什么必须分开：把能力限制也当缺口，等于每个进程、每一次扫描都永远"覆盖不完整"，
// 于是 AnalysisConclusion 的四态在生产里退化成三态，"查过了、在范围内没发现"
// 和"根本没查成"再也分不开 —— 那正是缺口这一维想避免的事。
extern const char* const kLimitNonExecutableNotScanned;  // inject.limit.non-executable
extern const char* const kLimitStackUnwindUnavailable;   // inject.limit.stack-unwind
// 本版本只按残留的 PE 头识别载荷结构，识别不了被擦除头部的载荷。
extern const char* const kLimitPayloadHeaderErased;      // inject.limit.payload-erased-header
// 本版本不做 CLR 等运行时归因。实测（2026-09-12，pwsh.exe 深扫）：
// System.Management.Automation.dll 上有一处 9 字节就地改写，归一化比较把它如实报成
// "未解释差异" —— 比较引擎没错，缺的是解释它的运行时视图。所以这一条必须显式列出，
// 否则用户会把一条托管运行时的正常改写读成注入证据。
extern const char* const kLimitRuntimeAttribution;       // inject.limit.runtime-attribution
// 内核采集依赖内核可信：有内核能力的对手可以改这里读到的元数据或参考页。
// 只要用了内核视图就恒挂这一条 —— 绝不宣传"有驱动便无法隐藏"。
extern const char* const kLimitKernelTrustAssumption;    // inject.limit.kernel-trust
// MMVAD_FLAGS 的位布局没有经过 build 验证，所以 VAD 的保护属性不参与矛盾判定。
extern const char* const kLimitKernelVadFlagsUnverified; // inject.limit.kernel-vad-flags
// 第三层（把进程映像页与 Image Section Object 的参考页比较）本版本没做。
extern const char* const kLimitKernelSectionCompare;     // inject.limit.kernel-section-compare
// 内核交叉差异的**合法成因目录**尚未在实机数据上建立。在建立之前，这类差异
// 一律只到"待解释"，不升 DifferenceObserved —— 没量过就不给确定性。
extern const char* const kLimitKernelBenignBaseline;     // inject.limit.kernel-benign-baseline
// 本机根本没有 KswordARK 设备。这是**能力限制**不是覆盖缺口：我们从来没有声明过
// 要在一台没装驱动的机器上做内核视图。写成缺口会让每一次扫描的 scopeIntact 恒为假，
// 四态又退化成三态 —— 与 kLimitNonExecutableNotScanned 同一条道理。
// 驱动**在**但某次调用失败（权限、profile 未验证）才是真缺口。
extern const char* const kLimitKernelBackendAbsent;      // inject.limit.kernel-backend-absent

// 已完成 / 未执行的检查项键。快速模式的结束条件用这两张表表达，
// 而不是一个 Injected/Clean 的布尔。
extern const char* const kCheckAddressSpaceIndex;      // inject.check.address-space
extern const char* const kCheckModuleCrossView;        // inject.check.module-cross-view
extern const char* const kCheckWorkingSetScreen;       // inject.check.working-set
extern const char* const kCheckThreadStart;            // inject.check.thread-start
extern const char* const kCheckNormalizedImageDiff;    // inject.check.image-diff
extern const char* const kCheckPayloadStructure;       // inject.check.payload-structure
extern const char* const kCheckNonExecutableScan;      // inject.check.non-executable
extern const char* const kCheckReliableStackWalk;      // inject.check.stack-walk
extern const char* const kCheckKernelVadCrossView;     // inject.check.kernel-vad
extern const char* const kCheckKernelPteScan;          // inject.check.kernel-pte

// ---------------------------------------------------------------------------
// 总入口
// ---------------------------------------------------------------------------

struct SurveyInput final {
    SurveyMode mode = SurveyMode::Fast;
    std::string detectorVersion;

    // 进程身份必须在扫描前后各取一次。只有 pid 的身份是弱身份，
    // 复核结果会是 Unverifiable，此时结论不得升到 NoDifferenceObserved。
    ProcessInstanceId processBefore;
    ProcessInstanceId processAfter;
    OptionalU64 collectedUtc100ns;

    ProcessArchitecture targetArchitecture = ProcessArchitecture::Unknown;
    CollectorArchitecture collectorArchitecture = CollectorArchitecture::Unknown;

    // 目标程序的版本身份（主映像身份串）。例外规则必须绑定到它 ——
    // 一条只写"允许改 ntdll 的这几个字节"的豁免，如果不绑定目标程序版本，
    // 就会在所有程序上生效。空串表示没取到，此时任何例外都匹配不上。
    std::string targetImageIdentity;

    AddressSpaceIndex addressSpace;
    ModuleCrossViewReport moduleCrossView;

    std::vector<ThreadStartFinding> threadStarts;
    CollectionOutcome threadEnumerationOutcome;

    bool workingSetQueried = false;
    CollectionOutcome workingSetOutcome;
    std::size_t workingSetPagesScreened = 0;
    std::size_t workingSetPrivatizedPages = 0;
    std::size_t workingSetInvalidPages = 0;

    std::vector<ImageComparisonOutcome> imageComparisons;
    // 计划里有、但这一轮没做成的比较目标数。>0 即覆盖缺口。
    std::size_t plannedComparisonsNotRun = 0;

    std::vector<PayloadCandidateEntry> payloadCandidates;

    // R0 扫描后端的交叉视图结果。默认全是 NotRequested —— 没驱动时整节静默跳过，
    // 不产生任何缺口（"没打算用"不是"想用没用上"）。
    KernelCrossViewReport kernelCrossView;
    KernelBackendState kernelVadState = KernelBackendState::NotRequested;
    KernelBackendState kernelPteState = KernelBackendState::NotRequested;

    // 可靠展开的栈帧是否可用。深度模式没有它就留缺口，而不是当成"线程都正常"。
    bool reliableStackWalkAvailable = false;
    // 深度模式是否扫了非可执行内存。载荷休眠时可以不保持执行权限，
    // 所以"只看当前带执行权限的页"在深度模式下是一个必须显式记录的缺口。
    bool nonExecutableMemoryScanned = false;

    std::vector<ExceptionRelation> exceptions;
    BudgetStop budgetStop = BudgetStop::Continue;

    // 采集器观测到的额外覆盖缺口（"打算查但没查成"）。会压制"未发现差异"。
    std::vector<std::string> extraCoverageGapKeys;
    // 采集器自报的能力限制（"本版本不做这件事"）。列出来，但不压制结论。
    std::vector<std::string> extraCapabilityLimitKeys;
};

struct SurveyReport final {
    SurveyMode mode = SurveyMode::Fast;
    std::string detectorVersion;
    std::uint32_t ruleSetVersion = kInjectionSurveyRuleSetVersion;

    ProcessInstanceId process;
    OptionalU64 firstObservedUtc100ns;
    IdentityRecheckVerdict identity = IdentityRecheckVerdict::Unverifiable;

    std::vector<InjectionFinding> findings;
    std::vector<ObservationClass> observations;

    std::vector<std::string> coverageGapKeys;      // 打算查但没查成 —— 压制结论
    std::vector<std::string> capabilityLimitKeys;  // 本版本不做 —— 只缩小适用范围
    std::vector<std::string> completedCheckKeys;
    std::vector<std::string> notPerformedCheckKeys;

    std::size_t dynamicCodeRegionCount = 0;
    std::size_t unexplainedImageDiffCount = 0;
    std::size_t exceptionExplainedCount = 0;
    std::size_t threadStartAnomalyCount = 0;
    std::size_t moduleCrossIssueCount = 0;      // 全部交叉视图问题
    std::size_t moduleCrossConflictCount = 0;   // 其中的"矛盾"档，只有它能升结论
    std::size_t kernelCrossIssueCount = 0;      // R3/VAD/页表 三视图之间的差异
    // 自洽载荷结构 **且** 有可靠展开的帧进入其中。只有它能和"归一化差异""交叉视图
    // 矛盾"一起撑起 DifferenceObserved；光有结构不行。
    std::size_t payloadWithExecutionCount = 0;

    CoverageAccount coverage;
    AnalysisConclusion conclusion = AnalysisConclusion::NoEvidence;

    // 覆盖完整度与结论是两个维度。
    //   * scopeIntact：声明要查的范围有没有破。为假时 conclusion 永远不可能是
    //     NoDifferenceObserved —— 这是硬闸门。
    //   * coverageComplete：连能力限制也算上的"什么都不缺"。它比 scopeIntact 严格，
    //     只用于展示；拿它当闸门会让结论永远到不了 NoDifferenceObserved。
    bool scopeIntact = false;
    bool coverageComplete = false;

    bool hasObservation(ObservationClass observation) const noexcept;
    bool hasGap(const std::string& gapKey) const noexcept;
    bool hasLimit(const std::string& limitKey) const noexcept;
};

SurveyReport RunInjectionSurvey(const SurveyInput& input);

} // namespace Ksword::Evidence
