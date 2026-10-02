#include "core/phonology.h"

#include <algorithm>
#include <map>
#include <set>

namespace svs {

const std::vector<PhonemeInfo> &phonemes() {
    // Read out of VocalWriter's own `ttvi` tables (manner, and the maximum
    // and minimum durations) in its internal order, which is the order of the
    // phoneme codes the engine and its dictionary use.
    static const std::vector<PhonemeInfo> table = {
        {"IY", 0, 170, 60},  {"IH", 0, 160, 60},  {"EH", 0, 160, 60},
        {"AE", 0, 230, 50},  {"AA", 1, 240, 90},  {"UX", 1, 160, 70},
        {"AO", 2, 240, 100}, {"UH", 2, 170, 50},  {"AX", 1, 120, 50},
        {"ER", 2, 180, 90},  {"EY", 0, 200, 110}, {"AY", 1, 250, 100},
        {"OY", 2, 260, 110}, {"AW", 1, 260, 110}, {"OW", 2, 220, 90},
        {"UW", 2, 210, 50},  {"YU", 0, 230, 100}, {"IR", 0, 230, 120},
        {"XR", 0, 250, 120}, {"AR", 1, 250, 120}, {"OR", 2, 250, 120},
        {"UR", 2, 230, 120}, {"IX", 0, 120, 50},  {"%", 3, 305, 200},
        {"RX", 2, 120, 70},  {"LX", 4, 100, 70},  {"EL", 4, 160, 110},
        {"EN", 3, 170, 100}, {"w", 4, 60, 15},    {"y", 0, 75, 30},
        {"r", 4, 65, 30},    {"l", 4, 75, 40},    {"h", 3, 70, 35},
        {"m", 3, 70, 60},    {"n", 3, 65, 35},    {"NG", 3, 80, 50},
        {"f", 3, 100, 60},   {"v", 3, 70, 55},    {"TH", 3, 100, 40},
        {"DH", 3, 60, 35},   {"s", 3, 115, 65},   {"z", 3, 75, 60},
        {"SH", 3, 115, 60},  {"ZH", 3, 70, 50},   {"p", 3, 85, 70},
        {"b", 3, 80, 60},    {"t", 3, 85, 50},    {"d", 3, 80, 40},
        {"k", 3, 90, 75},    {"g", 3, 90, 65},    {"CH", 3, 160, 100},
        {"JH", 3, 100, 70},  {"TX", 3, 70, 50},   {"Q", 3, 20, 20},
        {"QX", 3, 50, 50},   {"DD", 3, 60, 35},   {"O", 2, 160, 60},
    };
    return table;
}

const PhonemeInfo *phoneme(const std::string &symbol) {
    for (const PhonemeInfo &p : phonemes())
        if (symbol == p.symbol) return &p;
    return nullptr;
}

int phoneme_code(const std::string &symbol) {
    const auto &t = phonemes();
    for (size_t i = 0; i < t.size(); ++i)
        if (symbol == t[i].symbol) return int(i);
    return -1;
}

std::string palette_alias(const std::string &symbol) {
    if (symbol == "OH") return "O";
    if (symbol == "DX") return "DD";
    return symbol;
}

const std::vector<PaletteRow> &palette() {
    // The fifty the application's own palette offers, read out of its running
    // interface, with its example words; then the seven it leaves out, which
    // are real phonemes with formants and durations like any other.
    static const std::vector<PaletteRow> rows = [] {
        std::vector<PaletteRow> shown = {
            {"AE", "bAt"},   {"AA", "hOt"},   {"AX", "About"}, {"AO", "cAUght"},
            {"EH", "bEt"},   {"IH", "bIt"},   {"UX", "bUd"},   {"EY", "bAIt"},
            {"AY", "bIte"},  {"IY", "bEEt"},  {"UW", "bOOt"},  {"UH", "bOOk"},
            {"OW", "bOAt"},  {"AW", "bOUt"},  {"OY", "bOY"},   {"YU", "mUte"},
            {"ER", "bIRd"},  {"AR", "bAR"},   {"XR", "bEAR"},  {"IR", "bEER"},
            {"OR", "fOR"},   {"UR", "pOOR"},  {"O", "bOy"},    {"LX", "beLL"},
            {"EL", "bottLE"}, {"EN", "buttON"}, {"b", "Bin"},  {"CH", "CHin"},
            {"d", "Din"},    {"DH", "THem"},  {"f", "Fin"},    {"g", "Gain"},
            {"h", "Hat"},    {"JH", "Joy"},   {"k", "Kin"},    {"l", "Limb"},
            {"m", "Met"},    {"n", "Net"},    {"NG", "siNG"},  {"p", "Pin"},
            {"r", "Ran"},    {"s", "Sin"},    {"SH", "SHin"},  {"t", "Tin"},
            {"TH", "THin"},  {"v", "Van"},    {"w", "Wet"},    {"y", "Yet"},
            {"z", "Zoo"},    {"ZH", "meaSure"},
        };
        std::map<std::string, std::string> examples;
        for (const auto &r : shown) examples[r.symbol] = r.example;
        const PaletteRow unlisted[] = {
            {"IX", "rosES, beatEn"}, {"Q", "greaTer, heaTer"},
            {"DD", "beTTer, whaT"},  {"TX", "iT, can'T"},
            {"RX", "a short r-coloured vowel"}, {"QX", "a longer Q"},
            {"%", "silence, a rest"},
        };
        for (const auto &r : unlisted)
            if (!examples.count(r.symbol)) examples[r.symbol] = r.example;
        // The engine's own table decides the list, so nothing it can
        // pronounce is missing; the order is the engine's.
        std::vector<PaletteRow> out;
        for (const PhonemeInfo &p : phonemes())
            out.push_back({p.symbol, examples.count(p.symbol) ? examples[p.symbol] : ""});
        return out;
    }();
    return rows;
}

namespace {
// What counts as a syllable nucleus: manner 0, 1, 2 or 4 and a maximum of at
// least 110 ms. That separates y (the glide in "yes", manner 0, 75 ms) from
// the vowels, and EL (syllabic l, manner 4, 160 ms) from l, r, w and LX.
const int kNucleusMinMax = 110;
const std::set<std::string> kGlides = {"l", "r", "w", "y", "LX"};
const std::set<std::string> kAfterS = {"p", "t", "k", "m", "n", "f"};
}  // namespace

bool is_nucleus(const std::string &symbol) {
    const PhonemeInfo *p = phoneme(symbol);
    if (!p) return false;
    bool manner = p->manner == 0 || p->manner == 1 || p->manner == 2 || p->manner == 4;
    return manner && p->max_ms >= kNucleusMinMax;
}

bool is_vowel(const std::string &symbol) {
    static const std::set<std::string> v = {
        "IY", "IH", "EH", "AE", "AA", "UX", "AO", "UH", "AX", "ER", "EY", "AY",
        "OY", "AW", "OW", "UW", "YU", "IR", "XR", "AR", "OR", "UR", "IX", "O", "RX"};
    return v.count(symbol) > 0;
}

bool legal_onset(const std::vector<std::string> &syms) {
    if (syms.size() <= 1) return true;
    if (syms.size() == 2)
        return kGlides.count(syms[1]) || (syms[0] == "s" && kAfterS.count(syms[1]));
    if (syms.size() == 3 && syms[0] == "s")
        return legal_onset({syms[1], syms[2]});
    return false;
}

std::vector<std::vector<std::string>> syllabify(const std::vector<std::string> &syms) {
    std::vector<size_t> nuclei;
    for (size_t i = 0; i < syms.size(); ++i)
        if (is_nucleus(syms[i])) nuclei.push_back(i);
    if (nuclei.size() < 2) {
        if (syms.empty()) return {};
        return {syms};
    }
    std::vector<size_t> cuts;
    for (size_t k = 0; k + 1 < nuclei.size(); ++k) {
        size_t a = nuclei[k], b = nuclei[k + 1];
        std::vector<std::string> run(syms.begin() + a + 1, syms.begin() + b);
        size_t take = 0;
        while (take < run.size()) {
            std::vector<std::string> tail(run.end() - (take + 1), run.end());
            if (!legal_onset(tail)) break;
            ++take;
        }
        cuts.push_back(b - take);
    }
    std::vector<std::vector<std::string>> out;
    size_t start = 0;
    for (size_t c : cuts) {
        out.emplace_back(syms.begin() + start, syms.begin() + c);
        start = c;
    }
    out.emplace_back(syms.begin() + start, syms.end());
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::vector<std::string> &g) { return g.empty(); }),
              out.end());
    return out;
}

