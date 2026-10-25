# 怎么确认画面刷新改好了

[English](frame-timer-verification.md)

这次改动只改一件事：**什么时候刷新页面，由扩展决定；页面上显示什么，还是由 Lua 决定。**

改之前，Lua 在每轮循环里把整份状态发给页面，最多每 100 毫秒一次，内容没变就不发。Lua 一慢，页面就停住。改之后，Lua 用 `ReaWeb_SetFrame` 把内容存进窗口，扩展按自己的节奏把这份内容推给页面，同时直接从 REAPER 读播放状态、播放位置、速度和编辑光标一起带上。Lua 慢，只是新内容晚到，页面照样刷新。

要确认改动生效，两件事必须同时成立：**扩展在推帧**，**Lua 在写内容**。分开检查。

## 装什么

三个平台装的文件不一样：

| 平台 | `UserPlugins/` 里放什么 | 运行环境 |
| --- | --- | --- |
| Windows x64 | `reaper_reawebapi-x64.dll` | WebView2 运行时、VC++ x64 运行库 |
| macOS ARM64 / Intel | `reaper_reawebapi-arm64.dylib` 或 `reaper_reawebapi-x86_64.dylib` | 系统自带 WebKit |
| Linux x86_64 / aarch64 | `reaper_reawebapi-<架构>.so` **和** `reawebapi-webview-<架构>` | WebKitGTK 4.1、X11 或 XWayland |

三个平台都要再放一份 `Scripts/ReaWebAPI/Example/` 整个目录。

Linux 那个 `reawebapi-webview-<架构>` 是独立的可执行文件，扩展启动时会去自己所在目录找它，少了就打不开窗口。手工复制过去以后要给它执行权限：

```bash
chmod +x <资源目录>/UserPlugins/reawebapi-webview-*
```

用 Release 里的平台 ZIP 解压到资源目录的话，以上都已经放好，不用手工调。资源目录用 REAPER 菜单 **Options → Show REAPER resource path in explorer/finder** 打开。

复制前先退出 REAPER，复制完再启动。扩展只在启动时加载，开着 REAPER 复制不会生效。

## 第一步：扩展有没有这个新函数

在 Action List 里新建一个 ReaScript（Lua），只写一行：

```lua
reaper.ShowConsoleMsg(tostring(reaper.APIExists("ReaWeb_SetFrame")) .. "\n")
```

运行后控制台显示 `true`，说明 Lua 能把内容交给扩展。显示 `false`，说明装的还是旧扩展，或者没重启 REAPER。

## 第二步：看页面会不会自己动

运行 `Scripts/ReaWebAPI/Example/ReaWebAPI_Demo.lua`，然后在 REAPER 里按空格开始播放，盯着页面上 **Project snapshot** 面板里那排读数：

- **Playback** 从 `Stopped` 变成 `Playing`，后面的秒数连续在变。不是一秒跳一下，也不是停着不动。
- **Tempo** 显示当前速度。播放中改 REAPER 的速度，这个数立刻跟着变。

这两项背后的三个值（播放状态、播放位置、速度）由扩展直接读，不经过 Lua，所以即使 Lua 好一会儿没轮到，它们照样在变。

另外两个显示项不要拿来判断：

- **Edit cursor** 显示的也是扩展读来的值，但读的是编辑光标，不是播放头。播放本身不一定移动编辑光标，所以它停着不动是正常的。
- **Bar / beat** 不在这条通道里，它来自 Lua 发来的工程快照，只在切换轨道或读工程时更新。

## 第三步：看计数，不要凭感觉

点页面上的 **Backend diagnostics** 展开面板，再点 **Refresh snapshot**，然后展开 **View diagnostic JSON**，能看到原始数据：

```json
{
  "backend": {
    "backend": "Lua / reaper.defer + extension frame timer",
    "frameWrites": 42,
    "frameError": "",
    "stateSkipped": 18,
    "writeSkipped": 0,
    "received": 96,
    "sent": 138
  },
  "runtime": {
    "frames": {
      "pushed": 613,
      "intervalMs": 8,
      "luaWrites": 42,
      "luaFrameAgeMs": 12.3
    }
  }
}
```

`runtime` 是扩展返回的诊断数据，`backend` 是 Lua 返回的计数。两边分开看。`backend.frameWrites` 和 `frames.luaWrites` 统计的是同一件事（Lua 成功写入内容的次数），只是分别由 Lua 和扩展各自记的，两个数应该基本一致。

