# weston-mirror als RemoteApp-Server (RAIL) unter Debian 13

Dieser Fork von [microsoft/weston-mirror](https://github.com/microsoft/weston-mirror)
soll einzelne Linux-Anwendungen per RDP als **RemoteApp** bereitstellen – ohne
Desktop dahinter, vergleichbar mit RemoteApp auf Windows Server.

Grundlage ist Weston 9 mit dem RDP-Backend und der `rdprail-shell`
(RAIL = *Remote Applications Integrated Locally*, MS-RDPERP). Jedes Fenster
einer Anwendung erscheint beim Client als eigenes lokales Fenster.

## Stand

| Bereich | Status |
|---|---|
| Kompiliert unter Debian 13 (FreeRDP 3) | ja |
| Weston startet mit RDP-Backend + rdprail-shell | ja |
| TLS mit automatisch erzeugtem Zertifikat | ja |
| RemoteApp mit `mstsc` (Fenster, Tastatur inkl. Umlaute, Maus) | ja, getestet mit Firefox und weston-terminal |
| Mehrere Instanzen über eine Verbindung | ja (mstsc nutzt die offene Verbindung) |
| Abmelden, wenn die letzte App geschlossen wird | ja, nach 5 s |
| RemoteApp mit `xfreerdp3` (Linux-Client) | startet, Fensterinhalt bleibt schwarz (Client-Problem) |
| Mikrofon-Weiterleitung (audin) | unter FreeRDP 3 deaktiviert |
| App-Liste an den Client publizieren (`rdpapplist`) | nicht verfügbar (Microsoft-eigener Kanal) |
| Authentifizierung | **fehlt** (siehe Sicherheit) |
| Session pro User | geplant (siehe Roadmap) |

## Schnellstart

```bash
git clone https://github.com/zwiebelchen/weston-mirror.git
cd weston-mirror

./scripts/install-deps.sh      # Abhängigkeiten installieren
./build.sh                     # konfigurieren + bauen
./build.sh install             # nach /usr/local installieren (sudo)
```

### install-deps.sh

| Aufruf | Wirkung |
|---|---|
| `./scripts/install-deps.sh` | installiert alle fehlenden Pakete |
| `./scripts/install-deps.sh --list` | zeigt die komplette Paketliste |
| `./scripts/install-deps.sh --check` | zeigt, welche Pakete noch fehlen |
| `./scripts/install-deps.sh --force` | auch auf anderen Distributionen als Debian 13 |

Benötigte Pakete (Stand Debian 13):

- **Werkzeuge:** `build-essential git meson ninja-build pkgconf`
- **Wayland:** `libwayland-dev libwayland-bin wayland-protocols`
- **Grafik/Text:** `libpixman-1-dev libcairo2-dev libpango1.0-dev libpng-dev librsvg2-dev`
- **Eingabe:** `libxkbcommon-dev libinput-dev libevdev-dev libudev-dev`
- **Sonstiges:** `libdrm-dev libpam0g-dev libssl-dev libglib2.0-dev`
- **RDP:** `freerdp3-dev libwinpr3-dev`
- **Xwayland (X11-Apps):** `libx11-dev libxcb1-dev libxcb-composite0-dev libxcb-shape0-dev libxcb-xfixes0-dev libxcursor-dev`
- **Laufzeit:** `xwayland openssl`

### build.sh

| Aufruf | Wirkung |
|---|---|
| `./build.sh` | konfiguriert (falls nötig) und baut |
| `./build.sh install` | baut und installiert (sudo, danach `ldconfig`) |
| `./build.sh reconfigure` | meson-Optionen neu anwenden, dann bauen |
| `./build.sh clean` | Build-Verzeichnis löschen |
| `./build.sh rebuild` | clean + bauen |

Umgebungsvariablen: `PREFIX` (Standard `/usr/local`), `BUILD_DIR` (`build`),
`BUILDTYPE` (`debugoptimized`), `JOBS`. Ein geänderter `PREFIX` braucht
`PREFIX=... ./build.sh reconfigure`.

Nach dem ersten Lauf geht auch direkt `ninja -C build`.

Gebaut wird nur, was ein headless RemoteApp-Server braucht: RDP-Backend,
rdprail-shell, pixman-Renderer (kein GL, keine GPU nötig) und Xwayland.

## Starten (manueller Test)

```bash
export XDG_RUNTIME_DIR=/run/user/$(id -u)
weston --backend=rdp-backend.so --shell=rdprail-shell.so --port=3389 \
       --logger-scopes=log,rdp-backend,rdprail-shell
```

`XDG_RUNTIME_DIR` muss auf ein existierendes Verzeichnis zeigen. Fehlt
`/run/user/<uid>` (z. B. im LXC-Container ohne Login-Session):
`sudo loginctl enable-linger $USER` oder das Verzeichnis von Hand anlegen
(Besitzer = User, Rechte 700).

Eine UTF-8-Locale setzen (`export LANG=de_DE.UTF-8` bzw. `C.UTF-8`), sonst
zeigen Terminal-Anwendungen keine Umlaute an. Grafische Anwendungen wie
Firefox sind davon nicht betroffen.

Ohne `--rdp-tls-cert`/`--rdp-tls-key` erzeugt Weston beim Start ein
selbstsigniertes Zertifikat. Mehr Debug-Ausgaben: `WESTON_RDP_DEBUG_LEVEL=4`.

Client (Linux):

```bash
xfreerdp3 /v:SERVER:3389 /app:program:/usr/bin/weston-terminal /cert:ignore
```

Client (Windows, empfohlen): `.rdp`-Datei mit

```
full address:s:SERVER:3389
remoteapplicationmode:i:1
remoteapplicationprogram:s:/usr/bin/firefox
remoteapplicationname:s:Firefox
enablecredsspsupport:i:0
authentication level:i:0
prompt for credentials:i:0
```

`enablecredsspsupport:i:0` ist nötig, weil der Server (noch) kein NLA kann.

## Sicherheit – bitte lesen

Aktuell **nicht** im offenen Netz betreiben:

- Es gibt **keine Authentifizierung**. NLA ist aus, Benutzername und Passwort
  werden nicht geprüft. Wer den Port erreicht, bekommt eine Session.
- Der Client bestimmt, **welches Programm** gestartet wird. Jeder absolute Pfad
  ist erlaubt, also auch ein Terminal. Eine Allowlist ist geplant.
- Nur eine RDP-Verbindung pro Weston-Instanz.

## Roadmap: Session pro User

Der RAIL-Modus erlaubt nur einen Peer pro Compositor. Deshalb bekommt jeder
User eine eigene Weston-Instanz, verwaltet von einem Session-Broker:

```
Port 3389 → Broker (root: TLS, PAM-Login, Routing)
              ├── User A → weston --env-socket (als A) → Apps
              └── User B → weston --env-socket (als B) → Apps
```

Weston kann eine vom Broker angenommene Verbindung per `--env-socket` und
Umgebungsvariable `RDP_FD` übernehmen.

1. Broker mit festem Service-User + Allowlist für startbare Programme
2. PAM-Login im Broker, Weiterleitung per RDP-Server-Redirection mit Routing-Token
3. Wiederverbinden in eine laufende Session (FD-Übergabe per `SCM_RIGHTS`)

Mehrere Instanzen derselben Anwendung sind innerhalb einer Session einfach
weitere Wayland-Clients; jedes Fenster wird ein eigenes RAIL-Fenster.

## Änderungen gegenüber microsoft/weston-mirror

- Port auf die FreeRDP-3-API (Zertifikate, RFX, Clipboard, WinPR-Stringfunktionen)
- audin unter FreeRDP 3 als Stub
- Xwayland wird mit `-listenfd` statt dem veralteten `-listen` gestartet
- Build- und Installationsskripte
