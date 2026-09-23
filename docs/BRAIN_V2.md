# Brain v2 – Projektkontext

Stand: 22.09.2026. Ausgangsanalyse: Commit `f9cabdf1d04d314f240faecbc239ec706aa4da84`; anschließend lokaler, gebauter Kommunikationsfix (noch nicht auf Hardware geflasht).

Dieses Dokument trennt **aus dem Code verifizierten IST-Zustand**, **vom Nutzer geplante v2** und **offene bzw. experimentelle Entscheidungen**. Es ist keine Implementierung oder Freigabe einer Implementierung. README bleibt der Einstieg für Betriebsbefehle; dieses Dokument bündelt den ausdrücklich angefragten v2-Kontext.

Prüfumfang: sämtliche versionierten Firmware-Quellen und Header, gemeinsames Protokoll und Szenen, PlatformIO-Konfiguration, Cloudflare-Worker einschließlich Konfiguration, vorhandene Dokumentation und Konfigurationsvorlagen. Lokale Geheimnisse, generierte Build-Artefakte und Fremdbibliotheken sind kein Bestandteil der Quellcodeprüfung. Brain und Node wurden nach dem Kommunikationsfix gebaut; keine Firmware wurde geflasht, keine Cloud-Aufrufe oder Hardwaremessungen wurden durchgeführt. Aussagen über Fehlerpfade sind statisch belegt; ihre Häufigkeit und die Zuordnung zum konkret beobachteten Szenenproblem sind noch nicht gemessen.

## A. Verifizierter IST-Zustand aus dem Code

### A.1 Architektur und Zuständigkeiten

| Bereich | Tatsächlicher Stand | Quellen |
| --- | --- | --- |
| Build | Arduino/PlatformIO, `espressif32`, beide Environments `m5stack-atom`; Brain mit M5Atom und FastLED, Node mit FastLED. Keine explizite Versionsfixierung der Plattform/Bibliotheken und kein S3R-Environment. | `platformio.ini` |
| Brain | ATOM Matrix; Hauptloop für Mikrofon, Audio-Follow, Master-Clock, Szenen, Funkverwaltung, UI und Button. Boot zunächst nicht live; kurzer Tastendruck aktiviert die Show. | `apps/brain/src/main.cpp` |
| Audio | Unit Mini PDM; Pegel, Onsets, Tempo, Confidence, VU und optionale Aufnahme. | `pdm_microphone.{h,cpp}` |
| Clock-Follow | Eigenständige Zustandsmaschine zur Übernahme der Audio-Schätzung in die Master-Clock. | `audio_follow.{h,cpp}` |
| Node | Eine gemeinsame Firmware für Flashlight oder RGB; Typ und Rolle in Preferences gespeichert. UV ist nur Enum, kein unterstützter Renderer. | `apps/node/src/main.cpp`, `node_output.*` |
| Szenen | Gemeinsame Definitionen für Brain und lokale Node-Demo; zwei Szenen: die bestehende Szene 1 und die aus der Git-Historie wiederhergestellte Szene „dynamisch“. | `shared/include/scene_programs.h`, `apps/node/src/node_programs.h` |
| Transport | ESP-NOW-Broadcast auf Kanal 1, gemeinsame C++-Strukturen, eigenes Protokoll v13. | `shared/include/protocol.h`, `espnow_transport.h` |
| Text | Matrix-Scrolltext sowie gesonderte beatgebundene Morse-Lichtausgabe auf Nodes. | `text_playback.cpp`, `node_text_channel.h`, beide `main.cpp` |
| Cloud | Ein FreeRTOS-Worker auf Core 0 für WLAN/HTTPS; Cloudflare vermittelt AudD und Textgenerierung. | `api_client.cpp`, `wifi_manager.cpp`, `ai_mode.cpp`, `workers/decaflash/src/index.js` |

Brain-Module in der Tabelle ohne vollen Pfad liegen unter `apps/brain/src/`. Es gibt noch kein Personality-State-System, keine IMU-/TMOS-Auswertung, keine Frequenzbandanalyse, kein TinyML, kein lokales Sprachmodell und keine Speaker-Ausgabe.

Der Brain arbeitet überwiegend kooperativ im Arduino-Loop. Audio läuft nicht in einer eigenen, fest getakteten Analysetask. Die separate Cloud-Task beseitigt weder die gemeinsame Funkhardware noch sämtliche blockierenden Wege im Hauptloop.

### A.2 Brain ↔ Node: Datenmodell und Lebenszyklus

Der Brain sendet einen Szenenindex, keine Pixelströme und keine Rollenbefehle. Die gemeinsame Szenendefinition liegt in Brain und Nodes; jeder Node leitet aus Szenenindex und seiner gespeicherten Rolle die lokale Lichtausgabe ab.

| Nachricht | Richtung | Inhalt/Funktion |
| --- | --- | --- |
| `SceneSelect` | Brain → Broadcast | Index der gemeinsamen Szene |
| `ClockSync` | Brain → Broadcast | BPM, Beats pro Takt, Beat im Takt, Taktnummer |
| `BrainHello` | Brain → Broadcast | einmalige Begrüßung beim Brain-Boot; löst den dreifachen Node-Blink und `BrainWaiting` aus |
| `NodeText` | Brain → Broadcast | Node-Typ, Cancel-Flag, 48-Byte-Textfeld |

Headerprüfung: Magic `0x4443464C`, Version `13`, Nachrichtentyp; Empfang prüft außerdem `sizeof(...)`. Die Strukturen werden direkt als Speicherabbild gesendet. Es gibt keine explizite portable Serialisierung, keine Senderbindung auf dem Node, keine Verschlüsselung, kein Pairing und keine Anwendungs-ACKs für Szenen.

Nodes senden keine Status-, Heartbeat- oder Clock-Antworten. Der Brain weiß damit bewusst nicht, welche Nodes eine Szene angewendet haben. Eine erfolgreiche `esp_now_send()`-Rückgabe beweist weiterhin nicht, dass ein bestimmter Node die Nachricht empfangen hat.

Brain-Start und Senden:

