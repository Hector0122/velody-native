# Velody (nativo)

App de escritorio en C++ (JUCE + Tracktion Engine) para grabar desde tu interfaz de audio y procesar con plugins VST3/LV2. Sin Electron ni navegador.

Es la nueva versión de Velody: reemplaza a la app de Electron (`~/Developer/velody`, que queda deprecada). Comparte con ella la biblioteca, la venv de Python y la nube, así que las dos pueden convivir mientras termina la migración. Lo que falta traer está en [Pendiente](#pendiente-migración-desde-electron).

## Qué hace

**Pantalla**: arriba el logo, la canción o proyecto abierto (con su tempo, recorte y velocidad) y los menús *Canciones*, *Proyectos*, *Importar*, *Exportar* y *Ajustes* (interfaz de audio, plugins, nube, Vibes, instalación, carpeta). Debajo el transporte (⏮, Play, Grabar, el reloj) con los acordes al centro, la fila de práctica (count-ins, sonido del click, velocidad, tono), las pistas con regla de tiempo y un color por pista, y el mezclador en tiras verticales (se pliega con *Mezcla*). Tema oscuro con los colores y tipografías de la app anterior (Archivo Black, IBM Plex Mono).

- **Proyectos desde cero** (*Proyectos → Nuevo proyecto…*): nombre, tempo y compás; grabas tus pistas sobre el metrónomo del proyecto (botón **Click**; el count-in al grabar usa ese tempo). Viven en `~/Documentos/Velody/Proyectos/`; el menú los lista del más reciente al más viejo y deja renombrar o mandar a la papelera el abierto. Al abrir Velody se abre lo último que tenías (canción o proyecto).
- **Nombres de pista**: doble clic en el nombre (en la pista o en su tira del mezclador) para cambiarlo.

- **Buscar canción** (Ctrl+F, o *Canciones → Buscar canción…*): filtra mientras escribes, sin importar acentos ni mayúsculas y con varias palabras en cualquier orden («caifanes afuera»); ↑/↓ y Enter para abrir (una canción que solo está en la nube se descarga).
- **Actualizaciones**: instalado con el `.deb`, al abrir revisa la última versión en GitHub; si hay una nueva aparece **Actualizar a X.Y.Z** arriba a la derecha: la baja, la instala (pide tu contraseña en la ventana del sistema) y Velody se reinicia ya actualizado, guardando tu proyecto.
- **Canciones**: abre una canción de la biblioteca de Velody (`~/.config/velody/output/stems`) con una pista por stem (mute/solo en cada una) y tus entradas en pistas aparte, después de los stems. La biblioteca solo se lee: el proyecto de cada canción y sus tomas se guardan en `~/Documentos/Velody/<canción>.tracktionedit`.
- **Metrónomo**: al abrir una canción ya analizada (con BPM en su `.velody`) se agrega una pista *Metrónomo* después de los stems, con los mismos clicks y acentos que el metrónomo de Electron (rejilla de beats, "el 1" manual o detectado). Se regenera cada vez que abres la canción; mute/volumen como cualquier pista.
- **Inicio de la música**: al abrir una canción el cursor se coloca donde empieza a sonar (se salta el silencio inicial, como en Electron); **⏮ Inicio** regresa ahí.
- **Tramo** (botón del transporte, los *trim points* de Electron; no corta el audio, solo marca): *La canción empieza aquí* / *La canción termina aquí* en el cursor y *Canción completa*, para saltarte una intro u outro larga. La canción abre en el inicio del recorte, ⏮ Inicio regresa ahí, la reproducción se detiene sola al cruzar el fin (no mientras grabas, y puedes seguir escuchando lo que está después si das Play ahí) y **Exportar** saca solo ese tramo. Se guarda en el `.velody` (`trimStart`/`trimEnd`, igual que Electron) y viaja con la nube; los archivos de audio no se tocan. El botón dice *Tramo ✓* cuando la canción tiene uno. (A diferencia de Electron, la línea de tiempo no sombrea lo que queda fuera.)
- **Acordes**: a la derecha de la segunda fila, grande, el acorde que suena → el siguiente (del análisis del `.velody`; siguen la velocidad).
- **Count-in** (selector arriba): 1 o 2 compases de clicks sobre la rejilla de la canción antes de que entre, desde donde esté el cursor. Se guarda en el `.velody` de la canción (`playbackCountInBars`, igual que Electron), así que viaja con la nube.
- **Count-in al grabar** (selector junto al sonido del metrónomo; sin count-in, 1, 2 o 4 compases, como Electron, 1 por defecto, recordado para todas las canciones): al dar **Grabar** suenan los clicks sobre la rejilla de la canción con la canción en silencio, y la toma empieza exactamente en el cursor (punch-in), así queda alineada y sin el count-in. Sin tempo detectado graba directo.
- **Sonido del metrónomo** (selector junto al count-in): Woodblock, Beep, Clave o Cencerro. Se guarda para todas las canciones (`~/.config/Velody/Velody.settings` (antes quedaba en `~/Velody/`; se mueve solo)) y también lo usa el count-in.
- La línea de tiempo muestra la canción completa (se reajusta al cambiar la velocidad).
- **Velocidad** y **Tono**, con − / +: la velocidad se muestra como el tempo de la canción en BPM (− / + de 1 en 1; clic para escribir uno, p. ej. 120 → 110, o volver al original; 50–150 %) y el tono como su tonalidad, estimada de los acordes («A», o «A → B (+2)»; ±12 semitonos; clic: original). en tiempo real con Signalsmith Stretch (la misma librería que Electron); la velocidad no cambia el tono. El metrónomo y el count-in siguen la velocidad; el tono no afecta el click. Se guardan en el `.velody` de la canción (`practiceSpeed`/`transposeSemitones`, igual que Electron) y viajan con la nube; los valores que antes quedaban solo en el proyecto local se pasan al `.velody` la primera vez que abres la canción. Grabar se desactiva mientras no estén en 100 % / 0 st (igual que en Electron).
- **Mezcla** (abajo, tiras verticales como en REAPER): por pista perilla de pan, fader con medidor, M/S y nombre; doble clic regresa a 0. Se guarda con el proyecto. La tira **Master** (a la derecha) tiene el volumen general y el **Mix Pan** (como en Electron): manda toda la mezcla a un lado mientras el metrónomo conserva su propio pan, p. ej. la mezcla a la bocina por la izquierda y el click a tus audífonos por la derecha. El Mix Pan se recuerda para todas las canciones.
- **Importar** (cola, como el JobManager de Electron): *Archivos…* (uno o varios: wav/mp3/flac/m4a/ogg), *Desde YouTube…* (pegas el enlace; se descarga con `download.py`/yt-dlp y, si YouTube bloquea una versión vieja de yt-dlp, se actualiza en la venv y se reintenta una vez) o **arrastrando archivos a la ventana**. Cada canción se separa con Demucs (`python-worker/separate.py`, la venv de Electron en `~/.local/share/velody/venv`) y se analiza el tempo, la rejilla y los acordes (`detect_bpm.py`), **una a la vez**. El botón dice *Importar (N)* mientras hay pendientes y el menú muestra la cola: clic en una en espera o en curso la cancela, clic en una fallida muestra el error completo, *Limpiar las terminadas*. Si una falla, la cola sigue. *Calidad de los stems* (128, 192 o 320 kbps, como en Electron) se recuerda y aplica a la siguiente canción que se separe. La canción queda en la biblioteca compartida (también aparece en Electron) y se abre sola solo si no tienes otra canción abierta.
- **Desde Vibes**: en Vibes (compu o celular) cada canción que ya está en su nube tiene **Mandar a Velody**. Velody revisa esa lista al abrir, cada 5 minutos y con *Importar → Traer lo que mandaste desde Vibes*; baja el audio (`.m4a` del R2 de Vibes), lo mete a la cola como «Artista - Título» y, ya separada y analizada, la quita de la lista de Vibes. Si falla se queda en Vibes para reintentarla (después de *Limpiar las terminadas*). Se configura en *Importar → Vibes…* con la misma URL y token que usa Vibes (`~/.config/Velody/vibes.settings`, 0600).
- **Exportar**: mezcla o cada pista por separado, en WAV o MP3 (MP3 con ffmpeg). Se exporta **lo que escuchas**: volumen, pan, mute/solo, plugins y velocidad/tono; el metrónomo solo si su pista no está en mute. (Electron exportaba siempre a velocidad normal.)
- **Quitar una canción**: en **Canciones**, con la canción abierta, *Quitar «…»…*. Manda a la **papelera** la carpeta de la canción (stems + `.velody`) y su proyecto (y, si eliges, tus grabaciones). Si la canción también está en la nube, pregunta: *Quitar de esta laptop* (sube antes lo que falte, libera espacio, conserva tu proyecto aquí) o *Borrar en todas partes* (primero la nube; si eso falla no se borra nada local).
- **Nube** (*Canciones → Nube…*): copia de la biblioteca en tu Cloudflare R2 con el mismo Worker que usaba Electron (ahora en `cloud-worker/` de este repo), así que las dos apps ven la misma nube. Pones la URL del Worker y el token (se guarda en `~/.config/Velody/cloud.settings` con permisos 0600 y no se vuelve a mostrar; hay que volver a pegarlo, porque Electron lo guarda cifrado), *Probar conexión*, *Activar*. En el menú **Canciones**: ✓ = aquí y en la nube, ☁ = solo en la nube (se descarga completa y verificada al abrirla); al abrir una ✓ se trae el análisis más nuevo (gana la última escritura). *Subir «…» a la nube*, *Subir a la nube las N canciones que solo están aquí* (solo sube lo que cambió) y *Borrar de la nube* (canciones que solo están allá). Sin conexión todo sigue funcionando con lo local; el menú avisa. Las transferencias van en segundo plano con progreso en la barra de estado.
- **Análisis en segundo plano**: las canciones sin análisis (o de una versión vieja del analizador; y, con Beat This! instalado, las de rejilla `fallback` o sin acordes) se analizan solas una por una con `detect_bpm.py`, como en Electron: esperan mientras se separa una canción, la que abres va primero y, al terminar, le aparecen metrónomo, count-in y acordes sin reabrirla. Un análisis fallido no se guarda (se reintenta al volver a abrir Velody). El resultado se guarda en el `.velody` y viaja con la nube.
- **Instalación (Demucs)**: la primera vez (o si falta algo) se abre sola la ventana *Instalación*; también está en *Importar → Instalación (Demucs)…*. Busca Python 3.9–3.11 (si el sistema no tiene, como Ubuntu 24.04 con 3.12, baja un Python 3.11 portátil), crea la venv en `~/.local/share/velody/venv` e instala Demucs, PyTorch (~2 GB), librosa, yt-dlp y Beat This! (opcional), solo lo que falte. ffmpeg viene como dependencia del `.deb`. Revisar si ya está instalado es instantáneo (lee `pyvenv.cfg` y los `*.dist-info`, sin lanzar procesos). Si algo falla, muestra las últimas líneas de pip y el comando para hacerlo a mano. Es la misma carpeta que usaba la app de Electron, así que lo ya instalado se aprovecha.
- **Audio**: elegir interfaz, entradas, frecuencia de muestreo y buffer.
- **Plugins**: escanear VST3/LV2/LADSPA (la primera vez hay que darle *Options → Scan for new or updated … plug-ins* por formato). Además de las carpetas normales (`~/.vst3`, `/usr/lib/lv2`…), Velody busca los mismos plugins de Linux que usa REAPER (flatpak): las extensiones `org.freedesktop.LinuxAudio.Plugins.*` (Surge XT, Ratatouille, SWH, TAP, AVLDrums, CMT…), eligiendo de cada una la versión más nueva que carga en este sistema (se recuerda en `~/.config/Velody/plugin-paths.cache`). Los plugins propios de REAPER (ReaEQ, ReaComp…) no: son VST2 y vienen dentro de REAPER.
- **Editar tomas** (como en REAPER): arrastra un clip para moverlo (también a otra pista), sus bordes para recortarlo; clic derecho: *Cortar en el cursor* (S), *Duplicar* (Ctrl+D), *Silenciar el clip*, *Fades*, *Repetir esta parte (bucle)* y *Toma* (al grabar con el bucle activo cada pasada queda como toma del mismo clip). **Bucle** en el transporte: inicio/fin en el cursor o el clip seleccionado (L lo prende/apaga; se ve en la regla). Teclas: Espacio Play/Stop, S cortar, Ctrl+D duplicar, Supr borrar, Ctrl+Z / Ctrl+Shift+Z deshacer / rehacer. Si borras por error la pista de un stem: *Canciones → Restaurar las pistas de la canción*. En cada pista: **In** (entrada), **●** (armar para grabar), **M**, **S**, **Mon**; la lista de pistas se desplaza con la rueda o la barra. La columna derecha de cada pista es su cadena de efectos (*+ Efecto* y los plugins por nombre).
- **Buses de efectos y envíos** (como REAPER): *+ Pista → Bus de efectos* crea «Bus: …» con el efecto que elijas (o vacío); en el mezclador, **Envíos** de cada pista elige el bus y muestra una perilla con cuánto le manda (post-fader; todo a la izquierda = apagado). Así una sola reverb sirve a varias pistas.
- **Automatización**: prende **Automatizar** (en la barra de *Mezcla*) y mueve faders o perillas mientras suena: el movimiento queda grabado y la pista lo sigue después; la curva de volumen se dibuja sobre la pista. *Borrar automatización* la quita de la pista seleccionada.
- **Abre como lo dejaste**: la última canción o proyecto, el cursor donde estaba en cada canción y la ventana con su tamaño y posición. Todo lo que cambias se autoguarda cada 30 s (también si Velody se cierra a la fuerza). *+ Pista → Quitar pistas vacías* limpia las pistas sin clips ni efectos (deja 2 para grabar; Ctrl+Z las regresa). Además, al abrir un proyecto con más de 8 pistas vacías (las que creaban versiones anteriores con un dispositivo de audio genérico), las sobrantes se quitan solas. Las canciones mandadas desde Vibes salen de su lista en cuanto quedan separadas (aunque cierres Velody durante el análisis), y las que ya están en tu biblioteca no se vuelven a procesar.
- **Buscador de efectos**: *+ Efecto* abre una ventana con buscador (escribe «neural», «reverb», «eq»…; ↑/↓ y Enter). Cada efecto sale una sola vez aunque venga en VST3, LV2 y LADSPA (se usa el mejor formato), no salen instrumentos y los que usaste último van arriba. Los efectos de Velody (EQ, compresor, reverb, delay…) también están ahí. Si un plugin nuevo no aparece: *Ajustes → Plugins (VST3 / LV2)…* y escanea.
- **Zoom como en REAPER**: Ctrl+rueda acerca/aleja en el tiempo (alrededor del mouse), Alt+rueda hace todas las pistas más altas (ondas más grandes), Shift+rueda se mueve por la canción; también + / −, Ctrl+↑ / Ctrl+↓ y Ctrl+0 (toda la canción). Arrastra el borde de abajo del encabezado de una pista para hacer solo esa más alta (doble clic en el borde: regresa). La barra de abajo mueve la vista y, al reproducir con zoom, la vista sigue al cursor. El zoom no entra en Ctrl+Z.
- **Se detiene al final**: la reproducción para sola donde termina el audio (o en el fin del tramo si marcaste uno); no con el bucle prendido ni mientras grabas.
- **Count-in en pantalla**: durante el count-in aparece en grande cuántos tiempos faltan para entrar (y en qué compás vas).
- **MIDI e instrumentos** (teclados *class compliant* como el M-Audio Oxygen, sin drivers): *+ Pista → Pista de instrumento* con tu sintetizador escaneado (Surge XT, AVLDrums, sfizz…) o el **4OSC** integrado. El teclado va a la pista de instrumento seleccionada (se oye al tocar y Grabar graba un clip MIDI); entra por «All MIDI Ins» de Tracktion, así cualquier teclado conectado funciona y ninguna nota suena doble. *+ Pista → Clip MIDI vacío* crea 4 compases para dibujar notas; **doble clic** en un clip MIDI abre el editor de notas (clic: nota · arrastrar: mover / borde: duración · clic derecho o Supr: borrar · rueda: intensidad · Ctrl+Z; cuadrícula 1/4–1/32 y *Cuantizar*).
- **Tocar en vivo con amp sims** (NAM, Ratatouille…): pon el plugin en la pista de tu entrada (el «+» al final de la pista) y prende **Mon** en el encabezado de la pista para escucharte a través de él (con audífonos: con bocinas se retroalimenta). Para tocar cómodo baja la latencia en *Ajustes → Latencia (buffer)*: 128 o 64 muestras (~5 o ~3 ms ida y vuelta a 48 kHz). El `.deb` abre Velody con `pw-jack` (PipeWire) y la primera vez elige JACK solo; con JACK el buffer es el de PipeWire, que Velody fija mientras está abierto y regresa a automático al cerrarse. La barra de estado dice qué se usa (JACK/ALSA, buffer, latencia).
- Una pista mono por cada entrada (Scarlett 2i2 → pista 1 = entrada 1, pista 2 = entrada 2; como máximo 8, porque un dispositivo genérico puede listar decenas de canales), armada, con el monitoreo apagado (se prende en **I → Input Monitoring**; con bocinas causa retroalimentación).
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

Las pruebas de la nube y de Vibes levantan un Worker falso (`Tests/fake_cloud.py`, necesita `python3`, con las rutas de los dos Workers): nunca tocan tu nube real.

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
- `Source/CloudUi.h` — menú *Canciones* con estados de la nube, hilo de transferencias y ventana *Nube…* (también la usa *Vibes…*).
- `Source/Setup.h` — instalación inicial (Python, venv, paquetes, ffmpeg) y su ventana (port de `main/setup`).
- `Source/RecordCountIn.h` — count-in antes de grabar (plan de clicks y punch-in).
- `Source/SongSearch.h` — búsqueda de canciones.
- `Source/PluginPaths.h` — los plugins de Linux de REAPER (extensiones flatpak) en las rutas de Velody.
- `Source/Updater.h` — aviso de versión nueva e instalación del `.deb`.
- `Source/Projects.h` — proyectos desde cero (carpeta, nombres, tempo/compás/click).
- `Source/Theme.h`, `Source/ui/` — tema visual, ícono/logo (`Resources/brand/velody-icon.svg`), botones de transporte y la línea de tiempo (copia de los componentes de ejemplo de Tracktion, con el tema).
- `Source/Instruments.h`, `Source/ui/PianoRoll.h` — pistas de instrumento, teclados MIDI y el editor de notas.
- `Source/LiveMonitoring.h` — latencia (buffer / quantum de PipeWire) para tocar en vivo.
- `Source/Mixing.h` — buses de efectos, envíos y automatización.
- `Source/ClipEdit.h`, `Source/SongKey.h` — edición de clips y tomas; tonalidad estimada de los acordes.
- `Source/Trim.h` — recorte de inicio/fin por canción (en el `.velody`).
- `Source/LibraryAnalysis.h` — análisis en segundo plano de las canciones sin análisis.
- `Source/PracticeMeta.h` — velocidad/tono/count-in en el `.velody`.
- `Source/Vibes.h` — Vibes → Velody: lista de canciones mandadas desde Vibes, descarga a la cola y quitarlas de la lista.
- `cloud-worker/` — el Worker de Cloudflare (R2 + D1) de la nube de Velody, que antes vivía en el repo de Electron; contrato de la API en `cloud-worker/src/contract.ts`. Desplegar: `cd cloud-worker && pnpm install && pnpm migrate:remote && pnpm deploy`.
- `docs/MOBILE_APP.md` — notas de la app móvil (vienen del repo de Electron).
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
3. ~~Sincronización con la nube~~ — hecho en v0.1.0 (falta probarla a mano contra la nube real).
4. ~~Vibes → Velody~~ — hecho: la lista vive en el Worker de Vibes (tabla `velody_sends`, `GET/PUT/DELETE /v1/velody`), botón *Mandar a Velody* en Vibes compu y celular. **Falta aplicar la migración `0008_velody.sql` y desplegar el Worker de Vibes**, y probarlo a mano de punta a punta.
5. ~~Instalación inicial~~ — hecho (falta probarla a mano en una máquina o usuario sin la venv).

Otros faltantes, por prioridad:

- ~~Guardar velocidad/tono/count-in en el `.velody`~~ — hecho.
- ~~Analizar en segundo plano las canciones sin análisis~~ — hecho.
- **Imprescindible**: ~~decidir dónde quedan `cloud-worker/` y `MOBILE_APP.md`~~ — movidos aquí (`cloud-worker/`, `docs/`).
- ~~Volumen master y *Mix Pan*~~, ~~calidad de stems~~ — hecho.
- ~~Puntos de recorte~~ — hecho.
- ~~Count-in al grabar~~ — hecho (falta probarlo con la interfaz de audio).
- ~~Buscador de canciones~~, ~~aviso de actualización~~ — hecho.
- **Opcional**: marcadores + loop A-B. (Descartado: botón *Tabs*; por ahora solo `.deb`, sin AppImage.)
- **No se porta** (decisión): setlists / Live mode, corregir "el 1" o el BPM a mano, importar enlaces en lote, guardar las tomas en la biblioteca.

## Licencia

Tracktion Engine y JUCE son GPLv3/AGPLv3 (o comercial). Si publicas este repo, debe ser GPLv3.
