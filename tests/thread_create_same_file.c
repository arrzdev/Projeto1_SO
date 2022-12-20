#include "../fs/operations.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FILE_PATH "/f1"

void *copy_external_thread_func() {
    // copy from external
    char *path_copied_file = "/f1";
    char *path_src = "tests/file_to_copy.txt";
    int f = tfs_copy_from_external_fs(path_src, path_copied_file);
    assert(f != -1);
    return NULL;
}

int main() {
    assert(tfs_init(NULL) != -1);

    // Create threads
    pthread_t t1;
    pthread_t t2;
    assert(pthread_create(&t1, NULL, copy_external_thread_func, NULL) == 0);
    assert(pthread_create(&t2, NULL, copy_external_thread_func, NULL) == 0);

    // Wait for threads to finish
    assert(pthread_join(t1, NULL) == 0);
    assert(pthread_join(t2, NULL) == 0);

    printf("Successful test.\n");
}