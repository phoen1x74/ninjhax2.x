#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dirent.h>
#include <sys/iosupport.h>
#include <sys/param.h>
#include <unistd.h>

#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/sdmc.h>
#include <3ds/services/fs.h>
#include <3ds/util/utf.h>


/*! @internal
 *
 *  @file sdmc_dev.c
 *
 *  SDMC Device
 */

static int sdmc_translate_error(Result error);

static int       sdmc_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode);
static int       sdmc_close(struct _reent *r, int fd);
static ssize_t   sdmc_write(struct _reent *r, int fd, const char *ptr, size_t len);
static ssize_t   sdmc_write_safe(struct _reent *r, int fd, const char *ptr, size_t len);
static ssize_t   sdmc_read(struct _reent *r, int fd, char *ptr, size_t len);
static off_t     sdmc_seek(struct _reent *r, int fd, off_t pos, int dir);
static int       sdmc_fstat(struct _reent *r, int fd, struct stat *st);
static int       sdmc_stat(struct _reent *r, const char *file, struct stat *st);
static int       sdmc_link(struct _reent *r, const char *existing, const char  *newLink);
static int       sdmc_unlink(struct _reent *r, const char *name);
static int       sdmc_chdir(struct _reent *r, const char *name);
static int       sdmc_rename(struct _reent *r, const char *oldName, const char *newName);
static int       sdmc_mkdir(struct _reent *r, const char *path, int mode);
static DIR_ITER* sdmc_diropen(struct _reent *r, DIR_ITER *dirState, const char *path);
static int       sdmc_dirreset(struct _reent *r, DIR_ITER *dirState);
static int       sdmc_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
static int       sdmc_dirclose(struct _reent *r, DIR_ITER *dirState);
static int       sdmc_statvfs(struct _reent *r, const char *path, struct statvfs *buf);
static int       sdmc_ftruncate(struct _reent *r, int fd, off_t len);
static int       sdmc_fsync(struct _reent *r, int fd);
static int       sdmc_chmod(struct _reent *r, const char *path, mode_t mode);
static int       sdmc_fchmod(struct _reent *r, int fd, mode_t mode);
static int       sdmc_rmdir(struct _reent *r, const char *name);

/*! @cond INTERNAL */

/*! Open file struct */
typedef struct
{
  Handle fd;     /*! CTRU handle */
  int    flags;  /*! Flags used in open(2) */
  u64    offset; /*! Current file offset */
} sdmc_file_t;

/*! SDMC devoptab */
static devoptab_t
sdmc_devoptab =
{
  .name         = "sdmc",
  .structSize   = sizeof(sdmc_file_t),
  .open_r       = sdmc_open,
  .close_r      = sdmc_close,
  .write_r      = sdmc_write_safe,
  .read_r       = sdmc_read,
  .seek_r       = sdmc_seek,
  .fstat_r      = sdmc_fstat,
  .stat_r       = sdmc_stat,
  .link_r       = sdmc_link,
  .unlink_r     = sdmc_unlink,
  .chdir_r      = sdmc_chdir,
  .rename_r     = sdmc_rename,
  .mkdir_r      = sdmc_mkdir,
  .dirStateSize = sizeof(sdmc_dir_t),
  .diropen_r    = sdmc_diropen,
  .dirreset_r   = sdmc_dirreset,
  .dirnext_r    = sdmc_dirnext,
  .dirclose_r   = sdmc_dirclose,
  .statvfs_r    = sdmc_statvfs,
  .ftruncate_r  = sdmc_ftruncate,
  .fsync_r      = sdmc_fsync,
  .deviceData   = NULL,
  .chmod_r      = sdmc_chmod,
  .fchmod_r     = sdmc_fchmod,
  .rmdir_r      = sdmc_rmdir,
};

/*! SDMC archive handle */
static FS_Archive sdmcArchive;

/*! @endcond */

static char     __cwd[PATH_MAX+1] = "/";
static __thread char     __fixedpath[PATH_MAX+1];
static __thread uint16_t __utf16path[PATH_MAX+1];

