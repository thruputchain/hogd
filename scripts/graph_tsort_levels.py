#!/usr/bin/env python3
import argparse
import hashlib
import re
import subprocess
from collections import defaultdict, deque
from pathlib import Path


DOT_ID = r"[A-Za-z_][A-Za-z0-9_]*"
NODE_RE = re.compile(rf"^\s*({DOT_ID})\s+\[label=(.*)\];\s*$")
EDGE_RE = re.compile(rf"^\s*({DOT_ID})\s*->\s*({DOT_ID})\s*;\s*$")
TIME_RE = re.compile(r"(wait|run|total):\s*([0-9.]+)(us|ms|s)")
TIME_FACTORS = {
    "us": 0.000001,
    "ms": 0.001,
    "s": 1.0,
}


def node_sort_key(node_id):
    if node_id == "dag_start" or node_id == "start":
        return (-1, 0, node_id)
    if node_id.startswith("n") and node_id[1:].isdigit():
        return (0, int(node_id[1:]), node_id)
    if node_id == "dag_end" or node_id == "end":
        return (1, 0, node_id)
    return (0, 1_000_000_000, node_id)


def wrap_words(text, width):
    lines = []
    current = []
    current_len = 0

    for word in text.split():
        next_len = len(word) if not current else current_len + 1 + len(word)
        if current and next_len > width:
            lines.append(" ".join(current))
            current = [word]
            current_len = len(word)
        else:
            current.append(word)
            current_len = next_len

    if current:
        lines.append(" ".join(current))
    return lines


def compact_label(label):
    if not (label.startswith('"') and label.endswith('"')):
        return label

    body = label[1:-1]
    parts = body.split(r"\n")
    if not parts:
        return label

    first_line = parts[0]
    if ": " in first_line:
        prefix, name = first_line.split(": ", 1)
        wrapped_name = wrap_words(name, 18)
        parts[0:1] = [f"{prefix}: {wrapped_name[0]}"] + wrapped_name[1:]
    else:
        parts[0:1] = wrap_words(first_line, 22)

    return '"' + r"\n".join(parts) + '"'


def general_title(label):
    if not (label.startswith('"') and label.endswith('"')):
        return label

    first_line = label[1:-1].split(r"\n", 1)[0]
    if ": " in first_line:
        return first_line.split(": ", 1)[1]
    return first_line


def title_color(title, colors):
    digest = hashlib.sha256(title.encode("utf-8")).digest()
    return colors[int.from_bytes(digest[:4], "big") % len(colors)]


def title_group(title):
    group = re.sub(r"[^a-zA-Z0-9]+", "_", title).strip("_").lower()
    return group or "node"


def title_order(node_labels):
    titles = sorted(
        {general_title(label) for label in node_labels.values()},
        key=lambda title: min(
            node_sort_key(node_id)
            for node_id, label in node_labels.items()
            if general_title(label) == title
        ),
    )
    return {title: index for index, title in enumerate(titles)}


def ordered_level_nodes(nodes, node_labels, title_columns):
    return sorted(
        nodes,
        key=lambda node_id: (
            title_columns[general_title(node_labels[node_id])],
            node_sort_key(node_id),
        ),
    )


def repeated_title_alignment_edges(grouped, node_labels, title_columns):
    by_title_level = defaultdict(lambda: defaultdict(list))
    for level, nodes in grouped.items():
        for node_id in ordered_level_nodes(nodes, node_labels, title_columns):
            by_title_level[general_title(node_labels[node_id])][level].append(node_id)

    edges = []
    for _title, nodes_by_level in by_title_level.items():
        if len(nodes_by_level) < 2:
            continue

        previous_anchor = None
        for level in sorted(nodes_by_level):
            # If several same-title nodes share a level, the rightmost one becomes
            # the continuation anchor for the vertical column below it.
            anchor = nodes_by_level[level][-1]
            if previous_anchor is not None:
                edges.append((previous_anchor, anchor))
            previous_anchor = anchor

    return edges


def parse_node_times(label):
    times = {}
    for name, value, unit in TIME_RE.findall(label):
        times[name] = float(value) * TIME_FACTORS[unit]
    return times


