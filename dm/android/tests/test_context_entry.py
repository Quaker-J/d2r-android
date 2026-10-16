#!/usr/bin/env python3
"""执行真实专用入口机器码，验证 TLSDESC 解析时全部客体寄存器和状态不被破坏。
Execute the dedicated entry's actual machine code and verify all guest registers and state.
"""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as R
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get("DM_ANDROID_TOOLS", str(Path.home() / "dm-work/tools/android")))
BIN = TOOLS / "ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
MASK = (1 << 64) - 1
XREGS = [getattr(R, "UC_ARM64_REG_X" + str(i)) for i in range(31)]
QREGS = [getattr(R, "UC_ARM64_REG_Q" + str(i)) for i in range(32)]
STATE = [R.UC_ARM64_REG_SP, R.UC_ARM64_REG_NZCV, R.UC_ARM64_REG_FPSR,
         R.UC_ARM64_REG_FPCR, R.UC_ARM64_REG_TPIDR_EL0]


class ContextEntryTest(unittest.TestCase):
    def test_all_registers_and_thread_contexts(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            entry, library = temp / "entry.S", temp / "libcontext-test.so"
            subprocess.run([sys.executable, str(ROOT / "tools/generate_entry.py"), str(entry)], check=True)
            subprocess.run([str(BIN / "aarch64-linux-android29-clang"), "-shared", "-fPIC", "-O1",
                            "-I", str(ROOT / "native"), str(ROOT / "native/runtime.c"), str(entry),
                            str(ROOT / "tests/context_entry_probe.S"), str(ROOT / "tests/context_entry_test.c"),
                            "-Wl,-z,defs", "-ldl", "-o", str(library)], check=True)
            symbols = subprocess.check_output([str(BIN / "llvm-nm"), "-n", "--defined-only", str(library)], text=True)
            symbol_addr = {parts[2]: int(parts[0], 16) for line in symbols.splitlines()
                           if len(parts := line.split()) == 3}
            relocations = subprocess.check_output([str(BIN / "llvm-readelf"), "-r", str(library)], text=True)
            descriptors = re.findall(r"^([0-9a-f]+)\s+.*R_AARCH64_TLSDESC\s", relocations, re.M)
            self.assertEqual(len(descriptors), 1)
            descriptor = int(descriptors[0], 16)
            cache_address = symbol_addr["dm_android_context_cache"]
            cache_header = (ROOT / "native/runtime_context_cache.h").read_text()
            cache_bits = int(re.search(r"#define DM_ANDROID_CONTEXT_CACHE_BITS (\d+)", cache_header)[1])
            cache_hash_shift = int(re.search(r"#define DM_ANDROID_CONTEXT_CACHE_HASH_SHIFT (\d+)", cache_header)[1])
            cache_entry_shift = int(re.search(r"#define DM_ANDROID_CONTEXT_CACHE_ENTRY_SHIFT (\d+)", cache_header)[1])
            raw = library.read_bytes()
            phoff = struct.unpack_from("<Q", raw, 32)[0]
            phsize, phcount = struct.unpack_from("<HH", raw, 54)
            segments = [struct.unpack_from("<IIQQQQQQ", raw, phoff + i * phsize) for i in range(phcount)]
            loads = [s for s in segments if s[0] == 1]
            image_size = (max(s[3] + s[6] for s in loads) + 0xfff) & ~0xfff
            for seed in [0, 7, 123, 255]:
                for cache_mode in ["empty", "hit", "collision", "odd", "null_context", "changed_sequence", "odd_during_read"]:
                  for changed_status in [set(), {"nzcv"}, {"fpcr"}, {"fpsr"}, {"nzcv", "fpcr", "fpsr"}]:
                    with self.subTest(thread_seed=seed, cache=cache_mode, resolver_changes_status=changed_status):
                        base, stack, resolver, stop = 0x100000, 0x600000, 0x800000, 0x900000
                        tp, context = 0x2000000 + seed * 0x10000, 0x3000000 + seed * 0x20000
                        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
                        uc.mem_map(base, image_size)
                        for segment in loads:
                            uc.mem_write(base + segment[3], raw[segment[2]:segment[2] + segment[5]])
                        for address in [stack, resolver, stop]:
                            uc.mem_map(address, 0x10000)
                        uc.mem_write(resolver, struct.pack("<I", 0xd65f03c0))  # ret
                        uc.mem_write(base + descriptor, struct.pack("<QQ", resolver, (context - tp) & MASK))
                        bucket = ((tp ^ (tp >> cache_hash_shift)) >> 4) & ((1 << cache_bits) - 1)
                        cache_slot = base + cache_address + (bucket << cache_entry_shift)
                        if cache_mode != "empty":
                            tag = tp ^ (1 << 63) if cache_mode == "collision" else tp
                            sequence = 3 if cache_mode == "odd" else 2
                            candidate = 0 if cache_mode == "null_context" else context
                            if cache_mode in ["changed_sequence", "odd_during_read"]:
                                candidate = context + 0x800
                            uc.mem_write(cache_slot, struct.pack("<QQQQ", sequence, tag, candidate, 0))
                        for i, register in enumerate(XREGS):
                            uc.reg_write(register, (0x1234567890abcdef + seed * 31 + i) & MASK)
                        for i, register in enumerate(QREGS):
                            uc.reg_write(register, int.from_bytes(bytes((seed + i * 17 + j) % 256 for j in range(16)), "little"))
                        uc.reg_write(R.UC_ARM64_REG_X30, stop)
                        uc.reg_write(R.UC_ARM64_REG_SP, stack + 0x8000)
                        uc.reg_write(R.UC_ARM64_REG_TPIDR_EL0, tp)
                        uc.reg_write(R.UC_ARM64_REG_NZCV, 0xa0000000)
                        uc.reg_write(R.UC_ARM64_REG_FPSR, 0x1f)
                        uc.reg_write(R.UC_ARM64_REG_FPCR, 0x400000)
                        uc.mem_write(stack + 0x8000, bytes(range(128)))
                        before = {reg: uc.reg_read(reg) for reg in XREGS + QREGS + STATE}
                        calls, status_writes, injections = [], [], []
                        decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

                        def resolve(machine, address, size, user):
                            if address == resolver:
                                self.assertEqual(machine.reg_read(R.UC_ARM64_REG_X0), base + descriptor)
                                machine.reg_write(R.UC_ARM64_REG_X0, (context - tp) & MASK)
                                if "nzcv" in changed_status:
                                    machine.reg_write(R.UC_ARM64_REG_NZCV, 0)
                                if "fpsr" in changed_status:
                                    machine.reg_write(R.UC_ARM64_REG_FPSR, 0)
                                if "fpcr" in changed_status:
                                    machine.reg_write(R.UC_ARM64_REG_FPCR, 0)
                                calls.append(address)
                            else:
                                instruction = next(decoder.disasm(bytes(machine.mem_read(address, size)), address))
                                if instruction.mnemonic == "dmb" and instruction.op_str == "ishld" and not injections:
                                    if cache_mode in ["changed_sequence", "odd_during_read"]:
                                        # 已载入错误 payload，再改变 sequence；入口必须丢弃快照并回退。
                                        # The wrong payload is already loaded; changing sequence must discard it and fall back.
                                        sequence = 3 if cache_mode == "odd_during_read" else 4
                                        machine.mem_write(cache_slot, struct.pack("<QQQQ", sequence, tp, context, 0))
                                        injections.append(address)
                                if instruction.mnemonic == "msr":
                                    status_writes.append(instruction.op_str.split(",")[0])

                        uc.hook_add(UC_HOOK_CODE, resolve)
                        uc.emu_start(base + symbol_addr["dm_android_context_entry"], stop, count=1000)
                        self.assertEqual(uc.reg_read(R.UC_ARM64_REG_PC), stop)
                        self.assertEqual(len(calls), int(cache_mode != "hit"))
                        self.assertEqual(len(injections), int(cache_mode in ["changed_sequence", "odd_during_read"]))
                        for register, value in before.items():
                            if register != R.UC_ARM64_REG_X16:
                                self.assertEqual(uc.reg_read(register), value, f"register {register}")
                        self.assertEqual(uc.reg_read(R.UC_ARM64_REG_X16), context)
                        self.assertEqual(bytes(uc.mem_read(stack + 0x8000, 128)), bytes(range(128)))
                        self.assertEqual(status_writes.count("fpcr"), int(cache_mode != "hit" and "fpcr" in changed_status))
                        self.assertEqual(status_writes.count("fpsr"), int(cache_mode != "hit" and "fpsr" in changed_status))
                        self.assertEqual(status_writes.count("nzcv"), 1)


if __name__ == "__main__":
    unittest.main()
