/*
 * Mini tar implementation for busybox based on code taken from sash.
 *
 * Copyright (c) 1999 by David I. Bell
 * Permission is granted to use, distribute, or modify this source,
 * provided that this copyright notice remains intact.
 *
 * Permission to distribute this code under the GPL has been granted.
 *
 * Modified for busybox by Erik Andersen <andersee@debian.org>
 * Adjusted to grok stdin/stdout options.
 *
 * Modified to handle device special files by Matt Porter
 * <porter@debian.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */


#include "internal.h"
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <utime.h>
#include <sys/types.h>
#include <sys/sysmacros.h>


#ifdef BB_FEATURE_TAR_CREATE

static const char tar_usage[] =
"tar -[cxtvOf] [tarFileName] [FILE] ...\n\n"
"Create, extract, or list files from a tar file.\n\n"
"Options:\n"
"\tc=create, x=extract, t=list contents, v=verbose,\n"
"\tO=extract to stdout, f=tarfile or \"-\" for stdin\n";

#else

static const char tar_usage[] =
"tar -[xtvOf] [tarFileName] [FILE] ...\n\n"
"Extract, or list files stored in a tar file.  This\n"
"version of tar does not support creation of tar files.\n\n"
"Options:\n"
"\tx=extract, t=list contents, v=verbose,\n"
"\tO=extract to stdout, f=tarfile or \"-\" for stdin\n";

#endif


/*
 * Tar file constants.
 */
#define TAR_BLOCK_SIZE	512
#define TAR_NAME_SIZE	100


/*
 * The POSIX (and basic GNU) tar header format.
 * This structure is always embedded in a TAR_BLOCK_SIZE sized block
 * with zero padding.  We only process this information minimally.
 */
typedef struct {
    char name[TAR_NAME_SIZE];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checkSum[8];
    char typeFlag;
    char linkName[TAR_NAME_SIZE];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devMajor[8];
    char devMinor[8];
    char prefix[155];
} TarHeader;

#define	TAR_MAGIC	"ustar"
#define	TAR_VERSION	"00"

#define	TAR_TYPE_REGULAR	'0'
#define	TAR_TYPE_HARD_LINK	'1'
#define	TAR_TYPE_SOFT_LINK	'2'


/*
 * Static data.
 */
static int listFlag;
static int extractFlag;
static int createFlag;
static int verboseFlag;
static int tostdoutFlag;

static int inHeader; // <- check me
static int badHeader;
static int errorFlag;
static int skipFileFlag;
static int warnedRoot;
static int eofFlag;
static long dataCc;
static int outFd;
static const char *outName;

static int mode;
static int uid;
static int gid;
static time_t mtime;

/*
 * Static data associated with the tar file.
 */
static const char *tarName;
static int tarFd;
static dev_t tarDev;
static ino_t tarInode;


/*
 * Local procedures to restore files from a tar file.
 */
static void readTarFile (int fileCount, char **fileTable);
static void readData (const char *cp, int count);
static long getOctal (const char *cp, int len);

static void readHeader (const TarHeader * hp,
			int fileCount, char **fileTable);

static int wantFileName (const char *fileName,
			 int fileCount, char **fileTable);

#ifdef BB_FEATURE_TAR_CREATE
/*
 * Local procedures to save files into a tar file.
 */
static void saveFile (const char *fileName, int seeLinks);

static void saveRegularFile (const char *fileName,
			     const struct stat *statbuf);

static void saveDirectory (const char *fileName,
			   const struct stat *statbuf);

static void writeHeader (const char *fileName, const struct stat *statbuf);

static void writeTarFile (int fileCount, char **fileTable);
static void writeTarBlock (const char *buf, int len);
static int putOctal (char *cp, int len, long value);

#endif


