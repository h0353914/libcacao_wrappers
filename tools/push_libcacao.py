#!/usr/bin/env python3

import subprocess

reboot = True
adb = "/mnt/f/Android/platform-tools/adb.exe"
base_path = "~/lineageos/out/target/product/poplar_kddi/symbols/"
paths = [
    "system/lib64/libcacao_client.so",
    "system/lib64/libcacao_client_real.so",
    "system/lib/libcacao_client.so",
    "system/lib/libcacao_client_real.so",
    "system/lib/libcacao_service.so",
    "system/lib/libcacao_service_real.so",
    "system/lib/libimageprocessorjni.so",
    "system/lib/libimageprocessorjni_real.so",
    "system/lib64/libimageprocessorjni.so",
    "system/lib64/libimageprocessorjni_real.so",
    "system/lib/libcacao_process_ctrl_gateway.so",
    "system/lib/libcacao_process_ctrl_gateway_real.so",
]

cmds = [
    f"{adb} devices",
    f"{adb} root",
    f"{adb} wait-for-device",
    f"{adb} remount",
]


for cmd in cmds:
    print(f"Executing: {cmd}")
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if result.stdout.strip():
        print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip())
    if result.returncode != 0:
        print(f"Command failed: {cmd}")
        exit(result.returncode)
print()

for local in paths:
    date_cmd = f"date -r {base_path}{local} +'%Y-%m-%d %H:%M:%S'"
    result = subprocess.run(date_cmd, shell=True, capture_output=True, text=True)
    file_date = result.stdout.strip() if result.returncode == 0 else "unknown"
    
    push_cmd = f"{adb} push {base_path}{local} {local}"
    result = subprocess.run(push_cmd, shell=True, capture_output=True, text=True)
    
    source = f"{base_path}{local}"
    dest = local
    if result.returncode == 0:
        status = "成功"
    else:
        status = f"失敗({result.stderr.strip()})"
    
    print(f"來源:     {source:<40} {file_date}")
    print(f"目的地:  {dest}")
    print(status)
    print()

if reboot:
    reboot_cmd = f"{adb} reboot"
    print(f"Executing: {reboot_cmd}")
    result = subprocess.run(reboot_cmd, shell=True, capture_output=True, text=True)
    if result.stdout.strip():
        print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip())
    if result.returncode != 0:
        print(f"Command failed: {reboot_cmd}")
        exit(result.returncode)