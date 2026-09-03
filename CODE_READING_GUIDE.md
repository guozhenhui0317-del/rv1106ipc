# RV1106 IPC 代码阅读指南（C++ 初学者版）

这份文档帮助第一次接触 C++ 和 Rockchip 媒体开发的人读懂本项目。建议先理解“程序如何启动、数据如何流动、资源如何释放”，再深入每一个 SDK 结构体。

> 当前配置中 `video.source:enable_npu=1`，RockIVA 检测和画框链路默认启动；运行前需把官方模型部署到 `rockiva:model_path` 指定的目录。

## 1. 先看懂项目在做什么

程序从摄像头和麦克风取得原始数据，交给 RV1106 硬件编码器，再由 FFmpeg 封装并推送到 MediaMTX。FFmpeg 不重新编码。

~~~mermaid
flowchart LR
    SC3336[SC3336 摄像头] --> ISP[RKAIQ / ISP]
    ISP --> VI0[VI0 主码流图像]
    ISP --> VI1[VI1 AI 子码流图像]

    VI0 --> VENC0[VENC0 H.264/H.265]
    VI1 --> VENC1[VENC1 H.264/H.265]
    VENC0 --> PUB_MAIN[主码流 Publisher]
    VENC1 --> PUB_AI[AI 码流 Publisher]

    MIC[麦克风] --> AI0[AI0 音频输入]
    AI0 --> VQE[VQE 可选音频处理]
    VQE --> AENC0[AENC0 G711A]
    AENC0 --> PUB_MAIN

    PUB_MAIN --> MAIN_RTSP[RTSP main_rtsp]
    PUB_MAIN --> MAIN_RTMP[RTMP main_rtmp]
    PUB_AI --> AI_RTSP[RTSP ai_rtsp]
    PUB_AI --> AI_RTMP[RTMP ai_rtmp]
~~~

这里最容易误解的是两个 “AI”：

- **AI0**：Rockchip MPI 中的 Audio Input 0，也就是音频输入，不是人工智能。
- **AI 子码流**：项目给第二路视频取的业务名称；只有启用 RockIVA 时才会进行目标检测。

## 2. C++ 小白需要先认识的概念

### 2.1 头文件和源文件

- `include/*.h`：对外声明“这个模块能做什么”。
- `src/*.cpp`：具体实现“这个模块怎么做”。
- 阅读时先看头文件，再去源文件找同名函数，通常更容易建立整体认识。

例如，[media.hpp](include/media.hpp) 声明 `MediaRuntime`，[media.cpp](src/media.cpp) 实现它。

### 2.2 构造函数、析构函数与 RAII

构造函数在对象创建时自动执行，析构函数在对象离开作用域时自动执行。RAII 的核心是：**对象活着就持有资源，对象销毁就释放资源**。

本项目中的典型资源包括：

| C++ 对象 | 持有的资源 | 销毁时负责 |
|---|---|---|
| `Logger` | EasyLogger、日志文件 | 刷新并停止日志 |
| `Parameters` | INI 参数字典 | 反初始化参数系统 |
| `MediaRuntime` | ISP、MPI、VI、VENC、AI、AENC、线程 | 按安全顺序停止全部媒体资源 |
| `Output` | FFmpeg `AVFormatContext` | 关闭网络输出并释放上下文 |

在 [main.cpp](src/main.cpp) 中，这些对象是局部变量。函数退出时会按照**创建顺序的反方向**自动析构，这正是项目使用 RAII 的关键。

### 2.3 `std::unique_ptr`

`unique_ptr` 表示一份资源只有一个拥有者。拥有者销毁时，资源自动释放。项目还用它实现 PImpl：

~~~text
MediaRuntime
    └── unique_ptr<MediaPipeline>
            └── 真正的媒体实现和 SDK 资源
~~~

这样头文件不必暴露大量 Rockchip SDK 类型，也能减少模块之间的依赖。

### 2.4 线程、原子变量和互斥锁

