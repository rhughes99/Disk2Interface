/*	Disk2Controller.c
	Apple Disk II Interface Controller
	PRU0 handles phase signals to determine track
	PRU1 handles sending and receiving data on a sector-by-sector basis
	03/22/2020
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>

#define VERBOSE	1								// 1 = display track number

void myShutdown(int sig);
void changeImage(int sig);
void loadDiskImage(const char *imageName);
void saveDiskImage(const char *imageName);
void diskEncodeNib(unsigned char *nibble, unsigned char *data, unsigned char vol, unsigned char trk, unsigned char sec);
unsigned char dosTranslateSector(unsigned char sector);
unsigned char prodosTranslateSector(unsigned char sector);
unsigned char diskDecodeNib(unsigned char *data, unsigned char *nibble);
unsigned char decodeNibByte(unsigned char *nibInt, unsigned char *nibData);

// PRU Memory Locations
#define PRU_ADDR			0x4A300000		// Start of PRU memory Page 163 am335x TRM
#define PRU_LEN				0x80000			// Length of PRU memory
#define PRU1_DRAM			0x02000

// First 0x200 bytes of both PRUs RAM are STACK & HEAP

// PRU0 Memory Locations:
#define PRU0_TRK_NUM_ADDR	0x0300

// PRU1 Memory Locations:
#define TRACK_DATA_ADR		0x0300		// address of track start
#define ENABLE_ADR			0x1B00		// EN- state
#define SECTOR_ADR			0x1B01		// current sector number
#define WRITE_ADR			0x1B02		// 1 = write occurred
#define WRITE_CNT_ADR		0x1B03		// num write bytes, int
#define CONT_INT_ADR		0x1B07		// Controller interrupt, 1 = stop
#define WRITE_DATA_ADR		0x1C00		// address of first write byte

// Someday might move everything to shared memory
//#define PRU_SHAREDMEM	0x10000				// Offset to shared memory
//unsigned int *prusharedMem_32int_ptr;		// Points to the start of shared memory

// PRU0:
static unsigned char *pru0RAMptr;			// start of PRU0 memory
static unsigned char *pru0TrackPtr;			// track number commanded by A2

// PRU1:
static unsigned char *pru1RAMptr;			// start of PRU1 memory
static unsigned char *pru1TrackDataPtr;		// Controller puts loaded track data (starting) here
static unsigned char *pru1EnPtr;			// EN-
static unsigned char *pru1SectorPtr;		// current sector or 0xFF
static unsigned char *pru1WritePtr;			// 1 = write occurred
static unsigned int  *pru1WriteCntPtr;		// num bytes written by A2
static unsigned char *pru1InterruptPtr;		// set by Controller, 1 = stop sending to A2
static unsigned char *pru1WriteDataPtr;		// first byte of data written by A2

static unsigned char running;							// to allow graceful quit
unsigned char track = 100;								// starting point ???
unsigned char loadedTrk = 0;

const unsigned int NUM_TRACKS = 35;
const unsigned int NUM_SECTORS_PER_TRACK = 16;
const unsigned int NUM_BYTES_PER_SECTOR = 256;			// these are only data bytes
const unsigned int SMALL_NIBBLE_SIZE = 374;				// one encoded sector, sync + address + data, bytes
const unsigned int NUM_ENCODED_BYTES_PER_TRACK = 5984;	// 16 * 374
const unsigned int SECTOR_DATA_OFFSET = 26;				// location of first data byte, 0-based

unsigned char currentImageDir = 0;
const char *startupImages[] = 		// imageDir 0
{
	"BasicStartup.po",				// ProDOS 2.0.3
//	"Working.po",
	"MerlinWorking.po",
//	"SavedStart.po",				// saved combo of all below
//	"SystemData.po",				// SavedStart + some source files
//	"MySystem.po",					// ProDOS 2.0.3
//	"MerlinDisk1.dsk",				// ProDOS 1.1.1, MERLIN.SYSTEM
	"SmartApples.po",				// Smart Apples BASIC programs
	"xxx"
};

const char *utilityImages[] = 		// imageDir 1
{
//	"AEDesktopExpProDrive.dsk",		// ProDOS 1.1.1, PRODRIVE
	"Apple_DOS33.dsk",				// DOS 3.3
	"BagOfTricksII.dsk",			// ProDOS 1.1.1
	"BeagleCompiler22.dsk",			// ProDOS 1.2
//	"Copy2Plus74.dsk",				// ProDOS 1.2, UTIL.SYSTEM
	"DCode.dsk",					// ProDOS 1.1.1
	"Diagnostics_IIe.dsk",
//	"GPLE.dsk",						// ProDOS 1.0.1
	"MerlinDisk2.dsk",
//	"PDShrinkIts.dsk",				// ???
	"ProByter.po",					// ProDOS 1.1.1
	"ProgramWriter.dsk",			// ProDOS 1.1.1
//	"ScsiUtilities.po",				// ProDOS 1.8
//	"Timemaster2HO.dsk",			// ProDOS 1.4, SET.CLOCK
//	"ZipChipUtilities.dsk",			// ???
	"xxx"
};

const char *gameSimImages[] = 		// imageDir 2
{
	"A2_FS1_Flight_Sim.dsk",		// DOS 3.3
	"Apple_Classics_Side_1.dsk",	// DOS 3.3
	"Apple_Classics_Side_2.dsk",	// DOS 3.3
	"Aquarium.dsk",					// DOS 3.3
	"castle_wolfenstein_fixed.dsk",	// ???
	"castle_wolfenstein_stos161.dsk",	// starts, quickly gets wacky
	"castle_wolfenstein.dsk",
	"CastleWolfenstein.dsk",
	"Dinosaurs.dsk",				// DOS 3.3
	"Flight_Sim_II.dsk",			// Boot only
	"FlightSimulator2.dsk",			// Boot only, same as Flight_Sim_ II
	"FormulaNibble.dsk",
	"FS2.dsk",						// Boot only, same as Flight_Sim_ II
	"HighSeas_S1.dsk",				// ProDOS
	"HighSeas_S2.dsk",				// Data only
	"IOSilver.dsk",
	"SilentService.dsk",
	"Sudoku.dsk",					// ProDOS 1.8
	"xxx"
};

const char *holdingPenImages[] =	// imageDir 3
{
	"ChessMaster2000b.dsk",
	"Minesweeper.dsk",
	"Monopoly.dsk",
	"RISK.dsk",
	"sorry_s1.dsk",
	"sorry_s2.dsk",
	"xxx"
};

const char *sdCardDirs[] = {"Startup", "Utilities", "Games & Simulations", "HoldingPen"};

unsigned char theImage[35][16][374];			// [NUM_TRACKS][NUM_SECTORS_PER_TRACK][SMALL_NIBBLE_SIZE]

//____________________
const unsigned char translate6[64] =
{
	0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
	0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
	0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
	0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
	0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
	0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
	0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
	0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
};

unsigned char untranslate6[256];

//____________________
int main(int argc, char *argv[])
{
	unsigned char sector, prevSector, checksum, writeByte;
	unsigned int i, j, offset, trkCnt, writeByteCnt, sectorIndex;

	unsigned char *pru;		// start of PRU memory
	int	fd;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd == -1)
	{
		printf("*** ERROR: could not open /dev/mem.\n");
		return EXIT_FAILURE;
	}
	pru = mmap(0, PRU_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PRU_ADDR);
	if (pru == MAP_FAILED)
	{
		printf("*** ERROR: could not map memory.\n");
		return EXIT_FAILURE;
	}
	close(fd);

	// Set memory pointers
	// PRU 0
	pru0RAMptr		= pru;
	pru0TrackPtr	= pru0RAMptr + PRU0_TRK_NUM_ADDR;

	// PRU 1
	pru1RAMptr			= pru + PRU1_DRAM;
	pru1TrackDataPtr	= pru1RAMptr + TRACK_DATA_ADR;
	pru1EnPtr			= pru1RAMptr + ENABLE_ADR;
	pru1SectorPtr		= pru1RAMptr + SECTOR_ADR;
	pru1WritePtr		= pru1RAMptr + WRITE_ADR;
	pru1WriteCntPtr		= (int *)(pru1RAMptr + WRITE_CNT_ADR);
	pru1InterruptPtr	= pru1RAMptr + CONT_INT_ADR;
	pru1WriteDataPtr	= pru1RAMptr + WRITE_DATA_ADR;

	// Load disk image (into theImage and PRU 1)
	loadDiskImage(startupImages[0]);						// first startup image

	// Set up untranslate6 table
	for (i=0; i<NUM_BYTES_PER_SECTOR; i++)					// fill with FFs to detect when we are out of range
		untranslate6[i] = 0xFF;

	for (i=0; i<0x40; i++)									// inverse of translate6 table
		untranslate6[translate6[i]] = i;

	(void) signal(SIGINT,  myShutdown);						// ^c = graceful shutdown
	(void) signal(SIGTSTP, changeImage);					// ^z = cycle through images

	printf("\n--- Disk II IF running\n");
	printf("====================\n");
	printf("\t<ctrl>-z to change image or save\n");
	printf("\t<ctrl>-c to quit\n");
	printf("--------------------\n");

	running = 1;
	trkCnt = 0;
	prevSector = 0;
	do
	{
		usleep(10);

		track = *pru0TrackPtr;
		if (track != loadedTrk)						// has A2 moved disk head?
		{
			printf("NEW TRACK\n");

			*pru1InterruptPtr = 1;					// pause PRU1 while changing track
			usleep(1000);							// give PRU time to finish sector??? (1 sector > 11 ms)
			trkCnt++;

			// Copy new track to PRU
			for (sector=0; sector<NUM_SECTORS_PER_TRACK; sector++)
			{
				offset = sector * SMALL_NIBBLE_SIZE;
				for (i=0; i<SMALL_NIBBLE_SIZE; i++)
					*(pru1TrackDataPtr + offset + i) = theImage[track][sector][i];
			}
			*pru1InterruptPtr = 0;					// turn PRU1 back on

			loadedTrk = track;
			if (VERBOSE)
			{
				printf("0x%X\t", loadedTrk);
				if (trkCnt % 8 == 0)
					printf("\n");
			}
		}

		if (*pru1EnPtr == 0)					// is drive enabled?
		{
			// A2 has enabled drive, EN- = 0
			if (*pru1SectorPtr != prevSector)	// sector has changed
			{
				// PRU 1 ready to send next sector
				prevSector = *pru1SectorPtr;

				// But first, did a write occur during the last sector?
				if (*pru1WritePtr == 1)
				{
					// a write occurred during this sector
					// PRU put write data in ram startng at 0x1800 (6144)
					// Expecting 342 data bytes and 1 checksum byte

					printf("\n------- Write detected. Time to implement.\n\n");

					sector = *pru1SectorPtr;
					writeByteCnt = *pru1WriteCntPtr;
					if (writeByteCnt < 343)
						printf("*** writeByteCnt too small: %d (343)\n", writeByteCnt);

					// Copy sector from PRU write buffer to theImage[] and PRU track buffer
					sectorIndex = sector * 374;			// first sync byte of sector
					checksum = 0;
					for (i=0, j=SECTOR_DATA_OFFSET; i<343; i++, j++)
					{
						writeByte = *(pru1WriteDataPtr + i);
						theImage[loadedTrk][sector][j] = writeByte;
						*(pru1TrackDataPtr + sectorIndex + j) = writeByte;

						checksum ^= writeByte;
					}
					usleep(30000);
				}

				// enable sector
				*pru1InterruptPtr = 0;					// enable next sector
				usleep(1);								// short sleep to let PRU continue
				*pru1InterruptPtr = 1;					// PRU 1 stops before sending next sector
			}
		}
	} while (running);

	printf("---Shutting down...\n");

	if(munmap(pru, PRU_LEN))
		printf("*** ERROR: munmap failed at Shutdown\n");

	return EXIT_SUCCESS;
}

//____________________
void myShutdown(int sig)
{
	// ctrl-C
	printf("\n");
	running = 0;
	(void) signal(SIGINT, SIG_DFL);			// reset signal handlling of SIGINT
}

//____________________
void changeImage(int sig)
{
	// ctrl-Z
	char saveName[32], imageName[32];
	static unsigned char startupImageNum = 1;	// image 0 was loaded at startup
	static unsigned char utilityImageNum = 0;
	static unsigned char gameSimImageNum = 0;
	static unsigned char holdingPenImageNum = 0;

	size_t length;

	for (length=0; length<32; length++)		// to ensure imageName is terminated
		imageName[length] = '\0';

	// Ask about saving current image
	printf("\n--> Save image? Enter save name, new dir num, or <CR>: ");
	fgets(saveName, 32, stdin);
	length = strlen(saveName);
//	printf("length= %d\n", length);
	if (length > 5)							// new saved name entered
	{
		// Need to strip CR
		strncpy(imageName, saveName, length-1);
		saveDiskImage(imageName);			// this is compressed (256 bytes/sector) format, ready to re-loaded
	}

	else if (length == 2)					// new dir number entered
	{
		currentImageDir = atoi(saveName);
		if (currentImageDir < 4)			// can we compute this dynamically? number of elements in sdCardDirs[]?
		{
//			printf("New dir number entered: %d\n", currentImageDir);
			printf("\nSwitching to <<%s>>\n", sdCardDirs[currentImageDir]);
//			currentImageDir = atoi(saveName);
		}
		else
			printf("\n*** Invalid directory number entered!\n");
	}

	switch (currentImageDir)
	{
		case 0:
			loadDiskImage(startupImages[startupImageNum]);
			startupImageNum++;
			if (strcmp(startupImages[startupImageNum], "xxx") == 0)
				startupImageNum = 0;
			break;

		case 1:
			loadDiskImage(utilityImages[utilityImageNum]);
			utilityImageNum++;
			if (strcmp(utilityImages[utilityImageNum], "xxx") == 0)
				utilityImageNum = 0;
			break;

		case 2:
			loadDiskImage(gameSimImages[gameSimImageNum]);
			gameSimImageNum++;
			if (strcmp(gameSimImages[gameSimImageNum], "xxx") == 0)
				gameSimImageNum = 0;
			break;

		case 3:
			loadDiskImage(holdingPenImages[holdingPenImageNum]);
			holdingPenImageNum++;
			if (strcmp(holdingPenImages[holdingPenImageNum], "xxx") == 0)
				holdingPenImageNum = 0;
			break;

		default:
			printf("\n*** changeImage(): Unexpected currentImageDir value [%d]\n", currentImageDir);
	}
}

//____________________
void loadDiskImage(const char *imageName)
{
	/*	Loads disk image into theImage
		Accounts for sector interleaving
		Then loads track 0 into PRU1 data ram
	*/
	unsigned char trk, sector, translatedSector;
	unsigned char tempBuff[NUM_TRACKS][NUM_SECTORS_PER_TRACK][NUM_BYTES_PER_SECTOR];
	char imagePath[128];
	unsigned int i, offset;
	char *ext;
	size_t numElements;
	FILE *fd;	

	switch (currentImageDir)
	{
		case 0:
			sprintf(imagePath, "/root/DiskImages/Small/Startup/%s", imageName);	// full image path
			break;

		case 1:
			sprintf(imagePath, "/root/DiskImages/Small/Utilities/%s", imageName);
			break;

		case 2:
			sprintf(imagePath, "/root/DiskImages/Small/GamesSims/%s", imageName);
			break;

		case 3:
			sprintf(imagePath, "/root/DiskImages/Small/HoldingPen/%s", imageName);
			break;

		default:
			printf("\n*** loadDiskImage(): Unexpected currentImageDir value [%d]\n", currentImageDir);
	}

	printf("\n  --- %s ---\n", imageName);
	fd = fopen(imagePath, "rb");
	if (!fd)
	{
		printf("\n*** Problem opening disk image\n");
		return;
	}

	// Read file into tempBuff, no format/alignment adjustments yet
	for (trk=0; trk<NUM_TRACKS; trk++)
	{
		for (sector=0; sector<NUM_SECTORS_PER_TRACK; sector++)
		{
			numElements = fread(tempBuff[trk][sector], NUM_BYTES_PER_SECTOR, 1, fd);
			if (numElements != 1)
				printf("\n*** numElements= %zu (expecting 1)\n", numElements);
		}
	}
	fclose(fd);

	// Now rearrange and add synch, checksum, etc, into theImage
	ext = strrchr(imagePath, '.');		// get file extension
	for (trk=0; trk<NUM_TRACKS; trk++)
	{
		for (sector=0; sector<NUM_SECTORS_PER_TRACK; sector++)
		{
			// Assume we are only dealing with .dsk and .po files
			if (strcmp(ext, ".dsk") == 0)
				translatedSector = dosTranslateSector(sector);
			else
				translatedSector = prodosTranslateSector(sector);

			diskEncodeNib(theImage[trk][sector], tempBuff[trk][translatedSector], 254, trk, sector);
		}
	}

	// Load track 0 into PRU1 data ram
	*pru1InterruptPtr = 1;					// pause PRU1 while changing track

	for (sector=0; sector<NUM_SECTORS_PER_TRACK; sector++)
	{
		offset = sector * SMALL_NIBBLE_SIZE;
		for (i=0; i<SMALL_NIBBLE_SIZE; i++)
			*(pru1TrackDataPtr + offset + i) = theImage[0][sector][i];
	}
	*pru1InterruptPtr = 0;

	track = 0;
	loadedTrk = 0;
}

