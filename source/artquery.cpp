#include <cstring>
#include <cctype>
#include "artquery.hpp"

// extensions we recognize on library file names (zip may wrap a rom ext)
static bool isRomExt(const std::string& e) {
    static const char* exts[] = {"zip","7z","gba","agb","nds","dsi","srl","cia","3ds","gb","gbc"};
    for (const char* x : exts) if (e == x) return true;
    return false;
}

static std::string stripKnownExts(const std::string& name) {
    std::string s = name;
    for (;;) {
        size_t dot = s.rfind('.');
        if (dot == std::string::npos || dot == 0) return s;
        std::string ext = s.substr(dot + 1);
        for (auto& c : ext) c = tolower((unsigned char)c);
        if (!isRomExt(ext)) return s;
        s.erase(dot);
    }
}

// erase (...) and [...] blocks, innermost first, until stable
static std::string removeParenthesis(const std::string& in) {
    std::string s = in;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto pair : { std::make_pair('(', ')'), std::make_pair('[', ']') }) {
            size_t close = s.find(pair.second);
            while (close != std::string::npos) {
                size_t open = s.rfind(pair.first, close);
                if (open == std::string::npos) break;
                s.erase(open, close - open + 1);
                changed = true;
                close = s.find(pair.second);
            }
        }
    }
    return s;
}

