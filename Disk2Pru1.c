/*	Disk2 Interface PRU1
	Handles reading and writing data
	Modern OS, shared memory

	The challenge is the A2 can write a sector of data at any time,
		without warning. Need to regularly check WREQ-.


	Inputs:
		EN-		P8_28	R31_10
		WREQ-	P8_41	R31_4
		WSIG	P8_39_	R31_6

	Outputs:
		RDAT	P8_40_	R30_7
		TEST1	P8_27	R30_8
		TEST2	P8_29	R30_9

	Memory Locations shared with Controller:
		Track start		0x0300		 768
		Track end		0x1A5F		6751

		PRU -> Controller
		0x1B00 = EN-
		0x1B01 = current sector number
		0x1B02 = no write (0), write occurred (1)
		0x1B03 - 0x1B07 = number of write bytes

		Controller -> PRU
		0x1B07 = stop sending data to A2 (1)

		Write data start	0x1C00

	03/27/2020
*/
#include <stdint.h>
#include <pru_cfg.h>
#include "resource_table_empty.h"

// First 0x200 bytes of PRU RAM are STACK & HEAP
#define PRU0_DRAM		0x00000			// offset to Data RAM
volatile unsigned char *PRU1_RAM = (unsigned char *) PRU0_DRAM;

// Fixed PRU Memory Locations
#define TRACK_DATA_ADR		0x0300		// address of track start

#define ENABLE_ADR			0x1B00		// EN- state
#define SECTOR_ADR			0x1B01		// current sector number
#define WRITE_ADR			0x1B02		// 1 = write occurred
#define WRITE_CNT_ADR		0x1B03		// num write bytes, int
#define CONT_INT_ADR		0x1B07		// Controller interrupt, 1 = stop

#define WRITE_DATA_ADR		0x1C00		// address of first write byte

#define NUM_SECTORS_TRACK	16			// sectors per track
#define NUM_BYTES_SECTOR	0x0176		// 374, includes sync, prologue, data, everything

volatile register uint32_t __R30;
volatile register uint32_t __R31;

// Globals
uint32_t ENABLE, WREQ, WSIG;		// inputs
uint32_t RDAT, TEST1, TEST2;		// outputs

void SendSector(unsigned char sector);
void HandleWrite(void);
void InsertBit(signed char bit);

//____________________
int main(int argc, char *argv[])
{
	unsigned int i;
	unsigned char sector;

	// Set I/O constants
	ENABLE	= 0x1<<10;			// P8_28 input
	WREQ	= 0x1<<4;			// P8_41 input
	WSIG	= 0x1<<6;			// P8_39 input
	RDAT	= 0x1<<7;			// P8_40 output
	TEST1	= 0x1<<8;			// P8_27 output
	TEST2	= 0x1<<9;			// P8_29 output

	// Clear SYSCFG[STANDBY_INIT] to enable OCP master port
	CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;

	__R30 &= ~TEST1;		// TEST1 = 0
	__R30 &= ~TEST2;		// TEST2 = 0

	PRU1_RAM[WRITE_ADR] = 0;				// no write, yet


	for (i=0; i<400; i++)
		PRU1_RAM[WRITE_DATA_ADR + i] = 0xFF;


	while (1)
	{
		if ((__R31 & ENABLE) == 0)			// A2 enables us
		{
			PRU1_RAM[ENABLE_ADR] = 0;		// EN- = 0

			sector = 0;
			while ((__R31 & ENABLE) == 0)
			{
				if (PRU1_RAM[CONT_INT_ADR] == 0)	// Controller enables us
				{
//					__R30 |= TEST1;			// TEST1 = 1

					__delay_cycles(2000);			// 10.0 us ???
					SendSector(sector);

					PRU1_RAM[SECTOR_ADR] = sector;	// tell Controller this sector sent

					sector++;
					if (sector == 16)
						sector = 0;

					__R30 &= ~TEST1;		// TEST1 = 0
				}

				else
					while (PRU1_RAM[CONT_INT_ADR] == 1)	// wait here till Controller says go
						__delay_cycles(200);			// 1.0 us ???
			}
		}
		else
		{
			PRU1_RAM[ENABLE_ADR] = 1;		// EN- = 1

			__delay_cycles(200000);			// 1.0 ms
		}
	}
}

