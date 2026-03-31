add_library_unity(
  duckdb_operator_match_recognize
  OBJECT
  lexer.cpp
  parser.cpp
  nfa.cpp
  mr_nfa.cpp
  physical_match_recognize.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_operator_match_recognize>
    PARENT_SCOPE)