import os
import sys
import time
import subprocess
import re
import json

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

def parse_thermals():
    out = adb_cmd(["shell", "dumpsys", "thermalservice"])
    temps = {}
    for line in out.splitlines():
        if "Temperature{" in line and "mValue=" in line:
            m = re.search(r"mValue=([\d\.]+).*?mName=([A-Za-z0-9_]+)", line)
            if m:
                val = float(m.group(1))
                name = m.group(2)
                # Keep HAL/current values if repeated
                temps[name] = val
        if "Thermal Status:" in line:
            m = re.search(r"Thermal Status:\s*(\d+)", line)
            if m:
                temps["ThermalStatus"] = int(m.group(1))
    return temps

def parse_meminfo():
    out = adb_cmd(["shell", "dumpsys", "meminfo", "com.gta4port"])
    mem = {}
    for line in out.splitlines():
        line_s = line.strip()
        if line_s.startswith("TOTAL PSS:"):
            parts = line_s.split()
            if len(parts) >= 3:
                mem["Total_PSS_KB"] = int(parts[2])
        elif line_s.startswith("TOTAL RSS:"):
            parts = line_s.split()
            if len(parts) >= 3:
                mem["Total_RSS_KB"] = int(parts[2])
        elif line_s.startswith("TOTAL SWAP PSS:"):
            parts = line_s.split()
            if len(parts) >= 4:
                mem["Total_SwapPSS_KB"] = int(parts[3])
        elif "Native Heap" in line:
            parts = line_s.split()
            # Native Heap line format in dumpsys meminfo
            # Native Heap  pss  pssClean  sharedDirty  privDirty  sharedClean  privClean  swapPss  heapSize  heapAlloc  heapFree
            if len(parts) >= 3 and parts[0] == "Native" and parts[1] == "Heap":
                try:
                    mem["Native_Heap_PSS_KB"] = int(parts[2])
                except:
                    pass
        elif "Graphics" in line or "Gfx Dev" in line:
            parts = line_s.split()
            if len(parts) >= 2:
                try:
                    mem["Graphics_KB"] = int(parts[1]) if parts[0] in ("Graphics", "Gfx") else int(parts[2])
                except:
                    pass
    return mem

def parse_proc_io(pid):
    out = adb_cmd(["shell", "run-as", "com.gta4port", "cat", f"/proc/{pid}/io"])
    io_data = {}
    for line in out.splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            try:
                io_data[k.strip()] = int(v.strip())
            except:
                pass
    return io_data

