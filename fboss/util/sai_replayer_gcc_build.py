#!/usr/bin/env python3

# Run as python3 sai_replayer_gcc_build.py
#
#
# This script modifies the internal sai replayer logs
# and builds sai replayer logs using GCC.
#
# For example:
# python3 sai_replayer_gcc_build.py --sai_replayer_log sai_replayer.log
#                                   --sai_replayer_cpp sai_replayer.cpp
#                                   --sai_headers /path/to/sai/inc/
#                                   --sai_lib /path/to/libsai.a
#                                   --brcm_lib /path/to/libxgs_robo.a
#                                   --brcm_phymode_lib /path/to/libphymodepil.a
#                                   --brcm_epdm_lib /path/to/libepdm.a
#                                   --protobuf_lib /path/to/libprotobuf.a

import argparse
import subprocess

HEADERS = """#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>

extern "C" {
#include <sai.h>
}"""


def process_log(log_path, output_path):
    with open(log_path, "r") as log_file:
        with open(output_path, "w") as output_file:
            write_include = False

            for line in log_file:
                # Modify includes
                if line.startswith("#include"):
                    if not write_include:
                        write_include = True
                        output_file.write(HEADERS)
                # Rewrite run_trace() into main function
                elif line.startswith("void run_trace()"):
                    output_file.write("int main() {\n")
                # Skip fboss namespace
                elif "namespace facebook::fboss" not in line:
                    output_file.write(line)


def build_binary(cpp_path, sai_header, *libraries):
    command = f"gcc {cpp_path} -I {sai_header} -lm -lpthread -lrt -lstdc++ -ldl"
    for lib in libraries:
        command += f" {lib}"
    print(command)
    subprocess.run(command, shell=True)


def main():
    psr = argparse.ArgumentParser(
        description="This script modifies the internal sai replayer logs "
        "and builds sai replayer logs using GCC."
    )
    psr.add_argument(
        "--sai_replayer_log",
        required=False,
        type=str,
        default="sai_replayer.log",
        help="Input sai replayer log generated from runtime. This should be "
        "the log file produced by FBOSS agent.",
    )
    psr.add_argument(
        "--sai_replayer_cpp",
        required=False,
        type=str,
        default="sai_replayer.cpp",
        help="Processed cpp file that is used for gcc to compile. Since the "
        "original log file is designed to build with FBOSS, it needs to "
        "replace main function to compile as a standalone file in gcc.",
    )
    psr.add_argument("--sai_headers", required=True, type=str)
    psr.add_argument("--sai_lib", required=True, type=str)
    psr.add_argument("--brcm_lib", required=True, type=str)
    psr.add_argument("--brcm_phymode_lib", required=True, type=str)
    psr.add_argument("--brcm_epdm_lib", required=True, type=str)
    psr.add_argument("--protobuf_lib", required=True, type=str)
    args = psr.parse_args()
    # Process Sai Replayer Log from internal build form
    # to standalone main function.
    process_log(args.sai_replayer_log, args.sai_replayer_cpp)

    # Compile and link the binary from libraries provided.
    build_binary(
        args.sai_replayer_cpp,
        args.sai_headers,
        args.sai_lib,
        args.brcm_lib,
        args.brcm_phymode_lib,
        args.brcm_epdm_lib,
        args.protobuf_lib,
    )


if __name__ == "__main__":
    main()
