// pg io simulator.
//
// we simulate an index scan, sort of.
// given multiple files it more acts like one (fetching from index, fetching
// from the heap).
//
// we also support random writing. 
//
// the general idea is to open a number of files (hopefully larger han ram)
// and randomly seek around and read blocks. in write mode a percentage of
// the blocks are rewritten.
// 
// released under the BSD license.
// comments/questions: Jeff Trout - jeff@jefftrout.com
//
// now with threads!
//
// lets create a bitmap so we can try to avoid dupe blocks
// however that isn't how the PG index scan works (it'll repeatedly hit 
// the same blocks. but meh, we'll see how things go)
//


#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/errno.h>
#include <signal.h>
#include <pthread.h>

#define TV2DOUBLE(tv) ((double)(tv)->tv_sec + (double)((tv)->tv_usec) / 1000000)
#define MAXFILES 64

int stop = 0;
int numWorkers = 1;

char *fileList[MAXFILES];
int numFiles;
int maxReads = 0;
int curReads = 0;
int seqScan = 0;
int writeMode = 0;
int writePercent = 0;
int curWrites = 0;
int fsyncAfterWrite = 0;
int startBlock = 0;  
int totalBlocks = 0; 
double stallCheck = 0;

typedef struct _FileInfo
{
  int fd;
  int numBlocks;
  int curBlock;
  char filename[128];
} FileInfo;


void dropLinuxCache()
{
  if(getuid() != 0)
	{
	  printf("Only root may drop the vm caches\n");
	  exit(0);
	}
  
  // sync disks up first.
  system("sync; echo 3 > /proc/sys/vm/drop_caches");
  printf("caches dropped\n");
}


// doSIGINT
//
// called on ctrl-c to stop
//
void doSIGINT(int x)
{
  printf("CTRL-C Interrupt - stopping\n");
  stop = 1;
}


// getFile
//
// open a file, return its FileInfo structure
//
FileInfo *getFile(char *fn, int writeMode)
{
  FileInfo *f;
  struct stat sinfo;
  int flags = O_RDONLY;

  if(writeMode) 
	{
	  flags = O_RDWR;
	}

  f = malloc(sizeof(FileInfo));
  stat(fn, &sinfo);

  f->numBlocks = sinfo.st_size / 8192;
  f->fd = open(fn, flags);
  f->curBlock = 0;
  strncpy(f->filename, fn, 128);
  
  if(f->fd <= 0)
	{
	  printf("open failed: %s\n", fn);
	  exit(0);
	}
  
  return f;
}

// getBlock
// 
// take an arg and convert it to a block
// ie 16k = 8, etc
// 
int getBlock(char *arg)
{
  int numReads;

  numReads = atoi(arg);
  switch(arg[strlen(arg) - 1])
	{
	case 'k':
	case 'K':
	  printf("Reads in kB\n");
	  numReads /= 8; 
	  break;
	case 'm':
	case 'M':
	  printf("Reads in mB\n");
	  numReads = (numReads * 1024) / 8; 
	  break;
	case 'g':
	case 'G':
	  printf("Reads in gB\n");
	  numReads = (numReads * 1024 * 1024) / 8;
	  break;
	default:
	  printf("Unknown units of blocks\n");
	  break;
	}

  return numReads;
}


void *ioWorker(void *arg)
{
	int	x;
	FileInfo *files[MAXFILES];
	FileInfo *f;
	struct timeval tv;
	double start;
	double end;
	char buf[8192];
	off_t cb;
	int n;

	for(x = 0; x < numFiles; x++)
	{
		files[x] = getFile(fileList[x], writeMode);
		files[x]->curBlock = startBlock;
	}
	
	// randomly read the file(s).
	for(x = 0; x < maxReads && !stop; x++)
	{
		f = files[rand() % numFiles];
		
		// pick a file
		if(seqScan)
		{
			cb = f->curBlock % f->numBlocks;
			f->curBlock++;
		}
		else
		{
			cb = rand() % f->numBlocks;
		}
		
		// this shouldn't happen
		if(cb > f->numBlocks) 
		{
			printf("Dork\n");
		}
		
		if(lseek(f->fd, (off_t)8192 * cb, SEEK_SET) == -1)
		{
			printf("Seek %d failed: %d %s\n", x, cb * 8192, strerror(errno));
		}
		
		n = read(f->fd, buf, 8192);
		
		//	  printf("N: %d\n", n);
		if(n != 8192)
		{
			printf("Bad read: %d %s\n", n, strerror(errno));
		}
		
		if(writeMode)
		{
			// we just write the block back.
			if(rand() % 100 < writePercent)
			{
				gettimeofday(&tv, NULL);
				start = TV2DOUBLE(&tv);
				
				if(lseek(f->fd, (off_t)8192 * cb, SEEK_SET) == -1)
				{
					printf("Seek %d failed: %d %s\n", x, cb * 8192, strerror(errno));
				}
				if(write(f->fd, buf, 8192) != 8192)
				{
					printf("Write Failed: %s\n", strerror(errno));
					exit(0);
				}
				if(fsyncAfterWrite)
				{
					if(fsync(f->fd) != 0)
					{
						printf("failed to fsync\n");
						exit(0);
					}
				}
				gettimeofday(&tv, NULL);
				end = TV2DOUBLE(&tv);
				
				if(stallCheck
					&& (end - start > stallCheck))
				{
					printf("**WRITE STALL: %s %0.4f\n", f->filename, 
						end - start);
				}
				
				curWrites++;
			}
		}

		curReads++;		
	}
	
	for(x = 0; x < numFiles; x++)
	{
		close(files[x]->fd);
	}
}


