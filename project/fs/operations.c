#include "operations.h"
#include "betterassert.h"
#include "config.h"
#include "state.h"
#include "locks.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * tfs_open lock
 *
 * Description - The purpose of this lock is to not allow 2 files with the same
 * name to be created
 */
static pthread_mutex_t tfs_open_lock;

/*
 * free_open_file_entries_lock
 *
 * Description - A pointer to the free open file entries table lock, located
 * in state.c
 */
static pthread_mutex_t *free_open_file_entries_lock;

/*
 * free_blocks_lock
 *
 * Description - A pointer to the free blocks table lock, located
 * in state.c
 */
static pthread_mutex_t *free_blocks_lock;

/*
 * inode_locks
 *
 * Description - A pointer to the table of inode locks located in state.c
 */
static pthread_rwlock_t *inode_locks;

tfs_params tfs_default_params() {
    tfs_params params = {
        .max_inode_count = 64,
        .max_block_count = 1024,
        .max_open_files_count = 16,
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

    mutex_init(&tfs_open_lock);
    free_open_file_entries_lock = get_free_open_file_entries_lock();
    free_blocks_lock = get_free_blocks_lock();
    inode_locks = get_inode_locks();

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

    mutex_destroy(&tfs_open_lock);

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
    ALWAYS_ASSERT(root_inode->i_data_block == 0,
                  "tfs_lookup: the given root_inode does not correspond to the "
                  "root inode");
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

    // Lock tfs_open to not allow 2 files with the same name to be created
    mutex_lock(&tfs_open_lock);

    rwl_wrlock(&inode_locks[ROOT_DIR_INUM]);
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_open: root dir inode must exist");
    int inum = tfs_lookup(name, root_dir_inode);
    size_t offset;

    if (inum >= 0) {
        // The file already exists
        rwl_wrlock(&inode_locks[inum]);
        mutex_unlock(&tfs_open_lock);
        inode_t *inode = inode_get(inum);
        ALWAYS_ASSERT(inode != NULL,
                      "tfs_open: directory files must have an inode");

        // if the file is an initialized symlink, open its target
        if (inode->i_node_type == T_SYM_LINK && inode->i_size > 0) {
            void *data = data_block_get(inode->i_data_block);
            ALWAYS_ASSERT(data != NULL,
                          "tfs_open: symlink must have a data block");
            char buffer[inode->i_size];
            memcpy(buffer, data, inode->i_size);

            rwl_unlock(&inode_locks[inum]);
            rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
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
        rwl_unlock(&inode_locks[inum]);
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
    } else if (mode & TFS_O_CREAT) {
        // The file does not exist; the mode specified that it should be created
        // Create inode
        inum = inode_create(T_FILE);
        if (inum == -1) {
            rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
            mutex_unlock(&tfs_open_lock);
            return -1; // no space in inode table
        }

        // Add entry in the root directory
        if (add_dir_entry(root_dir_inode, name + 1, inum) == -1) {
            rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
            mutex_unlock(&tfs_open_lock);
            inode_delete(inum);
            return -1; // no space in directory
        }
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        mutex_unlock(&tfs_open_lock);

        offset = 0;
    } else {
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        mutex_unlock(&tfs_open_lock);
        return -1;
    }

    // Finally, add entry to the open file table and return the corresponding
    // handle
    return add_to_open_file_table(inum, offset);

    // Note: for simplification, if file was created with TFS_O_CREAT and there
    // is an error adding an entry to the open file table, the file is not
    // opened but it remains created
}

int tfs_sym_link(char const *target, char const *link_name) {
    rwl_wrlock(&inode_locks[ROOT_DIR_INUM]);
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_sym_link: root dir inode must exist");

    if (tfs_lookup(link_name, root_dir_inode) != -1) {
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        return -1; // there's already a file in root with link_name
    }

    int link_inumber;
    if ((link_inumber = inode_create(T_SYM_LINK)) == -1) {
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        return -1; // no free slots in inode table for the link inode
    }

    rwl_rdlock(&inode_locks[link_inumber]);
    inode_t *link_inode = inode_get(link_inumber);
    ALWAYS_ASSERT(link_inode != NULL, "tfs_sym_link: link inode doesn't exist");

    // add the soft link to the directory entry
    if (add_dir_entry(root_dir_inode, link_name + 1, link_inumber) == -1) {
        rwl_unlock(&inode_locks[link_inumber]);
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        inode_delete(link_inumber);
        return -1; // link filename not valid or root directory full of entries
    }

    rwl_unlock(&inode_locks[link_inumber]);
    rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
    // open symlink file so that we can write the target path in the data block
    int symlink_handle;
    if ((symlink_handle = tfs_open(link_name, 0)) == -1)
        return -1; // couldn't open file

    // write the target path in the sym link data block
    if (tfs_write(symlink_handle, target, sizeof(target)) < sizeof(target)) {
        // couldn't write the target path, which results in a broken symlink,
        // so we will delete it
        tfs_close(symlink_handle);
        tfs_unlink(link_name);
        return -1;
    }

    tfs_close(symlink_handle);

    return 0;
}

int tfs_link(char const *target, char const *link_name) {
    rwl_wrlock(&inode_locks[ROOT_DIR_INUM]);
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_link: root dir inode must exist");

    int inumber;
    if ((inumber = tfs_lookup(target, root_dir_inode)) == -1) {
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        return -1; // target doesn't exist
    }

    if (tfs_lookup(link_name, root_dir_inode) != -1) {
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        return -1; // there's already a file in root with link_name
    }

    rwl_wrlock(&inode_locks[inumber]);

    inode_t *target_inode = inode_get(inumber);
    ALWAYS_ASSERT(target_inode != NULL, "tfs_link: target inode doesn't exist");
    if (target_inode->i_node_type == T_SYM_LINK) {
        rwl_unlock(&inode_locks[inumber]);
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        return -1; // not allowed
    }

    // add the hard link to the directory entry
    if (add_dir_entry(root_dir_inode, link_name + 1, inumber) == -1) {
        rwl_unlock(&inode_locks[inumber]);
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        return -1; // link filename not valid or root directory full of entries
    }

    target_inode->hard_links++; // increment hard_links number
    rwl_unlock(&inode_locks[inumber]);
    rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
    return 0;
}

int tfs_close(int fhandle) {
    mutex_lock(free_open_file_entries_lock);
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        mutex_unlock(free_open_file_entries_lock);
        return -1; // invalid fd
    }

    remove_from_open_file_table(fhandle);
    mutex_unlock(free_open_file_entries_lock);

    return 0;
}

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    mutex_lock(free_open_file_entries_lock);
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        mutex_unlock(free_open_file_entries_lock);
        return -1;
    }

    rwl_wrlock(&inode_locks[file->of_inumber]);
    mutex_lock(&file->lock);
    mutex_unlock(free_open_file_entries_lock);

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
            mutex_lock(free_blocks_lock);
            int bnum = data_block_alloc();
            if (bnum == -1) {
                mutex_unlock(free_blocks_lock);
                mutex_unlock(&file->lock);
                rwl_unlock(&inode_locks[file->of_inumber]);
                return -1; // no space
            }
            mutex_unlock(free_blocks_lock);
            inode->i_data_block = bnum;
        }

        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_write: data block deleted mid-write");

        // Perform the actual write
        memcpy(block + file->of_offset, buffer, to_write);

        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
    }
    mutex_unlock(&file->lock);
    rwl_unlock(&inode_locks[file->of_inumber]);

    return (ssize_t)to_write;
}

ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    mutex_lock(free_open_file_entries_lock);
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        mutex_unlock(free_open_file_entries_lock);
        return -1;
    }

    rwl_rdlock(&inode_locks[file->of_inumber]);
    mutex_lock(&file->lock);
    mutex_unlock(free_open_file_entries_lock);

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
        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_read;
    }
    mutex_unlock(&file->lock);
    rwl_unlock(&inode_locks[file->of_inumber]);

    return (ssize_t)to_read;
}

