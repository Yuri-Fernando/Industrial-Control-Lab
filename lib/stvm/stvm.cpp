#include "stvm.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace icl::st {
// ---------------------------------------------------------------- AST
enum class Op { LIT, VAR, NEG, NOT, OR, XOR, AND, EQ, NE, LT, GT, LE, GE, ADD, SUB, MUL, DIV, MOD, CALL };
struct Expr {
    Op op = Op::LIT;
    Type type = Type::BOOL;  // statically inferred result type
    Value lit;
    std::size_t var = 0;
    std::string fn;
    std::vector<std::unique_ptr<Expr>> args;
    int line = 0;
};
enum class SKind { ASSIGN, IF, CASE, RETURN };
struct CaseArm {
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    std::vector<std::unique_ptr<Stmt>> body;
};
struct Stmt {
    SKind kind = SKind::ASSIGN;
    int line = 0;
    std::size_t target = 0;
    std::unique_ptr<Expr> expr;  // ASSIGN value / CASE selector
    std::vector<std::pair<std::unique_ptr<Expr>, std::vector<std::unique_ptr<Stmt>>>> branches;  // IF/ELSIF
    std::vector<CaseArm> arms;
    std::vector<std::unique_ptr<Stmt>> else_body;
};
Program::Program() = default;
Program::~Program() = default;

static bool is_bool(Type t) { return t == Type::BOOL; }
static bool is_num(Type t) { return t != Type::BOOL; }
static bool is_real_t(Type t) { return t == Type::REAL || t == Type::LREAL; }
static const char* type_name(Type t) {
    switch (t) { case Type::BOOL: return "BOOL"; case Type::INT: return "INT"; case Type::DINT: return "DINT";
                 case Type::REAL: return "REAL"; default: return "LREAL"; }
}

Value coerce(const Value& v, Type target) {
    Value out;
    out.type = target;
    switch (target) {
        case Type::BOOL: out.b = v.type == Type::BOOL ? v.b : (v.is_real() ? v.r != 0.0 : v.i != 0); break;
        case Type::INT: out.i = static_cast<std::int16_t>(v.is_real() ? static_cast<std::int64_t>(v.r) : v.i); break;
        case Type::DINT: out.i = static_cast<std::int32_t>(v.is_real() ? static_cast<std::int64_t>(v.r) : v.i); break;
        case Type::REAL: out.r = static_cast<double>(static_cast<float>(v.as_real())); break;
        case Type::LREAL: out.r = v.as_real(); break;
    }
    return out;
}

// ---------------------------------------------------------------- lexer
enum class Tok { IDENT, INT, REAL, SYM, END };
struct Token { Tok kind; std::string text; int line; std::int64_t ival = 0; double rval = 0.0; };

static std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    int line = 1;
    std::size_t p = 0;
    while (p < s.size()) {
        const char c = s[p];
        if (c == '\n') { ++line; ++p; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++p; continue; }
        if (c == '(' && p + 1 < s.size() && s[p + 1] == '*') {
            const int start = line;
            p += 2;
            while (p + 1 < s.size() && !(s[p] == '*' && s[p + 1] == ')')) { if (s[p] == '\n') ++line; ++p; }
            if (p + 1 >= s.size()) throw StError("unterminated comment", start);
            p += 2; continue;
        }
        if (c == '/' && p + 1 < s.size() && s[p + 1] == '/') { while (p < s.size() && s[p] != '\n') ++p; continue; }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t q = p;
            while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '_')) ++q;
            std::string id = s.substr(p, q - p);
            std::transform(id.begin(), id.end(), id.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            out.push_back({Tok::IDENT, id, line});
            p = q; continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string digits;
            std::size_t q = p;
            bool real = false;
            auto take_digits = [&] { while (q < s.size() && (std::isdigit(static_cast<unsigned char>(s[q])) || s[q] == '_')) { if (s[q] != '_') digits += s[q]; ++q; } };
            take_digits();
            if (q + 1 < s.size() && s[q] == '.' && std::isdigit(static_cast<unsigned char>(s[q + 1]))) { real = true; digits += '.'; ++q; take_digits(); }
            if (q < s.size() && (s[q] == 'e' || s[q] == 'E')) {
                real = true; digits += 'e'; ++q;
                if (q < s.size() && (s[q] == '+' || s[q] == '-')) digits += s[q++];
                take_digits();
            }
            Token t{real ? Tok::REAL : Tok::INT, digits, line};
            if (real) t.rval = std::stod(digits); else t.ival = std::stoll(digits);
            out.push_back(t);
            p = q; continue;
        }
        static const char* two[] = {":=", "<>", "<=", ">=", ".."};
        bool matched = false;
        for (const char* sym : two) {
            if (s.compare(p, 2, sym) == 0) { out.push_back({Tok::SYM, sym, line}); p += 2; matched = true; break; }
        }
        if (matched) continue;
        if (std::string("=<>+-*/();:,&").find(c) != std::string::npos) { out.push_back({Tok::SYM, std::string(1, c), line}); ++p; continue; }
        throw StError(std::string("unexpected character '") + c + "'", line);
    }
    out.push_back({Tok::END, "", line});
    return out;
}

