# SPDX-License-Identifier: Apache-2.0
# Synthetic-corpus producers (pandoc, typst, XeLaTeX, Chromium, WeasyPrint, LibreOffice,
# RayoMD) plus Poppler/qpdf for validation and the text-layer census.
# Build: docker build -f eval/docker/producers.Dockerfile -t rayopdf-eval-producers:1 eval
FROM archlinux:latest

RUN echo 'Server = https://archive.archlinux.org/repos/2026/11/01/$repo/os/$arch' > /etc/pacman.d/mirrorlist \
    && pacman -Syyuu --noconfirm --needed \
      pandoc-cli typst texlive-xetex texlive-latexextra texlive-fontsrecommended texlive-plaingeneric \
      texlive-pictures texlive-langcjk python-weasyprint chromium libreoffice-fresh \
      poppler qpdf python time curl \
      ttf-dejavu noto-fonts noto-fonts-cjk fontconfig \
    && pacman -Scc --noconfirm \
    && rm -rf /var/cache/pacman/pkg/* /var/lib/pacman/sync/*

# pandoc's LaTeX template loads xeCJK when CJKmainfont is set, and xeCJK needs ctexhook.sty
# from texlive-langchinese (not in texlive-langcjk). Kept as its own small layer so adding it
# did not re-download the layer above.
RUN pacman -Sy --noconfirm --needed texlive-langchinese \
    && pacman -Scc --noconfirm \
    && rm -rf /var/cache/pacman/pkg/* /var/lib/pacman/sync/*

RUN curl -fsSL -o /tmp/rayomd.tar.gz \
      https://github.com/Butterski/rayomd/releases/download/v5.3.0/rayomd-5.3.0-linux-x64-static.tar.gz \
    && echo '0a6145a30abcd64e2a0c54dd0650382566f1f07b4f9993b3123db22271e57bf0  /tmp/rayomd.tar.gz' | sha256sum -c - \
    && tar -xzf /tmp/rayomd.tar.gz -C /opt \
    && rm /tmp/rayomd.tar.gz \
    && ln -s /opt/rayomd-5.3.0-linux-x64-static/rayomd /usr/local/bin/rayomd

COPY producers/ /opt/eval/producers/
COPY census/ /opt/eval/census/

RUN python3 - <<'EOF' && chmod -R a+rX /opt/eval
import json, subprocess

def first(cmd):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except Exception as e:
        return f"error: {e}"
    for line in (p.stdout + "\n" + p.stderr).splitlines():
        if line.strip():
            return line.strip()
    return None

v = {
    "pandoc": first(["pandoc", "--version"]),
    "typst": first(["typst", "--version"]),
    "xelatex": first(["xelatex", "--version"]),
    "chromium": first(["chromium", "--version"]),
    "weasyprint": first(["weasyprint", "--version"]),
    "soffice": first(["soffice", "--version"]),
    "rayomd": first(["rayomd", "--version"]),
    "pdfinfo": first(["pdfinfo", "-v"]),
    "qpdf": first(["qpdf", "--version"]),
}
with open("/opt/eval/versions.json", "w") as f:
    json.dump(v, f, indent=2, sort_keys=True)
    f.write("\n")
print(json.dumps(v, indent=2))
EOF