extern int tar_main (int argc, char **argv)
{
    const char *options;

    argc--;
    argv++;

    if (argc < 1)
	usage( tar_usage);


    errorFlag = FALSE;
    extractFlag = FALSE;
    createFlag = FALSE;
    listFlag = FALSE;
    verboseFlag = FALSE;
    tostdoutFlag = FALSE;
    tarName = NULL;
    tarDev = 0;
    tarInode = 0;
    tarFd = -1;

    /* 
     * Parse the options.
     */
    if (**argv == '-')
	options = (*argv++) + 1;
    else 
	options = (*argv++);
    argc--;

    for (; *options; options++) {
	switch (*options) {
	case 'f':
	    if (tarName != NULL) {
		fprintf (stderr, "Only one 'f' option allowed\n");

		exit (FALSE);
	    }

	    tarName = *argv++;
	    argc--;

	    break;

	case 't':
	    if (extractFlag == TRUE || createFlag == TRUE )
		goto flagError;
	    listFlag = TRUE;
	    break;

	case 'x':
	    if (listFlag == TRUE || createFlag == TRUE )
		goto flagError;
	    extractFlag = TRUE;
	    break;
	case 'c':
	    if (extractFlag == TRUE || listFlag == TRUE)
		goto flagError;
	    createFlag = TRUE;
	    break;

	case 'v':
	    verboseFlag = TRUE;
	    break;

	case 'O':
	    tostdoutFlag = TRUE;
	    break;

	case '-':
	    usage( tar_usage);
	    break;

	default:
	    fprintf (stderr, "Unknown tar flag '%c'\n"
		    "Try `tar --help' for more information\n", 
		    *options);
	    exit (FALSE);
	}
    }

    /* 
     * Do the correct type of action supplying the rest of the
     * command line arguments as the list of files to process.
     */
    if (createFlag==TRUE) {
#ifndef BB_FEATURE_TAR_CREATE
	fprintf (stderr, "This version of tar was not compiled with tar creation support.\n" );
	exit (FALSE);
#else
	writeTarFile (argc, argv);
#endif 
    } else {
	readTarFile (argc, argv);
    }
    if (errorFlag==TRUE) {
	fprintf (stderr, "\n");
    }
    exit (!errorFlag);

flagError:
    fprintf (stderr, "Exactly one of 'c', 'x' or 't' must be specified\n");
    exit (FALSE);
}


/*
 * Read a tar file and extract or list the specified files within it.
 * If the list is empty than all files are extracted or listed.
 */
static void readTarFile (int fileCount, char **fileTable)
{
    const char *cp;
    int cc;
    int inCc;
    int blockSize;
    char buf[BUF_SIZE];

    skipFileFlag = FALSE;
    badHeader = FALSE;
    warnedRoot = FALSE;
    eofFlag = FALSE;
    inHeader = TRUE;
    inCc = 0;
    dataCc = 0;
    outFd = -1;
    blockSize = sizeof (buf);
    cp = buf;

    /* 
     * Open the tar file for reading.
     */
    if ((tarName == NULL) || !strcmp (tarName, "-")) {
	tarFd = fileno(stdin);
    } else
	tarFd = open (tarName, O_RDONLY);

    if (tarFd < 0) {
	perror (tarName);
	errorFlag = TRUE;
	return;
    }

    /* 
     * Read blocks from the file until an end of file header block
     * has been seen.  (A real end of file from a read is an error.)
     */
    while (eofFlag==FALSE) {
	/* 
	 * Read the next block of data if necessary.
	 * This will be a large block if possible, which we will
	 * then process in the small tar blocks.
	 */
	if (inCc <= 0) {
	    cp = buf;
	    inCc = fullRead (tarFd, buf, blockSize);

	    if (inCc < 0) {
		perror (tarName);
		errorFlag = TRUE;
		goto done;
	    }

	    if (inCc == 0) {
		fprintf (stderr,
			 "Unexpected end of file from \"%s\"", tarName);
		errorFlag = TRUE;
		goto done;
	    }
	}

	/* 
	 * If we are expecting a header block then examine it.
	 */
	if (inHeader==TRUE) {
	    readHeader ((const TarHeader *) cp, fileCount, fileTable);

	    cp += TAR_BLOCK_SIZE;
	    inCc -= TAR_BLOCK_SIZE;

	    continue;
	}

	/* 
	 * We are currently handling the data for a file.
	 * Process the minimum of the amount of data we have available
	 * and the amount left to be processed for the file.
	 */
	cc = inCc;

	if (cc > dataCc)
	    cc = dataCc;

	readData (cp, cc);

	/* 
	 * If the amount left isn't an exact multiple of the tar block
	 * size then round it up to the next block boundary since there
	 * is padding at the end of the file.
	 */
	if (cc % TAR_BLOCK_SIZE)
	    cc += TAR_BLOCK_SIZE - (cc % TAR_BLOCK_SIZE);

	cp += cc;
	inCc -= cc;
    }

  done:
    /* 
     * Close the tar file if needed.
     */
    if ((tarFd >= 0) && (close (tarFd) < 0))
	perror (tarName);

    /* 
     * Close the output file if needed.
     * This is only done here on a previous error and so no
     * message is required on errors.
     */
    if (tostdoutFlag == FALSE) {
	if (outFd >= 0) {
	    close (outFd);
	}
    }
}


