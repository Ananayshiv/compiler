#include "codescout.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace codescout {
namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool upper(char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; }
bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

void split_identifier(const std::string& word, std::vector<std::string>& result) {
    if (word.empty()) return;
    const std::string original = [&] {
        std::string s;
        for (char c : word) s.push_back(lower(c));
        return s;
    }();
    result.push_back(original);

    // Split underscores, camelCase, and acronym boundaries like HTTPServer.
    std::string segment;
    auto flush = [&]() {
        if (!segment.empty() && segment != original) result.push_back(segment);
        segment.clear();
    };
    for (std::size_t i = 0; i < word.size(); ++i) {
        const char c = word[i];
        if (c == '_') { flush(); continue; }
        if (i > 0 && word[i - 1] != '_' && !segment.empty()) {
            const char prev = word[i - 1];
            const bool boundary = (upper(c) && (std::islower(static_cast<unsigned char>(prev)) || digit(prev))) ||
                (upper(c) && upper(prev) && i + 1 < word.size() &&
                 std::islower(static_cast<unsigned char>(word[i + 1])));
            if (boundary) flush();
        }
        segment.push_back(lower(c));
    }
    flush();
}

bool supported(const fs::path& path) {
    static const std::unordered_set<std::string> ext = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".rs", ".py",
        ".js", ".jsx", ".ts", ".tsx", ".java", ".go", ".rb", ".cs",
        ".swift", ".kt", ".sql", ".md", ".txt", ".toml", ".json", ".yaml", ".yml"
    };
    std::string e = path.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), lower);
    return ext.count(e) != 0;
}

bool excluded_directory(const std::string& name) {
    static const std::unordered_set<std::string> ignored = {
        ".git", ".hg", ".svn", "node_modules", "target", "build", "dist", ".venv",
        "venv", "__pycache__", ".next", ".idea", ".vscode", "vendor"
    };
    return ignored.count(name) != 0;
}

void write_u32(std::ostream& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.put(static_cast<char>((value >> (8 * i)) & 255));
    if (!out) throw std::runtime_error("failed writing index");
}

std::uint32_t read_u32(std::istream& in) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int c = in.get();
        if (c == EOF) throw std::runtime_error("truncated or invalid index");
        result |= static_cast<std::uint32_t>(c) << (8 * i);
    }
    return result;
}

std::string read_string(std::istream& in, std::uint32_t limit) {
    auto length = read_u32(in);
    if (length > limit) throw std::runtime_error("index has oversized field");
    std::string value(length, '\0');
    if (!in.read(value.data(), length)) throw std::runtime_error("truncated index");
    return value;
}

} // namespace

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> result;
    std::string word;
    auto flush = [&] { split_identifier(word, result); word.clear(); };
    for (char c : text) {
        if (alnum(c) || c == '_') word.push_back(c);
        else flush();
    }
    flush();
    return result;
}

void Index::add(std::string path, std::uint32_t line, std::string text) {
    if (documents_.size() >= std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("too many indexed lines");
    const auto tokens = tokenize(text);
    if (tokens.empty()) return;
    const auto id = static_cast<std::uint32_t>(documents_.size());
    std::unordered_map<std::string, std::uint32_t> counts;
    for (const auto& token : tokens) ++counts[token];
    for (const auto& [token, frequency] : counts) postings_[token].push_back({id, frequency});
    lengths_.push_back(static_cast<std::uint32_t>(tokens.size()));
    total_length_ += tokens.size();
    documents_.push_back({std::move(path), line, std::move(text)});
}

std::size_t Index::index_directory(const std::string& root) {
    if (!fs::is_directory(root)) throw std::runtime_error("not a directory: " + root);
    std::vector<fs::path> paths;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_directory(ec)) {
            if (excluded_directory(it->path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || it->is_symlink(ec) || !supported(it->path())) continue;
        const auto size = it->file_size(ec);
        if (ec || size > 1024 * 1024) { ec.clear(); continue; }
        paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end()); // reproducible document IDs and tie-breaking
    const auto before = documents_.size();
    for (const auto& path : paths) {
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;
        char prefix[4096];
        input.read(prefix, sizeof(prefix));
        const auto count = input.gcount();
        if (std::find(prefix, prefix + count, '\0') != prefix + count) continue;
        input.clear();
        input.seekg(0);
        std::string line;
        std::uint32_t line_no = 0;
        const std::string rel = fs::relative(path, root).generic_string();
        while (std::getline(input, line)) {
            ++line_no;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > 16384) continue;
            if (line.find_first_not_of(" \t") != std::string::npos) add(rel, line_no, line);
        }
    }
    return documents_.size() - before;
}

