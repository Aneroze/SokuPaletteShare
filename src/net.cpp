#include "net.hpp"
#include "game.hpp"
#include "guard.hpp"
#include "original.hpp"
#include "patch.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "protocol.hpp"
#include <windows.h>
#include <SokuLib.hpp>
#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <thread>

// Transport: the game's UDP socket. Received datagrams are filtered in a hook on the game's
// recvfrom import (0x857280), so our packets never reach the game or the mods that look at
// packets after it returns (giuroll hooks 0x41DAEA). We send through the game's sendto import
// (0x857290) so mods that relay or rewrite traffic (Autopunch, InGameHostlist) also see ours.
// Hooks on these imports chain: we keep whatever was there before us.
//
// One kind of palette packet (PALETTE: side, seq, character, slot, hash, bytes) goes both ways:
// a player sends its own side's to the opponent, and any node a spectator watches through sends it
// both sides'. Each is resent until acknowledged (Send), and received palettes are reassembled,
// checked and kept by value per side (Received), whoever sent them.

namespace ps::net {
	namespace {
		constexpr DWORD IAT_RECVFROM = 0x857280;
		constexpr DWORD IAT_SENDTO = 0x857290;
		constexpr ULONGLONG HELLO_INTERVAL_MS = 500;
		constexpr int HELLO_MAX_TRIES = 40;       // ~20 s, then assume the peer has no mod
		constexpr ULONGLONG RESEND_INTERVAL_MS = 500;
		constexpr int OFFER_MAX_SENDS = 60;       // to the opponent, until acknowledged (~30 s)
		constexpr int SIDE_MAX_SENDS = 20;        // to a spectator, until acknowledged
		constexpr size_t MAX_PENDING = 4;
		// New custom palettes accepted per character select (per match when spectating); a palette
		// equal to the slot's original doesn't count. Keeps a peer from filling our memory.
		constexpr int MAX_NEW_PALETTES = 256;
		constexpr int MAX_IGNORED_LOGS = 3;  // "ignored a HELLO" lines per session
		constexpr uint8_t GAME_INIT_REQUEST = 0x05;  // the game's join packet: type, gameId[16],
		constexpr size_t INIT_REQUEST_TYPE_AT = 25;   // unknown[8], u8 request type
		constexpr uint8_t PLAY_REQUEST = 1;
		constexpr uint8_t SPECTATE_REQUEST = 0;
		constexpr size_t MAX_SPECTATORS = 16;        // modded spectators we serve at once
		constexpr ULONGLONG SPECTATOR_HELLO_GAP_MS = 500;  // at most one HELLO reply per address this often
		constexpr int JOIN_MAX_SENDS = 20;            // spectating: JOIN until a palette arrives
		constexpr ULONGLONG SPECTATE_REQUEST_TTL_MS = 30 * 60 * 1000;
		constexpr ULONGLONG NO_MOD_AFTER_MS = 3000;  // no HELLO by then: show "they don't have it"

		using RecvFrom = int (__stdcall *)(SOCKET, char *, int, int, sockaddr *, int *);
		using SendTo = int (__stdcall *)(SOCKET, const char *, int, int, const sockaddr *, int);

		RecvFrom orig_recvfrom = nullptr;

		// A palette we send, resent until the receiver acknowledges this very one (seq and hash).
		struct Send {
			uint32_t seq = 0;
			int charId = -1;
			int slot = -1;
			PaletteHash hash;
			std::vector<std::vector<uint8_t>> packets;
			bool acked = true;
			int sends = 0;
			ULONGLONG nextSend = 0;
		};

		Send makeSend(int side, uint32_t seq, int charId, int slot, const PaletteHash &hash, const std::vector<uint8_t> &bytes)
		{
			return Send{seq, charId, slot, hash,
				proto::makePalette(static_cast<uint8_t>(side), seq, static_cast<uint16_t>(charId), static_cast<uint8_t>(slot), hash, bytes), false};
		}

		bool acknowledge(Send &send, const proto::Ack &ack)
		{
			if (send.acked || send.seq != ack.seq || !(send.hash == ack.hash))
				return false;
			send.acked = true;
			return true;
		}

		// A palette received for a side, in the cache-free form we use it in.
		struct Received {
			uint32_t seq = 0;
			int slot = -1;
			PaletteHash hash;
			Palette palette;
		};

		struct Pending {
			uint32_t seq;
			int charId;
			int slot;
			std::vector<uint8_t> data;
			std::vector<bool> have;
			size_t missing;
			ULONGLONG started;
		};

