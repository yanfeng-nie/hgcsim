# HGCSim：大规模图聚类异构计算模拟器

《模式识别与机器学习》课程 · 聚类分析小组题目 **“大规模图聚类的异构计算技术”** 的配套模拟器。

HGCSim 在体系结构层面模拟一台完整的计算机，包括：

- **计算设备**：CPU、GPU、NPU、TPU、FPGA、近内存计算、可计算存储、RRAM/MRAM 存算一体；
- **存储层次**：HBM、GDDR、DRAM、SCM、Flash；
- **互连**：PCIe、CXL、DDR、NVLink。

模拟器在这台计算机上运行大规模图聚类的各类基线任务，用来评价答题人设计的**异构任务调度与数据管理机制**。

- 模拟器内核：C++17（无第三方依赖，JSON 库已内置），离散事件仿真 + 扩展 Roofline + 流级网络模型
- 前端：Python + Streamlit 网页，通过 C ABI（ctypes）向内核下发任务
- 不做真实数值计算：全部基线任务在数秒内跑完（上限 5 分钟）

**最快上手**（Ubuntu ≥ 20.04，在仓库根目录执行）：

```bash
bash install_deps.sh            # 1. 安装依赖（apt 编译工具 + 仓库内 .venv 中的 Python 包），只需一次
make build                      # 2. 编译 C++ 模拟器
bash scripts/run_frontend.sh    # 3. 启动网页前端，浏览器打开 http://localhost:8501
bash scripts/run_suite.sh       # 4. （可选）不用前端，在命令行跑全部基线任务
```

HGCSim 的使用分三步，三步彼此独立：

| 步骤 | 命令 | 什么时候需要做 |
|---|---|---|
| 安装依赖 | `bash install_deps.sh` | 只需一次（换机器或删除了 `.venv/` 后再做） |
| 编译 | `make build` / `make build BUILD_NAME=<build名>` | 首次使用；以及每次写了 / 改了 C++ 调度插件或模拟器源码之后 |
| 运行 | `bash scripts/run_frontend.sh`、`bash scripts/run_suite.sh` 等 | 随时；只改 JSON 配置不需要重新编译 |

下文所有命令都在**仓库根目录**（含 `Makefile` 的目录）执行。前端各页面的使用说明、默认配置下各选项的意义见 [docs/STUDENT_GUIDE.md](docs/STUDENT_GUIDE.md) 第 12、13 节。

---

## 环境要求

- **操作系统**：Ubuntu 20.04 / 22.04 / 24.04（只支持 Ubuntu）
- **编译工具**：GCC ≥ 9、CMake ≥ 3.16、GNU Make（`install_deps.sh` 会安装）
- **Python**：≥ 3.8（系统自带即可；前端依赖装在仓库内的 `.venv/`，约 500 MB）
- **仓库路径**：不要包含空格（Make 的限制），例如放在 `~/hgcsim`
- **硬件**：任意普通电脑即可，全部基线任务在数秒内仿真完成；不需要 GPU
- 已验证：GCC 9.5 与 GCC 13.3 编译通过；Streamlit 1.39 与 1.65 前端测试通过

## 安装依赖 install_deps.sh

```bash
bash install_deps.sh                    # apt 安装编译工具 + 创建 .venv 并安装前端依赖（会请求 sudo 密码）
```

脚本只安装依赖，**不编译**。它做两件事：

| 步骤 | 安装内容 | 安装位置 |
|---|---|---|
| apt | `build-essential`（gcc / g++ / make）、`cmake`、`python3`、`python3-venv` | 系统（需要 sudo） |
| pip | `requirements.txt`：`streamlit`、`plotly`、`pandas` 及其依赖 | 仓库内 `.venv/`，不写入系统 Python，不留 pip 缓存 |

可选参数：

| 参数 | 作用 |
|---|---|
| （无） | apt + pip 全部安装 |
| `--apt-only` | 只装 apt 软件包 |
| `--pip-only` | 只创建 `.venv/` 并安装 Python 包（已有编译工具时，例如机房电脑没有 sudo 权限） |
| `--mirror URL` | pip 使用指定镜像，例如 `--mirror https://pypi.tuna.tsinghua.edu.cn/simple` |
| `--recreate-venv` | 删除并重建 `.venv/`（Python 包损坏或想升级时） |
| `--dry-run` | 只打印将要执行的命令，不做任何修改 |
| `--uninstall` | 撤销安装：删除 `.venv/`，并卸载本脚本当初**新装**的 apt 软件包 |
| `--force` | 非 Ubuntu 或低于 20.04 时也继续（不保证可用） |
| `-h`, `--help` | 显示帮助 |

