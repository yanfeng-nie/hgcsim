// =============================================================================
// runtime.hpp : 离散事件仿真运行时
//
//  * 事件引擎：按仿真时间推进的优先队列
//  * 流级网络模型：每条链路（全双工时每个方向）是一个带宽资源，
//    并发传输按最大最小公平 (max-min fairness) 渐进填充分配带宽，
//    任一传输开始/结束时重新分配 —— 可体现 PCIe/CXL 链路争用
//  * 存储管理：每个设备本地存储作为 LRU 缓存，容量不足时逐出，
//    脏数据写回到其 home（溢出层）；所有分配都受容量约束（OOM 会报错）
//  * 设备执行：每个设备同一时刻执行一个 kernel；开启流水线重叠时，
//    可在执行当前 kernel 的同时为后续 prefetch_depth 个任务预取输入
//  * 零拷贝：对 CXL / 主机内存等支持访存语义的路径，kernel 可直接远程读取，
//    远程读取作为与计算并发的“流”参与链路带宽竞争
// =============================================================================
#pragma once
#include <string>

#include "hgcsim/params.hpp"

namespace hgc {

// request: { "system": {...}, "workload": {...}, "strategy": {...}, "options": {...} }
json simulate(const json& request);
json validate_system(const json& system);
json characterize(const json& workload);
json plan_strategy(const json& request);  // 只运行插件规划 / 自动分块，返回最终策略

}  // namespace hgc
