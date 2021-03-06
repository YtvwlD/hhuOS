/*
 * Copyright (C) 2018 Burak Akguel, Christian Gesse, Fabian Ruhland, Filip Krakowski, Michael Schoettner
 * Heinrich-Heine University
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <devices/isa/Isa.h>
#include <kernel/memory/SystemManagement.h>
#include <kernel/interrupts/IntDispatcher.h>
#include <devices/misc/Pic.h>
#include "SoundBlaster2.h"

SoundBlaster2::SoundBlaster2(uint16_t baseAddress, uint8_t irqNumber, uint8_t dmaChannel) : SoundBlaster(baseAddress),
        irqNumber(irqNumber), dmaChannel(dmaChannel) {
    plugin();
}

void SoundBlaster2::setSamplingRate(uint16_t samplingRate) {
    auto timeConstant = static_cast<uint16_t>(65536 - (256000000 / (samplingRate)));

    writeToDSP(0x40);
    writeToDSP(static_cast<uint8_t>((timeConstant & 0xff00) >> 8));
}

void SoundBlaster2::setBufferSize(uint32_t bufferSize) {
    writeToDSP(0x48);

    writeToDSP(static_cast<uint8_t>((bufferSize - 1) & 0x00ff));
    writeToDSP(static_cast<uint8_t>(((bufferSize - 1) & 0xff00) >> 8));
}

void SoundBlaster2::prepareDma(uint16_t addressOffset, uint32_t bufferSize, bool autoInitialize) {
    Isa::selectChannel(dmaChannel);
    Isa::setMode(dmaChannel, Isa::TRANSFER_MODE_READ, autoInitialize, false, Isa::DMA_MODE_SINGLE_TRANSFER);
    Isa::setAddress(dmaChannel, (uint32_t) SystemManagement::getInstance().getPhysicalAddress(dmaMemory) + addressOffset);
    Isa::setCount(dmaChannel, static_cast<uint16_t>(bufferSize - 1));
    Isa::deselectChannel(dmaChannel);
}

void SoundBlaster2::stopAutoInitialize() {
    writeToDSP(0xda);
}

void SoundBlaster2::ackInterrupt() {
    readBufferStatusPort.inb();
}

void SoundBlaster2::playPcmData(const Pcm &pcm) {
    if(pcm.getAudioFormat() != Pcm::PCM || pcm.getNumChannels() > 1 || pcm.getBitsPerSample() != 8 ||
       pcm.getSamplesPerSecond() < 4000 || pcm.getSamplesPerSecond() > 44100) {
        return;
    }

    if(minorVersion < 1 && pcm.getSamplesPerSecond() > 23000) {
        return;
    }

    uint8_t commandByte = 0x1c;

    if(pcm.getSamplesPerSecond() > 23000) {
        commandByte = 0x90;
    }

    uint32_t dataSize = pcm.getFrameSize() * pcm.getSampleCount();

    soundLock.acquire();

    stopPlaying = false;

    turnSpeakerOn();

    setSamplingRate(static_cast<uint16_t>(pcm.getSamplesPerSecond()));

    bool firstBlock = true;
    uint32_t count = ((dataSize) >= 0x10000) ? 0x10000 : dataSize;
    uint16_t addressOffset = 0;
    memcpy(dmaMemory, pcm.getPcmData(), count);

    prepareDma(addressOffset, dataSize < 0x10000 ? dataSize : 0x10000);
    setBufferSize(dataSize < 0x10000 ? dataSize : 0x8000);

    writeToDSP(commandByte);

    bool stop = false;

    for(uint32_t i = 0x10000; i < dataSize && !stop; i += 0x8000) {
        if(i + 0x8000 >= dataSize || stopPlaying) {
            stopAutoInitialize();
            stop = true;
        }

        receivedInterrupt = false;
        while(!receivedInterrupt);

        count = ((dataSize - i) >= 0x8000) ? 0x8000 : dataSize - i;
        addressOffset = static_cast<uint16_t>(firstBlock ? 0 : 0x8000);
        memcpy(reinterpret_cast<char*>(dmaMemory) + addressOffset, pcm.getPcmData() + i, count);
        memset(reinterpret_cast<char*>(dmaMemory) + addressOffset + count, 0, static_cast<uint32_t>(0x8000 - count));

        firstBlock = !firstBlock;

        ackInterrupt();
    }

    stopAutoInitialize();

    turnSpeakerOff();

    soundLock.release();
}

void SoundBlaster2::stopPlayback() {
    writeToDSP(0xda);
    stopPlaying = true;
}

void SoundBlaster2::plugin() {
    // Older DSPs (version < 4) don't support manual IRQ- and DMA-configuration.
    // They must be configured via jumpers and there is no real way to get the IRQ- and DMA-numbers in software.
    // We just assume the DSP to use IRQ 10 and DMA channel 1, if not specified else in the constructor.

    IntDispatcher::getInstance().assign(static_cast<uint8_t>(32 + irqNumber), *this);
    Pic::getInstance().allow(static_cast<Pic::Interrupt>(irqNumber));
}

void SoundBlaster2::trigger(InterruptFrame &frame) {
    receivedInterrupt = true;
}
