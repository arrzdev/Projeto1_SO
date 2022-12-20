#include "../fs/operations.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FILE_PATH "/f1"

void *unlink_external_thread_func(void *arg) {
    // copy from external
    const char *path_copied_file = "/f1";

    int f = tfs_unlink(path_copied_file);

    int v = *((int *)arg);

    if (v == 0)
        assert(f != -1);

    else
        assert(f == -1);

    return NULL;
}

int main() {
    assert(tfs_init(NULL) != -1);

    const char *path_copied_file = "/f1";
    tfs_open(path_copied_file, TFS_O_CREAT);

    int a = 0;
    int b = 1;

    // Create threads
    pthread_t t1;
    pthread_t t2;
    assert(pthread_create(&t1, NULL, unlink_external_thread_func, (void *)&a) ==
           0);
    assert(pthread_create(&t2, NULL, unlink_external_thread_func, (void *)&b) ==
           0);

    // Wait for threads to finish
    assert(pthread_join(t1, NULL) == 0);
    assert(pthread_join(t2, NULL) == 0);

    printf("Successful test.\n");
}