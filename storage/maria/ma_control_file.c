/* Copyright (C) 2006 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*
  WL#3234 Maria control file
  First version written by Guilhem Bichot on 2006-04-27.
  Does not compile yet.
*/

#include "maria_def.h"

/* Here is the implementation of this module */

/*
  a control file contains 3 objects: magic string, LSN of last checkpoint,
  number of last log.
*/

/* total size should be < sector size for atomic write operation */
#define CONTROL_FILE_MAGIC_STRING "\xfe\xfe\xc\1MACF"
#define CONTROL_FILE_MAGIC_STRING_OFFSET 0
#define CONTROL_FILE_MAGIC_STRING_SIZE (sizeof(CONTROL_FILE_MAGIC_STRING)-1)
#define CONTROL_FILE_CHECKSUM_OFFSET (CONTROL_FILE_MAGIC_STRING_OFFSET + CONTROL_FILE_MAGIC_STRING_SIZE)
#define CONTROL_FILE_CHECKSUM_SIZE 4
#define CONTROL_FILE_LSN_OFFSET (CONTROL_FILE_CHECKSUM_OFFSET + CONTROL_FILE_CHECKSUM_SIZE)
#define CONTROL_FILE_LSN_SIZE LSN_STORE_SIZE
#define CONTROL_FILE_FILENO_OFFSET (CONTROL_FILE_LSN_OFFSET + CONTROL_FILE_LSN_SIZE)
#define CONTROL_FILE_FILENO_SIZE 4
#define CONTROL_FILE_SIZE (CONTROL_FILE_FILENO_OFFSET + CONTROL_FILE_FILENO_SIZE)

/* This module owns these two vars. */
LSN    last_checkpoint_lsn= LSN_IMPOSSIBLE;
uint32 last_logno=          FILENO_IMPOSSIBLE;

/**
   @brief If log's lock should be asserted when writing to control file.

   Can be re-used by any function which needs to be thread-safe except when
   it is called at startup.
*/
my_bool maria_multi_threaded= FALSE;

/*
  Control file is less then  512 bytes (a disk sector),
  to be as atomic as possible
*/
static int control_file_fd= -1;

