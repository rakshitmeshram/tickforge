// Tiny --key value argument parser shared by the command-line tools.
#pragma once

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace tickforge::cli {

class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];
            if (k.rfind("--", 0) != 0) {
                std::fprintf(stderr, "unexpected argument: %s\n", argv[i]);
                std::exit(2);
            }
            k = k.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                kv_[k] = argv[++i];
            else
                kv_[k] = "1";
        }
    }
    [[nodiscard]] bool has(const std::string& k) const { return kv_.count(k) != 0; }
    [[nodiscard]] std::string str(const std::string& k, const std::string& def) const {
        auto it = kv_.find(k);
        return it == kv_.end() ? def : it->second;
    }
    // Integer option. Exits with status 2 on anything that is not a whole
    // number in [lo, hi] (atoll would silently turn "abc" into 0).
    [[nodiscard]] long long num(const std::string& k, long long def, long long lo = LLONG_MIN,
                                long long hi = LLONG_MAX) const {
        auto it = kv_.find(k);
        if (it == kv_.end()) return def;
        const char* s = it->second.c_str();
        char* end = nullptr;
        errno = 0;
        const long long v = std::strtoll(s, &end, 10);
        if (end == s || *end != '\0' || errno == ERANGE) {
            std::fprintf(stderr, "--%s: expected an integer, got '%s'\n", k.c_str(), s);
            std::exit(2);
        }
        if (v < lo || v > hi) {
            std::fprintf(stderr, "--%s: %lld is out of range [%lld, %lld]\n", k.c_str(), v, lo, hi);
            std::exit(2);
        }
        return v;
    }

private:
    std::map<std::string, std::string> kv_;
};

}  // namespace tickforge::cli
