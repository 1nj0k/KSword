# KSword arXiv preprint v1

整篇英文预印本，作者 **Jiaying Liu**，机构 **Unaffiliated**，
邮箱 **felixliujy@gmail.com**。状态为供作者审阅的初版，未执行 arXiv 上传或投稿。

## 文件

- `main.tex`：完整正文入口，标准 `article`，单栏 11 pt。
- `sections/`：引言、背景、设计、方法、实验、相关工作、局限、结论与复现附录。
- `figures/`：两张可随正文编译的 TikZ 结构图，不依赖外部图片文件。
- `tables/`：从已归档 JSON/CSV 生成的六张数值表；其余环境、故障和范围表在正文中。
- `main.bbl`：完整参考文献，直接输入，不依赖外部 BibTeX/Biber 运行。
- `title.txt`、`abstract.txt`、`authors.txt`、`metadata.json`：提交字段草稿。
- `submission-abstract.md`：与 `abstract.txt` 相同的纯文本摘要。
- `table-provenance.json`：各数值表输入的 SHA256 和关键统计重算值。
- `review-notes.md`：数据边界与作者审查入口。

运行构建后，交付文件在仓库 `output/pdf/`：

- `ksword-live-interposition-v1.pdf`：完整预印本。
- `ksword-arxiv-source-v1.zip`：只含编译所需 TeX、文献和图表源码的上传包。
- `arxiv-source-manifest.json`：源码包条目、SHA256 和静态依赖检查结果。

## 构建

在仓库根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File docs/next/arxiv/Build-Preprint.ps1
```

脚本只重算论文表格、编译文档并打包，不编译驱动，不运行虚拟机或 GUI。
默认查找 `.deps/tectonic/tectonic.exe`，也可用 `-TectonicPath` 指定已有安装。
本地使用 Tectonic 0.17.0（基于 XeTeX）验证。生成的源码使用标准 LaTeX 包；
在 TeX Live 中也可从解压根目录以 `xelatex main.tex` 编译两次。
不能把本地编译通过写成 arXiv 服务器处理已通过；后者尚未执行。

论文只用标准字体和包，无 shell escape、远程图片或私人样式依赖。
TeX 安装可能首次按需下载标准包；后续可使用其本地缓存。
单独重算数值表：

```powershell
python docs/next/arxiv/generate_tables.py
```

底层证据的离线重算步骤见
[当前数据集](../paper-data/20260916-followup/README.md)。
本稿以 `d0d15798f1d92cbdb66244370e6c0c2d4487feb9` 的证据为基线，
当前实现为 `bd703d84a0f45f7567c8d05113578ae9e4adf1bd`。
历史 `b9bdda83` 的 4×2 测量单列，不混入当前样本。

## arXiv 适配

遵循 [LaTeX 源码提交说明](https://info.arxiv.org/help/submit_tex.html)：
源码包不含编译生成的主 PDF、日志、辅助文件、无关原始实验数据或未使用图片；
参考文献随包提供，入口在包根目录，日期固定。
作者、标题和摘要按照
[元数据说明](https://info.arxiv.org/help/prep.html) 整理。
摘要为单段 ASCII、1,610 字符，不含 `Abstract` 标题、Markdown 树或格式命令。
示意图移入正文，不放进 arXiv 的摘要元数据字段。

`cs.OS` 是建议的主分类，`cs.DC` 是可考虑的交叉分类；最终分类由作者决定。
许可尚未选择。journal reference、DOI 保持为空，不把草稿写成已录用论文。
作者审查完成后再决定何时正式提交。

## 关键证据边界

1. 已验证的插入区间没有运行中的中间 VMM；VMware/TinyCore 在常驻后启动。
2. 内层 Hyper-V/TinyCore 基线可正常启动，但 KSword 因根分区未暴露 VMX 而拒绝准入。
3. 当前 CPUID 为关闭常驻的 8.76 倍，loopback RTT 增加 30.7%；未声称低开销已解决。
4. 6.23 ms 是受 pacing、调度和观测影响的 TCP 完成间隔，不是精确暂停。
5. 页生命周期是带前提的采样租约和回收协议，不证明任意页原子更新或相同位模式 ABA 检测。
6. HTTP 数据含 3 次完整、1 次不完整 EPT 试验，常量 holder-alive 字段缺陷在正文披露。
7. 无新的当前版本十分钟/整夜稳定性、其他机器、Win10 LTSC 或 GUI 运行验证。

本目录替代的是论文格式与完整文稿，原 EuroSys 材料保留供历史对照。
