#include "prx/libc/include/CpuTopology.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "cpu topology test failed: %s\n", what);
    std::exit(1);
}

static std::uint64_t Parse(const char* text) {
    const auto path = std::filesystem::temp_directory_path() / ("anyps5_cpu_topology_test_cpus-" + std::to_string(std::random_device{}()));
    {
        std::ofstream file(path, std::ios::trunc);
        file << text;
    }
    const auto mask = CpuTopology::MaskFromCpuList(path.c_str());
    std::filesystem::remove(path);
    return mask;
}

int main() {
    Require(Parse("0-15\n") == 0xffffull, "range");
    Require(Parse("16-31\n") == 0xffff0000ull, "high range");
    Require(Parse("0,2,4-5\n") == 0x35ull, "list of cpus and ranges");
    Require(Parse("3") == 0x8ull, "single cpu without a newline");
    Require(Parse("62-70\n") == 0xc000000000000000ull, "cpus past 63 are left out");
    Require(Parse("") == 0, "empty list");
    Require(Parse("4-2\n") == 0, "reversed range");
    Require(Parse("0-x\n") == 0, "malformed range");
    Require(Parse("0;1\n") == 0, "malformed separator");
    Require(CpuTopology::MaskFromCpuList("/nonexistent/anyps5/cpus") == 0, "missing file");

    {
        std::uint64_t byClass[4] = {};
        bool seen[4] = {};
        const auto empty = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(!empty.hybrid && empty.efficient == 0 && empty.performant == 0, "empty classes");

        byClass[0] = 0xff;
        seen[0] = true;
        const auto single = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(!single.hybrid && single.efficient == 0 && single.performant == 0, "single class");

        byClass[1] = 0xff00;
        seen[1] = true;
        const auto twoTier = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(twoTier.hybrid && twoTier.efficient == 0xff && twoTier.performant == 0xff00, "two-tier classes");

        byClass[2] = 0xff0000;
        seen[2] = true;
        const auto threeTier = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(threeTier.hybrid && threeTier.efficient == 0xffff && threeTier.performant == 0xff0000, "three-tier classes aggregate efficient");
    }

    {
        const std::vector<std::uint64_t> smt{0x3, 0xc, 0x30, 0xc0, 0x300, 0xc00};
        Require(CpuTopology::WithoutReservedCores(0xfff, smt, 1, 0) == 0x3ff, "one core of a 6-core SMT host is kept for host threads");
        Require(CpuTopology::WithoutReservedCores(0xfff, smt, 2, 0) == 0xff, "two cores are kept");
        Require(CpuTopology::WithoutReservedCores(0xfff, smt, 0, 0) == 0, "no reservation leaves guest threads unrestricted");
        Require(CpuTopology::WithoutReservedCores(0xfff, smt, 3, 0) == 0x3f, "three cores are kept when three remain");
        Require(CpuTopology::WithoutReservedCores(0xfff, smt, 4, 0) == 0, "a reservation that leaves fewer than three cores is not applied");
        const std::vector<std::uint64_t> unordered{0xc00, 0x3, 0x300, 0xc, 0xc0, 0x30};
        Require(CpuTopology::WithoutReservedCores(0xfff, unordered, 1, 0) == 0x3ff, "the highest core is kept whatever the enumeration order");
        Require(CpuTopology::WithoutReservedCores(0x3f, smt, 1, 0) == 0, "cores outside the process affinity do not count");
        Require(CpuTopology::WithoutReservedCores(0xff, smt, 1, 0) == 0x3f, "the highest core inside the process affinity is kept");
        Require(CpuTopology::WithoutReservedCores(0xfff, {}, 1, 0) == 0, "an unknown core layout leaves guest threads unrestricted");
        const std::vector<std::uint64_t> single{0x1, 0x2, 0x4, 0x8};
        Require(CpuTopology::WithoutReservedCores(0xf, single, 1, 0) == 0x7, "one logical cpu per core");
        const std::vector<std::uint64_t> hybrid{0x3, 0xc, 0x30, 0xc0, 0x100, 0x200, 0x400, 0x800};
        Require(CpuTopology::WithoutReservedCores(0xfff, hybrid, 1, 0xff) == 0xf3f, "a hybrid host keeps its highest performance core");
        Require(CpuTopology::WithoutReservedCores(0xfff, hybrid, 2, 0xff) == 0xf0f, "a hybrid host keeps two performance cores");
        Require(CpuTopology::WithoutReservedCores(0xfff, hybrid, 1, 0) == 0x7ff, "without a preference the highest core is kept");
        Require(CpuTopology::WithoutReservedCores(0xf3f, hybrid, 1, 0xc0) == 0x73f, "a preference outside the process affinity falls back to the highest core");
    }

    const auto& layout = CpuTopology::Get();
    Require(layout.process != 0, "process affinity read");
    Require(!layout.hybrid || (layout.performant & layout.efficient) == 0, "hybrid classes overlap");
#ifdef _WIN32
    std::uint64_t covered = 0;
    for (const auto core : layout.cores) covered |= core;
    Require((covered & layout.process) == layout.process, "every cpu of the process belongs to a listed core");
#endif
    const std::uint64_t first = layout.process & (~layout.process + 1);
    Require(CpuTopology::Pin(nullptr, first) == first, "pin to one cpu of the process");
    cpu_set_t set;
    CPU_ZERO(&set);
    Require(pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0, "read thread affinity");
    Require(CPU_COUNT(&set) == 1 && CPU_ISSET(static_cast<unsigned>(__builtin_ctzll(first)), &set), "thread runs on the pinned cpu only");
    Require(CpuTopology::Pin(nullptr, ~layout.process) == 0, "mask outside the process is not applied");
    int dummy = 0;
    Require(CpuTopology::Pin(&dummy, first) == 0, "other threads are not pinned by handle");
    return 0;
}
