/* ----------------------------------------------------------------------- *
 *
 *   Copyright 1998-2008 H. Peter Anvin - All Rights Reserved
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 53 Temple Place Ste 330,
 *   Boston MA 02111-1307, USA; either version 2 of the License, or
 *   (at your option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/*
 * syslinux.c - Linux installer program for SYSLINUX
 *
 * This is Linux-specific by now.
 *
 * This is an alternate version of the installer which doesn't require
 * mtools, but requires root privilege.
 */

/*
 * If DO_DIRECT_MOUNT is 0, call mount(8)
 * If DO_DIRECT_MOUNT is 1, call mount(2)
 */
#ifdef __KLIBC__
# define DO_DIRECT_MOUNT 1
#else
# define DO_DIRECT_MOUNT 0	/* glibc has broken losetup ioctls */
#endif

#define _GNU_SOURCE
#define _XOPEN_SOURCE 500	/* For pread() pwrite() */
#define _FILE_OFFSET_BITS 64
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>

#include <sys/ioctl.h>
#include <linux/fs.h>		/* FIGETBSZ, FIBMAP */
#include <linux/msdos_fs.h>	/* FAT_IOCTL_SET_ATTRIBUTES */
#ifndef FAT_IOCTL_SET_ATTRIBUTES
# define FAT_IOCTL_SET_ATTRIBUTES _IOW('r', 0x11, uint32_t)
#endif
#undef SECTOR_SIZE
#undef SECTOR_SHIFT

#include <paths.h>
#ifndef _PATH_MOUNT
# define _PATH_MOUNT "/bin/mount"
#endif
#ifndef _PATH_UMOUNT
# define _PATH_UMOUNT "/bin/umount"
#endif
#ifndef _PATH_TMP
# define _PATH_TMP "/tmp/"
#endif

#include "syslinux.h"

#if DO_DIRECT_MOUNT
# include <linux/loop.h>
#endif

#include <getopt.h>
#include <sysexits.h>
#include "syslxcom.h"
#include "setadv.h"
#include "syslxopt.h" /* unified options */

extern const char *program;	/* Name of program */

pid_t mypid;
char *mntpath = NULL;		/* Path on which to mount */

/*
 * Image file
 */
#define boot_image	syslinux_ldlinux
#define boot_image_len  syslinux_ldlinux_len

#if DO_DIRECT_MOUNT
int loop_fd = -1;		/* Loop device */
#endif

void __attribute__ ((noreturn)) die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", program, msg);

#if DO_DIRECT_MOUNT
    if (loop_fd != -1) {
	ioctl(loop_fd, LOOP_CLR_FD, 0);	/* Free loop device */
	close(loop_fd);
	loop_fd = -1;
    }
#endif

    if (mntpath)
	unlink(mntpath);

    exit(1);
}

/*
 * Mount routine
 */
int do_mount(int dev_fd, int *cookie, const char *mntpath, const char *fstype)
{
    struct stat st;

    (void)cookie;

    if (fstat(dev_fd, &st) < 0)
	return errno;

#if DO_DIRECT_MOUNT
    {
	if (!S_ISBLK(st.st_mode)) {
	    /* It's file, need to mount it loopback */
	    unsigned int n = 0;
	    struct loop_info64 loopinfo;
	    int loop_fd;

	    for (n = 0; loop_fd < 0; n++) {
		snprintf(devfdname, sizeof devfdname, "/dev/loop%u", n);
		loop_fd = open(devfdname, O_RDWR);
		if (loop_fd < 0 && errno == ENOENT) {
		    die("no available loopback device!");
		}
		if (ioctl(loop_fd, LOOP_SET_FD, (void *)dev_fd)) {
		    close(loop_fd);
		    loop_fd = -1;
		    if (errno != EBUSY)
			die("cannot set up loopback device");
		    else
			continue;
		}

		if (ioctl(loop_fd, LOOP_GET_STATUS64, &loopinfo) ||
		    (loopinfo.lo_offset = opt.offset,
		     ioctl(loop_fd, LOOP_SET_STATUS64, &loopinfo)))
		    die("cannot set up loopback device");
	    }

	    *cookie = loop_fd;
	} else {
	    snprintf(devfdname, sizeof devfdname, "/proc/%lu/fd/%d",
		     (unsigned long)mypid, dev_fd);
	    *cookie = -1;
	}

	return mount(devfdname, mntpath, fstype,
		     MS_NOEXEC | MS_NOSUID, "umask=077,quiet");
    }
#else
    {
	char devfdname[128], mnt_opts[128];
	pid_t f, w;
	int status;

	snprintf(devfdname, sizeof devfdname, "/proc/%lu/fd/%d",
		 (unsigned long)mypid, dev_fd);

	f = fork();
	if (f < 0) {
	    return -1;
	} else if (f == 0) {
	    if (!S_ISBLK(st.st_mode)) {
		snprintf(mnt_opts, sizeof mnt_opts,
			 "rw,nodev,noexec,loop,offset=%llu,umask=077,quiet",
			 (unsigned long long)opt.offset);
	    } else {
		snprintf(mnt_opts, sizeof mnt_opts,
			 "rw,nodev,noexec,umask=077,quiet");
	    }
	    execl(_PATH_MOUNT, _PATH_MOUNT, "-t", fstype, "-o", mnt_opts,
		  devfdname, mntpath, NULL);
	    _exit(255);		/* execl failed */
	}

	w = waitpid(f, &status, 0);
	return (w != f || status) ? -1 : 0;
    }
#endif
}

