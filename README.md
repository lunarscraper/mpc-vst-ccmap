# mpc-vst-ccmap

**CC Map** – MIDI-Controller-Mapper als VST2-Insert-Effekt für die Akai Force (MPC OS). Liest die
Novation Launch Control (MK1), den Evolution UC-33 und den Faderfox EC4 direkt über ALSA, legt jeden
Regler, Fader und jedes Pad per LEARN auf ein frei wählbares Ziel und sendet das Ergebnis über eigene
MIDI-Ports. Der EC4 bekommt die Werte zurück (Anzeige stimmt, keine Sprünge), die Potis von Launch
Control und UC-33 übernehmen per Pickup.

**Status:** Version 0.9.0, offline getestet (x86, ASan/UBSan). 0.1.0 läuft auf der Force (Presets erscheinen in der Liste).
0.2.0: Rückkanal zum EC4, Pickup, Relativ-Encoder, Tab SYNC.
0.3.0: Launch-Control-Template an Farbe und Pad-Nummer erkennbar, Anzeige LC TEMPLATE.
0.4.0: OXI E16 als eigenes Gerät mit Rückkanal (wie der EC4).
0.5.0: Handy-Ansicht im WLAN der Force, Namen für Slots (läuft auf Handy und iPad).
0.6.0: Regler und Pads auf dem Handy bedienbar, Stems-Download aus dem Ordner `Stems`.
0.7.0: gemeinsamer Hub mit den anderen Force-Plugins (z. B. Matrix): eine Adresse, eine Kachel pro Plugin.
0.7.1: Werk-Preset „LC > DRUMGEN (CH 15)“ für DrumGen / DrumGen FX.
0.8.0: **Baukasten** – eigene Seiten mit Fadern, Drehreglern, Tastern, Schaltern und XY-Pads auf dem Handy/iPad bauen, jede mit eigener CC, eigenem Kanal und Ausgang (eigene Kachel im Hub).
0.9.0: Baukasten-Element **Picker** – zeigt einen Wert, antippen öffnet eine Rolle zum Drehen, optional mit Namen für die Werte.
Projekte und Presets älterer Versionen laden weiter.

## Warum ein Insert-Effekt

- Belegt keinen der 8 Instrument-Slots, das Audio der Spur geht unverändert durch.
- Auf eine beliebige Spur (oder den Master) legen. Das Mapping läuft, solange das Projekt das Plugin geladen hat.
- Pro Projekt nur **eine** Instanz: Eine zweite lädt, bleibt aber passiv, damit nichts doppelt gesendet wird.

## Signalweg

```
Launch Control / UC-33 / anderer USB-Controller
        │  versteckter Eingang: CC Map verbindet sich selbst mit allen Hardware-MIDI-Eingängen
        ▼  (auch später eingesteckten) und erkennt die Geräte am Namen
   128 Slots, 4 Seiten (LEARN, CC / NOTE / PROGRAM / PAGE, MIN..MAX, MOMENT/TOGGLE)
        │
        ├──► Ports „Out A" … „Out D"   → als MIDI-Eingang einer Spur oder als Ausgang einer MIDI-Spur wählen
        ├──► „Link A": Out A zusätzlich an alle „Control In"-Ports anderer Plugins
        │     (Euclidier FX, Acid FX, Maze FX …), ganz ohne Routing
        ├──► LEDs der Launch Control (Pads zeigen ihren Slot, Pfeile die Seiten)
        └──► Rückkanal: gespeicherte Positionen zurück an den EC4
```

## Einrichtung auf der Force

1. Plugin installieren (Release-ZIP aus GitHub Actions, wie bei den anderen Plugins).
2. **CC Map** als Insert-Effekt auf eine Spur legen.
3. *Preferences → MIDI*: Bei den neuen Eingängen **CC Map Out A … D** „Track" einschalten.
4. Auf den Zielspuren als MIDI-Eingang **CC Map Out A** (oder B–D) wählen.
5. Die Controller selbst (Launch Control, UC-33) bei *Preferences → MIDI* **nicht** als Track-Eingang
   einschalten, sonst kommen ihre Rohdaten zusätzlich an. CC Map liest sie trotzdem.

Diagnose: `/tmp/ccmap_vst.log` – welche Geräte erkannt wurden, Verbindungen, alle 5 s eine Zeile mit
ein- und ausgehenden Nachrichten.

