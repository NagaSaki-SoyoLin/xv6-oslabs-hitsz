#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/riscv.h"
#include "user/user.h"

#include "ai_path.h"

struct worker_result {
  uint checksum;
  uint output[AI_MAX_REQUESTS];
  uint latency[AI_MAX_REQUESTS];
  struct ai_io io;
};

struct worker_workspace {
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

  if(path->load(session, family, shard, block, io) < 0)
    fail("模型或 embedding 分片加载失败");
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

  workspace.begin_tick[request] = uptime();
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
  int step;
  int dim;

  for(step = 0; step < AI_KV_TOKENS; step++) {
    score = 0;
    for(dim = 0; dim < AI_DIM; dim++)
      score += workspace.query[request][dim] * kv[step].key[dim];
    if(score > best_score) {
      best_score = score;
      best_index = step;
    }
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

  memset(&result, 0, sizeof(result));
  memset(&workspace, 0, sizeof(workspace));
  if(path->begin(&session, worker, requests) < 0)
    fail("学生通路尚未实现或初始化失败");
  if(read_exact_result(start_fd, &start, 1) < 0)
    fail("启动屏障读取失败");
  close(start_fd);

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
    if(sbrk(-PGSIZE) == (void *)-1)
      fail("无法释放原 KV 页面");
  }
  if(path->end_kv_write(&session) < 0)
    fail("无法结束 KV 写入阶段");
  if(path->begin_kv_read(&session) < 0)
    fail("无法开始 KV 恢复阶段");

  if(path->prefetch) {
    if(path->prefetch_next(&session, 0) < 0 ||
       path->prefetch_wait(&session, 0) < 0)
      fail("首条 KV 预取失败");
  }
  for(request = 0; request < requests; request++) {
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

  memset(run, 0, sizeof(*run));
  if(pipe(start_pipe) < 0)
    fail("无法创建启动管道");
  for(worker = 0; worker < workers; worker++)
    if(pipe(result_pipe[worker]) < 0)
      fail("无法创建结果管道");

  for(worker = 0; worker < workers; worker++) {
    if(fork() == 0) {
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

  before = snapshot_stats();
  run->elapsed_ticks = uptime();
  for(worker = 0; worker < workers; worker++)
    write_exact_or_fail(start_pipe[1], &start, 1);
  close(start_pipe[1]);

  for(worker = 0; worker < workers; worker++)
    wait(0);
  run->elapsed_ticks = uptime() - run->elapsed_ticks;
  if(run->elapsed_ticks <= 0)
    fail("运行时间为零");

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
