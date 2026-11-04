# 原生 CAGRA 建图与搜索联合调参（2026-11-04）

本轮在已提交的 `aefeef34afdf3407e635dfdfe78c31a694216c0c` 上减少 GPU 建图工作量，并增加原生 CAGRA 搜索的并行度和候选集。
完整 SQL 测量见 [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md)，原始指标见 [MEASUREMENTS_20261104.json](MEASUREMENTS_20261104.json)。

## 实现

| 参数 | V5 / 上一候选 | 本轮候选 |
|---|---|---|
| build_algo | NN_DESCENT | NN_DESCENT |
| graph_degree | 128 | 128 |
| intermediate_graph_degree | 256 | 128 |
| nn_descent_niter | 100 | 3 |
| search algo | C API 默认 SINGLE_CTA | 显式 MULTI_CTA |
| itopk_size | max(库默认值, 512, 4*k) | max(库默认值, 4096, 4*k) |
| search_width | 4 | 4 |

变化集中在 `tools/hipvs_bridge/seekdb_cuvs_bridge.c`。三个原生 GPU NN-descent refinement 轮次降低建图耗时；
MULTI_CTA 允许单行 SQL 的搜索使用更多 GPU 并行计算，较大的候选集改善短建图的搜索质量。
不能单独减少迭代就假定质量成立：本轮同时测量了独立 Gaussian 探针、簇状输入和公开样例，最终整条 SQL 链路的 Recall 高于或等于同轮 V5。
正式数据集上的 Recall 仍须由云端验证。

所有查询，包括首次查询、Top-10、Top-100、批量和重载后的查询，均执行同一套参数策略；没有按测试阶段选择不同路径。
搜索结果直接来自固定 hipVS CAGRA；没有 CPU 搜索替代、结果缓存、额外精排或修改 hipVS。
FP32 向量、外部 ID、最终图度数、SQL 和索引参数保持原有语义。之前的工作线程、紧凑快照和 OpenMP 配额优化保留。

测试替身补充原生搜索算法字段，并检查大 k 后 itopk 从 6000 恢复为 4096；原有分配失败、并发、资源释放、快照损坏和兼容性检查继续运行。
独立 bridge 诊断改用 HIP 返回的实际 GPU PCI 地址采样显存，避免临时实例换卡后读取旧设备计数器。

## 复现前提与源码构建

依赖使用组织方冻结版本，不安装新增包。环境、固定命令见 [OFFICIAL_RULES.md](OFFICIAL_RULES.md)。
以下假设有两个独立源码目录 `/work/v5` 和 `/work/candidate`；两者的冻结第三方依赖及离线 Rust vendor 均已准备好。
V5 必须检出 `634888d9b037153fa143d2c913ba4b25503e1d62`，candidate 使用本提交。
每份源码分别在根目录执行下列命令，`/work/build-output` 替换为该版本专用的绝对路径：

```bash
mkdir -p /work/build-output
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/build-output/libseekdb_cuvs_bridge.so
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/build-output/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

本次分别输出 `/work/v5-bridge/libseekdb_cuvs_bridge.so` 和 `/work/candidate-bridge/libseekdb_cuvs_bridge.so` 的等价隔离路径。
保存真实执行命令、源码/补丁、二进制 SHA-256。可用 `CARGO_NET_OFFLINE=true` 禁止 Cargo 联网；本次编译环境并行任务数为 12。

## 独立诊断输入

在候选源码根目录执行。输出目录必须不存在；本次 NumPy 为模板已有的 2.4.6。
数据生成和精确真值计算不计入构建时间。

```bash
export OPENBLAS_NUM_THREADS=8
/opt/venv/bin/python tools/hipvs_bridge/tests/prepare_clustered_sample.py \
  --dimension 128 --rows 250000 --nq 64 --seed 20261102 --output /work/raw128
/opt/venv/bin/python tools/hipvs_bridge/tests/prepare_gaussian_sample.py \
  --dimension 256 --rows 250000 --nq 256 --seed 20261104 \
  --query-seed 20261105 --output /work/raw256
/opt/venv/bin/python tools/hipvs_bridge/tests/prepare_public_sample.py \
  --base /work/raw128/base.f32 --queries /work/raw128/query.f32 --dimension 128 \
  --dataset LocalClusters250K128 --output /work/sql128
/opt/venv/bin/python tools/hipvs_bridge/tests/prepare_public_sample.py \
  --base /work/raw256/base.f32 --queries /work/raw256/query.f32 --dimension 256 \
  --dataset LocalGaussian250K256 --output /work/sql256
