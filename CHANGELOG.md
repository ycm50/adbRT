# 更新记录

格式参考 Keep a Changelog；版本号以 `packaging/identity.json` 为准。

## [1.0.0.0] — 首个可用版本

### 新增

- **声明式命令表**：52 条 adb / fastboot 命令，全部是数据表项；
  命令树、参数面板、CLI 共用同一份定义，改命令不需要动 UI 代码。
- **argv 只有一个来源**：`BuildCommand` 是唯一产出 argv 的入口，
  自动注入 `-s <serial>`，白名单校验 flags，永不拼接 shell 字符串。
- **主界面**：顶栏（设备下拉 / 刷新 / Fastboot 专家开关 / 设置 / 关于）+
  左命令树 + 右参数卡片；危险级别徽标；搜索过滤；`F5` 刷新、`Ctrl+F` 搜索、回车执行。
- **参数面板**：中文标签；按参数类型渲染下拉框（重启目标 / 槽位 / 分区 / 推送目标 /
  拉取目录）、相对路径输入、应用复选、属性复选、本机文件复选；
  执行前常驻的**真实命令行预览**（深色代码块，可复制）。
- **三段式 shell**：命令下拉 → 参数复选（可填值）→ 管道；`增加管道段` 追加一段，
  `删除最后一段` 连同它的管道符一起删。
- **截图取点**：点击 / 滑动都在手机截图上用鼠标完成，坐标为设备真实像素。
- **流式执行**：逐行回传、可取消（Job object 连子进程树一起结束）、
  `push`/`pull` 进度百分比解析。
- **内置窗口**：设备列表、设备信息（异步加载）、已安装应用（含导出 APK）、
  截屏、截图取点、实时日志（过滤/暂停/保存）、交互终端。
- **危险操作确认**：破坏性操作二次确认；fastboot 刷写/擦除/解锁需**手打设备序列号**。
- **设置**：路径可编辑/浏览，支持绝对、相对程序目录、`%环境变量%`、`~` 四种写法；
  用户 PATH 与桌面快捷方式开关。
- **安装/卸载**：**不写注册表**，只把安装目录加进用户 PATH 并建桌面快捷方式；
  支持 `-InstallDir`（相对或绝对）、`-NoPath`、`-NoDesktop`、`-DryRun`。
- **CLI 门面**：`--run` / `--list-commands` / `--dry-run` / `--out`，与 GUI 共用构造器。
- **图标**：矢量母版 `adbrt.svg`，同一份几何定义光栅化出 PNG / ICO（无第三方库）。
- **测试**：174 项单元测试（无设备）、31 项 GUI 自检、26 项真机全量、安装卸载端到端。
- **CI**：手动触发的 build（Debug + Release 矩阵）→ package → release；
  **只有构建全部成功才建 tag**，tag 号由人手动输入。

### 安全

- 坐标类参数（`input.tap` / `input.swipe`）走专用的 `ParamKind::Coords`：
  只允许数字与空格，`;` / `&&` / `$()` 在构造期即被拒绝。
- 所有 adb / fastboot 调用都直接 `CreateProcessW`，不经过 `cmd.exe`。
- 凭据不落盘：adb 私钥与无线调试配对码均不写入配置或日志。

### 已知限制

- `app.start` 走 `monkey`，stderr 会混入 `bash arg:` 调试行（退出码正常）。
- `screenrecord` 在 1272×2772 上会先编码失败再回退 720×1280。
- 参数面板暂不支持滚动，参数极多的命令会被裁切。

## 未发布

### 修复