def parse_cpu_freqs():
    # Read scaling_cur_freq for cpu0 to cpu7
    out = adb_cmd(["shell", "cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq"])
    freqs = []
    for line in out.splitlines():
        line = line.strip()
        if line.isdigit():
            freqs.append(int(line) // 1000) # MHz
    return freqs

def parse_top_cpu(pid):
    out = adb_cmd(["shell", f"top -b -n 1 -H -p {pid}"])
    threads = []
    total_cpu = 0.0
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 9 and parts[0].isdigit():
            try:
                cpu_pct = float(parts[8].replace("%", ""))
                tname = parts[-1]
                threads.append((tname, cpu_pct))
                total_cpu += cpu_pct
            except:
                pass
    threads.sort(key=lambda x: x[1], reverse=True)
    return total_cpu, threads[:5]

def main():
    print("[*] Iniciando medicion de rendimiento de 3 minutos (180s) en Poco X6 Pro...")
    adb_cmd(["shell", "am", "force-stop", "com.gta4port"])
    time.sleep(2)
    adb_cmd(["logcat", "-c"])

    # Launch native game with autopress through intro
    env_str = "GTA4_GPU=native GTA4_SKY_TRACE=1 GTA4_AUTOPRESS=1 GTA4_AUTOPRESS_START_UNTIL=35"
    print(f"[*] Lanzando APK con opciones: {env_str}")
    adb_cmd(["shell", f"am start -n com.gta4port/.MainActivity --es env '{env_str}'"])

    pid = None
    for _ in range(30):
        time.sleep(1)
        pid = get_pid()
        if pid:
            break

    if not pid:
        print("[!] ERROR: No se pudo obtener el PID de com.gta4port.")
        sys.exit(1)

    print(f"[+] Juego iniciado exitosamente con PID: {pid}")
    
    out_dir = os.path.join(os.path.dirname(__file__), "..", "..", "build", "perf", "native_3min")
    os.makedirs(out_dir, exist_ok=True)

    samples = []
    start_time = time.time()
    total_duration = 180 # 3 minutes
    
    maliprof_started = False
    maliprof_proc = None

    print(f"[*] Monitoreando telemetria cada 5 segundos durante {total_duration} segundos...")

    sample_idx = 0
    while True:
        elapsed = time.time() - start_time
        if elapsed >= total_duration:
            break

        # Check if process is still alive
        current_pid = get_pid()
        if current_pid != pid:
            print(f"[!] ADVERTENCIA: El juego parece haberse cerrado o reiniciado en t={elapsed:.1f}s.")
            break

        t_data = parse_thermals()
        m_data = parse_meminfo()
        io_data = parse_proc_io(pid)
        freqs = parse_cpu_freqs()
        tot_cpu, top_threads = parse_top_cpu(pid)

        # Logcat fps
        fps_matches = []
        log_sample = adb_cmd(["logcat", "-d", "-s", "xenia:I", "gta4:I"])
        for line in log_sample.splitlines():
            if "VblankThread:" in line and "swaps" in line:
                m = re.search(r"\((\d+\.?\d*)\s*fps\)", line)
                if m:
                    fps_matches.append(float(m.group(1)))

        current_fps = fps_matches[-1] if fps_matches else None

        record = {
            "timestamp": elapsed,
            "sample_index": sample_idx,
            "cpu_total_pct": tot_cpu,
            "top_threads": top_threads,
            "cpu_freqs_mhz": freqs,
            "thermals": t_data,
            "memory": m_data,
            "io": io_data,
            "current_fps": current_fps
        }
        samples.append(record)
        sample_idx += 1

        print(f"[t={elapsed:5.1f}s] CPU: {tot_cpu:5.1f}% | RAM PSS: {m_data.get('Total_PSS_KB', 0)/1024:6.1f} MB | Temp CPU: {t_data.get('CPU', 0):4.1f}C | Bat: {t_data.get('BATTERY', 0):4.1f}C | FPS: {current_fps if current_fps is not None else 'N/A'}")

        # Start maliprof around t=100s for 15s to get GPU metrics during city streaming
        if elapsed >= 100 and not maliprof_started:
            maliprof_started = True
            print("[*] Iniciando contador de hardware Mali GPU (maliprof 15s)...")
            maliprof_proc = subprocess.Popen(["adb", "shell", "/data/local/tmp/maliprof 15 1000"],
                                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        time.sleep(5)

    # Collect maliprof output
    gpu_profile_text = ""
    if maliprof_proc:
        try:
            stdout, _ = maliprof_proc.communicate(timeout=25)
            gpu_profile_text = stdout.decode("utf-8", "ignore")
            with open(os.path.join(out_dir, "mali_gpu_profile.txt"), "w", encoding="utf-8") as f:
                f.write(gpu_profile_text)
            print("[+] Perfil de GPU Mali capturado exitosamente.")
        except Exception as e:
            print(f"[!] Error esperando maliprof: {e}")

    # Capture final screenshot
    print("[*] Capturando screenshot final a los 3 minutos...")
    png_bytes = adb_bytes(["exec-out", "screencap", "-p"])
    if png_bytes.startswith(b"\x89PNG"):
        shot_path = os.path.join(out_dir, "screenshot_3min.png")
        with open(shot_path, "wb") as f:
            f.write(png_bytes)
        print(f"[+] Screenshot guardado en {shot_path}")

    # Full logcat dump
    full_logcat = adb_cmd(["logcat", "-d"])
    with open(os.path.join(out_dir, "logcat_3min.txt"), "w", encoding="utf-8") as f:
        f.write(full_logcat)

    # Save samples JSON
    with open(os.path.join(out_dir, "samples_3min.json"), "w", encoding="utf-8") as f:
        json.dump(samples, f, indent=2)

    print("\n[+] Medicion completada con exito. Procesando informe final...")

if __name__ == "__main__":
    main()
