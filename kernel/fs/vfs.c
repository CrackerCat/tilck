/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>

#include <tilck/kernel/fs/vfs.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/kernel/errno.h>
#include <tilck/kernel/process.h>

#include "fs_int.h"

static u32 next_device_id;

/*
 * Returns:
 *  - 0 in case of non match.
 *  - strlen(mp) in case of a match
 */
STATIC u32
check_mountpoint_match(const char *mp, u32 lm, const char *path, u32 lp)
{
   u32 m = 0;
   const u32 min_len = MIN(lm, lp);

   /*
    * Mount points MUST end with '/'.
    */
   ASSERT(mp[lm-1] == '/');

   for (size_t i = 0; i < min_len; i++) {

      if (mp[i] != path[i])
         break;

      m++;
   }

   /*
    * We assume that both the paths are absolute. Therefore, at least the
    * initial '/' must match.
    */
   ASSERT(m > 0);

   if (mp[m]) {

      if (mp[m] == '/' && !mp[m + 1] && !path[m]) {
         /* path is like '/dev' while mp is like '/dev/' */
         return m;
      }

      /*
       * The match stopped before the end of mount point's path.
       * Therefore, there is no match.
       */
      return 0;
   }

   if (path[m-1] != '/' && path[m-1] != 0) {

      /*
       * The match stopped before the end of a path component in 'path'.
       * In positive cases, the next character after a match (= position 'm')
       * is either a '/' or \0.
       */

      return 0;
   }

   return m;
}

void vfs_file_nolock(fs_handle h)
{
   /* do nothing */
}

int vfs_open(const char *path, fs_handle *out, int flags, mode_t mode)
{
   mountpoint *mp, *best_match = NULL;
   u32 pl, best_match_len = 0;
   const char *fs_path;
   mp_cursor cur;
   int rc;

   ASSERT(path != NULL);

   if (*path != '/')
      panic("vfs_open() works only with absolute paths");

   pl = (u32)strlen(path);
   mountpoint_iter_begin(&cur);

   while ((mp = mountpoint_get_next(&cur))) {

      u32 len = check_mountpoint_match(mp->path, mp->path_len, path, pl);

      if (len > best_match_len) {
         best_match = mp;
         best_match_len = len;
      }
   }

   if (!best_match) {
      rc = -ENOENT;
      goto out;
   }

   filesystem *fs = best_match->fs;
   fs_path = (best_match_len < pl) ? path + best_match_len - 1 : "/";

   /*
    * NOTE: we really DO NOT need to lock the whole FS in order to open/create
    * a file. At most, the directory where the file is/will be.
    *
    * TODO: make open() to NOT lock the whole FS.
    */
   if (flags & O_CREAT) {
      vfs_fs_exlock(fs);
      rc = fs->open(fs, fs_path, out, flags, mode);
      vfs_fs_exunlock(fs);
   } else {
      vfs_fs_shlock(fs);
      rc = fs->open(fs, fs_path, out, flags, mode);
      vfs_fs_shunlock(fs);
   }

   if (rc == 0) {
      fs->ref_count++;
   }

out:
   mountpoint_iter_end(&cur);
   return rc;
}

void vfs_close(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   filesystem *fs = hb->fs;

#ifndef UNIT_TEST_ENVIRONMENT
   process_info *pi = get_curr_task()->pi;
   remove_all_mappings_of_handle(pi, h);
#endif

   hb->fs->close(h);

   fs->ref_count--;

   /* while a filesystem is mounted, the minimum ref-count it can have is 1 */
   ASSERT(fs->ref_count > 0);
}

int vfs_dup(fs_handle h, fs_handle *dup_h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   int rc;

   if (!hb)
      return -EBADF;

   if ((rc = hb->fs->dup(h, dup_h)))
      return rc;

   hb->fs->ref_count++;
   ASSERT(*dup_h != NULL);
   return 0;
}

ssize_t vfs_read(fs_handle h, void *buf, size_t buf_size)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   ssize_t ret;

   if (!hb->fops.read)
      return -EINVAL;

   vfs_shlock(h);
   {
      ret = hb->fops.read(h, buf, buf_size);
   }
   vfs_shunlock(h);
   return ret;
}

ssize_t vfs_write(fs_handle h, void *buf, size_t buf_size)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   ssize_t ret;

   if (!hb->fops.write)
      return -EINVAL;

   vfs_exlock(h);
   {
      ret = hb->fops.write(h, buf, buf_size);
   }
   vfs_exunlock(h);
   return ret;
}

off_t vfs_seek(fs_handle h, s64 off, int whence)
{
   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops.seek)
      return -ESPIPE;

   // NOTE: this won't really work for big offsets in case off_t is 32-bit.
   return hb->fops.seek(h, (off_t) off, whence);
}

