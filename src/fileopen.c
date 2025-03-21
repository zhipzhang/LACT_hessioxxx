/* ============================================================================

   Copyright (C) 2000, 2003, 2006, 2007, 2008, 2009, 2010, 2014, 2015, 2016, 2017, 2018, 2019, 2021, 2022, 2023  Konrad Bernloehr

   This file is part of the eventio/hessio library.

   The eventio/hessio library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this library. If not, see <http://www.gnu.org/licenses/>.

============================================================================ */

/** @file fileopen.c 
 *  @short Allow searching of files in declared include paths (fopen replacement).
 *
 *  The functions provided in this file provide an enhanced replacement
 *  @c fileopen() for the C standard library's @c fopen() function.
 *  The enhancements are in several areas:
 *
 *  @li Where possible files are opened such that more than 2 gigabytes
 *      of data can be accessed on 32-bit systems when suitably compiled.
 *      This also works with software where a @c '-D_FILE_OFFSET_BITS=64' at
 *      compile-time cannot be used (of which ROOT is an infamous example).
 *  @li For reading files, a list of paths can be configured before the
 *      the first fileopen() call and all files without absolute paths
 *      will be searched in these paths. Writing always strictly
 *      follows the given file name and will not search in the path list.
 *  @li Files compressed with @c gzip or @c bzip2 can be handled on the
 *      fly. Files with corresponding file name extensions 
 *      ( @c .gz and @c .bz2 ) will be
 *      automatically decompressed when reading or compressed when
 *      writing (in a pipe, i.e. without producing temporary copies).
 *  @li In the same way, files compressed with @c lzop (for extension
 *      @c .lzo ), @c lzma (for extension  @c .lzma ) as well as
 *      @c xz (for extension @ .xz ) and @c lz4 (for extension @c .lz4 ) are handled
 *      on the fly. No check is made if these programs are installed.
 *  @li URIs (uniform resource identifiers) starting with @c http:,
 *      @c https:, or @c ftp: will also be opened in a pipe, with optional
 *      decompression, depending on the ending of the URI name.
 *      You can therefore easily process files located on a web or
 *      ftp server. Access is limited to reading.
 *  @li Files on any SSH server where you can login without a password
 *      can be read as @c 'ssh://user@host:filepath' where filepath can be
 *      an absolute path (starting with @c '/') or one relative to the
 *      users home directory.
 *  @li Input and output can also be from/to a user-defined program.
 *      Restrictions apply there which prevent execution of any
 *      program by default. Either a list of accepted execution paths
 *      has to be set up beforehand with initexepath()/addexepath()
 *      or permissive mode can be enabled, allowing execution of
 *      any given program.
 *
 *  @author  Konrad Bernloehr 
 *  @date    Nov. 2000 to 2023
 */

#include "initial.h"
#include "straux.h"
#include "fileopen.h"
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif

/** Use to decide if open/close success/failure is reported */
static int verbose = 0, parallel = 0, report = 0;
static int with_fallback = 1;
static int with_exec = 1;
#ifdef SAVE_ERRNO
static int s_errno = 0;
#endif
static const char *fileopen_list = ".fileopen.lis";

static void fileopen_print_report(void);

static int foei_done = 0;

static void fileopen_env_init(void);

/**
 * @short Initialize internal variables from environment on first call to fileopen().
 *
 * Environment variables recognized:
 *   - FILEOPEN_VERBOSE
 *   - FILEOPEN_PARALLEL
 *   - FILEOPEN_NO_FALLBACK
 *   - FILEOPEN_NO_EXEC
 *   - FILEOPEN_REPORT
 *   - FILEOPEN_LIST
 */ 

static void fileopen_env_init(void)
{
   char *s;
   if ( (s=getenv("FILEOPEN_VERBOSE")) != NULL )
   {
      verbose=atoi(s);
      if ( verbose > 0 )
         fprintf(stderr,"Setting verbose = %d for fileopen functions.\n", verbose);
   }
   if ( (s=getenv("FILEOPEN_PARALLEL")) != NULL )
      parallel = atoi(s);
   if ( getenv("FILEOPEN_NO_FALLBACK") != NULL )
      with_fallback = 0;
   if ( getenv("FILEOPEN_NO_EXEC") != NULL )
      with_exec = 0;
   if ( (s=getenv("FILEOPEN_REPORT")) != NULL )
   {
      report = atoi(s);
      if ( report != 0 )
         atexit(fileopen_print_report);
      if ( verbose > 0 && report != 0 )
         fprintf(stderr,"Reporting all files opened by fileopen with format option %d.\n", report);
   }
   if ( (s=getenv("FILEOPEN_LIST")) != NULL )
   {
      fileopen_list = s;
      if ( verbose > 0 && report != 0 )
         fprintf(stderr,"Reported list of files opened by fileopen goes to %s.\n", fileopen_list);
   }

   foei_done = 1;
}

struct rep_entry
{
   char *fname;
   char mode[4];
   size_t count;
   struct rep_entry *next;
};

struct rep_entry rep_base;

static void fileopen_add_report(const char *fname, const char *mode);

/**
 *  @short Add a filename to the list of filenames reported at program end into ".fileopen.lis" or FILEOPEN_LIST.
 *
 *  Compressed files are reported both with their real file name and with the
 *  pipe calling the external tool handling the compression.
 */

static void fileopen_add_report(const char *fname, const char *mode)
{
   struct rep_entry *rep = NULL;

   if ( !report )
      return;

   for ( rep=&rep_base; rep != NULL; rep=rep->next )
   {
      if ( rep->fname==(char *) NULL ) /* Happens only the first time */
      {
         rep->fname = strdup(fname);
         strncpy(rep->mode,mode,3);
         rep->mode[3] = '\0';
         rep->count = 1;
         return;
      }
      if ( strcmp(rep->fname,fname) == 0 ) /* This file has been opened before */
      {
         rep->count++;
         /* We leave mode at its first time value */
         return;
      }
      if ( rep->next == (struct rep_entry *) NULL ) /* We are at the end of the list, without a prior match */
      {
         if ( (rep->next = (struct rep_entry *) malloc(sizeof(struct rep_entry))) == NULL )
            return;
         rep = rep->next;
         rep->fname = strdup(fname);
         strncpy(rep->mode,mode,3);
         rep->mode[3] = '\0';
         rep->count = 1;
         rep->next = (struct rep_entry *) NULL;
         return;
      }
   }
}

