"""
Pre-build script: minify JavaScript, compact HTML and gzip into a build header.
"""

import gzip
import importlib.util
import json
import os
from pathlib import Path
import re

try:
    Import("env")
except NameError:
    env = None

# SCons executes extra scripts without defining __file__.
PROJECT_DIR = (os.path.abspath(env.get("PROJECT_DIR", ".")) if env is not None
               else os.path.dirname(os.path.abspath(__file__)))
RJS_MIN_PATH = os.path.join(PROJECT_DIR, "third_party", "rjsmin", "rjsmin.py")


def load_js_minifier():
    spec = importlib.util.spec_from_file_location("airbridge_rjsmin", RJS_MIN_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load JavaScript minifier: {RJS_MIN_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if module.__version__ != "1.2.5":
        raise RuntimeError(f"unsupported rjsmin version {module.__version__}; expected 1.2.5")
    return module._make_jsmin(python_only=True)


js_minify = load_js_minifier()


def fill_config_sections(html, source):
    # These two declarative C++ tables are shared with Config's HTTP filter.
    sections = {}
    rows = source.split('} config_sections[] = {', 1)[1].split('};', 1)[0]
    for row in rows.splitlines():
        if not row.strip():
            continue
        match = re.fullmatch(r'\s*\{Config::Section::(\w+), "([^"]+)", "([^"]+)"\},\s*', row)
        if not match:
            raise ValueError(f'unsupported config section: {row}')
        symbol, name, label = match.groups()
        sections[symbol] = {'id': name, 'label': label, 'keys': []}

    rows = source.split('static const KVEntry kv_table[] = {', 1)[1].split('};', 1)[0]
    for row in rows.splitlines():
        if not row.strip():
            continue
        match = re.fullmatch(r'\s*KV_\w+\("([^"]+)".*?, (\w+)\),\s*', row)
        if not match:
            raise ValueError(f'unsupported config field: {row}')
        key, section = match.groups()
        sections[section]['keys'].append(key)

    marker = '/* CONFIG_SECTIONS */ []'
    if html.count(marker) != 1:
        raise ValueError('expected one Config sections placeholder')
    return html.replace(marker, json.dumps(list(sections.values()), separators=(',', ':')))


def minify_html(html):
    # Regex minification corrupts JS strings, regex literals and line comments.
    # Minify JS separately and exclude raw-text blocks from HTML whitespace edits.
    blocks = re.split(
        r'(<!--.*?-->|<(?:script|style|pre|textarea)\b[^>]*>.*?</(?:script|style|pre|textarea)\s*>)',
        html, flags=re.DOTALL | re.IGNORECASE)
    for i, block in enumerate(blocks):
        if i % 2:
            if block.startswith('<!--'):
                blocks[i] = ''
            elif re.match(r'<script\b', block, re.IGNORECASE):
                opening = block.index('>') + 1
                closing = block.lower().rindex('</script')
                blocks[i] = block[:opening] + js_minify(block[opening:closing]) + block[closing:]
        else:
            block = re.sub(r'>\s+<', '><', block)
            blocks[i] = re.sub(r'\s+', ' ', block)
    return ''.join(blocks).strip()


def generate_header(project_dir, output_dir):
    html_path = os.path.join(project_dir, "www", "index.html")
    header_path = os.path.join(output_dir, "web_ui_generated.h")
    if not os.path.exists(html_path):
        print(f"[web_ui] WARNING: {html_path} not found")
        return
    raw = Path(html_path).read_text(encoding="utf-8")
    raw = fill_config_sections(raw, Path(project_dir, 'src', 'app_config.cpp').read_text(encoding='utf-8'))
    minified = minify_html(raw)
    compressed = gzip.compress(minified.encode("utf-8"), compresslevel=9, mtime=0)
    lines = [
        "// Auto-generated from www/index.html, do not edit!\n",
        "#pragma once\n",
        "#include <stdint.h>\n\n",
        f"#define HTML_PAGE_GZ_SIZE {len(compressed)}\n\n",
        "static const uint8_t HTML_PAGE_GZ[] PROGMEM = {\n",
    ]
    for i in range(0, len(compressed), 16):
        chunk = compressed[i:i+16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    lines.append("};\n")
    content = "".join(lines)
    header = Path(header_path)
    if not header.exists() or header.read_text(encoding="utf-8") != content:
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(content, encoding="utf-8")
        print(f"[web_ui] {html_path} ({len(raw)} -> {len(minified)} minified -> {len(compressed)} gzipped)")


if env is not None:
    output_dir = os.path.join(env.subst("$BUILD_DIR"), "generated")
    generate_header(PROJECT_DIR, output_dir)
    env.AppendUnique(CPPPATH=[output_dir])
elif __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", help="Directory for the generated header")
    generate_header(PROJECT_DIR, parser.parse_args().output_dir)
