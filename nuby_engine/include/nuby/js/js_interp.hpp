#pragma once

// ============================================================================
// NUBY JS — Intérprete REAL de un subconjunto de JavaScript.
//
// El motor anterior NO era un intérprete: partía el código por ';' y hacía
// comparaciones de texto exactas ("console.log(", "getElementById(").
// Esto lo reemplaza por un intérprete genuino con las 4 fases clásicas:
//
//   1. LEXER:    convierte el texto en tokens reales
//   2. PARSER:   descenso recursivo → AST real
//   3. RUNTIME:  valores, entornos con scope, closures
//   4. EVALUADOR: recorre el AST ejecutando de verdad
//
// Subconjunto soportado (todo real, nada simulado):
//   • var/let/const, asignación, números, strings, booleanos, undefined
//   • + - * / % (y concatenación con +), == != < > <= >=, && || !
//   • if/else, while, for(;;), funciones con return y closures
//   • console.log(...)
//   • document.getElementById("id") → handle real del elemento
//   • elem.innerHTML = "<b>..</b>"  → parsea el fragmento y MUTA el DOM
//   • elem.textContent (get/set)
//   • elem.onclick = function(){...} → se dispara con clicks reales
//   • atributos onclick="..." de HTML → se ejecutan al hacer click
//
// Lo que NO soporta todavía (honesto): objetos literales, arrays, clases,
// prototypes, try/catch, async, y el 99% del DOM API estándar. Las webs
// con JS pesado no funcionarán: no lo ocultamos.
// ============================================================================

#include "../html/document.hpp"
#include "../html/element.hpp"
#include "../html/parser.hpp"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <cmath>

namespace nuby::js {

// ---------------------------------------------------------------------------
// 1. LEXER
// ---------------------------------------------------------------------------
enum class TokKind { NUM, STR, IDENT, KEYWORD, PUNCT, END };

struct Token {
    TokKind kind;
    std::string text;
    int line;
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r') { ++pos_; continue; }
            if (c == '\n') { ++line_; ++pos_; continue; }
            if (c == '/' && peek(1) == '/') { while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_; continue; }
            if (c == '/' && peek(1) == '*') {
                pos_ += 2;
                while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
                    if (src_[pos_] == '\n') ++line_;
                    ++pos_;
                }
                pos_ += 2; continue;
            }
            if (std::isdigit((unsigned char)c) || (c == '.' && std::isdigit((unsigned char)peek(1)))) {
                out.push_back(number()); continue;
            }
            if (c == '"' || c == '\'') { out.push_back(string_tok(c)); continue; }
            if (std::isalpha((unsigned char)c) || c == '_' || c == '$') {
                out.push_back(ident()); continue;
            }
            out.push_back(punct());
        }
        out.push_back({TokKind::END, "", line_});
        return out;
    }

