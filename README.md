# FX3 CyAPI Python 采集工程

Python/PySide6 负责界面和业务逻辑，pybind11 连接 C++ 采集内核，Windows 内核调用官方 CyAPI 源码。当前工程读取 **Bulk IN 原始字节**，不假定设备的 ADC 通道、位宽、帧格式、采样率或控制命令。

仓库仅保存本项目的功能实现、测试和构建配置，**不包含官方 CyAPI/USB-Suite 源码、旧预编译库或参考例程**。Windows 构建需自行获取官方源码，按下文配置外部路径。

## Windows 安装与启动

从 GitHub 克隆后，先创建环境并指定官方源码目录（应直接包含 `src/CyAPI.cpp` 和 `inc/CyAPI.h`）：

```bat
conda env create -f environment.windows.yml
conda activate fx3-cyapi
set "CYAPI_DIR=E:\SDK\CyUSB3_USB_Suite_Source\CyAPI_Library_Source\CyAPI\library\cpp"
```

请将路径替换为实际下载位置。也可以用 `python -m pip install ".[test]" -Ccmake.define.CYAPI_DIR="E:/SDK/.../library/cpp"` 显式传入 CMake 参数。本机已有同级官方源码目录时会自动使用它；该目录始终被 Git 与源码发行包排除。源码无需复制到新仓库。

已验证工具组合：Windows x64、Conda `fx3-cyapi`、Python 3.12、Visual Studio 2026/MSVC x64、CMake、Ninja。

在命令提示符执行（脚本会寻找 MSVC 并激活 Miniforge 环境）：

```bat
cd /d E:\atom-laser\software\fx3_cyapi
scripts\build_windows.cmd
```

该脚本编译并安装本项目及 PySide6、pytest，然后运行测试。已有其他 Miniforge 路径时，在脚本中调整路径；手动安装也可在 x64 Developer Command Prompt 中执行：

```bat
call C:\Users\WeiQian\miniforge3\condabin\conda.bat activate fx3-cyapi
cd /d E:\atom-laser\software\fx3_cyapi
python -m pip install ".[test]"
python -m pytest -q
python -m fx3_cyapi.gui
```

界面默认勾选“模拟采集”，点击开始即可使用，不要求连接硬件。GUI 预览是原始 uint8 字节的降采样折线，横轴不是采样时间，纵轴不是电压。

生成 wheel：

```bat
python -m build --wheel
```

wheel 位于 `dist`，当前常规构建需匹配 Python ABI（例如 CPython 3.12）和 Windows x64。Windows 非 Python 系统依赖：MSVC 构建工具、Windows SDK；使用硬件时还需要匹配的 CyUSB3 驱动和设备固件。

## Python API

```python
import numpy as np
from fx3_cyapi import AcquisitionConfig, AcquisitionEngine

with AcquisitionEngine() as engine:
    engine.open(index=0, endpoint=0x81, simulated=True)
    cfg = AcquisitionConfig()
    cfg.block_size = 1024 * 1024
    cfg.queue_depth = 8
    cfg.queue_capacity = 16
    cfg.pool_extra = 8
    cfg.timeout_ms = 1000
    engine.configure(cfg)
    engine.start()
    block = engine.read_block(timeout_ms=1000)
    if block is not None:
        data = np.frombuffer(block, dtype=np.uint8)  # 零拷贝、只读
        print(block.sequence, block.size, data[:8])
        # 需长期保存或修改时主动复制；避免无限保留原生缓冲区。
        saved = data.copy()
        del data, block
    engine.stop()
    print(engine.stats().received_bytes)
```

- `list_devices()` 返回默认 CyUSB3 接口 GUID 下可打开设备的索引、VID/PID、名称、序列号。枚举会短暂打开设备，请在采集停止时使用。
- `open(index=0, endpoint=0x81, simulated=False)` 打开真实设备；`0x81` 只是默认候选地址，必须依据固件选择实际 Bulk IN 端点。
- `configure(config)` 只接受已打开、已停止并 join 的状态。`block_size` 必须为选定端点最大包长的整数倍，范围为 1..16 MiB。`queue_depth` 为 1..64。
- `start()` 等待原生线程初始化和初始请求入队；线程不调用 Python 回调。
- `read_block(timeout_ms=100)` 返回 `DataBlock` 或 `None`。`None` 表示等待超时或采集正常结束且队列已空；出错后先读完已完成队列，随后抛出 `RuntimeError`。
- `stats()` 返回线程安全的值快照，包含接收量、队列高水位、主机丢弃量、缓冲池大小和错误文本。
- `stop()` 停止并 join 线程，保留已完成队列以便读尾部数据；重复调用安全。错误停止后也需调用 `stop()` 再配置/重启。
- `close()` 先停止，再关闭设备并清空完成队列；`with` 退出时自动调用。
- `DataBlock.timestamp_ns` 是主机 steady-clock 的完成时间，非设备采样时间。`sequence` 是主机完成顺序号；丢弃会形成间断。

## 线程、缓冲区与 GIL

