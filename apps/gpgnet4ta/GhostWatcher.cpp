#include "GhostWatcher.h"

#include "jdplay/JDPlay.h"
#include "taflib/HexDump.h"

#include <QtCore/qdebug.h>
#include <QtCore/qfile.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qcoreevent.h>

#include <dplay.h>
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace
{
    const int TIMER_MS = 100;
    const unsigned SEARCH_INTERVAL_TICKS = 20;          // retry the session search every 2 s
    const unsigned SEARCH_GIVE_UP_TICKS = 6000;         // stop looking after 10 min
    const unsigned STATUS_INTERVAL_TICKS = 10;          // repeat our status every 1 s
    const unsigned UNIT_SYNC_QUIET_TICKS = 30;          // sync counts as done after 3 s of silence
    const unsigned NO_UNIT_SYNC_TICKS = 150;            // or after 15 s if the host never starts one
    const unsigned LOADING_STEP_TICKS = 5;
    const unsigned UNIT_LIST_QUIET_TICKS = 2;           // round one counts as finished after 200 ms of silence
    const unsigned LOG_FIRST_N_PER_CODE = 12;
    const unsigned LOG_EVERY_NTH_AFTER = 500;

    const std::uint8_t CLICKED_WATCHER = 0x40;
    const std::uint8_t CLICKED_IN = 0x20;

    const char* stateName(int state)
    {
        static const char* names[] = { "SEARCHING", "LOBBY", "LOADING", "PLAYING" };
        return state >= 0 && state < 4 ? names[state] : "?";
    }
}

GhostWatcher::GhostWatcher(QString dplayGuid, QString playerName, QString hostAddress, QString unitCrcFile) :
    m_dplayGuid(dplayGuid),
    m_playerName(playerName),
    m_hostAddress(hostAddress),
    m_state(State::SEARCHING),
    m_timerId(0),
    m_ticksInState(0u),
    m_dpId(0u),
    m_hostDpId(0u),
    m_tcpSeq(0xfffffffe),
    m_clickedIn(false),
    m_unitMessagesReceived(0u),
    m_ticksSinceUnitMessage(0u),
    m_unitCountDirty(false),
    m_unitCrcMisses(0u),
    m_sentUnitList(false),
    m_loadingPercent(0u),
    m_sentStart(false),
    m_hostTick(0u)
{
    loadUnitCrcs(unitCrcFile);
}

void GhostWatcher::loadUnitCrcs(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qWarning() << "[GhostWatcher::loadUnitCrcs] no unit sync table at" << path << "; unit sync will not complete";
        return;
    }
    QTextStream in(&file);
    while (!in.atEnd())
    {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
        {
            continue;
        }
        QStringList parts = line.split(',');
        bool okId = false;
        bool okCrc = false;
        std::uint32_t id = parts.value(0).toUInt(&okId, 16);
        std::uint32_t crc = parts.value(1).toUInt(&okCrc, 16);
        if (okId && okCrc)
        {
            m_unitCrcs[id] = crc;
        }
    }
    qInfo() << "[GhostWatcher::loadUnitCrcs]" << m_unitCrcs.size() << "units from" << path;
}

GhostWatcher::~GhostWatcher()
{
    if (m_jdPlay && m_dpId)
    {
        m_jdPlay->dpDestroyPlayer(m_dpId);
    }
}

void GhostWatcher::start()
{
    qInfo() << "[GhostWatcher::start] name" << m_playerName << "joining" << m_hostAddress << "guid" << m_dplayGuid;
    m_timerId = startTimer(TIMER_MS, Qt::TimerType::PreciseTimer);
}

void GhostWatcher::stopTimer()
{
    if (m_timerId != 0)
    {
        killTimer(m_timerId);
        m_timerId = 0;
    }
}

void GhostWatcher::setState(State state)
{
    qInfo() << "[GhostWatcher::setState]" << stateName(int(m_state)) << "->" << stateName(int(state)) << "after" << m_ticksInState << "ticks";
    m_state = state;
    m_ticksInState = 0u;
}

