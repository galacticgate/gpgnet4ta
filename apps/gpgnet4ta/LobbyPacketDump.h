#pragma once

#include "tapacket/TPacket.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// GG debug: hex-logs the TA conversation in both directions, the first few of each kind, so a
// real player's lobby traffic (unit sync above all) can be read off a test build's game log.
// "local" is what this machine's TA sent; "remote" is what it received.
class LobbyPacketDump : public tapacket::TaPacketHandler
{
public:
    void onDplaySuperEnumPlayerReply(std::uint32_t dplayId, const std::string& playerName, tapacket::DPAddress* tcp, tapacket::DPAddress* udp) override;
    void onDplayCreateOrForwardPlayer(std::uint16_t command, std::uint32_t dplayId, const std::string& name, tapacket::DPAddress* tcp, tapacket::DPAddress* udp) override;
    void onDplayDeletePlayer(std::uint32_t dplayId) override;
    void onTaPacket(std::uint32_t sourceDplayId, std::uint32_t otherDplayId, bool isLocalSource, const char* encrypted, int sizeEncrypted,
        const std::vector<tapacket::bytestring>& subpaks) override;

private:
    std::map<unsigned, unsigned> m_counts;
};
