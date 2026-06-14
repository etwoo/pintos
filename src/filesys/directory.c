#include "filesys/directory.h"

#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "filesys/inode_disk.h"
#include "threads/malloc.h"
#include "threads/thread.h"

#include <list.h>
#include <stdio.h>
#include <string.h>

static const char PATH_SEP_STR[] = "/";
static const char PATH_SEP_CHAR = '/';
static const char PATH_DOT[] = ".";
static const char PATH_DOT_DOT[] = "..";

/* A directory. */
struct dir {
	struct inode *inode; /* Backing store. */
	off_t pos;           /* Current position. */
	ino_t ino_dotdot;    /* Parent directory when originally opened. */
};

/* A single directory entry. */
struct dir_entry {
	ino_t ino;
	char name[NAME_MAX + 1]; /* Null terminated file name. */
	bool in_use;             /* In use or free? */
};

static const off_t DIRECTORY_SIZE_INIT = 16 * sizeof(struct dir_entry);

static bool
is_dot(const char *name)
{
	return (0 == strcmp(name, PATH_DOT));
}

static bool
is_dotdot(const char *name)
{
	return (0 == strcmp(name, PATH_DOT_DOT));
}

static bool
dir_read_entry(struct inode *inode, off_t offset, struct dir_entry *out)
{
	inode_lock_held_by_current_thread(inode);
	const off_t sz = sizeof(*out);
	return inode_locked_read_at(inode, out, sz, offset) == sz;
}

static size_t
dir_count_entries(struct inode *inode)
{
	inode_lock_held_by_current_thread(inode);
	size_t count = 0;

	struct dir_entry e = {0};
	for (off_t o = 0; dir_read_entry(inode, o, &e); o += sizeof(e)) {
		if (e.in_use && !is_dotdot(e.name)) {
			++count;
		}
	}

	return count;
}

static bool
dir_allocate_entry(struct inode *inode, const struct dir_entry *to_write)
{
	inode_lock_held_by_current_thread(inode);
	off_t ofs = 0;

	/* Set OFS to offset of free slot.
	   If there are no free slots, then it will be set to the
	   current end-of-file.

	   inode_read_at() will only return a short read at end of file.
	   Otherwise, we'd need to verify that we didn't get a short
	   read due to something intermittent such as low memory. */
	struct dir_entry e = {0};
	for (; dir_read_entry(inode, ofs, &e); ofs += sizeof(e)) {
		if (!e.in_use) {
			break;
		}
	}

	const off_t sz = sizeof(*to_write);
	return inode_locked_write_at(inode, to_write, sz, ofs) == sz;
}

static bool
dir_erase_entry(struct inode *inode, off_t pos)
{
	inode_lock_held_by_current_thread(inode);

	struct dir_entry e = {0};
	e.in_use = false;

	const off_t sz = sizeof(e);
	return inode_locked_write_at(inode, &e, sz, pos) == sz;
}

static bool dir_add_dotdot(ino_t, ino_t);

/* Creates root directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create_root(void)
{
	struct dir *root = dir_open_root();
	if (root != NULL) {
		dir_close(root);
		return true; /* Root directory already exists. */
	}

	ino_t ino = 0;
	if (inode_create(DIRECTORY_SIZE_INIT, INODE_FLAG_IS_DIR, &ino)) {
		ASSERT(ino == ROOT_DIRECTORY_INO);
		return dir_add_dotdot(ROOT_DIRECTORY_INO, ROOT_DIRECTORY_INO);
	}

	return false;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
