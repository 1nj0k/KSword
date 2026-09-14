; tickprobe —— 一个 512 字节引导扇区，只回答一个问题：
;
;   时钟中断有没有进到来宾，如果没有，是在哪一段断的。
;
; 为什么需要它：TinyCore 这个被测物把太多东西焊在一起了 —— 实模式与保护模式
; 每秒往返一万二千次、BIOS 调用、光驱、isolinux 自己的菜单逻辑。任何一处出问题
; 看起来都一样（画面不动），而且**关掉常驻 VMware 直接不启动，所以没有基线可比**。
;
; 这个程序反过来：实模式跑到底，不切模式、不调 BIOS、不碰磁盘，屏幕直接写
; 0B800h 的文本显存。除了下面那几条**故意加的**采样端口，来宾里不产生别的退出。
;
; 屏幕上的读数分成两组。
;
; 第一组是"中断到没到"：
;   SPIN  每轮循环 +1，纯 CPU 执行，不依赖任何中断 —— 阳性对照
;   TICK  读 0040:006C，只有 IRQ0 进来、BIOS 的 8 号 ISR 跑过才会变
;   OWN   我们自己装在 IVT[8] 的处理程序的计数，在 BIOS 那段之前递增
;
; 第二组是"断在哪一段"。驱动侧已经量到：来宾稳态每一次退出 RFLAGS.IF 都是 1，
; VMware 每秒拿到上万次控制权，却**一次注入都没请求过**。剩下两种可能，
; 而它们的分界线正好在 VMware 自己的虚拟芯片组寄存器上，来宾用端口就能读：
;
;   PIT   通道 0 的锁存计数值。它每 838ns 减一，**动 = VMware 的虚拟定时器
;         有时间基准**；恒定不动 = VMware 的时间根本没在推进。
;   IRR   主 PIC 的中断请求寄存器（OCW3=0Ah 后读 20h）。bit 0 = IRQ0 在等。
;   ISR   主 PIC 的在服务寄存器（OCW3=0Bh）。bit 0 置位而不清 = 有人没发 EOI，
;         那会把后面所有中断挡死，是另一种完全不同的故障。
;   SIRR  IRR 的粘滞或值；SISR 同理。IRQ0 抬起来只有几微秒，
;         110 次/秒的采样很可能次次错过 —— **瞬时值为零不能否定"抬过"，
;         粘滞值才能**。
;   RTC   CMOS 秒寄存器（BCD）。第二个互相独立的虚拟时钟：它走而 PIT 不走，
;         说明问题只在 8254 这一个设备上，不是 VMware 的时间总账。
;
; 判读表：
;   PIT 动 + SIRR bit0 = 1 + OWN = 0  -> VMware 抬了 IRQ0 却没投递，是投递路径
;   PIT 动 + SIRR bit0 = 0            -> 虚拟 PIT 在走但不抬中断
;   PIT 不动                          -> VMware 的虚拟时间没推进，往上游查
;   SISR bit0 恒 1                    -> EOI 没发，中断被自己挡住了
;
; 同样的九个读数每秒约 110 次原样写到 COM1，一行 38 个十六进制字符加 CRLF，
; 字段宽度依次是 8/8/8/4/2/2/2/2/2。屏幕那份是给人看的，串口那份是判据 ——
; 见 serout 处关于为什么不能只靠屏幕的说明。
;
; 用 ORG 7C00h 是因为 BIOS 把引导扇区装在 0000:7C00；代码内部只用相对跳转与
; 绝对低内存地址，所以不需要任何重定位。

; --- 为什么全程用基址寄存器寻址，而不是写绝对地址 ---
;
; 这个 MASM 只认 .386（.286 与 .8086 都报 `error A2008: syntax error : .`），
; 而 .386 下它把每个绝对内存操作数当成 32 位偏移，编码成
;
;   67& A1 00000500        mov ax, ds:[SPIN_LO]      ; 6 字节
;
; 6 字节干 3 字节的活。加上采样与显示那几行之后整段代码 615 字节，放不进引导扇区。
; 换成 16 位基址寄存器（32 位寻址模式里根本没有 BX/BP 作基址的形式，所以汇编器
; 必须退回 16 位编码，前缀自然消失）：
;
;   8B 07                  mov ax, [bx]              ; 2 字节
;
; 于是 BX 恒指向 0500h 的便签区、BP 恒指向 BDA 的滴答计数，主循环里不再出现
; 任何绝对地址。这不是风格选择，是这 512 字节唯一装得下的写法。
.386
_TEXT SEGMENT USE16 'CODE'
    ASSUME CS:_TEXT, DS:NOTHING, ES:NOTHING
    ORG 7C00h