// ---------------------------------------------------------------- parser + type checker
class Parser {
public:
    Parser(std::vector<Token> toks, Program& prog) : t_(std::move(toks)), prog_(prog) {}
    void parse_pou() {
        std::string end_kw;
        if (accept_kw("FUNCTION_BLOCK")) end_kw = "END_FUNCTION_BLOCK";
        else if (accept_kw("PROGRAM")) end_kw = "END_PROGRAM";
        else fail("expected FUNCTION_BLOCK or PROGRAM");
        prog_.pou_name = expect_ident();
        while (peek_kw("VAR_INPUT") || peek_kw("VAR_OUTPUT") || peek_kw("VAR")) parse_var_block();
        prog_.body = parse_stmts({end_kw});
        expect_kw(end_kw);
        if (cur().kind != Tok::END) fail("unexpected text after " + end_kw);
    }
private:
    const Token& cur() const { return t_[p_]; }
    [[noreturn]] void fail(const std::string& msg) const { throw StError(msg + " (near '" + cur().text + "')", cur().line); }
    bool peek_kw(const std::string& kw) const { return cur().kind == Tok::IDENT && cur().text == kw; }
    bool accept_kw(const std::string& kw) { if (peek_kw(kw)) { ++p_; return true; } return false; }
    void expect_kw(const std::string& kw) { if (!accept_kw(kw)) fail("expected " + kw); }
    bool peek_sym(const std::string& s) const { return cur().kind == Tok::SYM && cur().text == s; }
    bool accept_sym(const std::string& s) { if (peek_sym(s)) { ++p_; return true; } return false; }
    void expect_sym(const std::string& s) { if (!accept_sym(s)) fail("expected '" + s + "'"); }
    std::string expect_ident() {
        if (cur().kind != Tok::IDENT) fail("expected identifier");
        return t_[p_++].text;
    }

    void parse_var_block() {
        VarKind kind = VarKind::LOCAL;
        if (accept_kw("VAR_INPUT")) kind = VarKind::INPUT;
        else if (accept_kw("VAR_OUTPUT")) kind = VarKind::OUTPUT;
        else { expect_kw("VAR"); if (accept_kw("CONSTANT")) kind = VarKind::CONSTANT; }
        while (!accept_kw("END_VAR")) {
            std::vector<std::string> names{expect_ident()};
            while (accept_sym(",")) names.push_back(expect_ident());
            expect_sym(":");
            const std::string tn = expect_ident();
            Type type;
            if (tn == "BOOL") type = Type::BOOL;
            else if (tn == "INT") type = Type::INT;
            else if (tn == "DINT") type = Type::DINT;
            else if (tn == "REAL") type = Type::REAL;
            else if (tn == "LREAL") type = Type::LREAL;
            else fail("unsupported type " + tn);
            Value init = coerce(Value::integer(0), type);
            if (accept_sym(":=")) {
                auto e = parse_expr();
                if (e->op != Op::LIT && !(e->op == Op::NEG && e->args[0]->op == Op::LIT)) fail("initializer must be a literal");
                check_assignable(type, e->type, e->line);
                init = coerce(e->op == Op::LIT ? e->lit : negate(e->args[0]->lit), type);
            } else if (kind == VarKind::CONSTANT) fail("constant requires an initial value");
            expect_sym(";");
            for (const auto& n : names) {
                if (prog_.index.count(n)) fail("duplicate variable " + n);
                prog_.index[n] = prog_.vars.size();
                prog_.vars.push_back({n, type, kind, init});
            }
        }
    }

