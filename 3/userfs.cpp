#include "userfs.h"

#include "rlist.h"

#include <stddef.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>


enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char memory[BLOCK_SIZE];
	/** A link in the block list of the owner-file. */
	rlist in_block_list = RLIST_LINK_INITIALIZER;

	/* PUT HERE OTHER MEMBERS */

	size_t used = 0;
};

struct file {
	/**
	 * Doubly-linked intrusive list of file blocks. Intrusiveness of the
	 * list gives you the full control over the lifetime of the items in the
	 * list without having to use double pointers with performance penalty.
	 */
	rlist blocks = RLIST_HEAD_INITIALIZER(blocks);
	/** How many file descriptors are opened on the file. */
	int refs = 0;
	/** File name. */
	std::string name;
	/** A link in the global file list. */
	rlist in_file_list = RLIST_LINK_INITIALIZER;

	/* PUT HERE OTHER MEMBERS */

	size_t size = 0;
};

/**
 * Intrusive list of all files. In this case the intrusiveness of the list also
 * grants the ability to remove items from any position in O(1) complexity
 * without having to know their iterator.
 */
static rlist file_list = RLIST_HEAD_INITIALIZER(file_list);

struct filedesc {
	file *atfile;

	/* PUT HERE OTHER MEMBERS */

	block *current_block = nullptr;
	size_t block_position = 0;
	size_t global_position = 0;

	bool can_read;
	bool can_write;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static std::vector<filedesc*> file_descriptors;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

static file*
find_file(const char *name) {
	rlist *cur;

	rlist_foreach(cur, &file_list) {

		file *find_file = rlist_entry(cur, file, in_file_list);
		if(find_file->name == name) return find_file;
	}
	return nullptr;
}

static block*
block_from_link(rlist *l) {
	return rlist_entry(l, block, in_block_list);
}

static filedesc*
check_descriptor(int fd)

{
	if (fd < 0) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return nullptr;
	}

	if (fd >= (int)file_descriptors.size()) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return nullptr;
	}

	if (file_descriptors[fd] == nullptr) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return nullptr;
	}

	return file_descriptors[fd];
}


static bool
check_permission(filedesc *descriptor, bool need_read, bool need_write)
{

	if (need_read && !descriptor->can_read) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return false;
	}

	if (need_write && !descriptor->can_write) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return false;
	}

	return true;
}


