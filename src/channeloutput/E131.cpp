/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the GPL v2 as described in the
 * included LICENSE.GPL file.
 */

#include "fpp-pch.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <signal.h>
#include <unistd.h>

#include "ChannelOutputSetup.h"
#include "E131.h"
#include "FPD.h"
#include "Universe.h"
#include "channeloutputthread.h"
#include "e131defs.h"
#include "fpp.h"
#include "ping.h"

const unsigned char E131header[] = {
    0x00, 0x10, 0x00, 0x00, 0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00,
    0x72, 0x6e, 0x00, 0x00, 0x00, 0x04, 'F', 'A', 'L', 'C', 'O', 'N', ' ', 'F', 'P', 'P',
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x58, 0x00, 0x00, 0x00, 0x02, 'F', 'A', 'L', 'C',
    'O', 'N', 'C', 'H', 'R', 'I', 'S', 'T', 'M', 'A', 'S', '.', 'C', 'O', 'M', ' ',
    'B', 'Y', ' ', 'D', 'P', 'I', 'T', 'T', 'S', ' ', 'A', 'N', 'D', ' ', 'M', 'Y',
    'K', 'R', 'O', 'F', 'T', ' ', 'F', 'A', 'L', 'C', 'O', 'N', ' ', 'P', 'I', ' ',
    'P', 'L', 'A', 'Y', 'E', 'R', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x72, 0x0b, 0x02, 0xa1, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x00
};

static const std::string E131TYPE = "e1.31";

const std::string& E131OutputData::GetOutputTypeString() const {
    return E131TYPE;
}

E131OutputData::E131OutputData(const Json::Value& config) :
    UDPOutputData(config),
    universeCount(1) {
    sockaddr_in e131Address;
    memset((char*)&e131Address, 0, sizeof(sockaddr_in));
    e131Address.sin_family = AF_INET;
    e131Address.sin_port = htons(E131_DEST_PORT);

    universe = config["id"].asInt();
    priority = config["priority"].asInt();
    if (config.isMember("universeCount")) {
        universeCount = config["universeCount"].asInt();
    }
    if (universeCount < 1) {
        universeCount = 1;
    }
    switch (type) {
    case 0: // Multicast
        ipAddress = "";
        break;
    case 1: //UnicastAddress
        ipAddress = config["address"].asString();
        break;
    }

    if (type == E131_TYPE_MULTICAST) {
        int UniverseOctet[2];
        char sAddress[32];

        UniverseOctet[0] = universe / 256;
        UniverseOctet[1] = universe % 256;
        snprintf(sAddress, sizeof(sAddress), "239.255.%d.%d", UniverseOctet[0], UniverseOctet[1]);
        e131Address.sin_addr.s_addr = inet_addr(sAddress);
    } else {
        e131Address.sin_addr.s_addr = toInetAddr(ipAddress, valid);
        if (!valid && active) {
            WarningHolder::AddWarning("Could not resolve host name " + ipAddress + " - disabling output");
            active = false;
        }
    }

    e131Iovecs.resize(universeCount * 2);
    e131Headers.resize(universeCount);
    e131Addresses.resize(universeCount);
    for (int x = 0; x < universeCount; x++) {
        if (type == E131_TYPE_MULTICAST) {
            int UniverseOctet[2];
            char sAddress[32];

            int u = universe + x;
            UniverseOctet[0] = u / 256;
            UniverseOctet[1] = u % 256;
            snprintf(sAddress, sizeof(sAddress), "239.255.%d.%d", UniverseOctet[0], UniverseOctet[1]);
            e131Address.sin_addr.s_addr = inet_addr(sAddress);
        }
        e131Addresses[x] = e131Address;

        unsigned char* e131Buffer = (unsigned char*)malloc(E131_HEADER_LENGTH);
        e131Headers[x] = e131Buffer;
        memcpy(e131Buffer, E131header, E131_HEADER_LENGTH);

        int uni = universe + x;
        e131Buffer[E131_PRIORITY_INDEX] = priority;
        e131Buffer[E131_UNIVERSE_INDEX] = (char)(uni / 256);
        e131Buffer[E131_UNIVERSE_INDEX + 1] = (char)(uni % 256);

        // Property Value Count
        e131Buffer[E131_COUNT_INDEX] = ((channelCount + 1) / 256);
        e131Buffer[E131_COUNT_INDEX + 1] = ((channelCount + 1) % 256);

        // RLP Protocol flags and length
        int count = 638 - 16 - (512 - (channelCount));
        e131Buffer[E131_RLP_COUNT_INDEX] = (count / 256) + 0x70;
        e131Buffer[E131_RLP_COUNT_INDEX + 1] = count % 256;

        // Framing Protocol flags and length
        count = 638 - 38 - (512 - (channelCount));
        e131Buffer[E131_FRAMING_COUNT_INDEX] = (count / 256) + 0x70;
        e131Buffer[E131_FRAMING_COUNT_INDEX + 1] = count % 256;

        // DMP Protocol flags and length
        count = 638 - 115 - (512 - (channelCount));
        e131Buffer[E131_DMP_COUNT_INDEX] = (count / 256) + 0x70;
        e131Buffer[E131_DMP_COUNT_INDEX + 1] = count % 256;

        e131Buffer[E131_SEQUENCE_INDEX] = 0;

        // use scatter/gather for the packet.   One IOV will contain
        // the header, the second will point into the raw channel data
        // and will be set at output time.   This avoids any memcpy.
        e131Iovecs[x * 2].iov_base = e131Buffer;
        e131Iovecs[x * 2].iov_len = E131_HEADER_LENGTH;
        e131Iovecs[x * 2 + 1].iov_base = nullptr;
        e131Iovecs[x * 2 + 1].iov_len = channelCount;
    }
}

