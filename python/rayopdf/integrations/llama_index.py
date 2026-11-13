# SPDX-License-Identifier: Apache-2.0
"""LlamaIndex reader over rayopdf.convert.

    from rayopdf.integrations.llama_index import RayoPdfReader

    documents = RayoPdfReader().load_data("paper.pdf")
    reader = SimpleDirectoryReader("papers", file_extractor={".pdf": RayoPdfReader()})

Needs llama-index-core (`pip install llama-index-core`, or `pip install rayopdf[llama-index]`).
"""

from __future__ import annotations

import os
from typing import Any, Dict, List, Optional, Union

try:
    from llama_index.core.readers.base import BaseReader
    from llama_index.core.schema import Document
except ImportError as error:
    raise ImportError("rayopdf.integrations.llama_index needs llama-index-core: pip install llama-index-core") \
        from error

from rayopdf import convert

__all__ = ["RayoPdfReader"]


class RayoPdfReader(BaseReader):
    """Reads a PDF as one Document per chunk (`--format=chunks`), with metadata source, chunk,
    pages [first, last] and heading_path over `extra_info`. Keyword arguments are rayopdf.convert's
    options (chunk_chars, password, math, plain, ...), not `format`."""

    def __init__(self, **options: Any) -> None:
        super().__init__()
        if "format" in options:
            raise TypeError("RayoPdfReader reads chunks: leave out format")
        self._options = options

    def load_data(self, file: Union[str, os.PathLike], extra_info: Optional[Dict[str, Any]] = None) -> List[Document]:
        source = os.fspath(file)
        result = convert(file, format="chunks", **self._options)
        return [Document(text=chunk["markdown"],
                         metadata={**(extra_info or {}), "source": source, "chunk": chunk["chunk"],
                                   "pages": chunk["pages"], "heading_path": chunk["heading_path"]})
                for chunk in result.data]
