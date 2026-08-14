# Ghost Recon Wildlands VR (GRW-XR)

[English](README.md) | **Deutsch** | [한국어](README.ko.md)

> [!CAUTION]
> **DAS SPIEL-UPDATE VOM 13.08.2026 MACHT DIESE VERSION UNWIRKSAM.** Ubisoft hat
> `GRW.exe` erneut ersetzt. Das ist eine **neuere** Programmdatei als die, für die
> v0.7.0 Unterstützung hinzugefügt hat. Auf dieser Spielversion tut v0.8.5-alpha
> nichts.
>
> Das ist kein Absturz, und es wird nichts beschädigt. Der Mod prüft die Identität
> der Programmdatei, erkennt diese nicht, installiert nichts und schreibt
> `build pin: UNKNOWN GRW.exe binary` ins Protokoll. Das Spiel läuft dann einfach
> normal und ohne VR weiter. Es muss nichts deinstalliert werden.
>
> Unterstützung dafür ist in Arbeit, aber in **keiner** Version enthalten, und es
> gibt keinen Termin. Bitte nicht als Fehler melden.
>
> Das Update hat ausserdem Easy Anti-Cheat aus dem Spiel entfernt. An der Regel
> ändert das nichts: Der Mod bleibt ausschliesslich für die Solo-Kampagne.

> Diese Seite ist eine gekürzte deutsche Fassung. Sie enthält alle Warnungen, die
> Installation, die Basiseinstellungen und die ehrliche Einordnung dessen, was der
> Mod kann. Die ausführlichen Abschnitte (Roadmap, technische Funktionsweise,
> vollständige Konfigurationsliste, Versionsverlauf) stehen nur in der
> [englischen Fassung](README.md).
>
> Austausch auf Deutsch:
> [der Thread auf vrforum.de](https://vrforum.de/threads/ghost-recon-wildlands-grw-xr.14507/).

> [!WARNING]
> **NUR EINZELSPIELER. Solo-Kampagne, niemals Koop, niemals PvP, niemals
> Matchmaking.** Das Spiel setzt im Mehrspielermodus Easy Anti-Cheat ein, und dieser
> Mod darf in diesem Kontext niemals laufen. Während des Testens wird der
> Offline-Modus empfohlen (Steam offline oder Ubisoft Connect offline).
>
> **DIES IST KEIN FERTIGES VR-ERLEBNIS.** Der Mod befindet sich in einer frühen
> Entwicklungs- und Testphase. Die Darstellung ist bereits wirklich gut: echte
> Stereotiefe, ein bildfüllendes 4K-Bild und eine echte Ego-Kamera, die am
> Kopfknochen der Spielfigur hängt (Kopf ausgeblendet, Nahbereichs-Unschärfe
> entfernt).
>
> **DIE WAFFE FOLGT DEINEM CONTROLLER, seit v0.8.0.** Position und Ausrichtung,
> eins zu eins, im Headset bestätigt. Es ist die Waffe des Spiels selbst und kein
> Overlay: du richtest die Hand, und die Waffe zeigt dorthin; du bewegst die Hand,
> und sie geht mit.
>
> Die Kugeln folgen in dieser Version noch deinem Blick, über die Visierung (ADS)
> zu zielen bleibt also der genaue Weg zu treffen. Das ist das letzte Stück, es ist
> nah dran, und darum geht es in der nächsten Version. Sonst ändert sich an der
> Steuerung nichts: Sticks, Tasten, Trigger und Griffe werden weiterhin als
> gewöhnliches Gamepad gelesen, ein echtes Gamepad wird also nicht gebraucht.
>
> **FUNKTIONIERT MIT DEM TITEL-UPDATE "LAST RITES" (AUGUST 2026), ab v0.7.0.** Das
> Update hat die Programmdatei des Spiels ersetzt. Diese Version bringt eine
> vollständig verifizierte Adresstabelle dafür mit, und volles Stereo auf dem
> aktualisierten Spiel ist im Headset bestätigt. Steam und Ubisoft Connect liefern
> jetzt die IDENTISCHE Programmdatei aus, beide Stores sind also abgedeckt. Ein
> bekannter Ausfall, bis er neu hergeleitet ist: **das Ausblenden des Kopfes in der
> Ego-Perspektive funktioniert auf dem aktualisierten Spiel vorübergehend NICHT**
> (du siehst Haare oder Helm von innen).

Ein nativer OpenXR-VR-Mod für Tom Clancy's Ghost Recon Wildlands (AnvilNext 2.0,
DirectX 11). Kopfverfolgtes stereoskopisches 3D, gerendert von der Engine des
Spiels selbst und über einen `dxgi.dll`-Proxy eingeklinkt. Es werden niemals
Spieldateien verändert.