		std::mutex g_mutex;
		bool g_inSession = false;
		bool g_peerKnown = false;     // the opponent said HELLO with a compatible version
		bool g_peerIncompatible = false;
		sockaddr_in g_peer{};
		int g_helloTries = 0;
		ULONGLONG g_sessionStart = 0;
		ULONGLONG g_nextHello = 0;
		std::optional<Send> g_offer;  // our palette, to the opponent
		uint32_t g_nextSeq = 1;
		uint8_t g_version = proto::VERSION;  // agreed with the opponent

		// What we received, per side, by the sender's number, so a late or reordered datagram never
		// replaces a newer palette: the latest per character (what that side shows right now, for
		// previews) and per character and slot (what it loads with that slot).
		std::map<int, Received> g_latest[2];
		std::map<std::pair<int, int>, Received> g_bySlot[2];
		uint32_t g_latestSeq[2] = {0, 0};
		// Palettes we received lately (this character select or match), so one offered again
		// (e.g. for another slot) is only acknowledged, not sent again.
		std::map<std::array<uint8_t, 16>, Palette> g_known;
		std::map<std::pair<int, std::array<uint8_t, 16>>, Pending> g_pending;  // by side and hash
		int g_newPalettes = 0;

		// Host: who asked the game to play us lately (the game's own INIT_REQUEST packets). A player
		// HELLO must come from one of them, so nobody else can take over the exchange; spectators
		// ask to spectate instead and aren't counted.
		struct PlayRequest {
			sockaddr_in from;
			ULONGLONG at;
		};
		std::vector<PlayRequest> g_playRequests;

		// Spectators. Any node a spectator's game watches through (the host, or a spectator
		// relaying to others) serves it what each side is shown with: a player node its own palette
		// for its side and the raw palette the opponent sent for theirs, a spectator what it got from
		// upstream. Only addresses that asked the game to spectate are answered.
		struct Shown {
			uint32_t seq = 0;  // ours: numbers what we serve; a spectator acknowledges it
			int charId = -1;
			int slot = -1;
			PaletteHash hash;
			std::vector<uint8_t> bytes;  // serialized palette
		};
		Shown g_shown[2];
		uint32_t g_shownSeq = 0;
		// A spectator gets palettes only once it echoed the cookie of our HELLO reply (JOIN): then we
		// know our packets reach it at that address. Until then it costs one small reply per HELLO.
		struct Spectator {
			sockaddr_in addr;
			ULONGLONG since;
			uint32_t cookie = 0;
			ULONGLONG lastReply = 0;
			bool joined = false;
			Send sides[2];
		};
		std::vector<Spectator> g_spectators;
		std::vector<PlayRequest> g_spectateRequests;  // the game's join requests to spectate
		// Player node: the characters loaded on each side, and our own palette for our side.
		struct Loaded {
			int charId = -1;
			int slot = -1;
			std::optional<Palette> own;
			PaletteHash ownHash;
		};
		Loaded g_loaded[2];
		// Spectating: the node our game watches through.
		bool g_watching = false;
		bool g_upstreamKnown = false;
		sockaddr_in g_upstream{};
		int g_upstreamTries = 0;
		ULONGLONG g_nextUpstreamHello = 0;
		uint32_t g_joinCookie = 0;  // from upstream's HELLO reply
		int g_joinSends = 0;
		ULONGLONG g_nextJoin = 0;
		bool g_gotPalette = false;

		std::atomic<unsigned> g_generation = 0;
		std::atomic<bool> g_stop = false;
		std::thread *g_thread = nullptr;  // never destroyed: a running std::thread's destructor aborts

		bool sameAddr(const sockaddr_in &a, const sockaddr_in &b)
		{
			return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
		}

		std::string addressText(const sockaddr_in &a)
		{
			return std::string(inet_ntoa(a.sin_addr)) + ":" + std::to_string(ntohs(a.sin_port));
		}

		bool inNetplayScene()
		{
			switch (SokuLib::sceneId) {
			case SokuLib::SCENE_SELECTSV: case SokuLib::SCENE_SELECTCL:
			case SokuLib::SCENE_LOADINGSV: case SokuLib::SCENE_LOADINGCL:
			case SokuLib::SCENE_BATTLESV: case SokuLib::SCENE_BATTLECL:
				return true;
			default:
				return false;
			}
		}

		// Watching a netplay match (the game shows spectators only the loading screen and battles).
		bool spectating()
		{
			return SokuLib::mainMode == SokuLib::BATTLE_MODE_VSWATCH &&
			       (SokuLib::sceneId == SokuLib::SCENE_LOADINGWATCH || SokuLib::sceneId == SokuLib::SCENE_BATTLEWATCH);
		}

