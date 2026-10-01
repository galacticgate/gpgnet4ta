
if (ENABLE_IRC)
  set(IRC_SOURCES IrcForward.h;IrcForward.cpp)
else()
  set(IRC_SOURCES IrcForwardMock.h;IrcForwardMock.cpp)
endif()

# GG: the ghost watcher joins TA through DirectPlay, which only the Windows build has.
if (WIN32)
  set(GHOST_SOURCES GhostWatcher.h;GhostWatcher.cpp)
  set(GHOST_LIBS jdplay;${DIRECT_PLAY_LIBS})
endif()

set(CMAKE_AUTOMOC ON)
add_executable(gpgnet4ta
    GameEventHandlerQt.h
    GameEventHandlerQt.cpp
    GameMonitor2.h
    GameMonitor2.cpp
    gpgnet4ta.cpp
    GpgNetGameLauncher.h
    GpgNetGameLauncher.cpp
    TaLobby.h
    TaLobby.cpp
    ${IRC_SOURCES}
    ${GHOST_SOURCES}
    )

target_include_directories(gpgnet4ta 
    PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libs
    ${LIBCOMMUNI_INCLUDE_DIR}
    ${DIRECT_PLAY_INCLUDE_DIR}
    )

target_link_directories(gpgnet4ta
    PUBLIC
    ${LIBCOMMUNI_LIB_DIR}
    ${DIRECT_PLAY_LIB_DIR}
    )

target_link_libraries(gpgnet4ta
    gpgnet
    taflib
    tafnet
    talaunch
    tapacket
    tareplay
    Qt5::Core
    Qt5::Network
    ${LIBCOMMUNI_LIBS_STATIC}
    ${GHOST_LIBS}
    )

install(TARGETS gpgnet4ta)
