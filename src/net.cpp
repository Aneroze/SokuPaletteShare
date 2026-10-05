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
#include <thread>

// Transport: the game's UDP socket. Received datagrams are filtered in a hook on the game's
// recvfrom import (0x857280), so our packets never reach the game or the mods that look at
// packets after it returns (giuroll hooks 0x41DAEA). We send through the game's sendto import
// (0x857290) so mods that relay or rewrite traffic (Autopunch, InGameHostlist) also see ours.
// Hooks on these imports chain: we keep whatever was there before us.

namespace ps::net {
	namespace {
		constexpr DWORD IAT_RECVFROM = 0x857280;
		constexpr DWORD IAT_SENDTO = 0x857290;
		constexpr ULONGLONG HELLO_INTERVAL_MS = 500;
		constexpr int HELLO_MAX_TRIES = 40;       // ~20 s, then assume the peer has no mod
		constexpr ULONGLONG OFFER_INTERVAL_MS = 500;
		constexpr int OFFER_MAX_SENDS = 60;       // resend until acked, up to ~30 s
		constexpr size_t MAX_PENDING = 4;
		// New custom palettes from the opponent per character select (a palette equal to the slot's
		// original doesn't count): a peer can't fill our cache.
		constexpr int MAX_NEW_PALETTES = 256;
		constexpr int MAX_IGNORED_LOGS = 3;  // "ignored a HELLO" lines per session
		constexpr uint8_t GAME_INIT_REQUEST = 0x05;  // the game's join packet: type, gameId[16],
		constexpr size_t INIT_REQUEST_TYPE_AT = 25;   // unknown[8], u8 request type
		constexpr uint8_t PLAY_REQUEST = 1;
		constexpr uint8_t SPECTATE_REQUEST = 0;
		constexpr size_t MAX_SPECTATORS = 16;        // modded spectators we serve at once
		constexpr int SIDE_MAX_SENDS = 20;            // per palette and spectator, until acknowledged...
		constexpr int SIDE_MAX_SENDS_UNANSWERED = 4;  // ...or this many while it never acknowledged anything
		constexpr ULONGLONG SPECTATE_REQUEST_TTL_MS = 30 * 60 * 1000;
		constexpr ULONGLONG NO_MOD_AFTER_MS = 3000;  // no HELLO by then: show "they don't have it"

		using RecvFrom = int (__stdcall *)(SOCKET, char *, int, int, sockaddr *, int *);
		using SendTo = int (__stdcall *)(SOCKET, const char *, int, int, const sockaddr *, int);

		RecvFrom orig_recvfrom = nullptr;

		struct Offer {
			uint32_t seq;
			int charId;
			int slot;
			PaletteHash hash;
			std::vector<std::vector<uint8_t>> packets;
			bool acked = false;
			int sends = 0;
			ULONGLONG nextSend = 0;
		};

		struct Pending {
			int side;  // -1: from the opponent, else a side's palette from upstream (spectating)
			uint32_t seq;
			int charId;
			int slot;
			std::vector<uint8_t> data;
			std::vector<bool> have;
			size_t missing;
			ULONGLONG started;
		};

		struct RemoteEntry {
			uint32_t seq = 0;
			int slot = -1;
			PaletteHash hash;
		};