		SOCKET gameSocket()
		{
			auto obj = *reinterpret_cast<SokuLib::NetObject **>(SokuLib::ADDR_PNETOBJECT);
			return obj ? obj->netUdp.socket : INVALID_SOCKET;
		}

		// The game's peer: for a client the host, for a spectator the node it watches through (the
		// host learns its client's address from its HELLO).
		std::optional<sockaddr_in> hostAddress()
		{
			auto obj = *reinterpret_cast<SokuLib::NetObject **>(SokuLib::ADDR_PNETOBJECT);
			if (!obj || obj->netUdp.opponent.sin_port == 0)
				return std::nullopt;
			return obj->netUdp.opponent;
		}

		// Packets queued under g_mutex and sent after it's released: other mods hook sendto too (and
		// may lock around it).
		struct Outgoing {
			std::vector<uint8_t> packet;
			sockaddr_in to;
		};
		std::vector<Outgoing> g_outbox;
		int g_ignoredLogs = 0;
		int g_lastScene = -1;

		// Caller holds g_mutex.
		void sendTo(const std::vector<uint8_t> &packet, const sockaddr_in &to)
		{
			g_outbox.push_back({packet, to});
		}

		// Caller holds g_mutex.
		void pump(Send &send, const sockaddr_in &to, int maxSends, ULONGLONG now)
		{
			if (send.acked || send.sends >= maxSends || now < send.nextSend)
				return;
			for (auto &packet : send.packets)
				sendTo(packet, to);
			send.sends++;
			send.nextSend = now + RESEND_INTERVAL_MS;
		}

		// Caller doesn't hold g_mutex.
		void flushOutbox()
		{
			std::vector<Outgoing> outbox;
			{
				std::lock_guard lock(g_mutex);
				outbox.swap(g_outbox);
			}
			SOCKET s = gameSocket();
			if (s == INVALID_SOCKET)
				return;
			auto send = *reinterpret_cast<SendTo *>(IAT_SENDTO);
			for (auto &out : outbox)
				send(s, reinterpret_cast<const char *>(out.packet.data()), static_cast<int>(out.packet.size()), 0,
				     reinterpret_cast<const sockaddr *>(&out.to), sizeof(out.to));
		}

		// A new character select (or, spectating, a new match): the new-palette count starts over.
		void newRound()
		{
			g_newPalettes = 0;
			g_known.clear();
		}

		void resetSession()
		{
			g_peerKnown = false;
			g_peerIncompatible = false;
			g_peer = {};
			g_helloTries = 0;
			g_nextHello = 0;
			g_offer.reset();
			g_pending.clear();
			for (int side = 0; side < 2; side++) {
				g_latest[side].clear();
				g_bySlot[side].clear();
				g_latestSeq[side] = 0;
				g_shown[side] = Shown{};
				g_loaded[side] = Loaded{};
			}
			g_spectators.clear();
			g_upstreamKnown = false;
			g_upstream = {};
			g_upstreamTries = 0;
			g_nextUpstreamHello = 0;
			g_joinCookie = 0;
			g_joinSends = 0;
			g_nextJoin = 0;
			g_gotPalette = false;
			newRound();
			g_ignoredLogs = 0;
			g_version = proto::VERSION;
		}

		Spectator *spectatorAt(const sockaddr_in &from)
		{
			for (auto &spectator : g_spectators)
				if (sameAddr(spectator.addr, from))
					return &spectator;
			return nullptr;
		}

		void queuePush(Spectator &spectator, int side)
		{
			auto &shown = g_shown[side];
			if (shown.charId < 0 || spectator.sides[side].seq == shown.seq)
				return;
			spectator.sides[side] = makeSend(side, shown.seq, shown.charId, shown.slot, shown.hash, shown.bytes);
		}

		// What a side is shown with changed: spectators get it.
		void setShown(int side, int charId, int slot, const PaletteHash &hash, std::vector<uint8_t> bytes)
		{
			auto &shown = g_shown[side];
			if (shown.charId == charId && shown.slot == slot && shown.hash == hash)
				return;
			shown = Shown{++g_shownSeq, charId, slot, hash, std::move(bytes)};
			for (auto &spectator : g_spectators)
				if (spectator.joined)
					queuePush(spectator, side);
			if (!g_spectators.empty())
				log("net: spectators get side %d: char=%d slot=%d %s", side, charId, slot, hash.hex().c_str());
		}

