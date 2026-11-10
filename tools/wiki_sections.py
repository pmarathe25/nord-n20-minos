#!/usr/bin/env python3
"""Extract Java Edition passages from a saved Minecraft Wiki action=parse response."""

import argparse
from dataclasses import dataclass, field
from html.parser import HTMLParser
import json
import re
import sys


MAX_RESPONSE = 8 * 1024 * 1024
VOID = set('area base br col embed hr img input link meta param source track wbr'.split())
SKIP_TAGS = set('aside audio button figcaption figure footer form header nav script style svg video'.split())
SKIP_CLASSES = set('''
    gallery hatnote metadata msgbox mw-editsection mw-empty-elt mw-references-wrap
    navbox navigation-not-searchable noexcerpt noprint reference references sidebar toc
'''.split())
OMIT = {'external links', 'gallery', 'navigation', 'references', 'renders', 'screenshots', 'see also'}
ON_REQUEST = {
    'achievements', 'advancements', 'data history', 'data values', 'development',
    'history', 'issues', 'notes', 'sounds', 'trivia', 'videos',
}
OTHER_EDITION = re.compile(
    r'\b(?:Bedrock Edition|Pocket Edition|Legacy Console Edition|Minecraft Education|'
    r'(?:New Nintendo|PlayStation|Xbox|Wii)\w*(?: [\w]+)* Edition)\b', re.I
)
SHORT_EDITION = re.compile(r'\b(?:BE|PE|LCE)\b')
EDITION_LABEL = re.compile(
    r'(Java Edition|JE|Bedrock Edition|BE|Pocket Edition|Legacy Console Edition|Minecraft Education)\s*:?$', re.I
)
EDITION_VALUE = re.compile(r'\b(Java Edition|JE|Bedrock Edition|BE)\s*:')
JAVA_COMPARISON = re.compile(
    r',?\s+but (?:it|they) (?:can|cannot|do|does|is|are) in Bedrock Edition[.!?]?$', re.I
)
EDITION_PREFIX = re.compile(
    r'^(?:In )?(Java Edition|Bedrock Edition|Pocket Edition|Legacy Console Edition|Minecraft Education)[,:]', re.I
)
EXCLUSIVE = re.compile(r'is exclusive to (?:Bedrock Edition|Pocket Edition|Minecraft Education)', re.I)
DROP = '\x00'  # Preserve scope markers until text can be split into sentences.


@dataclass
class Element:
    tag: str
    attrs: dict = field(default_factory=dict)
    children: list = field(default_factory=list)

    @property
    def classes(self):
        return set((self.attrs.get('class') or '').split())

    def elements(self, *tags):
        return (child for child in self.children
                if isinstance(child, Element) and (not tags or child.tag in tags))


