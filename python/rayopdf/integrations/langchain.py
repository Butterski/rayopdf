# SPDX-License-Identifier: Apache-2.0
"""LangChain document loader over rayopdf.convert.

    from rayopdf.integrations.langchain import RayoPdfLoader

    documents = RayoPdfLoader("paper.pdf").load()                 # one Document a chunk
    whole = RayoPdfLoader("paper.pdf", mode="single").load()      # one Document, all the Markdown

Needs langchain-core (`pip install langchain-core`, or `pip install rayopdf[langchain]`).
"""

from __future__ import annotations

import os
from typing import Any, Iterator, Union

try:
    from langchain_core.document_loaders import BaseLoader
    from langchain_core.documents import Document
except ImportError as error:
    raise ImportError("rayopdf.integrations.langchain needs langchain-core: pip install langchain-core") from error

from rayopdf import convert

__all__ = ["RayoPdfLoader"]


class RayoPdfLoader(BaseLoader):
    """Loads a PDF as one Document per chunk (`mode="chunks"`, the default: `--format=chunks`, with
    metadata source, chunk, pages [first, last] and heading_path, and overlap with chunk_overlap) or
    as one Document holding the whole Markdown (`mode="single"`, metadata source). Other keyword
    arguments are rayopdf.convert's options (chunk_chars, chunk_overlap, chunk_by, pages, password,
    math, plain, ...), not `format`."""

    def __init__(self, file_path: Union[str, os.PathLike], *, mode: str = "chunks", **options: Any) -> None:
        if mode not in ("chunks", "single"):
            raise ValueError(f"mode takes chunks or single: {mode!r}")
        if "format" in options:
            raise TypeError("RayoPdfLoader chooses the format itself: use mode")
        self.file_path = file_path
        self.mode = mode
        self.options = options

    def lazy_load(self) -> Iterator[Document]:
        source = os.fspath(self.file_path)
        if self.mode == "single":
            result = convert(self.file_path, **self.options)
            yield Document(page_content=result.text, metadata={"source": source})
            return
        result = convert(self.file_path, format="chunks", **self.options)
        for chunk in result.data:
            metadata = {"source": source, "chunk": chunk["chunk"], "pages": chunk["pages"],
                        "heading_path": chunk["heading_path"]}
            if "overlap" in chunk:
                metadata["overlap"] = chunk["overlap"]
            yield Document(page_content=chunk["markdown"], metadata=metadata)