/*
 * Examine the header block that was just read.
 * This can specify the information for another file, or it can mark
 * the end of the tar file.
 */
static void
readHeader (const TarHeader * hp, int fileCount, char **fileTable)
{
    int checkSum;
    int cc;
    int hardLink;
    int softLink;
    int devFileFlag;
    unsigned int major;
    unsigned int minor;
    long size;
    struct utimbuf utb;

    /* 
     * If the block is completely empty, then this is the end of the
     * archive file.  If the name is null, then just skip this header.
     */
    outName = hp->name;

    if (*outName == '\0') {
	for (cc = TAR_BLOCK_SIZE; cc > 0; cc--) {
	    if (*outName++)
		return;
	}

	eofFlag = TRUE;

	return;
    }

    /* 
     * There is another file in the archive to examine.
     * Extract the encoded information and check it.
     */
    mode = getOctal (hp->mode, sizeof (hp->mode));
    uid = getOctal (hp->uid, sizeof (hp->uid));
    gid = getOctal (hp->gid, sizeof (hp->gid));
    size = getOctal (hp->size, sizeof (hp->size));
    mtime = getOctal (hp->mtime, sizeof (hp->mtime));
    checkSum = getOctal (hp->checkSum, sizeof (hp->checkSum));
    major = getOctal (hp->devMajor, sizeof (hp->devMajor));
    minor = getOctal (hp->devMinor, sizeof (hp->devMinor));

    if ((mode < 0) || (uid < 0) || (gid < 0) || (size < 0)) {
	if (badHeader==FALSE)
	    fprintf (stderr, "Bad tar header, skipping\n");

	badHeader = TRUE;

	return;
    }

    badHeader = FALSE;
    skipFileFlag = FALSE;
    devFileFlag = FALSE;

    /* 
     * Check for the file modes.
     */
    hardLink = ((hp->typeFlag == TAR_TYPE_HARD_LINK) ||
		(hp->typeFlag == TAR_TYPE_HARD_LINK - '0'));

    softLink = ((hp->typeFlag == TAR_TYPE_SOFT_LINK) ||
		(hp->typeFlag == TAR_TYPE_SOFT_LINK - '0'));

    /* 
     * Check for a directory.
     */
    if (outName[strlen (outName) - 1] == '/')
	mode |= S_IFDIR;

    /* 
     * Check for absolute paths in the file.
     * If we find any, then warn the user and make them relative.
     */
    if (*outName == '/') {
	while (*outName == '/')
	    outName++;

	if (warnedRoot==FALSE) {
	    fprintf (stderr,
		     "Absolute path detected, removing leading slashes\n");
	}

	warnedRoot = TRUE;
    }

    /* 
     * See if we want this file to be restored.
     * If not, then set up to skip it.
     */
    if (wantFileName (outName, fileCount, fileTable) == FALSE) {
	if ( !hardLink && !softLink && (S_ISREG (mode) || S_ISCHR (mode)
		    || S_ISBLK (mode) || S_ISSOCK(mode) || S_ISFIFO(mode) ) ) {
	    inHeader = (size == 0)? TRUE : FALSE;
	    dataCc = size;
	}

	skipFileFlag = TRUE;

	return;
    }

    /* 
     * This file is to be handled.
     * If we aren't extracting then just list information about the file.
     */
    if (extractFlag==FALSE) {
	if (verboseFlag==TRUE) {
	    printf ("%s %3d/%-d ", modeString (mode), uid, gid);
	    if( S_ISCHR (mode) || S_ISBLK (mode) )
		printf ("%4d,%4d %s ", major,minor, timeString (mtime));
	    else
		printf ("%9ld %s ", size, timeString (mtime));
	}
	printf ("%s", outName);

	if (hardLink)
	    printf (" (link to \"%s\")", hp->linkName);
	else if (softLink)
	    printf (" (symlink to \"%s\")", hp->linkName);
	else if (S_ISREG (mode) || S_ISCHR (mode) || S_ISBLK (mode) || 
		S_ISSOCK(mode) || S_ISFIFO(mode) ) {
	    inHeader = (size == 0)? TRUE : FALSE;
	    dataCc = size;
	}

	printf ("\n");

	return;
    }

    /* 
     * We really want to extract the file.
     */
    if (verboseFlag==TRUE)
	printf ("x %s\n", outName);

    if (hardLink) {
	if (link (hp->linkName, outName) < 0) {
	    perror (outName);
	    return;
	}
	/* Set the file time */
	utb.actime = mtime;
	utb.modtime = mtime;
	utime (outName, &utb);
	/* Set the file permissions */
	chown(outName, uid, gid);
	chmod(outName, mode);
	return;
    }

    if (softLink) {
#ifdef	S_ISLNK
	if (symlink (hp->linkName, outName) < 0) {
	    perror (outName);
	    return;
	}
	/* Try to change ownership of the symlink.
	 * If libs doesn't support that, don't bother.
	 * Changing the pointed-to file is the Wrong Thing(tm).
	 */
#if (__GLIBC__ >= 2) && (__GLIBC_MINOR__ >= 1)
	lchown(outName, uid, gid);
#endif

	/* Do not change permissions or date on symlink,
	 * since it changes the pointed to file instead.  duh. */
#else
	fprintf (stderr, "Cannot create symbolic links\n");
#endif
	return;
    }

    /* Set the umask for this process so it doesn't 
     * screw things up. */
    umask(0);

    /* 
     * If the file is a directory, then just create the path.
     */
    if (S_ISDIR (mode)) {
	if (createPath (outName, mode)==TRUE) { 
	    /* Set the file time */
	    utb.actime = mtime;
	    utb.modtime = mtime;
	    utime (outName, &utb);
	    /* Set the file permissions */
	    chown(outName, uid, gid);
	    chmod(outName, mode);
	    return;
	}
    }

    /* 
     * There is a file to write.
     * First create the path to it if necessary with default permissions.
     */
    createPath (outName, 0777);

    inHeader = (size == 0)? TRUE : FALSE;
    dataCc = size;

    /* 
     * Start the output file.
     */
    if (tostdoutFlag == TRUE)
	outFd = fileno(stdout);
    else {
	if ( S_ISCHR(mode) || S_ISBLK(mode) || S_ISSOCK(mode) ) {
	    devFileFlag = TRUE;
	    outFd = mknod (outName, mode, makedev(major, minor) );
	}
	else if (S_ISFIFO(mode) ) {
	    devFileFlag = TRUE;
	    outFd = mkfifo(outName, mode);
	} else {
	    outFd = open (outName, O_WRONLY | O_CREAT | O_TRUNC, mode);
	}
	if (outFd < 0) {
	    perror (outName);
	    skipFileFlag = TRUE;
	    return;
	}
	/* Set the file time */
	utb.actime = mtime;
	utb.modtime = mtime;
	utime (outName, &utb);
	/* Set the file permissions */
	chown(outName, uid, gid);
	chmod(outName, mode);
    }


    /* 
     * If the file is empty, then that's all we need to do.
     */
    if (size == 0 && (tostdoutFlag == FALSE) && (devFileFlag == FALSE)) {
	close (outFd);
	outFd = -1;
    }
}


