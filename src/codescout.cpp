#include "codescout.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
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

namespace {

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$'; }
bool ident_char(char c) { return alnum(c) || c == '_' || c == '$'; }

std::size_t skip_space(const std::string& s, std::size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

// Reads an identifier at s[i] (after optional whitespace); advances i.
std::string read_ident(const std::string& s, std::size_t& i) {
    i = skip_space(s, i);
    if (i >= s.size() || !ident_start(s[i])) return "";
    const auto start = i;
    while (i < s.size() && ident_char(s[i])) ++i;
    return s.substr(start, i - start);
}

// Skips a balanced (...) group starting at s[i] (after whitespace), if present.
void skip_parens(const std::string& s, std::size_t& i) {
    i = skip_space(s, i);
    if (i >= s.size() || s[i] != '(') return;
    int depth = 0;
    for (; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) { ++i; return; }
    }
}

const std::unordered_set<std::string>& modifiers() {
    static const std::unordered_set<std::string> m = {
        "pub", "export", "default", "async", "static", "public", "private", "protected",
        "internal", "inline", "virtual", "extern", "final", "abstract", "override",
        "unsafe", "open", "sealed", "partial", "constexpr", "explicit", "friend", "data"
    };
    return m;
}

const std::unordered_set<std::string>& declaration_keywords() {
    static const std::unordered_set<std::string> k = {
        "def", "class", "fn", "func", "function", "struct", "enum", "interface", "trait",
        "type", "typedef", "module", "impl", "union", "namespace", "record", "object",
        "macro_rules", "fun", "protocol", "extension", "using"
    };
    return k;
}

const std::unordered_set<std::string>& not_declarations() {
    static const std::unordered_set<std::string> k = {
        "if", "else", "for", "while", "switch", "return", "case", "do", "catch", "sizeof",
        "new", "delete", "throw", "await", "yield", "typeof", "assert", "elif", "with",
        "print", "go", "defer", "not", "and", "or", "in", "is", "lambda", "raise", "del"
    };
    return k;
}

} // namespace

std::string declared_symbol(const std::string& line) {
    std::size_t i = skip_space(line, 0);
    if (i >= line.size()) return "";
    const std::string rest = line.substr(i);
    if (rest.rfind("//", 0) == 0 || rest.rfind("/*", 0) == 0 || rest[0] == '*' || rest[0] == '@')
        return "";
    if (rest.rfind("#define", 0) == 0) {
        std::size_t j = i + 7;
        return read_ident(line, j);
    }
    if (rest[0] == '#') return "";

    std::size_t j = i;
    std::string word = read_ident(line, j);
    while (!word.empty() && modifiers().count(word)) {
        skip_parens(line, j);                       // pub(crate)
        word = read_ident(line, j);
    }
    if (word.empty()) return "";
    if (declaration_keywords().count(word)) {
        if (word == "using" || word == "impl") return "";  // aliases/impl blocks are not definitions of a new name
        skip_parens(line, j);                       // Go receiver: func (s *T) Name(
        std::string name = read_ident(line, j);
        if (name == "class" || name == "struct") name = read_ident(line, j);  // enum class X
        return not_declarations().count(name) ? "" : name;
    }
    if (word == "const" || word == "let" || word == "var") {  // JS/TS function values
        std::string name = read_ident(line, j);
        const auto eq = line.find('=', j);
        if (!name.empty() && eq != std::string::npos &&
            (line.find("=>", eq) != std::string::npos || line.find("function", eq) != std::string::npos))
            return name;
        return "";
    }
    if (not_declarations().count(word)) return "";

    // C-family signature: "<type tokens> name(" with no assignment before '('.
    const auto paren = line.find('(', i);
    if (paren == std::string::npos) return "";
    const std::string prefix = line.substr(i, paren - i);
    if (prefix.find('=') != std::string::npos) return "";
    std::size_t end = prefix.find_last_not_of(" \t");
    if (end == std::string::npos || !ident_char(prefix[end])) return "";
    std::size_t start = end;
    while (start > 0 && ident_char(prefix[start - 1])) --start;
    const std::string name = prefix.substr(start, end - start + 1);
    const std::string before = prefix.substr(0, start);
    if (name.empty() || !ident_start(name[0]) || not_declarations().count(name)) return "";
    // Everything before the name must look like a type: identifiers, spaces, *, &, ::, <>, commas.
    bool has_type = false;
    for (char c : before) {
        if (ident_char(c)) has_type = true;
        else if (!(c == ' ' || c == '\t' || c == '*' || c == '&' || c == ':' || c == '<' ||
                   c == '>' || c == ',' || c == '~')) return "";
    }
    const bool qualified = before.size() >= 2 && before.compare(before.size() - 2, 2, "::") == 0;
    if (!has_type) return "";
    if (!qualified && before.find_first_of(" \t*&") == std::string::npos) return "";
    return name;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string to_json(const Result& r) {
    std::ostringstream out;
    out.precision(6);
    out << "{\"path\":\"" << json_escape(r.document.path) << "\",\"line\":" << r.document.line
        << ",\"score\":" << r.score << ",\"definition\":" << (r.definition ? "true" : "false")
        << ",\"symbol\":\"" << json_escape(r.symbol) << "\",\"text\":\""
        << json_escape(r.document.text) << "\"}";
    return out.str();
}

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
    auto symbol = declared_symbol(text);
    if (!symbol.empty()) symbols_.emplace(id, std::move(symbol));
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
    SearchOptions options;
    options.match_all = match_all;
    options.limit = limit;
    options.path_filter = path_filter;
    options.ranking = Ranking::BM25;
    return search(query, options);
}

