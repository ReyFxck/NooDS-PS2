/*
    Copyright 2019-2021 Hydr8gon

    This file is part of NooDS.
*/

#include <ctime>

#include "rtc.h"
#include "core.h"

void Rtc::reset()
{
    writeCount = 0;
    command = 0;
    status1 = 0;
    status2 = 0;
    for (int i = 0; i < 7; i++) dateTime[i] = 0;
    for (int i = 0; i < 3; i++) alarm1[i] = alarm2[i] = 0;
    clockAdjust = 0;
    freeReg = 0;
}

void Rtc::updateDateTime()
{
    std::time_t t = std::time(nullptr);
    std::tm *time = std::localtime(&t);
    if (!time) return;

    time->tm_year %= 100;
    time->tm_mon++;

    uint8_t hour24 = static_cast<uint8_t>(time->tm_hour);
    if (!(status1 & BIT(1)))
        time->tm_hour %= 12;

    dateTime[0] = ((time->tm_year / 10) << 4) | (time->tm_year % 10);
    dateTime[1] = ((time->tm_mon  / 10) << 4) | (time->tm_mon  % 10);
    dateTime[2] = ((time->tm_mday / 10) << 4) | (time->tm_mday % 10);
    dateTime[3] = static_cast<uint8_t>((time->tm_wday + 6) % 7);
    dateTime[4] = ((time->tm_hour / 10) << 4) | (time->tm_hour % 10);
    dateTime[5] = ((time->tm_min  / 10) << 4) | (time->tm_min  % 10);
    dateTime[6] = ((time->tm_sec  / 10) << 4) | (time->tm_sec  % 10);

    if (hour24 >= 12)
        dateTime[4] |= BIT(6);
}

bool Rtc::readRegister(uint8_t index)
{
    uint8_t bit = writeCount & 7;

    switch (index)
    {
        case 0: // Status 1
        {
            bool value = (status1 >> bit) & 1;
            if (bit == 7) status1 &= 0x0E;
            return value;
        }

        case 1: // Status 2
            return (status2 >> bit) & 1;

        case 2: // Date/time
            if (writeCount == 8) updateDateTime();
            return (dateTime[(writeCount / 8) - 1] >> bit) & 1;

        case 3: // Time
            if (writeCount == 8) updateDateTime();
            return (dateTime[(writeCount / 8) + 3] >> bit) & 1;

        case 4: // Alarm 1 / INT1
            return (alarm1[((writeCount - 8) / 8) % 3] >> bit) & 1;

        case 5: // Alarm 2 / INT2
            return (alarm2[((writeCount - 8) / 8) % 3] >> bit) & 1;

        case 6: // Clock adjust
            return (clockAdjust >> bit) & 1;

        case 7: // Free register
            return (freeReg >> bit) & 1;
    }

    return 0;
}

void Rtc::writeRegister(uint8_t index, bool value)
{
    uint8_t bit = writeCount & 7;
    uint8_t mask = BIT(bit);

    switch (index)
    {
        case 0: // Status 1
            if (bit == 0 && value)
            {
                reset();
                return;
            }
            if (mask & 0x0E)
                status1 = (status1 & ~mask) | (value ? mask : 0);
            return;

        case 1: // Status 2
            status2 = (status2 & ~mask) | (value ? mask : 0);
            return;

        case 2: // Date/time latch
        {
            int byte = (writeCount / 8) - 1;
            if (byte >= 0 && byte < 7)
                dateTime[byte] = (dateTime[byte] & ~mask) | (value ? mask : 0);
            return;
        }

        case 3: // Time latch
        {
            int byte = (writeCount / 8) + 3;
            if (byte >= 4 && byte < 7)
                dateTime[byte] = (dateTime[byte] & ~mask) | (value ? mask : 0);
            return;
        }

        case 4:
        case 5:
        {
            int byte = ((writeCount - 8) / 8) % 3;
            uint8_t *alarm = (index == 4) ? alarm1 : alarm2;
            alarm[byte] = (alarm[byte] & ~mask) | (value ? mask : 0);
            return;
        }

        case 6:
            clockAdjust = (clockAdjust & ~mask) | (value ? mask : 0);
            return;

        case 7:
            freeReg = (freeReg & ~mask) | (value ? mask : 0);
            return;
    }
}

void Rtc::updateRtc(bool cs, bool sck, bool sio)
{
    if (cs)
    {
        if (!sckCur && sck)
        {
            if (writeCount < 8)
            {
                command |= (sio ? 1 : 0) << (7 - writeCount);

                if (writeCount == 7 && (command & 0xF0) != 0x60)
                {
                    uint8_t reversed = 0;
                    for (int i = 0; i < 8; i++)
                        reversed |= ((command >> i) & 1) << (7 - i);
                    command = reversed;
                }
            }
            else if (command & BIT(0))
            {
                sio = readRegister((command >> 1) & 7);
            }
            else
            {
                writeRegister((command >> 1) & 7, sio);
            }

            writeCount++;
        }
    }
    else
    {
        writeCount = 0;
        command = 0;
    }

    csCur = cs;
    sckCur = sck;
    sioCur = sio;
}

void Rtc::writeRtc(uint8_t value)
{
    rtc = value & ~0x07;

    bool cs  = (rtc & BIT(6)) ? (value & BIT(2)) : csCur;
    bool sck = (rtc & BIT(5)) ? !(value & BIT(1)) : sckCur;
    bool sio = (rtc & BIT(4)) ? (value & BIT(0)) : sioCur;
    updateRtc(cs, sck, sio);
}

uint8_t Rtc::readRtc()
{
    bool cs  = csCur;
    bool sck = (rtc & BIT(5)) ? 0 : sckCur;
    bool sio = (rtc & BIT(4)) ? 0 : sioCur;
    return rtc | (cs << 2) | (sck << 1) | (sio << 0);
}
