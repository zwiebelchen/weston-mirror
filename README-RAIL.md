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
| Neue Verbindung übernimmt die laufende Session | ja |
| Laufwerksumleitung (Client-Laufwerke im Server) | ja, über FUSE (siehe unten) |
| Druckerumleitung | ja, eine CUPS-Warteschlange je Client-Drucker (siehe unten) |
| RemoteApp mit `xfreerdp3` (Linux-Client) | startet, Fensterinhalt bleibt schwarz (Client-Problem) |
| Mikrofon-Weiterleitung (audin) | unter FreeRDP 3 deaktiviert |
| App-Liste an den Client publizieren (`rdpapplist`) | nicht verfügbar (Microsoft-eigener Kanal) |
| Allowlist für startbare Programme | ja (`/etc/weston-rail/apps.conf`) |
| Session pro User mit PAM-Login | ja, `weston-rail-broker` (siehe unten) |
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
- **Laufwerke:** `libfuse3-dev fuse3`
- **Drucken:** `cups cups-client cups-filters ghostscript`
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
xfreerdp3 /v:SERVER:3389 /app:program:'||terminal' /cert:ignore
```

Client (Windows, empfohlen): `.rdp`-Datei mit

```
full address:s:SERVER:3389
remoteapplicationmode:i:1
remoteapplicationprogram:s:||firefox
remoteapplicationname:s:Firefox
enablecredsspsupport:i:0
authentication level:i:0
prompt for credentials:i:0
```

`enablecredsspsupport:i:0` ist nötig, weil der Server (noch) kein NLA kann.

Für Laufwerke und Drucker zusätzlich:

```
drivestoredirect:s:*
redirectprinters:i:1
```

## Laufwerksumleitung

Die freigegebenen Laufwerke des Clients erscheinen in der Session unter
`~/RDP-Laufwerke/<Laufwerk>` (z. B. `~/RDP-Laufwerke/C`), anderer Ort über
`WESTON_RDP_DRIVES_DIR`, abschalten mit `WESTON_RDP_DISABLE_DRIVES=1`.
Anwendungen öffnen und speichern dort ganz normal.

Voraussetzungen: Paket `fuse3` und Zugriff auf `/dev/fuse`. Im Proxmox-LXC:
Optionen → Features → **FUSE** aktivieren.

Einschränkungen: Dateien über 4 GB, Kürzen auf eine andere Größe als 0,
Zeitstempel und Rechte setzen werden nicht unterstützt (letzteres wird
stillschweigend akzeptiert, damit `cp -p` & Co. funktionieren).

Der rdpdr-Server stammt aus FreeRDP 3.15 und liegt korrigiert unter
`libweston/backend-rdp/rdpdr/` (fünf Fehler behoben, die Laufwerke in
FreeRDP unbenutzbar machen; Details im Dateikopf).

## Sicherheit – bitte lesen

Aktuell **nicht** im offenen Netz betreiben:

- Beim **direkten Start** von Weston gibt es keine Authentifizierung: Wer den
  Port erreicht, bekommt eine Session des startenden Users. Im Betrieb daher
  den **Session-Broker** verwenden (PAM-Login, eine Session pro User).
- NLA wird noch nicht unterstützt; Zugangsdaten gehen TLS-verschlüsselt im
  Client-Info-PDU über die Leitung.
- Startbar sind nur Programme aus der Allowlist. Ein Terminal gehört nicht
  hinein, sonst hat jeder Client eine Shell.

## Allowlist: veröffentlichte Programme

Ein Client kann nur Programme starten, die in `/etc/weston-rail/apps.conf`
stehen (`./build.sh install` legt beim ersten Mal eine Beispieldatei an).
Die Datei wird bei jedem Start neu gelesen, Änderungen gelten sofort.

```ini
[app]
name=firefox
command=/usr/bin/firefox
client-arguments=false

[app]
name=writer
command=/usr/bin/libreoffice --writer
client-arguments=true
```

In der `.rdp`-Datei wird das Programm über seinen Namen angefordert, wie bei
Windows-RemoteApps: `remoteapplicationprogram:s:||firefox`. Der Pfad aus
`command` (`/usr/bin/firefox`) wird ebenfalls akzeptiert. `command` wird ohne
Shell ausgeführt; Argumente mit Leerzeichen in Anführungszeichen setzen.
Argumente aus der `.rdp`-Datei (`remoteapplicationcmdline:s:…`) werden nur
mit `client-arguments=true` angehängt, sonst ignoriert.

Alles andere lehnt der Server ab (mstsc zeigt dann, dass das Programm nicht
in der Liste der zulässigen Programme steht). Nur zum Testen lässt sich die
Prüfung mit `WESTON_RAIL_ALLOW_ANY_PROGRAM=1` abschalten – dann kann jeder
Client z. B. eine Shell starten. Andere Datei: `WESTON_RAIL_APPS_CONF=/pfad`.

## Druckerumleitung

Für jeden Drucker, den der Client meldet (`redirectprinters:i:1`), legt die
Session eine CUPS-Warteschlange an: `rdp-<user>-<Druckername>`, Beschreibung
„<Druckername> (<Client>)“. Sie erscheint in jedem Druckdialog (GTK, Firefox,
LibreOffice, `lp`). Der Standarddrucker des Clients wird Standard des Users.
Beim Trennen werden die Warteschlangen wieder entfernt.

Druckweg: Anwendung → CUPS (PDF) → Filter `rdpxps` (Ghostscript `xpswrite`)
→ Backend `rdpprint` → Session → rdpdr → Client. Drucker, die mstsc mit
`XPSFORMAT` meldet, bekommen XPS und werden vom Windows-Treiber des Clients
gedruckt – unabhängig vom Druckermodell. Andere Drucker bekommen generisches
PostScript. Erzwingen mit `WESTON_RDP_PRINT_FORMAT=xps` bzw. `ps`,
abschalten mit `WESTON_RDP_DISABLE_PRINTERS=1`.

Einmalig einrichten:

```bash
sudo usermod -aG lpadmin $USER     # darf Warteschlangen anlegen; danach neu anmelden
sudo systemctl restart cups        # nach ./build.sh install (neuer MIME-Typ)
```

`./build.sh install` legt das Backend nach `/usr/lib/cups/backend/rdpprint`
(root, 0700 – es muss in das private Laufzeitverzeichnis des Users) und den
Filter nach `/usr/lib/cups/filter/rdpxps`. Das Backend liefert nur an einen
Socket in `/run/user/<uid>` des Auftrags-Users, der diesem User gehört.

## Session-Broker: eine Session pro User

Für den Mehrbenutzerbetrieb startet man Weston nicht mehr selbst, sondern
den Broker. Er läuft als root auf Port 3389:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now weston-rail-broker
journalctl -u weston-rail-broker -f          # Log des Brokers
```

