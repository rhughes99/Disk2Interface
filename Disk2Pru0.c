/*	Disk2 Interface PRU0
	Monitors Phase inputs and determines current track
	Modern OS, shared memory

	Inputs:	
		P0		P9_31	R31_0
		P1		P9_29	R31_1
		P2		P9_30	R31_2
		P3		P9_28	R31_3
		EN-		P9_27	R31_5

	Outputs:
		None

	Memory Locations shared with Controller:
		Track number	0x300

	03/19/2020
*/
#include <stdint.h>
#include <pru_cfg.h>
#include "resource_table_empty.h"

// First 0x200 bytes of PRU RAM are STACK & HEAP
#define PRU0_DRAM		0x00000			// Offset to Data RAM
volatile unsigned char *PRU0_RAM = (unsigned char *) PRU0_DRAM;

// Fixed PRU Memory Locations
#define TRK_NUM_ADR		0x0300			// address of current track

volatile register uint32_t __R31;

//____________________
int main(int argc, char *argv[])
{
	unsigned char lastPhaseIn, newPhase1, newPhase2, track, phaseTrk, cogLocation;
	uint32_t PHASE0, PHASE1, PHASE2, PHASE3, ENABLE;	// inputs

	// Set I/O constants
	PHASE0 = 0x01<<0;
	PHASE1 = 0x01<<1;
	PHASE2 = 0x01<<2;
	PHASE3 = 0x01<<3;
	ENABLE = 0x01<<5;

	// Clear SYSCFG[STANDBY_INIT] to enable OCP master port
	CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;

	lastPhaseIn = 0x1F;				// to force a "new" phase report
	track = 0;
	phaseTrk = 0;
	cogLocation = 0;

	while (1)
	{
		while ((__R31 & ENABLE) == ENABLE)	// wait for Enable to go low
			__delay_cycles(200000);			// 1 ms

		// Drive is enabled
		newPhase1 = __R31 & (PHASE0 | PHASE1 | PHASE2 | PHASE3);	// sample phase inputs

		// Wait and then sample again; avoid reacting to short glitches on P0
		__delay_cycles(200000);				// 1 ms

		newPhase2 = __R31 & (PHASE0 | PHASE1 | PHASE2 | PHASE3);

		if (newPhase1 == newPhase2)			// consider phase valid
		{
			if (lastPhaseIn != newPhase1)	// any change?
			{
				lastPhaseIn = newPhase1;
				
				cogLocation = 1 << (phaseTrk % 4);

				if (newPhase1 && !(cogLocation & newPhase1))
				{	
					if (((cogLocation << 1) & newPhase1) || ((cogLocation >> 3) & newPhase1))
					{
						if (phaseTrk < 69)
							phaseTrk++;
					}
					else if (((cogLocation >> 1) & newPhase1) || ((cogLocation << 3) & newPhase1))
					{
						if (phaseTrk > 0)
							phaseTrk--;
					}

					if (track != (phaseTrk>>1))
						track = phaseTrk >> 1;
				}

				PRU0_RAM[TRK_NUM_ADR] = track;		// update track for Controller
			}
		}
	}
}
