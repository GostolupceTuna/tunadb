#ifndef PARSER_HPP
#define PARSER_HPP

#include "lexer.hpp"  

enum class NodeType {
    VAR, 
    CONCAT, 
    ALT, 
    STAR, 
    PLUS, 
    OPTIONAL
};

struct Node {
    NodeType type;
    char value;

    Node* left;
    Node* right;

    Node(NodeType type, char value = 0);
    ~Node();
};

struct Parser {
    Lexer &lexer;
    Token lookahead;
    bool has_lookahead;

    Parser(Lexer &lexer);

    Token peek();
    Token consume();

    Node* parse_pattern();
    Node* parse_branch();
    Node* parse_piece();
    Node* parse_atom();
};

#endif