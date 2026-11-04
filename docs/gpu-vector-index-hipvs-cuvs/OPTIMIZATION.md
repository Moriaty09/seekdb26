# V5 CAGRA 外围优化与复现方法

2026-11-04 在外围优化之上加入原生 CAGRA 建图/搜索联合调参；当前参数和完整复现步骤见
[CAGRA_BUILD_SEARCH_TUNING.md](CAGRA_BUILD_SEARCH_TUNING.md)，实测结果见 [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md)。

2026-11-02 新增无损紧凑快照和 OpenMP 预算优化，详见 [COMPACT_SNAPSHOT.md](COMPACT_SNAPSHOT.md)。
新版本写 OBCUVS03、继续读 OBCUVS02；真实 ROCm 编译、GPU 功能、SQL 配对和重启检查已完成。
本轮数值及适用范围见 [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md)。

## 当前交付状态

基线：`634888d9b037153fa143d2c913ba4b25503e1d62`。
官方 AMD 比赛仓库为 `zihaomu/seekdb`，分支为 `amd-oceanbase-official-contest-base-v5`。
完整专项规则与已核对的构建命令见 [OFFICIAL_RULES.md](OFFICIAL_RULES.md)。
代码、主机控制流程测试和本地 SQL 测试工具已经准备；真实 ROCm bridge 编译和公开 10K 样例 GPU 功能检查已通过。
完整 seekdb 编译、公开样例及 250K 合成 SQL 正确性与重启测试均已通过；本轮构建和查询均有实测提升。
实测方法、原始数据和 profiler 限制见 PERFORMANCE_REPORT.md；公开样例不证明正式数据集门槛。

## 修改内容

- `cuvs_worker.h` 为建图、快照保存/重载、单查和批量查询复用一个 32 MiB 栈的 GPU 工作线程。
  同步提交会等待任务完成，再返回调用方；不在工作线程中缓存图或查询结果。
  保留 hipVS 所需的大栈，减少每条 SQL 的 pthread 创建/销毁和 GPU 线程状态初始化。
  工作线程全局串行执行任务；同一进程的原始批量调用也会排队，跨调用并行吞吐尚未验证。
- `seekdb_cuvs_bridge.c` 为每个索引复用单查询输入/输出显存和搜索参数。
  仅缓存 `nq=1, k<=1024, dim<=4096` 的缓冲区，最大 24 KiB；增长时新旧缓冲区短暂并存。
  三个区域按 256 字节间隔对齐。批量查询保留临时分配方式，不增大缓存。
- 句柄内互斥保护搜索状态；销毁句柄前，调用方仍需保证不再有搜索或序列化操作。
- 每次搜索重新设置 `itopk_size=max(库默认值,4096,4*k)`、`search_width=4` 和原生 `MULTI_CTA`，避免先查大 k 导致后续小 k 参数变化。
- 建图使用 NN_DESCENT、graph_degree=128、intermediate_graph_degree=128、nn_descent_niter=3。
  数据保持 FP32，查询仍调用固定 hipVS CAGRA 搜索。
- 新快照直接保存 GPU 图和一份原始 FP32 向量，消除重复向量与临时文件中转。
  OBCUVS03 通过固定 hipVS 的 IndexFromArgs 恢复同一张图；不额外建图。
  旧 OBCUVS02 读取仍使用 4 MiB 中转块；详细格式、兼容性和资源开销见 COMPACT_SNAPSHOT.md。
- 首次建图根据 CPU 亲和与配额配置 OpenMP 默认 team，最多 16 线程，默认关闭 blocktime 忙等。
  尊重显式 OpenMP 设置；仅使用既有运行时的公开接口。库自身的建图、收敛和搜索实现保持不变。
- 距离回拷贝失败返回非零值；增加参数/溢出检查，DLPack shape 使用明确的 int64_t。
- `build.sh` 添加内部 pthread/dl 链接选项。对外构建入口不变，不新增第三方依赖。