private:
    char peek(size_t off) const { return pos_ + off < src_.size() ? src_[pos_ + off] : '\0'; }

    Token number() {
        size_t start = pos_;
        while (pos_ < src_.size() && (std::isdigit((unsigned char)src_[pos_]) || src_[pos_] == '.')) ++pos_;
        return {TokKind::NUM, src_.substr(start, pos_ - start), line_};
    }
    Token string_tok(char quote) {
        size_t start_line = line_;
        ++pos_;
        std::string s;
        while (pos_ < src_.size() && src_[pos_] != quote) {
            if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                ++pos_;
                char e = src_[pos_];
                if (e == 'n') s += '\n'; else if (e == 't') s += '\t';
                else if (e == 'r') s += '\r'; else s += e;
                ++pos_;
            } else {
                if (src_[pos_] == '\n') ++line_;
                s += src_[pos_++];
            }
        }
        if (pos_ < src_.size()) ++pos_;
        return {TokKind::STR, s, (int)start_line};
    }
    Token ident() {
        size_t start = pos_;
        while (pos_ < src_.size() && (std::isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_' || src_[pos_] == '$')) ++pos_;
        std::string w = src_.substr(start, pos_ - start);
        static const std::vector<std::string> kws = {
            "var","let","const","function","return","if","else","while","for",
            "true","false","undefined","null","break","continue"
        };
        for (auto& k : kws) if (w == k) return {TokKind::KEYWORD, w, line_};
        return {TokKind::IDENT, w, line_};
    }
    Token punct() {
        static const std::vector<std::string> two = {"==","!=","<=",">=","&&","||","+=","-=","++","--"};
        for (auto& t : two) {
            if (pos_ + 1 < src_.size() && src_.substr(pos_, 2) == t) {
                pos_ += 2; return {TokKind::PUNCT, t, line_};
            }
        }
        char c = src_[pos_++];
        return {TokKind::PUNCT, std::string(1, c), line_};
    }

    const std::string& src_;
    size_t pos_{0};
    size_t line_{1};
};

// ---------------------------------------------------------------------------
// 2. AST
// ---------------------------------------------------------------------------
enum class NK {
    NUM, STR, BOOL, UNDEF, IDENT,
    BINARY, UNARY, ASSIGN, UPDATE,       // UPDATE = ++/--
    VARDECL, BLOCK, IF, WHILE, FOR,
    FUNCDECL, CALL, MEMBER, RETURN,
    EXPR_STMT, SEQUENCE, BREAK, CONTINUE
};

struct Node {
    NK kind;
    int line{0};
    std::string str;                       // literal / nombre / operador
    double num{0};
    bool boolean{false};
    std::vector<std::shared_ptr<Node>> kids;
    std::vector<std::string> params;       // FUNCDECL

    static std::shared_ptr<Node> make(NK k, int line = 0) {
        auto n = std::make_shared<Node>(); n->kind = k; n->line = line; return n;
    }
};

// ---------------------------------------------------------------------------
// 3. PARSER (descenso recursivo)
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::shared_ptr<Node> parse_program() {
        auto prog = Node::make(NK::SEQUENCE);
        while (!at_end()) prog->kids.push_back(statement());
        return prog;
    }

private:
    const Token& cur() const { return toks_[pos_]; }
    const Token& prev() const { return toks_[pos_ - 1]; }
    bool at_end() const { return toks_[pos_].kind == TokKind::END; }
    const Token& advance() { if (!at_end()) ++pos_; return prev(); }
    bool check(const std::string& t) const { return !at_end() && toks_[pos_].text == t; }
    bool match(const std::string& t) { if (check(t)) { advance(); return true; } return false; }
    bool check_kind(TokKind k) const { return toks_[pos_].kind == k; }

    [[noreturn]] void error(const std::string& msg) {
        throw std::runtime_error("Nuby JS, linea " + std::to_string(cur().line) + ": " + msg);
    }
    const Token& expect(const std::string& t) {
        if (!match(t)) error("se esperaba '" + t + "' y vino '" + cur().text + "'");
        return prev();
    }

    std::shared_ptr<Node> statement() {
        if (check("var") || check("let") || check("const")) return var_decl();
        if (check("function")) return func_decl();
        if (check("if")) return if_stmt();
        if (check("while")) return while_stmt();
        if (check("for")) return for_stmt();
        if (check("return")) {
            int line = advance().line;
            auto n = Node::make(NK::RETURN, line);
            if (!check(";") && !check("}") && !at_end()) n->kids.push_back(expression());
            match(";");
            return n;
        }
        if (check("break")) { int l = advance().line; match(";"); return Node::make(NK::BREAK, l); }
        if (check("continue")) { int l = advance().line; match(";"); return Node::make(NK::CONTINUE, l); }
        if (check("{")) return block();
        auto e = expression();
        match(";");
        auto n = Node::make(NK::EXPR_STMT, e->line);
        n->kids.push_back(e);
        return n;
    }

    std::shared_ptr<Node> block() {
        int line = expect("{").line;
        auto n = Node::make(NK::BLOCK, line);
        while (!check("}") && !at_end()) n->kids.push_back(statement());
        expect("}");
        return n;
    }

    std::shared_ptr<Node> var_decl() {
        int line = advance().line; // var/let/const
        auto n = Node::make(NK::VARDECL, line);
        do {
            if (!check_kind(TokKind::IDENT)) error("se esperaba un nombre de variable");
            auto name = advance();
            auto decl = Node::make(NK::VARDECL, name.line);
            decl->str = name.text;
            if (match("=")) decl->kids.push_back(expression());
            n->kids.push_back(decl);
        } while (match(","));
        match(";");
        return n;
    }

    std::shared_ptr<Node> func_decl() {
        int line = expect("function").line;
        if (!check_kind(TokKind::IDENT)) error("se esperaba nombre de funcion");
        auto n = Node::make(NK::FUNCDECL, line);
        n->str = advance().text;
        parse_params_body(n);
        return n;
    }

    // function ( a, b ) { cuerpo }  — usado también por expresiones lambda
    void parse_params_body(std::shared_ptr<Node>& fn) {
        expect("(");
        while (!check(")") && !at_end()) {
            if (!check_kind(TokKind::IDENT)) error("parametro invalido");
            fn->params.push_back(advance().text);
            if (!match(",")) break;
        }
        expect(")");
        fn->kids.push_back(block()); // cuerpo
    }

    std::shared_ptr<Node> if_stmt() {
        int line = expect("if").line;
        auto n = Node::make(NK::IF, line);
        expect("("); n->kids.push_back(expression()); expect(")");
        n->kids.push_back(statement());
        if (match("else")) n->kids.push_back(statement());
        return n;
    }

    std::shared_ptr<Node> while_stmt() {
        int line = expect("while").line;
        auto n = Node::make(NK::WHILE, line);
        expect("("); n->kids.push_back(expression()); expect(")");
        n->kids.push_back(statement());
        return n;
    }

    std::shared_ptr<Node> for_stmt() {
        int line = expect("for").line;
        auto n = Node::make(NK::FOR, line);
        expect("(");
        if (!check(";")) { // init
            if (check("var") || check("let") || check("const")) n->kids.push_back(var_decl());
            else {
                auto init = Node::make(NK::EXPR_STMT, cur().line);
                init->kids.push_back(expression());
                match(";"); n->kids.push_back(init);
            }
        } else { match(";"); n->kids.push_back(nullptr); }
        if (!check(";")) n->kids.push_back(expression()); else n->kids.push_back(nullptr);
        expect(";");
        if (!check(")")) n->kids.push_back(expression()); else n->kids.push_back(nullptr);
        expect(")");
        n->kids.push_back(statement());
        return n;
    }

    std::shared_ptr<Node> expression() { return assignment(); }

    std::shared_ptr<Node> assignment() {
        auto left = or_expr();
        if (match("=")) {
            int line = prev().line;
            auto n = Node::make(NK::ASSIGN, line);
            n->kids.push_back(left);
            n->kids.push_back(assignment()); // asociativo a derecha
            return n;
        }
        if (match("+=") || match("-=")) {
            std::string op = prev().text;
            int line = prev().line;
            auto rhs = assignment();
            auto bin = Node::make(NK::BINARY, line);
            bin->str = (op == "+=") ? "+" : "-";
            bin->kids = {left, rhs};
            auto n = Node::make(NK::ASSIGN, line);
            n->kids = {left, bin};
            return n;
        }
        return left;
    }

    std::shared_ptr<Node> or_expr() {
        auto n = and_expr();
        while (check("||")) {
            int line = advance().line;
            auto b = Node::make(NK::BINARY, line); b->str = "||";
            b->kids = {n, and_expr()}; n = b;
        }
        return n;
    }
    std::shared_ptr<Node> and_expr() {
        auto n = equality();
        while (check("&&")) {
            int line = advance().line;
            auto b = Node::make(NK::BINARY, line); b->str = "&&";
            b->kids = {n, equality()}; n = b;
        }
        return n;
    }
    std::shared_ptr<Node> equality() {
        auto n = comparison();
        while (check("==") || check("!=")) {
            int line = advance().line;
            auto b = Node::make(NK::BINARY, line); b->str = prev().text;
            b->kids = {n, comparison()}; n = b;
        }
        return n;
    }
    std::shared_ptr<Node> comparison() {
        auto n = term();
        while (check("<") || check(">") || check("<=") || check(">=")) {
            int line = advance().line;
            auto b = Node::make(NK::BINARY, line); b->str = prev().text;
            b->kids = {n, term()}; n = b;
        }
        return n;
    }
    std::shared_ptr<Node> term() {
        auto n = factor();
        while (check("+") || check("-")) {
            int line = advance().line;
            auto b = Node::make(NK::BINARY, line); b->str = prev().text;
            b->kids = {n, factor()}; n = b;
        }
        return n;
    }
    std::shared_ptr<Node> factor() {
        auto n = unary();
        while (check("*") || check("/") || check("%")) {
            int line = advance().line;
            auto b = Node::make(NK::BINARY, line); b->str = prev().text;
            b->kids = {n, unary()}; n = b;
        }
        return n;
    }
    std::shared_ptr<Node> unary() {
        if (check("!") || check("-")) {
            int line = advance().line;
            auto n = Node::make(NK::UNARY, line);
            n->str = prev().text;
            n->kids.push_back(unary());
            return n;
        }
        return postfix();
    }
    std::shared_ptr<Node> postfix() {
        auto n = call_member();
        if (check("++") || check("--")) {
            int line = advance().line;
            auto u = Node::make(NK::UPDATE, line);
            u->str = prev().text;
            u->kids.push_back(n);
            return u;
        }
        return n;
    }

    std::shared_ptr<Node> call_member() {
        auto n = primary();
        for (;;) {
            if (match(".")) {
                if (!check_kind(TokKind::IDENT)) error("se esperaba propiedad tras '.'");
                auto m = Node::make(NK::MEMBER, n->line);
                m->str = advance().text;
                m->kids.push_back(n);
                n = m;
            } else if (check("(")) {
                int line = advance().line;
                auto c = Node::make(NK::CALL, line);
                c->kids.push_back(n);
                while (!check(")") && !at_end()) {
                    c->kids.push_back(expression());
                    if (!match(",")) break;
                }
                expect(")");
                n = c;
            } else break;
        }
        return n;
    }

    std::shared_ptr<Node> primary() {
        if (check_kind(TokKind::NUM)) {
            auto t = advance();
            auto n = Node::make(NK::NUM, t.line);
            try { n->num = std::stod(t.text); } catch (...) { n->num = 0; }
            return n;
        }
        if (check_kind(TokKind::STR)) {
            auto t = advance();
            auto n = Node::make(NK::STR, t.line);
            n->str = t.text;
            return n;
        }
        if (match("true")) { auto n = Node::make(NK::BOOL, prev().line); n->boolean = true; return n; }
        if (match("false")) { auto n = Node::make(NK::BOOL, prev().line); return n; }
        if (match("undefined") || match("null")) return Node::make(NK::UNDEF, prev().line);
        if (match("function")) { // lambda
            auto n = Node::make(NK::FUNCDECL, prev().line);
            if (check_kind(TokKind::IDENT)) n->str = advance().text; // nombre opcional
            parse_params_body(n);
            return n;
        }
        if (match("(")) {
            auto n = expression();
            expect(")");
            return n;
        }
        if (check_kind(TokKind::IDENT)) {
            auto t = advance();
            auto n = Node::make(NK::IDENT, t.line);
            n->str = t.text;
            return n;
        }
        error("expresion inesperada junto a '" + cur().text + "'");
    }

    std::vector<Token> toks_;
    size_t pos_{0};
};