**Status: experimentelle Alpha, laufende Entwicklung.** Das ist eine
Momentaufnahme, kein fertiger Mod. Rechne mit rauen Kanten. Alle Leistungsangaben
stammen von genau einem Testsystem; andere Hardware, Headsets und Einstellungen
können deutlich schlechter laufen.

## Was funktioniert

- Native OpenXR-Sitzung auf dem D3D11-Gerät des Spiels, getaktet vom Headset (72 Hz)
- Vollständiges Headtracking, das die echte Spielkamera steuert
- Stereoskopische Tiefe über abwechselndes Rendern pro Auge, mit korrekter
  Platzierung des jeweiligen Sichtkegels
- **Bildfüllende Ansicht**: der Mod überschreibt das gerenderte Sichtfeld
  (Standard 1,92 rad, etwa 110 Grad), damit das Bild das Headset ausfüllt statt als
  Fenster zu erscheinen
- **Interne 4K-Darstellung ohne Änderungen am Desktop.** Die gesamte Pipeline
  rendert mit 3840x2160, auch von einem gewöhnlichen 1080p-Desktop aus. Die
  Renderauflösung ist ein Konfigurationsschlüssel und damit zugleich der Regler
  für Qualität gegen Bildrate.
- **DIE WAFFE FOLGT DEINEM CONTROLLER**, in Position und Ausrichtung, eins zu eins.
  Es ist die Waffe des Spiels selbst, bewegt über den Knochen, an dem die Engine sie
  befestigt, genau in dem Moment, in dem die Engine diesen Knochen ausliest. Die
  Kugeln folgen vorerst noch deinem Blick.
- **Touch-Controller als EMULIERTES GAMEPAD, ein echtes Gamepad wird nicht
  benötigt.** Das ist Gamepad-Emulation, NICHT Bewegungssteuerung.
- **Handmarkierungen**: zwei farbige Punkte dort, wo deine Controller wirklich
  sind, mit echter Stereotiefe.
- **Echte Ego-Perspektive, verankert am Kopfknochen.** Die Augenhöhe folgt
  automatisch Stehen, Ducken und Liegen, und die Kamera bleibt beim Bewegen am Kopf.
- **Der Kopf deiner Figur wird in der Ego-Perspektive ausgeblendet** (auf dem
  Update von 08/2026 vorübergehend außer Betrieb, siehe Warnung oben).
- **Die Nahbereichs-Bewegungsunschärfe des Körpers ist entfernt.** Brust, Arme und
  Waffe verschmieren nicht mehr.
- **Robuster VR-Start.** Schläft das Headset beim Spielstart, wartet der Mod und
  aktiviert sich, sobald das Headset aufwacht. Kein Neustart nötig.
- **Durchgehendes 1:1-Kopfzielen** (optional, Numpad Komma/Entf). Kugeln folgen
  deinem Blick, der rechte Stick dreht weiterhin darunter.
- **Zieloptiken**: beim Blick durch das Zielfernrohr tritt der Mod zurück, damit
  Optik und Treffer exakt wie im flachen Spiel zusammenpassen.
- **Konfigurations-GUI und Hot Reload.** Alle Einstellungen liegen in
  `GRWVR\grwxr.cfg` und werden etwa eine Sekunde nach dem Speichern übernommen.
- Stabile 72 fps auf dem Testsystem über längere Open-World-Sitzungen

## Motion Controls: der genaue Stand

Dieser Abschnitt ist bewusst in beide Richtungen deutlich, denn Motion Controls
sind das, was die meisten am meisten interessiert, und zugleich das, was sich am
leichtesten schönreden lässt.

### Was jetzt funktioniert, im Headset bestätigt (10.08.2026)

**Die Waffe folgt deinem Controller, eins zu eins, in Position und Ausrichtung.**
Du richtest die Hand, und die Waffe zeigt dorthin. Du bewegst die Hand, und die
Waffe geht mit. Du kannst sie heben, senken und vor dem Körper schwenken. Das ist
kein schwebendes Overlay und kein Zusatzmodell: es ist die Waffe des Spiels
selbst, platziert von den Systemen des Spiels, der nur gesagt wird, wohin sie soll.

