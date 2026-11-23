# Windows 原生客户端（Direct2D）与 Web UI 功能对齐审计

审计对象：

- Web 行为：`frontend/` 中的 React UI、`frontend/src/lib/client.ts` 的协议层，以及
  `backend/src/session_controller.cpp` 中对应的宿主实现。
- Native 行为：`shell/windows/ui_kit_demo.cpp`、`shell/windows/session_client.cpp`、
  `shell/windows/session_client.h`、`shell/windows/ws_client.cpp`。
- 对照基准：当前 `main`（工作区干净）。所有结论都以代码为准，不按函数名推断。

状态图例：

| 状态 | 含义 |
|---|---|
| `ok` | 行为已对齐；剩余差异只是实现方式，不改变用户可感知结果 |
| `partial` | 有对应能力，但字段、范围、交互或反馈缺一块 |
| `gap` | 原生端没有这项能力，或协议消息类型都不对 |

约定：

- 协议示例中的字段名与线上 JSON 完全一致；省略号表示与本项无关的字段。
- 行号是本次审计时 `main` 的行号。
- Native 的会话协议集中在 `session_client.cpp`；UI 交互集中在 `ui_kit_demo.cpp`。
- 本文不评估 Linux/macOS shell，也不评估 WebView2 壳 `main.cpp` 的页面加载逻辑，只在
  “托盘/关闭行为”处引用它作为 Web 版客户端的对照。

---

## 总览

| ID | 域 | 项目 | 状态 | 一句话差异 | 关键位置 |
|---|---|---|---|---|---|
| A1 | 会话与画质 | 六档预设 + auto | gap | 原生仍是 turbo/smooth/balanced/quality/ultra/lossless/auto 七档旧表 | `ui_kit_demo.cpp:465` |
| A2 | 会话与画质 | `session-preset` 全字段消息 | gap | 原生发 `quality`，且没有 chroma/codec/preset 字段 | `session_client.cpp:1609` |
| A3 | 会话与画质 | 高级自定义画质面板 | partial | 有 fps/QP/码率/速率/格式，无 chroma、codec、重置回预设 | `ui_kit_demo.cpp:3280` |
| A4 | 会话与画质 | 宿主 `applied` 回执与 actual 行 | gap | 原生不解析 `session-preset` 回执 | `session_client.cpp:1301` |
| A5 | 会话与画质 | 切档重建编码器/关键帧 | partial | 靠宿主旧 `quality` 路径顺带请求关键帧，原生不主动请求 | `session_client.cpp:1609` |
| A6 | 会话与画质 | 首帧前应用预设 + 按 host 记忆 | gap | 原生 welcome 时不发预设，也不按 host:port 保存 | `session_client.cpp:1323` |
| A7 | 会话与画质 | 变更防抖/去重 | partial | 滑块松手才发，但无 120/300 ms 合并、无 last-sent 去重 | `ui_kit_demo.cpp:3402` |
| A8 | 会话与画质 | auto 档位阶梯 + 帧率负载控制 | gap | 原生没有观看端自适应控制器 | `ui_kit_demo.cpp:2499` |
| A9 | 会话与画质 | 多显示器选择 | gap | 原生没有 `display` 消息，也不解析 welcome.displays | `session_client.cpp:1323` |
| A10 | 会话与画质 | truecolor/4:4:4 的 BGRA 语义 | gap | 原生格式表有 BGRA，但无法请求 4:4:4/no_subsampling | `ui_kit_demo.cpp:479` |
| B1 | 剪贴板 | 文本入站实时同步 | ok | 宿主推送文本时原生会写入本机剪贴板 | `session_client.cpp:1489` |
| B2 | 剪贴板 | 文本出站实时同步 + 192 KiB | gap | 原生只有手动“发送到远端”，没有本地轮询 | `ui_kit_demo.cpp:2476` |
| C1 | 剪贴板 | 主控→被控文件（虚拟流/延迟渲染） | gap | 原生 hello 不宣告 clipboard_stream，也没有文件剪贴板发送 | `session_client.cpp:506` |
| C2 | 剪贴板 | 被控→主控文件（按需区间拉取） | gap | 原生只取文本，忽略 files/omitted，无 `clipboard-pull` | `session_client.cpp:1489` |
| C3 | 剪贴板 | 文件上限、omitted、嵌套 RDP | gap | 原生不解析 caps/omitted/nested_rdp，也没有提示 | `session_client.cpp:1489` |
| C4 | 剪贴板 | 粘贴进度与失败提示 | gap | 原生没有剪贴板文件 offer 横幅/进度/失败 toast | `ui_kit_demo.cpp:7341` |
| C5 | 剪贴板 | 回显抑制（hash/chain、2 分钟表） | gap | 原生无自动文件同步，因此也没有 echo 表 | — |
| F1 | 文件面板 | 本地 \| 远程双栏 | gap | 仍是“发送/接收”两个互斥 tab（各有本地/远程浏览器），不能同屏双栏 | `ui_kit_demo.cpp:8390` |
| F2 | 文件面板 | 本地绝对路径/盘符/根目录 | ok | 本地栏有路径框、盘符/主目录下拉、面包屑与 `FindFirstFileW` 列表 | `ui_kit_demo.cpp:6793` |
| F3 | 文件面板 | 远程绝对路径/盘符枚举/面包屑 | ok | 解析 `parent/roots/truncated`，路径框、roots 下拉、可点面包屑、非法路径内联报错 | `session_client.cpp:790` |
| F4 | 文件面板 | 多选与全选 | ok | 本地/远程列表都有勾选、Ctrl/Shift 多选与全选，动作按钮由选择集合驱动 | `ui_kit_demo.cpp:8972` |
| F5 | 文件面板 | 上传目录递归、空目录、自定义 dest | gap | 选择上传已带当前远程目录作 `dest`，但仍一文件一 offer，无递归/空目录/mkdir | `session_client.cpp:897` |
| F6 | 文件面板 | 下载目录递归与树形落盘 | gap | 原生只请求单个文件，平铺写入下载目录 | `ui_kit_demo.cpp:7617` |
| F7 | 文件面板 | 每任务进度/汇总/失败原因 | ok | 任务卡按批次聚合进度/字节/成功-失败-取消-跳过，失败可展开看原因 | `ui_kit_demo.cpp:8379` |
| F8 | 文件面板 | 并发/大小上限/超时策略 | ok | 8 GiB 上传预检、宿主 8 槽 reject/超时都有中文原因；按钮按 busy/无选择/无目标禁用 | `session_client.cpp:1280` |
| F9 | 文件面板 | 拖入/拖出 | partial | 有 WM_DROPFILES 拖入；文件夹不展开，不能拖出到系统 | `ui_kit_demo.cpp:9633` |
| F10 | 文件面板 | 剪贴板文件卡片（接收全部） | gap | 原生没有剪贴板文件列表 | `ui_kit_demo.cpp:7341` |
| D1 | 输入 | 完整键码映射 | partial | 缺媒体/浏览器/IME/ContextMenu/NumpadEnter 等 | `ui_kit_demo.cpp:4095` |
| D2 | 输入 | 小键盘与 NumLock 补偿 | partial | NumLock 关时小键盘键被映射成 Delete/End 等 | `ui_kit_demo.cpp:4103` |
| D3 | 输入 | 重复键与 IME/组合输入 | gap | 原生每个 WM_KEYDOWN 都转发，包括自动重复 | `ui_kit_demo.cpp:9297` |
| D4 | 输入 | 失焦释放按键 | gap | 只释放鼠标按键，没有按键集合与焦点丢失处理 | `ui_kit_demo.cpp:4051` |
| D5 | 输入 | 组合键转发菜单 | gap | 原生只有 Ctrl+Alt+Del，没有 Win+R/Alt+Tab 菜单 | `ui_kit_demo.cpp:2569` |
| D6 | 输入 | 绝对鼠标/滚轮/按键 | ok | 坐标归一化、1..3 键、滚轮均已实现 | `ui_kit_demo.cpp:4028` |
| D7 | 输入 | 相对/触控板鼠标 | gap | 原生没有虚拟鼠标模式 | — |
| E1 | 诊断/错误 | 本地诊断环形缓冲、7 天、可关 | gap | 原生只有一个无轮转的文本文件 | `ui_kit_demo.cpp:1163` |
| E2 | 诊断/错误 | 主机日志 tail/info/enable/disable | gap | 原生不处理 `logs` 消息 | `session_client.cpp:1301` |
| E3 | 诊断/错误 | 一键复制两端日志/导出/清空/打开目录 | gap | 原生没有日志界面 | `ui_kit_demo.cpp:1163` |
| E4 | 诊断/错误 | 错误分级：toast vs 会话横幅 | partial | 原生按“是否已认证”分，不按错误码分，toast 无等级 | `ui_kit_demo.cpp:5809` |
| E5 | 诊断/错误 | 重连策略与断线横幅/倒计时/停止 | partial | 原生 1 s/1 s 后每 10 s 无限重试，无倒计时/停止 | `ui_kit_demo.cpp:8658` |
| E6 | 诊断/错误 | 主机错误的可执行动作 | partial | 有内联验证码重试；无忽略、无“请求访问” | `ui_kit_demo.cpp:5828` |
| U1 | 界面 | 水印按区域接近淡化 | gap | 原生水印固定不透明 | `ui_kit_demo.cpp:3572` |
| U2 | 界面 | 水印内容完整度 | partial | 缺 backend/版本/控制中/HTTP 版本；顶部编码是配置值 | `ui_kit_demo.cpp:3593` |
| U3 | 界面 | 悬浮工具栏拖拽/固定/收起/全屏/显示模式 | ok | 球+条、拖拽、pin、全屏、三种显示模式都在 | `ui_kit_demo.cpp:3731` |
| U4 | 界面 | 主题深浅 | ok | 原生是 light/dark/auto 超集，能持久化 | `ui_kit_demo.cpp:2896` |
| U5 | 界面 | 语言切换 | gap | `settings_lang` 只弹 toast，不保存也不生效 | `ui_kit_demo.cpp:7001` |
| U6 | 界面 | 通知/toast 行为 | partial | 单条 2.5 s 纯文本；Web 6 s、分 info/warn/error | `ui_kit_demo.cpp:2487` |
| U7 | 界面 | 托盘行为与语言/关闭行为 | partial | 托盘菜单写死中文；关闭默认询问而非隐藏 | `ui_kit_demo.cpp:3002` |

统计：`items=47 gap=27 partial=16 ok=4`。

---

## A. 会话与画质

### A1 六档预设与 auto

**Web 行为**

- 预设集合固定为 `turbo / smooth / balanced / hd / truecolor / auto`，默认
  `smooth`：`frontend/src/lib/session-presets.ts:70-87`。
- 每档解析成 mode/fps/QP/码率/速率模式/format/chroma：
  `session-presets.ts:89-174`。关键值：`hd -> quality, 45fps`；
  `truecolor -> lossless, 18fps, bgra, 444, cq`；`auto -> balanced, 30fps`。
- 工具栏与设置面板都按这个顺序显示，`auto` 与其他五档分开：
  `frontend/src/components/QualityPanel.tsx:506-534`。

**Native 现状**

- 仍是七档旧表 `极速/流畅/平衡/清晰/超清/无损/自适应`：
  `shell/windows/ui_kit_demo.cpp:465-466`。
- 对应线上 mode 为 `turbo/smooth/balanced/quality/ultra/lossless/auto`：
  `ui_kit_demo.cpp:468-469`。
- 默认下标 `preset = 2`（平衡）：`ui_kit_demo.cpp:186`。
- 画质浮层按 7 个格子绘制与点击：`ui_kit_demo.cpp:3306-3325`、`3426-3435`。
- 设置里的“默认画质”是 8 张卡（含 auto/game），另有一套映射：
  `ui_kit_demo.cpp:6740-6756`、`6946-6956`。

**差异**

- 档位语义不同：Web 的 `hd` 是 `quality` 档；原生叫“清晰”的 `quality` 与“超清”
  `ultra`，并且没有 `hd` 这一档的 fps=45 计划。
- Web 的 `truecolor` 是 `lossless + bgra + 444 + cq`；原生 `lossless` 只发
  `mode=lossless, format=preset`，不会进入像素精确路径。
