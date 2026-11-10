#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""READoc's evaluator, document by document: the aggregate scores and every document's metrics.

Usage:
    readoc_docs.py --evaluator DIR --gold-dir G --pred-dir P --result-json R --docs-json D
                   [--jobs N]

Runs in the rayopdf-eval-readoc image. DIR is `src/evaluation` of a READoc clone; its code is
imported from there and never copied: `suite_e2e.file_evaluate` scores one document,
`suite_e2e.e2e_evaluate` aggregates a directory. For development suites only (the holdout is
scored with `suite_e2e.py` alone and has no per-document record).

Every `<stem>.md` of G is scored against P/<stem>.md (missing = empty text), N processes at a
time (default: the CPUs of the container). The evaluator compares every table of the ground
truth with every table of the output (`TableTEDS`, a tree edit distance), which takes minutes for
one document with many tables, so the table pairs are work items of their own:

1. `file_evaluate` per document, with a `TableTEDS` that notes the pairs it is asked for and
   answers 0, and a `TableEvaluate` that notes the two table lists it is given.
2. The evaluator's `TableTEDS` on every noted pair, largest first.
3. The evaluator's `TableEvaluate` again on the noted lists of each document that had pairs,
   with a `TableTEDS` that answers from step 2: the document's table metrics.
4. `e2e_evaluate(G, P, R)` unchanged, with `file_evaluate` answering from steps 1 to 3: R is
   the file `suite_e2e.py` writes.
5. The names `e2e_evaluate` gives its metrics (`text_eds`, ...) are learned from it: one more
   call on a one-document directory whose "scores" are the positions in the list of all
   metrics, so that each name's value tells which metric it reports.

When a worker dies (out of memory on a long document), what is left of that step runs one item
at a time.

D: `{"schema":1, "documents":n, "empty":[stems without output], "keys":{name: metric},
"aggregate":R's content, "docs":{stem:{name: value|null}}, "metrics":{stem:{metric: value}},
"seconds":{stem: CPU seconds of its evaluation}}`
where a metric is `<task>.<group>.<measure>` as the evaluator nests them (for example
`plain.metrics.edit_dist_sim`) and null marks a document the evaluator leaves out of that mean
(no table in the ground truth, ...). Exit 1 when the mean of a name over its documents is not
R's value.
"""

import argparse
import concurrent.futures
import concurrent.futures.process
import glob
import json
import os
import sys
import tempfile
import time


class Scores:
    """What `e2e_evaluate` reads of a `file_evaluate` result."""

    def __init__(self, metric_dict):
        self.metric_dict = metric_dict


def ReadPair(gold_path, pred_dir):
    # The same reads as e2e_evaluate, so that the texts are the same keys.
    with open(gold_path, "r") as f:
        gold = f.read()
    try:
        with open(os.path.join(pred_dir, gold_path.split("/")[-1]), "r") as f:
            pred = f.read()
    except Exception:
        pred = ""
    return gold, pred


def ScoreDocument(job):
    """Step 1: (scores, TableEvaluate's arguments or None, table pairs, seconds)."""
    import scoring
    import suite_e2e
    gold, pred = ReadPair(*job)
    pairs, lists = [], []

    class NotePairs:
        def __call__(self, *tables):
            pairs.append(tables)
            return 0.0

    def NoteLists(*args):
        lists.append(args)
        return table_evaluate(*args)

    table_teds, table_evaluate = scoring.TableTEDS, suite_e2e.TableEvaluate
    scoring.TableTEDS, suite_e2e.TableEvaluate = NotePairs, NoteLists
    start = time.process_time()
    try:
        result = suite_e2e.file_evaluate(gold, pred)
    finally:
        scoring.TableTEDS, suite_e2e.TableEvaluate = table_teds, table_evaluate
    return ({task: result[task].metric_dict for task in result}, lists[0] if pairs else None,
            sorted(set(pairs)), time.process_time() - start)


def ScorePair(tables):
    """Step 2: the evaluator's TEDS of one pair of tables."""
    import scoring
    start = time.process_time()
    return scoring.TableTEDS()(*tables), time.process_time() - start


def ScoreTables(job):
    """Step 3: the table metrics of one document from the TEDS of its pairs."""
    import scoring
    lists, teds = job

    class Known:
        def __call__(self, *tables):
            return teds[tables]

    table_teds = scoring.TableTEDS
    scoring.TableTEDS = Known
    start = time.process_time()
    try:
        return scoring.TableEvaluate(*lists).metric_dict, time.process_time() - start
    finally:
        scoring.TableTEDS = table_teds


def Parallel(function, jobs, workers):
    """[function(job)] in the order of jobs."""
    done = {}
    try:
        with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(function, job): i for i, job in enumerate(jobs)}
            for future in concurrent.futures.as_completed(futures):
                done[futures[future]] = future.result()
    except concurrent.futures.process.BrokenProcessPool:
        left = [i for i in range(len(jobs)) if i not in done]
        print("readoc_docs: a worker died; %d item(s) of %s one at a time"
              % (len(left), function.__name__), file=sys.stderr)
        for i in left:
            done[i] = function(jobs[i])
    return [done[i] for i in range(len(jobs))]