		// Player node: what a loaded side is shown with, as far as we know: our own palette, or the
		// one the opponent offered for that character and slot (none if they didn't: spectators see
		// the slot, as we do).
		void refreshShown(int side)
		{
			auto &loaded = g_loaded[side];
			if (loaded.charId < 0)
				return;
			if (side == localSide()) {
				if (loaded.own)
					setShown(side, loaded.charId, loaded.slot, loaded.ownHash, loaded.own->serialize());
				return;
			}
			auto it = g_bySlot[side].find({loaded.charId, loaded.slot});
			if (it != g_bySlot[side].end())
				setShown(side, loaded.charId, loaded.slot, it->second.hash, it->second.palette.serialize());
		}

		bool askedToSpectate(const sockaddr_in &from)
		{
			auto now = GetTickCount64();
			for (auto &request : g_spectateRequests)
				if (sameAddr(from, request.from) && now - request.at < SPECTATE_REQUEST_TTL_MS)
					return true;
			return false;
		}

		uint32_t newCookie()
		{
			static std::random_device random;  // rand_s: unpredictable, so a forged address can't guess it
			uint32_t cookie;
			do
				cookie = random();
			while (!cookie);
			return cookie;
		}

		void onSpectatorHello(const proto::Hello &hello, const sockaddr_in &from)
		{
			if (!askedToSpectate(from)) {
				if (g_ignoredLogs++ < MAX_IGNORED_LOGS)
					log("net: ignored a spectator HELLO from %s, which never asked to spectate", addressText(from).c_str());
				return;
			}
			uint8_t common = (std::min)(proto::VERSION, hello.maxVersion);
			if (common < (std::max)(proto::MIN_VERSION, hello.minVersion))
				return;  // can't talk to it: it sees the slots
			auto now = GetTickCount64();
			auto spectator = spectatorAt(from);
			if (!spectator) {
				if (g_spectators.size() >= MAX_SPECTATORS) {
					// Make room by dropping the oldest one that never joined; joined ones stay.
					auto drop = g_spectators.end();
					for (auto it = g_spectators.begin(); it != g_spectators.end(); ++it)
						if (!it->joined && (drop == g_spectators.end() || it->since < drop->since))
							drop = it;
					if (drop == g_spectators.end())
						return;  // all taken by spectators that joined
					g_spectators.erase(drop);
				}
				g_spectators.push_back(Spectator{from, now});
				spectator = &g_spectators.back();
				log("net: spectator %s has PaletteShare %s", addressText(from).c_str(), hello.modVersion.c_str());
			}
			if (now - spectator->lastReply < SPECTATOR_HELLO_GAP_MS)
				return;
			spectator->lastReply = now;
			// A HELLO after it joined means it starts over (e.g. it watches again): a new cookie, and
			// palettes again after its JOIN. Before that, the same cookie (a HELLO may be resent).
			if (spectator->joined || !spectator->cookie) {
				spectator->cookie = newCookie();
				spectator->joined = false;
				for (auto &send : spectator->sides)
					send = Send{};
			}
			sendTo(proto::makeHello(g_watching ? proto::ROLE_SPECTATOR : proto::ROLE_PLAYER, true, spectator->cookie), from);
		}

		void onJoin(uint32_t cookie, const sockaddr_in &from)
		{
			auto spectator = spectatorAt(from);
			if (!spectator || spectator->joined || spectator->cookie != cookie)
				return;
			spectator->joined = true;
			log("net: spectator %s joined", addressText(from).c_str());
			for (int side = 0; side < 2; side++)
				queuePush(*spectator, side);
		}

		// Whether a player HELLO comes from the game's own opponent.
		bool fromOpponent(const sockaddr_in &from)
		{
			if (localSide() == 1) {  // we joined: the host is the one we connected to
				auto host = hostAddress();
				return host && sameAddr(from, *host);
			}
			// The requests seen before this match (see notePlayRequest): the opponent is one of them.
			for (auto &request : g_playRequests)
				if (sameAddr(from, request.from))
					return true;
			if (g_playRequests.empty()) {
				// We never saw a play request (another mod may hide the game's packets from us):
				// rather than never sharing palettes, trust the first player that says hello.
				log("net: no play request ever seen, accepting the HELLO unverified");
				return true;
			}
			return false;
		}

