/* lib.c - various reusable stuff.
 *
 * Copyright 2006 Rob Landley <rob@landley.net>
 */

#include "toys.h"

void verror_msg(char *msg, int err, va_list va)
{
  char *s = ": %s";

  fprintf(stderr, "%s: ", toys.which->name);
  if (msg) vfprintf(stderr, msg, va);
  else s+=2;
  if (err) fprintf(stderr, s, strerror(err));
  putc('\n', stderr);
  if (!toys.exitval) toys.exitval++;
}

void error_msg(char *msg, ...)
{
  va_list va;

  va_start(va, msg);
  verror_msg(msg, 0, va);
  va_end(va);
}

void perror_msg(char *msg, ...)
{
  va_list va;

  va_start(va, msg);
  verror_msg(msg, errno, va);
  va_end(va);
}

// Die with an error message.
void error_exit(char *msg, ...)
{
  va_list va;

  if (CFG_TOYBOX_HELP && toys.exithelp) show_help();

  va_start(va, msg);
  verror_msg(msg, 0, va);
  va_end(va);

  xexit();
}

// Die with an error message and strerror(errno)
void perror_exit(char *msg, ...)
{
  va_list va;

  va_start(va, msg);
  verror_msg(msg, errno, va);
  va_end(va);

  xexit();
}

// Keep reading until full or EOF
ssize_t readall(int fd, void *buf, size_t len)
{
  size_t count = 0;

  while (count<len) {
    int i = read(fd, buf+count, len-count);
    if (!i) break;
    if (i<0) return i;
    count += i;
  }

  return count;
}

// Keep writing until done or EOF
ssize_t writeall(int fd, void *buf, size_t len)
{
  size_t count = 0;
  while (count<len) {
    int i = write(fd, buf+count, len-count);
    if (i<1) return i;
    count += i;
  }

  return count;
}

off_t lskip(int fd, off_t offset)
{
  off_t and = lseek(fd, offset, SEEK_CUR);

  if (and != -1 && offset >= lseek(fd, offset, SEEK_END)
    && offset+and == lseek(fd, offset+and, SEEK_SET)) return 0;
  else {
    char buf[4096];
    while (offset>0) {
      int try = offset>sizeof(buf) ? sizeof(buf) : offset, or;

      or = readall(fd, buf, try);
      if (or < 0) perror_msg("lskip to %lld", (long long)offset);
      else offset -= try;
      if (or < try) break;
    }

    return offset;
  }
}

// Split a path into linked list of components, tracking head and tail of list.
// Filters out // entries with no contents.
struct string_list **splitpath(char *path, struct string_list **list)
{
  char *new = path;

  *list = 0;
  do {
    int len;

    if (*path && *path != '/') continue;
    len = path-new;
    if (len > 0) {
      *list = xmalloc(sizeof(struct string_list) + len + 1);
      (*list)->next = 0;
      strncpy((*list)->str, new, len);
      (*list)->str[len] = 0;
      list = &(*list)->next;
    }
    new = path+1;
  } while (*path++);

  return list;
}

// Find all file in a colon-separated path with access type "type" (generally
// X_OK or R_OK).  Returns a list of absolute paths to each file found, in
// order.

struct string_list *find_in_path(char *path, char *filename)
{
  struct string_list *rlist = NULL, **prlist=&rlist;
  char *cwd = xgetcwd();

  for (;;) {
    char *next = path ? strchr(path, ':') : NULL;
    int len = next ? next-path : strlen(path);
    struct string_list *rnext;
    struct stat st;

    rnext = xmalloc(sizeof(void *) + strlen(filename)
      + (len ? len : strlen(cwd)) + 2);
    if (!len) sprintf(rnext->str, "%s/%s", cwd, filename);
    else {
      char *res = rnext->str;
      strncpy(res, path, len);
      res += len;
      *(res++) = '/';
      strcpy(res, filename);
    }

    // Confirm it's not a directory.
    if (!stat(rnext->str, &st) && S_ISREG(st.st_mode)) {
      *prlist = rnext;
      rnext->next = NULL;
      prlist = &(rnext->next);
    } else free(rnext);

    if (!next) break;
    path += len;
    path++;
  }
  free(cwd);

