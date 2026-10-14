#include "codescout.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
void usage() {
    std::cout << "CodeScout — code-aware BM25 search for local repositories\n\n"
              << "  codescout index <directory> [-o index.csix]\n"
              << "  codescout search <query> [-i index.csix] [--or] [--limit N] [--path filter]\n"
              << "  codescout stats [-i index.csix]\n"
              << "  codescout bench\n";
}

std::string value_after(const std::vector<std::string>& args, const std::string& flag,
                        const std::string& default_value) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == flag) {
            if (i + 1 == args.size()) throw std::runtime_error("missing value for " + flag);
            return args[i + 1];
        }
    }
    return default_value;
}

bool contains(const std::vector<std::string>& args, const std::string& flag) {
    for (const auto& a : args) if (a == flag) return true;
    return false;
}

std::size_t parse_limit(const std::string& s) {
    const auto count = std::stoull(s);
    if (count > 10000) throw std::runtime_error("limit must be <= 10000");
    return static_cast<std::size_t>(count);
}

// Brute-force reference search with EXACTLY the same matching, BM25 ranking,
// and top-k output as the indexed engine. Unlike an inverted index, this
// recomputes line tokens/term frequencies on every query.
std::vector<codescout::Result> brute_search(const std::vector<codescout::Document>& docs,
                                            const std::string& query, std::size_t limit) {
    auto terms = codescout::tokenize(query);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    if (terms.empty()) return {};
    struct Candidate {
        std::size_t length;
        std::unordered_map<std::string, std::size_t> tf;
    };
    std::vector<Candidate> parsed;
    std::unordered_map<std::string, std::size_t> df;
    double total_length = 0;
    parsed.reserve(docs.size());
    for (const auto& doc : docs) {
        const auto words = codescout::tokenize(doc.text);
        Candidate candidate{words.size(), {}};
        total_length += words.size();
        for (const auto& word : words) ++candidate.tf[word];
        for (const auto& term : terms) if (candidate.tf.count(term) > 0) ++df[term];
        parsed.push_back(std::move(candidate));
    }
    const double n = static_cast<double>(docs.size());
    const double average = docs.empty() ? 1.0 : total_length / n;
    constexpr double k1 = 1.2;
    constexpr double b = 0.75;
    std::vector<codescout::Result> result;
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const auto& c = parsed[i];
        bool matched = true;
        double score = 0;
        for (const auto& term : terms) {
            const auto it = c.tf.find(term);
            if (it == c.tf.end()) { matched = false; break; }
            const auto tf = static_cast<double>(it->second);
            const auto idf = std::log1p((n - static_cast<double>(df[term]) + 0.5) /
                                        (static_cast<double>(df[term]) + 0.5));
            const auto norm = k1 * (1 - b + b * c.length / average);
            score += idf * tf * (k1 + 1.0) / (tf + norm);
        }
        if (matched) result.push_back({docs[i], score});
    }
    std::sort(result.begin(), result.end(), [](const auto& x, const auto& y) {
        if (x.score != y.score) return x.score > y.score;
        if (x.document.path != y.document.path) return x.document.path < y.document.path;
        return x.document.line < y.document.line;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}

void bench() {
    using clock = std::chrono::steady_clock;
    codescout::Index index;
    const std::vector<std::string> components = {
        "cache eviction", "parser token", "database query", "http handler", "thread pool", "neural vector"
    };
    for (int i = 0; i < 20000; ++i) {
        const auto& c = components[static_cast<std::size_t>(i) % components.size()];
        index.add("src/module_" + std::to_string(i / 100) + ".cpp", i % 100 + 1,
                  "auto update" + std::to_string(i) + " = process " + c + " request value;");
    }
    const auto t0 = clock::now();
    std::size_t indexed_checksum = 0;
    for (int iteration = 0; iteration < 8; ++iteration)
        for (const auto& q : components)
            for (const auto& result : index.search(q, true, 10))
                indexed_checksum += result.document.line;
    const auto t1 = clock::now();
    std::size_t brute_checksum = 0;
    for (int iteration = 0; iteration < 8; ++iteration)
        for (const auto& q : components)
            for (const auto& result : brute_search(index.all_documents(), q, 10))
                brute_checksum += result.document.line;
    const auto t2 = clock::now();
    if (indexed_checksum != brute_checksum)
        throw std::runtime_error("benchmark outputs disagree");
    const double indexed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double brute_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::cout << "Documents: " << index.documents() << " | queries: 48 | top-k: 10\n"
              << std::fixed << std::setprecision(2)
              << "Indexed top-10 BM25: " << indexed_ms << " ms\n"
              << "Brute-force top-10 BM25: " << brute_ms << " ms\n"
              << "Speedup (query-only, synthetic corpus): " << brute_ms / indexed_ms << "x\n"
              << "Both paths use identical tokenization, AND matching, BM25 ranking and top-k.\n"
              << "Results checksum: " << indexed_checksum << " (matched)\n";
}

}

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.empty() || args[0] == "--help" || args[0] == "-h") { usage(); return 0; }
        const std::string cmd = args[0];
        if (cmd == "index") {
            if (args.size() < 2) throw std::runtime_error("index requires a directory");
            codescout::Index index;
            const auto start = std::chrono::steady_clock::now();
            index.index_directory(args[1]);
            const std::string output = value_after(args, "-o", "index.csix");
            index.save(output);
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cout << "Indexed " << index.documents() << " lines (" << index.terms()
                      << " terms) in " << std::fixed << std::setprecision(2) << elapsed << "s\n"
                      << "Saved: " << output << '\n';
        } else if (cmd == "search") {
            if (args.size() < 2) throw std::runtime_error("search requires a quoted query");
            const auto index = codescout::Index::load(value_after(args, "-i", "index.csix"));
            const bool all = !contains(args, "--or");
            const auto limit = parse_limit(value_after(args, "--limit", "10"));
            const auto results = index.search(args[1], all, limit, value_after(args, "--path", ""));
            std::cout << results.size() << " results for " << std::quoted(args[1]) << "\n";
            for (const auto& r : results) {
                std::cout << std::fixed << std::setprecision(3) << '[' << r.score << "] "
                          << r.document.path << ':' << r.document.line << " | " << r.document.text << '\n';
            }
        } else if (cmd == "stats") {
            const auto index = codescout::Index::load(value_after(args, "-i", "index.csix"));
            std::cout << "Indexed lines: " << index.documents() << "\nUnique terms: " << index.terms() << '\n';
        } else if (cmd == "bench") {
            bench();
        } else { usage(); return 2; }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
