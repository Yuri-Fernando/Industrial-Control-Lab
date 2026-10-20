#pragma once
#include "stvm.hpp"
#include <control.hpp>
#include <string>

namespace icl {
// Control engine whose interlock state machine is the IEC 61131-3 block st/FB_Interlocks.st executed by stvm.
// The PI cooling law stays in C++ (PiCooling), mirroring a PLC calling a vendor PID block from ST.
// Any ST runtime error de-energizes outputs and latches FAULT/WATCHDOG (fail-safe).
class StController : public ControlEngine {
public:
    explicit StController(std::shared_ptr<const st::Program> program);
    static StController from_file(const std::string& path);
    Outputs step(const Inputs& in, double dt_seconds) override;
    const char* name() const override { return "st"; }
    std::uint64_t statements_executed() const { return fb_.statements_executed(); }
    const std::string& runtime_error() const { return runtime_error_; }
private:
    st::Instance fb_;
    PiCooling pi_;
    Outputs out_{};
    std::string runtime_error_;
};
}
