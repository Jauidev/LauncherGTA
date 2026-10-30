import os
import sys
import time
import subprocess
import re

def adb_cmd(args, timeout=30):
    cmd = ["adb"] + args
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return res.stdout.decode("utf-8", "ignore")

def adb_bytes(args, timeout=30):
    cmd = ["adb"] + args
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return res.stdout

def get_pid():
    out = adb_cmd(["shell", "pidof", "com.gta4port"]).strip()
    pids = out.split()
    return int(pids[0]) if pids else None

def main():
    print("[*] Probando GTA4_GPU=native con gta4_native_early_fragment_tests=true en Poco X6 Pro...")
    adb_cmd(["shell", "am", "force-stop", "com.gta4port"])
    time.sleep(2)
    adb_cmd(["logcat", "-c"])

    env_opts = 'GTA4_GPU=native GTA4_SKY_TRACE=1 GTA4_AUTOPRESS=1 GTA4_AUTOPRESS_START_UNTIL=35 GTA4_NATIVE_CVARS="gta4_native_early_fragment_tests=true"'
    print(f"[*] Lanzando con: {env_opts}")
    adb_cmd(["shell", f"am start -n com.gta4port/.MainActivity --es env '{env_opts}'"])

    pid = None
    for _ in range(20):
        time.sleep(1)
        pid = get_pid()
        if pid:
            break

    if not pid:
        print("[!] ERROR: El juego no inicio.")
        sys.exit(1)

    print(f"[+] Juego corriendo con PID {pid}. Esperando 45s a entrar en escena...")
    time.sleep(45)

    print("[*] Midiendo 5 segundos con maliprof...")
    mali_out = adb_cmd(["shell", "/data/local/tmp/maliprof 5 1000"])

    out_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "perf", "test_early_zs")
    os.makedirs(out_dir, exist_ok=True)

    with open(os.path.join(out_dir, "mali_out.txt"), "w", encoding="utf-8") as f:
        f.write(mali_out)

    print("[*] Capturando screenshot de verificacion visual...")
    png = adb_bytes(["exec-out", "screencap", "-p"])
    shot_path = os.path.join(out_dir, "shot_early_zs.png")
    if png.startswith(b"\x89PNG"):
        with open(shot_path, "wb") as f:
            f.write(png)
        print(f"[+] Screenshot guardado en {shot_path}")

    # Parse Late ZS and Early ZS
    print("\n--- RESULTADOS MALI GPU CON EARLY FRAGMENT TESTS ---")
    avg_block = False
    for line in mali_out.splitlines():
        if "=== average" in line:
            avg_block = True
        if avg_block:
            for k in [
                "GPU active cycles",
                "Shader core fragment utilization",
                "Early ZS killed quad percentage",
                "Late ZS tested quad percentage",
                "Programmable core utilization"
            ]:
                if k in line:
                    print("  " + line.strip())

if __name__ == "__main__":
    main()