// ---------------------------------------------------------------------------
// 4. RUNTIME: valores, entornos, funciones
// ---------------------------------------------------------------------------
struct Value;
struct Function {
    std::string name;
    std::vector<std::string> params;
    std::shared_ptr<Node> body;
    std::shared_ptr<struct Env> closure;
};

struct Value {
    enum Kind { UNDEFINED, BOOL, NUMBER, STRING, ELEMENT, FUNCTION } kind{UNDEFINED};
    bool b{false};
    double n{0};
    std::string s;
    std::shared_ptr<html::Element> elem;
    std::shared_ptr<Function> fn;

    static Value undefined() { return {}; }
    static Value boolean(bool v) { Value x; x.kind = BOOL; x.b = v; return x; }
    static Value number(double v) { Value x; x.kind = NUMBER; x.n = v; return x; }
    static Value string(std::string v) { Value x; x.kind = STRING; x.s = std::move(v); return x; }
    static Value element(std::shared_ptr<html::Element> e) { Value x; x.kind = ELEMENT; x.elem = std::move(e); return x; }
    static Value function(std::shared_ptr<Function> f) { Value x; x.kind = FUNCTION; x.fn = std::move(f); return x; }

    bool truthy() const {
        switch (kind) {
            case UNDEFINED: return false;
            case BOOL: return b;
            case NUMBER: return n != 0;
            case STRING: return !s.empty();
            default: return true;
        }
    }
    std::string to_str() const {
        switch (kind) {
            case UNDEFINED: return "undefined";
            case BOOL: return b ? "true" : "false";
            case NUMBER: {
                if (n == (long long)n && std::fabs(n) < 1e15) return std::to_string((long long)n);
                std::ostringstream ss; ss << n; return ss.str();
            }
            case STRING: return s;
            case ELEMENT: return elem ? "[Elemento <" + elem->get_tag_name() + ">]" : "null";
            case FUNCTION: return "[funcion " + (fn ? fn->name : "") + "]";
        }
        return "";
    }
    double to_num() const {
        switch (kind) {
            case NUMBER: return n;
            case BOOL: return b ? 1 : 0;
            case STRING: { try { return std::stod(s); } catch (...) { return 0; } }
            default: return 0;
        }
    }
};

