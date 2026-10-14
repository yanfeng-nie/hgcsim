# 答题指南：大规模图聚类的异构计算技术

## 1. 题目

**标题**：大规模图聚类的异构计算技术

**题干**：大规模深度聚类根据其任务阶段的不同而表现出不同的负载特征，传统的同构计算系统在运行这类算法时往往因为计算、内存访问与数据搬运需求之间的不匹配而难以充分发挥硬件资源的效能。尤其在大规模图聚类任务中，特征提取、图结构构建与聚类迭代分别呈现出计算密集、内存密集和不规则访存等特征，单一计算架构难以兼顾各阶段的性能需求，且随着数据规模增长，图结构与节点特征的存储和搬运开销进一步增加。因此，如何依据不同阶段的负载特征合理分配计算任务与存储资源，发挥异构计算架构的协同优势，成为提升大规模图聚类效率与可扩展性的关键。

**问题**：如何针对大规模图聚类不同阶段在计算密度、内存访问模式和数据搬运需求上的差异，设计高效的异构计算任务调度与数据管理机制，以降低计算与存储资源之间的不匹配，提升大规模图聚类的执行效率与可扩展性？

## 2. 你手里的工具：HGCSim

HGCSim 是一个**体系结构级**的性能模拟器（C++ 实现，Python 网页前端）。它**不做真实的聚类计算**，而是根据数据集规模解析推导每个算子的运算量、访存量与访问模式，再在一台被完整建模的计算机上进行离散事件仿真：

- **计算设备**：CPU、GPU、NPU、TPU、FPGA、近内存计算（NMP/PIM）、可计算存储（CSD）、RRAM 存算一体、MRAM 存算一体
- **存储层次**：HBM、GDDR、DRAM、SCM（存储级内存）、Flash（NVMe SSD）
- **互连**：PCIe Gen1–7、CXL 1.1–4.0（含交换机、内存池化、P2P、零拷贝访存）、DDR 总线、NVLink
- **系统行为**：链路带宽争用（最大最小公平分配）、设备内存容量（LRU 缓存 + 写回 + OOM）、计算/传输重叠、分块流水线、全局同步

一次完整仿真只需毫秒级墙钟时间，所以你可以大量试错。

## 3. 赛道与评价（建议）

| 赛道 | 硬件 | 你要做的 | 建议评价指标 |
|---|---|---|---|
| **A. 调度赛道** | 固定为 `configs/systems/hetero_lab.json` | 只写调度策略（JSON 或 C++ 插件） | B1–B6 完成时间相对 `naive_gpu` 的几何平均加速比 |
| **B. 协同设计赛道** | 从 `configs/systems/codesign_budget.json` 出发自选硬件 | 硬件配置 + 调度策略 | 同上；系统必须满足预算（功耗 ≤ 2500W，成本 ≤ $60000） |

> 加分项：可扩展性曲线（R-MAT scale 18→24）、能耗、对“为什么快”的体系结构解释（用 Roofline、链路利用率、数据搬运分解说明）。

## 4. 快速开始

```bash
bash install_deps.sh              # 安装依赖：apt 编译工具 + 仓库内 .venv 中的 Python 包（只需一次）
make build                        # 编译 C++ 模拟器（写了插件后用 make build BUILD_NAME=<build名>）
bash scripts/run_frontend.sh      # 打开 http://localhost:8501
bash scripts/run_suite.sh         # 命令行跑全部基线（不需要前端）
```

依赖安装、编译选项（`make help`）、各运行脚本的全部参数与输出说明，见仓库根目录 [README.md](../README.md)。

网页前端页面：

1. **概览**：题目、流水线、基线任务的负载画像、配置文件位置
2. **系统配置**：选择/编辑/上传系统，增删设备，查看拓扑与推导出的性能指标，设置预算，保存为“我的系统配置”
3. **任务与策略**：选择任务，编辑策略 JSON（或指定 C++ 插件），保存为“我的策略”，运行
4. **结果分析**：甘特图、阶段跨度、设备/链路利用率、存储占用、数据搬运分解、Roofline、能耗、告警
5. **多配置对比**：最多 6 个运行（A~F）并排对比，可在多个系统配置上一次性运行同一任务
6. **基线测试套件**：多系统 × 多策略批量跑 B1–B6 与可扩展性扫描（墙钟上限 5 分钟）
7. **参数参考**：每个组件的全部可调参数、默认值、上下界

各页面的详细用法见第 12 节；默认配置下各选项的意义见第 13 节。

### 你的配置保存在哪里

| 类型 | 内置（只读） | 你自己的（前端保存 / 本地编辑） |
|---|---|---|
| 系统（硬件）配置 | `configs/systems/` | `configs/user/systems/` |
| 调度策略 | `configs/strategies/` | `configs/user/strategies/` |
| C++ 调度插件 | `plugins/example_scheduler.cpp` | `plugins/student/` |

- 在前端改完配置后点 **💾 保存到我的配置**，或直接在本地编辑上述目录中的 JSON 文件后点 **刷新列表**。
- 配置名 = 文件名，不能与内置配置重名；未保存的修改刷新页面就会丢失。
- 命令行套件可以直接用你的配置名：`bash scripts/run_suite.sh --systems hetero_lab --strategies naive_gpu my_strategy`。

## 5. 负载模型（你要调度的对象）

每个任务被切成 P 个分块（chunk），生成如下任务图：

```
X(特征) ──[特征提取: 编码器 GEMM，可含训练 epoch + 权重同步]──> Z(嵌入)
G(边表) ──[图划分(可选 METIS)]──[图构建: 排序/度统计/CSR]──> A(邻接 CSR)
                         或  Z ──[kNN 建图: 距离 GEMM + Top-k]──> A
A, Z ──[聚类迭代]──> 标签
          ├─ propagation：SpMM 图传播（每个分块需要其它分块的“halo”邻居行，按跳数做全局同步）
          └─ assignment ：K-means 分配 GEMM（k 很小的窄矩阵）+ 主节点中心更新；谱聚类为正交化 + 小规模特征分解
```

