#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

#include "ai_student_impl.h"

#define KV_TEST_REQUESTS 4

static uint saved_checksum[KV_TEST_REQUESTS];

static void
fail(char *message)
{
  printf("AIKVTEST: 失败：%s\n", message);
  exit(1);
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
fill_kv(struct kv_entry *kv, int request)
{
  int token;
  int dim;

  for(token = 0; token < AI_KV_TOKENS; token++) {
    for(dim = 0; dim < AI_DIM; dim++) {
      kv[token].key[dim] = request * 1009 + token * 31 + dim;
      kv[token].value[dim] = request * 997 - token * 17 - dim;
    }
  }
}

int
main(int argc, char *argv[])
{
  struct ai_session session;
  struct ai_io io;
  struct kv_entry *kv;
  int request;

  (void)argc;
  (void)argv;
  memset(&session, 0, sizeof(session));
  memset(&io, 0, sizeof(io));
  session.worker = 0;
  session.requests = KV_TEST_REQUESTS;

  if(ai_student_kv_begin(&session) < 0)
    fail("AI 附加题二：KV 持久化尚未实现");
  if(ai_student_kv_begin_write(&session) < 0) {
    ai_student_kv_end(&session);
    fail("无法开始写入阶段");
  }

  for(request = 0; request < KV_TEST_REQUESTS; request++) {
    kv = (struct kv_entry *)sbrk(PGSIZE);
    if(kv == (void *)-1) {
      ai_student_kv_end(&session);
      fail("无法申请 KV 页面");
    }
    fill_kv(kv, request);
    saved_checksum[request] = kv_checksum(kv);
    if(ai_student_kv_store(&session, request, kv, &io) < 0) {
      sbrk(-PGSIZE);
      ai_student_kv_end(&session);
      fail("KV 写盘失败");
    }
    sbrk(-PGSIZE);
  }
  if(ai_student_kv_end_write(&session) < 0) {
    ai_student_kv_end(&session);
    fail("无法结束写入阶段");
  }
  if(ai_student_kv_begin_read(&session) < 0) {
    ai_student_kv_end(&session);
    fail("无法开始恢复阶段");
  }

  for(request = 0; request < KV_TEST_REQUESTS; request++) {
    kv = (struct kv_entry *)sbrk(PGSIZE);
    if(kv == (void *)-1) {
      ai_student_kv_end(&session);
      fail("无法重新申请 KV 页面");
    }
    memset(kv, 0xa5, PGSIZE);
    if(ai_student_kv_restore(&session, request, kv, &io) < 0) {
      sbrk(-PGSIZE);
      ai_student_kv_end(&session);
      fail("KV 恢复失败");
    }
    if(kv_checksum(kv) != saved_checksum[request]) {
      sbrk(-PGSIZE);
      ai_student_kv_end(&session);
      fail("恢复后的 KV 内容错误");
    }
    sbrk(-PGSIZE);
  }
  ai_student_kv_end(&session);

  if(io.kv_write_bytes != KV_TEST_REQUESTS * AI_KV_FILE_BYTES ||
     io.kv_read_bytes != KV_TEST_REQUESTS * AI_KV_FILE_BYTES)
    fail("KV 读写字节数错误");

  printf("AIKVTEST: verify OK requests=%d bytes=%d\n",
         KV_TEST_REQUESTS, KV_TEST_REQUESTS * AI_KV_FILE_BYTES);
  exit(0);
}