- `std::thread`：同时执行视频取流、音频取流、AI 推理等循环。
- `std::atomic<bool>`：让多个线程安全地读取“是否停止”状态。
- `std::mutex`：保证同一个 FFmpeg 输出不会被视频线程和音频线程同时修改。
- `join()`：等待线程真正退出。只有线程退出后，才能销毁它正在访问的通道和缓冲区。

### 2.5 `extern "C"`

Rockchip 官方公共代码和 FFmpeg 很多接口是 C 接口。C++ 通过 `extern "C"` 引入它们，避免 C++ 名字改编导致链接时找不到函数。

### 2.6 异常

初始化失败时，代码会抛出异常。`main()` 最外层捕获异常、记录错误并返回非零状态。已经成功创建的局部对象仍会自动析构，因此异常路径同样能够触发资源清理。

## 3. 目录和模块职责

| 路径 | 作用 | 建议阅读时机 |
|---|---|---|
| [src/main.cpp](src/main.cpp) | 程序入口、命令行、信号和对象装配 | 第一 |
| [include/media.hpp](include/media.hpp) | 媒体运行时对外接口 | 第二 |
| [src/media.cpp](src/media.cpp) | ISP、MPI、视频、音频、RockIVA 和线程 | 第三 |
| [include/ffmpeg_publisher.h](include/ffmpeg_publisher.h) | 推流模块对外接口 | 第四 |
| [src/ffmpeg_publisher.cpp](src/ffmpeg_publisher.cpp) | Annex-B 解析、FFmpeg 建连和写包 | 第五 |
| [include/logger.hpp](include/logger.hpp) / [src/logger.cpp](src/logger.cpp) | EasyLogger 封装 | 辅助阅读 |
| [common/param](common/param) | 官方 INI 参数管理 | 需要追参数时 |
| [common/isp/rv1106](common/isp/rv1106) | 官方 RKAIQ/ISP 封装 | 需要查 ISP 时 |
| [common/rockiva](common/rockiva) | 官方 RockIVA 包装 | 启用 NPU 时 |
| [common/easylogger](common/easylogger) | 裁剪后的 EasyLogger 核心源码 | 需要查看日志底层时 |
| [rv1106_sc3336.ini](rv1106_sc3336.ini) | 分辨率、编码、地址、音频等配置 | 和代码对照阅读 |
| [CMakeLists.txt](CMakeLists.txt) | 头文件、源码、库和 RPATH 配置 | 最后看构建关系 |

## 4. 程序从哪里开始

入口是 [main.cpp](src/main.cpp) 中的 `main()`。

~~~mermaid
sequenceDiagram
    participant OS as Linux
    participant Main as main()
    participant Log as Logger
    participant Param as Parameters
    participant Media as MediaRuntime
    participant ISP as RKAIQ/ISP
    participant MPI as RK_MPI
    participant Pub as PublisherSet

    OS->>Main: 启动 /root/gzh_ipc
    Main->>Main: 解析 -c / -a / -l
    Main->>Main: 注册 SIGINT/SIGTERM，忽略 SIGPIPE
    Main->>Log: 初始化 EasyLogger
    Main->>Param: 加载 INI
    Main->>Media: 构造媒体运行时
    Media->>ISP: 初始化并应用 ISP 配置
    Media->>MPI: RK_MPI_SYS_Init
    Media->>Pub: 创建四个独立输出
    Media->>Media: 创建视频、音频和工作线程
    loop 直到收到退出信号
        Main->>Main: 等待
    end
    Main->>Media: 离开作用域，开始析构
~~~

命令行参数的含义：

| 参数 | 含义 |
|---|---|
| `-c` | 指定 INI 路径 |
| `-a` | 指定 RKAIQ IQ 文件目录 |
| `-l` | 指定日志等级 |

信号处理函数只修改一个 `sig_atomic_t` 标志，不在信号回调中关闭设备或写复杂日志。这是正确做法，因为大部分 SDK 和 C++ 操作都不是异步信号安全的。

`SIGPIPE` 被忽略。网络连接断开时，写 socket 不会直接杀死整个进程，而是让 FFmpeg 返回错误，由推流模块执行重连。

## 5. INI 是怎样进入代码的