static const char*
sdmc_fixpath(struct _reent *r,
             const char    *path)
{
  ssize_t       units;
  uint32_t      code;
  const uint8_t *p = (const uint8_t*)path;

  // Move the path pointer to the start of the actual path
  do
  {
    units = decode_utf8(&code, p);
    if(units < 0)
    {
      r->_errno = EILSEQ;
      return NULL;
    }

    p += units;
  } while(code != ':' && code != 0);

  // We found a colon; p points to the actual path
  if(code == ':')
    path = (const char*)p;

  // Make sure there are no more colons and that the
  // remainder of the filename is valid UTF-8
  p = (const uint8_t*)path;
  do
  {
    units = decode_utf8(&code, p);
    if(units < 0)
    {
      r->_errno = EILSEQ;
      return NULL;
    }

    if(code == ':')
    {
      r->_errno = EINVAL;
      return NULL;
    }

    p += units;
  } while(code != 0);

  if(path[0] == '/')
    strncpy(__fixedpath, path, PATH_MAX+1);
  else
  {
    strncpy(__fixedpath, __cwd, PATH_MAX+1);
    strncat(__fixedpath, path, PATH_MAX+1);
  }

  if(__fixedpath[PATH_MAX] != 0)
  {
    __fixedpath[PATH_MAX] = 0;
    r->_errno = ENAMETOOLONG;
    return NULL;
  }

  return __fixedpath;
}

static const FS_Path
sdmc_utf16path(struct _reent *r,
               const char    *path)
{
  ssize_t units;
  FS_Path fspath;

  fspath.data = NULL;

  if(sdmc_fixpath(r, path) == NULL)
    return fspath;

  units = utf8_to_utf16(__utf16path, (const uint8_t*)__fixedpath, PATH_MAX);
  if(units < 0)
  {
    r->_errno = EILSEQ;
    return fspath;
  }
  if(units >= PATH_MAX)
  {
    r->_errno = ENAMETOOLONG;
    return fspath;
  }

  __utf16path[units] = 0;

  fspath.type = PATH_UTF16;
  fspath.size = (units+1)*sizeof(uint16_t);
  fspath.data = (const u8*)__utf16path;

  return fspath;
}

extern int __system_argc;
extern char** __system_argv;

static bool sdmcInitialised = false;

/*! Initialize SDMC device */
Result sdmcInit(void)
{
  ssize_t  units;
  uint32_t code;
  char     *p;
  FS_Path sdmcPath = { PATH_EMPTY, 1, (u8*)"" };
  Result   rc = 0;

  if(sdmcInitialised)
    return rc;


  rc = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, sdmcPath);
  if(R_SUCCEEDED(rc))
  {
    fsExemptFromSession(sdmcArchive);

    int dev = AddDevice(&sdmc_devoptab);

    if(dev != -1)
    {
      setDefaultDevice(dev);
      if(__system_argc != 0 && __system_argv[0] != NULL)
      {
        if(FindDevice(__system_argv[0]) == dev)
        {
          strncpy(__fixedpath,__system_argv[0],PATH_MAX);
          if(__fixedpath[PATH_MAX] != 0)
          {
            __fixedpath[PATH_MAX] = 0;
          }
          else
          {
            char *last_slash = NULL;
            p = __fixedpath;
            do
            {
              units = decode_utf8(&code, (const uint8_t*)p);
              if(units < 0)
              {
                last_slash = NULL;
                break;
              }

              if(code == '/')
                last_slash = p;

              p += units;
            } while(code != 0);

            if(last_slash != NULL)
            {
              last_slash[0] = 0;
              chdir(__fixedpath);
            }
          }
        }
      }
    }
  }

  sdmcInitialised = true;

  return rc;
}

/*! Enable/disable safe sdmc_write
 *
 *  Safe sdmc_write is enabled by default. If it is disabled, you will be
 *  unable to write from read-only buffers.
 *
 *  @param[in] enable Whether to enable
 */
void sdmcWriteSafe(bool enable)
{
  if(enable)
    sdmc_devoptab.write_r = sdmc_write_safe;
  else
    sdmc_devoptab.write_r = sdmc_write;
}