    static Value negate(const Value& v) { return v.is_real() ? Value::real(-v.r, v.type) : Value::integer(-v.i, v.type); }

    void check_assignable(Type target, Type value, int line) const {
        if (is_bool(target) != is_bool(value))
            throw StError(std::string("type mismatch: cannot assign ") + type_name(value) + " to " + type_name(target), line);
        if (!is_real_t(target) && is_real_t(value))
            throw StError(std::string("implicit narrowing ") + type_name(value) + " to " + type_name(target) + " (use explicit conversion)", line);
    }

    std::vector<std::unique_ptr<Stmt>> parse_stmts(const std::vector<std::string>& terminators) {
        std::vector<std::unique_ptr<Stmt>> out;
        auto at_end = [&] {
            if (cur().kind == Tok::END) return true;
            for (const auto& kw : terminators) if (peek_kw(kw)) return true;
            return false;
        };
        while (!at_end()) {
            if (accept_sym(";")) continue;  // empty statement
            out.push_back(parse_stmt());
        }
        return out;
    }

    std::unique_ptr<Stmt> parse_stmt() {
        auto s = std::make_unique<Stmt>();
        s->line = cur().line;
        if (accept_kw("IF")) {
            s->kind = SKind::IF;
            do {
                auto cond = parse_expr();
                require_bool(*cond, "IF condition");
                expect_kw("THEN");
                auto body = parse_stmts({"ELSIF", "ELSE", "END_IF"});
                s->branches.emplace_back(std::move(cond), std::move(body));
            } while (accept_kw("ELSIF"));
            if (accept_kw("ELSE")) s->else_body = parse_stmts({"END_IF"});
            expect_kw("END_IF");
            expect_sym(";");
        } else if (accept_kw("CASE")) {
            s->kind = SKind::CASE;
            s->expr = parse_expr();
            if (!is_num(s->expr->type) || is_real_t(s->expr->type)) throw StError("CASE selector must be an integer", s->line);
            expect_kw("OF");
            while (!peek_kw("ELSE") && !peek_kw("END_CASE")) {
                CaseArm arm;
                do {
                    const auto lo = parse_case_label();
                    const auto hi = accept_sym("..") ? parse_case_label() : lo;
                    arm.ranges.emplace_back(lo, hi);
                } while (accept_sym(","));
                expect_sym(":");
                arm.body = parse_stmts_case();
                s->arms.push_back(std::move(arm));
            }
            if (accept_kw("ELSE")) s->else_body = parse_stmts({"END_CASE"});
            expect_kw("END_CASE");
            expect_sym(";");
        } else if (accept_kw("RETURN")) {
            s->kind = SKind::RETURN;
            expect_sym(";");
        } else {
            s->kind = SKind::ASSIGN;
            const auto name = expect_ident();
            const auto it = prog_.index.find(name);
            if (it == prog_.index.end()) throw StError("undeclared variable " + name, s->line);
            const auto& decl = prog_.vars[it->second];
            if (decl.kind == VarKind::CONSTANT) throw StError("cannot assign constant " + name, s->line);
            if (decl.kind == VarKind::INPUT) throw StError("cannot assign VAR_INPUT " + name + " inside the block", s->line);
            s->target = it->second;
            expect_sym(":=");
            s->expr = parse_expr();
            check_assignable(decl.type, s->expr->type, s->line);
            expect_sym(";");
        }
        return s;
    }