/**
 *  @short Function called at program exit to report all names of files opened through fileopen().
 *
 *  The value of the FILEOPEN_REPORT environment variable can be used to select format options (1...8), 0=off.
 *  The simplest format is for FILEOPEN_REPORT=1, showing just the file names.
 *  For other values, the name of the program executed is included and the (first-time) mode and/or
 *  the number of times the file was opened may be included.
 *  The output is appended to file '.fileopen.lis' or the file name given in environment variable FILEOPEN_LIST
 */

static void fileopen_print_report()
{
   struct rep_entry *rep = NULL;

   if ( !report )
      return;

   FILE *f = fopen((fileopen_list != NULL) ? fileopen_list : ".fileopen.lis","a");
   if ( f == NULL )
      return;

   if ( verbose )
      fprintf(stderr,"Report list of files opened by fileopen, appending to %s\n", fileopen_list);
   fprintf(f, "\n");
   if ( report != 1 )
   {
      const char *prg = NULL;
      char *s = getenv("_");
      if ( s != NULL )
         prg = s;
      if ( prg == NULL )
         fprintf(f, "Unknown program:\n");
      else
         fprintf(f, "%s:\n", prg);
   }

   for ( rep=&rep_base; rep != NULL; rep=rep->next )
   {
      if ( rep->fname != NULL )
      {
         if ( report != 1 )
         {
            fprintf(f, "   ");
            if ( (report & 2) != 0 )
               fprintf(f, "%s\t", rep->mode);
            if ( (report & 4) != 0 )
               fprintf(f, "%ju\t", rep->count);
         }
         fprintf(f, "%s\n", rep->fname);
      }
   }

   fclose(f);
}

static FILE *popenx(const char *fname, const char *mode);

/**
 *  @short Function called by fileopen() to actually open a pipe, including files compressed through external tools.
 */

static FILE *popenx(const char *fname, const char *mode)
{
#ifdef SAVE_ERRNO
   s_errno = 0;
#endif
   FILE *f = popen(fname,mode);
   if ( f != NULL )
   {
      if ( report )
         fileopen_add_report(fname,mode);
      if ( verbose >= 2 )
         fprintf(stderr,"Pipe for fileno=%d, mode=%s opened as: %s\n", fileno(f), mode, fname);
      /* Could try to remember what corresponds to what fileno here ... */
   }
   else
   {
#ifdef SAVE_ERRNO
      s_errno = errno;
#endif
      if ( verbose > 0 )
         perror(fname);
      if ( verbose > 2 )
         fprintf(stderr,"popenx(\"%s\",\"%s\") failed.\n", fname, mode);
   }
   return f;
}

static FILE *fopenx(const char *fname, const char *mode);

/**
 *  @short Function called by fileopen() to actually open a plain file.
 */

static FILE *fopenx(const char *fname, const char *mode)
{
   FILE *f;
#ifdef SAVE_ERRNO
   s_errno = 0;
#endif
#ifdef __USE_LARGEFILE64
   /* With explicit support of large files on 32-bit machines. */
   f = fopen64(fname,mode);
#else
   /* Support for large files is either implicit or not available. */
   f = fopen(fname,mode);
#endif
   if ( f != NULL )
   {
      if ( report )
         fileopen_add_report(fname,mode);
      if ( verbose >= 2 )
         fprintf(stderr,"File for fileno=%d, mode=%s opened: %s\n", fileno(f), mode, fname);
      /* Could try to remember what corresponds to what fileno here ... */
   }
   else
   {
#ifdef SAVE_ERRNO
      s_errno = errno;
#endif
      if ( verbose > 0 )
         perror(fname);
      if ( verbose > 2 )
         fprintf(stderr,"fopenx(\"%s\",\"%s\") failed.\n", fname, mode);
   }
   return f;
}

/** The starting element of include paths. */
static struct incpath *root_path = NULL;

/** The starting element for execution paths. */
static struct incpath *root_exe_path = NULL; /* No execution paths by default */

/** Allow any execution pipe command if this variable is non-zero. */

static int permissive_pipes = 0; /* Not permissive by default */

/** Enable or disable the permissive execution of pipes. */

void set_permissive_pipes(int p)
{
   if ( p )
      enable_permissive_pipes();
   else
      disable_permissive_pipes();
}

/** Enable the permissive execution of pipes. */

void enable_permissive_pipes()
{
   permissive_pipes = 1;
   if ( root_exe_path == NULL )
      initexepath("$PATH");
}

/** Disable the permissive execution of pipes. */

void disable_permissive_pipes()
{
   permissive_pipes = 0;
}

struct incpath *get_include_path(void)
{
   return root_path;
}

static void freepath(void);
static void freeexepath(void);

/** Init the path list, with default_path as the only entry. */

void initpath (const char *default_path)
{
   int ipos = 0;
   char pathname[PATH_MAX];
   char *s;
   
   fileopen_env_init();

   if ( default_path == NULL )
   {
      if ( (s = getenv("FILEOPEN_DEFAULT_PATH")) != NULL )
         default_path = s;
      else
         default_path = ".";
   }

   if ( root_path != NULL )
      freepath();
      
   if ( verbose )
   {
      fprintf(stderr,"Initializing search path for fileopen: %s\n", default_path);
      if ( parallel )
         fprintf(stderr,"Preferring parallel program versions opening some compressed file types with fileopen.\n");
   }


   while ( getword(default_path,&ipos,pathname,sizeof(pathname)-1,':','\n') > 0 )
      addpath(pathname);
}

/* Initialize the path list for command execution. */