/*
  @brief Initialize control file subsystem

  Looks for the control file. If none and creation is requested, creates file.
  If present, reads it to find out last checkpoint's LSN and last log, updates
  the last_checkpoint_lsn and last_logno global variables.
  Called at engine's start.

  @param  create_if_missing

  @note
  The format of the control file is:
  4 bytes: magic string
  4 bytes: checksum of the following bytes
  4 bytes: number of log where last checkpoint is
  4 bytes: offset in log where last checkpoint is
  4 bytes: number of last log

  @return Operation status
    @retval 0      OK
    @retval 1      Error (in which case the file is left closed)
*/
CONTROL_FILE_ERROR ma_control_file_create_or_open(my_bool create_if_missing)
{
  char buffer[CONTROL_FILE_SIZE];
  char name[FN_REFLEN];
  MY_STAT stat_buff;
  my_bool create_file;
  int open_flags= O_BINARY | /*O_DIRECT |*/ O_RDWR;
  int error= CONTROL_FILE_UNKNOWN_ERROR;
  DBUG_ENTER("ma_control_file_create_or_open");

  /*
    If you change sizes in the #defines, you at least have to change the
    "*store" and "*korr" calls in this file, and can even create backward
    compatibility problems. Beware!
  */
  DBUG_ASSERT(CONTROL_FILE_LSN_SIZE == (3+4));
  DBUG_ASSERT(CONTROL_FILE_FILENO_SIZE == 4);

  if (control_file_fd >= 0) /* already open */
    DBUG_RETURN(0);

  if (fn_format(name, CONTROL_FILE_BASE_NAME,
                maria_data_root, "", MYF(MY_WME)) == NullS)
    DBUG_RETURN(CONTROL_FILE_UNKNOWN_ERROR);

  create_file= test(my_access(name,F_OK));

  if (create_file)
  {
    if (!create_if_missing)
      DBUG_RETURN(CONTROL_FILE_MISSING);
    if ((control_file_fd= my_create(name, 0,
                                    open_flags, MYF(MY_SYNC_DIR))) < 0)
      DBUG_RETURN(CONTROL_FILE_UNKNOWN_ERROR);

    /*
      To be safer we should make sure that there are no logs or data/index
      files around (indeed it could be that the control file alone was deleted
      or not restored, and we should not go on with life at this point).

      TODO: For now we trust (this is alpha version), but for beta if would
      be great to verify.

      We could have a tool which can rebuild the control file, by reading the
      directory of logs, finding the newest log, reading it to find last
      checkpoint... Slow but can save your db. For this to be possible, we
      must always write to the control file right after writing the checkpoint
      log record, and do nothing in between (i.e. the checkpoint must be
      usable as soon as it has been written to the log).
    */

    /* init the file with these "undefined" values */
    DBUG_RETURN(ma_control_file_write_and_force(LSN_IMPOSSIBLE,
                                                FILENO_IMPOSSIBLE,
                                                CONTROL_FILE_UPDATE_ALL));
  }

  /* Otherwise, file exists */

  if ((control_file_fd= my_open(name, open_flags, MYF(MY_WME))) < 0)
    goto err;

  if (my_stat(name, &stat_buff, MYF(MY_WME)) == NULL)
    goto err;

  if ((uint)stat_buff.st_size < CONTROL_FILE_SIZE)
  {
    /*
      Given that normally we write only a sector and it's atomic, the only
      possibility for a file to be of too short size is if we crashed at the
      very first startup, between file creation and file write. Quite unlikely
      (and can be made even more unlikely by doing this: create a temp file,
      write it, and then rename it to be the control file).
      What's more likely is if someone forgot to restore the control file,
      just did a "touch control" to try to get Maria to start, or if the
      disk/filesystem has a problem.
      So let's be rigid.
    */
    /*
      TODO: store a message "too small file" somewhere, so that it goes to
      MySQL's error log at startup.
    */
    error= CONTROL_FILE_TOO_SMALL;
    goto err;
  }

  if ((uint)stat_buff.st_size > CONTROL_FILE_SIZE)
  {
    /* TODO: store "too big file" message */
    error= CONTROL_FILE_TOO_BIG;
    goto err;
  }

  if (my_read(control_file_fd, buffer, CONTROL_FILE_SIZE,
              MYF(MY_FNABP | MY_WME)))
    goto err;
  if (memcmp(buffer + CONTROL_FILE_MAGIC_STRING_OFFSET,
             CONTROL_FILE_MAGIC_STRING, CONTROL_FILE_MAGIC_STRING_SIZE))
  {
    /* TODO: store message "bad magic string" somewhere */
    error= CONTROL_FILE_BAD_MAGIC_STRING;
    goto err;
  }
  if (my_checksum(0, buffer + CONTROL_FILE_LSN_OFFSET,
                  CONTROL_FILE_SIZE - CONTROL_FILE_LSN_OFFSET) !=
      uint4korr(buffer + CONTROL_FILE_CHECKSUM_OFFSET))
  {
    /* TODO: store message "checksum mismatch" somewhere */
    error= CONTROL_FILE_BAD_CHECKSUM;
    goto err;
  }
  last_checkpoint_lsn= lsn_korr(buffer + CONTROL_FILE_LSN_OFFSET);
  last_logno= uint4korr(buffer + CONTROL_FILE_FILENO_OFFSET);

  DBUG_RETURN(0);
err:
  ma_control_file_end();
  DBUG_RETURN(error);
}