  return rlist;
}

// Convert unsigned int to ascii, writing into supplied buffer.  A truncated
// result contains the first few digits of the result ala strncpy, and is
// always null terminated (unless buflen is 0).
void utoa_to_buf(unsigned n, char *buf, unsigned buflen)
{
  int i, out = 0;

  if (buflen) {
    for (i=1000000000; i; i/=10) {
      int res = n/i;

      if ((res || out || i == 1) && --buflen>0) {
        out++;
        n -= res*i;
        *buf++ = '0' + res;
      }
    }
    *buf = 0;
  }
}

// Convert signed integer to ascii, using utoa_to_buf()
void itoa_to_buf(int n, char *buf, unsigned buflen)
{
  if (buflen && n<0) {
    n = -n;
    *buf++ = '-';
    buflen--;
  }
  utoa_to_buf((unsigned)n, buf, buflen);
}

// This static buffer is used by both utoa() and itoa(), calling either one a
// second time will overwrite the previous results.
//
// The longest 32 bit integer is -2 billion plus a null terminator: 12 bytes.
// Note that int is always 32 bits on any remotely unix-like system, see
// http://www.unix.org/whitepapers/64bit.html for details.

static char itoa_buf[12];

// Convert unsigned integer to ascii, returning a static buffer.
char *utoa(unsigned n)
{
  utoa_to_buf(n, itoa_buf, sizeof(itoa_buf));

  return itoa_buf;
}

char *itoa(int n)
{
  itoa_to_buf(n, itoa_buf, sizeof(itoa_buf));

  return itoa_buf;
}

// atol() with the kilo/mega/giga/tera/peta/exa extensions.
// (zetta and yotta don't fit in 64 bits.)
long atolx(char *numstr)
{
  char *c, *suffixes="bkmgtpe", *end;
  long val = strtol(numstr, &c, 0);

  if (*c) {
    if (c != numstr && (end = strchr(suffixes, tolower(*c)))) {
      int shift = end-suffixes;
      if (shift--) val *= 1024L<<(shift*10);
    } else {
      while (isspace(*c)) c++;
      if (*c) error_exit("not integer: %s", numstr);
    }
  }

  return val;
}

int numlen(long l)
{
  int len = 0;
  while (l) {
     l /= 10;
     len++;
  }
  return len;
}

int stridx(char *haystack, char needle)
{
  char *off;

  if (!needle) return -1;
  off = strchr(haystack, needle);
  if (!off) return -1;

  return off-haystack;
}

// Return how long the file at fd is, if there's any way to determine it.
off_t fdlength(int fd)
{
  off_t bottom = 0, top = 0, pos, old;
  int size;

  // If the ioctl works for this, return it.

  if (ioctl(fd, BLKGETSIZE, &size) >= 0) return size*512L;

  // If not, do a binary search for the last location we can read.  (Some
  // block devices don't do BLKGETSIZE right.)  This should probably have
  // a CONFIG option...

  old = lseek(fd, 0, SEEK_CUR);
  do {
    char temp;

    pos = bottom + (top - bottom) / 2;

    // If we can read from the current location, it's bigger.

    if (lseek(fd, pos, 0)>=0 && read(fd, &temp, 1)==1) {
      if (bottom == top) bottom = top = (top+1) * 2;
      else bottom = pos;

      // If we can't, it's smaller.

    } else {
      if (bottom == top) {
        if (!top) return 0;
        bottom = top/2;
      } else top = pos;
    }
  } while (bottom + 1 != top);

  lseek(fd, old, SEEK_SET);

  return pos + 1;
}

// Read contents of file as a single freshly allocated nul-terminated string.
char *readfile(char *name)
{
  off_t len;
  int fd;
  char *buf;

  fd = open(name, O_RDONLY);
  if (fd == -1) return 0;
  len = fdlength(fd);
  buf = xmalloc(len+1);
  buf[readall(fd, buf, len)] = 0;

  return buf;
}