int
ufs_open(const char *filename, int flags)
{
	file *new_file = find_file(filename);

	if(!new_file) {
		if (flags & UFS_CREATE) {
			new_file = new file;
			new_file->name = filename;
			rlist_add_tail(&file_list, &new_file->in_file_list);
		}
		else {
			ufs_error_code = UFS_ERR_NO_FILE;
			return -1;
		}
	}

	filedesc *new_descriptor = new filedesc;
	new_descriptor->atfile = new_file;

	switch (flags & (UFS_READ_ONLY | UFS_WRITE_ONLY | UFS_READ_WRITE)) {
		case UFS_READ_ONLY:
			new_descriptor->can_read = true;
			new_descriptor->can_write = false;
			break;
		case UFS_WRITE_ONLY:
			new_descriptor->can_read = false;
			new_descriptor->can_write = true;
			break;
		case UFS_READ_WRITE:
			new_descriptor->can_read = true;
			new_descriptor->can_write = true;
			break;
		default:
			new_descriptor->can_read = true;
			new_descriptor->can_write = true;
			break;
	}

	new_file->refs++;

	for(size_t i = 0; i < file_descriptors.size(); i++) {
		if (!file_descriptors[i]) {
			file_descriptors[i] = new_descriptor;
			return i;
		}
	}

	file_descriptors.push_back(new_descriptor);

	return file_descriptors.size() - 1;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
  filedesc *descriptor = check_descriptor(fd);
  if (!descriptor) return -1;

  if(!check_permission(descriptor, false, true)) return -1;

  file *file = descriptor->atfile;

  if (descriptor->global_position + size > MAX_FILE_SIZE) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }

  size_t bytes_written_total = 0;

  while (bytes_written_total < size) {

    if (!descriptor->current_block) {
      size_t pos = 0;
      rlist *node = file->blocks.next;
      descriptor->current_block = nullptr;

      while (node != &file->blocks) {
        block *b = block_from_link(node);
        if (pos + b->used > descriptor->global_position) {
          descriptor->current_block = b;
          descriptor->block_position = descriptor->global_position - pos;
          break;
        }
        pos += b->used;
        node = node->next;
      }

      if (!descriptor->current_block) {
        if (rlist_empty(&file->blocks)) {
          block *new_block = new block;
          rlist_add_tail(&file->blocks, &new_block->in_block_list);
          descriptor->current_block = new_block;
          descriptor->block_position = 0;
        } else {
          descriptor->current_block = block_from_link(file->blocks.prev);
          descriptor->block_position = descriptor->current_block->used;
        }
      }
    }

    block *current_block = descriptor->current_block;

    size_t free_space = BLOCK_SIZE - descriptor->block_position;
    size_t to_write = std::min(free_space, size - bytes_written_total);

    memcpy(current_block->memory + descriptor->block_position, buf + bytes_written_total, to_write);

    descriptor->block_position += to_write;
    descriptor->global_position += to_write;
    bytes_written_total += to_write;

    if (current_block->used < descriptor->block_position)
      current_block->used = descriptor->block_position;

    if (descriptor->block_position == BLOCK_SIZE) {

      if (current_block->in_block_list.next != &file->blocks) {
        descriptor->current_block =
          block_from_link(current_block->in_block_list.next);
      } else {

        block *new_block = new block;
        rlist_add_tail(&file->blocks,
                 &new_block->in_block_list);

        descriptor->current_block = new_block;
      }

      descriptor->block_position = 0;
    }
  }
  
  if (file->size < descriptor->global_position)
    file->size = descriptor->global_position;

  return bytes_written_total;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{

	filedesc *descriptor = check_descriptor(fd);
	if (!descriptor) return -1;

	if(!check_permission(descriptor, true, false)) return -1;

	file *file = descriptor->atfile;

	if (descriptor->global_position >= file->size) return 0;

	if (!descriptor->current_block) {

		if (rlist_empty(&file->blocks)) return 0;
		descriptor->current_block = block_from_link(file->blocks.next);
	}

	size_t bytes_read_total = 0;

	while (bytes_read_total < size && descriptor->global_position < file->size) {

		block *current_block = descriptor->current_block;

		size_t available_in_block = current_block->used - descriptor->block_position;
		size_t bytes_to_read_now = std::min(available_in_block, size - bytes_read_total);

		memcpy(buf + bytes_read_total, current_block->memory + descriptor->block_position, bytes_to_read_now);

		bytes_read_total += bytes_to_read_now;
		descriptor->block_position += bytes_to_read_now;
		descriptor->global_position += bytes_to_read_now;

		if (descriptor->block_position == current_block->used) {
			if (current_block->in_block_list.next == &file->blocks) break;
			descriptor->current_block = block_from_link(current_block->in_block_list.next);
			descriptor->block_position = 0;
		}
	}

	return bytes_read_total;
}

int
ufs_close(int fd)
{

	filedesc *descriptor = check_descriptor(fd);
	if (!descriptor) return -1;

	file *file = descriptor->atfile;

	file->refs--;
	delete descriptor;
	file_descriptors[fd] = nullptr;

	if (file->refs == 0 && file->name.empty()) {

		rlist *node = file->blocks.next;

		while (node != &file->blocks) {

			rlist *next = node->next;

			block *current_block = block_from_link(node);
			rlist_del(&current_block->in_block_list);
			delete current_block;

			node = next;
		}
		delete file;
	}
	return 0;
}

