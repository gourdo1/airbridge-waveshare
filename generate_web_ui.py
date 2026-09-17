"""
Pre-build script: minify JavaScript, compact HTML and gzip into src/web_ui_html.h
"""

import gzip
import importlib.util
import os
import re

try:
    Import("env")
except NameError:
    env = None

# SCons executes extra scripts without defining __file__.
PROJECT_DIR = (os.path.abspath(env.get("PROJECT_DIR", ".")) if env is not None
               else os.path.dirname(os.path.abspath(__file__)))
GENERATOR_PATH = os.path.join(PROJECT_DIR, "generate_web_ui.py")
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


def generate_header(project_dir):
    html_path = os.path.join(project_dir, "www", "index.html")
    header_path = os.path.join(project_dir, "src", "web_ui_html.h")
    if not os.path.exists(html_path):
        print(f"[web_ui] WARNING: {html_path} not found")
        return
    needs_update = not os.path.exists(header_path) or \
                   max(os.path.getmtime(html_path), os.path.getmtime(GENERATOR_PATH),
                       os.path.getmtime(RJS_MIN_PATH)) > os.path.getmtime(header_path)

    if needs_update:
        with open(html_path, "r", encoding="utf-8") as f:
            raw = f.read()

        minified = minify_html(raw)
        compressed = gzip.compress(minified.encode("utf-8"), compresslevel=9, mtime=0)

        with open(header_path, "w", encoding="utf-8") as f:
            f.write("// Auto-generated from www/index.html, do not edit!\n")
            f.write("#pragma once\n")
            f.write("#include <stdint.h>\n\n")
            f.write(f"#define HTML_PAGE_GZ_SIZE {len(compressed)}\n\n")
            f.write("static const uint8_t HTML_PAGE_GZ[] PROGMEM = {\n")
            for i in range(0, len(compressed), 16):
                chunk = compressed[i:i+16]
                f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
            f.write("};\n")

        print(f"[web_ui] {html_path} ({len(raw)} -> {len(minified)} minified -> {len(compressed)} gzipped)")


if env is not None:
    generate_header(PROJECT_DIR)
elif __name__ == "__main__":
    generate_header(PROJECT_DIR)
