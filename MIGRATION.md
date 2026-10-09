# 迁移到统一仓库

BSK/UE 适配器与遥操作平台已在 [love-of-city/space_sim_server](https://github.com/love-of-city/space_sim_server) 合并。后续配套改动在统一仓库中提交一个 PR。

## 获取代码

在新的工作目录获取完整平台：

```powershell
git clone https://github.com/love-of-city/space_sim_server.git
Set-Location .\space_sim_server
git lfs install --local
git lfs pull
```

两个子目录 space_sim_server/ 和 space_sim_UE_Adapter/ 已包含在同一次 checkout 中，不再分别 clone。本仓库代码与 main 历史通过 subtree 保留，导入基线为 bf6d3f3892abd66bb892e188463eee3428dce93f；旧标签在目标仓库中命名为 adapter/v0.2.0。

按统一仓库的 README 重新安装两个 editable 包，重新构建 UE，并按新路径生成资产 catalog。已有 episode、认证数据库和机器配置另行保留；不要将旧 Saved/、Intermediate/、run/ 或 DLL 复制到新 checkout。

## 历史记录与归档

本仓库的旧 PR、Issue 和发布记录继续供查询。管理员完成归档后仓库只读；新的开发和问题反馈使用统一仓库。

- [统一安装入口](https://github.com/love-of-city/space_sim_server#readme)
- [适配器文档](https://github.com/love-of-city/space_sim_server/blob/main/space_sim_UE_Adapter/README.md)
- [合并记录与验收](https://github.com/love-of-city/space_sim_server/blob/main/archive/2026_10_09_repository_merge.md)