- **无线调试的端口不再是「永远 5555」**（本次修的核心问题）：无线设备被扫成
  `IP:5555` 是三处叠加 —— 选择器拿不到端口时直接编一个 5555；扫描只探 5555 一个端口；
  端口发现只有 `adb mdns services` 一条路，而 adb ≥31 对**没连过的设备恒返回空表**
  （实测 36.0.2：`mdns check` 正常，`mdns services` 恒空）。而 Android 11+
  「无线调试」的端口是随机高位端口，每次重开手机端都会变。
  - 新增 `core/adb_mdns`：自己发 DNS-SD 组播查询（`_adb-tls-connect._tcp`、
    `_adb-tls-pairing._tcp`、`_adb._tcp`），PTR→SRV→A 串表拿到 `IP:端口`；
    与 `adb mdns services` 并行各问一次、去重合并；5353 被占用时自动改绑临时端口
    并用 QU 位请求单播应答。配对端口单独标记，`adb connect` 会跳过它。
  - 局域网扫描从「只探 5555」改成「探一组端口」（默认 5555/37221/5556/5557/5037 +
    mDNS 报出的端口），每台主机记录**实测开着的端口** `ScanHost::adbPorts`，
    列表里直接显示 `adb 端口 37221` 而不是永恒的「端口未开」。
  - `FindAdbEndpoint` 改成四级（devices 快路径 → mDNS → 扫描实测端口 →
    **用户显式勾选**的深度扫描），并删掉结尾的 5555 硬默认：
    找不到端口就明确失败，不再编一个让「已连接」随后条条命令超时。
  - 选择器：一打开就先跑 mDNS（不用扫全网段就能看到带端口的设备）；
    行数据改为存完整 `IP:端口`；没找到端口时窗口不关、不猜，提示重新打开无线调试 /
    勾深度扫描 / 手填，并新增「深度扫描端口段（慢）」复选框（默认关）。
- **后台线程与窗口的生命周期**：应用名称解析、文本窗口加载、实时日志三处
  都是"窗口销毁时 delete 上下文，线程还在里面跑"，属于标准的 use-after-free。
  改成线程只持有 `shared_ptr` 的共享状态 + 窗口数值句柄，窗口关闭时置取消位，
  线程后续的 `PostMessage` 因窗口不存在而失败，不再解引用已释放对象。
- **adb 管道句柄泄漏**：两次 `CreatePipe` 只要有一处失败，前面建好的句柄就漏了。
- **读线程与句柄的争用**：进程超时未退出时主线程仍会关管道并返回，
  读线程还在 `ReadFile`。改成读线程自己关句柄、自己置 finished，
  主线程最多等 1 秒取走数据，超时也不碰已释放内存。
- **配置与设备会话的数据竞争**：`Settings` 的 `std::map` 与
  `DeviceSession` 的序列号被 UI 线程和后台线程同时读写，补锁；
  `ListAdb()` 起进程时不持锁。
- **GDI 对象泄漏**：`CreateUiFont` 每次开窗都新建字体且从不释放（改为按字号/字体族缓存）；
  窗口类重复注册时每次都白建一个画刷（改为先 `GetClassInfoExW` 探测，画刷走缓存）。
- `-Werror` 下清理未使用的 `CollectNeighbors`。
- **「选择文件…」选完不回填**（安装 APK 选包、刷写分区选镜像、bugreport 选目录都中招）：
  参数面板重写时把"回填目标"统一成一个共享句柄 `g_panel.summary`，但只有
  本机文件 / 属性 / shell / 坐标 / 局域网这几个分支给它赋了值，APK / 镜像 / 目录
  走的 `default` 分支根本没赋。于是 `AskFile` 明明返回了路径（对话框正常关闭、
  `OFN_FILEMUSTEXIST` 校验也生效），`SetTextOf(nullptr, path)` 却是个空操作，
  值被静默丢掉，预览一直停在「（缺少参数：apk）」。
  - 改成由 `BuildParamBlock` 在**创建控件的地方**把该行输入框句柄经
    `editHwnd` 出参带出去（`g_paramEdit[0]` / `g_paramEdit[1]`），
    `g_panel.summary` 拆成只服务本机文件的 `g_filesSummary`，语义不再混用。
  - `BrowseForParam` 的回填统一走 `setEdit`：句柄为空时写 WARN 日志并在状态栏
    报错，不再静默丢弃。
  - 「扫描局域网…」原来单独写的 `g_panel.summary` 也并进 `setEdit`。

### 变更

- **安装脚本默认位置**：`install.ps1` 不指定 `-InstallDir` 时，
  改为装到**当前工作目录所在盘的根目录下的 `adbRT` 文件夹**
  （在 `A:\Downloads\adbRT` 下执行 → `A:\adbRT`）。
  之前用的是"脚本所在盘"，但发布包常被解压到别的盘（如 `D:\下载\...`），
  会装到用户预期之外的位置。显式传 `-InstallDir` 的行为不变。