说明：

- **不需要手动激活虚拟环境**。`scripts/` 下的运行脚本会自动使用 `.venv/`。如果想自己在命令行里用这些 Python 包，执行 `source .venv/bin/activate`。
- Ubuntu 23.04 起系统 Python 禁止直接 `pip install`（PEP 668），所以 Python 包一律装进 `.venv/`。
- **撤销安装**：脚本在 apt 安装前后对比软件包列表，把安装前不存在、由本脚本装入的软件包（包括随之安装的依赖）记到 `.hgcsim_deps/apt_new_packages.txt`。`--uninstall` 只卸载这份清单里的包，装 HGCSim 之前就已有的软件包一律不动。apt 会先列出明细，等你确认后才卸载。编译产物另用 `make clean` 删除。

## 编译 Makefile

所有编译选项都在根目录的 `Makefile` 中。直接运行 `make`（或 `make help`）会显示全部用法。

**目标**

| 命令 | 作用 |
|---|---|
| `make build` | 构建 `build/`。未修改的原始源码自动使用自带 build 名 `default` |
| `make build BUILD_NAME=<build名>` | 按给定 build 名构建。**修改过模拟器源码，或在 `plugins/student/` 下写了 C++ 调度插件后必须指定** |
| `make rebuild [BUILD_NAME=...]` | 先清除再完整构建（等价于 `make clean` 后 `make build`） |
| `make clean` | 清除所有 build（删除整个 `build/` 目录） |
| `make test` | 对现有 build 运行 C++ 单元测试（不重新编译） |
| `make info` | 显示当前 build 名、编译时间、包含的调度插件，以及之后源码是否又被修改过 |
| `make help` | 显示帮助（直接运行 `make` 也一样） |

**可选变量**（写在目标后面，可以组合，例如 `make build BUILD_NAME=my_v1 JOBS=4 TEST=0`）

| 变量 | 默认值 | 作用 |
|---|---|---|
| `BUILD_NAME` | 空（自动） | build 名，规则见下文 |
| `BUILD_TYPE` | `Release` | `Release`（-O2）/ `Debug`（便于 gdb 调试插件）/ `RelWithDebInfo` |
| `JOBS` | CPU 核数 | 并行编译任务数 |
| `TEST` | `1` | 构建后是否运行 C++ 单元测试；`TEST=0` 跳过 |
| `VERBOSE` | `0` | `VERBOSE=1` 显示完整编译命令 |
| `CXX` | 由 CMake 自动选择 | 指定 C++ 编译器，例如 `CXX=g++-9`；更换编译器时请用 `make rebuild` |
| `CMAKE_ARGS` | 空 | 追加传给 cmake 配置阶段的参数 |

`make build` 每次都会重新执行 CMake 配置，build 名、插件列表以及源码是否被修改，都按当前文件重新判断。

### build 名称

- **`default`**：未经任何修改的原始答题仓库的 build 名。不指定 `BUILD_NAME` 时自动使用。
- **什么时候必须起名**：满足以下任一条件时，不指定 `BUILD_NAME` 会在配置阶段报错停止：
  - `plugins/student/` 下有 `.cpp` 文件；
  - 以下文件与发布时不同：`src/`、`include/hgcsim/`、`plugins/*.cpp`、`CMakeLists.txt`。

  “与发布时不同”按源码指纹 `scripts/pristine.fingerprint` 判断。
- **命名规则**：以字母或数字开头，只含字母、数字、`.`、`_`、`-`，长度 1~40；不能与保留名重复（不区分大小写，例如 `Default` 也会被拒绝）。保留名会在报错信息和 `make info` 中列出。
- build 名被编译进 `libhgcsim.so`。前端侧边栏、`make info`、命令行套件的第一行都会显示它。提交时请注明 build 名。
- **重新编译后要重启前端**：在运行前端的终端按 `Ctrl+C` 停止，再执行 `bash scripts/run_frontend.sh`。如果改了插件却没有重新编译，前端侧边栏和 `make info` 都会提醒。

示例：

