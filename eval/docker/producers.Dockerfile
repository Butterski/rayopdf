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

# The Eisvogel pandoc LaTeX template (BSD-3-Clause), for the xelatex-eisvogel producer: READoc's
# GitHub subset was made with pandoc + Eisvogel, so this development producer shows the same layout
# without opening the holdout.
RUN curl -fsSL -o /tmp/eisvogel.tar.gz \
      https://github.com/Wandmalfarbe/pandoc-latex-template/releases/download/v3.5.1/Eisvogel-3.5.1.tar.gz \
    && echo '321ff753b4017e331575651bf97ba51004650762fe116be97a4c60a68c570181  /tmp/eisvogel.tar.gz' | sha256sum -c - \
    && mkdir -p /opt/eval/templates \
    && tar -xzf /tmp/eisvogel.tar.gz -C /tmp Eisvogel-3.5.1/eisvogel.latex Eisvogel-3.5.1/LICENSE \
    && mv /tmp/Eisvogel-3.5.1/eisvogel.latex /opt/eval/templates/eisvogel.latex \
    && mv /tmp/Eisvogel-3.5.1/LICENSE /opt/eval/templates/eisvogel-LICENSE \
    && rm -rf /tmp/eisvogel.tar.gz /tmp/Eisvogel-3.5.1

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