		void onHello(const proto::Hello &hello, const sockaddr_in &from)
		{
			if (hello.role == proto::ROLE_SPECTATOR && !hello.isReply) {
				onSpectatorHello(hello, from);
				return;
			}
			if (g_watching) {
				// The node we watch through answered: prove we're here with its cookie.
				auto upstream = hostAddress();
				if (!hello.isReply || !hello.cookie || !upstream || !sameAddr(from, *upstream))
					return;
				if (!g_upstreamKnown)
					log("net: watching through %s, which has PaletteShare %s", addressText(from).c_str(), hello.modVersion.c_str());
				g_upstreamKnown = true;
				g_upstream = from;
				if (hello.cookie != g_joinCookie) {  // the latest one counts
					g_joinCookie = hello.cookie;
					g_gotPalette = false;
					g_joinSends = 0;
					g_nextJoin = 0;
				}
				return;
			}
			if (hello.role != proto::ROLE_PLAYER)
				return;
			if (g_peerKnown && !sameAddr(from, g_peer))
				return;
			if (!g_peerKnown && !fromOpponent(from)) {
				if (g_ignoredLogs++ < MAX_IGNORED_LOGS) {
					std::string expected;
					if (auto host = localSide() == 1 ? hostAddress() : std::nullopt)
						expected = addressText(*host);
					else
						for (auto &request : g_playRequests)
							expected += (expected.empty() ? "" : ", ") + addressText(request.from);
					log("net: ignored a HELLO from %s, not our opponent (expected %s)", addressText(from).c_str(), expected.empty() ? "?" : expected.c_str());
				}
				return;
			}
			// The highest version both sides speak; checked on every HELLO, so nothing sticks.
			uint8_t common = (std::min)(proto::VERSION, hello.maxVersion);
			if (common < (std::max)(proto::MIN_VERSION, hello.minVersion)) {
				if (!g_peerIncompatible)
					log("net: opponent's PaletteShare %s speaks protocol v%d-v%d, we speak v%d-v%d; not exchanging palettes",
						hello.modVersion.c_str(), hello.minVersion, hello.maxVersion, proto::MIN_VERSION, proto::VERSION);
				g_peerIncompatible = true;
				return;
			}
			g_peerIncompatible = false;
			g_version = common;
			if (!g_peerKnown)
				log("net: opponent has PaletteShare %s (%s), protocol v%d", hello.modVersion.c_str(), hello.isReply ? "reply" : "hello", g_version);
			g_peerKnown = true;
			g_peer = from;
			if (!hello.isReply)
				sendTo(proto::makeHello(proto::ROLE_PLAYER, true), from);
			if (g_offer)
				g_offer->nextSend = 0;  // send our palette right away
		}

		// A side's palette, complete and checked. Older ones than what we have are ignored.
		void remember(int side, uint32_t seq, int charId, int slot, const PaletteHash &hash, const Palette &palette, bool fresh)
		{
			auto &bySlot = g_bySlot[side][{charId, slot}];
			if (seq > bySlot.seq) {
				bySlot = {seq, slot, hash, palette};
				if (g_inSession && g_loaded[side].charId == charId && g_loaded[side].slot == slot)
					refreshShown(side);  // the opponent's, arriving after their character loaded
			}
			if (g_watching && seq > g_latestSeq[side]) {
				g_latestSeq[side] = seq;
				setShown(side, charId, slot, hash, palette.serialize());  // for spectators watching through us
			}
			auto &latest = g_latest[side][charId];
			if (seq <= latest.seq) {
				if (seq < latest.seq)
					log("net: older palette #%u for side %d char=%d slot=%d arrived after #%u, not shown", seq, side, charId, slot, latest.seq);
				return;
			}
			bool changed = latest.slot != slot || !(latest.hash == hash);
			latest = {seq, slot, hash, palette};
			if (changed || fresh) {
				g_generation++;
				log("net: side %d palette #%u char=%d slot=%d %s%s", side, seq, charId, slot, hash.hex().c_str(), fresh ? "" : " (known)");
			}
		}

		// Who may send us a side's palette: the opponent its own side's, the node we watch through
		// either side's.
		bool mayReceive(const sockaddr_in &from, int side)
		{
			if (g_inSession)
				return g_peerKnown && sameAddr(from, g_peer) && side == 1 - localSide();
			auto upstream = hostAddress();
			return g_watching && upstream && sameAddr(from, *upstream);
		}

