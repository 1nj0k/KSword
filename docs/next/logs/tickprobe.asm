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
OWN_LO    EQU 0504h            ; 我们自己的 IRQ0 计数
SAVED_V8  EQU 0508h            ; 原来的 8 号中断向量（seg:off）
IVT_V8    EQU 0020h            ; 实模式中断向量表第 8 项
ATTR      EQU 0Fh              ; 亮白

; --- BIOS 参数块 ---
;
; 光有 55AA 签名不够。VMware 的软盘库会把引导扇区当成带 BPB 的 FAT 引导扇区来
; 校验，第一版没有 BPB，它把代码字节读成了字段并拒绝引导：
;
;   FLOPPYLIB-IMAGE: Invalid boot sector: signature aa55, sector size 952, sectors 49294
;   FLOPPYLIB-IMAGE: Expected:            signature aa55, sector size 512, sectors 2880
;
; 然后它安静地跳过软盘去引导了光盘 —— 屏幕上出现的是 TinyCore 菜单，看起来像
; "我的程序跑了但什么都没显示"。**夹具被拒绝和被测现象长得一模一样**，判据只在日志里。
;
; 这里的数值就是一张 1.44MB 软盘：2880 个 512 字节扇区、80 磁道 2 面 18 扇区。
; 文件系统字段是做给校验看的，我们不放 FAT，也没人会去读它。
entry:
    jmp SHORT start
    nop
    db 'KSWTICK '              ; OEM 名，8 字节
    dw 512                     ; 每扇区字节数
    db 1                       ; 每簇扇区数
    dw 1                       ; 保留扇区
    db 2                       ; FAT 个数
    dw 224                     ; 根目录项
    dw 2880                    ; 总扇区数
    db 0F0h                    ; 介质描述符：1.44MB 软盘
    dw 9                       ; 每 FAT 扇区数
    dw 18                      ; 每磁道扇区数
    dw 2                       ; 磁头数
    dd 0                       ; 隐藏扇区
    dd 0                       ; 大容量总扇区数
    db 0                       ; 驱动器号
    db 0                       ; 保留
    db 29h                     ; 扩展引导签名
    dd 4B535754h               ; 卷序列号
    db 'KSWORDTICK'            ; 卷标，11 字节
    db ' '
    db 'FAT12   '              ; 文件系统类型，8 字节

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
    mov di, 320
    mov si, OFFSET msg_own
    call puts

    ; 装一个自己的 8 号中断处理程序，链到 BIOS 原来那个。
    ;
    ; 为什么需要它：读数显示 IRQ0 只被注入过**一次**，之后再没有。这有两种可能 ——
    ; 中断根本不再来，或者来了但 BIOS 的 ISR 没跑（那样它既不会加 tick，也不会发
    ; EOI，而 IRQ0 是最高优先级，服务中位不清就把后面所有中断都挡住了）。
    ; TICK 不动这一个数字区分不了这两件事。
    ;
    ; 自己的计数器能：它在 BIOS 的 ISR 之前递增，所以 OWN 动而 TICK 不动就说明
    ; 中断到了、BIOS 那一段出了问题；两个都不动就说明中断真的没来。
    ; 递增完**链到**原处理程序而不是自己 iret，这样 EOI 与 tick 仍由 BIOS 负责，
    ; 不改变被测行为。
    cli
    mov ax, ds:[IVT_V8]
    mov ds:[SAVED_V8], ax
    mov ax, ds:[IVT_V8+2]
    mov ds:[SAVED_V8+2], ax
    mov word ptr ds:[IVT_V8], OFFSET irq0
    mov word ptr ds:[IVT_V8+2], 0
    xor ax, ax
    mov ds:[OWN_LO], ax
    mov ds:[OWN_LO+2], ax

    sti                        ; 到这里才放行中断；IRQ0 只可能从这之后进来

main:
    add word ptr ds:[SPIN_LO], 1
    adc word ptr ds:[SPIN_LO+2], 0

    ; 每 4000h 轮才输出一次。
    ;
    ; 第一版每轮都刷屏，结果自旋只有约 640 轮/秒 —— 因为写 0B800h 显存被当成
    ; MMIO 陷出去了（那一轮量到 76 万次 EPT 违规、14 万次 I/O 退出）。
    ; 循环本身应该只有一条加法，节流之后它才真的是"纯执行"的参照。
    mov ax, ds:[SPIN_LO]
    and ax, 3FFFh
    jnz main

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

    mov di, 332                ; 第 2 行第 6 列
    mov ax, ds:[OWN_LO+2]
    call hex16
    mov ax, ds:[OWN_LO]
    call hex16

    jmp main

; --- 我们自己的 IRQ0 处理程序，计数后链到 BIOS 原来那个 ---
;
; 不自己发 EOI、不自己 iret：EOI 与 tick 都留给原处理程序，这样除了多一个计数器
; 之外什么都没变。用远间接跳转链过去，返回地址仍是被中断的那条指令。
irq0:
    push ax
    add word ptr cs:[OWN_LO], 1
    adc word ptr cs:[OWN_LO+2], 0
    pop ax
    jmp dword ptr cs:[SAVED_V8]

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

; 串口输出曾经加过又去掉了：VMware 的 serial0 file 后端始终没把数据落到文件
;（日志 0 字节），而轮询发送每秒制造约七千次 I/O 退出，反过来扰动被测对象。
; 屏幕这条路在抓图颜色修好之后已经够用。

msg_spin db 'SPIN', 0
msg_tick db 'TICK', 0
msg_own  db 'OWN ', 0

_TEXT ENDS
END
