# trm-lite 变更日志

## preview.0 — 2026-08-26

- 初始化 trm-lite 独立仓库骨架（core/{gc,mnn}、lib、tests、docs、tie.pkg、main.tie 占位）。
- 设计定稿：Go 式静态内置 runtime，分两小级——简单 M:N/GC 作 tiec 内置原语（供 actor），复杂 M:N/GC 作 trm-lite runtime 库（`import` 静态链接）。与 trm 并行、互不干扰。
- 设计依据：`tie-main/docs/superpowers/specs/2026-08-26-trm-lite-design.md`。