/*
 * umount routine
 */
void do_umount(const char *mntpath, int cookie)
{
#if DO_DIRECT_MOUNT
    int loop_fd = cookie;

    if (umount2(mntpath, 0))
	die("could not umount path");

    if (loop_fd != -1) {
	ioctl(loop_fd, LOOP_CLR_FD, 0);	/* Free loop device */
	close(loop_fd);
	loop_fd = -1;
    }
#else
    pid_t f = fork();
    pid_t w;
    int status;
    (void)cookie;

    if (f < 0) {
	perror("fork");
	exit(1);
    } else if (f == 0) {
	execl(_PATH_UMOUNT, _PATH_UMOUNT, mntpath, NULL);
    }

    w = waitpid(f, &status, 0);
    if (w != f || status) {
	exit(1);
    }
#endif
}

/*
 * Make any user-specified ADV modifications
 */
int modify_adv(void)
{
    int rv = 0;

    if (opt.set_once) {
	if (syslinux_setadv(ADV_BOOTONCE, strlen(opt.set_once), opt.set_once)) {
	    fprintf(stderr, "%s: not enough space for boot-once command\n",
		    program);
	    rv = -1;
	}
    }

    return rv;
}

/*
 * Modify the ADV of an existing installation
 */
int modify_existing_adv(const char *path)
{
    if (opt.reset_adv)
	syslinux_reset_adv(syslinux_adv);
    else if (read_adv(path, "ldlinux.sys") < 0)
	return 1;

    if (modify_adv() < 0)
	return 1;

    if (write_adv(path, "ldlinux.sys") < 0)
	return 1;

    return 0;
}

