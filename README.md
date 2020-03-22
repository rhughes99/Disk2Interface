--- DIsk2InterfaceSharedMem Notes

How to run:
0) Before applying power
	SD card not inserted
	Enable toggle switch in center position
	A2 power off

1) source setup.sh

2) Set enable toggle switch

3) Insert SD card and mount
	mount -v /dev/mmcblk0p1 /root/DiskImages

4) make

5) ./Controller

6) Turn on A2	


(Ins and Outs relative to BBB)
PRU 0:
	Inputs:	
	P0		P9_31	r31.t0
	P1		P9_29	r31.t1
	P2		P9_30	r31.t2
	P3		P9_28	r31.t3
	EN-		P9_27	r31.t5

PRU 1:
	Inputs:
	EN-		P8_28	r31.t10
	WREQ-	P8_41	r31.t4
	WSIG	P8_39	r31.t6

	Outputs:
	RDAT	P8_40	r30.t7
	TEST1	P8_27	r30.t8
	TEST2	P8_29	r30.t9


gcc Disk2Controller.c -o Controller