```bash
make build                                  # 原始仓库：build 名 default
cp plugins/example_scheduler.cpp plugins/student/my_sched.cpp
make build BUILD_NAME=my_v1                 # 写了插件：必须起名，生成 build/plugins/libmy_sched.so
make build BUILD_NAME=my_v2 JOBS=4 TEST=0   # 4 线程编译，跳过单元测试
make rebuild BUILD_NAME=my_v2 CXX=g++-9     # 换编译器，从头构建
make info                                   # 查看当前 build
make clean                                  # 删除 build/
```

## 运行

所有运行入口都是 `scripts/` 下的脚本，每个脚本都支持 `-h` 查看用法。运行前需要先完成“安装依赖”与“编译”两步。

| 脚本 | 用途 | 需要前端依赖 |
|---|---|---|
| `bash scripts/run_frontend.sh` | 启动网页前端 | 是 |
| `bash scripts/run_suite.sh` | 命令行批量运行基线测试套件（**非前端**） | 否 |
| `./build/hgcsim_cli` | 命令行单次仿真 / 校验配置（**非前端**） | 否 |
| `bash scripts/run_tests.sh` | 全流程自检 | 是（可用 `--skip-frontend` 跳过前端部分） |
| `python3 scripts/export_workloads.py` | 把基线任务导出为 JSON | 否 |

### 网页前端 run_frontend.sh

```bash
bash scripts/run_frontend.sh                       # 默认 http://localhost:8501
bash scripts/run_frontend.sh --port 14541          # 指定端口
bash scripts/run_frontend.sh --address 127.0.0.1   # 只允许本机访问（默认监听所有网卡）
bash scripts/run_frontend.sh -- --server.maxUploadSize 50   # “--” 之后的参数原样传给 streamlit
```

