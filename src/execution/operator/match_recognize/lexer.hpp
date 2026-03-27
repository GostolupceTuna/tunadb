#ifndef LEXER_HPP
#define LEXER_HPP

#include <string>

enum class TokenType {
    VAR,       
    LPAREN,     
    RPAREN,  
    LBRACE, 
    RBRACE, 
    STAR,     
    PLUS,      
    OPTIONAL, 
    OR,                     
    END       
};

struct Token {
    TokenType type;
    char value; 
};

struct Lexer {
    const std::string& input;
    size_t pos;

    Lexer(const std::string& s);
    Token next_token();
};

#endif