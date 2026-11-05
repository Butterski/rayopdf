# SPDX-License-Identifier: Apache-2.0
FROM python:3.13-slim
RUN apt-get update && apt-get install -y --no-install-recommends time && rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y --no-install-recommends poppler-utils && rm -rf /var/lib/apt/lists/*
COPY competitors/convert.py /opt/eval/convert.py
RUN python3 - <<'EOF'
import json, subprocess
v = {}
r = subprocess.run('pdftotext -v', shell=True, capture_output=True, text=True)
v['pdftotext'] = ((r.stdout or '') + (r.stderr or '')).strip().splitlines()[0] if (r.stdout or r.stderr).strip() else None
out = subprocess.run(['python3', '-m', 'pip', 'freeze'], capture_output=True, text=True, check=True).stdout
v['pip'] = {l.split('==')[0]: l.split('==')[1] for l in out.splitlines() if '==' in l}
json.dump(v, open('/opt/eval/versions.json', 'w'), indent=1, sort_keys=True)
EOF
RUN chmod -R a+rX /opt/eval
