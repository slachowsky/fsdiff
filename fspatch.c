#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <libtar.h>
#include <utime.h>

static TAR *t;
static const char* base;

/* libtar */
#define HAVE_LCHOWN
static int
tar_set_file_perms(TAR *t, char *realname)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	struct utimbuf ut;
	char *filename;

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	uid = th_get_uid(t);
	gid = th_get_gid(t);
	ut.modtime = ut.actime = th_get_mtime(t);

	/* change owner/group */
	if (geteuid() == 0)
#ifdef HAVE_LCHOWN
		if (lchown(filename, uid, gid) == -1)
		{
# ifdef DEBUG
			fprintf(stderr, "lchown(\"%s\", %d, %d): %s\n",
				filename, uid, gid, strerror(errno));
# endif
#else /* ! HAVE_LCHOWN */
		if (!TH_ISSYM(t) && chown(filename, uid, gid) == -1)
		{
# ifdef DEBUG
			fprintf(stderr, "chown(\"%s\", %d, %d): %s\n",
				filename, uid, gid, strerror(errno));
# endif
#endif /* HAVE_LCHOWN */
			return -1;
		}

	/* change access/modification time */
	if (!TH_ISSYM(t) && utime(filename, &ut) == -1)
	{
#ifdef DEBUG
		perror("utime()");
#endif
		return -1;
	}

	/* change permissions */
	if (!TH_ISSYM(t) && chmod(filename, mode) == -1)
	{
#ifdef DEBUG
		perror("chmod()");
#endif
		return -1;
	}

	return 0;
}

/* return a buffer of the regfile contents */
int
tar_buffer_regfile(TAR *t, void **buffer, size_t *psize)
{
	size_t size;
	int i, k;
	void *buf;

#ifdef DEBUG
	printf("==> tar_extract_regfile(t=0x%lx, realname=\"%s\")\n", t,
	       realname);
#endif

	if (!TH_ISREG(t))
	{
		errno = EINVAL;
		return -1;
	}

	size = th_get_size(t);

	*buffer = buf = malloc((size + T_BLOCKSIZE - 1) & ~(T_BLOCKSIZE-1));
	if(psize)
		*psize = size;

	/* buffer the file */
	for (i = size; i > 0; i -= T_BLOCKSIZE, buf += T_BLOCKSIZE)
	{
		k = tar_block_read(t, buf);
		if (k != T_BLOCKSIZE)
		{
			if (k != -1)
				errno = EINVAL;
			return -1;
		}
	}

	return 0;
}

/* bspatch */
static off_t offtin(const u_char *buf)
{
	off_t y;

	y=buf[7]&0x7F;
	y=y*256;y+=buf[6];
	y=y*256;y+=buf[5];
	y=y*256;y+=buf[4];
	y=y*256;y+=buf[3];
	y=y*256;y+=buf[2];
	y=y*256;y+=buf[1];
	y=y*256;y+=buf[0];

	if(buf[7]&0x80) y=-y;

	return y;
}