void initexepath (const char *default_exe_path)
{
   int ipos = 0;
   char pathname[PATH_MAX];

   if ( root_exe_path != NULL )
      freeexepath();

   if ( default_exe_path == NULL )
      return;

   while ( getword(default_exe_path,&ipos,pathname,sizeof(pathname)-1,':','\n') > 0 )
      addexepath(pathname);

   fileopen_env_init();
}

/** Free a whole list of include path elements. */

static void freepath()
{
   struct incpath *path, *next = NULL;
   for ( path = root_path; path != NULL; path = next )
   {
      next = path->next;
      if ( path->path != NULL )
         free(path->path);
      free(path);
   }
   root_path = NULL;
}

/** Free a whole list of execution path elements. */

static void freeexepath()
{
   struct incpath *path, *next = NULL;
   for ( path = root_exe_path; path != NULL; path = next )
   {
      next = path->next;
      if ( path->path != NULL )
         free(path->path);
      free(path);
   }
   root_exe_path = NULL;
}

/** Show the list of include paths. */

void listpath (char *buffer, size_t bufsize)
{
   struct incpath *path;
   if ( buffer == NULL || bufsize <= 0 )
      return;
   for ( path = root_path; path != NULL; path = path->next )
   {
      if ( path->path == NULL )
         continue;
      if ( bufsize > strlen(path->path)+3 )
      {
         strcpy(buffer," -I");
         buffer += 3;
         bufsize -= 3;
         strcpy(buffer,path->path);
         buffer += strlen(path->path);
         bufsize -= strlen(path->path);
      }
   }
}

/** 
 *  @short Add a path to the list of include paths, if not already there.
 *  The path name is always copied to a newly allocated memory location.
 *  This path name can actually be a colon-separated list, as for initpath().
 *  Also environment variables (indicated by starting with '$', e.g. "$HOME")
 *  are accepted (and may expand into colon-separated list) but no 
 *  mixed expansion (like "$HOME/bin").
*/

void addpath (const char *name)
{
   struct incpath *path, *last = NULL;
   char pathname[PATH_MAX];
   int ipos = 0;
   if ( name == NULL )
      return;
   if ( *name == '\0' )
      return;
   /* Was an environment variable given? */
   if ( *name == '$' && strchr(name,'/') == NULL )
   {
      /* Use the value of the environment variable, if available */
      name = getenv(name+1);
      if ( name == NULL )
         return;
   }
   /* Find the end of the configured list of paths. */
   for ( path = root_path; path != NULL; path = path->next )
   {
      last = path;
      if ( strcmp(name,path->path) == 0 )
         return;
   }
   /* Add the path or each path in the colon-separated list of paths */
   while ( getword(name,&ipos,pathname,sizeof(pathname)-1,':','\n') > 0 )
   {
      if ( last == NULL )
         path = root_path = (struct incpath *) calloc(1,sizeof(struct incpath));
      else
         path = last->next = (struct incpath *) calloc(1,sizeof(struct incpath));
      if ( path != NULL )
         path->path = strdup(pathname);
      if ( last == NULL )
         last = root_path;
      else
         last = last->next;
   }

   if ( !foei_done )
      fileopen_env_init();

   if ( verbose )
      fprintf(stderr,"Adding '%s' to fileopen search path.\n", name);
}

/** 
 *  @short Add a path to the list of execution paths, if not already there.
 *  The path name is always copied to a newly allocated memory location.
 *  This path name can actually be a colon-separated list, as for initexepath().
*/

void addexepath (const char *name)
{
   /* We use the addpath function with temporarily exchanged paths. */
   struct incpath *tmp_path = root_path;
   root_path = root_exe_path;
   addpath(name);
   root_exe_path = root_path;
   root_path = tmp_path;
}

/** Helper function for opening a pipe from or to a given program. */

