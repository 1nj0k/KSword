// J 模块（进程内存植入与完整性检查）的离线自动测试。
//
// 覆盖 issue #196 第一阶段的五件事及其硬规则：
//   J-01 地址空间索引与保护值分类（含 issue 点名的 `protect & PAGE_EXECUTE` 漏检）
//   J-02 模块交叉视图 L/I/P（缺项推断的资格、WOW64 列表不完整、路径取不到是缺口）
//   J-03 工作集筛选（Valid/Shared 语义、ShareCount 不得替代 Shared、共享不放行）
//   J-04 线程起点落点（起点页现在不可执行不能忽略线索、没采到 ≠ 归属不一致）
//   J-05 归一化 profile 两模式必须相同、参考不确定不得升格成"修改已证实"
// 以及结论层：覆盖不完整永远不可能是"未发现差异"，私有 RX 本身不是"已注入"。
//
// 夹具全部由本文件用代码构造，期望值写成手算常量，不调用被测代码算期望 ——
// 否则测试只是在给实现盖章。

#include "TestSupport.h"

#include "../shared/evidence/InjectionSurvey.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace Ksword::Evidence;

// ---------------------------------------------------------------------------
// 夹具构造
// ---------------------------------------------------------------------------

RegionRecord MakeRegion(const std::uint64_t base,
                        const std::uint64_t size,
                        const RegionState state,
                        const RegionType type,
                        const std::uint32_t protect,
                        const std::uint64_t allocationBase = 0,
                        const std::string& mappedPath = std::string()) {
    RegionRecord record;
    record.base = OptionalU64::of(base);
    record.size = OptionalU64::of(size);
    record.state = state;
    record.type = type;
    record.protection = ToRegionProtection(
        ClassifyWin32Protection(OptionalU64::of(protect)));
    record.allocationBase = OptionalU64::of(allocationBase == 0U ? base : allocationBase);
    record.mappedPath = mappedPath;
    record.source = RegionEvidenceSource::R3VirtualQuery;
    return record;
}

CollectionOutcome Denied() {
    return CollectionOutcome::failure(CollectionStatus::AccessDenied, "WIN32", 5U,
                                      "ERROR_ACCESS_DENIED");
}

ProcessInstanceId MakeProcess(const std::uint64_t pid,
                              const std::uint64_t createTime,
                              const bool withBootId = true) {
    ProcessInstanceId id;
    id.pid = OptionalU64::of(pid);
    if (createTime != 0U) {
        id.createTime100ns = OptionalU64::of(createTime);
    }
    if (withBootId) {
        id.bootId = "boot-1";
    }
    id.imageName = "target.exe";
    return id;
}

ThreadInstanceId MakeThread(const ProcessInstanceId& process, const std::uint64_t tid) {
    ThreadInstanceId thread;
    thread.process = process;
    thread.tid = OptionalU64::of(tid);
    return thread;
}

DriverInstanceId MakeModule(const std::string& path,
                            const std::uint64_t base,
                            const std::uint64_t size) {
    DriverInstanceId module;
    module.imagePath = path;
    module.imageBase = OptionalU64::of(base);
    module.imageSize = OptionalU64::of(size);
    return module;
}

ImageCodeExtent MakeImage(const std::string& path,
                          const std::uint64_t base,
                          const std::uint64_t size,
                          const std::uint64_t codeBegin,
                          const std::uint64_t codeEnd,
                          const bool codeKnown = true) {
    ImageCodeExtent image;
    image.path = path;
    image.base = base;
    image.size = size;
    image.codeBeginRva = codeBegin;
    image.codeEndRva = codeEnd;
    image.codeExtentKnown = codeKnown;
    return image;
}

bool HasGap(const std::vector<std::string>& gaps, const char* key) {
    return std::find(gaps.begin(), gaps.end(), std::string(key)) != gaps.end();
}

bool HasRule(const std::vector<InjectionFinding>& findings, const char* ruleId) {
    return std::any_of(findings.begin(), findings.end(),
                       [ruleId](const InjectionFinding& finding) {
                           return finding.ruleId == ruleId;
                       });
}

const InjectionFinding* FindRule(const std::vector<InjectionFinding>& findings,
                                 const char* const ruleId) {
    const auto hit = std::find_if(findings.begin(), findings.end(),
                                  [ruleId](const InjectionFinding& finding) {
                                      return finding.ruleId == ruleId;
                                  });
    return hit == findings.end() ? nullptr : &*hit;
}

std::size_t CountIssue(const ModuleCrossViewReport& report, const ModuleCrossIssue issue) {
    return static_cast<std::size_t>(
        std::count_if(report.findings.begin(), report.findings.end(),
                      [issue](const ModuleCrossFinding& finding) {
                          return finding.issue == issue;
                      }));
}

// ---------------------------------------------------------------------------
// J-01：保护值分类
// ---------------------------------------------------------------------------
void TestProtectionClassification(KswordTests::Suite& suite) {
    // issue 点名的漏检：PAGE_EXECUTE_READ(0x20) & PAGE_EXECUTE(0x10) == 0，
    // 所以 `protect & PAGE_EXECUTE` 会把 R-X 页判成不可执行。
    suite.expect((0x20U & 0x10U) == 0U, L"J-01 PAGE_EXECUTE_READ 与 PAGE_EXECUTE 无公共位");
    suite.expect((0x40U & 0x10U) == 0U, L"J-01 PAGE_EXECUTE_READWRITE 与 PAGE_EXECUTE 无公共位");
    suite.expect((0x80U & 0x10U) == 0U, L"J-01 PAGE_EXECUTE_WRITECOPY 与 PAGE_EXECUTE 无公共位");

    const ProtectionFacts execute =
        ClassifyWin32Protection(OptionalU64::of(kWin32PageExecute));
    suite.expect(execute.execute == ExecuteProtection::Execute, L"J-01 PAGE_EXECUTE 分类");
    suite.expect(ExecuteProtectionIsExecutable(execute.execute), L"J-01 PAGE_EXECUTE 可执行");
    suite.expect(!execute.readable, L"J-01 PAGE_EXECUTE 不可读");

    const ProtectionFacts executeRead =
        ClassifyWin32Protection(OptionalU64::of(kWin32PageExecuteRead));
    suite.expect(executeRead.execute == ExecuteProtection::ExecuteRead, L"J-01 R-X 分类");
    suite.expect(ExecuteProtectionIsExecutable(executeRead.execute), L"J-01 R-X 可执行");
    suite.expect(executeRead.readable && !executeRead.writable, L"J-01 R-X 读写位");

    const ProtectionFacts rwx =
        ClassifyWin32Protection(OptionalU64::of(kWin32PageExecuteReadWrite));
    suite.expect(rwx.execute == ExecuteProtection::ExecuteReadWrite, L"J-01 RWX 分类");
    suite.expect(rwx.writable && rwx.readable, L"J-01 RWX 读写位");
    suite.expect(!rwx.copyOnWrite, L"J-01 RWX 非写时复制");

    const ProtectionFacts wcx =
        ClassifyWin32Protection(OptionalU64::of(kWin32PageExecuteWriteCopy));
    suite.expect(wcx.execute == ExecuteProtection::ExecuteWriteCopy, L"J-01 RXC 分类");
    suite.expect(wcx.copyOnWrite, L"J-01 RXC 写时复制位");
    suite.expect(ExecuteProtectionIsExecutable(wcx.execute), L"J-01 RXC 可执行");

    const ProtectionFacts rw = ClassifyWin32Protection(OptionalU64::of(kWin32PageReadWrite));
    suite.expect(rw.execute == ExecuteProtection::NotExecutable, L"J-01 RW 不可执行");
    suite.expect(rw.readable && rw.writable, L"J-01 RW 读写位");

    const ProtectionFacts noAccess =
        ClassifyWin32Protection(OptionalU64::of(kWin32PageNoAccess));
    suite.expect(noAccess.noAccess, L"J-01 NOACCESS 标记");
    suite.expect(!noAccess.readable, L"J-01 NOACCESS 不可读");

    const ProtectionFacts writeCopy =
        ClassifyWin32Protection(OptionalU64::of(kWin32PageWriteCopy));
    suite.expect(writeCopy.copyOnWrite && !ExecuteProtectionIsExecutable(writeCopy.execute),
                 L"J-01 WRITECOPY 写时复制且不可执行");

    // PAGE_GUARD 是修饰位，不改变基本保护值。
    const ProtectionFacts guarded = ClassifyWin32Protection(
        OptionalU64::of(kWin32PageExecuteRead | kWin32PageGuard));
    suite.expect(guarded.guard, L"J-01 PAGE_GUARD 单独成位");
    suite.expect(guarded.execute == ExecuteProtection::ExecuteRead,
                 L"J-01 PAGE_GUARD 不改变基本保护值");

    // 其它高位修饰同理不得影响基本值判定。
    const ProtectionFacts noCache = ClassifyWin32Protection(
        OptionalU64::of(kWin32PageExecuteReadWrite | kWin32PageNoCache |
                        kWin32PageWriteCombine | kWin32PageTargetsNoUpdate));
    suite.expect(noCache.execute == ExecuteProtection::ExecuteReadWrite,
                 L"J-01 高位修饰不影响基本保护值");

    // 看不懂的基本值：既不是可执行也不是不可执行。
    const ProtectionFacts weird = ClassifyWin32Protection(OptionalU64::of(0x07U));
    suite.expect(weird.unrecognizedBase, L"J-01 未知基本值置位");
    suite.expect(weird.execute == ExecuteProtection::Unknown, L"J-01 未知基本值不下判断");

    // 没给原始值 = 未知，绝不是"不可执行"。
    const ProtectionFacts unset = ClassifyWin32Protection(OptionalU64::unset());
    suite.expect(unset.execute == ExecuteProtection::Unknown, L"J-01 缺原始值即未知");
    suite.expect(!ExecuteProtectionIsExecutable(unset.execute), L"J-01 未知不算可执行");
    suite.expect(!unset.rawValue.present, L"J-01 缺原始值不补 0");

    // ToRegionProtection 往回投影。
    const RegionProtection projected = ToRegionProtection(rwx);
    suite.expect(projected.executable && projected.writable && projected.readable,
                 L"J-01 RWX 投影到 RegionProtection");
    suite.expect(projected.rawValue.present &&
                     projected.rawValue.value == kWin32PageExecuteReadWrite,
                 L"J-01 原始值无损保留");

    // EffectiveProtection：有原始值时以原始值为准，可纠正调用方算错的 executable。
    RegionRecord wrongFlags =
        MakeRegion(0x10000U, 0x1000U, RegionState::Commit, RegionType::Private,
                   kWin32PageExecuteRead);
    wrongFlags.protection.executable = false;  // 模拟调用方用错误位与算出来的结果
    const ProtectionFacts corrected = EffectiveProtection(wrongFlags);
    suite.expect(corrected.execute == ExecuteProtection::ExecuteRead,
                 L"J-01 原始值纠正错误的 executable 位");

    RegionRecord noRaw;
    noRaw.base = OptionalU64::of(0x20000U);
    noRaw.size = OptionalU64::of(0x1000U);
    noRaw.state = RegionState::Commit;
    noRaw.type = RegionType::Private;
    const ProtectionFacts empty = EffectiveProtection(noRaw);
    suite.expect(empty.execute == ExecuteProtection::Unknown,
                 L"J-01 保护字段全默认视为未知");

    noRaw.protection.readable = true;
    noRaw.protection.executable = true;
    const ProtectionFacts fallback = EffectiveProtection(noRaw);
    suite.expect(fallback.execute == ExecuteProtection::ExecuteRead,
                 L"J-01 无原始值时退回布尔字段");
}

// ---------------------------------------------------------------------------
// J-01：区域代码分类
// ---------------------------------------------------------------------------
void TestRegionCodeClass(KswordTests::Suite& suite) {
    const RegionRecord imageExec = MakeRegion(0x140001000ULL, 0x1000U, RegionState::Commit,
                                              RegionType::Image, kWin32PageExecuteRead);
    suite.expect(ClassifyRegionCode(imageExec) == RegionCodeClass::ImageExecutable,
                 L"J-01 映像可执行页分类");
    suite.expect(!IsDynamicCodeCandidate(RegionCodeClass::ImageExecutable),
                 L"J-01 映像可执行页不是动态代码候选");

    const RegionRecord privateExec = MakeRegion(0x200000U, 0x1000U, RegionState::Commit,
                                                RegionType::Private, kWin32PageExecuteRead);
    suite.expect(ClassifyRegionCode(privateExec) == RegionCodeClass::PrivateExecutable,
                 L"J-01 私有可执行页分类");
    suite.expect(IsDynamicCodeCandidate(RegionCodeClass::PrivateExecutable),
                 L"J-01 私有可执行页是候选");

    // "不能只扫私有内存，否则会漏掉映射型的非映像代码"。
    const RegionRecord mappedExec = MakeRegion(0x300000U, 0x1000U, RegionState::Commit,
                                               RegionType::Mapped, kWin32PageExecuteReadWrite);
    suite.expect(ClassifyRegionCode(mappedExec) == RegionCodeClass::MappedExecutable,
                 L"J-01 映射可执行页分类");
    suite.expect(IsDynamicCodeCandidate(RegionCodeClass::MappedExecutable),
                 L"J-01 映射可执行页是候选");

    const RegionRecord imageData = MakeRegion(0x140010000ULL, 0x1000U, RegionState::Commit,
                                              RegionType::Image, kWin32PageReadWrite);
    suite.expect(ClassifyRegionCode(imageData) == RegionCodeClass::NonExecutable,
                 L"J-01 映像数据页不可执行");

    const RegionRecord reserved = MakeRegion(0x400000U, 0x1000U, RegionState::Reserved,
                                             RegionType::Private, kWin32PageNoAccess);
    suite.expect(ClassifyRegionCode(reserved) == RegionCodeClass::NotCommitted,
                 L"J-01 未提交区域不参与代码分类");

    const RegionRecord freeRegion = MakeRegion(0x500000U, 0x1000U, RegionState::Free,
                                               RegionType::Unknown, kWin32PageNoAccess);
    suite.expect(ClassifyRegionCode(freeRegion) == RegionCodeClass::NotCommitted,
                 L"J-01 空闲区域不参与代码分类");

    // 可执行但类型未知：不能硬塞进任何一档。
    const RegionRecord unknownType = MakeRegion(0x600000U, 0x1000U, RegionState::Commit,
                                                RegionType::Unknown, kWin32PageExecuteRead);
    suite.expect(ClassifyRegionCode(unknownType) == RegionCodeClass::Unknown,
                 L"J-01 类型未知不硬判");
    suite.expect(!IsDynamicCodeCandidate(RegionCodeClass::Unknown),
                 L"J-01 未知不是候选");
}