/*
 * Handle a data block of some specified size that was read.
 */
static void readData (const char *cp, int count)
{
    /* 
     * Reduce the amount of data left in this file.
     * If there is no more data left, then we need to read
     * the header again.
     */
    dataCc -= count;

    if (dataCc <= 0)
	inHeader = TRUE;

    /* 
     * If we aren't extracting files or this file is being
     * skipped then do nothing more.
     */
    if (extractFlag==FALSE || skipFileFlag==TRUE)
	return;

    /* 
     * Write the data to the output file.
     */
    if (fullWrite (outFd, cp, count) < 0) {
	perror (outName);
	if (tostdoutFlag == FALSE) {
	    close (outFd);
	    outFd = -1;
	}
	skipFileFlag = TRUE;
	return;
    }

    /* 
     * Check if we are done writing to the file now.
     */
    if (dataCc <= 0 && tostdoutFlag == FALSE) {
	struct utimbuf utb;
	if (close (outFd))
	    perror (outName);

	/* Set the file time */
	utb.actime = mtime;
	utb.modtime = mtime;
	utime (outName, &utb);
	/* Set the file permissions */
	chown(outName, uid, gid);
	chmod(outName, mode);

	outFd = -1;
    }
}


/*
 * See if the specified file name belongs to one of the specified list
 * of path prefixes.  An empty list implies that all files are wanted.
 * Returns TRUE if the file is selected.
 */
