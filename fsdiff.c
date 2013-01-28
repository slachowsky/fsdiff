#define _BSD_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>

#include <libtar.h>

#define main bsdiff_main
#include "bsdiff.c"
#undef main

static int bsdiff(char *oldfile, char *newfile, char *patchfile)
{
	int cpid, ret;
	char *argv[5] = { "bsdiff", oldfile, newfile, patchfile, NULL};

	cpid = fork();
	if (cpid == -1) {
		perror("fork");
		return 1;
	}
	if (cpid == 0) {
		ret = bsdiff_main(4, argv);
		exit(ret);
	} else {
		wait(&ret);
	}
	return WEXITSTATUS(ret);
}

TAR *t;

static char *base1;
static char *base2;
static char prefix[PATH_MAX];

static int filter(const struct dirent *d)
{
	return strcmp(d->d_name, ".") && strcmp(d->d_name, "..");
}

static int cmpfiles(const char* a, const char* b, size_t len)
{
	// add error checking
	int fd1, fd2, ret;
	void *p1, *p2;
	fd1 = open(a, O_RDONLY);
	fd2 = open(b, O_RDONLY);
	p1 = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd1, 0);
	p2 = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd2, 0);
	ret = memcmp(p1, p2, len);
	munmap(p1, len);
	munmap(p2, len);
	close(fd1);
	close(fd2);
	return ret;
}

static int do_add(struct dirent *d)
{
	int ret;
	char realname[PATH_MAX];
	char savename[PATH_MAX];

	sprintf(realname, "%s%s/%s", base2, prefix, d->d_name);
	sprintf(savename, "%s%s/%s", "add", prefix, d->d_name);

	fprintf(stderr, "%s/%s was added\n", prefix, d->d_name);
	if(!t) return 0;

	if(d->d_type == DT_DIR)
		ret = tar_append_tree(t, realname, savename);
	else
		ret = tar_append_file(t, realname, savename);
	if(ret < 0)
		perror("tar_append_file");
}

static int do_delete(struct dirent *d)
{
	int ret;
	struct stat sb;
	char realname[PATH_MAX];
	char savename[PATH_MAX];

	sprintf(realname, "%s%s/%s", base1, prefix, d->d_name);
	sprintf(savename, "%s%s/%s", "delete", prefix, d->d_name);

	if(d->d_type == DT_DIR) {
		DIR *dir;
		struct dirent *dp;
		int len = strlen(prefix);
		strcat(prefix, "/");
		strcat(prefix, d->d_name);
		dir = opendir(realname);
		while ((dp = readdir(dir)) != NULL) {
			if (!filter(dp))
				continue;
			do_delete(dp);
		}
		prefix[len] = 0;
	}

	fprintf(stderr, "%s/%s was deleted\n", prefix, d->d_name);
	if(!t) return 0;

	ret = lstat(realname, &sb);
	th_set_from_stat(t, &sb);
	th_set_path(t, savename);
	th_set_size(t, 0);
	th_finish(t);
	if(t->options & TAR_VERBOSE)
		th_print_long_ls(t);
	th_write(t);
}

static int do_diff(struct dirent *d1, struct dirent *d2)
{
	int ret;
	struct stat sb;
	char realname1[PATH_MAX];
	char realname2[PATH_MAX];
	char savename[PATH_MAX];
	//char cmd[4096];
	fprintf(stderr, "%s/%s differs\n", prefix, d1->d_name);
	if(!t) return 0;

	sprintf(realname1, "%s%s/%s", base1, prefix, d1->d_name);
	sprintf(realname2, "%s%s/%s", base2, prefix, d2->d_name);
	sprintf(savename, "%s%s/%s", "diff", prefix, d1->d_name);

	//sprintf(cmd, "cp %s patch", realname2);
	//sprintf(cmd, "/home/stephan/rdiff %s %s patch", realname1, realname2);
	//sprintf(cmd, "/usr/bin/xdelta3 -e -s %s %s patch", realname1, realname2);
	//sprintf(cmd, "/usr/bin/xdelta3 -e -S djw -9 -s %s %s patch", realname1, realname2);
	//sprintf(cmd, "/usr/bin/bsdiff %s %s patch", realname1, realname2);
	//sprintf(cmd, "/home/stephan/src/fsdiff/bsdiff %s %s patch", realname1, realname2);
	//system(cmd);
	bsdiff(realname1, realname2, "patch");

	ret = lstat(realname2, &sb);
	th_set_from_stat(t, &sb);
	th_set_path(t, savename);
	ret = lstat("patch", &sb);
	th_set_size(t, sb.st_size);
	th_finish(t);
	if(t->options & TAR_VERBOSE)
		th_print_long_ls(t);
	th_write(t);
	tar_append_regfile(t, "patch");
	//system("rm patch");
	unlink("patch");
}

