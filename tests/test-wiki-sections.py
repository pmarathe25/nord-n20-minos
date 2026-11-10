#!/usr/bin/env python3
"""Regression tests for the desktop Minecraft Wiki passage parser."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from wiki_sections import extract_article, select_sections


def article(html, title='Example'):
    return extract_article({'parse': {'title': title, 'revid': 123,
                                     'text': f'<nav>Outside</nav><div class="mw-parser-output">{html}</div>'}})


def texts(sections):
    return [block['text'] for section in sections for block in section['blocks']]


class WikiSections(unittest.TestCase):
    def test_sections_have_anchors_and_cut_chrome(self):
        page = article('''
            <header><p>Header noise.</p></header><aside><p>Sidebar noise.</p></aside>
            <div class="navbox"><p>Navbox noise.</p></div>
            <p>Bedrock is a block. <a href="/w/Stone">Stone</a> is another.</p>
            <div class="mw-heading"><h2 id="Usage">Usage</h2>
                <span class="mw-editsection">Edit noise.</span></div>
            <h3 id="Steps">Steps</h3>
            <ol><li>Place a block.<ul><li>Use stone.</li></ul></li><li>Turn left.</li></ol>
            <figure><img src="image.png"><figcaption>Caption noise.</figcaption></figure>
            <p hidden>Hidden noise.</p><script>Script noise.</script>
            <h2 id="Gallery">Gallery</h2><h3 id="Screenshots">Screenshots</h3><p>Gallery noise.</p>
            <footer><p>Footer noise.</p></footer>''')
        self.assertEqual(page['revision'], 123)
        self.assertEqual(texts(select_sections(page)), [
            'Bedrock is a block. Stone is another.', 'Place a block.', 'Use stone.', 'Turn left.'
        ])
        self.assertEqual([s['anchor'] for s in select_sections(page, catalog=True)], ['', 'Usage', 'Steps'])
        steps, = select_sections(page, 'Usage')
        self.assertEqual(steps['path'], ['Usage', 'Steps'])
        self.assertEqual([b['depth'] for b in steps['blocks']], [0, 1, 0])
        self.assertEqual(select_sections(page, 'Gallery'), [])

    def test_inline_and_scoped_editions(self):
        only = '<sup class="Inline-Template"><span title="This statement only applies to Bedrock Edition">BE</span></sup>'
        future = '<sup class="Inline-Template"><span title="This statement applies only to upcoming versions">Upcoming</span></sup>'
        page = article(f'''
            <p>Loot comes from chests, fishing, raid drops{only}, or trading.</p>
            <p>This is exclusive.{only} This fact is shared.</p>
            <p>Future behavior.{future} Present behavior.</p>
            <p>In Java Edition, potion contents are ignored, but they are in Bedrock Edition.</p>
            <p>In Bedrock Edition, use a button. The button also resets the block.</p>
            <h2 id="Spawning">Spawning</h2>
            <h3 id="Java_Edition">Java Edition</h3><p>Java spawning.</p>
            <h3 id="Bedrock_Edition">Bedrock Edition</h3><p>Wrong edition.</p>
            <h4 id="Variant">Variant</h4><p>Wrong descendant.</p>
            <h2 id="Usage">Usage</h2><p><i>Bedrock Edition</i>:</p><p>Wrong scoped block.</p>
            <p><i>Java Edition</i>:</p><p>Java usage.</p>''')
        self.assertEqual(texts(select_sections(page)), [
            'Loot comes from chests, fishing, or trading.', 'This fact is shared.', 'Present behavior.',
            'In Java Edition, potion contents are ignored.', 'Java spawning.', 'Java usage.'
        ])

    def test_article_and_section_notices_are_scoped(self):
        notice = '<div class="msgbox"><p>This feature is exclusive to Bedrock Edition.</p></div>'
        self.assertEqual(article(notice + '<p>Exclusive page.</p>')['sections'], [])
        page = article(f'''
            <p>Shared page.</p><h2 id="Path">Preferred path</h2>{notice}<p>Wrong edition.</p>
            <h3 id="Child">Child</h3><p>Also wrong.</p><h2 id="Behavior">Behavior</h2><p>Useful behavior.</p>''')
        self.assertEqual(texts(select_sections(page)), ['Shared page.', 'Useful behavior.'])

    def test_labeled_tables_and_optional_sections(self):
        page = article('''
            <div class="infobox"><p>Java Edition</p><p>Bedrock Edition</p>
            <table class="infobox-rows"><tr><th>Waterloggable</th>
                <td><b>JE:</b> No<br><b>BE:</b> Yes</td></tr></table></div>
            <p>Shared introduction.</p>
            <h2 id="Usage">Usage</h2>
            <table class="wikitable"><caption>A single beacon pyramid</caption>
                <tr><th>Level</th><th>Mineral blocks</th></tr><tr><td>4</td><td>164</td></tr></table>
            <table class="wikitable"><tr><th>Property</th><th>Java Edition</th><th>Bedrock Edition</th></tr>
                <tr><td>Cooldown</td><td>7 ticks</td><td>8 ticks</td></tr></table>
            <table class="wikitable"><tr><th colspan="2">Bedrock Edition</th></tr>
                <tr><th>Name</th><th>Count</th></tr><tr><td>Wrong scope</td><td>12</td></tr></table>
            <h2 id="Sounds">Sounds</h2><p>Java Edition:</p>
            <table class="wikitable"><tr><th>Sound</th><th>Identifier</th></tr>
                <tr><td><audio>Audio noise.</audio></td><td>block.metal.hit</td></tr></table>
            <p>Bedrock Edition:</p><table class="wikitable"><tr><th>Identifier</th></tr>
                <tr><td>block.bedrock_only</td></tr></table>
            <h2 id="History">History</h2><h3 id="Java_History">Java Edition</h3>
            <table class="wikitable" data-description="History">
                <tr><th colspan="8">Java Edition</th></tr>
                <tr><th rowspan="2" colspan="3">1.9</th><th colspan="4">15w42a</th><td>Added an item.</td></tr>
                <tr><th colspan="4">15w43a</th><td>Updated the item.</td></tr>
                <tr><th colspan="7">1.21</th><td>Changed the item.</td></tr></table>''')
        self.assertEqual(texts(select_sections(page)), [
            'Waterloggable: No', 'Shared introduction.', 'A single beacon pyramid; Level: 4; Mineral blocks: 164',
            'Property: Cooldown; Java Edition: 7 ticks'
        ])
        self.assertEqual(texts(select_sections(page, 'Sounds')), ['Identifier: block.metal.hit'])
        self.assertEqual(texts(select_sections(page, 'History')), [
            '1.9 15w42a Added an item.', '1.9 15w43a Updated the item.', '1.21 Changed the item.'
        ])
        catalog = {s['anchor']: s['default'] for s in select_sections(page, catalog=True)}
        self.assertFalse(catalog['Sounds'])
        self.assertFalse(catalog['Java_History'])


if __name__ == '__main__':
    unittest.main()
