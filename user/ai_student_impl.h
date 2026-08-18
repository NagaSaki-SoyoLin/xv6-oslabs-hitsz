#ifndef XV6_AI_STUDENT_IMPL_H
#define XV6_AI_STUDENT_IMPL_H

#include "ai_path.h"

int ai_student_model_begin(struct ai_session *);
int ai_student_model_load(struct ai_session *, uint, uint, char *,
                          struct ai_io *);
void ai_student_model_end(struct ai_session *);

int ai_student_kv_begin(struct ai_session *);
int ai_student_kv_begin_write(struct ai_session *);
int ai_student_kv_store(struct ai_session *, int, struct kv_entry *,
                        struct ai_io *);
int ai_student_kv_end_write(struct ai_session *);
int ai_student_kv_begin_read(struct ai_session *);
int ai_student_kv_restore(struct ai_session *, int, struct kv_entry *,
                          struct ai_io *);
void ai_student_kv_end(struct ai_session *);

int ai_student_prefetch_begin(struct ai_session *);
int ai_student_prefetch_next(struct ai_session *, int);
int ai_student_prefetch_wait(struct ai_session *, int);
void ai_student_prefetch_end(struct ai_session *);

#endif
