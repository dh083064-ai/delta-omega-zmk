# Delta Omega ZMK — simplified 7-layer build

## Layer map

| # | Layer | Access |
|---|---|---|
| 0 | KOR | Korean QWERTY base |
| 1 | ENG | Gallium Colstag base |
| 2 | NAV_MOUSE | hold Space |
| 3 | NUM_FUN | hold Enter |
| 4 | SYM | hold Backspace |
| 5 | MAPLE | Esc + Enter combo toggles ON/OFF |
| 6 | MUTIL | hold Enter while MAPLE is active |

## Thumbs

Physical order stays:

```text
Esc | Space || Backspace | Enter
```

Typing layers:

```text
Esc tap / hold = switch KOR<->ENG
Space tap / hold = NAV_MOUSE
Backspace tap / hold = SYM
Enter tap / hold = NUM_FUN
```

Backspace also has `quick-tap-ms = 150`, so tap then immediately hold gives
normal OS Backspace auto-repeat instead of entering SYM.

## Combos

```text
physical K + L = LANG_HANGEUL (KOR/ENG only)
Esc + Enter = MAPLE toggle (KOR/ENG/MAPLE)
```

The K+L combo is position-based: positions 17+18, i.e. the physical K/L spots
on the QWERTY layout.

## NAV_MOUSE

Right top:
```text
mouse-left  mouse-down  mouse-up  mouse-right  Del
```

Right home:
```text
Left  Down  Up  Right  Del
```

Right bottom:
```text
LeftClick  RightClick  MiddleClick  ScrollUp  ScrollDown
```

`CONFIG_ZMK_POINTING=y` is enabled. Because this changes the HID descriptor,
remove/forget the Bluetooth keyboard and pair it again after flashing.

## NUM_FUN

```text
F1 F2 F3 F4 F5   F6 F7 F8 F9 F10
1  2  3  4  5    6  7  8  9  0
F11 F12 Del Ins Home   End PgUp PgDn PrtSc Caps
```

## SYM

Dedicated symbol layer; no conditional layer is used.

## MAPLE rapid-fire

Physical hold causes repeated HID taps with a fresh random interval of 20–26 ms each cycle.

```text
MAPLE: Q W E U H N B G X C
MUTIL: 1 2 3 '
```

## Build layout

The repository is also a Zephyr module through `config/zephyr/module.yml`.
The custom rapid-fire behavior lives under `config/src/` so it is discovered
through the existing `self.path: config` west manifest.

Build with the included GitHub Actions workflow.

Static structure checks were performed here, but the first GitHub Actions run
is the final compiler/API check for the custom C behavior.


## Rapid-fire random jitter

This revision replaces the fixed 20 ms repeat interval with a fresh randomized
delay for every repeated tap:

```text
minimum interval = 20 ms
maximum interval = 26 ms
tap duration     = 1 ms
```

Examples of successive intervals might be:

```text
20 ms, 25 ms, 22 ms, 26 ms, 21 ms, ...
```

The random value is regenerated for each repeat while the physical key remains
held. Rapid-fire targets remain unchanged:

```text
MAPLE: Q W E U H N B G X C
MUTIL: 1 2 3 '
```


---

## HRM / Backspace / Korean toggle tuning

### Faster home-row modifiers

Both HRM behaviors now use:

```text
flavor = hold-preferred
require-prior-idle-ms = 80
```

The opposite-hand positional trigger lists are retained. This means the
recommended fast shortcut pattern is to use the modifier from the opposite
hand. Example on KOR/QWERTY:

```text
Ctrl+A = hold physical K (right-hand RCTRL HRM) + tap A
Ctrl+S = hold physical K + tap S
Shift+A = hold physical J (right-hand RSHIFT HRM) + tap A
```

Using left D(Ctrl) + left A is same-hand and intentionally does not receive the
instant positional trigger; it may need to cross the tapping term. This protects
normal same-hand rolls from accidental modifiers.

### Backspace repeat

Backspace quick-tap window is now 250 ms.

```text
tap Backspace -> delete once
tap, then press-and-hold again within 250 ms -> normal held Backspace / OS repeat
plain hold without the preceding tap -> SYM layer
```

### Korean/English IME combo

Physical K+L remains the combo, active only on layer 0/1 (KOR/ENG), but the
output is now Right Alt (`RALT`) instead of the dedicated `LANG_HANGEUL` HID
usage. Combo timeout is 110 ms.

This is intended for Windows Korean IME setups where Right Alt acts as the
Han/Eng toggle.


## Persistent layer LED colors

The RGB LED now shows a steady color for the highest active layer instead of
the old blue layer-change blink sequence.

```text
KOR        (0) = Green
ENG        (1) = Yellow
NAV_MOUSE  (2) = Cyan
NUM_FUN    (3) = Magenta
SYM        (4) = Red
MAPLE      (5) = Blue
MUTIL      (6) = White
```

Color IDs follow zmk-rgbled-widget:
0 Black, 1 Red, 2 Green, 3 Yellow, 4 Blue, 5 Magenta, 6 Cyan, 7 White.


## Debounce

Global key debounce is explicitly set to:

```text
press   = 2 ms
release = 2 ms
```

These values override per-driver debounce settings.
