# CPU/GPU 索引构建性能报告

2026-10-30 已完成 V5 与候选的真实离线源码编译及公开 10K 样例 SQL 测试。
结论：查询吞吐提升稳定，构建加速尚未成立，未达到本题的构建性能目标。
正式 SIFT250K / Hidden250K、资源硬门槛和官方 GPU 抽检仍待验证，不能据此申报分数。

## 版本与环境

- 基线：634888d9b037153fa143d2c913ba4b25503e1d62。
- 实测候选：以 hipvs-optimize 工作区补丁标识，包含 GPU 工作线程复用、单查显存/搜索参数复用和 4 MiB 快照中转块。建图、搜索算法参数与 V5 相同。
- 实测候选补丁 SHA-256：5db6e323485eac189b9ff67dcf3893285fb72e4657084dcc71a93e7fc31854f0。测试后仅更新文档和辅助脚本，生产代码 SHA-256 已逐项核对一致。
- 基线二进制 SHA-256：1b6626b632829841bfff0a424d6956982ddd5a7d6ac5bfa297baa8590749f868。
- 候选二进制 SHA-256：84dcc92da830ec124b98e4cc094af648f14d3a05b1cb135584c75bb9f12179f8。
- Radeon Cloud gfx1100，96 CU、49136 MB 显存，管理工具显示 AMD Radeon Graphics。
- 容器实际配额 16 核、55 GiB RAM；主机可见 128 核不代表可用 128 核。正式评测为 24 核/96 GB，绝对时间不能直接类比。
- ROCm 7.2.4、GCC 13.3.0、CMake 3.31.10、Rust 1.97.1；主工程按既有配置使用 devtools Clang 17。
- 复用模板依赖缓存，其键 ac032efc6b707fc0273e6b553db53011 与 V5 el9 依赖清单 MD5 一致。Rust 按现有 Cargo.lock 从模板缓存离线 vendor，没有联网下载依赖。
- 安装的 hipVS 无 Git 元数据，libcuvs_c.so SHA-256 为 b63211897276e0a6911bb534fe77c8b0b59f31c50af1df2f108845f06ad46ebf。固定 commit 身份仍需镜像提供方确认，未替换或修改该库。

## 方法

使用模板 base.f32（10000 × 128 FP32）、query.f32（100 × 128 FP32），不是正式 SIFT250K 或 Hidden250K。
以可往返 FP32 的 9 位有效数字导入向量，外部 ID 使用 row_offset * 17 + 1009，验证非连续 SQL ID 映射。
精确近邻用原始数据直接距离计算获得。100 个公开查询循环至 4096 条 SQL，Top-10/100 分别顺序执行，不使用 batch_knn。
该重复分布仅用于诊断，不能替代正式独立查询分布。

每次 GPU 运行均使用全新服务目录，先 LOAD DATA 导入无索引表，从固定离线 CREATE VECTOR INDEX 开始计时，直到首条查询取回合法结果。
GPU 图不跨轮复用。两轮分别按 baseline → candidate、candidate → baseline 执行。
两版使用相同环境和系统 libmysqlclient；服务内存预算 16G、datafile_size=8G、datafile_maxsize=12G、log_disk_size=4G。
编译与测量不并行运行。

## 当前候选实测

| 顺序 | V5 GPU 构建＋首查 / s | 候选 GPU 构建＋首查 / s | 构建比 V5/候选 | 综合查询比 |
|---|---:|---:|---:|---:|
| baseline → candidate | 5.648 | 8.163 | 0.692× | 2.326× |
| candidate → baseline | 6.749 | 5.820 | 1.160× | 2.245× |
| 两轮比值几何平均（诊断） | — | — | 0.896× | 2.285× |

构建有明显波动，综合比值小于 1，不能声称构建加速。
第一轮候选还接近实验磁盘容量上限，构建数值尤其需要在足够空间和正式规模下复测；
这里保留原始有效测量，没有挑选较快结果来宣称收益。

