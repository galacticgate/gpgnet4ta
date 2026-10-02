#include "GhostWatcher.h"

#include "jdplay/JDPlay.h"
#include "taflib/HexDump.h"

#include <QtCore/qdebug.h>
#include <QtCore/qfile.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qcoreevent.h>
#include <QtCore/qelapsedtimer.h>

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
    const unsigned AI_SUMMARY_TICKS = 50;               // log what the AI rule sees every 5 s
    const qint64 AI_STALE_MS = 6000;                    // an AI is gone once its status (sent every ~2 s) stops this long
    const qint64 AI_MISSING_GRACE_MS = 0;               // object at once: removing an AI deletes its player
    const unsigned UNIT_SYNC_QUIET_TICKS = 30;          // sync counts as done after 3 s of silence
    const unsigned NO_UNIT_SYNC_TICKS = 150;            // or after 15 s if the host never starts one
    const unsigned LOADING_STEP_TICKS = 5;
    const unsigned UNIT_LIST_QUIET_TICKS = 5;           // round one counts as finished after 500 ms of silence
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

GhostWatcher::GhostWatcher(QString dplayGuid, QString playerName, QString hostAddress, QString unitCrcFile, int expectedSettings) :
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
    m_lateUnits(0u),
    m_reannounceUnits(false),
    m_loadingPercent(0u),
    m_sentStart(false),
    m_hostTick(0u),
    m_expectedSettings(expectedSettings),
    m_sawAi(false),
    m_aiMissingSinceMs(-1),
    m_haveSlotList(false),
    m_spawnOff(false)
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
    m_clock.start();
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
            else if (m_reannounceUnits && m_ticksSinceUnitMessage >= UNIT_LIST_QUIET_TICKS)
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
            if (m_ticksInState % AI_SUMMARY_TICKS == 0u)
            {
                logAiSummary();
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
        if (m_aiPlayers.erase(dpId) > 0u)
        {
            qInfo() << "[GhostWatcher::onSystemMessage] an AI player left";
        }
        if (dpId == m_hostDpId)
        {
            qInfo() << "[GhostWatcher::onSystemMessage] the host left; stopping";
            stopTimer();
        }
        break;
    }
    case DPSYS_SETSESSIONDESC:
        // The host changed a game setting. A real joiner answers by announcing its units again;
        // without it TA's host stayed on "Synching" after a settings change (game 2265).
        qInfo() << "[GhostWatcher::onSystemMessage] session settings changed; re-announcing our units";
        if (m_state == State::LOBBY && m_sentUnitList)
        {
            sendUnitList();
            sendStatus();
        }
        break;
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
        if (info.isAI() && fromId != m_dpId)
        {
            if (m_aiPlayers.insert(fromId).second)
            {
                qInfo() << "[GhostWatcher::onSubpacket] AI player" << fromId << "present";
                m_sawAi = true;
            }
            m_aiLastSeenMs[fromId] = m_clock.elapsed();
        }
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

    case tapacket::SubPacketCode::CHAT_05:
    {
        // TDraw's "+spawnoff" acts on the TA it is typed into, and its reply ("Unit spawn is
        // disabled ...") only shows on that screen; the command itself is broadcast, so follow
        // the host's. TA writes the line as "<name> text"; matched in any letter case (game 2266).
        if (fromId != m_hostDpId)
        {
            break;
        }
        QString text = QString::fromLatin1((const char*)s.data() + 1, int(std::min<std::size_t>(s.size() - 1u, 64u))).section(QChar(0), 0, 0);
        QString command = text.section("> ", 1).trimmed().toLower();
        if (command.startsWith("+spawnoff"))
        {
            qInfo() << "[GhostWatcher::onSubpacket] host switched unit spawn off";
            m_spawnOff = true;
        }
        else if (command.startsWith("+spawnon"))
        {
            qInfo() << "[GhostWatcher::onSubpacket] host switched unit spawn on";
            m_spawnOff = false;
        }
        break;
    }

    case tapacket::SubPacketCode::IDENT2_26:
    {
        // The host's slot list, repeated every couple of seconds. Removing an AI takes it off
        // this list but does not delete its DirectPlay player (game 2266), so this is how an AI
        // leaving shows.
        if (fromId != m_hostDpId)
        {
            break;
        }
        m_slotPlayers.clear();
        for (std::size_t at = 1u; at + 4u <= s.size(); at += 4u)
        {
            std::uint32_t dpid = std::uint32_t(s[at] | s[at + 1] << 8 | s[at + 2] << 16 | s[at + 3] << 24);
            if (dpid != 0u)
            {
                m_slotPlayers.insert(dpid);
            }
        }
        m_haveSlotList = true;
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
        m_unitStatusCounts.clear();
        m_lateUnits = 0u;
        m_reannounceUnits = false;
        m_hostUnitIds.clear();
        m_sentUnitList = false;
        break;

    case 0x03:
        ++m_unitMessagesReceived;
        ++m_unitStatusCounts[unit.u.statusAndLimit[0]];
        if (m_sentUnitList)
        {
            // Round one can pause mid-list (games 2263 and 2271 announced 307 of 319). A unit first
            // seen after our announcement means our count was short, and TA's host then waits on
            // "Synching" for good (game 2271), so announce the whole list again once the host goes
            // quiet. The demo's unit table needs every unit too: the client matches the unit
            // catalog by that table's length.
            if (std::find(m_hostUnitIds.begin(), m_hostUnitIds.end(), unit.id) == m_hostUnitIds.end())
            {
                m_hostUnitIds.push_back(unit.id);
                ++m_lateUnits;
                m_reannounceUnits = true;
            }
            break;
        }
        if (unit.u.statusAndLimit[0] == 0x0101 && !m_hostUnitIds.empty())
        {
            // Round two has begun; announce our list now if the quiet timer hasn't yet.
            sendUnitList();
        }
        else if (std::find(m_hostUnitIds.begin(), m_hostUnitIds.end(), unit.id) == m_hostUnitIds.end())
        {
            // Every unit in round one, in use (low byte 01) or not (00): a real joiner announces
            // its whole list (319 for RES), and the demo's unit table is matched by its length.
            m_hostUnitIds.push_back(unit.id);
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

void GhostWatcher::sendUnitChecksum(std::uint32_t id)
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
        qInfo() << "[GhostWatcher::sendUnitChecksum] unit not in table:" << QString::number(id, 16);
    }
    send(0u, unit.asSubPacket());
}

