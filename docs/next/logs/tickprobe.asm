; tickprobe —— 一个 512 字节引导扇区，只回答一个问题：
;
;   时钟中断有没有进到来宾。
;
; 为什么需要它：TinyCore 这个被测物把太多东西焊在一起了 —— 实模式与保护模式
; 每秒往返一万二千次、BIOS 调用、光驱、isolinux 自己的菜单逻辑。任何一处出问题
; 看起来都一样（画面不动），而且**关掉常驻 VMware 直接不启动，所以没有基线可比**。
;
; 这个程序反过来：实模式跑到底，不切模式、不调 BIOS、不碰磁盘、不做端口 I/O，
; 屏幕直接写 0B800h 的文本显存。整个来宾里会产生 VM exit 的东西**只剩中断本身**。
;
; 屏幕上两个数并排，判据全在它们的组合里：
;
;   SPIN  每轮循环 +1，纯 CPU 执行，不依赖任何中断
;   TICK  读 0040:006C，**只有** IRQ0 进来、BIOS 的 8 号 ISR 跑过才会变
;
;   SPIN 动 + TICK 动  -> 中断链路通，问题在原来那个被测物里
;   SPIN 动 + TICK 不动 -> 中断确实没进来，而且是在一个完全受控的来宾上证明的
;   两个都不动         -> 来宾根本没在执行，那是另一回事
;
; 用 ORG 7C00h 是因为 BIOS 把引导扇区装在 0000:7C00；代码内部只用相对跳转与
; 绝对低内存地址，所以不需要任何重定位。

; 用 .386 是因为这个 MASM 版本不再认 .286（`error A2008: syntax error : .`）。
; 代价是 MASM 给每个绝对内存操作数加一个 67h 地址长度前缀，即 32 位有效地址。
; 在实模式下这是合法的，只要有效地址不超过段限 —— 这里用到的 0500h 与 046Ch
; 远小于 0FFFFh，所以没有 #GP 的风险，只是每条指令长两个字节。
.386
_TEXT SEGMENT USE16 'CODE'
    ASSUME CS:_TEXT, DS:NOTHING, ES:NOTHING
    ORG 7C00h

VIDEO_SEG EQU 0B800h
BDA_TICK  EQU 046Ch            ; BIOS 定时器计数（32 位），IRQ0 的 ISR 每滴答 +1
SPIN_LO   EQU 0500h            ; 自旋计数放在 BIOS 数据区之后、引导扇区之前的空隙
ATTR      EQU 0Fh              ; 亮白

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 7C00h              ; 栈往下长，不会碰到 7C00h 起的代码
    cld

    ; 先把显示切到 80x25 文本模式。
    ;
    ; 第一版漏了这一步，结果是纯黑屏：BIOS 的启动画面用的是**图形模式**，
    ; 那时 0B800h 根本不是可见的文本缓冲，往里写什么都看不见 —— 而"看不见"
    ; 和"程序没跑起来"在截图上完全一样。
    ;
    ; 这是整个程序里唯一一次 BIOS 调用，发生在 sti 之前、循环之外，
    ; 所以不影响稳态测量：进入主循环之后来宾只剩显存写与中断。
    mov ax, 0003h
    int 10h

    mov ax, VIDEO_SEG
    mov es, ax

    ; 清屏，免得 BIOS 留下的字混进判读
    xor di, di
    mov cx, 80*25
    mov ax, (ATTR SHL 8) OR 20h
    rep stosw

    xor ax, ax
    mov word ptr ds:[SPIN_LO], ax
    mov word ptr ds:[SPIN_LO+2], ax

    ; 两行标签，让截图自解释
    mov di, 0
    mov si, OFFSET msg_spin
    call puts
    mov di, 160
    mov si, OFFSET msg_tick
    call puts

    sti                        ; 到这里才放行中断；IRQ0 只可能从这之后进来

main:
    add word ptr ds:[SPIN_LO], 1
    adc word ptr ds:[SPIN_LO+2], 0

    mov di, 12                 ; 第 0 行第 6 列
    mov ax, ds:[SPIN_LO+2]
    call hex16
    mov ax, ds:[SPIN_LO]
    call hex16

    mov di, 172                ; 第 1 行第 6 列
    mov ax, ds:[BDA_TICK+2]
    call hex16
    mov ax, ds:[BDA_TICK]
    call hex16

    jmp main

; --- AX 以四位十六进制写到 ES:DI，DI 前进 8 ---
hex16:
    mov cx, 4
hx_next:
    rol ax, 4
    push ax
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe hx_ok
    add al, 7
hx_ok:
    mov ah, ATTR
    stosw
    pop ax
    loop hx_next
    ret

; --- DS:SI 的零结尾字符串写到 ES:DI ---
puts:
    lodsb
    or al, al
    jz puts_done
    mov ah, ATTR
    stosw
    jmp puts
puts_done:
    ret

msg_spin db 'SPIN', 0
msg_tick db 'TICK', 0

_TEXT ENDS
END