- **断开设备**：原「断开无线设备…」改为「断开设备…」，参数从手填 IP:端口
  换成 `adb devices` 的下拉框（新增 `ParamKind::DeviceSerial`），
  旁边有「刷新设备…」重新取一次列表。

### 修复

- **CI 构建缓存形同虚设**：workflow 里装了 `ccache`、也配了
  `actions/cache`，但从没把 `ccache` 挂成 `CMAKE_CXX_COMPILER_LAUNCHER`，
  缓存目录永远是空的，ccache 命中率恒为 **0%**。现在：
  - 配置步骤显式传 `-DCMAKE_CXX_COMPILER_LAUNCHER=<ccache>`
  - 缓存目录改为按运行时用户解析（runner 用户名在不同版本间变过，
    写死 `runneradmin` 会导致永不命中）
  - 构建末尾加「ccache 统计」步骤，命中率直接出现在日志里
- **CI 用错工具链**：`setup-msys2` 用了 `path-type: inherit`，
  runner 预装的 `C:\mingw64\bin` 排在前面，实际编译用的是那套
  （实测日志：`g++ : C:\mingw64\bin\g++.exe`），与 UCRT64 的
  ccache、运行库都不是一套。改为 `path-type: strict`，并把编译器
  路径固定为 `C:\msys64\ucrt64\bin`，不再靠 PATH 解析。
- **CI 并行度不可见**：构建步骤显式打印并传入 `--parallel`（逻辑核数），
  便于从日志核对实际并行度。
- **package 阶段缺工具链**：该阶段要用 `objdump` 收集运行时 DLL，
  却从没装过 binutils，原来靠 `Get-Command g++.exe` 撞运气。
  现在显式安装 binutils 并固定 `toolBin` 路径。
- **action 版本过旧**：`checkout@v4` / `cache@v4` / `upload-artifact@v4` /
  `download-artifact@v4` 已触发 Node 20 弃用告警，升级到 v7 / v6 / v7 / v8。

### 移除

- **去掉「读取应用名称」功能**。原因：Android 的 `dumpsys package` / `pm` 都
  **不输出** `android:label`，设备侧也没有 `aapt`，要拿到名称只能把 APK 拉回本地
  再解析资源表。实测一个包 89~166 MB，`adb pull` 单包 **4.4 秒**，488 个包要跑
  半小时以上，还要反复往磁盘写几个 GB。代价远大于收益。
  - 应用选择器保留，改为只显示 **包名 + 类型**（列由三列变两列）
  - 删除 `core/apk_res.{h,cpp}`（AXML/ARSC 解析）、`core/label_cache.{h,cpp}`
    （名称缓存）、`core/inflate.{h,cpp}`（仅为解压 manifest 而存在）
  - 删除 `%LOCALAPPDATA%\AdbRT\labels` 缓存与 `Downloads\AdbRT-tmp` 临时目录逻辑
  - 选择器打开速度从"边加载边后台解析几十秒"变成 **两次 adb 调用、秒开**

### 性能

- **修掉"每条命令都重新启动 adb 服务"**（卡顿最直接的来源）：
  `RunProcess` 会给子进程挂一个 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job，
  用来在取消时连整棵子进程树一起结束。但 adb 客户端发现 server 没跑时，
  会 fork 出 `adb -L tcp:5037 fork-server server` 并让它常驻 ——
  这个 server 继承了同一个 Job，于是调用一返回、Job 句柄一关，
  server 就被连坐杀掉，下一条命令又得重新 fork/启动（每次多 0.5~1.5 秒）。

  实测（关闭 Job 后观察 fork 出的常驻进程）：不挂 Job → 存活；
  挂 `KILL_ON_JOB_CLOSE` → 被杀。

  修法：新增 `IsPersistentServerTool`，对 `adb.exe` 不挂 Job，
  取消时退化成只结束直接子进程，server 作为共享资源保留。
  验证：连续调用前后 server pid 不变（复用），不再反复重启。