static FILE *exe_popen (const char *fname, const char *mode)
{
   char tpath[PATH_MAX+20000]; /* must also accomodate for program arguments */
   const char *lp = NULL;
   size_t np = 0;
   size_t len_fn = 0;
   struct incpath *path;
   const char *s;
   int in_string = 0;

   if ( fname == NULL || mode == NULL )
      return NULL;
   len_fn = strlen(fname);

   /* Some restrictions apply, although not foolproof secure */
   for ( s=fname; *s != '\0'; s++ )
   {
      if ( *s == '"' )
      {
         if ( in_string==0 )
            in_string = 1;
         else if ( in_string==1 )
            in_string = 0;
      }
      if ( *s == '\'' )
      {
         if ( in_string==0 )
            in_string = 2;
         else if ( in_string==2 )
            in_string = 0;
      }
      if ( permissive_pipes )
      {
         /* Backslash escaping and non-pipe commands not allowed. */
         if ( *s == '\\' || 
               ( ( *s == ';' || (s[0]=='|' && s[1]=='|') || 
                  (s[0]=='&' && s[1]=='&') ) && !in_string ) )
         {
            errno=EINVAL;
            return NULL;
         }
      }
      else
      {
         /* Backslash escaping and further commands not allowed. */
         if ( *s == '\\' || 
               ( ( *s == ';' || *s == '|' || *s == '&' || *s == '\n' ) && !in_string ) )
         {
            errno=EINVAL;
            return NULL;
         }
      }
   }

   /* Search for path separating '/' but only up to first blank 
      to avoid mistaking '/' in parameters for end of path. */
   for ( s=fname; *s!='\0' && *s!=' '; s++ )
   {
      if ( *s == '/' )
         lp = s;
   }
   if ( lp != NULL )
      np = lp - fname;

   errno = 0;

   /* Not permissive and no execution path is a permission failure */
   if ( root_exe_path == NULL && !permissive_pipes )
   {
      errno = EPERM;
      return NULL;
   }

   if ( permissive_pipes )
   {
      if ( verbose )
         fprintf(stderr,"Trying permissive execution of program %s\n", fname);
      /* Explicit path given */
      if ( lp != NULL )
      {
         if ( verbose )
            fprintf(stderr,"Pipe %s program '%s' being started now.\n",
              mode[0]=='r' ? "from" : "to", fname);
         return popenx(fname,mode);
      }
      /* No explicit path and no path list: try current working directory */
      if ( root_exe_path == NULL && len_fn + 2 < sizeof(tpath) )
      {
         strcpy(tpath,"./");
         strcat(tpath,fname);
         if ( verbose )
            fprintf(stderr,"Pipe %s program '%s' being started now.\n",
              mode[0]=='r' ? "from" : "to", tpath);
         return popenx(tpath,mode);
      }
      /* Try each of the configured execution paths for a matching program */
      for ( path = root_exe_path; path != NULL; path = path->next )
      {
         size_t len_path;
         if ( path->path == NULL )
            continue;
         len_path = strlen(path->path);
         if ( len_path+2+len_fn < sizeof(tpath) )
         {
            char *b = NULL;
            strcpy(tpath,path->path);
            strcat(tpath,"/");
            strcat(tpath,fname);
            /* Temporarily clip at start of arguments before checking if program exists */
            b = strchr(tpath+len_path,' ');
            if ( b != NULL )
               *b = '\0';
            if ( access(tpath,X_OK) != 0 )
               continue;
            if ( verbose )
               fprintf(stderr,"Pipe %s program '%s' being started now.\n",
                 mode[0]=='r' ? "from" : "to", tpath);
            /* Recover arguments */
            if ( b != NULL )
               *b = ' ';
            return popenx(tpath,mode);
         }
      }
      errno = ENOENT;
      return NULL;
   }

   /* Not permissive but some execution paths exist */
   for ( path = root_exe_path; path != NULL; path = path->next )
   {
      size_t len_path;
      if ( path->path == NULL )
         continue;
      len_path = strlen(path->path);
      if ( len_path+2+len_fn < sizeof(tpath) )
      {
         char *b = NULL;
         if ( lp != NULL )
         {
            /* If we have an explicit path it must match a configured path */
            if ( strncmp(path->path,fname,np) != 0 || len_path != np )
               continue;
            strcpy(tpath,fname);
         }
         else
         {
            strcpy(tpath,path->path);
            strcat(tpath,"/");
            strcat(tpath,fname);
         }
         /* Temporarily clip at start of arguments before checking if program exists */
         b = strchr(tpath+len_path,' ');
         if ( b != NULL )
            *b = '\0';
         if ( access(tpath,X_OK) != 0 )
            continue;
         if ( verbose )
            fprintf(stderr,"Pipe %s program '%s' being started now.\n",
              mode[0]=='r' ? "from" : "to", tpath);
         /* Recover arguments */
         if ( b != NULL )
            *b = ' ';
         return popenx(tpath,mode);
      }
   }
   if ( lp != NULL )
   {
      char *b = NULL;
      strncpy(tpath,fname,sizeof(tpath)-1);
      b = strchr(tpath,' ');
      if ( b != NULL )
         *b = '\0';
      if ( access(tpath,X_OK) == 0 )
         errno = EPERM; /* The explicit path is not among configured */
      else
         errno = ENOENT; /* No such explicit program */
   }
   else
      errno = ENOENT; /* No matching entry among configured paths */
   return NULL;
}

/** Helper function for opening a compressed file through a fifo. */