/opt/venv/bin/python tools/hipvs_bridge/tests/capture_environment.py --output /work/sql128/environment.json
/opt/venv/bin/python tools/hipvs_bridge/tests/capture_environment.py --output /work/sql256/environment.json
git diff --binary 634888d9b037153fa143d2c913ba4b25503e1d62 HEAD > /work/tested-source.patch
```

生成器保留 FP32，采用非连续外部 ID `row_offset * 17 + 1009`。
128D 的 64 个探针重复 64 次，256D 的 256 个探针重复 16 次，每组分别得到 4096 条 SQL。
256D 探针使用独立 RNG；其 seed 与最初 64 探针筛选不同。这些重复请求的吞吐不代表正式 4096 个独立查询的分布。
核对生成文件与 MEASUREMENTS 中的输入 SHA-256；不同 NumPy/BLAS 版本产生不同文件时应记录新哈希并重新配对。

## 完整 SQL 配对

先确保测试端口空闲、至少 30 GiB 可用空间，且每个输出目录不存在。两版均使用新服务目录，LOAD DATA 后再开始 DDL 计时。

```bash
/opt/venv/bin/python tools/hipvs_bridge/tests/run_public_pair.py \
  --baseline-bin /work/v5/build_release/src/observer/seekdb \
  --baseline-bridge /work/v5-bridge/libseekdb_cuvs_bridge.so \
  --candidate-bin /work/candidate/build_release/src/observer/seekdb \
  --candidate-bridge /work/candidate-bridge/libseekdb_cuvs_bridge.so \
  --fixtures /work/sql128 --source-patch /work/tested-source.patch \
  --output /work/pair128 --order baseline,candidate
/opt/venv/bin/python tools/hipvs_bridge/tests/run_public_pair.py \
  --baseline-bin /work/v5/build_release/src/observer/seekdb \
  --baseline-bridge /work/v5-bridge/libseekdb_cuvs_bridge.so \
  --candidate-bin /work/candidate/build_release/src/observer/seekdb \
  --candidate-bridge /work/candidate-bridge/libseekdb_cuvs_bridge.so \
  --fixtures /work/sql256 --source-patch /work/tested-source.patch \
  --output /work/pair256 --order candidate,baseline
```

脚本会检查导入行数、首查合法性、每条结果恰好 k 个互异合法 ID、Recall，并在结束时停止服务。
它记录 V5 基准 SHA 和候选补丁 SHA；提交后的文档变化会改变完整补丁哈希，生产文件哈希仍应与实测清单相同。
`benchmark_sql.py` 使用现有 libmysqlclient；构建时间为 DDL 加首次查询，后续 Top-10/100 各 4096 条单行 SQL 分别计时。

显存/RSS 必须另行采样，采样设备需匹配 HIP 的 PCI 地址。本次 50 ms 采样包装器、CPU 比较、重启和 profiler 完整命令均在证据归档中。
CPU 250K 对比将独立 CPU 表的 DDL 设置为 `lib=vsag,ef_search=512`，其余 m/ef_construction 与上表一致；
同时保留 `lib=vsag,ef_search=64` 的公开 10K 基本功能回归。两者均在 GPU 初始化后的同一候选服务中运行。

## 功能、持久化与 GPU 路径

```bash
python tools/hipvs_bridge/tests/run_host_tests.py
/opt/venv/bin/python tools/hipvs_bridge/tests/gpu_smoke.py \
  --library /work/candidate-bridge/libseekdb_cuvs_bridge.so \
  --base /seekdb_workspace/datasets/base.f32 \
  --queries /seekdb_workspace/datasets/query.f32 \
  --require-compact --output /work/gpu-smoke.json
```

`gpu_smoke.py` 检查真实 GPU 单查、批量、并发、图快照逐边一致、恢复后输入所有权和搜索。
本次另在新进程重启读取 OBCUVS03，并由候选读取实际 V5 生成的 OBCUVS02，CPU 索引重启也通过。
rocprofv3 记录了 native NN-descent、prune/reverse-graph 和 MULTI_CTA 搜索 kernel；原始 CSV 随证据保存。
该跟踪对象是独立 bridge 功能测试，不能代替组织方完整 SQL GPU 抽检。

## 交付边界

生产代码未引入自定义 GPU 建图器、内部 FP16 或第三方依赖，未改动固定 hipVS。筛选中未采用的原型不参与编译。
上一候选正式得分为 10.706；本轮正式分数和 SIFT250K/Hidden250K 门槛仍待提测，不用本地合成数据估算正式分数。