- 设置卡片的 8 档与工具栏 7 档是两套索引，`preset_of[8]` 还要把 `ultra` 折回
  `hq`/`balanced`（`ui_kit_demo.cpp:6954`），选择会互相污染。

**实现建议**

- 在 native 侧建立与 `session-presets.ts` 一一对应的静态表（id、mode、fps、QP、
  bitrate、rate_mode、format、chroma），替换 `kPresets`/`kPresetModes`、
  `kPresetModes` 的 `apply_settings_quality` 映射和所有按下标取值的代码
  （`ui_kit_demo.cpp:465-480`、`2499-2584`、`3306-3335`、`6740-6756`）。
- 这张表最好放进一个新的跨平台头文件（不依赖 `windows.h`），这样可以在 Linux 上单测
  档位解析，Windows 只负责绘制。

---

### A2 `session-preset` 全字段消息

**Web 行为**

- 固定档发送：

  ```json
  {"type":"session-preset","preset":"balanced","fps":30,"quality":0,
   "bitrate_kbps":0,"rate_mode":"auto","format":"preset","chroma":"420",
   "codec":"auto"}
  ```

  `presetMessage()`：`session-presets.ts:563-581`；发送入口
  `frontend/src/lib/client.ts:1633-1636`。
- 高级面板发送 `preset:"custom"` 加用户值，
  `session-presets.ts:390-412`。
- 宿主按 preset 名或 `custom` 解析并把缺省字段补齐：
  `backend/src/session_controller.cpp:2633-2670`；
  `backend/src/session_preset.cpp:20-36`、`93-172`。

**Native 现状**

- 唯一的画质发送函数是：

  ```json
  {"type":"quality","mode":"balanced","fps":30,"qp":0,
   "bitrate_kbps":0,"rate_mode":"auto","format":"preset"}
  ```

  `shell/windows/session_client.cpp:1609-1621`，调用点
  `ui_kit_demo.cpp:2492-2497`。
- 没有 `preset` 名、没有 `chroma`、没有 `codec`；`format` 只有
  preset/gray8/rgb565/bgra 四项（`ui_kit_demo.cpp:479-480`），没有 `rgb24`。

**差异**

- 消息类型不同：Web 是 viewer-owned 的 `session-preset`，原生是宿主全局遗留的
  `quality`。宿主的 `quality` 分支仍然存在
  （`backend/src/session_controller.cpp:2801-2930`），但它不表达 chroma/codec，也
  不会返回 `applied/actual`。
- 选择语义不同：`quality` 路径下 `qp=0` 表示“跟随档位”，但 `chroma` 完全缺失，
  无法表达 Web 的 4:4:4/灰度/565。
- `preset` 名缺失意味着宿主无法按档位补默认值，原生必须把所有值自己填满。

**实现建议**

- 在 `session_client` 增加 `send_session_preset(...)`，参数用与
  `SessionPresetMessage` 相同的字段，直接构造 `type=session-preset`；保留
  `send_quality` 只作为旧宿主兼容路径或删除。
- `ui_kit_demo.cpp:2492` 的 `send_quality()` 改为生成完整 preset 消息；固定档用
  A1 的静态表解析，`custom` 用面板状态。
- `session_client::send_json` 已经能发任意消息（`session_client.cpp:1623-1625`），
  不需要新开 socket 路径。

---

### A3 高级自定义画质面板

**Web 行为**

- 高级参数：fps 5..60、quality(QP) 0..63、bitrate 0..50000、
  rate_mode、format、chroma(420/444/gray/565)、codec(auto/av1/vp9/vp8/h264/tiles)：
  `session-presets.ts:196-244`。
- 面板交互：fps/quality 滑块、bitrate 数字框、chroma/codec/rate_mode 下拉：
  `QualityPanel.tsx:368-479`。
- `chroma=444` 会自动把 format 推到 bgra（或保留 rgb24）：
  `session-presets.ts:364-385`；显式流编码器会把 444 降为 420：
  `session-presets.ts:390-412`。
- 提示：`forced420`、`tiles`、truecolor + 显式编码器：
  `session-presets.ts:417-425`、`QualityPanel.tsx:483-491`、`535-546`。
- “回到预设”按钮：`QualityPanel.tsx:492-499`。
- 300 ms 防抖：`session-presets.ts:219`、`440-469`。

**Native 现状**

- 浮层已有：7 个预设格、速率模式、画质/码率/帧率三个滑块、图像格式下拉、重置：
  `ui_kit_demo.cpp:3280-3399`；状态字段在 `ui_kit_demo.cpp:120-130`。
- 滑块范围：QP 0..63、码率 0..20000、fps 1..60：
  `ui_kit_demo.cpp:3354-3362`、`3452-3458`。
- 格式只有 4 项：`ui_kit_demo.cpp:479-480`。
- 没有 chroma 选择、没有 codec 选择、没有“回到预设”、没有 forced420/tiles 提示；
  `quality_custom()` 仅由 qp/bitrate/rate_mode/format 决定
  （`ui_kit_demo.cpp:853-856`），并且预设被选中时不会把“自定义”标记为活跃。

**差异**

- 参数缺口：chroma、codec、rgb24、bitrate 上限（Web 50000，原生 20000）、
  数字输入（原生只有滑块）。
- 反馈缺口：宿主实际应用值、444 被降为 420、tiles 提示、回到预设。
- 线上语义缺口：即使原生把这些控件画出来，`send_quality` 也传不出去（见 A2）。

**实现建议**

- 在 `app_state` 增加 `chroma_index`、`codec_index`、`advanced_quality`（若要做
  Web 的“高级模式”开关）。
- 复用 `advancedPresetMessage()` 的规则：444 + 显式流编码器 → 420；444 的 format
  强制 bgra/rgb24；`tiles` 保持 chroma。把它移植到跨平台头文件，Windows 只做 UI。
- 面板增加两个下拉（chroma、codec）和一个“回到预设”按钮；码率改为可输入数字，
  上限 50000；重置按钮保留。
- 实际值一行放在面板底部（见 A4）。

---

### A4 宿主 `applied` 回执与 actual 行

**Web 行为**

- 宿主回执：

  ```json
  {"type":"session-preset","applied":true,"preset":"balanced",
   "actual":{"fps":30,"quality":"...","format":"bgra","chroma":"420",
             "codec":"vp9"}}
  ```

  `backend/src/session_controller.cpp:2768-2794`。
- 客户端只在 `applied===true` 时更新 `appliedPreset`：
  `frontend/src/lib/client.ts:2012-2025`；连接层写回 tab：
  `frontend/src/lib/session-connection.ts:485-486`；store 字段：
  `frontend/src/store/app.ts:161-162`。
- 面板显示 “实际 fps / codec / chroma”，未收到时显示“等待宿主”：
  `QualityPanel.tsx:257-274`。
- 回执只观察，不移动选择器：`session-presets.ts:599-613`。

**Native 现状**

- `handle_text()` 的类型分支只有 cursor-position、ping、welcome、codec、transport、
  access-requested、hello_required、error、pong、chat、clipboard、file、
  stream-codec、video-mode、quality：
  `shell/windows/session_client.cpp:1301-1518`。
- `quality` 分支是空操作（`session_client.cpp:1512-1517`），`session-preset` 完全
  没有分支。
- UI 没有“实际值”行，水印显示的是配置档位（`ui_kit_demo.cpp:3596`、`3629`）。

**差异**

- 宿主即使支持 `session-preset`，原生也收不到/看不懂回执；用户无法知道宿主实际用了
  什么 fps/编码器/chroma。
- 旧 `quality` 回执（`backend/src/session_controller.cpp:2902-2928`）同样被空操作
  吞掉。

**实现建议**

- 在 `session_client` 增加：

  ```cpp
  struct preset_applied { bool applied; std::wstring preset;
                          int fps; std::wstring quality, format, chroma, codec; };
  const preset_applied& applied_preset() const;
  ```

- `handle_text()` 增加 `session-preset` 分支，解析 `applied/preset/actual.*`；同时
  解析旧 `quality` 回执作为降级。
- `draw_quality_panel()` 底部加一行“实际: N fps · CODEC · 4:2:0”；
  水印的编码 chip 用实际 codec（见 U2）。

---

### A5 切档重建编码器与关键帧

**Web 行为**

- 首次预设、format/rate/codec 变化、显式编码器变化时宿主请求关键帧：
  `backend/src/session_controller.cpp:2741-2753`。
- 客户端在档位变化后也主动请求关键帧：
  `frontend/src/components/SessionView.tsx:1280-1282`。
- `stream-epoch` 会清空旧 epoch 的解码链：
  `frontend/src/lib/client.ts:2061-2098`；原生解码路径没有对应处理。

**Native 现状**

- `send_quality()` 只发画质消息，不请求关键帧：`session_client.cpp:1609-1621`。
- 旧 `quality` 宿主在 mode 变化或 knob 变化时自己 `request_keyframe()`
  （`backend/src/session_controller.cpp:2825-2829`、`2896-2899`），所以多数切换
  仍会刷新整屏。
- 原生没有 `stream-epoch` 处理；本地 core 的 surface 由 core 负责重置。

**差异**

- 行为上大多数切换“碰巧”正确，但依赖宿主旧路径；一旦切到 `session-preset`，
  原生不主动请求关键帧，编码器重建边界会更慢。
- 原生不处理 `stream-epoch`，tile/stream 来回切时只能依赖 core/surface 重发布。

**实现建议**

- 预设发送成功后，如果 `authenticated() && live()`，补发
  `{"type":"request-keyframe"}`（与 Web 的 `SessionView.tsx:1282` 一致）。
- 在 `session_client::handle_text` 增加 `stream-epoch`：清空 `applied_cells_`、
  丢弃旧帧序号，并置 `has_image_=false` 等待新关键帧；surface 模式只清 tile 状态。

---

### A6 首帧前应用预设 + 按 host:port 记忆

**Web 行为**

- `SessionConnection` 在 welcome 前缓存 `pendingPreset`：
  `frontend/src/lib/session-connection.ts:143-145`、`996-1025`。
- `SessionView` 在会话 live 后立即发送当前档位：
  `SessionView.tsx:1259-1292`，因此宿主在第一帧前就按观看端档位编码。
- 每 host:port 保存档位与 fps：
  `session-presets.ts:471-538`；store 写入/读取：
  `frontend/src/store/app.ts:1019-1027`、`1671-1675`。
- 高级参数也按 host:port 保存：`session-presets.ts:315-359`。

**Native 现状**

- welcome 分支只发首次画面请求；平铺模式发送的是写死的
  `{"type":"quality","mode":"balanced","format":"bgra"}`：
  `session_client.cpp:1354-1364`。
- surface/stream 模式完全不发画质消息（`session_client.cpp:1348-1353`、
  `1388-1394`）。
- 画质状态只在 `app_state` 内存里（`ui_kit_demo.cpp:122-130`），没有按 host 保存；
  注册表只保存主题、pin、关闭行为（`ui_kit_demo.cpp:2887-2941`）。

**差异**

- 宿主在连接开始时先用自己的默认档位编码，用户看到的第一帧可能与工具栏档位不一致。
- 断开重连、切换设备或重启后，原生回到 `balanced`/`30fps`；Web 保持上次选择。

**实现建议**

- 把当前 preset/advanced 参数塞进 `session_endpoint` 或 `session_client`，在 `start()`
  后、`welcome` 时立即发送（`session_client.cpp:1323` 分支）。
- 按 `host:port` 保存到 `HKCU\Software\moLink\Quality`（或 core 的 `ui.json`），
  键与 Web 的 `molink.preset.*`/`molink.advanced.*` 语义一致；`start_connect()` 时读取
  并恢复 UI。

---

### A7 变更防抖/去重

**Web 行为**

- 固定档 120 ms 合并，只发最后一条：
  `frontend/src/lib/session-connection.ts:999-1015`。
- 高级面板 300 ms 防抖：
  `session-presets.ts:219`、`440-469`。
- `lastSentPreset` 用完整 JSON 去重，拖回原值会取消定时器：
  `SessionView.tsx:1235-1272`。

**Native 现状**

- 滑块拖动期间只改内存值；`WM_LBUTTONUP` 时统一 `send_quality()`
  （`ui_kit_demo.cpp:9542-9548`），相当于“松手才发”。