2026-11-02 版本保持 V5 算法参数；2026-11-04 版本联合调整建图和搜索参数，具体质量验证见调参文档。
性能按完整 SQL 计时与独立 GPU 诊断分别报告，正式数据集成绩仍由组织方确认。

## 已完成的主机检查

在 Windows 的既有 g++ 上运行：

```powershell
python tools/hipvs_bridge/tests/run_host_tests.py --temp-dir D:/projects/backups
python tools/hipvs_bridge/tests/test_benchmark.py D:/projects/backups
python tools/hipvs_bridge/tests/test_gpu_result_equivalence.py
git diff --check
git merge-base --is-ancestor 634888d9b037153fa143d2c913ba4b25503e1d62 HEAD
```

Linux 可省略两个测试命令的临时目录参数。
工作线程测试还覆盖 800 个并发同步提交、同一线程执行、32 MiB 栈（Linux）、异常恢复、嵌套调用和退出。
替身测试覆盖预热后 100 次搜索零新增分配/参数创建、Top-k 切换、批量临时缓冲区释放、
错误重试、同句柄并发序列化搜索、快照重载及全部测试资源释放。
另一个测试使用假数据库跑完整 4096 查询循环，验证 ID 检查、Recall 计算和不配对输入拒绝。
这些测试不依赖 ROCm，不验证 hipVS 头文件或 ABI，也不作为性能数据。

## GPU 环境准备及编译

按组织方部署文档准备 Linux、AMD Radeon PRO W7900D 和 ROCm 7.2.4，
GCC 13.3、CMake 3.31、Rust 1.97。hipVS 固定提交为
`f96a663d0a4441d6f5344bd3ef552b71ae7b370b`，放在 `/opt/hipvs`；ROCm 放在 `/opt/rocm`。
使用冻结 deps/3rd 和 Rust vendor，离线构建；不修改 hipVS。
已从官方 AMD 专题正文核对固定构建命令，封装为以下脚本。
脚本执行官方原命令，不追加 CMake/build 选项。

```bash
bash tools/hipvs_bridge/gpu_preflight.sh /work/evidence/preflight
bash tools/hipvs_bridge/build_contest.sh /work/bridge
```

使用组织方固定 profiler 检查实际构建与查询 GPU 路径。
复用 V5 的 `validation/run_ddl_persistence_smoke.sh` 验证离线 DDL、重启重载和 CPU 索引功能；
该脚本需要其已有的隔离容器、运行目录和 trace 配置，不能直接针对正式服务器运行。
复用 `validation/run_gpu_smoke.sh` 检查原有功能。部署/trace 环境详见同目录 README。

## 本地 CPU/GPU 对比

### 模板公开 10K 样例

可在 Radeon 模板中使用现有 NumPy 生成诊断输入：

```bash
/opt/venv/bin/python tools/hipvs_bridge/tests/prepare_public_sample.py \
  --base /seekdb_workspace/datasets/base.f32 \
  --queries /seekdb_workspace/datasets/query.f32 --output /work/fixtures
```

输出目录必须不存在。脚本保持 FP32 数值，使用可往返 FP32 的 9 位有效数字导出 TSV，
外部 ID 为非连续的 `row_offset * 17 + 1009`，验证 GPU 行偏移到 SQL ID 的映射。
100 个公开查询循环至 4096 条，真值用直接距离计算得到；不把它当作正式数据集。
在独立、无索引数据库执行（路径改为实际生成目录）：

```sql
CREATE DATABASE bench_public;
USE bench_public;
ALTER SYSTEM SET ob_vector_memory_limit_percentage=30;
CREATE TABLE items(id BIGINT PRIMARY KEY, v VECTOR(128));
LOAD DATA LOCAL INFILE '/work/fixtures/items.tsv' INTO TABLE items
FIELDS TERMINATED BY '\t' LINES TERMINATED BY '\n' (id,v);
```

