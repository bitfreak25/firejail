/*
 * Copyright (C) 2017 Firejail Authors
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "firejail.h"
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#define MAXBUF 4096

static const char * const lib_paths[] = {
	"/lib",
	"/lib/x86_64-linux-gnu",
	"/lib64",
	"/usr/lib",
	"/usr/lib/x86_64-linux-gnu",
	LIBDIR,
	"/usr/local/lib",
	NULL
};						  // Note: this array is duplicated in src/fldd/main.c

extern void fslib_install_stdc(void);
extern void fslib_install_system(void);

static int lib_cnt = 0;
static int dir_cnt = 0;

static void report_duplication(const char *full_path) {
	char *fname = strrchr(full_path, '/');
	if (fname && *(++fname) != '\0') {
		// report the file on all bin paths
		int i = 0;
		while (lib_paths[i]) {
			char *p;
			if (asprintf(&p, "%s/%s", lib_paths[i], fname) == -1)
				errExit("asprintf");
			fs_logger2("clone", p);
			free(p);
			i++;
		}
	}
}

static char *build_dest_dir(const char *full_path) {
	assert(full_path);
	if (strstr(full_path, "/x86_64-linux-gnu/"))
		return RUN_LIB_DIR "/x86_64-linux-gnu";
	return RUN_LIB_DIR;
}

// copy fname in private_run_dir
void fslib_duplicate(const char *full_path) {
	assert(full_path);

	struct stat s;
	if (stat(full_path, &s) != 0 || s.st_uid != 0 || access(full_path, R_OK))
		return;

	char *dest_dir = build_dest_dir(full_path);

	// don't copy it if the file is already there
	char *ptr = strrchr(full_path, '/');
	if (!ptr)
		return;
	ptr++;
	if (*ptr == '\0')
		return;

	char *name;
	if (asprintf(&name, "%s/%s", dest_dir, ptr) == -1)
		errExit("asprintf");
	if (stat(name, &s) == 0) {
		free(name);
		return;
	}
	free(name);

	if (arg_debug || arg_debug_private_lib)
		printf("copying %s to private %s\n", full_path, dest_dir);

	sbox_run(SBOX_ROOT| SBOX_SECCOMP, 4, PATH_FCOPY, "--follow-link", full_path, dest_dir);
	report_duplication(full_path);
	lib_cnt++;
}


// requires full path for lib
// it could be a library or an executable
// lib is not copied, only libraries used by it
void fslib_copy_libs(const char *full_path) {
	assert(full_path);
	if (arg_debug || arg_debug_private_lib)
		printf("fslib_copy_libs %s\n", full_path);

	// if library/executable does not exist or the user does not have read access to it
	// print a warning and exit the function.
	if (access(full_path, R_OK)) {
		if (arg_debug || arg_debug_private_lib)
			printf("cannot find %s for private-lib, skipping...\n", full_path);
		return;
	}

	// create an empty RUN_LIB_FILE and allow the user to write to it
	unlink(RUN_LIB_FILE);			  // in case is there
	create_empty_file_as_root(RUN_LIB_FILE, 0644);
	if (chown(RUN_LIB_FILE, getuid(), getgid()))
		errExit("chown");

	// run fldd to extact the list of files
	if (arg_debug || arg_debug_private_lib)
		printf("running fldd %s\n", full_path);
	sbox_run(SBOX_USER | SBOX_SECCOMP | SBOX_CAPS_NONE, 3, PATH_FLDD, full_path, RUN_LIB_FILE);

	// open the list of libraries and install them on by one
	FILE *fp = fopen(RUN_LIB_FILE, "r");
	if (!fp)
		errExit("fopen");

	char buf[MAXBUF];
	while (fgets(buf, MAXBUF, fp)) {
		// remove \n
		char *ptr = strchr(buf, '\n');
		if (ptr)
			*ptr = '\0';
		fslib_duplicate(buf);
	}
	fclose(fp);
}


void fslib_copy_dir(const char *full_path) {
	assert(full_path);
	if (arg_debug || arg_debug_private_lib)
		printf("fslib_copy_dir %s\n", full_path);

	// do nothing if the directory does not exist or is not owned by root
	struct stat s;
	if (stat(full_path, &s) != 0 || s.st_uid != 0 || !S_ISDIR(s.st_mode) || access(full_path, R_OK))
		return;

	char *dir_name = strrchr(full_path, '/');
	assert(dir_name);
	dir_name++;
	assert(*dir_name != '\0');

	// do nothing if the directory is already there
	char *dest;
	if (asprintf(&dest, "%s/%s", build_dest_dir(full_path), dir_name) == -1)
		errExit("asprintf");
	if (stat(dest, &s) == 0) {
		free(dest);
		return;
	}

	// create new directory and mount the original on top of it
	mkdir_attr(dest, 0755, 0, 0);

	if (mount(full_path, dest, NULL, MS_BIND|MS_REC, NULL) < 0 ||
		mount(NULL, dest, NULL, MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV|MS_REC, NULL) < 0)
		errExit("mount bind");
	fs_logger2("clone", full_path);
	fs_logger2("mount", full_path);
	dir_cnt++;
	free(dest);
}


// return 1 if the file is valid
static char *valid_file(const char *lib) {
	// filename check
	int len = strlen(lib);
	if (strcspn(lib, "\\&!?\"'<>%^(){}[];,*") != (size_t)len ||
	strstr(lib, "..")) {
		fprintf(stderr, "Error: \"%s\" is an invalid library\n", lib);
		exit(1);
	}

	// find the library
	int i;
	for (i = 0; lib_paths[i]; i++) {
		char *fname;
		if (asprintf(&fname, "%s/%s", lib_paths[i], lib) == -1)
			errExit("asprintf");

		// existing file owned by root, read access
		struct stat s;
		if (stat(fname, &s) == 0 && s.st_uid == 0 && !access(fname, R_OK)) {
			return fname;
		}
		free(fname);
	}

	fwarning("%s library not found, skipping...\n", lib);
	return NULL;
}


static void mount_directories(void) {
	if (arg_debug || arg_debug_private_lib)
		printf("Mount-bind %s on top of /lib /lib64 /usr/lib\n", RUN_LIB_DIR);

	if (is_dir("/lib")) {
		if (mount(RUN_LIB_DIR, "/lib", NULL, MS_BIND|MS_REC, NULL) < 0 ||
			mount(NULL, "/lib", NULL, MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV|MS_REC, NULL) < 0)
			errExit("mount bind");
		fs_logger2("tmpfs", "/lib");
		fs_logger("mount /lib");
	}

	if (is_dir("/lib64")) {
		if (mount(RUN_LIB_DIR, "/lib64", NULL, MS_BIND|MS_REC, NULL) < 0 ||
			mount(NULL, "/lib64", NULL, MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV|MS_REC, NULL) < 0)
			errExit("mount bind");
		fs_logger2("tmpfs", "/lib64");
		fs_logger("mount /lib64");
	}

	if (is_dir("/usr/lib")) {
		if (mount(RUN_LIB_DIR, "/usr/lib", NULL, MS_BIND|MS_REC, NULL) < 0 ||
			mount(NULL, "/usr/lib", NULL, MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV|MS_REC, NULL) < 0)
			errExit("mount bind");
		fs_logger2("tmpfs", "/usr/lib");
		fs_logger("mount /usr/lib");
	}

	// for amd64 only - we'll deal with i386 later
	if (is_dir("/lib32")) {
		if (mount(RUN_RO_DIR, "/lib32", "none", MS_BIND, "mode=400,gid=0") < 0)
			errExit("disable file");
		fs_logger("blacklist-nolog /lib32");
	}
	if (is_dir("/libx32")) {
		if (mount(RUN_RO_DIR, "/libx32", "none", MS_BIND, "mode=400,gid=0") < 0)
			errExit("disable file");
		fs_logger("blacklist-nolog /libx32");
	}
}

void fs_private_lib(void) {
#ifndef __x86_64__
	fwarning("private-lib feature is currently available only on amd64 platforms\n");
	return;
#endif
	char *private_list = cfg.lib_private_keep;
	if (arg_debug || arg_debug_private_lib)
		printf("Starting private-lib processing: program %s, shell %s\n",
			(cfg.original_program_index > 0)? cfg.original_argv[cfg.original_program_index]: "none",
		(arg_shell_none)? "none": cfg.shell);

	// create /run/firejail/mnt/lib directory
	mkdir_attr(RUN_LIB_DIR, 0755, 0, 0);

	// install standard C libraries
	fslib_install_stdc();

	timetrace_start();

	// copy the libs in the new lib directory for the main exe
	if (cfg.original_program_index > 0)
		fslib_copy_libs(cfg.original_argv[cfg.original_program_index]);

	// for the shell
	if (!arg_shell_none) {
		fslib_copy_libs(cfg.shell);
		// a shell is useless without ls command
		fslib_copy_libs("/bin/ls");
	}

	// for the listed libs
	if (private_list && *private_list != '\0') {
		if (arg_debug || arg_debug_private_lib)
			printf("Copying extra files (%s) in the new lib directory\n", private_list);

		char *dlist = strdup(private_list);
		if (!dlist)
			errExit("strdup");

		char *ptr = strtok(dlist, ",");
		char *lib = valid_file(ptr);
		if (lib) {
			if (is_dir(lib))
				fslib_copy_dir(lib);
			else
				fslib_copy_libs(lib);
			fslib_copy_libs(lib);
			free(lib);
		}

		while ((ptr = strtok(NULL, ",")) != NULL) {
			lib = valid_file(ptr);
			if (lib) {
				if (is_dir(lib))
					fslib_copy_dir(lib);
				else
					fslib_duplicate(lib);
				fslib_copy_libs(lib);
				free(lib);
			}
		}
		free(dlist);
		fs_logger_print();
	}

	// for private-bin files
	if (arg_private_bin) {
		FILE *fp = fopen(RUN_LIB_BIN, "r");
		if (fp) {
			char buf[MAXBUF];
			while (fgets(buf, MAXBUF, fp)) {
				// remove \n
				char *ptr = strchr(buf, '\n');
				if (ptr)
					*ptr = '\0';

				// copy libraries for this program
				fslib_copy_libs(buf);

				// load program data from /usr/lib/program or from /usr/lib/x86_64-linux-gnu
				ptr = strrchr(buf, '/');
				if (ptr && *(ptr + 1) != '\0') {
					ptr++;

					// /usr/lib/program
					char *name;
					if (asprintf(&name, "/usr/lib/%s", ptr) == -1)
						errExit("asprintf");
					if (is_dir(name)) {
						fslib_copy_dir(name);
						fslib_copy_libs(name);
					}
					free(name);

					// /usr/lib/x86_linux-gnu - debian & frriends
					if (asprintf(&name, "/usr/lib/x86_64-linux-gnu/%s", ptr) == -1)
						errExit("asprintf");
					if (is_dir(name)) {
						fslib_copy_dir(name);
						fslib_copy_libs(name);
					}
					free(name);

					// /usr/lib64 - CentOS, Fedora
					if (asprintf(&name, "/usr/lib64/%s", ptr) == -1)
						errExit("asprintf");
					if (is_dir(name)) {
						fslib_copy_dir(name);
						fslib_copy_libs(name);
					}
					free(name);
				}
			}
		}
		fclose(fp);
	}
	fmessage("Program libraries installed in %0.2f ms\n", timetrace_end());

	// install the reset of the system libraries
	fslib_install_system();

	fmessage("Installed %d libraries and %d directories\n", lib_cnt, dir_cnt);

	// bring in firejail directory for --trace options
	fslib_copy_dir(LIBDIR "/firejail");

	// ... and for sandbox in sandbox functionality
	fslib_copy_libs(LIBDIR "/firejail/faudit");
	fslib_copy_libs(LIBDIR "/firejail/fbuilder");
	fslib_copy_libs(LIBDIR "/firejail/fcopy");
	fslib_copy_libs(LIBDIR "/firejail/fldd");
	fslib_copy_libs(LIBDIR "/firejail/fnet");
	fslib_copy_libs(LIBDIR "/firejail/fnetfilter");
	fslib_copy_libs(LIBDIR "/firejail/fseccomp");
	fslib_copy_libs(LIBDIR "/firejail/ftee");
	// mount lib filesystem
	mount_directories();
}