- 预设格、速率模式、格式下拉点击后立即发送：
  `ui_kit_demo.cpp:3426-3443`、`3463-3467`。
- 没有 `last-sent` 比较，同一档位重复点击会重复发；没有定时器合并。

**差异**

- 单次拖动行为可以接受；但连续快速切档会重建多次编码器，且重复发送相同值。
- 高级参数在拖动中不发送，和 Web 的 300 ms 连续预览不同（体感更“卡顿”）。

**实现建议**

- 增加 `last_preset_json` 字符串；发送前比较，相同则跳过。
- 用 `SetTimer` 做 120 ms（固定档）/300 ms（自定义）合并；`WM_LBUTTONUP` 只启动
  定时器而不是直接发。
- 定时器回调里发 `session-preset` 并请求关键帧（A5）。

---

### A8 auto 档位阶梯与帧率负载控制

**Web 行为**

- 仅 `preset === 'auto'` 时启用档位阶梯：根据延迟、丢帧、wire fps、带宽在
  smooth/balanced/quality 之间移动；降档 3 个样本、升档 5 个样本，带 dwell：
  `SessionView.tsx:1294-1378`。
- 帧率控制器根据延迟、丢帧、欠帧、宿主 buildMs 调整目标 fps：
  `SessionView.tsx:1380-1459`。
- 宿主 buildMs 来自 `capture-stats`：
  `frontend/src/lib/client.ts:2001-2005`；store 更新
  `frontend/src/lib/session-connection.ts:519-521`。
- 每次调整都通过 `session-preset` 的 `fps` 字段下发（`SessionView.tsx:1275-1279`）。

**Native 现状**

- `auto` 只是旧 mode 表里的一项（`ui_kit_demo.cpp:469`），没有任何观看端调整逻辑。
- 原生的统计只有接收码率和显示 fps（`session_client.cpp:1577-1601`），不解析
  `capture-stats`，没有 `hostFps/buildMs`。
- 没有 `capture-fps` 发送路径（Web 虽然定义了 `setCaptureFps`，主路径仍走
  `session-preset.fps`）。

**差异**

- 选“自适应”时，Web 会随链路和编码能力改变画面档位与帧率；原生只是把 `auto` 交给
  宿主，之后什么都不做。
- 原生无法看到宿主编码耗时，无法避免“编码器跟不上但仍要求 30fps”。

**实现建议**

- 先完成 A2/A4，让原生有真实的 `applied` 和 `capture-stats`。
- 把 Web 的两个控制器移植成纯函数（输入 latency/dropped/wireFps/buildMs/target，
  输出 tier/fps），放在跨平台头文件里，Linux 单测；Windows 只负责每秒调用。
- `session_client` 增加 `capture-stats` 解析，暴露 `host_fps()`、`host_build_ms()`。

---

### A9 多显示器选择

**Web 行为**

- welcome 里的 `displays` 列表用于工具栏选择器：
  `SessionView.tsx:2391`、`3069-3088`。
- 选择时发送 `{"type":"display","index":1}`：
  `frontend/src/lib/client.ts:1638-1642`。
- 宿主处理并回报 `display`：`backend/src/session_controller.cpp:2932-2960`。

**Native 现状**

- 只有分辨率切换 `{"type":"display-mode","width":..,"height":..}`：
  `ui_kit_demo.cpp:2525-2534`。
- welcome 解析不包含 `displays`：`session_client.cpp:1323-1416` 只读
  `codec_preference/video_mode/session_token/avatar/screen_w/screen_h/device_name/security`。
- UI 没有显示器下拉。

**差异**

- 多显示器主机上，原生只能看当前采集屏，无法切到另一块屏；Web 可以。
- Web 还会收到宿主主动的 `display` 广播（`client.ts:1879-1883`），原生忽略。

**实现建议**

- `welcome` 解析 `displays`（数组：index/name/width/height/primary），保存到
  `app_state`；工具栏增加下拉。
- 新增 `session_client::send_display(int index)`，发
  `{"type":"display","index":n}`；处理宿主 `display` 消息更新当前索引。

---

### A10 truecolor/4:4:4 的 BGRA 语义

**Web 行为**

- `truecolor` 计划：`lossless + 18fps + cq + format=bgra + chroma=444`：
  `session-presets.ts:146-159`。
- 宿主侧：显式 BGRA/RGB24 + chroma=444 → `no_subsampling=true`，保持平铺像素精确路径，
  不启动 4:2:0 视频流：`backend/src/session_preset.cpp:154-172`。
- 显式流编码器会把 444 降为 420，并在面板说明：
  `session_preset.cpp:172-190`、`QualityPanel.tsx:535-546`。

**Native 现状**

- 格式下拉有“真彩 BGRA”（`ui_kit_demo.cpp:479-480`），但没有 chroma 字段，
  也没有 truecolor 档位（见 A1）。
- `send_quality` 只发 `format=bgra`（`session_client.cpp:1609-1621`），宿主旧路径会把
  format 设为 bgra，但 chroma/`no_subsampling` 由档位和编码器决定
  （`backend/src/session_controller.cpp:2874-2893`），原生无法显式要求 4:4:4。

**差异**

- 原生可以“选 BGRA”，但无法完整表达 Web 的 truecolor：没有 `chroma=444`、
  没有 `preset=truecolor`、没有“显式编码器会降 420”的提示。

**实现建议**

- 包含在 A1/A2/A3 中：新增 truecolor 档位、chroma 控件和完整 preset 消息。
- 原生解码路径已经支持 BGRA/RGB24/RGB565/Gray8（`session_client.cpp:233-315`），
  不需要新解码器；只需把请求发对。

---

## B. 剪贴板

### B1 文本入站实时同步

**Web 行为**

- 宿主推送 `{"type":"clipboard","text":"..."}` 或
  `{"type":"clipboard-sync","text":"..."}`；客户端转成 `onClipboard`：
  `frontend/src/lib/client.ts:1926-1961`。
- 连接层只让前台会话写本机剪贴板，并记住 `lastClipboard` 防止回显：
  `frontend/src/lib/session-connection.ts:526-541`。
- 本机写入由 store/sink 完成；文本上限 192 KiB 在宿主端截断
  （`backend/src/clipboard/windows_clipboard.cpp:1612-1613`），超限会回
  `clipboard_too_large`（`session_controller.cpp:3098`）。

**Native 现状**

- `handle_text()` 的 `clipboard`/`clipboard-sync` 分支把非空 `text` 放进
  `clipboard_inbox_`：`session_client.cpp:1489-1498`。
- UI 每秒取出并写入本机剪贴板，受 `clipboard_on` 控制：
  `ui_kit_demo.cpp:8574-8579`；写入函数 `ui_kit_demo.cpp:7633-7648`。
- 原生只维护一个会话，因此没有多标签“前台会话”竞争。

**差异**

- 功能已对齐：远端复制文本 → 原生本机可粘贴。
- 细节差异：原生不记录 hash/chain，也不在写回时更新一个“最后写入值”；由于没有
  出站轮询（B2），目前不会形成回显环。Web 还会在收到文本时清掉虚拟文件挂载
  （`session-connection.ts:526-531`），原生没有虚拟文件，暂不影响。

**实现建议**

- 保持现状；在实现 B2 时同时加入 `last_clipboard_text_`，避免自己写回的内容再次
  上送。hash/chain 字段可以原样回传或忽略，但不影响文本路径。

---

### B2 文本出站实时同步 + 192 KiB

**Web 行为**

- 每 1 s 轮询 `navigator.clipboard.readText()`，变化时发送
  `{"type":"clipboard-sync","text":...,"hash":...,"chain":""}`：
  `session-connection.ts:946-978`、`client.ts:1772-1786`。
- 页面隐藏时跳过轮询；连续失败 3 次后 5 s 退避：
  `session-connection.ts:951-972`。
- 手动“发送到远端”仍保留：`{"type":"clipboard","action":"set","text":...}`
  （`client.ts:1693-1695`）。
- 超限错误 `clipboard_too_large` 归类为 warn toast：
  `frontend/src/lib/session-alerts.ts:47-53`。

**Native 现状**

- 只有手动菜单/按钮：

  ```json
  {"type":"clipboard","action":"set","text":"..."}
  ```

  `ui_kit_demo.cpp:2476-2485`，入口在 `ui_kit_demo.cpp:2541-2549`、
  `4297-4304`。
- 没有剪贴板变化监听，没有轮询，没有 `clipboard-sync` 出站。
- 收到远端文本时会写本机剪贴板（B1），但不会把这次写入再同步出去。

**差异**

- 用户在本机 Ctrl+C 后，Web 会自动同步到远端；原生必须手动点“发送到远端”。
- 原生没有 192 KiB 的本地提示；超限时宿主会截断或回错，原生只把它当普通 `error`
  显示。
- 没有 `hash/chain`，无法参与 Web 的回显抑制链。

**实现建议**

- 在 `ui_kit_demo.cpp` 注册 `AddClipboardFormatListener(g.window)` 并处理
  `WM_CLIPBOARDUPDATE`；用 `GetClipboardSequenceNumber()` 去重，读
  `CF_UNICODETEXT`，与 `last_clipboard_text_` 比较后发送：

  ```json
  {"type":"clipboard-sync","text":"...","hash":"<fnv1a>","chain":""}
  ```

  注意：Windows 的 `AddClipboardFormatListener` 可用；若要保持 Win7 兼容，也可以
  仿照宿主后端用 `GetClipboardSequenceNumber` 每秒轮询（
  `backend/src/clipboard/windows_clipboard.cpp:1531-1556`）。
- 在 `session_client` 增加 `send_clipboard_sync(text)`；`clipboard_on=false` 时停止
  监听和发送。
- 收到 `clipboard_too_large` 时用 warn 级 toast（见 E4）。

---

### C1 主控→被控文件（虚拟流/延迟渲染）

**Web 行为**

- hello 宣告文件剪贴板能力：

  ```json
  {"type":"hello","token":"...","clipboard_stream":true,"clipboard_pull":true,
   "client":"molink-web","protocol":1,...}
  ```

  `frontend/src/lib/client.ts:1040-1050`。
- 本地 core 暴露本机剪贴板文件快照；页面每秒轮询：
  `frontend/src/lib/clipboard-sync.ts:42-44`、`92-130`。
- 宿主支持延迟渲染时，页面只发描述符：

  ```json
  {"type":"file","action":"clipboard-offer","files":[...],"dest":"~/moLink/clipboard-lazy"}
  ```

  `clipboard-sync.ts:346`；宿主保存并等待 `clipboard-fetch`。
- 不支持延迟渲染时走 eager：`offer` + `file_data` + `complete`，最后
  `{"type":"file","action":"clipboard-set","paths":[...]}`：
  `clipboard-sync.ts:571-635`。
- 上限：`clipboard_lazy` 8 个文件 / 2 GiB
  （`backend/src/clipboard_lazy.h:33-34`）；普通剪贴板路径另有
  `kClipboardPathScanMax=64`（`backend/src/clipboard/clipboard_files.h:26-28`）。

**Native 现状**

- hello 只有：

  ```json
  {"type":"hello","client":"molink-native","token":"...","seq_clock":true,
   "video_push":true,"h264_stream":false,"vp9_stream":...,"av1_native":...}
  ```

  `session_client.cpp:506-522`。没有 `clipboard_stream`、`clipboard_pull`、
  `protocol`。
- `request-access` 也没有这两个能力位：`session_client.cpp:495-497`。
- 没有读取本机 CF_HDROP/文件剪贴板，没有 `clipboard-offer`/`clipboard-fetch`/
  `clipboard-set`。

**差异**

- 原生完全无法把本机复制的文件粘贴到远端；宿主也不会把它当作可流式剪贴板客户端，
  只会有文本路径。
- 没有延迟渲染，也不会有虚拟挂载。

**实现建议**

- hello/request-access 增加 `clipboard_stream`、`clipboard_pull`（以及
  `protocol:1`），与 Web 一致。
- 在 native 侧枚举 `CF_HDROP`（`DragQueryFileW`），并监听剪贴板变化（B2 的监听器
  可复用）；构造 `clipboard-offer`，接收 `clipboard-fetch` 时按索引发送
  `file_data`，最后发 `clipboard-set`。