// ---------------------------------------------------------------------------
// J-01：地址空间索引
// ---------------------------------------------------------------------------
void TestAddressSpaceIndex(KswordTests::Suite& suite) {
    // 一块 0x100000 起、三个子区域的私有分配：RW / RX / RW。
    // 聚合后仍必须能看到中间那一页是 RX —— 这正是"不能丢失每个子区域自己的权限"。
    std::vector<RegionRecord> records;
    records.push_back(MakeRegion(0x102000U, 0x1000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageReadWrite, 0x100000U));
    records.push_back(MakeRegion(0x100000U, 0x1000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageReadWrite, 0x100000U));
    records.push_back(MakeRegion(0x101000U, 0x1000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageExecuteRead, 0x100000U));
    records.push_back(MakeRegion(0x140000000ULL, 0x2000U, RegionState::Commit,
                                 RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                 "C:\\app\\target.exe"));

    const AddressSpaceIndex index = BuildAddressSpaceIndex(records, CollectionOutcome::success());
    suite.expect(index.entries.size() == 4U, L"J-01 索引保留全部子区域");
    suite.expect(index.searchableCount == 4U, L"J-01 全部记录可参与查找");
    suite.expect(index.entries[0].base.value == 0x100000U, L"J-01 索引按 base 升序");
    suite.expect(index.entries[3].base.value == 0x140000000ULL, L"J-01 索引末项是映像");
    suite.expect(index.groups.size() == 2U, L"J-01 按 AllocationBase 聚合成两组");
    suite.expect(index.dynamicCodeCandidateCount == 1U, L"J-01 动态代码候选计数");
    suite.expect(index.committedBytes == 0x5000U, L"J-01 已提交字节合计");
    suite.expect(index.executableBytes == 0x3000U, L"J-01 可执行字节合计");

    const std::size_t rxEntry = index.findEntry(0x101800U);
    suite.expect(rxEntry != AddressSpaceIndex::kNoEntry, L"J-01 命中 RX 子区域");
    suite.expect(index.codeClasses[rxEntry] == RegionCodeClass::PrivateExecutable,
                 L"J-01 聚合后子区域权限未被吞掉");

    const AllocationGroup* group = index.groupForEntry(rxEntry);
    suite.expect(group != nullptr, L"J-01 子区域可回到分配组");
    if (group != nullptr) {
        suite.expect(group->allocationBase.present && group->allocationBase.value == 0x100000U,
                     L"J-01 分配组基址");
        suite.expect(group->entryIndices.size() == 3U, L"J-01 分配组含三个子区域");
        suite.expect(group->anyExecutable, L"J-01 分配组标记含可执行子区域");
        suite.expect(!group->anyWritableExecutable, L"J-01 R-X 不是可写可执行");
        suite.expect(group->executableBytes == 0x1000U, L"J-01 分配组可执行字节");
        suite.expect(group->committedBytes == 0x3000U, L"J-01 分配组已提交字节");
        suite.expect(!group->typeMixed, L"J-01 同类型不标混合");
    }

    // 边界：区间右端开。
    suite.expect(index.findEntry(0x100000U) != AddressSpaceIndex::kNoEntry, L"J-01 左端命中");
    suite.expect(index.findEntry(0x102FFFU) != AddressSpaceIndex::kNoEntry, L"J-01 右端内命中");
    suite.expect(index.findEntry(0x103000U) == AddressSpaceIndex::kNoEntry,
                 L"J-01 右端开区间不命中");
    suite.expect(index.findEntry(0x0FFFFFU) == AddressSpaceIndex::kNoEntry, L"J-01 左侧空洞");
    suite.expect(index.findEntry(0x7FFFFFFFFFFFULL) == AddressSpaceIndex::kNoEntry,
                 L"J-01 索引外地址不编造归属");

    suite.expect(index.coverage.succeeded == 4U, L"J-01 账目成功数");
    suite.expect(index.coverage.failed == 0U, L"J-01 账目失败数");
    suite.expect(index.usableForAbsenceInference(), L"J-01 成功且完整才可做缺项推断");

    // 残缺记录：保留但不参与查找，并计入 failed。
    RegionRecord broken;
    broken.base = OptionalU64::of(0x900000U);
    broken.state = RegionState::Commit;
    broken.type = RegionType::Private;
    std::vector<RegionRecord> withBroken = records;
    withBroken.push_back(broken);
    const AddressSpaceIndex brokenIndex =
        BuildAddressSpaceIndex(withBroken, CollectionOutcome::success());
    suite.expect(brokenIndex.entries.size() == 5U, L"J-01 残缺记录不被丢弃");
    suite.expect(brokenIndex.searchableCount == 4U, L"J-01 残缺记录不参与查找");
    suite.expect(brokenIndex.coverage.failed == 1U, L"J-01 残缺记录计入失败");
    suite.expect(!brokenIndex.coverage.fullyCovered(), L"J-01 有失败即非完整覆盖");
    suite.expect(!brokenIndex.usableForAbsenceInference(), L"J-01 不完整不得做缺项推断");
    suite.expect(brokenIndex.findEntry(0x900000U) == AddressSpaceIndex::kNoEntry,
                 L"J-01 残缺记录不产生假命中");

    // 采集失败：哪怕列表恰好完整，也没有缺项推断资格。
    const AddressSpaceIndex deniedIndex = BuildAddressSpaceIndex(records, Denied());
    suite.expect(!deniedIndex.usableForAbsenceInference(), L"J-01 拒绝访问不得做缺项推断");
    suite.expect(deniedIndex.outcome.nativeCode.present && deniedIndex.outcome.nativeCode.value == 5U,
                 L"J-01 失败保留原始错误码");

    // 没有 AllocationBase 的区域自成一组，不得并进别人的组。
    RegionRecord orphan = MakeRegion(0x800000U, 0x1000U, RegionState::Commit,
                                     RegionType::Private, kWin32PageReadWrite);
    orphan.allocationBase = OptionalU64::unset();
    std::vector<RegionRecord> withOrphan = records;
    withOrphan.push_back(orphan);
    const AddressSpaceIndex orphanIndex =
        BuildAddressSpaceIndex(withOrphan, CollectionOutcome::success());
    suite.expect(orphanIndex.groups.size() == 3U, L"J-01 无 AllocationBase 自成一组");

    // 空输入 + 失败状态：不是"地址空间是空的"。
    const AddressSpaceIndex emptyDenied = BuildAddressSpaceIndex({}, Denied());
    suite.expect(emptyDenied.entries.empty(), L"J-01 空输入无条目");
    suite.expect(!emptyDenied.usableForAbsenceInference(), L"J-01 空+失败不等于确认为空");
}

// ---------------------------------------------------------------------------
// J-01b：进程列表用的廉价筛选汇总
// ---------------------------------------------------------------------------
void TestSurfaceScreen(KswordTests::Suite& suite) {
    suite.expect(SurfaceScreenCountsAreMeaningful(SurfaceScreenState::Screened),
                 L"筛选 只有已筛选的计数有意义");
    suite.expect(!SurfaceScreenCountsAreMeaningful(SurfaceScreenState::AccessDenied),
                 L"筛选 访问受限的 0 不是没有");
    suite.expect(!SurfaceScreenCountsAreMeaningful(SurfaceScreenState::NotScreened),
                 L"筛选 未筛选的 0 不是没有");
    suite.expect(!SurfaceScreenCountsAreMeaningful(SurfaceScreenState::Failed),
                 L"筛选 失败的 0 不是没有");

    std::vector<RegionRecord> records;
    records.push_back(MakeRegion(0x140000000ULL, 0x10000U, RegionState::Commit,
                                 RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                 "C:\\app\\target.exe"));
    records.push_back(MakeRegion(0x200000U, 0x1000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageExecuteReadWrite));   // 私有 RWX
    records.push_back(MakeRegion(0x300000U, 0x2000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageExecuteRead));        // 私有 R-X（不可写）
    records.push_back(MakeRegion(0x400000U, 0x4000U, RegionState::Commit, RegionType::Mapped,
                                 kWin32PageExecuteRead));        // 映射 R-X
    records.push_back(MakeRegion(0x500000U, 0x8000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageReadWrite));          // 普通 RW，不计

    const AddressSpaceIndex index = BuildAddressSpaceIndex(records, CollectionOutcome::success());
    const ProcessSurfaceScreen screen =
        SummarizeSurfaceScreen(index, OptionalU64::of(0x1D100000000ULL));
    suite.expect(screen.state == SurfaceScreenState::Screened, L"筛选 成功态");
    suite.expect(screen.regionCount == 5U, L"筛选 区域总数");
    suite.expect(screen.dynamicCodeRegions == 3U, L"筛选 动态代码区域数（私有+映射）");
    suite.expect(screen.writableExecutableRegions == 1U, L"筛选 可写可执行区域数");
    suite.expect(screen.dynamicCodeBytes == 0x7000U, L"筛选 动态代码字节数");
    suite.expect(screen.screenedUtc100ns.present &&
                     screen.screenedUtc100ns.value == 0x1D100000000ULL,
                 L"筛选 记录首次观测时间");

    // 访问受限：计数保持 0，但状态说明这 0 是"不知道"。
    const AddressSpaceIndex denied = BuildAddressSpaceIndex(records, Denied());
    const ProcessSurfaceScreen deniedScreen =
        SummarizeSurfaceScreen(denied, OptionalU64::unset());
    suite.expect(deniedScreen.state == SurfaceScreenState::AccessDenied,
                 L"筛选 拒绝访问态");
    suite.expect(deniedScreen.dynamicCodeRegions == 0U, L"筛选 拒绝访问不给计数");
    suite.expect(!SurfaceScreenCountsAreMeaningful(deniedScreen.state),
                 L"筛选 拒绝访问的计数不可用");

    CollectionOutcome notCollected;
    const AddressSpaceIndex empty = BuildAddressSpaceIndex({}, notCollected);
    suite.expect(SummarizeSurfaceScreen(empty, OptionalU64::unset()).state ==
                     SurfaceScreenState::NotScreened,
                 L"筛选 未采集态");

    CollectionOutcome unsupported;
    unsupported.status = CollectionStatus::Unsupported;
    const AddressSpaceIndex unsup = BuildAddressSpaceIndex(records, unsupported);
    suite.expect(SummarizeSurfaceScreen(unsup, OptionalU64::unset()).state ==
                     SurfaceScreenState::Failed,
                 L"筛选 不支持归入失败态");

    // 部分成功仍然给计数（跑了就是跑了），但调用方能从状态看出不完整。
    CollectionOutcome partial;
    partial.status = CollectionStatus::Partial;
    const AddressSpaceIndex partialIndex = BuildAddressSpaceIndex(records, partial);
    const ProcessSurfaceScreen partialScreen =
        SummarizeSurfaceScreen(partialIndex, OptionalU64::unset());
    suite.expect(partialScreen.state == SurfaceScreenState::Screened,
                 L"筛选 部分成功仍给计数");
    suite.expect(partialScreen.dynamicCodeRegions == 3U, L"筛选 部分成功计数一致");
}

// ---------------------------------------------------------------------------
// J-02：模块交叉视图
// ---------------------------------------------------------------------------
void TestModuleCrossView(KswordTests::Suite& suite) {
    ModuleCrossViewInput input;
    input.loaderOutcome = CollectionOutcome::success();
    input.imageOutcome = CollectionOutcome::success();
    input.payloadOutcome = CollectionOutcome::success();
    input.loaderTrust = ModuleEnumerationTrust::Trusted;
    input.mainImagePathFromLoader = "C:\\app\\target.exe";
    input.mainImagePathFromKernel = "C:\\app\\target.exe";
    input.mainImagePathFromMapping = "\\Device\\HarddiskVolume3\\app\\target.exe";
    input.mainImageBaseFromLoader = OptionalU64::of(0x140000000ULL);
    input.mainImageBaseFromMapping = OptionalU64::of(0x140000000ULL);

    LoaderModuleEntry main;
    main.module = MakeModule("C:\\app\\target.exe", 0x140000000ULL, 0x10000U);
    main.listedName = "target.exe";
    main.isMainImage = true;
    LoaderModuleEntry ntdll;
    ntdll.module = MakeModule("C:\\Windows\\System32\\ntdll.dll", 0x7FF800000000ULL, 0x20000U);
    ntdll.listedName = "ntdll.dll";
    input.loaderView = { main, ntdll };

    ImageMappingEntry mainMap;
    mainMap.allocationBase = OptionalU64::of(0x140000000ULL);
    mainMap.mappedSize = OptionalU64::of(0x10000U);
    mainMap.mappedPath = "\\Device\\HarddiskVolume3\\app\\target.exe";
    mainMap.pathOutcome = CollectionOutcome::success();
    ImageMappingEntry ntdllMap;
    ntdllMap.allocationBase = OptionalU64::of(0x7FF800000000ULL);
    ntdllMap.mappedSize = OptionalU64::of(0x20000U);
    ntdllMap.mappedPath = "\\Device\\HarddiskVolume3\\Windows\\System32\\ntdll.dll";
    ntdllMap.pathOutcome = CollectionOutcome::success();
    input.imageView = { mainMap, ntdllMap };

    const ModuleCrossViewReport clean = EvaluateModuleCrossView(input);
    suite.expect(clean.matchedModules == 2U, L"J-02 两个模块都对上");
    suite.expect(clean.findings.empty(), L"J-02 设备路径与 DOS 路径不产生假不一致");
    suite.expect(clean.absenceInferenceAllowed, L"J-02 两侧成功才允许缺项推断");
    suite.expect(clean.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"J-02 干净场景结论");
    suite.expect(clean.coverageGapKeys.empty(), L"J-02 干净场景无缺口");

    // 手工映射：有映像映射，加载器列表里没有。
    ModuleCrossViewInput manual = input;
    ImageMappingEntry ghost;
    ghost.allocationBase = OptionalU64::of(0x7FF900000000ULL);
    ghost.mappedSize = OptionalU64::of(0x8000U);
    ghost.mappedPath = "\\Device\\HarddiskVolume3\\temp\\payload.dll";
    ghost.pathOutcome = CollectionOutcome::success();
    manual.imageView.push_back(ghost);
    const ModuleCrossViewReport manualReport = EvaluateModuleCrossView(manual);
    suite.expect(manualReport.mappingOnly == 1U, L"J-02 映像无加载器项计数");
    suite.expect(CountIssue(manualReport, ModuleCrossIssue::ImageMappingWithoutLoaderEntry) == 1U,
                 L"J-02 映像无加载器项产出");
    suite.expect(manualReport.conclusion == AnalysisConclusion::DifferenceObserved,
                 L"J-02 交叉矛盾结论");

    // 加载器有项但没有映射。
    ModuleCrossViewInput phantom = input;
    LoaderModuleEntry unmapped;
    unmapped.module = MakeModule("C:\\temp\\phantom.dll", 0x7FFA00000000ULL, 0x4000U);
    unmapped.listedName = "phantom.dll";
    phantom.loaderView.push_back(unmapped);
    const ModuleCrossViewReport phantomReport = EvaluateModuleCrossView(phantom);
    suite.expect(phantomReport.loaderOnly == 1U, L"J-02 加载器项无映射计数");
    suite.expect(CountIssue(phantomReport, ModuleCrossIssue::LoaderEntryWithoutImageMapping) == 1U,
                 L"J-02 加载器项无映射产出");

    // 加载器视图失败：缺项推断一律不做，只留缺口。
    ModuleCrossViewInput denied = manual;
    denied.loaderOutcome = Denied();
    const ModuleCrossViewReport deniedReport = EvaluateModuleCrossView(denied);
    suite.expect(!deniedReport.absenceInferenceAllowed, L"J-02 视图失败即无缺项资格");
    suite.expect(CountIssue(deniedReport, ModuleCrossIssue::ImageMappingWithoutLoaderEntry) == 0U,
                 L"J-02 视图失败不产出缺项");
    suite.expect(HasGap(deniedReport.coverageGapKeys, kGapLoaderViewUnavailable),
                 L"J-02 视图失败留缺口键");
    suite.expect(deniedReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"J-02 视图失败不得表述为未发现差异");

    // 部分成功同样没有缺项资格：Partial 不是 Success。
    ModuleCrossViewInput partial = manual;
    partial.imageOutcome.status = CollectionStatus::Partial;
    const ModuleCrossViewReport partialReport = EvaluateModuleCrossView(partial);
    suite.expect(!partialReport.absenceInferenceAllowed, L"J-02 部分成功无缺项资格");
    suite.expect(HasGap(partialReport.coverageGapKeys, kGapImageViewUnavailable),
                 L"J-02 部分成功留缺口键");

    // WOW64 采集器：模块过滤参数被忽略，列表天然不完整。
    ModuleCrossViewInput wow = manual;
    wow.loaderTrust = ModuleEnumerationTrust::FilterIgnoredUnderWow64;
    const ModuleCrossViewReport wowReport = EvaluateModuleCrossView(wow);
    suite.expect(!wowReport.absenceInferenceAllowed, L"J-02 WOW64 列表无缺项资格");
    suite.expect(HasGap(wowReport.coverageGapKeys, kGapModuleEnumerationWow64),
                 L"J-02 WOW64 留缺口键");

    // 路径取不到：是缺口，不是"无文件植入"，也不是"观测到差异"。
    ModuleCrossViewInput noPath = input;
    noPath.imageView[1].mappedPath.clear();
    noPath.imageView[1].pathOutcome = Denied();
    const ModuleCrossViewReport noPathReport = EvaluateModuleCrossView(noPath);
    suite.expect(CountIssue(noPathReport, ModuleCrossIssue::MappedPathUnavailable) == 1U,
                 L"J-02 路径失败留证据");
    suite.expect(HasGap(noPathReport.coverageGapKeys, kGapMappedPathUnavailable),
                 L"J-02 路径失败留缺口键");
    suite.expect(noPathReport.conclusion != AnalysisConclusion::DifferenceObserved,
                 L"J-02 路径失败本身不是差异");
    suite.expect(noPathReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"J-02 路径失败不得表述为未发现差异");

    // 路径不一致。
    ModuleCrossViewInput mismatch = input;
    mismatch.imageView[1].mappedPath = "\\Device\\HarddiskVolume3\\temp\\fake.dll";
    const ModuleCrossViewReport mismatchReport = EvaluateModuleCrossView(mismatch);
    suite.expect(CountIssue(mismatchReport, ModuleCrossIssue::LoaderPathMismatch) == 1U,
                 L"J-02 路径不一致产出");

    // 大小不一致：超过容差才报。
    ModuleCrossViewInput sizeClose = input;
    sizeClose.imageView[1].mappedSize = OptionalU64::of(0x20000U + 0x1000U);
    const ModuleCrossViewReport sizeCloseReport = EvaluateModuleCrossView(sizeClose);
    suite.expect(CountIssue(sizeCloseReport, ModuleCrossIssue::LoaderSizeMismatch) == 0U,
                 L"J-02 容差内不报大小不一致");

    ModuleCrossViewInput sizeFar = input;
    sizeFar.imageView[1].mappedSize = OptionalU64::of(0x200000U);
    const ModuleCrossViewReport sizeFarReport = EvaluateModuleCrossView(sizeFar);
    suite.expect(CountIssue(sizeFarReport, ModuleCrossIssue::LoaderSizeMismatch) == 1U,
                 L"J-02 超容差报大小不一致");

    // 主映像自相矛盾。
    ModuleCrossViewInput conflict = input;
    conflict.mainImagePathFromKernel = "C:\\Windows\\System32\\svchost.exe";
    const ModuleCrossViewReport conflictReport = EvaluateModuleCrossView(conflict);
    suite.expect(CountIssue(conflictReport, ModuleCrossIssue::MainImageIdentityConflict) == 1U,
                 L"J-02 主映像身份矛盾产出");

    ModuleCrossViewInput baseConflict = input;
    baseConflict.mainImageBaseFromMapping = OptionalU64::of(0x150000000ULL);
    const ModuleCrossViewReport baseConflictReport = EvaluateModuleCrossView(baseConflict);
    suite.expect(CountIssue(baseConflictReport, ModuleCrossIssue::MainImageIdentityConflict) == 1U,
                 L"J-02 主映像基址矛盾产出");

    // 主映像来源不足两个：缺口。
    ModuleCrossViewInput oneSource = input;
    oneSource.mainImagePathFromKernel.clear();
    oneSource.mainImagePathFromMapping.clear();
    const ModuleCrossViewReport oneSourceReport = EvaluateModuleCrossView(oneSource);
    suite.expect(HasGap(oneSourceReport.coverageGapKeys, kGapMainImageSourceMissing),
                 L"J-02 主映像来源不足留缺口");

    // 两侧都没有观测：NoEvidence，不是"未发现差异"。
    ModuleCrossViewInput nothing;
    nothing.loaderOutcome = Denied();
    nothing.imageOutcome = Denied();
    nothing.payloadOutcome = Denied();
    const ModuleCrossViewReport nothingReport = EvaluateModuleCrossView(nothing);
    suite.expect(nothingReport.conclusion == AnalysisConclusion::NoEvidence,
                 L"J-02 无观测即无证据");

    // 载荷候选计数透传。
    ModuleCrossViewInput payload = input;
    PayloadCandidateEntry candidate;
    candidate.base = OptionalU64::of(0x300000U);
    candidate.size = OptionalU64::of(0x2000U);
    candidate.type = RegionType::Private;
    candidate.structure = PayloadStructure::HeaderErasedPe;
    candidate.outcome = CollectionOutcome::success();
    payload.payloadView.push_back(candidate);
    const ModuleCrossViewReport payloadReport = EvaluateModuleCrossView(payload);
    suite.expect(payloadReport.payloadCandidates == 1U, L"J-02 载荷候选计数");
}

