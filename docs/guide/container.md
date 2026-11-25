# Container image

`ghcr.io/butterski/rayopdf` holds the static `rayopdf` binary alone: no shell, no other
program. It is built from the Linux release archive for each release, tagged with the version
(`ghcr.io/butterski/rayopdf:1.1.0`); `latest` names the newest release.

The image runs as user 65532 in the folder `/w`, and its entry point is `rayopdf`, so the
arguments are those of the [command line](cli.md). Mount the folder with your PDFs on `/w`, and
pass `-u` so that the program may write there and the files it writes are yours:

```sh
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/w" ghcr.io/butterski/rayopdf:1.1.0 \
    --convert paper.pdf paper.md
```

A folder:

```sh
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/w" ghcr.io/butterski/rayopdf:1.1.0 \
    --batch pdfs markdown --recursive
```

Keep the inputs and outputs under the mounted folder. If the output cannot be written (exit
code 12), check the `-u` option and the folder's permissions.

The image holds no [OCR add-on](ocr.md). It carries RayoPDF's licence files under
`/usr/share/doc/rayopdf/`.

To build the image yourself from a release archive, see `packaging/container/Dockerfile`.