~~~mermaid
flowchart LR
    INI[rv1106_sc3336.ini] --> Parameters[Parameters 构造函数]
    Parameters --> rkparam[rk_param_init]
    rkparam --> parser[iniparser 字典]
    parser --> getters[rk_param_get_int/string/...]
    getters --> Media[media.cpp]
    getters --> Publisher[ffmpeg_publisher.cpp]
    getters --> ISP[common/isp]
~~~

参数键通常采用 `段名:键名`，例如：

- `video.0:width`：主码流宽度。
- `video.source:enable_npu`：是否启动 RockIVA。
- `audio.0:sample_rate`：音频采样率。
- `network:main_rtsp_url`：主码流 RTSP 地址。

`Parameters` 是 C++ 包装，底层仍复用官方 `common/param`。不要绕开它再写一套 INI 解析器。

## 6. MediaRuntime：总调度器

`MediaRuntime::MediaPipeline` 是媒体系统总负责人。它的初始化逻辑可以概括为：

1. 初始化 RKAIQ/ISP，并从 INI 应用配置。
2. 如果 `enable_npu=1`，初始化 RockIVA。
3. 调用 `RK_MPI_SYS_Init`。
4. 初始化 FFmpeg 推流集合。
5. 创建视频链路。
6. 创建音频链路。
7. 启动各取流线程。

若中途失败，已经创建的成员和局部资源会逆序清理。正常退出时，`stop()` 也按依赖关系的反方向执行：

~~~mermaid
flowchart TD
    S[收到退出信号] --> A[停止 Audio 线程和通道]
    A --> V[停止 Video / 推理 / 画框线程]
    V --> P[关闭 FFmpeg 输出]
    P --> M[RK_MPI_SYS_Exit]
    M --> R[反初始化 RockIVA]
    R --> I[反初始化 ISP]
    I --> L[最后关闭日志]
~~~

这条顺序很重要：线程必须先停，之后才能销毁线程正在调用的 MPI 通道；MPI 资源清空后，才能关闭 ISP。

## 7. 视频链路：VI0/VI1 到 VENC0/VENC1

### 7.1 固定通道编号

代码使用的核心编号如下：

| 常量 | 数值 | 含义 |
|---|---:|---|
| `kViDev` | 0 | VI 设备 0 |
| `kViPipe` | 0 | VI 管线 0 |
| `kMain` | 0 | 主码流 VI0/VENC0 |
| `kAi` | 1 | AI 子码流 VI1/VENC1 |
| `kAenc` | 0 | 音频编码通道 0 |
| `kOverlay` | 7 | 检测框 RGN Overlay 句柄 |

### 7.2 创建顺序

视频对象构造时大致执行：

1. 配置并启用 VI 设备。
2. 创建 VI0 和 VI1。
3. 创建 VENC0 和 VENC1。
4. 绑定 VI0 → VENC0。
5. 绑定 VI1 → VENC1。
6. 如果启用 NPU，创建并绑定 2BPP Overlay 到 VENC1。
7. 启动 `venc-main` 和 `venc-ai` 线程。
8. 如果启用 NPU，再启动推理线程和 OSD 线程。

VI0 只供编码器使用，所以深度设为 0。VI1 在启用 NPU 时还需要由 CPU 调用 `GetChnFrame` 取得帧，因此深度设为 1。

### 7.3 编码参数从哪里来

每个 VENC 通道分别读取对应的 `video.0` 或 `video.1` 配置，设置：

- 宽、高和像素格式。
- H.264 或 H.265。
- 码率控制方式、码率、帧率和 GOP。
- 编码缓冲区等 SDK 属性。

真正的压缩工作由 RV1106 VENC 完成；FFmpeg 收到的已经是 H.264/H.265 压缩数据。

### 7.4 一帧视频如何走到网络