// ---------------------------------------------------------------------------
// J-03：工作集筛选
// ---------------------------------------------------------------------------
void TestWorkingSetScreen(KswordTests::Suite& suite) {
    WorkingSetPageFact notQueried;
    notQueried.va = 0x140001000ULL;
    suite.expect(ScreenWorkingSetPage(notQueried) == PageScreenVerdict::NotQueried,
                 L"J-03 没查过就是没查过");

    WorkingSetPageFact invalid;
    invalid.va = 0x140001000ULL;
    invalid.queried = true;
    invalid.valid = false;
    invalid.shared = true;  // Valid==0 时这个字段不该被当真
    suite.expect(ScreenWorkingSetPage(invalid) == PageScreenVerdict::InvalidNeedsRecheck,
                 L"J-03 无效页保守标待补查");

    WorkingSetPageFact invalidNotShared = invalid;
    invalidNotShared.shared = false;
    suite.expect(ScreenWorkingSetPage(invalidNotShared) == PageScreenVerdict::InvalidNeedsRecheck,
                 L"J-03 无效页不因 Shared 取值而改判");

    WorkingSetPageFact privatized;
    privatized.va = 0x140002000ULL;
    privatized.queried = true;
    privatized.valid = true;
    privatized.shared = false;
    suite.expect(ScreenWorkingSetPage(privatized) == PageScreenVerdict::PrivatizedCandidate,
                 L"J-03 有效且不可共享是私有化候选");

    // ShareCount == 1 不能代替 Shared：这一页仍然是"可共享"。
    WorkingSetPageFact sharedOne;
    sharedOne.va = 0x140003000ULL;
    sharedOne.queried = true;
    sharedOne.valid = true;
    sharedOne.shared = true;
    sharedOne.shareCount = OptionalU64::of(1U);
    suite.expect(ScreenWorkingSetPage(sharedOne) == PageScreenVerdict::SharedNotCleared,
                 L"J-03 ShareCount==1 不代替 Shared");

    WorkingSetPageFact sharedMany = sharedOne;
    sharedMany.shareCount = OptionalU64::of(7U);
    suite.expect(ScreenWorkingSetPage(sharedMany) == PageScreenVerdict::SharedNotCleared,
                 L"J-03 多进程共享同样不放行");

    // 快速模式只挑私有化页；深度模式不用共享状态排除任何映像页。
    suite.expect(PageSelectedForComparison(PageScreenVerdict::PrivatizedCandidate, SurveyMode::Fast),
                 L"J-03 快扫比较私有化页");
    suite.expect(!PageSelectedForComparison(PageScreenVerdict::SharedNotCleared, SurveyMode::Fast),
                 L"J-03 快扫优先不比较共享页");
    suite.expect(PageSelectedForComparison(PageScreenVerdict::SharedNotCleared, SurveyMode::Deep),
                 L"J-03 深扫不排除共享页");
    suite.expect(PageSelectedForComparison(PageScreenVerdict::InvalidNeedsRecheck, SurveyMode::Deep),
                 L"J-03 深扫补查无效页");
    suite.expect(!PageSelectedForComparison(PageScreenVerdict::NotQueried, SurveyMode::Deep),
                 L"J-03 没查过的页不能当成已比较");
    suite.expect(!PageSelectedForComparison(PageScreenVerdict::NotQueried, SurveyMode::Fast),
                 L"J-03 快扫同样不把未查页当已比较");
}

// ---------------------------------------------------------------------------
// J-04：线程起点
// ---------------------------------------------------------------------------
void TestThreadStarts(KswordTests::Suite& suite) {
    std::vector<RegionRecord> records;
    // 主映像 0x140000000，代码 RVA [0x1000, 0x5000)。
    records.push_back(MakeRegion(0x140000000ULL, 0x10000U, RegionState::Commit,
                                 RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                 "C:\\app\\target.exe"));
    // 私有可执行块。
    records.push_back(MakeRegion(0x300000U, 0x1000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageExecuteReadWrite));
    // 私有但当前不可执行（载荷休眠）。
    records.push_back(MakeRegion(0x310000U, 0x1000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageReadWrite));
    // 映射非映像。
    records.push_back(MakeRegion(0x320000U, 0x1000U, RegionState::Commit, RegionType::Mapped,
                                 kWin32PageExecuteRead, 0x320000U, "C:\\temp\\blob.bin"));
    // 已保留未提交。
    records.push_back(MakeRegion(0x330000U, 0x1000U, RegionState::Reserved, RegionType::Private,
                                 kWin32PageNoAccess));

    const AddressSpaceIndex index = BuildAddressSpaceIndex(records, CollectionOutcome::success());
    const std::vector<ImageCodeExtent> images = {
        MakeImage("C:\\app\\target.exe", 0x140000000ULL, 0x10000U, 0x1000U, 0x5000U),
    };

    const ProcessInstanceId process = MakeProcess(4321U, 0x1D000000000ULL);

    std::vector<ThreadStartInput> threads;
    ThreadStartInput inCode;
    inCode.thread = MakeThread(process, 1001U);
    inCode.startAddress = OptionalU64::of(0x140002000ULL);
    inCode.startAddressOutcome = CollectionOutcome::success();
    threads.push_back(inCode);

    ThreadStartInput outsideCode;
    outsideCode.thread = MakeThread(process, 1002U);
    outsideCode.startAddress = OptionalU64::of(0x140008000ULL);
    outsideCode.startAddressOutcome = CollectionOutcome::success();
    threads.push_back(outsideCode);

    ThreadStartInput privateStart;
    privateStart.thread = MakeThread(process, 1003U);
    privateStart.startAddress = OptionalU64::of(0x300100U);
    privateStart.startAddressOutcome = CollectionOutcome::success();
    threads.push_back(privateStart);

    ThreadStartInput dormant;
    dormant.thread = MakeThread(process, 1004U);
    dormant.startAddress = OptionalU64::of(0x310100U);
    dormant.startAddressOutcome = CollectionOutcome::success();
    threads.push_back(dormant);

    ThreadStartInput mappedStart;
    mappedStart.thread = MakeThread(process, 1005U);
    mappedStart.startAddress = OptionalU64::of(0x320100U);
    mappedStart.startAddressOutcome = CollectionOutcome::success();
    threads.push_back(mappedStart);

    ThreadStartInput reservedStart;
    reservedStart.thread = MakeThread(process, 1006U);
    reservedStart.startAddress = OptionalU64::of(0x330100U);
    reservedStart.startAddressOutcome = CollectionOutcome::success();
    threads.push_back(reservedStart);

    ThreadStartInput unmapped;
    unmapped.thread = MakeThread(process, 1007U);
    unmapped.startAddress = OptionalU64::of(0x900000U);
    unmapped.startAddressOutcome = CollectionOutcome::success();
    threads.push_back(unmapped);

    ThreadStartInput missing;
    missing.thread = MakeThread(process, 1008U);
    missing.startAddressOutcome = Denied();
    threads.push_back(missing);

    // 起点落在合法模块里，但第一跳离开该模块（trampoline）。
    ThreadStartInput trampoline;
    trampoline.thread = MakeThread(process, 1009U);
    trampoline.startAddress = OptionalU64::of(0x140003000ULL);
    trampoline.startAddressOutcome = CollectionOutcome::success();
    trampoline.entryInspected = true;
    trampoline.entryReadable = true;
    trampoline.immediateBranchTarget = OptionalU64::of(0x300200U);
    threads.push_back(trampoline);

    const std::vector<ThreadStartFinding> findings =
        EvaluateThreadStarts(threads, index, images);
    suite.expect(findings.size() == 9U, L"J-04 每个线程一条结果");

    suite.expect(findings[0].landing == ThreadStartLanding::ImageCodeRange,
                 L"J-04 起点落在映像代码范围");
    suite.expect(findings[0].owningPath == "C:\\app\\target.exe", L"J-04 归属模块路径");

    suite.expect(findings[1].landing == ThreadStartLanding::ImageOutsideCode,
                 L"J-04 起点在映像内但不符合代码布局");

    suite.expect(findings[2].landing == ThreadStartLanding::NonImagePrivate,
                 L"J-04 起点落在私有区域");
    suite.expect(findings[2].startPageExecutableKnown && findings[2].startPageExecutable,
                 L"J-04 私有起点页可执行事实");

    // 起点页现在不可执行，也不能据此忽略这条线索：landing 仍是 NonImagePrivate。
    suite.expect(findings[3].landing == ThreadStartLanding::NonImagePrivate,
                 L"J-04 起点页不可执行不改变落点判定");
    suite.expect(findings[3].startPageExecutableKnown && !findings[3].startPageExecutable,
                 L"J-04 起点页不可执行作为并列事实记录");

    suite.expect(findings[4].landing == ThreadStartLanding::NonImageMapped,
                 L"J-04 起点落在映射非映像区域");
    suite.expect(findings[4].owningPath == "C:\\temp\\blob.bin", L"J-04 映射来源路径");

    suite.expect(findings[5].landing == ThreadStartLanding::FreeOrReserved,
                 L"J-04 起点落在未提交区域");

    suite.expect(findings[6].landing == ThreadStartLanding::OutsideIndex,
                 L"J-04 索引未覆盖即缺口");

    // 起始地址没采到 —— 是没有观测，不是"归属不一致"。
    suite.expect(findings[7].landing == ThreadStartLanding::NotCollected,
                 L"J-04 起点未采集单独成态");
    suite.expect(!findings[7].startAddress.present, L"J-04 起点未采集不补 0");
    suite.expect(findings[7].outcome.status == CollectionStatus::AccessDenied,
                 L"J-04 起点未采集保留原因");

    suite.expect(findings[8].landing == ThreadStartLanding::ImageCodeRange,
                 L"J-04 trampoline 起点本身正常");
    suite.expect(findings[8].branchTargetLanding == ThreadStartLanding::NonImagePrivate,
                 L"J-04 trampoline 第一跳落点");
    suite.expect(findings[8].branchLeavesOwningModule, L"J-04 trampoline 跨模块");

    // 代码布局未知时不细分，也不假装正常。
    const std::vector<ImageCodeExtent> noLayout = {
        MakeImage("C:\\app\\target.exe", 0x140000000ULL, 0x10000U, 0U, 0U, false),
    };
    const std::vector<ThreadStartFinding> noLayoutFindings =
        EvaluateThreadStarts({ outsideCode }, index, noLayout);
    suite.expect(noLayoutFindings.size() == 1U, L"J-04 布局未知仍产出结果");
    suite.expect(noLayoutFindings[0].landing == ThreadStartLanding::ImageLayoutUnknown,
                 L"J-04 代码布局未知单独成态");

    // 线程上下文可信度。
    suite.expect(ClassifyThreadContextTrust(false, false, false) == ThreadContextTrust::NotCaptured,
                 L"J-04 未取上下文");
    suite.expect(ClassifyThreadContextTrust(true, false, false) ==
                     ThreadContextTrust::RunningThreadUntrusted,
                 L"J-04 运行中线程上下文不可信");
    suite.expect(ClassifyThreadContextTrust(true, true, false) ==
                     ThreadContextTrust::SuspendedOrSnapshot,
                 L"J-04 挂起/快照上下文可用");
    // 不挂起也没快照，但前后两次查都在等待 —— 这是本功能实际吃得到的那一态。
    suite.expect(ClassifyThreadContextTrust(true, false, true) ==
                     ThreadContextTrust::WaitingThreadStable,
                 L"J-04 等待中线程上下文稳定");
    // 挂起优先于"两次都在等待"：它不依赖"采集期间状态没变过"这个前提。
    suite.expect(ClassifyThreadContextTrust(true, true, true) ==
                     ThreadContextTrust::SuspendedOrSnapshot,
                 L"J-04 挂起优先于等待态");
    // 没取到上下文的话，线程在不在等待都无关紧要。
    suite.expect(ClassifyThreadContextTrust(false, true, true) == ThreadContextTrust::NotCaptured,
                 L"J-04 没取到就是没取到");
    suite.expect(!ContextUsableAsExecutionEvidence(ThreadContextTrust::RunningThreadUntrusted),
                 L"J-04 运行中上下文不得当执行证据");
    suite.expect(!ContextUsableAsExecutionEvidence(ThreadContextTrust::NotCaptured),
                 L"J-04 未取上下文不得当执行证据");
    suite.expect(ContextUsableAsExecutionEvidence(ThreadContextTrust::SuspendedOrSnapshot),
                 L"J-04 快照上下文可当执行证据");
    suite.expect(ContextUsableAsExecutionEvidence(ThreadContextTrust::WaitingThreadStable),
                 L"J-04 等待中上下文可当执行证据");

    // 栈证据三档分离。
    suite.expect(StackEvidenceCountsAsExecution(StackEvidenceKind::ReliableUnwoundFrame),
                 L"J-04 可靠帧算执行证据");
    suite.expect(!StackEvidenceCountsAsExecution(
                     StackEvidenceKind::HeuristicReturnAddressCandidate),
                 L"J-04 启发式候选返回地址不算执行证据");
    suite.expect(!StackEvidenceCountsAsExecution(StackEvidenceKind::PlainPointerReference),
                 L"J-04 栈上像地址的数值不算调用帧");
}