static FILE *cmp_popen (const char *fname, const char *mode, int compression)
{
   const char *cmd = NULL; /* Fifo command without input/output file name */
   char *s;          /* Full fifo command with redirection */
   FILE *f = NULL;   /* File pointer returned */
   const char *pmd = "?";  /* Fifo mode ("w" or "r") */
   const char *cs = "";

   if ( verbose > 5 )
      fprintf(stderr,"fileopen(fname=%s, mode=%s, compression=%d)\n", fname, mode,compression);

   int rc = 0;

   if ( fname == NULL || mode == NULL )
      return NULL;

   errno = 0;
   switch ( *mode )
   {
      case 'w':
         /* If we have no write access, we do not try to open the */
         /* fifo since it would result in a broken pipe later. */
         if ( (rc=access(fname,W_OK)) != 0 )
         {
            if ( errno != ENOENT )
            {
               int k = errno;
               fprintf(stderr,"Cannot write to ");
               perror(fname);
               errno = k;
               return NULL;
            }
            /* What is missing is to check for write permissions in directory. */
         }
         pmd = "w";
         switch ( compression )
         {
            case 1:
               cmd = parallel ? "pigz -c >" : "gzip -c >";
               break;
            case 2:
               cmd = parallel ? "pbzip2 -c >" : "bzip2 -c >";
               break;
            case 3:
               cmd = "lzop -c >";
               break;
            case 4:
               cmd = "lzma -c >";
               break;
            case 5:
               cmd = "xz -c >";
               break;
            case 6:
               cmd = "lz4 -c >";
               break;
            case 10:
               cmd = "zstd -c >";
               break;
         }
         break;
      case 'a':
         /* If we have no write access, we do not try to open the */
         /* fifo since it would result in a broken pipe later. */
         if ( (rc=access(fname,W_OK)) != 0 )
         {
            if ( errno != ENOENT )
            {
               int k = errno;
               fprintf(stderr,"Cannot write to ");
               perror(fname);
               errno = k;
               return NULL;
            }
            /* What is missing is to check for write permissions in directory 
               if the file to append does not exist yet (and thus cannot be created). */
         }
         pmd = "w";
         switch ( compression )
         {
            case 1:
               cmd = parallel ? "pigz -c >>" : "gzip -c >>";
               break;
            case 2:
               cmd = parallel ? "pbzip2 -c >>" : "bzip2 -c >>";
               break;
            case 3:
               cmd = "lzop -c >>";
               break;
            case 4:
               cmd = "lzma -c >>";
               break;
            case 5:
               cmd = "xz -c >>";
               break;
            case 6:
               cmd = "lz4 -c >>";
               break;
            case 10:
               cmd = "zstd -c >>";
               break;
         }
         break;
      case 'r':
         /* If we have no read access, we do not try to open the */
         /* fifo since it would result in a broken pipe later. */
         if ( (rc=access(fname,R_OK)) != 0 )
            return NULL;
         pmd = "r";
         switch ( compression )
         {
            case 1:
               cmd = parallel ? "pigz -d -c <" : "gzip -d -c <";
               break;
            case 2:
               cmd = parallel ? "pbzip2 -d -c <" : "bzip2 -d -c <";
               break;
            case 3:
               cmd = "lzop -d -c <";
               break;
            case 4:
               cmd = "lzma -d -c <";
               break;
            case 5:
               cmd = "xz -d -c <";
               break;
            case 6:
               cmd = "lz4 -d -c <";
               break;
            case 7:
               cmd = "tar zxOf - <";
               break;
            case 8:
               cmd = "unzip -p ";
               break;
            case 9:
               cmd = "tar xOf - <";
               cs = " | zcat";
               break;
            case 10:
               cmd = "zstd -d -c <";
               break;
            case 11:
               cmd = "tar xOf - <";
               cs = " | zstd -d -c";
               break;
         }
         break;
   }

   if ( cmd == NULL )
   {
      if ( verbose > 2 )
         fprintf(stderr,"No command to pipe to!\n");
      errno = EPERM;
      return NULL;
   }

   /* Allocate string for concatenating actual command */
   s = (char *) malloc(5+strlen(cmd)+strlen(fname)+strlen(cs)+3);
   if ( s == NULL )
      return NULL;

   /* At this point we could check if the requested (de)compression program exists ... */

   /* Usually we insert a "exec " in front of the command to avoid having an extra shell kept running. */
   if ( with_exec )
   {
      strcpy(s,"exec ");
      strcat(s,cmd);
   }
   else
      strcpy(s,cmd);
   strcat(s,"'");
   strcat(s,fname);
   strcat(s,"'");
   strcat(s,cs);

   if ( report )
      fileopen_add_report(fname,pmd);

   f = popenx(s,pmd);
   if ( verbose )
   {
      if ( f != NULL )
         fprintf(stderr,"Fileopen success: mode '%s' with command %s\n", pmd, s);
      else
         fprintf(stderr,"Fileopen failed: mode '%s' with command %s\n", pmd, s);
   }
   /* If the compression program does not exist, we may want to try a fallback
      without compression, but that is actually not what is going to happen. Too bad. */
   /* What happens instead is that a pipe gets opened but is unusable and fails later. */
   if ( f == NULL && *mode == 'w' && with_fallback )
   {
      char *x;
      if ( verbose )
         fprintf(stderr,"Fileopen with compression failed. Trying fall-back without compression.\n");
      strcpy(s,fname);
      x = strrchr(s,'.');
      if ( x != NULL )
         *x = '\0';
      f = fopenx(s,mode);
      if ( f != NULL )
         fprintf(stderr,"Fall-back to file %s (without .%s) succeeded.\n",s,x+1);
      else if ( verbose )
         fprintf(stderr,"Fall-back to file %s (without .%s) failed as well.\n",s,x+1);
   }

   /* Free allocated string */
   free(s);

   return f;
}

/** Helper function for opening a file with a URI (http:// etc.). */

static FILE *uri_popen (const char *fname, const char *mode, int compression);

static FILE *uri_popen (const char *fname, const char *mode, int compression)
{
   const char *get_cmd = "curl -s -S --netrc-optional "; /* Fifo command without input file name */
   const char *cmp_cmd = "";  /* Decompression of data */
   char *s;             /* Full fifo command with redirection */
   FILE *f = NULL;      /* File pointer returned */

   if ( fname == NULL || mode == NULL )
      return NULL;

   if ( *mode != 'r' ) /* Only read-mode is supported. */
   {
      fprintf(stderr,"Cannot write to %s.\n", fname);
      errno = EPERM;
      return NULL;
   }
   errno = 0;
   
   switch ( compression )
   {
      case 1:
         cmp_cmd = parallel ? " | pigz -d" : " | gzip -d";
         break;
      case 2:
         cmp_cmd = parallel ? " | pbzip2 -d" : " | bzip2 -d";
         break;
      case 3:
         cmp_cmd = " | lzop -d";
         break;
      case 4:
         cmp_cmd = " | lzma -d";
         break;
      case 5:
         cmp_cmd = " | xz -d";
         break;
      case 6:
         cmp_cmd = " | lz4 -d";
         break;
      case 7:
         cmp_cmd = " | tar zxOf -";
         break;
      case 9:
         cmp_cmd = " | tar xOf - | zcat";
         break;
      case 10:
         cmp_cmd = " | zstd -d";
         break;
      case 11:
         cmp_cmd = " | tar xOf - | zstd -d";
         break;
   }

   s = (char *) malloc(strlen(get_cmd)+strlen(fname)+strlen(cmp_cmd)+3);
   if ( s == NULL )
      return NULL;
   strcpy(s,get_cmd);
   strcat(s,"'");
   strcat(s,fname);
   strcat(s,"'");
   strcat(s,cmp_cmd);

   f = popenx(s,"r");
   if ( verbose )
   {
      if ( f != NULL )
         fprintf(stderr,"Fileopen success: mode 'r' with command %s\n", s);
      else
         fprintf(stderr,"Fileopen failed: mode 'r' with command %s\n", s);
   }
   free(s);

   return f;
}

/** Helper function for opening a file on a remote SSH server */

static FILE *ssh_popen (const char *fname, const char *mode, int compression);

