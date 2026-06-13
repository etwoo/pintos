#include "filesys/directory.h"

#include "filesys/filesys.h"
#include "filesys/inode.h"
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
};

/* A single directory entry. */
struct dir_entry {
	ino_t ino;
	char name[NAME_MAX + 1]; /* Null terminated file name. */
	bool in_use;             /* In use or free? */
};

static const off_t DIRECTORY_SIZE_INIT = 16 * sizeof(struct dir_entry);

/* Creates root directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create_root(void)
{
	struct dir *root = dir_open_root();
	if (root != NULL) {
		return true; /* Root directory already exists. */
	}

	ino_t ino = 0;
	if (inode_create(DIRECTORY_SIZE_INIT, INODE_FLAG_IS_DIRECTORY, &ino)) {
		ASSERT(ino == ROOT_DIRECTORY_INO);
		return true;
	}

	return false;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
static struct dir *
dir_open(struct inode *inode)
{
	struct dir *dir = calloc(1, sizeof *dir);
	if (inode != NULL && inode_isdir(inode) && dir != NULL) {
		dir->inode = inode;
		dir->pos = 0;
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
	return dir_open(inode_open(ROOT_DIRECTORY_INO));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen(struct dir *dir)
{
	return dir_open(inode_reopen(dir->inode));
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

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup(const struct dir *dir,
       const char *name,
       struct dir_entry *ep,
       off_t *ofsp)
{
	struct dir_entry e;
	size_t ofs;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);
	ASSERT(inode_isdir(dir->inode));

	if (inode_is_removed(dir->inode)) {
		/* Refuse new lookups into removed directories (lookups
		 * preceding removal remain valid). */
		return false;
	}

	for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
	     ofs += sizeof e)
		if (e.in_use && !strcmp(name, e.name)) {
			if (ep != NULL)
				*ep = e;
			if (ofsp != NULL)
				*ofsp = ofs;
			return true;
		}
	return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
static bool
dir_lookup_leaf(const struct dir *dir, const char *name, struct inode **inode)
{
	struct dir_entry e;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	if (lookup(dir, name, &e, NULL))
		*inode = inode_open(e.ino);
	else
		*inode = NULL;

	return *inode != NULL;
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
	*is_cwd = (0 == strcmp(path, PATH_DOT));

	char *save_ptr = NULL;
	for (char *token = strtok_r(path, PATH_SEP_STR, &save_ptr);
	     token != NULL;
	     token = strtok_r(NULL, PATH_SEP_STR, &save_ptr)) {
		if (0 == strcmp(token, PATH_DOT)) {
			continue;
		}

		/* Simplify ".." where possible, like: a/../b/c -> b/c. */
		if (0 == strcmp(token, PATH_DOT_DOT) && !list_empty(list)) {
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
		// TODO: handle ".." reaching outside of dir_start

		if (cur != NULL) {
			if (dir != dir_start) {
				dir_close(dir);
				dir = NULL;
			}
			dir = dir_open(cur); /* Takes ownership of <cur>. */
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
		*dir_out = dir_open(cur); /* Takes ownership. */
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
		*dir = dir_reopen(get_cwd());
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
dir_add_leaf(struct dir *dir, const char *name, off_t length, uint32_t flags)
{
	struct dir_entry e;
	off_t ofs;
	bool success = false;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	if (inode_is_removed(dir->inode)) {
		/* Refuse mutations on removed directories. */
		return false;
	}

	/* Check NAME for validity. */
	if (*name == '\0' || strlen(name) > NAME_MAX)
		return false;

	/* Check that NAME is not in use. */
	if (lookup(dir, name, NULL, NULL))
		goto done;

	/* Set OFS to offset of free slot.
	   If there are no free slots, then it will be set to the
	   current end-of-file.

	   inode_read_at() will only return a short read at end of file.
	   Otherwise, we'd need to verify that we didn't get a short
	   read due to something intermittent such as low memory. */
	for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
	     ofs += sizeof e)
		if (!e.in_use)
			break;

	if ((flags & INODE_FLAG_IS_DIRECTORY) != 0) {
		length = DIRECTORY_SIZE_INIT;
	}

	ino_t ino = 0;
	if (!inode_create(length, flags, &ino)) {
		goto done;
	}

	/* Write slot. */
	e.in_use = true;
	strlcpy(e.name, name, sizeof e.name);
	e.ino = ino;
	success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
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
	return dir_add_leaf(parent, leaf->name, *length, 0);
}

bool
dir_add(char *path, off_t length)
{
	return dir_leaf_action(path, dir_touch_leaf, &length);
}

static bool
dir_mkdir_leaf(struct dir *parent, struct path_part *leaf, void *aux UNUSED)
{
	return dir_add_leaf(parent, leaf->name, 0, INODE_FLAG_IS_DIRECTORY);
}

bool
dir_mkdir(char *path)
{
	return dir_leaf_action(path, dir_mkdir_leaf, NULL);
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
static bool
dir_remove_leaf(struct dir *dir, const char *name)
{
	struct dir_entry e;
	struct inode *inode = NULL;
	bool success = false;
	off_t ofs;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

	/* Find directory entry. */
	if (!lookup(dir, name, &e, &ofs))
		goto done;

	/* Open inode. */
	inode = inode_open(e.ino);
	if (inode == NULL)
		goto done;

	if (inode_isdir(inode)) {
		struct dir_entry e;
		for (size_t ofs = 0;
		     inode_read_at(inode, &e, sizeof(e), ofs) == sizeof(e);
		     ofs += sizeof(e)) {
			if (e.in_use) {
				/* Cannot remove non-empty subdirectory. */
				goto done;
			}
		}
	}

	/* Erase directory entry. */
	e.in_use = false;
	if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e)
		goto done;

	/* Remove inode. */
	inode_remove(inode);
	success = true;

done:
	inode_close(inode);
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

	while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
		dir->pos += sizeof e;
		if (e.in_use) {
			strlcpy(name, e.name, NAME_MAX + 1);
			return true;
		}
	}
	return false;
}