static int
wantFileName (const char *fileName, int fileCount, char **fileTable)
{
    const char *pathName;
    int fileLength;
    int pathLength;

    /* 
     * If there are no files in the list, then the file is wanted.
     */
    if (fileCount == 0)
	return TRUE;

    fileLength = strlen (fileName);

    /* 
     * Check each of the test paths.
     */
    while (fileCount-- > 0) {
	pathName = *fileTable++;

	pathLength = strlen (pathName);

	if (fileLength < pathLength)
	    continue;

	if (memcmp (fileName, pathName, pathLength) != 0)
	    continue;

	if ((fileLength == pathLength) || (fileName[pathLength] == '/')) {
	    return TRUE;
	}
    }

    return FALSE;
}

/*
 * Read an octal value in a field of the specified width, with optional
 * spaces on both sides of the number and with an optional null character
 * at the end.  Returns -1 on an illegal format.
 */
static long getOctal (const char *cp, int len)
{
    long val;

    while ((len > 0) && (*cp == ' ')) {
	cp++;
	len--;
    }

    if ((len == 0) || !isOctal (*cp))
	return -1;

    val = 0;

    while ((len > 0) && isOctal (*cp)) {
	val = val * 8 + *cp++ - '0';
	len--;
    }

    while ((len > 0) && (*cp == ' ')) {
	cp++;
	len--;
    }

    if ((len > 0) && *cp)
	return -1;

    return val;
}




/* From here to the end of the file is the tar writing stuff.
 * If you do not have BB_FEATURE_TAR_CREATE defined, this will
 * not be built.
 * */
#ifdef BB_FEATURE_TAR_CREATE

/*
 * Write a tar file containing the specified files.
 */
