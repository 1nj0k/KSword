#pragma once

// ============================================================
// DdmaPage.h
// 作用：
// 1) 配置并管理 DDMA（磁盘直接内存访问）通道，这份配置被"内存"页下的
//    内存搜索、内存查看器、驱动内存读写、系统内存审计四个子页共用；
// 2) 提供 DDMA 自身的物理读写入口；
// 3) 提供"标准通道 vs DDMA"同址复核，用来发现被 SLAT 重定向的物理页。
//
// DDMA 是什么：
// - 让磁盘控制器用总线主控 DMA 直接读写任意物理地址。数据通路走 HBA 而不
//   经过 CPU 页表，因此不受 SLAT/EPT 约束，能读到被上层虚拟化重定向或隐藏
//   的物理页内容。技术来源：https://github.com/btbd/ddma。
//
// 代价（界面上必须始终讲清楚，不能藏起来）：
// - 结构性地需要借用一块磁盘扇区当中转站，所以本页强制要求用户显式指定
//   暂存扇区 LBA 并确认其可被覆盖，不提供任何默认值；
// - 开着内核调试的机器上会命中 MiShowBadMapper 蓝屏，此时整条通道禁用。
// ============================================================

#include "MemoryAccessBackend.h"
// DdmaDiskEntry 按值存进 std::vector，必须拿到完整类型，不能前置声明。
#include "../ArkDriverClient/ArkDriverClient.h"

#include <QByteArray>
#include <QString>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

class QCheckBox;
class QEvent;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class HexEditorWidget;

namespace ks::ui
{
    class VisibleTableWidget;
}

class DdmaPage final : public QWidget
{
public:
    // 构造函数：
    // - 作用：构建 DDMA 页的全部界面并置为"未配置"状态；
    // - 参数 parent：Qt 父控件指针，可为空。
    explicit DdmaPage(QWidget* parent = nullptr);

    // session：
    // - 作用：返回当前 DDMA 会话配置；
    // - 返回：常量引用。本页是进程级会话的唯一写入者，这里返回的就是写进
    //   ksword::memory_backend 那一份的本地副本，两者恒等。
    const ksword::memory_backend::DdmaSession& session() const { return m_session; }

    // setSessionChangedCallback：
    // - 作用：注册会话变化回调，让 MemoryDock 在配置变动后刷新其它页的后端状态；
    // - 参数 callback：无参回调，在主线程调用。
    void setSessionChangedCallback(std::function<void()> callback);

protected:
    // changeEvent：
    // - 作用：深浅色切换后重新下发语义色样式；
    // - 参数 event：Qt 事件对象。
    void changeEvent(QEvent* event) override;

private:
    // ========================================================
    // 界面构建
    // ========================================================

    // initializeUi：构建根布局与四个分组。
    void initializeUi();
    // buildIntroGroup：构建顶部说明块，讲清 DDMA 是什么、代价是什么。
    QGroupBox* buildIntroGroup();
    // buildChannelGroup：构建暂存扇区输入、确认勾选、探测按钮与磁盘表。
    QGroupBox* buildChannelGroup();
    // buildAccessGroup：构建 DDMA 自身的物理读写入口。
    QGroupBox* buildAccessGroup();
    // buildCompareGroup：构建"标准通道 vs DDMA"同址复核入口。
    QGroupBox* buildCompareGroup();

    // applySemanticStyles：
    // - 作用：给会话状态标签与高危按钮下发语义色；
    // - 说明：语义色是调用瞬间的快照，构造期与主题切换必须走同一条路径。
    void applySemanticStyles();

    // ========================================================
    // 交互逻辑
    // ========================================================

    // parseScratchLbaFromUi：
    // - 作用：解析暂存 LBA 输入框；
    // - 参数 lbaOut：输出解析结果；
    // - 参数 errorTextOut：失败原因；
    // - 返回：true 表示用户确实填了一个合法 LBA。空输入一律判为未填写，
    //   不会退化成 0——LBA 0 是 MBR 所在扇区，绝不能靠默认值选中它。
    bool parseScratchLbaFromUi(std::uint64_t& lbaOut, QString& errorTextOut) const;

    // probeChannels：
    // - 作用：枚举 \Driver\Disk 设备并（在已填 LBA 时）逐块做一次 ATA DMA 读探测；
    // - 处理：结果写入磁盘表，同时刷新能力标志与会话可用性。
    void probeChannels();