int
ufs_delete(const char *filename)
{

	file *found_file = find_file(filename);

	if (!found_file) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	rlist_del(&found_file->in_file_list);

	found_file->name.clear();

	if (found_file->refs == 0) {

		rlist *node = found_file->blocks.next;

		while (node != &found_file->blocks) {

			rlist *next = node->next;
			block *current_block = block_from_link(node);
			rlist_del(&current_block->in_block_list);
			delete current_block;

			node = next;
		}
		delete found_file;
	}
	return 0;
}

#if NEED_RESIZE

int ufs_resize(int fd, size_t new_size)
{
    filedesc *descriptor = check_descriptor(fd);
    if (!descriptor) return -1;

    if(!check_permission(descriptor, false, true)) return -1;

    file *file = descriptor->atfile;

    if (new_size > MAX_FILE_SIZE) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    if (new_size == file->size)
        return 0;

    if (new_size > file->size) {
        size_t old_pos = descriptor->global_position;
        block *old_block = descriptor->current_block;
        size_t old_block_pos = descriptor->block_position;

        size_t bytes_to_add = new_size - file->size;

        descriptor->global_position = file->size;
        descriptor->current_block = nullptr;
        descriptor->block_position = 0;

        std::vector<char> zero_buffer(bytes_to_add, 0);
        if (ufs_write(fd, zero_buffer.data(), bytes_to_add) < 0)
            return -1;

        descriptor->global_position = old_pos;
        descriptor->current_block = old_block;
        descriptor->block_position = old_block_pos;

        return 0;
    }

    size_t current_offset = 0;
    rlist *node = file->blocks.next;
    std::vector<rlist*> nodes_to_delete;

    while (node != &file->blocks) {
        block *current_block = block_from_link(node);

        if (current_offset + current_block->used > new_size) {

			size_t bytes_to_keep = new_size - current_offset;
            current_block->used = bytes_to_keep;

            rlist *next_node = node->next;
            while (next_node != &file->blocks) {
                nodes_to_delete.push_back(next_node);
                next_node = next_node->next;
            }
            break;
        }

        current_offset += current_block->used;
        node = node->next;
    }

    for (rlist *del_node : nodes_to_delete) {
        block *del_block = block_from_link(del_node);
        rlist_del(del_node);
        delete del_block;
    }

    file->size = new_size;

    for (size_t i = 0; i < file_descriptors.size(); i++) {
        filedesc *d = file_descriptors[i];
        if (d && d->atfile == file) {
            if (d->global_position > new_size) {
                d->global_position = new_size;
                d->current_block = nullptr;
                d->block_position = 0;
            }

            if (d->global_position < file->size) {
                size_t pos = 0;
                rlist *n = file->blocks.next;
                while (n != &file->blocks) {
                    block *b = block_from_link(n);
                    if (pos + b->used > d->global_position) {
                        d->current_block = b;
                        d->block_position = d->global_position - pos;
                        break;
                    }
                    pos += b->used;
                    n = n->next;
                }
            }
        }
    }

    return 0;
}

#endif

void
ufs_destroy(void)
{

	for (size_t i = 0; i < file_descriptors.size(); i++) {
		if (file_descriptors[i] != nullptr) {
			ufs_close(i);
		}
	}

	rlist *file_node = file_list.next;

	while (file_node != &file_list) {

		rlist *next_file = file_node->next;
		file *current_file = rlist_entry(file_node, file, in_file_list);
		rlist *block_node = current_file->blocks.next;

		while (block_node != &current_file->blocks) {

			rlist *next_block = block_node->next;
			block *current_block = block_from_link(block_node);
			rlist_del(&current_block->in_block_list);
			delete current_block;

			block_node = next_block;
		}

		rlist_del(&current_file->in_file_list);
		delete current_file;

		file_node = next_file;
	}

	std::vector<filedesc*> empty_vector;
	file_descriptors.swap(empty_vector);
}