// Sleep for this many thousandths of a second
void msleep(long miliseconds)
{
  struct timespec ts;

  ts.tv_sec = miliseconds/1000;
  ts.tv_nsec = (miliseconds%1000)*1000000;
  nanosleep(&ts, &ts);
}

int64_t peek(void *ptr, int size)
{
  if (size & 8) {
    int64_t *p = (int64_t *)ptr;
    return *p;
  } else if (size & 4) {
    int *p = (int *)ptr;
    return *p;
  } else if (size & 2) {
    short *p = (short *)ptr;
    return *p;
  } else {
    char *p = (char *)ptr;
    return *p;
  }
}

void poke(void *ptr, uint64_t val, int size)
{
  if (size & 8) {
    uint64_t *p = (uint64_t *)ptr;
    *p = val;
  } else if (size & 4) {
    int *p = (int *)ptr;
    *p = val;
  } else if (size & 2) {
    short *p = (short *)ptr;
    *p = val;
  } else {
    char *p = (char *)ptr;
    *p = val;
  }
}

// Iterate through an array of files, opening each one and calling a function
// on that filehandle and name.  The special filename "-" means stdin if
// flags is O_RDONLY, stdout otherwise.  An empty argument list calls
// function() on just stdin/stdout.
//
// Note: read only filehandles are automatically closed when function()
// returns, but writeable filehandles must be close by function()
void loopfiles_rw(char **argv, int flags, int permissions, int failok,
  void (*function)(int fd, char *name))
{
  int fd;

  // If no arguments, read from stdin.
  if (!*argv) function(flags ? 1 : 0, "-");
  else do {
    // Filename "-" means read from stdin.
    // Inability to open a file prints a warning, but doesn't exit.

    if (!strcmp(*argv,"-")) fd=0;
    else if (0>(fd = open(*argv, flags, permissions)) && !failok) {
      perror_msg("%s", *argv);
      toys.exitval = 1;
      continue;
    }
    function(fd, *argv);
    if (flags == O_RDONLY) close(fd);
  } while (*++argv);
}

// Call loopfiles_rw with O_RDONLY and !failok (common case).
void loopfiles(char **argv, void (*function)(int fd, char *name))
{
  loopfiles_rw(argv, O_RDONLY, 0, 0, function);
}

// Slow, but small.

char *get_rawline(int fd, long *plen, char end)
{
  char c, *buf = NULL;
  long len = 0;

  for (;;) {
    if (1>read(fd, &c, 1)) break;
    if (!(len & 63)) buf=xrealloc(buf, len+65);
    if ((buf[len++]=c) == end) break;
  }
  if (buf) buf[len]=0;
  if (plen) *plen = len;

  return buf;
}

char *get_line(int fd)
{
  long len;
  char *buf = get_rawline(fd, &len, '\n');

  if (buf && buf[--len]=='\n') buf[len]=0;

  return buf;
}

int wfchmodat(int fd, char *name, mode_t mode)
{
  int rc = fchmodat(fd, name, mode, 0);

  if (rc) {
    perror_msg("chmod '%s' to %04o", name, mode);
    toys.exitval=1;
  }
  return rc;
}

static char *tempfile2zap;
static void tempfile_handler(int i)
{
  if (1 < (long)tempfile2zap) unlink(tempfile2zap);
  _exit(1);
}

// Open a temporary file to copy an existing file into.
int copy_tempfile(int fdin, char *name, char **tempname)
{
  struct stat statbuf;
  int fd;

  *tempname = xstrndup(name, strlen(name)+6);
  strcat(*tempname,"XXXXXX");
  if(-1 == (fd = mkstemp(*tempname))) error_exit("no temp file");
  if (!tempfile2zap) sigatexit(tempfile_handler);
  tempfile2zap = *tempname;

  // Set permissions of output file

  fstat(fdin, &statbuf);
  fchmod(fd, statbuf.st_mode);

  return fd;
}

// Abort the copy and delete the temporary file.
void delete_tempfile(int fdin, int fdout, char **tempname)
{
  close(fdin);
  close(fdout);
  unlink(*tempname);
  tempfile2zap = (char *)1;
  free(*tempname);
  *tempname = NULL;
}