int vfs_ioctl(fs_handle h, uptr request, void *argp)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   int ret;

   if (!hb->fops.ioctl)
      return -ENOTTY; // Yes, ENOTTY *IS* the right error. See the man page.

   vfs_exlock(h);
   {
      ret = hb->fops.ioctl(h, request, argp);
   }
   vfs_exunlock(h);
   return ret;
}

int vfs_stat64(fs_handle h, struct stat64 *statbuf)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   int ret;

   ASSERT(hb->fops.stat != NULL); /* stat is NOT optional */

   vfs_shlock(h);
   {
      ret = hb->fops.stat(h, statbuf);
   }
   vfs_shunlock(h);
   return ret;
}

void vfs_exlock(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   ASSERT(hb != NULL);

   if (hb->fops.exlock) {
      hb->fops.exlock(h);
   } else {
      ASSERT(!hb->fops.exunlock);
      vfs_fs_exlock(get_fs(h));
   }
}

void vfs_exunlock(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   ASSERT(hb != NULL);

   if (hb->fops.exunlock) {
      hb->fops.exunlock(h);
   } else {
      ASSERT(!hb->fops.exlock);
      vfs_fs_exunlock(get_fs(h));
   }
}

void vfs_shlock(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   ASSERT(hb != NULL);

   if (hb->fops.shlock) {
      hb->fops.shlock(h);
   } else {
      ASSERT(!hb->fops.shunlock);
      vfs_fs_shlock(get_fs(h));
   }
}

void vfs_shunlock(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   ASSERT(hb != NULL);

   if (hb->fops.shunlock) {
      hb->fops.shunlock(h);
   } else {
      ASSERT(!hb->fops.shlock);
      vfs_fs_shunlock(get_fs(h));
   }
}

void vfs_fs_exlock(filesystem *fs)
{
   ASSERT(fs != NULL);
   ASSERT(fs->fs_exlock);

   fs->fs_exlock(fs);
}

void vfs_fs_exunlock(filesystem *fs)
{
   ASSERT(fs != NULL);
   ASSERT(fs->fs_exunlock);

   fs->fs_exunlock(fs);
}

void vfs_fs_shlock(filesystem *fs)
{
   ASSERT(fs != NULL);
   ASSERT(fs->fs_shlock);

   fs->fs_shlock(fs);
}

void vfs_fs_shunlock(filesystem *fs)
{
   ASSERT(fs != NULL);
   ASSERT(fs->fs_shunlock);

   fs->fs_shunlock(fs);
}

int vfs_getdents64(fs_handle h, struct linux_dirent64 *user_dirp, u32 buf_size)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   int rc;

   ASSERT(hb != NULL);
   ASSERT(hb->fs->getdents64);

   vfs_fs_shlock(hb->fs);
   {
      // NOTE: the fs implementation MUST handle an invalid user 'dirp' pointer.
      rc = hb->fs->getdents64(h, user_dirp, buf_size);
   }
   vfs_fs_shunlock(hb->fs);
   return rc;
}

int vfs_fcntl(fs_handle h, int cmd, int arg)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   int ret;

   if (!hb->fops.fcntl)
      return -EINVAL;

   vfs_exlock(h);
   {
      ret = hb->fops.fcntl(h, cmd, arg);
   }
   vfs_exunlock(h);
   return ret;
}

u32 vfs_get_new_device_id(void)
{
   return next_device_id++;
}

bool vfs_read_ready(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   bool r;

   if (!hb->fops.read_ready)
      return true;

   vfs_shlock(h);
   {
      r = hb->fops.read_ready(h);
   }
   vfs_shunlock(h);
   return r;
}

bool vfs_write_ready(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   bool r;

   if (!hb->fops.write_ready)
      return true;

   vfs_shlock(h);
   {
      r = hb->fops.write_ready(h);
   }
   vfs_shunlock(h);
   return r;
}

bool vfs_except_ready(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   bool r;

   if (!hb->fops.except_ready)
      return false;

   vfs_shlock(h);
   {
      r = hb->fops.except_ready(h);
   }
   vfs_shunlock(h);
   return r;
}

kcond *vfs_get_rready_cond(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops.get_rready_cond)
      return NULL;

   return hb->fops.get_rready_cond(h);
}

kcond *vfs_get_wready_cond(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops.get_wready_cond)
      return NULL;

   return hb->fops.get_wready_cond(h);
}

kcond *vfs_get_except_cond(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops.get_except_cond)
      return NULL;

   return hb->fops.get_except_cond(h);
}