// ---------------------------------------------------------------------------
// J-05：比较计划与参考可信度
// ---------------------------------------------------------------------------
void TestComparisonPlan(KswordTests::Suite& suite) {
    // 硬约束：两种模式的归一化 profile 必须相同。
    suite.expect(std::string(NormalizationProfileId(SurveyMode::Fast)) ==
                     std::string(NormalizationProfileId(SurveyMode::Deep)),
                 L"J-05 快扫深扫归一化 profile 相同");
    suite.expect(NormalizationProfileVersion(SurveyMode::Fast) ==
                     NormalizationProfileVersion(SurveyMode::Deep),
                 L"J-05 快扫深扫归一化版本相同");

    ComparisonPlanInput input;
    input.mode = SurveyMode::Fast;
    input.images = {
        MakeImage("C:\\app\\target.exe", 0x140000000ULL, 0x10000U, 0x1000U, 0x5000U),
        MakeImage("C:\\Windows\\System32\\ntdll.dll", 0x7FF800000000ULL, 0x20000U, 0x1000U,
                  0x12000U),
    };
    input.mainImagePath = "C:\\app\\target.exe";
    input.mainImageEntryRva = OptionalU64::of(0x1500U);
    input.entryWindowBytes = 64U;
    input.threadEntrySites.push_back({ "C:\\Windows\\System32\\ntdll.dll", 0x2000U });
    input.screenedPages.push_back({ "C:\\app\\target.exe", 0x3000U });

    const ComparisonPlan fast = BuildComparisonPlan(input);
    suite.expect(fast.normalizationProfileId == std::string(kNormalizationProfileId),
                 L"J-05 计划记录归一化 profile");
    suite.expect(fast.targets.size() == 3U, L"J-05 快扫计划三个定向目标");
    suite.expect(fast.targets[0].reason == ComparisonReason::MainImageEntry,
                 L"J-05 主映像入口优先");
    suite.expect(fast.targets[0].range.rva == 0x1500U && fast.targets[0].range.length == 64U,
                 L"J-05 主映像入口窗口");
    suite.expect(fast.targets[1].reason == ComparisonReason::SuspiciousThreadEntry,
                 L"J-05 可疑线程入口进入计划");
    suite.expect(fast.targets[2].reason == ComparisonReason::WorkingSetScreenedPage,
                 L"J-05 工作集筛出页进入计划");
    suite.expect(fast.targets[2].range.length == 4096U, L"J-05 筛出页按页比较");
    suite.expect(fast.coverageGapKeys.empty(), L"J-05 快扫计划无缺口");

    ComparisonPlanInput deepInput = input;
    deepInput.mode = SurveyMode::Deep;
    const ComparisonPlan deep = BuildComparisonPlan(deepInput);
    suite.expect(deep.normalizationProfileId == fast.normalizationProfileId,
                 L"J-05 深扫沿用同一 profile");
    suite.expect(deep.targets.size() == 5U, L"J-05 深扫追加全量可执行范围");
    suite.expect(deep.targets[0].reason == ComparisonReason::FullExecutableCoverage,
                 L"J-05 深扫先覆盖全部可执行映像范围");
    suite.expect(deep.targets[0].range.rva == 0x1000U && deep.targets[0].range.length == 0x4000U,
                 L"J-05 深扫覆盖代码区间");

    // 主映像入口取不到：缺口，不是"没有入口"。
    ComparisonPlanInput noEntry = input;
    noEntry.mainImageEntryRva = OptionalU64::unset();
    const ComparisonPlan noEntryPlan = BuildComparisonPlan(noEntry);
    suite.expect(HasGap(noEntryPlan.coverageGapKeys, kGapMainImageSourceMissing),
                 L"J-05 主映像入口缺失留缺口");

    ComparisonPlanInput noMain = input;
    noMain.mainImagePath.clear();
    const ComparisonPlan noMainPlan = BuildComparisonPlan(noMain);
    suite.expect(HasGap(noMainPlan.coverageGapKeys, kGapMainImageSourceMissing),
                 L"J-05 主映像路径缺失留缺口");

    // 线程入口指向未知模块：缺口。
    ComparisonPlanInput unknownModule = input;
    unknownModule.threadEntrySites.push_back({ "C:\\temp\\unknown.dll", 0x1000U });
    const ComparisonPlan unknownPlan = BuildComparisonPlan(unknownModule);
    suite.expect(HasGap(unknownPlan.coverageGapKeys, kGapThreadStartUnavailable),
                 L"J-05 入口模块未知留缺口");

    // 范围被裁到映像边界内，不会越界。
    ComparisonPlanInput nearEnd = input;
    nearEnd.mainImageEntryRva = OptionalU64::of(0xFFE0U);
    const ComparisonPlan nearEndPlan = BuildComparisonPlan(nearEnd);
    suite.expect(!nearEndPlan.targets.empty(), L"J-05 边界附近仍产出目标");
    if (!nearEndPlan.targets.empty()) {
        suite.expect(nearEndPlan.targets[0].range.rva == 0xFFE0U &&
                         nearEndPlan.targets[0].range.length == 0x20U,
                     L"J-05 范围裁到映像边界");
    }

    // 代码布局未知的映像在深扫里覆盖整块，并留缺口。
    ComparisonPlanInput unknownLayout;
    unknownLayout.mode = SurveyMode::Deep;
    unknownLayout.images = {
        MakeImage("C:\\app\\opaque.dll", 0x180000000ULL, 0x4000U, 0U, 0U, false),
    };
    unknownLayout.mainImagePath = "C:\\app\\opaque.dll";
    unknownLayout.mainImageEntryRva = OptionalU64::of(0x100U);
    const ComparisonPlan unknownLayoutPlan = BuildComparisonPlan(unknownLayout);
    suite.expect(HasGap(unknownLayoutPlan.coverageGapKeys, kGapReferenceUncertain),
                 L"J-05 代码布局未知留缺口");
    suite.expect(!unknownLayoutPlan.targets.empty() &&
                     unknownLayoutPlan.targets[0].range.length == 0x4000U,
                 L"J-05 代码布局未知覆盖整块");

    // 参考可信度：只有已核对才能说"修改已证实"。
    suite.expect(ReferenceSupportsDifferenceClaim(ReferenceConfidence::ReferenceVerified),
                 L"J-05 已核对参考支持差异结论");
    suite.expect(!ReferenceSupportsDifferenceClaim(ReferenceConfidence::ReferenceUncertain),
                 L"J-05 参考不确定不得断言修改已证实");
    suite.expect(!ReferenceSupportsDifferenceClaim(ReferenceConfidence::NoReference),
                 L"J-05 无参考不得断言修改已证实");
}

// ---------------------------------------------------------------------------
// J-06：R0 扫描后端的交叉视图
// ---------------------------------------------------------------------------
SurveyInput MakeCleanInput();  // 定义在下面的"总入口"一节

// 造一个"可信上下文 + 全部由展开数据算出"的栈。frameIps 按栈顶到栈底排列。
// lastFrameHasUnwindData 决定最后一帧的 PC 自身有没有展开数据 —— 它只影响
// **再下一帧**可不可靠，对给定的这几帧没有影响。
ThreadStackInput MakeTrustedStack(const std::uint64_t tid,
                                  const std::vector<std::uint64_t>& frameIps,
                                  const bool lastFrameHasUnwindData = true) {
    ThreadStackInput stack;
    stack.thread.tid = OptionalU64::of(tid);
    stack.trust = ThreadContextTrust::WaitingThreadStable;
    for (std::size_t index = 0U; index < frameIps.size(); ++index) {
        RawStackFrame frame;
        frame.instructionPointer = OptionalU64::of(frameIps[index]);
        frame.stackPointer = OptionalU64::of(0x9000000ULL + index * 0x100ULL);
        frame.derivedFromUnwindData = true;
        frame.unwindDataAvailableAtPc =
            (index + 1U == frameIps.size()) ? lastFrameHasUnwindData : true;
        stack.frames.push_back(frame);
    }
    return stack;
}

void TestKernelCrossView(KswordTests::Suite& suite) {
    // 资格判据先钉死。
    suite.expect(KernelBackendSupportsAbsenceInference(KernelBackendState::Available),
                 L"内核 完整可用才有缺项资格");
    suite.expect(!KernelBackendSupportsAbsenceInference(KernelBackendState::Partial),
                 L"内核 部分完成无缺项资格");
    suite.expect(!KernelBackendSupportsAbsenceInference(KernelBackendState::ProfileUnverified),
                 L"内核 profile 未验证无缺项资格");
    suite.expect(!KernelBackendSupportsAbsenceInference(KernelBackendState::DriverUnavailable),
                 L"内核 驱动不可用无缺项资格");
    suite.expect(!KernelBackendSupportsAbsenceInference(KernelBackendState::NotRequested),
                 L"内核 未请求无缺项资格");

    // R3 索引：一段已提交私有区域 + 一段映像。
    std::vector<RegionRecord> records;
    records.push_back(MakeRegion(0x140000000ULL, 0x10000U, RegionState::Commit,
                                 RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                 "C:\\app\\target.exe"));
    records.push_back(MakeRegion(0x200000U, 0x2000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageReadWrite));
    const AddressSpaceIndex index = BuildAddressSpaceIndex(records, CollectionOutcome::success());
    suite.expect(index.usableForAbsenceInference(), L"内核 R3 索引可做缺项推断");

    auto makeVadRegion = [](std::uint64_t begin, std::uint64_t end, bool priv) {
        KernelVadRegion region;
        region.startVa = OptionalU64::of(begin);
        region.endVaExclusive = OptionalU64::of(end);
        region.privateMemory = priv;
        region.vadNodeAddress = OptionalU64::of(0xFFFFA00000000000ULL + begin);
        return region;
    };

    // --- 没请求内核后端：整节静默跳过，不留缺口 ---
    {
        KernelCrossViewInput input;
        input.r3Index = &index;
        const KernelCrossViewReport report = EvaluateKernelCrossView(input);
        suite.expect(report.findings.empty(), L"内核 未请求无结果");
        suite.expect(report.coverageGapKeys.empty(), L"内核 未请求不留缺口");
        suite.expect(report.capabilityLimitKeys.empty(), L"内核 未请求不留能力限制");
        suite.expect(report.conclusion == AnalysisConclusion::NoEvidence,
                     L"内核 未请求即无证据");
    }

    // --- 两侧一致：无 finding，恒挂"内核可信"这条限制 ---
    {
        KernelCrossViewInput input;
        input.r3Index = &index;
        input.vadView.state = KernelBackendState::Available;
        input.vadView.outcome = CollectionOutcome::success();
        input.vadView.regions = {
            makeVadRegion(0x140000000ULL, 0x140010000ULL, false),
            makeVadRegion(0x200000ULL, 0x202000ULL, true),
        };
        const KernelCrossViewReport report = EvaluateKernelCrossView(input);
        suite.expect(report.findings.empty(), L"内核 两侧一致无结果");
        suite.expect(report.absenceInferenceAllowed, L"内核 两侧完整才允许缺项推断");
        suite.expect(std::find(report.capabilityLimitKeys.begin(),
                               report.capabilityLimitKeys.end(),
                               std::string(kLimitKernelTrustAssumption)) !=
                         report.capabilityLimitKeys.end(),
                     L"内核 恒挂内核可信限制");
        suite.expect(std::find(report.capabilityLimitKeys.begin(),
                               report.capabilityLimitKeys.end(),
                               std::string(kLimitKernelVadFlagsUnverified)) !=
                         report.capabilityLimitKeys.end(),
                     L"内核 恒挂 VAD 位布局未验证限制");
        suite.expect(std::find(report.capabilityLimitKeys.begin(),
                               report.capabilityLimitKeys.end(),
                               std::string(kLimitKernelSectionCompare)) !=
                         report.capabilityLimitKeys.end(),
                     L"内核 恒挂第三层未做限制");
        suite.expect(report.conclusion == AnalysisConclusion::NoDifferenceObserved,
                     L"内核 两侧一致结论");
    }

    // --- VAD 有、R3 没有：最有价值的那一条（R3 视图被藏了东西）---
    {
        KernelCrossViewInput input;
        input.r3Index = &index;
        input.vadView.state = KernelBackendState::Available;
        input.vadView.outcome = CollectionOutcome::success();
        input.vadView.regions = {
            makeVadRegion(0x140000000ULL, 0x140010000ULL, false),
            makeVadRegion(0x200000ULL, 0x202000ULL, true),
            makeVadRegion(0x900000ULL, 0x901000ULL, true),   // R3 索引里没有
        };
        const KernelCrossViewReport report = EvaluateKernelCrossView(input);
        suite.expect(report.vadOnlyCount == 1U, L"内核 VAD 独有区域计数");
        suite.expect(!report.findings.empty() &&
                         report.findings[0].issue == KernelRegionCrossIssue::VadOnlyRange,
                     L"内核 VAD 独有区域产出");
        // 合法成因目录还没建，所以只到待解释。
        suite.expect(report.conclusion == AnalysisConclusion::Indeterminate,
                     L"内核 差异只到待解释");
        suite.expect(std::find(report.capabilityLimitKeys.begin(),
                               report.capabilityLimitKeys.end(),
                               std::string(kLimitKernelBenignBaseline)) !=
                         report.capabilityLimitKeys.end(),
                     L"内核 有差异即声明合法成因目录未建立");
    }

    // --- profile 没验证：一条 finding 都不产，只留缺口 ---
    {
        KernelCrossViewInput input;
        input.r3Index = &index;
        input.vadView.state = KernelBackendState::ProfileUnverified;
        input.vadView.outcome = CollectionOutcome{};
        input.vadView.regions = { makeVadRegion(0x900000ULL, 0x901000ULL, true) };
        const KernelCrossViewReport report = EvaluateKernelCrossView(input);
        suite.expect(report.findings.empty(), L"内核 profile 未验证不产结果");
        suite.expect(report.vadOnlyCount == 0U, L"内核 profile 未验证不计数");
        suite.expect(std::find(report.coverageGapKeys.begin(), report.coverageGapKeys.end(),
                               std::string(kGapKernelProfileUnverified)) !=
                         report.coverageGapKeys.end(),
                     L"内核 profile 未验证留缺口");
        suite.expect(report.conclusion != AnalysisConclusion::NoDifferenceObserved,
                     L"内核 profile 未验证不得表述为一致");
    }

    // --- 部分完成：同样没有缺项资格 ---
    {
        KernelCrossViewInput input;
        input.r3Index = &index;
        input.vadView.state = KernelBackendState::Partial;
        input.vadView.outcome = CollectionOutcome{ };
        input.vadView.unreadableNodeCount = 3U;
        input.vadView.regions = { makeVadRegion(0x900000ULL, 0x901000ULL, true) };
        const KernelCrossViewReport report = EvaluateKernelCrossView(input);
        suite.expect(!report.absenceInferenceAllowed, L"内核 部分完成无缺项资格");
        suite.expect(report.findings.empty(), L"内核 部分完成不产缺项结果");
        suite.expect(std::find(report.coverageGapKeys.begin(), report.coverageGapKeys.end(),
                               std::string(kGapKernelBackendUnavailable)) !=
                         report.coverageGapKeys.end(),
                     L"内核 部分完成留缺口");
    }

    // --- 页表说可执行、R3 说不可执行：正面观测，不需要缺项资格 ---
    {
        KernelCrossViewInput input;
        input.r3Index = &index;
        input.pteView.state = KernelBackendState::Available;
        input.pteView.outcome = CollectionOutcome::success();
        KernelExecutableExtent extent;
        extent.startVa = OptionalU64::of(0x200000ULL);   // R3 说这段是 RW，不可执行
        extent.byteLength = OptionalU64::of(0x1000ULL);
        extent.pageSize = 4096U;
        extent.executable = true;
        extent.writable = true;
        extent.userAccessible = true;
        extent.firstEntryValue = OptionalU64::of(0x8000000012345067ULL);
        input.pteView.extents = { extent };
        const KernelCrossViewReport report = EvaluateKernelCrossView(input);
        suite.expect(report.executableBeyondViewCount == 1U, L"内核 页表越过 R3 视图计数");
        suite.expect(!report.findings.empty() &&
                         report.findings[0].issue ==
                             KernelRegionCrossIssue::ExecutableBeyondR3View,
                     L"内核 页表越过 R3 视图产出");
        suite.expect(!report.absenceInferenceAllowed,
                     L"内核 页表正面观测不依赖缺项资格");
        suite.expect(report.conclusion == AnalysisConclusion::Indeterminate,
                     L"内核 页表差异只到待解释");
    }

    // --- 页表与 R3 一致：不产结果 ---
    {
        KernelCrossViewInput input;
        input.r3Index = &index;
        input.pteView.state = KernelBackendState::Available;
        input.pteView.outcome = CollectionOutcome::success();
        KernelExecutableExtent extent;
        extent.startVa = OptionalU64::of(0x140001000ULL);  // 映像 RX 段
        extent.byteLength = OptionalU64::of(0x1000ULL);
        extent.pageSize = 4096U;
        extent.executable = true;
        extent.userAccessible = true;
        input.pteView.extents = { extent };
        const KernelCrossViewReport report = EvaluateKernelCrossView(input);
        suite.expect(report.executableBeyondViewCount == 0U,
                     L"内核 页表与 R3 一致不产结果");
    }

    // --- 缺 R3 索引：不猜 ---
    {
        KernelCrossViewInput input;
        input.vadView.state = KernelBackendState::Available;
        input.vadView.outcome = CollectionOutcome::success();
        const KernelCrossViewReport report = EvaluateKernelCrossView(input);
        suite.expect(report.findings.empty(), L"内核 缺 R3 索引不产结果");
        suite.expect(report.conclusion == AnalysisConclusion::NoEvidence,
                     L"内核 缺 R3 索引即无证据");
    }

    // --- 总入口透传 ---
    {
        SurveyInput survey = MakeCleanInput();
        survey.kernelVadState = KernelBackendState::Available;
        KernelCrossViewInput crossInput;
        crossInput.r3Index = &survey.addressSpace;
        crossInput.vadView.state = KernelBackendState::Available;
        crossInput.vadView.outcome = CollectionOutcome::success();
        // VAD 必须同时覆盖 MakeCleanInput 里的两段 R3 区域，否则会额外产出
        // R3OnlyCommittedRange，计数就不是 1 了。
        crossInput.vadView.regions = {
            makeVadRegion(0x140000000ULL, 0x140010000ULL, false),
            makeVadRegion(0x100000ULL, 0x101000ULL, true),
            makeVadRegion(0x900000ULL, 0x901000ULL, true),
        };
        survey.kernelCrossView = EvaluateKernelCrossView(crossInput);
        const SurveyReport report = RunInjectionSurvey(survey);
        suite.expect(report.kernelCrossIssueCount == 1U, L"内核 总入口交叉计数");
        suite.expect(HasRule(report.findings, kRuleIdKernelRegionHiddenFromR3),
                     L"内核 总入口产出隐藏区域规则");
        suite.expect(report.hasLimit(kLimitKernelTrustAssumption),
                     L"内核 总入口透传内核可信限制");
        suite.expect(report.conclusion == AnalysisConclusion::Indeterminate,
                     L"内核 总入口结论只到待解释");
        suite.expect(std::find(report.completedCheckKeys.begin(),
                               report.completedCheckKeys.end(),
                               std::string(kCheckKernelVadCrossView)) !=
                         report.completedCheckKeys.end(),
                     L"内核 总入口登记已完成检查");
    }

    // --- 本机没有设备：能力限制，不是覆盖缺口 ---
    {
        SurveyInput survey = MakeCleanInput();
        survey.extraCapabilityLimitKeys.push_back(kLimitKernelBackendAbsent);
        const SurveyReport report = RunInjectionSurvey(survey);
        suite.expect(report.hasLimit(kLimitKernelBackendAbsent),
                     L"内核 无设备记为能力限制");
        suite.expect(!report.hasGap(kLimitKernelBackendAbsent),
                     L"内核 无设备不记为覆盖缺口");
        suite.expect(report.scopeIntact, L"内核 无设备不破坏声明范围");
        suite.expect(report.conclusion == AnalysisConclusion::NoDifferenceObserved,
                     L"内核 无设备不压制结论");
    }

    // --- 设备在但调用失败：那是真缺口 ---
    {
        SurveyInput survey = MakeCleanInput();
        survey.kernelVadState = KernelBackendState::DriverUnavailable;
        KernelCrossViewInput crossInput;
        crossInput.r3Index = &survey.addressSpace;
        crossInput.vadView.state = KernelBackendState::DriverUnavailable;
        crossInput.vadView.outcome = Denied();
        survey.kernelCrossView = EvaluateKernelCrossView(crossInput);
        const SurveyReport report = RunInjectionSurvey(survey);
        suite.expect(report.hasGap(kGapKernelBackendUnavailable),
                     L"内核 调用失败记为覆盖缺口");
        suite.expect(!report.scopeIntact, L"内核 调用失败破坏声明范围");
        suite.expect(report.conclusion == AnalysisConclusion::Indeterminate,
                     L"内核 调用失败压制结论");
    }

    // --- 没有驱动时不背假缺口 ---
    {
        const SurveyReport report = RunInjectionSurvey(MakeCleanInput());
        suite.expect(!report.hasGap(kGapKernelBackendUnavailable),
                     L"内核 未请求不产生假缺口");
        suite.expect(std::find(report.notPerformedCheckKeys.begin(),
                               report.notPerformedCheckKeys.end(),
                               std::string(kCheckKernelVadCrossView)) !=
                         report.notPerformedCheckKeys.end(),
                     L"内核 未请求登记为未执行");
        suite.expect(report.conclusion == AnalysisConclusion::NoDifferenceObserved,
                     L"内核 未请求不压制结论");
    }
}