struct Env {
    std::unordered_map<std::string, Value> vars;
    std::shared_ptr<Env> parent{nullptr};

    Value* lookup(const std::string& name) {
        auto it = vars.find(name);
        if (it != vars.end()) return &it->second;
        if (parent) return parent->lookup(name);
        return nullptr;
    }
    void declare(const std::string& name, Value v) { vars[name] = std::move(v); }
    bool assign(const std::string& name, Value v) {
        auto it = vars.find(name);
        if (it != vars.end()) { it->second = std::move(v); return true; }
        if (parent) return parent->assign(name, std::move(v));
        return false;
    }
};

// ---------------------------------------------------------------------------
// 5. EVALUADOR (tree-walk)
// ---------------------------------------------------------------------------
class Interpreter {
public:
    explicit Interpreter(std::shared_ptr<html::Document> doc) : doc_(std::move(doc)) {
        global_ = std::make_shared<Env>();
    }

    const std::vector<std::string>& logs() const { return logs_; }
    bool dom_mutated() const { return dom_mutated_; }
    void clear_mutation() { dom_mutated_ = false; }

    // Ejecuta un programa JS completo
    void run(const std::string& source) {
        Lexer lex(source);
        Parser parser(lex.run());
        auto ast = parser.parse_program();
        exec_seq(ast, global_);
    }