客户端需启用 LOCAL INFILE，例如模板已有的 `mysql --local-infile=1`。
然后使用下文 benchmark 命令，数据集名改为 `PublicSample10K`、行数改为 10000，
传入 `--client mysqlclient` 和对应的生成文件。两版各自使用全新服务目录和数据库。
开发中未提交的候选可用 `--server-commit <基线完整SHA> --source-patch <完整补丁>`
同时记录基线与补丁 SHA-256；正式测试应记录已提交候选的完整 commit。

### 样例自动配对与重启核验

公开样例自动配对（先停止占用同一端口的测试服务，保留至少 30 GiB 可用空间）：

```bash
/opt/venv/bin/python tools/hipvs_bridge/tests/run_public_pair.py \
  --baseline-bin /work/baseline/seekdb \
  --baseline-bridge /work/baseline/libseekdb_cuvs_bridge.so \
  --candidate-bin /work/candidate/seekdb \
  --candidate-bridge /work/candidate/libseekdb_cuvs_bridge.so \
  --fixtures /work/fixtures --source-patch /work/tested-source.patch \
  --output /work/pair-1 --order baseline,candidate
```

fixtures 还需 environment.json，记录实际设备/软件/服务参数，可用以下只读采集命令生成：

```bash
/opt/venv/bin/python tools/hipvs_bridge/tests/capture_environment.py \
  --output /work/fixtures/environment.json
```

采集程序仅记录现状，不加载 hipVS 或认证硬件/门槛。本次环境记录已保存在交付证据中。
输出目录必须不存在。第二轮交换 order 为 candidate,baseline，并使用新的输出目录。
工具创建自己的服务目录，完成后停止服务，保留数据与日志；旧实验数据需先保存证据再清理。
实际 seekdb 运行目录没有 .meta 文件，重启已有目录时直接调用同一二进制和原启动参数，
不要用以 .meta 为判断条件、会删除目录的初始化包装脚本。

SQL 重启功能检查可用 gpu_sql_smoke.py：指定 binary、bridge、base-dir、fixtures、output、
gpu-database、cpu-database，均指向上述隔离实验；它会重启旧 GPU/CPU 索引并新建另一个 GPU 图。
本镜像中 rocprofv3 导致 V5 和候选均在建索引前退出，尚未定位原因；
可显式传 --profiler none 只核对 SQL 功能，不将其作为 profiler 抽检成功。

### 250K 合成规模复现

以下命令使用模板已有的 NumPy 和离线编译产物。合成输入用于规模诊断，标签固定以 Local 开头，
不能填成正式 SIFT250K 或 Hidden250K。为复现生成文件哈希，使用记录的 NumPy 版本和 BLAS 线程数。

```bash
OPENBLAS_NUM_THREADS=8 /opt/venv/bin/python \
  tools/hipvs_bridge/tests/prepare_clustered_sample.py \
  --dimension 128 --output /work/raw128
/opt/venv/bin/python tools/hipvs_bridge/tests/prepare_public_sample.py \
  --base /work/raw128/base.f32 --queries /work/raw128/query.f32 \
  --dimension 128 --dataset LocalSyntheticClusters250K128D --output /work/sql128
/opt/venv/bin/python tools/hipvs_bridge/tests/capture_environment.py \
  --output /work/sql128/environment.json
git diff --binary 634888d9b037153fa143d2c913ba4b25503e1d62 HEAD > /work/candidate.patch
/opt/venv/bin/python tools/hipvs_bridge/tests/run_public_pair.py \
  --baseline-bin /work/baseline/seekdb \
  --baseline-bridge /work/baseline/libseekdb_cuvs_bridge.so \
  --candidate-bin /work/candidate/seekdb \
  --candidate-bridge /work/candidate/libseekdb_cuvs_bridge.so \
  --fixtures /work/sql128 --source-patch /work/candidate.patch \
  --output /work/pair128 --order baseline,candidate
```