| 字段 | 含义 | 正常时是什么样 |
| --- | --- | --- |
| `frames.pushed` | 扩展一共推了多少帧 | 窗口就绪后就一直涨，**什么都不操作、页面没订阅的时候也在涨** |
| `frames.intervalMs` | 设定的最小推送间隔 | 固定是 `8`，不是实测值 |
| `frames.luaWrites` | Lua 成功写了几次内容 | 切换轨道、拖推子时涨；不动的时候不涨，因为内容没变就不重写 |
| `frames.luaFrameAgeMs` | 页面上这份内容是多少毫秒前写的 | 只在 Lua 写入新内容时归零。**页面静止不动时这个数会一直涨，涨到几万也是正常的** |
| `backend.writeSkipped` | 拖推子时被更新的值取代、没有真正写进 REAPER 的拖动值 | 拖推子时会涨 |
| `backend.stateSkipped` | 拖动应答里省掉整份状态的次数 | 拖推子时会涨。它统计的是真正写进 REAPER 的那些拖动值，所以比 `writeSkipped` 涨得多 |

`frames.intervalMs` 那个 8 是怎么来的。这个通道的节奏本来就由定时器定：25 次探针运行实测，REAPER 调定时器的间隔中位数 31.7 毫秒，所以正常跑的时候定时器每叫醒一次就推一帧，这个 8 根本用不上。它只在卡顿之后那一次追赶里起作用。25 次运行里，卡顿 130 到 141 毫秒之后，REAPER 下一次叫醒是 10.16 到 10.52 毫秒之后。把下限定成 16 毫秒就压在这些追赶之上，把追赶那一帧删掉了：三次卡顿运行里每次 240 帧丢了 20 帧。而这些正是卡顿之后的第一帧，页面本来画得出来。8 毫秒低于实测出现过的所有间隔，所以定时器送来的帧一帧都不会被删；同时它仍然挡住比实测更密的突发。

面板上还有两处文字：

- **REAPER access** 那行末尾的 `N frames drawn`，是页面收到了多少帧。扩展在页面订阅之前也在推帧，所以它一般比 `pushed` 小。
- **Lua messages** 那行显示 `received`、`sent`、`frameWrites`、`writeSkipped`、`stateSkipped` 五个计数。

怎么用这些数判断：

1. `pushed` 在涨：扩展在推帧，这次改动的核心部分生效了。
2. `pushed` 在涨，但 `luaWrites` 一直不动：扩展在推，Lua 没在写。去 REAPER 控制台看 Lua 有没有报错。
3. `pushed` 完全不涨：扩展没在推帧，装的还是旧扩展。回到第一步。
4. `pushed` 和 `luaWrites` 都在涨，页面内容却不更新：Lua 写了，页面没收到，是页面订阅 `frame` 失败。这种情况下 `pushed` 照样在涨，光看计数判断不出来，要看页面顶部错误条有没有 `UNKNOWN_EVENT` 或 `SCHEMA_MISMATCH`。
5. `luaFrameAgeMs` 在页面静止时很大：正常，不用管。要判断它有没有问题，切换一次轨道，它应该立刻掉回几十毫秒以内。
6. `stage` 一直停在 `loading`：窗口一直没握手成功。

## 和改之前比，区别在哪

改之前：Lua 的循环里写的是最多每 100 毫秒发一次完整状态，但循环本身要排队等 REAPER 调 `defer`，实测两轮之间中位数约 31.7 毫秒，忙碌时最慢拖到 111 毫秒。两段时间叠加，实际发送间隔实测中位数约 126.8 毫秒。这中间页面什么都不发生。

改之后：扩展按自己的节奏推帧，不管 Lua 有没有动。Lua 慢，只是新内容晚到。

能直接看出来的地方：开始播放，然后让 REAPER 忙起来，比如打开一个很大的工程，或者跑一个很重的动作。改之前页面上的播放秒数会明显停住；改之后它还在走，只有轨道名这类需要 Lua 提供的内容会晚一点到。

## 出问题看哪里

- 页面顶部的错误条：出现 `ERROR: ...` 就是页面这边报错了。
- REAPER 控制台（**View → Show console output**）：Lua 报错打在这里。
- `diagnostic-output` 里的 `runtime.recentLogs`：扩展这边的最近日志。

三处都没有东西，但页面确实不动，那就是没装对，回到第一步。