    // Dispara el handler onclick registrado para un elemento (click real)
    bool dispatch_click(const std::string& element_id) {
        // handlers registrados por JS: elem.onclick = fn
        auto it = click_handlers_.find(element_id);
        if (it != click_handlers_.end()) {
            call_function(it->second, {});
            return true;
        }
        // atributo onclick="codigo" del HTML
        if (doc_) {
            auto el = doc_->get_element_by_id(element_id);
            if (el && el->has_attribute("onclick")) {
                std::string code = el->get_attribute("onclick");
                // `this` y los globales se evalúan en el entorno global
                run(code);
                return true;
            }
        }
        return false;
    }

private:
    struct SigReturn { Value v; };
    struct SigBreak {};
    struct SigContinue {};

    std::shared_ptr<html::Document> doc_;
    std::shared_ptr<Env> global_;
    std::vector<std::string> logs_;
    bool dom_mutated_{false};
    std::unordered_map<std::string, std::shared_ptr<Function>> click_handlers_;
    int op_budget_{400000}; // freno antibucles infinitos (real y necesario)

    void tick(int line) {
        if (--op_budget_ <= 0)
            throw std::runtime_error("Nuby JS: bucle demasiado largo, ejecucion detenida (linea " + std::to_string(line) + ")");
    }