std::vector<std::vector<std::string>> regroup(const std::vector<std::string> &phonemes,
                                              int count) {
    auto parts = syllabify(phonemes);
    int n = int(parts.size());
    count = std::max(1, std::min(count, n));
    if (count == n) return parts;
    std::vector<std::vector<std::string>> out;
    int at = 0;
    for (int k = 0; k < count; ++k) {
        int take = (n - at) / (count - k);
        std::vector<std::string> group;
        for (int j = at; j < at + take; ++j)
            group.insert(group.end(), parts[j].begin(), parts[j].end());
        out.push_back(group);
        at += take;
    }
    return out;
}

void consonant_factors(double consonants, double *onset, double *coda) {
    // One control rather than two, drawn through the two settings known to
    // work: 1.0, the natural length, and 0.40, at which the codas sit at 0.70.
    const double kCoda = 0.5, kMin = 0.1;
    double o = std::max(kMin, std::min(1.0, consonants));
    *onset = o;
    *coda = 1.0 - (1.0 - o) * kCoda;
}

std::vector<double> syllable_lengths(const std::vector<std::string> &syms, double ms,
                                     double consonants) {
    size_t n = syms.size();
    std::vector<double> mx(n), mn(n);
    for (size_t i = 0; i < n; ++i) {
        const PhonemeInfo *p = phoneme(syms[i]);
        mx[i] = p && p->max_ms ? p->max_ms : 80.0;
        mn[i] = p && p->min_ms ? p->min_ms : 40.0;
    }
    double summn = 0;
    for (double v : mn) summn += v;
    if (summn <= 0) return mn;
    std::vector<size_t> nuclei;
    for (size_t i = 0; i < n; ++i)
        if (is_nucleus(syms[i])) nuclei.push_back(i);
    if (nuclei.empty()) {
        // no vowel: stretch the phoneme with the most room
        size_t best = 0;
        for (size_t i = 1; i < n; ++i)
            if (mx[i] - mn[i] > mx[best] - mn[best]) best = i;
        nuclei.push_back(best);
    }
    auto in_nuclei = [&](size_t i) {
        return std::find(nuclei.begin(), nuclei.end(), i) != nuclei.end();
    };
    size_t first = nuclei[0];
    double onset, coda;
    consonant_factors(consonants, &onset, &coda);
    std::vector<double> floor_(n), want(n);
    for (size_t i = 0; i < n; ++i) {
        floor_[i] = in_nuclei(i) ? mn[i] : mn[i] * (i < first ? onset : coda);
        want[i] = in_nuclei(i) ? mx[i] : floor_[i];
    }
    double smin = 0, total = 0;
    for (size_t i = 0; i < n; ++i) {
        smin += floor_[i];
        total += want[i];
    }
    std::vector<double> out(n);
    if (ms <= smin) {
        double k = smin ? ms / smin : 1.0;
        for (size_t i = 0; i < n; ++i) out[i] = floor_[i] * k;
        return out;
    }
    if (ms <= total) {
        double k = total > smin ? (ms - smin) / (total - smin) : 0.0;
        for (size_t i = 0; i < n; ++i) out[i] = floor_[i] + k * (want[i] - floor_[i]);
        return out;
    }
    double extra = (ms - total) / nuclei.size();
    out = want;
    for (size_t i : nuclei) out[i] += extra;
    return out;
}