E131OutputData::~E131OutputData() {
    for (auto a : e131Headers) {
        free(a);
    }
}

bool E131OutputData::IsPingable() {
    return type == 1;
}

void E131OutputData::PrepareData(unsigned char* channelData, UDPOutputMessages& msgs) {
    if (valid && active) {
        unsigned char* cur = channelData + startChannel - 1;
        int start = 0;
        bool skipped = false;
        bool allSkipped = true;
        for (int x = 0; x < universeCount; x++) {
            if (NeedToOutputFrame(channelData, startChannel - 1, start, channelCount)) {
                struct mmsghdr msg;
                memset(&msg, 0, sizeof(msg));

                msg.msg_hdr.msg_name = &e131Addresses[x];
                msg.msg_hdr.msg_namelen = sizeof(sockaddr_in);
                msg.msg_hdr.msg_iov = &e131Iovecs[x * 2];
                msg.msg_hdr.msg_iovlen = 2;
                msg.msg_len = channelCount + E131_HEADER_LENGTH;
                if (type == E131_TYPE_MULTICAST) {
                    msgs[MULTICAST_MESSAGES_KEY].push_back(msg);
                } else {
                    msgs[e131Addresses[x].sin_addr.s_addr].push_back(msg);
                }

                ++e131Headers[x][E131_SEQUENCE_INDEX];
                e131Iovecs[x * 2 + 1].iov_base = (void*)cur;
                allSkipped = false;
            } else {
                skipped = true;
            }
            cur += channelCount;
            start += channelCount;
        }
        if (skipped) {
            skippedFrames++;
        } else {
            skippedFrames = 0;
        }
        if (!allSkipped) {
            SaveFrame(&channelData[startChannel - 1], start);
        }
    }
}

void E131OutputData::GetRequiredChannelRange(int& min, int& max) {
    min = startChannel - 1;
    max = startChannel + (channelCount * universeCount) - 1;
}

void E131OutputData::DumpConfig() {
    LogDebug(VB_CHANNELOUT, "E1.31 Universe: %s   %d:%d:%d:%d:%d:%d  %s\n",
             description.c_str(),
             active,
             universe,
             startChannel,
             channelCount,
             type,
             universeCount,
             ipAddress.c_str());
}