~~~mermaid
sequenceDiagram
    participant ISP as ISP/VI
    participant VENC as VENC0 或 VENC1
    participant T as venc 线程
    participant MB as MediaBuffer
    participant Pub as PublisherSet
    participant Out as RTSP/RTMP Output

    ISP->>VENC: 绑定通道自动送入 NV12 帧
    T->>VENC: RK_MPI_VENC_GetStream
    VENC-->>T: VENC_STREAM_S + PTS
    T->>MB: RK_MPI_MB_Handle2VirAddr
    MB-->>T: 编码数据首地址
    T->>Pub: write_video(stream_id, data, len, pts, key)
    Pub->>Pub: 立即复制数据
    Pub->>Out: 分别写入对应 RTSP 和 RTMP
    T->>VENC: RK_MPI_VENC_ReleaseStream
~~~

这里有一个已经验证过的 SDK 差异点：在当前 RV1106 SDK 中，`RK_MPI_MB_Handle2VirAddr` 返回的地址已经指向当前有效编码数据，不能再次叠加 `VENC_PACK_S::u32Offset`。

设备日志曾显示类似：

~~~text
len=995, offset=6912, mb_size=995
~~~

如果再执行“首地址 + 6912”，就会越过有效缓冲区，破坏 Annex-B 数据，最终出现 MediaMTX `invalid body size`，甚至进程崩溃。当前代码按实际 SDK 行为直接使用返回地址。

推流模块必须在 `RK_MPI_VENC_ReleaseStream` 之前复制数据，因为释放后 MediaBuffer 的内存不再属于当前线程。

## 8. 音频链路：AI0 到 AENC0

~~~mermaid
flowchart LR
    MIC[麦克风] --> AI0[AI0: 8000 Hz 单声道]
    AI0 --> VQE[VQE: AEC/降噪等]
    VQE --> AENC0[AENC0: G711A]
    AENC0 --> THREAD[aenc-main 线程]
    THREAD --> RTSP[主码流 RTSP]
    THREAD --> RTMP[主码流 RTMP]
~~~

音频初始化遵循官方 Demo 的 8000 Hz、单声道和 G711A 配置。VQE 根据 INI 和 JSON 配置启用，并依赖官方音频处理库。

音频线程的核心循环与视频类似：

1. `RK_MPI_AENC_GetStream` 取得一个编码音频包。
2. 取得 MediaBuffer 虚拟地址。
3. 调用 `PublisherSet::write_audio`。
4. `RK_MPI_AENC_ReleaseStream` 释放包。

音频只写入主码流的 RTSP/RTMP；AI 子码流输出没有音频轨道。

## 9. FFmpeg 推流模块

### 9.1 四个 URL 是四个独立输出

`PublisherSet` 内部创建四个 `Output`，每个都有独立 `AVFormatContext` 和互斥锁：

| 输出 | 视频通道 | 音频 |
|---|---:|---|
| `rtsp-main` | 0 | G711A |
| `rtmp-main` | 0 | G711A |
| `rtsp-ai` | 1 | 无 |
| `rtmp-ai` | 1 | 无 |

一个地址断开时，只关闭和重连自己的上下文，不应影响另外三个输出。

### 9.2 初始化阶段先检查什么

推流模块会检查：

- 编码格式是否支持。
- NV12 宽高是否为偶数。
- URL 是否为空、协议是否匹配。
- RTMP 是否错误地配置为 H.265。

FFmpeg 4.x 的传统 FLV/RTMP 不支持这里需要的 H.265 推送，因此启用 RTMP 时视频必须选择 H.264；H.265 可用于 RTSP。

### 9.3 为什么必须等待 SPS/PPS

VENC 输出是 Annex-B 格式，由多个以 `00 00 01` 或 `00 00 00 01` 开头的 NAL 单元组成。

- H.264 建连需要完整 SPS 和 PPS。
- H.265 建连需要完整 VPS、SPS 和 PPS。
- 只有标记为关键帧还不够；关键帧里也可能没有这些参数集。

`parameter_sets` 会扫描 NAL 单元。参数不完整时丢弃该帧，继续等待，而不是拿损坏或不完整的数据建立连接。

### 9.4 每个输出的状态机

~~~mermaid
stateDiagram-v2
    [*] --> 等待关键帧
    等待关键帧 --> 等待关键帧: 非关键帧或参数集不完整
    等待关键帧 --> 建立连接: 完整 SPS/PPS（H265 还含 VPS）
    建立连接 --> 推流中: avformat_write_header 成功
    建立连接 --> 退避: 连接失败
    推流中 --> 推流中: av_interleaved_write_frame 成功
    推流中 --> 退避: 写包失败
    退避 --> 等待关键帧: 3 秒后