std::vector<std::string> allophones(const std::vector<std::string> &phones) {
    static const std::map<std::pair<std::string, std::string>, std::string> r_coloured = {
        {{"AA", "r"}, "AR"}, {{"AO", "r"}, "OR"}, {{"AX", "r"}, "ER"},
        {{"UX", "r"}, "ER"}, {{"EH", "r"}, "XR"}, {{"IH", "r"}, "IR"},
        {{"UH", "r"}, "UR"}, {{"IY", "r"}, "IR"}, {{"UW", "r"}, "UR"}};
    static const std::set<std::string> vowels = {
        "IY", "IH", "EH", "AE", "AA", "UX", "AO", "UH", "AX", "ER", "EY", "AY",
        "OY", "AW", "OW", "UW", "YU", "IR", "XR", "AR", "OR", "UR", "IX", "O"};
    std::vector<std::string> out;
    size_t i = 0;
    while (i < phones.size()) {
        bool has_pair = i + 1 < phones.size();
        if (has_pair) {
            auto it = r_coloured.find({phones[i], phones[i + 1]});
            if (it != r_coloured.end()) {
                out.push_back(it->second);
                i += 2;
                continue;
            }
            if (phones[i] == "y" && phones[i + 1] == "UW" && i + 2 == phones.size()) {
                out.push_back("YU");
                i += 2;
                continue;
            }
            if (phones[i] == "AX" && phones[i + 1] == "l" &&
                (i + 2 == phones.size() || !vowels.count(phones[i + 2]))) {
                out.push_back("EL");
                i += 2;
                continue;
            }
        }
        if (phones[i] == "l" && (i + 1 == phones.size() || !vowels.count(phones[i + 1]))) {
            out.push_back("LX");
            ++i;
            continue;
        }
        out.push_back(phones[i]);
        ++i;
    }
    return out;
}

}  // namespace svs
