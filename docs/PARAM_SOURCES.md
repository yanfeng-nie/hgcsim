# 参数上界与出处

参数上界取自截至 2026 年的公开器件参数。标注 **✓** 的条目在开发时（2026-10）联网核对过；标注 **○** 的条目依据公开资料的常识性数值，未逐条联网核对，使用前建议自行复核。所有上界都在 C++ 参数定义中（`src/devices.cpp`、`src/memory.cpp`、`src/interconnect.cpp`、`src/system.cpp`），可在前端“参数参考”页查看。

## 计算设备

| 参数 | 上界 / 模板值 | 依据 | 来源 |
|---|---|---|---|
| CPU 核数 | 192（单路），参数上界 384（双路） | EPYC 9965：192 核、384MB L3、12 通道 DDR5-6400（614 GB/s）、PCIe 5.0 x128、500W | ✓ [AMD EPYC 9965](https://www.amd.com/en/products/processors/server/epyc/9005-series/amd-epyc-9965.html) |
| GPU SM 数 | 256（Rubin 级模板 224） | Rubin：224 SM，288GB HBM4，最高 22 TB/s，NVLink 6 3.6 TB/s，TDP 1.8–2.3 kW | ✓ [Rubin 架构解析](https://blog.barrack.ai/nvidia-rubin-specs-architecture-2026/) |
| GPU Tensor FP16/SM/周期 | 32768 | B200 量级 ≈ 8192（FP16 稠密≈2.25 PFLOPS） | ○ NVIDIA B200 公开规格 |
| 消费级 GPU（默认） | 46 SM @2.475GHz，29.1 TFLOPS FP32，12GB GDDR6X 504 GB/s | RTX 4070 公开规格 | ○ |
| TPU | MXU 32 个、边长 256、2.5 GHz | Ironwood：单芯片 4614 TFLOPS、192GB HBM、7.2 TB/s | ✓ [Ironwood](https://ssojet.com/news/google-unveils-ironwood-7th-gen-tpu-for-enhanced-ai-inference) |
| TPU v5e 模板 | 197 TFLOPS BF16，16GB HBM 819 GB/s | Google Cloud 公开规格 | ○ |
| NPU | 128 AI Core、2.5 GHz | 昇腾 910B 量级模板 ≈ 393 TFLOPS FP16、64GB HBM 1.6 TB/s | ○ 华为公开资料 |
| FPGA | 20000 DSP、800 MHz、200MB 片上 | Alveo V80：10848 DSP、2.6M LUT、132Mb BRAM + 541Mb URAM、32GB HBM2e 820 GB/s、PCIe Gen4 x16、190W、$9495 | ✓ [AMD Alveo V80](https://www.amd.com/en/products/accelerators/alveo/v80/a-v80-p64g-pq-g.html) |
| NMP（UPMEM） | 单元 8192、1.5 GHz | 2556 DPU @350MHz、每 DPU 64MB、单 DPU MRAM 带宽 0.7 GB/s（实测 0.62）、聚合 1.7 TB/s；DPU 间无直接通信，须经主机；加法 58.56 MOPS/DPU | ✓ [Gómez-Luna et al., arXiv:2105.03814](https://arxiv.org/pdf/2105.03814) |
| NMP（HBM-PIM / CXL-PNM 模板） | — | Samsung HBM-PIM（FP16 SIMD PCU）、CXL-PNM（512GB，≈1.1 TB/s） | ○ 厂商公开资料 |
| CSD | 盘内算力 20 TFLOPS | SmartSSD（KU15P FPGA、4TB） | ○ |
| RRAM CIM | 阵列 1024×1024、65536 个、ADC 10 bit、单元 ≤4 bit | 28nm 576×512 2T2R 阵列，64 个 ADC 服务 512 列；35.6 TOPS/W；ADC 与驱动占 99% 功耗 | ✓ [J. Semicond. 46(6) 2025](https://m.researching.cn/articles/OJ547aea9cf486faae) |
| MRAM CIM | 单元 ≤2 bit、写入 2–1000ns、耐久 1e8–1e16 | MRAM 交叉阵列存内计算（Samsung, Nature 2022） | ○ |

## 存储层级

| 参数 | 上界 | 依据 | 来源 |
|---|---|---|---|
| HBM 接口位宽 / 速率 / 单栈容量 | 2048 bit / 12 Gb/s / 64 GB | JEDEC HBM4（JESD270-4）：2048 bit、8 Gb/s、2 TB/s/栈、4–16 层、24/32Gb die、16 层 64GB、32 通道 | ✓ [JEDEC HBM4](https://www.allaboutcircuits.com/news/jedec-officially-releases-hbm4-memory-standard) |
| HBM3E | 9.8 Gb/s、48 GB/栈（16-Hi） | 关系约束 | ○ 厂商公开资料 |
| GDDR | 512 bit、36 Gb/s、96 GB | GDDR7 32–36 Gb/s | ○ |
| DRAM | 24 通道、8800 MT/s（MRDIMM）、12 TB | DDR5-6400 / MRDIMM 8800 | ○ |
| SCM | 单模块读 25 GB/s、1 TB | Optane PMem 200（已停产）作为默认参考 | ○ |
| Flash | 32 NAND 通道、4800 MT/s、256 TB、10M IOPS | Micron 9650：PCIe 6.0、最高 28 GB/s、随机读约 5.5M IOPS、7.68–30.72TB、18W | ✓ [The Register](https://www.theregister.com/2026/02/17/micron_pcie_6/) |

## 互连

| 参数 | 上界 | 依据 | 来源 |
|---|---|---|---|
| PCIe 代数 | Gen7（128 GT/s） | PCIe 7.0 规范 2025 年发布；Gen6 起 PAM4 + FLIT | ○ PCI-SIG |
| CXL 版本 | 4.0 | CXL 4.0（2025-11）：128 GT/s（基于 PCIe 7.0）、沿用 256B flit、捆绑端口、原生 x2 链路、最多 4 个 retimer | ✓ [Converge Digest](https://convergedigest.com/cxl-4-0-spec-doubles-bandwidth-to-128gt-s/) / [CXL 4.0 白皮书](https://computeexpresslink.org/wp-content/uploads/2025/11/CXL_4.0-White-Paper_FINAL.pdf) |
| CXL 访存延迟 | 端口 5–100ns、交换机 50–500ns | 实测 CXL 内存比本地 DRAM 多约 70–150ns，经交换机更高 | ○ 公开测量论文 |
| NVLink | 36 条 × 100 GB/s/方向 | NVLink 6：3.6 TB/s/GPU | ✓ 同 Rubin 来源 |

## 预算模型说明

成本与功耗公式（各组件 `cost_usd()` / `tdp_w()`）只用于协同设计赛道的约束，用若干已知产品校准量级：RTX 4070 级 GPU ≈ 190W、A100 级 ≈ 260W、B200 级 ≈ 950W、Rubin 级 ≈ 2.1kW；EPYC 9965 级 CPU ≈ 466W；Alveo V80 级 FPGA ≈ 130W、≈ $10.9k。它们不是真实报价。
