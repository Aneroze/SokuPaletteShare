#include "sitereport.hpp"
#include "guard.hpp"
#include "log.hpp"
#include "net.hpp"
#include <windows.h>
#include <winhttp.h>
#include <SokuLib.hpp>
#include <atomic>
#include <mutex>
#include <thread>

namespace ps::site {
	namespace {
		std::wstring g_url = L"https://soku.cleista.cc/api/v1/palette-share";
		std::mutex g_mutex;
		std::string g_pending, g_sent;  // the body to send, the last one sent
		ULONGLONG g_pendingAt = 0;
		std::atomic<bool> g_stop = false;
		std::thread *g_thread = nullptr;  // never destroyed: a running std::thread's destructor aborts

		constexpr ULONGLONG SETTLE_MS = 1500;  // both sides load within a few ms of each other
		constexpr int TIMEOUT_MS = 5000;

		// One attempt; the HTTP status, 0 without an answer.
		DWORD post(const std::string &body)
		{
			URL_COMPONENTS url{sizeof(url)};
			wchar_t host[256], path[2048];
			url.lpszHostName = host, url.dwHostNameLength = ARRAYSIZE(host);
			url.lpszUrlPath = path, url.dwUrlPathLength = ARRAYSIZE(path);
			if (!WinHttpCrackUrl(g_url.c_str(), 0, 0, &url))
				return 0;
			std::string version = PALETTESHARE_VERSION;
			DWORD status = 0, size = sizeof(status);
			HINTERNET session = WinHttpOpen((L"PaletteShare/" + std::wstring(version.begin(), version.end())).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			// Windows 7 leaves TLS 1.2 off in WinHTTP: ask for it (refused without KB3140245: defaults).
			DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
			if (session) {
				WinHttpSetTimeouts(session, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS);
				WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls));
			}
			HINTERNET connection = session ? WinHttpConnect(session, host, url.nPort, 0) : nullptr;
			HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			                                                    url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
			if (request && WinHttpSendRequest(request, L"Content-Type: application/json\r\n", static_cast<DWORD>(-1), const_cast<char *>(body.data()),
			                                  static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) && WinHttpReceiveResponse(request, nullptr))
				WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			else
				log("site: no answer (error %lu)", GetLastError());
			for (HINTERNET handle : {request, connection, session})
				if (handle)
					WinHttpCloseHandle(handle);
			return status;
		}

		void run()
		{
			while (!g_stop) {
				Sleep(250);
				std::string body;
				{
					std::lock_guard lock(g_mutex);
					if (g_pending == g_sent)
						g_pending.clear();  // same seed and palettes as last time
					if (g_pending.empty() || GetTickCount64() - g_pendingAt < SETTLE_MS)
						continue;
					body = g_sent = std::move(g_pending);
					g_pending.clear();
				}
				log("site: report sent, HTTP %lu", post(body));
			}
		}
	}

	void start(bool upload, const std::wstring &url)
	{
		if (!url.empty())
			g_url = url;
		log("site: %s%ls", upload ? "reports to " : "reports off ([Site] Upload=0)", upload ? g_url.c_str() : L"");
		if (upload)
			g_thread = new std::thread([] { guarded("site reports", run); });
	}

	void shutdown()
	{
		if (!g_thread || !g_thread->joinable())
			return;
		g_stop = true;
		WaitForSingleObject(g_thread->native_handle(), 500);  // a request in flight may take its timeouts
		g_thread->detach();
	}

	void offer(const nlohmann::json &value)
	{
		if (!g_thread)
			return;
		// Online only: a player in netplay, or spectating (the watch scenes; replays play in others).
		int side = net::localSide();
		bool watching = SokuLib::sceneId == SokuLib::SCENE_LOADINGWATCH || SokuLib::sceneId == SokuLib::SCENE_BATTLEWATCH;
		if (side < 0 && !watching)
			return;
		auto text = body(SokuLib::gameParams.randomSeed, side == 0 ? "p1" : side == 1 ? "p2" : "spectator", value);
		std::lock_guard lock(g_mutex);
		g_pending = std::move(text);  // empty (no palettes now): nothing pending
		g_pendingAt = GetTickCount64();
	}
}