void GhostWatcher::timerEvent(QTimerEvent*)
{
    try
    {
        ++m_ticksInState;
        switch (m_state)
        {
        case State::SEARCHING:
            if (m_ticksInState % SEARCH_INTERVAL_TICKS == 1u && tryJoin())
            {
                setState(State::LOBBY);
            }
            else if (m_ticksInState > SEARCH_GIVE_UP_TICKS)
            {
                qWarning() << "[GhostWatcher::timerEvent] no TA session found; giving up";
                stopTimer();
            }
            break;

        case State::LOBBY:
            receiveAll();
            ++m_ticksSinceUnitMessage;
            if (!m_sentUnitList && !m_hostUnitIds.empty() && m_ticksSinceUnitMessage >= UNIT_LIST_QUIET_TICKS)
            {
                sendUnitList();
            }
            updateClickedIn();
            if (m_unitMessagesReceived > 0u)
            {
                // A real joiner repeats its count constantly (hundreds a second); once a tick will do.
                sendUnitCount();
            }
            if (m_ticksInState % STATUS_INTERVAL_TICKS == 0u)
            {
                sendStatus();
            }
            break;

        case State::LOADING:
            receiveAll();
            if (m_ticksInState % LOADING_STEP_TICKS == 0u)
            {
                sendLoadingProgress();
            }
            break;

        case State::PLAYING:
            receiveAll();
            sendKeepAlive();
            break;
        }
    }
    catch (const std::exception& e)
    {
        qWarning() << "[GhostWatcher::timerEvent] exception:" << e.what();
    }
    catch (...)
    {
        qWarning() << "[GhostWatcher::timerEvent] unknown exception";
    }
}

bool GhostWatcher::tryJoin()
{
    if (!m_jdPlay)
    {
        // searchValidationCount 0: join on the first sighting. 3 made jdplay wait for the same
        // session description four times running, about a minute here.
        m_jdPlay.reset(new jdplay::JDPlay(m_playerName.toStdString().c_str(), 0, NULL));
        if (!m_jdPlay->initialize(m_dplayGuid.toStdString().c_str(), m_hostAddress.toStdString().c_str(), false, 10))
        {
            qWarning() << "[GhostWatcher::tryJoin] jdplay failed to initialise";
            m_jdPlay.reset();
            return false;
        }
    }

    if (!m_jdPlay->searchOnce())
    {
        qInfo() << "[GhostWatcher::tryJoin] no session at" << m_hostAddress << "yet";
        return false;
    }

    // A joining TA creates its DirectPlay player with player data attached, and the host
    // rejected us within a second when ours had none. Mirror the host's, as with the status.
    std::string playerData;
    for (std::uint32_t id : m_jdPlay->dpPlayerIds())
    {
        std::string data = m_jdPlay->dpGetPlayerData(id);
        std::ostringstream ss;
        taflib::HexDump(data.data(), std::min<std::size_t>(data.size(), 128u), ss);
        qInfo() << "[GhostWatcher::tryJoin] existing player" << id << "player data" << data.size() << "bytes" << ss.str().c_str();
        if (playerData.empty() && !data.empty())
        {
            playerData = data;
        }
    }

    m_dpId = playerData.empty()
        ? m_jdPlay->dpCreatePlayer(m_playerName.toStdString().c_str())
        : m_jdPlay->dpCreatePlayer(m_playerName.toStdString().c_str(), playerData.data(), std::uint32_t(playerData.size()));
    qInfo() << "[GhostWatcher::tryJoin] joined; our dpid" << m_dpId << "with" << playerData.size() << "bytes of player data";
    return m_dpId != 0u;
}

void GhostWatcher::receiveAll()
{
    std::uint8_t buffer[10000];
    for (;;)
    {
        std::uint32_t size = sizeof(buffer);
        std::uint32_t fromId = 0u;
        std::uint32_t toId = 0u;
        if (!m_jdPlay->dpReceive(buffer, size, fromId, toId))
        {
            break;
        }
        if (fromId == DPID_SYSMSG)
        {
            onSystemMessage(buffer, size);
        }
        else
        {
            onTaMessage(fromId, buffer, size);
        }
    }
}

