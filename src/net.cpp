#include "net.hpp"
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
		std::atomic<unsigned> g_generation = 0;

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

		void sendTo(const std::vector<uint8_t> &packet, const sockaddr_in &to)
		{
			SOCKET s = gameSocket();
			if (s == INVALID_SOCKET)
				return;
			auto send = *reinterpret_cast<SendTo *>(IAT_SENDTO);
			send(s, reinterpret_cast<const char *>(packet.data()), static_cast<int>(packet.size()), 0,
			     reinterpret_cast<const sockaddr *>(&to), sizeof(to));
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
			g_remote.clear();
			g_remoteBySlot.clear();
		}

		void onHello(const proto::Hello &hello, const sockaddr_in &from)
		{
			if (hello.role != proto::ROLE_PLAYER)
				return;  // spectator support comes later; this version only talks to the opponent
			if (g_peerKnown && !sameAddr(from, g_peer))
				return;
			if (hello.version != proto::VERSION) {
				if (!g_peerIncompatible)
					log("net: opponent speaks protocol v%d, we speak v%d; not exchanging palettes", hello.version, proto::VERSION);
				g_peerIncompatible = true;
				return;
			}
			if (!g_peerKnown)
				log("net: opponent has PaletteShare (%s)", hello.isReply ? "reply" : "hello");
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
			if (seq > bySlot.seq)
				bySlot = {seq, slot, hash};
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
					sendTo(proto::makeAck(chunk.hash), from);
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
			cache.store(*palette, chunk.hash);
			remember(seq, charId, slot, chunk.hash, true);
			sendTo(proto::makeAck(chunk.hash), from);
		}

		void onAck(const PaletteHash &hash, const sockaddr_in &from)
		{
			if (!g_peerKnown || !sameAddr(from, g_peer))
				return;
			if (g_offer && g_offer->hash == hash && !g_offer->acked) {
				g_offer->acked = true;
				log("net: opponent has our palette %s", hash.hex().c_str());
			}
		}

		void handle(const uint8_t *data, size_t size, const sockaddr_in &from)
		{
			std::lock_guard lock(g_mutex);
			if (!g_inSession)
				return;
			if (auto hello = proto::parseHello(data, size))
				onHello(*hello, from);
			else if (proto::versionOf(data, size) != proto::VERSION)
				return;  // other kinds are only understood within our version
			else if (auto chunk = proto::parsePalette(data, size))
				onPalette(*chunk, from);
			else if (auto ack = proto::parseAck(data, size))
				onAck(*ack, from);
		}

		int __stdcall onRecvFrom(SOCKET s, char *buf, int len, int flags, sockaddr *from, int *fromlen)
		{
			for (;;) {
				int r = orig_recvfrom(s, buf, len, flags, from, fromlen);
				if (r <= 0 || (flags & MSG_PEEK) || !proto::isOurs(reinterpret_cast<uint8_t *>(buf), r))
					return r;
				if (from && fromlen && *fromlen >= static_cast<int>(sizeof(sockaddr_in)) && from->sa_family == AF_INET)
					handle(reinterpret_cast<uint8_t *>(buf), r, *reinterpret_cast<sockaddr_in *>(from));
				// Ours: hide it from the game and wait for the next datagram.
			}
		}

		void tick()
		{
			std::lock_guard lock(g_mutex);
			bool session = localSide() >= 0 && inNetplayScene();
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
			if (!g_inSession || g_peerIncompatible)
				return;

			auto now = GetTickCount64();
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

	void offerLocal(int charId, int slot, const Palette &palette, const PaletteHash &hash)
	{
		std::lock_guard lock(g_mutex);
		if (localSide() < 0)
			return;
		if (g_offer && g_offer->charId == charId && g_offer->slot == slot && g_offer->hash == hash)
			return;

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

	unsigned remoteGeneration()
	{
		return g_generation;
	}

	void install()
	{
		DWORD old;
		auto slot = reinterpret_cast<RecvFrom *>(IAT_RECVFROM);
		VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old);
		orig_recvfrom = *slot;
		*slot = onRecvFrom;
		VirtualProtect(slot, sizeof(*slot), old, &old);

		std::thread([] {
			for (;;) {
				tick();
				Sleep(50);
			}
		}).detach();
		log("net: recvfrom hooked (previous %p)", orig_recvfrom);
	}
}