- 如果不想在 native 里重复文件剪贴板逻辑，可以调用本地 core 的
  `/api/clipboard/files` 与 `/api/clipboard`（Web 走的就是这条路），但 native 自己
  就是本机进程，直接 Win32 更简单。

---

### C2 被控→主控文件（按需区间拉取）

**Web 行为**

- 客户端解析：

  ```json
  {"type":"clipboard-sync","files":[{"id":1,"name":"a.txt","size":123,
    "path":"C:\\..."}],"nested_rdp":true,"omitted":2}
  ```

  `client.ts:1938-1961`。
- 页面按 256 KiB 发区间请求：

  ```json
  {"type":"file","action":"clipboard-pull","id":1,"offset":0,"length":262144}
  ```

  `frontend/src/lib/remote-clipboard.ts:690-695`。
- 宿主回二进制包 `0x18 CLIPBOARD_FILE`（`u32 id + u32 status + u64 offset + data`）：
  `backend/src/session_controller.cpp:3429-3510`、
  `backend/src/protocol/frame_packet.h`。
- Linux 上还可以把远端文件挂成 FUSE 虚拟目录，按需拉取：
  `frontend/src/lib/virtual-clipboard.ts:22-30`、`179`。

**Native 现状**

- `handle_text()` 的 clipboard 分支只看 `text`，`action:"files"` 被忽略：
  `session_client.cpp:1489-1498`。
- `handle_binary()` 只处理 `0x12 file_data`、`0x15 tile_clock`、`0x02 frame`：
  `session_client.cpp:528-562`；没有 `0x18`，也没有发 `0x17`。
- 没有 `clipboard-pull` 发送。

**差异**

- 远端 Explorer 复制的文件在 Web 上会出现在文件面板/本机剪贴板；原生完全看不到。
- 嵌套 RDP 场景（被控端里再开 mstsc）也只能在 Web 上工作。

**实现建议**

- `session_client` 增加剪贴板文件 offer 状态：解析 `files[]`（id/name/size/path/
  omitted/nested_rdp），暴露给 UI。
- 增加 `clipboard_pull(id, offset, length)`；`handle_binary` 增加 `0x18` 解析，
  按 offset 顺序落盘（可复用 `download_state` 的“等 eof 再完成”思路，
  `session_client.cpp:1131-1162`）。
- UI 增加“接收全部”与单项下载（Web 的 F10），落盘到用户选择目录或
  `~/Downloads/moLink`。

---

### C3 文件上限、omitted、嵌套 RDP

**Web 行为**

- 宿主对每个 viewer 的 id 列表按 8 个文件 / 单文件 1 TiB / 总量 4 TiB 截断，并回
  `omitted`：`backend/src/session_controller.cpp:77-86`、`325-362`。
- `clipboard-sync.files` 会带 `nested_rdp:true`：
  `session_controller.cpp:395-400`。
- 客户端把 `omitted>0` 转成“远端剪贴板太大”提示：
  `frontend/src/lib/remote-clipboard.ts:234`、
  `session-alerts.ts:47-53`。
- 嵌套 RDP 只作为一个信息 badge，不再禁用虚拟文件：
  `session-connection.ts:280-287`。

**Native 现状**

- 不解析 `omitted`、`nested_rdp`、`files`；welcome 也不解析
  `clipboard_lazy/clipboard_lazy_files/clipboard_lazy_bytes/clipboard_nested_rdp`
  （`session_client.cpp:1323-1416`）。
- 没有上限提示。

**差异**

- 原生用户看不到“文件太多已截断”或“内层 RDP 剪贴板”的状态；实现 C1/C2 后必须补
  这些字段，否则会出现“只收到前 8 个文件但没有解释”的情况。

**实现建议**

- 解析并在 UI 上显示 `omitted`（例如“另有 N 个文件未列出”）。
- 解析 `nested_rdp`，在剪贴板/文件面板显示 badge；不要因此关闭虚拟/流式路径。

---

### C4 粘贴进度与失败提示

**Web 行为**

- 远端剪贴板文件超过自动上限时，顶部出现 offer 横幅：文件数/总大小、下载进度条、
  “下载 / 取消 / 忽略”按钮：`SessionView.tsx:2576-2633`。
- 自动/手动拉取失败会走 warn/error toast：
  `frontend/src/lib/remote-clipboard.ts:86`、`session-alerts.ts:39-53`。
- 文件面板中还有“剪贴板文件”卡片，可“全部接收”或单项下载：
  `FileTransferPanel.tsx:4156-4210`。

**Native 现状**

- 文件抽屉只有普通传输列表（`ui_kit_demo.cpp:7341-7553`），没有剪贴板文件区。
- 只有通用的 `show_toast`（`ui_kit_demo.cpp:2487-2490`），没有进度条/取消/忽略。

**差异**

- 原生无法在粘贴前告知用户“要传多少”，也无法中途取消剪贴板文件下载；失败只有一条
  2.5 s 文本。

**实现建议**

- 复用文件抽屉的进度条与取消按钮（`ui_kit_demo.cpp:7524-7551`），在接收 tab 顶部加
  “剪贴板文件”卡片；按钮语义与 Web 一致：接收全部/单项/取消/忽略。
- 失败原因沿用 `file_event.message`，至少显示到列表行，而不是只弹 toast。

---

### C5 回显抑制（hash/chain、2 分钟表）

**Web 行为**

- `ClipboardEcho` 记住本机刚写到远端剪贴板的 host 路径或描述符，TTL 2 分钟、最多
  64 条：`frontend/src/lib/clipboard-echo.ts:17-22`、`96-140`。
- 反向同步前过滤 `echo===true` 和已记住的路径/name+size：
  `clipboard-sync.ts:104-116`、`remote-clipboard.ts`。
- `clipboard-sync` 带 FNV-1a `hash` 和 `chain`（`client.ts:1772-1786`）。

**Native 现状**

- 没有自动文件同步，因此没有 echo 表；手动 `clipboard set` 也不会记录。
- 文本出站缺失（B2），所以暂时不会形成环。

**差异**

- 实现 C1/C2 后，如果没有 echo 抑制，会出现“本机复制 → 远端再公告 → 又传回来”的
  循环，尤其是文件 `a (1).txt` 的重命名链。

**实现建议**

- 与 B2/C1/C2 同批实现：在 native 侧维护一个 64 条、2 分钟的表，记录 host 路径
  （大小写/分隔符归一化）和 `name+size` 描述符；发送前跳过，接收公告时先查表。
- `clipboard-sync` 的 `hash/chain` 可以照抄 Web 的 FNV-1a 算法；宿主当前主要用
  文本内容判断，但保持字段一致有助于后续兼容。

---

## F. 文件面板

### F1 本地 | 远程双栏

**Web 行为**

- 文件面板是 `grid-cols-[1fr_74px_1fr]`：左本地、中间“发送/接收”、右远程：
  `FileTransferPanel.tsx:3725-4315`。
- 两个 pane 都有标题、根目录选择、上/主页/刷新、路径输入、全选、列表：
  本地 `3728-3984`，远程 `4021-4313`。

**Native 现状**

- 抽屉只有“发送 / 接收”两个 tab：`ui_kit_demo.cpp:7370-7386`。
- 发送 tab 是拖放区/文件选择器（`7397-7418`），接收 tab 是远端目录浏览器
  （`7419-7488`）；两者互斥，没有本地文件列表。

**差异**

- 原生无法同时看到本地和远端目录，也无法在本地目录里挑多个文件/文件夹。
- 用户的“找文件 → 传到对面目录”动线比 Web 长。

**实现建议**

- 把 `draw_files_drawer()` 改成双栏布局：左栏本地、右栏远程、中间两个动作按钮。
- 左栏数据来源二选一：
  1. 直接 Win32 `FindFirstFileW`/`GetLogicalDrives`；
  2. 复用本地 core 的 `/api/fs/list`（Web 的做法，
     `frontend/src/lib/api.ts:957-981`），native 已有 `local_api_token`
     （`ui_kit_demo.cpp:303-304`）。
- 保留现有接收 tab 的目录浏览逻辑作为右栏。

---

### F2 本地绝对路径/盘符/根目录

**Web 行为**

- 桌面壳（本机 core）通过 `/api/fs/list?path=...` 列出任意本地路径，返回
  `path/root/parent/roots/entries`：`frontend/src/lib/api.ts:957-981`。
- 本地栏有根目录下拉（`FileTransferPanel.tsx:3732-3753`）、绝对路径输入
  （`3785-3805`）、面包屑（`3871-3889`）、上/主页/刷新（`3754-3783`）。
- 浏览器环境退化为 `showDirectoryPicker` / `webkitdirectory`：
  `1810-1860`。

**Native 现状**

- 发送侧只有“拖放文件”和 `pick_files()` 的 `GetOpenFileNameW` 多选
  （`ui_kit_demo.cpp:7397-7418`、`6172-6197`）。
- 没有本地目录枚举、盘符列表、路径输入。

**差异**

- 原生无法浏览本地目录，只能逐个选文件；选择的文件也不能保持目录结构（F5）。
- Web 在桌面壳里已经能输入 `C:\Users\me\Desktop` 直接跳转，原生不能。

**实现建议**

- 增加 `local_path`、`local_entries` 状态；实现 `local_browse(path)`（Win32 或
  `/api/fs/list`）与 `local_roots()`（`GetLogicalDrives` + `GetDriveType`）。
- 复用文件的 `unique_download_path`/`format_bytes`（`ui_kit_demo.cpp:923-950`、
  `6078-6086`）做行渲染。

---

### F3 远程绝对路径/盘符枚举/面包屑

**Web 行为**

- 远程栏根目录下拉来自 `listing.roots`：`FileTransferPanel.tsx:4025-4043`。
- 路径输入框支持绝对路径、`~`、相对路径与 `..`：
  `4074-4098`；解析规则在
  `frontend/src/lib/file-transfer.ts:176-202`。
- 面包屑：`4111-4134`；上/主页/刷新：`4044-4072`。
- 宿主 listing 返回 `path/parent/root/roots/truncated`：
  `backend/src/file_transfer.cpp:551-561`。

**Native 现状**

- listing 解析只取 `path/root/entries`，忽略 `parent/roots/truncated`：
  `session_client.cpp:760-819`。
- UI 显示路径文本（`ui_kit_demo.cpp:7438-7446`），只有上/刷新（`7599-7615`）；
  没有路径输入、没有盘符列表、没有面包屑。
- “上一级”在路径没有分隔符时直接清空回到 home，而不是 roots：
  `ui_kit_demo.cpp:7603-7613`。

**差异**

- 原生只能从 home 往下点，无法直接输入 `D:\data`，也看不到“此电脑”的盘符列表。
- 到达盘符根后无法回到 roots 视图。

**实现建议**

- `file_event` 增加 `parent`、`roots[]`（name/path/kind）、`truncated`；
  `handle_file_json` 解析（`session_client.cpp:764-819`）。
- UI 增加路径输入框（回车 `file_browse(resolve_path)`）、roots 下拉、面包屑。
- “上一级”使用 `parent`；`parent==""` 时发 `{"type":"file","action":"browse","path":""}`
  回到 roots（宿主空路径就是 roots，`backend/src/file_transfer.cpp:494-533`）。

---

### F4 多选与全选

**Web 行为**

- 本地/远程行都有 checkbox，支持全选，动作按钮按选择数量启用：
  `FileTransferPanel.tsx:3891-3907`、`4136-4154`。
- 选择集合分别保存 `localSelection`/`remoteSelection`：
  `712-718`、`1991-2010`。
- 上传/下载任务以选择集合为单位创建（多文件任务）：
  `3356-3374`、`3394-3401`。

**Native 现状**

- 本地只有文件选择器多选：`pick_files()` 解析
  `dir\0name1\0name2\0\0`（`ui_kit_demo.cpp:6183-6195`）。
- 远程列表点击即进入目录或请求单文件（`ui_kit_demo.cpp:7617-7629`），没有勾选
  状态、没有全选、没有批量动作。

**差异**

- 原生无法一次下载多个远端文件，也无法对已列出的本地文件做批量选择。
- 选择器多选是唯一批量入口，且无法跨目录。

**实现建议**