//____________________
void SendSector(unsigned char sector)
{
	// Outputs all data for one sector
	unsigned char byteInProgress, bitMask, sendDone;;
	unsigned int sectorAdr;

	// Set up parameters
	sectorAdr = TRACK_DATA_ADR + sector * NUM_BYTES_SECTOR;
	bitMask = 0x80;						// we send msb first
	sendDone = 0;						// 1 = done
	while (sendDone == 0)
	{
		byteInProgress = PRU1_RAM[sectorAdr];
		if (byteInProgress == 0x00)		// end of packet marker
			sendDone = 1;

		byteInProgress &= bitMask;
		if (byteInProgress == bitMask)	// we have a 1
			__R30 &= ~RDAT;				// RDAT = 0
		else
			__R30 |= RDAT;				// RDAT still 1, for timing

		__delay_cycles(350);			// 1.75 us

		__R30 |= RDAT;					// RDAT = 1

		if (bitMask == 1)				// we just sent lsb so time for next byte
		{
			sectorAdr++;
			bitMask = 0x80;

			if ((__R31 & WREQ) == 0)	// is A2 writing something this sectod?
			{
				HandleWrite();
				return;
			}
		}
		else
			bitMask = bitMask >> 1;

		__delay_cycles(410);			// 2.05 us
	}
}

//____________________
void HandleWrite(void)
{
	// WREQ- is 0
	unsigned char edgeCnt, count, lastWSIG;

	PRU1_RAM[WRITE_ADR] = 1;		// tell Controller

	// Set up InsertBit()
	InsertBit(-1);

	__delay_cycles(4100);		// to get to 00 after first synch byte


	// spin for n WSIG rising edges to get past synchs and garbage
	for (edgeCnt=0; edgeCnt<13; edgeCnt++)			// [21]
	{
		while ((__R31 & WSIG) == 0);		// spin while WSIG = 0
		__delay_cycles(200);				// 1 us
		while ((__R31 & WSIG) == WSIG);		// spin while WSIG = 1
		__delay_cycles(1);
	}

	__R30 |= TEST1;		// TEST1 = 1

//	while ((__R31 & WSIG) == WSIG);		// wait for WDAT to go low
//	while ((__R31 & WSIG) == 0);		// wait for WDAT to go high

//	__R30 &= ~TEST1;	// TEST1 = 0

	while (1)
	{
		count = 0;
		lastWSIG = __R31 & WSIG;
		while ((__R31 & WSIG) == lastWSIG)
		{
			count++;
			if (count > 65)	// was 64
				return;

			__delay_cycles(100);	// 0.5 us
		}

		// Convert count into bit(s)
		if (count < 10)			// 1
		{
			InsertBit(1);
		}
		else if (count < 17)	// 01
		{
			InsertBit(0);
			InsertBit(1);
		}
		else if (count < 24)	// 001
		{
			InsertBit(0);
			InsertBit(0);
			InsertBit(1);
		}
		else if (count < 31)	// 0001
		{
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(1);
		}
		else if (count < 38)	// 0 0001
		{
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(1);
		}
		else if (count < 45)	// 00 0001
		{
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(1);
		}
		else if (count < 52)	// 000 0001
		{
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(1);
		}
		else					// 0000 0001
		{
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(0);
			InsertBit(1);
		}
	}
}

//____________________
void InsertBit(signed char bit)
{
	// Insert bit into byteInProcess and put in RAM when byte completed
	// If bit = -1, reset parameters (start of packet)
	static unsigned char bitCnt, byteInProcess;
	static unsigned int writeAdr;

	if (bit == -1)
	{
		bitCnt  = 0;
//		byteCnt = 0;
		byteInProcess = 0x00;
		writeAdr = WRITE_DATA_ADR;
	}
	else
	{
		if (bit == 0)
			byteInProcess &= 0xFE;	// clear LSB
		else
			byteInProcess |= 0x01;	// set LSB

		if (bitCnt == 7)
		{
			PRU1_RAM[writeAdr] = byteInProcess;
			writeAdr++;
			bitCnt = 0;
//			byteCnt++;
		}
		else
		{
			byteInProcess = byteInProcess<<1;	// shift bits left
			bitCnt++;
		}
	}
}