//____________________
void diskEncodeNib(unsigned char *nibble, unsigned char *data, unsigned char vol, unsigned char trk, unsigned char sec)
{
	// Converts 256 byte file sector to 374 byte disk sector
	unsigned int checksum, oldValue, xorValue, i;

	static const unsigned char syncStream[]		= {0xFF, 0x3F, 0xCF, 0xF3, 0xFC};
	static const unsigned char addrPrologue[]	= {0xD5, 0xAA, 0x96};
	static const unsigned char dataPrologue[]	= {0xD5, 0xAA, 0xAD};
	static const unsigned char epilogue1[]		= {0xDE, 0xAA, 0xEB};
	static const unsigned char epilogue2[]		= {0xDE, 0xAA, 0xEB, 0x00, 0x00};
	unsigned char *nibByte;

	// Set up header values
	checksum = vol ^ trk ^ sec;

	nibByte = memset(nibble, 0xFF, SMALL_NIBBLE_SIZE);
	memcpy(nibByte, syncStream, 5);			nibByte += 5;
	memcpy(nibByte, addrPrologue, 3);		nibByte += 3;

	*nibByte++	= (vol >> 1) | 0xAA;
	*nibByte++	= vol | 0xAA;

	*nibByte++	= (trk >> 1) | 0xAA;
	*nibByte++	= trk | 0xAA;

	*nibByte++	= (sec >> 1) | 0xAA;
	*nibByte++	= sec | 0xAA;

	*nibByte++	= (checksum >> 1) | 0xAA;
	*nibByte++	= (checksum) | 0xAA;

	memcpy(nibByte, epilogue1, 3);			nibByte += 3;
	memcpy(nibByte, syncStream+1, 4);		nibByte += 4;
	memcpy(nibByte, dataPrologue, 3);		nibByte += 3;

	xorValue = 0;
	for (i=0; i<342; i++)
	{
		if (i >= 0x56)
		{
			// 6 bit
			oldValue = data[i - 0x56];
			oldValue = oldValue >> 2;
		}
		else
		{
			// 3 * 2 bit
			oldValue = 0;
			oldValue |= (data[i + 0x00] & 0x01) << 1;
			oldValue |= (data[i + 0x00] & 0x02) >> 1;
			oldValue |= (data[i + 0x56] & 0x01) << 3;
			oldValue |= (data[i + 0x56] & 0x02) << 1;
			if (i + 0xAC < NUM_BYTES_PER_SECTOR)
			{
				oldValue |= (data[i + 0xAC] & 0x01) << 5;
				oldValue |= (data[i + 0xAC] & 0x02) << 3;
			}
		}
		xorValue ^= oldValue;
		*nibByte++ = translate6[xorValue & 0x3F];
		xorValue = oldValue;
	}
	*nibByte++ = translate6[xorValue & 0x3F];

	memcpy(nibByte, epilogue2, 5);
}