static std::string collapseSpaces(const std::string& in) {
    std::string out;
    bool space = true; // trims leading
    for (char c : in) {
        if (c == ' ' || c == '\t') {
            if (!space) out += ' ';
            space = true;
        } else {
            out += c;
            space = false;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-'))
        out.pop_back();
    while (!out.empty() && (out.front() == ' ' || out.front() == '-'))
        out.erase(0, 1);
    return out;
}

static bool endsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); i++)
        if (tolower((unsigned char)s[off + i]) != tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

// "Legend of Zelda, The - X" -> "The Legend of Zelda - X" (also ", A"/", An")
static std::string flipArticle(const std::string& in) {
    size_t dash = in.find(" - ");
    std::string head = (dash == std::string::npos) ? in : in.substr(0, dash);
    std::string tail = (dash == std::string::npos) ? "" : in.substr(dash);
    for (const char* suf : {", The", ", An", ", A"}) {
        if (endsWithNoCase(head, suf)) {
            std::string article(suf + 2); // past ", "
            head = article + " " + head.substr(0, head.size() - strlen(suf));
            break;
        }
    }
    return head + tail;
}

std::string artSanitizeQuery(const std::string& fsName) {
    std::string s = stripKnownExts(fsName);
    s = removeParenthesis(s);
    for (auto& c : s) if (c == '_') c = ' ';
    s = flipArticle(collapseSpaces(s));
    return collapseSpaces(s);
}

// fold the common UTF-8 Latin accents/macrons to ASCII; drop other multibyte
static void foldUtf8(const std::string& in, std::string& out) {
    struct Fold { const char* seq; const char* to; };
    static const Fold folds[] = {
        {"\xC3\xA1","a"},{"\xC3\xA0","a"},{"\xC3\xA2","a"},{"\xC3\xA4","a"},{"\xC3\xA3","a"},{"\xC3\xA5","a"},
        {"\xC3\xA9","e"},{"\xC3\xA8","e"},{"\xC3\xAA","e"},{"\xC3\xAB","e"},
        {"\xC3\xAD","i"},{"\xC3\xAC","i"},{"\xC3\xAE","i"},{"\xC3\xAF","i"},
        {"\xC3\xB3","o"},{"\xC3\xB2","o"},{"\xC3\xB4","o"},{"\xC3\xB6","o"},{"\xC3\xB5","o"},{"\xC3\xB8","o"},
        {"\xC3\xBA","u"},{"\xC3\xB9","u"},{"\xC3\xBB","u"},{"\xC3\xBC","u"},
        {"\xC3\xB1","n"},{"\xC3\xA7","c"},{"\xC3\xA6","ae"},{"\xC3\x9F","ss"},
        {"\xC4\x81","a"},{"\xC4\x93","e"},{"\xC4\xAB","i"},{"\xC5\x8D","o"},{"\xC5\xAB","u"},
        // uppercase variants land here already lowercased by caller only for
        // ASCII; fold the common uppercase accents too
        {"\xC3\x81","a"},{"\xC3\x80","a"},{"\xC3\x84","a"},{"\xC3\x89","e"},{"\xC3\x88","e"},
        {"\xC3\x8D","i"},{"\xC3\x93","o"},{"\xC3\x96","o"},{"\xC3\x9A","u"},{"\xC3\x9C","u"},
        {"\xC3\x91","n"},{"\xC5\x8C","o"},{"\xC5\xAA","u"},
    };
    for (size_t i = 0; i < in.size();) {
        unsigned char c = in[i];
        if (c < 0x80) { out += in[i]; i++; continue; }
        bool matched = false;
        for (const Fold& f : folds) {
            size_t n = strlen(f.seq);
            if (in.compare(i, n, f.seq) == 0) {
                out += f.to;
                i += n;
                matched = true;
                break;
            }
        }
        if (!matched) {
            // skip the whole unknown UTF-8 sequence
            i++;
            while (i < in.size() && (in[i] & 0xC0) == 0x80) i++;
        }
    }
}

std::string artNorm(const std::string& name) {
    std::string folded;
    foldUtf8(name, folded);
    // tokenize on non-alnum; & becomes its own "and" token
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : folded) {
        if (isalnum((unsigned char)c)) {
            cur += tolower((unsigned char)c);
        } else {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            if (c == '&') tokens.push_back("and");
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    // drop a trailing platform token
    if (tokens.size() > 1) {
        const std::string& last = tokens.back();
        if (last == "gba" || last == "nds" || last == "ds")
            tokens.pop_back();
    }
    std::string out;
    for (auto& t : tokens) out += t;
    return out;
}

ArtConfidence artConfidence(const std::string& query,
                            const std::vector<std::string>& resultNames,
                            int* bestIdx) {
    if (bestIdx) *bestIdx = -1;
    if (resultNames.empty()) return ART_MATCH_NONE;
    std::string q = artNorm(query);
    if (q.empty()) return ART_MATCH_WEAK;
    for (size_t i = 0; i < resultNames.size(); i++) {
        if (artNorm(resultNames[i]) == q) {
            if (bestIdx) *bestIdx = (int)i;
            return ART_MATCH_STRONG;
        }
    }
    std::string top = artNorm(resultNames[0]);
    if (!top.empty() &&
        (top.find(q) != std::string::npos || q.find(top) != std::string::npos)) {
        if (bestIdx) *bestIdx = 0;
        return ART_MATCH_MEDIUM;
    }
    return ART_MATCH_WEAK;
}

// RetroArch thumbnail filename rule: these characters become '_'
static std::string retroarchSanitize(const std::string& s) {
    std::string out = s;
    for (auto& c : out)
        if (strchr("&*/:`<>?\\|", c)) c = '_';
    return out;
}

std::vector<std::string> libretroNameVariants(const std::string& fsName) {
    std::vector<std::string> out;
    std::string stem = stripKnownExts(fsName);
    out.push_back(retroarchSanitize(stem));
    // retry variant: drop [..] blocks and (Translated..)-style paren blocks
    std::string retry;
    for (size_t i = 0; i < stem.size();) {
        if (stem[i] == '[') {
            size_t close = stem.find(']', i);
            if (close == std::string::npos) { retry += stem.substr(i); break; }
            i = close + 1;
        } else if (stem[i] == '(') {
            size_t close = stem.find(')', i);
            std::string block = (close == std::string::npos) ? "" : stem.substr(i + 1, close - i - 1);
            std::string lower = block;
            for (auto& c : lower) c = tolower((unsigned char)c);
            if (close != std::string::npos && lower.find("translat") != std::string::npos) {
                i = close + 1;
            } else {
                retry += stem[i];
                i++;
            }
        } else {
            retry += stem[i];
            i++;
        }
    }
    retry = retroarchSanitize(collapseSpaces(retry));
    if (retry != out[0] && !retry.empty()) out.push_back(retry);
    return out;
}
