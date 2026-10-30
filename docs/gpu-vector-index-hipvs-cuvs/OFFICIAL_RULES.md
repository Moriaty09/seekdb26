# AMD 专项官方规则核对（2026-10-30）

来源：[官方 AMD 题目](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb)。
页面正文更新时间为 2026-10-29 23:12（北京时间）。下文为执行要点，不代替官方全文。

## 基线与提交

| 项目 | AMD 专题页要求 |
|---|---|
| 比赛基线仓库 | https://github.com/zihaomu/seekdb |
| 比赛分支 | amd-oceanbase-official-contest-base-v5 |
| 冻结 Tag | amd-cagra-contest-baseline-v5 |
| 冻结 commit | 634888d9b037153fa143d2c913ba4b25503e1d62 |
| 答题仓库 | 私有仓库，提交必须是冻结 commit 的后代 |
| 提测 | 比赛平台绑定仓库，指定分支/commit；组织方离线重新编译并在 AMD GPU 上配对评测 |

GitHub 已核对上述 AMD 分支 HEAD 与冻结提交一致。本地现有源码也基于该提交。
官方示例参赛分支名称为 `amd-cagra-submit`，并无要求参赛分支必须叫这个名字；本地分支为 `hipvs-optimize`。
AMD 专题页明确有独立云端评测；通用介绍中的 PR/GitHub Actions 流程不能代替这条要求。
2026-competition 当前 HEAD 为 `5bce9e06d1281347ad61344e4b01fab9158c58a2`，与 V5 分叉；
`vldb_2026` 查询返回分支不存在。应使用上表已核验的 AMD 基线。

仓库地址中携带只读令牌的格式要求仅适用于比赛平台登记；聊天、提交、文档及 Git remote 不保存令牌。
按用户指定，答题仓库固定为 `https://github.com/Moriaty09/seekdb26`，不另建仓库。
此前核查其为公开 fork，当前连接无管理员权限；正式提测前仍需由所有者按比赛要求处理可见性并登记。

## 固定构建命令

将 `<输出目录>` 替换为绝对路径，其余参数不变：

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh <输出目录>/libseekdb_cuvs_bridge.so
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=<输出目录>/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

`bash tools/hipvs_bridge/build_contest.sh /work/bridge` 封装以上命令，不追加 CMake 或构建选项。
它没有替代评测入口；组织方仍可在仓库根目录直接执行原命令。
编译资源上限为 64 核/128 GB/2 小时；hipVS 和冻结第三方依赖保持不变。

## 固定 SQL 与计时

计时前 LOAD DATA 已完成，表上无向量索引，且无可复用图/缓存。

```sql
CREATE VECTOR INDEX idx_items ON items(v)
  WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);
SELECT id FROM items ORDER BY l2_distance(v, <query>) APPROXIMATE LIMIT <k>;
```

`<query>` 和 `<k>` 使用组织方固定输入。完整构建计时包含 CREATE INDEX 到首次固定查询返回合法结果，
包括扫描、传输、GPU 建图同步、持久化及首次重载。后续 nq=4096 是逐条执行单行查询 SQL。

每个数据集 baseline/candidate 各运行一次，配对顺序为：A：baseline → candidate；B：candidate → baseline。
两个数据集权重分别为 SIFT250K 0.4、Hidden250K 0.6。

## 评分与硬门槛

令各数据集构建比值 `B_i = T_baseline / T_candidate`，
查询比值 `Q_i = sqrt((QPS_candidate10 / QPS_baseline10) * (QPS_candidate100 / QPS_baseline100))`。

```text
B = B_SIFT^0.4 * B_Hidden^0.6
Q = Q_SIFT^0.4 * Q_Hidden^0.6
构建分 = 14 * clamp(ln(B) / ln(3.0), 0, 1)
查询分 =  2 * clamp(ln(Q) / ln(1.5), 0, 1)
全部硬门槛通过：总分 = 4 + 构建分 + 查询分
任意硬门槛失败：总分 = 0
```

每个数据集自身 Q_i 必须至少 0.90；综合平均不能掩盖单个数据集查询回归。
SIFT Recall@10/100 ≥0.95；Hidden Recall@10 ≥0.90、@100 ≥0.85；各项相对配对基线下降不超过 0.005。
返回 k 个合法且互异的外部 ID。真实 GPU 建图/规定的 hipVS CAGRA 查询、CPU 基本功能、
索引完整性、稳定性、编译限制、15 分钟 GPU 轮次及资源峰值还须独立核验。
显存同时满足 ≤配对基线 1.25 倍和 ≤45 GB；进程 RAM≤96 GB、CPU≤24 核。

`score_runs.py` 仅做本地估分。没有完整外部门槛证据时输出 `local_score: null`，
不会把未测项默认当作通过。Hidden 数据由组织方保管，不能用自建 256D 数据宣称完成正式估分。

## GPU 环境入口

- [官方 Radeon 部署指南](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lcuknwgl955v8ou7)
- [AMD 开发者计划](https://developer.amd.com.cn/login?source=0u9Eqno7R)
- [Radeon Cloud 平台与验证指南](https://amd-aim.github.io/radeon-cloud-docs/introduction/)
- [Radeon Cloud 中国站登录入口](https://developer.amd.com.cn/radeon/)
- [AMD 赛事算力最新公告](https://github.com/AMD-DEV-CONTEST/Oceanbase-2026-competition-AMD-Doc)

用户先完成个人账号注册、Radeon Cloud 验证和赛事登记，然后在 Gallery 搜索 `seekdb-hipvs` 模板。
2026-10-30 已实测：中国站入口必须保留末尾 `/`；`/radeon` 返回开发者站并可能显示 404，
`/radeon/` 返回标题为 Radeon Cloud 的页面。直接打开上面的完整入口。
模板预装组件不等于候选代码已编译；实验仍需执行上述固定源码构建。
最新算力公告含 10 月 30 日至 11 月 4 日的额度调整，优先查看最新公告及账户状态，
不按旧截图中的手动兑换方式推断当前额度。账号验证需要本人完成。
准备好实例后提供已有 SSH 别名及非敏感连接信息；密码、令牌和私钥不发送到聊天。
实例最长 4 小时，文件不持久化，释放前同步源码和测试证据回本地。
