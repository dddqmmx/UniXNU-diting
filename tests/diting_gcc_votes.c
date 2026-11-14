/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "DitingGccVotes.h"

typedef struct {uint32_t word; unsigned writes; bool drop;} Vote;
static uint32_t rd(void *context) {return ((Vote *)context)->word;}
static void wr(void *context, uint32_t value) {
    Vote *v = context; ++v->writes; if (!v->drop) v->word = value;
}
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Vote shared = {.word = 1u << 27};
static void *client(void *argument) {
    const uint32_t mask = 1u << *(unsigned *)argument;
    const DitingGccVoteIo io = {&shared, rd, wr};
    for (unsigned i = 0; i < 10000; ++i) {
        assert(!pthread_mutex_lock(&lock));
        assert(diting_gcc_vote_io(&io, mask, 0));
        assert(diting_gcc_vote_io(&io, 0, mask));
        assert(!pthread_mutex_unlock(&lock));
    }
    assert(!pthread_mutex_lock(&lock));
    assert(diting_gcc_vote_io(&io, mask, 0));
    assert(!pthread_mutex_unlock(&lock)); return NULL;
}
int main(void) {
    Vote v = {.word = (1u << 14) | (1u << 27)}; const DitingGccVoteIo io = {&v, rd, wr};
    assert(diting_gcc_vote_io(&io, 0x3f, 0));
    assert(v.word == ((1u << 14) | (1u << 27) | 0x3f));
    assert(diting_gcc_vote_io(&io, 0, 1u << 4));
    assert(v.word == ((1u << 14) | (1u << 27) | 0x2f));
    unsigned before = v.writes;
    assert(diting_gcc_vote_io(&io, 1u << 14, 0) && v.writes == before);
    assert(!diting_gcc_vote_io(&io, 1, 1) && v.writes == before);
    v.drop = true; assert(!diting_gcc_vote_io(&io, 1u << 15, 0));
    assert(diting_gcc_branch_ready(0, false));
    assert(diting_gcc_branch_ready(0x80000002, true));
    assert(!diting_gcc_branch_ready(0x80000002, false));
    assert(!diting_gcc_branch_ready(0x80000000, true));
    pthread_t threads[4]; unsigned bits[] = {4, 14, 15, 18};
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(threads + i, NULL, client, bits + i));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    assert(shared.word == ((1u << 27) | (1u << 4) | (1u << 14) | (1u << 15) | (1u << 18)));
    puts("GCC: vote preservation/readback, HWCG and four concurrent clients PASS");
    return 0;
}