		std::mutex g_mutex;
		bool g_inSession = false;
		bool g_peerKnown = false;     // the opponent said HELLO with a compatible version
		bool g_peerIncompatible = false;
		sockaddr_in g_peer{};
		int g_helloTries = 0;
		ULONGLONG g_sessionStart = 0;
		ULONGLONG g_nextHello = 0;
		std::optional<Offer> g_offer;
		std::map<std::array<uint8_t, 16>, Pending> g_pending;
		// What the opponent offered, by offer number, so a late or reordered datagram never replaces a
		// newer offer: the latest per character (what they show right now, for previews) and the
		// latest per character and slot (what they'll load with that slot).
		std::map<int, RemoteEntry> g_remote;
		std::map<std::pair<int, int>, RemoteEntry> g_remoteBySlot;
		uint32_t g_nextSeq = 1;
		int g_newPalettes = 0;
		uint8_t g_version = proto::VERSION;  // agreed with the opponent

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
		// upstream. Only addresses that asked the game to spectate are served (no traffic for forged
		// HELLOs), and each palette is resent until acknowledged.
		struct Shown {
			uint32_t seq = 0;  // ours: numbers what we serve; a spectator acknowledges it
			int charId = -1;
			int slot = -1;
			PaletteHash hash;
			std::vector<uint8_t> bytes;  // serialized palette
		};
		Shown g_shown[2];
		uint32_t g_shownSeq = 0;
		struct Push {
			uint32_t seq = 0;
			PaletteHash hash;
			std::vector<std::vector<uint8_t>> packets;
			bool acked = true;
			int sends = 0;
			ULONGLONG nextSend = 0;
		};
		struct Spectator {
			sockaddr_in addr;
			ULONGLONG since;
			bool answered = false;  // acknowledged something: it's really there
			Push sides[2];
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
		// Spectating: the node our game watches through, and what it sent for each side.
		bool g_watching = false;
		bool g_upstreamKnown = false;
		sockaddr_in g_upstream{};
		int g_upstreamTries = 0;
		ULONGLONG g_nextUpstreamHello = 0;
		RemoteEntry g_watched[2];
		int g_watchedChar[2] = {-1, -1};

		std::atomic<unsigned> g_generation = 0;
		std::atomic<bool> g_stop = false;
		std::thread *g_thread = nullptr;  // never destroyed: a running std::thread's destructor aborts