void GhostWatcher::onSystemMessage(const std::uint8_t* payload, std::uint32_t size)
{
    if (size < sizeof(DPMSG_GENERIC))
    {
        return;
    }
    const DPMSG_GENERIC* msg = (const DPMSG_GENERIC*)payload;
    switch (msg->dwType)
    {
    case DPSYS_CREATEPLAYERORGROUP:
        qInfo() << "[GhostWatcher::onSystemMessage] player joined" << ((const DPMSG_CREATEPLAYERORGROUP*)payload)->dpId;
        break;
    case DPSYS_DESTROYPLAYERORGROUP:
    {
        std::uint32_t dpId = ((const DPMSG_DESTROYPLAYERORGROUP*)payload)->dpId;
        qInfo() << "[GhostWatcher::onSystemMessage] player left" << dpId;
        if (dpId == m_hostDpId)
        {
            qInfo() << "[GhostWatcher::onSystemMessage] the host left; stopping";
            stopTimer();
        }
        break;
    }
    case DPSYS_SESSIONLOST:
        qInfo() << "[GhostWatcher::onSystemMessage] session lost; stopping";
        stopTimer();
        break;
    default:
        qInfo() << "[GhostWatcher::onSystemMessage] type" << QString::number(msg->dwType, 16);
        break;
    }
}

void GhostWatcher::onTaMessage(std::uint32_t fromId, const std::uint8_t* _payload, std::uint32_t size)
{
    tapacket::bytestring payload(_payload, size);
    std::uint16_t checksum[2];
    tapacket::TPacket::decrypt(payload, 0u, checksum[0], checksum[1]);
    if (checksum[0] != checksum[1])
    {
        return;
    }
    if (tapacket::PacketCode(payload[0]) == tapacket::PacketCode::COMPRESSED)
    {
        payload = tapacket::TPacket::decompress(payload, 3);
    }

    for (const tapacket::bytestring& s : tapacket::TPacket::unsmartpak(payload, true, true))
    {
        unsigned expectedSize = tapacket::TPacket::getExpectedSubPacketSize(s);
        if (expectedSize == 0u || s.size() != expectedSize)
        {
            logSubpacket("recv?", fromId, s);
            continue;
        }
        onSubpacket(fromId, s);
    }
}

void GhostWatcher::onSubpacket(std::uint32_t fromId, const tapacket::bytestring& s)
{
    logSubpacket("recv", fromId, s);
    switch (tapacket::SubPacketCode(s[0]))
    {
    case tapacket::SubPacketCode::PING_02:
    {
        // Answer as the replayer answers a joining TA's pings.
        tapacket::TPing ping(s);
        ping.value = 1000000u + std::uint32_t(rand()) % 1000000u;
        send(fromId, ping.asSubPacket());
        break;
    }

    case tapacket::SubPacketCode::PLAYER_INFO_20:
    {
        tapacket::TPlayerInfo info(s);
        if (!info.isAI() && !info.isWatcher() && fromId != m_dpId)
        {
            if (m_hostDpId == 0u)
            {
                qInfo() << "[GhostWatcher::onSubpacket] host is dpid" << fromId << "map" << info.getMapName().c_str();
                m_hostDpId = fromId;
            }
            if (fromId == m_hostDpId)
            {
                bool first = m_hostStatus.empty();
                m_hostStatus = s;
                if (first)
                {
                    // A joiner answers with its own status at once; don't wait for the timer.
                    sendStatus();
                }
            }
        }
        break;
    }

    case tapacket::SubPacketCode::UNIT_DATA_1A:
        onUnitData(fromId, s);
        break;

    case tapacket::SubPacketCode::REJECT_1B:
    {
        std::uint32_t rejected = std::uint32_t(s[1] | s[2] << 8 | s[3] << 16 | s[4] << 24);
        qWarning() << "[GhostWatcher::onSubpacket] dpid" << fromId << "rejected" << rejected
                   << (rejected == m_dpId ? "(us)" : "") << "reason" << QString::number(s[5], 16);
        break;
    }

    case tapacket::SubPacketCode::LOADING_STARTED_08:
        if (m_state == State::LOBBY)
        {
            setState(State::LOADING);
        }
        break;

    case tapacket::SubPacketCode::START_1E:
    case tapacket::SubPacketCode::START_15:
    case tapacket::SubPacketCode::UNIT_STAT_AND_MOVE_2C:
        if (tapacket::SubPacketCode(s[0]) == tapacket::SubPacketCode::UNIT_STAT_AND_MOVE_2C && s.size() >= 7u && fromId == m_hostDpId)
        {
            m_hostTick = std::max(m_hostTick, std::uint32_t(s[3] | s[4] << 8 | s[5] << 16 | s[6] << 24));
        }
        if (m_state == State::LOADING && m_sentStart)
        {
            setState(State::PLAYING);
        }
        break;

    default:
        break;
    }
}