1. Setup initialisiert Mikrofon, Cloud-Task, ESP-NOW und UI; `brainLive=false`.
2. Bei betriebsbereitem Funk sendet er genau einmal `BrainHello`. Es ist nur die Begrüßung für bereits laufende Nodes, keine Berechtigung für Szene oder Clock. Es gibt keine Session-ID und kein wiederholtes Hello.
3. Kurzer Buttondruck startet mit 120 BPM und vier Beats pro Takt. Weitere kurze Drücke wählen die nächste Szene.
4. Beim Start und jeder Szenenauswahl wird dieselbe `SceneSelect`-Nachricht einmal gesendet. Während der Show folgt derselbe Broadcast alle 30 Sekunden.
5. Clock-Sync erfolgt auf Beat 1 jedes Taktes, zusätzlich beim nächsten Beat nach einer Sync-Anforderung.

Node-Zustände:

- `Demo`: lokale Szene ab Boot, eigene 120-BPM-Clock.
- `BrainWaiting`: ausschließlich nach einem empfangenen Hello; `serviceClock()` führt bis zum ersten Clock-Sync keine Beats aus.
- `BrainRunning`: nach einem gültigen Clock-Sync nach Hello; zwischen Syncs läuft die lokale Clock weiter.

Es gibt aktuell keinen Timeout, der einen laufenden Node nach Funkverlust zurück in Demo schaltet. Er läuft mit dem letzten Tempo und Befehl weiter. Ein wartender Node bleibt dagegen bis zum nächsten Clock-Sync wartend. Das ist kein expliziter, zeitgesteuerter Node-Holdover-Zustand.

### A.3 Szenen, Renderer und konkrete Problemstellen

`kScenes` enthält jetzt zwei Szenen. Szene 1 ist der bisherige Zustand. Szene 2 heißt „dynamisch“ und wurde aus dem früheren gemeinsamen Szenenmodell (Commit `07f4003`) wiederhergestellt. Ihre RGB-Farben, Pegel und Taktmuster entsprechen der damaligen Definition. Der alte `Breathe`-Renderer existiert im aktuellen Modell nicht mehr; der Wash verwendet deshalb dessen heutiges beatgebundenes Gegenstück `BarWave`. Das frühere Flash-Modell mit 2er- und 3er-Bursts wird durch ein entsprechendes Variationsprofil aus Drive-, Double- und Quad-Motiven umgesetzt.

Szene 1:

- Flash: deterministisches Variationsprofil, Fenster acht Takte, Seed 17; Gewichte Drive/Heavy/Double/Quad/Riser = 30/14/24/20/12. Die fünf Motive sind keine fünf aktuell auswählbaren Szenen.
- Wash: blau/eisblau, `BarWave` über vier Takte.
- Pulse: blauer Beat-Puls.
- Accent: roter Herzschlag über vier Beats.
- Flicker: beatbezogener alternierender Runner.

RGB wird über zwei GPIOs (26/32) aus demselben 15-Pixel-Puffer ausgegeben, also gespiegelte Ausgänge. Zusätzlich erzeugt der Renderer zeitbasierte Aktivität, Schatten, Farbstich und Dunkelstellen. Diese `activity` ist **kein Audio-Messwert**: Sie stammt aus `millis()`, Hash-/Noise-Funktionen und langsamen Pendelverläufen. Diese Modulationen sind auch nicht an eine gemeinsame Brain-Zeit gebunden; unterschiedliche Bootzeiten können Unterschiede erzeugen.

Der lokale Kommunikationsfix vereinfacht die frühere Fehlkopplung wie folgt:

1. **Eine Nachricht pro Szene.** Fremde Rollenpakete können nichts mehr überschreiben, weil Rollenbefehle nicht mehr über Funk gesendet werden.
2. **Idempotente Auswahl.** Ein Node vergleicht den aus dem Szenenindex abgeleiteten Befehl mit seinem aktiven Befehl. Eine Wiederholung derselben Szene verändert die laufende Ausgabe nicht.
3. **Einzelner, periodischer Szenenbroadcast.** Der Brain sendet `SceneSelect` beim Start oder Wechsel einmal und wiederholt den aktuellen Stand alle 30 Sekunden.
4. **Hello ist nur die Begrüßung.** Ein Node ohne Hello läuft zunächst in seiner lokalen Demo, übernimmt aber die nächste Szene und Clock unmittelbar. Nach einem empfangenen Hello blinkt er dreimal und wartet auf die ersten Brain-Nachrichten.
5. **Keine globale Szene-Barriere.** Nodes übernehmen eine Szene beim Eintreffen und verwenden dabei ihre laufende Beatposition. Eine kurzzeitig alte Szene auf einem einzelnen Node ist zulässig; die Installation wartet nicht auf andere Nodes.
6. **Kein Node-Rückfunk.** Status- und Clock-Antworten sind entfernt. Der Brain bekommt keine Empfangsbestätigung, erzeugt dafür aber keinen gleichzeitigen Antwortverkehr von mehreren Nodes.
7. **Blockierungen vergrößern weiterhin Empfangs- und Clock-Latenzen.** Flash-Ausgabe verwendet `delay(flashMs)` sowie zusätzliche 8-ms-Pausen. Hello erzeugt drei blockierende Bestätigungsblitze über ungefähr 2,3 Sekunden; Rollenbestätigung blockiert ebenfalls.
8. **Clock-Ankunft ist keine präzise Beatzeit.** Details siehe A.5. Drift, Verarbeitungszeit und Sprünge können wie Szenenfehler aussehen.
9. **Text kann die Szene absichtlich verdrängen.** Morse ersetzt vorübergehend die reguläre Node-Ausgabe. Textpakete werden nach Node-Typ gefiltert.

Quellen: `apps/brain/src/main.cpp` (`sendSceneSelect`, `selectNextScene`, Loop), `apps/node/src/main.cpp` (`stageIncomingSceneSelect`, `processPendingRadio`, `processPendingSceneSelectMessage`, `enterBrainWaitingMode`), Node-Renderer. Die tatsächliche Ursache eines konkreten Hardwarevorfalls bleibt ohne Symptom-/Logkorrelation offen; insbesondere ist kein bestimmter Paketverlustanteil nachgewiesen.

### A.4 BPM-Scanner: vollständiger Ablauf

Es gibt drei verschiedene Tempoangaben:

- `PdmMicrophone::detectedBpm()`: gewählter Tempovertreter aus der Onset-Analyse.
- `PdmMicrophone::clockBpm()`: daraus mit Halb-/Doppeltempo-Heuristik gewähltes Clock-Ziel.
- `currentBpm` im Brain: tatsächlich laufendes Showtempo nach `audio_follow`.

