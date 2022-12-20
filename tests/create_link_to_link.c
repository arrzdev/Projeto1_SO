#include "../fs/operations.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FILE_PATH "/f0"
#define FIRST_HARD_PATH "/f1"
#define SECOND_HARD_PATH "/f2"
/*
thread B starts by opening file
thread A deletes file opened
thread B tries to read from file
*/

int main() {
    assert(tfs_init(NULL) != -1);

    // Create file
    int fd = tfs_open(FILE_PATH, TFS_O_CREAT);
    assert(fd != -1);

    // Write contents to file
    const char *contents = "Hello World!";
    assert(tfs_write(fd, contents, strlen(contents) + 1) != -1);
    assert(tfs_close(fd) != -1);

    // Create first hard link
    assert(tfs_link(FILE_PATH, FIRST_HARD_PATH) != -1);

    // Create second hard link pointing to the first hard link
    assert(tfs_link(FIRST_HARD_PATH, SECOND_HARD_PATH) != -1);

    // delete both hard links
    assert(tfs_unlink(FIRST_HARD_PATH) != -1);
    assert(tfs_unlink(SECOND_HARD_PATH) != -1);

    // check if the original file still exist
    assert(tfs_open(FILE_PATH, 0) != -1);

    // delete the file
    assert(tfs_unlink(FILE_PATH) != -1);

    // check if the file was deleted
    assert(tfs_open(FILE_PATH, 0) == -1);

    printf("Successful test.\n");
}