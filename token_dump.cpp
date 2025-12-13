#include "third_party/libpg_query/include/postgres_parser.hpp"
#include <iostream>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: token_dump \"SQL\"\n";
        return 1;
    }
    duckdb::PostgresParser::SetPreserveIdentifierCase(false);
    auto tokens = duckdb::PostgresParser::Tokenize(argv[1]);
    for (auto &tok : tokens) {
        std::cout << (int)tok.type << " : [" << tok.start << "]" << std::endl;
    }
    return 0;
}