static struct dir *
dir_open(struct inode *inode, ino_t dotdot)
{
	struct dir *dir = calloc(1, sizeof *dir);
	if (inode != NULL && inode_isdir(inode) && dir != NULL) {
		dir->inode = inode;
		dir->pos = 0;
		dir->ino_dotdot = dotdot;
		return dir;
	} else {
		inode_close(inode);
		free(dir);
		return NULL;
	}
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root(void)
{
	const ino_t dotdot = ROOT_DIRECTORY_INO; /* Root is its own parent. */
	return dir_open(inode_open(ROOT_DIRECTORY_INO), dotdot);
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen(struct dir *dir)
{
	return dir_open(inode_reopen(dir->inode), dir->ino_dotdot);
}

/* Destroys DIR and frees associated resources. */
void
dir_close(struct dir *dir)
{
	if (dir != NULL) {
		inode_close(dir->inode);
		free(dir);
	}
}

/* Returns the identifier of the inode encapsulated by DIR. */
ino_t
dir_get_inumber(struct dir *dir)
{
	return inode_get_inumber(dir->inode);
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
static bool
dir_lookup_leaf(struct dir *dir, const char *name, struct inode **inode)
{
	ASSERT(dir != NULL);
	ASSERT(name != NULL);
	*inode = NULL;

	/* See dir_add_leaf() and dir_remove_leaf(). */
	inode_lock_acquire(dir->inode);
	const bool is_removed = inode_locked_is_removed(dir->inode);
	inode_lock_release(dir->inode);

	if (is_removed) {
		/* Refuse new lookups into removed directories (lookups
		 * preceding removal remain valid). */
		return false;
	}

	/* Find requested directory entry. */
	struct dir_entry e = {0};
	for (off_t o = 0; dir_read_entry(dir->inode, o, &e); o += sizeof(e)) {
		if (e.in_use && 0 == strcmp(name, e.name)) {
			*inode = inode_open(e.ino);
			break;
		}
	}

	return *inode != NULL;
}

static struct dir *
dir_open_disk(struct inode *inode)
{
	struct dir *dir = dir_open(inode, ROOT_DIRECTORY_INO);
	if (dir == NULL) {
		return NULL;
	}

	struct inode *parent = NULL;
	if (!dir_lookup_leaf(dir, PATH_DOT_DOT, &parent)) {
		dir_close(dir);
		return NULL;
	}

	ASSERT(parent != NULL);
	dir->ino_dotdot = inode_get_inumber(parent);
	return dir;
}

struct path_part {
	const char *name; /* Does not own. */
	struct list_elem elem;
};

static void
path_part_list_elem_free(struct list_elem *e)
{
	struct path_part *part = list_entry(e, struct path_part, elem);
	free(part);
}

static bool
path_part_list_init(char *path,
                    struct list *list,
                    bool *is_absolute,
                    bool *is_cwd)
{
	*is_absolute = (path[0] == PATH_SEP_CHAR);
	*is_cwd = is_dot(path);

	char *save_ptr = NULL;
	for (char *token = strtok_r(path, PATH_SEP_STR, &save_ptr);
	     token != NULL;
	     token = strtok_r(NULL, PATH_SEP_STR, &save_ptr)) {
		if (is_dot(token)) {
			continue;
		}

		/* Simplify ".." where possible, like: a/../b/c -> b/c. */
		if (is_dotdot(token) && !list_empty(list)) {
			path_part_list_elem_free(list_pop_back(list));
		}

		struct path_part *p = malloc(sizeof(*p));
		if (p == NULL) {
			return false;
		}
		p->name = token;
		list_push_back(list, &p->elem);
	}

	return !list_empty(list) || *is_absolute || *is_cwd;
}

static void
path_part_list_free(struct list *list)
{
	while (!list_empty(list)) {
		path_part_list_elem_free(list_pop_front(list));
	}
}

/* Reach into file.c internals. */
struct file *file_open(struct inode *);

static bool
dir_lookup_impl(struct dir *dir_start,
                struct list *path_parts,
                bool is_absolute,
                struct file **file_out,
                struct dir **dir_out)
{
	bool success = false;

	struct inode *cur = NULL;
	struct dir *dir = is_absolute ? dir_open_root() : dir_start;

	if (list_empty(path_parts)) {
		ASSERT(cur == NULL);
		if (is_absolute) {
			cur = inode_open(ROOT_DIRECTORY_INO);
		} else {
			cur = inode_reopen(dir->inode);
		}
	}

	struct list_elem *e = list_begin(path_parts);
	for (; e != list_end(path_parts); e = list_next(e)) {
		struct path_part *part = list_entry(e, struct path_part, elem);

		if (cur != NULL) {
			if (dir != dir_start) {
				dir_close(dir);
				dir = NULL;
			}
			dir = dir_open_disk(cur); /* Takes ownership. */
			cur = NULL;
			if (dir == NULL) {
				goto done;
			}
		}

		ASSERT(cur == NULL);
		ASSERT(dir != NULL);

		if (!dir_lookup_leaf(dir, part->name, &cur)) {
			goto done;
		}
	}

	if (inode_isdir(cur)) {
		*dir_out = dir_open_disk(cur); /* Takes ownership. */
	} else {
		*file_out = file_open(cur); /* Takes ownership. */
	}
	cur = NULL;
	success = true;

done:
	ASSERT(cur == NULL);
	if (dir != dir_start) {
		dir_close(dir);
	}
	return success;
}

static struct dir *
get_cwd(void)
{
	struct thread *t = thread_current();
	if (t->fs.cwd == NULL) {
		t->fs.cwd = dir_open_root();
	}
	return t->fs.cwd;
}

bool
dir_lookup(char *path, struct file **file, struct dir **dir)
{
	struct list path_parts;
	list_init(&path_parts);

	bool ok = false;

	bool is_absolute = false;
	bool is_cwd = false;
	if (!path_part_list_init(path, &path_parts, &is_absolute, &is_cwd)) {
		ok = false;
	} else if (is_absolute && list_empty(&path_parts)) {
		*dir = dir_open_root();
		ok = (*dir != NULL);
	} else if (is_cwd) {
		struct dir *cwd = get_cwd();
		inode_lock_acquire(cwd->inode);
		const bool is_removed = inode_locked_is_removed(cwd->inode);
		inode_lock_release(cwd->inode);
		if (!is_removed) {
			*dir = dir_reopen(cwd);
		}
		ok = (*dir != NULL);
	} else {
		struct dir *cwd = get_cwd();
		ok = dir_lookup_impl(cwd, &path_parts, is_absolute, file, dir);
	}

	path_part_list_free(&path_parts);
	return ok;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
static bool
dir_add_leaf(struct dir *dir,
             const char *name,
             off_t length,
             uint32_t flags,
             ino_t *ino_preallocated)
{
	struct dir_entry e;
	bool success = false;
	ino_t new_ino = 0; /* Sentinel Value. */

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	/* Hold per-inode lock, unique per on-disk directory, to prevent TOCTOU
	   bugs when checking for collision with an existing directory, choosing
	   a free dir_entry, and adding the requested entry for NAME. */
	inode_lock_acquire(dir->inode);

	/* Refuse changes to removed directories. */
	if (inode_locked_is_removed(dir->inode)) {
		goto done;
	}

	/* Check NAME for validity. */
	if (*name == '\0' ||           /* Name must be non-empty. */
	    strlen(name) > NAME_MAX || /* Name must fit within dir_entry. */
	    is_dot(name)) {
		goto done;
	}

	/* Check that NAME is not in use. */
	for (off_t o = 0; dir_read_entry(dir->inode, o, &e); o += sizeof(e)) {
		if (e.in_use && 0 == strcmp(name, e.name)) {
			goto done;
		}
	}

	const bool isdir = ((flags & INODE_FLAG_IS_DIR) != 0);
	if (isdir) {
		length = DIRECTORY_SIZE_INIT;
	}

	if (ino_preallocated != NULL) {
		new_ino = *ino_preallocated;
	} else if (!inode_create(length, flags, &new_ino)) {
		goto done;
	}

	e.ino = new_ino;
	strlcpy(e.name, name, sizeof e.name);
	e.in_use = true;

	success = dir_allocate_entry(dir->inode, &e);

done:
	inode_lock_release(dir->inode);

	if (success &&          /* Added new dir_entry successfully. */
	    isdir &&            /* New dir_entry is a subdirectory.  */
	    !is_dotdot(name) && /* Subdirectory name is not ".."     */
	    new_ino > 0 &&      /* See note on Sentinel Value above. */
	    /* Add special ".." as first dir_entry in new directory. */
	    !dir_add_dotdot(new_ino, dir_get_inumber(dir))) {
		success = false;
	}

	return success;
}

static bool
dir_leaf_action(char *path,
                bool on_leaf(struct dir *, struct path_part *, void *),
                void *aux)
{
	bool success = false;
	struct list_elem *leaf_elem = NULL;
	struct file *file_unexpected = NULL;
	struct dir *parent = NULL;

	struct list path_parts;
	list_init(&path_parts);

	bool is_absolute = false;
	bool is_cwd = false;
	if (!path_part_list_init(path, &path_parts, &is_absolute, &is_cwd)) {
		goto done;
	}

	if (list_empty(&path_parts)) {
		goto done;
	}

	leaf_elem = list_pop_back(&path_parts);

	file_unexpected = NULL;
	if (!dir_lookup_impl(get_cwd(),
	                     &path_parts,
	                     is_absolute,
	                     &file_unexpected,
	                     &parent)) {
		goto done;
	}

	if (parent == NULL) {
		goto done;
	}

	struct path_part *leaf = list_entry(leaf_elem, struct path_part, elem);
	if (!on_leaf(parent, leaf, aux)) {
		goto done;
	}

	success = true;

done:
	file_close(file_unexpected);
	dir_close(parent);
	if (leaf_elem != NULL) {
		path_part_list_elem_free(leaf_elem);
	}
	path_part_list_free(&path_parts);
	return success;
}

static bool
dir_touch_leaf(struct dir *parent, struct path_part *leaf, void *aux)
{
	const off_t *length = aux;
	return dir_add_leaf(parent, leaf->name, *length, 0, NULL);
}

bool
dir_add(char *path, off_t length)
{
	return dir_leaf_action(path, dir_touch_leaf, &length);
}

static bool
dir_mkdir_leaf(struct dir *parent, struct path_part *leaf, void *aux UNUSED)
{
	return dir_add_leaf(parent, leaf->name, 0, INODE_FLAG_IS_DIR, NULL);
}

bool
dir_mkdir(char *path)
{
	return dir_leaf_action(path, dir_mkdir_leaf, NULL);
}

static bool
dir_add_dotdot(ino_t child, ino_t dotdot)
{
	struct dir *d = dir_open(inode_open(child), dotdot);
	bool ok = dir_add_leaf(d, PATH_DOT_DOT, 0, INODE_FLAG_IS_DIR, &dotdot);
	dir_close(d);
	return ok;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
static bool
dir_remove_leaf(struct dir *dir, const char *name)
{
	struct dir_entry e = {0};
	bool found_entry = false;
	struct inode *to_remove = NULL;
	bool success = false;
	off_t ofs = 0;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	/* Example TOCTOU (race condition) to avoid with locking:

	   1) thread A finds name N1 in directory D at offset X
	   2) scheduler preempts thread A, runs thread B
	   3) thread B finds name N1 in directory D at offset X
	   4) thread B removes entry at offset X
	   5) scheduler runs thread C
	   6) thread C finds free entry in directory D at offset X
	   7) thread C adds a new file with name N2 at offset X
	   8) scheduler preempts thread C, runs threads A
	   9) thread A removes entry at offset X

	   Thread A deletes file named N2, instead of file named N1! */
	inode_lock_acquire(dir->inode);

	/* Find directory entry to remove. */
	for (; dir_read_entry(dir->inode, ofs, &e); ofs += sizeof(e)) {
		if (e.in_use && 0 == strcmp(name, e.name)) {
			found_entry = true;
			break;
		}
	}

	if (!found_entry) {
		goto done;
	}

	to_remove = inode_open(e.ino);
	if (to_remove == NULL) {
		goto done;
	}

	if (inode_disk_isdir(e.ino)) {
		/* Directory hierarchy must be a tree. Barring on-disk
		   corruption, there should never be cycles. Given this
		   assumption, it should be safe to acquire the inode lock
		   of a subdirectory while holding the parent directory inode
		   lock, without causing AB-BA deadlocks. In other words, the
		   directory hierarchy implicitly forces inode locks to be
		   acquired in a consistent order by all threads. */
		inode_lock_acquire(to_remove);
		const bool nonempty = dir_count_entries(to_remove);
		inode_lock_acquire(to_remove);

		/* Cannot remove non-empty subdirectory. */
		if (nonempty) {
			goto done;
		}
	}

	success = dir_erase_entry(dir->inode, ofs);

done:
	inode_lock_release(dir->inode);

	if (success && to_remove != NULL) {
		inode_remove(to_remove);
	}
	inode_close(to_remove);

	return success;
}

static bool
dir_remove_leaf_glue(struct dir *parent,
                     struct path_part *leaf,
                     void *aux UNUSED)
{
	return dir_remove_leaf(parent, leaf->name);
}

bool
dir_remove(char *path)
{
	return dir_leaf_action(path, dir_remove_leaf_glue, NULL);
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir(struct dir *dir, char name[NAME_MAX + 1])
{
	struct dir_entry e;

	// TODO: should this take lock, sychronize with add/remove?
	while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
		dir->pos += sizeof e;
		if (e.in_use && !is_dotdot(e.name)) {
			strlcpy(name, e.name, NAME_MAX + 1);
			return true;
		}
	}
	return false;
}
