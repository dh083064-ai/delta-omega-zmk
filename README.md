# Delta Omega ZMK — KOR / Gallium / MAPLE

Ready-to-drop ZMK user-config files for the uploaded Delta Omega setup.

## Hardware/build assumptions

- Delta Omega 34-key 3x5+2 split
- `seeeduino_xiao_ble`
- `delta_omega_left` / `delta_omega_right`
- ZMK pinned to `v0.3`
- existing custom Delta Omega module remote preserved
- `zmk-rgbled-widget` pinned to `v0.3`
- NKRO enabled
- normal firmware builds do **not** enable USB logging

## Repository layout

```text
.
├── .github/
│   └── workflows/
│       └── build.yml
├── build.yaml
└── config/
    ├── delta_omega.conf
    ├── delta_omega.keymap
    └── west.yml
```

## Layers

| # | Layer | Purpose |
|---|---|---|
| 0 | KOR | Korean QWERTY physical layout + HRM |
| 1 | ENG | Gallium Colstag + HRM |
| 2 | NAV | typing navigation |
| 3 | NUM | normal number-row HID codes |
| 4 | FUN | F1-F12 + editing/navigation |
| 5 | UTIL | KOR/ENG/MAPLE switching + 한/영 |
| 6 | MAPLE | game layer: 26 alphabet + direct arrows |
| 7 | MUTIL | Maple numbers/F-keys/editing |
| 8 | SYM | conditional NUM+FUN symbol layer |

## Thumb order

The physical thumb order is preserved as requested:

```text
left -> right
ESC | SPACE || BACKSPACE | ENTER
```

### KOR / ENG

```text
tap:  ESC       SPACE       BACKSPACE      ENTER
hold: UTIL      NAV         FUN            NUM
```

Holding `NUM + FUN` together activates `SYM`.

### MAPLE

Thumb **tap outputs remain the same**:

```text
ESC | SPACE || BACKSPACE | ENTER
```

To fit Ctrl/Alt/M-UTIL without deleting any of the 26 letters or the four
direct arrow keys:

```text
hold ESC       -> Ctrl
hold BACKSPACE -> Alt
hold ENTER     -> M-UTIL
SPACE          -> plain Space
```

Only these thumbs are dual-role in MAPLE. The 30 main game keys contain no
home-row mods.

## MAPLE direct layout

```text
Q W E R T   Y U ↑ O P
A S D F G   H ← ↓ → I
Z X C V B   N M J K L
```

This keeps all A-Z letters while moving I/J/K/L to the former punctuation
positions and using the original I/J/K/L area as an inverted-T arrow cluster.

## M-UTIL

Hold the MAPLE Enter thumb:

```text
1  2  3  4  5    6  7  8  9  0
F1 F2 F3 F4 F5   F6 F7 F8 F9 F10
F11 F12 KOR ENG INS   HOME END PGUP PGDN DEL

thumbs while M-UTIL:
Shift | Ctrl | Alt | [Enter is being held]
```

Numbers are normal number-row HID usages (`N1` ... `N0`), not keypad usages.

## Typing HRM

GACS mirrored:

```text
left:  GUI  ALT  CTRL SHIFT
right: SHIFT CTRL ALT GUI
```

Settings:

```text
flavor = balanced
tapping-term = 200 ms
quick-tap = 175 ms
require-prior-idle = 150 ms
opposite-hand positional hold triggers
hold-trigger-on-release
```

## Switching modes

From KOR/ENG, hold `Esc` for UTIL:

- `Q` position -> KOR
- `W` position -> ENG
- `E` position -> MAPLE
- `R` position -> `LANG_HANGEUL`

From MAPLE, hold Enter for M-UTIL:

- bottom-row `C` physical position -> KOR
- bottom-row `V` physical position -> ENG

## Build

1. Put the extracted files at the root of a GitHub repository.
2. Push to GitHub.
3. Open **Actions**.
4. Run/wait for **Build ZMK firmware**.
5. Download the firmware artifact.
6. Flash the left and right `.uf2` files to their matching halves.

`settings_reset` is included only as a recovery image for clearing stored
settings/BLE bonding when needed.

## NKRO / BLE note

`CONFIG_ZMK_HID_REPORT_TYPE_NKRO=y` is enabled. Because HID descriptor
changes can be cached by BLE hosts, if keys behave strangely after the first
NKRO flash, forget the keyboard on the host and pair it again.

## Important

These files were statically checked for 34 bindings per layer and for required
features. A complete ZMK/Zephyr compile cannot be performed in this chat
runtime; the included GitHub Actions workflow is the final compiler check.