`updateClockFromAudio()` übergibt ausdrücklich `microphone.clockBpm()` an das Feld `audio_follow::Input::detectedBpm`. Der Feldname ist daher leicht missverständlich.

#### 1. Aufnahme und Vorverarbeitung

Quelle: `pdm_microphone.cpp`, `begin`, `update`, `accumulateSamples`.

- PDM RX auf `I2S_NUM_0`, DATA GPIO26, CLK GPIO32, 16 kHz, 16 Bit, Mono links, DMA 8 × 128, PDM-Downsampling `I2S_PDM_DSR_16S`.
- Je Loop-Aufruf bis zu vier nicht wartende `i2s_read()` mit je 256 Samples: maximal 1024 Samples bzw. 64 ms Audiodaten pro Aufruf. Das ist **kein festes 64-ms-Analyseintervall**.
- Pro Sample: `dc += (sample - dc) >> 6`; anschließend `centered = sample - dc`. Dies ist ein kontinuierlicher DC-Schätzer mit Hochpasswirkung, kein Bassbandfilter.
- `blockLevel` ist der Mittelwert von `abs(centered)` über alle im aktuellen Update gelesenen Samples, nicht RMS. Außerdem wird der größte Absolutwert erfasst.
- Eine weitere Sample-Hüllkurve mit Gewicht 1/16 wird mitgeführt, ist aber nicht die Grundlage des Tempo-Scores.
- Pro Update mit neuen Samples erfolgt genau eine Analyse, Zeitstempel `millis()` nach dem Lesen. Die Onset-Zeit ist somit Verarbeitungszeit, nicht die samplegenaue Position eines Transienten im DMA-Puffer.

#### 2. Energiehüllkurven und Music-Gate

Quelle: `updateAnalysis`. Bei Nullinitialisierung wird direkt der aktuelle Blockpegel übernommen; danach gelten ganzzahlige Filter:

| Größe | Steigender Eingang | Fallender Eingang |
| --- | --- | --- |
| schnelle Analysehüllkurve F | `(2F + block)/3` | `(5F + block)/6` |
| langsame Analysehüllkurve S | `(15S + block)/16` | `(31S + block)/32` |
| Analyse-Floor N | `(255N + S)/256` | `(7N + S)/8` |

`musicPresent` schaltet an bei `S > N + 22` und bleibt an, solange `S > N + 12`. Dies ist Pegelhysterese, keine Unterscheidung zwischen Sprache, Musik und Geräusch.

Transient `T = max(F-S, 0)`. Onsetschwelle `H = 18 + max(S-N, 0)/3`.

#### 3. Onsets und History

Ein Onset wird akzeptiert, wenn:

- Music-Gate aktiv ist;
- seit dem letzten Onset mindestens 300 ms vergangen sind (oder es noch keinen gibt);
- `T > H`;
- der Blockpeak höchstens das 18-Fache des Blockpegels beträgt (bei Blockpegel 0 wird diese Peakprüfung übersprungen).

Bei bestehendem Tempo und Confidence ≥ 50 werden zusätzlich zu frühe Intervalle verworfen: kleiner als 88 % des erwarteten Beatintervalls, außer sie liegen im Bereich 45–65 %. Die absolute 300-ms-Sperre gilt weiterhin und beschränkt diese Halbintervall-Ausnahme.

Akzeptierte Onsets erhalten den aktuellen Verarbeitungszeitstempel. Gespeichert werden die letzten acht Onset-Zeiten und die letzten acht gültigen Intervalle. Intervalle von 300–1200 ms lösen die Temposchätzung aus; sie beginnt erst mit mindestens drei Onset-Zeitstempeln. Bei einer Lücke über 1200 ms bleiben nur der neue Zeitstempel und Confidence 0; Intervallhistory wird gelöscht, Tempo-Buckets werden an dieser Stelle nicht gelöscht.

Nach mehr als 2200 ms ohne Onset wird Confidence auf 0 gesetzt. Nur wenn zugleich das Music-Gate aus ist, werden auch BPM, Clock-BPM, Histories und Tempo-Buckets zurückgesetzt. Bei fortbestehendem Pegel können alte BPM daher mit Confidence 0 stehen bleiben.

#### 4. Puls-History als zusätzliches Periodizitätssignal

Bei jedem Analyseupdate wird `min(255, max(T-H,0)/4)` in einen 64-Einträge-Ring geschrieben; ohne Music-Gate wird 0 geschrieben.

Das geschätzte Frameintervall startet mit 64 ms. Nur tatsächlich gemessene Updateabstände zwischen 20 und 120 ms fließen mit Gewicht 1/8 ein. Dennoch wird für **jedes** Update ein Pulswert abgelegt. Bei häufigeren Updates unter 20 ms stimmt deshalb die Zeitannahme der History nicht zwingend mit ihrer tatsächlichen Abtastung überein. Die 64 Einträge sind kein garantiertes Zeitfenster.

Für jedes Kandidatentempo wird ein auf ganze Frames gerundeter Ein-Beat-Lag bestimmt. Ab acht History-Einträgen und gültigem Lag (mindestens 2, kleiner als Historylänge) summiert der Code `min(pulse[i], pulse[i-lag])` für Paare ungleich null. Das ist eine einfache verzögerte Ähnlichkeit, keine FFT und keine klassische Produkt-Autokorrelation.

#### 5. Kandidatensuche und Scores

Quelle: `updateTempoEstimate`. Es werden alle **101 ganzzahligen BPM-Werte von 80 bis 180** bewertet. Nachfolgende Formeln beschreiben die tatsächlich verwendeten Gewichte; Divisionen sind ganzzahlig.

Für jedes Paar der höchstens acht Onsets:

1. Abstand `d` muss positiv und höchstens 2200 ms sein.
2. Nächste Beatanzahl `m = floor((d*bpm + 30000)/60000)`; zulässig 1–8.
3. Erwarteter Abstand `e = floor((60000*m + bpm/2)/bpm)`.
4. Toleranz `t = max(24 ms, floor(e*9/100))`.
5. Bei Fehler `a = abs(d-e) <= t`: Scorezuwachs `(1+newerIndex)*(9-m)*8 + t-a`.

Dieser Paarscore heißt hier I. Der zusätzliche Primärscore Q zählt bei `m=1` den doppelten Score, bei `m=2` ein Viertel und sonst nichts. Ein-Beat-Treffer werden separat gezählt.

