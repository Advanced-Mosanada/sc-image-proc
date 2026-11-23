#pragma once

// What this binary is. Filled in at build time (see cmake/BuildInfo.cmake).

namespace scip::build {

struct Info {
    const char* version;     ///< project version, "0.1.0"
    const char* commit;      ///< abbreviated git commit, "unknown" outside git
    const char* describe;    ///< git describe, "-dirty" when built with local changes
    const char* branch;      ///< "" when detached
    const char* commitDate;  ///< ISO 8601 date of the commit
    bool dirty;              ///< built with uncommitted changes to tracked files
    const char* targetCpu;   ///< "x86_64", "aarch64"
    const char* buildType;   ///< "Debug", "Release"...
    const char* compiler;    ///< "GNU 13.2.0"
};

const Info& info();

}  // namespace scip::build