int tfs_unlink(char const *target) {
    rwl_wrlock(&inode_locks[ROOT_DIR_INUM]);
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_unlink: root dir inode must exist");

    int target_inumber;
    if ((target_inumber = tfs_lookup(target, root_dir_inode)) == -1) {
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        return -1; // target doesn't exist
    }

    mutex_lock(free_open_file_entries_lock);

    rwl_wrlock(&inode_locks[target_inumber]);

    inode_t *target_inode = inode_get(target_inumber);
    ALWAYS_ASSERT(target_inode != NULL,
                  "tfs_unlink: target inode doesn't exist");

    switch (target_inode->i_node_type) {
    case T_SYM_LINK:
        // remove its entry from the root directory
        if (clear_dir_entry(root_dir_inode, target + 1) == -1) {
            rwl_unlock(&inode_locks[target_inumber]);
            mutex_unlock(free_open_file_entries_lock);
            rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
            return -1; // target doesn't exist anymore
        }

        rwl_unlock(&inode_locks[target_inumber]);
        mutex_unlock(free_open_file_entries_lock);
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        // free the inode and the associated block
        inode_delete(target_inumber);
        break;
    case T_FILE: // hard link
        // can't delete an opened file
        if (is_file_opened(target_inumber) == 0 &&
            target_inode->hard_links == 1) {
            rwl_unlock(&inode_locks[target_inumber]);
            mutex_unlock(free_open_file_entries_lock);
            rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
            return -1;
        }

        // remove its entry from the root directory
        if (clear_dir_entry(root_dir_inode, target + 1) == -1) {
            rwl_unlock(&inode_locks[target_inumber]);
            mutex_unlock(free_open_file_entries_lock);
            rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
            return -1; // target doesn't exist anymore
        }

        if (target_inode->hard_links-- == 1) {
            // free the inode and the associated block
            rwl_unlock(&inode_locks[target_inumber]);
            mutex_unlock(free_open_file_entries_lock);
            rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
            inode_delete(target_inumber);
        } else {
            rwl_unlock(&inode_locks[target_inumber]);
            mutex_unlock(free_open_file_entries_lock);
            rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        }
        break;
    case T_DIRECTORY:
        // deleting root is not allowed
        rwl_unlock(&inode_locks[target_inumber]);
        mutex_unlock(free_open_file_entries_lock);
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        return -1;
        break;
    default:
        rwl_unlock(&inode_locks[target_inumber]);
        mutex_unlock(free_open_file_entries_lock);
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        break;
    }

    return 0;
}

