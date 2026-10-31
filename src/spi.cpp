/*
    Copyright 2019-2021 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#include "spi.h"
#include "core.h"
#include "settings.h"

void Spi::loadFirmware()
{
    // Attempt to load the firmware
    FILE *firmwareFile = fopen(Settings::getFirmwarePath().c_str(), "rb");
    if (!firmwareFile) throw 1;
    fread(firmware, sizeof(uint8_t), 0x40000, firmwareFile);
    fclose(firmwareFile);
}

void Spi::directBoot()
{
    // Load the user settings into memory
    for (uint32_t i = 0; i < 0x70; i++)
        core->memory.write<uint8_t>(0, 0x27FFC80 + i, firmware[0x3FF00 + i]);
}

void Spi::setTouch(int x, int y)
{
    // Read calibration points from the firmware
    uint16_t adcX1 = U8TO16(firmware, 0x3FF58);
    uint16_t adcY1 = U8TO16(firmware, 0x3FF5A);
    uint8_t  scrX1 = firmware[0x3FF5C];
    uint8_t  scrY1 = firmware[0x3FF5D];
    uint16_t adcX2 = U8TO16(firmware, 0x3FF5E);
    uint16_t adcY2 = U8TO16(firmware, 0x3FF60);
    uint8_t  scrX2 = firmware[0x3FF62];
    uint8_t  scrY2 = firmware[0x3FF63];

    // Ensure the coordinates are within bounds
    // A one pixel border around the screen is ignored to avoid potential underflow/overflow
    // This should be fine; GBATEK says that pressing near the screen borders may be impossible anyways
    if (x < 1) x = 1; else if (x > 254) x = 254;
    if (y < 1) y = 1; else if (y > 190) y = 190;

    // Convert the coordinates to ADC values
    if (scrX2 - scrX1 != 0) touchX = (x - (scrX1 - 1)) * (adcX2 - adcX1) / (scrX2 - scrX1) + adcX1;
    if (scrY2 - scrY1 != 0) touchY = (y - (scrY1 - 1)) * (adcY2 - adcY1) / (scrY2 - scrY1) + adcY1;
}

void Spi::clearTouch()
{
    // Set the ADC values to their default state
    touchX = 0x000;
    touchY = 0xFFF;
}

void Spi::writeSpiCnt(uint16_t mask, uint16_t value)
{
    // Write to the SPICNT register
    mask &= 0xCF03;
    spiCnt = (spiCnt & ~mask) | (value & mask);
}

void Spi::writeSpiData(uint8_t value)
{
    // Don't do anything if the SPI isn't enabled
    if (!(spiCnt & BIT(15)))
    {
        spiData = 0;
        return;
    }

    if (writeCount == 0)
    {
        // On the first write, set the command byte
        command = value;
        address = 0;
        spiData = 0;

        // Firmware flash single-byte commands.
        if (((spiCnt & 0x0300) >> 8) == 1)
        {
            if (command == 0x06) firmwareStatus |= BIT(1);
            if (command == 0x04) firmwareStatus &= ~BIT(1);
        }
    }
    else
    {
        switch ((spiCnt & 0x0300) >> 8) // Device
        {
            case 0: // Power management
            {
                uint8_t index = command & 0x7F;
                if (index >= 5) { spiData = 0; break; }
                if (command & BIT(7))
                    spiData = powerRegs[index];
                else
                {
                    switch (index)
                    {
                        case 0: powerRegs[0] = value & 0x7F; break;
                        case 1: break;
                        case 2: powerRegs[2] = value & 0x01; break;
                        case 3: powerRegs[3] = value & 0x03; break;
                        case 4: powerRegs[4] = value & 0x07; break;
                    }
                    spiData = 0;
                }
                break;
            }

            case 1: // Firmware
            {
                switch (command)
                {
                    case 0x03: // Read data bytes
                    {
                        if (writeCount < 4)
                        {
                            // On writes 2-4, set the 3 byte address to read from
                            address |= value << ((3 - writeCount) * 8);
                        }
                        else
                        {
                            // On writes 5+, read data from the firmware and send it back
                            spiData = (address < 0x40000) ? firmware[address] : 0;

                            // Increment the address
                            // 16-bit mode is bugged; the address is incremented accordingly, but only the lower 8 bits are sent
                            address += (spiCnt & BIT(10)) ? 2 : 1;
                        }
                        break;
                    }

                    case 0x05: // RDSR - Read Status Register
                        spiData = firmwareStatus & 0x03;
                        break;

                    case 0x04: // WRDI
                    case 0x06: // WREN
                        spiData = 0;
                        break;

                    default:
                    {
#ifdef NOODS_PS2
#ifdef DEBUG
                        static unsigned int unknownFirmwareLogs = 0;
                        if (unknownFirmwareLogs < 16)
                            LOG("Write to SPI with unknown firmware command: 0x%X\n", command);
                        else if (unknownFirmwareLogs == 16)
                            LOG("Unknown firmware SPI commands: further messages suppressed on PS2 debug build.\n");
                        unknownFirmwareLogs++;
#else
                        LOG("Write to SPI with unknown firmware command: 0x%X\n", command);
#endif
#else
                        LOG("Write to SPI with unknown firmware command: 0x%X\n", command);
#endif
                        spiData = 0;
                        break;
                    }
                }
                break;
            }

            case 2: // Touchscreen
            {
                switch ((command & 0x70) >> 4) // Channel
                {
                    case 1: // Y-coordinate
                    {
                        // Send the ADC Y coordinate MSB first, with 3 dummy bits in front
                        spiData = ((touchY << 11) & 0xFF00) | ((touchY >> 5) & 0x00FF) >> ((writeCount - 1) % 2);
                        break;
                    }

                    case 5: // X-coordinate
                    {
                        // Send the ADC X coordinate MSB first, with 3 dummy bits in front
                        spiData = ((touchX << 11) & 0xFF00) | ((touchX >> 5) & 0x00FF) >> ((writeCount - 1) % 2);
                        break;
                    }

                    case 0: // Temperature 0
                    case 2: // Auxiliary ADC
                    case 3: // Z1
                    case 4: // Z2
                    case 6: // AUX / microphone
                    case 7: // Temperature 1
                        spiData = 0;
                        break;

                    default:
                    {
#ifdef NOODS_PS2
#ifdef DEBUG
                        static unsigned int unknownTouchLogs = 0;
                        if (unknownTouchLogs < 32)
                            LOG("Write to SPI with unknown touchscreen channel: %d\n", (command & 0x70) >> 4);
                        else if (unknownTouchLogs == 32)
                            LOG("Unknown touchscreen SPI channels: further messages suppressed on PS2 debug build.\n");
                        unknownTouchLogs++;
#else
                        LOG("Write to SPI with unknown touchscreen channel: %d\n", (command & 0x70) >> 4);
#endif
#else
                        LOG("Write to SPI with unknown touchscreen channel: %d\n", (command & 0x70) >> 4);
#endif
                        spiData = 0;
                        break;
                    }
                }
                break;
            }

            default:
            {
                LOG("Write to SPI with unknown device: %d\n", (spiCnt & 0x0300) >> 8);
                spiData = 0;
                break;
            }
        }
    }

    // Keep track of the write count
    if (spiCnt & BIT(11)) // Keep chip selected
        writeCount++;
    else // Deselect chip
        writeCount = 0;

    // Trigger a transfer finished IRQ if enabled
    if (spiCnt & BIT(14))
        core->interpreter[1].sendInterrupt(23);
}
