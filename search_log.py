import os
import re

local_app_data = os.environ.get("LOCALAPPDATA", "")
log_path = os.path.join(local_app_data, "RotMG Exalt DLL Trace.log")
print("Log path:", log_path)

if not os.path.exists(log_path):
    print("Log file does not exist!")
    exit(0)

patterns = ["AimHooks", "autoFire", "resolved", "FAILED", "resolution"]
compiled = [re.compile(p, re.IGNORECASE) for p in patterns]

matching_lines = []
with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
        line_str = line.strip()
        # skip heartbeat logs
        if "[AutoAim] hb" in line_str or "[Present] f=" in line_str:
            continue
        if any(c.search(line_str) for c in compiled):
            matching_lines.append(line_str)

print(f"Total matching lines (excluding heartbeats/frames): {len(matching_lines)}")
for line in matching_lines[-50:]:
    print(line)
