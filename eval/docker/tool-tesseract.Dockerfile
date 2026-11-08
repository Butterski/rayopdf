# SPDX-License-Identifier: Apache-2.0
# OCR text baseline: pdftoppm (Poppler) renders pages, Tesseract (Apache-2.0) with the eng and osd
# models writes plain text. Debian trixie packages pinned by version on a base pinned by digest.
# Build: docker build -f eval/docker/tool-tesseract.Dockerfile -t rayopdf-eval-tesseract:1 eval
FROM python:3.13-slim@sha256:09a17cad02668ae61ece136b4fd36ac3849e14f670ebedb637ea3e15580525bc
RUN apt-get update \
 && apt-get install -y --no-install-recommends time=1.9-0.2 \
      tesseract-ocr=5.5.0-1+b1 tesseract-ocr-eng=1:4.1.0-2 tesseract-ocr-osd=1:4.1.0-2 \
      poppler-utils=25.03.0-5+deb13u4 \
 && rm -rf /var/lib/apt/lists/*
COPY competitors/convert.py /opt/eval/convert.py
RUN python3 - <<'EOF'
import hashlib, json, subprocess
v = {}
r = subprocess.run(['tesseract', '--version'], capture_output=True, text=True)
v['tesseract'] = (r.stdout + r.stderr).strip().splitlines()[0]
r = subprocess.run(['pdftoppm', '-v'], capture_output=True, text=True)
v['pdftoppm'] = (r.stdout + r.stderr).strip().splitlines()[0]
out = subprocess.run(['dpkg-query', '-W', '-f', '${Package}=${Version}\n', 'tesseract-ocr',
                      'tesseract-ocr-eng', 'tesseract-ocr-osd', 'poppler-utils', 'libtesseract5',
                      'libleptonica6'], capture_output=True, text=True).stdout
v['dpkg'] = dict(l.split('=', 1) for l in out.splitlines() if '=' in l)
models = subprocess.run(['sh', '-c', 'ls /usr/share/tesseract-ocr/*/tessdata/*.traineddata'],
                        capture_output=True, text=True).stdout.split()
v['models_sha256'] = {m: hashlib.sha256(open(m, 'rb').read()).hexdigest() for m in models}
json.dump(v, open('/opt/eval/versions.json', 'w'), indent=1, sort_keys=True)
EOF
RUN chmod -R a+rX /opt/eval