## Bedienung

### Tab MAP – Slot bearbeiten

| Element | Funktion |
|---|---|
| SLOT | Wählt einen der 128 Slots. Anzeige: Nummer und Quelle |
| SOURCE | Die gelernte Quelle, z. B. `LC U1 KNOB 5`, `LC U1 PAD 3`, `UC CH3 CC 7` |
| LEARN | Einmal tippen, dann am Controller drehen oder ein Pad drücken. Nochmals tippen bricht ab |
| PAGE | Seite des Slots: ALL oder 1–4 |
| OUT | Ausgangsport A–D |
| CHANNEL | MIDI-Kanal des Ziels |
| TYPE | OFF, CC, NOTE, PROGRAM, PAGE |
| NUMBER | CC- oder Notennummer. Bei PAGE: 0 = nächste Seite, 1–4 = diese Seite |

Ein frisch gelernter Slot gibt den Regler unverändert weiter (Out A, gleicher Kanal, gleiche Nummer).
Danach einfach das Ziel umstellen.

### Tab RANGE

| Element | Funktion |
|---|---|
| MIN / MAX | Wertebereich des Ziels. MIN größer als MAX dreht die Richtung um |
| BUTTON | MOMENT (Wert folgt der Taste) oder TOGGLE (jeder Druck schaltet um) |
| ENCODER | ABSOLUTE, REL 64 (64 = Stillstand, 65 = +1, 63 = −1) oder REL 2C (1 = +1, 127 = −1) |
| CLEAR | Löscht den gewählten Slot |
| LAST | Zeigt die zuletzt eingegangene Bewegung, z. B. `LC U1 KNOB 5 = 64` |

PROGRAM sendet den Wert selbst als Programmnummer, also z. B. MIN 0 / MAX 7 für 8 Programme auf einem Fader.

### Tab SETUP

| Element | Funktion |
|---|---|
| ACTIVE PAGE | Aktive Seite 1–4 |
| FOLLOW | ON: Wer einen gemappten Regler bewegt, bekommt dessen Slot im Editor angezeigt |
| LEDS | OFF, ON (nur Slot-Zustände) oder ON + TEMPLATE (zusätzlich Template-Farbe, Voreinstellung) |
| UNMAPPED | DROP oder THRU A (nicht gemappte Bewegungen unverändert an Out A) |
| LINK A | CONTROL IN: Out A speist alle „Control In"-Ports anderer Plugins |
| INPUT | MY CONTROLLERS (Launch Control, UC-33, EC4, E16) oder ALL USB (jeder USB-Controller außer der Force selbst) |
| ARROWS | PAGE/SLOT oder MAPPABLE (Pfeiltasten frei belegbar) |
| LC TEMPLATE | Aktives Template der Launch Control, z. B. `USER 3` |

### Tab SYNC – Rückkanal und Pickup

Jeder CC-Slot merkt sich, wo sein Regler steht. Die Position wird mit Projekt und Preset gespeichert.

| Element | Funktion |
|---|---|
| FEEDBACK | OFF, EC4 + E16 oder + ALL USB (auch andere USB-Controller bekommen ihre Werte zurück) |
| TAKEOVER | JUMP oder PICKUP für Potis ohne Rückkanal (Launch Control, UC-33) |
| SEND VALUES | Schickt die Positionen der aktiven Seite sofort an den EC4 |
| LAST | Letzte Bewegung, bei Pickup das Ziel: `LC U1 KNOB 1: TO 100` |

- **Rückkanal:** Bei Seitenwechsel, beim Laden von Projekt oder Preset und beim Einstecken des EC4
  bekommt jeder Encoder der Seite den Wert seines Slots (gleicher Kanal, gleiche CC wie gelernt).
- **Pickup:** Nach einem Seitenwechsel übernimmt ein Poti den Slot erst, wenn es dessen Wert erreicht
  oder überfahren hat. Tasten und die Endanschläge (0 / 127) übernehmen sofort.
- Relativ-Slots brauchen keinen Rückkanal und bekommen keinen.

### Tab PRESET

32 Speicherplätze nach Rattler-Muster: PRESET-Regler blättert, LOAD lädt, SAVE speichert. Die Plätze
erscheinen auch in der Preset-Liste der Force. Datei: `ccmap_presets.txt` neben dem Plugin-Ordner.