| 任务 | 数据集 | 规模 | 算法 | 特点 |
|---|---|---|---|---|
| B1 | Cora | 2.7K 点 / 1.1 万边，d=1433 | GAE 训练 200 轮 + K-means | 冒烟测试；卸载开销可能大于收益 |
| B2 | ogbn-arxiv | 17 万 / 233 万 | DEC 式，每轮都传播 | 迭代同步开销显著 |
| B3 | Reddit | 23 万 / 1.15 亿 | 谱聚类 | 稠密图（平均度 ≈ 492），SpMM 主导 |
| B4 | ogbn-products | 245 万 / 1.24 亿 | 编码器训练 10 轮 + 传播 + K-means | 三个阶段都有分量 |
| B5 | ogbn-papers100M | 1.1 亿 / 16 亿 | 编码器 1 轮 + 传播 + K-means | 原始数据 ≈ 70 GB，超出普通显存和内存，存储层次是关键 |
| B6 | SIFT1M | 100 万点，无图 | kNN 建图 + 谱聚类 | O(N²) 距离计算主导 |
| S18–S24 | R-MAT | 2^18 ~ 2^24 点，16 × 点数条边 | 传播 + K-means | 可扩展性曲线 |

## 6. 答题接口 1：JSON 策略

模板见 `configs/strategies/hetero_template.json`。全部字段如下，各字段省略时的缺省值与可选值见第 13 节“调度策略 JSON 的字段与缺省值”：

```jsonc
{
  "name": "my_strategy",
  "dataset_location": "ssd0",              // 数据集初始所在设备（SSD / CSD / 主机），缺省为第一块 SSD
  "placement": {
    "features": "home",                    // 特征 X 暂存到哪个存储（"home"=不暂存；如 "dram0"、"cxlmem0.mem"）
    "graph":    "home",                    // 原始边表 G 暂存位置
    "spill":    "auto"                     // 中间数据（Z/A/H…）的 home / 溢出层，"auto" 按容量自动选择
  },
  "partition": {
    "num_chunks": "auto",                  // 分块数 P（整数 1–1024，或 "auto" 按设备内存自动选择）
    "method": "range"                      // range（按编号）/ random / metis（主机上额外运行一次划分，割边少）
  },
  "pipeline": {
    "overlap": true,                       // 是否在执行当前 kernel 时预取后续任务的输入；false 时各阶段之间还有全局栅栏
    "prefetch_depth": 2,                   // 预取深度（同时准备的任务数）
    "halo_cache": false                    // halo 成员整块缓存到本地（多跳/多轮复用），否则逐行临时拉取
  },
  "stages": {
    "feature_extraction": {
      "devices": [{"id": "gpu0", "share": 0.7}, {"id": "npu0", "share": 0.3}],  // 份额决定分块分配比例
      "precision": "fp16",                 // fp32 / fp16 / bf16 / fp8 / int8 / int4（设备必须支持）
      "assign": "dynamic",                 // static（按份额连续分配）/ dynamic（分块就绪时按最早完成时间选设备）
      "zero_copy": false                   // 允许直接远程访存主机内存/CXL 内存（不搬运到本地）
    },
    "graph_construction": { "devices": ["cpu0"] },
    "clustering": {
      "devices": ["gpu0"], "precision": "fp16",
      "propagation": { "devices": ["fpga0"], "precision": "fp16" },   // 可选：单独指定 SpMM 传播
      "assignment":  { "devices": ["rram0"], "precision": "int8" }    // 可选：单独指定分配/正交化
    }
  },
  "leader": "cpu0"                         // 全局归约（中心更新、权重同步、特征分解）所在设备
}
```

## 7. 答题接口 2：C++ 调度插件

接口定义在 `include/hgcsim/scheduler.hpp`，模板 `plugins/example_scheduler.cpp`。

```cpp
class IScheduler {
  virtual std::string name() const = 0;
  // 静态规划：返回一份与第 6 节格式相同的策略 JSON
  virtual json plan(const SystemView& sys, const WorkloadView& wl, const json& args, const json& base) = 0;
  // 动态调度（stage 设为 "assign":"dynamic" 时每个分块回调），返回候选设备下标，-1 = 内置 EFT
  virtual int select_device(const TaskInfo& task, const std::vector<DeviceState>& candidates);
};
HGC_EXPORT_SCHEDULER(MyScheduler)
```

`SystemView` 提供：设备列表与类型、本地存储与容量、`stage_time(stage, dev, precision)`（该阶段在该设备上的估计计算时间，支持 `clustering.propagation` / `clustering.assignment` 子阶段）、任意两点间路径带宽/延迟/是否支持访存语义、`describe()`（完整系统描述，含每个设备的推导峰值与全部参数）。`WorkloadView` 提供数据集与算法配置、各阶段 FLOPs/访存量/算术强度。

编译与使用：

```bash
cp plugins/example_scheduler.cpp plugins/student/my_scheduler.cpp   # 文件名即插件名
make build BUILD_NAME=my_v1                                         # 生成 build/plugins/libmy_scheduler.so
```

策略 JSON 保存为 `configs/user/strategies/mine.json`：`{"name": "mine", "plugin": "build/plugins/libmy_scheduler.so", "plugin_args": {...}}`，然后**重启前端**（`Ctrl+C` 停止后重新运行 `bash scripts/run_frontend.sh`）。

### build 名称

- 未经任何修改的原始仓库，`make build` 的 build 名为 **default**。
- 只要添加了 `plugins/student/*.cpp` 或修改了模拟器源码，就必须用 `make build BUILD_NAME=<build名>` 给这次 build 起名，且不能叫 `default`（不区分大小写），否则编译会在配置阶段停止并提示。
- `make info` 查看当前 build 名；`make clean` 清除所有 build；`make rebuild BUILD_NAME=<build名>` 从头构建。
- 名称规则：以字母或数字开头，只含字母、数字、`.`、`_`、`-`，长度 1~40。
- 前端侧边栏显示当前 build 名与包含的调度插件；如果你改了插件但没有重新 build，侧边栏会提醒。

## 8. 模拟器刻画了哪些“坑”和“机会”

这些都是真实系统中的权衡，模拟器会如实体现——它们正是解题的切入点：

