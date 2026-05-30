#include "userprog/fd.h"

// TODO: skip STDIN_FILENO, STDOUT_FILENO (no stderr in pintos)
// TODO: register fd->fh mapping, hashtable?
int
fd_create(struct file *file)
{
	// use per-thread mapping + per-thread lock for mapping?
	return FD_INVALID;
}
