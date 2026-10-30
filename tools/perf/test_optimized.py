import subprocess
import time
import os

def adb(cmd):
    return subprocess.check_output(['adb'] + cmd).decode('utf-8', 'ignore')

def main():
    print('[*] Deteniendo juego previo...')
    subprocess.run(['adb', 'shell', 'am', 'force-stop', 'com.gta4port'])
    time.sleep(2)
    subprocess.run(['adb', 'logcat', '-c'])

    env_opts = 'GTA4_GPU=native GTA4_SKY_TRACE=1 GTA4_AUTOPRESS=1 GTA4_AUTOPRESS_START_UNTIL=35 GTA4_NATIVE_CVARS="gta4_native_early_fragment_tests=true"'
    print(f'[*] Lanzando APK optimizado (ctr_as_local + cr_as_local + early_zs)...')
    subprocess.run(['adb', 'shell', f"am start -n com.gta4port/.MainActivity --es env '{env_opts}'"])

    pid = None
    for _ in range(20):
        time.sleep(1)
        out = adb(['shell', 'pidof', 'com.gta4port']).strip()
        if out:
            pid = int(out.split()[0])
            break

    if not pid:
        print('[!] Error: No se pudo obtener el PID del juego.')
        return

    print(f'[+] Juego corriendo con PID {pid}! Esperando 45s a entrar en escena...')
    time.sleep(45)

    # Check alive
    current_pid = adb(['shell', 'pidof', 'com.gta4port']).strip()
    print(f'[*] Comprobacion de estabilidad: PID={current_pid} (Esta vivo: {bool(current_pid)})')

    # Top CPU
    top_out = adb(['shell', f'top -b -n 1 -p {pid}'])
    print('\n--- TOP CPU PROCESS ---')
    for line in top_out.splitlines():
        if 'com.gta4port' in line or '%CPU' in line:
            print('  ' + line.strip())

    # Thermals
    thermals = adb(['shell', 'dumpsys', 'thermalservice'])
    for line in thermals.splitlines():
        if 'Temperature{' in line and any(k in line for k in ['CPU', 'BATTERY', 'GPU']):
            print('  ' + line.strip())

    # Take screenshot
    shot = subprocess.check_output(['adb', 'exec-out', 'screencap', '-p'])
    out_path = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'perf', 'test_optimized_shot.png')
    with open(out_path, 'wb') as f:
        f.write(shot)
    print(f'[+] Screenshot guardado en {out_path}')

if __name__ == '__main__':
    main()
