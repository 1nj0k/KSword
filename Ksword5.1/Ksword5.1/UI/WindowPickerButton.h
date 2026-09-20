#pragma once

// ============================================================
// WindowPickerButton.h
// 作用：
// - 提供一个"拖十字准星到目标窗口"的进程拾取按钮，松手即得到该窗口所属进程。
//
// 为什么要有它：
// - 按进程名选目标在同名进程多的程序上是选不准的。QQ 这类程序一开就是十个
//   同名进程，主进程和辅助进程只有 PID 与内存量的差别；从别处抄来的地址属于
//   其中某一个，附加到另一个上的表现是"地址读不到"，而读取通路其实完全正常，
//   排查会被整个带偏到内存读取上去。
// - 用户真正知道的是"哪个窗口是我要的那个"，而不是 PID。所以让他们指窗口，
//   由工具去解析 PID——这条路径不存在选错的可能。
//
// 边界：
// - 拾取期间跳过本进程自己的窗口，否则鼠标经过主界面时会把自己报成目标。
// - 高亮框必须对命中测试透明（WS_EX_TRANSPARENT），否则 WindowFromPoint 会
//   打到高亮框自己身上，拖到哪儿都只认得出它。
// ============================================================

#include <QString>
#include <QToolButton>

#include <cstdint>

class QWidget;

namespace ks::ui
{
    // WindowPickerHighlight：覆盖在目标窗口外沿的高亮边框。
    class WindowPickerHighlight;

    class WindowPickerButton : public QToolButton
    {
        Q_OBJECT

    public:
        explicit WindowPickerButton(QWidget* parent = nullptr);
        ~WindowPickerButton() override;

    signals:
        // processPicked：松手完成拾取。processId 为 0 表示落点上没有可用目标。
        void processPicked(quint32 processId, const QString& processName);
        // hoverPreview：拖动过程中目标变化，供调用方做实时提示。
        void hoverPreview(quint32 processId, const QString& processName);
        // pickingChanged：进入/退出拾取状态，供调用方切换提示文案。
        void pickingChanged(bool picking);

    protected:
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;

    private:
        void beginPicking();
        void finishPicking(bool commit);
        void refreshTargetUnderCursor();

        bool m_picking = false;
        quint32 m_hoverProcessId = 0;
        QString m_hoverProcessName;
        WindowPickerHighlight* m_highlight = nullptr;
    };
}