void GhostWatcher::onUnitData(std::uint32_t fromId, const tapacket::bytestring& s)
{
    // Unit sync, as captured from a real joiner (Oscar joining a game, 2026-10-01):
    //   host:   sub 0, a request; then sub 3 per unit, status 0x0001 (round one, the host's list);
    //           later sub 3 per unit again, status 0x0101 (round two).
    //   joiner: sub 1 announcing how many units it has (count at [10]), then sub 2 per unit with
    //           its own (CRC_FBI, CRC_all), and all along sub 4, its count of the host's messages
    //           (1 for the request plus each sub 3; 639 for 319 units).
    // Without the sub 1 announcement the host waits forever on "Synching".
    if (fromId != m_hostDpId && m_hostDpId != 0u)
    {
        return;
    }
    tapacket::TUnitData unit(s);
    m_ticksSinceUnitMessage = 0u;
    switch (unit.sub)
    {
    case 0x00:
        m_unitMessagesReceived = 1u;
        m_unitCrcMisses = 0u;
        m_hostUnitIds.clear();
        m_sentUnitList = false;
        break;

    case 0x03:
        ++m_unitMessagesReceived;
        if (unit.u.statusAndLimit[0] == 0x0001)
        {
            m_hostUnitIds.push_back(unit.id);
        }
        else if (!m_sentUnitList && !m_hostUnitIds.empty())
        {
            // Round two has begun; announce our list now if the quiet timer hasn't yet.
            sendUnitList();
        }
        break;

    case 0x04:
        qInfo() << "[GhostWatcher::onUnitData] host has received" << unit.u.statusAndLimit[0] << "of our unit messages";
        break;

    default:
        ++m_unitMessagesReceived;
        break;
    }
}

void GhostWatcher::sendUnitList()
{
    // Our units are the host's units: announce the count, then each with its checksum.
    tapacket::TUnitData count(0u, 0u, false);
    count.sub = 0x01;
    count.u.crc = 0u;
    count.u.statusAndLimit[0] = std::uint16_t(m_hostUnitIds.size());
    send(0u, count.asSubPacket());

    for (std::uint32_t id : m_hostUnitIds)
    {
        tapacket::TUnitData unit(id, 0u, false);
        unit.sub = 0x02;
        unit.u.crc = 0u;
        auto it = m_unitCrcs.find(id);
        if (it != m_unitCrcs.end())
        {
            unit.u.crc = it->second;
        }
        else
        {
            ++m_unitCrcMisses;
            qInfo() << "[GhostWatcher::sendUnitList] unit not in table:" << QString::number(id, 16);
        }
        send(0u, unit.asSubPacket());
    }
    m_sentUnitList = true;
    qInfo() << "[GhostWatcher::sendUnitList] announced" << m_hostUnitIds.size() << "units," << m_unitCrcMisses << "not in table";
}

void GhostWatcher::updateClickedIn()
{
    // G1: click in once unit sync has gone quiet. G4 adds the mission checks here: the AI is
    // present, unit spawn is on, LOS and cheats match the preset.
    if (m_clickedIn || m_hostStatus.empty())
    {
        return;
    }
    bool syncDone = m_sentUnitList && m_ticksSinceUnitMessage > UNIT_SYNC_QUIET_TICKS;
    bool noSync = m_unitMessagesReceived == 0u && m_ticksInState > NO_UNIT_SYNC_TICKS;
    if (syncDone || noSync)
    {
        qInfo() << "[GhostWatcher::updateClickedIn] clicking in; unit messages received" << m_unitMessagesReceived
                << "table" << m_unitCrcs.size() << "units, not in table" << m_unitCrcMisses;
        m_clickedIn = true;
        sendStatus();
    }
}

