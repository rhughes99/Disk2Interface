#!/bin/bash

echo Setting up PRU pins...

# Configure PRU pins
# Inputs:
# P0		P9_31
# P1		P9_29
# P2		P9_30
# P3		P9_28
# EN-		P9_27
# EN-		P8_28
# WREQ-		P8_41
# WSIG		P8_39
config-pin P9_31 pruin
config-pin P9_29 pruin
config-pin P9_30 pruin
config-pin P9_28 pruin
config-pin P9_27 pruin
config-pin P8_28 pruin
config-pin P8_41 pruin
config-pin P8_39 pruin

# Outputs:
# RDAT		P8_40
# TEST1		P8_27
# TEST2		P8_29
config-pin P8_40 pruout
config-pin P8_27 pruout
config-pin P8_29 pruout
