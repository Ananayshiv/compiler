#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace codescout {

struct Document {
    std::string path;
    std::uint32_t line = 0;
    std::string text;
};

struct Result {
    Document document;
    double score = 0.0;
    std::uint32_t document_id = 0;  // stable within one index build
    bool definition = false;   // line declares a function/class/type/...
    std::string symbol;        // declared identifier, if `definition`
};

enum class Ranking {
    BM25,       // plain line-level BM25 (reference semantics)
    CodeAware   // BM25 x (1 + definition boost + path boost)
};

struct SearchOptions {
    bool match_all = true;
    std::size_t limit = 10;
    std::string path_filter;
    Ranking ranking = Ranking::CodeAware;
    // Multiplicative boosts. Defaults were tuned on the Python 3.12 standard
    // library and evaluated on the held-out Go standard library
    // (see bench/RESULTS.md); they are heuristics, not learned weights.
    double definition_boost = 2.0;
    double path_boost = 0.0;   // off by default: did not help on the tuning corpus
};

// Produces lowercase whole identifiers and camelCase/snake_case constituent words.
std::vector<std::string> tokenize(const std::string& text);

// Returns the identifier a source line declares ("" if it is not a declaration).
// Language-agnostic heuristic covering def/class/fn/func/type/struct/... and
// C-family function signatures. Calls, control flow and statements return "".
std::string declared_symbol(const std::string& line);

class Index {
public:
    void add(std::string path, std::uint32_t line, std::string text);
    std::size_t index_directory(const std::string& root);
    std::vector<Result> search(const std::string& query, const SearchOptions& options) const;
    // Backwards-compatible overload: plain BM25 semantics.
    std::vector<Result> search(const std::string& query, bool match_all = true,
                               std::size_t limit = 10, const std::string& path_filter = "") const;
    void save(const std::string& filename) const;
    static Index load(const std::string& filename);

    std::size_t documents() const { return documents_.size(); }
    std::size_t terms() const { return postings_.size(); }
    std::size_t definitions() const { return symbols_.size(); }
    const std::vector<Document>& all_documents() const { return documents_; }

private:
    struct Posting {
        std::uint32_t document;
        std::uint32_t frequency;
    };
    std::vector<Document> documents_;
    std::unordered_map<std::string, std::vector<Posting>> postings_;
    std::vector<std::uint32_t> lengths_;
    std::unordered_map<std::uint32_t, std::string> symbols_;  // only declaration lines
    std::uint64_t total_length_ = 0;
};

// Minimal JSON string escaping for CLI/agent output.
std::string json_escape(const std::string& text);
std::string to_json(const Result& result);

} // namespace codescout
