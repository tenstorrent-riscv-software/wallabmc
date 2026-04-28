#!/usr/bin/env python3
# SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

import subprocess
import sys
import time
import os

def getline(proc, procname, start_time, timeout_sec):
    if time.time() - start_time > timeout_sec:
        print("\n!!! [Test Runner] TIMEOUT: Test took too long !!!")
        raise RuntimeError(f"Timed out waiting for {procname} stdout line.")
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError(f"Broken pipe waiting for {procname} stdout line.")
    print(f"|{procname}|{line.rstrip()}") # Print to CI logs
    return line


def run_qemu_ci():
    # --- CONFIGURATION ---
    # Load from CI environment variables or use defaults for local testing
    timeout_sec = int(os.environ.get("TEST_TIMEOUT", "30"))

    # How long to wait after sending 'shutdown' before forcing a kill
    grace_period = 1.0

    # We assume 'west' is in the PATH (provided by the CI venv)
    #west_cmd = ["west", "build", "-t", "run"]
    qemu_cmd = f"qemu-system-arm -M lm3s6965evb -nographic -kernel {sys.argv[1]} -net nic,model=stellaris -net user,hostfwd=tcp::5023-:23".split()
    telnet_cmd = "telnet localhost 5023".split()

    qemu_proc = None
    telnet_proc = None

    print(f"--- [Test Runner] Starting Cycle ---")

    try:
        # Start QEMU
        print(f"--- [Test Runner] Device found. Launching QEMU... ---")
        qemu_proc = subprocess.Popen(
            qemu_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=0 # Unbuffered I/O
        )

        start_time = time.time()
        shutdown_sent_time = None
        cmd_sent = False
        forced_success = False
        seen_prompt = False
        seen_dhcp = False

        # Wait for console prompt and DHCP lease
        while not seen_dhcp:
            line = getline(qemu_proc, "BMC", start_time, timeout_sec)
            if "Address: " in line:
                seen_dhcp = True
            if "uart:~$" in line:
                seen_prompt = True

        qemu_proc.stdin.write("\n")
        qemu_proc.stdin.flush()

        while not seen_prompt:
            line = getline(qemu_proc, "BMC", start_time, timeout_sec)
            if "uart:~$" in line:
                seen_prompt = True

        qemu_proc.stdin.write("power on\n")
        qemu_proc.stdin.flush()

        while True:
            line = getline(qemu_proc, "BMC", start_time, timeout_sec)
            if "uart:~$" in line:
                break

        # Network is up, run telnet
        print(f"--- [Test Runner] Network up. Connecting telnet... ---")
        telnet_proc = subprocess.Popen(
            telnet_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=0 # Unbuffered I/O
        )

        while True:
            line = getline(telnet_proc, "Telnet", start_time, timeout_sec)
            if "Escape character is" in line:
                break

        # remotely power off the BMC from telnet
        print("|Telnet|~$bmc poweroff") # Print to CI logs
        telnet_proc.stdin.write("bmc poweroff\n")
        telnet_proc.stdin.flush()

        while True:
            line = getline(qemu_proc, "BMC", start_time, timeout_sec)
            if "Poweroff BMC" in line:
                break

    except Exception as e:
        print(f"\n!!! [Test Runner] EXCEPTION: {e}")
        raise

    finally:
        if qemu_proc and qemu_proc.poll() is None:
            print("--- [Test Runner] Cleaning up QEMU ---")
            qemu_proc.terminate()
            qemu_proc.wait()

if __name__ == "__main__":
    run_qemu_ci()