- **输出内存策略统一收口**（"越用越卡"的主因）：
  - **进程输出捕获有上限**：`RunProcess` 原来把整份 stdout/stderr 无限 append 进
    `std::string`，跑一条 `logcat` 或 `ls -R` 就能吃掉几百 MB。现在单行超过 8 KiB
    截断、每条流超过 2 MiB 丢最旧的一半，并记账丢弃字节数（`RunResult::droppedBytes`）。
  - **读线程不再 O(n²)**：管道解析从"切一段 erase 一段"改成游标扫描；
    无换行的超长输出攒到 8 KiB 就强制切行，`pending` 不会再跟着进程无限长。
  - **新增 `BoundedText`**（`core/bounded_text.{h,cpp}`）：按字节预算的丢旧留新缓冲，
    纯标准库实现，直接进单元测试。
  - **运行结果改成批量投递**：原来是"每输出一行就 PostMessage 一次 + EM_REPLACESEL
    + 全选滚动"，输出越多越慢；现在工作线程只写有界缓冲，主线程 100ms 批量取走、
    一次追加，结果框保留上限 8 MiB。
  - **`ScrollToEnd` 不再全选控件**：原实现用 `EM_SETSEL 0,-1`，等于每追加一条就把
    整个结果框全选一遍，文本越长越慢。另外新增 `FollowTailIfAtBottom`：
    用户往上翻看历史时不再被强行拽回底部。
  - **实时日志改增量渲染**：原来是每 500ms 用 `SetWindowTextW` 重建全部文本；
    现在记住已渲染位置，只贴新增行，行数据本身是 4 MiB 字节预算的 `BoundedText`。
- **修复 EDIT 控件 30000 字符上限导致输出被吞**：Windows 的 EDIT 默认只接受
  30000 个字符，超出部分**静默丢弃** —— 这是"跑完命令结果框里只有一小段"的真正原因。
  新增 `RelaxEditLimit`，在运行结果、命令行预览、实时日志、文本窗口创建后统一放宽。
- **修复超预算裁剪算错**：`TrimEditToBudget` 原来只削"预算的 1/4"，
  文本远超预算时永远回不到预算内；改为直接裁到预算的 3/4。
- **修复收尾漏刷**：进程结束得比 100ms 刷新快时，剩余输出留在缓冲里被丢掉；
  现在 `FlushOutput` 在一帧内循环取空积压（单帧上限 8 MiB）。
- **列表类开销**：应用选择器的 `std::map` 换成 `unordered_map`；过滤改为预计算
  小写索引串（原来每敲一个字就对每行做两次 `ToLowerAscii` 分配）；
  行号查询从线性扫描改反查表；系统应用判定从 O(n²) 改成哈希集合。
- 文本窗口不再把正文存两份（EDIT 里那份就是权威副本）。
- **局域网扫描重写**，三层并发 + 二层优先：
  - TCP 批量探测 adb 端口：254 个非阻塞 `connect` 一次发出，一个 `select` 收完；
  - **ARP 蜂群**：对全段各发一个 UDP 包触发内核 ARP，等 150ms 直接读邻居表
    —— 不受手机对 ICMP 降优先级影响（实测手机 ping 要 125ms 才回，
    按 50ms 阈值会被误判为不在局域网）；
  - ICMP 兜底，按待探测数量开线程（上限 254），一轮打完。
  实测 192.168.1.0/24：**2.0 秒 → 344~563 毫秒**，除了原先漏掉的手机也能扫到。
- 时延阈值统一为 **50ms**：超过这个数才回来的响应不算"在同一局域网"。
- 端口段扫描改为批量 `select`（每批 2048），2 万个端口从 40 轮降到 10 轮；
  端口探测超时与"局域网时延阈值"解耦（固定 250ms）。
- 找 adb 端口改为四级：`adb devices` 快路径 → mDNS → 5555 → 端口段扫描，
  每个候选都 `connect` 后用 `adb devices` 复核状态
  （原来匹配 `connected to` 字样，设备已连着时返回 `already connected to` 会误判）。
  实测从 3313ms 降到 15ms。
- 网卡列表排序：真网卡（WLAN / 以太网）在前，WSL / VMware / Hyper-V 等虚拟网卡在后。

- 深色主题（调色板已集中在 `theme.h`）
- 自绘按钮（跟随主题色）
- AI 助手：自然语言 → 命令 key + 参数 → 白名单校验