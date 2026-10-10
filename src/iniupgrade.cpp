#include "iniupgrade.hpp"
#include "log.hpp"
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace ps::ini {
	namespace {
		// One ini line. Keys: KEY = Key=value, COMMENTED_KEY = ";Key=value" (no blank after the ';').
		struct Line {
			enum Kind { OTHER, SECTION, KEY, COMMENTED_KEY } kind = OTHER;
			std::string section, key, value, raw;
			bool used = false;
		};

		bool same(const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

		std::string trimmed(const std::string &s)
		{
			size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
			return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
		}

		bool keyChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

		std::vector<Line> parse(const std::string &text)
		{
			std::vector<Line> out;
			std::string section;
			size_t at = 0;
			if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
				at = 3;  // UTF-8 byte order mark
			while (at < text.size()) {
				size_t nl = text.find('\n', at);
				std::string raw = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
				at = nl == std::string::npos ? text.size() : nl + 1;
				if (!raw.empty() && raw.back() == '\r')
					raw.pop_back();
				Line line{Line::OTHER, section, "", "", raw};
				std::string t = trimmed(raw);
				if (!t.empty() && t[0] == '[') {
					size_t close = t.find(']');
					section = line.section = t.substr(1, close == std::string::npos ? std::string::npos : close - 1);
					line.kind = Line::SECTION;
				} else if (!t.empty()) {
					bool commented = t[0] == ';';
					size_t k = commented ? 1 : 0, eq = t.find('=');
					std::string key = eq == std::string::npos ? "" : trimmed(t.substr(k, eq - k));
					bool ident = !key.empty() && t[k] != ' ' && t[k] != '\t';
					for (char c : key)
						ident = ident && keyChar(c);
					if (ident) {
						line.kind = commented ? Line::COMMENTED_KEY : Line::KEY;
						line.key = key;
						line.value = trimmed(t.substr(eq + 1));
					}
				}
				out.push_back(line);
			}
			return out;
		}

		// The first line of `kind` for section/key (case-insensitive, like GetPrivateProfileString).
		Line *find(std::vector<Line> &lines, Line::Kind kind, const std::string &section, const std::string &key)
		{
			for (auto &line : lines)
				if (line.kind == kind && same(line.section, section) && same(line.key, key))
					return &line;
			return nullptr;
		}

		// Marks every line of section/key (any kind) as carried over, so duplicates aren't appended.
		void markUsed(std::vector<Line> &lines, const std::string &section, const std::string &key)
		{
			for (auto &line : lines)
				if ((line.kind == Line::KEY || line.kind == Line::COMMENTED_KEY) && same(line.section, section) && same(line.key, key))
					line.used = true;
		}

		// Values whose meaning changed, rewritten into what this version's ini means by them.
		void fixup(std::vector<Line> &user, unsigned long long from)
		{
			if (from < version("1.1.0")) {
				// Up to 1.0.0, a [Hotkey] Modifier or Key taken out of the ini meant none: it says so now.
				for (const char *key : {"Modifier", "Key"})
					if (!find(user, Line::KEY, "Hotkey", key))
						user.push_back(Line{Line::KEY, "Hotkey", key, "", ""});
				// Test builds' [Replay] Storage is gone.
				markUsed(user, "Replay", "Storage");
			}
		}

		// Appends the player's keys of `section` not carried over yet, before the section's last blank lines.
		void appendLeftovers(std::vector<std::string> &out, std::vector<Line> &user, const std::string &section)
		{
			if (section.empty())
				return;  // keys before any section are never read
			size_t at = out.size();
			while (at > 0 && trimmed(out[at - 1]).empty())
				at--;
			std::vector<std::string> add;
			for (auto &line : user)
				if (line.kind == Line::KEY && !line.used && same(line.section, section)) {
					add.push_back(line.key + "=" + line.value);
					markUsed(user, line.section, line.key);
				}
			out.insert(out.begin() + at, add.begin(), add.end());
		}

		bool hasLeftovers(const std::vector<Line> &user, const std::string &section)
		{
			for (auto &line : user)
				if (line.kind == Line::KEY && !line.used && same(line.section, section))
					return true;
			return false;
		}

		std::vector<std::string> merge(std::vector<Line> &shipped, std::vector<Line> &user)
		{
			std::vector<std::string> out;
			std::string section;
			for (auto &t : shipped) {
				if (t.kind == Line::SECTION) {
					appendLeftovers(out, user, section);
					section = t.section;
					out.push_back(t.raw);
				} else if (t.kind == Line::KEY && same(t.key, "IniVersion")) {
					out.push_back(t.raw);  // the shipped ini has this build's version
					markUsed(user, section, t.key);
				} else if (t.kind == Line::KEY || t.kind == Line::COMMENTED_KEY) {
					auto u = find(user, Line::KEY, section, t.key);
					auto c = t.kind == Line::KEY && !u ? find(user, Line::COMMENTED_KEY, section, t.key) : nullptr;
					out.push_back(u ? t.key + "=" + u->value : c ? c->raw : t.raw);
					markUsed(user, section, t.key);
				} else {
					out.push_back(t.raw);
				}
			}
			appendLeftovers(out, user, section);
			for (auto &s : user) {  // sections the shipped ini doesn't have, if anything is left in them
				if (s.kind != Line::SECTION || s.used)
					continue;
				bool known = false;
				for (auto &t : shipped)
					known = known || (t.kind == Line::SECTION && same(t.section, s.section));
				for (auto &o : user)
					if (o.kind == Line::SECTION && same(o.section, s.section))
						o.used = true;
				if (known || !hasLeftovers(user, s.section))
					continue;
				out.push_back("");
				out.push_back("[" + s.section + "]");
				appendLeftovers(out, user, s.section);
			}
			while (!out.empty() && out.back().empty())
				out.pop_back();
			return out;
		}

		std::string iniVersionOf(std::vector<Line> &lines)
		{
			auto line = find(lines, Line::KEY, "Display", "IniVersion");
			return line ? line->value : "";
		}
	}

	unsigned long long version(const std::string &text)
	{
		unsigned long long v = 0;
		const char *s = text.c_str();
		for (int i = 0; i < 4; i++) {
			unsigned part = 0;
			while (*s >= '0' && *s <= '9')
				part = part * 10 + (*s++ - '0');
			v = (v << 16) | (part & 0xFFFF);
			if (*s == '.')
				s++;
		}
		return v;
	}

	std::string upgraded(const std::string &shipped, const std::string &current, const std::string &from)
	{
		auto tmpl = parse(shipped);
		auto user = parse(current);
		fixup(user, version(from));
		std::string text;
		for (auto &line : merge(tmpl, user))
			text += line + "\r\n";
		return text;
	}

	void upgrade(HMODULE module, const std::filesystem::path &file)
	{
		HRSRC res = FindResourceW(module, L"PS_DEFAULT_INI", MAKEINTRESOURCEW(10));  // RT_RCDATA
		HGLOBAL handle = res ? LoadResource(module, res) : nullptr;
		auto data = handle ? static_cast<const char *>(LockResource(handle)) : nullptr;
		if (!data) {
			log("ini: no shipped ini in the DLL, not upgrading");
			return;
		}
		std::string shipped(data, SizeofResource(module, res));

		std::string current;
		{
			std::ifstream in(file, std::ios::binary);
			if (!in)
				return;  // no ini: the defaults
			current.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		if (current.size() > (1u << 20) || (current.size() >= 2 && static_cast<uint8_t>(current[0]) == 0xFF && static_cast<uint8_t>(current[1]) == 0xFE))
			return;  // huge or UTF-16

		auto lines = parse(current);
		auto from = iniVersionOf(lines);
		auto tmplLines = parse(shipped);
		auto to = iniVersionOf(tmplLines);
		if (version(from) >= version(to))
			return;

		auto text = upgraded(shipped, current, from);
		auto temp = file;
		temp += L".tmp";
		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			if (!out.write(text.data(), text.size())) {
				log("ini: could not write %ls, not upgrading", temp.c_str());
				return;
			}
		}
		if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			DWORD error = GetLastError();
			DeleteFileW(temp.c_str());
			log("ini: could not replace %ls (error %lu), not upgrading", file.c_str(), error);
			return;
		}
		log("ini: upgraded from %s to %s", from.empty() ? "an ini without IniVersion (1.0.0)" : from.c_str(), to.c_str());
	}
}
