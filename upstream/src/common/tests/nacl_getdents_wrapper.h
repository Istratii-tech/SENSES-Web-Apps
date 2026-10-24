// ARC MOD TRACK "third_party/nacl-glibc/sysdeps/nacl/nacl_getdents_wrapper.c"
// ARC MOD BEGIN
// We changed the extension of this file from .c to .h so
// NinjaGenerator does not find this file automatically, and made it
// C++ so this can be included from unittest_irthook.cc.
// ARC MOD END

/* The purpose of this file is to be #included by generic readdir
   implementations.  */

static const int d_name_shift = offsetof (DIRENT_TYPE, d_name) -
    offsetof (struct nacl_abi_dirent, nacl_abi_d_name);

/* Calls __nacl_irt_getdents and converts resulting buffer to glibc abi.
   This wrapper is required since glibc abi for DIRENT_TYPE differs from
   struct nacl_abi_dirent. */
// ARC MOD BEGIN
// Do not use glibc specific macros.
static ssize_t nacl_getdents_wrapper(int fd, char *buf, size_t buf_size)
// ARC MOD END
{
  /* __nacl_irt_getdents fills buffer with overlapped structures
     nacl_abi_dirent. Right after d_reclen bytes of one structure end the next
     structure begins, and so on. For example if nacl_abi_dirent contains 14
     bytes long string in d_name field then it will occupy 10+14 bytes in the
     buffer. This wrapper fills buf so that every DIRENT_TYPE occupies in it
     one byte more than corresponding nacl_abi_dirent in buffer filled by nacl
     syscall. To avoid overwhelming of buf it is necessary to make nacl_buf
     smaller. It is ok to make nacl_buf_size equal buf_size * 0.9 because
     minimal size of nacl_abi_dirent is 12 bytes. */
  int nacl_buf_size = buf_size - buf_size / 10 - 1;
  char nacl_buf[nacl_buf_size];
  size_t nbytes;
  // ARC MOD BEGIN
  // Add a cast and use __nacl_irt_getdents_real instead of
  // __nacl_irt_getdents, which is hooked.
  int rv = __nacl_irt_getdents_real(fd,
                                    reinterpret_cast<struct dirent*>(nacl_buf),
                                    nacl_buf_size, &nbytes);
  // ARC MOD END
  struct nacl_abi_dirent *nacl_dp;
  DIRENT_TYPE *dp;
  size_t nacl_offset = 0;
  int offset = 0;
  int d_name_len;

  if (rv > 0)
    {
      // ARC MOD BEGIN UPSTREAM nacl-getdents-return
      errno = rv;
      return -1;
      // ARC MOD END UPSTREAM
    }
  while (nacl_offset < nbytes)
    {
      nacl_dp = (struct nacl_abi_dirent *) (nacl_buf + nacl_offset);
      dp = (DIRENT_TYPE *) (buf + offset);
      // ARC MOD BEGIN
      // Add a cast.
      if (static_cast<size_t>(offset + nacl_dp->nacl_abi_d_reclen +
                              d_name_shift) >= buf_size)
      // ARC MOD END
        {
          errno = EINVAL;
          return -1;
        }
      dp->d_ino = nacl_dp->nacl_abi_d_ino;
      dp->d_off = nacl_dp->nacl_abi_d_off;
      dp->d_reclen = nacl_dp->nacl_abi_d_reclen + d_name_shift;
      dp->d_type = 0;
      d_name_len =  nacl_dp->nacl_abi_d_reclen -
          offsetof (struct nacl_abi_dirent, nacl_abi_d_name);
      memcpy (dp->d_name, nacl_dp->nacl_abi_d_name, d_name_len);
      offset += dp->d_reclen;
      nacl_offset += nacl_dp->nacl_abi_d_reclen;
    }
  return offset;
}
