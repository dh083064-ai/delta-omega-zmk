# Local keymap-drawer workflow (urob/zmk-config style): `just init` once,
# then `just draw` any time config/delta_omega.keymap changes, to
# regenerate draw/delta_omega.svg straight from the real keymap source -
# no GitHub Actions involved yet, this is the locally-reproducible half.
# See draw/README (if you're reading this before it exists yet, see the
# top-level README.md) for how the generated SVG gets shown there.

venv_python := if os() == "windows" { ".venv/Scripts/python.exe" } else { ".venv/bin/python" }
keymap_bin := if os() == "windows" { ".venv/Scripts/keymap.exe" } else { ".venv/bin/keymap" }

# 9 layers, in delta_omega.keymap's own declaration order - keep this in
# sync if a layer is ever added/removed/reordered there.
layer_names := "KOR ENG NAV FN NUM SYS MOUSE GAMING MUTIL"

# This project's physical layout: 3 rows x 5 columns per hand + 2 thumbs
# per hand (0-33 position numbering used throughout config/ and the
# combos block) - there's no QMK/ZMK catalog entry for this board, so an
# ortho layout stands in for a real physical layout file.
ortho_layout := '{split: true, rows: 3, columns: 5, thumbs: 2}'

# One-time (or after requirements.txt changes) setup of the local venv
# used by `just draw`. Uses an explicit absolute path to a real
# python.org-built Python rather than whatever "python"/"py" happens to
# resolve to on PATH - a python.org install via winget doesn't always
# land on PATH in an already-open shell, and (learned the hard way on
# this project) some MSYS2-packaged Pythons can't install keymap-drawer's
# compiled deps (wrong wheel platform tag) at all. Adjust the path below
# if your Python lives somewhere else.
init:
    "C:/Users/dh083/AppData/Local/Programs/Python/Python312/python.exe" -m venv .venv
    {{venv_python}} -m pip install -r requirements.txt

# Parse config/delta_omega.keymap straight from source and redraw
# draw/delta_omega.svg. Run this any time the keymap changes and you
# want the diagram to match - nothing is cached, every run reparses the
# real .keymap file.
#
# Combos are pulled off every individual layer and consolidated onto
# one dedicated "Combos" section at the end (via --virtual-layers plus
# reassigning every combo's target layer to it) - same technique
# urob/zmk-config's own Justfile uses for draw/base.svg, so the KOR/ENG/
# NAV/etc. sections stay as clean single-purpose key diagrams instead of
# each carrying its own scatter of combo boxes.
draw:
    {{keymap_bin}} -c keymap_drawer.config.yaml parse -z config/delta_omega.keymap -l {{layer_names}} -c 5 --virtual-layers Combos -o draw/delta_omega.yaml
    {{venv_python}} draw/_reassign_combos.py draw/delta_omega.yaml
    {{keymap_bin}} -c keymap_drawer.config.yaml draw draw/delta_omega.yaml --ortho-layout '{{ortho_layout}}' -o draw/delta_omega.svg