Die bis zu acht direkten Onsetintervalle ergeben Score D: Vergleich mit `floor(60000/bpm)`, Toleranz `max(24 ms, 8 % dieses Intervalls)`, Zuwachs `(2+intervalIndex)*24 + 2*(t-a)`.

Mit Pulsscore P wird je BPM ein Gedächtnisscore M aktualisiert:

`M = M - (M >> 3) + I + Q + floor(P/2)`.

Dieser Zerfall geschieht je Temposchätzung, nicht je Sekunde. Ein kombinierter Kandidatenscore lautet:

`C = M + 2D + 6Q + 6P + 30 % M_des_linken_Nachbarn + 30 % M_des_rechten_Nachbarn`.

Nur Kandidaten mit aktuellem Paartreffer nehmen an der Auswahl teil. Bei Abstand ≤ 2 zum vorigen erkannten BPM kommen `(3-Abstand)*18*6` Punkte hinzu; sonst bei Confidence ≥ 60 und gleicher Tempofamilie `18*4`.

#### 6. Halb-/Doppeltempo-Familien und BPM-Auswahl

`canonicalTempoFamilyBpm()` betrachtet BPM, ganzzahlig BPM/2 und BPM×2 innerhalb 80–180. Bevorzugt wird zunächst die geringste Entfernung zum Fenster 90–170, dann die geringste Entfernung zu dessen bevorzugtem Zentrum 130.

Die kombinierten Scores werden unter diesem kanonischen BPM zusammengefasst. Die vorige Familie erhält beim Ranking 216 Bonuspunkte. Ab bisheriger Confidence 60 bleibt sie erhalten, wenn die neue Familienwertung weniger als 112 % ihrer vorhandenen Unterstützung erreicht.

Innerhalb der Siegerfamilie wird ein tatsächlicher Tempovertreter gewählt:

`R = 2I + 8Q + D + 8P + floor(M/4) + 30 % I_der_beiden_Nachbarn`.

Der kanonische Vertreter erhält 180 Zusatzpunkte. Andere Vertreter verlieren `abs(bpm-kanonisch)*18*5` Punkte; diese Strafe wird geviertelt, wenn ihr Primärscore oder ihre Primärtrefferzahl stärker als beim Familienanker ist. Bei gleicher Familie mit dem vorherigen BPM gibt es weitere Kontinuitätsboni: bis Abstand 2 `(3-Abstand)*18*8`, sonst bis Abstand 6 `(7-Abstand)*18*2`.

Wichtig: `inSameTempoFamily()` bedeutet nicht allgemein „nahe BPM“. Es prüft gleiche kanonische Werte oder annähernde Verdoppelung der kanonischen Werte mit maximal 4 BPM Fehler. Einige vermeintliche Glättungen wirken deshalb nur bei dieser engen Bedingung.

#### 7. Confidence und zwei Analyse-BPM-Ausgänge

Für den gewählten Vertreter:

- Präzision = `max(0, 100 - floor(mittlerer_Paarfehler*320/Beatintervall))`.
- Abdeckung = `min(100, floor(Paartreffer*100/8))`.
- Primärabdeckung = `min(100, floor(Ein-Beat-Paartreffer*100/4))`.
- Trennung = relativer Abstand der besten zur zweitbesten Familienwertung in Prozent; 0, wenn kein positiver Abstand besteht.
- Kontinuität = initial 60, bei unveränderter Familie 100, sonst `max(0,100-6*Familienabstand)`.

`confidence = floor((2*Präzision + Abdeckung + Primärabdeckung + Trennung + Kontinuität)/6)`.

Das ist ein heuristischer Score 0–100, keine kalibrierte Wahrscheinlichkeit. Bei fehlendem gültigem Sieger sinkt der bisherige Wert um 6. Die deklarierte Konstante `kAnalysisTempoMinimumMatches=3` wird nicht verwendet: Tatsächlich erforderlich sind zunächst drei Zeitstempel; die spätere Auswahl prüft nicht ausdrücklich drei passende Paare. Auch mehrere weitere deklarierte Familien-/Doppelintervall-Konstanten sind derzeit unbenutzt.

`detectedBpm` übernimmt den gewählten Vertreter, mit zusätzlicher Familienhysterese: Bei mindestens 6 BPM Unterschied und bestehender Unterstützung muss der neue Vertreter mindestens 125 % der alten Unterstützung (`6Q+D+P`) erreichen. Bei gleichem Familienbezug und Abstand ≤ 2 wird gemittelt.

`clockBpm` kann stattdessen den schnelleren kanonischen Familienwert wählen, wenn dort Primär-/Intervall-/Pulsunterstützung vorhanden ist: mindestens ein Primärtreffer oder Evidenz `6Q+D+6P` von mindestens 35 % des besten Vertreter-Scores. Der Wechsel zu einer langsameren Unterteilung innerhalb derselben Familie wird verzögert. Trotz Konstante „4 Hits“ hält der aktuelle Kontrollfluss beim ersten Kandidaten und beim Hochzählen bis 4 das alte Tempo fest; erst beim **fünften** passenden Durchlauf wird der langsamere Kandidat übernommen. Kleine Änderungen werden wiederum nur bei gleichem Familienbezug gemittelt.

#### 8. Audio-Follow: Übernahme in die Show-Clock

Quelle: `audio_follow.cpp`; läuft nur bei `brainLive`.

- Zustände `Searching`, `Locked`, `Holdover`.
- Ein frischer Onset benötigt Music-Gate, nichtnull Clock-Ziel-BPM und einen bislang ungesehenen Onset-Zeitstempel.
- Zum ersten Lock: drei frische Kandidaten mit Confidence ≥ 68, BPM innerhalb ±4 zum laufend gemittelten Kandidaten.
- Beim Lock wird das Tempo direkt übernommen, `nextBeatAtMs` auf den Onset-Zeitstempel gesetzt und Clock-Sync angefordert. Der Taktzähler wird dabei nicht musikalisch neu bestimmt.
- Im Lock wird Confidence ≥ 62 verlangt. Onsetintervalle müssen normalerweise 88–112 % oder 176–224 % des laufenden Beatintervalls betragen. Beim Wiederaufnehmen aus Holdover wird diese Intervallprüfung einmal übersprungen.
- Zwei akzeptierte Updates mit exakt gleichem abweichendem Zieltempo erlauben eine Änderung um höchstens **1 BPM**; danach beginnt die Zählung erneut.
- Phasenfehler = Abstand des Onsets zum näheren der vorigen/nächsten geplanten Beatzeit. Kleine Fehler verschieben den nächsten Beat um Fehler/3.
- Große Fehler: Schwelle `max(20 ms, min(90 ms, Beatintervall/5))`. Zunächst Fehler/2 korrigieren; nach zwei großen Fehlern mit gleichem Vorzeichen die ganze Abweichung korrigieren und Sync anfordern.
- Nach ungefähr vier Beatintervallen ohne akzeptierten Onset wird unter den jeweiligen Prüfbedingungen Holdover betreten, spätestens nach mehr als 4 Sekunden wird der Follow-Zustand zurückgesetzt. Die laufende Master-Clock wird dabei **nicht** auf 120 BPM zurückgesetzt und nicht angehalten.

