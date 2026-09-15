---
name: ksword-object-callback-remove
description: Object Callback 枚举与安全注销链路的句柄语义、重验证和 fail-closed 边界
metadata:
  type: project
---

# Object Callback 安全注销

- `_OBJECT_TYPE.CallbackList` 中的 `_CALLBACK_ENTRY_ITEM.EntryItemList` 节点不是
  `ObRegisterCallbacks` 返回的 `RegistrationHandle`，回调函数地址也不是句柄。枚举协议中，
  仅由匹配内核 PDB profile 的 `_CALLBACK_ENTRY_ITEM.CallbackEntry` 字段解析出的值可放入
  `registrationAddress`；链节点必须单独放在 `rawStorageValue`。
- 启发式 Object Callback 扫描按产品要求开放高风险候选移除：不得设置 `HANDLE` 或
  `VERIFIED_REMOVE`，但找到非零 registration block 时可设置 `REMOVABLE_CANDIDATE`，通过
  EX 协议携带该候选值；UI 必须继续显示 candidate 而不是 verified。
- Object Callback 注销只走 `REMOVE_EXTERNAL_CALLBACK_EX`。请求必须来自完整 V3 枚举行，
  携带 callback、真实 RegistrationHandle、raw node、object/operation mask、trust flags、
  identity hash 和完整快照 generation，并要求 revalidation。
- R0 在调用 `ObUnRegisterCallbacks` 前重新构建与默认 R3 枚举相同的完整有序快照；只有代次
  一致且精确行身份唯一匹配时才使用重新枚举得到的句柄或启发式候选。调用后再次枚举，目标
  精确行仍存在或复核失败都不能报告成功。
- 旧版纯 callback-address 移除入口必须拒绝 Object Callback。Registry 与 ETW 当前没有可靠
  安全注销路径，UI 和 R0 都保持禁用；进程、线程、镜像、Minifilter、WFP 继续使用现有公开 API。