static void writeTarFile (int fileCount, char **fileTable)
{
    struct stat statbuf;

    /* 
     * Make sure there is at least one file specified.
     */
    if (fileCount <= 0) {
	fprintf (stderr, "No files specified to be saved\n");
	errorFlag = TRUE;
    }

    /* 
     * Create the tar file for writing.
     */
    if ((tarName == NULL) || !strcmp (tarName, "-")) {
	tostdoutFlag = TRUE;
	tarFd = fileno(stdout);
    } else
	tarFd = open (tarName, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (tarFd < 0) {
	perror (tarName);
	errorFlag = TRUE;
	return;
    }

    /* 
     * Get the device and inode of the tar file for checking later.
     */
    if (fstat (tarFd, &statbuf) < 0) {
	perror (tarName);
	errorFlag = TRUE;
	goto done;
    }

    tarDev = statbuf.st_dev;
    tarInode = statbuf.st_ino;
		
    /* 
     * Append each file name into the archive file.
     * Follow symbolic links for these top level file names.
     */
    while (errorFlag==FALSE && (fileCount-- > 0)) {
	saveFile (*fileTable++, FALSE);
    }

    /* 
     * Now write an empty block of zeroes to end the archive.
     */
    writeTarBlock ("", 1);


  done:
    /* 
     * Close the tar file and check for errors if it was opened.
     */
    if ((tostdoutFlag == FALSE) && (tarFd >= 0) && (close (tarFd) < 0))
	perror (tarName);
}

/*
 * Save one file into the tar file.
 * If the file is a directory, then this will recursively save all of
 * the files and directories within the directory.  The seeLinks
 * flag indicates whether or not we want to see symbolic links as
 * they really are, instead of blindly following them.
 */
static void saveFile (const char *fileName, int seeLinks)
{
    int status;
    struct stat statbuf;

    if (verboseFlag==TRUE)
	printf ("a %s\n", fileName);

    /* 
     * Check that the file name will fit in the header.
     */
    if (strlen (fileName) >= TAR_NAME_SIZE) {
	fprintf (stderr, "%s: File name is too long\n", fileName);

	return;
    }

    /* 
     * Find out about the file.
     */
#ifdef	S_ISLNK
    if (seeLinks==TRUE)
	status = lstat (fileName, &statbuf);
    else
#endif
	status = stat (fileName, &statbuf);

    if (status < 0) {
	perror (fileName);

	return;
    }

    /* 
     * Make sure we aren't trying to save our file into itself.
     */
    if ((statbuf.st_dev == tarDev) && (statbuf.st_ino == tarInode)) {
	fprintf (stderr, "Skipping saving of archive file itself\n");

	return;
    }

    /* 
     * Check the type of file.
     */
    mode = statbuf.st_mode;

    if (S_ISDIR (mode)) {
	saveDirectory (fileName, &statbuf);

	return;
    }
    if (S_ISREG (mode)) {
	saveRegularFile (fileName, &statbuf);

	return;
    }
    
    /* Some day add support for tarring these up... but not today. :) */
//  if (S_ISLNK(mode) || S_ISFIFO(mode) || S_ISBLK(mode) || S_ISCHR (mode) ) {
//	fprintf (stderr, "%s: This version of tar can't store this type of file\n", fileName);
//  }

    /* 
     * The file is a strange type of file, ignore it.
     */
    fprintf (stderr, "%s: not a directory or regular file\n", fileName);
}


/*
 * Save a regular file to the tar file.
 */
static void
saveRegularFile (const char *fileName, const struct stat *statbuf)
{
    int sawEof;
    int fileFd;
    int cc;
    int dataCount;
    long fullDataCount;
    char data[TAR_BLOCK_SIZE * 16];

    /* 
     * Open the file for reading.
     */
    fileFd = open (fileName, O_RDONLY);

    if (fileFd < 0) {
	perror (fileName);

	return;
    }

    /* 
     * Write out the header for the file.
     */
    writeHeader (fileName, statbuf);

    /* 
     * Write the data blocks of the file.
     * We must be careful to write the amount of data that the stat
     * buffer indicated, even if the file has changed size.  Otherwise
     * the tar file will be incorrect.
     */
    fullDataCount = statbuf->st_size;
    sawEof = FALSE;

    while (fullDataCount > 0) {
	/* 
	 * Get the amount to write this iteration which is
	 * the minumum of the amount left to write and the
	 * buffer size.
	 */
	dataCount = sizeof (data);

	if (dataCount > fullDataCount)
	    dataCount = (int) fullDataCount;

	/* 
	 * Read the data from the file if we haven't seen the
	 * end of file yet.
	 */
	cc = 0;

	if (sawEof==FALSE) {
	    cc = fullRead (fileFd, data, dataCount);

	    if (cc < 0) {
		perror (fileName);

		(void) close (fileFd);
		errorFlag = TRUE;

		return;
	    }

	    /* 
	     * If the file ended too soon, complain and set
	     * a flag so we will zero fill the rest of it.
	     */
	    if (cc < dataCount) {
		fprintf (stderr,
			 "%s: Short read - zero filling", fileName);

		sawEof = TRUE;
	    }
	}

	/* 
	 * Zero fill the rest of the data if necessary.
	 */
	if (cc < dataCount)
	    memset (data + cc, 0, dataCount - cc);

	/* 
	 * Write the buffer to the TAR file.
	 */
	writeTarBlock (data, dataCount);

	fullDataCount -= dataCount;
    }

    /* 
     * Close the file.
     */
    if ((tostdoutFlag == FALSE) && close (fileFd) < 0)
	fprintf (stderr, "%s: close: %s\n", fileName, strerror (errno));
}


/*
 * Save a directory and all of its files to the tar file.
 */
static void saveDirectory (const char *dirName, const struct stat *statbuf)
{
    DIR *dir;
    struct dirent *entry;
    int needSlash;
    char fullName[NAME_MAX];

    /* 
     * Construct the directory name as used in the tar file by appending
     * a slash character to it.
     */
    strcpy (fullName, dirName);
    strcat (fullName, "/");

    /* 
     * Write out the header for the directory entry.
     */
    writeHeader (fullName, statbuf);

    /* 
     * Open the directory.
     */
    dir = opendir (dirName);

    if (dir == NULL) {
	fprintf (stderr, "Cannot read directory \"%s\": %s\n",
		 dirName, strerror (errno));

	return;
    }

    /* 
     * See if a slash is needed.
     */
    needSlash = (*dirName && (dirName[strlen (dirName) - 1] != '/'));

    /* 
     * Read all of the directory entries and check them,
     * except for the current and parent directory entries.
     */
    while (errorFlag==FALSE && ((entry = readdir (dir)) != NULL)) {
	if ((strcmp (entry->d_name, ".") == 0) ||
	    (strcmp (entry->d_name, "..") == 0)) {
	    continue;
	}

	/* 
	 * Build the full path name to the file.
	 */
	strcpy (fullName, dirName);

	if (needSlash)
	    strcat (fullName, "/");

	strcat (fullName, entry->d_name);

	/* 
	 * Write this file to the tar file, noticing whether or not
	 * the file is a symbolic link.
	 */
	saveFile (fullName, TRUE);
    }

    /* 
     * All done, close the directory.
     */
    closedir (dir);
}


/*
 * Write a tar header for the specified file name and status.
 * It is assumed that the file name fits.
 */
static void writeHeader (const char *fileName, const struct stat *statbuf)
{
    long checkSum;
    const unsigned char *cp;
    int len;
    TarHeader header;

    /* 
     * Zero the header block in preparation for filling it in.
     */
    memset ((char *) &header, 0, sizeof (header));

    /* 
     * Fill in the header.
     */
    strcpy (header.name, fileName);

    strncpy (header.magic, TAR_MAGIC, sizeof (header.magic));
    strncpy (header.version, TAR_VERSION, sizeof (header.version));

    putOctal (header.mode, sizeof (header.mode), statbuf->st_mode & 0777);
    putOctal (header.uid, sizeof (header.uid), statbuf->st_uid);
    putOctal (header.gid, sizeof (header.gid), statbuf->st_gid);
    putOctal (header.size, sizeof (header.size), statbuf->st_size);
    putOctal (header.mtime, sizeof (header.mtime), statbuf->st_mtime);

    header.typeFlag = TAR_TYPE_REGULAR;

    /* 
     * Calculate and store the checksum.
     * This is the sum of all of the bytes of the header,
     * with the checksum field itself treated as blanks.
     */
    memset (header.checkSum, ' ', sizeof (header.checkSum));

    cp = (const unsigned char *) &header;
    len = sizeof (header);
    checkSum = 0;

    while (len-- > 0)
	checkSum += *cp++;

    putOctal (header.checkSum, sizeof (header.checkSum), checkSum);

    /* 
     * Write the tar header.
     */
    writeTarBlock ((const char *) &header, sizeof (header));
}


/*
 * Write data to one or more blocks of the tar file.
 * The data is always padded out to a multiple of TAR_BLOCK_SIZE.
 * The errorFlag static variable is set on an error.
 */
static void writeTarBlock (const char *buf, int len)
{
    int partialLength;
    int completeLength;
    char fullBlock[TAR_BLOCK_SIZE];

    /* 
     * If we had a write error before, then do nothing more.
     */
    if (errorFlag==TRUE)
	return;

    /* 
     * Get the amount of complete and partial blocks.
     */
    partialLength = len % TAR_BLOCK_SIZE;
    completeLength = len - partialLength;

    /* 
     * Write all of the complete blocks.
     */
    if ((completeLength > 0) && !fullWrite (tarFd, buf, completeLength)) {
	perror (tarName);

	errorFlag = TRUE;

	return;
    }

    /* 
     * If there are no partial blocks left, we are done.
     */
    if (partialLength == 0)
	return;

    /* 
     * Copy the partial data into a complete block, and pad the rest
     * of it with zeroes.
     */
    memcpy (fullBlock, buf + completeLength, partialLength);
    memset (fullBlock + partialLength, 0, TAR_BLOCK_SIZE - partialLength);

    /* 
     * Write the last complete block.
     */
    if (!fullWrite (tarFd, fullBlock, TAR_BLOCK_SIZE)) {
	perror (tarName);

	errorFlag = TRUE;
    }
}


/*
 * Put an octal string into the specified buffer.
 * The number is zero and space padded and possibly null padded.
 * Returns TRUE if successful.
 */
static int putOctal (char *cp, int len, long value)
{
    int tempLength;
    char *tempString;
    char tempBuffer[32];

    /* 
     * Create a string of the specified length with an initial space,
     * leading zeroes and the octal number, and a trailing null.
     */
    tempString = tempBuffer;

    sprintf (tempString, " %0*lo", len - 2, value);

    tempLength = strlen (tempString) + 1;

    /* 
     * If the string is too large, suppress the leading space.
     */
    if (tempLength > len) {
	tempLength--;
	tempString++;
    }

    /* 
     * If the string is still too large, suppress the trailing null.
     */
    if (tempLength > len)
	tempLength--;

    /* 
     * If the string is still too large, fail.
     */
    if (tempLength > len)
	return FALSE;

    /* 
     * Copy the string to the field.
     */
    memcpy (cp, tempString, len);

    return TRUE;
}
#endif

/* END CODE */
