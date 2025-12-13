#include "third_party/libpg_query/include/postgres_parser.hpp"
#include <iostream>
#
int main() {
    std::string query = "select * from tuna match_recognize(order by bla  measures bla AS bla one row per match after match skip past last row  within pattern define);";
    duckdb::PostgresParser parser;
    parser.Parse(query);
    if (parser.success) {
        std::cout << "Parse success\n";
    } else {
        std::cout << "Parse failed at position " << parser.error_location
                  << " with message: " << parser.error_message << "\n";
    }
    return 0;
}

