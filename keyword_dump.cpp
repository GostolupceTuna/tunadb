#include "third_party/libpg_query/include/parser/gram.hpp"
#include "third_party/libpg_query/include/common/keywords.hpp"
#include "third_party/libpg_query/include/parser/kwlist.hpp"
#include <iostream>

int main() {
    const char *word = "last";
    const auto *kw = duckdb_libpgquery::ScanKeywordLookup(
        word,
        duckdb_libpgquery::ScanKeywords,
        duckdb_libpgquery::NumScanKeywords
    );
    if (!kw) {
        std::cout << "No keyword entry for '" << word << "'\n";
    } else {
        std::cout << "Keyword '" << word << "' maps to token value "
                  << kw->value << " and category " << kw->category << "\n";
    }
    return 0;
}