//____________________
unsigned char dosTranslateSector(unsigned char sector)
{
	// DOS order (*.dsk)
	static const unsigned char skewing[] =
	{
		0x00, 0x07, 0x0E, 0x06, 0x0D, 0x05, 0x0C, 0x04,
		0x0B, 0x03, 0x0A, 0x02, 0x09, 0x01, 0x08, 0x0F
	};
	return skewing[sector];
}

//____________________
unsigned char prodosTranslateSector(unsigned char sector)
{
	// ProDOS order (*.po)
	static const unsigned char skewing[] =
	{
		0x00, 0x08, 0x01, 0x09, 0x02, 0x0A, 0x03, 0x0B,
		0x04, 0x0C, 0x05, 0x0D, 0x06, 0x0E, 0x07, 0x0F
	};
	return skewing[sector];
}

//____________________
void saveDiskImage(const char *fileName)
{
	/*	Saves disk image to /root/DiskImages/Small/fileName in format that can be loaded
		Inverse of loadDiskImage()
		Will overwrite existing file!
		Accounts for sector interleaving
	*/
	unsigned char trk, sector, unTranslatedSector, resp;
	unsigned char tempBuff[NUM_TRACKS][NUM_SECTORS_PER_TRACK][NUM_BYTES_PER_SECTOR];
	unsigned char unTranslateSector_DOS[16], unTranslateSector_ProDOS[16];
	char imagePath[64];
	unsigned int i;
	char *ext;
	FILE *fd;

	// Set up unTranslateSector tables
	for (i=0; i<16; i++)
	{
		unTranslateSector_DOS[dosTranslateSector(i)] = i;
		unTranslateSector_ProDOS[prodosTranslateSector(i)] = i;
	}

	// Decode image into tempBuff, accounting for sector interleaving
	sprintf(imagePath, "/root/DiskImages/Small/%s", fileName);	// create image path
	ext = strrchr(imagePath, '.');				// get file extension
	for (trk=0; trk<NUM_TRACKS; trk++)
	{
		for (sector=0; sector<NUM_SECTORS_PER_TRACK; sector++)
		{
			// Assume we are only dealing with .dsk and .po files
			if (strcmp(ext, ".dsk") == 0)
				unTranslatedSector = unTranslateSector_DOS[sector];
			else
				unTranslatedSector = unTranslateSector_ProDOS[sector];

			resp = diskDecodeNib(tempBuff[trk][sector], theImage[trk][unTranslatedSector]);
			if (resp == 1)		// error occurred
			{
				printf("\n***   trk= %d sector= %d\n", trk, sector);
				return;
			}
		}
	}

	// Set up save path and open file
	printf("\n--- Saving: %s ---\n", fileName);
	fd = fopen(imagePath, "wb");
	if (!fd)
	{
		printf("\n*** Problem opening file for save\n");
		return;
	}

	// Copy image from tempBuff to /root/DiskImages/imageName
	for (trk=0; trk<NUM_TRACKS; trk++)
	{
		for (sector=0; sector<NUM_SECTORS_PER_TRACK; sector++)
			fwrite(tempBuff[trk][sector], NUM_BYTES_PER_SECTOR, 1, fd);
	}
	fclose(fd);
}