// ---------------------------------------------------------------------------
// 例外（白名单）
// ---------------------------------------------------------------------------
ExceptionRelation MakeException() {
    ExceptionRelation rule;
    rule.ruleId = "vendor.hook.amsi";
    rule.ruleVersion = 3U;
    rule.category = ExceptionCategory::SecurityInstrumentation;
    rule.targetImageIdentity = "app:target.exe:1.2.3";
    rule.modifiedModuleIdentity = "mod:amsi.dll:10.0.26100";
    rule.modifiedRange = RvaRange{ 0x2000U, 0x10U };
    rule.evidenceText = "vendor advisory 2026-07";
    return rule;
}

void TestExceptionRelations(KswordTests::Suite& suite) {
    const ExceptionRelation good = MakeException();
    suite.expect(AdmitExceptionRelation(good) == ExceptionAdmission::Accepted,
                 L"例外 完整规则准入");

    // 模块身份键：有强身份时用跨会话主键（能区分同名不同版本），否则退化为归一化路径。
    DriverInstanceId weakModule = MakeModule("C:\\Windows\\System32\\NTDLL.DLL", 0x1000U, 0x2000U);
    suite.expect(ModuleIdentityKeyFor(weakModule) == "c:\\windows\\system32\\ntdll.dll",
                 L"例外 弱身份退化为归一化路径");
    DriverInstanceId strongModule = weakModule;
    strongModule.pdbSignature = "RSDS-0011-2233-4455-1";
    const std::string strongKey = ModuleIdentityKeyFor(strongModule);
    suite.expect(strongKey != ModuleIdentityKeyFor(weakModule),
                 L"例外 强身份与弱身份不共键");
    suite.expect(strongKey.find("RSDS-0011-2233-4455-1") != std::string::npos,
                 L"例外 强身份键含 PDB 签名");

    ExceptionRelation noId = good;
    noId.ruleId.clear();
    suite.expect(AdmitExceptionRelation(noId) == ExceptionAdmission::MissingRuleId,
                 L"例外 缺 ruleId 被拒");

    ExceptionRelation noCategory = good;
    noCategory.category = ExceptionCategory::Unspecified;
    suite.expect(AdmitExceptionRelation(noCategory) == ExceptionAdmission::MissingCategory,
                 L"例外 缺类别被拒");

    ExceptionRelation noTarget = good;
    noTarget.targetImageIdentity.clear();
    suite.expect(AdmitExceptionRelation(noTarget) ==
                     ExceptionAdmission::MissingTargetImageIdentity,
                 L"例外 缺目标程序身份被拒");

    ExceptionRelation noModule = good;
    noModule.modifiedModuleIdentity.clear();
    suite.expect(AdmitExceptionRelation(noModule) == ExceptionAdmission::MissingModuleIdentity,
                 L"例外 缺被修改模块身份被拒");

    ExceptionRelation emptyRange = good;
    emptyRange.modifiedRange = RvaRange{ 0x2000U, 0U };
    suite.expect(AdmitExceptionRelation(emptyRange) == ExceptionAdmission::EmptyRange,
                 L"例外 空范围被拒");

    // 覆盖整模块的"豁免"等于永久放行 —— 必须拒。
    ExceptionRelation wide = good;
    wide.modifiedRange = RvaRange{ 0U, 0xFFFFFFFFU };
    suite.expect(AdmitExceptionRelation(wide) == ExceptionAdmission::RangeTooWide,
                 L"例外 超宽范围被拒");

    ExceptionRelation atLimit = good;
    atLimit.modifiedRange = RvaRange{ 0x2000U, kExplanationRuleMaxSpanBytes };
    suite.expect(AdmitExceptionRelation(atLimit) == ExceptionAdmission::Accepted,
                 L"例外 上限内准入");
    ExceptionRelation overLimit = good;
    overLimit.modifiedRange = RvaRange{ 0x2000U, kExplanationRuleMaxSpanBytes + 1U };
    suite.expect(AdmitExceptionRelation(overLimit) == ExceptionAdmission::RangeTooWide,
                 L"例外 超出上限一字节即拒");

    // 匹配。
    ExceptionQuery query;
    query.targetImageIdentity = "app:target.exe:1.2.3";
    query.modifiedModuleIdentity = "mod:amsi.dll:10.0.26100";
    query.range = RvaRange{ 0x2004U, 0x8U };
    const ExceptionMatchResult matched = MatchExceptionRelation({ good }, query);
    suite.expect(matched.match == ExceptionMatch::Matched, L"例外 范围被完全包含时命中");
    suite.expect(matched.ruleId == "vendor.hook.amsi" && matched.ruleVersion == 3U,
                 L"例外 命中记录规则身份");
    suite.expect(matched.category == ExceptionCategory::SecurityInstrumentation,
                 L"例外 命中记录类别");

    suite.expect(MatchExceptionRelation({}, query).match == ExceptionMatch::NoRule,
                 L"例外 无规则");

    ExceptionQuery otherApp = query;
    otherApp.targetImageIdentity = "app:other.exe:1.0";
    suite.expect(MatchExceptionRelation({ good }, otherApp).match ==
                     ExceptionMatch::TargetImageMismatch,
                 L"例外 目标程序不同不命中");

    ExceptionQuery otherModule = query;
    otherModule.modifiedModuleIdentity = "mod:ntdll.dll:10.0.26100";
    suite.expect(MatchExceptionRelation({ good }, otherModule).match ==
                     ExceptionMatch::ModuleMismatch,
                 L"例外 模块不同不命中");

    // 部分覆盖不算命中。
    ExceptionQuery spill = query;
    spill.range = RvaRange{ 0x200CU, 0x10U };
    suite.expect(MatchExceptionRelation({ good }, spill).match == ExceptionMatch::RangeNotCovered,
                 L"例外 部分覆盖不算命中");

    // 跳转目标要求。
    ExceptionRelation withBranch = good;
    withBranch.allowedBranchTargetModuleIdentity = "mod:vendor.dll:2.1";
    ExceptionQuery wrongBranch = query;
    wrongBranch.actualBranchTargetModuleIdentity = "mod:evil.dll:0.1";
    suite.expect(MatchExceptionRelation({ withBranch }, wrongBranch).match ==
                     ExceptionMatch::BranchTargetMismatch,
                 L"例外 跳转目标不符不命中");
    ExceptionQuery rightBranch = query;
    rightBranch.actualBranchTargetModuleIdentity = "mod:vendor.dll:2.1";
    suite.expect(MatchExceptionRelation({ withBranch }, rightBranch).match ==
                     ExceptionMatch::Matched,
                 L"例外 跳转目标相符命中");
    // 没有跳转事实时同样不能放行。
    suite.expect(MatchExceptionRelation({ withBranch }, query).match ==
                     ExceptionMatch::BranchTargetMismatch,
                 L"例外 缺跳转事实不放行");

    // 字节检查：读不到字节就不命中（fail-closed）。
    ExceptionRelation withBytes = good;
    withBytes.expectedBytes = { 0xE9U, 0x00U, 0x00U, 0x00U };
    suite.expect(MatchExceptionRelation({ withBytes }, query).match ==
                     ExceptionMatch::BytesUnavailable,
                 L"例外 要求字节但读不到不命中");

    ExceptionQuery wrongBytes = query;
    wrongBytes.bytesAvailable = true;
    wrongBytes.actualBytes = { 0xCCU, 0x00U, 0x00U, 0x00U };
    suite.expect(MatchExceptionRelation({ withBytes }, wrongBytes).match ==
                     ExceptionMatch::BytesMismatch,
                 L"例外 字节不符不命中");

    ExceptionQuery rightBytes = query;
    rightBytes.bytesAvailable = true;
    rightBytes.actualBytes = { 0xE9U, 0x00U, 0x00U, 0x00U };
    suite.expect(MatchExceptionRelation({ withBytes }, rightBytes).match ==
                     ExceptionMatch::Matched,
                 L"例外 字节相符命中");

    // 被拒的规则不参与匹配，且丢弃必须可见。
    const ExceptionMatchResult rejected = MatchExceptionRelation({ wide, noCategory }, query);
    suite.expect(rejected.match == ExceptionMatch::AllRulesRejected, L"例外 全被拒");
    suite.expect(rejected.rejectedRuleCount == 2U, L"例外 被拒条数可见");

    const ExceptionMatchResult mixed = MatchExceptionRelation({ wide, good }, query);
    suite.expect(mixed.match == ExceptionMatch::Matched, L"例外 合法规则仍生效");
    suite.expect(mixed.rejectedRuleCount == 1U, L"例外 混合场景记录被拒条数");
}

// ---------------------------------------------------------------------------
// 观测语义表与身份复核
// ---------------------------------------------------------------------------
void TestSemanticsAndIdentity(KswordTests::Suite& suite) {
    const ObservationClass all[] = {
        ObservationClass::PrivateOrMappedExecutablePresent,
        ObservationClass::NormalizedImageDiffers,
        ObservationClass::PayloadStructureWithReliableFrame,
        ObservationClass::MappedModuleOutsideBaseline,
        ObservationClass::ScanCompleteNoStrongEvidence,
        ObservationClass::KeyInputUnavailable,
    };
    for (const ObservationClass observation : all) {
        const ObservationSemantics semantics = SemanticsFor(observation);
        suite.expect(semantics.observation == observation, L"语义表 观测回填");
        suite.expect(semantics.allowedConclusionKey != nullptr &&
                         semantics.allowedConclusionKey[0] != '\0',
                     L"语义表 允许结论键非空");
        suite.expect(semantics.forbiddenConclusionKey != nullptr &&
                         semantics.forbiddenConclusionKey[0] != '\0',
                     L"语义表 禁止结论键非空");
        suite.expect(std::string(semantics.allowedConclusionKey) !=
                         std::string(semantics.forbiddenConclusionKey),
                     L"语义表 两个键不同");
    }

    // 私有 RX 只到"待解释"，不到"已注入"。
    suite.expect(SemanticsFor(ObservationClass::PrivateOrMappedExecutablePresent).contribution ==
                     AnalysisConclusion::Indeterminate,
                 L"语义表 私有可执行只到待解释");
    // 关键输入拿不到只能是"结论不完整"，不能是"目标干净"。
    suite.expect(SemanticsFor(ObservationClass::KeyInputUnavailable).contribution ==
                     AnalysisConclusion::Indeterminate,
                 L"语义表 关键输入缺失不得表述为干净");
    suite.expect(SemanticsFor(ObservationClass::NormalizedImageDiffers).contribution ==
                     AnalysisConclusion::DifferenceObserved,
                 L"语义表 归一化差异是观测到差异");
    suite.expect(SemanticsFor(ObservationClass::ScanCompleteNoStrongEvidence).contribution ==
                     AnalysisConclusion::NoDifferenceObserved,
                 L"语义表 扫完无强证据只是已覆盖范围内未发现");

    // 身份复核。
    const ProcessInstanceId before = MakeProcess(1234U, 0x1D000000000ULL);
    suite.expect(RecheckProcessIdentity(before, before) == IdentityRecheckVerdict::Same,
                 L"身份 同实例");
    const ProcessInstanceId reused = MakeProcess(1234U, 0x1D000000999ULL);
    suite.expect(RecheckProcessIdentity(before, reused) == IdentityRecheckVerdict::Changed,
                 L"身份 PID 复用判变更");
    const ProcessInstanceId weak = MakeProcess(1234U, 0U, false);
    suite.expect(RecheckProcessIdentity(weak, weak) == IdentityRecheckVerdict::Unverifiable,
                 L"身份 只有 PID 不足以确认");
    const ProcessInstanceId otherPid = MakeProcess(5678U, 0x1D000000000ULL);
    suite.expect(RecheckProcessIdentity(before, otherPid) == IdentityRecheckVerdict::Changed,
                 L"身份 PID 不同判变更");

    // WOW64 采集器信任度。
    suite.expect(EvaluateModuleEnumerationTrust(CollectorArchitecture::Wow64,
                                                ProcessArchitecture::Wow64) ==
                     ModuleEnumerationTrust::FilterIgnoredUnderWow64,
                 L"身份 WOW64 采集器过滤被忽略");
    suite.expect(EvaluateModuleEnumerationTrust(CollectorArchitecture::Native64,
                                                ProcessArchitecture::Wow64) ==
                     ModuleEnumerationTrust::Trusted,
                 L"身份 原生 64 位采集器可信");
    suite.expect(EvaluateModuleEnumerationTrust(CollectorArchitecture::Unknown,
                                                ProcessArchitecture::X64) ==
                     ModuleEnumerationTrust::Unknown,
                 L"身份 采集器架构未知不假设可信");
    suite.expect(EvaluateModuleEnumerationTrust(CollectorArchitecture::Native64,
                                                ProcessArchitecture::Unknown) ==
                     ModuleEnumerationTrust::Unknown,
                 L"身份 目标架构未知不假设可信");
}

// ---------------------------------------------------------------------------
// 总入口
// ---------------------------------------------------------------------------

