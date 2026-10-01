#pragma once

#include "tapacket/TPacket.h"

#include <QtCore/qobject.h>
#include <QtCore/qstring.h>

#include <cstdint>
#include <map>
#include <memory>

namespace jdplay {
    class JDPlay;
}

// GG: a receive-only watcher that joins the local TA game the way a remote watcher would.
//
// gpgnet4ta only sees (and so only records) what TA sends to remote players. A player alone
// with an AI has no remote players, so the game is never recorded. With this watcher in the
// game, TA sends it everything, and the normal recording path picks it up.
//
// It talks to TA through real DirectPlay (jdplay, as the replayer does), joining through a
// second TaLobby that gpgnet4ta wires to its own. It runs on its own thread: DirectPlay's
// session search blocks, and the search passes through this process's own tafnet sockets.
class GhostWatcher : public QObject
{
    Q_OBJECT

public:
    // unitCrcFile: the unit sync table, one "CRC_FBI,CRC_all" hex pair per line.
    GhostWatcher(QString dplayGuid, QString playerName, QString hostAddress, QString unitCrcFile);
    ~GhostWatcher();

public slots:
    // Starts polling. Call it from the thread the object was moved to.
    void start();

private:
    enum class State { SEARCHING, LOBBY, LOADING, PLAYING };

    void timerEvent(QTimerEvent* event) override;
    void setState(State state);
    void stopTimer();
    void loadUnitCrcs(const QString& path);
    bool tryJoin();
    void receiveAll();
    void onSystemMessage(const std::uint8_t* payload, std::uint32_t size);
    void onTaMessage(std::uint32_t fromId, const std::uint8_t* payload, std::uint32_t size);
    void onSubpacket(std::uint32_t fromId, const tapacket::bytestring& s);
    void onUnitData(std::uint32_t fromId, const tapacket::bytestring& s);
    void updateClickedIn();
    void sendStatus();
    void sendUnitCount();
    void sendLoadingProgress();
    void sendKeepAlive();
    void send(std::uint32_t toId, const tapacket::bytestring& subpak);
    void sendUdp(std::uint32_t toId, const tapacket::bytestring& subpak);
    void logSubpacket(const char* direction, std::uint32_t peerId, const tapacket::bytestring& s);

    const QString m_dplayGuid;
    const QString m_playerName;
    const QString m_hostAddress;
    std::unique_ptr<jdplay::JDPlay> m_jdPlay;

    State m_state;
    int m_timerId;
    unsigned m_ticksInState;            // 100 ms timer ticks since the last state change

    std::uint32_t m_dpId;
    std::uint32_t m_hostDpId;
    std::uint32_t m_tcpSeq;
    tapacket::bytestring m_hostStatus;  // the host's last PLAYER_INFO_20, the template for ours
    bool m_clickedIn;

    unsigned m_unitMessagesReceived;
    unsigned m_ticksSinceUnitMessage;
    bool m_unitCountDirty;
    std::map<std::uint32_t, std::uint32_t> m_unitCrcs;    // CRC_FBI (the unit's id on the wire) -> CRC_all
    unsigned m_unitCrcMisses;

    unsigned m_loadingPercent;
    bool m_sentStart;
    std::uint32_t m_hostTick;

    std::map<unsigned, unsigned> m_loggedCounts;
};