    // Statements inside a CASE arm end at the next label (INT ..), ELSE or END_CASE.
    std::vector<std::unique_ptr<Stmt>> parse_stmts_case() {
        std::vector<std::unique_ptr<Stmt>> out;
        while (!peek_kw("ELSE") && !peek_kw("END_CASE") && cur().kind != Tok::END) {
            if (cur().kind == Tok::INT || peek_sym("-")) break;
            if (cur().kind == Tok::IDENT && is_constant_label()) break;
            if (accept_sym(";")) continue;
            out.push_back(parse_stmt());
        }
        return out;
    }
    bool is_constant_label() const {
        const auto it = prog_.index.find(cur().text);
        if (it == prog_.index.end() || prog_.vars[it->second].kind != VarKind::CONSTANT) return false;
        const auto& next = t_[p_ + 1];
        return next.kind == Tok::SYM && (next.text == ":" || next.text == "," || next.text == "..");
    }
    std::int64_t parse_case_label() {
        bool neg = accept_sym("-");
        std::int64_t v;
        if (cur().kind == Tok::INT) v = t_[p_++].ival;
        else if (cur().kind == Tok::IDENT && is_constant_label()) {
            const auto& d = prog_.vars[prog_.index.at(t_[p_++].text)];
            if (!d.initial.is_int()) fail("CASE label constant must be an integer");
            v = d.initial.i;
        } else fail("expected integer CASE label");
        return neg ? -v : v;
    }

    void require_bool(const Expr& e, const char* what) const {
        if (!is_bool(e.type)) throw StError(std::string(what) + " must be BOOL", e.line);
    }
    static Type arith_type(const Expr& a, const Expr& b) {
        if (!is_real_t(a.type) && !is_real_t(b.type)) return (a.type == Type::INT && b.type == Type::INT) ? Type::INT : Type::DINT;
        if (a.type == Type::LREAL || b.type == Type::LREAL) return Type::LREAL;
        return Type::REAL;
    }
    std::unique_ptr<Expr> binary(Op op, std::unique_ptr<Expr> a, std::unique_ptr<Expr> b, int line) {
        auto e = std::make_unique<Expr>();
        e->op = op; e->line = line;
        switch (op) {
            case Op::OR: case Op::XOR: case Op::AND:
                if (!is_bool(a->type) || !is_bool(b->type)) throw StError("logical operator needs BOOL operands (bitwise integer ops unsupported)", line);
                e->type = Type::BOOL; break;
            case Op::EQ: case Op::NE:
                if (is_bool(a->type) != is_bool(b->type)) throw StError("comparison between BOOL and number", line);
                e->type = Type::BOOL; break;
            case Op::LT: case Op::GT: case Op::LE: case Op::GE:
                if (!is_num(a->type) || !is_num(b->type)) throw StError("ordering comparison needs numeric operands", line);
                e->type = Type::BOOL; break;
            case Op::MOD:
                if (!is_num(a->type) || !is_num(b->type) || is_real_t(a->type) || is_real_t(b->type)) throw StError("MOD needs integer operands", line);
                e->type = arith_type(*a, *b); break;
            default:
                if (!is_num(a->type) || !is_num(b->type)) throw StError("arithmetic needs numeric operands", line);
                e->type = arith_type(*a, *b);
        }
        e->args.push_back(std::move(a));
        e->args.push_back(std::move(b));
        return e;
    }