#### 9. Master-Clock und musikalische Aussagegrenze

Quelle: `apps/brain/src/main.cpp`, `setClockBpm`, `updateClockFromAudio`, `onBeat`, Loop.

Start 120 BPM, erlaubte Clock-Grenzen 60–180, Intervall `floor(60000/BPM)` Millisekunden. Im Loop werden fällige Beats mit einer `while`-Schleife nachgeholt. Vier Beats ergeben einen Takt. Die tatsächlich gesendeten Syncs werden erst in `onBeat()` erzeugt.

Der Scanner erkennt **keine musikalische Eins**, keine Taktart und keine Songphrase. „Beat 1“ ist der Zählerstand der Installation. Onsets können durch rhythmische Akzente oder Geräusche entstehen; sie sind keine sicher erkannten Bassdrums. Es gibt keine Bass/Mid/High-Trennung, kein Genre-, Break- oder Drop-Modell. Bei langen Loop-Pausen können mehrere nachgeholte Beats nahezu gleichzeitig verarbeitet werden.

### A.5 Node-Clock ist derzeit kein Soft-Sync

`apps/node/src/main.cpp::applyClockSync()`:

1. Ein gültiger Clock-Sync mit BPM ungleich null wird nur außerhalb des Demo-Modus übernommen, also nach Hello.
2. BPM und Taktlänge werden übernommen, der Zustand wird `BrainRunning`.
3. Wenn derselbe Beat/Takt in den letzten 250 ms bereits gerendert wurde, wird kein weiterer Beat ausgegeben, aber `nextBeatAtMs = now + beatIntervalMs` gesetzt.
4. Sonst werden Beat/Takt übernommen, sofort `onBeat()` ausgeführt und danach ebenfalls `nextBeatAtMs = now + beatIntervalMs` gesetzt.

`now` ist die Verarbeitungszeit im Node-Hauptloop; das Paket enthält keinen Senderzeitstempel und der Callback speichert keinen Empfangszeitstempel. Bei blockierender Flash-Ausgabe kann der Zeitpunkt nach der Ausgabe bereits zurückliegen. Es gibt keinen PLL-Regler und keine Funk-Telemetrie zum Brain. Die README-Aussage über sanften Node-Phasentrim entspricht diesem Code nicht.

Zusätzlich fehlen Plausibilitätsgrenzen für empfangene Clock-BPM: `bpmToIntervalMs(message.bpm)` dividiert direkt. Das ist ein Protokollrobustheitsbedarf, nicht der Nachweis, dass der vorhandene Brain ungültige BPM sendet.

### A.6 VU-Meter und Matrix-Rendering

Die VU-Pegelberechnung ist von der Tempoanalyse getrennt (`updateMeterLevel`): schnelle Hüllkurve mit Attack 1/4 und Release 1/8, langsame mit 1/8 und 1/16. Adaptiver Noise-Floor: nach unten 1/8, nach oben 1/128. Signal-Ceiling folgt nach oben mit 1/4 und nach unten mit 1/16.

Gate = Floor + 6 + 10; Darstellungsbeginn weitere 18 darüber. Grundpegel wird auf maximal 20 Einheiten skaliert, mit mindestens 180 Pegelspanne und sonst zusätzlichem Headroom 140. Einheiten ≤1 werden unterdrückt. Transientenanteil über 18 liefert bis zu fünf zusätzliche Einheiten über eine Spanne von 140. Gesamtmaximum 25, Release zwei Einheiten je Analyseupdate; nach vier ruhigen Updates wird auf 0 gesetzt. Die berechnete Variable `meterDisplayLevel_` wird für diese endgültige Skalierung nicht verwendet.

`matrix_meter.cpp` zeichnet höchstens alle 40 ms, also nominell maximal 25 Aktualisierungen/s. Von 25 LEDs bleiben 23 für den Meter; Pixel 0 ist WLAN-Status und Pixel 4 Beatpunkt. 24/25 gemessene VU-Einheiten werden auf 23 sichtbare Meterpixel gekappt.

Der Meter ist keine klassische Balkenanzeige: Eine zufällige Pixelreihenfolge entscheidet über die aktiven Flächen und verändert sich alle 220 ms. Pro Zeichnen steigen lokale Pixelwerte um 18 oder fallen um 14. Ihre Werte laufen durch Farbverläufe:

- normal: Schwarz → Blau `(0,105,255)` → Pink `(255,40,150)` → Violett `(235,0,255)`;
- Aufnahme/AI-Verarbeitung: Schwarz → Grün → Hellgrün → sehr helles Grün.

Diese Pegel-, Aktivierungs-, Übergangs- und Farbidee ist auf Iris-Polygone übertragbar; `M5.dis.drawpix()` und der 5×5-Framebuffer selbst sind hardwaregebunden.

UI-Vorrang im Grundrenderpfad: Text, Szenenanzeige/temporäre Sperre, AI-Animation, danach VU. Text blockiert Beat-/WLAN-Overlay. Beatpunkt leuchtet 140 ms: **gelb auf Beat 1**, weiß auf anderen Beats, rot als Audio-Sync-Korrekturmarkierung. Das unterscheidet sich von der README-Beschreibung „rot auf Beat 1“.

Text: 96-Byte-Puffer, normalisierte Großbuchstaben/Umlaute, 5×5-Glyphen, Introblitz 90 ms, Pause 1000 ms, dann alle 130 ms eine Scrollspalte. Es gibt noch kein Auge und keinen Text-Overlay-Compositor.

