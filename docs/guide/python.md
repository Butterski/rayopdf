# Python

The `rayopdf` package runs RayoPDF's converter inside your Python process: the converter is a
shared library in the package, so nothing else needs to be installed. It needs Python 3.9 or
newer on Linux x64 (glibc 2.28 or newer) or Windows x64.

## Install

```sh
pip install rayopdf
```

The wheels are also attached to each [release](https://github.com/Butterski/rayopdf/releases/latest)
(`rayopdf-<version>-py3-none-manylinux_2_28_x86_64.whl` for Linux,
`rayopdf-<version>-py3-none-win_amd64.whl` for Windows) for installing without PyPI:

```sh
pip install rayopdf-1.1.0-py3-none-manylinux_2_28_x86_64.whl   # Linux
pip install rayopdf-1.1.0-py3-none-win_amd64.whl                # Windows
```

The LangChain and LlamaIndex loaders and the Haystack component need their own packages: the
extras `rayopdf[langchain]` (`langchain-core`), `rayopdf[llama-index]` (`llama-index-core`) and
`rayopdf[haystack]` (`haystack-ai`) install them.

## Convert

```python
import rayopdf

result = rayopdf.convert("paper.pdf")         # a path, or the PDF's bytes
print(result.text)                            # the Markdown
print(result.pages, result.report["tables"])  # pages converted, a counter of --report

doc = rayopdf.convert("paper.pdf", format="json").data       # the JSON document as a dict
chunks = rayopdf.convert("paper.pdf", format="chunks").data  # a list of chunk dicts
latex = rayopdf.convert("paper.pdf", math="latex", pages="1-3").text
```

`convert(source, **options)` takes a path or the PDF's bytes and the options of
`rayopdf --convert` as keyword arguments:

| Argument | Default | Values |
|---|---|---|
| `format` | `"markdown"` | `"markdown"`, `"json"`, `"chunks"`, `"inspect"`, `"text"`, `"html"` |
| `pages` | all | `3`, `"2-5"`, `"-4"`, or a tuple `(2, None)` |
| `password` | `None` | the user or owner password |
| `images` | `None` (no images) | a folder: image files are written there and linked as `FOLDER/NAME` |
| `math` | `"unicode"` | `"unicode"`, `"latex"` |
| `page_markers`, `front_matter`, `plain` | `False` | `True` turns them on |
| `furniture` | `"drop"` | `"drop"`, `"keep"` |
| `tables` | `"auto"` | `"auto"`, `"html"`, `"pipe"`, `"pipe-latex"` |
| `quotes` | `"keep"` | `"keep"`, `"ascii"` |
| `small_caps` | `"lower"` | `"lower"`, `"keep"` |
| `figure_text` | `"keep"` | `"keep"`, `"drop"` |
| `chunk_chars` | 4,000 | 256 to 1,000,000 bytes a chunk |
| `chunk_overlap` | 0 | bytes of the chunk before each chunk begins with (at most half of `chunk_chars`) |
| `chunk_by` | `"size"` | `"size"`, `"page"`: with `format="chunks"`, a chunk also ends at every page |
| `json_detail` | `"blocks"` | `"blocks"`, `"lines"`, `"words"`: with `format="json"`, each block's lines, or lines and words, too |
| `structure`, `exact` | `True` | `False` is `--no-structure`, `--no-exact` |
| `annotations` | `True` | `True` or `"all"`, `"links"`, `False` or `"none"`: `--annotations` (form fields and FreeText annotations as text, links only, none) |
| `ocr` | `"off"` | `"off"`, `"auto"`, `"force"`: the [OCR add-on](ocr.md), found as the command line finds it |
| `ocr_addon` | `None` | the add-on's path |
| `workers` | 1 | page threads, 1 to 64 |

The [command-line guide](cli.md#options) explains each option, and
[output formats](formats.md) what each format holds.

The result has:

| Attribute | Value |
|---|---|
| `text` | The output: Markdown, or the JSON, JSON Lines or HTML of `format`. |
| `data` | The parsed JSON object for `format="json"`, the list of chunks for `format="chunks"`, else `None`. |
| `report` | The counters of `--report`, as a dict ([report record](formats.md#the-report-record)). |
| `pages` | The pages converted. |

`rayopdf.version()` returns the library's version. A call releases the GIL while it converts,
so several threads can convert at once.

## Errors

A failed conversion raises a `RayoPdfError`. Its `status` is the command line's exit code and
`message` says why. One subclass a status:

| Exception | Status | Cause |
|---|---|---|
| `UsageError` | 2 | An option with an invalid value. |
| `UnreadableError` | 3 | The input cannot be read. |
| `CorruptError` | 10 | Not a PDF, corrupt beyond repair, or past a document limit. |
| `PasswordRequiredError` | 11 | Encrypted, and the password does not open it. |
| `UnwritableError` | 12 | An output (an image file) could not be written. |
| `UnsupportedEncryptionError` | 13 | Encrypted with a security handler RayoPDF does not implement. |

```python
try:
    rayopdf.convert(b"not a pdf")
except rayopdf.CorruptError as error:
    print(error.status, error)
```

## LangChain

```python
from rayopdf.integrations.langchain import RayoPdfLoader

docs = RayoPdfLoader("paper.pdf").load()                  # one Document a chunk
whole = RayoPdfLoader("paper.pdf", mode="single").load()  # one Document, all the Markdown
```

In the default `mode="chunks"`, each `Document` is one chunk of `format="chunks"`, with the
metadata `source`, `chunk`, `pages` (first and last), `heading_path` and, with `chunk_overlap`,
`overlap`. Other keyword arguments are `convert`'s options (`chunk_chars`, `chunk_overlap`,
`chunk_by`, `pages`, `password`, `math`, `plain`, …). Importing the
loader without `langchain-core` installed raises an `ImportError` that names the package.

## LlamaIndex

```python
from rayopdf.integrations.llama_index import RayoPdfReader

documents = RayoPdfReader(chunk_chars=2000).load_data("paper.pdf")
```

`load_data(file, extra_info=None)` returns one `Document` a chunk, with the same metadata as the
LangChain loader plus `extra_info`.

## Haystack

```python
from haystack import Pipeline
from rayopdf.integrations.haystack import RayoPdfToDocument

documents = RayoPdfToDocument().run(sources=["paper.pdf"])["documents"]  # one Document a PDF
pipeline = Pipeline()
pipeline.add_component("converter", RayoPdfToDocument(format="chunks", chunk_chars=2000))
```

`RayoPdfToDocument` is a Haystack 2 component. `run(sources, meta=None)` takes paths and
`ByteStream`s and returns `{"documents": [...]}`: one `Document` a PDF with its Markdown (with
`format="text"` or `"html"`, its plain text or HTML) and the metadata `pages` (the number
converted), or with `format="chunks"` one `Document` a chunk with the LangChain loader's chunk
metadata. Every `Document` also gets `file_path` for a path, a `ByteStream`'s own metadata, and
`meta`: one dictionary for every source, or a list of one a source. A source that cannot be read
or converted is skipped with a warning; an invalid option raises `UsageError`. Other keyword
arguments are `convert`'s options; `to_dict` and `from_dict` save and load them with a pipeline.
Importing the component without `haystack-ai` installed raises an `ImportError` that names the
package.
