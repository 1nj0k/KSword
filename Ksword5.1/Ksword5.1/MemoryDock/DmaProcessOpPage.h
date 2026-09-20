#pragma once

// ============================================================
// DmaProcessOpPage.h
// 作用：
// - 通过 DDMA 往目标进程的物理页里写字节：注入载荷，或写 UD2 让目标崩掉。
//
// 与 R-1 那条注入（IOCTL_KSWORD_ARK_HVM_INJECT）的关系：**不是换个后端。**
//
// R-1 注入把载荷放在影子页里，装一个执行视图（执行走影子、读写看真页），并武装
// 一个触发点。真页从头到尾没被改过，任何扫描器读到的都是原样。
//
// DMA 一样都没有。它只有"往物理页写字节"这一件事，于是：
//   * **真页真的被改了**，任何读取路径都看得见，包括本工具自己的 R3/R0/HVM；
//   * **没有触发点**，载荷躺在那里等目标自己执行到，写进一个永远不会被执行的
//     位置等于什么都没做；
//   * **还原是我们的责任**，所以本页强制先备份、并把备份一直留在界面上。
//
// 关于 UD2 那条"结束"：DMA 调不了 PsTerminateProcess，往目标会执行到的代码里写
// UD2 让它因未处理异常退出，是不依赖内核结构偏移的唯一办法。它**不是一个可靠的
// terminate**：目标可能带异常处理器把 #UD 吞掉、生效与否取决于那段代码会不会被
// 执行到、会留下崩溃转储。界面必须把这些说出来，而不是给一个叫"结束进程"的按钮。
//
// 判据与计划全部在 shared/evidence/DmaProcessOpPlan.h，Qt-free，离线套件覆盖。
// 本页只负责采集页内容、发起写入、把读回校验的结果如实展示。
// ============================================================

#include "MemoryAccessBackend.h"

#include "../../../shared/evidence/DmaProcessOpPlan.h"

#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace ksword::memory_dock
{
    // DmaOpRecord：一次已经落地的写入，留着才能还原。
    struct DmaOpRecord
    {
        std::uint64_t virtualAddress = 0;
        std::uint64_t physicalAddress = 0;
        std::size_t offsetInPage = 0;
        std::vector<std::uint8_t> writtenBytes;
        std::vector<std::uint8_t> originalBytes;
        QString description;
        bool verified = false;
    };

    // 不带 Q_OBJECT：与同目录其它页一致，全部用 lambda + connect(this, ...)。
    class DmaProcessOpPage final : public QWidget
    {
    public:
        explicit DmaProcessOpPage(QWidget* parent = nullptr);
        ~DmaProcessOpPage() override;

        void setAttachedProcess(std::uint32_t processId, const QString& processName);
        void refreshChannelAvailability();

    private:
        void buildUi();
        void wireSignals();
        void updateActionState();

        // performWrite：三步都在这里完成，不拆开给调用方——拆开就会出现
        // "写了但没校验"或"没备份就写了"的调用序列。
        void performWrite(bool injectPayload);
        void restoreLastWrite();
        void appendLog(const QString& line);

        bool resolveTargetPage(
            std::uint64_t& virtualAddressOut,
            std::uint64_t& pagePhysicalOut,
            std::vector<std::uint8_t>& pageBytesOut,
            QString& errorOut);

        std::uint32_t m_attachedPid = 0;
        QString m_attachedProcessName;
        std::vector<DmaOpRecord> m_records;

        QLabel* m_targetLabel = nullptr;
        QLineEdit* m_addressEdit = nullptr;
        QLineEdit* m_payloadEdit = nullptr;
        QCheckBox* m_forceCheck = nullptr;
        QCheckBox* m_acknowledgeCheck = nullptr;
        QPushButton* m_injectButton = nullptr;
        QPushButton* m_ud2Button = nullptr;
        QPushButton* m_restoreButton = nullptr;
        QLabel* m_channelHintLabel = nullptr;
        QLabel* m_statusLabel = nullptr;
        QPlainTextEdit* m_logText = nullptr;
    };
}