int main(int argc, char *argv[])
{
    static unsigned char sectbuf[SECTOR_SIZE];
    int dev_fd, fd;
    struct stat st;
    int err = 0;
    char mntname[128];
    char *ldlinux_name;
    char *ldlinux_path;
    const char *subdir;
    uint32_t *sectors = NULL;
    int ldlinux_sectors = (boot_image_len + SECTOR_SIZE - 1) >> SECTOR_SHIFT;
    const char *errmsg;
    int mnt_cookie;
    int patch_sectors;
    int i;

    mypid = getpid();
    umask(077);
    parse_options(argc, argv, MODE_SYSLINUX);

    subdir = opt.directory;

    if (!opt.device)
	usage(EX_USAGE, MODE_SYSLINUX);

    /*
     * First make sure we can open the device at all, and that we have
     * read/write permission.
     */
    dev_fd = open(opt.device, O_RDWR);
    if (dev_fd < 0 || fstat(dev_fd, &st) < 0) {
	perror(opt.device);
	exit(1);
    }

    if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode)) {
	die("not a device or regular file");
    }

    if (opt.offset && S_ISBLK(st.st_mode)) {
	die("can't combine an offset with a block device");
    }

    fs_type = VFAT;
    xpread(dev_fd, sectbuf, SECTOR_SIZE, opt.offset);
    fsync(dev_fd);

    /*
     * Check to see that what we got was indeed an MS-DOS boot sector/superblock
     */
    if ((errmsg = syslinux_check_bootsect(sectbuf))) {
	fprintf(stderr, "%s: %s\n", opt.device, errmsg);
	exit(1);
    }

    /*
     * Now mount the device.
     */
    if (geteuid()) {
	die("This program needs root privilege");
    } else {
	int i = 0;
	struct stat dst;
	int rv;

	/* We're root or at least setuid.
	   Make a temp dir and pass all the gunky options to mount. */

	if (chdir(_PATH_TMP)) {
	    perror(program);
	    exit(1);
	}
#define TMP_MODE (S_IXUSR|S_IWUSR|S_IXGRP|S_IWGRP|S_IWOTH|S_IXOTH|S_ISVTX)

	if (stat(".", &dst) || !S_ISDIR(dst.st_mode) ||
	    (dst.st_mode & TMP_MODE) != TMP_MODE) {
	    die("possibly unsafe " _PATH_TMP " permissions");
	}

	for (i = 0;; i++) {
	    snprintf(mntname, sizeof mntname, "syslinux.mnt.%lu.%d",
		     (unsigned long)mypid, i);

	    if (lstat(mntname, &dst) != -1 || errno != ENOENT)
		continue;

	    rv = mkdir(mntname, 0000);

	    if (rv == -1) {
		if (errno == EEXIST || errno == EINTR)
		    continue;
		perror(program);
		exit(1);
	    }

	    if (lstat(mntname, &dst) || dst.st_mode != (S_IFDIR | 0000) ||
		dst.st_uid != 0) {
		die("someone is trying to symlink race us!");
	    }
	    break;		/* OK, got something... */
	}

	mntpath = mntname;
    }

    if (do_mount(dev_fd, &mnt_cookie, mntpath, "vfat") &&
	do_mount(dev_fd, &mnt_cookie, mntpath, "msdos")) {
	rmdir(mntpath);
	die("mount failed");
    }

    ldlinux_path = alloca(strlen(mntpath) +  (subdir ? strlen(subdir) + 2 : 0));
    sprintf(ldlinux_path, "%s%s%s",
	    mntpath, subdir ? "//" : "", subdir ? subdir : "");

    ldlinux_name = alloca(strlen(ldlinux_path) + 14);
    if (!ldlinux_name) {
	perror(program);
	err = 1;
	goto umount;
    }
    sprintf(ldlinux_name, "%s//ldlinux.sys", ldlinux_path);

    /* update ADV only ? */
    if (opt.update_only == -1) {
	if (opt.reset_adv || opt.set_once) {
	    modify_existing_adv(ldlinux_path);
	    do_umount(mntpath, mnt_cookie);
	    sync();
	    rmdir(mntpath);
	    exit(0);
	} else
	    usage(EX_USAGE, MODE_SYSLINUX);
    }

    /* Read a pre-existing ADV, if already installed */
    if (opt.reset_adv)
	syslinux_reset_adv(syslinux_adv);
    else if (read_adv(ldlinux_path, "ldlinux.sys") < 0)
	syslinux_reset_adv(syslinux_adv);
    if (modify_adv() < 0)
	exit(1);

    if ((fd = open(ldlinux_name, O_RDONLY)) >= 0) {
	uint32_t zero_attr = 0;
	ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &zero_attr);
	close(fd);
    }

    unlink(ldlinux_name);
    fd = open(ldlinux_name, O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd < 0) {
	perror(opt.device);
	err = 1;
	goto umount;
    }

    /* Write it the first time */
    if (xpwrite(fd, boot_image, boot_image_len, 0) != (int)boot_image_len ||
	xpwrite(fd, syslinux_adv, 2 * ADV_SIZE,
		boot_image_len) != 2 * ADV_SIZE) {
	fprintf(stderr, "%s: write failure on %s\n", program, ldlinux_name);
	exit(1);
    }

    fsync(fd);
    /*
     * Set the attributes
     */
    {
	uint32_t attr = 0x07;	/* Hidden+System+Readonly */
	ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attr);
    }

    /*
     * Create a block map.
     */
    ldlinux_sectors += 2; /* 2 ADV sectors */
    sectors = calloc(ldlinux_sectors, sizeof *sectors);
    if (sectmap(fd, sectors, ldlinux_sectors)) {
	perror("bmap");
	exit(1);
    }
    close(fd);
    sync();

umount:
    do_umount(mntpath, mnt_cookie);
    sync();
    rmdir(mntpath);

    if (err)
	exit(err);

    /*
     * Patch ldlinux.sys and the boot sector
     */
    i = syslinux_patch(sectors, ldlinux_sectors, opt.stupid_mode, opt.raid_mode, subdir);
    patch_sectors = (i + SECTOR_SIZE - 1) >> SECTOR_SHIFT;

    /*
     * Write the now-patched first sectors of ldlinux.sys
     */
    for (i = 0; i < patch_sectors; i++) {
	xpwrite(dev_fd, boot_image + i * SECTOR_SIZE, SECTOR_SIZE,
		opt.offset + ((off_t) sectors[i] << SECTOR_SHIFT));
    }

    /*
     * To finish up, write the boot sector
     */

    /* Read the superblock again since it might have changed while mounted */
    xpread(dev_fd, sectbuf, SECTOR_SIZE, opt.offset);

    /* Copy the syslinux code into the boot sector */
    syslinux_make_bootsect(sectbuf);

    /* Write new boot sector */
    xpwrite(dev_fd, sectbuf, SECTOR_SIZE, opt.offset);

    close(dev_fd);
    sync();

    /* Done! */

    return 0;
}