Dahin zu kommen hieß, viererlei einzeln im Headset zu bestätigen: dass die Waffe
überhaupt bewegt werden kann, welcher Knochen der Spielfigur sie tatsächlich
trägt, welche Achse dieses Knochens der Lauf ist, und dann den Lauf direkt auf den
Strahl des Controllers zu setzen, statt ihn relativ zur bisherigen Zielrichtung zu
verschieben. Zwei dieser vier Punkte waren zuvor angenommen worden, und beide
Annahmen waren falsch. Das ist der wesentliche Grund, warum es so lange gedauert hat.

### Was noch kommt

**Die Kugeln folgen deinem Blick statt der Waffe**, über die Visierung (ADS) zu
zielen bleibt also der genaue Weg zu treffen. Das ist das letzte Stück, und es ist
nah dran: die Arbeit ist inzwischen auf einen einzigen identifizierten Kandidaten
eingegrenzt, nachdem drei andere Mechanismen jeweils getestet, als tatsächlich
ausgeführt bestätigt und mit Beleg ausgeschlossen wurden. **Darum geht es in der
nächsten Version.**

**Die Hüftfeuer-Streuung ist unangetastet**, ein korrekt gerichteter Lauf streut
also trotzdem. **Die Arme deiner Figur folgen der Waffe nicht**, die Waffe kann
also losgelöst vom Körper wirken. **Es gibt weiterhin keine Hände, keine Gesten
und keine Waffenhandhabung**: kein Greifen, kein Nachladen per Geste, kein
Magazinwechsel, kein beidhändiger Griff. Nachladen, Waffenwechsel und
Fahrzeugeinstieg sind ganz normale Tastendrücke.

### Alles andere an der Steuerung

Abgesehen von der Waffe werden deine Touch-Controller weiterhin als **emuliertes
Gamepad** gelesen: Sticks, Trigger, Griffe und Tasten werden zu gewöhnlichen
Gamepad-Eingaben, ein echtes Gamepad wird also nicht gebraucht. Dein Kopf zielt und
schaut 1:1, und zwei Handmarkierungen werden dort gezeichnet, wo deine Controller
sind, mit echter Stereotiefe.

Dieser Teil ist Gamepad-Emulation und keine Bewegungssteuerung, und diese Seite
wird ihn auch nicht anders nennen. Wer die Waffenfunktion abschaltet
(`wgun = 0` in der Konfiguration), bekommt exakt das Verhalten von v0.7.0 zurück.

**Zielen über die Visierung (ADS) bleibt beim Kopfzielen stimmig**, denn das Spiel
zeichnet sein Visierbild in der Bildmitte, und genau dorthin fliegt die Kugel. Da
die Kugeln deinem Blick folgen, bleibt ADS in dieser Version der genaue Weg zu
treffen.

### Warum das so lange dauert

Wildlands ist eine geschlossene AAA-Engine von 2017: kein Quellcode, kein SDK,
keine Mod-Schnittstelle, dazu ein Kopierschutz. Und es ist ein Third-Person-Spiel,
es gibt also kein Ego-Rig, von dem man sich etwas leihen könnte. Nirgends ist
dokumentiert, wie diese Engine eine Waffe platziert, einen Schuss ausrichtet oder
ein Skelett stellt. Jede Adresse, die der Mod benutzt, wurde aus der
ausgelieferten Programmdatei herausgearbeitet, und jede wird im Headset bestätigt,
bevor ihr vertraut wird, denn eine plausibel aussehende falsche Antwort kostet
Tage, bis man sie widerlegt hat.

Deshalb scheitert der Mod lieber sicher, als zu raten: was sich auf deiner
Spielversion nicht verifizieren lässt, schaltet sich selbst ab und schreibt das
ins Log, statt an eine Adresse zu schreiben, bei der es sich nicht sicher ist.

Fortschritt kommt daher in einzelnen bestätigten Schritten statt gleichmäßig. Der
Vorteil: was hier als funktionierend steht, funktioniert wirklich und wurde durch
ein Headset dabei beobachtet, nicht aus einem Log erschlossen.

## Bekannte Einschränkungen

- **Das Ausblenden des Kopfes fehlt auf dem Update von 08/2026 vorübergehend.**
- **Die Kugeln folgen deinem Blick, nicht der Waffe**, ziele also über die
  Visierung, um genau zu treffen. Das letzte Stück, und Thema der nächsten Version.