Node-Text: höchstens 47 Nutzbytes, A–Z/0–9 als Morse-ähnliche Lichtfolge, Start auf nächstem lokalen Beat. Einheit ist ein halber Beat, Punkt 1 Einheit, Strich 2, Symbolpause 1, Buchstabenpause 2, Wortpause 4. Das weicht bewusst vom klassischen Morse-Zeitverhältnis ab und ist eine eigene beatgebundene Scheduling-Funktion. Matrixtext und Node-Morse enden nicht notwendigerweise gleichzeitig.

### A.7 ESP-NOW, WLAN und Cloud

ESP-NOW nutzt Wi-Fi STA, festen Offline-Kanal 1 und einen unverschlüsselten Broadcast-Peer mit `peer.channel=0` (= aktueller lokaler Funkkanal). Die Nodes verbinden sich nicht mit einem Access Point.

Der WLAN-Manager scannt synchron, probiert konfigurierte SSIDs in Listenpriorität und wartet pro Verbindungsversuch bis zu 15 Sekunden, in 250-ms-Schritten. Er filtert nicht auf Kanal 1. Manuelle `wifi scan`/`wifi connect` laufen aus der Shell im Brain-Hauptloop und können Audioabholung, Clock und UI blockieren.

Cloud-Arbeit läuft in einer separaten Task auf Core 0, Priorität 1, Stackargument 12288. Es gibt einen Jobslot. Jede eingeplante Cloud-Anfrage aktiviert `managedWifiSessionActive`; `radioPauseActive()` verhindert währenddessen Szenen-, Clock- und Node-Text-Sends. Dies gilt **auch bei einem AP auf Kanal 1**. Nach Abschluss wird WLAN getrennt und ESP-NOW samt Broadcast-Peer wiederhergestellt; der Recovery-Versuch ist auf ein Intervall von einer Sekunde begrenzt.

Bei manuell verbundenem WLAN außerhalb Kanal 1 blockiert `serviceEspNowState()` ESP-NOW ausdrücklich. Mehr RAM oder ein S3R-Port beseitigen das Kanalproblem nicht. Bereits laufende Nodes spielen autonom weiter, bekommen während der Pause aber keine neuen Szenen oder Clock-Korrekturen.

Aktueller Aufnahme-/AI-Pfad:

- Langer Brain-Buttondruck ab 1200 ms toggelt AI-Listening.
- Nach 1500 ms kontinuierlichem Music-Gate wird eine Aufnahme vorbereitet. Trigger bevorzugt BPM vorhanden, Confidence ≥18 und Onset höchstens 180 ms alt; nach weiteren 1200 ms greift ein Fallback ohne diese Beatbedingungen.
- Standard und Maximum sind bereits **12 Sekunden** bei 16 kHz. Aufnahme ist µ-law, ein Byte pro Sample, Zielgröße 192000 Bytes. Bei RAM-Mangel wird in 250-ms-Schritten bis mindestens eine Sekunde gekürzt. Keine explizite PSRAM-Allokation.
- Onset-/VU-Analyse läuft während der Aufnahme weiter. Der ganze Clip liegt vor Upload im RAM; Multipart-Streaming vermeidet einen zweiten vollständigen Upload-Puffer, streamt aber nicht direkt vom laufenden Mikrofon ins Netz.
- Brain sendet µ-law an `/api/audd`; Worker dekodiert zu 16-Bit-WAV und fragt AudD. Bei Treffer erzeugt er aus Titel/Artist zusätzlich einen kurzen deutschen Text. Kein BPM-Rückkanal aus AudD.
- Erfolgreiche Verarbeitung: Cooldown 420 Sekunden; Fehlschlag 60 Sekunden; WLAN-Fehlschlag deaktiviert den AI-Modus.
- IMA-ADPCM-Code ist vorhanden und serverseitig unterstützt, aber im aktiven Brain-Aufnahmepfad wird µ-law verwendet.
- Worker bietet außerdem `/api/chattie`, Health und geschützte Debug-WAV/JSON-Endpunkte. Letzte dekodierte Aufnahme und Metadaten werden in zwei KV-Schlüsseln überschrieben.
- HTTPS-Client verwendet aktuell `setInsecure()`; Zertifikatsprüfung ist also nicht eingerichtet. Lokale Secrets sind gitignoriert, Vorlagen versioniert. Normaler Worker-Deploy ist Git/Cloudflare-Integration.

Damit sind die Hinweise auf ausschließlich kurze Clips und noch fehlende 8–12-s-Unterstützung überholt. Nicht erledigt ist eine belastbare Qualitätsentscheidung, ob ein Clip für Recognition geeignet ist; der Fallback macht das Gate weiterhin grob.

### A.8 Abweichungen vorhandener Dokumentation

README zuerst gelesen, anschließend alle Aussagen gegen Code geprüft. Sie beschreibt u. a. noch ein altes `NodeCommand`-Modell, nur Flashlight-Nodes, fünf lokale Programme, Rohpegel-Serialreports und Node-Soft-Sync. Im Code existieren getrennte Flash-/RGB-Nachrichten, RGB-Rollen, nur eine Szene, ein weitgehend stillgelegtes `printReport()` und der oben beschriebene direkte Node-Resync.

`docs/v1-outline.md` ist historisch: Aussagen „kein Funk/Mikrofon/RGB“ sind kein heutiger IST-Zustand. Die Node-Bootmeldung „Node V2“ bedeutet nicht, dass die hier geplante Brain-v2 bereits existiert. README und alte Dokumentation wurden in diesem Planungsschritt nicht verändert.

## B. Geplante Brain v2 – Nutzeranforderungen, noch nicht implementiert

### B.1 Grundprinzip und Hardware

Die eigentliche Lichtshow muss vollständig ohne Access Point, Internet und KI-Dienste funktionieren. WLAN/Internet liefern ausschließlich Enhanced Features. Kein zusätzlicher ESP/Controller im Brain; die bestehenden Licht-Nodes bleiben Teil der Installation. Display hat geringere Priorität als Audio und Node-Kommunikation.

Zielhardware:

- M5Stack AtomS3R, ESP32-S3-PICO-1-N8R8, 8 MB Flash, 8 MB PSRAM.
- 128×128-Farbdisplay, BMI270-IMU, programmierbarer Frontbutton.
- Atomic Voice Base mit MEMS-Mikrofon und Speaker.
- Externer M5Stack U185 mit STHS34PF80 zur Präsenzdetektion.
- Keine Kamera.