Ein Preset ist die **ganze Belegung**: alle 128 Slots, die Einstellungen und die gemerkten
Reglerpositionen. Mit dem Projekt wird die aktuelle Belegung ohnehin gespeichert, Presets braucht
man also nur, um eine Belegung in andere Projekte mitzunehmen. Ein leerer Platz lässt beim Laden
alles, wie es ist.

**Empfehlung – ein Plugin pro Template:** Weil jeder Slot an das Template gebunden ist, auf dem er
gelernt wurde, passen mehrere Plugins in **ein** Preset: z. B. USER 1 = Euclidier FX, USER 2 = Acid,
USER 3 = Rattler. Das Template am Gerät umschalten wechselt das Plugin, ohne Preset-Wechsel. Einzelne
Presets pro Plugin gehen genauso, dann aber immer nur eins zur Zeit.

| Platz | Werk-Preset |
|---|---|
| 1 | **LC > EUCLIDIER FX**: obere Reihe = Steps Lane 1–8, untere Reihe = Fill, Pads = Lane an/aus (Toggle). Kanal 16, LINK A an |
| 2 | **LC 4 PAGES > CH 1-4**: 16 Regler und 8 Pads unverändert, Seite 1–4 = Kanal 1–4 auf Out A |
| 3 | **LC 4 PAGES > OUT A-D**: wie 2, aber Seite 1–4 = Out A–D, Kanal 1 (vier Synth-Spuren, je ein Port) |
| 4 | **EMPTY (LEARN)**: leere Belegung |
| 5 | **LC > DRUMGEN (CH 15)**: DrumGen oder DrumGen FX, LINK A an. **In DrumGen CONTROL CH auf 15 stellen** (ab Werk 16, den nutzt schon das Euclidier-Preset mit denselben CC-Nummern). Seite 1 PLAY: oben Density, unten Pattern der Lanes 1–8, Pads = Lane an/aus. Seite 2 LANES: oben Length, unten Note, Pads = Dice (neu würfeln). Seite 3 GLOBAL: oben Style, Random, Swing, Gate, Auto, Remix Type, Remix Every, Echo Time; unten Echo Repeats, Echo Prob, Echo Falloff, Glitch Repeats, Glitch Gate, Glitch Prob, Glitch Random, Preset; Pads = Generate, Variate, Remix an/aus, Echo an/aus, Glitch an/aus, Load Preset. Seite 4: Pads = Lock der Lanes 1–8, Regler frei zum Lernen |

## Launch Control (MK1)

- Kanal = Template: User 1–8 = Kanal 1–8, Factory 1–8 = Kanal 9–16. Ein Slot gilt nur für das
  Template, auf dem er gelernt wurde. Die 16 Templates sind damit 16 zusätzliche Bänke.
- Standardbelegung (Werk-Presets setzen sie voraus): Regler CC 21–28 und 41–48, Pads Noten 9–12 und
  25–28, Pfeile CC 114–117. Bei geänderten Templates einfach neu lernen.
- Pfeile (ARROWS = PAGE/SLOT): links/rechts = Seite, hoch/runter = Slot im Editor.
- LEDs per SysEx: Pad mit TOGGLE = grün (an) / schwach rot (aus), Pad mit MOMENT = schwach grün,
  gedrückt amber, PAGE-Pad = amber, Pad im LEARN = rot, Pfeile zeigen, ob es links/rechts weitere Seiten gibt.
- Ein Template-Wechsel am Gerät wird erkannt (SysEx), die LEDs folgen.
- **Template erkennen (LEDS = ON + TEMPLATE):** Jedes Template 1–8 hat eine eigene Farbe, die die
  Pads in Ruhe zeigen (gemappte Pads; ist auf dem Template kein Pad gemappt, alle 8):

  | Template | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
  |---|---|---|---|---|---|---|---|---|
  | Farbe | Rot | Orange | Amber | Gelbgrün | Grün | schwach Rot | schwach Amber | schwach Grün |

  Hoch/Runter leuchten rot bei USER, grün bei FACTORY. Beim Umschalten leuchtet gut eine Sekunde
  lang nur das Pad mit der Template-Nummer (rot = USER, grün = FACTORY). Zustände bleiben in voller
  Helligkeit: Toggle an = grün, gedrückt = amber, LEARN = rot.

