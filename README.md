# trm-lite — tie 平台的 Go 式静态内置运行时

> 状态：**初始化阶段**（仓库骨架 + 设计定稿，未实现）

trm-lite 为 tie 引入 **Go 式静态内置 runtime** 形态，对标 Go 把调度器/GC 以库形式**静态链接**进单一零依赖二进制。与 trm（字节码 VM，路线 B）**并行开发、互不干扰**。

分两小级：

- **简单 M:N + GC**：作 **tiec 内置原语**（`spawn`/`yield`/channel + 简单 stop-the-world mark-sweep），原生供 actor 使用，零配置。
- **复杂 M:N + GC**：作 **trm-lite runtime 库**（work-stealing 调度 + 并发三色 GC + 可迁移栈），`import trm-lite` 触发静态链接进单一二进制。

哲学：**简单形态走纯编译零依赖；复杂形态以库静态内置。** 同源一套 tie 源码，`import` 即选择。

## 权威设计

架构与验收口径待项目规范后写入 `docs/`（当前以 `tie-main/docs/superpowers/specs/2026-08-26-trm-lite-design.md` 为设计依据）。

## 快速开始

（首里程碑实现后填充）

## 工程结构

```
trm-lite/
├── core/
│   ├── gc/              #  GC（简单：stop-the-world；复杂：并发三色）
│   └── mnn/             #  M:N 调度（简单：协作；复杂：work-stealing）
├── lib/                 # 库层业务能力
├── tests/               # 验收 / 回归
├── docs/                 # 设计文档（独立副本）
├── tie.pkg              # 包清单
└── main.tie             # 入口（占位）
```

## License

本仓库按 tie-lang 组织自创的宽松许可证 **TIE-LANG Open Source License v1.0** 授权发布（全文见 [LICENSE](LICENSE)）：你可自由使用、修改并分发本软件源码，包括用于商业产品，仅需保留版权声明并附本许可证；而用该语言开发的自有软件完全归你所有，不附带任何署名义务。

This repository is released under the **TIE-LANG Open Source License v1.0** (full text in [LICENSE](LICENSE)): you may freely use, modify, and redistribute the source code, including in commercial products, provided you retain the copyright notice and a copy of the license; programs you write in the language are entirely your own, with no attribution obligation.