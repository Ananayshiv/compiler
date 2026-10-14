#include "codescout.hpp"

// Assertions must remain active in Release builds used by CI.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
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

int main() {
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