    // activateSelectedDisk：
    // - 作用：把磁盘表当前选中行设为 DDMA 通道，落地成会话配置。
    void activateSelectedDisk();

    // clearSession：
    // - 作用：清空会话配置，让所有页面立刻退回标准通道。
    void clearSession();

    // refreshSessionState：
    // - 作用：按当前控件与探测结果重算会话可用性并刷新状态标签；
    // - 说明：唯一的可用性判据在 MemoryAccessBackend::isDdmaUsable，本函数
    //   只负责把结论展示出来，不重复实现判据。
    void refreshSessionState();

    // readPhysicalFromUi：按界面参数用 DDMA 读取物理内存并填充十六进制视图。
    void readPhysicalFromUi();

    // writePhysicalFromUi：把十六进制视图里改动过的字节用 DDMA 写回物理内存。
    void writePhysicalFromUi();

    // compareBackendsFromUi：
    // - 作用：对同一物理地址分别用标准通道与 DDMA 各读一页并逐字节比对；
    // - 说明：两者不一致正是"这一页被 SLAT 重定向或隐藏"的直接证据，
    //   这也是 DDMA 相对标准通道的全部价值所在。
    void compareBackendsFromUi();

    // parseAddressText：解析十进制或 0x 十六进制地址。
    static bool parseAddressText(const QString& text, std::uint64_t& valueOut);
    // formatAddress：格式化成 16 位十六进制文本。
    static QString formatAddress(std::uint64_t address);

private:
    // ========================================================
    // 会话与探测缓存
    // ========================================================

    ksword::memory_backend::DdmaSession m_session;      // 当前会话配置。
    std::vector<ksword::ark::DdmaDiskEntry> m_diskCache; // 最近一次探测到的磁盘。
    bool m_probeCompleted = false;                      // 是否至少成功探测过一次。
    bool m_kernelDebuggerEnabled = false;               // 探测到的内核调试状态。
    std::uint32_t m_transferBytes = 0;                  // R0 自报的一次传输长度。
    std::uint32_t m_scratchSectorCount = 0;             // R0 自报的暂存扇区数。
    std::function<void()> m_sessionChangedCallback;     // 会话变化回调。

    // ========================================================
    // 读写快照
    // ========================================================

    QByteArray m_originalBytes;     // 读回的原始字节，用于比对差异。
    QByteArray m_editedBytes;       // 编辑缓存，写回时只提交差异部分。
    std::uint64_t m_snapshotAddress = 0; // 快照起始物理地址。
    bool m_hasSnapshot = false;     // 是否已有有效快照。

    // ========================================================
    // 控件
    // ========================================================

    QLineEdit* m_scratchLbaEdit = nullptr;          // 暂存扇区 LBA 输入。
    QCheckBox* m_scratchAckCheck = nullptr;         // 覆盖确认勾选。
    QLabel* m_scratchImpactLabel = nullptr;         // 实时显示会覆盖哪几个扇区。
    QPushButton* m_probeButton = nullptr;           // 探测按钮。
    QPushButton* m_activateButton = nullptr;        // 启用为通道按钮。
    QPushButton* m_clearButton = nullptr;           // 清除会话按钮。
    ks::ui::VisibleTableWidget* m_diskTable = nullptr; // 磁盘表。
    QLabel* m_capabilityLabel = nullptr;            // 能力标志摘要。
    QLabel* m_sessionStateLabel = nullptr;          // 会话可用性状态。

    QLineEdit* m_accessAddressEdit = nullptr;       // 物理地址输入。
    QSpinBox* m_accessLengthSpin = nullptr;         // 读取长度。
    QPushButton* m_accessReadButton = nullptr;      // DDMA 读取按钮。
    QPushButton* m_accessWriteButton = nullptr;     // DDMA 写回按钮。
    HexEditorWidget* m_accessHexEditor = nullptr;   // 十六进制编辑视图。
    QLabel* m_accessStatusLabel = nullptr;          // 读写状态文本。

    QLineEdit* m_compareAddressEdit = nullptr;      // 复核物理地址输入。
    QPushButton* m_compareButton = nullptr;         // 复核按钮。
    QLabel* m_compareResultLabel = nullptr;         // 复核结论。
    QTableWidget* m_compareTable = nullptr;         // 逐字节差异明细。
};