- 增加 `local_selected`/`remote_selected` 集合（按 name 或完整路径）。
- 行左侧画 checkbox（参考 `ui_kit_demo.cpp:7469-7486` 的点击区），标题行加全选。
- 中间按钮按选择数量启用；空选择时给出原因（参考 F8）。

---

### F5 上传目录递归、空目录、自定义 dest

**Web 行为**

- 上传计划把选择展开成“文件 + 需要显式创建的空目录”：
  `frontend/src/lib/file-transfer.ts:326-384`。
- 每个文件用相对路径作为 `name`，并带目标目录：

  ```json
  {"type":"file","action":"offer","name":"photos/a.jpg","size":123,
   "dest":"D:\\backup"}
  ```

  `FileTransferPanel.tsx:2509-2513`；空目录先发
  `{"type":"file","action":"mkdir","path":"photos/empty","dest":"..."}`
  （`2471-2472`）。
- 256 KiB 一块，`complete` 在最后一块之后：
  `FileTransferPanel.tsx:2560-2645`、`backend/src/file_transfer.h:30`。
- 目标目录取当前远程 listing 的 `path`：`FileTransferPanel.tsx:3216`。

**Native 现状**

- `file_offer(name, local_path, destination="")` 只接受单文件：
  `session_client.cpp:855-873`。
- `pump_files()` 一次只处理一个 offer，`dest` 永远是空：
  `session_client.cpp:973-988`、`980-984`。
- 没有 `mkdir`，没有目录遍历；`add_transfer()` 只对单个路径调用
  （`ui_kit_demo.cpp:6109-6123`）。
- 拖入文件夹时 `GetFileAttributesExW` 能成功但大小是 0，随后 `ifstream` 打开目录
  失败（`session_client.cpp:1049-1056`），用户看到“无法读取本地文件”。

**差异**

- 原生不能上传文件夹，不能保留目录结构，不能指定远端目标目录（只能落到宿主默认
  `~/moLink`）。
- Web 的 `mkdir` 只用于空目录，原生完全没有。

**实现建议**

- 增加 `file_offer_relative(relative_name, local_path, dest)` 和
  `file_mkdir(relative_path, dest)`；`pump_files` 支持队列（可保留单活动 offer，但
  `dest` 要带上）。
- 在 native UI 侧实现递归遍历（`FindFirstFileW`），生成相对路径；空目录收集后先发
  `mkdir`。
- 目标目录取本地选择的远端路径；F3 完成后用户可以直接输入/选择。

---

### F6 下载目录递归与树形落盘

**Web 行为**

- 对选中的远端目录先递归 `browse`，再对每个文件发 `request`：
  `FileTransferPanel.tsx:2684-2750`（walk）、`3384-3545`（download run）。
- 落盘路径通过 `relativeUnder()`/`safeTargetSegments()` 保持远端树：
  `frontend/src/lib/file-transfer.ts:219-240`、`554-565`。
- 有 File System Access 时写目录句柄；否则退化为逐文件 Save/浏览器下载：
  `file-transfer.ts:578-584`、`FileTransferPanel.tsx:3481-3532`。

**Native 现状**

- `file_request()` 只发单文件路径：`session_client.cpp:880-883`。
- UI 只对非目录行调用 `file_request`（`ui_kit_demo.cpp:7617-7627`）。
- 下载落盘固定为 `%USERPROFILE%\Downloads\moLink`，用 `unique_download_path`
  平铺（`session_client.cpp:1107-1114`、`923-950`）。

**差异**

- 原生不能下载文件夹；多个同名文件会被重命名成 `a (1).txt`，而不是保留目录结构。
- 用户不能选择保存目录（只能改 registry/API 的 `download_dir`，UI 没有入口；
  `ui_kit_demo.cpp:419`、`6098`）。

**实现建议**

- UI 侧对目录行递归 `file_browse` 建树，再对叶子文件发 `request`；记录
  `relative_path`。
- `download_state` 增加相对路径/根目录字段，落盘前 `CreateDirectoryW` 建父目录。
- 增加“选择保存目录”按钮（`SHBrowseForFolder`/`IFileDialog`），保存到设置。

---

### F7 每任务进度/汇总/失败原因

**Web 行为**

- 任务卡：方向、名称、`done/total` 文件数、已传/总量、速度、取消；
  完成后显示失败/取消/跳过与目录统计：
  `FileTransferPanel.tsx:4431-4560`。
- 进度按字节聚合并按子项状态计算：`file-transfer.ts:404-461`。
- 失败原因保留在子项 `error` 字段并显示（`4521-4560`）。

**Native 现状**

- `file_transfers()` 只返回扁平行：id/name/size/sent/download/state：
  `session_client.cpp:1241-1264`。
- 抽屉只显示最近 3 行（`ui_kit_demo.cpp:7391-7395`、`7495-7552`），每行一个百分比；
  失败只显示“失败”（`7513`），原因只弹 2.5 s toast（`6164-6167`）。
- 没有文件数、总字节、速度、跳过计数、目录统计。

**差异**

- 传输多个文件时，原生无法知道总进度，也无法回看失败原因；只保留最近 3 行。
- 取消是逐 id，不是按任务。

**实现建议**

- 把 `file_transfer_state` 扩成任务+子项（或新增 `file_task` 聚合），至少带
  `done_bytes/total_bytes/done_files/failed_files/skipped/error`。
- 抽屉改为可滚动任务列表，展示汇总与每个子项的错误文本。
- 保留 per-id 取消，但任务卡上的取消应取消整组（Web 的 `cancelTask`）。

---

### F8 并发/大小上限/超时策略

**Web 行为**

- 宿主固定 8 个上传/下载槽；超过回 `reject`/`sending` 失败：
  `backend/src/file_transfer.cpp:289-294`、`567-587`。
- 单文件上限 8 GiB：`backend/src/file_transfer.h:30-31`。
- 浏览 15 s、offer 30 s、done 90 s、下载 30 s 无进展超时：
  `FileTransferPanel.tsx:112-115`、`897-900`、`1090-1105`。
- 按钮禁用原因明确：`file-transfer.ts:472-501`（no-selection/no-target/
  no-source/busy/unsupported）。

**Native 现状**

- 上传/下载超时：30 s 无进展、eof 后 3 s 未补齐 → `error`：
  `session_client.cpp:1167-1185`。
- 上传每 tick 最多两块，`complete` 延后一 tick：
  `session_client.cpp:1191-1232`。
- 没有 8 GiB 预检、没有 busy/无目标等禁用原因；`file_offer` 失败只弹“无法发送”
  （`ui_kit_demo.cpp:6116-6118`）。

**差异**

- 大文件会传到宿主才被 `reject`，用户等待后才知道；Web 在本地计划阶段就能提示。
- 没有“请先选择目标目录/文件”的按钮说明，只有失败 toast。

**实现建议**

- 上传前检查 `size > 8 GiB`；下载 `sending.size` 超限直接失败并保留原因。
- 复用 Web 的 `uploadBlockReason/downloadBlockReason` 枚举，按钮 tooltip 显示原因。
- 增加一个全局“活动任务数”提示；宿主 8 槽限制可以通过 `reject.message` 展示。

---

### F9 拖入/拖出

**Web 行为**

- 面板整体接收拖放，按目录树展开并上传：
  `FileTransferPanel.tsx:3681-3697`、`frontend/src/lib/file-transfer.ts:289-308`。
- 远端文件可拖出到系统（Chromium 的 DownloadURL/拖拽）：
  `FileTransferPanel.tsx:4233-4247`、`4317-4354`。
- 会话画面本身也支持拖放文件 → 打开文件面板并上传：
  `SessionView.tsx:1081-1114`。

**Native 现状**

- 注册了 `DragAcceptFiles` 并处理 `WM_DROPFILES`：`ui_kit_demo.cpp:9633-9650`；
  每个顶层路径调用 `add_transfer()`（`6109-6123`）。
- 拖入文件夹不会展开（见 F5）。
- 没有拖出（`IDropSource`/`DoDragDrop`）实现。

**差异**

- 原生拖入单个文件可用；文件夹失败。
- 无法把远端文件直接拖到 Explorer/桌面，只能下载到固定目录。

**实现建议**

- 拖入：对每个路径判断 `FILE_ATTRIBUTE_DIRECTORY`，目录走 F5 的递归计划。
- 拖出：先实现“下载到临时文件 + `DoDragDrop(CF_HDROP)`”，或在文件面板提供
  “另存为”；后者的成本远低于 OLE 拖出，可以先做。

---

### F10 剪贴板文件卡片（接收全部）

**Web 行为**

- 远程栏上方有剪贴板文件卡片：文件名/大小、单项下载、“全部接收”：
  `FileTransferPanel.tsx:4156-4210`。
- 点击单项走 `startQuickDownload()`；全部接收走 `startClipboardDownloads()`。
- 浏览器无法写系统剪贴板时提示改用文件面板（`4181-4188`）。

**Native 现状**

- 没有该卡片；剪贴板文件支持本身缺失（C2/C3）。

**差异**

- 原生完全看不到远端剪贴板里的文件列表。

**实现建议**

- 在 C2 完成后，把 `clipboard_files` 直接渲染到文件抽屉接收 tab 顶部；按钮语义与
  Web 一致：全部接收 / 单项 / 取消。

---

## D. 输入

### D1 完整键码映射

**Web 行为**

- 优先用 `event.code`（物理键），归一化 `OSLeft/OSRight` 等旧别名：
  `frontend/src/lib/key-code.ts:125-138`。
- `event.key + location` 作为回退，数字键盘用 `location===3` 保留 Numpad 码：
  `key-code.ts:140-162`。
- 覆盖命名键：媒体键、浏览器键、IME 键、ContextMenu、Clear、Help、Select 等：
  `key-code.ts:56-123`。
- 转发前只取归一化后的 code，空串则丢弃：
  `SessionView.tsx:2137-2142`、`2157-2158`。

**Native 现状**

- `browser_key_code()` 覆盖：
  - 字母/数字；
  - Enter/Escape/Tab/Backspace/Delete/Insert/Space；
  - 方向/Home/End/PageUp/PageDown；
  - Shift/Ctrl/Alt/Win/Caps/Num/Scroll/Pause/PrintScreen；
  - OEM 标点、F1..F24、Numpad0..9/Add/Subtract/Multiply/Divide/Decimal；
  - `ui_kit_demo.cpp:4095-4159`。
- 缺少：ContextMenu、Help、Select、Execute、Clear、Again、Undo、Cut、Copy、Paste、
  Find、Cancel、Power/Sleep/WakeUp、音量/媒体/浏览器键、IME（Convert/Kana 等）、
  AltGraph、NumpadEnter/NumpadComma/NumpadEqual/NumpadSeparator。
- OEM 标点用 `VK_OEM_*` 映射到 US 布局的物理位置；非 US 布局或 `VK_OEM_102` 会
  落到空串（`4150-4158`），而 Web 的 `event.code` 与布局无关。

**差异**

- 媒体/浏览器/IME 键在原生端直接丢失。
- 非 US 键盘上，标点键可能发送错误的 code 或干脆不发。
- 小键盘 Enter 被当成主键盘 Enter（`4103`），丢失 NumpadEnter。

**实现建议**

- 用 `MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX)` 得到扫描码，按扫描码生成
  `KeyA/Digit1/...`，这样与键盘布局无关；对 OEM 键保留 `VK_OEM_*` 作为回退。
- 扩展命名表，至少覆盖 `key-code.ts:56-123` 的全部项；未知键保留空串丢弃而非发错。
- 小键盘用 `lParam` 的 extended 位与 `VK_NUMLOCK` 区分 NumpadEnter/Decimal（见 D2）。

---

### D2 小键盘与 NumLock 补偿

**Web 行为**

- 浏览器始终发送物理 code（如 `Numpad7`、`NumpadDecimal`、`NumpadEnter`）；
  NumLock 状态由宿主补偿。
- 宿主在 Windows 上读取主机 NumLock，按键按下时临时打开/恢复：
  `backend/src/input/windows_input.cpp:183-230`；哪些 code 需要补偿由
  `backend/src/input/keymap.h:37-39` 定义。
- 小键盘 Clear（NumLock 关）保持 `Clear`，不映射成 Numpad5：
  `key-code.ts:143-147`。

**Native 现状**

