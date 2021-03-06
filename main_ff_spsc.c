#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/user.h>
#include <unistd.h>
#include "fixed_size_ring_buffer.h"
#include "fixed_size_ring_buffer.c"

#define MSG_INITIAL_PAD 4
#define DEFAULT_MSG_LENGTH 12

struct ring_buffer_test {
    struct fixed_size_ring_buffer_header *header;
    uint8_t *buffer;
    uint64_t tests;
    uint64_t messages;
};

#define MAX_LOOKAHEAD_CLAIM 4096

void *producer(void *arg) {
    struct ring_buffer_test *test = (struct ring_buffer_test *) arg;
    struct fixed_size_ring_buffer_header *header = test->header;
    uint8_t *buffer = test->buffer;
    const uint64_t tests = test->tests;
    const uint64_t messages = test->messages;
    uint64_t msg_id = 0;
    uint8_t *message_content = NULL;
    for (uint64_t t = 0; t < tests; t++) {
        struct timespec start_time;
        struct timespec end_produce_time;
        struct timespec end_time;
        uint64_t total_try = 0;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start_time);
        for (uint64_t m = 0; m < messages; m++) {

            const uint64_t next_msg_id = msg_id + 1;
            while (!try_fixed_size_ring_buffer_lookahead_claim(buffer, header, MAX_LOOKAHEAD_CLAIM, &message_content)) {
                //while (!try_fixed_size_ring_buffer_claim(buffer, header, &message_content)) {
                __asm__ __volatile__("pause;");
                total_try++;
                //wait strategy
            }
            total_try++;
            //provides better way to perform zero copy!!!!
            uint64_t *content_offset = (uint64_t *) (message_content + MSG_INITIAL_PAD);
            *content_offset = next_msg_id;
            fixed_size_ring_buffer_commit_claim(message_content);
            msg_id = next_msg_id;
        }
        //to verify the theory of the false sharing when the consumer is too fast...
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end_produce_time);
        //wait until all the messages get consumed
        while (fixed_size_ring_buffer_size(header) != 0) {
            __asm__ __volatile__("pause;");
            //employ wait strategy
        }
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end_time);
        const uint64_t wait_nanos =
                ((end_time.tv_sec - end_produce_time.tv_sec) * 1000000000) +
                (end_time.tv_nsec - end_produce_time.tv_nsec);
        const uint64_t elapsed_nanos =
                ((end_time.tv_sec - start_time.tv_sec) * 1000000000) + (end_time.tv_nsec - start_time.tv_nsec);
        const uint64_t tpt = (messages * 1000L) / elapsed_nanos;

        printf("%ldM ops/sec %ld/%ld failed tries end latency:%ld ns\n", tpt, total_try - messages, (uint64_t) messages,
               wait_nanos);
    }
    return NULL;
}

void *consumer(void *arg) {
    struct ring_buffer_test *test = (struct ring_buffer_test *) arg;
    struct fixed_size_ring_buffer_header *header = test->header;
    uint8_t *buffer = test->buffer;
    const uint64_t tests = test->tests;
    const uint64_t messages = test->messages;
    const uint64_t total_messages = tests * messages;
    uint8_t *message_read = NULL;
    uint64_t read_messages = 0;
    uint64_t failed_read = 0;
    while (read_messages < total_messages) {
        while (!try_fixed_size_ring_buffer_read(buffer, header, &message_read)) {
            __asm__ __volatile__("pause;");
            failed_read++;
        }
        const uint64_t expected_msg = read_messages + 1;
        const uint64_t *content_offset = (uint64_t *) (message_read + MSG_INITIAL_PAD);
        const uint64_t content = *content_offset;
        fixed_size_ring_buffer_commit_read(message_read);
        if (content != expected_msg) {
            printf("ERROR!\n");
            return NULL;
        }
        read_messages++;
    }
    printf("%ld/%ld failed reads\n", failed_read, total_messages);
    return NULL;
}

inline static bool on_message(uint8_t *const buffer, void *const context) {
    int64_t *expected_content = (int64_t *) context;
    const uint64_t expected_msg_content = *expected_content;
    //PAD REQUIRED TO GET 8 BYTES ALIGNED READ
    const uint64_t *msg_content_address = (uint64_t *) (buffer + MSG_INITIAL_PAD);
    const uint64_t msg_content = *msg_content_address;
    if (expected_msg_content != msg_content) {
        *expected_content = -1;
        return false;
    }
    //change next expected content!
    const uint64_t next_expected_content = expected_msg_content + 1;
    *expected_content = next_expected_content;
    return true;
}