```text
PySide6 主线程：控件、50 ms 定时器、统计、少量显示预览
    │ pybind11：open/configure/start/read_block/stats/stop/close
    │ 阻塞方法显式释放 GIL
C++ Engine：生命周期互斥锁、完成队列互斥锁、条件变量
    └─ 原生线程 → CyAPI 多个在途请求 → 有界完成队列
                    └─ 固定缓冲池 → 只读 DataBlock → NumPy/memoryview
```

GUI 的打开、启动、停止、关闭通过 Qt 工作线程执行，设备 I/O 全部在 C++ 内部。GUI 每次定时最多取 32 个完整数据块，仅复制最多 1024 个预览字节，不逐 USB 包调度请求。此界面是监视器，不保证完整保存采集数据；需要完整处理时应使用专门消费者，必要时扩展原生解析/落盘。

每个池分配 `queue_depth + queue_capacity + pool_extra` 个 `block_size` 缓冲区，上限为 **512 MiB/引擎**。这个上限只计算应用负载池，不包含驱动、CyAPI 请求上下文、Python 主动复制及 GUI 内存。CyAPI direct 模式仍为每个请求分配小型上下文，不能称整个传输过程零分配。

完成队列满或池无空闲块时，丢弃**新完成**的数据块并立即复用该请求的负载缓冲区，累积 `dropped_blocks/dropped_bytes`。不覆盖已交给 Python 的块，也不无限分配。此计数仅表示主机应用丢弃，不证明设备/USB 链路没有其他丢数；端到端完整性需固件帧计数器。

`DataBlock` 的只读 buffer protocol 让 NumPy/memoryview 持有原生块的共享所有权；即使关闭或销毁引擎，已有视图仍有效。保留视图会占用池并可能导致主机丢弃。重配、重启或重新打开前需释放上一代所有 `DataBlock` 及其视图，以防反复启动积累多代缓冲池。单个视图可能让整代池继续存活。

停止流程：原生线程察觉停止标志（常规轮询 20 ms），通过 `CancelIoEx` 取消所有在途请求，逐个等待终态，再调用 `FinishDataXfer` 回收上下文，最后释放事件与负载缓冲。为避免 use-after-free，不设置“超时就强制释放内存”。驱动若不完成取消，停止可能继续等待；GUI 保持响应，但进程退出需等待驱动，不能保证硬截止时间。

## 硬件接入与验证

1. 安装与设备 VID/PID 匹配的官方签名 CyUSB3 驱动。这里直接编译 `CyAPI_Library_Source/.../library/cpp/src/CyAPI.cpp`，使用同目录头文件，不使用旧 `CyAPI/lib/CyAPI.lib`，不编译内核驱动源码。
2. 确认默认 `CYUSBDRV_GUID` 与设备驱动 INF 中的接口 GUID 一致。当前 API 使用默认 GUID；自定义 GUID/alternate setting 需后续增加显式配置。
3. 确认设备固件已开始发送数据，选择正确 Bulk IN 端点。当前不发送应用控制命令，也不烧写固件。
4. 先低速运行 `python scripts/smoke_acquisition.py --hardware --index 0 --endpoint 0x81`，检查实际长度和数据，再增加块大小与在途请求数。
5. 用带序号的测试数据/固件检查连续性、吞吐、短包、超时、拔插及停止/重启。仅模拟测试通过不能证明硬件路径可靠。

Linux 构建只提供同一 C++ 内核的模拟后端，不能通过 Linux/WSL Python 调用 Windows CyAPI。

## 文件与参考

- `native/engine.*`：可独立于 Qt 运行的采集调度、固定池和模拟后端。
- `native/usb_windows.cpp`：Windows CyAPI 异步传输及取消回收。
- `native/bindings.cpp`：粗粒度绑定和只读 buffer protocol。
- `python/fx3_cyapi/gui.py`：界面与降采样预览。
- `tests`：模拟数据、内存所有权、溢出、故障、并发和 Qt 生命周期测试。
- [pybind11 GIL](https://pybind11.readthedocs.io/en/stable/advanced/misc.html)
- [pybind11 buffer protocol](https://pybind11.readthedocs.io/en/stable/advanced/pycpp/numpy.html)
- 官方异步例程：`CyUSB3_USB_Suite_Source/CyAPI_Library_Source/CyAPI/application/cpp/streamer/Streamer.h`。官方源码原样保留其授权声明。

## 本次验证记录（2026-11-06）

- 已在 Windows x64 / Python 3.12.16 / VS2026 18.11.3 / MSVC 19.51.36260 / Windows SDK 10.0.28000.0 下编译通过。
- `fx3-cyapi` 环境已安装本包、PySide6 6.12.0 和 pytest 9.2.0；包依赖检查通过。
- 21 项测试通过，包括模拟数据模式、只读零拷贝视图、视图跨关闭/析构存活、固定池溢出、GIL/并发读取、故障、请求超时、重复生命周期，以及无显示 Qt 窗口的启动/关闭。
- 测试报告：`artifacts/test-results.xml`。wheel：`dist/fx3_cyapi-0.1.0-cp312-cp312-win_amd64.whl`。
- **未进行真实 FX3 采集、吞吐测量、拔插或驱动取消实测**。模拟后端不验证 CyUSB3 驱动和设备固件。
