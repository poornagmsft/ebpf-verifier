// Copyright (c) Prevail Verifier contributors.
// SPDX-License-Identifier: MIT
#pragma once

#if __linux__

#include <iostream>
#include <tuple>
#include <vector>

#include "asm_syntax.hpp"
#include "gpl/spec_type_descriptors.hpp"
#include "linux_ebpf.hpp"

int create_map_linux(uint32_t map_type, uint32_t key_size, uint32_t value_size, uint32_t max_entries);
std::tuple<bool, double> bpf_verify_program(BpfProgType type, const std::vector<ebpf_inst>& raw_prog);

#else

#define create_map_linux (nullptr)

std::tuple<bool, double> bpf_verify_program(BpfProgType type, const std::vector<ebpf_inst>& raw_prog) {
    std::cerr << "linux domain is unsupported on this machine\n";
    exit(64);
    return {{}, {}};
}
#endif
