# Velody (nativo)

App de escritorio en C++ (JUCE + Tracktion Engine) para grabar desde tu interfaz de audio y procesar con plugins VST3/LV2. Sin Electron ni navegador.

Es la nueva versión de Velody: reemplaza a la app de Electron (`~/Developer/velody`, que queda deprecada). Comparte con ella la biblioteca, la venv de Python y la nube, así que las dos pueden convivir mientras termina la migración. Lo que falta traer está en [Pendiente](#pendiente-migración-desde-electron).

## Qué hace

- **Canciones**: abre una canción de la biblioteca de Velody (`~/.config/velody/output/stems`) con una pista por stem (mute/solo en cada una) y tus entradas en pistas aparte, después de los stems. La biblioteca solo se lee: el proyecto de cada canción y sus tomas se guardan en `~/Documentos/Velody/<canción>.tracktionedit`.
- **Metrónomo**: al abrir una canción ya analizada (con BPM en su `.velody`) se agrega una pista *Metrónomo* después de los stems, con los mismos clicks y acentos que el metrónomo de Electron (rejilla de beats, "el 1" manual o detectado). Se regenera cada vez que abres la canción; mute/volumen como cualquier pista.
- **Inicio de la música**: al abrir una canción el cursor se coloca donde empieza a sonar (se salta el silencio inicial, como en Electron); **⏮ Inicio** regresa ahí.
- **Acordes**: a la derecha de la segunda fila, grande, el acorde que suena → el siguiente (del análisis del `.velody`; siguen la velocidad).
- **Count-in** (selector arriba): 1 o 2 compases de clicks sobre la rejilla de la canción antes de que entre, desde donde esté el cursor. Arranca con el valor de Electron (`playbackCountInBars`) y tu cambio se guarda en el proyecto de la canción.
- **Sonido del metrónomo** (selector junto al count-in): Woodblock, Beep, Clave o Cencerro. Se guarda para todas las canciones (`~/.config/Velody/Velody.settings`) y también lo usa el count-in.
- La línea de tiempo muestra la canción completa (se reajusta al cambiar la velocidad).
- **Velocidad** (50–150 %) y **Tono** (±12 semitonos): en tiempo real con Signalsmith Stretch (la misma librería que Electron); la velocidad no cambia el tono. El metrónomo y el count-in siguen la velocidad; el tono no afecta el click. Arrancan con `practiceSpeed`/`transposeSemitones` de Electron. Grabar se desactiva mientras no estén en 100 % / 0 st (igual que en Electron).
- **Mezcla** (abajo): volumen (dB) y pan por pista; doble clic regresa a 0. Se guarda con el proyecto.
- **Importar** (cola, como el JobManager de Electron): *Archivos…* (uno o varios: wav/mp3/flac/m4a/ogg), *Desde YouTube…* (pegas el enlace; se descarga con `download.py`/yt-dlp y, si YouTube bloquea una versión vieja de yt-dlp, se actualiza en la venv y se reintenta una vez) o **arrastrando archivos a la ventana**. Cada canción se separa con Demucs (`python-worker/separate.py`, la venv de Electron en `~/.local/share/velody/venv`) y se analiza el tempo, la rejilla y los acordes (`detect_bpm.py`), **una a la vez**. El botón dice *Importar (N)* mientras hay pendientes y el menú muestra la cola: clic en una en espera o en curso la cancela, clic en una fallida muestra el error completo, *Limpiar las terminadas*. Si una falla, la cola sigue. La canción queda en la biblioteca compartida (también aparece en Electron) y se abre sola solo si no tienes otra canción abierta.
- **Exportar**: mezcla o cada pista por separado, en WAV o MP3 (MP3 con ffmpeg). Se exporta **lo que escuchas**: volumen, pan, mute/solo, plugins y velocidad/tono; el metrónomo solo si su pista no está en mute. (Electron exportaba siempre a velocidad normal.)
- **Quitar una canción**: en **Canciones**, con la canción abierta, *Quitar «…»…*. Manda a la **papelera** la carpeta de la canción (stems + `.velody`) y su proyecto (y, si eliges, tus grabaciones). Si la canción también está en la nube, pregunta: *Quitar de esta laptop* (sube antes lo que falte, libera espacio, conserva tu proyecto aquí) o *Borrar en todas partes* (primero la nube; si eso falla no se borra nada local).
- **Nube** (*Canciones → Nube…*): copia de la biblioteca en tu Cloudflare R2 con el mismo Worker que Electron (`velody/cloud-worker`), así que las dos apps ven la misma nube. Pones la URL del Worker y el token (se guarda en `~/.config/Velody/cloud.settings` con permisos 0600 y no se vuelve a mostrar; hay que volver a pegarlo, porque Electron lo guarda cifrado), *Probar conexión*, *Activar*. En el menú **Canciones**: ✓ = aquí y en la nube, ☁ = solo en la nube (se descarga completa y verificada al abrirla); al abrir una ✓ se trae el análisis más nuevo (gana la última escritura). *Subir «…» a la nube*, *Subir a la nube las N canciones que solo están aquí* (solo sube lo que cambió) y *Borrar de la nube* (canciones que solo están allá). Sin conexión todo sigue funcionando con lo local; el menú avisa. Las transferencias van en segundo plano con progreso en la barra de estado.
- **Audio**: elegir interfaz, entradas, frecuencia de muestreo y buffer.
- **Plugins**: escanear VST3/LV2 (la primera vez hay que darle *Options → Scan for new or updated VST3 plug-ins*).
- Una pista mono por cada entrada (Scarlett 2i2 → pista 1 = entrada 1, pista 2 = entrada 2), armada, con el monitoreo apagado (se prende en **I → Input Monitoring**; con bocinas causa retroalimentación).
- **+** en el pie de cada pista: agrega un plugin; clic en el plugin abre su ventana.
- **Grabar / Detener**, **Play / Stop**, **+ Pista**, **Borrar** (clip, plugin o pista seleccionada).
- El proyecto y las tomas (WAV) se guardan en `~/Documentos/Velody/` (tu carpeta de Documentos) (botón **Carpeta**).
- Arriba a la derecha: interfaz, Hz, buffer y latencia que reporta el driver.

## Compilar en Linux (Debian/Ubuntu)

```bash
sudo apt install build-essential cmake ninja-build git libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libxi-dev ladspa-sdk libcurl4-openssl-dev
```

```bash
git -c url."https://github.com/".insteadOf="git@github.com:" clone --recurse-submodules <url-del-repo> velody-native && cd velody-native
```

(El `insteadOf` es porque Tracktion declara JUCE con URL SSH; sin llaves SSH en GitHub el clon falla.)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

La primera compilación tarda varios minutos (compila JUCE y Tracktion completos).

## Pruebas

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Las pruebas de la nube levantan un Worker falso (`Tests/fake_cloud.py`, necesita `python3`): nunca tocan tu nube real.

Prueba real de separación (opcional, tarda): `VELODY_E2E_SEPARATE=/ruta/a/cancion.wav ./build/VelodyTests_artefacts/Release/VelodyTests`. Los stems van a una carpeta temporal, no a la biblioteca.

## Correr con baja latencia (PipeWire)

```bash
pw-jack ./build/Velody_artefacts/Release/Velody
```

En **Audio**, elige tipo **JACK**. Para bajar la latencia: `pw-metadata -n settings 0 clock.force-quantum 128`.

Si quieres oírte sin ninguna latencia mientras grabas, usa el monitoreo directo de la Scarlett (botón Direct Monitor o `alsa-scarlett-gui`); el monitoreo de la pista ya viene apagado.

## Plugins de Windows

Instala [yabridge](https://github.com/robbert-vdh/yabridge), agrega tus carpetas de plugins con `yabridgectl add <carpeta>` y corre `yabridgectl sync`. Quedan en `~/.vst3/yabridge`, que el escáner de Velody ya revisa.

## Estructura

- `Source/Main.cpp` — ventana, botones y transporte.
- `Source/Library.h` — escaneo de la biblioteca de canciones (mismas reglas que `scanSongsLibrary` de Electron).
- `Source/SongEdit.h` — arma las pistas de stems de una canción.
- `Source/MixerPanel.h` — volumen y pan por pista.
- `Source/Metronome.h` — lectura del `.velody`, clicks del metrónomo y count-in (port de `downbeat.ts`).
- `Source/CountInPlugin.h` — plugin del bus master que silencia la canción y toca el count-in.
- `Source/PythonWorker.h` — lanza los scripts de Python y lee su JSON (con SIGTERM para cancelar).
- `Source/SongMeta.h` — escribe el `.velody` igual que `updateMeta` de Electron.
- `Source/SongInfo.h` — inicio de la música y acordes (port de `utils.ts`).
- `Source/SongDelete.h` — quitar una canción (a la papelera, con validación de rutas).
- `Source/Exporter.h` — render offline de la mezcla o de cada pista.
- `Source/ImportQueue.h` — cola de importación (descargar → separar → analizar, uno a la vez, cancelable) y el menú *Importar*.
- `Source/CloudClient.h` — cliente HTTP del Worker de la nube y ajustes (port de `cloud/client.ts`).
- `Source/CloudSync.h` — sincronización: lista combinada, subir, descargar, metadatos, quitar/borrar (port de `cloud/sync.ts`).
- `Source/CloudUi.h` — menú *Canciones* con estados de la nube, hilo de transferencias y ventana *Nube…*.
- `python-worker/` — copia de los scripts de Python de Electron (Demucs, BPM, acordes).
- `Tests/` — pruebas con `juce::UnitTest` (sin dependencias extra).
- `libs/tracktion_engine` — submódulo; incluye JUCE. La línea de tiempo y las ventanas de plugins se reutilizan de `examples/common` del propio Tracktion.

## Publicar una versión

Solo con tags (no hay CI en cada push):

1. Sube la versión en `CMakeLists.txt` (`project(VelodyNative VERSION 0.2.0)`) y haz commit.
2. `git tag v0.2.0 && git push origin main v0.2.0`

`.github/workflows/release.yml` compila, corre las pruebas, arma el `.deb` (`velody-native`, convive con el `velody` de Electron) y lo publica en GitHub Releases. Para armarlo local: `cd build && cpack -G DEB`.

## Pendiente (migración desde Electron)

Inventario del 2026-11-24 (comparando `velody/src` contra este repo). Orden acordado:

1. ~~Inicio de la música y acordes~~ — hecho.
2. ~~Quitar canciones~~ — hecho.
3. ~~Sincronización con la nube~~ — hecho (rama `sincronizacion-nube`, falta probarla a mano contra la nube real y mezclar a `main`).
4. **Vibes → Velody**: desde Vibes (`~/Developer/vibes`, tiene su propio Worker con R2 + D1) mandar canciones a Velody; Velody baja el MP3 de R2 y lo mete a la cola de importación. Toca los dos repos; falta diseñar dónde vive la cola de "por importar" (D1 de Vibes o el Worker de Velody).
5. **Instalación inicial**: crear la venv (`~/.local/share/velody/venv`) con Demucs/PyTorch (CUDA si hay) y Beat This!, y ffmpeg, con pantalla de progreso (port de `main/setup/index.ts` + `SetupScreen.tsx`). Hoy se usa lo que dejó Electron: **imprescindible antes de desinstalarlo**.

Otros faltantes, por prioridad:

- **Imprescindible**: guardar velocidad/tono/count-in en el `.velody` (hoy solo en el proyecto local, así que no viajan por la nube); analizar en segundo plano/al abrir las canciones sin análisis o con `tracker: fallback`; decidir dónde quedan `cloud-worker/` y `MOBILE_APP.md` cuando se archive el repo de Electron.
- **Recomendable**: puntos de recorte (inicio/fin, en `.velody`), volumen master y *Mix Pan* (click a un lado, mezcla al otro), count-in al grabar, calidad de stems (128/192/320 kbps), buscador de canciones.
- **Opcional**: marcadores + loop A-B, aviso de actualización, botón *Tabs*, grabar en MP3, AppImage.
- **No se porta** (decisión): setlists / Live mode, corregir "el 1" o el BPM a mano, importar enlaces en lote, guardar las tomas en la biblioteca.

## Licencia

Tracktion Engine y JUCE son GPLv3/AGPLv3 (o comercial). Si publicas este repo, debe ser GPLv3.