~~~

写失败后的 `Failed to update header with correct duration/filesize` 通常是 FFmpeg 关闭不可寻址的网络 FLV 时产生的附带警告。真正需要先看的，是它之前的连接或写包错误。

### 9.5 音视频时间戳

第一个成功用于发布的视频关键帧 PTS 被设为公共时间原点：

~~~text
相对 PTS = 当前硬件微秒 PTS - 首个视频关键帧微秒 PTS
~~~

之后视频和音频都从同一硬件微秒时间轴换算到各自 FFmpeg time base。早于原点的音频会被丢弃，这样主码流不会以负时间戳开头。

### 9.6 为什么需要互斥锁

主视频线程和音频线程可能同时写入 `rtsp-main` 或 `rtmp-main`。FFmpeg 的同一个 `AVFormatContext` 不能被这两个线程无保护地同时操作，所以每个 `Output` 使用自己的 mutex 串行化打开、写包和关闭操作。

## 10. RockIVA 与检测框链路

当前 `enable_npu=1`，模型文件就绪后，数据流程是：

~~~mermaid
sequenceDiagram
    participant VI1 as VI1
    participant Infer as inference 线程
    participant MB as MediaBuffer/DMABUF
    participant IVA as RockIVA
    participant CB as 检测回调
    participant OSD as overlay 线程
    participant RGN as RGN7
    participant VENC1 as VENC1

    Infer->>VI1: RK_MPI_VI_GetChnFrame
    Infer->>MB: 取得 DMABUF fd
    Infer->>IVA: PushFrame(fd, frameId)
    IVA-->>CB: BA 检测结果
    CB->>OSD: 保存最多 10 个目标框
    IVA-->>Infer: 帧释放回调/信号量
    Infer->>VI1: RK_MPI_VI_ReleaseChnFrame
    OSD->>OSD: 归一化坐标换算为像素坐标
    OSD->>RGN: 绘制 2BPP Overlay
    RGN->>VENC1: 叠加到编码画面
~~~

两个生命周期细节不能颠倒：

1. RockIVA 没有发出帧释放回调前，不能把 VI 帧还回去。
2. Overlay 必须先停止更新并解绑，才能销毁 VENC1。

检测框坐标是 0～10000 的归一化坐标，OSD 线程根据 AI 子码流分辨率换算为像素坐标，再对齐到 RGN 要求。

## 11. 日志如何工作

~~~mermaid
flowchart LR
    Module[各模块 LOG_I/W/E] --> EasyLogger[EasyLogger]
    EasyLogger --> Console[stderr 控制台]
    EasyLogger --> File[/root/rkipc.log]
    ThreadName[线程名] --> EasyLogger
~~~

[logger.cpp](src/logger.cpp) 封装 EasyLogger，[elog_port.c](src/elog_port.c) 实现设备端输出。日志端口用互斥锁保护文件写入，防止多个媒体线程的内容互相穿插。

常见线程名能帮助定位来源：

- `gzh_ipc`：主线程。
- `venc-main`：主视频取流。
- `venc-ai`：AI 子码流取流。
- `aenc-main`：音频取流。
- 推理和 OSD 线程只在 NPU 启用后存在。

## 12. 退出顺序为什么这样设计

从“谁还在使用谁”来理解退出顺序：

~~~mermaid
flowchart BT
    Thread[工作线程] --> Channel[VI/VENC/AI/AENC 通道]
    Channel --> MPI[RK MPI 系统]
    MPI --> ISP[ISP 摄像头系统]
~~~

箭头表示“上层正在使用下层”。释放时必须从上到下：

1. 设置停止标志。
2. 等待音频、视频、推理和 OSD 线程 `join()`。
3. 解绑并销毁 AENC/AI、VENC/VI、RGN。
4. 关闭 FFmpeg 输出。
5. 调用 `RK_MPI_SYS_Exit`。
6. 关闭 RockIVA。
7. 关闭 ISP。
8. 最后关闭日志。