void *statsUpdate(void *arg)
{
	struct timeval tv;
	double start;
	double end;
	double elapsed;
	int lastRead = 0;
	int lastWrite = 0;
	int deltaRead = 0;
	
	gettimeofday(&tv, NULL);
	start = TV2DOUBLE(&tv);
	sleep(10); // initial sleep.

	for(;;)
	{
		gettimeofday(&tv, NULL);
		end = TV2DOUBLE(&tv);

		elapsed = end - start;
		// num Reads is total reads
		// curReads
		// totalBlocks
		// writeBlocks
		// writeBlcoks
		//
		deltaRead = curReads - lastRead;
		
		// units are blocks.
		printf("%3.2f%%, %6d read, %6d written, %7.2fkB/sec %7.2f iops\n",
		 (double)((double)curReads / maxReads) * 100, 
		 deltaRead, 
		 curWrites - lastWrite,
		 (((double)deltaRead * 8192) / elapsed) / 1024,
		 (double)deltaRead / elapsed);
		 
		lastRead = curReads;
		lastWrite = curWrites;
		
		start = end;
		sleep(10);
	}
}


int main(int argc, char **argv)
{
  FileInfo *files[MAXFILES]; // up to 25 files we can use. 
  int x;
  pthread_t statsTid;
  pthread_t workerTid;
  char ch;
  
  int verbose = 0; 
  int startBlock = 0; 
  int numThreads = 1;
  double start;
  struct timeval tv;
  
  srand(getpid());

  signal(SIGINT, doSIGINT);
  gettimeofday(&tv, NULL);
  start = TV2DOUBLE(&tv);


  while((ch = getopt(argc, argv, "cyb:st:w:h?o:va:")) != EOF)
	{
	  switch(ch)
		{
		case 't':
			numThreads = atoi(optarg);
			break;
		case 'c':
		  dropLinuxCache();
		  break;
		case 'b':
		  // numReads is in blocks. so if we get kmg on the end we 
		  // adjust the value to be the number of blocks.
		  totalBlocks = getBlock(optarg);
		  break;

		case 'v':
		  printf("Verbose\n");
		  verbose = 1;
		  break;
		case 'o':
		  startBlock = getBlock(optarg);
		  printf("Set start block %d\n", startBlock);
		  break;
		case 's':
		  printf("Seq Scan\n");
		  seqScan = 1;
		  break;
		case 'y':
		  printf("fsync after each write\n");
		  fsyncAfterWrite = 1;
		  break;
		case 'w':
		  writeMode = 1;
		  if(optarg)
			{
			  writePercent = atoi(optarg);
			}
		  printf("Write Mode: %d%%\n", writePercent);
		  break;
	  case 'a':
	  	  stallCheck = atof(optarg);
	  	  printf("Stallcheck at %f\n", stallCheck);
	  	  break;
		case 'h':
		case '?':
		  printf("usage: pgiosim -b blocks (8kb) or xxxM xxxK xxxG\n");
		  printf("\t-c - drop vm caches (root only)\n");
		  printf("\t-s - seq scan\n");
		  printf("\t-y - fsync after writes\n");
		  printf("\t-w percentage - we will rewrite this percentage of blocks\n");
		  printf("\t-v - verbose - print stats every gB or so\n");
		  printf("\t-o startblock - start at block startblock\n");
		  printf("\t-a seconds(float) alert when a write stalls longer than n seconds\n");
		  printf("\t file1 ... file2 - files to use as input\n");
		  exit(0);
		}
	}

  argv += optind;
  argc -= optind;

  printf("Arg: %d\n", argc);
  printf("Read %d blocks\n", totalBlocks);

  if(argc > MAXFILES)
  {
	  printf("ERROR: Too many files specified. I can only handle %d\n", MAXFILES);
	  exit(1);
  }
  
  for(numFiles = 0; numFiles < argc; numFiles++)
  {
	  printf("Added %s\n", argv[numFiles]);
	  fileList[numFiles] = argv[numFiles];
  }

  // adjust so each thread gets roughly the same amount of work.
  maxReads = totalBlocks / numThreads;
  
  pthread_create(&statsTid, NULL, statsUpdate, NULL);
  
  for(x = 0; x < numThreads; x++)
  {
  	  pthread_create(&workerTid, NULL, ioWorker, NULL);
  }
  
  pthread_join(workerTid, NULL);

//  showStats(curReads, maxReads, writeBlocks, start);
  
}

