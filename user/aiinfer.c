#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/riscv.h"
#include "user/user.h"

#include "ai_path.h"

// aiinfer 是本实验的端到端 workload。它通过 ai_path 运行 baseline、student
// 或 prefetch 通路，并在相同的 prefill、KV 生命周期和 decode 计算下比较
// 正确性与性能。学生通常不修改本文件，而应从这里反推各接口的调用契约。

struct worker_result {
  // 每个 worker 独立产生逐请求输出、延迟与 I/O；父进程最后统一聚合。
  uint checksum;
  uint output[AI_MAX_REQUESTS];
  uint latency[AI_MAX_REQUESTS];
  struct ai_io io;
};

struct worker_workspace {
  // workspace 保存无需持久化的推理中间量。真正被测试的 KV 页面不保留在
  // 这里，而是写盘、释放、重新申请并从文件恢复。
  int query[AI_MAX_REQUESTS][AI_DIM];
  uint partial[AI_MAX_REQUESTS];
  uint kv_checksum[AI_MAX_REQUESTS];
  uint begin_tick[AI_MAX_REQUESTS];
};

struct lock_stats {
  uint kmem_spins;
  uint bcache_spins;
  int total_spins;
};

struct path_result {
  struct worker_result worker[AI_MAX_WORKERS];
  uint checksum;
  uint p95_ticks;
  struct ai_io io;
  struct lock_stats stats;
  int elapsed_ticks;
};

static char stats_buffer[4096];
static struct worker_workspace workspace;
// 较大结果结构放在静态区，避免超过 xv6 的单页用户栈。
static struct path_result single_run;
static struct path_result compare_baseline;
static struct path_result compare_student;
static struct path_result compare_prefetch;

static void
fail(char *message)
{
  printf("AIINFER: 失败：%s\n", message);
  exit(1);
}

