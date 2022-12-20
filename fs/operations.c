#include "operations.h"
#include "config.h"
#include "state.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betterassert.h"

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
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() {
    if (state_destroy() != 0) {
        return -1;
    }
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
    int inum = tfs_lookup(name, root_dir_inode);
    size_t offset;

    if (inum >= 0) {
        // The file already exists
        inode_t *inode = inode_get(inum);
        ALWAYS_ASSERT(inode != NULL,
                      "tfs_open: directory files must have an inode");

        // handle recursion of symbolic links
        if (inode->i_node_type == T_LINK) {
            // read symlink file content
            int file = add_to_open_file_table(inum, 0);

            // copy file data to buffer
            char buffer[BLOCK_SIZE];
            tfs_read(file, buffer, BLOCK_SIZE);

            // close and remove from open file table the symlink file
            remove_from_open_file_table(file);

            if (valid_pathname(buffer))
                return tfs_open(buffer, mode);
        }

        // Truncate (if requested)
        if (mode & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                data_block_free(inode->i_data_block);
                inode->i_size = 0;
            }
        }
        // Determine initial offset
        if (mode & TFS_O_APPEND) {
            offset = inode->i_size;
        } else {
            offset = 0;
        }
    } else if (mode & TFS_O_CREAT) {
        // The file does not exist; the mode specified that it should be created
        // Create inode
        inum = inode_create(T_FILE);
        if (inum == -1) {
            return -1; // no space in inode table
        }

        // Add entry in the root directory
        if (add_dir_entry(root_dir_inode, name + 1, inum) == -1) {
            inode_delete(inum);
            return -1; // no space in directory
        }

        offset = 0;
    } else {
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

    inode_t *rootNode = inode_get(ROOT_DIR_INUM);

    // check if the file exists
    int inumber = tfs_lookup(target, rootNode);
    if (inumber == -1) // if the file doesnt exist
        return -1;

    // create soft link inode
    int soft_link_inumber = inode_create(T_LINK);
    if (soft_link_inumber == -1)
        return -1;

    // add dir entry
    if (add_dir_entry(rootNode, link_name + 1, soft_link_inumber) == -1) {
        inode_delete(soft_link_inumber);
        return -1;
    }

    inode_t *soft_link_inode = inode_get(soft_link_inumber);
    if (soft_link_inode == NULL)
        return -1;

    // open the file of tfs and write the path of the target
    int soft_link_file = tfs_open(link_name, TFS_O_APPEND);

    if (tfs_write(soft_link_file, target,
                  (strlen(target) + 1) * sizeof(char)) == -1)
        return -1;

    // close file
    tfs_close(soft_link_file);

    soft_link_inode->i_size = strlen(target) + 1;
    return 0;
}

int tfs_link(char const *target, char const *link_name) {
    if (!valid_pathname(link_name))
        return -1;

    inode_t *rootNode = inode_get(ROOT_DIR_INUM);

    // check if the file exists
    int inumber = tfs_lookup(target, rootNode);
    if (inumber == -1) // if the file doesnt exist
        return -1;

    // check if it is soft_link
    inode_t *inodeToLink = inode_get(inumber);
    if (inodeToLink->i_node_type == T_LINK)
        return -1;

    // add hardlink and handle error
    if (add_dir_entry(rootNode, link_name + 1, inumber) == -1)
        return -1;

    inodeToLink->hard_links++;
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
    inode_t *rootNode = inode_get(ROOT_DIR_INUM);

    int inumber = tfs_lookup(target, rootNode);
    if (inumber == -1) {
        printf("Hello");
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

    if (clear_dir_entry(rootNode, target + 1) == -1)
        return -1;

    return 0;
}

int tfs_copy_from_external_fs(char const *source_path, char const *dest_path) {
    if (!valid_pathname(dest_path))
        return -1;

    /*
    // TODO: verify test if new file have less data than the already existent
    file
    // check if fileExist
    int fileIndex = tfs_lookup(dest_path, inode_get(ROOT_DIR_INUM));
    int fileIndex =
      tfs_open(dest_path, fileExist != -1 ? TFS_O_TRUNC : TFS_O_CREAT);
    */

    // Open the file for reading
    FILE *fp = fopen(source_path, "r");
    if (!fp) {
        return -1;
    }

    // additional logic to check if fopen reads all the bytes of the file
    fseek(fp, 0, SEEK_END); // moves file pointer to the end of file
    size_t fileSize = (size_t)ftell(fp); // returns current byte
    rewind(fp); // moves file pointer to the beginning of file

    // Allocate a buffer to hold the file contents
    char buffer[BLOCK_SIZE];

    // Read the file contents into the buffer
    size_t bytesRead = fread(buffer, 1, fileSize, fp);
    // close file
    fclose(fp);

    if (bytesRead < fileSize) {
        // Error reading the file
        return -1;
    }

    // Do something with the buffer...
    int fileIndex = tfs_open(dest_path, TFS_O_CREAT | TFS_O_TRUNC);
    tfs_write(fileIndex, buffer, fileSize);
    tfs_close(fileIndex);

    // Free the buffer and close the file
    return 0;
}