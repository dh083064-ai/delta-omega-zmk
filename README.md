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