若先销毁通道再等待线程，线程可能还在调用已经失效的句柄，典型结果是段错误、卡死或驱动报错。

## 13. CMake 中的依赖关系

[CMakeLists.txt](CMakeLists.txt) 完成四类工作：

1. 加入项目、FFmpeg、EasyLogger 和 Rockchip SDK 头文件路径。
2. 编译 `src` 与选定的 `common` 官方代码。
3. 链接 Rockchip、VQE、FFmpeg 和线程等动态库。
4. 设置构建时链接搜索和设备运行时 RPATH。

主要动态库用途：

| 库 | 用途 |
|---|---|
| `librockit.so` | Rockchip MPI 总体接口 |
| `librockchip_mpp.so.1` | MPP 编解码底层 |
| `librkaiq.so` | ISP/RKAIQ |
| `librockiva.so` | RockIVA |
| `librknnmrt.so` | NPU 运行时 |
| `librga.so` | 图像处理 |
| `librksysutils.so` | Rockchip 系统工具 |
| `librkaudio*.so` | VQE 音频处理 |
| `libavformat.so` | RTSP/RTMP 封装和网络输出 |
| `libavcodec.so` | 编码格式定义和相关支持 |
| `libavutil.so` | FFmpeg 基础工具 |

`-rpath-link` 主要帮助交叉链接器在构建阶段找到动态库的间接依赖，它不能代替板端动态库。程序放在 `/root/gzh_ipc` 时，板端仍要满足以下至少一种方式：

- 依赖库位于系统默认的 `/lib` 或 `/usr/lib`。
- 可执行文件带有指向实际部署目录的 RPATH/RUNPATH。
- 启动前正确设置 `LD_LIBRARY_PATH`。

## 14. 常用 INI 配置到代码的映射

| INI 区域 | 使用模块 | 影响 |
|---|---|---|
| `video.source` | ISP、VI | 摄像头和 ISP 基础配置 |
| `video.0` | Video、Publisher | 主码流分辨率、编码和码率 |
| `video.1` | Video、RockIVA、Publisher | AI 子码流和 NPU 开关 |
| `audio.0` | Audio、Publisher | 采样率、通道、VQE、G711A |
| `network` | Publisher | 四个 RTSP/RTMP URL 和开关 |
| ISP 相关段 | `common/isp` | 曝光、补光、图像参数等 |

改配置前先用 `rg "参数名" src common` 找到读取位置，确认单位和默认值，再修改 INI。

## 15. 推荐阅读顺序

不要从 `media.cpp` 第一行一直读到最后一行。对初学者更友好的顺序是：

1. 看 [include/media.hpp](include/media.hpp) 和 [include/ffmpeg_publisher.h](include/ffmpeg_publisher.h)，知道对外接口。
2. 看 [main.cpp](src/main.cpp)，建立程序生命周期。
3. 看 [logger.cpp](src/logger.cpp)，熟悉较小的 RAII 模块。
4. 从 [media.cpp](src/media.cpp) 底部的 `MediaRuntime` 开始，先看总调度。
5. 再看 Video 类的构造、`stop()` 和 VENC 线程。
6. 看 Audio 类的构造、`stop()` 和 AENC 线程。
7. 看 [ffmpeg_publisher.cpp](src/ffmpeg_publisher.cpp) 的 `PublisherSet`，再进入单个 `Output`。
8. 用 [common/param/param.c](common/param/param.c) 追踪 INI。
9. 只有准备启用 NPU 时，再深入 `common/rockiva` 和 OSD。
10. 最后看 `common/isp/rv1106` 中本项目实际调用的初始化、应用配置和反初始化入口。

每读一个函数，建议回答三个问题：

- 输入是什么，来自哪个对象或线程？
- 它取得了什么资源，所有者是谁？
- 成功和失败时，资源分别在哪里释放？

## 16. 用一帧数据练习追代码

### 主视频帧

