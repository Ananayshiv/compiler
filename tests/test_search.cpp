#include "codescout.hpp"

// Assertions must remain active in Release builds used by CI.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace codescout;

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

void test_declared_symbol() {
    // Declarations across languages
    assert(declared_symbol("def parse_header(line):") == "parse_header");
    assert(declared_symbol("    async def fetch_all(self):") == "fetch_all");
    assert(declared_symbol("class EnumType(type):") == "EnumType");
    assert(declared_symbol("func ReadFull(r Reader, buf []byte) (n int, err error) {") == "ReadFull");
    assert(declared_symbol("func (s *Server) ListenAndServe() error {") == "ListenAndServe");
    assert(declared_symbol("func Map[K comparable, V any](m map[K]V) {") == "Map");
    assert(declared_symbol("type Reader interface {") == "Reader");
    assert(declared_symbol("pub(crate) fn build_index(root: &Path) -> Index {") == "build_index");
    assert(declared_symbol("pub struct Posting {") == "Posting");
    assert(declared_symbol("export default function renderPage(props) {") == "renderPage");
    assert(declared_symbol("const handleClick = (event) => {") == "handleClick");
    assert(declared_symbol("void parseHTTPHeader(const std::string& request) {") == "parseHTTPHeader");
    assert(declared_symbol("static inline int *alloc_buffer(size_t n);") == "alloc_buffer");
    assert(declared_symbol("std::vector<Result> Index::search(const std::string& q) const {") == "search");
    assert(declared_symbol("Index::Index(int n) {") == "Index");
    assert(declared_symbol("public static void main(String[] args) {") == "main");
    assert(declared_symbol("enum class Ranking {") == "Ranking");
    assert(declared_symbol("#define MAX_TOKENS 128") == "MAX_TOKENS");
    // Not declarations
    assert(declared_symbol("    return parse_header(line);").empty());
    assert(declared_symbol("if (ready) {").empty());
    assert(declared_symbol("result = compute(x)").empty());
    assert(declared_symbol("printf(\"%d\", x);").empty());
    assert(declared_symbol("obj.method(a, b);").empty());
    assert(declared_symbol("    go func() {").empty());
    assert(declared_symbol("// void commented(int x) {").empty());
    assert(declared_symbol("@decorator(arg)").empty());
    assert(declared_symbol("#include <vector>").empty());
    assert(declared_symbol("const limit = 10;").empty());
    assert(declared_symbol("else if (x) {").empty());
    assert(declared_symbol("").empty());
}

void test_code_aware_ranking() {
    Index idx;
    // Many call sites mention the words; only one line defines the symbol.
    idx.add("net/http/client.go", 10, "resp, err := readFull(conn, buf) // read full frame");
    idx.add("net/http/server.go", 20, "n, err := readFull(r, b)");
    idx.add("io/io.go", 30, "func ReadFull(r Reader, buf []byte) (n int, err error) {");
    idx.add("io/io_test.go", 40, "if _, err := ReadFull(r, buf); err != nil { // read full");
    const auto bm25 = idx.search("read full");  // legacy overload = plain BM25
    SearchOptions code;
    const auto aware = idx.search("read full", code);
    assert(aware.size() == bm25.size() && aware.size() == 4);
    assert(aware[0].document.path == "io/io.go");
    assert(aware[0].definition && aware[0].symbol == "ReadFull");
    assert(bm25[0].document.path != "io/io.go");  // BM25 alone favours the repetitive call site
    // Boost is multiplicative and bounded by (1 + boost) for a full name match.
    double plain = 0;
    for (const auto& r : bm25) if (r.document.path == "io/io.go") plain = r.score;
    assert(std::abs(aware[0].score - plain * (1.0 + code.definition_boost)) < 1e-9);
    SearchOptions off;
    off.definition_boost = 0;
    const auto unboosted = idx.search("read full", off);
    for (std::size_t i = 0; i < bm25.size(); ++i) {
        assert(unboosted[i].document.path == bm25[i].document.path);
        assert(std::abs(unboosted[i].score - bm25[i].score) < 1e-12);
    }
    // Path boost: same line text, the file whose path matches the query ranks first.
    Index paths;
    paths.add("util/misc.go", 1, "cache entries here");
    paths.add("cache/store.go", 1, "cache entries here");
    SearchOptions path;
    path.definition_boost = 0;
    path.path_boost = 1.0;
    assert(paths.search("cache entries", path)[0].document.path == "cache/store.go");
    // Path filter and limit still apply under code-aware ranking.
    SearchOptions limited;
    limited.limit = 1;
    limited.path_filter = "net/";
    const auto filtered = idx.search("read full", limited);
    assert(filtered.size() == 1 && filtered[0].document.path.rfind("net/", 0) == 0);
}

void test_json() {
    assert(json_escape("a\"b\\c\n\t") == "a\\\"b\\\\c\\n\\t");
    assert(json_escape(std::string(1, '\x01')) == "\\u0001");
    assert(json_escape("नमस्ते") == "नमस्ते");  // UTF-8 bytes pass through
    Result r;
    r.document = {"src/a.cpp", 3, "int \"x\";"};
    r.score = 1.5;
    const auto j = to_json(r);
    assert(j.find("\"path\":\"src/a.cpp\"") != std::string::npos);
    assert(j.find("\"text\":\"int \\\"x\\\";\"") != std::string::npos);
    assert(j.find("\"definition\":false") != std::string::npos);
}