## Evolution UC-33

- Wird class compliant erkannt. CC Map erkennt ihn am Namen („UC-33") und liest alle seine Ports.
- Er hat keine LEDs zum Ansteuern, Rückmeldung gibt es nur im Skin (SOURCE, LAST).
- Am besten den UC-33 auf ein Preset mit eindeutigen CCs stellen und **nicht mehr umprogrammieren**.
  Die ganze Zuordnung liegt in CC Map, pro Projekt und in den 32 Presets.
- Seitenwechsel ohne Pfeiltasten: eine Taste lernen und TYPE = PAGE setzen.

## Faderfox EC4

- Wird am Namen („EC4") erkannt. CC Map liest ihn und schreibt die Werte auf seinen Eingang zurück.
- Am EC4 die Encoder auf **CC absolut (CCAb)** lassen, 7 Bit. Dann zeigt er nach jedem Seiten- oder
  Presetwechsel den richtigen Wert, und nichts springt.
- Die Relativ-Modi des EC4 (CCR1, CCR2) gehen auch: im Slot ENCODER auf REL 64 oder REL 2C stellen.
  Welcher EC4-Modus welcher Kodierung entspricht, im Zweifel ausprobieren: Dreht der Wert falsch herum
  oder springt er, den anderen nehmen.
- Gruppen und Setups des EC4 senden andere CCs/Kanäle, sie brauchen also keine Seiten in CC Map:
  einfach jede Gruppe lernen, Seite ALL lassen.
- Die Push-Taster der Encoder (z. B. Typ Note) lassen sich wie Pads lernen, etwa als TOGGLE oder als PAGE.

## Handy-Ansicht (Tab SYNC)

CC Map stellt im WLAN der Force eine Webseite bereit. Darauf siehst du die Launch Control des aktiven
Templates und der aktiven Seite: jeden Regler mit Namen, Ziel (z. B. `CC 74 / CH 5 / A`) und Stellung,
die Pads in ihrer LED-Farbe, darunter alle Slots der übrigen Controller (EC4, E16, UC-33 …). Der zuletzt
bewegte Regler ist gelb umrandet. Die Seite aktualisiert sich etwa viermal pro Sekunde.

So öffnest du sie:
1. Die Force ins WLAN bringen (Einstellungen der Force → WLAN).
2. In CC Map den Tab **SYNC** öffnen: Unter „OPEN ON PHONE: HTTP://" steht die Adresse, z. B.
   `192.168.178.40:8000`.
3. Das Handy ins selbe WLAN, im Browser `http://` und die Adresse eingeben. Es erscheint der **Hub**:
   eine Kachel für jedes Plugin mit Handy-Ansicht, das gerade auf der Force läuft (CC Map, Matrix …).
   Kachel antippen öffnet das Plugin, das Haus-Symbol oben links führt zurück. Tipp: den Hub als
   Lesezeichen oder „Zum Home-Bildschirm" sichern – er gilt für alle Plugins.

**Hub und Ports:** Jedes Plugin nimmt den ersten freien Port von 8000 bis 8009 (wer zuerst lädt, bekommt
8000). Jede dieser Adressen zeigt denselben Hub; welcher Port wem gehört, steht klein auf der Kachel.
Es läuft kein zusätzlicher Dienst, jedes Plugin bringt seinen Teil selbst mit (`forcehub.h`).

**Spielen vom Handy:** Auf dem Bogen eines Reglers auf- und abwischen stellt den Wert ein (die ganze
Strecke ≈ 220 Pixel), ein Pad antippen drückt es (TOGGLE schaltet um). Das Ziel bekommt den Wert wie
von der Hardware; EC4/E16 bekommen ihn zurückgemeldet, ein Poti der Launch Control / des UC-33 muss
den neuen Wert danach per Pickup wieder abholen (LAST zeigt `… : TO 100`).

**Namen vergeben:** Auf dem Handy den Namen unter einem Regler antippen, Namen eintippen, „Speichern". Der Name
erscheint auch auf der Force (SLOT-Anzeige und LAST, z. B. `Clem Cutoff = 87`) und wird mit Projekt und
Preset gespeichert. Die Werk-Presets „LC > EUCLIDIER FX" und „LC > DRUMGEN (CH 15)" bringen ihre Namen schon mit.

Hinweise:
- **Stems herunterladen (Tab „Stems" auf der Seite):** Lege auf der SD-Karte (oberste Ebene) einen
  Ordner `Stems` an und exportiere die Stems dorthin, gern mit einem Unterordner pro Song. Mit
  **STEMS DOWNLOAD = ON** (Tab SYNC, Voreinstellung OFF) zeigt die Seite diesen Ordner: jede Datei
  einzeln laden oder „Alle als ZIP laden" (ohne Kompression, bis 4 GB pro ZIP). Nur dieser Ordner,
  nur Audio-/MIDI-Dateien, nur lesen. Downloads laufen mit niedrigster Priorität – trotzdem besser
  nicht während des Live-Spiels.
- **PHONE VIEW** (Tab SYNC) schaltet die Seite ab. Jeder im selben WLAN kann sie öffnen, Namen
  ändern und die Regler bewegen (und mit STEMS DOWNLOAD die Stems laden); im Heimnetz unkritisch, in fremden Netzen lieber abschalten.
- Steht dort `NO NETWORK`, ist die Force nicht im WLAN; die Adresse erscheint, sobald sie verbunden ist.
- Lässt sich die Seite nicht öffnen, obwohl eine Adresse angezeigt wird, blockiert vermutlich die Force
  den Port; dann bitte `/tmp/ccmap_vst.log` schicken.

## Baukasten (Hub-Kachel „Baukasten")

Im Hub erscheint neben CC Map eine zweite Kachel **Baukasten**. Dort stellst du dir eigene Bedienseiten
zusammen – unabhängig von Hardware-Controllern. Jedes Element sendet **seine eigene CC auf seinem eigenen
Kanal** über einen der Ausgänge **Out A–D** von CC Map (mit LINK A also auch direkt an alle „Control In“-Ports).

| Element | Spielen |
|---|---|
| Fader | Position antippen oder ziehen (Wert = Fingerposition) |
| Drehregler | auf- und abwischen (relativ, springt nicht; ganze Strecke ≈ 200 Pixel) |
| Taster | gedrückt = MAX, losgelassen = MIN |
| Schalter | jeder Tipp schaltet zwischen MIN und MAX |
| XY-Pad | im Feld ziehen: X sendet CC, Y sendet CC Y |
| Picker | zeigt den aktuellen Wert; antippen öffnet eine **Rolle**: nach oben/unten wischen, der Wert im Rahmen wird sofort gesendet. ▲/▼ gehen einen Wert weiter, ein Wert antippen springt hin, „Fertig“ oder daneben tippen schließt |

**Bauen:** Oben „Bearbeiten“ tippen. „+ Seite“ legt eine Seite an (Handy 4 Spalten, iPad 8), „+ Element“
fügt ein Element hinzu, ein Element antippen öffnet seine Einstellungen: Typ, Name, Farbe, Ausgang A–D,
Kanal 1–16, CC (beim XY-Pad zwei), Min/Max (Min größer als Max dreht um), Breite und Höhe im Raster.
Beim **Picker** stattdessen: erster und letzter Wert, **Schritt** (z. B. 0 bis 48 in Schritten von 16 = 0, 16, 32, 48)
und optional **Namen der Werte**, durch Komma getrennt (`Sinus, Säge, Rechteck, Rauschen`): der erste Name gehört
zum ersten Wert usw.; Werte ohne Namen zeigen die Zahl. Der Picker sendet genau den gewählten Wert. Die Force
zeigt in LAST den Namen, z. B. `Wave = Rechteck (32)`.
Dort auch: früher/später einsortieren, duplizieren, löschen. Die Zeile unten im Editor sagt in Klartext,
was das Element sendet, und warnt, wenn dieselbe CC auf demselben Kanal schon auf der Seite liegt.
„Seite einstellen“ ändert Name und Spaltenzahl, verschiebt oder löscht die Seite. „Fertig“ schaltet zurück
zum Spielen.

**Aus CC Map übernehmen:** macht aus allen CC-Slots der aktiven CC-Map-Seite eine fertige Baukasten-Seite
(Namen, Kanal, CC, Min/Max übernommen; Pads werden Taster bzw. Schalter, Launch-Control-/EC4-/E16-Regler
Drehregler, alles andere Fader). NOTE-, PROGRAM- und PAGE-Slots werden übersprungen.

- Gespeichert wird bei jeder Änderung in **`ccmap_kit.txt`** (Dateien von 0.8.0 laden weiter) neben `ccmap_presets.txt` – der Baukasten gilt
  für alle Projekte. Höchstens 16 Seiten mit je 48 Elementen.
- Die Stellungen merkt sich nur das laufende Plugin: Nach einem Neustart zeigen die Elemente „–“, bis sie
  bewegt werden. Ein Drehregler ohne bekannte Stellung startet in der Mitte.
- Mehrere Handys/iPads gleichzeitig gehen: Jedes sieht die Bewegungen und Änderungen der anderen nach
  spätestens einer halben Sekunde.
- Die Force zeigt die letzte Baukasten-Bewegung in LAST (z. B. `Cutoff = 87`).
- Läuft, solange PHONE VIEW (Tab SYNC) eingeschaltet ist; mit PHONE VIEW OFF ist auch der Baukasten aus.
  Wie die übrige Handy-Ansicht: jeder im selben WLAN kann ihn öffnen und ändern.
- Port: der nächste freie nach CC Map (meist 8001). Weitere Hub-Plugins wie Matrix rücken einen Port weiter.

Programmierschnittstelle (alles GET, Antwort JSON): `/kit`, `/vals?p=`, `/send?p=&i=&v=[&v2=]`,
`/page?op=add|set|del|move`, `/el?op=put|del|move|dup` (Picker: `st=` Schritt, `lb=` Namen), `/import` – Einzelheiten in `ccmap_vst.cpp` bei `kit_route`.

## OXI E16

- Wird am Namen („E16") erkannt; OXI One und OXI Coral zählen weiter als normale USB-Geräte.
- Rückkanal wie beim EC4: Ringe und Display bekommen bei Seiten-/Presetwechsel, beim Laden und beim
  Einstecken die Werte der Slots. Kein Pickup nötig, nichts springt.
- Werte gehen nur an den **ersten** USB-Port des E16. Seine weiteren virtuellen Ports können zu den
  TRS-Ausgängen führen; dort sollen keine Rückmeldungen landen.
- Encoder im E16 auf **CC, 7 Bit, absolut** stellen. NRPN und 14 Bit wertet CC Map nicht aus.
  Relativ geht auch: dann im Slot ENCODER passend wählen.
- Die 12 Seiten einer E16-Szene senden eigene CCs/Kanäle: in CC Map einfach mit Seite ALL lernen.
- Die Ringfarben stellst du in der OXI App ein; CC Map setzt sie nicht.

## Dateien

| Datei | Inhalt |
|---|---|
| `vst/ccmap_core.h` | Mapping-Kern ohne VST und ALSA (Learn, Seiten, Ziele, LEDs, Zustand als Text) |
| `vst/ccmap_vst.cpp` | VST2-Hülle (handgeschrieben), ALSA-Thread, Presets, Projekt-Chunk |
| `vst/ccmap_files.h` | Stems-Ordner: Liste, Download, gestreamtes ZIP |
| `vst/forcehub.h` | gemeinsamer Hub aller Force-Plugins: HTTP-Server auf dem ersten freien Port 8000–8009, `/` Kachelseite, `/info`, `/app` (gleiche Datei in jedem Plugin-Repo) |
| `vst/ccmap_web.h`, `vst/page/index.html` | Handy-Ansicht von CC Map auf dem Hub; `gen_page.py` bettet die Seite als `ccmap_page.h` ein |
| `vst/ccmap_kit.h`, `vst/page/kit.html` | Baukasten: Seiten, Elemente, Speicherformat, Senden; die Seite wird als `ccmap_kit_page.h` eingebettet |
| `vst/module.json`, `vst/vst.json`, `vst/layout.conf` | Parameter und Skin für `gen_vst.py` |
| `vst/captions.py`, `vst/fonts/` | Beschriftungen in Titillium Web (aus mpc-vst-euclidier, SIL OFL) |
| `vst/host_test.cpp`, `vst/test.sh` | Offline-Test über Test-Hooks statt ALSA |

Build: Actions → „VST release (draft)" → Run workflow (Vorlage: mpc-vst-rattler, sd88me/mpc-vst-plugins).

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic). „Novation", „Launch Control" und „Evolution UC-33"
gehören ihren Inhabern, keine Verbindung.