		void onPalette(const proto::PaletteChunk &chunk, const sockaddr_in &from)
		{
			if (!mayReceive(from, chunk.side))
				return;
			if (g_watching)
				g_gotPalette = true;  // it got our JOIN

			int side = chunk.side;
			auto key = std::make_pair(side, chunk.hash.bytes);
			auto it = g_pending.find(key);
			if (it == g_pending.end()) {
				// Already have it: just acknowledge, so the sender stops resending.
				auto known = g_known.find(chunk.hash.bytes);
				if (known != g_known.end()) {
					remember(side, chunk.seq, chunk.charId, chunk.slot, chunk.hash, known->second, false);
					sendTo(proto::makeAck(chunk.hash, chunk.seq), from);
					return;
				}
				if (g_pending.size() >= MAX_PENDING) {
					auto oldest = g_pending.begin();
					for (auto p = g_pending.begin(); p != g_pending.end(); ++p)
						if (p->second.started < oldest->second.started)
							oldest = p;
					g_pending.erase(oldest);
				}
				Pending p{chunk.seq, chunk.charId, chunk.slot, std::vector<uint8_t>(chunk.totalSize), std::vector<bool>(chunk.totalSize), chunk.totalSize, GetTickCount64()};
				it = g_pending.emplace(key, std::move(p)).first;
			}

			auto &p = it->second;
			if (p.data.size() != chunk.totalSize)
				return;
			if (chunk.seq > p.seq) {  // the same palette offered again (e.g. for another slot): keep the newest
				p.seq = chunk.seq;
				p.charId = chunk.charId;
				p.slot = chunk.slot;
			}
			for (size_t i = 0; i < chunk.data.size(); i++) {
				size_t at = chunk.offset + i;
				if (!p.have[at]) {
					p.have[at] = true;
					p.missing--;
				}
				p.data[at] = chunk.data[i];
			}
			if (p.missing)
				return;

			auto palette = Palette::deserialize(p.data.data(), p.data.size());
			uint32_t seq = p.seq;
			int charId = p.charId, slot = p.slot;
			g_pending.erase(it);
			if (!palette || !(hashPalette(*palette) == chunk.hash)) {
				log("net: dropped a side %d palette that doesn't match its hash", side);
				return;
			}
			// A slot's original palette can't be abusive, and browsing offers plenty of them.
			auto original = game::normalCharacter(charId) ? original::palette(game::characterName(charId), slot) : std::nullopt;
			if (!(original && sameColors(*original, *palette)) && ++g_newPalettes > MAX_NEW_PALETTES) {
				if (g_newPalettes == MAX_NEW_PALETTES + 1)
					log("net: more than %d new palettes from %s, ignoring the rest for now", MAX_NEW_PALETTES, addressText(from).c_str());
				return;
			}
			g_known[chunk.hash.bytes] = *palette;
			remember(side, seq, charId, slot, chunk.hash, *palette, true);
			sendTo(proto::makeAck(chunk.hash, seq), from);
		}

		void onAck(const proto::Ack &ack, const sockaddr_in &from)
		{
			if (auto spectator = spectatorAt(from)) {
				for (int side = 0; side < 2; side++)
					if (acknowledge(spectator->sides[side], ack))
						log("net: spectator %s has side %d's palette", addressText(from).c_str(), side);
				return;
			}
			// Only the offer it answers: one for an earlier offer with the same palette (the wheel
			// stepping towards its slot) mustn't stop the current one from being sent.
			if (g_peerKnown && sameAddr(from, g_peer) && g_offer && acknowledge(*g_offer, ack))
				log("net: opponent has our offer #%u", ack.seq);
		}

		void handle(const uint8_t *data, size_t size, const sockaddr_in &from)
		{
			std::lock_guard lock(g_mutex);
			if (!g_inSession && !g_watching)
				return;
			if (auto hello = proto::parseHello(data, size)) {
				onHello(*hello, from);
				return;
			}
			auto version = proto::versionOf(data, size);
			if (!version || *version < proto::MIN_VERSION || *version > proto::VERSION)
				return;
			if (g_peerKnown && sameAddr(from, g_peer) && *version != g_version)
				return;  // the opponent talks the version we agreed on
			if (auto chunk = proto::parsePalette(data, size))
				onPalette(*chunk, from);
			else if (auto cookie = proto::parseJoin(data, size))
				onJoin(*cookie, from);
			else if (auto ack = proto::parseAck(data, size))
				onAck(*ack, from);
		}

		void noteRequest(std::vector<PlayRequest> &requests, size_t max, const sockaddr_in &from)
		{
			auto now = GetTickCount64();
			for (auto &request : requests)
				if (sameAddr(request.from, from)) {
					request.at = now;
					return;
				}
			if (requests.size() >= max)
				requests.erase(requests.begin());
			requests.push_back({from, now});
		}

