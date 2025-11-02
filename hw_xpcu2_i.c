/*-----------------------------------------------------------------------------
 *
 * Hardware-dependent code for usb_jtag
 *-----------------------------------------------------------------------------
 * Copyright (C) 2007 Kolja Waschk, ixo.de
 *-----------------------------------------------------------------------------
 * This code is part of usbjtag. usbjtag is free software; you can redistribute
 * it and/or modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version. usbjtag is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.  You should have received a
 * copy of the GNU General Public License along with this program in the file
 * COPYING; if not, write to the Free Software Foundation, Inc., 51 Franklin
 * St, Fifth Floor, Boston, MA  02110-1301  USA
 *-----------------------------------------------------------------------------
 */

/*
 * Xilinx Platform Cable II (DLC10): Access internal XC3S200A-FT256-4C FPGA
 */

#include "hardware.h"
#include "fx2regs.h"
#include "syncdelay.h"

//---------------------------------------------------------------------------

#define SetOrClear(port, mask, input) \
  ((input) ? (port|=mask) : (port&=~mask))

// PB7 -> tristate buffer -> FPGA_TCK
#define bmTCK bmBIT7 // Output
#define SetTCK(x)     SetOrClear(IOB, bmTCK, x)

// PB4 -> FPGA_TMS
#define bmTMS bmBIT4 // Output
#define SetTMS(x)     SetOrClear(IOB, bmTMS, x)

// PB0 -> FPGA_TDI
#define bmTDI bmBIT0 // Output - Data from FX2 into FPGA
#define SetTDI(x)     SetOrClear(IOB, bmTDI, x)

// PC6 <- FPGA_TDO
#define bmTDO bmBIT6 // Input - Data from FPGA into FX2
#define bitTDO 6

#define GetTDO()      GetTDOToBit(0)
#define GetTDOToBit(bitPos) \
 (((int)(bitTDO-bitPos) > (int)0) ? \
   ((IOC & bmTDO)>>(bitTDO-bitPos)) : \
   ((IOC & bmTDO)<<(bitPos-bitTDO)) ) \

// CTL2 -> active-high output enable for FPGA_TCK
#define bmTCK_OE bmBIT2

// PC7 -> MAX6412 active-low Manual Reset (MR) -> FPGA_PROG_B
#define bmFPGA_RESET bmBIT7

// PE6 -> active-high FPGA power enable
#define bmFPGA_POWER bmBIT6

/* XPCU2 has neither AS nor PS mode pins */

//-----------------------------------------------------------------------------

void ProgIO_Poll(void)    {}
void ProgIO_Enable(void)  {}
void ProgIO_Disable(void) {}
void ProgIO_Deinit(void)  {}

void ProgIO_Init(void)
{
  /* The following code depends on your actual circuit design.
     Make required changes _before_ you try the code! */

  // set the CPU clock to 48MHz, enable clock output to FPGA
  CPUCS = bmCLKOE | bmCLKSPD1;

  // Use internal 48 MHz, enable output, use "Port" mode for all pins
  IFCONFIG = bmIFCLKSRC | bm3048MHZ | bmIFCLKOE;

  GPIFCTLCFG = 0x00;
  GPIFIDLECTL = bmTCK_OE;
  GPIFABORT = 0xFF;

  PORTACFG = 0x00; OEA = 0x00; IOA = 0x00;
  OEB = bmTCK | bmTMS | bmTDI; IOB = 0x00;
  PORTCCFG = 0x00; OEC = bmFPGA_RESET; IOC = bmFPGA_RESET;
  OED = 0x00;
  PORTECFG = 0x00; OEE = bmFPGA_POWER; IOE = bmFPGA_POWER;
}

void ProgIO_Set_State(unsigned char d)
{
  /* Set state of output pins
   * (d is the byte from the host):
   *
   * d.0 => TCK
   * d.1 => TMS
   * d.2 => nCE (only #ifdef HAVE_AS_MODE)
   * d.3 => nCS (only #ifdef HAVE_AS_MODE)
   * d.4 => TDI
   * d.6 => LED / Output Enable
   */

  SetTCK((d & bmBIT0) ? 1 : 0);
  SetTMS((d & bmBIT1) ? 1 : 0);
  SetTDI((d & bmBIT4) ? 1 : 0);
#ifdef HAVE_OE_LED
  SetOELED((d & bmBIT5) ? 1 : 0);
#endif
}

unsigned char ProgIO_Set_Get_State(unsigned char d)
{
  /* Set state of output pins (s.a.)
   * then read state of input pins:
   *
   * TDO => d.0
   * DATAOUT => d.1 (only #ifdef HAVE_AS_MODE)
   */

  ProgIO_Set_State(d);
  return 2|GetTDO(); /* DATAOUT assumed high, no AS mode */
}

void ProgIO_ShiftOut(unsigned char c)
{
  /* Shift out byte C:
   *
   * 8x {
   *   Output least significant bit on TDI
   *   Raise TCK
   *   Shift c right
   *   Lower TCK
   * }
   */

  unsigned char lc=c;

  SetTDI(lc & bmBIT0); SetTCK(1); lc>>=1; SetTCK(0);
  SetTDI(lc & bmBIT0); SetTCK(1); lc>>=1; SetTCK(0);
  SetTDI(lc & bmBIT0); SetTCK(1); lc>>=1; SetTCK(0);
  SetTDI(lc & bmBIT0); SetTCK(1); lc>>=1; SetTCK(0);

  SetTDI(lc & bmBIT0); SetTCK(1); lc>>=1; SetTCK(0);
  SetTDI(lc & bmBIT0); SetTCK(1); lc>>=1; SetTCK(0);
  SetTDI(lc & bmBIT0); SetTCK(1); lc>>=1; SetTCK(0);
  SetTDI(lc & bmBIT0); SetTCK(1); lc>>=1; SetTCK(0);
}

unsigned char ProgIO_ShiftInOut(unsigned char c)
{
  /* Shift out byte C, shift in from TDO:
   *
   * 8x {
   *   Read carry from TDO
   *   Output least significant bit on TDI
   *   Raise TCK
   *   Shift c right, append carry (TDO) at left (into MSB)
   *   Lower TCK
   * }
   * Return c.
   */

  unsigned char carry;
  unsigned char lc=c;

  carry = GetTDOToBit(7); SetTDI(lc & bmBIT0); SetTCK(1); lc=carry|(lc>>1); SetTCK(0); 
  carry = GetTDOToBit(7); SetTDI(lc & bmBIT0); SetTCK(1); lc=carry|(lc>>1); SetTCK(0);
  carry = GetTDOToBit(7); SetTDI(lc & bmBIT0); SetTCK(1); lc=carry|(lc>>1); SetTCK(0);
  carry = GetTDOToBit(7); SetTDI(lc & bmBIT0); SetTCK(1); lc=carry|(lc>>1); SetTCK(0);

  carry = GetTDOToBit(7); SetTDI(lc & bmBIT0); SetTCK(1); lc=carry|(lc>>1); SetTCK(0);
  carry = GetTDOToBit(7); SetTDI(lc & bmBIT0); SetTCK(1); lc=carry|(lc>>1); SetTCK(0);
  carry = GetTDOToBit(7); SetTDI(lc & bmBIT0); SetTCK(1); lc=carry|(lc>>1); SetTCK(0);
  carry = GetTDOToBit(7); SetTDI(lc & bmBIT0); SetTCK(1); lc=carry|(lc>>1); SetTCK(0);

  return lc;
}


