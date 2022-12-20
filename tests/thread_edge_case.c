#include "../fs/operations.h"
#include "../fs/state.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FILE_PATH "/f1"

/*
thread B starts by opening file
thread A deletes file opened
thread B tries to overwrite file but creates a new one
*/

// sleep ensure concurrency

// THREAD A
void *delete_thread_func() {
    sleep(1);
    // Delete file
    assert(tfs_unlink(FILE_PATH) != -1);

    return NULL;
}

// THREAD B
void *read_thread_func() {
    // Open file for reading
    int fd = tfs_open(FILE_PATH, 0);
    assert(fd != -1);

    // Sleep for a short time to allow the other thread to start deleting the
    // file
    sleep(2);

    assert(tfs_open(FILE_PATH, TFS_O_CREAT) != -1);

    int fd2 = tfs_open(FILE_PATH, 0);

    // Close file
    assert(tfs_close(fd2) != -1);

    // checks that file has different file handler
    assert(fd2 != fd);

    return NULL;
}

int main() {
    assert(tfs_init(NULL) != -1);

    // Create file
    int fd = tfs_open(FILE_PATH, TFS_O_CREAT);
    assert(fd != -1);

    // Write contents to file
    const char *contents = "Hello World!\n";
    assert(tfs_write(fd, contents, strlen(contents)) != -1);

    assert(tfs_close(fd) != -1);

    // Create threads
    pthread_t delete_thread;
    pthread_t read_thread;
    assert(pthread_create(&delete_thread, NULL, delete_thread_func, NULL) == 0);
    assert(pthread_create(&read_thread, NULL, read_thread_func, NULL) == 0);

    // Wait for threads to finish
    assert(pthread_join(delete_thread, NULL) == 0);
    assert(pthread_join(read_thread, NULL) == 0);

    printf("Successful test.\n");
}