std::vector<Result> Index::search(const std::string& query, const SearchOptions& options) const {
    const bool match_all = options.match_all;
    const std::size_t limit = options.limit;
    const std::string& path_filter = options.path_filter;
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

    const bool code_aware = options.ranking == Ranking::CodeAware;
    const std::unordered_set<std::string> query_terms(tokens.begin(), tokens.end());
    auto overlap = [&](const std::string& text) {
        std::size_t hit = 0;
        std::unordered_set<std::string> seen;
        for (const auto& t : tokenize(text))
            if (query_terms.count(t) && seen.insert(t).second) ++hit;
        return static_cast<double>(hit) / static_cast<double>(query_terms.size());
    };
    // How precisely a declared name matches the query: Jaccard overlap of the
    // name's components with the query terms, so `specialsIter` beats
    // `newSpecialsIter` for "specials iter". Typing the whole identifier counts
    // as an exact match.
    auto name_match = [&](const std::string& symbol) {
        auto parts = tokenize(symbol);
        if (parts.empty()) return 0.0;
        if (query_terms.count(parts.front())) return 1.0;      // whole identifier typed
        if (parts.size() > 1) parts.erase(parts.begin());      // keep components only
        std::unordered_set<std::string> name(parts.begin(), parts.end());
        std::size_t hit = 0;
        for (const auto& t : name) hit += query_terms.count(t);
        const auto uni = name.size() + query_terms.size() - hit;
        return uni ? static_cast<double>(hit) / static_cast<double>(uni) : 0.0;
    };
    std::unordered_map<std::string, double> path_overlap;  // cache: many lines share a path

    std::vector<Result> result;
    for (const auto& [id, candidate] : scores) {
        if (match_all && candidate.matches != tokens.size()) continue;
        const auto& document = documents_[id];
        if (!path_filter.empty() && document.path.find(path_filter) == std::string::npos) continue;
        Result r;
        r.document = document;
        r.document_id = id;
        r.score = candidate.score;
        if (code_aware) {
            double boost = 1.0;
            const auto symbol = symbols_.find(id);
            if (symbol != symbols_.end()) boost += options.definition_boost * name_match(symbol->second);
            auto cached = path_overlap.find(document.path);
            if (cached == path_overlap.end())
                cached = path_overlap.emplace(document.path, overlap(document.path)).first;
            boost += options.path_boost * cached->second;
            r.score *= boost;
        }
        result.push_back(std::move(r));
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
    // Attach declaration metadata only to the returned top-k lines.
    for (auto& r : result) {
        const auto symbol = symbols_.find(r.document_id);
        if (symbol != symbols_.end()) { r.definition = true; r.symbol = symbol->second; }
    }
    return result;
}

void Index::save(const std::string& filename) const {
    // CSIX02: documents + the complete inverted index, so loading is a
    // sequential read instead of re-tokenizing every line (CSIX01 behaviour).
    const std::string tmp = filename + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write: " + filename);
        out.write("CSIX02", 6);
        write_u32(out, static_cast<std::uint32_t>(documents_.size()));
        for (std::size_t id = 0; id < documents_.size(); ++id) {
            const auto& doc = documents_[id];
            write_u32(out, static_cast<std::uint32_t>(doc.path.size()));
            out.write(doc.path.data(), static_cast<std::streamsize>(doc.path.size()));
            write_u32(out, doc.line);
            write_u32(out, static_cast<std::uint32_t>(doc.text.size()));
            out.write(doc.text.data(), static_cast<std::streamsize>(doc.text.size()));
            write_u32(out, lengths_[id]);
        }
        write_u32(out, static_cast<std::uint32_t>(symbols_.size()));
        std::vector<std::uint32_t> symbol_ids;
        symbol_ids.reserve(symbols_.size());
        for (const auto& kv : symbols_) symbol_ids.push_back(kv.first);
        std::sort(symbol_ids.begin(), symbol_ids.end());  // deterministic file bytes
        for (const auto id : symbol_ids) {
            const auto& name = symbols_.at(id);
            write_u32(out, id);
            write_u32(out, static_cast<std::uint32_t>(name.size()));
            out.write(name.data(), static_cast<std::streamsize>(name.size()));
        }
        std::vector<const std::string*> terms;
        terms.reserve(postings_.size());
        for (const auto& kv : postings_) terms.push_back(&kv.first);
        std::sort(terms.begin(), terms.end(), [](auto a, auto b) { return *a < *b; });
        write_u32(out, static_cast<std::uint32_t>(terms.size()));
        for (const auto* term : terms) {
            const auto& list = postings_.at(*term);
            write_u32(out, static_cast<std::uint32_t>(term->size()));
            out.write(term->data(), static_cast<std::streamsize>(term->size()));
            write_u32(out, static_cast<std::uint32_t>(list.size()));
            for (const auto& posting : list) {
                write_u32(out, posting.document);
                write_u32(out, posting.frequency);
            }
        }
        out.flush();
        if (!out) throw std::runtime_error("failed writing index");
    }
    std::error_code ec;
    fs::rename(tmp, filename, ec);  // atomic replace: a crash never leaves a half-written index
    if (ec) throw std::runtime_error("cannot finalize index: " + ec.message());
}

