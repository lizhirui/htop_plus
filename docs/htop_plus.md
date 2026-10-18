# htop_plus 使用说明

源码来自 [htop 官方仓库](https://github.com/htop-dev/htop)。
下载时间为 2026-10-18，上游版本为 `3.6.0-dev`，提交为 `05c0af5ef82bd4729ab1a22666413689a04bfc2d`。
原版代码与新增 C 模块均采用 GPL-2.0-or-later，见根目录 `COPYING`。
串口协议参考 `~/workspace/12V-2X6Ti开源` 中的设备说明、固件通信格式与实际采样，没有复制该目录的 GPL-3.0-only Python 实现。

## 启动

```sh
cd ~/workspace/htop_plus
./htop_plus
```

已在本机编译完成，无需安装即可使用。
启动器首次运行会将 `htoprc.example` 复制成当前目录的 `htoprc`，之后保留你的修改。
两栏顶部恢复每核 CPU 条，本机 0～95 号逻辑 CPU 全部显示。
左右栏各显示一半核心，每栏分成四个小列；GPU 在左侧 CPU/内存下方，电源线在右侧 CPU/任务信息下方。
在 F2 → Meters 中可添加每核 CPU、移动或删除仪表；新的两个仪表名为 `NVIDIA GPU sensors` 和 `5090 12V-2X6 Ti cable`。
F10 或 q 退出。

直接运行 `./htop` 也支持新仪表，但读取原有 htop 配置；原配置没有新仪表时，可通过 F2 添加。
没有原配置时，Linux 默认布局会在左右栏各追加 GPU 和电源线仪表。
启动器支持 htop 原有参数，例如 `./htop_plus --readonly`。
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
| 启动命令 | `/usr/local/bin/htop_plus` |
| 修改版 htop 本体 | `/usr/local/libexec/htop_plus/htop` |
| 默认界面模板 | `/usr/local/share/htop_plus/htoprc.example` |
| 文档与许可证 | `/usr/local/share/doc/htop_plus/` |
| man 手册 | `/usr/local/share/man/man1/htop_plus.1` |

安装版在首次运行时创建 `${XDG_CONFIG_HOME:-$HOME/.config}/htop_plus/htoprc`，保存每个用户自己的布局。
不会覆盖原版 `htop`、已有个人布局或 `/etc/nvpowermon/config.json`。
更新时重新运行安装脚本，已运行的旧版进程可继续运行；重新启动命令后使用新版。
源码目录里的 `./htop_plus` 仍使用源码目录的 `htoprc`。
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

默认读取 `/etc/nvpowermon/config.json`，与原来的 nvpowermon 共用同一份连接配置。
有 `usb_serial` 时，在 `/sys/class/tty` 中枚举 USB 串口，沿设备父目录读取序列号、VID/PID，只选择唯一匹配的设备。
此时忽略配置中的旧 `port`，因此插拔后变成 `ttyACM1` 仍能定位；无匹配或匹配多个设备时报错，不回退到其他端口。
`usb_serial` 为 null 或省略时才使用 `port`；如果同时设置了 VID/PID，也会验证指定端口的 USB 信息。
本机配置的序列号是 `5CF9119752`，VID/PID 是 `1a86:55d3`。
同样读取 `baudrate` 和 `timeout_seconds`；波特率只接受 115200，超时必须是有限正数。
配置只读，每次重新连接时重新读取，不修改 `/etc/nvpowermon`。

```sh
./htop_plus  # 默认使用 /etc/nvpowermon/config.json

# 临时指定另一份同格式配置
HTOP_POWER_CABLE_CONFIG=/path/to/config.json ./htop_plus

# 显式端口覆盖：绕过配置定位，适合调试
HTOP_POWER_CABLE_PORT=/dev/ttyACM0 ./htop_plus

# 关闭串口采集
HTOP_POWER_CABLE_PORT=off ./htop_plus
```

使用 115200、8N1、无流控；独占串口并检查可见的已有读取进程。
同一时间请只运行一个串口监控程序；`nvpowermon -t`、WINGFORCE 等已占用设备时会显示 busy。
只发送 `*IDN?` 和 `SENSORDATA?`，先验证型号，不发送配置、风扇控制、复位、报警测试或保护控制命令。
程序不主动切换 DTR/RTS、不发送 BREAK，清除 HUPCL 以避免关闭时请求拉低控制线；内核首次打开设备仍可能设置默认控制线状态。
使用非阻塞状态机，不等待串口应答；查询超时读取配置的 `timeout_seconds`，省略时为 4 秒，下次界面刷新时处理超时。
显式设置 `HTOP_POWER_CABLE_PORT` 覆盖时使用 4 秒超时。
每次成功响应后至少隔 2 秒再查询，设备自身缓存约 2 秒更新一次。
每 5 秒重试无法连接的设备，重连时重新验证 ID。
超时、非法响应或检测到拔线时立即清除有效标志，面板改为 `N/A`，不会继续把旧读数显示为当前值。
采样超过 10 秒未更新也会失效。
协议未返回报警状态、传感器有效标志或设备采样时间戳，因此这里展示的是设备报告的缓存读数。

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
自动测试使用伪终端与模拟 NVML，不接触真实 GPU 和串口。
测试也覆盖配置文件解析、按 USB 序列号与 VID/PID 定位、串口改号、不匹配时不回退和重复序列号。
测试覆盖多 GPU、单位换算、RPM ABI、单项不支持、驱动初始化失败、分段串口响应、错误 ID、非法字段、超长响应、超时、占用和断线。

测试后可用诊断程序检查真实采集：

```sh
tests/build/hardware-probe 1           # 一次 GPU 查询
tests/build/hardware-probe 100 cable   # 约五秒串口采集
scripts/verify-hardware-ui.sh         # 用 tmux 检查 180、120、80 列终端
```

终端检查输出保存在 `tests/build/ui-180.txt`、`ui-120.txt` 和 `ui-80.txt`。
检查脚本会临时独占智能线，退出后关闭测试终端。

Docker 自动测试使用模拟 Unix HTTP 服务，不访问真实 Docker；真机界面检查可运行 `scripts/verify-docker-users.sh`，结果保存在 `tests/build/ui-docker-users.txt`。