// 构造一份"什么都成功、什么都干净"的输入，各用例在它上面改一处。
SurveyInput MakeCleanInput() {
    SurveyInput input;
    input.mode = SurveyMode::Fast;
    input.detectorVersion = "j-module/1.0.0";
    input.processBefore = MakeProcess(4321U, 0x1D000000000ULL);
    input.processAfter = input.processBefore;
    input.collectedUtc100ns = OptionalU64::of(0x1D100000000ULL);
    input.targetArchitecture = ProcessArchitecture::X64;
    input.collectorArchitecture = CollectorArchitecture::Native64;
    input.targetImageIdentity = "app:target.exe:1.2.3";

    std::vector<RegionRecord> records;
    records.push_back(MakeRegion(0x140000000ULL, 0x10000U, RegionState::Commit,
                                 RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                 "C:\\app\\target.exe"));
    records.push_back(MakeRegion(0x100000U, 0x1000U, RegionState::Commit, RegionType::Private,
                                 kWin32PageReadWrite));
    input.addressSpace = BuildAddressSpaceIndex(records, CollectionOutcome::success());

    ModuleCrossViewInput crossInput;
    crossInput.loaderOutcome = CollectionOutcome::success();
    crossInput.imageOutcome = CollectionOutcome::success();
    crossInput.payloadOutcome = CollectionOutcome::success();
    crossInput.loaderTrust = ModuleEnumerationTrust::Trusted;
    crossInput.mainImagePathFromLoader = "C:\\app\\target.exe";
    crossInput.mainImagePathFromKernel = "C:\\app\\target.exe";
    crossInput.mainImagePathFromMapping = "C:\\app\\target.exe";
    crossInput.mainImageBaseFromLoader = OptionalU64::of(0x140000000ULL);
    crossInput.mainImageBaseFromMapping = OptionalU64::of(0x140000000ULL);
    LoaderModuleEntry main;
    main.module = MakeModule("C:\\app\\target.exe", 0x140000000ULL, 0x10000U);
    main.listedName = "target.exe";
    main.isMainImage = true;
    crossInput.loaderView = { main };
    ImageMappingEntry mainMap;
    mainMap.allocationBase = OptionalU64::of(0x140000000ULL);
    mainMap.mappedSize = OptionalU64::of(0x10000U);
    mainMap.mappedPath = "C:\\app\\target.exe";
    mainMap.pathOutcome = CollectionOutcome::success();
    crossInput.imageView = { mainMap };
    input.moduleCrossView = EvaluateModuleCrossView(crossInput);

    input.workingSetQueried = true;
    input.workingSetOutcome = CollectionOutcome::success();
    input.workingSetPagesScreened = 16U;

    input.threadEnumerationOutcome = CollectionOutcome::success();
    ThreadStartInput mainThread;
    mainThread.thread = MakeThread(input.processBefore, 1001U);
    mainThread.startAddress = OptionalU64::of(0x140002000ULL);
    mainThread.startAddressOutcome = CollectionOutcome::success();
    const std::vector<ImageCodeExtent> images = {
        MakeImage("C:\\app\\target.exe", 0x140000000ULL, 0x10000U, 0x1000U, 0x5000U),
    };
    input.threadStarts = EvaluateThreadStarts({ mainThread }, input.addressSpace, images);

    ImageComparisonOutcome comparison;
    comparison.module = MakeModule("C:\\app\\target.exe", 0x140000000ULL, 0x10000U);
    comparison.referenceConfidence = ReferenceConfidence::ReferenceVerified;
    comparison.report.outcome = CollectionOutcome::success();
    comparison.report.conclusion = AnalysisConclusion::NoDifferenceObserved;
    input.imageComparisons = { comparison };

    return input;
}

ImageDiffEntry MakeDiffEntry(const std::uint32_t rva,
                             const std::uint32_t length,
                             const DiffExplanation explanation = DiffExplanation::Unexplained) {
    ImageDiffEntry entry;
    entry.kind = DiffKind::ByteDifference;
    entry.rva = rva;
    entry.va = 0x140000000ULL + rva;
    entry.length = length;
    entry.sectionName = ".text";
    entry.explanation = explanation;
    entry.readStatus = ByteReadStatus::Read;
    entry.referenceBytes.assign(length, 0x90U);
    entry.liveBytes.assign(length, 0xE9U);
    return entry;
}