void GhostWatcher::sendUnitList()
{
    // Our units are the host's units: announce the count, then each with its checksum.
    m_unitCrcMisses = 0u;
    m_reannounceUnits = false;
    tapacket::TUnitData count(0u, 0u, false);
    count.sub = 0x01;
    count.u.crc = 0u;
    count.u.statusAndLimit[0] = std::uint16_t(m_hostUnitIds.size());
    send(0u, count.asSubPacket());

    for (std::uint32_t id : m_hostUnitIds)
    {
        sendUnitChecksum(id);
    }
    m_sentUnitList = true;
    QStringList statuses;
    for (const auto& kv : m_unitStatusCounts)
    {
        statuses << QString("%1:%2").arg(kv.first, 4, 16, QChar('0')).arg(kv.second);
    }
    qInfo() << "[GhostWatcher::sendUnitList] announced" << m_hostUnitIds.size() << "units," << m_unitCrcMisses << "not in table; sub 3 statuses so far" << statuses.join(' ');
}

void GhostWatcher::updateClickedIn()
{
    // G4: once unit sync is done, the ghost is ready exactly while the mission is intact, and
    // says in the battleroom chat what to fix when it isn't. TA's host can only press Start
    // while every player is clicked in, so nothing wrong can start.
    if (m_hostStatus.empty())
    {
        return;
    }
    bool syncDone = m_sentUnitList && m_ticksSinceUnitMessage > UNIT_SYNC_QUIET_TICKS;
    bool noSync = m_unitMessagesReceived == 0u && m_ticksInState > NO_UNIT_SYNC_TICKS;
    if (!syncDone && !noSync)
    {
        return;
    }

    QString violation = missionViolation();
    if (violation != m_lastViolation)
    {
        if (!violation.isEmpty())
        {
            say(violation);
        }
        else if (!m_lastViolation.isEmpty())
        {
            say("Mission OK. Ready.");
        }
        m_lastViolation = violation;
    }

    bool ready = violation.isEmpty();
    if (ready != m_clickedIn)
    {
        qInfo() << "[GhostWatcher::updateClickedIn]" << (ready ? "clicking in" : "clicking out:") << violation
                << "; units" << m_hostUnitIds.size() << "of which late" << m_lateUnits
                << "; unit messages received" << m_unitMessagesReceived << "table" << m_unitCrcs.size() << "units, not in table" << m_unitCrcMisses;
        m_clickedIn = ready;
        sendStatus();
    }
}