    void exec_seq(const std::shared_ptr<Node>& seq, std::shared_ptr<Env> env) {
        for (auto& st : seq->kids) exec_stmt(st, env);
    }

    void exec_stmt(const std::shared_ptr<Node>& n, std::shared_ptr<Env> env) {
        if (!n) return;
        tick(n->line);
        switch (n->kind) {
            case NK::VARDECL:
                for (auto& d : n->kids) {
                    Value v = d->kids.empty() ? Value::undefined() : eval(d->kids[0], env);
                    env->declare(d->str, v);
                }
                break;
            case NK::FUNCDECL: {
                auto f = std::make_shared<Function>();
                f->name = n->str; f->params = n->params; f->body = n->kids[0]; f->closure = env;
                if (!n->str.empty()) env->declare(n->str, Value::function(f));
                break;
            }
            case NK::BLOCK: {
                exec_seq(n, env); // var hoisting simplificado: scope de bloque compartido
                break;
            }
            case NK::IF:
                if (eval(n->kids[0], env).truthy()) exec_stmt(n->kids[1], env);
                else if (n->kids.size() > 2) exec_stmt(n->kids[2], env);
                break;
            case NK::WHILE:
                while (eval(n->kids[0], env).truthy()) {
                    tick(n->line);
                    try { exec_stmt(n->kids[1], env); }
                    catch (SigBreak&) { break; }
                    catch (SigContinue&) { continue; }
                }
                break;
            case NK::FOR:
                exec_stmt(n->kids[0], env);
                for (;;) {
                    tick(n->line);
                    if (n->kids[1] && !eval(n->kids[1], env).truthy()) break;
                    try { exec_stmt(n->kids[3], env); }
                    catch (SigBreak&) { break; }
                    catch (SigContinue&) {}
                    if (n->kids[2]) eval(n->kids[2], env);
                }
                break;
            case NK::RETURN: {
                Value v = n->kids.empty() ? Value::undefined() : eval(n->kids[0], env);
                throw SigReturn{v};
            }
            case NK::BREAK: throw SigBreak{};
            case NK::CONTINUE: throw SigContinue{};
            case NK::EXPR_STMT: eval(n->kids[0], env); break;
            default: eval(n, env); break;
        }
    }

    Value eval(const std::shared_ptr<Node>& n, std::shared_ptr<Env> env) {
        tick(n->line);
        switch (n->kind) {
            case NK::NUM: return Value::number(n->num);
            case NK::STR: return Value::string(n->str);
            case NK::BOOL: return Value::boolean(n->boolean);
            case NK::UNDEF: return Value::undefined();
            case NK::IDENT: {
                Value* v = env->lookup(n->str);
                if (!v) throw std::runtime_error("Nuby JS: variable no definida '" + n->str + "'");
                return *v;
            }
            case NK::UNARY: {
                Value v = eval(n->kids[0], env);
                if (n->str == "-") return Value::number(-v.to_num());
                return Value::boolean(!v.truthy());
            }
            case NK::BINARY: return eval_binary(n, env);
            case NK::UPDATE: return eval_update(n, env);
            case NK::ASSIGN: return eval_assign(n, env);
            case NK::FUNCDECL: {
                auto f = std::make_shared<Function>();
                f->name = n->str.empty() ? "(anonima)" : n->str;
                f->params = n->params; f->body = n->kids[0]; f->closure = env;
                return Value::function(f);
            }
            case NK::CALL: return eval_call(n, env);
            case NK::MEMBER: return eval_member(n, env);
            default:
                throw std::runtime_error("Nuby JS: nodo no evaluable en linea " + std::to_string(n->line));
        }
    }

