// Unit tests for FEAT-0023 machine-id resolution (ResolveMachineIdFrom). The
// pure resolution (override precedence + trim + host-name lower-casing) is
// tested here; the GetComputerNameW / file-read wrapper (ResolveMachineId) is
// thin platform glue exercised when wired into startup.

#include "machine_identity.h"

#include "test_helpers.h"

#include <string>

namespace {

using svg_mb_control::ResolveMachineIdFrom;

void TestOverrideWinsWhenPresent() {
    ExpectEqual(ResolveMachineIdFrom("OTHER-HOST", "snd-desk"), "snd-desk",
                "a non-empty machine-id override wins over the host name");
}

void TestOverrideIsTrimmed() {
    ExpectEqual(ResolveMachineIdFrom("OTHER", "  snd-desk \r\n"), "snd-desk",
                "the machine-id override is trimmed");
}

void TestHostNameUsedAndLowercasedWhenNoOverride() {
    ExpectEqual(ResolveMachineIdFrom("SND-DESK", ""), "snd-desk",
                "the host name is used (trimmed + lower-cased) when no override");
}

void TestWhitespaceOverrideFallsBackToHost() {
    ExpectEqual(ResolveMachineIdFrom("HOST", "   \r\n"), "host",
                "a whitespace-only override falls back to the host name");
}

void TestEmptyHostAndOverrideYieldEmpty() {
    ExpectEqual(ResolveMachineIdFrom("", ""), "",
                "an empty host and override yield an empty machine id");
}

}  // namespace

int main() {
    TestOverrideWinsWhenPresent();
    TestOverrideIsTrimmed();
    TestHostNameUsedAndLowercasedWhenNoOverride();
    TestWhitespaceOverrideFallsBackToHost();
    TestEmptyHostAndOverrideYieldEmpty();
    if (g_failures == 0) {
        std::cout << "machine_identity_tests: all passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