static int bspatch(const char *oldfile, const char *newfile, int patch)
{
	const u_char header[32];
	u_char *old, *new;
	ssize_t oldsize, newsize;
	ssize_t ctrlsize, diffsize, nctrl;
	off_t oldpos, newpos;
	off_t *ctrl;
	off_t i, j;
	int fd, ret;

	read(patch, header, sizeof(header));

	if (memcmp(header, "BSDIFFXX", 8) != 0)
		return 1;

	ctrlsize = offtin(header + 8);
	diffsize = offtin(header + 16);
	newsize = offtin(header + 24);

	ctrl = malloc(ctrlsize);
	read(patch, ctrl, ctrlsize);
	nctrl = ctrlsize/sizeof(off_t);
	for(i=0;i<nctrl;i++)
		ctrl[i] = offtin((u_char*)&ctrl[i]);

	fd = open(oldfile, O_RDONLY);
	oldsize = lseek(fd, 0, SEEK_END);
	old = mmap(NULL, oldsize, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);

	fd = open(newfile, O_CREAT|O_RDWR, 0666);
	ret = ftruncate(fd, newsize);
	new = mmap(NULL, newsize, PROT_WRITE, MAP_SHARED, fd, 0);
	if (new == MAP_FAILED)
		perror("mmap");
	close(fd);

	oldpos=0;newpos=0;
	for(j=0;j<nctrl;j+=3) {
		off_t x = ctrl[j], y = ctrl[j+1], z = ctrl[j+2];

		/* Read diff string */
		read(patch, new + newpos, x);

		/* Add old data to diff string */
		for(i=0;i<x;i++)
			if((oldpos+i>=0) && (oldpos+i<oldsize))
				new[newpos+i]+=old[oldpos+i];

		/* Adjust pointers */
		newpos+=x+y;
		oldpos+=x+z;
	}
	oldpos=0;newpos=0;
	for(j=0;j<nctrl;j+=3) {
		off_t x = ctrl[j], y = ctrl[j+1], z = ctrl[j+2];

		/* Read extra string */
		read(patch, new + newpos + x, y);

		/* Adjust pointers */
		newpos+=x+y;
	}

	//while(newpos<newsize) check me!

	free(ctrl);
	munmap(old, oldsize);
	munmap(new, newsize);
	return 0;
}

static int do_add(const char* name)
{
	char realname[PATH_MAX];
	sprintf(realname, "%s/%s", base, name);
	printf("adding %s\n", realname);
	tar_extract_file(t, realname);
}

static int do_delete(const char* name)
{
	int ret;
	char realname[PATH_MAX];
	sprintf(realname, "%s/%s", base, name);
	printf("deleting %s\n", realname);
	if(TH_ISDIR(t)) {
		ret = rmdir(realname);
		if(ret == -1)
			perror("rmdir");
	} else {
		ret = unlink(realname);
		if(ret == -1)
			perror("unlink");
	}
}

static int do_patch(const char* name)
{
	int ret;
	size_t size;
	int pipefd[2];
	void *patch;
	char realname[PATH_MAX];
	char tmpname[PATH_MAX];
	//char cmd[4096];
	sprintf(realname, "%s/%s", base, name);
	sprintf(tmpname, "%s/%sXXXXXX", base, name);
	printf("patching %s\n", realname);
	//tar_extract_regfile(t, patchname);
	tar_buffer_regfile(t, &patch, &size);
	pipe(pipefd);
	if(fork()) {
		close(pipefd[1]);
		ret = bspatch(realname, tmpname, pipefd[0]);
		wait(NULL);
	} else {
		close(pipefd[0]);
		ret = write(pipefd[1], patch, size);
		exit(0);
	}
	free(patch);
	//printf("bspatch returned %d\n", ret);
	//system(cmd);
	unlink(realname);
	link(tmpname, realname);
	unlink(tmpname);
	//unlink(patchname);
	tar_set_file_perms(t, realname);
}

int main(int argc, char **argv)
{
	int ret;
	if(argc != 3) {
		fprintf(stderr, "Usage: %s patch.tar dir\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	ret = tar_open(&t, argv[1], NULL, O_RDONLY, 0, TAR_GNU/*|TAR_VERBOSE*/);
	if(ret != 0) {
		printf("%d\n", ret);
		perror("tar_open");
	}

	base = argv[2];
	while( (ret = th_read(t)) == 0) {
		char* verb = th_get_pathname(t);
		//printf("tar: %s\n", verb);
		if(!strncmp(verb, "add/", 4)) {
			do_add(verb+4);
		} else if (!strncmp(verb, "delete/", 7)) {
			do_delete(verb+7);
		} else if (!strncmp(verb, "diff/", 5)) {
			do_patch(verb+5);
		} else {
			printf("unknown verb\n");
			tar_skip_regfile(t);
		}
		free(verb);
	}
	if(ret < 0) {
		printf("%d\n", ret);
		perror("th_read");
	}

	tar_close(t);

	return ret;
}