    Value eval_binary(const std::shared_ptr<Node>& n, std::shared_ptr<Env> env) {
        if (n->str == "&&") {
            Value l = eval(n->kids[0], env);
            return l.truthy() ? eval(n->kids[1], env) : l;
        }
        if (n->str == "||") {
            Value l = eval(n->kids[0], env);
            return l.truthy() ? l : eval(n->kids[1], env);
        }
        Value l = eval(n->kids[0], env);
        Value r = eval(n->kids[1], env);
        const std::string& op = n->str;
        if (op == "+") {
            if (l.kind == Value::STRING || r.kind == Value::STRING)
                return Value::string(l.to_str() + r.to_str());
            return Value::number(l.to_num() + r.to_num());
        }
        if (op == "-") return Value::number(l.to_num() - r.to_num());
        if (op == "*") return Value::number(l.to_num() * r.to_num());
        if (op == "/") return Value::number(l.to_num() / r.to_num());
        if (op == "%") return Value::number(std::fmod(l.to_num(), r.to_num()));
        if (op == "==") return Value::boolean(equals(l, r));
        if (op == "!=") return Value::boolean(!equals(l, r));
        if (op == "<") return Value::boolean(l.to_num() < r.to_num());
        if (op == ">") return Value::boolean(l.to_num() > r.to_num());
        if (op == "<=") return Value::boolean(l.to_num() <= r.to_num());
        if (op == ">=") return Value::boolean(l.to_num() >= r.to_num());
        throw std::runtime_error("Nuby JS: operador desconocido " + op);
    }

    static bool equals(const Value& a, const Value& b) {
        if (a.kind == b.kind) {
            switch (a.kind) {
                case Value::UNDEFINED: return true;
                case Value::BOOL: return a.b == b.b;
                case Value::NUMBER: return a.n == b.n;
                case Value::STRING: return a.s == b.s;
                case Value::ELEMENT: return a.elem == b.elem;
                case Value::FUNCTION: return a.fn == b.fn;
            }
        }
        if ((a.kind == Value::NUMBER || a.kind == Value::STRING || a.kind == Value::BOOL) &&
            (b.kind == Value::NUMBER || b.kind == Value::STRING || b.kind == Value::BOOL))
            return a.to_num() == b.to_num() && a.to_str() == b.to_str();
        return false;
    }

    Value eval_update(const std::shared_ptr<Node>& n, std::shared_ptr<Env> env) {
        // target debe ser IDENT (limite honesto del subconjunto)
        auto& target = n->kids[0];
        if (target->kind != NK::IDENT)
            throw std::runtime_error("Nuby JS: ++/-- solo sobre variables simples");
        Value* cur = env->lookup(target->str);
        if (!cur) throw std::runtime_error("Nuby JS: variable no definida '" + target->str + "'");
        double old = cur->to_num();
        *cur = Value::number(n->str == "++" ? old + 1 : old - 1);
        return Value::number(old);
    }

    Value eval_assign(const std::shared_ptr<Node>& n, std::shared_ptr<Env> env) {
        auto& lhs = n->kids[0];
        if (lhs->kind == NK::IDENT) {
            Value v = eval(n->kids[1], env);
            if (!env->assign(lhs->str, v)) env->declare(lhs->str, v);
            return v;
        }
        if (lhs->kind == NK::MEMBER) return assign_member(lhs, n->kids[1], env);
        throw std::runtime_error("Nuby JS: asignacion invalida");
    }

    // obj.prop = valor   (innerHTML / textContent / onclick)
    Value assign_member(const std::shared_ptr<Node>& lhs, const std::shared_ptr<Node>& rhs_node,
                        std::shared_ptr<Env> env) {
        Value obj = eval(lhs->kids[0], env);
        Value v = eval(rhs_node, env);
        if (obj.kind != Value::ELEMENT || !obj.elem)
            throw std::runtime_error("Nuby JS: solo elementos del DOM tienen propiedades asignables");

        if (lhs->str == "innerHTML") {
            // REAL: parsea el fragmento HTML con el parser de Nuby y reemplaza hijos
            set_inner_html(obj.elem, v.to_str());
            dom_mutated_ = true;
            return v;
        }
        if (lhs->str == "textContent") {
            obj.elem->clear_children();
            obj.elem->append_child(std::make_shared<html::TextNode>(v.to_str()));
            dom_mutated_ = true;
            return v;
        }
        if (lhs->str == "onclick") {
            if (v.kind == Value::FUNCTION && obj.elem->has_attribute("id")) {
                click_handlers_[obj.elem->get_attribute("id")] = v.fn;
            }
            return v;
        }
        // cualquier otra → atributo HTML real
        obj.elem->set_attribute(lhs->str, v.to_str());
        dom_mutated_ = true;
        return v;
    }