- **精度**：Tensor/Cube/MXU 矩阵单元在 FP16/BF16/INT8 下吞吐远高于 FP32；存算一体只支持 int8/int4 定点 GEMM。
- **形状**：TPU 的 256×256 脉动阵列、GPU 的 Tensor Core 都有分块粒度；簇数 k 很小的分配 GEMM 会严重浪费阵列。
- **不规则访存**：SpMM 的随机行访问受访问粒度放大（Flash 4KB、HBM/GDDR 32B）、缓存命中率（度分布偏斜）与并发请求数（Little 定律）限制；NPU/TPU 的 DMA 面向块访问，gather 效率低（TPU 可配 SparseCore）；FPGA 的定制访存引擎对随机访问更友好。
- **FPGA**：可驻留内核数受 LUT 限制，切换算子需要重配置（百毫秒级）。
- **存算一体**：权重需写入交叉阵列——RRAM 写入慢（µs/行）、耐久有限；MRAM 写入快、耐久高但单元位数低（需要更多位切片）。权重放不下时每次调用都要分轮重编程；ADC 位数不足会产生精度告警。
- **近内存计算**：内部带宽极高，但处理单元算力弱、浮点更弱；UPMEM 类单元之间不能直接通信，跨分区 halo 必须经主机中转。
- **可计算存储**：盘内带宽高于对外 PCIe 带宽，适合“就地过滤/压缩后再送出”；盘内算力有限。
- **CXL**：2.0 起支持交换与池化；3.0 起交换机下设备间可直接 P2P、内存可被多个加速器共享访问；4.0 支持 128GT/s 与捆绑端口。CXL 内存可被 CPU 与 CXL Type-2 设备**零拷贝**访问，但每次访存多出数百纳秒延迟。
- **PCIe 零拷贝**（UVA）可用但效率较低；每次 DMA 有微秒级建立开销，分块过细会被开销淹没。
- **容量**：设备本地内存放不下时按 LRU 逐出，脏数据写回到溢出层；放不下单个任务会直接 OOM。
- **划分**：range 划分零开销但割边多；METIS 类划分割边少但要在主机上额外花时间；只有跨设备或数据不驻留时，割边才真正产生搬运。
- **阶段并行**：有图时“特征提取”与“图构建”没有依赖，可在不同设备上同时运行。

## 9. 结果指标

- `makespan_s`：仿真完成时间（主指标）
- `stages`：各阶段起止时间与各设备忙碌时间
- `devices` / `links` / `memories`：利用率、搬运量、峰值内存占用
- `transfers.by_kind`：fetch（按需拉取）、halo（跨分区）、writeback（逐出写回）、stage_in（暂存）、zero_copy（零拷贝访存）、stream（流式读入）
- `energy`：计算 / 传输 / 静态能耗
- `roofline`：每类 kernel 的算术强度与实测吞吐
- `warnings`：精度损失、阵列容量不足、耐久消耗等

全部结果字段见第 13 节“仿真结果字段”。

## 10. 建议提交物

1. 策略文件（`configs/user/strategies/*.json`）或插件源码（`plugins/student/*.cpp`），以及协同设计赛道的系统配置（`configs/user/systems/*.json`），并注明 build 名
2. `bash scripts/run_suite.sh --systems hetero_lab --strategies naive_gpu 你的策略 --out results.csv` 的结果（终端会直接给出相对 `naive_gpu` 的 B1–B6 几何平均加速比）
3. 一份简短报告：各阶段负载特征分析 → 映射与数据管理决策 → 实验结果 → 体系结构层面的解释

## 11. 常见错误

| 报错 | 原因与处理 |
|---|---|
| `OOM: 任务 ... 需要 X GB 工作内存` | 单个分块放不进设备内存：增大 `num_chunks` 或换设备 |
| `仿真停滞 ... 存储容量不足而阻塞` | 并发预取占满内存：减小 `prefetch_depth`、增大分块数，或把 `spill` 放到更大的层级 |
| `... 无可用设备` | 该设备不支持该算子/精度（如 SpMM 放到存算一体、FP8 放到不支持的 GPU） |
| `主机 CPU 不支持 CXL x.x` | 主机 `cxl_max_version` 太低，或需要先添加 CXL 交换机 |
| `超出预算` | 协同设计赛道中总功耗/成本超过上限 |

## 12. 前端使用说明