void GhostWatcher::sendStatus()
{
    if (m_hostStatus.empty())
    {
        return;
    }
    // Our status is the host's own, re-addressed, marked as a watcher, and clicked in when ready.
    tapacket::TPlayerInfo info(m_hostStatus);
    info.setDpId(m_dpId);
    info.clicked = std::uint8_t((info.clicked & ~(CLICKED_WATCHER | CLICKED_IN)) | CLICKED_WATCHER | (m_clickedIn ? CLICKED_IN : 0));
    send(0u, info.asSubPacket());
}

void GhostWatcher::sendUnitCount()
{
    tapacket::TUnitData count(0u, 0u, false);
    count.sub = 0x04;
    count.u.crc = 0u;
    count.u.statusAndLimit[0] = std::uint16_t(m_unitMessagesReceived);
    send(0u, count.asSubPacket());
    m_unitCountDirty = false;
}

void GhostWatcher::sendLoadingProgress()
{
    if (m_loadingPercent < 100u)
    {
        m_loadingPercent = std::min(100u, m_loadingPercent + 20u);
        send(0u, tapacket::TProgress(std::uint8_t(m_loadingPercent)).asSubPacket());
    }
    else if (!m_sentStart)
    {
        static const std::uint8_t startMsg[] = { std::uint8_t(tapacket::SubPacketCode::START_15) };
        send(0u, tapacket::bytestring(startMsg, sizeof(startMsg)));
        m_sentStart = true;
        qInfo() << "[GhostWatcher::sendLoadingProgress] loaded";
    }
}

void GhostWatcher::sendKeepAlive()
{
    // An idle player's per-tick packet: no units, just the game clock. Keeping ours at the host's
    // tick means TA never slows the game down to wait for us.
    std::uint32_t tick = m_hostTick;
    std::uint8_t pkt[] = {
        std::uint8_t(tapacket::SubPacketCode::UNIT_STAT_AND_MOVE_2C), 0x0b, 0x00,
        std::uint8_t(tick), std::uint8_t(tick >> 8), std::uint8_t(tick >> 16), std::uint8_t(tick >> 24),
        0xff, 0xff, 0x01, 0x00 };
    sendUdp(0u, tapacket::bytestring(pkt, sizeof(pkt)));
}

void GhostWatcher::send(std::uint32_t toId, const tapacket::bytestring& subpak)
{
    logSubpacket("send", toId, subpak);
    tapacket::bytestring bs = tapacket::TPacket::trivialSmartpak(subpak, toId == 0u ? m_tcpSeq-- : 0xffffffff);
    tapacket::TPacket::encrypt(bs);
    m_jdPlay->dpSend(m_dpId, toId, DPSEND_GUARANTEED, (void*)bs.data(), DWORD(bs.size()));
}

void GhostWatcher::sendUdp(std::uint32_t toId, const tapacket::bytestring& subpak)
{
    logSubpacket("send", toId, subpak);
    tapacket::bytestring bs = tapacket::TPacket::trivialSmartpak(subpak, 0u);
    tapacket::TPacket::encrypt(bs);
    bs[0] = 0x03;
    m_jdPlay->dpSend(m_dpId, toId, 0, (void*)bs.data(), DWORD(bs.size()));
}

void GhostWatcher::logSubpacket(const char* direction, std::uint32_t peerId, const tapacket::bytestring& s)
{
    // The first test's job is to show us the conversation, so log the first few of each kind in
    // full, then thin out.
    if (s.empty())
    {
        return;
    }
    unsigned key = (direction[0] == 's' ? 0x100u : 0u) + (direction[4] == '?' ? 0x200u : 0u) + s[0];
    unsigned n = ++m_loggedCounts[key];
    if (n > LOG_FIRST_N_PER_CODE && n % LOG_EVERY_NTH_AFTER != 0u)
    {
        return;
    }
    std::ostringstream ss;
    taflib::HexDump(s.data(), std::min<std::size_t>(s.size(), 64u), ss);
    qInfo() << "[GhostWatcher]" << direction << "peer" << peerId << "code" << QString::number(s[0], 16)
            << "size" << s.size() << "#" << n << "\n" << ss.str().c_str();
}
