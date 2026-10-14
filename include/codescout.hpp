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
};

// Produces lowercase whole identifiers and camelCase/snake_case constituent words.
std::vector<std::string> tokenize(const std::string& text);

class Index {
public:
    void add(std::string path, std::uint32_t line, std::string text);
    std::size_t index_directory(const std::string& root);
    std::vector<Result> search(const std::string& query, bool match_all = true,
                               std::size_t limit = 10, const std::string& path_filter = "") const;
    void save(const std::string& filename) const;
    static Index load(const std::string& filename);

    std::size_t documents() const { return documents_.size(); }
    std::size_t terms() const { return postings_.size(); }
    const std::vector<Document>& all_documents() const { return documents_; }

private:
    struct Posting {
        std::uint32_t document;
        std::uint32_t frequency;
    };
    std::vector<Document> documents_;
    std::unordered_map<std::string, std::vector<Posting>> postings_;
    std::vector<std::uint32_t> lengths_;
    std::uint64_t total_length_ = 0;
};

} // namespace codescout
