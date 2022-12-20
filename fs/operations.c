#include "operations.h"
#include "config.h"
#include "state.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betterassert.h"
#include <pthread.h>

static pthread_mutex_t *mutex_global;
static pthread_rwlock_t *inode_locks;

tfs_params tfs_default_params() {
    tfs_params params = {
        .max_inode_count = 64,
        .max_block_count = 1024,
        .max_open_files_count =
            16, // TODO: Check if any sanitazion is needed on this
        .block_size = 1024,
    };
    return params;
}

int tfs_init(tfs_params const *params_ptr) {
    tfs_params params;
    if (params_ptr != NULL) {
        params = *params_ptr;
    } else {
        params = tfs_default_params();
    }

    if (state_init(params) != 0) {
        return -1;
    }

    // create root inode
    if (inode_create(T_DIRECTORY) != ROOT_DIR_INUM) {
        return -1;
    }

    mutex_global = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (mutex_global == NULL)
        return -1;

    if (pthread_mutex_init(mutex_global, NULL) != 0) {
        return -1;
    }

    inode_locks =
        (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t) * INODE_TABLE_SIZE);

    if (inode_locks == NULL)
        return -1;

    // initialize inode locks
    for (int i = 0; i < INODE_TABLE_SIZE; i++) {
        if (pthread_rwlock_init(&inode_locks[i], NULL) != 0) {
            return -1;
        }
    }

    return 0;
}

int tfs_destroy() {
    if (state_destroy() != 0) {
        return -1;
    }

    if (pthread_mutex_destroy(mutex_global) != 0) {
        return -1;
    }

    free(mutex_global);

    // destroy inode locks
    for (int i = 0; i < INODE_TABLE_SIZE; i++) {
        if (pthread_rwlock_destroy(&inode_locks[i]) != 0) {
            return -1;
        }
    }

    free(inode_locks);
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}

/**
 * Looks for a file.
 *
 * Note: as a simplification, only a plain directory space (root directory only)
 * is supported.
 *
 * Input:
 *   - name: absolute path name
 *   - root_inode: the root directory inode
 * Returns the inumber of the file, -1 if unsuccessful.
 */
static int tfs_lookup(char const *name, inode_t const *root_inode) {
    inode_t *root = inode_get(ROOT_DIR_INUM);

    ALWAYS_ASSERT(root != NULL, "tfs_open: root dir inode must exist");

    if (root != root_inode) {
        return -1;
    }

    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;
    return find_in_dir(root_inode, name);
}

int tfs_open(char const *name, tfs_file_mode_t mode) {
    // Checks if the path name is valid
    if (!valid_pathname(name)) {
        return -1;
    }

    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_open: root dir inode must exist");

    pthread_mutex_lock(mutex_global);
    int inum = tfs_lookup(name, root_dir_inode);
    size_t offset = 0;

    if (inum >= 0) {
        // The file already exists
        inode_t *inode = inode_get(inum);
        pthread_mutex_unlock(mutex_global);

        ALWAYS_ASSERT(inode != NULL,
                      "tfs_open: directory files must have an inode");

        // handle recursion open of symbolic links
        if (inode->i_node_type == T_LINK) {
            // read symlink file content
            int file = add_to_open_file_table(inum, 0);

            pthread_rwlock_wrlock(&inode_locks[inum]);
            // copy file data to buffer
            char buffer[BLOCK_SIZE];
            tfs_read(file, buffer, BLOCK_SIZE);
            pthread_rwlock_unlock(&inode_locks[inum]);

            // close and remove from open file table the symlink file
            remove_from_open_file_table(file);

            if (valid_pathname(buffer))
                return tfs_open(buffer, mode);
        }

        // Truncate (if requested)
        if (mode & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                pthread_mutex_lock(mutex_global);
                data_block_free(inode->i_data_block);
                inode->i_size = 0;
                pthread_mutex_unlock(mutex_global);
            }
        }
        // Determine initial offset
        if (mode & TFS_O_APPEND) {
            offset = inode->i_size;
        }
    } else if (mode & TFS_O_CREAT) {
        // The file does not exist; the mode specified that it should be created
        // Create inode
        inum = inode_create(T_FILE);
        if (inum == -1) {
            pthread_mutex_unlock(mutex_global);
            return -1; // no space in inode table
        }

        // Add entry in the root directory
        if (add_dir_entry(root_dir_inode, name + 1, inum) == -1) {
            inode_delete(inum);
            pthread_mutex_unlock(mutex_global);
            return -1; // no space in directory
        }
        pthread_mutex_unlock(mutex_global);
    } else {
        pthread_mutex_unlock(mutex_global);
        return -1;
    }

    return add_to_open_file_table(inum, offset);

    // Note: for simplification, if file was created with TFS_O_CREAT and there
    // is an error adding an entry to the open file table, the file is not
    // opened but it remains created
}