		// The game's join requests: to play us (only those before a match count: during one the game
		// turns new requests away, so a stranger can't become "the opponent" by sending one), or to
		// spectate (at any time; we may also be a spectator relaying the match).
		void noteJoinRequest(uint8_t type, const sockaddr_in &from)
		{
			std::lock_guard lock(g_mutex);
			if (type == PLAY_REQUEST && !g_inSession)
				noteRequest(g_playRequests, 8, from);
			else if (type == SPECTATE_REQUEST)
				noteRequest(g_spectateRequests, 32, from);
		}

		int __stdcall onRecvFrom(SOCKET s, char *buf, int len, int flags, sockaddr *from, int *fromlen)
		{
			for (;;) {
				int r = orig_recvfrom(s, buf, len, flags, from, fromlen);
				if (r <= 0 || (flags & MSG_PEEK))
					return r;
				bool fromV4 = from && fromlen && *fromlen >= static_cast<int>(sizeof(sockaddr_in)) && from->sa_family == AF_INET;
				if (!proto::isOurs(reinterpret_cast<uint8_t *>(buf), r)) {
					if (fromV4 && r > static_cast<int>(INIT_REQUEST_TYPE_AT) && static_cast<uint8_t>(buf[0]) == GAME_INIT_REQUEST)
						guarded("network", [&] { noteJoinRequest(static_cast<uint8_t>(buf[INIT_REQUEST_TYPE_AT]), *reinterpret_cast<sockaddr_in *>(from)); });
					return r;
				}
				if (fromV4) {
					guarded("network", [&] { handle(reinterpret_cast<uint8_t *>(buf), r, *reinterpret_cast<sockaddr_in *>(from)); });
					guarded("network send", flushOutbox);
				}
				// Ours: hide it from the game and wait for the next datagram.
			}
		}

		void tick()
		{
			std::lock_guard lock(g_mutex);
			bool session = localSide() >= 0 && inNetplayScene();
			bool watching = spectating();
			if (watching != g_watching) {
				resetSession();
				g_watching = watching;
				log("net: %s spectating", watching ? "started" : "stopped");
			}
			if (session != g_inSession) {
				// Keep an offer made just before the session was noticed (the first character-select
				// preview can come before this thread's next tick); drop everything when leaving.
				auto offer = std::move(g_offer);
				resetSession();
				if (session && offer) {
					offer->acked = false;
					offer->sends = 0;
					offer->nextSend = 0;
					g_offer = std::move(offer);
				}
				g_inSession = session;
				g_sessionStart = GetTickCount64();
				log("net: %s netplay session", session ? "entered" : "left");
			}
			int scene = SokuLib::sceneId;
			bool sceneChanged = scene != g_lastScene;
			g_lastScene = scene;
			if (sceneChanged && g_inSession && (scene == SokuLib::SCENE_SELECTSV || scene == SokuLib::SCENE_SELECTCL)) {
				// Each character select starts fresh: a lost handshake gets another chance (per
				// rematch), and the new-palette count starts over. The loaded characters are history:
				// what the opponent browses now mustn't reach spectators still watching the last match.
				newRound();
				for (auto &loaded : g_loaded)
					loaded = Loaded{};
				if (!g_peerKnown && (g_helloTries > 0 || g_peerIncompatible)) {
					g_helloTries = 0;
					g_peerIncompatible = false;
					log("net: new character select, trying the handshake again");
				}
			}
			if (sceneChanged && g_watching && scene == SokuLib::SCENE_LOADINGWATCH)
				newRound();  // spectating: a new match

			auto now = GetTickCount64();
			for (auto &spectator : g_spectators)
				if (spectator.joined)
					for (auto &send : spectator.sides)
						pump(send, spectator.addr, SIDE_MAX_SENDS, now);
			if (g_watching) {
				// Ask the node our game watches through (the host, or a spectator relaying it).
				if (!g_upstreamKnown && g_upstreamTries < HELLO_MAX_TRIES && now >= g_nextUpstreamHello) {
					if (auto upstream = hostAddress()) {
						if (g_upstreamTries == 0)
							log("net: spectating through %s, asking for the players' palettes", addressText(*upstream).c_str());
						sendTo(proto::makeHello(proto::ROLE_SPECTATOR, false), *upstream);
						g_upstreamTries++;
						g_nextUpstreamHello = now + HELLO_INTERVAL_MS;
						if (g_upstreamTries == HELLO_MAX_TRIES)
							log("net: no answer; the game we watch has no PaletteShare (or an incompatible one)");
					}
				}
				// JOIN until a palette arrives (it may have none yet: then our JOIN is all it needs).
				if (g_upstreamKnown && !g_gotPalette && g_joinSends < JOIN_MAX_SENDS && now >= g_nextJoin) {
					sendTo(proto::makeJoin(g_joinCookie), g_upstream);
					g_joinSends++;
					g_nextJoin = now + HELLO_INTERVAL_MS;
				}
				return;
			}
			if (!g_inSession || g_peerIncompatible)
				return;

			if (!g_peerKnown) {
				// The client opens the conversation; the host answers it.
				if (localSide() == 1 && g_helloTries < HELLO_MAX_TRIES && now >= g_nextHello) {
					if (auto host = hostAddress()) {
						sendTo(proto::makeHello(proto::ROLE_PLAYER, false), *host);
						g_helloTries++;
						g_nextHello = now + HELLO_INTERVAL_MS;
						if (g_helloTries == HELLO_MAX_TRIES)
							log("net: no answer from the opponent; assuming they don't have PaletteShare");
					}
				}
				return;
			}
			if (g_offer)
				pump(*g_offer, g_peer, OFFER_MAX_SENDS, now);
		}