class ArticleHTML(HTMLParser):
    """Build a tree from the balanced article fragment returned by MediaWiki."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = [Element('document')]

    def handle_starttag(self, tag, attrs):
        if len(self.stack) > 128:
            raise ValueError('article HTML nested too deeply')
        node = Element(tag, dict(attrs))
        self.stack[-1].children.append(node)
        if tag not in VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID:
            self.stack.pop()

    def handle_endtag(self, tag):
        for index in range(len(self.stack) - 1, 0, -1):
            if self.stack[index].tag == tag:
                del self.stack[index:]
                break

    def handle_data(self, data):
        self.stack[-1].children.append(data)


def walk(node):
    yield node
    for child in node.elements():
        yield from walk(child)


def excluded(node):
    return node.tag in SKIP_TAGS or bool(node.classes & SKIP_CLASSES) or 'hidden' in node.attrs


def other_edition(text):
    return bool(OTHER_EDITION.search(text) or SHORT_EDITION.search(text))


def edition_label(text):
    match = EDITION_LABEL.fullmatch(text)
    if match:
        return 'java' if match[1].casefold() in {'java edition', 'je'} else 'other'
    if text.endswith(' Edition') and other_edition(text):
        return 'other'
    return None


def plain(node):
    if isinstance(node, str):
        return node
    if node.tag == 'sup' and node.classes & {'Inline-Template', 'reference'}:
        labels = ' '.join(child.attrs.get('title') or '' for child in walk(node))
        return DROP if other_edition(labels) or 'only to upcoming versions' in labels else ''
    if excluded(node) or node.tag in {'dl', 'img', 'ol', 'table', 'ul'}:
        return ''
    if node.tag == 'br':
        return '\n'
    text = plain_children(node)
    return '\n' + text + '\n' if node.tag in {'div', 'li', 'p'} else text


def plain_children(node):
    return ''.join(plain(child) for child in node.children)


def compact(text):
    return ' '.join(text.replace('\u200b', '').replace('\u200c', '').split())


def java_text(text):
    kept = []
    scope = None
    # Inline edition badges sometimes qualify one comma-separated list entry.
    text = re.sub(r',\s*[^,.;:\n\x00]+?' + DROP + r'\s*(?=,)', '', text)
    # A qualifier immediately after punctuation still labels the preceding sentence.
    text = re.sub(r'([.!?])[ \u200b\u200c]*' + DROP, DROP + r'\1', text)
    for part in re.split(r'(?<=[.!?])\s+|\n+', text):
        part = compact(part)
        prefix = EDITION_PREFIX.match(part)
        if prefix:
            scope = edition_label(prefix[1])
        elif part.casefold().startswith(('in both editions,', 'in both versions,')):
            scope = None
        if DROP in part:
            continue
        if prefix and scope == 'java':
            part = JAVA_COMPARISON.sub('.', part)
        if part and scope != 'other' and not other_edition(part):
            kept.append(part)
    return ' '.join(kept)


def table_rows(table):
    for child in table.elements():
        if child.tag == 'tr':
            yield child
        elif child.tag in {'thead', 'tbody', 'tfoot'}:
            yield from child.elements('tr')


def java_value(text):
    """Select a Java-labeled value before removing ambiguous edition prose."""
    labels = list(EDITION_VALUE.finditer(text))
    if not labels:
        return java_text(text)
    for i, label in enumerate(labels):
        if label[1] in {'JE', 'Java Edition'}:
            end = labels[i + 1].start() if i + 1 < len(labels) else len(text)
            return java_text(text[label.end():end])
    return ''


def history_rows(rows):
    version = ''
    remaining = 0
    for row in rows:
        tags = list(row.elements('th', 'td'))
        header_cells = [cell for cell in tags if cell.tag == 'th']
        headings = [compact(plain(cell)) for cell in header_cells]
        changes = [java_text(plain(cell)) for cell in tags if cell.tag == 'td']
        if len(headings) == 1 and edition_label(headings[0]):
            if edition_label(headings[0]) == 'other':
                return
            continue
        inherited = version if remaining else ''
        remaining = max(remaining - 1, 0)
        if not any(changes) or any(other_edition(text) or DROP in text for text in headings):
            continue
        if headings and (header_cells[0].attrs.get('rowspan') or '').isdecimal():
            version = headings[0]
            remaining = int(header_cells[0].attrs['rowspan']) - 1
        yield ' '.join(filter(None, [inherited, *headings, *changes]))


def extract_table(table):
    rows = list(table_rows(table))
    kind = table.attrs.get('data-description') or ''
    if not rows or not table.classes & {'wikitable', 'infobox-rows'}:
        return
    if kind == 'History':
        yield from history_rows(rows)
        return
    caption = next(table.elements('caption'), None)
    caption_text = plain(caption) if caption is not None else ''
    if edition_label(compact(caption_text)) == 'other':
        return
    title = java_text(caption_text)
    headers = []
    edition = None
    for row in rows:
        cells = list(row.elements('th', 'td'))
        if len(cells) == 1 and cells[0].tag == 'th':
            label = edition_label(compact(plain(cells[0])))
            if label:
                edition = label
                headers = []
                continue
        if edition == 'other':
            continue
        if not cells or any(cell.attrs.get('colspan', '1') != '1' or
                            cell.attrs.get('rowspan', '1') != '1' for cell in cells):
            continue
        if all(cell.tag == 'th' for cell in cells):
            headers = [compact(plain(cell)) for cell in cells]
            continue
        values = [java_value(plain(cell)) for cell in cells]
        if 'infobox-rows' in table.classes and len(values) == 2 and all(values):
            yield f'{values[0]}: {values[1]}'
        elif len(headers) == len(cells) and not any(
            DROP in label or other_edition(label) and not edition_label(label) for label in headers
        ):
            labeled = [f'{label}: {value}' for label, value in zip(headers, values)
                       if label and value and edition_label(label) != 'other']
            if labeled:
                yield '; '.join(filter(None, [title, *labeled]))


def extract_article(payload):
    """Return plain-text blocks with a section anchor, hierarchy and revision."""
    page = payload.get('parse', payload)
    title, revision, html = page.get('title'), page.get('revid'), page.get('text')
    if isinstance(html, dict):
        html = html.get('*')
    if not isinstance(title, str) or type(revision) is not int or revision <= 0 or not isinstance(html, str):
        raise ValueError('expected an action=parse response with title, revid and text')
    result = {'title': title, 'revision': revision, 'sections': []}
    if re.match(r'^(?:Bedrock Edition|Pocket Edition|Minecraft Education)\b', title, re.I):
        return result
    parser = ArticleHTML()
    parser.feed(html)
    parser.close()
    root = next((node for node in walk(parser.stack[0]) if 'mw-parser-output' in node.classes), None)
    if root is None:
        raise ValueError('MediaWiki article body not found')
    sections = result['sections']
    current = {'heading': 'Overview', 'anchor': '', 'path': [], 'default': True, 'blocks': []}
    sections.append(current)
    headings = []
    article_excluded = False
    label = None

    def visit(node, depth=0):
        nonlocal current, label, article_excluded
        if article_excluded:
            return
        if 'msgbox' in node.classes and EXCLUSIVE.search(compact(plain_children(node))):
            if not headings:
                sections.clear()
                article_excluded = True
            else:
                headings[-1][3] = True
                if current is not None:
                    sections.remove(current)
                current = None
            return
        if excluded(node):
            return
        if 'infobox' in node.classes:
            if current is not None and label != 'other':
                for table in walk(node):
                    if table.tag == 'table' and 'infobox-rows' in table.classes:
                        current['blocks'].extend({'type': 'row', 'text': text} for text in extract_table(table))
            return
        if re.fullmatch('h[1-6]', node.tag):
            level = int(node.tag[1])
            while headings and headings[-1][0] >= level:
                headings.pop()
            heading = compact(plain(node))
            anchor = node.attrs.get('id') or next((child.attrs['id'] for child in walk(node)
                                                  if 'mw-headline' in child.classes and child.attrs.get('id')), '')
            names = [item[1].casefold() for item in headings] + [heading.casefold()]
            omit = names[-1] in OMIT or edition_label(heading) == 'other'
            headings.append([level, heading, anchor, omit])
            label = None
            current = None
            if any(item[3] for item in headings):
                return
            current = {'heading': heading, 'anchor': anchor,
                       'path': [item[2] for item in headings if item[2]],
                       'default': not any(name in ON_REQUEST for name in names), 'blocks': []}
            sections.append(current)
            return
        if node.tag == 'p':
            text = compact(plain(node))
            found = edition_label(text) if text.endswith(':') else None
            if found:
                label = found
                return
        if current is not None and label != 'other':
            if node.tag == 'table':
                current['blocks'].extend({'type': 'row', 'text': text} for text in extract_table(node))
                return
            if node.tag in {'p', 'li', 'dt', 'dd', 'pre'}:
                text = java_text(plain(node))
                if text:
                    block = {'type': 'list' if node.tag == 'li' else 'text', 'text': text}
                    if node.tag == 'li':
                        block['depth'] = depth
                    current['blocks'].append(block)
                if node.tag != 'li' or not text:
                    return
                for child in node.elements('ul', 'ol'):
                    visit(child, depth + 1)
                return
        if node.tag != 'table':
            for child in node.elements():
                visit(child, depth)

    visit(root)
    return result


def select_sections(article, anchor=None, catalog=False):
    sections = article['sections']
    if catalog:
        populated = {anchor for section in sections if section['blocks'] for anchor in section['path']}
        return [dict(section, blocks=len(section['blocks'])) for section in sections
                if section['blocks'] or section['anchor'] in populated]
    if anchor is not None:
        return [section for section in sections if section['blocks'] and
                (section['anchor'] == anchor or anchor in section['path'])]
    return [section for section in sections if section['default'] and section['blocks']]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('response', help='MediaWiki action=parse JSON file, or - for stdin')
    options = parser.add_mutually_exclusive_group()
    options.add_argument('--list', action='store_true', help='list section anchors without article text')
    options.add_argument('--section', metavar='ANCHOR', help='extract one section and its subsections')
    args = parser.parse_args()
    try:
        if args.response == '-':
            raw = sys.stdin.buffer.read(MAX_RESPONSE + 1)
        else:
            with open(args.response, 'rb') as source:
                raw = source.read(MAX_RESPONSE + 1)
        if len(raw) > MAX_RESPONSE:
            raise ValueError('response exceeds size limit')
        article = extract_article(json.loads(raw))
        article['sections'] = select_sections(article, args.section, args.list)
        print(json.dumps(article, indent=2))
    except (OSError, ValueError, AttributeError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