    // IEC precedence, lowest first: OR, XOR, AND/&, = <>, < > <= >=, + -, * / MOD, unary - NOT, primary.
    std::unique_ptr<Expr> parse_expr() {
        auto e = parse_xor();
        while (peek_kw("OR")) { const int l = cur().line; ++p_; e = binary(Op::OR, std::move(e), parse_xor(), l); }
        return e;
    }
    std::unique_ptr<Expr> parse_xor() {
        auto e = parse_and();
        while (peek_kw("XOR")) { const int l = cur().line; ++p_; e = binary(Op::XOR, std::move(e), parse_and(), l); }
        return e;
    }
    std::unique_ptr<Expr> parse_and() {
        auto e = parse_equality();
        while (peek_kw("AND") || peek_sym("&")) { const int l = cur().line; ++p_; e = binary(Op::AND, std::move(e), parse_equality(), l); }
        return e;
    }
    std::unique_ptr<Expr> parse_equality() {
        auto e = parse_relational();
        while (peek_sym("=") || peek_sym("<>")) {
            const int l = cur().line; const Op op = cur().text == "=" ? Op::EQ : Op::NE; ++p_;
            e = binary(op, std::move(e), parse_relational(), l);
        }
        return e;
    }
    std::unique_ptr<Expr> parse_relational() {
        auto e = parse_additive();
        while (peek_sym("<") || peek_sym(">") || peek_sym("<=") || peek_sym(">=")) {
            const int l = cur().line; const auto s = cur().text; ++p_;
            const Op op = s == "<" ? Op::LT : s == ">" ? Op::GT : s == "<=" ? Op::LE : Op::GE;
            e = binary(op, std::move(e), parse_additive(), l);
        }
        return e;
    }
    std::unique_ptr<Expr> parse_additive() {
        auto e = parse_term();
        while (peek_sym("+") || peek_sym("-")) {
            const int l = cur().line; const Op op = cur().text == "+" ? Op::ADD : Op::SUB; ++p_;
            e = binary(op, std::move(e), parse_term(), l);
        }
        return e;
    }
    std::unique_ptr<Expr> parse_term() {
        auto e = parse_unary();
        while (peek_sym("*") || peek_sym("/") || peek_kw("MOD")) {
            const int l = cur().line; const Op op = cur().text == "*" ? Op::MUL : cur().text == "/" ? Op::DIV : Op::MOD; ++p_;
            e = binary(op, std::move(e), parse_unary(), l);
        }
        return e;
    }
    std::unique_ptr<Expr> parse_unary() {
        const int l = cur().line;
        if (accept_sym("-")) {
            auto inner = parse_unary();
            if (!is_num(inner->type)) throw StError("unary minus needs a number", l);
            auto e = std::make_unique<Expr>(); e->op = Op::NEG; e->type = inner->type; e->line = l;
            e->args.push_back(std::move(inner)); return e;
        }
        if (accept_kw("NOT")) {
            auto inner = parse_unary();
            require_bool(*inner, "NOT operand");
            auto e = std::make_unique<Expr>(); e->op = Op::NOT; e->type = Type::BOOL; e->line = l;
            e->args.push_back(std::move(inner)); return e;
        }
        return parse_primary();
    }
    std::unique_ptr<Expr> parse_primary() {
        auto e = std::make_unique<Expr>();
        e->line = cur().line;
        if (accept_sym("(")) { e = parse_expr(); expect_sym(")"); return e; }
        if (cur().kind == Tok::INT) { e->op = Op::LIT; e->lit = Value::integer(t_[p_++].ival); e->type = Type::DINT; return e; }
        if (cur().kind == Tok::REAL) { e->op = Op::LIT; e->lit = Value::real(t_[p_++].rval); e->type = Type::LREAL; return e; }
        if (accept_kw("TRUE")) { e->op = Op::LIT; e->lit = Value::boolean(true); e->type = Type::BOOL; return e; }
        if (accept_kw("FALSE")) { e->op = Op::LIT; e->lit = Value::boolean(false); e->type = Type::BOOL; return e; }
        const auto name = expect_ident();
        if (accept_sym("(")) return parse_call(name, e->line);
        const auto it = prog_.index.find(name);
        if (it == prog_.index.end()) throw StError("undeclared variable " + name, e->line);
        const auto& d = prog_.vars[it->second];
        if (d.kind == VarKind::CONSTANT) { e->op = Op::LIT; e->lit = d.initial; e->type = d.type; return e; }  // constant folding
        e->op = Op::VAR; e->var = it->second; e->type = d.type;
        return e;
    }
    std::unique_ptr<Expr> parse_call(const std::string& name, int line) {
        auto e = std::make_unique<Expr>();
        e->op = Op::CALL; e->fn = name; e->line = line;
        if (!peek_sym(")")) do e->args.push_back(parse_expr()); while (accept_sym(","));
        expect_sym(")");
        auto& a = e->args;
        auto numeric_result = [&](std::size_t from) {
            Type t = a[from]->type;
            for (std::size_t k = from; k < a.size(); ++k) {
                if (!is_num(a[k]->type)) throw StError(name + " needs numeric arguments", line);
                Expr lhs; lhs.type = t; t = arith_type(lhs, *a[k]);
            }
            return t;
        };
        if (name == "ABS") { if (a.size() != 1) throw StError("ABS takes 1 argument", line); e->type = numeric_result(0); }
        else if (name == "MIN" || name == "MAX") { if (a.size() < 2) throw StError(name + " takes >= 2 arguments", line); e->type = numeric_result(0); }
        else if (name == "LIMIT") { if (a.size() != 3) throw StError("LIMIT(MN, IN, MX) takes 3 arguments", line); e->type = numeric_result(0); }
        else if (name == "SEL") {
            if (a.size() != 3 || !is_bool(a[0]->type)) throw StError("SEL(G: BOOL, IN0, IN1)", line);
            if (is_bool(a[1]->type) != is_bool(a[2]->type)) throw StError("SEL inputs must have compatible types", line);
            e->type = is_bool(a[1]->type) ? Type::BOOL : numeric_result(1);
        } else throw StError("unsupported function " + name, line);
        return e;
    }