void TestSurveyPipeline(KswordTests::Suite& suite) {
    // --- 干净路径 ---
    const SurveyReport clean = RunInjectionSurvey(MakeCleanInput());
    suite.expect(clean.identity == IdentityRecheckVerdict::Same, L"总入口 身份一致");
    suite.expect(clean.findings.empty(), L"总入口 干净路径无结果");
    suite.expect(clean.scopeIntact, L"总入口 干净路径声明范围完好");
    // 快速模式恒带"没扫非可执行内存""没有可靠栈回溯"两条能力限制，所以
    // coverageComplete 为假是**正确**的；它只是不该压制结论。
    suite.expect(!clean.coverageComplete, L"总入口 快扫覆盖不完整（能力限制）");
    suite.expect(clean.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 干净路径结论");
    suite.expect(clean.hasObservation(ObservationClass::ScanCompleteNoStrongEvidence),
                 L"总入口 干净路径记录观测类别");
    suite.expect(clean.firstObservedUtc100ns.present &&
                     clean.firstObservedUtc100ns.value == 0x1D100000000ULL,
                 L"总入口 首次观测时间");
    suite.expect(clean.detectorVersion == "j-module/1.0.0", L"总入口 检测器版本");
    suite.expect(clean.ruleSetVersion == kInjectionSurveyRuleSetVersion, L"总入口 规则集版本");
    // 快速模式的结束条件是"完成了哪些检查"，不是 Injected/Clean 二选一。
    suite.expect(!clean.completedCheckKeys.empty(), L"总入口 列出已完成检查");
    suite.expect(!clean.notPerformedCheckKeys.empty(), L"总入口 列出未执行检查");
    suite.expect(std::find(clean.completedCheckKeys.begin(), clean.completedCheckKeys.end(),
                           std::string(kCheckAddressSpaceIndex)) != clean.completedCheckKeys.end(),
                 L"总入口 地址空间索引已完成");
    suite.expect(std::find(clean.notPerformedCheckKeys.begin(),
                           clean.notPerformedCheckKeys.end(),
                           std::string(kCheckNonExecutableScan)) !=
                     clean.notPerformedCheckKeys.end(),
                 L"总入口 快扫不声称扫了非可执行内存");

    // --- 私有 RX 本身只是"待解释"，不是"已注入" ---
    SurveyInput dynamicCode = MakeCleanInput();
    std::vector<RegionRecord> withRx;
    withRx.push_back(MakeRegion(0x140000000ULL, 0x10000U, RegionState::Commit,
                                RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                "C:\\app\\target.exe"));
    withRx.push_back(MakeRegion(0x200000U, 0x1000U, RegionState::Commit, RegionType::Private,
                                kWin32PageExecuteReadWrite));
    dynamicCode.addressSpace = BuildAddressSpaceIndex(withRx, CollectionOutcome::success());
    const SurveyReport dynamicReport = RunInjectionSurvey(dynamicCode);
    suite.expect(dynamicReport.dynamicCodeRegionCount == 1U, L"总入口 动态代码区域计数");
    suite.expect(HasRule(dynamicReport.findings, kRuleIdDynamicCodeRegion),
                 L"总入口 动态代码产出结果");
    suite.expect(dynamicReport.conclusion == AnalysisConclusion::Indeterminate,
                 L"总入口 私有 RX 只到待解释");
    suite.expect(dynamicReport.conclusion != AnalysisConclusion::DifferenceObserved,
                 L"总入口 私有 RX 不等于观测到差异");
    suite.expect(dynamicReport.hasObservation(
                     ObservationClass::PrivateOrMappedExecutablePresent),
                 L"总入口 动态代码观测类别");
    // 注入源进程没有证据就是未知，且不提供升格入口。
    suite.expect(!dynamicReport.findings.empty() &&
                     dynamicReport.findings[0].injectorAttribution == OwnerAttribution::Unknown,
                 L"总入口 注入源默认未知");
    suite.expect(!dynamicReport.findings.empty() &&
                     dynamicReport.findings[0].injectorCandidates.empty(),
                 L"总入口 无证据不列注入源候选");
    suite.expect(!dynamicReport.findings.empty() &&
                     dynamicReport.findings[0].firstObservedUtc100ns.present,
                 L"总入口 结果带首次观测时间");

    // 映射型可执行内存同样是候选（不能只扫私有）。
    SurveyInput mappedCode = MakeCleanInput();
    std::vector<RegionRecord> withMapped;
    withMapped.push_back(MakeRegion(0x140000000ULL, 0x10000U, RegionState::Commit,
                                    RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                    "C:\\app\\target.exe"));
    withMapped.push_back(MakeRegion(0x400000U, 0x2000U, RegionState::Commit, RegionType::Mapped,
                                    kWin32PageExecuteRead));
    mappedCode.addressSpace = BuildAddressSpaceIndex(withMapped, CollectionOutcome::success());
    const SurveyReport mappedReport = RunInjectionSurvey(mappedCode);
    suite.expect(mappedReport.dynamicCodeRegionCount == 1U, L"总入口 映射可执行计入候选");
    suite.expect(!mappedReport.findings.empty() &&
                     mappedReport.findings[0].regionType == RegionType::Mapped,
                 L"总入口 映射候选保留类型");

    // --- 归一化差异（参考已核对）---
    SurveyInput diffInput = MakeCleanInput();
    diffInput.imageComparisons[0].report.entries.push_back(MakeDiffEntry(0x1200U, 5U));
    const SurveyReport diffReport = RunInjectionSurvey(diffInput);
    suite.expect(diffReport.unexplainedImageDiffCount == 1U, L"总入口 未解释差异计数");
    suite.expect(HasRule(diffReport.findings, kRuleIdImageBytesUnexplained),
                 L"总入口 未解释差异产出结果");
    suite.expect(diffReport.conclusion == AnalysisConclusion::DifferenceObserved,
                 L"总入口 归一化差异升到观测到差异");
    suite.expect(diffReport.hasObservation(ObservationClass::NormalizedImageDiffers),
                 L"总入口 归一化差异观测类别");
    suite.expect(!diffReport.findings.empty() &&
                     diffReport.findings[0].rva.present &&
                     diffReport.findings[0].rva.value == 0x1200U,
                 L"总入口 差异带 RVA");
    suite.expect(!diffReport.findings.empty() &&
                     diffReport.findings[0].sectionName == ".text",
                 L"总入口 差异带节名");

    // ImageDiff 自己解释掉的差异不再计入未解释。
    SurveyInput explained = MakeCleanInput();
    explained.imageComparisons[0].report.entries.push_back(
        MakeDiffEntry(0x1200U, 5U, DiffExplanation::Explained));
    const SurveyReport explainedReport = RunInjectionSurvey(explained);
    suite.expect(explainedReport.unexplainedImageDiffCount == 0U,
                 L"总入口 已解释差异不计入未解释");
    suite.expect(explainedReport.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 已解释差异不改变结论");

    // 参考不确定：不得升格成"修改已证实"。
    SurveyInput uncertain = MakeCleanInput();
    uncertain.imageComparisons[0].referenceConfidence = ReferenceConfidence::ReferenceUncertain;
    uncertain.imageComparisons[0].report.entries.push_back(MakeDiffEntry(0x1200U, 5U));
    const SurveyReport uncertainReport = RunInjectionSurvey(uncertain);
    suite.expect(uncertainReport.unexplainedImageDiffCount == 0U,
                 L"总入口 参考不确定不计入已证实差异");
    suite.expect(HasRule(uncertainReport.findings, kRuleIdImageReferenceUncertain),
                 L"总入口 参考不确定单独成规则");
    suite.expect(uncertainReport.hasGap(kGapReferenceUncertain), L"总入口 参考不确定留缺口");
    suite.expect(uncertainReport.conclusion == AnalysisConclusion::Indeterminate,
                 L"总入口 参考不确定不得断言差异");

    // 现场读不到的字节是缺口，不是"相同"也不是"差异"。
    SurveyInput missingBytes = MakeCleanInput();
    ImageDiffEntry hole = MakeDiffEntry(0x1300U, 8U);
    hole.kind = DiffKind::MissingLiveBytes;
    hole.readStatus = ByteReadStatus::Unreadable;
    hole.liveBytes.clear();
    missingBytes.imageComparisons[0].report.entries.push_back(hole);
    const SurveyReport missingReport = RunInjectionSurvey(missingBytes);
    suite.expect(missingReport.unexplainedImageDiffCount == 0U, L"总入口 缺字节不算差异");
    suite.expect(missingReport.hasGap(kGapAddressSpaceIncomplete), L"总入口 缺字节留缺口");
    suite.expect(missingReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 缺字节不得表述为未发现差异");

    // 例外命中：保留结果但不计入未解释，也不把结论抬起来。
    SurveyInput whitelisted = MakeCleanInput();
    whitelisted.imageComparisons[0].report.entries.push_back(MakeDiffEntry(0x2004U, 4U));
    ExceptionRelation rule = MakeException();
    // 规则身份必须用同一个函数生成：匹配是严格等值比较，手写路径大小写不同就永远
    // 匹配不上。这条断言同时把"写规则的人该怎么拿到身份串"钉在测试里。
    rule.modifiedModuleIdentity =
        ModuleIdentityKeyFor(MakeModule("C:\\app\\target.exe", 0x140000000ULL, 0x10000U));
    suite.expect(rule.modifiedModuleIdentity == "c:\\app\\target.exe",
                 L"总入口 身份不足时模块键退化为归一化路径");
    whitelisted.exceptions = { rule };
    const SurveyReport whitelistedReport = RunInjectionSurvey(whitelisted);
    suite.expect(whitelistedReport.exceptionExplainedCount == 1U, L"总入口 例外命中计数");
    suite.expect(whitelistedReport.unexplainedImageDiffCount == 0U,
                 L"总入口 例外命中不计入未解释");
    suite.expect(!whitelistedReport.findings.empty() &&
                     whitelistedReport.findings[0].explainedByException(),
                 L"总入口 例外命中仍保留结果");
    suite.expect(whitelistedReport.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 例外命中不抬结论");

    // --- 覆盖缺口一律压制"未发现差异" ---
    SurveyInput noWorkingSet = MakeCleanInput();
    noWorkingSet.workingSetQueried = false;
    noWorkingSet.workingSetOutcome = Denied();
    const SurveyReport noWorkingSetReport = RunInjectionSurvey(noWorkingSet);
    suite.expect(noWorkingSetReport.hasGap(kGapWorkingSetUnavailable), L"总入口 工作集缺口");
    suite.expect(!noWorkingSetReport.coverageComplete, L"总入口 有缺口即覆盖不完整");
    suite.expect(noWorkingSetReport.conclusion == AnalysisConclusion::Indeterminate,
                 L"总入口 有缺口不得表述为未发现差异");
    suite.expect(noWorkingSetReport.hasObservation(ObservationClass::KeyInputUnavailable),
                 L"总入口 缺口记录为关键输入不可获得");

    SurveyInput noThreads = MakeCleanInput();
    noThreads.threadStarts.clear();
    noThreads.threadEnumerationOutcome = Denied();
    const SurveyReport noThreadsReport = RunInjectionSurvey(noThreads);
    suite.expect(noThreadsReport.hasGap(kGapThreadStartUnavailable), L"总入口 线程缺口");
    suite.expect(noThreadsReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 线程缺口压制干净结论");

    SurveyInput denied = MakeCleanInput();
    denied.addressSpace.outcome = Denied();
    const SurveyReport deniedReport = RunInjectionSurvey(denied);
    suite.expect(deniedReport.hasGap(kGapAddressSpaceIncomplete), L"总入口 地址空间缺口");
    suite.expect(deniedReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 拒绝访问不得输出未发现注入");

    // 预算截断：必须显示，绝不返回"干净"。
    SurveyInput truncated = MakeCleanInput();
    truncated.budgetStop = BudgetStop::TimeExhausted;
    const SurveyReport truncatedReport = RunInjectionSurvey(truncated);
    suite.expect(truncatedReport.hasGap(kGapBudgetTruncated), L"总入口 截断留缺口");
    suite.expect(truncatedReport.coverage.limitHit, L"总入口 截断记入账目");
    suite.expect(!truncatedReport.coverage.cancelled, L"总入口 命中上限不是取消");
    suite.expect(truncatedReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 截断不得表述为干净");

    SurveyInput cancelled = MakeCleanInput();
    cancelled.budgetStop = BudgetStop::Cancelled;
    const SurveyReport cancelledReport = RunInjectionSurvey(cancelled);
    suite.expect(cancelledReport.coverage.cancelled, L"总入口 取消与命中上限分开记录");
    suite.expect(!cancelledReport.coverage.limitHit, L"总入口 取消不伪装成命中上限");

    SurveyInput notRun = MakeCleanInput();
    notRun.plannedComparisonsNotRun = 3U;
    const SurveyReport notRunReport = RunInjectionSurvey(notRun);
    suite.expect(notRunReport.hasGap(kGapBudgetTruncated), L"总入口 计划未跑完留缺口");

    // 采集器自报的覆盖缺口同样压制"未发现差异"。
    SurveyInput collectorGap = MakeCleanInput();
    collectorGap.extraCoverageGapKeys.push_back(kGapMappedPathUnavailable);
    const SurveyReport collectorGapReport = RunInjectionSurvey(collectorGap);
    suite.expect(collectorGapReport.hasGap(kGapMappedPathUnavailable),
                 L"总入口 采集器缺口透传");
    suite.expect(collectorGapReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 采集器缺口压制干净结论");
    suite.expect(!collectorGapReport.coverageComplete, L"总入口 采集器缺口即覆盖不完整");

    // 采集器自报的能力限制只列出来，不压制结论。
    SurveyInput collectorLimit = MakeCleanInput();
    collectorLimit.extraCapabilityLimitKeys.push_back(kLimitPayloadHeaderErased);
    collectorLimit.extraCapabilityLimitKeys.push_back(kLimitRuntimeAttribution);
    const SurveyReport collectorLimitReport = RunInjectionSurvey(collectorLimit);
    suite.expect(collectorLimitReport.hasLimit(kLimitPayloadHeaderErased),
                 L"总入口 采集器能力限制透传");
    suite.expect(collectorLimitReport.hasLimit(kLimitRuntimeAttribution),
                 L"总入口 运行时归因限制透传");
    suite.expect(!collectorLimitReport.hasGap(kLimitPayloadHeaderErased),
                 L"总入口 能力限制不混进缺口");
    suite.expect(collectorLimitReport.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 能力限制不压制结论");

    // --- 身份变更：整份证据作废 ---
    SurveyInput reused = MakeCleanInput();
    reused.processAfter = MakeProcess(4321U, 0x1D000000999ULL);
    const SurveyReport reusedReport = RunInjectionSurvey(reused);
    suite.expect(reusedReport.identity == IdentityRecheckVerdict::Changed, L"总入口 身份变更");
    suite.expect(reusedReport.findings.empty(), L"总入口 身份变更丢弃全部结果");
    suite.expect(reusedReport.conclusion == AnalysisConclusion::NoEvidence,
                 L"总入口 身份变更即无证据");
    suite.expect(reusedReport.hasGap(kGapIdentityChanged), L"总入口 身份变更留缺口");
    suite.expect(!reusedReport.coverageComplete, L"总入口 身份变更覆盖不完整");

    SurveyInput weakIdentity = MakeCleanInput();
    weakIdentity.processBefore = MakeProcess(4321U, 0U, false);
    weakIdentity.processAfter = weakIdentity.processBefore;
    const SurveyReport weakReport = RunInjectionSurvey(weakIdentity);
    suite.expect(weakReport.identity == IdentityRecheckVerdict::Unverifiable,
                 L"总入口 弱身份无法确认");
    suite.expect(weakReport.hasGap(kGapIdentityUnverifiable), L"总入口 弱身份留缺口");
    suite.expect(weakReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 弱身份压制干净结论");

    // --- 线程起点异常 ---
    SurveyInput badThread = MakeCleanInput();
    std::vector<RegionRecord> withPrivate;
    withPrivate.push_back(MakeRegion(0x140000000ULL, 0x10000U, RegionState::Commit,
                                     RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                     "C:\\app\\target.exe"));
    withPrivate.push_back(MakeRegion(0x500000U, 0x1000U, RegionState::Commit,
                                     RegionType::Private, kWin32PageExecuteReadWrite));
    badThread.addressSpace = BuildAddressSpaceIndex(withPrivate, CollectionOutcome::success());
    ThreadStartInput strayThread;
    strayThread.thread = MakeThread(badThread.processBefore, 2002U);
    strayThread.startAddress = OptionalU64::of(0x500100U);
    strayThread.startAddressOutcome = CollectionOutcome::success();
    badThread.threadStarts = EvaluateThreadStarts(
        { strayThread }, badThread.addressSpace,
        { MakeImage("C:\\app\\target.exe", 0x140000000ULL, 0x10000U, 0x1000U, 0x5000U) });
    const SurveyReport badThreadReport = RunInjectionSurvey(badThread);
    suite.expect(badThreadReport.threadStartAnomalyCount == 1U, L"总入口 线程起点异常计数");
    suite.expect(HasRule(badThreadReport.findings, kRuleIdThreadStartOutsideImage),
                 L"总入口 线程起点异常产出结果");
    suite.expect(!badThreadReport.findings.empty() &&
                     !badThreadReport.findings.back().relatedThreads.empty(),
                 L"总入口 线程结果关联线程身份");
    suite.expect(badThreadReport.conclusion == AnalysisConclusion::Indeterminate,
                 L"总入口 线程起点异常只到待解释");

    // 起点没采到：用不同的 ruleId，且不抬成"异常"计数。
    SurveyInput unknownThread = MakeCleanInput();
    ThreadStartInput missingStart;
    missingStart.thread = MakeThread(unknownThread.processBefore, 2003U);
    missingStart.startAddressOutcome = Denied();
    unknownThread.threadStarts = EvaluateThreadStarts(
        { missingStart }, unknownThread.addressSpace,
        { MakeImage("C:\\app\\target.exe", 0x140000000ULL, 0x10000U, 0x1000U, 0x5000U) });
    const SurveyReport unknownThreadReport = RunInjectionSurvey(unknownThread);
    suite.expect(unknownThreadReport.threadStartAnomalyCount == 0U,
                 L"总入口 起点未采集不计入异常");
    suite.expect(HasRule(unknownThreadReport.findings, kRuleIdThreadStartUnknown),
                 L"总入口 起点未采集单独规则");
    suite.expect(!HasRule(unknownThreadReport.findings, kRuleIdThreadStartOutsideImage),
                 L"总入口 起点未采集不混用归属不一致规则");
    suite.expect(unknownThreadReport.hasGap(kGapThreadStartUnavailable),
                 L"总入口 起点未采集留缺口");

    // --- 模块交叉矛盾 ---
    SurveyInput crossIssue = MakeCleanInput();
    ModuleCrossFinding ghost;
    ghost.issue = ModuleCrossIssue::ImageMappingWithoutLoaderEntry;
    ghost.base = OptionalU64::of(0x7FF900000000ULL);
    ghost.mappedPath = "C:\\temp\\payload.dll";
    ghost.mappedSize = OptionalU64::of(0x8000U);
    ghost.inputOutcome = CollectionOutcome::success();
    crossIssue.moduleCrossView.findings.push_back(ghost);
    crossIssue.moduleCrossView.conclusion = AnalysisConclusion::DifferenceObserved;
    const SurveyReport crossReport = RunInjectionSurvey(crossIssue);
    suite.expect(crossReport.moduleCrossIssueCount == 1U, L"总入口 交叉视图问题计数");
    suite.expect(HasRule(crossReport.findings, kRuleIdImageWithoutLoaderEntry),
                 L"总入口 交叉视图问题产出结果");
    // "映像有、加载器没有"有大量合法成因（资源映射、元数据映像、.NET），
    // 实测 explorer.exe 上稳定十几条，所以它只到待解释。
    suite.expect(crossReport.moduleCrossConflictCount == 0U,
                 L"总入口 缺项不是矛盾");
    suite.expect(crossReport.conclusion == AnalysisConclusion::Indeterminate,
                 L"总入口 缺项只到待解释");
    suite.expect(!crossReport.findings.empty() &&
                     crossReport.findings[0].moduleName == "payload.dll",
                 L"总入口 交叉视图问题带模块名");

    // 真正的矛盾（同一个对象两个视图说法冲突）才升到观测到差异。
    SurveyInput crossConflict = MakeCleanInput();
    ModuleCrossFinding renamed;
    renamed.issue = ModuleCrossIssue::LoaderPathMismatch;
    renamed.base = OptionalU64::of(0x7FF800000000ULL);
    renamed.loaderPath = "C:\\Windows\\System32\\ntdll.dll";
    renamed.mappedPath = "C:\\temp\\evil.dll";
    renamed.inputOutcome = CollectionOutcome::success();
    crossConflict.moduleCrossView.findings.push_back(renamed);
    crossConflict.moduleCrossView.conclusion = AnalysisConclusion::DifferenceObserved;
    const SurveyReport crossConflictReport = RunInjectionSurvey(crossConflict);
    suite.expect(crossConflictReport.moduleCrossConflictCount == 1U, L"总入口 路径冲突是矛盾");
    suite.expect(crossConflictReport.conclusion == AnalysisConclusion::DifferenceObserved,
                 L"总入口 矛盾升到观测到差异");

    SurveyInput mainConflict = MakeCleanInput();
    ModuleCrossFinding mainMismatch;
    mainMismatch.issue = ModuleCrossIssue::MainImageIdentityConflict;
    mainMismatch.inputOutcome = CollectionOutcome::success();
    mainConflict.moduleCrossView.findings.push_back(mainMismatch);
    mainConflict.moduleCrossView.conclusion = AnalysisConclusion::DifferenceObserved;
    const SurveyReport mainConflictReport = RunInjectionSurvey(mainConflict);
    suite.expect(mainConflictReport.moduleCrossConflictCount == 1U, L"总入口 主映像矛盾计入");
    suite.expect(mainConflictReport.conclusion == AnalysisConclusion::DifferenceObserved,
                 L"总入口 主映像矛盾升到观测到差异");

    // 分档函数本身。
    suite.expect(ModuleCrossIssueIsContradiction(ModuleCrossIssue::LoaderPathMismatch),
                 L"分档 路径不一致是矛盾");
    suite.expect(ModuleCrossIssueIsContradiction(ModuleCrossIssue::LoaderSizeMismatch),
                 L"分档 大小不一致是矛盾");
    suite.expect(ModuleCrossIssueIsContradiction(ModuleCrossIssue::MainImageIdentityConflict),
                 L"分档 主映像自相矛盾是矛盾");
    suite.expect(!ModuleCrossIssueIsContradiction(
                     ModuleCrossIssue::ImageMappingWithoutLoaderEntry),
                 L"分档 映像无加载器项不是矛盾");
    suite.expect(!ModuleCrossIssueIsContradiction(
                     ModuleCrossIssue::LoaderEntryWithoutImageMapping),
                 L"分档 加载器项无映射不是矛盾");
    suite.expect(!ModuleCrossIssueIsContradiction(ModuleCrossIssue::MappedPathUnavailable),
                 L"分档 路径查不到不是矛盾");

    // 路径取不到不产出 finding，只留缺口。
    SurveyInput pathGap = MakeCleanInput();
    ModuleCrossFinding noPath;
    noPath.issue = ModuleCrossIssue::MappedPathUnavailable;
    noPath.base = OptionalU64::of(0x7FF900000000ULL);
    noPath.inputOutcome = Denied();
    pathGap.moduleCrossView.findings.push_back(noPath);
    pathGap.moduleCrossView.coverageGapKeys.push_back(kGapMappedPathUnavailable);
    const SurveyReport pathGapReport = RunInjectionSurvey(pathGap);
    suite.expect(pathGapReport.moduleCrossIssueCount == 0U, L"总入口 路径缺失不计入矛盾");
    suite.expect(pathGapReport.hasGap(kGapMappedPathUnavailable), L"总入口 路径缺失留缺口");

    // --- 能力限制与覆盖缺口是两类东西 ---
    SurveyInput deep = MakeCleanInput();
    deep.mode = SurveyMode::Deep;
    const SurveyReport deepReport = RunInjectionSurvey(deep);
    suite.expect(deepReport.hasLimit(kLimitStackUnwindUnavailable),
                 L"能力 无可靠栈回溯记为能力限制");
    suite.expect(deepReport.hasLimit(kLimitNonExecutableNotScanned),
                 L"能力 未扫非可执行内存记为能力限制");
    suite.expect(!deepReport.hasGap(kLimitStackUnwindUnavailable),
                 L"能力 限制不混进覆盖缺口");
    // 关键分界：能力限制不压制结论（否则四态永远退化成三态），但覆盖完整度为假。
    suite.expect(deepReport.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"能力 限制不压制已覆盖范围内的结论");
    suite.expect(deepReport.scopeIntact, L"能力 限制不破坏声明范围");
    suite.expect(!deepReport.coverageComplete, L"能力 有限制即覆盖不完整");

    SurveyInput deepFull = MakeCleanInput();
    deepFull.mode = SurveyMode::Deep;
    deepFull.threadStacks = { MakeTrustedStack(100U, { 0x7FF800001000ULL }) };
    deepFull.nonExecutableMemoryScanned = true;
    const SurveyReport deepFullReport = RunInjectionSurvey(deepFull);
    suite.expect(!deepFullReport.hasLimit(kLimitStackUnwindUnavailable),
                 L"能力 栈回溯可用即无限制");
    suite.expect(!deepFullReport.hasLimit(kLimitNonExecutableNotScanned),
                 L"能力 已扫非可执行内存即无限制");
    suite.expect(deepFullReport.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"能力 全覆盖结论");
    suite.expect(deepFullReport.coverageComplete, L"能力 全覆盖标记");
    suite.expect(deepFullReport.scopeIntact, L"能力 全覆盖范围完整");

    // 快速模式默认就带这两条能力限制，但同样不压制结论。
    const SurveyReport fastLimits = RunInjectionSurvey(MakeCleanInput());
    suite.expect(fastLimits.hasLimit(kLimitNonExecutableNotScanned),
                 L"能力 快扫记录非可执行内存限制");
    suite.expect(fastLimits.hasLimit(kLimitStackUnwindUnavailable),
                 L"能力 快扫记录栈回溯限制");
    suite.expect(fastLimits.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"能力 快扫限制不压制结论");

    // 反过来：真正的覆盖缺口必须压制。
    SurveyInput scopeBroken = MakeCleanInput();
    scopeBroken.extraCoverageGapKeys.push_back(kGapWorkingSetUnavailable);
    const SurveyReport scopeBrokenReport = RunInjectionSurvey(scopeBroken);
    suite.expect(!scopeBrokenReport.scopeIntact, L"能力 覆盖缺口破坏声明范围");
    suite.expect(scopeBrokenReport.conclusion == AnalysisConclusion::Indeterminate,
                 L"能力 覆盖缺口压制结论");

    // ImageDiff 的 limitationKeys 同样要按这条线分流。
    // "没有磁盘参考可比"（DVRT/零填充/重定位无法归一化）定义范围，不压制结论。
    SurveyInput noReferenceBytes = MakeCleanInput();
    noReferenceBytes.imageComparisons[0].report.limitationKeys = {
        "integrity.limitation.excludedNotComparable",
        "integrity.limitation.relocationDirectoryMissing",
    };
    const SurveyReport noReferenceReport = RunInjectionSurvey(noReferenceBytes);
    suite.expect(noReferenceReport.hasLimit("integrity.limitation.excludedNotComparable"),
                 L"能力 不可比范围记为能力限制");
    suite.expect(noReferenceReport.hasLimit("integrity.limitation.relocationDirectoryMissing"),
                 L"能力 重定位无法归一化记为能力限制");
    suite.expect(noReferenceReport.scopeIntact, L"能力 不可比范围不破坏声明范围");
    suite.expect(noReferenceReport.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"能力 不可比范围不压制结论");

    // "打算比但没比成"（读不到 / 命中上限 / 模块过期）必须压制。
    SurveyInput readFailed = MakeCleanInput();
    readFailed.imageComparisons[0].report.limitationKeys = {
        "integrity.limitation.unreadableBytes",
    };
    const SurveyReport readFailedReport = RunInjectionSurvey(readFailed);
    suite.expect(readFailedReport.hasGap("integrity.limitation.unreadableBytes"),
                 L"能力 读不到字节记为覆盖缺口");
    suite.expect(!readFailedReport.scopeIntact, L"能力 读不到字节破坏声明范围");
    suite.expect(readFailedReport.conclusion == AnalysisConclusion::Indeterminate,
                 L"能力 读不到字节压制结论");

    SurveyInput staleModule = MakeCleanInput();
    staleModule.imageComparisons[0].report.limitationKeys = {
        "integrity.limitation.moduleStale",
    };
    const SurveyReport staleModuleReport = RunInjectionSurvey(staleModule);
    suite.expect(staleModuleReport.hasGap("integrity.limitation.moduleStale"),
                 L"能力 模块过期记为覆盖缺口");
    suite.expect(!staleModuleReport.scopeIntact, L"能力 模块过期破坏声明范围");

    // --- 非映像载荷结构 ---
    SurveyInput payload = MakeCleanInput();
    PayloadCandidateEntry erased;
    erased.base = OptionalU64::of(0x600000U);
    erased.size = OptionalU64::of(0x3000U);
    erased.type = RegionType::Private;
    erased.structure = PayloadStructure::HeaderErasedPe;
    erased.outcome = CollectionOutcome::success();
    erased.structureFacts.push_back("payload.unwind=present");
    payload.payloadCandidates = { erased };
    const SurveyReport payloadReport = RunInjectionSurvey(payload);
    suite.expect(HasRule(payloadReport.findings, kRuleIdPayloadStructure),
                 L"总入口 载荷结构产出结果");
    suite.expect(!payloadReport.hasObservation(
                     ObservationClass::PayloadStructureWithReliableFrame),
                 L"总入口 无可靠栈帧时载荷结构不升格");

    // 同一块内存不出两行：载荷结构并进那条动态代码区域的证据里。
    SurveyInput sameRegion = MakeCleanInput();
    std::vector<RegionRecord> rxRegion;
    rxRegion.push_back(MakeRegion(0x140000000ULL, 0x10000U, RegionState::Commit,
                                  RegionType::Image, kWin32PageExecuteRead, 0x140000000ULL,
                                  "C:\\app\\target.exe"));
    rxRegion.push_back(MakeRegion(0x600000U, 0x3000U, RegionState::Commit, RegionType::Private,
                                  kWin32PageExecuteReadWrite));
    sameRegion.addressSpace = BuildAddressSpaceIndex(rxRegion, CollectionOutcome::success());
    sameRegion.payloadCandidates = { erased };  // erased.base 正是 0x600000
    const SurveyReport sameRegionReport = RunInjectionSurvey(sameRegion);
    suite.expect(sameRegionReport.dynamicCodeRegionCount == 1U, L"总入口 同一区域只计一次");
    suite.expect(!HasRule(sameRegionReport.findings, kRuleIdPayloadStructure),
                 L"总入口 同一区域不再单独出载荷结构行");
    suite.expect(sameRegionReport.findings.size() == 1U, L"总入口 同一块内存只出一行");
    suite.expect(!sameRegionReport.findings.empty() &&
                     std::any_of(sameRegionReport.findings[0].facts.begin(),
                                 sameRegionReport.findings[0].facts.end(),
                                 [](const std::string& fact) {
                                     return fact == "payload.structure=HeaderErasedPe";
                                 }),
                 L"总入口 载荷结构并进区域证据");

    // 光有"栈回溯能力可用"不够：必须**这块内存确实被可靠帧进入**。
    // 这个栈是可信的、帧也都可靠，但落点全在 0x600000 那块之外。
    SurveyInput payloadWalkOnly = payload;
    payloadWalkOnly.threadStacks = {
        MakeTrustedStack(201U, { 0x7FF800001000ULL, 0x7FF800002000ULL })
    };
    const SurveyReport payloadWalkOnlyReport = RunInjectionSurvey(payloadWalkOnly);
    suite.expect(!payloadWalkOnlyReport.hasObservation(
                     ObservationClass::PayloadStructureWithReliableFrame),
                 L"总入口 栈回溯可用本身不构成执行关联");
    suite.expect(payloadWalkOnlyReport.payloadWithExecutionCount == 0U,
                 L"总入口 无帧进入不计执行关联");
    suite.expect(payloadWalkOnlyReport.conclusion != AnalysisConclusion::DifferenceObserved,
                 L"总入口 无帧进入不升到观测到差异");

    // 睡着的信标就是这个形状：栈顶两帧在 ntdll/kernelbase，第三帧落进 0x600000 那块
    // 非映像内存里。第三帧是**从有展开数据的调用者算出来的**，所以它进可靠前缀。
    SurveyInput payloadWithFrame = payload;
    payloadWithFrame.threadStacks = {
        MakeTrustedStack(202U,
                         { 0x7FF800001000ULL, 0x7FF800002000ULL, 0x600100ULL },
                         /*lastFrameHasUnwindData=*/false)
    };
    const SurveyReport payloadFrameReport = RunInjectionSurvey(payloadWithFrame);
    suite.expect(payloadFrameReport.hasObservation(
                     ObservationClass::PayloadStructureWithReliableFrame),
                 L"总入口 结构 + 可靠栈帧才升格");
    suite.expect(payloadFrameReport.payloadWithExecutionCount == 1U,
                 L"总入口 执行关联计数");
    suite.expect(payloadFrameReport.conclusion == AnalysisConclusion::DifferenceObserved,
                 L"总入口 载荷与执行关联升到观测到差异");

    // 帧进入了，但结构只是"数据里的一个 PE 文件" —— 仍然不升格。
    SurveyInput dataPeWithFrame = MakeCleanInput();
    PayloadCandidateEntry dataOnlyFramed = erased;
    dataOnlyFramed.structure = PayloadStructure::DataOnlyPeFile;
    dataPeWithFrame.payloadCandidates = { dataOnlyFramed };
    dataPeWithFrame.threadStacks = {
        MakeTrustedStack(203U, { 0x7FF800001000ULL, 0x600100ULL },
                         /*lastFrameHasUnwindData=*/false)
    };
    const SurveyReport dataPeFramedReport = RunInjectionSurvey(dataPeWithFrame);
    suite.expect(dataPeFramedReport.payloadWithExecutionCount == 0U,
                 L"总入口 数据中的 PE 即使有帧也不升格");

    // 数据里的 PE 不等于已被加载执行。
    SurveyInput dataPe = MakeCleanInput();
    PayloadCandidateEntry dataOnly = erased;
    dataOnly.structure = PayloadStructure::DataOnlyPeFile;
    dataPe.payloadCandidates = { dataOnly };
    dataPe.threadStacks = { MakeTrustedStack(204U, { 0x7FF800001000ULL }) };
    const SurveyReport dataPeReport = RunInjectionSurvey(dataPe);
    suite.expect(!dataPeReport.hasObservation(
                     ObservationClass::PayloadStructureWithReliableFrame),
                 L"总入口 数据中的 PE 不升格为已执行载荷");

    // 没检查过的候选不产出结论，只落在"未执行的检查"里。
    SurveyInput notExamined = MakeCleanInput();
    PayloadCandidateEntry unchecked = erased;
    unchecked.structure = PayloadStructure::NotExamined;
    notExamined.payloadCandidates = { unchecked };
    const SurveyReport notExaminedReport = RunInjectionSurvey(notExamined);
    suite.expect(!HasRule(notExaminedReport.findings, kRuleIdPayloadStructure),
                 L"总入口 未检查候选不产出结果");
    suite.expect(std::find(notExaminedReport.notPerformedCheckKeys.begin(),
                           notExaminedReport.notPerformedCheckKeys.end(),
                           std::string(kCheckPayloadStructure)) !=
                     notExaminedReport.notPerformedCheckKeys.end(),
                 L"总入口 未检查候选登记为未执行");

    // --- 栈回溯：可靠前缀 ---
    // AdmitStackFrames 单独测一遍，再测它经过总入口的效果。
    {
        // 不可信上下文：整条链作废，不是"降级成启发式"。
        ThreadStackInput running = MakeTrustedStack(300U, { 0x7FF800001000ULL, 0x600100ULL });
        running.trust = ThreadContextTrust::RunningThreadUntrusted;
        suite.expect(AdmitStackFrames(running) == 0U, L"栈 运行中线程的上下文整条作废");

        ThreadStackInput notCaptured = MakeTrustedStack(301U, { 0x7FF800001000ULL });
        notCaptured.trust = ThreadContextTrust::NotCaptured;
        suite.expect(AdmitStackFrames(notCaptured) == 0U, L"栈 没取到上下文不产生可靠帧");

        // 挂起/快照与"两次都在等待"都算可信。
        ThreadStackInput suspended = MakeTrustedStack(302U, { 0x7FF800001000ULL });
        suspended.trust = ThreadContextTrust::SuspendedOrSnapshot;
        suite.expect(AdmitStackFrames(suspended) == 1U, L"栈 挂起/快照可信");
        suite.expect(AdmitStackFrames(MakeTrustedStack(303U, { 0x7FF800001000ULL })) == 1U,
                     L"栈 等待中线程可信");

        // 可靠前缀在"本帧 PC 查不到展开数据"处**收下本帧后**截断。
        // 这正是 shellcode 帧的位置：它由调用者算出来，所以它算数；它下面的不算。
        ThreadStackInput beacon = MakeTrustedStack(
            304U, { 0x7FF800001000ULL, 0x7FF800002000ULL, 0x600100ULL });
        beacon.frames[2].unwindDataAvailableAtPc = false;
        beacon.frames.push_back(RawStackFrame{ OptionalU64::of(0x7FF800009000ULL),
                                               OptionalU64::of(0x9000400ULL), false, true });
        suite.expect(AdmitStackFrames(beacon) == 3U, L"栈 载荷帧进可靠前缀而其后的不进");

        // 缺 PC 的帧直接截断：没有落点的帧无法参与任何判定。
        ThreadStackInput missingPc = MakeTrustedStack(305U, { 0x7FF800001000ULL, 0x600100ULL });
        missingPc.frames[1].instructionPointer = OptionalU64{};
        suite.expect(AdmitStackFrames(missingPc) == 1U, L"栈 无落点的帧截断可靠前缀");

        // 猜出来的帧不进可靠前缀，哪怕它自己有展开数据。
        ThreadStackInput guessed = MakeTrustedStack(306U, { 0x7FF800001000ULL, 0x600100ULL });
        guessed.frames[1].derivedFromUnwindData = false;
        suite.expect(AdmitStackFrames(guessed) == 1U, L"栈 扫栈猜出的帧不进可靠前缀");
    }

    // 走了栈但一个可信上下文都没拿到 —— 是缺口，不是能力限制。两者混同会让
    // "这次没查成"被说成"本版本不做"。
    SurveyInput allRunning = MakeCleanInput();
    allRunning.mode = SurveyMode::Deep;
    allRunning.nonExecutableMemoryScanned = true;
    ThreadStackInput runningStack = MakeTrustedStack(310U, { 0x7FF800001000ULL });
    runningStack.trust = ThreadContextTrust::RunningThreadUntrusted;
    allRunning.threadStacks = { runningStack };
    const SurveyReport allRunningReport = RunInjectionSurvey(allRunning);
    suite.expect(allRunningReport.hasGap(kGapStackWalkUntrusted),
                 L"栈 全在跑记成缺口");
    suite.expect(!allRunningReport.hasLimit(kLimitStackUnwindUnavailable),
                 L"栈 做过就不再记能力限制");
    suite.expect(allRunningReport.stackThreadsWalked == 1U, L"栈 走过的线程计数");
    suite.expect(allRunningReport.stackThreadsTrusted == 0U, L"栈 可信线程计数为零");
    suite.expect(allRunningReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"栈 缺口压制干净结论");

    // 一个线程都没走过：那才是能力限制。
    SurveyInput noStacks = MakeCleanInput();
    noStacks.mode = SurveyMode::Deep;
    noStacks.nonExecutableMemoryScanned = true;
    const SurveyReport noStacksReport = RunInjectionSurvey(noStacks);
    suite.expect(noStacksReport.hasLimit(kLimitStackUnwindUnavailable),
                 L"栈 没做记能力限制");
    suite.expect(!noStacksReport.hasGap(kGapStackWalkUntrusted), L"栈 没做不记缺口");
    suite.expect(noStacksReport.conclusion == AnalysisConclusion::NoDifferenceObserved,
                 L"栈 能力限制不压制干净结论");

    // 账目：可靠帧计数与线程回源。
    SurveyInput beaconSurvey = MakeCleanInput();
    PayloadCandidateEntry beaconPayload;
    beaconPayload.base = OptionalU64::of(0x600000U);
    beaconPayload.size = OptionalU64::of(0x3000U);
    beaconPayload.type = RegionType::Private;
    beaconPayload.structure = PayloadStructure::HeaderErasedPe;
    beaconPayload.outcome = CollectionOutcome::success();
    beaconSurvey.payloadCandidates = { beaconPayload };
    beaconSurvey.threadStacks = {
        MakeTrustedStack(311U, { 0x7FF800001000ULL, 0x7FF800002000ULL, 0x600100ULL },
                         /*lastFrameHasUnwindData=*/false)
    };
    const SurveyReport beaconReport = RunInjectionSurvey(beaconSurvey);
    suite.expect(beaconReport.stackThreadsTrusted == 1U, L"栈 可信线程计数");
    suite.expect(beaconReport.stackReliableFrameCount == 3U, L"栈 可靠帧计数");
    suite.expect(beaconReport.payloadWithExecutionCount == 1U, L"栈 落点自动判出执行关联");
    suite.expect(beaconReport.conclusion == AnalysisConclusion::DifferenceObserved,
                 L"栈 落点升到观测到差异");
    {
        const InjectionFinding* const hit =
            FindRule(beaconReport.findings, kRuleIdPayloadStructure);
        suite.expect(hit != nullptr, L"栈 载荷条目存在");
        if (hit != nullptr) {
            suite.expect(hit->confidence == EvidenceConfidence::CorroboratedIndependent,
                         L"栈 结构与执行两个来源互证");
            suite.expect(!hit->relatedThreads.empty() &&
                             hit->relatedThreads.front().tid.valueOr(0U) == 311ULL,
                         L"栈 落点回源到具体线程");
            suite.expect(std::any_of(hit->facts.begin(), hit->facts.end(),
                                     [](const std::string& fact) {
                                         return fact == "stack.frame.depth=2";
                                     }),
                         L"栈 落点记录栈深度");
        }
    }

    // 休眠载荷：采集时不可执行的候选**只列不升结论**。
    // 实测底噪是每进程约 0.3 个"首页像个正经 PE"的非可执行区域，放进升结论路径
    // 会让干净机器常态给出"观测到差异"。
    suite.expect(PayloadCandidateCanRaiseConclusion(beaconPayload),
                 L"栈 可执行候选可参与升结论");
    {
        PayloadCandidateEntry dormant = beaconPayload;
        dormant.executableAtScanTime = false;
        suite.expect(!PayloadCandidateCanRaiseConclusion(dormant),
                     L"休眠 不可执行候选不参与升结论");

        SurveyInput dormantSurvey = MakeCleanInput();
        dormantSurvey.payloadCandidates = { dormant };
        // 给一个真的落进这块内存的可靠帧 —— 连这个都不该把它抬上去。
        dormantSurvey.threadStacks = {
            MakeTrustedStack(320U, { 0x7FF800001000ULL, 0x600100ULL },
                             /*lastFrameHasUnwindData=*/false)
        };
        const SurveyReport dormantReport = RunInjectionSurvey(dormantSurvey);
        suite.expect(dormantReport.payloadWithExecutionCount == 0U,
                     L"休眠 不可执行候选即便有帧进入也不升格");
        suite.expect(dormantReport.conclusion != AnalysisConclusion::DifferenceObserved,
                     L"休眠 不可执行候选不升到观测到差异");
        // 但它必须**仍然出现在条目里** —— 不升结论不等于不给人看。
        suite.expect(HasRule(dormantReport.findings, kRuleIdPayloadStructure),
                     L"休眠 不可执行候选仍然列出条目");
        const InjectionFinding* const dormantHit =
            FindRule(dormantReport.findings, kRuleIdPayloadStructure);
        suite.expect(dormantHit != nullptr &&
                         std::any_of(dormantHit->facts.begin(), dormantHit->facts.end(),
                                     [](const std::string& fact) {
                                         return fact == "payload.executable-at-scan=false";
                                     }),
                     L"休眠 条目记录采集时不可执行");
    }

    // 扫过非可执行内存就不该再记那条能力限制。
    {
        SurveyInput scanned = MakeCleanInput();
        scanned.mode = SurveyMode::Deep;
        scanned.nonExecutableMemoryScanned = true;
        scanned.threadStacks = { MakeTrustedStack(321U, { 0x7FF800001000ULL }) };
        const SurveyReport scannedReport = RunInjectionSurvey(scanned);
        suite.expect(!scannedReport.hasLimit(kLimitNonExecutableNotScanned),
                     L"休眠 扫过即无该能力限制");
        suite.expect(scannedReport.conclusion == AnalysisConclusion::NoDifferenceObserved,
                     L"休眠 扫过后仍可给出干净结论");
    }

    // 落点在载荷范围外一个字节：不算进入。
    SurveyInput justOutside = beaconSurvey;
    justOutside.threadStacks = {
        MakeTrustedStack(312U, { 0x7FF800001000ULL, 0x603000ULL },
                         /*lastFrameHasUnwindData=*/false)
    };
    const SurveyReport justOutsideReport = RunInjectionSurvey(justOutside);
    suite.expect(justOutsideReport.payloadWithExecutionCount == 0U,
                 L"栈 范围外一字节不算进入");

    // 不可靠的那一段里出现落点：不算数。把 shellcode 帧标成猜出来的，
    // 它就不该再撑起执行关联。
    SurveyInput guessedHit = beaconSurvey;
    ThreadStackInput guessedStack =
        MakeTrustedStack(313U, { 0x7FF800001000ULL, 0x600100ULL });
    guessedStack.frames[1].derivedFromUnwindData = false;
    guessedHit.threadStacks = { guessedStack };
    const SurveyReport guessedHitReport = RunInjectionSurvey(guessedHit);
    suite.expect(guessedHitReport.payloadWithExecutionCount == 0U,
                 L"栈 猜出来的落点不撑执行关联");
    suite.expect(guessedHitReport.conclusion != AnalysisConclusion::DifferenceObserved,
                 L"栈 猜出来的落点不升结论");

    // --- WOW64 采集器缺口透传 ---
    SurveyInput wow = MakeCleanInput();
    wow.collectorArchitecture = CollectorArchitecture::Wow64;
    const SurveyReport wowReport = RunInjectionSurvey(wow);
    suite.expect(wowReport.hasGap(kGapModuleEnumerationWow64), L"总入口 WOW64 缺口透传");
    suite.expect(wowReport.conclusion != AnalysisConclusion::NoDifferenceObserved,
                 L"总入口 WOW64 列表不完整压制干净结论");

    // --- 完全没有观测 ---
    SurveyInput nothing;
    nothing.processBefore = MakeProcess(4321U, 0x1D000000000ULL);
    nothing.processAfter = nothing.processBefore;
    nothing.addressSpace.outcome = Denied();
    nothing.moduleCrossView.conclusion = AnalysisConclusion::NoEvidence;
    nothing.threadEnumerationOutcome = Denied();
    const SurveyReport nothingReport = RunInjectionSurvey(nothing);
    suite.expect(nothingReport.conclusion == AnalysisConclusion::NoEvidence,
                 L"总入口 无观测即无证据");
    suite.expect(!nothingReport.coverageComplete, L"总入口 无观测覆盖不完整");
}

} // namespace

int RunInjectionSurveyTests() {
    KswordTests::Suite suite(L"J injection survey");
    TestProtectionClassification(suite);
    TestRegionCodeClass(suite);
    TestAddressSpaceIndex(suite);
    TestSurfaceScreen(suite);
    TestModuleCrossView(suite);
    TestWorkingSetScreen(suite);
    TestThreadStarts(suite);
    TestComparisonPlan(suite);
    TestKernelCrossView(suite);
    TestExceptionRelations(suite);
    TestSemanticsAndIdentity(suite);
    TestSurveyPipeline(suite);
    suite.report();
    return suite.failures();
}