static int cmpdir(const char* a, const char* b)
{
	struct dirent **namelist1, **namelist2;
	int n1, n2;
	int i1 = 0, i2 = 0;
	n1 = scandir(a, &namelist1, filter, alphasort);
	if (n1 < 0)
		perror("scandir");
	//printf("%d entries in %s\n", n1, a);
	n2 = scandir(b, &namelist2, filter, alphasort);
	if (n2 < 0)
		perror("scandir");
	//printf("%d entries in %s\n", n2, b);
	while (i1 < n1 || i2 < n2) {
		int ret; 
		if (i1 >= n1)
			ret = 1;
		else if (i2 >= n2)
			ret = -1;
		else
			ret = alphasort((const struct dirent **)namelist1+i1, (const struct dirent **)namelist2+i2);

		if (ret < 0) {
			do_delete(namelist1[i1]);
			free(namelist1[i1]);
			i1++;
		} else if (ret > 0) {
			do_add(namelist2[i2]);
			free(namelist2[i2]);
			i2++;
		} else {
			char buf1[1024], buf2[1024];
			//printf("%s match (recurse)\n", namelist1[i1]->d_name);
			sprintf(buf1, "%s/%s", a, namelist1[i1]->d_name);
			sprintf(buf2, "%s/%s", b, namelist2[i2]->d_name);
			if(namelist1[i1]->d_type != namelist2[i2]->d_type)
			{
				fprintf(stderr, "%s types differ, delete then add?\n", namelist1[i1]->d_name);
			} else if(namelist1[i1]->d_type == DT_LNK) {
				int len1, len2;
				len1 = readlink(buf1, buf1, 1024);
				len2 = readlink(buf2, buf2, 1024);
				//printf("SYMLINK %d %d\n", len1, len2);
				if(len1 != len2 || strncmp(buf1, buf2, len1)) {
					fprintf(stderr, "%s symlink target mismatch\n", namelist1[i1]->d_name);
				}
			} else if(namelist1[i1]->d_type == DT_DIR) {
				int len;
				len = strlen(prefix);
				strcat(prefix, "/");
				strcat(prefix, namelist1[i1]->d_name);
				cmpdir(buf1,buf2);
				prefix[len] = 0;
			} else {
				// stat files and compare
				struct stat sb1, sb2;
				ret = stat(buf1, &sb1);
				if(ret != 0)
					fprintf(stderr, "couldn't stat %s\n", buf1);
				ret = stat(buf2, &sb2);
				if(ret != 0)
					fprintf(stderr, "couldn't stat %s\n", buf2);
				if(sb1.st_size != sb2.st_size ||
						cmpfiles(buf1, buf2, sb1.st_size)) {
					 do_diff(namelist1[i1], namelist2[i2]);
				}
			}
			free(namelist1[i1]);
			free(namelist2[i2]);
			i1++, i2++;
		}
	}
	free(namelist1);
	free(namelist2);

	return 0;
}

int main(int argc, char **argv)
{
	int ret;
	if(argc < 3) {
		fprintf(stderr, "Usage: %s old new [patch]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	t = NULL;

	if(argc >= 4) {
		if(!strcmp(argv[3], "-"))
			ret = tar_fdopen(&t, 1, "stdout", NULL, O_WRONLY|O_CREAT, 0644, TAR_GNU/*|TAR_VERBOSE*/);
		else
			ret = tar_open(&t, argv[3], NULL, O_WRONLY|O_CREAT, 0644, TAR_GNU/*|TAR_VERBOSE*/);
		if(ret != 0) {
			fprintf(stderr, "%d\n", ret);
			perror("tar_open");
		}
	}

	base1 = argv[1];
	base2 = argv[2];
	cmpdir(argv[1], argv[2]);

	if(t)
		tar_close(t);

	return 0;
}
