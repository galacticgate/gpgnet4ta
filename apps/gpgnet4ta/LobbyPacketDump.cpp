#include "LobbyPacketDump.h"

#include "taflib/HexDump.h"

#include <QtCore/qdebug.h>
#include <QtCore/qstring.h>

#include <algorithm>
#include <sstream>

namespace
{
    const unsigned CAP_PER_KIND = 20u;
    const unsigned CAP_UNIT_DATA = 15u;
    const unsigned LOG_EVERY_NTH_AFTER = 100u;
}

void LobbyPacketDump::onDplaySuperEnumPlayerReply(std::uint32_t dplayId, const std::string& playerName, tapacket::DPAddress*, tapacket::DPAddress*)
{
    qInfo() << "[LobbyPacketDump] super enum player" << dplayId << playerName.c_str();
}

void LobbyPacketDump::onDplayCreateOrForwardPlayer(std::uint16_t command, std::uint32_t dplayId, const std::string& name, tapacket::DPAddress*, tapacket::DPAddress*)
{
    qInfo() << "[LobbyPacketDump] create or forward player, command" << QString::number(command, 16) << "dpid" << dplayId << name.c_str();
}

void LobbyPacketDump::onDplayDeletePlayer(std::uint32_t dplayId)
{
    qInfo() << "[LobbyPacketDump] delete player" << dplayId;
}

void LobbyPacketDump::onTaPacket(std::uint32_t sourceDplayId, std::uint32_t otherDplayId, bool isLocalSource, const char*, int,
    const std::vector<tapacket::bytestring>& subpaks)
{
    for (const tapacket::bytestring& s : subpaks)
    {
        if (s.empty())
        {
            continue;
        }
        unsigned code = s[0];
        unsigned sub = (code == 0x1a && s.size() > 1u) ? s[1] : 0u;
        unsigned key = (isLocalSource ? 0x10000u : 0u) | (code << 8) | sub;
        unsigned n = ++m_counts[key];

        // Unit sync's request, counts and recorder markers are the point, so they are never
        // thinned out; everything else logs its first few, then every hundredth.
        bool always = code == 0x1a && (sub == 0x00 || sub == 0x04 || sub == 0x09);
        unsigned cap = code == 0x1a ? CAP_UNIT_DATA : CAP_PER_KIND;
        if (!always && n > cap && n % LOG_EVERY_NTH_AFTER != 0u)
        {
            continue;
        }

        std::ostringstream ss;
        taflib::HexDump(s.data(), std::min<std::size_t>(s.size(), 64u), ss);
        qInfo() << "[LobbyPacketDump]" << (isLocalSource ? "local" : "remote") << "from" << sourceDplayId << "to" << otherDplayId
                << "code" << QString::number(code, 16) << "sub" << sub << "size" << s.size() << "#" << n << ss.str().c_str();
    }
}