/*

//_____________________________		Capture a sector of write data
WRITING:
	// debounce WREQ-
	AND		r1, r1, r1					// NOP
	AND		r1, r1, r1					// NOP
	QBBS	NEWSECTOR, r31.t4
	SET		TEST1						// TEST1 = 1 when we are in write mode
	
	// Set up write registers
	LDI		r11.b0, 0x00				// last WSIG
	LDI		r13.b0, 0x07				// reset write bit shift count
	LDI		r14.b0, 0x80				// init current write byte in work
	MOV		r15, r21					// init write data ram index
	LDI		r17, 0						// write byte count
	LDI		r1, 6004
	SBBO	r17, r1, 0, 4				// clear write byte count in ram
	
	//======================			Here we count WSIG rising edges to get to first data bit
	LDI		r1.b0, 32
EDGE:
	WBS		r31.t6
	
	LDI		r1.w2, 500
EDGEDEL:
	SUB		r1.w2, r1.w2, 1
	QBNE	EDGEDEL, r1.w2,0
	
	SUB		r1.b0, r1.b0, 1
	QBNE	EDGE, r1.b0, 0
	
	LDI		r1, 800
WD10:
	SUB		r1, r1, 1
	QBNE	WD10, r1, 0
	//=======================

//_____________________________		Stay in Write Loop until WREQ- goes high
WLOOP:
//	AND		r1, r1, r1					// NOP / SET, timing is critical
	SET		TEST2						// 5 ns before WSIG sample
	AND		r10.b0, r31, 0x40			// sample WSIG in r10.b0
	
	LSR		r10.b0, r10.b0, 6			// move bit to bit 0, WSIG = r31.t6
	XOR		r12.b0, r10.b0, r11.b0		// r12.b0 = current WSIG xor last WSIG
	MOV		r11.b0, r10.b0				// copy shifted WSIG into last WSIG
	
	LSL		r12.b1, r12.b0, r13.b0		// shift bit to its location in byte
	OR		r14.b0, r14.b0, r12.b1		// put bit in current write byte in work
	SUB		r13.b0, r13.b0, 1			// decrement write bit shift count
	QBEQ	WBDONE, r13.b0, 0xFF		// finished this byte?
	
WBNOTDONE:
	AND		r1, r1, r1					// NOP 1
	AND		r1, r1, r1					// NOP 2
	AND		r1, r1, r1					// NOP 3
	AND		r1, r1, r1					// NOP 4
	AND		r1, r1, r1					// NOP 5
	AND		r1, r1, r1					// NOP 6
	AND		r1, r1, r1					// NOP 7
	QBA		WRITEDELAY
	
WBDONE:
	SBBO	r14.b0, r15, 0, 1			// put byte in work in ram for Controller
	ADD		r15, r15, 1					// increment write data ram index
	LDI		r13.b0, 0x07				// reset write bit shift count
	LDI		r14.b0, 0x80				// reset current write byte in work
	ADD		r17, r17, 1					// increment write byte count
	
	QBBS	WRITEDONE, r31.t4			// check WREQ- every byte, write is done when WREQ- goes high
	QBA		WRITEDELAY
	
WRITEDELAY:
	LDI		r1, 190
WD2:
	SUB		r1, r1, 1
	QBNE	WD2, r1, 0
	
	CLR		TEST2
//	AND		r1, r1, r1					// CLR / NOP, for timing
	AND		r1, r1, r1					// NOP
	
	LDI		r1, 191
WD3:
	SUB		r1, r1, 1
	QBNE	WD3, r1, 0
	
	QBA		WLOOP


//____________________________		Put some parameters in ram
WRITEDONE:
	LBBO	r1.b0, r24, 0, 1		// get current status
	OR		r1.b0, r1, 0x04			// set write bit
	SBBO	r1.b0, r24, 0, 1		// put status back in ram
	
	SBBO	r17, r24, 4, 4			// put write byte count in ram
	
	CLR		TEST1					// TEST1 = 0 when writing and storing done
	CLR		TEST2
	
	QBA		NEXTSEC
*/

