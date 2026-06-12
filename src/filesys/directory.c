#include "filesys/directory.h"

#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"

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

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create(size_t entry_cnt)
{
	ino_t ino = 0;
	return inode_create(entry_cnt * sizeof(struct dir_entry), &ino);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open(struct inode *inode)
{
	struct dir *dir = calloc(1, sizeof *dir);
	// TODO: verify inode corresponds to directory (not reg file)
	if (inode != NULL && dir != NULL) {
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

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode(struct dir *dir)
{
	return dir->inode;
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
	// TODO: verify inode corresponds to directory (not reg file)

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
bool
dir_lookup(const struct dir *dir, const char *name, struct inode **inode)
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
path_part_list_init(char *path, struct list *list)
{
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
	return true;
}

static void
path_part_list_free(struct list *list)
{
	while (!list_empty(list)) {
		path_part_list_elem_free(list_pop_front(list));
	}
}

bool
dir_lookup_r(struct dir *dir_start, char *path, struct inode **inode)
{
	bool success = false;
	const bool absolute = (path != NULL && path[0] == PATH_SEP_CHAR);
	struct inode *cur = NULL;
	struct dir *dir = absolute ? dir_open_root() : dir_start;

	struct list path_parts;
	list_init(&path_parts);

	if (!path_part_list_init(path, &path_parts)) {
		goto done;
	}

	struct list_elem *e = list_begin(&path_parts);
	for (; e != list_end(&path_parts); e = list_next(e)) {
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

		if (!dir_lookup(dir, part->name, &cur)) {
			goto done;
		}
	}

	*inode = cur; /* Caller takes ownership of <cur>. */
	success = true;

done:
	ASSERT(cur == NULL);
	if (dir != dir_start) {
		dir_close(dir);
	}
	path_part_list_free(&path_parts);
	return success;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add(struct dir *dir, const char *name, ino_t ino)
{
	struct dir_entry e;
	off_t ofs;
	bool success = false;

	ASSERT(dir != NULL);
	ASSERT(name != NULL);

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
	// TODO: grow directory file size if more slots needed

	/* Write slot. */
	e.in_use = true;
	strlcpy(e.name, name, sizeof e.name);
	e.ino = ino;
	success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
	return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove(struct dir *dir, const char *name)
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
