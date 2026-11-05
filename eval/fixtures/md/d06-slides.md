# Introduction

## Welcome

Sentinel start D06.

- Project status for the fourth quarter
- Presented by the platform team

## Agenda

- Goals for this quarter
- Results so far
- Technical details
- Next steps

# Results

## Highlights

- Release 2.4 shipped on time
- Crash rate fell by 40 %
  - Mostly from the new parser
  - Fuzzing found 12 bugs
- Two new customers signed

## Numbers

| Metric | Q3 | Q4 |
| --- | ---: | ---: |
| Active users | 1,200 | 1,650 |
| Pages per second | 850 | 2,100 |
| Open bugs | 48 | 31 |

## Chart

![chart](img/chart.png)

# Technical Details

## Example code

```python
def convert(path):
    doc = open_pdf(path)
    return to_markdown(doc)
```

## Architecture

- Reader parses objects
- Interpreter emits glyphs
- Layout orders blocks
- Writer produces Markdown

# Next Steps

## Plan

1. Finish table detection
2. Add tagged PDF support
3. Publish benchmarks

Sentinel end D06.
