# 关闭 SteamVR 调试命令（按 S 键触发截图）

## 一句话结论

VR 里按 `S`（本意是驱动映射的"后退"）会触发 SteamVR 截图，**根因是 SteamVR 内置的调试命令（Debug Commands）把单字母键 `S` 映射成了 `screenshot_request`**。

SteamVR **没有提供关闭调试命令的开关**；实测**关掉 SteamVR 的 “Debug Commands” 面板**后不再触发。

---

## 现象

- 在 VR 里按 `S`，SteamVR 会截图；
- 截图文件落在：`G:\Games\steamapps\common\SteamVR\screenshots`，形如
  `2026-10-15-PM_10_51_34.png` + `-raw.png`（另有 `-L/-R` 成对的情况）；
- 重启 Steam / SteamVR 后依旧；
- `Ctrl+Space` 等组合键**不会**触发（调试命令的匹配包含修饰键）。

## 根因

### 1. 按键定义在 `debugcommands.js`

`G:\Games\steamapps\common\SteamVR\resources\webinterface\dashboard\debugcommands.js`
（压缩过的 webpack chunk）里有默认键表 `k_DefaultShortcuts`：

```js
screenshot_request:     new d(83, !1, !1)   // 83 = 'S'          -> 普通 S
screenshot_all_request: new d(83, !0, !1)   // 83 + shift        -> Shift+S
seated_position_reset:  new d(90, ...)      // 'Z'
grid_mode_cycle:        new d(71, ...)      // 'G'
simulate_3dof_toggle:   new d(84, ...)      // 'T'
...
```

`d(keyCode, shift, control)`，keyCode 用 Windows 虚拟键码（`83 = VK_S`）。

### 2. 命令由合成器执行

- `vrcompositor.exe` 里包含一份**原生**的调试命令名表：
  `screenshot_request`、`screenshot_all_request`、`grid_mode_cycle`、`seated_position_reset`、`simulate_3dof_toggle` … 以及它自己的窗口输入处理 `hmdwindowbase_input`。
- `vrcompositor.exe` **不读** `debugCommandShortcuts`（该字符串只出现在网页 JS 和 `vrcompositor.exe` 的命令名里，并不作为设置键被读取）。
- 网页面板通过一个小邮箱（mailbox）把命令名发给合成器执行。

### 3. 打开着 Debug Commands 面板就会响应

`steamvr.vrsettings` → `DesktopUI.debugcommands_desktop` 记录了该面板的窗口位置。
**只要这个面板处于打开状态，它就会把键盘输入解释成调试命令**，`S` 即被翻译成 `screenshot_request`。

> 定位证据（`G:\Games\logs\vrserver.txt`，驱动临时诊断日志）：
> `22:51:34.438 [diag] key vk=0x53 DOWN`（0x53 = 'S'）
> → 同时生成 `2026-10-15-PM_10_51_34.png`。

## 最终解决办法

1. **关闭 SteamVR 的 “Debug Commands” 面板**（彻底关闭，不要最小化/后台常驻）；
2. 重启 SteamVR；
3. 验证：按 `S` 不再产生截图。

> 本方案与下面"加固项"是**同时生效**的；其中**关掉该面板**是最直接的对应措施。

## 加固项（可选，一并保留）

| 项目 | 位置 | 内容 |
|---|---|---|
| 调试图标默认键改掉 | `...\SteamVR\resources\webinterface\dashboard\debugcommands.js` | `screenshot_all_request:new d(83,..)` → `new d(0,..)`；`screenshot_request:new d(83,..)` → `new d(0,..)`（0 = 无效键） |
| 清网页缓存 | `C:\Users\GGR\AppData\Local\SteamVR\htmlcache` | 删除 `Cache` / `Code Cache` / `GPUCache`（强制网页重新加载上面的 JS） |
| 关闭截图功能 | `G:\Games\config\steamvr.vrsettings` | 增加 `"userinterface" : { "screenshots" : false }` |
| 配置备份 | `G:\Games\config\steamvr.vrsettings.bak-20261015-222258` | 改动前的备份，可据此回滚 |

> 注意：`debugcommands.js` 属于 SteamVR 安装目录，**Steam"验证文件完整性"或 SteamVR 更新会把它还原**（`S` 又会回来），届时需重做该改动。

## 试过但**无效**的方案（避免重复踩坑）

| 方案 | 为什么无效 |
|---|---|
| 改 `steamvr.vrsettings` → `steamvr.debugCommandShortcuts` | `vrcompositor.exe` 不读这个键；且"清空"不会被持久化 |
| 只改 `debugcommands.js` | 被 CEF 磁盘缓存命中，页面未重新加载 |
| 把键盘焦点切到非 SteamVR 窗口再按 `S` | 与窗口焦点无关 |
| `Prop_DisplayDebugMode_Bool = false` | 与调试命令无关 |
| 怀疑 Steam 自身截图热键 | `localconfig.vdf` 里 `InGameOverlayScreenshotHotKey = KEY_NONE`（本就是关闭） |

## 调试命令默认键位一览（供参考，尽量别用这些单键）

