// tbmatch_proto.h - wire format between the game and tbmatch, the matchmaking + relay server.
// Shared by server/tbmatch.cpp and runtime/netplay.cpp. All integers little-endian, packed.
//
// Flow: the game sends QUEUE every second while searching (quick match, create room, join room);
// the server answers STATUS, and MATCH once two compatible players are paired. Both games then try
// to reach each other directly (PUNCH packets, game to game); if that fails within a few seconds
// they RELAY_BIND to the server with the session's token and wrap their packets in RELAY_DATA,
// which the server forwards only between the two bound endpoints of that session.
#pragma once
#include <cstdint>

namespace tbm {

const uint32_t MAGIC = 0x534D4254; // "TBMS"
const uint8_t VERSION = 1;
const int DEFAULT_PORT = 7846;
const int MAX_PACKET = 1400;

enum Type : uint8_t {
    QUEUE = 1,     // game -> server
    CANCEL,        // game -> server
    STATUS,        // server -> game
    MATCH,         // server -> game
    MATCH_ACK,     // game -> server
    PUNCH,         // game -> game (direct path probe)
    RELAY_BIND,    // game -> server
    RELAY_OK,      // server -> game
    RELAY_DATA,    // game <-> server, payload = one game packet
    PING,          // anyone -> server (health / round trip)
    PONG,          // server -> anyone
};

enum Mode : uint8_t { MODE_QUICK = 1, MODE_CREATE_ROOM, MODE_JOIN_ROOM };
enum State : uint8_t { ST_SEARCHING = 1, ST_ROOM_OPEN, ST_NO_SUCH_ROOM, ST_INCOMPATIBLE, ST_BUSY, ST_BAD_VERSION };

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
};
struct Addr4 {
    uint32_t ip;   // network byte order
    uint16_t port; // network byte order
};
struct Queue {
    Header h;
    uint64_t ticket;   // random per search; identifies this player until matched
    uint64_t build;    // game build fingerprint (must match)
    uint64_t disc;     // disc fingerprint (must match)
    Addr4 lan;         // the game's LAN address, for players behind the same router
    uint8_t mode;
    char room[6];      // MODE_JOIN_ROOM: code typed by the player (5 chars + 0)
};
struct Cancel { Header h; uint64_t ticket; };
struct Status {
    Header h;
    uint64_t ticket;
    uint8_t state;
    char room[6];      // ST_ROOM_OPEN: the code to share
    Addr4 seen;        // the game's public address as the server sees it
    uint16_t waiting;  // players searching right now (quick match)
};
struct Match {
    Header h;
    uint64_t ticket;
    uint8_t role;          // 0 = host (player 1), 1 = client (player 2)
    Addr4 peer_public, peer_lan;
    uint8_t session[16];
    uint8_t token[16];     // proves this side owns the session when binding the relay
};
struct MatchAck { Header h; uint64_t ticket; };
struct Punch { Header h; uint8_t session[16]; uint8_t role; };
struct RelayBind { Header h; uint8_t session[16]; uint8_t token[16]; uint8_t role; };
struct RelayOk { Header h; uint8_t session[16]; };
struct RelayData { Header h; uint8_t session[16]; uint8_t role; /* payload follows */ };
struct Ping { Header h; uint32_t t; };
#pragma pack(pop)

} // namespace tbm