VIDEO_SEG EQU 0B800h
BDA_TICK  EQU 046Ch            ; BIOS 定时器计数（32 位），IRQ0 的 ISR 每滴答 +1
SCRATCH   EQU 0500h            ; 便签区：BIOS 数据区之后、引导扇区之前的空隙
IVT_V8    EQU 0020h            ; 实模式中断向量表第 8 项
ATTR      EQU 0Fh              ; 亮白

; 便签区里各项相对 BX 的偏移
O_SPIN    EQU 000h             ; dd 自旋计数
O_OWN     EQU 004h             ; dd 我们自己的 IRQ0 计数
O_SAVED   EQU 008h             ; dd 原来的 8 号中断向量（seg:off）
O_SIRR    EQU 00Ch             ; db IRR 的粘滞或值
O_SISR    EQU 00Dh             ; db ISR 的粘滞或值
O_PIT     EQU 010h             ; dw 本轮读到的 PIT 通道 0 计数
O_IRR     EQU 012h             ; db 本轮 IRR
O_ISR     EQU 013h             ; db 本轮 ISR
O_RTC     EQU 014h             ; db 本轮 CMOS 秒

; 中断处理程序里用得着的绝对地址（那里不能借用被中断代码的 BX）
OWN_ABS   EQU SCRATCH + O_OWN
SAVED_ABS EQU SCRATCH + O_SAVED

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
    ; 这是整个程序里唯一一次 BIOS 调用，发生在 sti 之前、循环之外。
    mov ax, 0003h
    int 10h

    mov ax, VIDEO_SEG
    mov es, ax

    ; 清屏，免得 BIOS 留下的字混进判读
    xor di, di
    mov cx, 80*25
    mov ax, (ATTR SHL 8) OR 20h
    rep stosw

    ; 九行标签，每行四个字符，行距 160 字节。
    ; 用一个循环而不是九次调用：九次 `mov di / mov si / call` 加九个零结尾字符串
    ; 要 126 字节，这样只要 58 字节，而引导扇区里差的正是这几十字节。
    mov si, OFFSET labels
    xor di, di
    mov dx, 9
lab_row:
    push di
    mov cx, 4
