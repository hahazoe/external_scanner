# YZST 源码与 luac 解密

profiler 行号对解密后的 `.lua`，不对 `E:/YZST` 里的加密容器。

## 两个目录

| 路径 | 是什么 |
|---|---|
| `E:/YZST` | 《原始征途》安装目录。`sealapp.exe`、`libcocos2d.dll`、`liblua.dll`。`src/`、`res/` 是加密 lua。`config/Patch/` 是热更，运行时优先于包内同路径。 |
| `E:/YZST_lua_decrypted` | 解密结果。`src/`（`app/`、`cocos/`、`Config/`、`tbx/`、`xml/`、`main.lua`）是逻辑；`res/` 是导出的 UI Lua。根上的 `decrypt_yszt_lua.py` 只负责解密。 |

`data/`、`locales/` 不是逻辑树。这棵解密树没有贴图和 C++，不能单独当游戏跑。

## 解密

在仓库外执行。默认参数就是这两个目录：

```
python E:/YZST_lua_decrypted/decrypt_yszt_lua.py E:/YZST E:/YZST_lua_decrypted
```

1. 先解密 `E:/YZST/src` 与 `E:/YZST/res`，输出保留 `src/`、`res/` 前缀。
2. 再解密 `E:/YZST/config/Patch`，按热更的相对路径写进同一输出根，同名文件被覆盖。
3. 文件头是签名，后面是 cocos2d-x XXTEA。签名和密钥只在 `decrypt_yszt_lua.py` 里。不要抄进报告、游戏代码或本技能。
4. 明文不是 Lua 字节码（不以 `\x1bLua` / `\x1bLJ` 开头）时，输出扩展名改成 `.lua`。仍是字节码则保留 `.luac`，那种文件不能按源码行号读。

`--clean` 会先删掉输出目录下的 `src`、`res`、`data`、`config`。用户没要求重建整棵树时不要加。`--copy-plain` 会把没有签名的文件一并拷出。

## profiler 路径 → 明文

去掉方法名里的 `[string "` 前缀和 `"]:行号:...` 后缀，再改扩展名 `.luac` → `.lua`。

| chunk | 明文 |
|---|---|
| `app/data/BossListData.luac` | `E:/YZST_lua_decrypted/src/app/data/BossListData.lua` |
| `src/tbx/npc_data.luac` | `E:/YZST_lua_decrypted/src/tbx/npc_data.lua` |
| `cocos/cocos2d/functions.luac` | `E:/YZST_lua_decrypted/src/cocos/cocos2d/functions.lua` |
| `E:/YZST/config/Patch/src/main.luac` | `E:/YZST_lua_decrypted/src/main.lua`（热更覆盖后的那份） |
| `E:/YZST/src/...` | `E:/YZST_lua_decrypted/src/...` |
| `Unity/...` 或 `res/` 下的界面 | `E:/YZST_lua_decrypted/res/` 下同相对路径 |

相对路径先试 `src/`，没有再试 `res/`。行号就是该 `.lua` 的源码行。
