# SPDX-License-Identifier: Apache-2.0
FROM debian:12-slim
RUN apt-get update && apt-get install -y --no-install-recommends python3 time && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /opt/eval
COPY competitors/convert.py /opt/eval/convert.py
RUN python3 - <<'EOF'
import json, subprocess
v = {}
r = subprocess.run('python3 --version', shell=True, capture_output=True, text=True)
v['python3'] = ((r.stdout or '') + (r.stderr or '')).strip().splitlines()[0] if (r.stdout or r.stderr).strip() else None
json.dump(v, open('/opt/eval/versions.json', 'w'), indent=1, sort_keys=True)
EOF
RUN chmod -R a+rX /opt/eval
