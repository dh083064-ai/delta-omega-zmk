import sys
import yaml

path = sys.argv[1]
with open(path, encoding="utf-8") as f:
    data = yaml.safe_load(f)

for combo in data.get("combos", []):
    combo["l"] = ["Combos"]

with open(path, "w", encoding="utf-8") as f:
    yaml.dump(data, f, allow_unicode=True, sort_keys=False, default_flow_style=None)