def format_duration(seconds):
    if seconds < 0.001:
        return f"{seconds * 1_000_000:.0f}us"
    if seconds < 1:
        return f"{seconds * 1_000:.3f}ms"
    return f"{seconds:.3f}s"


def critical_run_label(nodes, node_labels, critical_nodes):
    critical_level_nodes = [node_id for node_id in nodes if node_id in critical_nodes]
    if not critical_level_nodes:
        return None

    node_id = max(
        critical_level_nodes,
        key=lambda current: parse_node_times(node_labels[current]).get("run", 0.0),
    )
    run = parse_node_times(node_labels[node_id]).get("run")
    if run is None:
        return None
    return f"critical run: {format_duration(run)}"


def level_label(level, nodes, node_labels, critical_nodes):
    waits = []
    totals = []
    for node_id in nodes:
        times = parse_node_times(node_labels[node_id])
        if "wait" in times:
            waits.append(times["wait"])
        if "total" in times:
            totals.append(times["total"])

    critical_run = critical_run_label(nodes, node_labels, critical_nodes)
    critical_line = f"\\n{critical_run}" if critical_run else ""

    if not waits or not totals:
        return f"Level {level}{critical_line}"

    first_start = min(waits)
    last_finish = max(totals)
    delta = last_finish - first_start
    return (
        f"Level {level}\\n"
        f"start: {format_duration(first_start)}\\n"
        f"finish: {format_duration(last_finish)}\\n"
        f"delta: {format_duration(delta)}"
        f"{critical_line}"
    )


def slowest_nodes_by_level(grouped, node_labels):
    slowest = set()
    for _level, nodes in grouped.items():
        best_node = None
        best_run = None
        for node_id in nodes:
            times = parse_node_times(node_labels[node_id])
            run = times.get("run")
            if run is None:
                continue
            if best_run is None or run > best_run:
                best_node = node_id
                best_run = run
        if best_node is not None:
            slowest.add(best_node)
    return slowest


def slowest_path(node_labels, edges, levels):
    successors = defaultdict(list)
    indegree = {node_id: 0 for node_id in node_labels}

    for source, target in edges:
        successors[source].append(target)
        indegree[target] = indegree.get(target, 0) + 1
        indegree.setdefault(source, 0)

    source_nodes = sorted(
        [node_id for node_id, degree in indegree.items() if degree == 0],
        key=node_sort_key,
    )
    queue = deque(source_nodes)
    topo_order = []
    while queue:
        node_id = queue.popleft()
        topo_order.append(node_id)
        for target in sorted(successors[node_id], key=node_sort_key):
            indegree[target] -= 1
            if indegree[target] == 0:
                queue.append(target)

    if len(topo_order) != len(node_labels):
        raise ValueError("cycle detected; cannot compute slowest path")

    run_times = {
        node_id: parse_node_times(label).get("run", 0.0)
        for node_id, label in node_labels.items()
    }
    best_time = {node_id: float("-inf") for node_id in node_labels}
    for node_id in source_nodes:
        best_time[node_id] = run_times[node_id]
    predecessor = {node_id: None for node_id in node_labels}

    for source in topo_order:
        for target in successors[source]:
            candidate = best_time[source] + run_times[target]
            if candidate > best_time[target]:
                best_time[target] = candidate
                predecessor[target] = source

    if not best_time:
        return set()

    deepest_level = max(levels.values())
    terminal_nodes = [
        node_id
        for node_id, level in levels.items()
        if level == deepest_level
    ]
    end = max(terminal_nodes, key=lambda node_id: best_time[node_id])
    path_nodes = {end}
    path_edges = set()
    while predecessor[end] is not None:
        source = predecessor[end]
        path_edges.add((source, end))
        path_nodes.add(source)
        end = source

    return path_nodes, path_edges


def explicit_terminal_nodes(node_labels):
    return {
        node_id
        for node_id in node_labels
        if node_id in {"start", "dag_start", "end", "dag_end"}
    }