QString GhostWatcher::missionViolation()
{
    tapacket::TPlayerInfo host(m_hostStatus);
    if (host.isCheatsEnabled())
    {
        return "Cheats must be off for this mission.";
    }
    // 0x40 is Location: Fixed (game 2280; Random reads 0x0e). Missions start at fixed positions
    // and lockOptions holds it there, so only the commander, mapping and LOS bits are compared.
    const std::uint8_t LOCATION_FIXED_BIT = 0x40;
    if (m_expectedSettings >= 0 &&
        (host.getPermLosByte() & ~LOCATION_FIXED_BIT) != (std::uint8_t(m_expectedSettings) & ~LOCATION_FIXED_BIT))
    {
        qInfo() << "[GhostWatcher::missionViolation] settings byte" << QString::number(host.getPermLosByte(), 16)
                << "want" << QString::number(m_expectedSettings, 16);
        return "Mission needs: Game ends, Mapped, LOS True.";
    }
    if (m_spawnOff)
    {
        return "Unit spawn is off. Type +spawnon.";
    }
    // The AI counts as present while its DirectPlay player exists and its status keeps coming.
    if (m_sawAi && !aiPresent())
    {
        if (m_aiMissingSinceMs < 0)
        {
            m_aiMissingSinceMs = m_clock.elapsed();
        }
        if (m_clock.elapsed() - m_aiMissingSinceMs >= AI_MISSING_GRACE_MS)
        {
            return "This mission needs its AI. Add it back.";
        }
    }
    else
    {
        m_aiMissingSinceMs = -1;
    }
    return QString();
}

bool GhostWatcher::aiPresent()
{
    for (std::uint32_t ai : m_aiPlayers)
    {
        auto seen = m_aiLastSeenMs.find(ai);
        // Removing an AI deletes its DirectPlay player (it leaves m_aiPlayers at once; game 2268);
        // a status that stops arriving is the backup. The slot list is not used: it names a newly
        // added AI about 2 s after the AI's own first status, which made adding look like removal.
        bool recent = seen != m_aiLastSeenMs.end() && m_clock.elapsed() - seen->second < AI_STALE_MS;
        if (recent)
        {
            return true;
        }
    }
    return false;
}

void GhostWatcher::logAiSummary()
{
    QStringList slotIds;
    for (std::uint32_t dpid : m_slotPlayers)
    {
        slotIds << QString::number(dpid);
    }
    QStringList ais;
    for (std::uint32_t ai : m_aiPlayers)
    {
        auto seen = m_aiLastSeenMs.find(ai);
        qint64 age = seen == m_aiLastSeenMs.end() ? -1 : m_clock.elapsed() - seen->second;
        ais << QString("%1 status %2 ms ago %3").arg(ai).arg(age).arg(m_slotPlayers.count(ai) ? "in slot list" : "not in slot list");
    }
    qInfo() << "[GhostWatcher::logAiSummary] slot list" << slotIds.join(' ') << "| AI:" << (ais.isEmpty() ? QString("none") : ais.join("; "))
            << "| present" << aiPresent();
}

void GhostWatcher::say(const QString& text)
{
    // TA shows at most 63 characters of a chat line, our name included, so wrap on spaces.
    qInfo() << "[GhostWatcher::say]" << text;
    const QString prefix = "<" + m_playerName + "> ";
    const int width = 63 - prefix.size();
    QString rest = text;
    while (!rest.isEmpty())
    {
        int cut = rest.size() <= width ? rest.size() : rest.lastIndexOf(' ', width);
        if (cut <= 0)
        {
            cut = std::min(width, int(rest.size()));
        }
        std::string line = (prefix + rest.left(cut)).toStdString();
        sendUdp(0u, tapacket::TPacket::createChatSubpacket(line));
        rest = rest.mid(cut).trimmed();
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