| 顺序 | 实现 | Top-10 QPS | Top-100 QPS | Top-10 P95 / ms | Top-100 P95 / ms | Recall@10/100 |
|---|---|---:|---:|---:|---:|---|
| 第一轮 | V5 GPU | 146.127 | 143.079 | 7.236 | 7.426 | 1.000 / 1.000 |
| 第一轮 | 候选 GPU | 357.002 | 316.742 | 2.898 | 3.297 | 1.000 / 1.000 |
| 第二轮 | 候选 GPU | 358.256 | 315.672 | 2.901 | 3.307 | 1.000 / 1.000 |
| 第二轮 | V5 GPU | 150.692 | 148.880 | 7.106 | 7.103 | 1.000 / 1.000 |

所有记录的样例查询均返回 k 个合法、互异 ID。
另有一轮因磁盘不足、基线未完成启动而作废，未计入表格。实验工具现要求至少 30 GiB 空间。

CPU VSAG 在同一候选二进制上的单次诊断：构建＋首查 2.668 s，Top-10/100 为 4204.641 / 2305.580 QPS，
Recall 为 0.639917 / 0.652378。CPU SQL 建表、LOAD DATA、建索引和查询正常工作。
CPU 与 GPU 检索质量不同，不能视作同质量性能比较，也没有证明小规模 GPU 构建优于 CPU。
CPU 结果不参与正式计分。

## 正确性、GPU 与资源证据

- 两版 bridge 通过实际 hipcc 编译，完整 seekdb 按固定 CMake 命令构建成功。
- 主机测试覆盖缓存、参数复位、错误处理、并发和资源释放；Windows/Linux 工作线程测试覆盖 800 个并发提交、同一线程执行、异常恢复、嵌套调用与退出。Linux 确认栈不小于 32 MiB。
- 真实 bridge：Recall@10/100=1，单查/批量结果一致，释放后快照重载一致，4 线程查询通过。
- SQL 重启：新候选读取已持久化的 GPU/CPU 索引；ID 合法，GPU 重载与新建索引首查结果完全一致，服务按 SIGTERM 正常停止。
- rocprofv3 bridge 跟踪记录 NN-descent、剪枝和 CAGRA 搜索 GPU 内核，共 1430 次 dispatch。SQL 日志也记录 built=true 的实际 CAGRA 快照。
- 完整 SQL 进程在 rocprofv3 下启动失败：未修改 V5 和候选均在 ObTimerService 初始化触发 SIGABRT，发生于建索引前。原因未确认，不能宣称通过正式 SQL GPU 抽检。gpu_sql_smoke.py --profiler none 仅执行功能测试。
- 内核 VmHWM：基线约 1.13–1.14 GiB、候选约 1.12 GiB，仅为本次进程 RSS 诊断；峰值 VRAM 与正式 1.25 倍门槛未验证。

## 复现与证据

方法和固定编译命令见 OPTIMIZATION.md、OFFICIAL_RULES.md。
prepare_public_sample.py 从公开 FP32 样例生成 TSV、SQL 与真值；
run_public_pair.py 用源码构建的二进制执行隔离配对，compare_runs.py 汇总。
不要将服务启动和数据导入计入构建时间。

开发机交付目录保存：

- gpu-evidence-20261030-final/：两轮完整 JSON、命令、环境、源码/二进制哈希、主机状态、快照日志、profiler CSV、失败诊断与原始公开数据。
- gpu-evidence-final.tar.gz，SHA-256：475150c074564e2128c84d1b5b50dbaadb85993773e721fb173b60569c0dcbeb。
- gpu-evidence-20261030/tested-source-worker.patch：与测量对应的原始补丁。
- deployment-20261030/：单独保存 Notebook 部署结果和 UUID。模板演示二进制及其批量查询图表未参与本报告的构建计分。

## 正式数据集待完成

| 数据集 | CPU VSAG | V5 GPU | 候选 GPU | Recall/资源/全部硬门槛 |
|---|---|---|---|---|
| SIFT250K / 128D | 未测 | 未测 | 未测 | 未验证 |
| Hidden250K / 256D | 未测 | 未测 | 未测 | 未验证 |

需要正式公开数据/测试脚本、兼容的 SQL profiler 核验和云端统一评测。
Hidden250K 由组织方评测，不生成或推测其内容，不以公开 10K 样例替代。
答题仓库按用户指定使用 Moriaty09/seekdb26；测量使用上述源码补丁，正式提测应选择该仓库已推送的候选 commit。正式分数未知。
