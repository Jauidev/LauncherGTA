# LauncherGTA

Port nativo y no oficial de **GTA IV** y **Episodes from Liberty City** (Xbox 360) a Android, con su propio launcher.

El juego no se emula: el ejecutable de Xbox 360 se recompila a código nativo ARM64 y se dibuja con un renderizador Vulkan.

> **Este repositorio solo tiene código fuente.** No incluye el juego, ni APK, ni ISO, ni ningún archivo de Rockstar. Para usarlo necesitas tu propia copia legal del juego.

## Estado

Jugable en los dos móviles de prueba, sin errores gráficos conocidos. El rendimiento depende sobre todo de la CPU del móvil, de su temperatura y de su RAM.

| Móvil | GPU | Driver |
|---|---|---|
| Poco X6 Pro | Mali-G615 | El del móvil |
| Redmi Note 11 | Adreno 610 | Turnip (incluido) |

- **Requisitos:** Android 10 o superior, ARM64 y un driver con Vulkan 1.2 o superior. En Adreno se puede usar Turnip si el driver del móvil no llega.
- **Discos admitidos:** las ISO de la versión USA de GTA IV y de Episodes from Liberty City, sin actualizaciones. El código está recompilado desde esos ejecutables, y otras versiones no funcionan.

## Launcher

El launcher es nativo (C++ con ImGui sobre OpenGL ES 3, en `runtime/os/launcher*.cpp`). Desde él se puede:

- Importar el juego desde tu ISO, sin copiar archivos con adb.
- Elegir entre GTA IV y Episodes from Liberty City.
- Ajustar los gráficos con perfiles o a mano.
- Elegir el driver en Adreno: automático, el del sistema, Turnip o uno importado de AdrenoTools.
- Configurar los controles táctiles o el mando.

Está traducido a español, inglés, francés, alemán e italiano.

## Qué no se publica

| Falta | Motivo | De dónde sale |
|---|---|---|
| `ppc/`, `ppc_iv/` | Código recompilado del juego | Se genera desde tu copia (paso 3) |
| ISO, `default.xex`, datos del juego | Propiedad de Rockstar Games | Tu copia legal |
| `shader_cache.cpp`, `shader_override_cache.cpp`, `third_party/rexglue/LibertyRecompLib/` | Shaders compilados o reconstruidos a partir del juego, y fuentes del juego | [LibertyRecomp](https://github.com/vaduur/LibertyRecompAndroid) y tu copia |
| Fondos del launcher (`LAUNCHER/*.png`) | No son parte del código | Pon tus propias imágenes |
| Xenia, XenonRecomp y otras dependencias | Se descargan aparte | `tools/fetch-deps.sh` |
| Turnip | Binario de terceros | [K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers), versión v26.0.0-rc08, en `third_party/turnip/v26.0.0-R8/` |

## Compilar

> **Aviso:** todavía no se ha probado compilar desde un clon limpio. Es probable que algún paso necesite ajustes.

**Hace falta:** Windows con Git Bash, Python 3, Node.js, Android SDK (build-tools 36.0.0, plataforma 34, CMake 3.31.6), NDK 27.2.12479018, JDK y Visual Studio Build Tools 2022.

1. **Dependencias:** desde la raíz del repositorio, ejecuta `bash tools/fetch-deps.sh`. Descarga las dependencias en la versión usada y aplica los cambios de `patches/`.
2. **Archivos de LibertyRecomp:** copia `LibertyRecompLib` en `third_party/rexglue/` y genera las cachés de shaders. `rexglue_glue/shader_override_cache.cpp` se genera con `tools/gta4_shader_override_compiler.py`.
3. **Recompilar el juego:** extrae `default.xex` de tu ISO y ponlo en `private/gta4/default.xex` (GTA IV) y `private/default.xex` (Episodes from Liberty City). Compila XenonRecomp con `build-xenonrecomp.bat` y ejecuta `tools\recompile-iv.bat` y `tools\recompile.bat`.
4. **Fondos:** añade `LAUNCHER/FONDO DE MALI.png` y `LAUNCHER/FONDO DE ADRENO.png`.
5. **APK:**

   ```bash
   bash tools/build-android.sh iv gta4app
   bash tools/build-android.sh gta4app
   bash tools/build-apk.sh
   ```

6. **Instalar:** ejecuta `adb install -r build/apk/gta4port.apk`, abre la app e importa tu ISO desde el launcher.

## Estructura

| Carpeta | Contenido |
|---|---|
| `runtime/` | Kernel de Xbox 360 reimplementado, memoria, audio, launcher y código de Android |
| `rexglue_glue/` | Enganches sobre el código recompilado y opciones del juego |
| `third_party/rexglue/` | Base de ReXGlue y el renderizador nativo, con los cambios del port |
| `android/` | Actividad de Android y manifiesto |
| `config/`, `analysis/` | Configuración de la recompilación y tablas de direcciones entre versiones |
| `tools/` | Compilación y análisis |
| `patches/` | Cambios del port sobre Xenia y XenonRecomp |

## Créditos

Este proyecto se apoya en:

- [LibertyRecomp](https://github.com/vaduur/LibertyRecompAndroid): renderizador nativo y enganches del juego (GPL-3.0).
- ReXGlue, de Tom Clay, y [Xenia](https://github.com/xenia-canary/xenia-canary) (BSD).
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) y [XenosRecomp](https://github.com/sonicnext-dev/XenosRecomp).
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), como referencia.
- [libadrenotools](https://github.com/bylaws/libadrenotools) y Turnip (Mesa).
- [o1heap](https://github.com/pavel-kirienko/o1heap), [unordered_dense](https://github.com/martinus/unordered_dense) y libmspack.

## Licencia

[GPL-3.0](LICENSE), la licencia que exige el código de LibertyRecomp. Las partes de terceros conservan sus propias licencias y avisos de copyright.

## Aviso legal

Proyecto de aficionados, sin relación con Rockstar Games ni Take-Two Interactive ni respaldo de ninguna de ellas. Grand Theft Auto y sus nombres y marcas pertenecen a sus dueños. No se distribuye ningún archivo del juego: no pidas ni compartas ISO, ejecutables ni datos del juego en este repositorio.
