# SPDX-License-Identifier: Apache-2.0
"""Haystack converter component over rayopdf.convert.

    from rayopdf.integrations.haystack import RayoPdfToDocument

    converter = RayoPdfToDocument()                           # one Document a PDF, its Markdown
    documents = converter.run(sources=["paper.pdf"])["documents"]
    chunked = RayoPdfToDocument(format="chunks", chunk_chars=2000)  # one Document a chunk

Needs haystack-ai (`pip install haystack-ai`, or `pip install rayopdf[haystack]`).
"""

# No `from __future__ import annotations`: Haystack reads run()'s parameter types for the
# component's input sockets, which string annotations would not give.

import logging
import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

try:
    from haystack import Document, component, default_from_dict, default_to_dict
    from haystack.dataclasses import ByteStream
except ImportError as error:
    raise ImportError("rayopdf.integrations.haystack needs haystack-ai: pip install haystack-ai") from error

from rayopdf import RayoPdfError, UsageError, convert

__all__ = ["RayoPdfToDocument"]

logger = logging.getLogger(__name__)

# The formats whose output is one Document a PDF; "chunks" gives one Document a chunk.
_WHOLE_FORMATS = ("markdown", "text", "html")


@component
class RayoPdfToDocument:
    """Converts PDFs (paths or ByteStreams) to Documents: one a PDF with its Markdown (or, with
    `format="text"` or `"html"`, its plain text or HTML) and the meta pages (the pages converted),
    or with `format="chunks"` one a chunk, with the meta chunk, pages [first, last], heading_path
    and, with chunk_overlap, overlap. Each Document's meta also holds the source's ByteStream meta,
    `file_path` for a path, and the `meta` given to run. A source that cannot be read or converted
    is skipped with a warning (an invalid option raises UsageError). Other keyword arguments are
    rayopdf.convert's options (chunk_chars, chunk_overlap, chunk_by, pages, password, math, plain,
    ...)."""

    def __init__(self, format: str = "markdown", **options: Any) -> None:
        if format not in _WHOLE_FORMATS and format != "chunks":
            raise ValueError(f"format takes markdown, text, html or chunks: {format!r}")
        self.format = format
        self.options = options

    def to_dict(self) -> Dict[str, Any]:
        return default_to_dict(self, format=self.format, **self.options)

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "RayoPdfToDocument":
        return default_from_dict(cls, data)

    @component.output_types(documents=List[Document])
    def run(self, sources: List[Union[str, Path, ByteStream]],
            meta: Optional[Union[Dict[str, Any], List[Dict[str, Any]]]] = None) -> Dict[str, List[Document]]:
        """`meta`: one dict for every source, or a list of one dict a source."""
        if meta is None or isinstance(meta, dict):
            metas = [meta or {} for _ in sources]
        else:
            metas = list(meta)
            if len(metas) != len(sources):
                raise ValueError(f"meta has {len(metas)} entries for {len(sources)} sources")
        documents: List[Document] = []
        for source, extra in zip(sources, metas):
            if isinstance(source, ByteStream):
                pdf: Union[str, bytes] = source.data
                base = dict(source.meta or {})
            else:
                pdf = os.fspath(source)
                base = {"file_path": pdf}
            base.update(extra)
            try:
                result = convert(pdf, format=self.format, **self.options)
            except UsageError:
                raise  # the component's options, not the source
            except (OSError, RayoPdfError) as error:
                logger.warning("rayopdf could not convert %s, skipping it: %s", base.get("file_path", "a source"),
                               error)
                continue
            if self.format != "chunks":
                documents.append(Document(content=result.text, meta={**base, "pages": result.pages}))
                continue
            for chunk in result.data:
                chunk_meta = {**base, "chunk": chunk["chunk"], "pages": chunk["pages"],
                              "heading_path": chunk["heading_path"]}
                if "overlap" in chunk:
                    chunk_meta["overlap"] = chunk["overlap"]
                documents.append(Document(content=chunk["markdown"], meta=chunk_meta))
        return {"documents": documents}
