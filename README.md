# mpc-vst-ccmap

**CC Map** – MIDI-Controller-Mapper als VST2-Insert-Effekt für die Akai Force (MPC OS). Liest die
Novation Launch Control (MK1), den Evolution UC-33 und den Faderfox EC4 direkt über ALSA, legt jeden
Regler, Fader und jedes Pad per LEARN auf ein frei wählbares Ziel und sendet das Ergebnis über eigene
MIDI-Ports. Der EC4 bekommt die Werte zurück (Anzeige stimmt, keine Sprünge), die Potis von Launch
Control und UC-33 übernehmen per Pickup.

**Status:** Version 0.2.0, offline getestet (x86, ASan/UBSan), Test auf der Force steht aus.
0.2.0: Rückkanal zum EC4, Pickup, Relativ-Encoder, Tab SYNC. Projekte und Presets aus 0.1.0 laden weiter.

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
| LEDS | LEDs der Launch Control steuern |
| UNMAPPED | DROP oder THRU A (nicht gemappte Bewegungen unverändert an Out A) |
| LINK A | CONTROL IN: Out A speist alle „Control In"-Ports anderer Plugins |
| INPUT | LC UC-33 EC4 oder ALL USB (jeder USB-Controller außer der Force selbst) |
| ARROWS | PAGE/SLOT oder MAPPABLE (Pfeiltasten frei belegbar) |

### Tab SYNC – Rückkanal und Pickup

Jeder CC-Slot merkt sich, wo sein Regler steht. Die Position wird mit Projekt und Preset gespeichert.

| Element | Funktion |
|---|---|
| FEEDBACK | OFF, EC4 oder EC4 + USB (auch andere USB-Controller bekommen ihre Werte zurück) |
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

| Platz | Werk-Preset |
|---|---|
| 1 | **LC > EUCLIDIER FX**: obere Reihe = Steps Lane 1–8, untere Reihe = Fill, Pads = Lane an/aus (Toggle). Kanal 16, LINK A an |
| 2 | **LC 4 PAGES > CH 1-4**: 16 Regler und 8 Pads unverändert, Seite 1–4 = Kanal 1–4 auf Out A |
| 3 | **LC 4 PAGES > OUT A-D**: wie 2, aber Seite 1–4 = Out A–D, Kanal 1 (vier Synth-Spuren, je ein Port) |
| 4 | **EMPTY (LEARN)**: leere Belegung |

## Launch Control (MK1)

- Kanal = Template: User 1–8 = Kanal 1–8, Factory 1–8 = Kanal 9–16. Ein Slot gilt nur für das
  Template, auf dem er gelernt wurde. Die 16 Templates sind damit 16 zusätzliche Bänke.
- Standardbelegung (Werk-Presets setzen sie voraus): Regler CC 21–28 und 41–48, Pads Noten 9–12 und
  25–28, Pfeile CC 114–117. Bei geänderten Templates einfach neu lernen.
- Pfeile (ARROWS = PAGE/SLOT): links/rechts = Seite, hoch/runter = Slot im Editor.
- LEDs per SysEx: Pad mit TOGGLE = grün (an) / schwach rot (aus), Pad mit MOMENT = schwach grün,
  gedrückt amber, PAGE-Pad = amber, Pad im LEARN = rot, Pfeile zeigen, ob es links/rechts weitere Seiten gibt.
- Ein Template-Wechsel am Gerät wird erkannt (SysEx), die LEDs folgen.

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

## Dateien

| Datei | Inhalt |
|---|---|
| `vst/ccmap_core.h` | Mapping-Kern ohne VST und ALSA (Learn, Seiten, Ziele, LEDs, Zustand als Text) |
| `vst/ccmap_vst.cpp` | VST2-Hülle (handgeschrieben), ALSA-Thread, Presets, Projekt-Chunk |
| `vst/module.json`, `vst/vst.json`, `vst/layout.conf` | Parameter und Skin für `gen_vst.py` |
| `vst/captions.py`, `vst/fonts/` | Beschriftungen in Titillium Web (aus mpc-vst-euclidier, SIL OFL) |
| `vst/host_test.cpp`, `vst/test.sh` | Offline-Test über Test-Hooks statt ALSA |

Build: Actions → „VST release (draft)" → Run workflow (Vorlage: mpc-vst-rattler, sd88me/mpc-vst-plugins).

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic). „Novation", „Launch Control" und „Evolution UC-33"
gehören ihren Inhabern, keine Verbindung.