static FILE *ssh_popen (const char *fname, const char *mode, int compression)
{
   const char *get_cmd = "ssh '%s' \"cat '%s'\"%s"; /* Fifo command to be filled in. */
   const char *cmp_cmd = "";  /* Decompression of data */
   const char *s;
   const char *c;
   char *t;
   FILE *f = NULL;      /* File pointer returned */
   size_t i, n;
   char remote_loc[256]; /* user@host format, intentionally no password support */
   const char *remote_fn = NULL; /* relative or absolute path on remote host */

   errno = EPROTO;

   if ( fname == NULL || mode == NULL )
      return NULL;

   if ( strncmp(fname,"ssh://",6) != 0 )
      return NULL;

   /* Check for some shell-relevant ASCII characters, even though the filename is in quotes. */
   for ( i=0, n=strlen(fname); i<n; i++ )
      if ( fname[i] < 32 || fname[i] == 127 || 
           fname[i] == ';' ||
           fname[i] == '$' ||
           fname[i] == '\\' ||
           fname[i] == '>' || /* No worries about '<' */
           fname[i] == '&' ||
           fname[i] == '!' ||
           fname[i] == '|' ||
           fname[i] == 96 /* back-quote */)
      {
         fprintf(stderr,"Invalid character in '%s'.\n", fname);
         errno = EPERM;
         return NULL;
      }

   s = (const char *) strchr(fname+7,':');
   if ( s == NULL )
      s = strchr(fname+7,'/'); /* Try other separator */
   if ( s == NULL )
      return NULL;
   if ( (size_t)(s - fname - 7) >= sizeof(remote_loc) )
      return NULL;
   for (n=0, c=fname+6; c!=s; c++, n++)
      remote_loc[n] = *c;
   remote_loc[n] = '\0';
   remote_fn = s+1;

   if ( *mode != 'r' ) /* Only read-mode is supported. */
   {
      fprintf(stderr,"Cannot write to %s.\n", fname);
      errno = EPERM;
      return NULL;
   }
   errno = 0;
   
   switch ( compression )
   {
      case 1:
         cmp_cmd = parallel ? " | pigz -d" : " | gzip -d";
         break;
      case 2:
         cmp_cmd = parallel ? " | pbzip2 -d" : " | bzip2 -d";
         break;
      case 3:
         cmp_cmd = " | lzop -d";
         break;
      case 4:
         cmp_cmd = " | lzma -d";
         break;
      case 5:
         cmp_cmd = " | xz -d";
         break;
      case 6:
         cmp_cmd = " | lz4 -d";
         break;
      case 7:
         cmp_cmd = " | tar zxOf -";
         break;
      case 8:
         errno = ENOTSUP;
         return NULL;
      case 9:
         cmp_cmd = " | tar xOf - | zcat";
         break;
      case 10:
         cmp_cmd = " | zstd -d";
         break;
      case 11:
         cmp_cmd = " | tar xOf - | zstd -d";
         break;
   }

   n = strlen(get_cmd)+strlen(fname)+strlen(cmp_cmd)+1;
   t = (char *) malloc(n);
   if ( t == NULL )
      return NULL;
   snprintf(t,n,get_cmd,remote_loc,remote_fn,cmp_cmd);

   f = popenx(t,"r");
   if ( verbose )
   {
      if ( f != NULL )
         fprintf(stderr,"Fileopen success: mode 'r' with command %s\n", s);
      else
         fprintf(stderr,"Fileopen failed: mode 'r' with command %s\n", s);
   }
   free(t);

   return f;
}
static FILE *xrootd_popen(const char *fname, const char *mode, int compression);
static FILE *xrootd_popen(const char *fname, const char *mode, int compression)
{
    char get_cmd[1024];
    snprintf(get_cmd, sizeof(get_cmd), "xrdcp -f %s -", fname);
    const char *cmp_cmd = "";  /* Decompression of data */
    char *s;             /* Full fifo command with redirection */
    FILE *f = NULL;      /* File pointer returned */

    if (fname == NULL || mode == NULL)
        return NULL;

    if (*mode != 'r') /* Only read-mode is supported. */
    {
        fprintf(stderr, "Cannot write to %s.\n", fname);
        errno = EPERM;
        return NULL;
    }
    errno = 0;
    
    /* Set up compression command based on compression type */
    switch (compression)
    {
        case 1:
            cmp_cmd = parallel ? " | pigz -d" : " | gzip -d";
            break;
        case 2:
            cmp_cmd = parallel ? " | pbzip2 -d" : " | bzip2 -d";
            break;
        case 3:
            cmp_cmd = " | lzop -d";
            break;
        case 4:
            cmp_cmd = " | lzma -d";
            break;
        case 5:
            cmp_cmd = " | xz -d";
            break;
        case 6:
            cmp_cmd = " | lz4 -d";
            break;
        case 7:
            cmp_cmd = " | tar zxOf -";
            break;
        case 9:
            cmp_cmd = " | tar xOf - | zcat";
            break;
        case 10:
            cmp_cmd = " | zstd -d";
            break;
        case 11:
            cmp_cmd = " | tar xOf - | zstd -d";
            break;
    }

    s = (char *) malloc(strlen(get_cmd) + strlen(fname) + strlen(cmp_cmd) + 3);
    if (s == NULL)
        return NULL;
    strcpy(s, get_cmd);
    strcat(s, cmp_cmd);

    f = popenx(s, "r");
    if (verbose)
    {
        if (f != NULL)
            fprintf(stderr, "Fileopen success: mode 'r' with command %s\n", s);
        else
            fprintf(stderr, "Fileopen failed: mode 'r' with command %s\n", s);
    }
    free(s);

    return f;
}

/** Search for a file in the include path list and open it if possible. */

