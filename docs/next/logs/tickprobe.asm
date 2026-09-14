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

    call serial_init

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

    ; 同一组数再从串口发一份。
    ;
    ; 屏幕这条路不可靠：来宾显示器超时之后 Hyper-V 的缩略图返回整帧零，而整帧零
    ; 与"来宾屏幕就是黑的"在像素上无法区分，Windows 又**不接受合成输入唤醒显示器**，
    ; 所以一旦睡着就没法自动救回来。串口写进宿主的一个文件，与显示状态无关，
    ; 而且是机器可读的，不用我再去解 RGB565。
    mov si, OFFSET msg_spin
    call serial_puts
    mov ax, ds:[SPIN_LO+2]
    call serial_hex16
    mov ax, ds:[SPIN_LO]
    call serial_hex16
    mov si, OFFSET msg_tick
    call serial_puts
    mov ax, ds:[BDA_TICK+2]
    call serial_hex16
    mov ax, ds:[BDA_TICK]
    call serial_hex16
    mov si, OFFSET msg_crlf
    call serial_puts

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

; --- 16550 串口，COM1 ---
;
; 只用轮询，不开中断：这个探针的全部意义就是判断中断进不进得来，
; 让输出本身依赖中断就成了循环论证。
COM1     EQU 3F8h
COM_LSR  EQU 3FDh               ; 线路状态，bit5 = 发送保持寄存器空

serial_init:
    mov dx, 3FBh                ; 线路控制
    mov al, 80h                 ; DLAB = 1，下面两个口变成除数寄存器
    out dx, al
    mov dx, COM1
    mov al, 1                   ; 除数 1 -> 115200
    out dx, al
    mov dx, 3F9h
    xor al, al
    out dx, al
    mov dx, 3FBh
    mov al, 3                   ; 8 位、无校验、1 停止位，DLAB = 0
    out dx, al
    mov dx, 3FCh                ; 调制解调器控制：DTR | RTS
    mov al, 3
    out dx, al
    ret

; AL 从串口发出去
serial_putc:
    push ax
serial_wait:
    mov dx, COM_LSR
    in al, dx
    test al, 20h
    jz serial_wait
    pop ax
    mov dx, COM1
    out dx, al
    ret

; DS:SI 的零结尾字符串发出去
serial_puts:
    lodsb
    or al, al
    jz serial_puts_done
    call serial_putc
    jmp serial_puts
serial_puts_done:
    ret

; AX 以四位十六进制发出去
serial_hex16:
    mov cx, 4
sh_next:
    rol ax, 4
    push ax
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe sh_ok
    add al, 7
sh_ok:
    call serial_putc
    pop ax
    loop sh_next
    ret

msg_spin db 'SPIN ', 0
msg_tick db ' TICK ', 0
msg_crlf db 13, 10, 0

_TEXT ENDS
END