Index Index::load(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read: " + filename);
    char magic[6];
    if (!in.read(magic, 6)) throw std::runtime_error("invalid index header");
    const std::string version(magic, 6);
    if (version != "CSIX01" && version != "CSIX02") throw std::runtime_error("invalid index header");
    const auto count = read_u32(in);
    if (count > 10000000) throw std::runtime_error("index exceeds safety limit");
    Index index;
    if (version == "CSIX01") {  // legacy: rebuild postings from text
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
    index.documents_.reserve(count);
    index.lengths_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto path = read_string(in, 4096);
        const auto line = read_u32(in);
        auto text = read_string(in, 16384);
        const auto length = read_u32(in);
        if (length == 0 || length > 16384) throw std::runtime_error("invalid document length in index");
        index.documents_.push_back({std::move(path), line, std::move(text)});
        index.lengths_.push_back(length);
        index.total_length_ += length;
    }
    const auto symbol_count = read_u32(in);
    if (symbol_count > count) throw std::runtime_error("invalid symbol table");
    for (std::uint32_t i = 0; i < symbol_count; ++i) {
        const auto id = read_u32(in);
        if (id >= count) throw std::runtime_error("symbol refers to missing line");
        index.symbols_.emplace(id, read_string(in, 4096));
    }
    const auto term_count = read_u32(in);
    if (term_count > 50000000) throw std::runtime_error("index exceeds safety limit");
    index.postings_.reserve(term_count);
    for (std::uint32_t t = 0; t < term_count; ++t) {
        auto term = read_string(in, 16384);
        const auto size = read_u32(in);
        if (size == 0 || size > count) throw std::runtime_error("invalid posting list");
        std::vector<Posting> list(size);
        for (auto& posting : list) {
            posting.document = read_u32(in);
            posting.frequency = read_u32(in);
            if (posting.document >= count || posting.frequency == 0)
                throw std::runtime_error("invalid posting");
        }
        if (!index.postings_.emplace(std::move(term), std::move(list)).second)
            throw std::runtime_error("duplicate term in index");
    }
    if (in.peek() != EOF) throw std::runtime_error("trailing data in index");
    return index;
}

} // namespace codescout