256 维分别将 dimension 改为 256、标签改为 LocalSyntheticClusters250K256D，
所有 raw/sql/pair 输出目录换成新的 256 后缀目录，order 使用 candidate,baseline。
默认生成 250000 个向量、64 个独立查询，后者循环为 4096 条单行 SQL。
配对工具为导入会话统一设置 300 秒超时；导入在计时外，建图和查询仍执行固定 SQL。
--source-patch 必须对应实际编译源码；上述 git diff 适用于已提交且工作区干净的候选。
补丁在本地结果中与 V5 完整 SHA 一起标识候选，服务二进制另记录 SHA-256。
未提交改动也需纳入补丁，不能用 HEAD 补丁标识不同的二进制。

CPU 回归可在另一张干净表运行生成的 ddl-cpu.sql，再用 benchmark_sql.py 的 cpu-vsag 变体。
本轮 250K CPU 对比将 CPU ef_search 显式提高到 512，并报告实际 Recall；这项差异必须保留在报告中，
不能作为严格同质量的 CPU/GPU 加速倍数。实际数值见 PERFORMANCE_REPORT.md。

### 正式规模配对方法

`tools/hipvs_bridge/benchmark_sql.py` 使用仓库已有工具引用的 `mysql.connector`，不自动安装包。
若实验镜像没有 Python connector，但已安装系统 libmysqlclient，
可传 `--client mysqlclient`，通过 ctypes 使用现有 MySQL C 客户端，无需安装包。
两版配对必须使用同一客户端；该适配器仅供单连接、单活动 cursor、文本 SQL 诊断测试。
脚本不参与 seekdb 的离线编译。
官方 DDL 已保存为 `tools/hipvs_bridge/contest_ddl.sql`。
查询遵循 OFFICIAL_RULES.md 中的固定 SQL 模板，向量与真值由调用者提供。

为每个数据集准备：

1. 原始 FP32 数据导入后的独立数据库；计时前无向量索引、旧图、快照或进程缓存。
2. `ddl.sql`：一条原样的官方 CREATE VECTOR INDEX 语句；`first.sql`：一条固定首次查询，仅返回 ID 列。
3. `valid_ids.txt`：原始数据全部合法外部 ID，一行一个整数，不能用连续行号替代任意外部 ID。
4. `queries.jsonl`：恰好 4096 行；每行包含 `sql_10`、`sql_100` 和 `truth_100`。
   两条 SQL 分别查询同一个向量的 Top-10/100，只返回 ID 列；真值为对应查询的精确 Top-100 外部 ID，按距离顺序排列。
5. `environment.json`：记录设备型号、系统、驱动、ROCm/hipVS/GCC/CMake/Rust 版本、CPU 核数/绑核配置，
   数据集、服务配置及 profiler 配置。配对运行使用同一配置文件。
6. 原始导入文件 SHA-256、运行服务器的真实源码 commit、该服务器二进制路径。

以下示例中的文件路径和 commit 需替换为已核验的实验值：

```bash
export MYSQL_HOST=127.0.0.1 MYSQL_PORT=2881 MYSQL_USER=root
# 有密码时在当前环境设置 MYSQL_PWD，勿写入提交或报告。
python tools/hipvs_bridge/benchmark_sql.py \
  --variant baseline-gpu --dataset SIFT250K --dimension 128 --rows 250000 \
  --database bench_sift --ddl /work/fixtures/ddl.sql \
  --first-query /work/fixtures/first.sql --first-k 10 \
  --queries /work/fixtures/queries.jsonl --valid-ids /work/fixtures/valid_ids.txt \
  --data-sha256 "$DATA_SHA256" \
  --server-commit 634888d9b037153fa143d2c913ba4b25503e1d62 \
  --server-binary /work/baseline/build_release/src/observer/seekdb \
  --environment /work/fixtures/environment.json --output /work/results/sift-baseline.json
```

