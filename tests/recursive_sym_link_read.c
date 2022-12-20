#include "../fs/operations.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FILE_PATH "/f0"
#define FIRST_SYM_PATH "/f1"
#define SECOND_SYM_PATH "/f2"
#define THIRD_SYM_PATH "/f3"

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

    // Create first sym link
    assert(tfs_sym_link(FILE_PATH, FIRST_SYM_PATH) != -1);

    // Create second sym link
    assert(tfs_sym_link(FIRST_SYM_PATH, SECOND_SYM_PATH) != -1);

    // Create third sym link
    assert(tfs_sym_link(SECOND_SYM_PATH, THIRD_SYM_PATH) != -1);

    // check recursive sym link open
    int fd3 = tfs_open(THIRD_SYM_PATH, 0);
    assert(fd3 != -1);

    // read content of third sym link
    char cur_content[1024];
    tfs_read(fd3, cur_content, 1024);

    // check if it is the same content as the original file meaning recursion
    // open worked
    assert(strcmp(cur_content, contents) == 0);

    printf("Successful test.\n");
}