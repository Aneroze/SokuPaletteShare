#include "saved.hpp"
#include "log.hpp"
#include "palettefile.hpp"
#include <windows.h>
#include <algorithm>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

namespace ps::saved {
	namespace {
		struct Job {
			std::wstring character, player;
			Palette palette;
			bool notify = false;
		};

		std::filesystem::path g_folder;
		std::function<void(const Result &)> g_onNotify;
		std::mutex g_mutex;
		std::condition_variable g_wake;
		std::deque<Job> g_jobs;
		bool g_stop = false;
		std::thread *g_thread = nullptr;  // never destroyed: a running std::thread's destructor aborts

		constexpr size_t MAX_QUEUED = 64;

		std::wstring lower(std::wstring s)
		{
			std::transform(s.begin(), s.end(), s.begin(), towlower);
			return s;
		}

		std::wstring wide(const std::string &s, UINT codePage)
		{
			int n = MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
			if (n <= 0)
				return std::wstring(s.begin(), s.end());  // no such code page (unlikely): ASCII as is
			std::wstring w(n, L'\0');
			MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
			return w;
		}

		// NN of "<prefix>NN.pal" (prefix compared without case, like Windows does), else 0.
		int numberOf(const std::wstring &lowerFile, const std::wstring &lowerPrefix)
		{
			if (lowerFile.size() <= lowerPrefix.size() + 4 || lowerFile.compare(0, lowerPrefix.size(), lowerPrefix) != 0 ||
			    lowerFile.compare(lowerFile.size() - 4, 4, L".pal") != 0)
				return 0;
			auto digits = lowerFile.substr(lowerPrefix.size(), lowerFile.size() - lowerPrefix.size() - 4);
			if (digits.empty() || digits.size() > 6 || !std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
				return 0;
			return std::stoi(digits);
		}

		void run()
		{
			for (;;) {
				std::unique_lock lock(g_mutex);
				g_wake.wait(lock, [] { return g_stop || !g_jobs.empty(); });
				if (g_jobs.empty())
					return;  // stopping, and everything is written
				auto job = std::move(g_jobs.front());
				g_jobs.pop_front();
				lock.unlock();

				Result result;
				try {
					result = saveNow(g_folder, job.character, job.player, job.palette);
				} catch (const std::exception &e) {
					log("saved: %s", e.what());
				}
				switch (result.kind) {
				case Result::Saved: log("saved: opponent palette written to %ls", result.file.c_str()); break;
				case Result::AlreadySaved: log("saved: opponent palette already saved as %ls", result.file.c_str()); break;
				case Result::Failed: log("saved: could not write the opponent's palette to %ls", g_folder.c_str()); break;
				}
				if (job.notify && g_onNotify)
					g_onNotify(result);
			}
		}
	}

	std::wstring decodeName(const std::string &name, unsigned codePage, unsigned fallbackCodePage)
	{
		int n = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
		if (n > 0 || name.empty())
			return wide(name, codePage);
		return wide(name, fallbackCodePage);
	}

	std::wstring cleanName(const std::wstring &name)
	{
		std::wstring out;
		for (wchar_t c : name)
			out += (c < 0x20 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
		auto blank = [](wchar_t c) { return c == L' ' || c == L'.' || c == L'\x3000'; };
		while (!out.empty() && blank(out.back()))
			out.pop_back();
		size_t start = 0;
		while (start < out.size() && blank(out[start]))
			start++;
		out.erase(0, start);
		return out.empty() ? L"unknown" : out;
	}

	Result saveNow(const std::filesystem::path &folder, const std::wstring &character, const std::wstring &player, const Palette &palette)
	{
		Result result;
		if (!palette.valid())
			return result;
		auto prefix = cleanName(character) + L"-" + cleanName(player) + L"-";
		auto lowerPrefix = lower(prefix);
		auto hash = hashPalette(palette);

		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		int last = 0;
		for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
			auto file = it->path().filename().wstring();
			int number = numberOf(lower(file), lowerPrefix);
			if (!number)
				continue;
			last = (std::max)(last, number);
			auto existing = readPaletteFile(it->path());
			if (existing && hashPalette(*existing) == hash) {
				result.kind = Result::AlreadySaved;
				result.file = file;
				return result;
			}
		}
		if (ec)
			return result;

		wchar_t number[16];
		swprintf(number, 16, L"%02d", last + 1);
		auto file = prefix + number + L".pal";
		auto target = folder / file;
		auto temp = folder / (file + L".tmp");
		auto bytes = palette.serialize();
		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			if (!out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()))
				return result;
		}
		if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {  // never over another file
			std::filesystem::remove(temp, ec);
			return result;
		}
		result.kind = Result::Saved;
		result.file = file;
		return result;
	}

	void start(const std::filesystem::path &folder, std::function<void(const Result &)> onNotify)
	{
		g_folder = folder;
		g_onNotify = std::move(onNotify);
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		g_thread = new std::thread(run);
	}

	void shutdown()
	{
		{
			std::lock_guard lock(g_mutex);
			g_stop = true;
		}
		g_wake.notify_all();
		if (!g_thread || !g_thread->joinable())
			return;
		// Bounded: at process exit the thread may already be gone or unable to finish.
		WaitForSingleObject(g_thread->native_handle(), 1000);
		g_thread->detach();
	}

	void save(const std::string &character, const std::string &player, unsigned playerCodePage, const Palette &palette, bool notify)
	{
		{
			std::lock_guard lock(g_mutex);
			if (g_stop || !g_thread || g_jobs.size() >= MAX_QUEUED)
				return;
			g_jobs.push_back(Job{wide(character, CP_ACP), decodeName(player, playerCodePage, GetACP()), palette, notify});
		}
		g_wake.notify_one();
	}
}