- `VK_NUMPAD0..9` 映射 Numpad0..9（`ui_kit_demo.cpp:4155-4157`），仅在 NumLock
  开启时 Windows 才发这些 VK；NumLock 关闭时物理小键盘 7 发的是 `VK_HOME`
  （`4108-4117`），原生会发 `Home`，宿主补偿逻辑收不到 `Numpad7`。
- `VK_DECIMAL` 仅在 NumLock 开时出现；关闭时是 `VK_DELETE`（`4107`）。
- `VK_RETURN` 永远映射 `Enter`（`4103`），无法区分 NumpadEnter。
- 没有发送 `NumpadComma/NumpadEqual/NumpadSeparator`。

**差异**

- NumLock 关闭时，原生把数字键盘当方向/编辑键发；Web 仍发 Numpad 码，宿主补偿后
  行为与本地一致。
- 小键盘 Enter 在宿主看来是主 Enter，多数场景无感，但依赖 NumpadEnter 的应用会不同。

**实现建议**

- 在 `WM_KEYDOWN/WM_KEYUP` 里读取 `lParam` bit 24（extended）与
  `GetKeyState(VK_NUMLOCK)`：
  - `VK_RETURN + extended` → `NumpadEnter`；
  - `VK_DELETE + !extended + NumLock off` → `NumpadDecimal`；
  - `VK_HOME/END/LEFT/RIGHT/UP/DOWN/PRIOR/NEXT/CLEAR` 且 `!extended` →
    对应 `Numpad7/1/4/6/8/2/9/3/5`；
  - 保持 `NumLock` 键本身的映射。
- 这样宿主现成的补偿逻辑（`windows_input.cpp:183-230`）就能工作。

---

### D3 重复键与 IME/组合输入

**Web 行为**

- `keydown` 忽略 `event.repeat` 与 `event.isComposing`：
  `SessionView.tsx:2154-2156`；`keyup` 也忽略 composing：
  `2177-2179`。
- 输入法组合期间不把中间态发给远端。

**Native 现状**

- `WM_KEYDOWN` 对 `remote_input_active()` 的每个消息都 `send_remote_key(wparam,
  true)`：`ui_kit_demo.cpp:9297-9300`。
- 没有检查 `lParam` 的重复位（bit 30），也没有 IME/组合状态判断。
- `WM_CHAR` 在远端输入激活时直接返回 0（`9214-9215`），所以文本输入完全依赖
  keydown 的 code；中文/日文输入法无法通过组合输入上屏。

**差异**

- 按住一个键时，远端会收到一串重复 keydown；Web 只发一次。
- 原生没有 IME 文本通道：用中文输入法在远端打字时，组合过程/候选词无法发送。
- Web 也主要依赖物理键，但浏览器会先把 IME 组合处理掉；原生需要显式处理
  `WM_IME_*` 或至少提供 `input k=text` 通道。

**实现建议**

- `WM_KEYDOWN` 检查 `lParam & (1<<30)`，重复则丢弃（仍可转发 keyup）。
- 增加 `WM_IME_STARTCOMPOSITION/COMPOSITION/ENDCOMPOSITION` 处理；组合结束时用
  `{"type":"input","k":"text","text":"..."}`（宿主已支持 `text`，
  见 `SessionView.tsx:1585-1587` 的日志特判与 `client.ts:1568-1595`）。
- 远端输入激活时不要吞掉本地 IME 消息。

---

### D4 失焦释放按键

**Web 行为**

- 维护 `pressedKeys`/`pressedButtons`；在 `blur`、`visibilitychange=hidden`、
  effect 卸载时把所有按下的键和鼠标键发 keyup/button up：
  `SessionView.tsx:2195-2235`。
- 也暴露 `__molinkReleaseKeys` 给 shell 的抓取逻辑（`2205-2209`）。

**Native 现状**

- 只维护 `remote_buttons`，`release_remote_buttons()` 释放 1..3 键：
  `ui_kit_demo.cpp:4051-4059`。
- 关闭标签、关闭控制时会调用它（`4061-4075`、`4339-4342`）。
- 没有按键集合；没有 `WM_KILLFOCUS`/`WM_ACTIVATE` 处理（代码中不存在）。
- 切换窗口后，远端可能保持 Ctrl/Shift/Alt 按下，直到用户再按一次。

**差异**

- Web 切走窗口会释放所有键；原生不会，容易出现“远端 Ctrl 卡住”。

**实现建议**

- 增加 `std::set<std::string> pressed_codes_`，在 `send_remote_key` 成功时记录。
- 处理 `WM_KILLFOCUS`、`WM_ACTIVATE(WA_INACTIVE)`、`WM_CLOSE`、控制关闭：对集合中
  每个 code 发 keyup，然后清空。
- 与 D3 的重复过滤共用同一套状态。

---

### D5 组合键转发菜单

**Web 行为**

- 固定组合：Win、Win+R、Win+E、Win+D、Win+Tab、Win+L（确认）、Alt+Tab、
  Alt+F4、Ctrl+Esc、Ctrl+Shift+Esc、Win+Shift+S、PrintScreen：
  `frontend/src/lib/key-combos.ts:25-54`。
- 工具栏菜单按 `keySequence()` 发送按下/反序释放：
  `key-combos.ts:60-65`、`SessionView.tsx:2311-2320`。
- 有副作用的组合（Win+L）先 `window.confirm`：
  `SessionView.tsx:2330-2333`。
- 首次使用会询问是否启用键盘抓取（Windows shell 目前没有抓取，仅影响提示）：
  `SessionView.tsx:2322-2354`、`3772-3820`。

**Native 现状**

- 工具菜单只有“聊天/文件/停止控制/Ctrl+Alt+Del”：
  `ui_kit_demo.cpp:812-817`、`2551-2572`。
- Ctrl+Alt+Del 发 `{"type":"input","k":"sas"}`：
  `ui_kit_demo.cpp:2569`、`4305-4308`。
- 没有 Win/Alt+Tab 等组合菜单。Windows 会在消息到达窗口前吞掉 Win 键和 Alt+Tab，
  所以用户确实需要这个菜单。

**差异**

- 原生无法发送 Win+R、Win+E、Alt+Tab、Ctrl+Esc 等被系统抢占的组合。
- 只有 SAS，没有 Win+L 这类需要确认的操作。

**实现建议**

- 增加“组合键”子菜单，条目与 `KEY_COMBOS` 一致；发送时按“修饰键按下 → 主键
  按下/释放 → 修饰键反序释放”的顺序调用 `send_remote_key`。
- Win+L/注销类操作加确认弹窗（可复用 `g.close_prompt` 的模态绘制）。
- 不需要改协议，宿主已经接受普通 key 事件。

---

### D6 绝对鼠标/滚轮/按键

**Web 行为**

- 指针位置归一化到画面矩形后发送：

  ```json
  {"type":"input","k":"move","x":0.5,"y":0.25}
  ```

  `SessionView.tsx:2055-2115`。
- 按键 1..3：`{"type":"input","k":"button","button":1,"down":true}`。
- 滚轮只发方向：`{"type":"input","k":"wheel","dx":0,"dy":1}`：
  `2117-2131`。
- 1:1 显示模式下滚轮用于本地平移，不发给远端（`2121-2123`）。

**Native 现状**

- `send_remote_move()` 按 `picture_rect` 归一化，发 `k=move`：
  `ui_kit_demo.cpp:4028-4040`。
- `send_remote_button()` 发 1..3 键：`4042-4049`。
- `send_remote_wheel()` 发 dx/dy：`4084-4090`。
- 左键按下时会先 move 再 button：`4192-4200`。

**差异**

- 主要路径已对齐。
- 原生没有 rAF 合并，每个 `WM_MOUSEMOVE` 都发一条；Web 每帧最多一条。
- 原生在 1:1 模式没有本地平移，因此不存在“滚轮不发远端”的分支；行为不冲突，但
  1:1 下滚轮仍会滚远端。

**实现建议**

- 可用 `SetTimer`/dirty flag 把 move 合并到 ~60 Hz，降低弱网下的消息量。
- 如果后续实现本地平移，再补 1:1 下的滚轮分流。

---

### D7 相对/触控板鼠标

**Web 行为**

- 有“虚拟鼠标”模式（触摸设备）：滑动移动、点按左键、长按右键、双指滚动：
  `SessionView.tsx:522`、`2057-2100`、`3690-3717`；
  locale 说明在 `frontend/src/locales/zh-CN.json:319-321`。
- 触摸输入以相对增量驱动光标，而不是绝对定位。

**Native 现状**

- 没有虚拟鼠标/相对模式；只有绝对归一化坐标。
- Windows 原生客户端主要面向桌面，触摸板由系统处理，通常不需要这个模式。

**差异**

- 在二合一/触摸 Windows 设备上，Web 的触控板模式不可用；原生同样没有。
- 对普通鼠标用户没有影响。

**实现建议**

- 低优先级。若要对齐，增加一个“触控板模式”开关：捕获触摸拖动的增量，维护一个
  本地虚拟光标坐标，按画面尺寸归一化后发 `k=move`；点按/长按映射 1/3 键。
- 桌面场景可以明确记为“本轮不做”。

---

## E. 诊断与错误

### E1 本地诊断环形缓冲、7 天、可关

**Web 行为**

- 本地 ring：最多 600 条、7 天保留、256 KiB 存储上限，字段 `t/lvl/ev/msg/data`：
  `frontend/src/lib/diag-log.ts:38-48`、`116-122`、`197-212`、`263-292`。
- 默认开启，只有显式 `'0'` 关闭；关闭后内存仍记录，只是不落盘：
  `diag-log.ts:75-105`。
- 每条日志脱敏 token/密码/长 hex/ID，消息截断 500 字：
  `diag-log.ts:124-160`。
- 未捕获错误/unhandled rejection 也会进入 ring，重复消息 5 s 内合并：
  `diag-log.ts:435-495`。
- 设置页底部有低对比“运行日志”入口：
  `frontend/src/components/SettingsPanel.tsx:803-804`、`947-966`。

**Native 现状**

- `native_log()` 只是追加写
  `%LOCALAPPDATA%\moLink\native-session.log`，每行带本地时间：
  `ui_kit_demo.cpp:1163-1191`。
- 没有级别、ring、保留天数、大小轮转、开关；文件会无限增长。
- 调用点是零散的 session/surface/core watchdog 记录，例如
  `ui_kit_demo.cpp:8649-8662`、`8449-8453`；没有统一的 info/warn/error 入口。
- 没有 UI 入口。

**差异**

- 原生没有可查看、可导出、可关闭的诊断记录；只有开发者知道路径的文本文件。
- 没有脱敏和大小上限，长期运行会一直变大。

**实现建议**

- 在 native 侧实现一个与 `diag_log` 等价的轻量类：级别、环形内存、按天文件名
  `molink-YYYYMMDD.log`、7 天清理、单文件上限、脱敏。
- 给 `session_client` 和 UI 各加一个 `native_diag(level, event, message, fields)`
  入口，替换现有 `native_log` 调用。
- 设置页增加“运行日志”入口（诊断面板）。

---

### E2 主机日志 tail/info/enable/disable

**Web 行为**

- 客户端请求：

  ```json
  {"type":"logs","action":"tail","lines":500}
  {"type":"logs","action":"info"}
  {"type":"logs","action":"enable"}   // 或 disable
  ```

  `frontend/src/lib/client.ts:1303-1342`。
- 宿主返回 `tail/size/lines` 或 `enabled/directory/retention_days/current_bytes/file`：
  `backend/src/session_controller.cpp:2268-2330`。
- 主机日志按天滚动、7 天保留、单文件 32 MiB 轮转、2000 行 ring：
  `backend/src/diag_log.h:20-60`。

**Native 现状**

- `handle_text()` 没有 `logs` 分支（`session_client.cpp:1301-1518`）。
- UI 没有主机日志入口。

**差异**

- 原生无法读取被控端的日志，也无法远程开关被控端记录；排查“对方说卡”时只能拿到
  本地一侧的信息。

**实现建议**

- `session_client` 增加异步请求接口：`request_log_tail(lines)`、
  `request_log_info()`、`set_host_logging(enabled)`；把回执存到 UI 可读状态。
- 诊断面板增加“主机日志”标签页（与本地日志并列）。

---