def parse_dot(path):
    node_labels = {}
    edges = []

    for line in path.read_text().splitlines():
        node_match = NODE_RE.match(line)
        if node_match:
            node_labels[node_match.group(1)] = node_match.group(2)
            continue

        edge_match = EDGE_RE.match(line)
        if edge_match:
            source, target = edge_match.groups()
            edges.append((source, target))
            node_labels.setdefault(source, f'"{source}"')
            node_labels.setdefault(target, f'"{target}"')

    return node_labels, edges


def compute_levels(node_labels, edges):
    successors = defaultdict(list)
    indegree = {node_id: 0 for node_id in node_labels}

    for source, target in edges:
        successors[source].append(target)
        indegree[target] = indegree.get(target, 0) + 1
        indegree.setdefault(source, 0)

    levels = {node_id: 0 for node_id in node_labels}
    queue = deque(sorted(
        [node_id for node_id, degree in indegree.items() if degree == 0],
        key=node_sort_key,
    ))

    seen = 0
    while queue:
        node_id = queue.popleft()
        seen += 1

        for target in sorted(successors[node_id], key=node_sort_key):
            levels[target] = max(levels[target], levels[node_id] + 1)
            indegree[target] -= 1
            if indegree[target] == 0:
                queue.append(target)

    if seen != len(indegree):
        raise ValueError("cycle detected; cannot assign topological levels")

    grouped = defaultdict(list)
    for node_id, level in levels.items():
        grouped[level].append(node_id)

    for level in grouped:
        grouped[level].sort(key=node_sort_key)

    return grouped, levels


def write_ranked_dot(node_labels, edges, grouped, levels, output_path, splines):
    colors = [
        "#dbeafe",
        "#fef3c7",
        "#dcfce7",
        "#fee2e2",
        "#ede9fe",
        "#ccfbf1",
        "#fce7f3",
        "#e0e7ff",
        "#ffedd5",
        "#ecfccb",
        "#cffafe",
        "#f3e8ff",
    ]

    title_columns = title_order(node_labels)
    critical_nodes, critical_edges = slowest_path(node_labels, edges, levels)
    critical_nodes |= explicit_terminal_nodes(node_labels)

    lines = [
        "digraph schedulercoord_nominate_levels {",
        f'  graph [rankdir=TB, newrank=true, ranksep=0.75, nodesep=0.08, splines={splines}, pad=0.18, outputorder=edgesfirst];',
        '  node [shape=box, style="rounded,filled", fontname=Helvetica, fontsize=9, margin=0.05];',
        '  edge [color="#555555", arrowsize=0.7];',
        "",
    ]

    for node_id in sorted(node_labels, key=node_sort_key):
        title = general_title(node_labels[node_id])
        fill = title_color(title, colors)
        group = title_group(title)
        extra_attrs = ""
        if node_id in critical_nodes:
            extra_attrs = ', color="#dc2626", penwidth=3'
        lines.append(
            f'  {node_id} [label={compact_label(node_labels[node_id])}, '
            f'fillcolor="{fill}", group="{group}"{extra_attrs}];'
        )

    lines.append("")
    for level in sorted(grouped):
        level_node = f"level_{level}"
        ordered_nodes = ordered_level_nodes(grouped[level], node_labels, title_columns)
        members = " ".join([level_node] + ordered_nodes)
        label = level_label(level, ordered_nodes, node_labels, critical_nodes)
        lines.append(
            f'  {level_node} [label="{label}", shape=plaintext, '
            'fontname=Helvetica, fontsize=14, group="level_summary"];'
        )
        lines.append(f"  {{ rank=same; {members}; }}")

    lines.append("")
    sorted_levels = sorted(grouped)
    for current, next_level in zip(sorted_levels, sorted_levels[1:]):
        lines.append(
            f"  level_{current} -> level_{next_level} [style=invis, weight=100];"
        )

    lines.append("")
    for level in sorted(grouped):
        ordered_nodes = ordered_level_nodes(grouped[level], node_labels, title_columns)
        ordered_rank = [f"level_{level}"] + ordered_nodes
        for source, target in zip(ordered_rank, ordered_rank[1:]):
            lines.append(
                f"  {source} -> {target} "
                "[style=invis, constraint=false, weight=50];"
            )

    lines.append("")
    for source, target in repeated_title_alignment_edges(grouped, node_labels, title_columns):
        lines.append(f"  {source} -> {target} [style=invis, weight=500];")

    lines.append("")
    for source, target in edges:
        if (source, target) in critical_edges:
            lines.append(
                f'  {source} -> {target} '
                '[color="#dc2626", penwidth=3, arrowsize=1.0];'
            )
        else:
            lines.append(f"  {source} -> {target};")

    lines.append("}")
    output_path.write_text("\n".join(lines) + "\n")