/*! Clean up SDMC device */
Result sdmcExit(void)
{
  Result rc = 0;

  if(!sdmcInitialised) return rc;

  rc = FSUSER_CloseArchive(sdmcArchive);
  if(R_SUCCEEDED(rc))
  {
    fsUnexemptFromSession(sdmcArchive);
    RemoveDevice("sdmc:");
    sdmcInitialised = false;
  }

  return rc;
}

/*! Open a file
 *
 *  @param[in,out] r          newlib reentrancy struct
 *  @param[out]    fileStruct Pointer to file struct to fill in
 *  @param[in]     path       Path to open
 *  @param[in]     flags      Open flags from open(2)
 *  @param[in]     mode       Permissions to set on create
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_open(struct _reent *r,
          void          *fileStruct,
          const char    *path,
          int           flags,
          int           mode)
{
  Handle        fd;
  Result        rc;
  u32           sdmc_flags = 0;
  u32           attributes = 0;
  FS_Path       fs_path;

  fs_path = sdmc_utf16path(r, path);
  if(fs_path.data == NULL)
    return -1;

  /* get pointer to our data */
  sdmc_file_t *file = (sdmc_file_t*)fileStruct;

  /* check access mode */
  switch(flags & O_ACCMODE)
  {
    /* read-only: do not allow O_APPEND */
    case O_RDONLY:
      sdmc_flags |= FS_OPEN_READ;
      if(flags & O_APPEND)
      {
        r->_errno = EINVAL;
        return -1;
      }
      break;

    /* write-only */
    case O_WRONLY:
      sdmc_flags |= FS_OPEN_WRITE;
      break;

    /* read and write */
    case O_RDWR:
      sdmc_flags |= (FS_OPEN_READ | FS_OPEN_WRITE);
      break;

    /* an invalid option was supplied */
    default:
      r->_errno = EINVAL;
      return -1;
  }

  /* create file */
  if(flags & O_CREAT)
    sdmc_flags |= FS_OPEN_CREATE;

  /* Test O_EXCL. */
  if((flags & O_CREAT) && (flags & O_EXCL))
  {
    rc = FSUSER_CreateFile(sdmcArchive, fs_path, attributes, 0);
    if(R_FAILED(rc))
    {
      r->_errno = sdmc_translate_error(rc);
      return -1;
    }
  }

  /* set attributes */
  /*if(!(mode & S_IWUSR))
    attributes |= FS_ATTRIBUTE_READONLY;*/

  /* open the file */
  rc = FSUSER_OpenFile(&fd, sdmcArchive, fs_path,
                       sdmc_flags, attributes);
  if(R_SUCCEEDED(rc))
  {
    if((flags & O_ACCMODE) != O_RDONLY && (flags & O_TRUNC))
    {
      rc = FSFILE_SetSize(fd, 0);
      if(R_FAILED(rc))
      {
        FSFILE_Close(fd);
        r->_errno = sdmc_translate_error(rc);
        return -1;
      }
    }

    file->fd     = fd;
    file->flags  = (flags & (O_ACCMODE|O_APPEND|O_SYNC));
    file->offset = 0;
    return 0;
  }

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Close an open file
 *
 *  @param[in,out] r  newlib reentrancy struct
 *  @param[in]     fd Pointer to sdmc_file_t
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_close(struct _reent *r,
           int           fd)
{
  Result      rc;

  /* get pointer to our data */
  sdmc_file_t *file = (sdmc_file_t*)fd;

  rc = FSFILE_Close(file->fd);
  if(R_SUCCEEDED(rc))
    return 0;

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Write to an open file
 *
 *  @param[in,out] r   newlib reentrancy struct
 *  @param[in,out] fd  Pointer to sdmc_file_t
 *  @param[in]     ptr Pointer to data to write
 *  @param[in]     len Length of data to write
 *
 *  @returns number of bytes written
 *  @returns -1 for error
 */
static ssize_t
sdmc_write(struct _reent *r,
           int           fd,
           const char    *ptr,
           size_t        len)
{
  Result      rc;
  u32         bytes;
  u32         sync = 0;

  /* get pointer to our data */
  sdmc_file_t *file = (sdmc_file_t*)fd;

  /* check that the file was opened with write access */
  if((file->flags & O_ACCMODE) == O_RDONLY)
  {
    r->_errno = EBADF;
    return -1;
  }

  /* check if this is synchronous or not */
  if(file->flags & O_SYNC)
    sync = FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME;

  if(file->flags & O_APPEND)
  {
    /* append means write from the end of the file */
    rc = FSFILE_GetSize(file->fd, &file->offset);
    if(R_FAILED(rc))
    {
      r->_errno = sdmc_translate_error(rc);
      return -1;
    }
  }

  rc = FSFILE_Write(file->fd, &bytes, file->offset,
                    (u32*)ptr, len, sync);
  if(R_FAILED(rc))
  {
    r->_errno = sdmc_translate_error(rc);
    return -1;
  }

  file->offset += bytes;

  return bytes;
}

/*! Write to an open file
 *
 *  @param[in,out] r   newlib reentrancy struct
 *  @param[in,out] fd  Pointer to sdmc_file_t
 *  @param[in]     ptr Pointer to data to write
 *  @param[in]     len Length of data to write
 *
 *  @returns number of bytes written
 *  @returns -1 for error
 */
static ssize_t
sdmc_write_safe(struct _reent *r,
                int           fd,
                const char    *ptr,
                size_t        len)
{
  Result      rc;
  u32         bytes, bytesWritten = 0;
  u32         sync = 0;

  /* get pointer to our data */
  sdmc_file_t *file = (sdmc_file_t*)fd;

  /* check that the file was opened with write access */
  if((file->flags & O_ACCMODE) == O_RDONLY)
  {
    r->_errno = EBADF;
    return -1;
  }

  /* check if this is synchronous or not */
  if(file->flags & O_SYNC)
    sync = FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME;

  if(file->flags & O_APPEND)
  {
    /* append means write from the end of the file */
    rc = FSFILE_GetSize(file->fd, &file->offset);
    if(R_FAILED(rc))
    {
      r->_errno = sdmc_translate_error(rc);
      return -1;
    }
  }

  /* Copy to internal buffer and write in chunks.
   * You cannot write from read-only memory.
   */
  static __thread char tmp_buffer[8192];
  while(len > 0)
  {
    size_t toWrite = len;
    if(toWrite > sizeof(tmp_buffer))
      toWrite = sizeof(tmp_buffer);

    /* copy to internal buffer */
    memcpy(tmp_buffer, ptr, toWrite);

    /* write the data */
    rc = FSFILE_Write(file->fd, &bytes, file->offset,
                      (u32*)tmp_buffer, (u32)toWrite, sync);
    if(R_FAILED(rc))
    {
      /* return partial transfer */
      if(bytesWritten > 0)
        return bytesWritten;

      r->_errno = sdmc_translate_error(rc);
      return -1;
    }

    file->offset += bytes;
    bytesWritten += bytes;
    ptr          += bytes;
    len          -= bytes;
  }

  return bytesWritten;
}

/*! Read from an open file
 *
 *  @param[in,out] r   newlib reentrancy struct
 *  @param[in,out] fd  Pointer to sdmc_file_t
 *  @param[out]    ptr Pointer to buffer to read into
 *  @param[in]     len Length of data to read
 *
 *  @returns number of bytes read
 *  @returns -1 for error
 */
static ssize_t
sdmc_read(struct _reent *r,
          int           fd,
          char          *ptr,
          size_t         len)
{
  Result      rc;
  u32         bytes;

  /* get pointer to our data */
  sdmc_file_t *file = (sdmc_file_t*)fd;

  /* check that the file was opened with read access */
  if((file->flags & O_ACCMODE) == O_WRONLY)
  {
    r->_errno = EBADF;
    return -1;
  }

  /* read the data */
  rc = FSFILE_Read(file->fd, &bytes, file->offset, (u32*)ptr, (u32)len);
  if(R_SUCCEEDED(rc))
  {
    /* update current file offset */
    file->offset += bytes;
    return (ssize_t)bytes;
  }

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Update an open file's current offset
 *
 *  @param[in,out] r      newlib reentrancy struct
 *  @param[in,out] fd     Pointer to sdmc_file_t
 *  @param[in]     pos    Offset to seek to
 *  @param[in]     whence Where to seek from
 *
 *  @returns new offset for success
 *  @returns -1 for error
 */
static off_t
sdmc_seek(struct _reent *r,
          int           fd,
          off_t         pos,
          int           whence)
{
  Result      rc;
  u64         offset;

  /* get pointer to our data */
  sdmc_file_t *file = (sdmc_file_t*)fd;

  /* find the offset to see from */
  switch(whence)
  {
    /* set absolute position; start offset is 0 */
    case SEEK_SET:
      offset = 0;
      break;

    /* set position relative to the current position */
    case SEEK_CUR:
      offset = file->offset;
      break;

    /* set position relative to the end of the file */
    case SEEK_END:
      rc = FSFILE_GetSize(file->fd, &offset);
      if(R_FAILED(rc))
      {
        r->_errno = sdmc_translate_error(rc);
        return -1;
      }
      break;

    /* an invalid option was provided */
    default:
      r->_errno = EINVAL;
      return -1;
  }

  /* TODO: A better check that prevents overflow. */
  if(pos < 0 && offset < -pos)
  {
    /* don't allow seek to before the beginning of the file */
    r->_errno = EINVAL;
    return -1;
  }

  /* update the current offset */
  file->offset = offset + pos;
  return file->offset;
}

/*! Get file stats from an open file
 *
 *  @param[in,out] r  newlib reentrancy struct
 *  @param[in]     fd Pointer to sdmc_file_t
 *  @param[out]    st Pointer to file stats to fill
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_fstat(struct _reent *r,
           int           fd,
           struct stat   *st)
{
  Result      rc;
  u64         size;
  sdmc_file_t *file = (sdmc_file_t*)fd;

  rc = FSFILE_GetSize(file->fd, &size);
  if(R_SUCCEEDED(rc))
  {
    memset(st, 0, sizeof(struct stat));
    st->st_size = (off_t)size;
    st->st_nlink = 1;
    st->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    return 0;
  }

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Get file stats
 *
 *  @param[in,out] r    newlib reentrancy struct
 *  @param[in]     file Path to file
 *  @param[out]    st   Pointer to file stats to fill
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_stat(struct _reent *r,
          const char    *file,
          struct stat   *st)
{
  Handle  fd;
  Result  rc;
  FS_Path fs_path;

  fs_path = sdmc_utf16path(r, file);
  if(fs_path.data == NULL)
    return -1;

  if(R_SUCCEEDED(rc = FSUSER_OpenFile(&fd, sdmcArchive, fs_path, FS_OPEN_READ, 0)))
  {
    sdmc_file_t tmpfd = { .fd = fd };
    rc = sdmc_fstat(r, (int)&tmpfd, st);
    FSFILE_Close(fd);

    return rc;
  }
  else if(R_SUCCEEDED(rc = FSUSER_OpenDirectory(&fd, sdmcArchive, fs_path)))
  {
    memset(st, 0, sizeof(struct stat));
    st->st_nlink = 1;
    st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
    FSDIR_Close(fd);
    return 0;
  }

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Hard link a file
 *
 *  @param[in,out] r        newlib reentrancy struct
 *  @param[in]     existing Path of file to link
 *  @param[in]     newLink  Path of new link
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_link(struct _reent *r,
          const char    *existing,
          const char    *newLink)
{
  r->_errno = ENOSYS;
  return -1;
}

/*! Unlink a file
 *
 *  @param[in,out] r    newlib reentrancy struct
 *  @param[in]     name Path of file to unlink
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_unlink(struct _reent *r,
            const char    *name)
{
  Result  rc;
  FS_Path fs_path;

  fs_path = sdmc_utf16path(r, name);
  if(fs_path.data == NULL)
    return -1;

  rc = FSUSER_DeleteFile(sdmcArchive, fs_path);
  if(R_SUCCEEDED(rc))
    return 0;

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Change current working directory
 *
 *  @param[in,out] r    newlib reentrancy struct
 *  @param[in]     name Path to new working directory
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_chdir(struct _reent *r,
           const char    *name)
{
  Handle  fd;
  Result  rc;
  FS_Path fs_path;

  fs_path = sdmc_utf16path(r, name);
  if(fs_path.data == NULL)
    return -1;

  rc = FSUSER_OpenDirectory(&fd, sdmcArchive, fs_path);
  if(R_SUCCEEDED(rc))
  {
    FSDIR_Close(fd);
    strncpy(__cwd, __fixedpath, PATH_MAX);
    return 0;
  }

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Rename a file
 *
 *  @param[in,out] r       newlib reentrancy struct
 *  @param[in]     oldName Path to rename from
 *  @param[in]     newName Path to rename to
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_rename(struct _reent *r,
            const char    *oldName,
            const char    *newName)
{
  Result  rc;
  FS_Path fs_path_old, fs_path_new;
  static __thread uint16_t __utf16path_old[PATH_MAX+1];

  fs_path_old = sdmc_utf16path(r, oldName);
  if(fs_path_old.data == NULL)
    return -1;

  memcpy(__utf16path_old, __utf16path, sizeof(__utf16path));
  fs_path_old.data = (const u8*)__utf16path_old;

  fs_path_new = sdmc_utf16path(r, newName);
  if(fs_path_new.data == NULL)
    return -1;

  rc = FSUSER_RenameFile(sdmcArchive, fs_path_old, sdmcArchive, fs_path_new);
  if(R_SUCCEEDED(rc))
    return 0;

  rc = FSUSER_RenameDirectory(sdmcArchive, fs_path_old, sdmcArchive, fs_path_new);
  if(R_SUCCEEDED(rc))
    return 0;

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Create a directory
 *
 *  @param[in,out] r    newlib reentrancy struct
 *  @param[in]     path Path of directory to create
 *  @param[in]     mode Permissions of created directory
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_mkdir(struct _reent *r,
           const char    *path,
           int           mode)
{
  Result  rc;
  FS_Path fs_path;

  fs_path = sdmc_utf16path(r, path);
  if(fs_path.data == NULL)
    return -1;

  /* TODO: Use mode to set directory attributes. */

  rc = FSUSER_CreateDirectory(sdmcArchive, fs_path, 0);
  if(rc == 0xC82044BE)
  {
    r->_errno = EEXIST;
    return -1;
  }
  else if(R_SUCCEEDED(rc))
    return 0;

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Open a directory
 *
 *  @param[in,out] r        newlib reentrancy struct
 *  @param[in]     dirState Pointer to open directory state
 *  @param[in]     path     Path of directory to open
 *
 *  @returns dirState for success
 *  @returns NULL for error
 */
static DIR_ITER*
sdmc_diropen(struct _reent *r,
             DIR_ITER      *dirState,
             const char    *path)
{
  Handle  fd;
  Result  rc;
  FS_Path fs_path;

  fs_path = sdmc_utf16path(r, path);

  if(fs_path.data == NULL)
    return NULL;

  /* get pointer to our data */
  sdmc_dir_t *dir = (sdmc_dir_t*)(dirState->dirStruct);

  /* open the directory */
  rc = FSUSER_OpenDirectory(&fd, sdmcArchive, fs_path);
  if(R_SUCCEEDED(rc))
  {
    dir->magic = SDMC_DIRITER_MAGIC;
    dir->fd    = fd;
    dir->index = -1;
    dir->size  = 0;
    memset(&dir->entry_data, 0, sizeof(dir->entry_data));
    return dirState;
  }

  r->_errno = sdmc_translate_error(rc);
  return NULL;
}

/*! Reset an open directory to its intial state
 *
 *  @param[in,out] r        newlib reentrancy struct
 *  @param[in]     dirState Pointer to open directory state
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_dirreset(struct _reent *r,
              DIR_ITER      *dirState)
{
  r->_errno = ENOSYS;
  return -1;
}

/*! Fetch the next entry of an open directory
 *
 *  @param[in,out] r        newlib reentrancy struct
 *  @param[in]     dirState Pointer to open directory state
 *  @param[out]    filename Buffer to store entry name
 *  @param[out]    filestat Buffer to store entry attributes
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_dirnext(struct _reent *r,
             DIR_ITER      *dirState,
             char          *filename,
             struct stat   *filestat)
{
  Result              rc;
  u32                 entries;
  ssize_t             units;
  FS_DirectoryEntry   *entry;

  /* get pointer to our data */
  sdmc_dir_t *dir = (sdmc_dir_t*)(dirState->dirStruct);

  static const size_t max_entries = sizeof(dir->entry_data) / sizeof(dir->entry_data[0]);

  /* check if it's in the batch already */
  if(++dir->index < dir->size)
  {
    rc = 0;
  }
  else
  {
    /* reset batch info */
    dir->index = -1;
    dir->size  = 0;

    /* fetch the next batch */
    memset(dir->entry_data, 0, sizeof(dir->entry_data));
    rc = FSDIR_Read(dir->fd, &entries, max_entries, dir->entry_data);
    if(R_SUCCEEDED(rc))
    {
      if(entries == 0)
      {
        /* there are no more entries; ENOENT signals end-of-directory */
        r->_errno = ENOENT;
        return -1;
      }

      dir->index = 0;
      dir->size  = entries;
    }
  }

  if(R_SUCCEEDED(rc))
  {
    entry = &dir->entry_data[dir->index];

    /* fill in the stat info */
    filestat->st_ino = 0;
    if(entry->attributes & FS_ATTRIBUTE_DIRECTORY)
      filestat->st_mode = S_IFDIR;
    else
      filestat->st_mode = S_IFREG;

    /* convert name from UTF-16 to UTF-8 */
    memset(filename, 0, NAME_MAX);
    units = utf16_to_utf8((uint8_t*)filename, entry->name, NAME_MAX);
    if(units < 0)
    {
      r->_errno = EILSEQ;
      return -1;
    }

    if(units >= NAME_MAX)
    {
      r->_errno = ENAMETOOLONG;
      return -1;
    }

    return 0;
  }

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Close an open directory
 *
 *  @param[in,out] r        newlib reentrancy struct
 *  @param[in]     dirState Pointer to open directory state
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_dirclose(struct _reent *r,
              DIR_ITER      *dirState)
{
  Result         rc;

  /* get pointer to our data */
  sdmc_dir_t *dir = (sdmc_dir_t*)(dirState->dirStruct);

  /* close the directory */
  rc = FSDIR_Close(dir->fd);
  if(R_SUCCEEDED(rc))
    return 0;

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Get filesystem statistics
 *
 *  @param[in,out] r    newlib reentrancy struct
 *  @param[in]     path Path to filesystem to get statistics of
 *  @param[out]    buf  Buffer to fill
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_statvfs(struct _reent  *r,
             const char     *path,
             struct statvfs *buf)
{
  Result rc;
  FS_ArchiveResource resource;
  bool writable = false;

  rc = FSUSER_GetSdmcArchiveResource(&resource);

  if(R_SUCCEEDED(rc))
  {
    buf->f_bsize   = resource.clusterSize;
    buf->f_frsize  = resource.clusterSize;
    buf->f_blocks  = resource.totalClusters;
    buf->f_bfree   = resource.freeClusters;
    buf->f_bavail  = resource.freeClusters;
    buf->f_files   = 0; //??? how to get
    buf->f_ffree   = resource.freeClusters;
    buf->f_favail  = resource.freeClusters;
    buf->f_fsid    = 0; //??? how to get
    buf->f_flag    = ST_NOSUID;
    buf->f_namemax = 0; //??? how to get

    rc = FSUSER_IsSdmcWritable(&writable);
    if(R_FAILED(rc) || !writable)
      buf->f_flag |= ST_RDONLY;

    return 0;
  }

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Truncate an open file
 *
 *  @param[in,out] r   newlib reentrancy struct
 *  @param[in]     fd  Pointer to sdmc_file_t
 *  @param[in]     len Length to truncate file to
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_ftruncate(struct _reent *r,
               int           fd,
               off_t         len)
{
  Result      rc;

  /* get pointer to our data */
  sdmc_file_t *file = (sdmc_file_t*)fd;

  /* make sure length is non-negative */
  if(len < 0)
  {
    r->_errno = EINVAL;
    return -1;
  }

  /* set the new file size */
  rc = FSFILE_SetSize(file->fd, len);
  if(R_SUCCEEDED(rc))
    return 0;

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Synchronize a file to media
 *
 *  @param[in,out] r  newlib reentrancy struct
 *  @param[in]     fd Pointer to sdmc_file_t
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_fsync(struct _reent *r,
           int           fd)
{
  Result rc;

  /* get pointer to our data */
  sdmc_file_t *file = (sdmc_file_t*)fd;

  rc = FSFILE_Flush(file->fd);
  if(R_SUCCEEDED(rc))
    return 0;

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

/*! Change a file's mode
 *
 *  @param[in,out] r    newlib reentrancy struct
 *  @param[in]     path Path to file to update
 *  @param[in]     mode New mode to set
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_chmod(struct _reent *r,
          const char    *path,
          mode_t        mode)
{
  r->_errno = ENOSYS;
  return -1;
}

/*! Change an open file's mode
 *
 *  @param[in,out] r    newlib reentrancy struct
 *  @param[in]     fd   Pointer to sdmc_file_t
 *  @param[in]     mode New mode to set
 *
 *  @returns 0 for success
 *  @returns -1 for failure
 */
static int
sdmc_fchmod(struct _reent *r,
            int           fd,
            mode_t        mode)
{
  r->_errno = ENOSYS;
  return -1;
}

/*! Remove a directory
 *
 *  @param[in,out] r    newlib reentrancy struct
 *  @param[in]     name Path of directory to remove
 *
 *  @returns 0 for success
 *  @returns -1 for error
 */
static int
sdmc_rmdir(struct _reent *r,
           const char    *name)
{
  Result  rc;
  FS_Path fs_path;

  fs_path = sdmc_utf16path(r, name);
  if(fs_path.data == NULL)
    return -1;

  rc = FSUSER_DeleteDirectory(sdmcArchive, fs_path);
  if(R_SUCCEEDED(rc))
    return 0;

  r->_errno = sdmc_translate_error(rc);
  return -1;
}

Result
sdmc_getmtime(const char *name,
              u64        *mtime)
{
  Result        rc;
  FS_Path       fs_path;
  struct _reent r;

  r._errno = 0;

  fs_path = sdmc_utf16path(&r, name);
  if(r._errno != 0)
    errno = r._errno;

  if(fs_path.data == NULL)
    return -1;

  rc = FSUSER_ControlArchive(sdmcArchive, ARCHIVE_ACTION_GET_TIMESTAMP,
                             (void*)fs_path.data, fs_path.size,
                             mtime, sizeof(*mtime));
  if(rc == 0)
  {
    /* convert from milliseconds to seconds */
    *mtime /= 1000;
    /* convert from 2000-based timestamp to UNIX timestamp */
    *mtime += 946684800;
  }

  return rc;

}

/*! Error map */
typedef struct
{
  Result fs_error; //!< Error from FS service
  int    error;    //!< POSIX errno
} error_map_t;

/*! Error table */
static const error_map_t error_table[] =
{
  /* keep this list sorted! */
  { 0x082044BE, EEXIST,       },
  { 0x086044D2, ENOSPC,       },
  { 0xC8804478, ENOENT,       },
  { 0xC92044FA, ENOENT,       },
  { 0xE0E046BE, EINVAL,       },
  { 0xE0E046BF, ENAMETOOLONG, },
};
static const size_t num_errors = sizeof(error_table)/sizeof(error_table[0]);

/*! Comparison function for bsearch on error_table
 *
 *  @param[in] p1 Left side of comparison
 *  @param[in] p2 Right side of comparison
 *
 *  @returns <0 if lhs < rhs
 *  @returns >0 if lhs > rhs
 *  @returns 0  if lhs == rhs
 */
static int
error_cmp(const void *p1, const void *p2)
{
  const error_map_t *lhs = (const error_map_t*)p1;
  const error_map_t *rhs = (const error_map_t*)p2;

  if((u32)lhs->fs_error < (u32)rhs->fs_error)
    return -1;
  else if((u32)lhs->fs_error > (u32)rhs->fs_error)
    return 1;
  return 0;
}

/*! Translate FS service error to errno
 *
 *  @param[in] error FS service error
 *
 *  @returns errno
 */
static int
sdmc_translate_error(Result error)
{
  error_map_t key = { .fs_error = error };
  const error_map_t *rc = bsearch(&key, error_table, num_errors,
                                  sizeof(error_map_t), error_cmp);

  if(rc != NULL)
    return rc->error;

  return (int)error;
}
