# htop_plus 使用说明

源码来自 [htop 官方仓库](https://github.com/htop-dev/htop)。
下载时间为 2026-10-18，上游版本为 `3.6.0-dev`，提交为 `05c0af5ef82bd4729ab1a22666413689a04bfc2d`。
原版代码与新增 C 模块均采用 GPL-2.0-or-later，见根目录 `COPYING`。
电源线数据接口对齐 `~/workspace/12V-2X6Ti开源` 的 nvpowermon 2.0.1（提交 `7afb11c`）及其 socket 协议 v1。该项目现采用 MIT；本仓库独立实现 C 订阅客户端，整体许可证保持 GPL-2.0-or-later。

## 启动

```sh
cd ~/workspace/htop_plus
./htop_plus
```

已在本机编译完成，无需安装即可使用。
程序首次运行会将 `htoprc.example` 复制成当前目录的 `htoprc`，之后保留你的修改。
两栏顶部恢复每核 CPU 条，本机 0～95 号逻辑 CPU 全部显示。
左右栏各显示一半核心，每栏分成四个小列；GPU 在左侧 CPU/内存下方，电源线在右侧 CPU/任务信息下方。
在 F2 → Meters 中可添加每核 CPU、移动或删除仪表；新的两个仪表名为 `NVIDIA GPU sensors` 和 `5090 12V-2X6 Ti cable`。
F10 或 q 退出。

在 F2 中可以添加、移除新仪表。
程序支持 htop 原有参数，例如 `./htop_plus --readonly`。
可通过 `HTOPRC=/path/to/htoprc ./htop_plus` 使用其他配置。
建议使用 120 列、32 行及以上终端，以便完整显示 96 个核心的百分比和进程列表。
终端较窄时每核条内的文字会截断；可在 F2 调整 CPU 小列数量和布局。

## 安装到系统目录

```sh
sudo ./install.sh
htop_plus
man htop_plus
```

脚本会在必要时执行 autogen、configure 和 make；编译依赖见后面的编译说明。
已经完成编译时可使用 `sudo ./install.sh --no-build`，只安装现有程序。

| 文件 | 默认安装位置 |
| --- | --- |
| 原生可执行程序 | `/usr/local/bin/htop_plus` |
| 默认界面模板 | `/usr/local/share/htop_plus/htoprc.example` |
| 文档与许可证 | `/usr/local/share/doc/htop_plus/` |
| man 手册 | `/usr/local/share/man/man1/htop_plus.1` |

安装版在首次运行时创建 `${XDG_CONFIG_HOME:-$HOME/.config}/htop_plus/htoprc`，保存每个用户自己的布局。
不会覆盖原版 `htop`、已有个人布局或 `/etc/nvpowermon/config.json`。
更新时重新运行安装脚本，已运行的旧版进程可继续运行；重新启动命令后使用新版。
源码目录里的 `./htop_plus` 使用源码目录的 `htoprc`。
编译产物与安装文件均为名为 `htop_plus` 的原生程序，进程名、命令行和可执行文件路径保持一致。
升级会清理旧版 `/usr/local/libexec/htop_plus/htop`，不再使用启动脚本。
`HTOPRC` 可以显式指定其他布局文件。

```sh
# 卸载程序，保留个人配置与设备配置
sudo ./uninstall.sh

# 源码目录已删除时，也可以使用安装的卸载脚本
sudo /usr/local/share/htop_plus/uninstall.sh

# 自定义安装位置；卸载时指定同一个前缀
sudo ./install.sh --prefix /opt/htop_plus
sudo ./uninstall.sh --prefix /opt/htop_plus

# 打包或验证，先安装到暂存目录
DESTDIR=/tmp/htop_plus-package ./install.sh --no-build
```

## Docker 进程用户

Linux 上的 USER 列会显示 Docker 容器名和容器内用户名，例如 `[palworld-server-lxh]steam`。
普通宿主机进程继续显示宿主机用户名。
列宽自动扩展，按 USER 排序时使用实际显示名称；用户筛选和权限检查仍以真实宿主机 UID 为依据。
通过进程的 cgroup 识别 Docker ID，支持 cgroup v1/v2 的 `/docker/<id>` 与 `docker-<id>.scope` 路径。
根据进程的 `uid_map` 把宿主机 UID 转换成容器内 UID，再查找容器的 `/etc/passwd`。
这能区分同为 UID 1000、但分别属于不同容器的用户名。

后台线程每约 5 秒更新一次容器名和 passwd 缓存，界面刷新不等待 Docker。
通过 [Docker Engine API](https://docs.docker.com/reference/api/engine/version/v1.45/) 的本地 Unix socket 发出两类 GET 请求：列举运行中的容器、读取容器 `/etc/passwd` 的归档。
不执行容器内命令，不修改容器。
默认使用 `/var/run/docker.sock`；若 `$XDG_RUNTIME_DIR/docker.sock` 可访问则优先使用它，支持 `DOCKER_HOST=unix:///path/to/docker.sock` 指定本地 socket。
远程 TCP Docker 地址不用于本地进程归属解析。
当前运行用户需要已有的 Docker socket 访问权限，程序不会自动调整权限。

无法读取 Docker 名称时显示 `[容器短ID]数字UID`，无法读取 passwd 或找不到用户时显示 `[容器名]数字UID`。
无法读取 UID 映射时用 `uid:宿主机UID` 明确标注，而不套用宿主机用户名。
容器内通过 LDAP 等外部 NSS 服务提供、未写入 `/etc/passwd` 的用户名暂时显示数字 UID。
容器重命名或用户文件修改后，会在下一次后台更新中反映。

## GPU 仪表

每张显卡占六行，使用 htop 自带的条形绘制器，与 CPU/内存保持相同样式。

| 行 | 内容 | 条形刻度 |
| --- | --- | --- |
| GPU0/GPU1… | 显卡名称；索引来自 NVML | 文本 |
| GPU | 核心利用率 | 100% |
| VRM | 已用/总显存，按容量自动使用 K/M/G 等单位 | 显存总量 |
| Tmp | GPU 温度，℃ | 100℃ |
| Fan | 风扇百分比，支持时同时显示第 0 个风扇 RPM | 100% |
| Pwr | GPU 整卡功率/驱动报告的当前功率上限，W | 驱动报告的功率上限 |

斜杠两侧分别保留单位，例如 `31.4G/31.8G` 和 `28.4W/600W`。
显存复用 htop 原生的容量格式化逻辑，例如 `519M/22.0G`，达到 1G 后使用 G。
显存容量数字使用统一文字颜色，不随绿色占用条的填充位置改变颜色。

运行时动态加载 `libnvidia-ml.so.1`，不需要 CUDA 开发包，不调用 `nvidia-smi` 子进程。
最多展示 32 张 NVIDIA GPU，每次采集间隔至少 1 秒。
不支持的单项显示 `N/A`，不会以 0 代替；没有功率上限时只显示瓦数，不绘制假定上限的填充条。
风扇静止的 0 RPM 是有效读数。
无 NVIDIA 驱动时显示不可用，CPU、内存及进程列表仍可使用。
当前实现面向本机 NVIDIA 显卡；AMD/Intel 的整套传感器未接入此仪表，上游已有的 GPU usage 仪表保留。

## 5090 外接电源线

面板标题为 `5090 / 12V-2X6 Ti`。
`Ti` 是外接智能线的型号后缀，不是显卡型号的一部分。
数据来自外接的 WINGSTUDIO 智能线，与 GPU 索引独立；将它接到其他显卡时，标题不会自动识别接线对象。

| 行 | 内容 |
| --- | --- |
| A～F | 六路电压 V、电流 A、单路计算功率 V×A（W）；条形表示电流 |
| Pwr | 设备报告的线缆总功率 W |
| Con | 接头温度 ℃ |
| Ctl | 主控板温度 ℃ |

电流条的固定显示刻度为 10A，总功率条为 1000W，温度条为 100℃。
功率显示为 `实际线缆功率W/1000W`；1000W 按本机电源额定功率设置为图表刻度，读数仍然只覆盖外接线缆。
这些刻度用于绘图，不是设备报警或保护配置，也不会修改硬件限值。
各路功率由返回的电压、电流相乘，可能因设备内部计算和四舍五入而与设备总功率不完全相等。
NVML 的整卡功率与外接线功率测量范围不同，分别显示。

数据由 **nvpowermon 2.0.1 服务**采集，htop_plus 默认订阅 `/run/nvpowermon/status.sock`。
htop_plus 与 `nvpowermon --watch`、`nvpowermon -t` 等客户端可以同时运行，共享同一份采样。
只有服务打开串口，采样频率由服务决定，增加客户端不会增加串口查询次数。

设备配置仍为 `/etc/nvpowermon/config.json`，由 nvpowermon 服务读取。
服务根据配置里的 USB 序列号及 VID/PID 定位串口；设备重新插拔导致串口编号变化时，由服务重新定位。
htop_plus 不再读取设备配置或访问串口，也不会在服务不可用时回退为串口直连。
客户端只需 socket 访问权限，无需串口权限。

```sh
# 已安装 nvpowermon，但尚未注册服务时（在上游项目中先执行其 install.sh）
sudo nvpowermon --register

# 已注册的服务可用以下命令检查、启动
nvpowermon --status
sudo nvpowermon --start

./htop_plus

# 临时订阅另一个 nvpowermon 服务
HTOP_POWER_CABLE_SOCKET=/path/to/status.sock ./htop_plus

# 也兼容 nvpowermon 自身的环境变量；HTOP_POWER_CABLE_SOCKET 优先
NVPOWERMON_SOCKET=/path/to/status.sock ./htop_plus

# 关闭电源线订阅
HTOP_POWER_CABLE_SOCKET=off ./htop_plus
```

保留旧的 `HTOP_POWER_CABLE_PORT=off` 禁用方式；其他端口覆盖及 `HTOP_POWER_CABLE_CONFIG` 已停用，设置后会显示迁移提示。
需要更换设备或配置文件时，应修改 nvpowermon 服务配置。
htop_plus 的安装脚本只安装本工具；nvpowermon 服务由其自身项目管理。

订阅使用非阻塞 Unix socket，接收 UTF-8 JSON Lines，不发送请求、串口命令或控制指令。
支持分段、多帧合并和协议允许的最长 65536 字节帧，忽略未知扩展字段。
检查协议版本、元数据、六路有序通道及全部必需数值；拒绝非有限数值、错误类型和非法帧。
同一连接上忽略重复及旧序号，重连后重新计数，以适应服务重启。
服务的 `starting`、`error` 状态和连接中断会立即使面板显示 `N/A`；服务错误原因显示在标题下方。
断线每 5 秒尝试重连，首次等待超时 15 秒，后续推送超时 10 秒。
按服务提供的 `sample_monotonic_s` 与本机单调时钟检查 10 秒有效期，收到旧缓存和持续连接停止更新时均会失效。
这个时间戳表示服务发起查询的时间；硬件自身缓存约每 2 秒更新一次。

## 编译与验证

```sh
./autogen.sh
CC=/usr/bin/gcc ./configure --enable-unicode
make -j8
python3 tests/test_hardware.py
python3 tests/test_docker_users.py
python3 tests/test_install.py
```

需要 C 编译器、make、autoconf、automake、pkg-config，以及 ncurses、json-c 和 libcurl 开发包（Debian/Ubuntu 对应 `libncurses-dev libjson-c-dev libcurl4-openssl-dev`）。
使用 `/usr/bin/gcc` 可避开本机环境中的 ccache 缓存目录权限问题。
本次构建关闭了可选的 sensors 和 capabilities；NVML 和智能线采集不依赖它们。
自动测试使用模拟 NVML 和 Unix socket 服务，不接触真实 GPU 和串口。
测试覆盖多 GPU、单位换算、RPM ABI、单项不支持、驱动初始化失败，以及分段/合并 JSON 帧、长度上限、非法字段、协议版本、启动/错误/恢复、缓存过期、超时、断线重连和序号重置。

测试后可用诊断程序检查真实采集：

```sh
tests/build/hardware-probe 1           # 一次 GPU 查询
tests/build/hardware-probe 100 cable   # 约五秒服务订阅
scripts/verify-hardware-ui.sh         # 用 tmux 检查 180、120、80 列终端
```

终端检查输出保存在 `tests/build/ui-180.txt`、`ui-120.txt` 和 `ui-80.txt`。
检查脚本订阅已有 nvpowermon 服务，退出后关闭测试终端。

Docker 自动测试使用模拟 Unix HTTP 服务，不访问真实 Docker；真机界面检查可运行 `scripts/verify-docker-users.sh`，结果保存在 `tests/build/ui-docker-users.txt`。
