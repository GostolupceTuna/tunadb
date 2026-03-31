#include "lexer.hpp"

Lexer::Lexer(const std::string& s) 
    : input(s), pos(0) {}

Token Lexer::next_token() {
    // skip whitespace
    while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\n' || input[pos] == '\r')) {
        pos++;
    }
        
    if (pos >= input.size()) 
        return {TokenType::END, 0};

    char c = input[pos++];
    switch(c) {
        case '(': return {TokenType::LPAREN, '('};
        case ')': return {TokenType::RPAREN, ')'};
        case '{': return {TokenType::LBRACE, '{'};
        case '}': return {TokenType::RBRACE, '}'};
        case '*': return {TokenType::STAR, '*'};
        case '+': return {TokenType::PLUS, '+'};
        case '?': return {TokenType::OPTIONAL, '?'};
        case '|': return {TokenType::OR, '|'};
        default:  return {TokenType::VAR, c};  
    }
}