/*
  Write information durably to the control file; stores this information into
  the last_checkpoint_lsn and last_logno global variables.
  Called when we have created a new log (after syncing this log's creation)
  and when we have written a checkpoint (after syncing this log record).
  Variables last_checkpoint_lsn and last_logno must be protected by caller
  using log's lock, unless this function is called at startup.

  SYNOPSIS
    ma_control_file_write_and_force()
    checkpoint_lsn       LSN of last checkpoint
    logno                last log file number
    objs_to_write        which of the arguments should be used as new values
                         (for example, CONTROL_FILE_UPDATE_ONLY_LSN will not
                         write the logno argument to the control file and will
                         not update the last_logno global variable); can be:
                         CONTROL_FILE_UPDATE_ALL
                         CONTROL_FILE_UPDATE_ONLY_LSN
                         CONTROL_FILE_UPDATE_ONLY_LOGNO.

  NOTE
    We always want to do one single my_pwrite() here to be as atomic as
    possible.

  RETURN
    0 - OK
    1 - Error
*/

int ma_control_file_write_and_force(const LSN checkpoint_lsn, uint32 logno,
                                 uint objs_to_write)
{
  char buffer[CONTROL_FILE_SIZE];
  my_bool update_checkpoint_lsn= FALSE, update_logno= FALSE;
  DBUG_ENTER("ma_control_file_write_and_force");

  DBUG_ASSERT(control_file_fd >= 0); /* must be open */
#ifndef DBUG_OFF
  if (maria_multi_threaded)
    translog_lock_assert_owner();
#endif

  memcpy(buffer + CONTROL_FILE_MAGIC_STRING_OFFSET,
         CONTROL_FILE_MAGIC_STRING, CONTROL_FILE_MAGIC_STRING_SIZE);

  if (objs_to_write == CONTROL_FILE_UPDATE_ONLY_LSN)
    update_checkpoint_lsn= TRUE;
  else if (objs_to_write == CONTROL_FILE_UPDATE_ONLY_LOGNO)
    update_logno= TRUE;
  else if (objs_to_write == CONTROL_FILE_UPDATE_ALL)
    update_checkpoint_lsn= update_logno= TRUE;
  else /* incorrect value of objs_to_write */
    DBUG_ASSERT(0);

  if (update_checkpoint_lsn)
    lsn_store(buffer + CONTROL_FILE_LSN_OFFSET, checkpoint_lsn);
  else /* store old value == change nothing */
    lsn_store(buffer + CONTROL_FILE_LSN_OFFSET, last_checkpoint_lsn);

  if (update_logno)
    int4store(buffer + CONTROL_FILE_FILENO_OFFSET, logno);
  else
    int4store(buffer + CONTROL_FILE_FILENO_OFFSET, last_logno);

  {
    uint32 sum= (uint32)
      my_checksum(0, buffer + CONTROL_FILE_LSN_OFFSET,
                  CONTROL_FILE_SIZE - CONTROL_FILE_LSN_OFFSET);
    int4store(buffer + CONTROL_FILE_CHECKSUM_OFFSET, sum);
  }

  if (my_pwrite(control_file_fd, buffer, sizeof(buffer),
                0, MYF(MY_FNABP |  MY_WME)) ||
      my_sync(control_file_fd, MYF(MY_WME)))
    DBUG_RETURN(1);

  if (update_checkpoint_lsn)
    last_checkpoint_lsn= checkpoint_lsn;
  if (update_logno)
    last_logno= logno;

  DBUG_RETURN(0);
}


/*
  Free resources taken by control file subsystem

  SYNOPSIS
    ma_control_file_end()
*/

int ma_control_file_end()
{
  int close_error;
  DBUG_ENTER("ma_control_file_end");

  if (control_file_fd < 0) /* already closed */
    DBUG_RETURN(0);

  close_error= my_close(control_file_fd, MYF(MY_WME));
  /*
    As my_close() frees structures even if close() fails, we do the same,
    i.e. we mark the file as closed in all cases.
  */
  control_file_fd= -1;
  /*
    As this module owns these variables, closing the module forbids access to
    them (just a safety):
  */
  last_checkpoint_lsn= LSN_IMPOSSIBLE;
  last_logno= FILENO_IMPOSSIBLE;

  DBUG_RETURN(close_error);
}