FILE *fileopen (const char *fname, const char *mode)
{
   struct incpath *path = root_path;
   char try_fname[PATH_MAX];
   FILE *f = NULL;
   int nerr = 0;
   int compression = 0;
   int l;
   
   /* The set of search paths might not be initialized yet */
   if ( path == NULL )
   {
      initpath(NULL);
      if ( (path = root_path) == NULL )
         return NULL;
   }
   /* Arguments could be incomplete */
   if ( fname == NULL || mode == NULL )
      return NULL;

   if ( verbose )
   {
      fprintf(stderr,"\nfileopen(\"%s\", \"%s\")\n", fname, mode);
   }

   if ( strcmp(fname,"-") == 0 ) /* denotes standard input or standard output */
   {
      if ( *mode == 'r' )
         return stdin;
      else
         return stdout;
   }

   if ( !foei_done )
      fileopen_env_init();

   /* An input or output pipe from/to a user program */
   if ( fname[0] == '|' )
   {
      f = exe_popen(fname+1,mode);

      if ( verbose )
      {
         if ( f != NULL )
            fprintf(stderr,"Fileopen success: mode '%s' on program '%s'\n", mode, fname+1);
         else
            fprintf(stderr,"Fileopen failed: mode '%s' on program '%s'\n", mode, fname+1);
      }
      return f;
   }

   /* Check if compressed files are meant. */
   l = strlen(fname);
   if ( l > 3 && strcmp(fname+l-3,".gz") == 0 )
      compression = 1;
   else if ( l > 4 && strcmp(fname+l-4,".bz2") == 0 )
      compression = 2;
   else if ( l > 4 && strcmp(fname+l-4,".lzo") == 0 )
      compression = 3;
   else if ( l > 5 && strcmp(fname+l-5,".lzma") == 0 )
      compression = 4;
   else if ( l > 3 && strcmp(fname+l-3,".xz") == 0 )
      compression = 5;
   else if ( l > 4 && strcmp(fname+l-4,".lz4") == 0 )
      compression = 6;
   else if ( l > 7 && strcmp(fname+l-7,".tar.gz") == 0 )
      compression = 7;
   else if ( l > 4 && strcmp(fname+l-4,".zip") == 0 )
      compression = 8;
   else if ( l > 7 && strcmp(fname+l-7,".gz.tar") == 0 )
      compression = 9;
   else if ( l > 7 && strcmp(fname+l-4,".zst") == 0 )
      compression = 10;
   else if ( l > 7 && strcmp(fname+l-8,".zst.tar") == 0 )
      compression = 11;

   if ( strchr(fname,':') != NULL )
   {
      if ( strncmp(fname,"http://",7) == 0 ||
           strncmp(fname,"https://",8) == 0 ||
           strncmp(fname,"ftp://",6) == 0 ||
           strncmp(fname,"file://",7) == 0 )
         return uri_popen(fname,mode,compression);
      if ( strncmp(fname,"ssh://",6) == 0 )
         return ssh_popen(fname,mode,compression);
      if (strncmp(fname,"root://",7) == 0)
         return xrootd_popen(fname,mode,compression);
   }

   /* For modes other than read-only, no search is done. */
   if ( *mode != 'r' )
   {
      switch (compression)
      {
         case 0: /* Normal file */
            f = fopenx(fname,mode);
            if ( verbose )
            {
               if ( f != NULL )
                  fprintf(stderr,"Fileopen success: mode '%s' on file '%s'\n", mode, fname);
               else
                  fprintf(stderr,"Fileopen failed: mode '%s' on file '%s'\n", mode, fname);
            }
            return f;
            break;

         case 1: /* Create FIFO to gzip to file */
         case 2: /* Create FIFO to bzip2 to file */
         case 3: /* Create FIFO to lzop to file */
         case 4: /* Create FIFO to lzma to file */
         case 5: /* Create FIFO to xz to file */
         case 6: /* Create FIFO to lz4 to file */
         case 10: /* Create FIFO to zstd to file */
            return cmp_popen(fname,mode,compression);
            break;

         default:
            return NULL;
      }
   }            

   /* If the name includes (part of) a path, no search is done. */
   if ( strchr(fname,'/') != NULL )
   {
      switch (compression)
      {
         case 0: /* Normal file */
            f = fopenx(fname,mode);
            if ( verbose )
            {
               if ( f != NULL )
                  fprintf(stderr,"Fileopen success: mode '%s' on file '%s'\n", mode, fname);
               else
                  fprintf(stderr,"Fileopen failed: mode '%s' on file '%s'\n", mode, fname);
            }
            return f;
            break;

         case 1: /* Create FIFO from gunzip from file */
         case 2: /* Create FIFO from bunzip2 from file */
         case 3: /* Create FIFO from lzop from file */
         case 4: /* Create FIFO from lzma from file */
         case 5: /* Create FIFO from xz from file */
         case 6: /* Create FIFO from lz4 from file */
         case 7: /* Create FIFO from tar unpacking uncompressed files from compressed tar package to stdout */
         case 8: /* Create FIFO from zip unpacking uncompressed files from zip archive to stdout */
         case 9: /* Create FIFO from tar unpacking gzip compressed files from uncompressed tar package to stdout */
         case 10: /* Create FIFO from zstd from file */
         case 11: /* Create FIFO from tar unpacking zstd compressed files from uncompressed tar package to stdout */
            return cmp_popen(fname,mode,compression);
            break;

         default:
            return NULL;
      }
   }            

   /* Now try all the paths we have */
   for ( ; path != NULL; path=path->next )
   {
#ifdef __USE_LARGEFILE64
      struct stat64 st;
#else
      struct stat st;
#endif
      if ( path->path == NULL )
         continue;
      if ( strlen(path->path)+strlen(fname)+2 >= sizeof(try_fname) )
         continue;
      /* We just checked that the combined name fits, so no point in
         having a stupid compiler complaining about a possible overflow. */
#pragma GCC diagnostic push
#ifdef __GNUC__
# if __GNUC__ > 6
#pragma GCC diagnostic ignored "-Wformat-overflow"
# endif
#endif
      if ( strcmp(path->path,".") == 0 )
         strcpy(try_fname,fname); /* No need to expand local path */
      else
         sprintf(try_fname,"%s/%s",path->path,fname);
      if ( verbose )
      {
         fprintf(stderr,"Try file '%s' in directory '%s' as '%s'.\n",
             fname, path->path, try_fname);
      }
#pragma GCC diagnostic pop

      errno = 0;
      /* Check if the file is available */
#ifdef __USE_LARGEFILE64
      if ( stat64(try_fname,&st) != 0 )
#else
      if ( stat(try_fname,&st) != 0 )
#endif
      {
         if ( verbose && errno == ENOENT )
         {
            fprintf(stderr,"No file named '%s' in directory '%s'\n", fname, path->path);
         }
         if ( errno != 0 && errno != ENOENT )
         {
            perror(try_fname);
            nerr++;
         }
         continue;
      }

      errno = 0;
      /* The file exists and is hopefully readable */
      switch (compression)
      {
         case 0: /* Normal file */
            if ( (f = fopenx(try_fname,mode)) != NULL )
            {
               if ( verbose )
               {
                  fprintf(stderr,"Fileopen success: mode '%s' on file '%s'\n", mode, try_fname);
               }
               return f;
            }
            break;

         case 1: /* Create FIFO from gunzip from file */
         case 2: /* Create FIFO from bunzip2 from file */
         case 3: /* Create FIFO from lzop from file */
         case 4: /* Create FIFO from lzma from file */
         case 5: /* Create FIFO from xz from file */
         case 6: /* Create FIFO from lz4 from file */
         case 7: /* Create FIFO from tar unpacking uncompressed files from compressed tar package to stdout */
         case 8: /* Create FIFO from zip unpacking uncompressed files from zip archive to stdout */
         case 9: /* Create FIFO from tar unpacking gzip compressed files from uncompressed tar package to stdout */
         case 10: /* Create FIFO from zstd from file */
         case 11: /* Create FIFO from tar unpacking zstd compressed files from uncompressed tar package to stdout */
            if ( (f = cmp_popen(try_fname,"r",compression)) != NULL )
               return f;
            break;

         default:
            return NULL;
      }

      if ( errno != 0 && errno != ENOENT )
      {
         perror(try_fname);
         nerr++;
      }
   }

   if ( f != NULL )
      return f;

#ifdef SAVE_ERRNO
   if ( s_errno != 0 )
      errno = s_errno;
   s_errno = 0;
#endif

   if ( f == NULL && nerr == 0 )
   {
      fprintf(stderr,"%s: Not found in any include directory.\n",fname);
      if ( errno == 0 )
         errno = ENOENT;
   }

   return NULL;
}