Ablauf:

1. mstsc verbindet sich, der Broker prüft Benutzername und Passwort per PAM
   (Dienst `weston-rail`, `/etc/pam.d/weston-rail`) gegen die Linux-Konten.
2. Der Broker leitet den Client mit einem Einmal-Token (60 s gültig) auf sich
   selbst um. mstsc verbindet sich sofort neu, das sieht man nicht.
3. Die neue Verbindung geht an die Weston-Instanz des Users. Hat er noch
   keine, startet der Broker sie unter seinem Konto: eigene PAM-/logind-
   Session, `/run/user/<uid>`, eigener D-Bus, Locale aus `/etc/default/locale`.
4. Weitere Verbindungen desselben Users (zweite App, Wiederverbinden)
   landen in derselben Instanz. Andere User bekommen ihre eigene.
5. Ist kein Client mehr verbunden und keine App mehr offen, endet die
   Session nach 60 s (`--idle-exit=SEK`).

`.rdp`-Datei für den Broker:

```
full address:s:SERVER:3389
remoteapplicationmode:i:1
remoteapplicationprogram:s:||firefox
remoteapplicationname:s:Firefox
username:s:BENUTZER
enablecredsspsupport:i:0
prompt for credentials:i:0
authentication level:i:0
disableconnectionsharing:i:1
drivestoredirect:s:*
redirectprinters:i:1
```

Anmeldedaten: Solange der Broker kein NLA kann, fragt mstsc das Passwort
nicht selbst ab (`prompt for credentials:i:1` verlangt CredSSP). mstsc
schickt aber gespeicherte Zugangsdaten mit. Einmalig auf dem Client:

```
cmdkey /generic:TERMSRV/SERVERNAME /user:BENUTZER /pass:PASSWORT
```

`SERVERNAME` genau so, wie er in `full address` steht. Root-Anmeldungen sind
gesperrt (`--allow-root`).

`authentication level:i:0`: Die Verbindung ist per TLS verschlüsselt, mstsc
prüft aber nicht, ob das Serverzertifikat vertrauenswürdig ist. Für den
Betrieb besser ein vertrauenswürdiges Zertifikat einrichten (siehe unten) und
`authentication level:i:2` setzen – mit `2` bricht mstsc bei einem
unbekannten Zertifikat ab.

### Zertifikat

Beim ersten Start erzeugt der Broker `/etc/weston-rail/tls.crt/.key`
(selbstsigniert), gültig für Rechnername, vollqualifizierten Namen, alle
IP-Adressen und die mit `--name` angegebenen Namen. Jede Session bekommt eine
Kopie, damit der Client nur ein Zertifikat sieht.

Externen DNS-Namen aufnehmen:

```bash
sudo systemctl edit weston-rail-broker
#   [Service]
#   Environment=BROKER_ARGS=--name=rdp.example.org
sudo rm /etc/weston-rail/tls.crt /etc/weston-rail/tls.key
sudo systemctl restart weston-rail-broker
```

Damit Windows dem selbstsignierten Zertifikat vertraut, `tls.crt` auf den
Client kopieren und als Administrator importieren (oder per GPO verteilen):

```
certutil -addstore Root tls.crt
```

Alternativ ein Zertifikat einer eigenen oder öffentlichen CA (z. B. Let's
Encrypt) als `tls.crt`/`tls.key` ablegen.

Log einer Session: `/run/user/<uid>/weston-rail.log`.

Drucken im Mehrbenutzerbetrieb: Jeder User braucht die Gruppe `lpadmin`,
damit seine Session Warteschlangen anlegen darf.

Optionen: `weston-rail-broker --help` (Port, Zertifikat, Weston-Pfad,
Leerlaufzeit). Für Tests ohne Broker funktioniert der direkte Start von
Weston wie oben weiterhin.

## Änderungen gegenüber microsoft/weston-mirror

- Port auf die FreeRDP-3-API (Zertifikate, RFX, Clipboard, WinPR-Stringfunktionen)
- audin unter FreeRDP 3 als Stub
- Xwayland wird mit `-listenfd` statt dem veralteten `-listen` gestartet
- Build- und Installationsskripte