- **Die Waffe sitzt eventuell nicht genau in der Faust.** Sie wird an dem Punkt
  platziert, an dem die Engine sie befestigt, also nahe am Verschluss. Ein
  Griffversatz kommt noch; bis dahin regelt `wgun_pos_scale` die Reichweite.
- Keine Hände, keine Gesten, keine Waffenhandhabung. Die Arme folgen der Waffe nicht.
- Die Kamera hängt sich nach Respawn oder Schnellreise gelegentlich an den
  falschen Körper. Ego-Perspektive aus- und wieder einschalten behebt das.
- Fahrzeuge in der Ego-Perspektive sind unfertig. Bodenfahrzeuge sind spielbar,
  Innenräume von Flugzeugen und Hubschraubern noch nicht.
- Weitwinkel kann zu den Rändern hin verzerrt wirken; die Projektionsgeometrie
  wird noch abgestimmt.
- In dichten Städten fällt die Bildrate auf dem Testsystem unter 72.
- Das Desktop-Bild zeigt ein beschnittenes einzelnes Auge. Beurteile das Bild nur
  im Headset.
- Getestet auf genau einer Konfiguration. Andere Headsets und Runtimes sind
  ungetestet.

## Welche Spielversion brauche ich?

**Das aktuelle, aktualisierte Spiel (Patch "Last Rites", August 2026) ist auf Steam
im Headset verifiziert.** Seit diesem Update liefern Steam und Ubisoft Connect die
byteidentische Programmdatei aus, Ubisoft-Connect-Installationen sind also von
derselben verifizierten Adresstabelle abgedeckt.

Der Mod findet Kamera- und Projektionscode an bestimmten Adressen in `GRW.exe`.
Jede eigenständige Version dieser Datei braucht daher ihre eigene verifizierte
Tabelle. Diese Version bringt drei mit: die Steam-Version vor dem Update
(2023-09-14), die Epic-/Ubisoft-Connect-Version vor dem Update (2023-09-08, offline
maschinell verifiziert, nie von einem Store-Nutzer im Headset bestätigt) und die
aktuelle Version des Updates von 08/2026.

Trifft der Mod auf eine `GRW.exe`, die er nicht kennt, schreibt er das ins Log,
nennt die ihm bekannten Versionen und **installiert nichts**: dein Spiel läuft dann
völlig unverändert. Das Symptom ist ein kleines flaches Fenster im Headset, das
nicht auf Kopfbewegungen reagiert. Prüfe in `GRWVR\grwxr-<pid>.log` die Zeile
"build pin:".

## Voraussetzungen

- Ghost Recon Wildlands, aktuelle Version (Update "Last Rites", August 2026),
  Steam oder Ubisoft Connect.
- Ein PC-VR-Headset mit OpenXR-Runtime. Getestet ausschließlich mit Meta Quest 3
  über Link-Kabel und der Meta-Quest-Link-Runtime.
- **Asynchronous Spacewarp muss deaktiviert sein** (Oculus Debug Tool, ASW auf
  Disabled). Der Mod verwaltet das jeweils ältere Auge selbst; ASW verstärkt die
  Artefakte zusätzlich.
- Eine GPU mit Reserven: das Testsystem ist eine RTX 5060 Ti 16 GB mit Ryzen 7 9700X.
- Zum Selbstkompilieren: Visual Studio 2022 oder neuer mit C++-Workload
  (MSVC x64, `ml64`).

## Schnellinstallation (ohne Kompilieren)