void test_snapshots() {
    namespace fs = std::filesystem;
    Index idx;
    idx.add("a/parser.py", 1, "def parse_header(line):");
    idx.add("a/parser.py", 2, "    return parse_header_value(line)");
    idx.add("b/io.go", 7, "func ReadFull(r Reader) error {");
    const auto file = fs::temp_directory_path() / "codescout_v2.csix";
    idx.save(file.string());
    {   // header is the new format
        std::ifstream in(file, std::ios::binary);
        char magic[6];
        in.read(magic, 6);
        assert(std::string(magic, 6) == "CSIX02");
    }
    const Index loaded = Index::load(file.string());
    assert(loaded.documents() == idx.documents() && loaded.terms() == idx.terms());
    assert(loaded.definitions() == 2);
    for (const char* q : {"parse header", "read full", "line", "reader error"}) {
        const auto a = idx.search(q, SearchOptions{});
        const auto b = loaded.search(q, SearchOptions{});
        assert(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            assert(a[i].document.line == b[i].document.line && a[i].document.path == b[i].document.path);
            assert(std::abs(a[i].score - b[i].score) < 1e-12 && a[i].symbol == b[i].symbol);
        }
    }
    // Deterministic bytes for identical content.
    const auto again = fs::temp_directory_path() / "codescout_v2_again.csix";
    loaded.save(again.string());
    std::ifstream f1(file, std::ios::binary), f2(again, std::ios::binary);
    const std::string b1((std::istreambuf_iterator<char>(f1)), {}), b2((std::istreambuf_iterator<char>(f2)), {});
    assert(b1 == b2);
    // Truncated / tampered CSIX02 files are rejected.
    for (std::size_t cut : {b1.size() - 1, b1.size() / 2, std::size_t{10}}) {
        { std::ofstream out(again, std::ios::binary | std::ios::trunc); out.write(b1.data(), static_cast<std::streamsize>(cut)); }
        bool rejected = false;
        try { (void)Index::load(again.string()); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    { std::ofstream out(again, std::ios::binary | std::ios::trunc); out << b1 << 'x'; }
    bool trailing = false;
    try { (void)Index::load(again.string()); } catch (const std::runtime_error&) { trailing = true; }
    assert(trailing);
    // Legacy CSIX01 snapshots still load (postings rebuilt from text).
    {
        std::ofstream out(again, std::ios::binary | std::ios::trunc);
        auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) out.put(static_cast<char>((v >> (8 * i)) & 255)); };
        out.write("CSIX01", 6);
        u32(1);
        const std::string path = "x.go", text = "func ReadFull(r Reader) error {";
        u32(static_cast<std::uint32_t>(path.size())); out << path;
        u32(9);
        u32(static_cast<std::uint32_t>(text.size())); out << text;
    }
    const Index legacy = Index::load(again.string());
    assert(legacy.documents() == 1 && legacy.definitions() == 1);
    assert(legacy.search("read full", SearchOptions{})[0].symbol == "ReadFull");
    fs::remove(file);
    fs::remove(again);
}

int main() {
    test_declared_symbol();
    test_code_aware_ranking();
    test_json();
    test_snapshots();
    const auto t = tokenize("parseHTTPHeader foo_bar userID42");
    assert(has(t, "parsehttpheader") && has(t, "parse") && has(t, "http") && has(t, "header"));
    assert(has(t, "foo_bar") && has(t, "foo") && has(t, "bar"));
    assert(has(t, "user") && has(t, "id42"));
    assert(tokenize(" -+ ").empty());

    Index idx;
    idx.add("core/parser.cpp", 4, "Token parseHTTPHeader(token input)");
    idx.add("core/cache.cpp", 8, "void evictCacheEntry()");
    idx.add("core/parser.cpp", 15, "parse string token token token");
    assert(idx.documents() == 3);
    auto results = idx.search("parse token");
    assert(results.size() == 2);
    assert(results[0].document.line == 15); // higher token frequency
    assert(idx.search("parse cache").empty());
    assert(idx.search("parse cache", false).size() == 3);
    assert(idx.search("HTTP header").size() == 1);
    assert(idx.search("unknown").empty());
    assert(idx.search("token", true, 1).size() == 1);
    assert(idx.search("parse", true, 10, "cache").empty());

    const auto index_file = std::filesystem::temp_directory_path() / "codescout_test_index.csix";
    idx.save(index_file.string());
    Index loaded = Index::load(index_file.string());
    assert(loaded.documents() == idx.documents());
    assert(loaded.terms() == idx.terms());
    assert(loaded.search("parse token")[0].document.line == 15);
    std::filesystem::remove(index_file);

    const auto dir = std::filesystem::temp_directory_path() / "codescout_test_dir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "src");
    std::filesystem::create_directories(dir / "node_modules");
    { std::ofstream f(dir / "src" / "main.cpp"); f << "int computeResult = 42;\nreturn computeResult;\n"; }
    { std::ofstream f(dir / "node_modules" / "vendor.cpp"); f << "ignored token\n"; }
    { std::ofstream f(dir / "src" / "fake.bin"); f << "ignored token\n"; }
    Index from_folder;
    assert(from_folder.index_directory(dir.string()) == 2);
    assert(from_folder.search("compute result").size() == 2);
    assert(from_folder.search("ignored").empty());
    std::filesystem::remove_all(dir);

    const auto corrupt_file = std::filesystem::temp_directory_path() / "codescout_bad_index.csix";
    { std::ofstream f(corrupt_file, std::ios::binary); f << "bad"; }
    bool rejected = false;
    try { (void)Index::load(corrupt_file.string()); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    std::filesystem::remove(corrupt_file);

    std::cout << "All CodeScout tests passed\n";
}