/** Close a file or fifo but not if it is one of the standard streams. */

int fileclose (FILE *f)
{
#ifdef __USE_LARGEFILE64
   struct stat64 st;
#else
   struct stat st;
#endif
   int rc = 0;
   int input_only = 0;
   int fno = -1;

   if ( f == NULL )
      return -1;

   errno = 0;

   /* We never close the standard streams here. */
   if ( f == stdin || f == stdout || f == stderr )
      return 0;

   if ( !foei_done )
      fileopen_env_init();

   /* Check what kind of stream we have */
   if ( (fno=fileno(f)) == -1 )
   {
      fprintf(stderr,"Trying to close stream: no file handle\n");
      errno = EBADF;
      return -1;
   }

#ifdef __USE_LARGEFILE64
   if ( fstat64(fno,&st) != 0 )
#else
   if ( fstat(fno,&st) != 0 )
#endif
   {
      if ( errno != 0 )
         perror("Trying to close stream");
      errno = EBADF;
      return -1;
   }

#ifdef __GLIBC__
   /* Note: works with gcc and clang on Linux but may be glibc-specific: */
   if ( f->_IO_write_base == f->_IO_write_end )
      input_only = 1;
#endif

#ifdef S_ISFIFO
   if ( S_ISFIFO(st.st_mode) )
#else
   if ( (st.st_mode & S_IFMT) == S_IFIFO )
#endif
   {
      if ( verbose )
      {
#ifdef __GLIBC__
         fprintf(stderr,"Closing now %s pipe on fileno=%d, mode=%08x\n", input_only ? "input" : "output", fno,st.st_mode);
#else
         fprintf(stderr,"Closing now pipe on fileno=%d, mode=%08x\n", fno,st.st_mode);
#endif
      }
      errno = 0;
#ifdef __GLIBC__
      /* We should actually do this only for output pipes: */
      if ( ! input_only )
      {
         /* May result in an error message when applied to input files on non-glibc (Linux) systems. */
         rc = fflush(f); /* Not sure if that helps to make sure the connected program is finished with pclose, but cannot harm. */
         if ( rc != 0 || errno != 0 )
         {
            /* fprintf(stderr,"fflush: (rc=%d, errno=%d, fileno=%d)\n",rc,errno,fileno(f)); */
            if ( verbose )
               perror("Trying to flush stream before closing"); /* Always fails on input files. */
            errno = 0;
         }
      }
#endif
      /* fsync(fileno(f)); */ /* If not, perhaps that would help. */
      rc = pclose(f); /* According to documentation, the program should be finished then - but I have my doubts. */
      if ( rc == -1 && errno != 0 )
      {
#ifdef __GLIBC__
         perror(input_only ? "Trying to close input stream" : "Trying to close stream"); /* An error as specified in the POSIX etc. standards */
#else
         perror("Trying to close stream"); /* An error as specified in the POSIX etc. standards */
#endif
      }
      else if ( rc != 0 )
      {
         if ( rc == -1 ) /* errno = 0 ? */
#ifdef __GLIBC__
            fprintf(stderr,"Trying to close %s stream apparently failed (rc=%d, errno=%d, fileno=%d)\n",
               input_only ? "input" : "output", rc, errno, fno);
#else
            fprintf(stderr,"Trying to close stream apparently failed (rc=%d, errno=%d, fileno=%d)\n",
               rc, errno, fno);
#endif
         else if ( verbose >= 2 ) /* Non-standard return codes could come from the executed command. */
            fprintf(stderr,"Non-standard return code from pclose (rc=%d, errno=%d, fileno=%d)\n",rc,errno,fno);
      }
   }
   else
   {
      if ( verbose )
      {
#ifdef __GLIBC__
         fprintf(stderr,"Closing now %s file on fileno=%d, mode=%08x\n", input_only ? "input" : "output", fno, st.st_mode);
#else
         fprintf(stderr,"Closing now file on fileno=%d, mode=%08x\n", fno, st.st_mode);
#endif
      }
      rc = fclose(f);
   }

   /* In case of remembering which file handle was which file, we would reset that there. */

   if ( rc != 0 )
   {
      if ( errno == 0 )
         errno = EBADMSG;
   }
   return rc;
}

