/*	Disk2Controller.c
	Apple Disk II Interface Controller
	PRU0 handles phase signals to determine track
	PRU1 handles sending and receiving data on a sector-by-sector basis
	04/2022
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>

#define VERBOSE	0							// 1 = display track number

void myShutdown(int sig);
void changeImage(int sig);
void loadDiskImage(const char *imageName);
void saveDiskImage(const char *imageName);
void diskEncodeNib(unsigned char *nibble, unsigned char *data, unsigned char vol, unsigned char trk, unsigned char sec);
unsigned char dosTranslateSector(unsigned char sector);
unsigned char prodosTranslateSector(unsigned char sector);
unsigned char diskDecodeNib(unsigned char *data, unsigned char *nibble);
unsigned char decodeNibByte(unsigned char *nibInt, unsigned char *nibData);
unsigned char computeDataChecksum(unsigned char *nibble);

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
static unsigned char *pru1SectorPtr;		// last sector sent to A2
static unsigned char *pru1WritePtr;			// 1 = write occurred
static unsigned char *pru1InterruptPtr;		// set by Controller, 1 = stop sending to A2
static unsigned char *pru1WriteDataPtr;		// first byte of data written by A2

static unsigned char running;							// to allow graceful quit
unsigned char track = 0;
unsigned char loadedTrk = 0;

const unsigned int NUM_TRACKS = 35;
const unsigned int NUM_SECTORS_PER_TRACK = 16;
const unsigned int NUM_BYTES_PER_SECTOR = 256;			// these are only data bytes
const unsigned int SMALL_NIBBLE_SIZE = 374;				// one encoded sector, sync + address + data, bytes
const unsigned int NUM_ENCODED_BYTES_PER_TRACK = 5984;	// 16 * 374
const unsigned int SECTOR_DATA_OFFSET = 26;				// location of first data byte, 0-based

// First image is loaded at startup
const char *theImages[] =
{
	"Startup/BasicStartup.po",					// ProDOS 2.0.3

	"Games/Action/ABM.dsk",
	"Games/Action/AcidTrip.dsk",
	"Games/Action/AE_Back.dsk",
	"Games/Action/AE_Front.dsk",
	"Games/Action/ae1.dsk",
	"Games/Action/ae2.dsk",
	"Games/Action/Aeronaut.dsk",
	"Games/Action/Airheart.dsk",
	"Games/Action/Alcazar.dsk",
	"Games/Action/Alf.dsk",
	"Games/Action/AlienPlus.dsk",
	"Games/Action/AlienRain.dsk",
	"Games/Action/ALIENS1.dsk",
	"Games/Action/ALIENS2.dsk",
	"Games/Action/AntiISDA_Warrior.dsk",
	"Games/Action/Aplcidsp.dsk",
	"Games/Action/AppleBowling.dsk",
	"Games/Action/ApplePanic_Joystick.dsk",
	"Games/Action/ApplePanic.dsk",
	"Games/Action/ApplePanicPlus.dsk",
	"Games/Action/ArcaseBootCamp.dsk",
	"Games/Action/ArcadeInsanity.dsk",
	"Games/Action/ArticFox.dsk",
	"Games/Action/ArdyTheAardvark.dsk",
	"Games/Action/Argos.dsk",
	"Games/Action/Arkanoi2.dsk",
	"Games/Action/arkanoid.dsk",
	"Games/Action/arkedit.dsk",
	"Games/Action/Artesians.dsk",
	"Games/Action/Asteroid.dsk",
	"Games/Action/Asteroids_nm_h5.dsk",
	"Games/Action/Aztec.dsk",
	"Games/Action/Aztec_alt.dsk",
	"Games/Action/Aztec.dsk",


	"BLANK.po"
};

//				[NUM_TRACKS][NUM_SECTORS_PER_TRACK][SMALL_NIBBLE_SIZE]
unsigned char theImage[35][16][374];
unsigned char loadedImageName[64];

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
	unsigned char sector, lastSectorSent, prevSector, checksum, writeByte;
	unsigned int i, j, k, offset, trkCnt, writeByteCnt, sectorIndex;
//	unsigned char tempSector[SMALL_NIBBLE_SIZE];

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
	pru1InterruptPtr	= pru1RAMptr + CONT_INT_ADR;
	pru1WriteDataPtr	= pru1RAMptr + WRITE_DATA_ADR;

	// Load disk image (into theImage and PRU 1)
	loadDiskImage(theImages[0]);					// first image in list

	// Set up untranslate6 table
	for (i=0; i<NUM_BYTES_PER_SECTOR; i++)			// fill with FFs to detect when we are out of range
		untranslate6[i] = 0xFF;

	for (i=0; i<0x40; i++)							// inverse of translate6 table
		untranslate6[translate6[i]] = i;

	(void) signal(SIGINT,  myShutdown);				// ^c = graceful shutdown
	(void) signal(SIGTSTP, changeImage);			// ^z = cycle through images

	printf("\n--- Disk II IF running\n");
	printf("====================\n");
	printf("  <ctrl>-z to change image or save\n");
	printf("  <ctrl>-c to quit\n");
	printf("--------------------\n");

	running = 1;
	trkCnt = 0;
	prevSector = 0;
	do
	{
		usleep(10);

		// OK because PRU0 only updates track when drive enabled
		track = *pru0TrackPtr;
		if (track != loadedTrk)					// has A2 moved disk head?
		{
			*pru1InterruptPtr = 1;				// pause sending while changing track

			// Copy new track to PRU
			for (sector=0; sector<NUM_SECTORS_PER_TRACK; sector++)
			{
				offset = sector * SMALL_NIBBLE_SIZE;
				for (i=0; i<SMALL_NIBBLE_SIZE; i++)
					*(pru1TrackDataPtr + offset + i) = theImage[track][sector][i];
			}
			*pru1InterruptPtr = 0;				// turn sending back on

			loadedTrk = track;
			if (VERBOSE)
			{
				printf("%d\t", loadedTrk);
//				printf("0x%X\t", loadedTrk);
				trkCnt++;						// for display
				if (trkCnt % 8 == 0)
					printf("\n");
			}
		}

		if (*pru1EnPtr == 0)					// is drive enabled?
		{
			lastSectorSent = *pru1SectorPtr;

			// A2 has enabled drive, EN- = 0
			if (lastSectorSent != prevSector)	// PRU finished sending sector
			{
				prevSector = lastSectorSent;

				// But first, did a write occur during last sector?
				if (*pru1WritePtr == 1)
				{
					// Write occurred during this sector
					// Expecting 342 data bytes + 1 checksum byte + [DE AA EB]

					// Gross write integrity check: Is epilogue in expected location?
					if (*(pru1WriteDataPtr+347) != 0xDE ||
						*(pru1WriteDataPtr+348) != 0xAA ||
						*(pru1WriteDataPtr+349) != 0xEB)
						printf("*** BAD write epilogue\n");

					// Copy sector from PRU write buffer to theImage[] and PRU track buffer
					sectorIndex = prevSector * 374;			// first sync byte of sector
					for (i=4, j=SECTOR_DATA_OFFSET, k=0; i<347; i++, j++, k++)
					{
						writeByte = *(pru1WriteDataPtr + i);
						theImage[loadedTrk][prevSector][j] = writeByte;
						*(pru1TrackDataPtr + sectorIndex + j) = writeByte;

//						tempSector[k] = writeByte;
					}

					// Debug - yet another checksum thought
//					checksum = computeDataChecksum(tempSector);
//					if (checksum != *(pru1WriteDataPtr + 346))
//						printf("*** BAD checksum: 0x%X 0x%X\n", checksum, *(pru1WriteDataPtr + 346));

					*pru1WritePtr = 0;		// turn off write flag
				}

				// enable sector
				*pru1InterruptPtr = 0;					// enable next sector
				usleep(10);								// short sleep to let PRU continue
				*pru1InterruptPtr = 1;					// PRU 1 stops before sending next sector
			}
		}
	} while (running);

	printf("---Shutting down...\n");

	// Debug
//	for (i=0; i<360; i++)
//		printf("%d\t0x%X\n", i, *(pru1WriteDataPtr + i));

	if (munmap(pru, PRU_LEN))
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
	unsigned int i, selection;
	size_t numImages, length;
	char saveName[32];

	printf("\n\n");
	printf("Loaded image: %s\n", loadedImageName);
	numImages = sizeof(theImages) / sizeof(theImages[0]);

	printf("========== ========== ========== ========== ========== ==========\n");
	for (i=0; i<numImages; i++)
	{
		printf("[%d] %s", i, theImages[i]);
		if (i%3 == 2)
			printf("\n");
		else
			printf("\t");
	}
	printf("\n========== ========== ========== ========== ========== ==========\n");

//	printf("Save loaded image? Enter name (???.po or ???.dsk) or <CR>: ");
//	fgets(saveName, 32, stdin);
//	fgets(saveName, 32, stdin);		// do it again since can't flush stdin???

//	length = strlen(saveName) - 1;	// points to last char in saveName
//	if (saveName[length] == '\n')
//		saveName[length] = '\0';

//	if (length > 5)
//		saveDiskImage(saveName);

	printf("Select image to load: ");
	scanf("%d", &selection);
//	if (selection > numImages-1)
//	{
//		printf("Current image: %s\n", loadedImageName);
//		return;
//	}
    if (selection < numImages)
	    loadDiskImage(theImages[selection]);
    else
        printf("*** Bad image number\n");
}

//____________________
void loadDiskImage(const char *imageName)
{
	/*	Loads disk image into theImage
		Accounts for sector interleaving
	*/
	unsigned char trk, sector, translatedSector;
	unsigned char tempBuff[NUM_TRACKS][NUM_SECTORS_PER_TRACK][NUM_BYTES_PER_SECTOR];
	char imagePath[128];
	unsigned int i, offset;
	char *ext;
	size_t numElements;
	FILE *fd;

	printf("\n  --- %s ---\n", imageName);
	sprintf(imagePath, "/root/DiskImages/Small/%s", imageName);
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

	strcpy(loadedImageName, imageName);

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
	/*	Saves disk image to /root/DiskImages/Small/Saved/fileName in format that can be loaded
		Inverse of loadDiskImage()
		Will overwrite existing file!
		Accounts for sector interleaving
	*/
	unsigned char trk, sector, unTranslatedSector, resp;
	unsigned char tempBuff[NUM_TRACKS][NUM_SECTORS_PER_TRACK][NUM_BYTES_PER_SECTOR];
	unsigned char unTranslateSector_DOS[16], unTranslateSector_ProDOS[16];
	char imagePath[128];
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
	sprintf(imagePath, "/root/DiskImages/Small/Saved/%s", fileName);
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
			if (resp == 1)		// decode error occurred
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

//____________________
unsigned char computeDataChecksum(unsigned char *nibble)
{
	// Converts 342 data bytes to 256 bytes & returns checksum (0 if error)
	unsigned char b, xorValue, newValue;
	unsigned int i;

	char data[NUM_BYTES_PER_SECTOR];

	xorValue = 0;
	for (i=0; i<342; i++)
	{
		b = untranslate6[nibble[i]];		// first data
		if (b == 0xFF)
		{
			printf("\n*** ComputeDataChecksum: Out of range in untranslate6: %d\n", nibble[i]);
			return 0;
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
	return xorValue;
}