| 键 | 命令 |
|---|---|
| `S` | screenshot_request / screenshot_all_request |
| `Z` | seated_position_reset |
| `G` | grid_mode_cycle |
| `T` | simulate_3dof_toggle |
| `X` | reflection_map_reload |
| `C` | camera_room_view_toggle |
| `E` | application_render_model_visibility_toggle |
| `I` / `O` / `P` / `H` | collision_bounds_* |
| `K` | black_clamp_toggle |
| `R` / `U` / `V` / `Y` | application_throttling / async_mode / gpu_profiler / pixel_line_double_sim |
| `Space` | debug_counter_increment |
| `Shift+F` / `Shift+A` / `Shift+J` | flip_universe / async_mode / motion_vector_debug |
| `.` `=` `-` `[` `]` `;` `'` `\` | single_frame / ipd_offset / vsync_to_photons / brightness / shaders_force_reload |

> 如果以后要在大屏/裸眼屏方案里彻底规避，另一条路是把显示组件从"扩展桌面窗口"改为
> `IVRDriverDirectModeComponent`（驱动自建窗口并自行 Present），不再走合成器的窗口调试路径——工作量较大。

## 相关路径速查

- SteamVR 安装：`G:\Games\steamapps\common\SteamVR`
- 配置：`G:\Games\config\steamvr.vrsettings`
- 截图输出：`G:\Games\steamapps\common\SteamVR\screenshots`
- 网页缓存：`C:\Users\GGR\AppData\Local\SteamVR\htmlcache`
- 调试命令键表：`...\SteamVR\resources\webinterface\dashboard\debugcommands.js`
- 运行时日志：`G:\Games\logs\vrserver.txt` / `vrcompositor.txt`
- 驱动：`U:\Projects\C++\SteamVR\shiyan_nakedmonitor`

## 如何确认 “Debug Commands” 面板是否打开 / 如何关闭

该面板是 SteamVR 的一个**桌面 CEF 窗口**，标题固定为 `debugcommands`
（定义在 `...\SteamVR\resources\webhelperoverlays.json` 的 `debugcommands_desktop`；
页面 `.../webinterface/dashboard/debugcommands.html`，其 `<title>` 就是 `debugcommands`）。

**确认是否打开**

1. 看任务栏 / Alt+Tab：是否有标题为 `debugcommands` 的窗口；
2. 或运行脚本枚举（PowerShell）：

```powershell
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class W {
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
 public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
}
"@
$found = @()
$cb = [W+EnumWindowsProc]{ param($h,$l)
  if([W]::IsWindowVisible($h)){
    $n=[W]::GetWindowTextLength($h)
    if($n -gt 0){ $sb=New-Object System.Text.StringBuilder ($n+1); [void][W]::GetWindowText($h,$sb,$n+1)
      if($sb.ToString() -match 'debugcommands'){ $script:found += $sb.ToString() } }
  }
  return $true }
[void][W]::EnumWindows($cb,[IntPtr]::Zero)
if($found.Count -eq 0){ "未打开" } else { "已打开: " + ($found -join ", ") }
```

3. `steamvr.vrsettings` → `DesktopUI.debugcommands_desktop` 只记录**上次的位置/尺寸**
   （如 `641,239,1112,634,0`），**它的存在只代表"曾经打开过"，不能判定"当前是否打开"**。

**打开 / 关闭**

- 入口：桌面上的 **SteamVR 状态窗口（`vrmonitor.exe`）** → 左上角 **☰ 菜单** → 找 **Debug Commands**
  （通常在 **开发者 / Developer** 分组下）。同名菜单项即为开关。
  （`vrmonitor.exe` 里对应的处理函数是 `OnDebugCommands`。）
- 也可以直接关闭那个窗口（点 ✕，或选中它按 **Alt+F4**）。

**⚠️ 关键**：该窗口在 `webhelperoverlays.json` 里带 **`persist_visibility: true`** —— SteamVR 会
**记住它的可见性并在下次启动时恢复**。所以：

- 只"重启 SteamVR"**不会**关掉它（退出时开着 → 下次启动又自动打开）；
- 必须在 SteamVR **运行中**把它关掉，并让 SteamVR 正常退出，才会持久生效。

**持久化处理（推荐，一劳永逸）**

把该窗口的"可见性持久化"关掉，它就不会在启动时自动恢复成打开：

1. `...\SteamVR\resources\webhelperoverlays.json` 中把 `debugcommands_desktop` 的
   `"persist_visibility": true` 改为 `false`（原文件已备份为 `webhelperoverlays.json.bak`）；
2. 清空 SteamVR 网页配置目录 `C:\Users\GGR\AppData\Local\SteamVR\htmlcache`
   （重置已保存的窗口状态 + JS 缓存）；
3. 重启 SteamVR。

> 这两处都在 SteamVR 安装目录/用户目录下，Steam "验证文件完整性" 或 SteamVR 更新会还原，届时需重做。

## 遗留事项

- 驱动里本次排查用的**临时 `[diag]` 日志已移除**（`shiyan_nakedmonitor\src\device_provider.cpp`），并已重新编译——不再刷 `vrserver.txt`。
- 若以后又出现按 `S` 截图，**首先检查是不是又打开了 “Debug Commands” 面板**。