    std::vector<Token> t_;
    std::size_t p_ = 0;
    Program& prog_;
};

std::shared_ptr<const Program> compile(const std::string& source) {
    auto prog = std::make_shared<Program>();
    Parser(lex(source), *prog).parse_pou();
    return prog;
}

std::shared_ptr<const Program> compile_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open ST file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return compile(ss.str());
}

// ---------------------------------------------------------------- evaluator
namespace {
struct ReturnSignal {};

Value arith(Op op, const Value& a, const Value& b, Type result, int line) {
    if (is_real_t(result)) {
        const double x = a.as_real(), y = b.as_real();
        double r = 0;
        switch (op) {
            case Op::ADD: r = x + y; break;
            case Op::SUB: r = x - y; break;
            case Op::MUL: r = x * y; break;
            case Op::DIV: if (y == 0.0) throw StError("division by zero", line); r = x / y; break;
            default: break;
        }
        return coerce(Value::real(r), result);
    }
    const std::int64_t x = a.i, y = b.i;
    std::int64_t r = 0;
    switch (op) {
        case Op::ADD: r = x + y; break;
        case Op::SUB: r = x - y; break;
        case Op::MUL: r = x * y; break;
        case Op::DIV: if (y == 0) throw StError("division by zero", line); r = x / y; break;
        case Op::MOD: if (y == 0) throw StError("division by zero", line); r = x % y; break;
        default: break;
    }
    return coerce(Value::integer(r), result);
}

int compare(const Value& a, const Value& b) {
    if (a.type == Type::BOOL) return (a.b == b.b) ? 0 : (a.b ? 1 : -1);
    if (a.is_int() && b.is_int()) return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    const double x = a.as_real(), y = b.as_real();
    return x < y ? -1 : (x > y ? 1 : 0);
}

Value eval(const Expr& e, const std::vector<Value>& mem) {
    switch (e.op) {
        case Op::LIT: return e.lit;
        case Op::VAR: return mem[e.var];
        case Op::NEG: { const auto v = eval(*e.args[0], mem); return v.is_real() ? coerce(Value::real(-v.r), e.type) : coerce(Value::integer(-v.i), e.type); }
        case Op::NOT: return Value::boolean(!eval(*e.args[0], mem).b);
        case Op::OR: return Value::boolean(eval(*e.args[0], mem).b || eval(*e.args[1], mem).b);
        case Op::AND: return Value::boolean(eval(*e.args[0], mem).b && eval(*e.args[1], mem).b);
        case Op::XOR: return Value::boolean(eval(*e.args[0], mem).b != eval(*e.args[1], mem).b);
        case Op::EQ: case Op::NE: case Op::LT: case Op::GT: case Op::LE: case Op::GE: {
            const int c = compare(eval(*e.args[0], mem), eval(*e.args[1], mem));
            switch (e.op) {
                case Op::EQ: return Value::boolean(c == 0);
                case Op::NE: return Value::boolean(c != 0);
                case Op::LT: return Value::boolean(c < 0);
                case Op::GT: return Value::boolean(c > 0);
                case Op::LE: return Value::boolean(c <= 0);
                default: return Value::boolean(c >= 0);
            }
        }
        case Op::CALL: {
            std::vector<Value> v;
            for (const auto& a : e.args) v.push_back(eval(*a, mem));
            if (e.fn == "SEL") return coerce(v[0].b ? v[2] : v[1], e.type);
            if (e.fn == "ABS") return v[0].is_real() ? coerce(Value::real(std::fabs(v[0].r)), e.type) : coerce(Value::integer(v[0].i < 0 ? -v[0].i : v[0].i), e.type);
            if (e.fn == "LIMIT") {
                const Value& mn = v[0]; const Value& in = v[1]; const Value& mx = v[2];
                const Value& r = compare(in, mn) < 0 ? mn : (compare(in, mx) > 0 ? mx : in);
                return coerce(r, e.type);
            }
            Value best = v[0];
            for (std::size_t k = 1; k < v.size(); ++k) {
                const int c = compare(v[k], best);
                if ((e.fn == "MIN" && c < 0) || (e.fn == "MAX" && c > 0)) best = v[k];
            }
            return coerce(best, e.type);
        }
        default: return arith(e.op, eval(*e.args[0], mem), eval(*e.args[1], mem), e.type, e.line);
    }
}

void run(const std::vector<std::unique_ptr<Stmt>>& body, std::vector<Value>& mem, const Program& prog, std::uint64_t& count) {
    for (const auto& s : body) {
        ++count;
        switch (s->kind) {
            case SKind::ASSIGN: mem[s->target] = coerce(eval(*s->expr, mem), prog.vars[s->target].type); break;
            case SKind::RETURN: throw ReturnSignal{};
            case SKind::IF: {
                bool taken = false;
                for (const auto& br : s->branches) {
                    if (eval(*br.first, mem).b) { run(br.second, mem, prog, count); taken = true; break; }
                }
                if (!taken) run(s->else_body, mem, prog, count);
                break;
            }
            case SKind::CASE: {
                const auto sel = eval(*s->expr, mem).i;
                bool taken = false;
                for (const auto& arm : s->arms) {
                    for (const auto& r : arm.ranges) if (sel >= r.first && sel <= r.second) { taken = true; break; }
                    if (taken) { run(arm.body, mem, prog, count); break; }
                }
                if (!taken) run(s->else_body, mem, prog, count);
                break;
            }
        }
    }
}
}