重建干净实验状态并切换至候选服务，使用相同输入运行 `--variant candidate-gpu`，
填写候选真实 commit/二进制并输出 `sift-candidate.json`。
第三次在独立干净表上使用 `lib=vsag` 的对应 DDL 和 `--variant cpu-vsag`，输出 `sift-cpu.json`；
此项仅补充 CPU/GPU 诊断，CPU DDL 的库选项与 GPU 不同，不替换正式评测 SQL。
CPU 路径仍需验证 SQL 建表、建索引、查询及合法 ID。

```bash
python tools/hipvs_bridge/compare_runs.py \
  --baseline /work/results/sift-baseline.json \
  --candidate /work/results/sift-candidate.json \
  --cpu /work/results/sift-cpu.json --output /work/results/sift-report.md
```

计时从执行 DDL 前到首条查询结果取回后，包含索引持久化及首次查询触发的重载/同步。
4096 查询逐条执行，复用同一个数据库连接；每组 QPS 计入查询及客户端循环开销，
P95 使用逐条 execute/fetch 延迟的最近秩估计。ID/Recall 检查放在该组计时结束后。
脚本预先读取文件和计算哈希，避免将其计入建图时间。
脚本不自动删除数据库、清除缓存、启动/重启服务、导入数据或申请云端资源。
出现查询错误或 ID 不完整会非零退出，并保留能记录的诊断 JSON。

官方配对顺序为数据集 A：baseline → candidate，数据集 B：candidate → baseline。
本地按此交叉顺序执行，可额外重复若干次观察波动。
Hidden250K 未公开，不生成、推测或替代它；其它 256 维数据只能作为本地诊断。
每轮重新建立无索引状态。外部记录峰值 VRAM/RAM、GPU profiler、日志、服务版本和顺序。
要求显存不超过本轮基线 1.25 倍且不超过 45 GB、进程 RAM 不超过 96 GB、CPU 24 核。
本地脚本不自动宣称 GPU 路径/资源门槛通过，也不代替组织方正式评测。

完整正式数据集结果可用以下命令本地汇总，缺少外部门槛证据时分数为 null：

```bash
python tools/hipvs_bridge/score_runs.py \
  --sift-baseline /work/results/sift-baseline.json \
  --sift-candidate /work/results/sift-candidate.json \
  --hidden-baseline /work/results/hidden-baseline.json \
  --hidden-candidate /work/results/hidden-candidate.json \
  --output /work/results/local-score.json
```

证据全部核验后可传 `--evidence external-gates.json`，键名见 score_runs.py 中 EXTERNAL_GATES，
值为核验结果 true/false/null。该文件是本地实验记录，不能视为组织方认证。
隐藏数据不公开时无需运行此汇总，更不能自行构造隐藏数据或结果。

## 验证完成与正式待测

- 已完成真实离线源码编译、10K 与合成 250K 完整 SQL 配对、ID/Recall 检查、新旧快照重载和 lib=vsag SQL 回归。
- 已保存 CPU/V5 GPU/候选 GPU 实测报告、独立 bridge 的 GPU kernel 跟踪与完整 SQL 资源采样。
- 正式 SIFT250K/Hidden250K 尚未实测；镜像 hipVS 身份仅有库哈希，固定 commit 身份及全部硬门槛由组织方核验。
  本地完整 SQL profiler 在两个版本上均于 ready 前 SIGABRT，不能视为正式 GPU 抽检通过。
- 按用户选择继续使用 `Moriaty09/seekdb26`；正式提测前由所有者处理题面要求的仓库可见性和登记。
  评测登记令牌只在比赛平台保存，不写入本仓库、文档或聊天。
- 提交后核验候选为 V5 后代；推送到答题仓库后，再填写分支及完整 commit，不能直接使用已分叉的远端 master。

修改前备份保存在本地 `D:/projects/backups/seekdb26-v5`，原始 V5 归档为
`D:/projects/seekdb26-v5.zip`；这些本地备份不属于提交源码。