// Copy the rest of the data and replace the original with the copy.
void replace_tempfile(int fdin, int fdout, char **tempname)
{
  char *temp = xstrdup(*tempname);

  temp[strlen(temp)-6]=0;
  if (fdin != -1) {
    xsendfile(fdin, fdout);
    xclose(fdin);
  }
  xclose(fdout);
  rename(*tempname, temp);
  tempfile2zap = (char *)1;
  free(*tempname);
  free(temp);
  *tempname = NULL;
}

// Create a 256 entry CRC32 lookup table.

void crc_init(unsigned int *crc_table, int little_endian)
{
  unsigned int i;

  // Init the CRC32 table (big endian)
  for (i=0; i<256; i++) {
    unsigned int j, c = little_endian ? i : i<<24;
    for (j=8; j; j--)
      if (little_endian) c = (c&1) ? (c>>1)^0xEDB88320 : c>>1;
      else c=c&0x80000000 ? (c<<1)^0x04c11db7 : (c<<1);
    crc_table[i] = c;
  }
}

// Quick and dirty query size of terminal, doesn't do ANSI probe fallback.
// set *x=0 and *y=0 before calling to detect failure to set either, or
// x=80 y=25 to provide defaults

void terminal_size(unsigned *x, unsigned *y)
{
  struct winsize ws;
  int i;

  //memset(&ws, 0, sizeof(ws));
  for (i=0; i<3; i++) {
    if (ioctl(i, TIOCGWINSZ, &ws)) continue;
    if (x) *x = ws.ws_col;
    if (y) *y = ws.ws_row;
  }
  if (x) {
    char *s = getenv("COLUMNS");

    i = s ? atoi(s) : 0;
    if (i>0) *x = i;
  }
  if (y) {
    char *s = getenv("ROWS");

    i = s ? atoi(s) : 0;
    if (i>0) *y = i;
  }
}

int yesno(char *prompt, int def)
{
  char buf;

  fprintf(stderr, "%s (%c/%c):", prompt, def ? 'Y' : 'y', def ? 'n' : 'N');
  fflush(stderr);
  while (fread(&buf, 1, 1, stdin)) {
    int new;

    // The letter changes the value, the newline (or space) returns it.
    if (isspace(buf)) break;
    if (-1 != (new = stridx("ny", tolower(buf)))) def = new;
  }

  return def;
}

struct signame {
  int num;
  char *name;
};

// Signals required by POSIX 2008:
// http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/signal.h.html

#define SIGNIFY(x) {SIG##x, #x}

static struct signame signames[] = {
  SIGNIFY(ABRT), SIGNIFY(ALRM), SIGNIFY(BUS),
  SIGNIFY(FPE), SIGNIFY(HUP), SIGNIFY(ILL), SIGNIFY(INT), SIGNIFY(KILL),
  SIGNIFY(PIPE), SIGNIFY(QUIT), SIGNIFY(SEGV), SIGNIFY(TERM),
  SIGNIFY(USR1), SIGNIFY(USR2), SIGNIFY(SYS), SIGNIFY(TRAP),
  SIGNIFY(VTALRM), SIGNIFY(XCPU), SIGNIFY(XFSZ),

  // Start of non-terminal signals

  SIGNIFY(CHLD), SIGNIFY(CONT), SIGNIFY(STOP), SIGNIFY(TSTP),
  SIGNIFY(TTIN), SIGNIFY(TTOU), SIGNIFY(URG)
};

// not in posix: SIGNIFY(STKFLT), SIGNIFY(WINCH), SIGNIFY(IO), SIGNIFY(PWR)
// obsolete: SIGNIFY(PROF) SIGNIFY(POLL)