### B.2 Display und Interaktion

Normalansicht: animiertes Low-Poly-Auge im PS1-Stil auf Schwarz, wenige Flat-Shaded-Polygone, bewusst niedrige/ruckelige Framerate erlaubt. Idle-Verhalten enthält Blinzeln und Blickbewegungen. Iris ist das VU-Meter und übernimmt die bestehende Pegel-/Farblogik als visuelles Prinzip.

Persönlichkeit äußert sich zunächst ausschließlich über Text. Text erscheint bei Reaktionen; Overlay über weiterlaufendem Auge ist eine mögliche Darstellung. Langer Text darf das Auge temporär ersetzen. Daraus folgt noch keine Entscheidung über Grafikbibliothek, Polygonanzahl oder konkrete FPS.

Inputs: Mikro/Musik, IMU, Frontbutton, TMOS-Präsenz; optional Internet. Die spätere IMU-Auswertung soll Bass/Vibration, leichtes Tippen, Doppeltippen, Stoß/Schlag und Schütteln/Bewegen unterscheiden. Ein TinyML-Modell ist dafür eine Option, keine festgelegte Voraussetzung.

### B.3 Personality

Die Persönlichkeit wird durch lokale Zustände und Regeln gesteuert, **nicht durch ein Sprachmodell**. Vorgesehene Zustände ungefähr 0–100:

- `energy`
- `annoyance`
- `attention`
- `loneliness`
- `depression` als bewusst humorvoller Grufti-Party-State

Events verändern Werte; ein Teil der Werte baut sich über Zeit ab. Wiederholtes Tippen erhöht beispielsweise annoyance, fehlende Präsenz loneliness, Interaktion attention; annoyance sinkt langsam wieder.

Zunächst `state + event → lokales Textpaket`. Später optional `state + event → kleines lokales Sprachmodell → kurzer Text`. Das Modell wäre ausschließlich der „Mund“; Zustandsentwicklung und Lichtentscheidungen bleiben lokal determinierbar.

States dürfen später Licht beeinflussen, z. B. sehr hohe annoyance → temporär rot. Exakte Grenzen, Dauern, Prioritäten und Rückkehr zur laufenden Szene sind noch nicht spezifiziert. Ein weißer Flashlight-Node kann physisch kein Rot ausgeben; diese konkrete Farbwirkung betrifft RGB-Nodes. Für Flash sind passende Helligkeits-/Rhythmusreaktionen gesondert zu entscheiden.

### B.4 AudioAnalyzer v2

Zuerst bestehenden Scanner verstehen, messen und stabil übernehmen. Mögliche spätere Ausgänge: BPM, Beat, Beat confidence, Bass/Mid/High-Energie, Gesamtenergie, rhythmische Regelmäßigkeit, Break/Drop/Transition und eventuell grobe Musikklassen. Diese Erweiterungen sind nicht bereits implementiert und nicht alle verbindlich beschlossen. TinyML-Audioklassifikation bleibt eine spätere Option.

### B.5 Verbindliche Reihenfolge der vorgesehenen Arbeit

1. Bestehende Brain-/Node-Kommunikation verstehen und stabilisieren.
2. Insbesondere das aktuelle Szenenproblem lösen.
3. Bestehendes System auf AtomS3R portieren.
4. Audio/BPM stabil übernehmen.
5. Low-Poly-Auge mit VU.
6. Personality-State-System und einfache lokale Textpakete.
7. IMU/Button/TMOS.
8. Personality mit Lichtshow verbinden.
9. TinyML.
10. Experimentelles lokales Sprachmodell ganz zuletzt.

Diese Reihenfolge ist Planung, kein Auftrag zur Umsetzung in diesem Analyseschritt.

## C. Technischer Abgleich, offene Entscheidungen und Experimente

### C.1 Hardwareabgleich – Herstellerangaben, keine Codeverifikation

Am 22.09.2026 anhand offizieller M5Stack-Dokumentation geprüft:

