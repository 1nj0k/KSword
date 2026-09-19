#pragma once

// KvmWatchPanel：R-1 内存监视（首次访问归因）页。
//
// 它要回答的问题，是这套 ARK 里其它页都答不了的那一个。快照式检测能告诉用户
// 「SSDT / DriverObject / 回调 / 内核代码现在不对劲」，但答不出**是谁改的、
// 从哪条指令改的、第一次改发生在什么时候**——因为改动那一刻已经过去了。
// 这一页把问题换个方向问：盯住目标，等下一次访问，把那一刻的现场记下来。
//
// 界面上有三件事必须说清楚，因为它们都属于"读起来正确、理解起来会错"的那一类：
//
// - **监视单位是 4 KiB 物理页，不是用户选的那几个字节。** EPT 权限就是页粒度。
//   用户从 DriverObject->MajorFunction[14] 这样一个 8 字节字段建监视，装到硬件
//   上的仍然是那一页。所以请求范围与实际监视页必须并排显示，绝不能把它描述成
//   "8 字节硬件断点"。
// - **请求的访问类型与实际生效的可能不同。** EPT 不允许 W=1 而 R=0，所以"只监视
//   读"在硬件上一定连写也监视了。两栏都留着。
// - **这不是保护。** 命中后原访问照常完成；目标只要把自己那一页换个物理页就不在
//   被监视的页上了；DMA 根本不经过 CPU EPT。它是观察与归因，不是不可绕过的守卫。
//
// 还有一个容易被显示成反面的状态：命中了但事件环没接住。那时"没有事件"与"没被
// 访问过"在事件列表里长得一模一样，而结论正好相反，所以这一页用 watch 自己的
// hitCount / lastHitStatus 来区分，不依赖事件行是否存在。

#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;
class QTextEdit;

namespace ksword::kvm
{
    struct KvmWatchEntry;
}

class KvmWatchPanel final : public QWidget
{
public:
    explicit KvmWatchPanel(QWidget* parent = nullptr);

    // onBusyChanged：与其它 KVM 入口共用的串行化回调。
    // 安装与撤销都要独占驱动侧那把状态锁，别处的命令在飞时不能同时下手。
    std::function<void(bool)> onBusyChanged;

    // refreshAsync：后台读一次 watch 表。查询是阻塞 IOCTL，不能在 UI 线程直接调。
    void refreshAsync();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void setBusy(bool busy);
    void updateEnabledState();
    // applyWatches：把一次快照铺进表格，并顺带做虚拟地址重映射检测。
    void applyWatches(const QVector<ksword::kvm::KvmWatchEntry>& watches);
    // selectedWatch：当前选中行对应的快照，没选中返回 false。
    bool selectedWatch(ksword::kvm::KvmWatchEntry* entryOut) const;
    void showDetail(const ksword::kvm::KvmWatchEntry& entry);

    void startAdd();
    void startRearm();
    void startRemove();
    void openWriterDisassembly();
    void copyEvidence();

    QLabel* m_hintLabel = nullptr;
    QTableWidget* m_table = nullptr;
    QTextEdit* m_detail = nullptr;
    QLabel* m_statusLabel = nullptr;

    QPushButton* m_addButton = nullptr;
    QPushButton* m_rearmButton = nullptr;
    QPushButton* m_removeButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_disassembleButton = nullptr;
    QPushButton* m_copyButton = nullptr;

    bool m_busy = false;
    bool m_queryInFlight = false;
};
