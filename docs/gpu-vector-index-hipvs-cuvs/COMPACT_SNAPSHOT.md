# 第二轮候选：无损紧凑快照（2026-11-02）

状态：Windows/Ubuntu 主机测试、ROCm 源码编译、公开 10K 和两组 250K 合成输入的完整 SQL 配对已完成。
本轮还加入 OpenMP 线程预算优化；实际性能、CPU 对比、资源采样和 profiler 限制见 PERFORMANCE_REPORT.md。
用户反馈分数未满，但本次成绩截图未能读取，具体分数、build/query 比值和门槛明细未知。

## 问题和改动

V5 的 OBCUVS02 在 ID 和原始向量之后，另存一份包含相同向量的 hipVS 索引文件。
建图完成后，这会额外传回一次向量，并通过临时文件写入/读取完整 hipVS 载荷。

新 OBCUVS03 直接导出实际 GPU CAGRA 图，只保存一份 FP32 向量。
重载使用原始向量及原图调用固定 hipVS 的 `cuvsCagraIndexFromArgs`，不重复建图。
搜索仍调用同一个 `cuvsCagraSearch`，建图 NN_DESCENT/128/256/100 和搜索参数均未调整。
100 是 NN-descent 的迭代上限，固定库自身的提前收敛逻辑保持不变。
新增接口均来自题目固定 hipVS commit 的公开 C API，没有修改或复制库实现。

源接口：[固定版本 cagra.h](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/include/cuvs/neighbors/cagra.h)。
主机输入的所有权和复制行为见同版本的 [cagra.hpp](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/include/cuvs/neighbors/cagra.hpp)。

## OpenMP 辅助线程预算

本镜像中进程实际配额为 16 核，hipVS 使用的 LLVM OpenMP 默认却可创建 128 个线程。
NN-descent 的 CPU 图更新、采样和排序反复启动并行区域，默认线程数量与配额不匹配。
`host_thread_budget.h` 在首次 GPU 建图时通过已加载 LLVM OpenMP 的公开 `kmp_set_defaults` 配置一次：

- 预算取当前 CPU 亲和范围、cgroup CPU 配额和比赛 24 核上限中的有效最小值。
- 每个 OpenMP team 最多使用 16 个线程；较小 CPU 配额会进一步缩小。
- 默认 `KMP_BLOCKTIME=0`，减少等待 GPU 或并行区域结束时的忙等。
- 保留显式 `OMP_NUM_THREADS`、`KMP_BLOCKTIME` 环境配置；其他 OpenMP 运行时没有该公开接口时维持原行为。

这是进程内 OpenMP 默认值，会作用于后续 OpenMP 线程；不改变 OS CPU 配额或亲和设置。
已实际验证同一候选进程先创建 GPU 索引，再创建和查询 250K CPU VSAG 索引。
16 核环境上的 4/8/12/16 线程比较保存在原始证据中，16 线程在本次两种维度上最快。
没有修改 hipVS、搜索算法、图度数、精度或迭代上限，也没有按数据集或评测阶段选择实现。

公开接口说明：[OpenMP 扩展例程](https://www.intel.com/content/www/us/en/docs/dpcpp-cpp-compiler/developer-guide-reference/2026-2/intel-compiler-extension-routines-to-openmp.html)。

## 格式与兼容

| 字段 | 字节数 | 含义 |
|---|---:|---|
| magic | 8 | OBCUVS03 |
| graph_bytes | 8 | N × degree × 4 |
| dim | 4 | FP32 向量维度 |
| degree | 4 | GPU 图的实际出度 |
| N | 8 | 实际向量数 |
| IDs | N × 8 | 原外部 ID，保持顺序 |
| vectors | N × dim × 4 | 原始 FP32，逐字节保留 |
| graph | N × degree × 4 | 实际 GPU 图的 uint32 行偏移 |

继续读取旧 OBCUVS02；VSAG 原有序列化路径保持不变。
旧 V5 二进制不能读取新 OBCUVS03，回退二进制时应使用原版数据或重新建索引。
评测每轮从无索引状态开始，不复用上一轮快照。

以 250000 行、degree=128 计算，忽略旧 hipVS 格式的少量元数据：

| 维度 | OBCUVS02 载荷 / MB | OBCUVS03 / MB | 字节数减少 |
|---|---:|---:|---:|
| 128 | 386 | 258 | 33.16% |
| 256 | 642 | 386 | 39.88% |

MB 按 10^6 字节计。此表是格式字节数推导，不能推导相同比例的端到端耗时改善。
CPU 临时图缓冲区为 N × degree × 4 字节（250K 时 128 MB）；没有新增常驻 GPU 缓冲区。
实际 VRAM 峰值和 1.25 倍门槛仍需测量。

## 已验证和待验证

主机测试直接编译 adaptor 中的实际注册表与快照代码段，配合 hipVS API 替身，验证：

- 128/256 维 FP32、非连续正负外部 ID、图边逐字节一致。
- 从保存的图恢复后不再调用建图；输入缓冲区不被索引借用。
- 导出支持行间 padding 和 byte_offset，释放 DLPack 视图的元数据。
- 截断、维度/大小/出度错误、非法邻接行号、复制/恢复失败不会注册半成品。
- OBCUVS02 兼容读取、并发搜索、原查询参数、工作线程生命周期。

Windows 和已有 Ubuntu 均执行通过：

```bash
python3 tools/hipvs_bridge/tests/run_host_tests.py
python3 tools/hipvs_bridge/tests/test_benchmark.py
git diff --check
```

这些检查没有执行 GPU 内核，不证明 ROCm 编译、正式 Recall、GPU 抽检或性能提升。

GPU 实例就绪后，先用官方命令编译，再执行：

```bash
bash tools/hipvs_bridge/build_contest.sh /workspace/compact-bridge
/opt/venv/bin/python tools/hipvs_bridge/tests/gpu_smoke.py \
  --library /workspace/compact-bridge/libseekdb_cuvs_bridge.so \
  --base /seekdb_workspace/datasets/base.f32 \
  --queries /seekdb_workspace/datasets/query.f32 --dimension 128 \
  --require-compact --output /workspace/compact-gpu-smoke.json
```

GPU 检查导出、恢复并重新导出原图，验证逐边相同；修改恢复输入后，再对全部公开查询
验证 Top-10/100 的 ID 集合完全相同、距离及位置变化受 FP32 舍入误差约束。
该规则处理已观察到的近等距结果次序变化，不放宽每条返回 k 个互异合法 ID 的要求。
它仍只是 bridge 功能检查。
随后按 OPTIMIZATION.md 执行独立目录的 V5/候选 SQL 配对、SQL 重启和 lib=vsag 检查。
必须从建索引开始计时至首条合法查询结束，保留全部构建、持久化和重载耗时。
新日志记录 `gpu_build_us`、`build_us`、`snapshot_us` 和 `graph_bytes`，供分阶段定位。
Hidden250K 仍由组织方评测，不构造或替代隐藏数据。