int tfs_sym_link(char const *target, char const *link_name) {
    if (!valid_pathname(link_name))
        return -1;

    inode_t *root_node = inode_get(ROOT_DIR_INUM);

    // check if the target file exists
    pthread_mutex_lock(mutex_global);
    if (tfs_lookup(target, root_node) == -1) { // if the file doesnt exist
        pthread_mutex_unlock(mutex_global);
        return -1;
    }
    pthread_mutex_unlock(mutex_global);

    // create the file (with tfs open trick)
    int fhandle = tfs_open(link_name, TFS_O_CREAT);
    if (fhandle == -1)
        return -1;

    // get the inumber
    pthread_mutex_lock(mutex_global);
    int inumber = tfs_lookup(link_name, root_node);
    if (inumber == -1) {
        pthread_mutex_unlock(mutex_global);
        return -1;
    }

    // get the inode
    inode_t *inode = inode_get(inumber);
    if (inode == NULL) {
        pthread_mutex_unlock(mutex_global);
        return -1;
    }

    // set the inode type to T_LINK
    inode->i_node_type = T_LINK;
    pthread_mutex_unlock(mutex_global);

    pthread_rwlock_wrlock(&inode_locks[inumber]);
    // write the path of the target to the file
    if (tfs_write(fhandle, target, strlen(target) + 1) == -1) {
        pthread_rwlock_unlock(&inode_locks[inumber]);
        return -1;
    }
    pthread_rwlock_unlock(&inode_locks[inumber]);

    // close the file
    tfs_close(fhandle);
    return 0;
}

int tfs_link(char const *target, char const *link_name) {
    if (!valid_pathname(link_name))
        return -1;

    pthread_mutex_lock(mutex_global);
    inode_t *root_node = inode_get(ROOT_DIR_INUM);

    // check if the target file exists
    int target_inumber = tfs_lookup(target, root_node);
    if (target_inumber == -1) {
        pthread_mutex_unlock(mutex_global);
        return -1;
    }

    // check if it is soft_link
    inode_t *target_node = inode_get(target_inumber);
    if (target_node->i_node_type == T_LINK) {
        pthread_mutex_unlock(mutex_global);
        return -1;
    }

    // add hardlink and handle error
    if (add_dir_entry(root_node, link_name + 1, target_inumber) == -1) {
        pthread_mutex_unlock(mutex_global);
        return -1;
    }

    target_node->hard_links++;
    pthread_mutex_unlock(mutex_global);
    return 0;
}

int tfs_close(int fhandle) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1; // invalid fd
    }

    remove_from_open_file_table(fhandle);
    return 0;
}

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    //  From the open file table entry, we get the inode
    inode_t *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_write: inode of open file deleted");

    // Determine how many bytes to write
    size_t block_size = state_block_size();
    if (to_write + file->of_offset > block_size) {
        to_write = block_size - file->of_offset;
    }

    if (to_write > 0) {
        if (inode->i_size == 0) {
            // If empty file, allocate new block
            int bnum = data_block_alloc();
            if (bnum == -1) {
                return -1; // no space
            }

            inode->i_data_block = bnum;
        }

        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_write: data block deleted mid-write");

        // Perform the actual write
        memcpy(block + file->of_offset, buffer, to_write);

        // The offset associated with the file handle is incremented
        // accordingly
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
    }

    return (ssize_t)to_write;
}

ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    // From the open file table entry, we get the inode
    inode_t const *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_read: inode of open file deleted");

    // Determine how many bytes to read
    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    if (to_read > 0) {
        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_read: data block deleted mid-read");

        // Perform the actual read
        memcpy(buffer, block + file->of_offset, to_read);
        // The offset associated with the file handle is incremented
        // accordingly
        file->of_offset += to_read;
    }

    return (ssize_t)to_read;
}

int tfs_unlink(char const *target) {
    inode_t *root_node = inode_get(ROOT_DIR_INUM);

    pthread_mutex_lock(mutex_global);
    int inumber = tfs_lookup(target, root_node);
    if (inumber == -1) {
        pthread_mutex_unlock(mutex_global);
        return -1;
    }

    inode_t *node = inode_get(inumber);

    // Soft-link
    if (node->i_node_type == T_LINK) {
        inode_delete(inumber);
    } else {
        // Hard-link
        node->hard_links--;
        if (node->hard_links == 0) {
            inode_delete(inumber);
        }
    }

    if (clear_dir_entry(root_node, target + 1) == -1) {
        pthread_mutex_unlock(mutex_global);
        return -1;
    }

    pthread_mutex_unlock(mutex_global);
    return 0;
}

int tfs_copy_from_external_fs(char const *source_path, char const *dest_path) {
    if (!valid_pathname(dest_path))
        return -1;

    // Open the file for reading
    FILE *fp = fopen(source_path, "r");
    if (!fp) {
        return -1;
    }

    // additional logic to check if fopen reads all the bytes of the file
    fseek(fp, 0, SEEK_END); // moves file pointer to the end of file
    size_t file_size = (size_t)ftell(fp); // returns current byte
    rewind(fp); // moves file pointer to the beginning of file

    // Allocate a buffer to hold the file contents
    char buffer[BLOCK_SIZE];

    // Read the file contents into the buffer
    size_t bytes_read = fread(buffer, 1, file_size, fp);
    // close file
    fclose(fp);

    if (bytes_read < file_size) {
        // Error reading the file
        return -1;
    }

    int file_handle = tfs_open(dest_path, TFS_O_CREAT | TFS_O_TRUNC);
    if (file_handle == -1) {
        return -1;
    }

    // get tfs file inumber
    pthread_mutex_lock(mutex_global);
    int inumber = tfs_lookup(dest_path, inode_get(ROOT_DIR_INUM));
    pthread_mutex_unlock(mutex_global);

    pthread_rwlock_wrlock(&inode_locks[inumber]);
    if (tfs_write(file_handle, buffer, file_size) == -1) {
        pthread_rwlock_unlock(&inode_locks[inumber]);
        return -1;
    }
    pthread_rwlock_unlock(&inode_locks[inumber]);

    if (tfs_close(file_handle) == -1) {
        return -1;
    }
    return 0;
}