- Die genannten AtomS3R-Eckdaten stimmen. Zusätzlich besitzt er einen BMM150-Magnetometer; dessen Nutzung ist nicht verlangt. Interner BMI270-Bus: SDA G45/SCL G0, Button G41, Grove G2/G1. Display benötigt einen anderen Treiberpfad als M5Atom-Matrix. Quelle: [AtomS3R](https://docs.m5stack.com/en/core/AtomS3R).
- Atomic Voice Base verwendet einen ES8311-Audiocodec und I²S. Der bisherige PDM-Treiber samt GPIO26/32 ist nicht direkt übernehmbar. Quelle: [Atomic Voice Base](https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base).
- M5Stack dokumentiert die Kombination S3R + Voice Base: Codec-I²C SDA G38/SCL G39; I²S-Ausgang vom Host G5, LRCK G6, Eingang zum Host G7, BCLK G8. Quelle: [AtomS3R + Voice Base](https://docs.m5stack.com/en/core/AtomS3R-AI%20Chatbot).
- U185/STHS34PF80 verwendet I²C-Adresse 0x5A, Präsenz-/Bewegungserkennung, keinen Kamerabildstrom. Quelle: [Unit TMOS PIR](https://docs.m5stack.com/en/unit/UNIT-TMOS%20PIR).

Vor dem Port sind Board-/Flash-/PSRAM-Einstellungen, tatsächliche Hardwarevariante, Codec-Gain, PCM-Format, Displayinitialisierung und I²C-Busbelegung gemeinsam zu prüfen. Interne Sensoren, Codec und Grove liegen an verschiedenen Pinpaaren; die Bibliotheks- und Busverwaltung darf nicht als automatisch konfliktfrei angenommen werden. Elektrische/mechanische Gesamtintegration wurde hier nicht am Gerät geprüft.

### C.2 Ausdrückliche Widersprüche bzw. noch nicht erfüllte Anforderungen

| Planung/Annahme | Befund und Konsequenz |
| --- | --- |
| Enhanced Features dürfen die Show nicht voraussetzen oder beeinträchtigen | Offline-Grundpfad existiert, aber aktive Cloud-Sessions pausieren die Brain-Funksteuerung. Die starke Prioritätsanforderung ist damit noch nicht erfüllt. |
| Ein Brain-Controller mit WLAN und ESP-NOW | Mögliches Ziel, aber kein unabhängiger Zweitkanal. Mit festen Nodes auf Kanal 1 funktionieren beliebige AP-Kanäle nicht gleichzeitig zuverlässig. |
| Bestehendes System einfach auf S3R übernehmen | Algorithmische Teile sind übertragbar; Board, Matrix, GPIOs, Mikrofontreiber und SDK-Schnittstellen brauchen einen wirklichen Port. |
| Bestehende Szenen durchschalten | Drei klar unterscheidbare Testszenen machen Übernahmefehler sichtbar; ihr endgültiges Lichtdesign ist offen. |
| Audio/BPM stabil übernehmen | Scanner ist block-/loopabhängig und benutzt absolute Pegelschwellen. Neuer Codec/Gain und andere Looplast können das Verhalten trotz identischer Formeln ändern. |
| Iris übernimmt bestehendes VU | Sinnvoll als Pegel-/Farbübernahme; aktuelle Geometrie und 23-Pixel-Auswahl sind nicht direkt das Displaymodell. |
| Gesamte Installation bei annoyance rot | Nur farbfähige Nodes können rot werden. Zudem färbt der aktuelle RGB-Renderer Farben weiter um; ein garantiert roter Override benötigt eine definierte Priorität. |
| Lokales Sprachmodell auf 8 MB PSRAM | Unbewiesenes Experiment; verfügbare Speicherkapazität ist kein Nachweis für passende Qualität, Modellgröße oder Latenz. Kein Bestandteil des Offline-Kernversprechens. |

Espressif dokumentiert die Kanalbindung und dass selbst erfolgreiche MAC-Zustellung keine Verarbeitung auf Anwendungsebene garantiert. Quelle: [ESP-NOW für ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_now.html). Die dortige aktuelle API ist keine Aussage über die im bestehenden PlatformIO-Build installierte SDK-Version.

### C.3 Offene Designentscheidungen – keine vorweggenommene Implementierung

- **Szenenprotokoll:** Frühe Zielrollenfilterung, fortlaufende Brain-Sitzung und idempotente Ein-Sekunden-Wiederholung sind implementiert. Offen bleiben eine mögliche Empfangsqueue bei künftig mehr Nachrichtentypen, optionale Zustandsbestätigungen sowie Messung von Wiederholungsintervall und Rejoin-Verhalten auf Hardware. Keine gemeinsame Szenenbarriere ist vorgesehen.
- **Zeitbasis:** Empfangszeitstempel, echtes Beatserial vs. Sync-Paketserial, latenzbewusster Phasenabgleich, Umgang mit verspäteten Paketen und nichtblockierende Node-Ausgabe. Keine erfundene musikalische Eins aus dem bisherigen Onsetdetektor ableiten.
- **WLAN-Politik:** Eine mögliche Basispolitik wäre, bei unverändertem ESP-NOW-Kanal 1 nur passende WLAN-Netze als Enhancement zuzulassen und sonst offline zu bleiben. Gemeinsame Kanalwechsel aller Nodes sind eine andere, deutlich aufwendigere Option. Noch keine Auswahl getroffen; reine Holdover-Wiedergabe ersetzt nicht die geforderte kontinuierliche Steuerbarkeit.
- **Audioport:** Capture von Analyse trennen; feste Framezeit bzw. echte Sample-Zeitbasis als zu prüfende Verbesserung. Zunächst alte Signalcharakteristik und Confidence-/Lockverhalten vergleichen, nicht stillschweigend den Algorithmus austauschen.
- **Audioqualität:** Clipbereitschaft, Clipping, Pegel, zeitliche Kontinuität und tatsächliche Aufnahmelänge bewerten. Die vorhandene 12-s-Anfrage garantiert bei RAM-Kürzung oder verlorenen DMA-Samples keinen lückenlosen 12-s-Clip.
- **Display:** Iris-Polygonmapping, Textlayout, Framebudget und Auslassen von Frames bei Last. Speaker ist Hardwareumfang, aber noch kein geplanter sprachlicher Personality-Ausgang.
- **Sensorik:** TMOS-Präsenz benötigt zeitliche Filterung und ist kein Personenidentifikator. IMU-Klassen, Abtastrate, Montage und Trainingsdaten sind offen. Bass/Vibration und leichtes Tippen müssen am tatsächlichen Gehäuse unterscheidbar sein; Speakerbetrieb kann Audio- und Vibrationsinputs beeinflussen.
- **Personality:** genaue Eventregeln, Abklingkurven, Persistenz, Textauswahl/Cooldowns, Kombination mehrerer States, zeitlich begrenzte Licht-Overrides und saubere Wiederaufnahme der Szene.
- **TinyML:** zunächst messbarer Bedarf und Datensatz, dann Modellentscheidung; getrennte Optionen für IMU und Audio.
- **Lokales Sprachmodell:** erst am Ende Machbarkeit mit Ressourcenbudget prüfen; lokale Textpakete bleiben die funktionierende Basis.

### C.4 Vorgeschlagene spätere Abnahme, noch nicht durchgeführt

1. Offline-Start aller Node-Typen/Rollen; unterschiedliche Bootreihenfolge; Node-/Brain-Neustart; Rollenwechsel; mehrfacher Szenenwechsel und gezielte Paketverluste.
2. Mehrfacher Wechsel zwischen beiden Szenen, auch während laufender Beats; keine unbeabsichtigte Wartephase bei Szenenwechseln und Übernahme trotz eines verlorenen Pakets innerhalb des dreifachen `SceneSelect`-Bursts.
3. Clockmessung unter Flash-/RGB-Last, ohne Netz sowie mit WLAN-Ausfall und inkompatiblem AP-Kanal. Display-/Cloudlast darf Audio und Node-Steuerung nicht aushungern.
4. Vergleich desselben Audio-Testmaterials vor/nach Port: erkannte BPM, Clock-Ziel, Show-BPM, Confidence, Lockzeit, Phasenfehler, Halb-/Doppeltempo, Breaks/Stille, Sprache/Tippen und Signalübersteuerung. Relevante Messwerte werden aktuell nicht vollständig seriell ausgegeben.
5. Erst danach zusätzliche Analyzer-Merkmale und Personality/Sensorik, anschließend experimentelle Modelle.

Alle Punkte in C sind offene Entscheidungen, abgeleitete Prüfbedarfe oder ausdrücklich bezeichnete Experimente. Sie dürfen nicht als bereits vorhandene Funktion oder als beschlossene konkrete Softwarearchitektur behandelt werden.