Lade das aktuelle Release-Zip von der
[Releases-Seite](https://github.com/Firejumper93/GhostReconWildlandsVR/releases),
entpacke es an einen beliebigen Ort, führe `install.bat` aus und lies die
beiliegende `INSTALL.txt`. Anleitung zum Selbstkompilieren: siehe
[englische Fassung](README.md#building).

## Basis-Grafikeinstellungen (damit anfangen, bevor du irgendetwas beurteilst)

Das ist die getestete Ausgangslage. Prüfe sie, bevor du ein Grafik- oder
Leistungsproblem meldest: zwei dieser Einstellungen (Fenstermodus und Kantenglättung)
können unbemerkt die halbe Bildrate kosten.

Die Video-Einstellungen im Spiel schreiben in `GRW.ini` unter
`Dokumente\My Games\Ghost Recon Wildlands`:

| Einstellung | Wert | Warum |
|---|---|---|
| Auflösung / Fenster | 1920x1080, Vollbild (`WindowMode=1`) | Der Mod rendert intern in 4K, unabhängig von der Desktopgröße. Ein Fenster MIT Rahmen bindet das Spiel an die Bildwiederholrate des Monitors und deckelt VR bei 60 |
| Bildratenbegrenzung | 72 (`FpsLimit=72`) | Passt zur Quest-3-Bildwiederholrate, auf die der Mod taktet |
| Supersampling | 0,90 (`Supersampling=0.90`) | 1,00 sprengt das Budget der getesteten GPU bei 4K; 0,90 hält 72 fps |
| Kantenglättung | **SMAA oder Aus. NIEMALS ein temporaler (TAA) Modus** (`AntiAliasingMode=3` ist SMAA, `0` ist aus) | Temporales AA vermischt die abwechselnden Augenperspektiven zu Geisterbildern und ist teuer: mit TAA fiel die Bildrate dauerhaft von 72 in die unteren 60er |
| Bewegungsunschärfe | Aus | Verschmiert beim Headtracking |

Zwei Fallen, beide auf dem Testsystem beobachtet:

- **Das Spiel schreibt `GRW.ini` neu, sobald du im Menü irgendetwas übernimmst**,
  und hat dabei schon stillschweigend `FpsLimit` gelöscht. Prüfe die Datei nach
  jeder Menüänderung erneut.
- **Alte Spielstände können alte Einstellungen mitbringen.** Ein Spielstand von vor
  dieser Ausgangslage lud mit aktivem TAA und las sich als "der Mod ruckelt jetzt".

Und für die aktuelle Spielversion: das Update hat FSR-Upscaling gebracht,
**lass FSR aus**, solange du den Mod nutzt.

## Im Headset

Es gibt nur noch DREI Tastenkürzel. Alles andere wird über `GRWVR\grwxr.cfg`
eingestellt (Hot Reload etwa eine Sekunde nach dem Speichern) oder über die
mitgelieferte Regler-Oberfläche `tools\cfg_gui\cfg_gui.exe`.

| Taste | Funktion |
|---|---|
| Pos1 (Home) | Neu zentrieren (schau dorthin, wo vorne sein soll, dann drücken) |
| Numpad 8 | Ego-Perspektive an / aus (zentriert zugleich neu) |
| Numpad , (Entf) | 1:1-Kopfzielen an / aus (Kugeln folgen deinem Blick; Standard aus) |

### Touch-Controller (als Gamepad emuliert)

Der Mod führt die Touch-Controller als Gamepad ins Spiel, das gesamte normale
Steuerungsschema funktioniert also und ein echtes Gamepad ist nicht nötig. Um es
deutlich zu sagen: das ist Gamepad-Emulation, keine Bewegungssteuerung.

| Eingabe | Funktion |
|---|---|
| Kopf | Zielen: Kugeln folgen deinem Blick (Umschalten mit Numpad Komma) |
| Beide Controller | Handmarkierungen: blauer (links) und oranger (rechts) Punkt |
| Rechter Trigger, ganz durchgezogen | Feuern (halten für Dauerfeuer) |
| Linker Trigger (halten) | Über die Visierung zielen (ADS) |

Ein echtes Gamepad funktioniert weiterhin, falls du das bevorzugst.

## Konfiguration

`GRWVR\grwxr.cfg` im Spielordner enthält die dauerhaften Einstellungen und
dokumentiert jeden Schlüssel in Kommentaren. Die wichtigsten:

| Schlüssel | Bedeutung |
|---|---|
| `ipd_scale` | Augenabstands-Faktor (Standard 0,50) |
| `fullscreen_fov` | Gerendertes Sichtfeld in Radiant (Standard 1,92) |
| `upsize_width` / `upsize_height` | Interne Renderauflösung (Standard 3840x2160). Niedriger, etwa 3200x1800, tauscht Schärfe gegen Bildrate |
| `fp_head_anchor` | `1` (Standard) verankert die Ego-Perspektive am Kopfknochen |
| `fp_head_eye` | Augenversatz über dem Kopfknochen in Metern (Standard 0,10) |
| `aim_yaw_sign`, `aim_pitch_sign` | Richtungskalibrierung des Kopfzielens. Nur umdrehen, wenn das Zielen auf einer Achse falsch herum läuft |
| `aim_source` | `0` (Standard) Zielen folgt dem Kopf; `1` ist das zurückgezogene Controller-Experiment |
| `aim_reticle` | `1` (Standard) zeichnet den Hüftfeuer-Punkt; `0` blendet ihn aus |
| `xinput_touch` | `1` (Standard) führt die Touch-Controller als Gamepad zusammen |
| `hand_markers` | `1` (Standard) zeichnet die beiden Handpunkte |
| `desktop_fov` | Beschnitt der Desktop-Aufnahmeansicht, `0` deaktiviert |

Die vollständige Liste steht in der [englischen Fassung](README.md#configuration)
und in den Kommentaren der Konfigurationsdatei selbst.

## Deaktivieren und deinstallieren

- Vorübergehend deaktivieren: `dxgi.dll` im Spielordner umbenennen (etwa in
  `dxgi.dll.off`). Das Spiel läuft dann völlig unverändert.
- Deinstallieren: `deploy.bat remove` ausführen, oder `dxgi.dll`, `dxgi_real.dll`,
  `openxr_loader.dll` und den Ordner `GRWVR` aus dem Spielverzeichnis löschen.

## Nutzungsregeln

- **Nur Solo-Kampagne. Niemals in Koop, PvP oder Matchmaking verwenden.** Das Spiel
  setzt im Mehrspielermodus Easy Anti-Cheat ein; dieser Mod darf dort niemals laufen.
- Vorerst wird der Offline-Modus empfohlen (Steam offline oder Ubisoft Connect
  offline). Das hält die Sitzung eindeutig im Einzelspieler.
- Dieses Repository enthält ausschließlich Quellcode. Keine Spieldateien, keine
  Ubisoft-Binärdateien, keine Anti-Cheat-Bestandteile, und es verändert niemals eine
  Datei deiner Installation.
- Nutzung auf eigene Gefahr. Der Kopierschutz des Spiels reagiert gelegentlich
  unvorhersehbar auf Hooks. Wenn etwas kaputt aussieht, benenne `dxgi.dll` um und
  prüfe erneut, bevor du Spiel oder Mod verdächtigst.

## Danksagungen

- **[mutars/anvilengine2vr](https://github.com/mutars/anvilengine2vr)** (MIT): die
  Referenzimplementierung, von der dieses Projekt eine Portierung ist.
- **[elliotttate/vrframework](https://github.com/elliotttate/vrframework)**: die
  Feldhandbücher zu dieser Technikfamilie für AnvilNext-Titel.
- **[dariulone/cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port)**
  (MIT) und **[pancreations/Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR)**
  (MIT): verwandte Architekturen, deren dokumentierte Erfahrungen die Diagnose des
  hartnäckigsten Fehlers in diesem Mod ermöglicht haben.
- **[Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)** (Apache 2.0):
  Loader und Header für OpenXR.
- **SolKutTeR und [vrforum.de](https://vrforum.de)**, für den
  [Thread über diesen Mod](https://vrforum.de/threads/ghost-recon-wildlands-grw-xr.14507/)
  und dafür, deutschsprachige Spielerinnen und Spieler darauf aufmerksam gemacht zu
  haben. Dieser Thread ist der Grund, warum es diese deutsche Fassung gibt.
- **아키아PhD**, für den
  [Beitrag über diesen Mod im Naver Cafe](https://cafe.naver.com/ca-fe/cafes/27902572/articles/252399)
  und dafür, koreanischsprachige Spielerinnen und Spieler darauf aufmerksam gemacht zu
  haben. Dieser Beitrag ist der Grund, warum es die koreanische Fassung gibt
  ([README.ko.md](README.ko.md)).
- Tom Clancy's Ghost Recon Wildlands ist Eigentum von Ubisoft. Dieses Projekt steht
  in keiner Verbindung zu Ubisoft, wird von Ubisoft weder unterstützt noch
  gebilligt, und verbreitet keine Inhalte von Ubisoft.

## Das Projekt unterstützen

Einige haben gefragt, ob sie spenden können; dafür gibt es jetzt eine Seite:
[buymeacoffee.com/firejumper93](https://buymeacoffee.com/firejumper93). Völlig
freiwillig und niemals erforderlich: der Mod ist kostenlos, nichts ist hinter
Spenden verborgen, und eine Spende ändert nichts an dem, was man bekommt. Sie
hilft lediglich bei Werkzeugen und langen Nächten, und sie wird geschätzt.

## Lizenz

MIT, siehe [LICENSE](LICENSE). Teile abgeleitet von anvilengine2vr,
Copyright (c) 2024 mutars, MIT.