		bool sameAddr(const sockaddr_in &a, const sockaddr_in &b)
		{
			return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
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

		// The client knows the host's address from the connection; the host learns the client's
		// from its HELLO.
		std::optional<sockaddr_in> hostAddress()
		{
			auto obj = *reinterpret_cast<SokuLib::NetObject **>(SokuLib::ADDR_PNETOBJECT);
			if (!obj || obj->netUdp.opponent.sin_port == 0)
				return std::nullopt;
			return obj->netUdp.opponent;
		}

		// Packets and disk writes queued under g_mutex and done after it's released: other mods hook
		// sendto too (and may lock around it), and a slow disk mustn't stall the game's receive
		// thread mid-match.
		struct Outgoing {
			std::vector<uint8_t> packet;
			sockaddr_in to;
		};
		std::vector<Outgoing> g_outbox;
		std::vector<std::pair<Palette, PaletteHash>> g_toStore;
		int g_ignoredLogs = 0;
		int g_lastScene = -1;

		// Caller holds g_mutex.
		void sendTo(const std::vector<uint8_t> &packet, const sockaddr_in &to)
		{
			g_outbox.push_back({packet, to});
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

		// Our thread, without g_mutex.
		void storeReceived()
		{
			std::vector<std::pair<Palette, PaletteHash>> toStore;
			{
				std::lock_guard lock(g_mutex);
				toStore.swap(g_toStore);
			}
			for (auto &[palette, hash] : toStore)
				cache.store(palette, hash);
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
			for (auto &shown : g_shown)
				shown = Shown{};
			for (auto &loaded : g_loaded)
				loaded = Loaded{};
			g_spectators.clear();
			g_upstreamKnown = false;
			g_upstream = {};
			g_upstreamTries = 0;
			g_nextUpstreamHello = 0;
			for (int side = 0; side < 2; side++) {
				g_watched[side] = RemoteEntry{};
				g_watchedChar[side] = -1;
			}
			g_remote.clear();
			g_remoteBySlot.clear();
			g_newPalettes = 0;
			g_ignoredLogs = 0;
			g_version = proto::VERSION;
		}

		std::string addressText(const sockaddr_in &a)
		{
			return std::string(inet_ntoa(a.sin_addr)) + ":" + std::to_string(ntohs(a.sin_port));
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
			auto &push = spectator.sides[side];
			if (shown.charId < 0 || push.seq == shown.seq)
				return;
			push = Push{shown.seq, shown.hash,
				proto::makeSide(static_cast<uint8_t>(side), shown.seq, static_cast<uint16_t>(shown.charId), static_cast<uint8_t>(shown.slot), shown.hash, shown.bytes),
				false, 0, 0};
		}

		// What a side is shown with changed: spectators get it.
		void setShown(int side, int charId, int slot, const PaletteHash &hash, std::vector<uint8_t> bytes)
		{
			auto &shown = g_shown[side];
			if (shown.charId == charId && shown.slot == slot && shown.hash == hash)
				return;
			shown = Shown{++g_shownSeq, charId, slot, hash, std::move(bytes)};
			for (auto &spectator : g_spectators)
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
			auto it = g_remoteBySlot.find({loaded.charId, loaded.slot});
			if (it == g_remoteBySlot.end())
				return;
			if (auto palette = cache.load(it->second.hash))
				setShown(side, loaded.charId, loaded.slot, it->second.hash, palette->serialize());
		}

		bool askedToSpectate(const sockaddr_in &from)
		{
			auto now = GetTickCount64();
			for (auto &request : g_spectateRequests)
				if (sameAddr(from, request.from) && now - request.at < SPECTATE_REQUEST_TTL_MS)
					return true;
			return false;
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
			auto spectator = spectatorAt(from);
			if (!spectator) {
				if (g_spectators.size() >= MAX_SPECTATORS) {
					auto oldest = g_spectators.begin();
					for (auto it = g_spectators.begin(); it != g_spectators.end(); ++it)
						if (it->since < oldest->since)
							oldest = it;
					g_spectators.erase(oldest);
				}
				g_spectators.push_back(Spectator{from, GetTickCount64()});
				spectator = &g_spectators.back();
				log("net: spectator %s has PaletteShare %s", addressText(from).c_str(), hello.modVersion.empty() ? "(old)" : hello.modVersion.c_str());
			}
			sendTo(proto::makeHello(g_watching ? proto::ROLE_SPECTATOR : proto::ROLE_PLAYER, true), from);
			for (int side = 0; side < 2; side++) {
				spectator->sides[side].seq = 0;  // (again) everything we have
				queuePush(*spectator, side);
			}
		}

		// Spectating: a side's palette from upstream, complete and verified (in the cache).
		void watched(int side, uint32_t seq, int charId, int slot, const PaletteHash &hash, const Palette &palette, bool fresh)
		{
			auto &entry = g_watched[side];
			if (seq <= entry.seq)
				return;
			entry = {seq, slot, hash};
			g_watchedChar[side] = charId;
			g_generation++;
			log("net: side %d palette #%u char=%d slot=%d %s%s", side, seq, charId, slot, hash.hex().c_str(), fresh ? "" : " (already cached)");
			setShown(side, charId, slot, hash, palette.serialize());  // for spectators watching through us
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
				// The node we watch through answered.
				auto upstream = hostAddress();
				if (hello.isReply && upstream && sameAddr(from, *upstream) && !g_upstreamKnown) {
					g_upstreamKnown = true;
					g_upstream = from;
					log("net: watching through %s, which has PaletteShare %s", addressText(from).c_str(),
						hello.modVersion.empty() ? "(old)" : hello.modVersion.c_str());
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
						expected = std::string(inet_ntoa(host->sin_addr)) + ":" + std::to_string(ntohs(host->sin_port));
					else
						for (auto &request : g_playRequests)
							expected += std::string(expected.empty() ? "" : ", ") + inet_ntoa(request.from.sin_addr) + ":" +
							            std::to_string(ntohs(request.from.sin_port));
					std::string sender = std::string(inet_ntoa(from.sin_addr)) + ":" + std::to_string(ntohs(from.sin_port));
					log("net: ignored a HELLO from %s, not our opponent (expected %s)", sender.c_str(), expected.empty() ? "?" : expected.c_str());
				}
				return;
			}
			// The highest version both sides speak; checked on every HELLO, so nothing sticks.
			uint8_t common = (std::min)(proto::VERSION, hello.maxVersion);
			if (common < (std::max)(proto::MIN_VERSION, hello.minVersion)) {
				if (!g_peerIncompatible)
					log("net: opponent's PaletteShare %s speaks protocol v%d-v%d, we speak v%d-v%d; not exchanging palettes",
						hello.modVersion.empty() ? "(old)" : hello.modVersion.c_str(), hello.minVersion, hello.maxVersion,
						proto::MIN_VERSION, proto::VERSION);
				g_peerIncompatible = true;
				return;
			}
			g_peerIncompatible = false;
			g_version = common;
			if (!g_peerKnown)
				log("net: opponent has PaletteShare %s (%s), protocol v%d", hello.modVersion.empty() ? "(old)" : hello.modVersion.c_str(),
					hello.isReply ? "reply" : "hello", g_version);
			g_peerKnown = true;
			g_peer = from;
			if (!hello.isReply)
				sendTo(proto::makeHello(proto::ROLE_PLAYER, true), from);
			if (g_offer)
				g_offer->nextSend = 0;  // send our palette right away
		}

		// Records an offer whose palette is in the cache; offers older than what we have are ignored.
		void remember(uint32_t seq, int charId, int slot, const PaletteHash &hash, bool fresh)
		{
			auto &bySlot = g_remoteBySlot[{charId, slot}];
			if (seq > bySlot.seq) {
				bySlot = {seq, slot, hash};
				int remote = 1 - localSide();
				if (remote >= 0 && remote <= 1 && g_loaded[remote].charId == charId && g_loaded[remote].slot == slot)
					refreshShown(remote);  // arrived after their character loaded
			}
			auto &latest = g_remote[charId];
			if (seq <= latest.seq) {
				if (seq < latest.seq)
					log("net: older offer #%u char=%d slot=%d arrived after #%u, not shown", seq, charId, slot, latest.seq);
				return;
			}
			bool changed = latest.slot != slot || !(latest.hash == hash);
			latest = {seq, slot, hash};
			if (changed || fresh) {
				g_generation++;
				log("net: opponent palette #%u char=%d slot=%d %s%s", seq, charId, slot, hash.hex().c_str(), fresh ? "" : " (already cached)");
			}
		}

		void onPalette(const proto::PaletteChunk &chunk, const sockaddr_in &from)
		{
			if (!g_peerKnown || !sameAddr(from, g_peer))
				return;

			auto it = g_pending.find(chunk.hash.bytes);
			if (it == g_pending.end()) {
				// Already have it: just acknowledge, so the sender stops resending.
				if (cache.load(chunk.hash)) {
					remember(chunk.seq, chunk.charId, chunk.slot, chunk.hash, false);
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
				Pending p{-1, chunk.seq, chunk.charId, chunk.slot, std::vector<uint8_t>(chunk.totalSize), std::vector<bool>(chunk.totalSize), chunk.totalSize, GetTickCount64()};
				it = g_pending.emplace(chunk.hash.bytes, std::move(p)).first;
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
				log("net: dropped a palette from the opponent that doesn't match its hash");
				return;
			}
			// A slot's original palette can't be abusive, and browsing offers plenty of them.
			auto original = game::normalCharacter(charId) ? original::palette(game::characterName(charId), slot) : std::nullopt;
			if (!(original && sameColors(*original, *palette)) && ++g_newPalettes > MAX_NEW_PALETTES) {
				if (g_newPalettes == MAX_NEW_PALETTES + 1)
					log("net: the opponent sent more than %d new palettes in this character select, ignoring the rest", MAX_NEW_PALETTES);
				return;
			}
			cache.keep(*palette, chunk.hash);
			g_toStore.emplace_back(*palette, chunk.hash);
			remember(seq, charId, slot, chunk.hash, true);
			sendTo(proto::makeAck(chunk.hash, seq), from);
		}

		// Spectating: a chunk of a side's palette from the node we watch through.
		void onSide(const proto::SideChunk &sideChunk, const sockaddr_in &from)
		{
			auto upstream = hostAddress();
			if (!g_watching || !upstream || !sameAddr(from, *upstream))
				return;
			auto &chunk = sideChunk.chunk;
			int side = sideChunk.side;
			// Keyed by hash and side: both sides may share a palette.
			auto key = chunk.hash.bytes;
			key[0] ^= static_cast<uint8_t>(0x80 * side);
			auto it = g_pending.find(key);
			if (it == g_pending.end()) {
				if (auto palette = cache.load(chunk.hash)) {
					watched(side, chunk.seq, chunk.charId, chunk.slot, chunk.hash, *palette, false);
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
				Pending p{side, chunk.seq, chunk.charId, chunk.slot, std::vector<uint8_t>(chunk.totalSize), std::vector<bool>(chunk.totalSize), chunk.totalSize, GetTickCount64()};
				it = g_pending.emplace(key, std::move(p)).first;
			}
			auto &p = it->second;
			if (p.data.size() != chunk.totalSize)
				return;
			if (chunk.seq > p.seq) {
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
			auto original = game::normalCharacter(charId) ? original::palette(game::characterName(charId), slot) : std::nullopt;
			if (!(original && sameColors(*original, *palette)) && ++g_newPalettes > MAX_NEW_PALETTES)
				return;
			cache.keep(*palette, chunk.hash);
			g_toStore.emplace_back(*palette, chunk.hash);
			watched(side, seq, charId, slot, chunk.hash, *palette, true);
			sendTo(proto::makeAck(chunk.hash, seq), from);
		}

		void onAck(const proto::Ack &ack, const sockaddr_in &from)
		{
			if (auto spectator = spectatorAt(from)) {
				for (int side = 0; side < 2; side++) {
					auto &push = spectator->sides[side];
					if (push.seq == ack.seq && push.hash == ack.hash && !push.acked) {
						push.acked = true;
						spectator->answered = true;
						log("net: spectator %s has side %d's palette", addressText(from).c_str(), side);
					}
				}
				return;
			}
			if (!g_peerKnown || !sameAddr(from, g_peer))
				return;
			// Only the offer it answers: one for an earlier offer with the same palette (the wheel
			// stepping towards its slot) mustn't stop the current one from being sent.
			if (g_offer && g_offer->seq == ack.seq && g_offer->hash == ack.hash && !g_offer->acked) {
				g_offer->acked = true;
				log("net: opponent has our offer #%u", ack.seq);
			}
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
			else if (auto side = proto::parseSide(data, size))
				onSide(*side, from);
			else if (auto ack = proto::parseAck(data, size))
				onAck(*ack, from);
		}

		// The game's join requests to play us. Only those before a match count: during one the game
		// turns new requests away, so a stranger can't become "the opponent" by sending one.
		void notePlayRequest(const sockaddr_in &from)
		{
			std::lock_guard lock(g_mutex);
			if (g_inSession)
				return;
			auto now = GetTickCount64();
			for (auto &request : g_playRequests)
				if (sameAddr(request.from, from)) {
					request.at = now;
					return;
				}
			if (g_playRequests.size() >= 8)
				g_playRequests.erase(g_playRequests.begin());
			g_playRequests.push_back({from, now});
		}

		// The game's join requests to spectate us (at any time; we may also be a spectator relaying).
		void noteSpectateRequest(const sockaddr_in &from)
		{
			std::lock_guard lock(g_mutex);
			auto now = GetTickCount64();
			for (auto &request : g_spectateRequests)
				if (sameAddr(request.from, from)) {
					request.at = now;
					return;
				}
			if (g_spectateRequests.size() >= 32)
				g_spectateRequests.erase(g_spectateRequests.begin());
			g_spectateRequests.push_back({from, now});
		}

		int __stdcall onRecvFrom(SOCKET s, char *buf, int len, int flags, sockaddr *from, int *fromlen)
		{
			for (;;) {
				int r = orig_recvfrom(s, buf, len, flags, from, fromlen);
				if (r <= 0 || (flags & MSG_PEEK))
					return r;
				bool fromV4 = from && fromlen && *fromlen >= static_cast<int>(sizeof(sockaddr_in)) && from->sa_family == AF_INET;
				if (!proto::isOurs(reinterpret_cast<uint8_t *>(buf), r)) {
					if (fromV4 && r > static_cast<int>(INIT_REQUEST_TYPE_AT) && static_cast<uint8_t>(buf[0]) == GAME_INIT_REQUEST) {
						uint8_t type = static_cast<uint8_t>(buf[INIT_REQUEST_TYPE_AT]);
						if (type == PLAY_REQUEST)
							notePlayRequest(*reinterpret_cast<sockaddr_in *>(from));
						else if (type == SPECTATE_REQUEST)
							guarded("network", [&] { noteSpectateRequest(*reinterpret_cast<sockaddr_in *>(from)); });
					}
					return r;
				}
				if (fromV4) {
					guarded("network", [&] { handle(reinterpret_cast<uint8_t *>(buf), r, *reinterpret_cast<sockaddr_in *>(from)); });
					guarded("network send", flushOutbox);
				}
				// Ours: hide it from the game and wait for the next datagram.
			}
		}

		void sendToSpectators(ULONGLONG now)
		{
			for (auto &spectator : g_spectators)
				for (auto &push : spectator.sides) {
					int limit = spectator.answered ? SIDE_MAX_SENDS : SIDE_MAX_SENDS_UNANSWERED;
					if (push.acked || push.sends >= limit || now < push.nextSend)
						continue;
					for (auto &packet : push.packets)
						sendTo(packet, spectator.addr);
					push.sends++;
					push.nextSend = now + OFFER_INTERVAL_MS;
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
			bool newSelect = g_inSession && scene != g_lastScene && (scene == SokuLib::SCENE_SELECTSV || scene == SokuLib::SCENE_SELECTCL);
			g_lastScene = scene;
			if (newSelect) {
				// Each character select starts fresh: a lost handshake gets another chance (per
				// rematch), and the new-palette count starts over.
				g_newPalettes = 0;
				if (!g_peerKnown && (g_helloTries > 0 || g_peerIncompatible)) {
					g_helloTries = 0;
					g_peerIncompatible = false;
					log("net: new character select, trying the handshake again");
				}
			}
			auto now = GetTickCount64();
			sendToSpectators(now);
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
							log("net: no answer; the game we watch has no PaletteShare (or an older one)");
					}
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

			if (g_offer && !g_offer->acked && g_offer->sends < OFFER_MAX_SENDS && now >= g_offer->nextSend) {
				for (auto &packet : g_offer->packets)
					sendTo(packet, g_peer);
				g_offer->sends++;
				g_offer->nextSend = now + OFFER_INTERVAL_MS;
			}
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
		if (localSide() < 0)
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
		Offer offer{seq, charId, slot, hash, proto::makePalette(seq, static_cast<uint16_t>(charId), static_cast<uint8_t>(slot), hash, palette.serialize())};
		g_offer = std::move(offer);
		log("net: offering #%u char=%d slot=%d %s", seq, charId, slot, hash.hex().c_str());
	}

	std::optional<PaletteHash> remotePalette(int charId, int slot)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_remoteBySlot.find({charId, slot});
		if (it == g_remoteBySlot.end())
			return std::nullopt;
		return it->second.hash;
	}

	std::optional<PaletteHash> remoteShown(int charId)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_remote.find(charId);
		if (it == g_remote.end())
			return std::nullopt;
		return it->second.hash;
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

	std::optional<PaletteHash> watchedPalette(int side, int charId, int slot)
	{
		std::lock_guard lock(g_mutex);
		if (!g_watching || side < 0 || side > 1 || g_watchedChar[side] != charId || g_watched[side].slot != slot)
			return std::nullopt;
		return g_watched[side].hash;
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
				guarded("cache", storeReceived);
				Sleep(50);
			}
		});
		log("net: recvfrom hooked (previous %p)", orig_recvfrom);
	}
}