def add_svg_level_separators(svg_path):
    text = svg_path.read_text()
    viewbox_match = re.search(
        r'<svg width="[^"]+" height="[^"]+"\s+viewBox="([^"]+)"',
        text,
    )
    if not viewbox_match:
        raise ValueError("could not find SVG viewBox")

    min_x, _min_y, width, _height = map(float, viewbox_match.group(1).split())
    label_positions = []
    for match in re.finditer(
        r'<text[^>]* y="([-0-9.]+)"[^>]*>Level (\d+)</text>',
        text,
    ):
        label_positions.append((int(match.group(2)), float(match.group(1))))

    label_positions.sort()
    if len(label_positions) < 2:
        raise ValueError("could not find at least two SVG level labels")

    left = min_x + 24
    right = min_x + width - 24
    separator_lines = ['<g id="level-separators" class="level-separators">']
    for index in range(len(label_positions) - 1):
        y = (label_positions[index][1] + label_positions[index + 1][1]) / 2
        separator_lines.append(
            f'<line x1="{left:.2f}" y1="{y:.2f}" x2="{right:.2f}" y2="{y:.2f}" '
            'stroke="#777777" stroke-width="1.2" stroke-dasharray="5,7" '
            'opacity="0.75" />'
        )
    separator_lines.append("</g>")

    text = re.sub(
        r'(<text\b(?![^>]*fill=)([^>]*)>critical run: [^<]+</text>)',
        lambda match: match.group(1).replace("<text", '<text fill="#dc2626"', 1),
        text,
    )

    polygon_start = text.find('<polygon fill="white"')
    polygon_end = text.find("/>\n", polygon_start)
    if polygon_start == -1 or polygon_end == -1:
        raise ValueError("could not find SVG background polygon")

    polygon_end += 3
    text = text[:polygon_end] + "\n".join(separator_lines) + "\n" + text[polygon_end:]
    svg_path.write_text(text)


def render_svg(dot_path, svg_path):
    subprocess.run(["dot", "-Tsvg", str(dot_path), "-o", str(svg_path)], check=True)
    add_svg_level_separators(svg_path)


def main():
    parser = argparse.ArgumentParser(
        description="Rewrite a simple Graphviz DOT DAG with explicit topological level ranks."
    )
    parser.add_argument("input", type=Path, help="Input DOT file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output DOT file, default: INPUT.stem + .levels.dot",
    )
    parser.add_argument(
        "--svg",
        type=Path,
        help="Optional SVG output path. When set, runs dot and adds dotted level separators.",
    )
    parser.add_argument(
        "--splines",
        choices=("line", "polyline", "ortho", "spline", "curved"),
        default="polyline",
        help=(
            "Graphviz edge routing style for the generated DOT. "
            "Default: polyline. Use ortho for slower right-angle edges."
        ),
    )
    args = parser.parse_args()

    output = args.output or args.input.with_name(f"{args.input.stem}.levels.dot")
    node_labels, edges = parse_dot(args.input)
    grouped, levels = compute_levels(node_labels, edges)
    write_ranked_dot(node_labels, edges, grouped, levels, output, args.splines)
    print(f"wrote {output} with {len(grouped)} levels and {len(node_labels)} nodes")
    if args.svg:
        render_svg(output, args.svg)
        print(f"wrote {args.svg} with {len(grouped) - 1} dotted level separators")


if __name__ == "__main__":
    main()