static int
read_exact_result(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  // pipe 和文件都是字节流；父子进程传递结果时也必须循环处理短读。
  while(length > 0) {
    n = read(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

static void
write_exact_or_fail(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  // 启动信号和 worker_result 必须完整写入管道，部分消息无法安全解释。
  while(length > 0) {
    n = write(fd, p, length);
    if(n <= 0)
      fail("管道写入不完整");
    p += n;
    length -= n;
  }
}

static int
parse_positive(char *text)
{
  int value = 0;

  if(*text == 0)
    fail("参数不能为空");
  while(*text) {
    if(*text < '0' || *text > '9')
      fail("参数必须是正整数");
    value = value * 10 + *text - '0';
    text++;
  }
  if(value <= 0)
    fail("参数必须大于零");
  return value;
}

static struct ai_path *
parse_mode(char *mode)
{
  if(strcmp(mode, "baseline") == 0)
    return &ai_baseline_path;
  if(strcmp(mode, "student") == 0)
    return &ai_student_path;
  if(strcmp(mode, "prefetch") == 0)
    return &ai_prefetch_path;
  return 0;
}

static void
verify_assets(void)
{
  struct ai_manifest manifest;
  int fd;

  // aimeta 是模型资产的入口。先验证版本、规模和整体 checksum，避免在
  // 缺失或过期的分片集合上运行性能测试。
  fd = open("aimeta", O_RDONLY);
  if(fd < 0)
    fail("请先运行 modelprep");
  if(read_exact_result(fd, &manifest, sizeof(manifest)) < 0) {
    close(fd);
    fail("模型清单不完整");
  }
  close(fd);
  if(manifest.magic != AI_MAGIC || manifest.version != AI_VERSION ||
     manifest.model_shards != AI_MODEL_SHARDS ||
     manifest.embed_shards != AI_EMBED_SHARDS ||
     manifest.shard_bytes != AI_SHARD_BYTES ||
     manifest.checksum != ai_expected_checksum())
    fail("模型清单内容错误");
}

static uint
shard_feature(struct ai_path *path, struct ai_session *session,
              uint family, uint shard, struct ai_io *io)
{
  char block[AI_SHARD_BYTES];
  uint feature = 0;
  int offset;

  // workload 只通过 path->load 获取分片：baseline 会每次读文件，student
  // 可以缓存，但两者交付给后续计算的 1024 字节必须完全相同。
  if(path->load(session, family, shard, block, io) < 0)
    fail("模型或 embedding 分片加载失败");
  // 再逐字节核验并压缩成一个确定性 feature，使错误尽量在数据入口暴露。
  for(offset = 0; offset < AI_SHARD_BYTES; offset++) {
    if((uchar)block[offset] != ai_byte(family, shard, offset))
      fail("模型或 embedding 分片内容损坏");
    feature = feature * 131 + (uchar)block[offset];
  }
  return feature;
}

static uint
kv_checksum(struct kv_entry *kv)
{
  uchar *bytes = (uchar *)kv;
  uint checksum = 0;
  int offset;

  // checksum 覆盖将要写盘的完整 2048 字节，恢复后再次计算即可确认
  // 文件 I/O 没有丢字节、错序或交付其他请求的 KV。
  for(offset = 0; offset < AI_KV_FILE_BYTES; offset++)
    checksum = ai_checksum_step(checksum, bytes[offset]);
  return checksum;
}

static void
prefill_request(struct ai_path *path, struct ai_session *session,
                int worker, int request, struct kv_entry *kv,
                struct ai_io *io)
{
  uint model_feature;
  uint embed_feature;
  uint result = worker * 97 + request * 53 + 1;
  uint token = (worker * 7 + request * 13) % AI_VOCAB;
  int step;
  int dim;

  // 一个请求的延迟从 prefill 开始计时；后续模型读取、KV 写盘、排队、
  // 恢复和 decode 都包含在该请求的端到端延迟中。
  workspace.begin_tick[request] = uptime();
  // 每个 token 步骤各读取一个模型分片和 embedding 分片，再生成简化的
  // key/value。重复访问让模型缓存优化具有可观察收益。
  for(step = 0; step < AI_KV_TOKENS; step++) {
    model_feature = shard_feature(path, session, AI_MODEL_FAMILY,
                                  (request + step) % AI_MODEL_SHARDS, io);
    embed_feature = shard_feature(path, session, AI_EMBED_FAMILY,
                                  (worker * 11 + request * 7 + step) %
                                  AI_EMBED_SHARDS, io);
    for(dim = 0; dim < AI_DIM; dim++) {
      kv[step].key[dim] =
        ((model_feature >> ((dim % 4) * 8)) & 0x1f) - 16;
      kv[step].value[dim] =
        ((embed_feature >> (((dim + 1) % 4) * 8)) & 0x1f) - 16;
      workspace.query[request][dim] =
        ((token + dim * 3 + step) & 0x1f) - 16;
    }
    result = result * 33 + model_feature + embed_feature + token;
    token = result % AI_VOCAB;
  }
  // query 和 partial 属于普通中间状态；KV checksum 则用于验证随后真实
  // 写盘并恢复的那一页数据，而不是允许保存 KV 副本。
  workspace.partial[request] = result;
  workspace.kv_checksum[request] = kv_checksum(kv);
}

static uint
decode_request(int request, struct kv_entry *kv)
{
  int best_score = -2147483647;
  int best_index = 0;
  int score;
  uint result = workspace.partial[request];
  int round;
  int step;
  int dim;

  // 对 query 与每条 key 做点积，选择分数最高的 value。这里是简化的
  // attention-style CPU 计算，不追求真实模型精度，只保留 KV 数据依赖。
  // 重复若干轮也给选做 prefetch 留出与下一条磁盘读取重叠的时间。
  for(round = 0; round < AI_ATTENTION_ROUNDS; round++) {
    best_score = -2147483647;
    best_index = 0;
    for(step = 0; step < AI_KV_TOKENS; step++) {
      score = 0;
      for(dim = 0; dim < AI_DIM; dim++)
        score += workspace.query[request][dim] * kv[step].key[dim];
      score += round & 1;
      if(score > best_score) {
        best_score = score;
        best_index = step;
      }
    }
    result = result * 17 + (uint)best_score;
  }
  for(dim = 0; dim < AI_DIM; dim++)
    result = result * 17 + kv[best_index].value[dim];
  return result;
}

static void
worker_main(struct ai_path *path, int worker, int requests,
            int start_fd, int result_fd)
{
  struct worker_result result;
  struct ai_session session;
  struct kv_entry *kv;
  uint before;
  char start;
  int request;

  // 每个子进程拥有独立 session、workspace 和结果。begin 在启动屏障前
  // 完成初始化，屏障释放后多个 worker 才同时进入被测数据路径。
  memset(&result, 0, sizeof(result));
  memset(&workspace, 0, sizeof(workspace));
  if(path->begin(&session, worker, requests) < 0)
    fail("学生通路尚未实现或初始化失败");
  if(read_exact_result(start_fd, &start, 1) < 0)
    fail("启动屏障读取失败");
  close(start_fd);

  // 阶段一：prefill 生成 KV，通路把完整记录写入 xv6 文件系统，然后立即
  // 释放原页面。释放后任何实现都不能依赖旧 kv 指针或内存残留。
  if(path->begin_kv_write(&session) < 0)
    fail("无法开始 KV 写入阶段");
  for(request = 0; request < requests; request++) {
    kv = (struct kv_entry *)sbrk(PGSIZE);
    if(kv == (void *)-1)
      fail("无法申请 KV 页面");
    prefill_request(path, &session, worker, request, kv, &result.io);
    before = result.io.kv_write_bytes;
    if(path->store_kv(&session, request, kv, &result.io) < 0)
      fail("KV 写盘失败");
    if(result.io.kv_write_bytes - before != AI_KV_FILE_BYTES)
      fail("KV 写入字节数不正确");
    // store 成功后马上归还页面，强制后面的 decode 使用文件恢复结果。
    if(sbrk(-PGSIZE) == (void *)-1)
      fail("无法释放原 KV 页面");
  }
  if(path->end_kv_write(&session) < 0)
    fail("无法结束 KV 写入阶段");
  if(path->begin_kv_read(&session) < 0)
    fail("无法开始 KV 恢复阶段");

  // 阶段二：首条预取没有前一条 decode 可以重叠，所以循环前先发起并等待。
  if(path->prefetch) {
    if(path->prefetch_next(&session, 0) < 0 ||
       path->prefetch_wait(&session, 0) < 0)
      fail("首条 KV 预取失败");
  }
  for(request = 0; request < requests; request++) {
    // 重新申请的页面先用固定字节污染，防止“未真正恢复却碰巧校验通过”。
    kv = (struct kv_entry *)sbrk(PGSIZE);
    if(kv == (void *)-1)
      fail("无法重新申请 KV 页面");
    memset(kv, 0xa5, PGSIZE);
    before = result.io.kv_read_bytes;
    if(path->restore_kv(&session, request, kv, &result.io) < 0)
      fail("KV 恢复失败");
    if(result.io.kv_read_bytes - before != AI_KV_FILE_BYTES)
      fail("KV 读取字节数不正确");
    if(kv_checksum(kv) != workspace.kv_checksum[request])
      fail("恢复后的 KV 内容错误");

    // 稳态预取顺序：恢复 i 后发起 i+1，计算 i，最后才等待 i+1。
    // next 和 wait 之间的 decode 是形成 I/O/计算重叠的关键窗口。
    if(path->prefetch && request + 1 < requests &&
       path->prefetch_next(&session, request + 1) < 0)
      fail("下一条 KV 预取启动失败");
    result.output[request] = decode_request(request, kv);
    result.checksum += result.output[request];
    result.latency[request] = uptime() - workspace.begin_tick[request];
    if(sbrk(-PGSIZE) == (void *)-1)
      fail("无法释放恢复后的 KV 页面");
    if(path->prefetch && request + 1 < requests &&
       path->prefetch_wait(&session, request + 1) < 0)
      fail("下一条 KV 预取等待失败");
  }

  // 通路先回收文件、缓存和可选辅助进程，再把完整 worker_result 交给父进程。
  path->end(&session);
  write_exact_or_fail(result_fd, &result, sizeof(result));
  close(result_fd);
  exit(0);
}

static int
parse_number(char *text)
{
  int value = 0;

  if(*text == '-')
    return -1;
  while(*text >= '0' && *text <= '9') {
    value = value * 10 + *text - '0';
    text++;
  }
  return value;
}

static int
matches_prefix(char *text, char *prefix, int length)
{
  int i;

  for(i = 0; i < length; i++)
    if(text[i] != prefix[i])
      return 0;
  return 1;
}

static char *
find_text(char *text, char *needle)
{
  int length = strlen(needle);

  while(*text) {
    if(matches_prefix(text, needle, length))
      return text;
    text++;
  }
  return 0;
}

static uint
prefix_spins(char *prefix)
{
  char *line = stats_buffer;
  char *end;
  char *field;
  int prefix_length = strlen(prefix);
  uint total = 0;

  // statistics() 返回文本快照。按锁名前缀累加 #fetch-and-add，使学生拆分
  // 出的 kmem*/bcache* 多把锁仍能归入同一类竞争指标。
  while(*line) {
    end = line;
    while(*end && *end != '\n')
      end++;
    if(matches_prefix(line, "lock: ", 6) &&
       matches_prefix(line + 6, prefix, prefix_length)) {
      field = find_text(line, "#fetch-and-add ");
      if(field && field < end)
        total += parse_number(field + strlen("#fetch-and-add "));
    }
    line = *end ? end + 1 : end;
  }
  return total;
}

static int
all_spins(void)
{
  char *field = find_text(stats_buffer, "tot= ");

  if(field == 0)
    return -1;
  return parse_number(field + strlen("tot= "));
}

static struct lock_stats
snapshot_stats(void)
{
  struct lock_stats result;

  // workload 前后各取一次累计锁统计，run_path 最终使用两次快照的差值。
  memset(stats_buffer, 0, sizeof(stats_buffer));
  if(statistics(stats_buffer, sizeof(stats_buffer) - 1) <= 0)
    fail("无法读取锁统计");
  result.kmem_spins = prefix_spins("kmem");
  result.bcache_spins = prefix_spins("bcache");
  result.total_spins = all_spins();
  if(result.total_spins < 0)
    fail("锁名称不符合 Lab3 统计要求");
  return result;
}

static void
sort_latencies(uint *latency, int count)
{
  uint value;
  int i;
  int j;

  // 样本最多只有 3 * 24 个，简单插入排序足够，也避免引入额外库依赖。
  for(i = 1; i < count; i++) {
    value = latency[i];
    j = i;
    while(j > 0 && latency[j - 1] > value) {
      latency[j] = latency[j - 1];
      j--;
    }
    latency[j] = value;
  }
}

static void
add_io(struct ai_io *total, struct ai_io *one)
{
  total->model_read_bytes += one->model_read_bytes;
  total->embed_read_bytes += one->embed_read_bytes;
  total->kv_write_bytes += one->kv_write_bytes;
  total->kv_read_bytes += one->kv_read_bytes;
}

static void
run_path(struct ai_path *path, int workers, int requests,
         struct path_result *run)
{
  struct lock_stats before;
  struct lock_stats after;
  uint latency[AI_MAX_WORKERS * AI_MAX_REQUESTS];
  int start_pipe[2];
  int result_pipe[AI_MAX_WORKERS][2];
  char start = 's';
  int worker;
  int other;
  int request;
  int sample_count = 0;
  int total_requests = workers * requests;
  int p95_index;

  // 父进程先为每个 worker 建结果管道，再 fork 子进程。所有 worker 共享
  // 一条启动管道作为屏障，尽量让文件、内存和锁访问在时间上发生重叠。
  memset(run, 0, sizeof(*run));
  if(pipe(start_pipe) < 0)
    fail("无法创建启动管道");
  for(worker = 0; worker < workers; worker++)
    if(pipe(result_pipe[worker]) < 0)
      fail("无法创建结果管道");

  for(worker = 0; worker < workers; worker++) {
    if(fork() == 0) {
      // 子进程只保留启动读端和属于自己的结果写端；关闭多余端点可以
      // 避免 EOF 判断失效，也不会耗尽 xv6 很小的 fd 表。
      close(start_pipe[1]);
      for(other = 0; other < workers; other++) {
        close(result_pipe[other][0]);
        if(other != worker)
          close(result_pipe[other][1]);
      }
      worker_main(path, worker, requests, start_pipe[0],
                  result_pipe[worker][1]);
    }
  }
  close(start_pipe[0]);
  for(worker = 0; worker < workers; worker++)
    close(result_pipe[worker][1]);

  // 初始化不计入被测区间；快照后一次写入一个启动字节，释放所有 worker。
  before = snapshot_stats();
  run->elapsed_ticks = uptime();
  // wait 确保所有 worker 完成后再停止计时，elapsed 是整条并发通路时间。
  for(worker = 0; worker < workers; worker++)
    write_exact_or_fail(start_pipe[1], &start, 1);
  close(start_pipe[1]);

  for(worker = 0; worker < workers; worker++)
    wait(0);
  run->elapsed_ticks = uptime() - run->elapsed_ticks;
  if(run->elapsed_ticks <= 0)
    fail("运行时间为零");

  // 收集每个 worker 的完整结果，同时验证 KV I/O 契约并汇总延迟样本。
  for(worker = 0; worker < workers; worker++) {
    if(read_exact_result(result_pipe[worker][0], &run->worker[worker],
                         sizeof(run->worker[worker])) < 0)
      fail("worker 未返回完整结果");
    close(result_pipe[worker][0]);
    if(run->worker[worker].io.kv_write_bytes !=
         requests * AI_KV_FILE_BYTES ||
       run->worker[worker].io.kv_read_bytes !=
         requests * AI_KV_FILE_BYTES)
      fail("worker 的 KV I/O 字节数不正确");
    run->checksum += run->worker[worker].checksum;
    add_io(&run->io, &run->worker[worker].io);
    for(request = 0; request < requests; request++)
      latency[sample_count++] = run->worker[worker].latency[request];
  }
  after = snapshot_stats();
  if(after.kmem_spins < before.kmem_spins ||
     after.bcache_spins < before.bcache_spins ||
     after.total_spins < before.total_spins)
    fail("锁统计发生回退");
  run->stats.kmem_spins = after.kmem_spins - before.kmem_spins;
  run->stats.bcache_spins = after.bcache_spins - before.bcache_spins;
  run->stats.total_spins = after.total_spins - before.total_spins;

  // p95 使用向上取整后的第 ceil(0.95*N) 个样本，数组下标因此再减 1。
  sort_latencies(latency, sample_count);
  p95_index = (total_requests * 95 + 99) / 100 - 1;
  run->p95_ticks = latency[p95_index];
  if(run->checksum == 0)
    fail("输出校验和为零");
}

static void
print_metrics(struct ai_path *path, struct path_result *run,
              int workers, int requests)
{
  int total_requests = workers * requests;

  printf("AIINFER: verify OK mode=%s\n", path->name);
  printf("AIINFER_METRICS mode=%s workers=%d requests=%d "
         "elapsed_ticks=%d throughput_milli=%d p95_ticks=%l "
         "kmem_spins=%l bcache_spins=%l total_spins=%d "
         "model_read_bytes=%l embed_read_bytes=%l "
         "kv_write_bytes=%l kv_read_bytes=%l checksum=%l\n",
         path->name, workers, requests, run->elapsed_ticks,
         total_requests * 10000 / run->elapsed_ticks,
         (uint64)run->p95_ticks, (uint64)run->stats.kmem_spins,
         (uint64)run->stats.bcache_spins, run->stats.total_spins,
         (uint64)run->io.model_read_bytes,
         (uint64)run->io.embed_read_bytes,
         (uint64)run->io.kv_write_bytes,
         (uint64)run->io.kv_read_bytes, (uint64)run->checksum);
}

static void
verify_equal(struct path_result *left, struct path_result *right,
             int workers, int requests)
{
  int worker;
  int request;

  // 汇总 checksum 相同仍可能掩盖两个请求互换，因此 compare 还会逐 worker、
  // 逐请求检查输出。不同通路可以更改 I/O 组织，但不能更改推理语义。
  if(left->checksum != right->checksum ||
     left->io.kv_write_bytes != right->io.kv_write_bytes ||
     left->io.kv_read_bytes != right->io.kv_read_bytes)
    fail("通路汇总结果不等价");
  for(worker = 0; worker < workers; worker++)
    for(request = 0; request < requests; request++)
      if(left->worker[worker].output[request] !=
         right->worker[worker].output[request])
        fail("逐请求推理输出不等价");
}

static void
compare_paths(int workers, int requests)
{
  // baseline 与 student 在独立运行中使用同一 workload；若实现了选做，
  // prefetch 还必须与 student 等价。性能评分在正确性检查通过后才有意义。
  if(!ai_student_path.implemented)
    fail("任务三和任务四尚未全部完成");
  run_path(&ai_baseline_path, workers, requests, &compare_baseline);
  run_path(&ai_student_path, workers, requests, &compare_student);
  verify_equal(&compare_baseline, &compare_student, workers, requests);
  if(ai_prefetch_path.implemented) {
    run_path(&ai_prefetch_path, workers, requests, &compare_prefetch);
    verify_equal(&compare_student, &compare_prefetch, workers, requests);
  }
  printf("AIINFER_COMPARE: verify OK checksum=%l\n",
         (uint64)compare_baseline.checksum);
}

int
main(int argc, char *argv[])
{
  struct ai_path *path;
  int workers = AI_MAX_WORKERS;
  int requests = AI_MAX_REQUESTS;
  int argument;

  // 默认规模固定为 3 worker * 24 请求；-w/-r 只允许缩小到实验上限内，
  // 便于调试少量请求，同时保持正式测量入口一致。
  if(argc < 2)
    fail("缺少运行模式");
  for(argument = 2; argument < argc; argument += 2) {
    if(argument + 1 >= argc)
      fail("命令行参数缺少取值");
    if(strcmp(argv[argument], "-w") == 0)
      workers = parse_positive(argv[argument + 1]);
    else if(strcmp(argv[argument], "-r") == 0)
      requests = parse_positive(argv[argument + 1]);
    else
      fail("未知命令行参数");
  }
  if(workers > AI_MAX_WORKERS || requests > AI_MAX_REQUESTS)
    fail("worker 或请求数量超过实验上限");

  verify_assets();
  if(strcmp(argv[1], "compare") == 0) {
    compare_paths(workers, requests);
    exit(0);
  }
  path = parse_mode(argv[1]);
  if(path == 0)
    fail("未知运行模式");
  if(!path->implemented)
    fail("所选学生通路尚未实现");
  run_path(path, workers, requests, &single_run);
  print_metrics(path, &single_run, workers, requests);
  exit(0);
}