// Install the same handler on every signal that defaults to killing the process
void sigatexit(void *handler)
{
  int i;
  for (i=0; signames[i].num != SIGCHLD; i++) signal(signames[i].num, handler);
}
// Convert name to signal number.  If name == NULL print names.
int sig_to_num(char *pidstr)
{
  int i;

  if (pidstr) {
    char *s;
    i = strtol(pidstr, &s, 10);
    if (!*s) return i;

    if (!strncasecmp(pidstr, "sig", 3)) pidstr+=3;
  }
  for (i = 0; i < sizeof(signames)/sizeof(struct signame); i++)
    if (!pidstr) xputs(signames[i].name);
    else if (!strcasecmp(pidstr, signames[i].name)) return signames[i].num;

  return -1;
}

char *num_to_sig(int sig)
{
  int i;

  for (i=0; i<sizeof(signames)/sizeof(struct signame); i++)
    if (signames[i].num == sig) return signames[i].name;
  return NULL;
}

// premute mode bits based on posix mode strings.
mode_t string_to_mode(char *modestr, mode_t mode)
{
  char *whos = "ogua", *hows = "=+-", *whats = "xwrstX", *whys = "ogu";
  char *s, *str = modestr;

  // Handle octal mode
  if (isdigit(*str)) {
    mode = strtol(str, &s, 8);
    if (*s || (mode & ~(07777))) goto barf;

    return mode;
  }

  // Gaze into the bin of permission...
  for (;;) {
    int i, j, dowho, dohow, dowhat, amask;

    dowho = dohow = dowhat = amask = 0;

    // Find the who, how, and what stanzas, in that order
    while (*str && (s = strchr(whos, *str))) {
      dowho |= 1<<(s-whos);
      str++;
    }
    // If who isn't specified, like "a" but honoring umask.
    if (!dowho) {
      dowho = 8;
      umask(amask=umask(0));
    }
    if (!*str || !(s = strchr(hows, *str))) goto barf;
    dohow = *(str++);

    if (!dohow) goto barf;
    while (*str && (s = strchr(whats, *str))) {
      dowhat |= 1<<(s-whats);
      str++;
    }

    // Convert X to x for directory or if already executable somewhere
    if ((dowhat&32) &&  (S_ISDIR(mode) || (mode&0111))) dowhat |= 1;

    // Copy mode from another category?
    if (!dowhat && *str && (s = strchr(whys, *str))) {
      dowhat = (mode>>(3*(s-whys)))&7;
      str++;
    }

    // Are we ready to do a thing yet?
    if (*str && *(str++) != ',') goto barf;

    // Ok, apply the bits to the mode.
    for (i=0; i<4; i++) {
      for (j=0; j<3; j++) {
        mode_t bit = 0;
        int where = 1<<((3*i)+j);

        if (amask & where) continue;

        // Figure out new value at this location
        if (i == 3) {
          // suid/sticky bit.
          if (j) {
            if ((dowhat & 8) && (dowho&(8|(1<<i)))) bit++;
          } else if (dowhat & 16) bit++;
        } else {
          if (!(dowho&(8|(1<<i)))) continue;
          if (dowhat&(1<<j)) bit++;
        }

        // When selection active, modify bit

        if (dohow == '=' || (bit && dohow == '-')) mode &= ~where;
        if (bit && dohow != '-') mode |= where;
      }
    }

    if (!*str) break;
  }
  return mode;
barf:
  error_exit("bad mode '%s'", modestr);
}

// Format access mode into a drwxrwxrwx string
void mode_to_string(mode_t mode, char *buf)
{
  char c, d;
  int i, bit;

  buf[10]=0;
  for (i=0; i<9; i++) {
    bit = mode & (1<<i);
    c = i%3;
    if (!c && (mode & (1<<((d=i/3)+9)))) {
      c = "tss"[d];
      if (!bit) c &= ~0x20;
    } else c = bit ? "xwr"[c] : '-';
    buf[9-i] = c;
  }

  if (S_ISDIR(mode)) c = 'd';
  else if (S_ISBLK(mode)) c = 'b';
  else if (S_ISCHR(mode)) c = 'c';
  else if (S_ISLNK(mode)) c = 'l';
  else if (S_ISFIFO(mode)) c = 'p';
  else if (S_ISSOCK(mode)) c = 's';
  else c = '-';
  *buf = c;
}