### E3 一键复制两端日志/导出/清空/打开目录

**Web 行为**

- “复制最近日志”会把本机 ring（默认 200 行）和主机 tail 拼成一份纯文本：
  `frontend/src/components/DiagLogPanel.tsx:143-159`；
  `diag-log.ts:411-433`。
- 导出按当前过滤条件生成文件：`DiagLogPanel.tsx:205-222`、`diag-log.ts:358-379`。
- 清空需要二次确认：`DiagLogPanel.tsx:224-231`。
- “打开日志目录”通过 shell 命令 `open-log-dir`：
  `DiagLogPanel.tsx:161-166`、`frontend/src/lib/shell.ts:118-122`；
  WebView2 壳在 `shell/windows/main.cpp:473-493` 打开 `%ProgramData%\moLink\logs`。
- 面板还有 all/warn/error 过滤、搜索、计数、导出文件名带时间：
  `DiagLogPanel.tsx:184-203`、`79-87`。

**Native 现状**

- 没有日志界面，也没有复制/导出/清空/打开目录的入口。
- `native_log()` 的目录是 `%LOCALAPPDATA%\moLink`（`ui_kit_demo.cpp:1164-1168`），
  与 WebView2 壳/core 的 `%ProgramData%\moLink\logs` 不同。

**差异**

- 原生用户无法把“本地 + 主机”日志一次性发给维护者；这是排查现场问题的主要缺失。
- 日志目录与 shell/core 不一致，容易让人看错文件。

**实现建议**

- 诊断面板提供“复制最近日志”（本地 + 主机 tail）、“导出”、“清空”、“打开目录”。
- 复制内容格式对齐 `buildCopyLogText()`：版本/时间/本地段/主机段。
- 目录直接调用 `%LOCALAPPDATA%\moLink` 或统一到 core 的日志目录；两者选一个并在
  文档中写清楚。

---

### E4 错误分级：toast vs 会话横幅

**Web 行为**

- 纯分类规则：
  - 本地功能失败（`bad_input/unknown_key/bad_clipboard/bad_file_action/file_*` 等）
    → toast，不碰会话横幅：`frontend/src/lib/session-alerts.ts:77-98`、
    `124-142`。
  - 会话级失败（`unauthorized/invalid_token/too_many_attempts/ip_banned/locked/
    access_denied/...`、握手/连接关闭）→ 横幅，并决定是否给“重新连接”：
    `session-alerts.ts:57-73`、`105-114`。
  - `clipboard_too_large/clipboard_files_*` → warn，`clipboard_files_staged` → info：
    `session-alerts.ts:39-53`。
- toast 6 s、同文刷新同一条、按 tab 存：`session-alerts.ts:16-27`、`146-176`。
- 输入错误绝不升级为会话错误：`frontend/src/lib/client.ts:2166-2205`。
- 渲染：横幅 `SessionView.tsx:2484-2573`；toast 组件 `SessionView.tsx:385-431`。

**Native 现状**

- `session_client` 对任何 `{"type":"error"}` 都写 `last_error_/last_error_code_`
  （`session_client.cpp:1451-1464`），不区分通道。
- UI 每秒检查 `last_error()`，设置 `connect_error` 并弹 toast
  （`ui_kit_demo.cpp:8471-8486`）。
- 横幅只在“未认证 + connect_error 非空”时绘制（`ui_kit_demo.cpp:5809-5812`）；
  已认证时错误只会变成 toast。
- toast 单条、2.5 s、无级别/颜色/堆叠：`ui_kit_demo.cpp:2487-2490`、
  `4489-4493`。
- 输入/文件错误在未认证阶段会被当成会话横幅；已认证阶段不会。

**差异**

- 没有按错误码分级：`bad_input`、`unknown_key`、`bad_file_action` 等与真正的
  会话错误走同一字段。
- 没有 warn/info tone，没有 6 s 生命周期，没有同文刷新。
- 错误码 `display_mode_busy` 等已有中文映射（`ui_kit_demo.cpp:2380-2392`），但没有
  “可重连性”判断。

**实现建议**

- 移植 `classifyError()` 到 native（纯函数，跨平台可单测）；`session_client` 增加
  `last_error_code` 的通道分类，或 UI 侧直接按 code 分类。
- `session_client` 把“输入/文件”错误与“会话”错误分开存储；不要再让
  `bad_input` 覆盖会话错误。
- toast 增加 tone 与 6 s 生命周期；同一消息刷新同一条。
- 横幅保留“需要重连”与“只需忽略”两种按钮（见 E6）。

---

### E5 重连策略与断线横幅/倒计时/停止

**Web 行为**

- 连接关闭后：立即重试 1 次，然后 1 s 两次；都失败则进入 `disconnected`，60 s 后
  再试：`frontend/src/lib/client.ts:1214-1246`。
- `disconnected` 时 UI 显示倒计时，并提供“立即重连/停止重试”：
  `SessionView.tsx:780-795`、`2830-2860`。
- 每次成功 welcome 重置 `retryAttempts`：`client.ts:1845`。

**Native 现状**

- 会话掉线后 `reconnecting=true`，重置 `retry_attempt=0`，立即重连：
  `ui_kit_demo.cpp:8658-8682`。
- 重连失败：前两次 1 s，之后 10 s，无限循环：
  `ui_kit_demo.cpp:9602-9625`。
- 画面中央显示固定文案“连接已断开，正在重连…”，没有倒计时/停止/立即重连：
  `ui_kit_demo.cpp:5796-5808`。
- 已认证会话掉线会自动重连；策略错误（`last_error` 非空）不重连：
  `ui_kit_demo.cpp:8643-8662`。

**差异**

- 原生重连更频繁（10 s vs 60 s），长时间断网会持续打扰；Web 有倒计时和停止。
- 原生没有“立即重连”按钮；用户只能等下一次定时器。
- 原生不区分“已停止重连”和“正在重连”的 UI 状态。

**实现建议**

- 对齐 Web 的 1 次立即 + 2 次 1 s + 60 s 周期；提供“立即重连”和“停止重试”。
- 断线遮罩显示倒计时；停止后显示“已停止”，错误横幅保留“重新连接”按钮。
- 保留“已认证才自动重连、策略错误不重连”的现有判断。

---

### E6 主机错误的可执行动作

**Web 行为**

- 横幅按错误码给动作：
  - 验证码错误 → 内联输入框 + 连接按钮：`SessionView.tsx:2510-2535`；
  - 可重连错误 → “重新连接”：`2536-2544`；
  - 不可重连 → “忽略”：`2545-2553`；
  - 显示模式错误 → “忽略”：`2554-2562`；
  - token/unauth 类错误 → “请求访问”：`2563-2572`。
- 重连可用性由 `needsReconnect()` 决定：`session-alerts.ts:105-114`。

**Native 现状**

- 横幅对 `invalid_code/unauthorized` 显示内联验证码框和按钮：
  `ui_kit_demo.cpp:5828-5880`；点击处理
  `ui_kit_demo.cpp:2422-2441`、`2400-2419`。
- 其它错误只显示“重连”按钮（`5841-5844`），没有“忽略”；没有“请求访问”。
- 横幅只在未认证时显示，已认证会话的错误没有横幅动作。

**差异**

- 原生无法忽略一个不可重连的错误（例如显示模式失败），横幅会一直占着底部空间直到
  重连或关闭会话。
- 没有从错误横幅直接发起“请求访问”的入口。

**实现建议**

- 给 `connect_error` 增加 `dismissible/reconnectable` 标志，渲染“忽略”按钮。
- 对 token/unauth 类错误增加“请求访问”动作（复用 `start_connect` 的空 code 流程，
  `ui_kit_demo.cpp:2024`）。

---

## U. 界面细节

### U1 水印按区域接近淡化

**Web 行为**

- 三个独立目标：左上 badges、右下 stats、中央 connecting；指针进入目标矩形外扩
  28 px 时淡化，离开 80 ms 后恢复，透明度降到 0.08：
  `frontend/src/lib/proximity-fade.ts:17-22`、`55-67`；
  `frontend/src/lib/use-proximity-fade.ts:59-193`。
- 由会话画面的 `pointermove` 驱动，rAF 合并：
  `SessionView.tsx:619-625`、`2093-2097`。
- 默认开启，设置可关：`frontend/src/store/app.ts:211-213`、`862`；
  设置项 `PreferencesControls.tsx:758-767`。

**Native 现状**

- `draw_watermark()` 一次性绘制左上 4 个 chip 和右下 stats bar，固定不透明：
  `ui_kit_demo.cpp:3572-3666`。
- 已有 `g.hover_x/g.hover_y`（`ui_kit_demo.cpp:187`），但水印绘制没有使用。
- 设置里只有水印开关，没有“接近淡化”开关：
  `ui_kit_demo.cpp:6698-6712`。

**差异**

- 原生水印会一直遮挡画面角落；Web 在鼠标靠近时才显示，移开即淡出。

**实现建议**

- 在 `draw_watermark()` 里分别计算左上 chip 区、右下 stats 区的矩形，用
  `inside(rect expanded by 28, hover_x, hover_y)` 决定 alpha；用 `SetTimer` 实现
  80 ms 恢复延迟。
- 增加“水印淡化”设置，默认开启，保存到注册表。
- 绘制时把 `dark_chip`/stats bar 的颜色 alpha 乘以淡出系数。

---

### U2 水印内容完整度

**Web 行为**

- 左上 badges：分辨率、当前帧 codec、编码档位、链路/协议/HTTP 版本、采集后端+版本、
  合成画面标记、“控制中”：`SessionView.tsx:2717-2764`。
- 右下 stats：fps、延迟、带宽、编码、path/protocol/http：
  `SessionView.tsx:2766-2785`。
- codec 取当前显示帧（`tab.frame.codec`），不是配置值：
  `SessionView.tsx:2417-2425`。

**Native 现状**

- 左上：分辨率、配置 codec、预设名、transport：
  `ui_kit_demo.cpp:3593-3604`。
- 右下：fps、延迟、速率、quality label、live codec、path/protocol：
  `ui_kit_demo.cpp:3627-3639`。
- 没有采集后端、版本、控制中、合成画面、HTTP 版本独立 chip；左上 codec 是配置值，
  右上 live codec 是实际帧 codec（`session_client.cpp:1627-1636`）。

**差异**

- 原生缺少“采集后端/版本”“控制中”“合成画面”等信息；排查环境差异时少一条线索。
- 左上和右下的编码含义不同（配置 vs 实际），但 UI 没有说明，容易误读。
- Http 版本只出现在 protocol 字符串里，没有 Web 的独立 `HTTP/2`/`HTTP/3` 标签。

**实现建议**

- `welcome` 增加 `version`、`tiles.backend`、`displays` 解析；保存到 `app_state`。
- 左上 chip 改为“当前帧 codec”（与 Web 一致），实际编码器放右下；增加“控制中”chip。
- transport chip 拆出 HTTP 版本（`protocol_label()` 已有 http2/http3，
  `ui_kit_demo.cpp:3718-3728`）。

---

### U3 悬浮工具栏拖拽/固定/收起/全屏/显示模式

**Web 行为**

- 40×40 浮动球，点击展开快速栏；快速栏顶部居中、可拖拽：
  `SessionView.tsx:2862-2923`、`1720-1771`。
- pin 状态存 `molink.toolbarPinned`：`SessionView.tsx:177-181`、`2877-2892`。
- 收起、全屏、fit/1:1/fill 三种显示模式：`2954-2976`。
- 窄宽度时按钮组折叠进菜单：`1052-1075`。

**Native 现状**

- 球 + 条、拖拽（球和整条）、pin、收起、全屏、三种显示模式：
  `ui_kit_demo.cpp:3668-3698`、`3731-4018`、`4203-4212`、`4365-4375`。
- pin 存 `HKCU\Software\moLink\Ui\ToolbarPinned`：`ui_kit_demo.cpp:2887-2895`、
  `3015-3025`。
- 窄宽度按 tier 降级：`ui_kit_demo.cpp:709-775`。

**差异**

- 已对齐；原生甚至允许拖动整条 bar（Web 只拖球），属于超集。
- 折叠策略不同（原生 tier 降级 vs Web 组菜单），但用户可完成同样操作。

**实现建议**