lab_ch:
    lodsb
    mov ah, ATTR
    stosw
    loop lab_ch
    pop di
    add di, 160
    dec dx
    jnz lab_row

    mov bx, SCRATCH            ; 之后所有便签区访问都走 [bx+偏移]
    mov bp, BDA_TICK           ; [bp] 默认段是 SS，这里 SS=0，正是 BDA 所在段

    ; 把串口设成 8 位字长、无校验、1 停止位。
    ;
    ; 不设这一行，日志里的每个字节都会**只剩低 5 位**：BIOS 把线路控制寄存器
    ; 留在 0（5 位字长），UART 就只发低 5 位。上一轮的 989 KB 日志正是这样 ——
    ; 数据一个都没丢（'0'-'9' 掩成 10h-19h、'A'-'F' 掩成 01h-06h，两段不重叠，
    ; 可以无歧义还原），但看着像满屏控制字符。
    ; 写 3 同时清掉 DLAB，所以之后 3F8h 就是发送保持寄存器。
    mov dx, 3FBh
    mov al, 3
    out dx, al

    ; 只放行 IRQ0，别的全屏蔽。
    ;
    ; 不是节流，是**把读数变成单变量**：BIOS 默认掩码 0B8h 还放着 IRQ1 与 IRQ6，
    ; 而那台虚拟机的软驱连不上、IRR bit6 一直悬着。屏蔽掉之后，"中断通了"
    ; 就只可能是 IRQ0 通了。
    mov al, 0FEh
    out 21h, al                ; 主片掩码：只留 IRQ0
    mov al, 0FFh
    out 0A1h, al               ; 从片掩码：全屏蔽

    ; 这里**故意不发 EOI**。
    ;
    ; 上一版在这里发过八遍非指定 EOI，用来验证一件事：主 PIC 当时 ISR 恒为 03h、
    ; IRR 恒为 41h，把在服务位清掉之后 TICK 与 OWN 立刻开始走（85 次滴答对上
    ; 5 秒 RTC，16.8Hz）。那证明了投递链路本身是好的，死结是历史上丢掉的两个
    ; 中断造成的 —— 它们被 L1 从虚拟控制器上应答取走，却没有任何处理程序跑过，
    ; 于是 EOI 永远不会发。
    ;
    ; 真因已经在 L0 里修掉了（退出中止了事件投递时按 IDT-vectoring 信息补投）。
    ; 再留着这几条 EOI，就会把"修好了"和"来宾自己把死结解开了"混成同一个读数 ——
    ; **验证修复的探针不能带着绕过缺陷的补丁**。

    xor ax, ax
    mov [bx+O_SPIN], ax
    mov [bx+O_SPIN+2], ax
    mov [bx+O_SIRR], ax        ; 一次写掉 SIRR 与 SISR 两个粘滞字节

    ; 装一个自己的 8 号中断处理程序，链到 BIOS 原来那个。
    ;
    ; 为什么需要它：TICK 不动这一个数字分不清"中断没来"和"中断来了但 BIOS 的
    ; ISR 没跑完"。自己的计数器在 BIOS 那段之前递增，所以 OWN 动而 TICK 不动
    ; 就说明中断到了、BIOS 那一段出了问题；两个都不动才是中断真的没来。
    ; 递增完**链到**原处理程序而不是自己 iret，这样 EOI 与 tick 仍由 BIOS 负责，
    ; 不改变被测行为。
    mov di, IVT_V8
    mov ax, [di]
    mov [bx+O_SAVED], ax
    mov ax, [di+2]
    mov [bx+O_SAVED+2], ax
    mov word ptr [di], OFFSET irq0
    mov word ptr [di+2], 0
    xor ax, ax
    mov [bx+O_OWN], ax
    mov [bx+O_OWN+2], ax

    sti                        ; 到这里才放行中断；IRQ0 只可能从这之后进来

main:
    add word ptr [bx+O_SPIN], 1
    adc word ptr [bx+O_SPIN+2], 0

    ; 低 16 位归零才采样一次，即每 65536 轮、约 110 次/秒。
    ;
    ; 第一版每轮都刷屏，结果自旋只有约 640 轮/秒 —— 因为写 0B800h 显存被当成
    ; MMIO 陷出去了。节流之后 SPIN 约七百万轮/秒。原来取 4000h（220 次/秒），
    ; 加上串口之后每次采样要多发四十个字节，于是退到 110 次/秒 ——
    ; 对 18.2Hz 的目标信号仍然过采样六倍，而总退出率跟加串口之前持平。
    mov ax, [bx+O_SPIN]
    or  ax, ax
    jnz main

    ; --- 采样 VMware 的虚拟芯片组 ---

    ; PIT 通道 0：先发锁存命令（控制字 00h），再读低、高两个字节。
    ; 锁存是为了拿到一个自洽的 16 位值；不锁存直接读会读到正在变的计数器。
    mov al, 0
    out 43h, al
    in  al, 40h
    mov dl, al
    in  al, 40h
    mov dh, al
    mov [bx+O_PIT], dx

    ; 主 PIC：OCW3 选 IRR 再读，OCW3 选 ISR 再读。读完把它留在 IRR 模式，
    ; 免得别人（BIOS 的 ISR）按默认语义读到 ISR。
    mov al, 0Ah
    out 20h, al
    in  al, 20h
    mov [bx+O_IRR], al
    or  [bx+O_SIRR], al
    mov al, 0Bh
    out 20h, al
    in  al, 20h
    mov [bx+O_ISR], al
    or  [bx+O_SISR], al
    mov al, 0Ah
    out 20h, al

    ; CMOS 秒。写 70h 的 bit 7 同时控制 NMI 屏蔽，写 0 就是开着，与上电一致。
    mov al, 0
    out 70h, al
    in  al, 71h
    mov [bx+O_RTC], al

    ; --- 显示，各行第 6 列 ---
    mov di, 12
    mov ax, [bx+O_SPIN+2]
    call hex16
    mov ax, [bx+O_SPIN]
    call hex16

    mov di, 172
    mov ax, [bp+2]
    call hex16
    mov ax, [bp]
    call hex16

    mov di, 332
    mov ax, [bx+O_OWN+2]
    call hex16
    mov ax, [bx+O_OWN]
    call hex16

    mov di, 492
    mov ax, [bx+O_PIT]
    call hex16

    mov di, 652
    mov al, [bx+O_IRR]
    call hex8

    mov di, 812
    mov al, [bx+O_ISR]
    call hex8

    mov di, 972
    mov al, [bx+O_SIRR]
    call hex8

    mov di, 1132
    mov al, [bx+O_SISR]
    call hex8

    mov di, 1292
    mov al, [bx+O_RTC]
    call hex8

    ; 一行结束。串口上每行是定宽的 38 个十六进制字符，按偏移就能切开，
    ; 所以只需要这一个分隔符。
    mov al, 13
    call serout
    mov al, 10
    call serout

    jmp main