//____________________
unsigned char diskDecodeNib(unsigned char *data, unsigned char *nibble)
{
	// Converts 374 byte disk sector to 256 byte file sector

	unsigned char readVolume, readTrack, readSector, readChecksum;
	unsigned char b, xorValue, newValue;
	unsigned int i;

	// Pick apart volume/track/sector info and checksum - sanity checks?
	if (decodeNibByte(&readVolume, &nibble[8]))
	{
		printf("\n*** diskDecodeNib: Failed to decode volume\n");
		return 1;
	}

	if (decodeNibByte(&readTrack, &nibble[10]))
	{
		printf("\n*** diskDecodeNib: Failed to decode track\n");
		return 1;
	}

	if (decodeNibByte(&readSector, &nibble[12]))
	{
		printf("\n*** diskDecodeNib: Failed to decode sector\n");
		return 1;
	}

	if (decodeNibByte(&readChecksum, &nibble[14]))
	{
		printf("\n*** diskDecodeNib: Failed to decode checksum\n");
		return 1;
	}

	if (readChecksum != (readVolume ^ readTrack ^ readSector))
	{
		printf("\n*** diskDecodeNib: Failed address checksum\n");
		return 1;
	}

	// Decode nibble core
	xorValue = 0;
	for (i=0; i<342; i++)
	{
		b = untranslate6[nibble[i+26]];		// first data
		if (b == 0xFF)
		{
			printf("\n*** diskDecodeNib: Out of range in untranslate6: %d\n", nibble[i+26]);
			return 1;
		}

		newValue = b ^ xorValue;

		if (i >= 0x56)
		{
			// 6 bit
			data[i - 0x56] |= (newValue << 2);
		}
		else
		{
			// 3 * 2 bit
			data[i + 0x00] = ((newValue >> 1) & 0x01) | ((newValue << 1) & 0x02);
			data[i + 0x56] = ((newValue >> 3) & 0x01) | ((newValue >> 1) & 0x02);
			if (i + 0xAC < NUM_BYTES_PER_SECTOR)
				data[i + 0xAC] = ((newValue >> 5) & 0x01) | ((newValue >> 3) & 0x02);
		}
		xorValue = newValue;
	}
	return 0;
}

//____________________
unsigned char decodeNibByte(unsigned char *nibInt, unsigned char *nibData)
{
	if ((nibData[0] & 0xAA) != 0xAA)
		return 1;

	if ((nibData[1] & 0xAA) != 0xAA)
		return 1;

	*nibInt  = (nibData[0] & ~0xAA) << 1;
	*nibInt |= (nibData[1] & ~0xAA) << 0;
	return 0;
}