void *batch_consumer(void *arg) {
    struct ring_buffer_test *test = (struct ring_buffer_test *) arg;
    struct fixed_size_ring_buffer_header *header = test->header;
    uint8_t *buffer = test->buffer;
    const uint64_t tests = test->tests;
    const uint64_t messages = test->messages;
    const uint32_t batch_size = header->capacity / 64;
    const uint64_t total_messages = tests * messages;
    const fixed_size_message_consumer consumer = &on_message;
    int64_t expected_content = 1;
    uint64_t read_messages = 0;
    uint64_t failed_read = 0;
    uint64_t success = 0;
    while (read_messages < total_messages && expected_content > 0) {
        const uint32_t read = fixed_size_ring_buffer_batch_read(buffer, header, consumer, batch_size,
                                                                &expected_content);
        if (read == 0) {
            __asm__ __volatile__("pause;");
            failed_read++;
        } else {
            success++;
            read_messages += read;
        }
    }
    if (expected_content < 0) {
        printf("read %ld messages instead of %ld!", read_messages, total_messages);
    } else {
        printf("avg batch reads:%ld %ld/%ld failed reads\n", read_messages / success, failed_read, total_messages);
    }

    return NULL;
}

void *stream_batch_consumer(void *arg) {
    struct ring_buffer_test *test = (struct ring_buffer_test *) arg;
    struct fixed_size_ring_buffer_header *header = test->header;
    uint8_t *buffer = test->buffer;
    const uint64_t tests = test->tests;
    const uint64_t messages = test->messages;
    const uint32_t batch_size = header->capacity / 64;
    const uint64_t total_messages = tests * messages;
    const fixed_size_message_consumer consumer = &on_message;
    int64_t expected_content = 1;
    uint64_t read_messages = 0;
    uint64_t failed_read = 0;
    uint64_t success = 0;
    while (read_messages < total_messages && expected_content > 0) {
        const uint32_t read = fixed_size_ring_buffer_stream_batch_read(buffer, header, consumer, batch_size,
                                                                &expected_content);
        if (read == 0) {
            __asm__ __volatile__("pause;");
            failed_read++;
        } else {
            success++;
            read_messages += read;
        }
    }
    if (expected_content < 0) {
        printf("read %ld messages instead of %ld!", read_messages, total_messages);
    } else {
        printf("avg batch reads:%ld %ld/%ld failed reads\n", read_messages / success, failed_read, total_messages);
    }

    return NULL;
}

int main() {
    const bool batch_read = true;
    const bool stream = false;
    const index_t requested_capacity = 64 * 1024;

    const index_t buffer_capacity = fixed_size_ring_buffer_capacity(requested_capacity, DEFAULT_MSG_LENGTH);

    uint8_t *buffer = aligned_alloc(PAGE_SIZE, buffer_capacity);
    printf("ALLOCATED %d bytes aligned on: %ld\n", buffer_capacity, PAGE_SIZE);

    struct fixed_size_ring_buffer_header header;
    if (!init_fixed_size_ring_buffer_header(buffer, &header, requested_capacity, DEFAULT_MSG_LENGTH)) {
        return 1;
    }

    //on the stack it will need memset!!!
    //memset(buffer, 0, buffer_capacity);
    //check alignment minumum
    const bool is_aligned = (((int64_t) buffer) & 7) == 0;
    if (!is_aligned) {
        return 1;
    }

    struct ring_buffer_test test;
    test.buffer = buffer;
    test.header = &header;
    test.messages = 1000000000;
    test.tests = 10;
    pthread_t consumer_processor;
    if (batch_read) {
        if(stream){
            pthread_create(&consumer_processor, NULL, stream_batch_consumer, &test);
        }else {
            pthread_create(&consumer_processor, NULL, batch_consumer, &test);
        }
    } else {
        pthread_create(&consumer_processor, NULL, consumer, &test);
    }
    pthread_t producer_processor;
    pthread_create(&producer_processor, NULL, producer, &test);

    pthread_join(producer_processor, NULL);
    pthread_join(consumer_processor, NULL);
    free(buffer);
    return 0;
}