    // innerHTML real: usa el parser HTML de Nuby sobre el fragmento
    static void set_inner_html(std::shared_ptr<html::Element> elem, const std::string& html_str) {
        html::HTMLParser frag_parser(html_str);
        auto frag_doc = frag_parser.parse();
        elem->clear_children();
        auto body = frag_doc->get_body();
        if (body) {
            for (auto& child : body->get_children()) elem->append_child(child);
        } else if (frag_doc->get_root_element()) {
            for (auto& child : frag_doc->get_root_element()->get_children()) elem->append_child(child);
        }
    }

    Value eval_member(const std::shared_ptr<Node>& n, std::shared_ptr<Env> env) {
        Value obj = eval(n->kids[0], env);

        // document es un builtin, no una variable
        if (n->kids[0]->kind == NK::IDENT && n->kids[0]->str == "document") {
            if (n->str == "getElementById") return Value::string("__builtin_getElementById");
            if (n->str == "title") return Value::string(doc_ ? doc_->get_title() : "");
            throw std::runtime_error("Nuby JS: document." + n->str + " no soportado en el subconjunto");
        }
        if (n->kids[0]->kind == NK::IDENT && n->kids[0]->str == "console") {
            if (n->str == "log") return Value::string("__builtin_console_log");
            throw std::runtime_error("Nuby JS: console." + n->str + " no soportado");
        }

        if (obj.kind == Value::ELEMENT && obj.elem) {
            if (n->str == "innerHTML" || n->str == "textContent")
                return Value::string(obj.elem->get_text_content());
            if (n->str == "id") return Value::string(obj.elem->get_id());
            if (n->str == "tagName") return Value::string(obj.elem->get_tag_name());
        }
        if (obj.kind == Value::STRING) {
            if (n->str == "length") return Value::number((double)obj.s.size());
        }
        if (obj.kind == Value::STRING && (obj.s == "__builtin_getElementById" || obj.s == "__builtin_console_log"))
            return obj; // se resuelve en CALL
        return Value::undefined();
    }

    Value eval_call(const std::shared_ptr<Node>& n, std::shared_ptr<Env> env) {
        auto& callee = n->kids[0];

        // Builtins representados como strings marcador (truco interno, transparente)
        if (callee->kind == NK::MEMBER && callee->kids[0]->kind == NK::IDENT) {
            const std::string& base = callee->kids[0]->str;
            if (base == "document" && callee->str == "getElementById") {
                Value arg = n->kids.size() > 1 ? eval(n->kids[1], env) : Value::undefined();
                auto el = doc_ ? doc_->get_element_by_id(arg.to_str()) : nullptr;
                if (!el) return Value::undefined();
                return Value::element(el);
            }
            if (base == "console" && callee->str == "log") {
                std::string line;
                for (size_t i = 1; i < n->kids.size(); ++i) {
                    if (i > 1) line += " ";
                    line += eval(n->kids[i], env).to_str();
                }
                logs_.push_back(line);
                return Value::undefined();
            }
        }

        Value fn_val = eval(callee, env);
        if (fn_val.kind != Value::FUNCTION || !fn_val.fn)
            throw std::runtime_error("Nuby JS: intento de llamar algo que no es funcion (linea " +
                                     std::to_string(n->line) + ")");

        std::vector<Value> args;
        for (size_t i = 1; i < n->kids.size(); ++i) args.push_back(eval(n->kids[i], env));
        return call_function(fn_val.fn, args);
    }

    Value call_function(std::shared_ptr<Function> fn, const std::vector<Value>& args) {
        auto local = std::make_shared<Env>();
        local->parent = fn->closure;
        for (size_t i = 0; i < fn->params.size(); ++i)
            local->declare(fn->params[i], i < args.size() ? args[i] : Value::undefined());
        try {
            exec_seq(fn->body, local);
        } catch (SigReturn& r) {
            return r.v;
        }
        return Value::undefined();
    }
};

} // namespace nuby::js