		std::optional<Palette> find(const std::map<std::pair<int, int>, Received> &map, int charId, int slot)
		{
			auto it = map.find({charId, slot});
			if (it == map.end())
				return std::nullopt;
			return it->second.palette;
		}
	}

	int localSide()
	{
		switch (SokuLib::mainMode) {
		case SokuLib::BATTLE_MODE_VSCLIENT: return 0;  // hosting: we play against a client
		case SokuLib::BATTLE_MODE_VSSERVER: return 1;  // joined: we play against the server
		default: return -1;
		}
	}

	void offerLocal(int charId, int slot, const Palette &palette, const PaletteHash &hash, bool again)
	{
		std::lock_guard lock(g_mutex);
		int side = localSide();
		if (side < 0)
			return;
		if (g_offer && g_offer->charId == charId && g_offer->slot == slot && g_offer->hash == hash) {
			if (again && g_offer->acked) {  // the character loads with it: make sure the opponent has it
				g_offer->acked = false;
				g_offer->sends = 0;
				g_offer->nextSend = 0;
			}
			return;
		}
		uint32_t seq = g_nextSeq++;
		g_offer = makeSend(side, seq, charId, slot, hash, palette.serialize());
		log("net: offering #%u char=%d slot=%d %s", seq, charId, slot, hash.hex().c_str());
	}

	std::optional<Palette> received(int side, int charId, int slot)
	{
		std::lock_guard lock(g_mutex);
		if (side < 0 || side > 1 || (!g_inSession && !g_watching))
			return std::nullopt;
		return find(g_bySlot[side], charId, slot);
	}

	std::optional<Palette> receivedNow(int side, int charId)
	{
		std::lock_guard lock(g_mutex);
		if (side < 0 || side > 1 || (!g_inSession && !g_watching))
			return std::nullopt;
		auto it = g_latest[side].find(charId);
		if (it == g_latest[side].end())
			return std::nullopt;
		return it->second.palette;
	}

	PeerStatus peerStatus()
	{
		std::lock_guard lock(g_mutex);
		if (!g_inSession)
			return PeerStatus::NotInNetplay;
		if (g_peerIncompatible)
			return PeerStatus::Incompatible;
		if (g_peerKnown)
			return PeerStatus::HasMod;
		return GetTickCount64() - g_sessionStart < NO_MOD_AFTER_MS ? PeerStatus::Checking : PeerStatus::NoMod;
	}

	void sideLoaded(int side, int charId, int slot, const Palette *own)
	{
		std::lock_guard lock(g_mutex);
		if (!g_inSession || side < 0 || side > 1)
			return;
		auto &loaded = g_loaded[side];
		loaded = Loaded{charId, slot};
		if (own) {
			loaded.own = *own;
			loaded.ownHash = hashPalette(*own);
		}
		refreshShown(side);
	}

	unsigned remoteGeneration()
	{
		return g_generation;
	}

	void shutdown()
	{
		if (!g_thread || !g_thread->joinable())
			return;
		g_stop = true;
		// Bounded: at process exit the thread may already be gone or unable to finish.
		WaitForSingleObject(g_thread->native_handle(), 500);
		g_thread->detach();
	}

	void install()
	{
		orig_recvfrom = patch::pointer(IAT_RECVFROM, &onRecvFrom, "network");
		if (!orig_recvfrom)
			return;

		g_thread = new std::thread([] {
			original::palette("reimu", 0);  // index the game's archives now, not on the receive thread
			while (!g_stop) {
				guarded("network tick", tick);
				guarded("network send", flushOutbox);
				Sleep(50);
			}
		});
		log("net: recvfrom hooked (previous %p)", orig_recvfrom);
	}
}
