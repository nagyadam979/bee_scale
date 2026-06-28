# H2 — HX711 power-path (excitation-lekapcsolás)

**Típus:** Hardver chunk · **Setup:** 0 (1 node, multiméter, rádió nélkül) · **Irány:** (A) elektromos/energetikai

## Cél
A HX711 mérőerősítő-modul nyugalmi áramának kimérése, és annak igazolása, hogy alvás közben a
**load cell gerjesztése (excitation) ténylegesen lekapcsol** (0 V a cellán). Az olcsó modulok
LED-je / szivárgó tápútja a deep-sleep mérleget elronthatja (jó modul ~1 µA, olcsó ~120 µA — a
LED-del azonos nagyságrend).

## Függőségek
- **Előfeltétel:** nincs — **független H1-től, párhuzamosítható** vele.
- **Mit blokkol:** a kritikus adatút eleme (H1 → **H2** → Sz0 → …); a node végső deep-sleep
  száma a HX711 nélkül nem teljes.

## Bemenet / előfeltételek
- HX711-M modul(ok) — érdemes a konkrét beszerzett darabot mérni (modulonként eltér).
- Load cell (HBS-SCL-50) vagy híd-helyettesítő.
- Multiméter (µA + V), GPIO-vezérelhető power-down (`PD_SCK` magasra húzva altatható).

## Megvalósítás vázlata (NEM kód)
1. Modul nyugalmi áramának mérése `PD_SCK` magas (power-down) állapotban.
2. **Excitation-ellenőrzés:** feszültségmérés a load cell gerjesztő pontjain alvás közben — cél 0 V.
3. Ha a modulon LED van / a tápút szivárog → cseréljünk vagy módosítsunk, újramérés.
4. Eredmény a Setup 0 jegyzőkönyvbe (modul → µA + excitation 0 V igen/nem).

## Kimenet / deliverable
- HX711-modul nyugalmi árama mérve + excitation-lekapcsolás igazolva (0 V a cellán alvásban).

## Pass / fail
| Feltétel | Pass |
|---|---|
| HX711 nyugalmi áram | **< ~5 µA** |
| Load cell excitation alvás közben | **0 V** (ténylegesen lekapcsol) |
| Tápút / LED | nincs szivárgó tápút, nincs LED |

> A pass **csak akkor** áll, ha mindhárom teljesül a konkrét modulon. Jó modul ~1 µA, olcsó ~120 µA.

## Nyitott kérdések / kockázatok
- A konkrét HX711-M modulon kapcsolható-e az excitation egyáltalán? Ha nem → modulcsere kell.
- HW-pontosság szempont (nem itt mérve, de összefügg): üzem közben a HX711 **LDO-ról szűrt tiszta
  3.3V**-ot kapjon, ne a süllyedő cellasínt — lásd Sz1 / rendszerterv.

## Hivatkozások
- `../../docs/new_base_components.md` (Slave BOM, HX711 megjegyzés), `../../MEMORY.md` (7. nyitott kérdés #3).
- Társ-chunk: [`../H1_node_board_prep/`](../H1_node_board_prep/).
