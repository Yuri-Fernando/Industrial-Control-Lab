#pragma once
// stvm — compact interpreter for a subset of IEC 61131-3 Structured Text.
//
// Purpose: execute the project's ST function blocks inside the C++ scan cycle and differential-test them
// against the C++ engine. It is NOT a conformant IEC 61131-3 compiler (no TON/CTU library, no loops,
// no user functions, no arrays/structs). Supported subset:
//   FUNCTION_BLOCK/PROGRAM, VAR_INPUT / VAR_OUTPUT / VAR / VAR CONSTANT, types BOOL INT DINT REAL LREAL,
//   assignment, IF/ELSIF/ELSE, CASE (integer labels, lists, ranges), RETURN,
//   OR XOR AND & NOT, = <> < > <= >=, + - * / MOD, unary -, builtins ABS MIN MAX LIMIT SEL.
// Type fidelity: INT stores as int16, DINT as int32, REAL is rounded to float32, LREAL is double.
// Without loops, one scan executes a bounded number of statements (deterministic worst-case time).
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace icl::st {
enum class Type { BOOL, INT, DINT, REAL, LREAL };

struct Value {
    Type type = Type::BOOL;
    bool b = false;
    std::int64_t i = 0;
    double r = 0.0;
    static Value boolean(bool v) { Value x; x.type = Type::BOOL; x.b = v; return x; }
    static Value integer(std::int64_t v, Type t = Type::DINT) { Value x; x.type = t; x.i = v; return x; }
    static Value real(double v, Type t = Type::LREAL) { Value x; x.type = t; x.r = v; return x; }
    bool is_real() const { return type == Type::REAL || type == Type::LREAL; }
    bool is_int() const { return type == Type::INT || type == Type::DINT; }
    double as_real() const { return is_real() ? r : static_cast<double>(i); }
};

class StError : public std::runtime_error {
public:
    StError(const std::string& msg, int line) : std::runtime_error("ST line " + std::to_string(line) + ": " + msg), line_(line) {}
    int line() const { return line_; }
private:
    int line_;
};

enum class VarKind { INPUT, OUTPUT, LOCAL, CONSTANT };
struct VarDecl {
    std::string name;  // upper-case (ST identifiers are case-insensitive)
    Type type;
    VarKind kind;
    Value initial;
};

struct Expr;
struct Stmt;
// Parsed, type-checked program; immutable and shareable between instances.
struct Program {
    std::string pou_name;
    std::vector<VarDecl> vars;
    std::unordered_map<std::string, std::size_t> index;
    std::vector<std::unique_ptr<Stmt>> body;
    Program();
    ~Program();
};

std::shared_ptr<const Program> compile(const std::string& source);
std::shared_ptr<const Program> compile_file(const std::string& path);

// One function-block instance: persistent variable memory + execute() = one scan.
class Instance {
public:
    explicit Instance(std::shared_ptr<const Program> program);
    void set(const std::string& name, bool v);
    void set(const std::string& name, double v);
    void set_int(const std::string& name, std::int64_t v);
    Value get(const std::string& name) const;
    bool get_bool(const std::string& name) const { return get(name).b; }
    double get_real(const std::string& name) const { return get(name).as_real(); }
    std::int64_t get_int(const std::string& name) const { return get(name).i; }
    void execute();
    std::uint64_t statements_executed() const { return statements_; }
    const Program& program() const { return *program_; }
private:
    std::size_t slot(const std::string& name, bool for_write) const;
    std::shared_ptr<const Program> program_;
    std::vector<Value> mem_;
    std::uint64_t statements_ = 0;
};

Value coerce(const Value& v, Type target);
}