~~~text
ISP
→ VI0
→ RK_MPI_SYS_Bind
→ VENC0
→ RK_MPI_VENC_GetStream
→ RK_MPI_MB_Handle2VirAddr
→ PublisherSet::write_video(0, ...)
→ rtsp-main / rtmp-main
→ RK_MPI_VENC_ReleaseStream
~~~

### AI 子码流视频帧

~~~text
ISP
→ VI1
→ VENC1
→ PublisherSet::write_video(1, ...)
→ rtsp-ai / rtmp-ai
~~~

启用 NPU 后，同一个 VI1 还会经 `GetChnFrame + DMABUF` 送给 RockIVA，但这不是 VENC1 编码路径的替代，而是一条并行分支。

### 主音频包

~~~text
麦克风
→ AI0
→ VQE
→ AENC0
→ RK_MPI_AENC_GetStream
→ PublisherSet::write_audio(...)
→ rtsp-main / rtmp-main
→ RK_MPI_AENC_ReleaseStream
~~~

## 17. 调试时优先看什么

| 现象 | 优先检查 |
|---|---|
| 程序启动就报动态库 not found | `readelf -d`、RPATH/RUNPATH、`LD_LIBRARY_PATH`、库架构和 SONAME |
| ISP 启动失败 | IQ 路径、sensor 名、设备节点、RKAIQ 日志 |
| VENC 没数据 | VI/VENC 创建结果、Bind 结果、摄像头是否出帧 |
| RTSP 有画面但 RTMP `invalid body size` | Annex-B 数据地址、长度、SPS/PPS、是否重复加 `u32Offset` |
| RTMP 连接立刻断开 | MediaMTX 日志、H.264/H.265 配置、URL |
| 主码流没声音 | AI/AENC/VQE 日志、G711A 参数、主输出音频轨 |
| 退出时崩溃 | 线程是否先 join、通道是否被提前销毁 |
| AI 没框 | `enable_npu`、模型/IQ、RockIVA 回调、Overlay 绑定 |

MediaMTX 的 `RTP packets are too big ... remuxing them into smaller ones` 是服务器在重新分片的提示，通常不是推流失败原因。

## 18. 修改代码时的安全边界

- 不猜 SDK API，先在当前 SDK 头文件和官方 Demo 中搜索真实声明。
- 不大段重写 `common`，优先复用官方实现。
- 不让 FFmpeg 重新编码 VENC/AENC 数据。
- 不在释放 MPI stream 后继续使用其地址。
- 不在信号处理函数里调用复杂清理函数。
- 不给 RTMP 配 H.265。
- 修改构造流程时，同时检查失败回滚和 `stop()` 反向释放顺序。
- 修改完成后至少运行 `git diff --check`，再根据设备日志逐项验证。

## 19. 一张总调用表

| 调用者 | 被调用模块 | 目的 |
|---|---|---|
| `main()` | Logger | 初始化设备日志 |
| `main()` | Parameters | 加载 INI |
| `main()` | MediaRuntime | 启动完整媒体系统 |
| MediaRuntime | ISP wrapper | 初始化 SC3336/RKAIQ |
| MediaRuntime | RockIVA wrapper | 可选 NPU 检测 |
| MediaRuntime | RK MPI | 初始化媒体系统 |
| MediaRuntime | Video | 创建 VI/VENC/RGN 和线程 |
| MediaRuntime | Audio | 创建 AI/AENC/VQE 和线程 |
| Video 线程 | PublisherSet | 写入两路视频 |
| Audio 线程 | PublisherSet | 写入主码流音频 |
| PublisherSet | Output | 分发到四个独立 URL |
| Output | FFmpeg | 建连、封装和发送 |
| 所有模块 | EasyLogger | 输出控制台和文件日志 |

## 20. 最后记住这条主线

阅读本项目时，只要始终抓住下面四句话，就不容易迷路：

1. **INI 决定参数，MediaRuntime 负责装配。**
2. **Rockchip 硬件负责采集和编码，FFmpeg 只负责封装与网络。**
3. **每个线程取得数据后交给 Publisher，并在释放 MPI 缓冲前完成复制。**
4. **停止时先停线程，再拆通道，最后关闭 MPI、RockIVA 和 ISP。**
