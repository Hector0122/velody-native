# Velody (nativo) — prueba mínima

App de escritorio en C++ (JUCE + Tracktion Engine) para grabar desde tu interfaz de audio y procesar con plugins VST3/LV2. Sin Electron ni navegador.

Esta prueba sirve para validar tres cosas en tu PC antes de migrar velody:

1. La Scarlett se ve como entrada y graba a WAV con latencia baja (JACK/PipeWire).
2. Tus plugins cargan y abren su ventana.
3. El flujo (pistas, grabar, reproducir) se siente bien.

## Qué hace

- **Audio**: elegir interfaz, entradas, frecuencia de muestreo y buffer.
- **Plugins**: escanear VST3/LV2 (la primera vez hay que darle *Options → Scan for new or updated VST3 plug-ins*).
- Una pista mono por cada entrada (Scarlett 2i2 → pista 1 = entrada 1, pista 2 = entrada 2), armada y con monitoreo.
- **+** en el pie de cada pista: agrega un plugin; clic en el plugin abre su ventana.
- **Grabar / Detener**, **Play / Stop**, **+ Pista**, **Borrar** (clip, plugin o pista seleccionada).
- El proyecto y las tomas (WAV) se guardan en `~/Documents/Velody/` (botón **Carpeta**).
- Arriba a la derecha: interfaz, Hz, buffer y latencia que reporta el driver.

## Compilar en Linux (Debian/Ubuntu)

```bash
sudo apt install build-essential cmake ninja-build git libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev
```

```bash
git -c url."https://github.com/".insteadOf="git@github.com:" clone --recurse-submodules <url-del-repo> velody-native && cd velody-native
```

(El `insteadOf` es porque Tracktion declara JUCE con URL SSH; sin llaves SSH en GitHub el clon falla.)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

La primera compilación tarda varios minutos (compila JUCE y Tracktion completos).

## Correr con baja latencia (PipeWire)

```bash
pw-jack ./build/Velody_artefacts/Release/Velody
```

En **Audio**, elige tipo **JACK**. Para bajar la latencia: `pw-metadata -n settings 0 clock.force-quantum 128`.

Si quieres oírte sin ninguna latencia mientras grabas, usa el monitoreo directo de la Scarlett (botón Direct Monitor o `alsa-scarlett-gui`) y apaga el monitoreo en la pista.

## Plugins de Windows

Instala [yabridge](https://github.com/robbert-vdh/yabridge), agrega tus carpetas de plugins con `yabridgectl add <carpeta>` y corre `yabridgectl sync`. Quedan en `~/.vst3/yabridge`, que el escáner de Velody ya revisa.

## Estructura

- `Source/Main.cpp` — toda la app (~200 líneas).
- `libs/tracktion_engine` — submódulo; incluye JUCE. La línea de tiempo y las ventanas de plugins se reutilizan de `examples/common` del propio Tracktion.

## Licencia

Tracktion Engine y JUCE son GPLv3/AGPLv3 (o comercial). Si publicas este repo, debe ser GPLv3.