; --- 我们自己的 IRQ0 处理程序，计数后链到 BIOS 原来那个 ---
;
; 不自己发 EOI、不自己 iret：EOI 与 tick 都留给原处理程序，这样除了多一个计数器
; 之外什么都没变。用远间接跳转链过去，返回地址仍是被中断的那条指令。
;
; 这里用绝对地址而不是 [bx]：中断可能落在任何一条指令上，借用被中断代码的寄存器
; 是一个只在"恰好是我们的主循环"时成立的假设。CS 在这里必然是 0（我们自己往
; IVT[8] 的段部分写的就是 0），所以 cs: 前缀下的绝对地址无条件正确。
irq0:
    push ax
    add word ptr cs:[OWN_ABS], 1
    adc word ptr cs:[OWN_ABS+2], 0
    pop ax
    jmp dword ptr cs:[SAVED_ABS]

; --- AX 以四位十六进制写到 ES:DI，DI 前进 8 ---
hex16:
    mov cx, 4
hx_next:
    rol ax, 4
    push ax
    and al, 0Fh
    call nibble
    pop ax
    loop hx_next
    ret

; --- AL 的低两个半字节写到 ES:DI，DI 前进 4 ---
; 低半字节要在高半字节写完之后才用，而 nibble 会把 AH 改成属性字节，
; 所以原值压栈保留，不能靠寄存器。
hex8:
    push ax
    shr al, 4
    call nibble
    pop ax
    and al, 0Fh
    call nibble
    ret

; --- AL 的低半字节写成一个字符，屏幕与串口各送一份 ---
nibble:
    add al, '0'
    cmp al, '9'
    jbe nb_ok
    add al, 7
nb_ok:
    mov ah, ATTR
    stosw
    ; 落到 serout。所有经过 nibble 的字符都会同时出现在串口上，
    ; 而标签是直接 stosw 写的，不会混进串口那一路。

; --- AL 送 COM1 ---
;
; 为什么非要有这条路：**屏幕这条路依赖 VMware 有一个可见窗口**，而从
; PowerShell Direct 起虚拟机会把 UI 开在 session 0 的不可见桌面上，宿主侧的
; 缩略图里什么都没有。串口后端是一个文件，与会话、窗口、焦点全都无关，
; 而且给的是时间序列而不是两张快照 —— TICK 是"卡住"还是"走得慢"，
; 只有时间序列分得清。
;
; 等待发送保持寄存器空必须有上界。无上界的轮询在串口没接上时会把程序
; 永远停在这里，而"程序停住"和"来宾收不到时钟"在任何一个观测面上都长得一样 ——
; **夹具的故障不能长成被测现象的样子**。超时就丢掉这个字节：日志里少一个字符
; 一眼可辨（行宽对不上），挂死则不可辨。
serout:
    mov ah, al                 ; 暂存要发的字节，AL 马上要拿去读状态
    mov si, 40h                ; 轮询上界；文件后端下第一次就该是空的
    mov dx, 3FDh               ; 线路状态寄存器
so_wait:
    in  al, dx
    test al, 20h               ; bit 5：发送保持寄存器空
    jnz so_ok
    dec si
    jnz so_wait
    ret                        ; 超时，丢字节
so_ok:
    mov dx, 3F8h               ; 发送保持寄存器
    mov al, ah
    out dx, al
    ret

labels db 'SPINTICKOWN PIT IRR ISR SIRRSISRRTC '

_TEXT ENDS
END