def Leaves(scores):
    """{"task.group.measure": number} of one document."""
    out = {}
    for task, groups in scores.items():
        for group, measures in groups.items():
            for measure, value in measures.items():
                if isinstance(value, (int, float)) and not isinstance(value, bool):
                    out["%s.%s.%s" % (task, group, measure)] = float(value)
    return out


def LearnNames(suite_e2e, metrics):
    """{name: metric} of e2e_evaluate's result for a suite whose documents have `metrics`."""
    probe = {}
    for index, metric in enumerate(metrics):
        task, group, measure = metric.split(".")
        probe.setdefault(task, {}).setdefault(group, {})[measure] = float(index)
    suite_e2e.file_evaluate = lambda gold, pred: {t: Scores(g) for t, g in probe.items()}
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "gold"))
        with open(os.path.join(tmp, "gold", "probe.md"), "w") as f:
            f.write("probe\n")
        target = os.path.join(tmp, "names.json")
        suite_e2e.e2e_evaluate(os.path.join(tmp, "gold"), os.path.join(tmp, "pred"), target)
        with open(target) as f:
            return {name: metrics[int(value)] for name, value in json.load(f).items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--evaluator", required=True)
    ap.add_argument("--gold-dir", required=True)
    ap.add_argument("--pred-dir", required=True)
    ap.add_argument("--result-json", required=True)
    ap.add_argument("--docs-json", required=True)
    ap.add_argument("--jobs", type=int, default=len(os.sched_getaffinity(0)))
    args = ap.parse_args()
    workers = max(1, args.jobs)

    sys.path.insert(0, args.evaluator)
    import suite_e2e

    # Long documents and large tables first: they decide the wall time.
    paths = sorted(glob.glob("%s/*.md" % args.gold_dir), key=lambda p: -os.path.getsize(p))
    stems = [os.path.basename(p)[:-len(".md")] for p in paths]
    documents = Parallel(ScoreDocument, [(p, args.pred_dir) for p in paths], workers)
    seconds = [d[3] for d in documents]

    pairs = sorted({pair for d in documents for pair in d[2]},
                   key=lambda pair: -len(pair[0]) * len(pair[1]))
    teds = dict(zip(pairs, Parallel(ScorePair, pairs, workers)))
    with_pairs = [i for i, d in enumerate(documents) if d[1] is not None]
    tables = Parallel(ScoreTables, [(documents[i][1], {pair: teds[pair][0]
                                                       for pair in documents[i][2]})
                                    for i in with_pairs], workers)
    for i, (metric_dict, spent) in zip(with_pairs, tables):
        documents[i][0]["table"] = metric_dict
        seconds[i] += spent + sum(teds[pair][1] for pair in documents[i][2])

    by_text, per_doc, empty = {}, {}, []
    for path, stem, document in zip(paths, stems, documents):
        gold, pred = ReadPair(path, args.pred_dir)
        by_text[(gold, pred)] = {t: Scores(g) for t, g in document[0].items()}
        per_doc[stem] = Leaves(document[0])
        if not pred.strip():
            empty.append(stem)

    evaluate = suite_e2e.file_evaluate
    suite_e2e.file_evaluate = lambda gold, pred: by_text[(gold, pred)]
    suite_e2e.e2e_evaluate(args.gold_dir, args.pred_dir, args.result_json)
    with open(args.result_json) as f:
        aggregate = json.load(f)
    metrics = sorted({m for leaves in per_doc.values() for m in leaves})
    names = LearnNames(suite_e2e, metrics)
    suite_e2e.file_evaluate = evaluate

    docs = {s: {name: per_doc[s].get(metric) for name, metric in names.items()} for s in stems}
    with open(args.docs_json, "w") as f:
        json.dump({"schema": 1, "documents": len(stems), "empty": sorted(empty), "keys": names,
                   "aggregate": aggregate, "docs": docs, "metrics": per_doc,
                   "seconds": {s: round(t, 2) for s, t in zip(stems, seconds)}},
                  f, indent=1, sort_keys=True)
        f.write("\n")

    bad = []
    for name, value in aggregate.items():
        values = [docs[s][name] for s in stems if docs[s].get(name) is not None]
        if not values or abs(sum(values) / len(values) - value) > 1e-9:
            bad.append(name)
    if bad:
        print("readoc_docs: the per-document means differ from the aggregate: %s"
              % ", ".join(bad), file=sys.stderr)
        return 1
    print("readoc_docs: %d documents, %d without output, %d table pairs, %d metrics, "
          "%.0f CPU seconds" % (len(stems), len(empty), len(pairs), len(metrics), sum(seconds)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