int tfs_copy_from_external_fs(char const *source_path, char const *dest_path) {
    struct stat stat_buffer;

    if (stat(source_path, &stat_buffer) == -1 ||
        stat_buffer.st_size > state_block_size()) {
        // pathname does not exist or file size exceeds block size
        return -1;
    }

    size_t source_size = (size_t)stat_buffer.st_size;

    rwl_rdlock(&inode_locks[ROOT_DIR_INUM]);
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_copy_from_external_fs: root dir inode must exist");

    int fhandle;
    if (tfs_lookup(dest_path, root_dir_inode) == -1) {
        // if file doesn't exist, create it
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        fhandle = tfs_open(dest_path, TFS_O_CREAT);
    } else {
        // if file exists, delete current content
        rwl_unlock(&inode_locks[ROOT_DIR_INUM]);
        fhandle = tfs_open(dest_path, TFS_O_TRUNC);
    }

    if (fhandle == -1)
        return -1;

    if (source_size == 0) {
        // nothing to copy
        tfs_close(fhandle);
        return 0;
    }

    // opens source file
    int fd = open(source_path, O_RDONLY);
    if (fd == -1)
        return -1;

    // reads source file content to a buffer
    char buffer[source_size];
    if (read(fd, buffer, source_size) < source_size)
        return -1;
    close(fd);

    // writes the buffer content in destination file
    if (tfs_write(fhandle, buffer, source_size) == -1) {
        tfs_close(fhandle);
        return -1;
    }
    tfs_close(fhandle);

    return 0;
}
