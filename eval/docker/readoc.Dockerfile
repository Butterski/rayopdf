# SPDX-License-Identifier: Apache-2.0
# READoc evaluation suite runtime (plan step 5). READoc's code is NOT copied in:
# mount a clone of icip-cas/READoc@afa0477 read-only and run src/evaluation/suite_e2e.py.
# Build context: eval/.
FROM python:3.11-slim

ARG PANDOC_VERSION=3.1.11.1
ENV NLTK_DATA=/opt/nltk_data \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PYTHONDONTWRITEBYTECODE=1

# Pins resolved 2026-11-05 from the unpinned list in the plan (READoc's requirements.txt
# names PIL instead of pillow and omits fire).
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && curl -fsSL -o /tmp/pandoc.tar.gz \
      "https://github.com/jgm/pandoc/releases/download/${PANDOC_VERSION}/pandoc-${PANDOC_VERSION}-linux-amd64.tar.gz" \
 && tar -xzf /tmp/pandoc.tar.gz -C /opt \
 && ln -s "/opt/pandoc-${PANDOC_VERSION}/bin/pandoc" /usr/local/bin/pandoc \
 && rm /tmp/pandoc.tar.gz \
 && apt-get purge -y curl && apt-get autoremove -y && rm -rf /var/lib/apt/lists/* \
 && pip install --no-cache-dir \
      pandoc==2.4 fire==0.7.1 scipy==1.17.1 numpy==2.4.6 nltk==3.10.4 apted==1.0.3 \
      Levenshtein==0.27.5 lxml==6.1.3 beautifulsoup4==4.15.0 pymupdf==2.1 pillow==13.0.0 \
 && python -m nltk.downloader -d "$NLTK_DATA" wordnet omw-1.4

# nltk.translate.meteor needs wordnet (+ omw-1.4); scoring runs with --network none.
RUN mkdir -p /opt/eval \
 && python - <<'EOF' \
 && chmod -R a+rX /opt/eval "$NLTK_DATA"
import json, subprocess
pip = {}
for line in subprocess.run(["pip", "freeze"], capture_output=True, text=True, check=True).stdout.splitlines():
    if "==" in line:
        name, ver = line.split("==", 1)
        pip[name] = ver
out = {
    "pandoc": subprocess.run(["pandoc", "--version"], capture_output=True, text=True).stdout.splitlines()[0],
    "python": subprocess.run(["python", "--version"], capture_output=True, text=True).stdout.strip(),
    "nltk_data": ["wordnet", "omw-1.4"],
    "pip": pip,
}
json.dump(out, open("/opt/eval/versions.json", "w"), indent=1, sort_keys=True)
EOF