Instance::Instance(std::shared_ptr<const Program> program) : program_(std::move(program)) {
    for (const auto& v : program_->vars) mem_.push_back(v.initial);
}
std::size_t Instance::slot(const std::string& name, bool for_write) const {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    const auto it = program_->index.find(key);
    if (it == program_->index.end()) throw std::out_of_range("ST variable not found: " + name);
    if (for_write && program_->vars[it->second].kind != VarKind::INPUT)
        throw std::invalid_argument("only VAR_INPUT can be written from outside: " + name);
    return it->second;
}
void Instance::set(const std::string& name, bool v) {
    const auto k = slot(name, true);
    if (program_->vars[k].type != Type::BOOL) throw std::invalid_argument("not a BOOL input: " + name);
    mem_[k] = Value::boolean(v);
}
void Instance::set(const std::string& name, double v) {
    const auto k = slot(name, true);
    if (program_->vars[k].type == Type::BOOL) throw std::invalid_argument("not a numeric input: " + name);
    mem_[k] = coerce(Value::real(v), program_->vars[k].type);
}
void Instance::set_int(const std::string& name, std::int64_t v) {
    const auto k = slot(name, true);
    if (program_->vars[k].type == Type::BOOL) throw std::invalid_argument("not a numeric input: " + name);
    mem_[k] = coerce(Value::integer(v), program_->vars[k].type);
}
Value Instance::get(const std::string& name) const { return mem_[slot(name, false)]; }
void Instance::execute() {
    try { run(program_->body, mem_, *program_, statements_); }
    catch (const ReturnSignal&) {}
}
}