- 按 `Ctrl+C` 停止。前端只是界面，所有计算都在 C++ 模拟器中完成。
- **远程访问**：在虚拟机或服务器上运行时，可以在自己电脑上建 SSH 隧道 `ssh -L 8501:localhost:8501 用户名@服务器`，然后浏览器打开 `http://localhost:8501`；也可以直接访问 `http://服务器IP:端口`（需放行防火墙端口）。
- 前端各页面的用法见 [docs/STUDENT_GUIDE.md](docs/STUDENT_GUIDE.md#12-前端使用说明) 第 12 节。

### 命令行基线测试套件 run_suite.sh（非前端）

不打开网页、在终端里批量运行基线任务，适合反复对比自己的策略、生成提交用的结果表。

```bash
bash scripts/run_suite.sh                     # 默认组合：系统 default_pc、hetero_lab × 默认策略
bash scripts/run_suite.sh --list              # 列出可用的系统、策略（内置 + 我的配置）与任务
bash scripts/run_suite.sh --systems hetero_lab --strategies naive_gpu my_strategy --out results.csv
bash scripts/run_suite.sh --systems my_codesign --strategies naive_gpu my_plugin --tasks B4 B5 --no-scaling
bash scripts/run_suite.sh --system-file ~/try/sys.json --strategy-file ~/try/st.json
```

| 参数 | 说明 |
|---|---|
| `--list` | 列出可用的系统配置、调度策略、任务以及默认组合，然后退出 |
| `--systems 名称 ...` | 系统配置名，即 `configs/systems/` 或 `configs/user/systems/` 中的文件名（不含 `.json`） |
| `--strategies 名称 ...` | 策略名，即 `configs/strategies/` 或 `configs/user/strategies/` 中的文件名；**第一个策略作为加速比基准** |
| `--system-file 路径 ...` | 额外的系统 JSON 文件（任意位置；名称取文件名） |
| `--strategy-file 路径 ...` | 额外的策略 JSON 文件（任意位置；名称取文件名） |
| `--tasks 任务 ...` | 只运行指定任务：`B1`~`B6`、`S18` `S20` `S22` `S24` |
| `--no-scaling` | 不运行 R-MAT 可扩展性扫描（S18–S24） |
| `--out 文件` | 逐项结果写到 `.csv` 或 `.json` |
| `-h`, `--help` | 显示帮助 |

- 不写 `--systems` 时使用 `default_pc hetero_lab`；不写 `--strategies` 时使用默认策略（`--list` 的最后一行会显示）。
- 相对路径（`--system-file`、`--strategy-file`、`--out`）都相对于你运行命令时所在的目录。

输出说明：

1. 第一行：模拟器版本、**当前 build 名**、系统与策略列表。如果源码在 build 之后被修改过，会给出警告。
2. 进度条（输出到标准错误）。
3. 仿真完成时间表：行是任务，列是“系统/策略”。数字的单位是秒，带 `m` 后缀的是毫秒；`ERR` 表示该组合仿真失败，失败原因列在表下方。
4. **B1–B6 几何平均加速比**：每个系统上，各策略相对第一个策略的几何平均加速比。这正是调度赛道的建议评价指标，所以把 `naive_gpu` 放在 `--strategies` 的第一个即可。
5. 最后一行：仿真总数与墙钟时间，以及是否在 300 秒上限内完成。

`--out` 的 CSV / JSON 中，每次仿真一行，包含以下列：

- `task`、`system`、`strategy`、`status`
- `makespan_s`（仿真完成时间，秒）、`energy_j`、`chunks`、`transfer_GB`
- `wall_s`（这次仿真本身花的墙钟时间）
- `speedup_vs_<基准策略>`
- `error`

**退出码**：`0` 表示全部仿真在上限内完成，`1` 表示超时或未完成，`2` 表示参数错误。单个组合仿真失败不影响退出码，会在表中显示为 `ERR`。

### 单次命令行仿真 hgcsim_cli（非前端）

```bash
./build/hgcsim_cli run configs/systems/hetero_lab.json configs/workloads/B5.json configs/user/strategies/my_strategy.json out.json
./build/hgcsim_cli validate configs/user/systems/my_codesign.json    # 校验系统配置，打印推导出的峰值算力、带宽、功耗、成本
./build/hgcsim_cli characterize configs/workloads/B4.json            # 打印任务各阶段的 FLOPs、访存量、算术强度
./build/hgcsim_cli catalog                                           # 打印全部组件类型的可调参数、默认值与上下界（JSON）
```

| 命令 | 说明 |
|---|---|
| `run` | 参数依次为系统、任务、策略三个 JSON 文件，可选第四个参数为结果输出文件。终端打印完成时间、分块数、各阶段起止时间与能耗；输出文件中是完整结果，字段见 [docs/STUDENT_GUIDE.md](docs/STUDENT_GUIDE.md#13-默认配置下各选项的意义) 第 13 节“仿真结果字段” |
| `validate` | 校验系统配置，打印推导出的峰值算力、带宽、功耗、成本 |
| `characterize` | 打印任务各阶段的 FLOPs、访存量、算术强度 |
| `catalog` | 打印全部组件类型的可调参数、默认值与上下界（JSON） |

- 任务文件在 `configs/workloads/` 下：`B1.json`~`B6.json`、`S18.json`~`S24.json`。
- 策略可以是 JSON 策略，也可以是指向 C++ 插件的策略。插件路径相对于仓库根目录，所以请在仓库根目录运行。

### 全流程自检 run_tests.sh

```bash
bash scripts/run_tests.sh                   # 检查当前 build（不会重新编译）
bash scripts/run_tests.sh --skip-frontend   # 没装前端依赖时，跳过前端冒烟测试
```

依次运行以下五项，任何一步失败都会停止：

1. C++ 单元测试；
2. build 名审查测试（只跑 CMake 配置阶段，使用临时目录，不影响 `build/`）；
3. 一次命令行仿真；
4. 完整基线套件，要求 5 分钟内完成，结果写到 `build/suite_results.json`；
5. 前端冒烟测试，使用临时目录，不改动 `configs/user/`。

如果源码在当前 build 之后被修改过，脚本会要求先重新 `make build`。

### 导出基线任务 export_workloads.py

```bash
python3 scripts/export_workloads.py         # 把前端内置的基线任务导出为 configs/workloads/*.json
```

基线任务定义在 `frontend/hgcsim_frontend/tasks.py` 中，`configs/workloads/` 下已经附带导出好的文件。只有修改了 `tasks.py` 后才需要重新导出。

## 答题流程

**赛道**（建议的评价方式，详见 [docs/STUDENT_GUIDE.md](docs/STUDENT_GUIDE.md)）

| 赛道 | 硬件 | 你要做的 | 评价指标 |
|---|---|---|---|
| A. 调度赛道 | 固定为 `hetero_lab` | 只写调度策略（JSON 或 C++ 插件） | B1–B6 完成时间相对 `naive_gpu` 的几何平均加速比 |
| B. 协同设计赛道 | 从 `codesign_budget` 出发自选硬件 | 硬件配置 + 调度策略 | 同上；系统必须满足预算（功耗 ≤ 2500 W，成本 ≤ $60000） |

**方式 1：JSON 策略**（不写代码、不需要重新编译）

1. 打开前端“③ 任务与策略”页，载入 `hetero_template`。
2. 修改设备映射、精度、数据放置、分块、流水线（各字段的含义与缺省值见 [docs/STUDENT_GUIDE.md](docs/STUDENT_GUIDE.md#13-默认配置下各选项的意义) 第 13 节），点“▶ 运行仿真”，到“④ 结果分析”页看瓶颈。
3. 填写配置名，点“💾 保存到我的配置”，文件会保存为 `configs/user/strategies/<名称>.json`。也可以直接在本地编辑这个文件，再在前端点“刷新列表”。
4. 在终端跑全部基线，得到结果表：
   ```bash
   bash scripts/run_suite.sh --systems hetero_lab --strategies naive_gpu <名称> --out results.csv
   ```

**方式 2：C++ 调度插件**（能实现任意调度逻辑）

```bash
cp plugins/example_scheduler.cpp plugins/student/my_sched.cpp    # 文件名即插件名
# ……编辑 my_sched.cpp：实现 plan()（静态规划）与 select_device()（动态分配）……
make build BUILD_NAME=my_v1                                      # 必须起名；生成 build/plugins/libmy_sched.so
cat > configs/user/strategies/my_sched.json <<'EOF'
{"name": "my_sched", "plugin": "build/plugins/libmy_sched.so", "plugin_args": {}}
EOF
bash scripts/run_frontend.sh                                     # 重启前端，侧边栏显示 build：my_v1
bash scripts/run_suite.sh --systems hetero_lab --strategies naive_gpu my_sched
```

插件接口说明见 [docs/STUDENT_GUIDE.md 第 7 节](docs/STUDENT_GUIDE.md)，接口定义在 `include/hgcsim/scheduler.hpp`。

**协同设计赛道**

1. 在“② 系统配置”页载入 `codesign_budget`。
2. 在“增删设备与预算”标签页中从模板添加设备、更换主机，在“编辑参数”标签页中修改参数。页面顶部会实时显示成本、功耗和是否满足预算。
3. 保存为“我的系统配置”，例如 `my_codesign`，文件位于 `configs/user/systems/`。
4. 配合 JSON 策略或插件运行：
   ```bash
   bash scripts/run_suite.sh --systems my_codesign --strategies naive_gpu <你的策略>
   ```

**建议提交物**

1. 文件：
   - 策略 JSON（`configs/user/strategies/*.json`）和/或插件源码（`plugins/student/*.cpp`）；
   - 协同设计赛道的系统配置（`configs/user/systems/*.json`）；
   - 注明 **build 名**。
2. `run_suite.sh ... --out results.csv` 的结果。
3. 简短报告：各阶段负载特征分析 → 映射与数据管理决策 → 实验结果 → 体系结构层面的解释（可引用前端的 Roofline、链路利用率、数据搬运分解）。

## 配置文件放在哪里

| 类型 | 内置（只读，随代码包发布） | 我的配置（前端保存 / 本地编辑） |
|---|---|---|
| 系统（硬件）配置 | `configs/systems/*.json` | `configs/user/systems/*.json` |
| 调度策略 | `configs/strategies/*.json` | `configs/user/strategies/*.json` |
| C++ 调度插件 | `plugins/example_scheduler.cpp`（模板） | `plugins/student/*.cpp`（编译为 `build/plugins/lib<文件名>.so`） |
| 元件模板 | `configs/devices/*.json`（设备、交换机）、`configs/hosts/*.json`（主机） | — |
| 基线任务 | `configs/workloads/*.json`（由 `tasks.py` 导出） | 可在前端“自定义 workload JSON”中临时输入 |

- 配置名就是文件名（不含 `.json`）。规则：
  - 以字母或数字开头，只含字母、数字、下划线、短横线，长度 1~40；
  - **不能与内置配置重名**（不区分大小写）。
- 前端里未保存的修改只存在于当前浏览器会话，刷新页面就会丢失；页面会提示“有未保存的修改”。
- 前端、命令行套件、`hgcsim_cli` 读取的是同一批文件。
- 环境变量 `HGCSIM_USER_CONFIG_DIR=<目录>` 可以把“我的配置”目录换到别处（该目录下同样分 `systems/` 与 `strategies/`）。

## 常见问题

| 现象 | 原因与处理 |
|---|---|
| `make build` 报“必须为这次 build 指定名称” | 你改了源码或添加了插件：用 `make build BUILD_NAME=<build名>` |
| `make build` 报“与自带 build 的保留名冲突” | 换一个名字；`default` 等保留名不区分大小写 |
| 前端侧边栏提示“源码或调度插件在本次 build 之后被修改过” | 重新 `make build BUILD_NAME=<build名>`，然后重启前端 |
| 重新编译后前端没有变化 | 前端在启动时加载模拟器库：`Ctrl+C` 停止后重新运行 `bash scripts/run_frontend.sh` |
| `错误：缺少 Python 依赖` / `No module named streamlit` | 先运行 `bash install_deps.sh`（没有 sudo 时用 `--pip-only`） |
| `错误：还没有编译模拟器` / 找不到 `libhgcsim.so` | 先运行 `make build` |
| `找不到 cmake` | 先运行 `bash install_deps.sh`（或 `--apt-only`） |
| pip 下载很慢或失败 | `bash install_deps.sh --pip-only --mirror https://pypi.tuna.tsinghua.edu.cn/simple` |
| 端口被占用 | `bash scripts/run_frontend.sh --port 8502` |
| 仿真报 `OOM`、`仿真停滞`、`无可用设备`、`超出预算` | 见 [docs/STUDENT_GUIDE.md 第 11 节](docs/STUDENT_GUIDE.md) |
| 想恢复到干净状态 | 删除 `configs/user/` 下自己的 JSON 与 `plugins/student/*.cpp`，再 `make rebuild`（build 名回到 `default`） |

---

## 附录

### 文档索引

| 文档 | 内容 |
|---|---|
| [docs/STUDENT_GUIDE.md](docs/STUDENT_GUIDE.md) | **答题指南**：题目、赛道、负载模型、JSON 策略格式、C++ 插件接口、模拟器刻画的权衡、常见错误、前端各页面的使用说明、默认配置下各选项的意义 |
| [docs/MODEL.md](docs/MODEL.md) | 建模说明：各组件的性能推导公式、网络与内存模型、负载模型、局限性 |
| [docs/PARAM_SOURCES.md](docs/PARAM_SOURCES.md) | 参数上界与出处 |

### 目录结构

```
install_deps.sh      依赖安装（apt + 仓库内 .venv）
Makefile             编译入口（make build / make clean …）
include/hgcsim/      C++ 公共头文件（参数、存储、互连、设备、系统、负载、调度插件接口、运行时、C ABI）
src/                 C++ 实现（含命令行 hgcsim_cli）
cmake/               源码指纹与 build 信息模板
plugins/             调度插件：example_scheduler.cpp（模板）、student/（答题人的插件，自动编译）
configs/
  systems/             内置系统：default_pc（默认）、hetero_lab（调度赛道平台）、codesign_budget（协同设计起点）
  strategies/          内置策略：naive_cpu、naive_gpu、hetero_template、example_plugin
  user/                我的配置（systems/、strategies/）
  devices/ hosts/      设备 / 交换机 / 主机模板
  workloads/           基线任务 JSON（供命令行使用）
frontend/            Python 前端（app.py；hgcsim_frontend/ 含 ctypes 绑定、配置存储、build 信息、基线任务、套件、可视化）
.streamlit/          前端主题配置（固定浅色主题，保证图表可读）
scripts/             运行脚本：run_frontend.sh、run_suite.sh、run_tests.sh、export_workloads.py
tests/               C++ 单元测试、build 名审查测试、前端冒烟测试
docs/                文档
third_party/         nlohmann/json（MIT）
build/               编译产物（make build 生成，make clean 删除）
.venv/               Python 虚拟环境（install_deps.sh 生成）
```

### 扩展模拟器

- **新设备类型**：
  1. 在 `include/hgcsim/device.hpp` 中继承 `ComputeDevice`；
  2. 在构造函数里用 `p.add(...)` 声明参数（默认值、上下界、单位、说明）；
  3. 重写 `peak_flops / supports / op_efficiency / estimate / tdp_w / cost_usd`；
  4. 在 `make_device()` 与 `device_types()` 中注册。
- **新算子 / 新算法**：在 `src/workload.cpp` 中添加 kernel 生成函数与任务图构建逻辑。
- 修改源码后用 `make build BUILD_NAME=<build名>` 重新编译。

### 许可

本项目代码供课程教学使用。`third_party/nlohmann/json.hpp` 采用 MIT 许可（版权归 Niels Lohmann）。