- 保持现状。若追求视觉完全一致，可把 tier 降级换成 Web 的分组菜单；成本不低，
  收益主要是外观。

---

### U4 主题深浅

**Web 行为**

- 只有 `light`/`dark`：`frontend/src/store/app.ts:90`。
- 读取顺序 localStorage/cookie，写入 localStorage：
  `store/app.ts:397`、`474`、`858`、`914-922`。
- 设置面板两段式切换：`PreferencesControls.tsx:631-660`。

**Native 现状**

- `automatic/light/dark` 三种模式，默认 automatic：
  `ui_kit_demo.cpp:71-73`、`2601-2615`。
- 持久化到 `HKCU\Software\moLink\Ui\ThemeMode`：
  `ui_kit_demo.cpp:2896-2917`。
- 设置面板两段式（亮/暗），头部方形按钮循环三态：
  `ui_kit_demo.cpp:6509-6515`、`8850-8860`。

**差异**

- 原生是超集：多了“跟随系统”，且持久化方式不同。Web 没有 automatic。
- 对用户没有负面差异；如果要求严格一致，需要决定是否隐藏 automatic。

**实现建议**

- 保持 automatic（它是 Windows 桌面客户端的合理能力），在文档/设置提示中说明。
- 若必须和 Web 完全一致，把设置里的 automatic 隐藏，只保留 light/dark；头部循环也
  改成两态。

---

### U5 语言切换

**Web 行为**

- i18next 资源 `en` / `zh-CN`，初始语言由 localStorage 或 `navigator.language` 决定：
  `frontend/src/i18n.ts:6-34`。
- 切换即时调用 `changeLanguage` 并写 localStorage：
  `i18n.ts:52-60`。
- 同时把语言镜像到 core 的 `ui.json`，供原生壳托盘使用：
  `i18n.ts:36-50`。
- 设置项：`PreferencesControls.tsx:690-700`。

**Native 现状**

- `settings_lang = 1`（中文）写在 `app_state`：`ui_kit_demo.cpp:221`。
- 设置面板有 English/中文两段：`ui_kit_demo.cpp:6530-6535`。
- 点击只改内存并弹“语言设置已保存（重启后生效）”：
  `ui_kit_demo.cpp:7001-7005`。
- 没有加载/保存语言，也没有任何字符串表；所有文案硬编码中文。
- 托盘菜单同样硬编码中文：`ui_kit_demo.cpp:3002-3010`。

**差异**

- 原生切换语言完全不生效，重启后丢失；提示文案本身也是假的。
- WebView2 壳会读取 `ui.json` 的 language 来本地化托盘
  （`shell/windows/main.cpp:251-260`、`974-984`），原生忽略了它。

**实现建议**

- 短期：至少把 `settings_lang` 持久化到 `ui.json`/注册表，并让托盘菜单读取它；
  这能消除“提示保存但没保存”的错误反馈。
- 中期：建立字符串表（中/英），把 `ui_kit_demo.cpp` 的硬编码文案批量替换；工作量大，
  但与 Web 的 i18n 对齐必须做。
- 如果本轮不打算做完整 i18n，建议暂时隐藏语言开关，避免误导。

---

### U6 通知/toast 行为

**Web 行为**

- toast 生命周期 6 s，tone 为 info/warn/error，颜色与图标不同：
  `frontend/src/lib/session-alerts.ts:14-27`、`16-24`；
  `SessionView.tsx:385-431`。
- 同一文本在 TTL 内刷新同一条，不会堆叠；新消息替换旧消息：
  `session-alerts.ts:146-176`。
- 更新提示是独立组件/状态：`frontend/src/components/UpdateNotice.tsx`。

**Native 现状**

- 单条 toast，2.5 s，无 tone、无图标、无同文刷新：
  `ui_kit_demo.cpp:2487-2490`、`4489-4493`。
- 文件/连接/剪贴板消息共用同一 toast 槽；新消息直接覆盖旧消息（`show_toast`）。
- 更新检查结果也走 toast（`ui_kit_demo.cpp:9660-9675`）。

**差异**

- 原生提示消失更快（2.5 s vs 6 s），长错误容易读不完。
- 没有 warn/error 色彩，用户无法从视觉上区分“提示”和“失败”。
- 同时发生多个事件时，前面的提示会被覆盖。

**实现建议**

- toast 结构增加 `tone`、`at`、`id`；同一文本刷新时间；生命周期 6 s。
- 错误/警告使用 danger/warn 颜色（现有 `draw_toast` 可以按 tone 选色）。
- 可选：保留一个短队列（2-3 条）或把重要错误留在横幅而不是 toast。

---

### U7 托盘行为与语言/关闭行为

**Web（WebView2 壳）行为**

- 关闭窗口时隐藏到托盘，不退出：`shell/windows/main.cpp:1075-1082`。
- 托盘双击/左键显示窗口，右键菜单“显示/退出”：`main.cpp:1063-1073`、
  `974-984`。
- 托盘语言从 core 的 `ui.json` 读取，缺省跟随 Windows UI 语言：
  `main.cpp:251-260`。
- 页面自身有 NoticePanel/更新提示：`frontend/src/components/simple/NoticePanel.tsx`。

**Native 现状**

- 托盘图标添加/删除、点击显示、右键菜单：`ui_kit_demo.cpp:2947-3010`。
- 关闭行为三态：0 询问、1 最小化到托盘、2 关闭会话 tab；持久化在
  `HKCU\Software\moLink\Ui\WindowCloseBehavior`：
  `ui_kit_demo.cpp:2919-2941`、`8212-8227`。
- 关闭提示面板：`ui_kit_demo.cpp:7670-7760`。
- 托盘菜单固定中文：`ui_kit_demo.cpp:3002-3010`。

**差异**

- 关闭行为：WebView2 壳总是隐藏到托盘；原生默认询问，用户可选。原生更灵活，但默认
  行为不同。
- 无会话标签时，原生 `WM_CLOSE` 直接销毁窗口（`8213-8216`），WebView2 壳仍隐藏到
  托盘（除非 URL 模式）。
- 托盘语言：原生不跟随语言设置/`ui.json`，WebView2 壳会。
- 原生托盘菜单没有“显示”以外的会话状态（Web 壳也没有，差异小）。

**实现建议**

- 托盘菜单从 `ui.json`/注册表读取语言，至少支持中/英；与 U5 的持久化一起做。
- 决定关闭默认值：若要严格对齐 Web，首次关闭就隐藏到托盘；保留提示作为可选设置。
- 无会话时也保持托盘存活，避免用户误以为“关掉会话就退出”。

---

## 分批实现建议

排序依据：用户可感知程度高、依赖少、能尽快让 native 与 Web 使用同一套会话语义的优先。
每批 1-3 项。成本为相对估计（S < M < L）。

| 批次 | 条目 | 用户可感知 | 成本 | 本机（Linux）验证 | 说明 |
|---|---|---|---|---|---|
| 1 | A1、A2、A4、A10 | 极高：画质档位、宿主实际值 | M | 可验证协议层 | 先把 `session-preset` 全字段、六档表和 `applied` 回执做对；宿主回执可用 Linux core + 脚本断言 JSON。Native 编译需 Windows CI/release 工作流。 |
| 2 | A5、A6、A7、A3 | 高：首帧档位、切档刷新、高级画质 | M | 部分 | 预设先于首帧、按 host 记忆、防抖和 chroma/codec 控件。协议层可本机验证；UI 只能在 Windows 看。 |
| 3 | B2、B1 的 hash/chain、C5 | 高：本机复制自动同步 | M | 协议层可验证 | 剪贴板监听 + `clipboard-sync` 出站 + echo 表；这是 Web 与 native 体感差距最大的一项。 |
| 4 | D3、D4、D2、D5 | 高：输入正确性/不卡键 | M | 纯逻辑可单测 | 重复键、失焦释放、小键盘/NumLock、组合键菜单。建议把键码映射抽成跨平台头文件，Linux 单测表。 |
| 5 | F3、F4、F7、F8 | 高：文件面板可用性 | M | 协议层可验证 | 远程路径输入/roots/面包屑、多选、任务汇总、上限提示。 |
| 6 | F5、F6、F9 | 高：文件夹传输 | L | 协议层可验证 | 递归上传/下载、mkdir、dest、拖入文件夹展开。这是文件面板工作量最大的一批。 |
| 7 | F1、F2、F10 与 C1–C4 | 高：双栏与剪贴板文件 | L | 部分 | 本地栏可用 Win32 或 `/api/fs/list`；剪贴板文件涉及 0x18/0x17 二进制包和落盘。 |
| 8 | E1、E2、E3、E4、E5、E6 | 中高：可诊断、可恢复 | M | 协议层可验证 | 本地 ring/7 天/开关、主机日志、复制两端、错误分级、重连倒计时。 |
| 9 | U1、U2、U5、U7 | 中：视觉与语言 | M | 仅代码审查 | 水印淡化/内容、语言持久化+托盘、关闭行为。语言完整 i18n 成本最高，可拆成“先持久化托盘”与“再全量字符串表”。 |
| 10 | A8、A9、D7、F10 的触控部分 | 中低：自适应/多屏/触控 | L | 纯逻辑可单测 | auto 阶梯、帧率控制器、显示器选择、触控板模式；依赖前几批的 preset/stats 基础。 |

### 批次依赖关系

- 批次 2、10 依赖批次 1 的 `session-preset` 与 `applied`。
- 批次 3 的 echo 表依赖 S2（文本出站）；C1/C2 依赖 C5 的剪贴板文件路径。
- 批次 7 的剪贴板文件依赖批次 6 的落盘/目录逻辑。
- 批次 9 的语言完整实现可以与任何批次并行，但“托盘先本地化”建议和 U5 同批。

### 本机验证边界

- **可以在 Linux 本机验证**：
  - `frontend`：`npm test`（`frontend/src/lib/*.test.mjs` 与 `frontend/tests/*.test.mjs`）、
    `npm run typecheck`、`npm run build`。
  - `backend`：`cmake -S backend -B backend/build -DMOLINK_BUILD_CORE=OFF` +
    `ctest`；协议层与 `session_preset`/`diag_log`/`file_transfer` 的单元测试都在
    这里。
  - 起一个 Linux core，用 WebSocket 脚本或 `uvp` 工具发送/断言
    `session-preset`、`quality`、`clipboard-sync`、`logs`、`file` 消息。
- **不能在 Linux 本机验证**：
  - `shell/windows/ui_kit_demo.cpp` 与 `session_client.cpp` 使用 Win32/Direct2D，
    无法在 Linux 编译或运行。
  - 原生代码的编译检查目前只在 `release-desktop` 工作流（MinGW）和
    `packaging/windows/build-win7-x86.ps1` 中进行；PR 的 `ci.yml` 只有
    `windows-shell` 构建 WebView2 壳，没有构建 native。
  - 建议（下一轮）把 native 的编译加入 Windows CI，至少 `-fsyntax-only` 或完整
    MinGW 构建，否则原生改动只能靠发布前发现。
- **建议的跨平台拆分**：把 A1/A2/A3 的预设解析、A8 的控制器、D1 的键码表、
  E4 的错误分类抽成不依赖 Windows 的头文件，这样 Linux 单测可以覆盖大部分“逻辑
  正确性”，Windows 只验证绘制和消息泵。

---

## 附：本次审计未覆盖但建议记录的差异

以下不属于六个域的核心，但实现时会被顺带遇到，先记录以免遗漏：

- 原生只处理一种会话（`g.session` 单对象），Web 支持多标签会话；剪贴板“前台会话”
  判定在原生可以省略，但多标签一旦实现就需要补。
- 原生 `ws_client` 只走 WebSocket；Web 还支持 WebTransport/HTTP2/HTTP3。原生在
  `welcome` 里没有 `protocol`、没有 `webtransport` 处理，水印的 HTTP 版本只来自
  `transport.protocol`（`session_client.cpp:1426-1434`）。
- 原生的 surface 模式把解码交给本地 core；tile 模式和 stream 模式的“实际 codec”
  来源不同（`session_client.cpp:1627-1636`），做 U2 时要区分。
- 原生文件传输的下载目录目前只能通过 `set_download_dir` 内部设置，UI 没有入口
  （`ui_kit_demo.cpp:419`、`6098`）；Web 有保存对话框/目录句柄。
