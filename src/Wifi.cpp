/*
    Copyright 2016-2017 StapleButter

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <string.h>
#include "NDS.h"
#include "SPI.h"
#include "Wifi.h"


namespace Wifi
{

u8 RAM[0x2000];
u16 IO[0x1000>>1];

#define IOPORT(x) IO[(x)>>1]

u16 Random;

u64 USCounter;
u64 USCompare;

u16 BBCnt;
u8 BBWrite;
u8 BBRegs[0x100];
u8 BBRegsRO[0x100];

u8 RFVersion;
u16 RFCnt;
u16 RFData1;
u16 RFData2;
u32 RFRegs[0x40];


void Reset()
{
    memset(RAM, 0, 0x2000);
    memset(IO, 0, 0x1000);

    Random = 1;

    memset(BBRegs, 0, 0x100);
    memset(BBRegsRO, 0, 0x100);

    #define BBREG_FIXED(id, val)  BBRegs[id] = val; BBRegsRO[id] = 1;
    BBREG_FIXED(0x00, 0x6D);
    BBREG_FIXED(0x0D, 0x00);
    BBREG_FIXED(0x0E, 0x00);
    BBREG_FIXED(0x0F, 0x00);
    BBREG_FIXED(0x10, 0x00);
    BBREG_FIXED(0x11, 0x00);
    BBREG_FIXED(0x12, 0x00);
    BBREG_FIXED(0x16, 0x00);
    BBREG_FIXED(0x17, 0x00);
    BBREG_FIXED(0x18, 0x00);
    BBREG_FIXED(0x19, 0x00);
    BBREG_FIXED(0x1A, 0x00);
    BBREG_FIXED(0x27, 0x00);
    BBREG_FIXED(0x4D, 0x00); // 00 or BF
    BBREG_FIXED(0x5D, 0x01);
    BBREG_FIXED(0x5E, 0x00);
    BBREG_FIXED(0x5F, 0x00);
    BBREG_FIXED(0x60, 0x00);
    BBREG_FIXED(0x61, 0x00);
    BBREG_FIXED(0x64, 0xFF); // FF or 3F
    BBREG_FIXED(0x66, 0x00);
    for (int i = 0x69; i < 0x100; i++)
    {
        BBREG_FIXED(i, 0x00);
    }
    #undef BBREG_FIXED

    RFVersion = SPI_Firmware::GetRFVersion();
    memset(RFRegs, 0, 4*0x40);

    memset(&IOPORT(0x018), 0xFF, 6);
    memset(&IOPORT(0x020), 0xFF, 6);
}


void SetIRQ(u32 irq)
{
    u32 oldflags = IOPORT(W_IF) & IOPORT(W_IE);

    IOPORT(W_IF) |= (1<<irq);
    u32 newflags = IOPORT(W_IF) & IOPORT(W_IE);

    if ((oldflags == 0) && (newflags != 0))
        NDS::SetIRQ(1, NDS::IRQ_Wifi);
}

void SetIRQ13()
{
    SetIRQ(13);

    if (!(IOPORT(W_PowerTX) & 0x0002))
    {
        IOPORT(0x034) = 0x0002;
        // TODO: 03C
        IOPORT(W_RFPins) = 0x0046;
        IOPORT(W_RFStatus) = 9;
    }
}

void SetIRQ14(bool forced)
{
    SetIRQ(14);

    if (!forced)
        IOPORT(W_BeaconCount1) = IOPORT(W_BeaconInterval);
    else
        printf("wifi: weird forced IRQ14\n");

    IOPORT(W_BeaconCount2) = 0xFFFF;
    IOPORT(W_TXReqRead) &= 0xFFF2; // todo, eventually?

    // TODO: actually send beacon

    if (IOPORT(W_ListenCount) == 0)
        IOPORT(W_ListenCount) = IOPORT(W_ListenInterval);

    IOPORT(W_ListenCount)--;
}

void SetIRQ15()
{
    SetIRQ(15);

    if (IOPORT(W_PowerTX) & 0x0001)
    {
        IOPORT(W_RFPins) |= 0x0080;
        IOPORT(W_RFStatus) = 1;
    }
}


void MSTimer()
{
    IOPORT(W_BeaconCount1)--;
    if (IOPORT(W_USCompareCnt))
    {
        if (IOPORT(W_BeaconCount1) == 0) SetIRQ14(false);
    }

    if (IOPORT(W_BeaconCount2) != 0)
    {
        IOPORT(W_BeaconCount2)--;
        if (IOPORT(W_BeaconCount2) == 0) SetIRQ13();
    }
}

void USTimer(u32 param)
{
    if (IOPORT(W_USCountCnt))
    {
        USCounter++;
        u32 uspart = (USCounter & 0x3FF);

        if (IOPORT(W_USCompareCnt))
        {
            if (USCounter == USCompare) SetIRQ14(false);

            u32 beaconus = (IOPORT(W_BeaconCount1) << 10) | (0x3FF - uspart);
            if (beaconus == IOPORT(W_PreBeacon)) SetIRQ15();
        }

        if (!uspart) MSTimer();
    }

    if (IOPORT(W_ContentFree) != 0)
        IOPORT(W_ContentFree)--;

    // TODO: make it more accurate, eventually
    // in the DS, the wifi system has its own 22MHz clock and doesn't use the system clock
    NDS::ScheduleEvent(NDS::Event_Wifi, true, 33, USTimer, 0);
}


void RFTransfer_Type2()
{
    u32 id = (IOPORT(W_RFData2) >> 2) & 0x1F;

    if (IOPORT(W_RFData2) & 0x0080)
    {
        u32 data = RFRegs[id];
        IOPORT(W_RFData1) = data & 0xFFFF;
        IOPORT(W_RFData2) = (IOPORT(W_RFData2) & 0xFFFC) | ((data >> 16) & 0x3);
    }
    else
    {
        u32 data = IOPORT(W_RFData1) | ((IOPORT(W_RFData2) & 0x0003) << 16);
        RFRegs[id] = data;
    }
}

void RFTransfer_Type3()
{
    u32 id = (IOPORT(W_RFData1) >> 8) & 0x3F;

    u32 cmd = IOPORT(W_RFData2) & 0xF;
    if (cmd == 6)
    {
        IOPORT(W_RFData1) = (IOPORT(W_RFData1) & 0xFF00) | (RFRegs[id] & 0xFF);
    }
    else if (cmd == 5)
    {
        u32 data = IOPORT(W_RFData1) & 0xFF;
        RFRegs[id] = data;
    }
}


// TODO: wifi waitstates

u16 Read(u32 addr)
{
    if (addr >= 0x04810000)
        return 0;

    addr &= 0x7FFE;
    //printf("WIFI: read %08X\n", addr);
    if (addr >= 0x4000 && addr < 0x6000)
    {
        return *(u16*)&RAM[addr & 0x1FFE];
    }
    if (addr >= 0x2000 && addr < 0x4000)
        return 0xFFFF; // checkme

    bool activeread = (addr < 0x1000);

    switch (addr)
    {
    case W_Random: // random generator. not accurate
        Random = (Random & 0x1) ^ (((Random & 0x3FF) << 1) | (Random >> 10));
        return Random;

    case W_Preamble:
        return IOPORT(W_Preamble) & 0x0003;

    case W_USCount0: return (u16)(USCounter & 0xFFFF);
    case W_USCount1: return (u16)((USCounter >> 16) & 0xFFFF);
    case W_USCount2: return (u16)((USCounter >> 32) & 0xFFFF);
    case W_USCount3: return (u16)(USCounter >> 48);

    case W_USCompare0: return (u16)(USCompare & 0xFFFF);
    case W_USCompare1: return (u16)((USCompare >> 16) & 0xFFFF);
    case W_USCompare2: return (u16)((USCompare >> 32) & 0xFFFF);
    case W_USCompare3: return (u16)(USCompare >> 48);

    case W_BBRead:
        if ((IOPORT(W_BBCnt) & 0xF000) != 0x6000)
        {
            printf("WIFI: bad BB read, CNT=%04X\n", IOPORT(W_BBCnt));
            return 0;
        }
        return BBRegs[IOPORT(W_BBCnt) & 0xFF];

    case W_BBBusy:
        return 0; // TODO eventually (BB busy flag)
    case W_RFBusy:
        return 0; // TODO eventually (RF busy flag)

    case W_RXBufDataRead:
        if (activeread)
        {
            u32 rdaddr = IOPORT(W_RXBufReadAddr) & 0x1FFE;

            u16 ret = *(u16*)&RAM[rdaddr];

            rdaddr += 2;
            if (rdaddr == (IOPORT(W_RXBufEnd) & 0x1FFE))
                rdaddr = (IOPORT(W_RXBufBegin) & 0x1FFE);
            if (rdaddr == (IOPORT(W_RXBufGapAddr) & 0x1FFE))
            {
                rdaddr += ((IOPORT(W_RXBufGapSize) & 0x0FFF) << 1);
                if (rdaddr >= (IOPORT(W_RXBufEnd) & 0x1FFE))
                    rdaddr = rdaddr + (IOPORT(W_RXBufBegin) & 0x1FFE) - (IOPORT(W_RXBufEnd) & 0x1FFE);
            }

            IOPORT(W_RXBufReadAddr) = rdaddr & 0x1FFE;
            IOPORT(W_RXBufDataRead) = ret;
        }
        break;
    }

    //printf("WIFI: read %08X\n", addr);
    return IOPORT(addr&0xFFF);
}

void Write(u32 addr, u16 val)
{
    if (addr >= 0x04810000)
        return;

    addr &= 0x7FFE;
    //printf("WIFI: write %08X %04X\n", addr, val);
    if (addr >= 0x4000 && addr < 0x6000)
    {
        *(u16*)&RAM[addr & 0x1FFE] = val;
        return;
    }
    if (addr >= 0x2000 && addr < 0x4000)
        return;

    switch (addr)
    {
    case W_ModeReset:
        {
            u16 oldval = IOPORT(W_ModeReset);

            if (!(oldval & 0x0001) && (val & 0x0001))
            {
                IOPORT(0x034) = 0x0002;
                IOPORT(W_RFPins) = 0x0046;
                IOPORT(W_RFStatus) = 9;
                IOPORT(0x27C) = 0x0005;
                // TODO: 02A2??
            }
            else if ((oldval & 0x0001) && !(val & 0x0001))
            {
                IOPORT(0x27C) = 0x000A;
            }

            if (val & 0x2000)
            {
                IOPORT(W_RXBufWriteAddr) = 0;
                IOPORT(W_CmdTotalTime) = 0;
                IOPORT(W_CmdReplyTime) = 0;
                IOPORT(0x1A4) = 0;
                IOPORT(0x278) = 0x000F;
                // TODO: other ports??
            }
            if (val & 0x4000)
            {
                IOPORT(W_ModeWEP) = 0;
                IOPORT(W_TXStatCnt) = 0;
                IOPORT(0x00A) = 0;
                IOPORT(W_MACAddr0) = 0;
                IOPORT(W_MACAddr1) = 0;
                IOPORT(W_MACAddr2) = 0;
                IOPORT(W_BSSID0) = 0;
                IOPORT(W_BSSID1) = 0;
                IOPORT(W_BSSID2) = 0;
                IOPORT(W_AIDLow) = 0;
                IOPORT(W_AIDFull) = 0;
                IOPORT(W_TXRetryLimit) = 0x0707;
                IOPORT(0x02E) = 0;
                IOPORT(W_RXBufBegin) = 0x4000;
                IOPORT(W_RXBufEnd) = 0x4800;
                IOPORT(W_TXBeaconTIM) = 0;
                IOPORT(W_Preamble) = 0x0001;
                IOPORT(W_RXFilter) = 0x0401;
                IOPORT(0x0D4) = 0x0001;
                IOPORT(W_RXFilter2) = 0x0008;
                IOPORT(0x0EC) = 0x3F03;
                IOPORT(W_TXHeaderCnt) = 0;
                IOPORT(0x198) = 0;
                IOPORT(0x1A2) = 0x0001;
                IOPORT(0x224) = 0x0003;
                IOPORT(0x230) = 0x0047;

            }
        }
        break;

    case W_ModeWEP:
        val &= 0x007F;
        break;

    case W_IF:
        IOPORT(W_IF) &= ~val;
        return;
    case W_IFSet:
        IOPORT(W_IF) |= (val & 0xFBFF);
        printf("wifi: force-setting IF %04X\n", val);
        return;

    case W_PowerState:
        if (val & 0x0002)
        {
            // TODO: delay for this
            SetIRQ(11);
            IOPORT(W_PowerState) = 0x0000;
        }
        return;
    case W_PowerForce:
        if ((val&0x8001)==0x8000) printf("WIFI: forcing power %04X\n", val);
        val &= 0x8001;
        if (val == 0x8001)
        {
            IOPORT(0x034) = 0x0002;
            IOPORT(W_PowerState) = 0x0200;
            IOPORT(W_TXReqRead) = 0;
            IOPORT(W_RFPins) = 00046;
            IOPORT(W_RFStatus) = 9;
        }
        break;
    case W_PowerUS:
        // schedule timer event when the clock is enabled
        // TODO: check whether this resets USCOUNT (and also which other events can reset it)
        if ((IOPORT(W_PowerUS) & 0x0001) && !(val & 0x0001))
            NDS::ScheduleEvent(NDS::Event_Wifi, true, 33, USTimer, 0);
        else if (!(IOPORT(W_PowerUS) & 0x0001) && (val & 0x0001))
            NDS::CancelEvent(NDS::Event_Wifi);
        break;

    case W_USCountCnt: val &= 0x0001; break;
    case W_USCompareCnt:
        if (val & 0x0002) SetIRQ14(true);
        val &= 0x0001;
        break;

    case W_USCount0: USCounter = (USCounter & 0xFFFFFFFFFFFF0000) | (u64)val; return;
    case W_USCount1: USCounter = (USCounter & 0xFFFFFFFF0000FFFF) | ((u64)val << 16); return;
    case W_USCount2: USCounter = (USCounter & 0xFFFF0000FFFFFFFF) | ((u64)val << 32); return;
    case W_USCount3: USCounter = (USCounter & 0x0000FFFFFFFFFFFF) | ((u64)val << 48); return;

    case W_USCompare0:
        USCompare = (USCompare & 0xFFFFFFFFFFFF0000) | (u64)(val & 0xFC00);
        if (val & 0x03FF)
            printf("wifi: mysterious USCOMPARE bits set %08X%08X %04X\n", (u32)(USCompare>>32), (u32)USCompare, val); // TODO
        return;
    case W_USCompare1: USCompare = (USCompare & 0xFFFFFFFF0000FFFF) | ((u64)val << 16); return;
    case W_USCompare2: USCompare = (USCompare & 0xFFFF0000FFFFFFFF) | ((u64)val << 32); return;
    case W_USCompare3: USCompare = (USCompare & 0x0000FFFFFFFFFFFF) | ((u64)val << 48); return;

    case W_BBCnt:
        IOPORT(W_BBCnt) = val;
        if ((IOPORT(W_BBCnt) & 0xF000) == 0x5000)
        {
            u32 regid = IOPORT(W_BBCnt) & 0xFF;
            if (!BBRegsRO[regid])
                BBRegs[regid] = IOPORT(W_BBWrite) & 0xFF;
        }
        return;

    case W_RFData2:
        IOPORT(W_RFData2) = val;
        if (RFVersion == 3) RFTransfer_Type3();
        else                RFTransfer_Type2();
        return;
    case W_RFCnt:
        val &= 0x413F;
        break;


    case W_TXBufDataWrite:
        {
            u32 wraddr = IOPORT(W_TXBufWriteAddr) & 0x1FFE;
            *(u16*)&RAM[wraddr] = val;

            wraddr += 2;
            if (wraddr == (IOPORT(W_TXBufGapAddr) & 0x1FFE))
                wraddr += ((IOPORT(W_TXBufGapSize) & 0x0FFF) << 1);

            IOPORT(W_TXBufWriteAddr) = wraddr & 0x1FFE;
        }
        return;

    case 0x80:
        printf("BEACON ADDR %04X\n", val);
        break;

    // read-only ports
    case 0x000:
    case 0x044:
    case 0x054:
    case 0x0B0:
    case 0x0B6:
    case 0x0B8:
    case 0x15C:
    case 0x15E:
    case 0x180:
    case 0x19C:
    case 0x1A8:
    case 0x1AC:
    case 0x1C4:
    case 0x210:
    case 0x214:
    case 0x268:
        return;
    }

    //printf("WIFI: write %08X %04X\n", addr, val);
    IOPORT(addr&0xFFF) = val;
}

}
