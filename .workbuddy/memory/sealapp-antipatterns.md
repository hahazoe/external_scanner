# 目标样本 sealapp 的已知性能反模式（2026-09-18 源码级确认）

分析录制时用这些"已知项"快速对号入座，不必每次重新从数据猜。

## 【已修并验证】队伍邀请列表残留 dump() 调试打印
`app/gui/UITeamInviteList.lua:354 dump(mem)`、`:357 dump(g_GameData:getMyCountry())`、
`:363 dump(color)`，都在 `:345 refreshChar` 里；同函数 346/353/356 的 `print("yx -- ...")`
已被注释 → 是"清 print 漏删 dump"。
调用链：`UITeamInvite.lua:26 registerTickEvent("lowUpdate",1,onMemUpdate)`（1s 定时器）**或**
队伍成员同步消息（`DataManager:dispatchMsg → TeamsData:1831 → UITaskTraceDlg:1536
→ UITeamInvite:139 refreshAllMem`）→ `UITeamInviteList:170 refresh` ×N → `:345 refreshChar` ×N
→ **每次 3 次 dump()**。每次命中单帧烧 12–43ms Lua（incl），164046 里 20 帧（2.1%）命中
却是全部掉帧的 50%。判据：`functions.luac:61 dump_` self>1ms 且 base≈20/969；每次命中恒定
12 dump / 223 dump_ / 126 msg_format_indent / 1200 write / 2833 tostring / 12 traceback。
- `dump()` 是重活（`cocos/cocos2d/functions.lua:52-107`）：traceback + 每键 format +
  带 tostring 比较器的 sort + 每行 print。若值含 protobuf message，`__tostring` 会展开成
  整段 text_format 文本 → 放大几十倍。
- `UITeamInvite:refreshAllMem` 被 1s tick 全量驱动，即便删了 dump 也该改脏标记/变更驱动；
  同时背着 `MapConfig.getMapNameByMapId`（`:5924`，O(n) 扫配置，4 次/帧 ~0.6ms/次）。

## 未修反模式
- **PackageData 线性扫描**：`app/data/PackageData.lua:394 getObjectThisIDByBaseID`（164046 里
  114 次/帧，self 6.7ms）与 `:922 GetObjectNumByBaseId`（12 次/帧）。与 patch 过的全局
  tonumber 叠加可造成 70–90ms 单帧（seq 1978/2063）。
- **UI 预制体 create() 内无条件 addSpriteFrames 8 个 plist**（`Unity/role/UIGrid.lua:9-18`，
  Unity 导出模板，所有预制体同构）。addSpriteFrames 不幂等 → 每次 create 重新解析 8 个 plist。
  是同步加载类尖峰（100ms 级）的头号来源。
- **UIManager:getPanel miss 级联**（`app/gui/UIManager.lua:632`）：判据
  `getPanelName 调用数 ≈ 2 × getPanel 调用数`，说明 :643 的 miss 分支几乎每次都走。
- **UIFolderData:getPanelName**（`:657`）每次 string.format；`:671 getPanel` 里 pairs 递归。
- **tolua.isnull 与 STL/List:size 被高频滥用**（`SceneNpc.lua:947` 一带，各 90~130 次/帧）。
- `checkSummonState`（`SceneNpc.lua:2367`）每次 pairs 整张 SummonCareerConfig。
- patch 框架重定义全局 tonumber（`E:/YZST/config/Patch/src/main.luac:266`，加密容器）。
  173501 实测已降到 33.8 次/帧 · 0.031ms/帧，不再是大头（164046 时 9413 次/帧 · 6.0ms）。

## 173501 新出现（C++ 渲染侧为主，未定案）
- **`cc.Scene:render` 遍历尖刺**：27/35 个 >20ms 帧（77%），S/R 20–60×，Lua 1.9–7.5ms 平坦，
  内存平坦，尖刺 ±2 帧内稀有 Lua 方法覆盖率最高仅 3/27。节拍中位数 **1.321s**，
  而 Lua 周期定时器只有 1.00/1.02/1.07/1.99/0.53s → **触发源在 Lua 之外**。
  需插桩（`Label::updateContent` / `cCharacterExt` visit / `Node::sortAllChildren`）。
- **`SceneNpc.lua:331 createEntity` 同步创建角色实体**：单次 **11.5–45.0ms**（全录制仅 9 次）。
  项目已有 `g_GlobalConf.MaxEntityCreatePerFrame` 分帧预算（`SceneManager.lua:1405/1442/1464/1503`；
  掉落用 `MaxEntityDropCreatePerFrame`；Pet 是 dropItem 时放宽 ×3 见 `:1498`），
  但限的是**个数**不是**耗时** —— seq=3265 只调 1 次、单价 45ms，个数预算挡不住。
  `SceneManager.lua:2343-2355` 有一段被整块注释掉的「分帧创建」循环。
  修法：造型预热 + 预算改耗时 + 屏外延后 + 部件缓存池。
- **`FXManager.lua:558 _shaderFileExists` → `[C]:fullPathForFilename` 单次 10.49ms**：
  链 `:45 apply → :154 createState → :568 loadProgram → :558 ×2`，全录制仅 34 次调用。
  修法：缓存存在性检查结果 + shader 预编译。
- **`UIMiniChat.luac:1925 processChatMsg` → `RichLabel:244 setString` → `labelparser:54 parse` 7.44ms**：
  RichLabel 按字符拆节点。修法：消息入队、每帧限 1 条。
- **`lua_c_self / lua_cpu = 33.9%`**（0.967 / 2.850）：大头 `[C]:<anonymous>` 646.6 次/帧、
  `(for generator)` 311.2、`type` 176.6、`format` 129.0。

## 修复验证记录（对比录制必须同阈值重跑）
| 录制 | 阈值 | 问题帧 | dump 链成因 |
|---|---|---|---|
| 164046（修复前） | body≥20ms | 31（3.20%） | 20 帧（64.5%） |
| 173501（修复后） | body≥20ms | 35（1.85%） | **0** |

判据：173501 里 `functions.luac:61 dump_` / `:52 dump` / `text_format:42 msg_format_indent` /
`print_field` / `[C]:print` / `[C]:traceback` / `[C]:sort` 及整个 `UITeamInvite*` 家族**零命中**。
**两份录制场景不同，绝对帧数不可比；dump 链的"有/无"是二值判据，才是决定性的。**