std::vector<Result> Index::search(const std::string& query, bool match_all,
                                 std::size_t limit, const std::string& path_filter) const {
    auto tokens = tokenize(query);
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    if (tokens.empty() || limit == 0) return {};

    struct Candidate { double score = 0; std::size_t matches = 0; };
    std::unordered_map<std::uint32_t, Candidate> scores;
    const double avg = documents_.empty() ? 1.0 : static_cast<double>(total_length_) / documents_.size();
    const double n = static_cast<double>(documents_.size());
    constexpr double k1 = 1.2;
    constexpr double b = 0.75;
    for (const auto& token : tokens) {
        auto found = postings_.find(token);
        if (found == postings_.end()) {
            if (match_all) return {};
            continue;
        }
        const auto& posting = found->second;
        const double df = static_cast<double>(posting.size());
        const double idf = std::log1p((n - df + 0.5) / (df + 0.5));
        for (const auto& p : posting) {
            const double tf = static_cast<double>(p.frequency);
            const double norm = k1 * (1.0 - b + b * lengths_[p.document] / avg);
            auto& candidate = scores[p.document];
            candidate.score += idf * (tf * (k1 + 1.0)) / (tf + norm);
            ++candidate.matches;
        }
    }

    std::vector<Result> result;
    for (const auto& [id, candidate] : scores) {
        if (match_all && candidate.matches != tokens.size()) continue;
        const auto& document = documents_[id];
        if (!path_filter.empty() && document.path.find(path_filter) == std::string::npos) continue;
        result.push_back({document, candidate.score});
    }
    auto cmp = [](const Result& a, const Result& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.document.path != b.document.path) return a.document.path < b.document.path;
        return a.document.line < b.document.line;
    };
    if (result.size() > limit) {
        std::partial_sort(result.begin(), result.begin() + limit, result.end(), cmp);
        result.resize(limit);
    } else {
        std::sort(result.begin(), result.end(), cmp);
    }
    return result;
}

void Index::save(const std::string& filename) const {
    std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write: " + filename);
    out.write("CSIX01", 6);
    write_u32(out, static_cast<std::uint32_t>(documents_.size()));
    for (const auto& doc : documents_) {
        write_u32(out, static_cast<std::uint32_t>(doc.path.size()));
        out.write(doc.path.data(), static_cast<std::streamsize>(doc.path.size()));
        write_u32(out, doc.line);
        write_u32(out, static_cast<std::uint32_t>(doc.text.size()));
        out.write(doc.text.data(), static_cast<std::streamsize>(doc.text.size()));
        if (!out) throw std::runtime_error("failed writing index");
    }
}

Index Index::load(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read: " + filename);
    char magic[6];
    if (!in.read(magic, 6) || std::string(magic, 6) != "CSIX01")
        throw std::runtime_error("invalid index header");
    const auto count = read_u32(in);
    if (count > 10000000) throw std::runtime_error("index exceeds safety limit");
    Index index;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto path = read_string(in, 4096);
        const auto line = read_u32(in);
        auto text = read_string(in, 16384);
        index.add(std::move(path), line, std::move(text));
    }
    if (in.peek() != EOF) throw std::runtime_error("trailing data in index");
    if (index.documents() != count) throw std::runtime_error("invalid empty document in index");
    return index;
}

} // namespace codescout