启动：`bash scripts/run_frontend.sh`，浏览器打开 `http://localhost:8501`（端口等启动参数见仓库根目录 [README.md](../README.md#网页前端-run_frontendsh)）。

### 侧边栏

- **当前 build**：当前加载的 build 名、版本、编译时间与包含的调度插件。源码或插件在 build 之后被修改过时，这里会显示黄色提醒。
- **页面**：①~⑦ 七个页面，见下文。
- **当前系统 / 当前策略**：“② 系统配置”和“③ 任务与策略”页正在编辑的配置名。带“（有未保存修改）”说明改动还没有保存成文件。
- **运行记录**：本次会话中运行过的仿真条数。“④ 结果分析”和“⑤ 多配置对比”页从这里选取结果。

> 前端的编辑内容和运行记录都保存在浏览器会话里，**刷新页面就会清空**。要长期保留，请保存为“我的配置”，或下载结果 JSON / CSV。

### ① 概览

- 题目、模拟器简介，以及两种答题方式（JSON 策略 / C++ 插件）和协同设计赛道的入口说明。
- **配置文件放在哪里**：内置配置与“我的配置”的目录（同第 4 节“你的配置保存在哪里”）。
- **流水线与负载特征**：数据加载 → 特征提取（计算密集）→ 图构建（内存密集）→ 聚类迭代（不规则访存 + 迭代同步），以及每个阶段对应的策略字段。
- **基线任务**：B1–B6 的规模，以及模拟器解析出的各阶段 GFLOP、数据量、随机访问次数。可以借此判断每个任务的瓶颈在哪个阶段。

### ② 系统配置

编辑“硬件”：主机、设备、存储、链路、交换机、预算。

1. **顶部工具栏**
   - “选择系统配置”下拉框列出内置配置（标“（内置）”）和我的配置（标“（我的）”）。
   - 点“载入（覆盖当前编辑）”开始编辑。
   - 在本地新增或修改了 JSON 文件后，点“刷新列表”。
2. **从 JSON 文件上传 / 下载当前配置**：上传一个系统 JSON 直接使用，或把当前编辑内容下载为文件。
3. **状态行**
   - 正在编辑的配置名与来源，以及是否有未保存的修改；
   - 校验结果：绿色表示所有参数在上下界内、关系约束满足；红色会列出具体错误。
4. **保存为我的系统配置**
   - 填写配置名，点“💾 保存到我的配置”，写入 `configs/user/systems/<名称>.json`；
   - 同名的“我的配置”需要勾选“覆盖同名配置”才会被覆盖；
   - 不能与内置配置重名；
   - 编辑的是“我的配置”时，会多出“🗑 删除我的配置”按钮。
5. **概要指标**：计算/存储设备数、估算总成本、估算峰值功耗，以及预算是否满足（未启用预算时显示“未启用”）。
6. **四个标签页**
   - **拓扑**：树状拓扑图，显示主机、交换机、设备，链路上标注类型与带宽。
   - **设备 / 存储 / 链路**：三张表。
     - 设备：各精度峰值算力、本地存储、功耗、成本；
     - 存储：容量、峰值 / 顺序带宽、延迟、访问粒度、是否字节寻址；
     - 链路：原始 / 有效带宽、延迟、是否全双工、是否支持访存语义（零拷贝）。
   - **编辑参数**
     - 在“选择组件”中选择主机 CPU、主机内存、某个设备、设备存储、挂载链路、交换机或交换机链路，修改后点“应用修改”；
     - 输入框限定在合法上下界内，鼠标悬停可以看到参数说明与默认值；
     - 设备存储可以切换存储类型（例如 GDDR ↔ HBM）；挂载链路可以切换链路类型（PCIe / CXL / DDR / NVLink）和挂载点（主机或某个交换机）。
   - **增删设备与预算**
     - 从模板添加设备或交换机：填写新设备 id 后点“添加”；
     - 删除设备或交换机；
     - 更换主机 CPU 模板；
     - 设置预算约束：协同设计赛道启用后，填写功耗与成本上限并点“应用”；
     - 编辑系统说明（保存在配置的 `description` 字段）。

> 修改参数后记得在页面上方保存。数值越界的情况在输入时就被阻止；参数之间的关系约束由 C++ 校验器检查，例如 CXL 版本特性、挂载点是否支持该链路、预算。

### ③ 任务与策略

页面顶部显示当前系统（在“② 系统配置”页修改）。

1. **选择任务**
   - **B1~B6 基线任务**：下方会给出各阶段的 GFLOP、访存量、随机访问次数、算术强度。
   - **R-MAT 合成图**：用滑块选择 scale（节点数 = 2^scale，边数 = 16 × 节点数）。
   - **自定义 workload JSON**：直接编辑任务参数，字段见第 13 节“负载（workload）字段”，各基线任务见第 5 节。
2. **选择 / 编辑策略**
   - 下拉选择策略后点“载入”，策略 JSON 会出现在编辑框里，可以直接修改；
   - 含 `"plugin"` 字段的策略由 C++ 插件规划；
   - “刷新列表”用于加载本地新建的策略文件。
3. **保存为我的策略**：规则同系统配置，写入 `configs/user/strategies/<名称>.json`。
4. **运行**
   - **▶ 运行仿真**：在当前系统上运行。完成后显示完成时间、能耗、平均功率、数据搬运量、分块数与任务数，以及各阶段起止时间图和告警，并加入运行记录。
   - **仅查看规划结果**：不运行仿真，只显示最终生效的策略。插件规划结果、自动分块数、`auto` 字段都会被解析成具体值。用插件时可以借此检查插件到底做了什么决定。

### ④ 结果分析

在“选择运行记录”中挑选一次运行，查看汇总指标与六个标签页：

| 标签页 | 内容 |
|---|---|
| 阶段与时间线 | 各阶段起止时间（阶段之间可以重叠）；设备执行甘特图（可拖选放大）；各阶段在每个设备上的忙碌时间表 |
| 设备与链路 | 计算设备利用率；互连链路利用率（取较繁忙方向）与上下行数据量；每个设备的忙碌时间、kernel 数、动态 / 静态能耗、存算一体耐久消耗 |
| 存储与数据搬运 | 各存储层级的峰值占用与容量；按类型分解的数据搬运量（按需拉取、halo、写回、暂存、零拷贝、流式读入）；能耗分解（计算 / 传输 / 静态） |
| Roofline | 每次选一个设备，画出该设备各精度、各计算单元的屋顶线，以及在其上运行的 kernel 点。点上的编号对应下表，表中列出算术强度、实测吞吐与该精度的峰值 |
| 策略与告警 | 全部告警（精度损失、阵列容量不足、耐久消耗等）；任务图摘要（分块数、割边比例、halo 行数、溢出层等）；最终生效的策略 |
| 原始结果 | 下载完整结果 JSON，并以树形查看 |

### ⑤ 多配置对比

最多同时对比 **6 个**运行，依次标记为 A~F。

- **对比来源一：在多个系统配置上运行同一任务**
  - 勾选最多 6 个系统（可以包括“当前编辑中的系统”）；
  - 选择一个任务和一个策略（所有系统用同一策略），点“▶ 运行对比”。
  - JSON 策略里写死了设备 id，只适用于包含这些设备的系统；C++ 插件策略会按每个系统自动规划。
- **对比来源二：从运行记录中选择**：例如同一系统上的不同策略。

对比视图包括：

- **汇总表**：完成时间、相对 A 的加速比、能耗、平均功率、数据搬运、分块、系统成本与峰值功耗、告警数。
- **四个标签页**：
  - 完成时间与阶段；
  - 设备利用率热力图（“—”表示该系统没有这个设备）；
  - 数据搬运与能耗；
  - 逐个查看：各运行的阶段图使用同一时间轴。
- 可下载对比汇总 CSV。所选运行不是同一任务时，页面会提醒，因为不同任务的完成时间不能直接比较。

### ⑥ 基线测试套件

前端版的 `run_suite.sh`：

- **选择**
  - 多选系统和策略，第一个策略作为加速比基准；
  - 可以勾选“同时运行正在编辑的系统 / 策略”（未保存的修改也能参与）；
  - 可以选择是否包含 R-MAT 可扩展性扫描。
- **运行**：点“▶ 运行套件”。页面显示进度，并检查是否在 300 秒墙钟上限内完成。
- **结果**
  - 完成时间透视表；
  - 每个系统一张各策略完成时间图；
  - 相对基准策略的加速比表；
  - 失败的仿真列表；
  - 可下载 CSV / JSON。

### ⑦ 参数参考

按类别（计算设备 / 存储层级 / 互连链路 / 交换机）和组件类型，列出全部可调参数的默认值、上下界、单位与说明。所有参数都在 C++ 中定义并校验，上界取自截至 2026 年的公开器件参数，出处见 [PARAM_SOURCES.md](PARAM_SOURCES.md)。峰值算力、带宽等性能指标都由这些结构参数推导，不能直接填写。

## 13. 默认配置下各选项的意义

### 默认状态一览

| 项目 | 默认值 | 含义 |
|---|---|---|
| build 名 | `default` | 未修改源码、没有学生插件时的 build 名 |
| 编译选项 | `BUILD_TYPE=Release`、`TEST=1`、`JOBS=CPU 核数` | 见仓库根目录 [README.md](../README.md#编译-makefile) |
| 前端地址 | `http://localhost:8501`，监听所有网卡 | `run_frontend.sh --port/--address` 可改 |
| 前端初始系统 | `default_pc` | 一台普通台式机，见本节“默认系统 default_pc 的各项参数” |
| 前端初始策略 | `naive_gpu` | 朴素 CPU+GPU 卸载基线，见本节“调度策略 JSON 的字段与缺省值” |
| 套件默认系统 | `default_pc`、`hetero_lab` | `run_suite.sh` 不带 `--systems` 时 |
| 套件默认策略 | `naive_cpu`、`naive_gpu`、`hetero_template` | `run_suite.sh` 不带 `--strategies` 时；第一个为加速比基准 |
| 套件墙钟上限 | 300 秒 | 超过即停止并判为“超时” |
| 多配置对比上限 | 6 个运行 | A~F |
| 我的配置目录 | `configs/user/` | 环境变量 `HGCSIM_USER_CONFIG_DIR` 可改 |
| 模拟器库位置 | `build/libhgcsim.so` | 环境变量 `HGCSIM_LIB` 可指定其它文件 |
| Python 环境 | 仓库内 `.venv/` | 运行脚本自动使用 |

### 系统配置文件的结构

以默认系统 `configs/systems/default_pc.json` 为例：

```jsonc
{
  "name": "default_pc",                 // 配置名（与文件名一致）
  "description": "普通台式计算机……",     // 说明文字
  "host": {                             // 主机：每个系统有且只有一个
    "id": "cpu0",                       // 设备 id（策略中用它指代主机 CPU）
    "params": {},                       // CPU 参数；空对象 = 全部取默认值（见下文 default_pc 参数表）
    "memory": {"id": "dram0", "type": "DRAM", "params": {}}   // 主机内存
  },
  "devices": [                          // 其它设备（计算设备、SSD、内存扩展器……）
    {
      "id": "gpu0", "type": "GPU", "params": {},              // 设备 id、类型、参数
      "memory": {"type": "GDDR", "params": {}},               // 设备本地存储（id 自动为 gpu0.mem）
      "attach": {"to": "cpu0",                                // 挂载点：主机或某个交换机 id
                 "link": {"type": "PCIe", "params": {"gen": 4, "lanes": 16}}}   // 挂载链路
    },
    {
      "id": "ssd0", "type": "SSD", "params": {},
      "memory": {"type": "FLASH", "params": {}},              // 闪存介质（id 自动为 ssd0.flash）
      "attach": {"to": "cpu0", "link": {"type": "PCIe", "params": {"gen": 4, "lanes": 4}}}
    }
  ],
  "switches": [],                       // 可选：PCIe / CXL 交换机，设备可以挂到交换机下
  "budget": {"enabled": false, "power_w": 0, "cost_usd": 0}  // 预算约束（协同设计赛道启用）
}
```

- `params` 中**没有写出的参数一律取默认值**，写出的参数会被校验是否在上下界内。所以 `default_pc` 的大部分参数是空对象，它就是“全部默认值”构成的普通计算机。
- 设备类型（`type`）：
  - 计算设备：`CPU`、`GPU`、`NPU`、`TPU`、`FPGA`、`NMP`（近内存计算）、`CSD`（可计算存储）、`CIM_RRAM`、`CIM_MRAM`（存算一体）；
  - 纯存储设备：`SSD`、`MEM_EXPANDER`（CXL / DIMM 内存扩展）。
- 存储类型：`HBM`、`GDDR`、`DRAM`、`SCM`、`FLASH`；链路类型：`PCIe`、`CXL`、`DDR`、`NVLink`；交换机类型：`PCIe`、`CXL`。
- 各类型的全部参数可在前端“⑦ 参数参考”页或 `./build/hgcsim_cli catalog` 中查看。

### 默认系统 default_pc 的各项参数

`default_pc` 模拟一台普通台式机：

- 8 核 4.0 GHz CPU（AVX2）；
- DDR5-5600 双通道 32 GB 内存；
- 消费级独显：46 SM，12 GB GDDR6X，PCIe 4.0 x16；
- PCIe 4.0 x4 NVMe SSD，2 TB。

没有挂载任何异构加速器，也没有 HBM、SCM、CXL。这也是模拟器全部参数的默认值。下表中“默认值”即 `default_pc` 实际使用的值，“取值范围”是模拟器允许的上下界（来自公开器件参数）。

**主机 CPU（`host.params`，类型 CPU）**

| 参数 | 默认值 | 单位 | 取值范围 | 含义 |
|---|---|---|---|---|
| `cores` | 8 | 个 | 1 ~ 384 | 物理核数（单路最高 192，双路 384） |
| `freq_ghz` | 4 | GHz | 0.8 ~ 6 | 全核频率 |
| `simd_bits` | 256 | bit | 128、256、512 | SIMD 宽度 (SSE/AVX2/AVX-512) |
| `fma_units` | 2 | 个 | 1 ~ 4 | 每核 FMA 单元数 |
| `amx` | 0 | — | 0 ~ 1 | 是否具备 AMX 矩阵扩展 (BF16/INT8) |
| `llc_mb` | 32 | MB | 1 ~ 1152 | 末级缓存容量（3D V-Cache 最高 1152MB） |
| `lfb_per_core` | 12 | 个 | 4 ~ 64 | 每核可并发的未命中请求数 |
| `launch_overhead_us` | 2 | us | 0.1 ~ 10000 | kernel 启动/调度开销 |
| `max_link_gen` | 5 | — | 1 ~ 7 | 设备侧支持的最高 PCIe 代数 |
| `max_lanes` | 16 | — | 1、2、4、8、16 | 设备侧最大通道数 |
| `cxl_max_version` | 0 | — | 0、1.1、2、3、3.1、3.2、4 | 设备支持的最高 CXL 版本（0 = 不支持 CXL） |
| `pj_per_flop` | 20 | pJ | 0 ~ 500 | FP32 单次运算能耗（低精度按位宽线性缩放；纯存储设备为 0） |

**主机内存 dram0（`host.memory`，类型 DRAM）**

| 参数 | 默认值 | 单位 | 取值范围 | 含义 |
|---|---|---|---|---|
| `channels` | 2 | 个 | 1 ~ 24 | 内存通道数 (双路 12 通道服务器=24) |
| `data_rate_mtps` | 5600 | MT/s | 1600 ~ 8800 | 数据速率 (DDR5 最高 6400, MRDIMM 8800) |
| `channel_width_bits` | 64 | bit | 16、32、64 | 通道位宽 (DDR=64, LPDDR=16/32) |
| `capacity_gb` | 32 | GB | 1 ~ 12288 | 容量 |
| `efficiency` | 0.75 | — | 0.3 ~ 0.95 | 顺序访问可达带宽比例 |
| `random_efficiency` | 0.35 | — | 0.05 ~ 0.9 | 随机访问带宽利用率 |
| `latency_ns` | 90 | ns | 50 ~ 250 | 加载到使用延迟 |
| `granularity_bytes` | 64 | B | 32、64 | 缓存行粒度 |
| `pj_per_bit` | 15 | pJ/bit | 3 ~ 40 | 访问能耗（含 I/O） |

**独立显卡 gpu0（`devices[0].params`，类型 GPU）**

| 参数 | 默认值 | 单位 | 取值范围 | 含义 |
|---|---|---|---|---|
| `sms` | 46 | 个 | 1 ~ 256 | 流多处理器数 (Rubin: 224) |
| `freq_ghz` | 2.475 | GHz | 0.5 ~ 3 | 加速频率 |
| `fp32_flops_per_sm_cycle` | 256 | FLOP | 64、128、256、512 | 每 SM 每周期 FP32 运算数 |
| `tensor_fp16_flops_per_sm_cycle` | 1024 | FLOP | 0 ~ 32768 | 每 SM 每周期 FP16 Tensor 稠密运算数 (B200≈8192) |
| `fp8_ratio` | 2 | × | 0 ~ 2 | FP8 相对 FP16 Tensor 吞吐倍数 (0=不支持) |
| `int8_ratio` | 2 | × | 0 ~ 2 | INT8 相对 FP16 Tensor 吞吐倍数 |
| `int4_ratio` | 4 | × | 0 ~ 4 | INT4 相对 FP16 Tensor 吞吐倍数 (0=不支持) |
| `l2_mb` | 36 | MB | 0.5 ~ 512 | L2 缓存容量 |
| `outstanding_per_sm` | 256 | 个 | 16 ~ 2048 | 每 SM 可并发访存请求 |
| `launch_overhead_us` | 5 | us | 0.1 ~ 10000 | kernel 启动/调度开销 |
| `max_link_gen` | 4 | — | 1 ~ 7 | 设备侧支持的最高 PCIe 代数 |
| `max_lanes` | 16 | — | 1、2、4、8、16 | 设备侧最大通道数 |
| `cxl_max_version` | 0 | — | 0、1.1、2、3、3.1、3.2、4 | 设备支持的最高 CXL 版本（0 = 不支持 CXL） |
| `pj_per_flop` | 6 | pJ | 0 ~ 500 | FP32 单次运算能耗（低精度按位宽线性缩放；纯存储设备为 0） |

**显存 gpu0.mem（`devices[0].memory`，类型 GDDR）**

| 参数 | 默认值 | 单位 | 取值范围 | 含义 |
|---|---|---|---|---|
| `bus_width_bits` | 192 | bit | 32、64、96、128、160、192、256、320、384、448、512 | 显存总线位宽 |
| `gbps_per_pin` | 21 | Gb/s | 8 ~ 36 | 每引脚速率 (GDDR6 16~20, GDDR6X 21~24, GDDR7 28~36) |
| `capacity_gb` | 12 | GB | 1 ~ 96 | 显存容量 |
| `efficiency` | 0.85 | — | 0.3 ~ 0.98 | 顺序访问可达带宽比例 |
| `random_efficiency` | 0.4 | — | 0.05 ~ 0.9 | 随机访问带宽利用率 |
| `latency_ns` | 350 | ns | 150 ~ 900 | 加载到使用延迟 |
| `granularity_bytes` | 32 | B | 32、64 | 最小访问粒度 |
| `pj_per_bit` | 7 | pJ/bit | 3 ~ 15 | 访问能耗 |

**固态硬盘 ssd0（`devices[1].params`，类型 SSD）**

| 参数 | 默认值 | 单位 | 取值范围 | 含义 |
|---|---|---|---|---|
| `launch_overhead_us` | 0.1 | us | 0.1 ~ 10000 | kernel 启动/调度开销 |
| `max_link_gen` | 4 | — | 1 ~ 7 | 设备侧支持的最高 PCIe 代数 |
| `max_lanes` | 4 | — | 1、2、4、8、16 | 设备侧最大通道数 |
| `cxl_max_version` | 0 | — | 0、1.1、2、3、3.1、3.2、4 | 设备支持的最高 CXL 版本（0 = 不支持 CXL） |
| `pj_per_flop` | 0 | pJ | 0 ~ 500 | FP32 单次运算能耗（低精度按位宽线性缩放；纯存储设备为 0） |

**闪存介质 ssd0.flash（`devices[1].memory`，类型 FLASH）**

| 参数 | 默认值 | 单位 | 取值范围 | 含义 |
|---|---|---|---|---|
| `nand_channels` | 8 | 个 | 1 ~ 32 | NAND 通道数 |
| `channel_mtps` | 2400 | MT/s | 200 ~ 4800 | NAND 接口速率 (ONFI 5.x 3600) |
| `dies` | 64 | 个 | 1 ~ 1024 | NAND die 总数（并行度） |
| `page_kb` | 16 | KB | 4、8、16、32 | NAND 页大小 |
| `read_latency_us` | 50 | us | 3 ~ 200 | 页读延迟 tR (TLC≈50, SLC/XL-Flash≈3~5) |
| `program_latency_us` | 600 | us | 20 ~ 3000 | 页编程延迟 tProg |
| `capacity_tb` | 2 | TB | 0.064 ~ 256 | 容量 |
| `controller_iops_m` | 1.2 | M IOPS | 0.05 ~ 10 | 主控 4KB 随机读 IOPS 上限 |
| `controller_bw_gbps` | 14 | GB/s | 0.5 ~ 64 | 主控后端带宽上限 |
| `pj_per_bit` | 100 | pJ/bit | 10 ~ 1000 | 访问能耗 |

**挂载链路（`devices[*].attach.link`，类型 PCIe；gpu0 为 x16、ssd0 为 x4）**

| 参数 | 默认值 | 单位 | 取值范围 | 含义 |
|---|---|---|---|---|
| `gen` | 4 | — | 1 ~ 7 | PCIe 代数 (Gen7=128GT/s, 2025 年发布) |
| `lanes` | 16 | — | 1、2、4、8、16 | 通道数 |
| `max_payload_bytes` | 256 | B | 128、256、512、1024、2048、4096 | 最大 TLP 负载 |
| `dma_efficiency` | 0.92 | — | 0.5 ~ 1 | DMA 引擎效率 |
| `latency_ns` | 400 | ns | 50 ~ 2000 | 单程延迟（含根复合体） |
| `dma_setup_us` | 5 | us | 0.5 ~ 50 | 单次 DMA 传输建立开销（驱动/API 调用） |
| `zero_copy_efficiency` | 0.6 | — | 0.1 ~ 1 | 零拷贝（UVA/P2P 访存）相对 DMA 带宽效率 |
| `pj_per_bit` | 5 | pJ/bit | 1 ~ 20 | 传输能耗 |

由这些参数推导出的主要指标（前端“② 系统配置 → 设备 / 存储 / 链路”页可以看到同样的数值）：

| 组件 | 推导指标 |
|---|---|
| CPU cpu0 | FP32 峰值 ≈ 1.02 TFLOPS（8 核 × 4.0 GHz × 256 bit / 32 × 2 FMA × 2），INT8 ≈ 4.1 TOPS |
| 内存 dram0 | 32 GB，峰值 89.6 GB/s，顺序读 ≈ 67.2 GB/s，延迟 90 ns |
| GPU gpu0 | FP32 ≈ 29.1 TFLOPS，FP16 Tensor ≈ 116.6 TFLOPS，INT8 ≈ 233 TOPS |
| 显存 gpu0.mem | 12 GB，峰值 504 GB/s，顺序读 ≈ 428 GB/s |
| SSD ssd0 | 2 TB，介质顺序读 ≈ 12.6 GB/s，但受 PCIe 4.0 x4 链路限制，有效 ≈ 6.3 GB/s |
| 链路 gpu0↔cpu0 | PCIe 4.0 x16，原始 32 GB/s，扣除编码、TLP / DLLP 与 DMA 开销后有效 ≈ 25.2 GB/s |
| 整机 | 估算成本 ≈ $1,600，峰值功耗 ≈ 308 W，空闲功耗 ≈ 48 W |

### 其它内置系统与元件模板

| 系统 | 用途 | 组成 |
|---|---|---|
| `default_pc` | 默认状态、性能基线 | 见上文“默认系统 default_pc 的各项参数” |
| `hetero_lab` | **调度赛道的标准答题平台**（硬件固定） | 32 核服务器 CPU（AVX-512 + AMX，CXL 3.0）+ DDR5-4800 8 通道 256 GB；L40S 级 GPU 48 GB；昇腾级 NPU（64 GB HBM，经 CXL 3.0 交换机）；v5e 级 TPU；Alveo V80 级 FPGA；UPMEM 级近内存计算（DDR 总线）；SmartSSD 级可计算存储 4 TB；RRAM 与 MRAM 存算一体卡；CXL 交换机下 512 GB DRAM 扩展器与 1 TB SCM 扩展器；PCIe 5.0 NVMe 8 TB。估算 ≈ $46k、≈ 2.0 kW |
| `codesign_budget` | **协同设计赛道的起点** | 硬件与 `default_pc` 相同，但启用了预算约束：峰值功耗 ≤ 2500 W、成本 ≤ $60000 |

元件模板在前端“增删设备与预算”标签页中使用，共两类：

- **设备与交换机**（`configs/devices/`，25 个）：
  - GPU：RTX 4070、L40S、A100、B200、Rubin 级；
  - NPU：昇腾 PCIe 版、CXL Type-2 版；
  - TPU：v5e、Ironwood 级；
  - FPGA：Alveo V80；
  - 近内存计算：UPMEM、HBM-PIM、CXL-PNM；
  - 可计算存储：SmartSSD；
  - 存算一体：RRAM、MRAM；
  - 内存扩展：CXL 内存扩展（经交换机 / 直连）、CXL SCM、DIMM 持久内存；
  - SSD：PCIe 4.0 / 5.0 / 6.0；
  - 交换机：CXL 3.0、PCIe 5.0。
- **主机**（`configs/hosts/`）：桌面 8 核（默认）、服务器 32 核、EPYC 192 核级。

每个模板的说明里注明了前置条件，例如“需先添加 cxlsw0”“需主机 max_link_gen≥6”。

### 调度策略 JSON 的字段与缺省值

策略决定“哪个阶段在哪个设备上、用什么精度、数据放在哪里、如何分块与流水”。下表给出每个字段的含义，以及**字段省略时**模拟器使用的缺省值：

| 字段 | 省略时 | 可选值 | 含义 |
|---|---|---|---|
| `name` | `unnamed` | 字符串 | 策略名 |
| `description` | — | 字符串 | 说明文字（不影响仿真） |
| `dataset_location` | 第一块 SSD（没有 SSD 时依次为 CSD、主机） | 设备 id 或存储 id | 数据集（特征 X、边表 G）初始所在位置 |
| `placement.features` | `home` | `home` / 存储 id（如 `dram0`、`cxlmem0.mem`）/ 设备 id | 特征 X 是否先整体暂存到某个存储；`home` 表示不暂存，直接从数据集位置读取 |
| `placement.graph` | `home` | 同上 | 原始边表 G 的暂存位置 |
| `placement.spill` | `auto` | `auto` / 存储 id / 设备 id | 中间数据（嵌入 Z、邻接 A、传播结果 H 等）的 home 与溢出层。`auto`：工作集小于主机内存一半时用主机内存，否则选主机可见的最大容量内存（如 CXL 扩展），仍放不下时用数据集所在闪存 |
| `partition.num_chunks` | `auto` | 1~1024 的整数或 `auto` | 分块数 P；`auto` 按参与设备的内存容量与预取深度自动选择 |
| `partition.method` | `range` | `range` / `random` / `metis` | 图划分方式。`range` 按编号切分（零开销，割边取决于数据局部性）；`random` 割边最多；`metis` 割边少，但要在主机上额外运行一次划分 |
| `pipeline.overlap` | `false` | `true` / `false` | 执行当前 kernel 时是否预取后续任务的输入；`false` 时各阶段之间还有全局栅栏 |
| `pipeline.prefetch_depth` | `1` | 0~16 | 同时处于“准备 / 就绪 / 执行”窗口的任务数（`overlap=true` 时生效）；越大越能隐藏传输，但占用更多设备内存 |
| `pipeline.halo_cache` | `false` | `true` / `false` | 跨分区的 halo 邻居行是否整块缓存在本地、多跳 / 多轮复用；`false` 时每次临时拉取 |
| `stages.<阶段>.devices` | `["cpu0"]`（主机） | 设备 id 列表，或 `{"id":…, "share":…}` 列表 | 该阶段使用的设备与份额。份额按比例分配分块，只写 id 时份额为 1 |
| `stages.<阶段>.precision` | `fp32` | `fp32` / `fp16` / `bf16` / `fp8` / `int8` / `int4` | 计算精度；所选设备必须支持 |
| `stages.<阶段>.assign` | `static` | `static` / `dynamic` | `static`：按份额连续分配分块；`dynamic`：分块就绪时按最早完成时间选设备（插件可通过 `select_device` 接管） |
| `stages.<阶段>.zero_copy` | `false` | `true` / `false` | 允许直接远程访存主机内存或 CXL 内存，而不先搬到本地（需要链路支持访存语义） |
| `stages.clustering.propagation` | 继承 `clustering` | 同上 | 单独指定聚类中的 SpMM 图传播（不规则访存） |
| `stages.clustering.assignment` | 继承 `clustering` | 同上 | 单独指定聚类中的簇分配 GEMM / 正交化（窄矩阵） |
| `leader` | 主机 `cpu0` | 设备 id | 全局归约所在设备，包括 K-means 中心更新、编码器权重同步、谱聚类的小规模特征分解 |
| `plugin` | — | `.so` 路径（相对仓库根目录） | 指定后由 C++ 插件生成上述所有字段；JSON 中的其它字段作为 `base` 传给插件 |
| `plugin_args` | `{}` | 任意 JSON 对象 | 原样传给插件的 `plan()` |

`<阶段>` 为 `feature_extraction`（特征提取）、`graph_construction`（图构建）、`clustering`（聚类迭代）。完整的带注释示例见第 6 节与 `configs/strategies/hetero_template.json`。

**内置策略**

| 策略 | 含义 |
|---|---|
| `naive_cpu` | 全部阶段在主机 CPU 上，FP32，不重叠，range 划分（同构基线） |
| `naive_gpu` | 特征提取与聚类放 GPU、图构建放 CPU，FP32，不重叠（朴素卸载基线，**调度赛道的加速比基准**） |
| `hetero_template` | JSON 策略模板：GPU + FP16 + 计算 / 传输重叠，展示全部可配置项，答题时在此基础上修改 |
| `example_plugin` | 加载示例插件 `plugins/example_scheduler.cpp`：每个阶段选单个最快设备，演示插件 API |

注意：`naive_*` 和 `hetero_template` 里写了具体设备 id（`gpu0` 等），只适用于含这些设备的系统。插件策略会按每个系统自动规划。

### 负载（workload）字段

| 字段 | 省略时 | 含义 |
|---|---|---|
| `dataset.nodes` / `edges` | 必填 / 0 | 节点数、边数 |
| `dataset.feature_dim` | 必填 | 节点特征维度 d |
| `dataset.clusters` | 必填 | 簇数 k |
| `dataset.has_graph` | `true` | `false` 表示没有图，需要用 kNN 从嵌入建图 |
| `dataset.knn_k` | 10 | kNN 建图时每点的邻居数 |
| `dataset.degree_skew` | 0.5 | 度分布偏斜（0~0.95）；越大，热点行越集中，缓存命中率越高 |
| `dataset.locality` | 0.3 | 编号局部性（0~1）；越大，range 划分的割边越少 |
| `dataset.metis_cut_base` | 0.1 | METIS 划分的割边基数（割边比例 ≈ base × log2 P） |
| `dataset.feature_bytes` / `index_bytes` | 4 / 4 | 原始特征、索引的字节数 |
| `algorithm.encoder_dims` | 必填 | 编码器各层输出维度，最后一维为嵌入维 h |
| `algorithm.encoder_epochs` | 0 | 编码器训练轮数；0 表示只做推理 |
| `algorithm.train_flops_factor` | 6 | 训练一轮相对推理的 FLOPs 倍数 |
| `algorithm.method` | `kmeans` | `kmeans`（传播 + K-means）或 `spectral`（谱聚类：Lanczos 迭代 + K-means） |
| `algorithm.iterations` | 20 | K-means 或 Lanczos 迭代轮数 |
| `algorithm.prop_hops` | 2 | 图传播跳数 |
| `algorithm.prop_every_iter` | `false` | 是否每轮迭代都做传播 |
| `algorithm.lanczos_block` | 32 | 谱聚类的块大小 |
| `algorithm.kmeans_after` | 10 | 谱聚类之后的 K-means 轮数 |
| `algorithm.knn_candidate_ratio` | 1.0 | kNN 建图时候选点占全部点的比例（<1 表示近似搜索） |

各基线任务的规模、算法与特点见第 5 节。

### 仿真结果字段

| 字段 | 含义 |
|---|---|
| `status` / `errors` / `warnings` | `ok` 或 `error`；错误原因；告警（精度损失、阵列容量不足、耐久消耗等） |
| `makespan_s` | **仿真完成时间**（秒，主指标），是被模拟系统上的时间，不是电脑实际运行时间 |
| `wall_time_s` / `sim_events` | 这次仿真本身花的墙钟时间 / 处理的事件数 |
| `stages` | 各阶段起止时间与各设备忙碌时间 |
| `devices` | 每个设备的忙碌时间、利用率、kernel 数、动态 / 静态能耗 |
| `links` | 每条链路的有效带宽、上下行数据量、利用率 |
| `memories` | 每个存储的容量、峰值占用、读写量 |
| `transfers` | 总搬运量与 `by_kind` 分解：`fetch` 按需拉取、`halo` 跨分区邻居、`writeback` 逐出写回、`stage_in` 暂存、`zero_copy` 零拷贝访存、`stream` 流式读入 |
| `energy` | 计算 / 传输 / 静态能耗与平均功率 |
| `budget` | 系统估算成本、峰值功耗与预算上限 |
| `roofline` | 每类 kernel 的算术强度、实测吞吐与峰值 |
| `graph` | 任务图摘要：分块数、任务数、割边比例、halo 行数、溢出层、数据量 |
| `strategy` | 最终生效的策略（插件规划 / 自动分块之后） |
| `timeline` | 设备执行与传输的时间线（前端甘特图使用） |
